// Draws, binds and dispatches of the Vulkan op-stream replay: the strong
// vk_exec_op_draw behind vk_exec.c's weak hook. The bind ops fill the VkExec slots; a draw opens the pass, resolves the
// bound program to translated shaders (the translator's answers come from the bound state), asks vk_pipeline.c for the
// pipeline, pushes (or allocates) set 0 as VkPipeInfo.bindings says and records the dynamic state, vertex and index
// buffers and the draw. A dispatch ends the pass and does the same for the compute shader. UPDATE_UB is a buffer copy
// from transient memory between full barriers.
//
// Per command buffer the replay keeps transient memory (support buffers, copied uniform ranges, generated indices),
// descriptor pools, image views and the storage-buffer ranges the GPU may write; all of it is freed, and the ranges are
// reported with vk_gpu_written, from one completion callback once the submission's fence is done (the write watch
// does not see GPU writes). Nothing here locks: it runs on the submitting thread under vk_sync_submit's lock.
//
// Provenance: the op semantics (bind ops, program and slot conventions, draw and dispatch flow, vertex streams with a
// zeroed buffer for unbound ones) follow MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m:7409-7460,
// 9365-9488, 9489-9921, 10013-10079 @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou); the uniform-buffer spans, texture and
// sampler descriptor lookups and the dispatch flow follow LAS Recomp runtime/nvn/vulkan/exec.c:1851-1908, 2300-2516
// @ da35b83a (MIT, Copyright (c) 2026 Peiikai). The support buffer layout is Ryujinx's SupportBuffer.cs (MIT).
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_geometry_control.h"
#include "nvn_indirect_reset.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

#define MAX_BINDINGS 128
// NVN 2 compute parameter layout of the program being dispatched (gfx_shader_c0_layout), NULL for NVN 1: read by
// vk_c0_fill, set by the dispatch ops around bind_resources like c0_dim.
static const Nvn2C0Layout *c0_lay;
static const uint32_t (*c0_push)[NVN2_ROOT_WORDS];
#define MAX_DESC 4096   // KAR_ARR_N material arrays (up to 1024 elements each, as Eden reads them) need more than 1024
#define CHUNK_BYTES (4u << 20)
#define ZERO_BYTES 65536u
#define SUPPORT_BYTES 1408u   // Ryujinx SupportBuffer (std140): alpha_test, is_bgra[8], viewport_inverse, viewport_size, ...
#define NVN_STAGE_VS 0
#define NVN_STAGE_FS 1
#define NVN_STAGE_GS 2
#define NVN_STAGE_CS 5
static uint8_t geometry_input;                              // normalized actual draw primitive; zero outside opt-in GS
#define ONESHOT_WAIT_MS 30000u   // prepare_draw: how long a one-shot draw waits for its pipeline before it is skipped

static void c0_lay_set(VkExec *ex, const GfxShader *cs) {
    c0_lay = gfx_shader_c0_layout(cs);
    c0_push = ex->push[1];
}

enum { W_PROGRAM, W_SHADER, W_PIPELINE, W_NO_VS, W_INDEX, W_UB, W_BIND, W_SSBO_ALIGN, W_SSBO_SLOT, W_RESTART, W_INDIRECT, W_FAULT_SKIP, W_FAULT_SKIP2, W_SKIP_VS, W_SKIP_FS, W_RESET, W_GEOMETRY, W_COUNT };

static void warn_once(int key, const char *fmt, ...) {
    static volatile int32_t seen[W_COUNT];
    if (__atomic_exchange_n(&seen[key], 1, __ATOMIC_RELAXED)) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    rt_log("vk: %s (logged once)", msg);
}

static void *grow(void *p, int *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 8;
    p = realloc(p, elem * (size_t)*cap);
    if (!p) rt_fatal("vk: out of memory");
    return p;
}

// ---- resources of one command buffer, freed by its completion callback

typedef struct { VkBuffer buf; VmaAllocation alloc; uint8_t *map; uint64_t size, used; } Chunk;
typedef struct {
    VkImage image;
    VkFormat format;
    VkImageViewType type;
    uint32_t aspect, level0, levels, layer0, layers, swz;
} ViewKey;
_Static_assert(sizeof(ViewKey) == 40, "ViewKey is hashed as bytes: no padding");
typedef struct { uint64_t addr, size; } Range;
typedef struct {
    Chunk *chunks;
    int nchunks, cchunks;
    VkDescriptorPool *pools;
    int npools, cpools;
    Range *written;
    int nwritten, cwritten;
    uint32_t *written_slot;              // note_written: open-addressed index of written, entry = index + 1, 0 empty
    uint32_t written_slots;              // a power of two
    struct DsCacheEnt *dsc;              // ds_cache: descriptor sets written in this command buffer, by content
} CbRes;

// Descriptor sets by content within one command buffer (its pools live until it completes): a draw whose packed
// descriptors equal those of any earlier draw of the command buffer binds that set instead of allocating and writing
// a new one. Before, only the previous draw's set was reused, and draws alternating between materials wrote a set
// (with 512-element material arrays) each time (16.4a). Keyed by the layout and a 128-bit hash of the packed bytes.
#define DSC_N 4096
typedef struct DsCacheEnt { uint64_t h1, h2; VkDescriptorSetLayout sl; uint32_t n; VkDescriptorSet set; } DsCacheEnt;

static void ds_hash(const void *p, uint32_t n, uint64_t *h1, uint64_t *h2) {
    uint64_t a = 0x9E3779B97F4A7C15ull ^ n, b = 0xC2B2AE3D27D4EB4Full + n;
    const uint8_t *q = p;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, q + i, 8);
        a = (a ^ w) * 0xff51afd7ed558ccdull, a ^= a >> 32;
        b = (b + w) * 0xc4ceb9fe1a85ec53ull, b ^= b >> 29;
    }
    for (; i < n; i++) a = (a ^ q[i]) * 0x100000001b3ull, b = (b + q[i]) * 0x9E3779B97F4A7C15ull;
    *h1 = a ^ (a >> 31), *h2 = b ^ (b >> 33);
}

static int dsc_on(void) {
    static int on = -1;
    // Off unless KAR_DS_CACHE=1: with it the vehicle preview cards in the menu showed garbage (2026-11-12, g_std and
    // g_noblk against g_nodc), so the packed bytes do not determine a set after all (an image view replaced within
    // the command buffer is the suspect), and no frame-time gain was measured.
    if (on < 0) on = getenv("KAR_DS_CACHE") && *getenv("KAR_DS_CACHE") == '1';
    return on;
}

static CbRes *cur;
static uint64_t cur_fence;

// Descriptor pools and full-size transient chunks go back to these lists when their command buffer completes, instead
// of being destroyed and created again for the next one: each creation is a driver memory allocation (on Windows an
// allocation, a map and a paging-fence wait on the CPU), about ten of them per frame inside the replay.
#define SPARE_POOLS 64
#define SPARE_CHUNKS 16
static PlatMutex spare_lock = PLAT_MUTEX_INIT;
static VkDescriptorPool spare_pool[SPARE_POOLS];
static Chunk spare_chunk[SPARE_CHUNKS];
static int nspare_pool, nspare_chunk;

// State of the command buffer being recorded that the Vulkan command buffer itself holds.
static struct {
    VkPipeline pipe[2];                  // bound per bind point: 0 graphics, 1 compute
    VkPipeState st;                      // the state vk_pipeline_set_dynamic last recorded
    int st_valid;
    VkBuffer vb[17];                     // vertex buffers last bound
    VkDeviceSize vo[17], vsz[17], vstr[17];
    int vcls[17];                        // their pool class and pool record, for the census
    void *vpool[17];
    NVNbufferRange vin[16];              // the ranges they were resolved from, under vk_mem_gen vgen
    uint64_t vgen;
    int vb_valid;
    uint8_t support[SUPPORT_BYTES];      // last support buffer and where it lives
    float sup_in[3];                     // the support_fill arguments it was filled from
    VkBuffer sup_buf;
    VkDeviceSize sup_off;
    int sup_valid;
    uint8_t c0[4][VK_C0_BYTES];          // last c0 bank per stage (vertex, fragment, compute, geometry) and where it lives
    VkBuffer c0_buf[4];
    VkDeviceSize c0_off[4];
    int c0_valid[4];
    struct { p_bind_buf ssbo[16], ubo[16]; uint32_t dim[3], binst; Nvn2C0Layout lay; uint8_t has_lay;
             uint32_t roots[NVN2_ROOT_TABLES][NVN2_ROOT_WORDS]; } c0_in[4];   // what vk_c0_fill read for c0[k]
    VkPipelineLayout ds_layout;          // graphics layout the sets below were last pushed or bound with
    VkDescriptorSet ds_bound[2];         // allocated set bound per set number (0: none or pushed since)
    VkDescriptorSet ds_last[2];          // last allocated and written set per set number, contents in DS
    VkDescriptorSetLayout ds_last_layout[2];
    int ds_pushed[2];                    // set number holds the push of DS.psig under ds_layout
    int res_valid;                       // the graphics sets bound hold what the inputs in RS give (bind_resources)
} B;

// Contents of B.ds_last: the set's packed descriptors (VkDescSlot), which with its layout determine every write. A draw
// whose allocated set would hold the same reuses the set instead of allocating and writing one (a per-draw
// cost). psig: the same for the last push per set number; a push equal to it under the same layout is not recorded
// again (the descriptors stay bound across pipeline binds of that layout).
#define DS_SIG_BYTES (MAX_DESC * sizeof(VkDescSlot))
static struct { uint32_t n[2], pn[2]; uint8_t sig[2][DS_SIG_BYTES], psig[2][DS_SIG_BYTES]; } DS;

// TOTK_BIND_STATS counters (bind_stats).
static struct {
    int on;
    uint64_t draws, bind[2], desc[2], push, push_skip, set_new, set_reuse, ac_miss, same_prog, same_all, bind_skip, arr_desc, pool_desc, arr_own_samp;
} BS = {-1};

// The inputs of the last graphics bind_resources (bind_sig) and what its build counted for the traffic census, which a
// skipped build counts again. RS_BYTES bounds the bound-slot bytes a pipeline reads (RsList): per binding at most one
// c0 storage table, otherwise one bound range or handle per descriptor.
#define RS_BYTES (MAX_BINDINGS * sizeof(((VkExec *)0)->ssbo[0]) + MAX_DESC * sizeof(p_bind_buf))
typedef struct { int cls, use; uint64_t bytes; } RsCount;
static struct {
    int on;                              // TOTK_BIND_SKIP: 1 skip (default), 2 check, 0 off
    uint32_t hn;                         // words of h the last signature filled; 0: none
    uint64_t h[32];                      // generations, descriptor tables, feedback images, support inputs, base instance
    const VkPipeInfo *info;              // the pipeline whose RsList ranges body holds, as of bind op generation gen
    uint64_t gen;
    uint8_t body[RS_BYTES];
    int rec;                             // a graphics build is running: count_use records, scratch copies mark volatile
    int volatile_;                       // the build copied guest bytes into transient memory: a rebuild differs
    RsCount cnt[2 * VK_MCLS_N], pcnt[2 * VK_MCLS_N];
    int ncnt, pncnt, cnt_over;
    int cmds;                            // check mode: pushes, binds and set writes the build recorded
    uint32_t ckn;                        // check mode: the last build's packed descriptors (bytes in ck)
    uint8_t ck[DS_SIG_BYTES];
} RS = {-1};

// The bound-slot bytes of VkExec a pipeline's descriptors read, as ranges merged where they touch, built once per
// pipeline info: c0's storage table, the uniform and storage ranges of the bound slots and the texture and image
// handles. sup and c0: the support inputs and the base instance count too. p: storage buffers whose address the shader
// loads from a user uniform block; the address read from the block counts too. ok 0: never skipped (too many
// descriptors or such buffers).
#define RS_PTRS 16
typedef struct { uint32_t off, len; } RsRange;
typedef struct { int st; const GfxShaderRes *res; } RsPtr;
typedef struct { uint32_t n, bytes; uint8_t sup, c0, ok, np; RsPtr p[RS_PTRS]; RsRange r[]; } RsList;
static nvn_hmap rs_lists;                // VkPipeInfo pointer -> RsList, until shutdown
static const VkPipeInfo *rs_last;        // the last lookup
static const RsList *rs_last_l;

static uint32_t c0_dim[3], c0_binst;     // the dispatch size and base instance of the draw or dispatch being bound
static uint64_t bind_gen = 1;            // bumped by every bind op that writes ex->prog, ubo, ssbo, tex or img

static void cb_done(void *arg) {   // completion thread
    CbRes *r = arg;
    VkCtx *c = vk_ctx();
    for (int i = 0; i < r->nwritten; i++) vk_gpu_written(r->written[i].addr, r->written[i].size);
    free(r->dsc), r->dsc = NULL;
    for (int i = 0; i < r->npools; i++) {
        vkResetDescriptorPool(c->device, r->pools[i], 0);
        plat_mutex_lock(&spare_lock);
        int kept = nspare_pool < SPARE_POOLS;
        if (kept) spare_pool[nspare_pool++] = r->pools[i];
        plat_mutex_unlock(&spare_lock);
        if (!kept) vkDestroyDescriptorPool(c->device, r->pools[i], NULL);
    }
    for (int i = 0; i < r->nchunks; i++) {
        plat_mutex_lock(&spare_lock);
        int kept = r->chunks[i].size == CHUNK_BYTES && nspare_chunk < SPARE_CHUNKS;
        if (kept) spare_chunk[nspare_chunk++] = r->chunks[i];
        plat_mutex_unlock(&spare_lock);
        if (!kept) vmaDestroyBuffer(c->vma, r->chunks[i].buf, r->chunks[i].alloc);
    }
    free(r->written), free(r->written_slot), free(r->pools), free(r->chunks), free(r);
}

void vk_exec_draw_new_cb(void) { memset(&B, 0, sizeof B); }

static CbRes *cb_res(VkExec *ex) {
    if (cur && cur_fence == ex->fence) return cur;
    cur = calloc(1, sizeof *cur);
    if (!cur) rt_fatal("vk: out of memory");
    cur_fence = ex->fence;
    memset(&B, 0, sizeof B);
    vk_completion_add(ex->fence, cb_done, cur);
    return cur;
}

static uint8_t *scratch(VkExec *ex, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    CbRes *r = cb_res(ex);
    size = (size + 255) & ~255ull;
    Chunk *c = r->nchunks ? &r->chunks[r->nchunks - 1] : NULL;
    if (!c || c->used + size > c->size) {
        if (r->nchunks == r->cchunks) r->chunks = grow(r->chunks, &r->cchunks, sizeof *r->chunks);
        c = &r->chunks[r->nchunks];
        uint64_t n = size > CHUNK_BYTES ? size : CHUNK_BYTES;
        int reused = 0;
        if (n == CHUNK_BYTES) {
            plat_mutex_lock(&spare_lock);
            if (nspare_chunk) *c = spare_chunk[--nspare_chunk], reused = 1;
            plat_mutex_unlock(&spare_lock);
        }
        if (reused) {
            c->used = 0;
            r->nchunks++;
            *buf = c->buf;
            *off = 0;
            c->used = size;
            return c->map;
        }
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n,
                                 .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VkResult res = vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &c->buf, &c->alloc, &info);
        if (res != VK_SUCCESS) rt_fatal("vk: transient buffer of %llu bytes failed (%d)", (unsigned long long)n, (int)res);
        c->map = info.pMappedData, c->size = n, c->used = 0;
        r->nchunks++;
    }
    *buf = c->buf;
    *off = c->used;
    uint8_t *p = c->map + c->used;
    c->used += size;
    return p;
}

static uint32_t written_hash(uint64_t addr, uint64_t size) {
    uint64_t h = (addr ^ size * 0xc2b2ae3d27d4eb4full) * 0x9e3779b97f4a7c15ull;
    return (uint32_t)(h >> 32);
}

// Each distinct range once per command buffer, in first-write order. A scene with thousands of written ranges and tens
// of thousands of draws binding them spent most of the replay searching this list, so the search goes through a table.
static void note_written(VkExec *ex, uint64_t addr, uint64_t size) {
    CbRes *r = cb_res(ex);
    if ((uint32_t)r->nwritten * 2 >= r->written_slots) {
        uint32_t slots = r->written_slots ? r->written_slots * 2 : 256;
        uint32_t *table = calloc(slots, sizeof *table);
        if (!table) rt_fatal("vk: out of memory");
        for (int i = 0; i < r->nwritten; i++) {
            uint32_t at = written_hash(r->written[i].addr, r->written[i].size) & (slots - 1);
            while (table[at]) at = (at + 1) & (slots - 1);
            table[at] = (uint32_t)i + 1;
        }
        free(r->written_slot);
        r->written_slot = table, r->written_slots = slots;
    }
    uint32_t mask = r->written_slots - 1, at = written_hash(addr, size) & mask;
    for (; r->written_slot[at]; at = (at + 1) & mask) {
        const Range *w = &r->written[r->written_slot[at] - 1];
        if (w->addr == addr && w->size == size) return;
    }
    if (r->nwritten == r->cwritten) r->written = grow(r->written, &r->cwritten, sizeof *r->written);
    r->written[r->nwritten++] = (Range){addr, size};
    r->written_slot[at] = (uint32_t)r->nwritten;
    vk_mem_gpu_pending(addr, size);
}

static int reset_mapped(void *context, uint64_t addr, uint32_t size) {
    (void)context;
    VkMemSpan span;
    return vk_mem_resolve(addr, size, &span);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

// Whether two nonnull targets share a byte (duplicates included). Their copies then need a barrier between them to
// keep the input order; disjoint targets can all be copied after one barrier.
static int reset_targets_overlap(const uint64_t *pointers, uint32_t count) {
    uint64_t *sorted = malloc((size_t)count * sizeof *sorted);
    if (!sorted) rt_fatal("vk: out of memory checking indirect-reset targets");
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++)
        if (pointers[i]) sorted[n++] = pointers[i];
    qsort(sorted, n, sizeof *sorted, cmp_u64);
    int overlap = 0;
    for (uint32_t i = 1; i < n && !overlap; i++) overlap = sorted[i] - sorted[i - 1] < NVN_INDIRECT_RESET_RECORD_BYTES;
    free(sorted);
    return overlap;
}

// The pointer list may have been written earlier in this submission. Read its GPU-visible contents after submitting
// that prefix, then validate every target before recording a write. Null pointers have no record to reset.
int vk_exec_reset_indirect(VkExec *ex, uint64_t list_addr, uint32_t descriptor_bytes, int64_t first, int64_t count) {
    NvnIndirectResetList list;
    if (!nvn_indirect_reset_list(list_addr, descriptor_bytes, first, count, &list)) return 0;
    uint64_t *pointers = malloc(list.bytes);
    if (!pointers) rt_fatal("vk: out of memory reading an indirect-reset pointer list");
    if (!vk_exec_read_buffer(ex, list.begin, list.bytes, pointers) ||
        !nvn_indirect_reset_targets(&list, pointers, reset_mapped, NULL)) {
        free(pointers);
        return 0;
    }
    VkBuffer source;
    VkDeviceSize source_offset;
    static const uint32_t record[5] = {0, 1, 0, 0, 0};
    memcpy(scratch(ex, sizeof record, &source, &source_offset), record, sizeof record);
    CbRes *resources = cb_res(ex);
    Chunk *chunk = &resources->chunks[resources->nchunks - 1];
    if (vmaFlushAllocation(vk_ctx()->vma, chunk->alloc, source_offset, sizeof record) != VK_SUCCESS)
        rt_fatal("vk: flushing indirect-reset values failed");
    int ordered = reset_targets_overlap(pointers, list.count);
    vk_exec_barrier(ex);   // the copies follow every earlier access to the records
    for (uint32_t i = 0; i < list.count; i++) {
        uint64_t addr = pointers[i];
        if (!addr) continue;
        VkMemSpan span;
        if (!vk_mem_resolve(addr, sizeof record, &span))
            rt_fatal("vk: an indirect-reset mapping changed during replay");
        // Commit first-use imports to their mirror before choosing the destination. A later indirect bind must
        // read this same backing rather than create a mirror from the old contents.
        VK_MEM_USE(span);
        if (ordered) vk_exec_barrier(ex);   // preserves the input order of duplicate or overlapping target ranges
        VkBufferCopy copy = {source_offset, span.offset, sizeof record};
        vkCmdCopyBuffer(ex->cb, source, span.buffer, 1, &copy);
        ex->recorded++;
        vk_mem_gpu_wrote(&span, sizeof record);
        vk_usnap_drop(addr, sizeof record);
        vk_mem_count(span.cls, VK_MUSE_GPU_WRITE, sizeof record);
        note_written(ex, addr, sizeof record);
    }
    free(pointers);
    vk_exec_barrier(ex);   // subsequent shader accesses and indirect reads see every completed record
    return 1;
}

// ---- stand-ins for what a binding names but the game did not bind (zeros, a 1x1 image, the default sampler)

enum { DV_1D, DV_1D_ARRAY, DV_2D, DV_2D_ARRAY, DV_3D, DV_CUBE, DV_CUBE_ARRAY, DV_2D_MS, DV_2D_MS_ARRAY, DV_COUNT };

static struct {
    int ready;
    VkBuffer zbuf;
    VmaAllocation zalloc;
    VkBufferView zview;
    VkSampler samp, samp_shadow;
    struct { VkImage image; VmaAllocation alloc; VkImageView view, sview; } img[DV_COUNT];
} Z;

static const struct { VkImageType t; VkImageViewType vt; uint32_t layers; VkSampleCountFlagBits s; VkImageCreateFlags f; } dinfo[DV_COUNT] = {
    {VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, VK_SAMPLE_COUNT_4_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, VK_SAMPLE_COUNT_4_BIT, 0}};

// A 1x1 stand-in image of view type dinfo[i] in `fmt`, moved to GENERAL by the upload batch (which reaches the queue before
// the submission that uses it). *sview is the view a storage descriptor takes (a cube becomes a 2D array).
static void make_dummy(int i, VkFormat fmt, VkImage *image, VmaAllocation *alloc, VkImageView *view, VkImageView *sview) {
    VkCtx *c = vk_ctx();
    int ms = dinfo[i].s != VK_SAMPLE_COUNT_1_BIT;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = dinfo[i].f, .imageType = dinfo[i].t,
                            .format = fmt, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = dinfo[i].layers,
                            .samples = dinfo[i].s, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | (ms ? 0 : VK_IMAGE_USAGE_STORAGE_BIT),
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ia = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ia, image, alloc, NULL) != VK_SUCCESS) rt_fatal("vk: stand-in image %d failed", i);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *image,
                                .viewType = dinfo[i].vt, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dinfo[i].layers}};
    if (vkCreateImageView(c->device, &vi, NULL, view) != VK_SUCCESS) rt_fatal("vk: stand-in view failed");
    *sview = *view;
    if (!ms && (dinfo[i].vt == VK_IMAGE_VIEW_TYPE_CUBE || dinfo[i].vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (vkCreateImageView(c->device, &vi, NULL, sview) != VK_SUCCESS) rt_fatal("vk: stand-in view failed");
    }
    VkImageMemoryBarrier2 bar = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = *image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dinfo[i].layers}};
    VkCommandBuffer cb = vk_upload_begin();
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &bar};
    vkCmdPipelineBarrier2(cb, &di);
    vk_upload_end();
}

// Storage image stand-ins in the format a shader declares (GfxShaderRes.format: Ryujinx TextureFormat order); Vulkan wants
// the view format to match the declaration. Created on first use.
static const VkFormat tex_format[40] = {
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
    VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
    VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM,
    VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM,
    VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
    VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT,
    VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
    VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32};
static struct { int used, di; VkFormat fmt; VkImage image; VmaAllocation alloc; VkImageView view, sview; } SD[48];

static VkImageView storage_dummy(int di, int code) {
    VkFormat fmt = code > 0 && code < 40 ? tex_format[code] : VK_FORMAT_R8G8B8A8_UNORM;
    if (fmt == VK_FORMAT_R8G8B8A8_UNORM || dinfo[di].s != VK_SAMPLE_COUNT_1_BIT) return Z.img[di].sview;
    for (int i = 0; i < 48; i++) {
        if (!SD[i].used) {
            SD[i].used = 1, SD[i].di = di, SD[i].fmt = fmt;
            make_dummy(di, fmt, &SD[i].image, &SD[i].alloc, &SD[i].view, &SD[i].sview);
            return SD[i].sview;
        }
        if (SD[i].di == di && SD[i].fmt == fmt) return SD[i].sview;
    }
    return Z.img[di].sview;
}

static void dummies_init(void) {
    if (Z.ready) return;
    VkCtx *c = vk_ctx();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ZERO_BYTES,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &Z.zbuf, &Z.zalloc, &info) != VK_SUCCESS) rt_fatal("vk: zero buffer failed");
    memset(info.pMappedData, 0, ZERO_BYTES);
    VkBufferViewCreateInfo bv = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = Z.zbuf,
                                 .format = VK_FORMAT_R32G32B32A32_SFLOAT, .range = ZERO_BYTES};
    if (vkCreateBufferView(c->device, &bv, NULL, &Z.zview) != VK_SUCCESS) rt_fatal("vk: zero buffer view failed");
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    sp.lod_max = 1000.0f;
    Z.samp = vk_sampler_for(&sp);
    sp.cmp_mode = 1, sp.cmp_func = NVN_DEPTH_FUNC_ALWAYS;
    Z.samp_shadow = vk_sampler_for(&sp);

    for (int i = 0; i < DV_COUNT; i++)
        make_dummy(i, VK_FORMAT_R8G8B8A8_UNORM, &Z.img[i].image, &Z.img[i].alloc, &Z.img[i].view, &Z.img[i].sview);
    Z.ready = 1;
}

static void twins_release(VkCtx *c);

// Releases the stand-ins and the depth twins. The device must be idle; the game path leaves them to process exit like
// the pipelines.
void vk_exec_draw_shutdown(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    twins_release(c);
    for (int i = 0; Z.ready && i < DV_COUNT; i++) {
        if (Z.img[i].sview != Z.img[i].view) vkDestroyImageView(c->device, Z.img[i].sview, NULL);
        vkDestroyImageView(c->device, Z.img[i].view, NULL);
        vmaDestroyImage(c->vma, Z.img[i].image, Z.img[i].alloc);
    }
    for (int i = 0; i < 48 && SD[i].used; i++) {
        if (SD[i].sview != SD[i].view) vkDestroyImageView(c->device, SD[i].sview, NULL);
        vkDestroyImageView(c->device, SD[i].view, NULL);
        vmaDestroyImage(c->vma, SD[i].image, SD[i].alloc);
    }
    memset(SD, 0, sizeof SD);
    nvn_hm_clear(&rs_lists, free);
    rs_last = NULL, rs_last_l = NULL;
    for (int i = 0; i < nspare_pool; i++) vkDestroyDescriptorPool(c->device, spare_pool[i], NULL);
    for (int i = 0; i < nspare_chunk; i++) vmaDestroyBuffer(c->vma, spare_chunk[i].buf, spare_chunk[i].alloc);
    nspare_pool = nspare_chunk = 0;
    cur = NULL, cur_fence = 0;
    vk_usnap_shutdown();
    if (Z.ready) {
        vkDestroyBufferView(c->device, Z.zview, NULL);
        vmaDestroyBuffer(c->vma, Z.zbuf, Z.zalloc);
    }
    memset(&Z, 0, sizeof Z);
}

// ---- resource types

static VkImageViewType res_view_type(uint16_t t) {
    int arr = t & 0x100;
    switch (t & 0xff) {
    case 1: return arr ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case 4: return VK_IMAGE_VIEW_TYPE_3D;
    case 5: return arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    default: return arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

static int dummy_index(VkImageViewType vt, int ms) {
    switch (vt) {
    case VK_IMAGE_VIEW_TYPE_1D: return DV_1D;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return DV_1D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_3D: return DV_3D;
    case VK_IMAGE_VIEW_TYPE_CUBE: return DV_CUBE;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return DV_CUBE_ARRAY;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return ms ? DV_2D_MS_ARRAY : DV_2D_ARRAY;
    default: return ms ? DV_2D_MS : DV_2D;
    }
}

// GFX_TEX_* of a bound texture slot (the translator's tex_type / img_type answers).
static int gfx_type_of(const VkTexImage *img, const view_t *v, int hv) {
    if (hv && v->set_target) return vk_pipeline_tex_type(v->target);
    int ms = img->samples > 1;
    switch (img->view_type) {
    case VK_IMAGE_VIEW_TYPE_1D: return GFX_TEX_1D;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return GFX_TEX_1D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return ms ? GFX_TEX_2D_MS_ARRAY : GFX_TEX_2D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_3D: return GFX_TEX_3D;
    case VK_IMAGE_VIEW_TYPE_CUBE: return GFX_TEX_CUBE;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return GFX_TEX_CUBE_ARRAY;
    default: return ms ? GFX_TEX_2D_MS : GFX_TEX_2D;
    }
}

static VkComponentSwizzle swz_of(uint8_t s) {
    switch (s) {
    case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    default: return VK_COMPONENT_SWIZZLE_A;
    }
}

// A view in STENCIL mode samples the stencil plane (TextureViewSetDepthStencilMode); otherwise depth wins. The
// translator declares every sampled image float and the shader takes the raw bits, as for integer formats (nxnative
// nvn_mtl.m).
VkImageAspectFlags vk_exec_view_aspect(const VkTexImage *img, const view_t *v, int hv) {
    if (hv && v->set_dsmode && v->dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL && (img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    return img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT     ? VK_IMAGE_ASPECT_DEPTH_BIT
           : img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                       : VK_IMAGE_ASPECT_COLOR_BIT;
}

// Image views of registered textures, kept for their image's lifetime: made per command buffer they were created again
// every frame (vkCreateImageView took about 8% of the fight replay on the 7900X). Used under the texture lock, as
// vk_exec_image_gone is (destroy_tex), which hands the views of a destroyed image to the deferred release with it.
typedef struct ViewNode { struct ViewNode *next; ViewKey k; VkImageView view; } ViewNode;
static nvn_hmap view_of;                 // ViewKey -> VkImageView
static nvn_hmap views_by_image;          // VkImage -> its first ViewNode

// An image view of a registered texture for a shader resource of type `rtype` (GfxShaderRes.type); VK_NULL_HANDLE when
// the image cannot serve it (the caller binds a stand-in). Texture lock held.
static VkImageView get_view(VkExec *ex, const VkTexImage *img, const view_t *v, int hv, uint16_t rtype, int storage) {
    (void)ex;
    VkImageViewType vt = res_view_type(rtype);
    int ms = (rtype & 0x200) != 0;
    if ((img->samples > 1) != ms || (storage && ms)) return VK_NULL_HANDLE;
    if (storage && !(img->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return VK_NULL_HANDLE;
    int cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    int ok = 0;
    switch (vt) {
    case VK_IMAGE_VIEW_TYPE_1D:
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: ok = img->type == VK_IMAGE_TYPE_1D; break;
    case VK_IMAGE_VIEW_TYPE_3D: ok = img->type == VK_IMAGE_TYPE_3D; break;
    case VK_IMAGE_VIEW_TYPE_CUBE:
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        ok = img->type == VK_IMAGE_TYPE_2D && (img->view_type == VK_IMAGE_VIEW_TYPE_CUBE || img->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY);
        break;
    default: ok = img->type == VK_IMAGE_TYPE_2D; break;
    }
    if (!ok) return VK_NULL_HANDLE;
    uint32_t l0 = hv && v->set_levels ? v->base_level : 0;
    if (l0 >= img->levels) return VK_NULL_HANDLE;
    uint32_t nl = hv && v->set_levels && v->nlevels ? v->nlevels : img->levels - l0;
    if (l0 + nl > img->levels) nl = img->levels - l0;
    if (storage) nl = 1;
    uint32_t y0 = 0, ny = 1;
    if (vt != VK_IMAGE_VIEW_TYPE_3D) {
        y0 = hv && v->set_layers ? v->min_layer : 0;
        if (y0 >= img->layers) return VK_NULL_HANDLE;
        uint32_t rest = hv && v->set_layers && v->nlayers ? v->nlayers : img->layers - y0;
        if (y0 + rest > img->layers) rest = img->layers - y0;
        if (vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_2D) ny = 1;
        else if (vt == VK_IMAGE_VIEW_TYPE_CUBE) ny = 6;
        else if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) ny = rest / 6 * 6;
        else ny = rest;
        if (!ny || y0 + ny > img->layers) return VK_NULL_HANDLE;
    }
    if (storage && cube) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    uint32_t aspect = vk_exec_view_aspect(img, v, hv);
    uint8_t sw[4] = {NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A};
    if (!storage) {   // MNG nvn_mtl.m tex_swizzle_sel: view selectors replace the texture's, then the format remap
        memcpy(sw, hv && v->set_swizzle ? v->swz : img->swz, 4);
        if (img->alpha_one || img->chan[0])
            for (int i = 0; i < 4; i++) {
                if (img->alpha_one && sw[i] == NVN_TEXTURE_SWIZZLE_A) sw[i] = NVN_TEXTURE_SWIZZLE_ONE;
                else if (img->chan[0] && sw[i] >= NVN_TEXTURE_SWIZZLE_R && sw[i] <= NVN_TEXTURE_SWIZZLE_A)
                    sw[i] = img->chan[sw[i] - NVN_TEXTURE_SWIZZLE_R];
            }
    }
    uint32_t swz = (uint32_t)sw[0] | (uint32_t)sw[1] << 8 | (uint32_t)sw[2] << 16 | (uint32_t)sw[3] << 24;
    ViewKey k = {img->image, img->format, vt, aspect, l0, nl, y0, ny, swz};
    if (!view_of.klen) nvn_hm_init(&view_of, sizeof(ViewKey)), nvn_hm_init(&views_by_image, sizeof(VkImage));
    VkImageView found = (VkImageView)nvn_hm_get(&view_of, &k);
    if (found) return found;
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img->image, .viewType = vt,
                                .format = img->format,
                                .components = {swz_of(sw[0]), swz_of(sw[1]), swz_of(sw[2]), swz_of(sw[3])},
                                .subresourceRange = {aspect, l0, nl, y0, ny}};
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    if (img->block_views) ci.pNext = &use;   // the image's COLOR_ATTACHMENT usage is its block views' (vk_texture.c)
    VkImageView view;
    if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    ViewNode *n = malloc(sizeof *n);
    if (!n) rt_fatal("vk: out of memory");
    *n = (ViewNode){nvn_hm_get(&views_by_image, &img->image), k, view};
    nvn_hm_put(&views_by_image, &img->image, n);
    nvn_hm_put(&view_of, &k, (void *)view);
    return view;
}

// An attachment view of a registered texture, kept with its sampled views for the image's lifetime: made per command
// buffer, every render target's views were created again each frame (vkCreateImageView under vk_exec_pass_begin, 2%
// of the fight replay on the 7900X). Attachment keys carry a swizzle word no sampled key has (those hold four NVN
// selectors, each below 8). The texture lock guards the tables, as for get_view; vk_exec_pass_begin may already hold it.
VkImageView vk_exec_target_view(const VkTexImage *img, VkFormat format, VkImageViewType type, uint32_t aspect,
                                uint32_t level, uint32_t base, uint32_t layers, int blocks) {
    ViewKey k = {img->image, format, type, aspect, level, 1, base, layers, blocks ? 0xfffffffeu : 0xffffffffu};
    vk_texture_lock();
    if (!view_of.klen) nvn_hm_init(&view_of, sizeof(ViewKey)), nvn_hm_init(&views_by_image, sizeof(VkImage));
    VkImageView view = (VkImageView)nvn_hm_get(&view_of, &k);
    if (!view) {
        VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img->image,
                                    .viewType = type, .format = format, .subresourceRange = {aspect, level, 1, base, layers}};
        VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                          .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        if (blocks) ci.pNext = &use;   // not the BC image's sampling and transfer usages
        // A view in another format (an sRGB target over a UNORM texture) drops the image's storage usage, which that
        // format may lack: every colour texture that allows it is made with storage usage (vk_texture.c create_tex).
        else if (format != img->format && (img->usage & VK_IMAGE_USAGE_STORAGE_BIT))
            use.usage = img->usage & ~VK_IMAGE_USAGE_STORAGE_BIT, ci.pNext = &use;
        if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS)
            rt_fatal("vk: render target view (format %d) could not be created", (int)format);
        ViewNode *n = malloc(sizeof *n);
        if (!n) rt_fatal("vk: out of memory");
        *n = (ViewNode){nvn_hm_get(&views_by_image, &img->image), k, view};
        nvn_hm_put(&views_by_image, &img->image, n);
        nvn_hm_put(&view_of, &k, (void *)view);
    }
    vk_texture_unlock();
    return view;
}

void vk_exec_image_gone(VkImage image) {
    vk_exec_bc_gone(image);
    if (!views_by_image.klen) return;
    for (ViewNode *n = nvn_hm_del(&views_by_image, &image), *next; n; n = next) {
        next = n->next;
        nvn_hm_del(&view_of, &n->k);
        vk_defer_view(n->view);
        free(n);
    }
}

// ---- program to shaders

// (program, stage, answers) -> shader, 4-way set-associative, most recently used first. A miss costs a SHA-256 of the
// whole shader code (gfx_shader.c req_init), slow on CPUs without SHA instructions: a field frame looks up ~3000 and
// the 2048-slot direct-mapped table this replaced missed 3% of them on conflicts.
#define SHADER_SETS 2048
#define SHADER_WAYS 4
typedef struct { uint64_t serial; int stage; int used; GfxShaderAnswers a; const GfxShader *s; } ShaderEnt;
static ShaderEnt shader_tab[SHADER_SETS][SHADER_WAYS];

static int find_stage_scan(const prog_t *p, int nvn_stage, const uint8_t **ctl, const uint8_t **code, uint32_t *len) {
    for (uint32_t i = 0; i < p->nstage && i < 6; i++) {
        const uint8_t *c = p->sd[i].control;
        if (!c || !p->sd[i].data) continue;
        uint32_t st;
        memcpy(&st, c + 0x714, 4);
        if ((int)st != nvn_stage) continue;
        memcpy(len, c + 0x704, 4);
        *ctl = c, *code = (const uint8_t *)(uintptr_t)p->sd[i].data;
        return 1;
    }
    return 0;
}

// Which shader of a program serves each NVN stage, and its code length, per program serial (new at every
// nvnProgramSetShaders): the scan reads two words of every stage's control block, cold guest cache lines, and runs
// several times a draw. Direct-mapped; a collision only scans again.
#define STAGEM_N 512
static struct { uint64_t serial; int8_t idx[6]; uint32_t len[6]; } stagem[STAGEM_N];

static int find_stage(const prog_t *p, int nvn_stage, const uint8_t **ctl, const uint8_t **code, uint32_t *len) {
    if (!p->serial || (unsigned)nvn_stage >= 6) return find_stage_scan(p, nvn_stage, ctl, code, len);
    typeof(stagem[0]) *m = &stagem[p->serial * 0x9E3779B97F4A7C15ull >> 55];
    if (m->serial != p->serial) {
        m->serial = p->serial;
        memset(m->idx, -1, sizeof m->idx);
        for (uint32_t i = 0; i < p->nstage && i < 6; i++) {
            const uint8_t *c = p->sd[i].control;
            if (!c || !p->sd[i].data) continue;
            uint32_t st;
            memcpy(&st, c + 0x714, 4);
            if (st < 6 && m->idx[st] < 0) m->idx[st] = (int8_t)i, memcpy(&m->len[st], c + 0x704, 4);
        }
    }
    int i = m->idx[nvn_stage];
    if (i < 0) return 0;
    *ctl = p->sd[i].control, *code = (const uint8_t *)(uintptr_t)p->sd[i].data, *len = m->len[nvn_stage];
    return 1;
}

// Texture and image types of a stage's first 32 slots, each kept with the handle and the generation and slot token it
// was looked up under: a slot is looked up again, a hash probe, only when the handle changed or its slot may resolve
// otherwise (slot_still).
static struct { uint64_t context, tex[32], img[32], ttok[32], itok[32]; uint32_t tgen[32], igen[32]; uint8_t tt[32], it[32]; } AC[6];
static uint64_t descriptor_serial = 1;

// vk_texture_slot_token of the slot a texture or image handle names; an empty handle always resolves to nothing.
static uint64_t token_of(const VkExec *ex, uint64_t h) {
    return h ? vk_texture_slot_token(&ex->tex_pool, (int)(h & 0xfffff)) : 1;
}

static void descriptor_state(VkExec *ex);

// Whether what handle h resolved to under the global generation *egen and its slot token etok (both read before that
// lookup) still holds at global generation gen: nothing at all changed since, or not its slot (the token, read only when
// the global generation moved, about every 16 draws). A token hit moves *egen to gen.
static int slot_still(const VkExec *ex, uint32_t *egen, uint64_t etok, uint64_t h, uint32_t gen) {
    if (*egen == gen) return 1;
    uint64_t t = token_of(ex, h);
    if (!t || t != etok) return 0;
    *egen = gen;
    return 1;
}
static int shader_memo_on(void);

// The slots of (program, stage, GS input topology) whose types its translation reads (gfx_shader_slot_mask), learned from its first
// shader; ~0u while unknown. The types of other slots select the same shader, so answers_for leaves them zero instead
// of looking up all 64 handles. Direct-mapped by serial and stage; a collision only relearns.
#define SLOTM_N 4096
static struct { uint64_t serial; int st; uint32_t m; } slotm[SLOTM_N];

static uint32_t slotm_idx(uint64_t serial, int st) { return (uint32_t)((serial * 8 + (uint64_t)st) * 0x9E3779B97F4A7C15ull >> 52); }

static int slot_mask_stage(int st) { return st == NVN_STAGE_GS ? st + 8 * geometry_input : st; }

static uint32_t slot_mask_of(uint64_t serial, int st) {
    st = slot_mask_stage(st);                           // GS topology can change which texture queries survive translation
    const typeof(slotm[0]) *e = &slotm[slotm_idx(serial, st)];
    return serial && e->serial == serial && e->st == st ? e->m : ~0u;
}

static void slot_mask_learn(uint64_t serial, int st, const GfxShader *s) {
    if (!serial || slot_mask_of(serial, st) != ~0u) return;
    st = slot_mask_stage(st);
    slotm[slotm_idx(serial, st)] = (typeof(slotm[0])){serial, st, gfx_shader_slot_mask(s)};
}

static uint8_t type_of_handle(const VkExec *ex, uint64_t h) {   // texture lock held
    view_t v;
    int hv;
    const VkTexImage *img = h ? vk_texture_by_id_locked(&ex->tex_pool, (int)(h & 0xfffff), &v, &hv) : NULL;
    return img ? (uint8_t)gfx_type_of(img, &v, hv) : 0;
}

// The texture handle a shader's texture slot reads. NVN 2 compute keeps texture and image handles in one packed bank;
// a program with images and no textures (Kirby Air Riders' colour-grading LUT, cs 92b396e446a73883: TXQ on handle 8,
// image stores through handle 8) queries its image's size through the texture path, where the bank holds the image
// handle. Images register in the texture pool too (nvnTexturePoolRegisterImage), so the image handle resolves there.
static uint64_t tex_or_image(const VkExec *ex, int st, int slot) {
    uint64_t h = ex->tex[st][slot];
    if (!h && st == NVN_STAGE_CS && slot < 32) h = ex->img[st][slot];
    return h;
}

// NVN 2 compute: the number of texture handles in the program's packed bank, from the "DKC1" layout nvn2_lower.cpp
// stores in the lowered control at 0x860 (byte 4 images, 7 textures), or -1 for another control. The translator
// (tools/xlate Program.cs TexSlot) numbers a texture handle that falls in the bank's image range by its image slot and
// takes the type of "tex" slot n before "img" slot n, so for n at or past the texture count the texture answer must be
// the image's: else a stale texture bound at that slot answers (the colour-grading LUT, no textures and two 3D images,
// was translated with a 2D size query: R and G of the grade were 0).
static int cs_tex_count(const uint8_t *ctl) {
    return ctl && !memcmp(ctl + 0x860, "DKC1", 4) ? ctl[0x860 + 7] : -1;
}

// The answers of stage st for the slots in mask; the types of other slots stay zero.
uint64_t vk_rp_sh[5];   // KAR_REPLAY_PROF, compute shader lookups: answers, texture lock wait, translate/get, memo misses, lookups
static void answers_for(VkExec *ex, int st, const uint8_t *ctl, uint32_t mask, GfxShaderAnswers *a) {
    descriptor_state(ex);
    memset(a, 0, sizeof *a);
    if (st == NVN_STAGE_VS) vk_pipeline_answers(&ex->pipe, a, NULL);
    else if (st == NVN_STAGE_FS) vk_pipeline_answers(&ex->pipe, NULL, a);
    else if (st == NVN_STAGE_GS) a->geometry_input = geometry_input;
    else if (st == NVN_STAGE_CS)
        for (int k = 0; k < 3; k++) {
            uint32_t l;
            memcpy(&l, ctl + 0x748 + 4 * k, 4);
            a->local[k] = (uint16_t)(l ? l : 1);
        }
    typeof(AC[0]) *c = &AC[st];
    if (c->context != descriptor_serial) {
        memset(c, 0, sizeof *c);
        c->context = descriptor_serial;
    }
    // The generation and tokens are read before the lookups (one that creates an entry changes them) and unlocked: a
    // registration racing with them is ordered after this draw, and a slot stored under older ones is only looked up
    // again.
    uint32_t gen = vk_texture_slots_gen();
    int locked = 0, check = shader_memo_on() == 2, ntex = st == NVN_STAGE_CS ? cs_tex_count(ctl) : -1;
    for (uint32_t m = mask; m; m &= m - 1) {
        int slot = __builtin_ctz(m);
        uint64_t hi = ex->img[st][slot], h = ntex >= 0 && slot >= ntex ? hi : tex_or_image(ex, st, slot);
        int tmiss = c->tex[slot] != h || !c->ttok[slot] || !slot_still(ex, &c->tgen[slot], c->ttok[slot], h, gen);
        int imiss = c->img[slot] != hi || !c->itok[slot] || !slot_still(ex, &c->igen[slot], c->itok[slot], hi, gen);
        uint64_t tt = tmiss ? token_of(ex, h) : 0, it = imiss ? token_of(ex, hi) : 0;
        if ((tmiss || imiss || check) && !locked) {
            uint64_t lw = st == NVN_STAGE_CS ? plat_now_ns() : 0;
            vk_texture_lock(), locked = 1, BS.ac_miss++;
            if (lw) vk_rp_sh[1] += plat_now_ns() - lw;
        }
        if (tmiss) c->tt[slot] = type_of_handle(ex, h), c->tex[slot] = h, c->ttok[slot] = tt, c->tgen[slot] = gen;
        if (imiss) c->it[slot] = type_of_handle(ex, hi), c->img[slot] = hi, c->itok[slot] = it, c->igen[slot] = gen;
        if (check && (type_of_handle(ex, h) != c->tt[slot] || type_of_handle(ex, hi) != c->it[slot]))   // TOTK_SHADER_MEMO=check
            rt_fatal("vk_exec: kept texture types of stage %d slot %d differ from a lookup", st, slot);
        a->tex_type[slot] = c->tt[slot], a->img_type[slot] = c->it[slot];
    }
    if (locked) vk_texture_unlock();
}

// DKB_NATIVE_GS runs lowered native passthrough geometry stages through the portable path (issue #45). On by default
// (runtime/cfg.c): without it the destructible terrain draws black; DKB_NATIVE_GS=0 turns it off.
// Eglantine takes the varyings from the hardware skip mask (Maxwell register 0x1240, translate_program.cpp,
// LowerGeometryPassthrough); the host does not have that mask for NVN 2 programs, so this infers it: what the vertex
// stage writes and the fragment stage reads. On AMD's RDNA 1/2 Windows driver the mine voxel stage is the known
// exception: its captured hardware mask forwards locations 0 and 19 across both its depth and colour variants. RADV
// and the other drivers keep the reflected mask.
static int native_gs_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_bool("TOTK_NATIVE_GS");
    return on;
}

static const GfxShader *native_gs_vs, *native_gs_fs;   // the draw's stages, set before its geometry lookup

static const uint8_t *native_gs_control(const uint8_t *ctl, const uint8_t *code, uint32_t len) {
    // nvn2_lower.cpp::nvn2_lower keeps these source controls for the process lifetime. Keep the packed copies
    // just as long, including after the first 256 contexts; shader_for borrows them even on shader-cache hits.
    static struct { const uint8_t *ctl; uint32_t mask; uint8_t *copy; } *made;
    static size_t count, capacity;
    GfxShaderIo io[32];
    const uint32_t fs_in = gfx_shader_inputs(native_gs_fs, io), vs_out = gfx_shader_outputs(native_gs_vs, io);
    uint32_t mask = fs_in & vs_out;
    if (vk_caps.amd_sc && nvn2_origin(code) == UINT64_C(0x3dbaaf649cb5f927))
        mask |= (UINT32_C(1) << 0) | (UINT32_C(1) << 19);
    for (size_t i = 0; i < count; i++)
        if (made[i].ctl == ctl && made[i].mask == mask) return made[i].copy;
    uint32_t skip[8];
    skip[0] = 0x0fffffffu;
    for (unsigned i = 1; i < 8; i++) skip[i] = UINT32_MAX;
    for (unsigned l = 0; l < 32; l++)
        if (mask >> l & 1) skip[1 + l / 8] &= ~(15u << (l % 8 * 4));
    uint8_t *copy = malloc(0x880);
    if (!copy) return NULL;
    memcpy(copy, ctl, 0x880);
    if (!nvn_geometry_control_pack_lowered(copy, 0x880, code, len, skip)) {
        rt_log("native GS: stage %016llx does not fit the portable control", (unsigned long long)nvn2_origin(code));
        free(copy);
        return NULL;
    }
    if (count == capacity) {
        if (capacity > SIZE_MAX / 2) { free(copy); return NULL; }
        size_t next = capacity ? capacity * 2 : 256;
        if (next > SIZE_MAX / sizeof *made) { free(copy); return NULL; }
        void *grown = realloc(made, next * sizeof *made);
        if (!grown) { free(copy); return NULL; }
        made = grown, capacity = next;
    }
    made[count].ctl = ctl, made[count].mask = mask, made[count].copy = copy;
    count++;
    rt_log("native GS: stage %016llx forwards locations %#x (vertex writes %#x, fragment reads %#x)",
           (unsigned long long)nvn2_origin(code), mask, vs_out, fs_in);
    return copy;
}

// *present: the program has this stage. Returns NULL when it has not or the translation failed. pending (graphics
// stages, vk_pipeline_async_on): a shader not translated yet is queued and NULL returned with *pending = 1. await: a
// shader queued earlier is waited for (gfx_shader_await) instead.
static const GfxShader *shader_for(VkExec *ex, int st, int *present, int *pending, int await) {
    const uint8_t *ctl, *code;
    uint32_t len;
    *present = 0;
    if (pending) *pending = 0;
    if (!ex->prog || !find_stage(ex->prog, st, &ctl, &code, &len)) return NULL;
    *present = 1;
    if (st == NVN_STAGE_GS && native_gs_on() && nvn2_origin(code)) {
        if (!native_gs_vs || !native_gs_fs) {   // its varyings follow from the other stages, which are still pending
            if (pending) *pending = 1;
            return NULL;
        }
        if (!(ctl = native_gs_control(ctl, code, len))) return NULL;
    }
    GfxShaderAnswers a;
    uint64_t serial = ex->prog->serial;
    uint64_t ra = st == NVN_STAGE_CS ? plat_now_ns() : 0;
    answers_for(ex, st, ctl, slot_mask_of(serial, st), &a);
    if (ra) vk_rp_sh[0] += plat_now_ns() - ra, vk_rp_sh[4]++;
    ShaderEnt *set = NULL;
    if (serial) {
        uint64_t h = (serial * 0x9E3779B97F4A7C15ull ^ (uint64_t)st) ^ nvn_hm_hash(&a, sizeof a);   // a is zero-padded
        set = shader_tab[(h ^ h >> 29) & (SHADER_SETS - 1)];
        for (int w = 0; w < SHADER_WAYS && set[w].used; w++) {
            ShaderEnt *e = &set[w];
            if (e->serial != serial || e->stage != st || memcmp(&e->a, &a, sizeof a)) continue;
            if (w) {   // to the front
                ShaderEnt hit = *e;
                memmove(&set[1], &set[0], w * sizeof *set);
                set[0] = hit;
            }
            if (st == NVN_STAGE_CS) gfx_shader_note_cs_used(set[0].s);   // cheap after the first: a flag
            return set[0].s;
        }
    }
    int gs = st == NVN_STAGE_VS ? GFX_STAGE_VS : st == NVN_STAGE_FS ? GFX_STAGE_FS :
             st == NVN_STAGE_GS ? GFX_STAGE_GS : st == NVN_STAGE_CS ? GFX_STAGE_CS : -1;
    if (gs < 0) return NULL;
    uint64_t rg = st == NVN_STAGE_CS ? plat_now_ns() : 0;
    const GfxShader *s = await ? gfx_shader_await(gs, ctl, code, len, &a)
                       : pending && vk_pipeline_async_on() ? gfx_shader_get_async(gs, ctl, code, len, &a, pending)
                                                           : gfx_shader_get(gs, ctl, code, len, NULL, &a);
    if (rg) vk_rp_sh[2] += plat_now_ns() - rg, vk_rp_sh[3]++;
    if (pending && *pending) return NULL;
    if (s) slot_mask_learn(serial, st, s);
    if (s && st == NVN_STAGE_CS) gfx_shader_note_cs_used(s);
    if (!s) warn_once(W_SHADER, "a stage %d shader of program %llu failed to translate: draws with it are skipped", st,
                      (unsigned long long)serial);
    else if (set) {   // in front, the least recently used way drops out
        memmove(&set[1], &set[0], (SHADER_WAYS - 1) * sizeof *set);
        set[0] = (ShaderEnt){.serial = serial, .stage = st, .used = 1, .a = a, .s = s};
    }
    return s;
}

// ---- descriptors

static int nvn_stage_of(int gfx_stage) {
    return gfx_stage == GFX_STAGE_VS ? NVN_STAGE_VS : gfx_stage == GFX_STAGE_FS ? NVN_STAGE_FS :
           gfx_stage == GFX_STAGE_GS ? NVN_STAGE_GS : gfx_stage == GFX_STAGE_CS ? NVN_STAGE_CS : -1;
}

static VkDescriptorBufferInfo zero_info(uint64_t max) {
    return (VkDescriptorBufferInfo){Z.zbuf, 0, max < ZERO_BYTES ? max : ZERO_BYTES};
}

// vk_mem_count, also kept per (class, use) while a graphics build runs (RS).
static void count_use(int cls, int use, uint64_t n) {
    vk_mem_count(cls, use, n);
    if (!RS.rec) return;
    for (int i = 0; i < RS.ncnt; i++)
        if (RS.cnt[i].cls == cls && RS.cnt[i].use == use) {
            RS.cnt[i].bytes += n;
            return;
        }
    if (RS.ncnt < (int)(sizeof RS.cnt / sizeof RS.cnt[0])) RS.cnt[RS.ncnt++] = (RsCount){cls, use, n};
    else RS.cnt_over = 1;
}

// A buffer descriptor for [addr, addr+size) of guest memory: its backing (a pool's VRAM mirror when it has one) when
// aligned, a copy in transient memory when the backing is host memory with an unaligned offset. 0 when neither works.
// written: the shader stores into it (a mirror's range goes back to guest memory at the end of the submission).
// direct_only: guarded aliases must share the real backing; a transient copy would lose writes and alias coherence.
static int bind_range(VkExec *ex, uint64_t addr, uint64_t size, uint64_t align, uint64_t max, int use, int written,
                      int direct_only, VkDescriptorBufferInfo *out) {
    VkMemSpan sp;
    if (!addr || !vk_mem_resolve(addr, 1, &sp)) return 0;
    uint64_t n = size && size < sp.avail ? size : sp.avail;
    if (n > max) n = max;
    if (!n) return 0;
    if (sp.offset % align == 0) {
        VK_MEM_USE(sp);
        *out = (VkDescriptorBufferInfo){sp.buffer, sp.offset, n};
        if (written) vk_mem_gpu_wrote(&sp, n), vk_usnap_drop(addr, n);
        count_use(sp.cls, use, n);
        VK_MEM_BOUND(sp);
        return 1;
    }
    if (sp.alloc || direct_only) return 0;
    VkBuffer b;
    VkDeviceSize o;
    RS.volatile_ = 1;
    memcpy(scratch(ex, n, &b, &o), (const void *)(uintptr_t)addr, n);
    vk_mem_count(sp.cls, VK_MUSE_CPU_READ, n);
    vk_mem_count(VK_MCLS_SCRATCH, use, n);
    *out = (VkDescriptorBufferInfo){b, o, n};
    return 1;
}

static void c0_table(uint8_t *at, const p_bind_buf b[16]) {
    for (int r = 0; r < 16; r++) {
        uint64_t a = b[r].addr;
        uint32_t n = (uint32_t)b[r].size;
        memcpy(at + 16 * r, &a, 8);
        memcpy(at + 16 * r + 8, &n, 4);
    }
}

// The driver constant bank c0 as GLSLC-compiled shaders read it. Storage-buffer tables {u64 address, u32 size} every
// 16 bytes at 0x110 (vertex), 0x510 (fragment), 0x310 (compute): Ryujinx's GlobalToStorage fallback loads each
// candidate base from here and compares the global pointer against it. Compute: dispatch size at 0x200, the uniform
// buffer table at 0x210 + 16 r. Graphics: the largest point size (Maxwell's limit, GLSLC clamps gl_PointSize to it) at
// 0x610, the base instance at 0x644. Other stages: zeros. Unbound slots read zero, since the replay keeps them zero.
// Layout: nxnative dm/gfx/nvn/nvn_mtl.m (MIT).
void vk_c0_fill(uint8_t bank[VK_C0_BYTES], int stage, const p_bind_buf ssbo[16], const p_bind_buf ubo[16],
                const uint32_t dispatch[3], uint32_t base_instance) {
    if (stage == NVN_STAGE_CS && c0_lay) {
        if (!nvn2_c0_pack(bank, VK_C0_BYTES, c0_lay, c0_push, ssbo, ubo))
            rt_fatal("vk: invalid NVN 2 compute parameter-block layout");
        return;
    }
    memset(bank, 0, VK_C0_BYTES);
    int base =stage == NVN_STAGE_VS ? 0x110 : stage == NVN_STAGE_FS ? 0x510 : stage == NVN_STAGE_CS ? 0x310 :
              stage == NVN_STAGE_GS ? 0x410 : -1;   // geometry (DKB_NATIVE_GS): the table ssbo_slot expects
    if (base < 0) return;
    c0_table(bank + base, ssbo);
    if (stage == NVN_STAGE_CS) {
        memcpy(bank + 0x200, dispatch, 12);
        c0_table(bank + 0x210, ubo);
        return;
    }
    const float max_point = 2047.0f;
    memcpy(bank + 0x610, &max_point, 4);
    memcpy(bank + 0x644, &base_instance, 4);
}

static int c0_on(void) {   // TOTK_FIX_C0=0: c0 reads zeros instead of the driver constants
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("TOTK_FIX_C0");
    }
    return on;
}

// c0 from replay scratch; a bank equal to the stage's previous one reuses its range, so it costs no copy.
static VkDescriptorBufferInfo c0_info(VkExec *ex, int st) {
    if (st != NVN_STAGE_VS && st != NVN_STAGE_FS && st != NVN_STAGE_CS && st != NVN_STAGE_GS)
        rt_fatal("vk_exec: synthetic c0 is unavailable for stage %d", st);
    int k = st == NVN_STAGE_VS ? 0 : st == NVN_STAGE_FS ? 1 : st == NVN_STAGE_GS ? 3 : 2;
    // vk_c0_fill reads buffer bindings, compute root words and dispatch size, or the graphics base instance:
    // equal inputs give the same bank, so neither the fill nor the 2 KiB compare is needed.
    typeof(B.c0_in[0]) in;
    memset(&in, 0, sizeof in);
    memcpy(in.ssbo, ex->ssbo[st], sizeof in.ssbo);
    if (st == NVN_STAGE_CS) memcpy(in.ubo, ex->ubo[st], sizeof in.ubo), memcpy(in.dim, c0_dim, sizeof in.dim);
    else in.binst = c0_binst;
    if (st == NVN_STAGE_CS && c0_lay) {
        in.lay = *c0_lay, in.has_lay = 1;
        for (unsigned i = 0; i < NVN2_ROOT_TABLES; i++)
            memcpy(in.roots[i], ex->push[1][i], 4u * c0_lay->roots[i]);
    }
    if (B.c0_valid[k] && !memcmp(&in, &B.c0_in[k], sizeof in))
        return (VkDescriptorBufferInfo){B.c0_buf[k], B.c0_off[k], VK_C0_BYTES};
    B.c0_in[k] = in;
    uint8_t tmp[VK_C0_BYTES];
    vk_c0_fill(tmp, st, ex->ssbo[st], ex->ubo[st], c0_dim, c0_binst);
    if (!B.c0_valid[k] || memcmp(tmp, B.c0[k], VK_C0_BYTES)) {
        memcpy(scratch(ex, VK_C0_BYTES, &B.c0_buf[k], &B.c0_off[k]), tmp, VK_C0_BYTES);
        memcpy(B.c0[k], tmp, VK_C0_BYTES);
        B.c0_valid[k] = 1;
    }
    return (VkDescriptorBufferInfo){B.c0_buf[k], B.c0_off[k], VK_C0_BYTES};
}

// The buffer range limits, read once: the device's properties are a cold cache line on every binding of every draw.
static struct { uint64_t ubo_max, ubo_align, sbo_max, sbo_align; } LIM;

static void limits_init(void) {
    const VkPhysicalDeviceLimits *l = &vk_ctx()->props.limits;
    LIM.ubo_align = l->minUniformBufferOffsetAlignment, LIM.sbo_max = l->maxStorageBufferRange;
    LIM.sbo_align = l->minStorageBufferOffsetAlignment;
    __atomic_store_n(&LIM.ubo_max, l->maxUniformBufferRange, __ATOMIC_RELEASE);
}

static VkDescriptorBufferInfo ubo_info(VkExec *ex, int st, int slot) {
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    uint64_t max = LIM.ubo_max, align = LIM.ubo_align;
    VkDescriptorBufferInfo bi;
    if (slot == 0 && c0_on()) return c0_info(ex, st);
    if (slot == 1 && ex->prog) {   // compiler constants stored after the shader code
        const uint8_t *ctl, *code;
        uint32_t len, c1;
        if (find_stage(ex->prog, st, &ctl, &code, &len)) {
            memcpy(&c1, ctl + 0x700, 4);
            if (c1 < len && bind_range(ex, (uint64_t)(uintptr_t)code + c1, len - c1, align, max, VK_MUSE_UBO, 0, 0, &bi))
                return bi;
            if (c1 < len && len - c1 <= max) {   // NVN 2 stages: the lowered code (nvn2_lower.cpp) is host memory
                VkBuffer b;
                VkDeviceSize o;
                RS.volatile_ = 1;
                memcpy(scratch(ex, len - c1, &b, &o), code + c1, len - c1);
                return (VkDescriptorBufferInfo){b, o, len - c1};
            }
        }
    } else if (slot == 17 && st != NVN_STAGE_GS) {
        // Switch 2 full-screen blits read a parameter block the console supplies from bank 24, which the lowering moves
        // to bank 17 (never bound by games). For a blit over the whole render area (Eglantine, ir_opt/position_pass.cpp
        // BlitConstantsPass): texel size at 0x0/0x4 and 0x10/0x14, size at 0x8/0xc, UV scale 1 at 0x18/0x1c, UV clamp
        // rectangle 0,0..1,1 at 0x30-0x3c, zero dither bias, integer source rectangle 0,0..W-1,H-1 at 0x50-0x5c.
        // Bank 24 is NVN 2's graphics root table 0: the words the game pushed (OP_PUSH_CONST) replace the guess
        // (Bananza pushes 1/w, 1/h, w, h). No Bananza compute stage reads bank 24; one that did would get zeros plus
        // the compute pushes, never the graphics guess.
        float w = ex->pass_w ? (float)ex->pass_w : 1.0f, h = ex->pass_h ? (float)ex->pass_h : 1.0f;
        union { float f[64]; uint32_t u[64]; } blk = {{1 / w, 1 / h, w, h, 1 / w, 1 / h, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1}};
        blk.u[22] = (uint32_t)(w - 1), blk.u[23] = (uint32_t)(h - 1);
        // Kirby Air Riders: the only readers of 0x10-0x18 are its two SMAA neighbourhood-blend shaders, which add those
        // words AS INTEGERS to the output colour bits (a dither; zero on the console). Texel size and 1.0 there shift
        // every channel's bits by a different amount: the "rainbow". Same finding and fix in Lichii (zortos293/Lichii
        // PR #56, "white world": the blit dither bias). KAR_BLIT_DITHER=1 keeps the old block for comparison.
        static int kar_dither = -1;
        if (kar_dither < 0) {
            const char *e = getenv("KAR_BLIT_DITHER");
            kar_dither = rt_game_is_kar() && !(e && *e == '1');
        }
        if (kar_dither) blk.u[4] = blk.u[5] = blk.u[6] = blk.u[7] = 0;
        int k = st == NVN_STAGE_CS;
        if (k) memset(&blk, 0, sizeof blk);
        for (int i = 0; i < 64; i++)
            if (ex->push_set[k][0] >> i & 1) blk.u[i] = ex->push[k][0][i];
        VkBuffer b;
        VkDeviceSize o;
        RS.volatile_ = 1;
        memcpy(scratch(ex, sizeof blk, &b, &o), &blk, sizeof blk);
        return (VkDescriptorBufferInfo){b, o, sizeof blk};
    } else if (slot >= 3 && slot - 3 < 18) {
        const p_bind_buf *b = &ex->ubo[st][slot - 3];
        if (b->size && vk_usnap_find(b->addr, b->size < max ? b->size : max, align, &bi)) {   // as of the submit call
            count_use(VK_MCLS_SCRATCH, VK_MUSE_UBO, bi.range);
            return bi;
        }
        if (bind_range(ex, b->addr, b->size, align, max, VK_MUSE_UBO, 0, 0, &bi)) return bi;
    }
    return zero_info(max);
}

// The NVN slot of a storage binding. Ryujinx numbers a shader's storage buffers in the order it meets them
// (GfxShaderRes.slot); the NVN slot is the entry of the driver constant buffer c0 that holds the buffer's address
// (cbuf_slot 0, handle = word offset): 16-byte entries at byte 0x110 (vertex), 0x510 (fragment) and 0x310 (compute).
// A compute address at c0 0x210..0x30f is uniform block (offset - 0x210) / 16 read as storage: *ubo is set. -1: the
// address is not in c0 (the shader loads it from a user uniform block: ubo_ptr) or the stage has no known table.
// Mapping: MacNativeGames games/totk/tools/totk_xlate/Service.cs:206-222 @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou).
static int ssbo_slot(const GfxShaderRes *r, int st, int *ubo) {
    *ubo = 0;
    if (r->cbuf_slot != 0) return -1;
    // Eglantine shader_recompiler/frontend/maxwell/translate_program.cpp::AddNVNStorageBuffers uses a distinct
    // ordinary geometry table at c0+0x410. This only maps declared
    // bindings; it does not synthesize that driver bank or establish the native NVN2 geometry ABI.
    int off = r->handle * 4, base = st == NVN_STAGE_VS ? 0x110 : st == NVN_STAGE_FS ? 0x510 :
                                  st == NVN_STAGE_GS ? 0x410 : st == NVN_STAGE_CS ? 0x310 : -1;
    if (st == NVN_STAGE_CS && off >= 0x210 && off < 0x310 && (off - 0x210) % 16 == 0) {
        *ubo = 1;
        return (off - 0x210) / 16;
    }
    return base >= 0 && off >= base && off < base + 0x100 && (off - base) % 16 == 0 ? (off - base) / 16 : -1;
}

// 1 when the shader loads the storage buffer's address from a user uniform block (cbuf_slot 3.., NVN uniform slot
// cbuf_slot - 3, byte offset handle * 4). The Depths terrain reads its vertices through one, about 500 draws a frame;
// with a zero buffer they collapse and the ground is missing. MacNativeGames resolves these the same way (NVN_RB_SBUFU,
// games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m @ b75be8cf).
static int ubo_ptr_res(const GfxShaderRes *r) {
    return r->cbuf_slot >= 3 && r->cbuf_slot - 3 < 18;
}

// The inline uniform updates of the submission being replayed, newest last (a ring of UPD_N). op_update_ub writes them
// into the buffer's GPU copy; guest memory gets them only from the write-back when the submission ends. The terrain
// rewrites its pointer block this way before every draw, all at one address, so ubo_ptr reads the latest update that
// covers the pointer, else memory. Payloads stay in the recorded command buffer for the whole replay.
#define UPD_N 4096
static struct {
    uint64_t fence;                      // the submission the entries belong to
    uint32_t n;                          // updates noted in it
    struct { uint64_t lo, hi; const uint8_t *data; } u[UPD_N];
} UPD;

static void upd_note(const VkExec *ex, const p_update_ub *u) {
    if (UPD.fence != ex->fence) UPD.fence = ex->fence, UPD.n = 0;
    UPD.u[UPD.n++ % UPD_N] = (typeof(UPD.u[0])){u->dst, u->dst + u->size, u->data};
}

// 1 when an update of this submission supplied [a, a+n).
static int upd_read(const VkExec *ex, uint64_t a, void *out, size_t n) {
    if (UPD.fence != ex->fence) return 0;
    for (uint32_t i = UPD.n, end = UPD.n > UPD_N ? UPD.n - UPD_N : 0; i-- > end;) {
        const typeof(UPD.u[0]) *e = &UPD.u[i % UPD_N];
        if (a >= e->hi || a + n <= e->lo) continue;
        if (a < e->lo || a + n > e->hi) return 0;   // partly covered: memory
        memcpy(out, e->data + (a - e->lo), n);
        return 1;
    }
    return 0;
}

// That address as the bound block holds it at this point of the replay; 0 when the block is unbound or too short. The
// block has no size field.
static uint64_t ubo_ptr(const VkExec *ex, int st, const GfxShaderRes *r) {
    const p_bind_buf *u = &ex->ubo[st][r->cbuf_slot - 3];
    uint64_t off = (uint64_t)r->handle * 4, a = 0;
    VkMemSpan sp;
    if (!u->addr || u->size < off + 8 || upd_read(ex, u->addr + off, &a, 8)) return a;
    if (vk_usnap_read(u->addr + off, &a, 8)) return a;   // as the shaders see it: the copy taken at the submit call
    if (vk_mem_resolve(u->addr + off, 8, &sp)) memcpy(&a, (const void *)(uintptr_t)(u->addr + off), 8);
    return a;
}

// The 8 bytes at byte off of the uniform buffer bound at c[cbuf] of stage st (3..20), as ubo_ptr reads them; 0 when
// unbound, short or unmapped. Texture arrays indexed through a material buffer read their handles this way. The
// bytes come from the submit-time copy the shaders bind (vk_usnap.c) when there is one: the game rewrites per-object
// uniform blocks right after the submit, and reading live memory here gave late draws another object's texture
// handles, so whole runs showed random textures in rainbow colours, depending on the timing.
static uint64_t uw_n[5];   // KAR diagnostic: ubo_word sources (unbound, inline update, snapshot, live, unmapped)
static uint64_t ubo_word(const VkExec *ex, int st, int cbuf, uint64_t off) {
    if (cbuf < 3 || cbuf - 3 >= 18) return 0;
    const p_bind_buf *u = &ex->ubo[st][cbuf - 3];
    uint64_t a = 0;
    VkMemSpan sp;
    static int uw_log = -1;   // KAR_DIAG=1: log the counts every 2^20 reads
    if (uw_log < 0) uw_log = getenv("KAR_DIAG") && *getenv("KAR_DIAG") == '1';
    if ((++uw_n[0] & 0xfffff) == 0 && uw_log)
        rt_log("vk_exec: material handle reads: %llu, inline-update %llu, snapshot %llu, live %llu, unmapped %llu",
               (unsigned long long)uw_n[0], (unsigned long long)uw_n[1], (unsigned long long)uw_n[2],
               (unsigned long long)uw_n[3], (unsigned long long)uw_n[4]);
    if (!u->addr || u->size < off + 8) return 0;
    if (upd_read(ex, u->addr + off, &a, 8)) { uw_n[1]++; return a; }
    if (vk_usnap_read(u->addr + off, &a, 8)) { uw_n[2]++; return a; }   // as the shaders see it: the submit-time copy
    if (vk_mem_resolve(u->addr + off, 8, &sp)) { uw_n[3]++; memcpy(&a, (const void *)(uintptr_t)(u->addr + off), 8); }
    else uw_n[4]++;
    return a;
}

// n words at off of uniform block cbuf, each as ubo_word reads it, in one copy where that gives the same words: no
// inline update in this command buffer touches the range and the submit-time copy holds all of it. Otherwise word by
// word. A material array of 512 handles read per word was most of a draw's binding time (16.4a).
static void ubo_block(const VkExec *ex, int st, int cbuf, uint64_t off, uint32_t n, uint64_t *out) {
    const p_bind_buf *u = cbuf >= 3 && cbuf - 3 < 18 ? &ex->ubo[st][cbuf - 3] : NULL;
    uint32_t have = u && u->addr && u->size > off ? (uint32_t)((u->size - off) / 8 < n ? (u->size - off) / 8 : n) : 0;
    int fast = have > 0;
    if (fast && UPD.fence == ex->fence)
        for (uint32_t i = UPD.n, end = UPD.n > UPD_N ? UPD.n - UPD_N : 0; i-- > end && fast;) {
            const typeof(UPD.u[0]) *e = &UPD.u[i % UPD_N];
            if (u->addr + off < e->hi && u->addr + off + 8ull * have > e->lo) fast = 0;
        }
    if (fast && vk_usnap_read(u->addr + off, out, 8ull * have)) {
        uw_n[0] += have, uw_n[2] += have;
    } else {
        for (uint32_t k = 0; k < have; k++) out[k] = ubo_word(ex, st, cbuf, off + 8ull * k);
    }
    for (uint32_t k = have; k < n; k++) out[k] = 0;
}

// Guarded NVN 2 candidates use the same bytes that the dispatch binds as uniform banks. Bank 0 includes pushed
// roots and packed descriptor tables; bank 1 is the program's bounded compiler-constant data. No scalar is guessed
// to be a pointer here: bind_range determines whether a candidate has backing, and the shader checks the true range.
static p_bind_buf guarded_storage_binding(const VkExec *ex, int st, const GfxShaderRes *r) {
    uint64_t off = (uint64_t)r->handle * 4;
    p_bind_buf b = {0};
    if (st != NVN_STAGE_CS || !c0_lay) return b;
    if (r->cbuf_slot == 0) {
        if (!c0_on() || off > VK_C0_BYTES - 8) return b;
        uint8_t bank[VK_C0_BYTES];
        vk_c0_fill(bank, st, ex->ssbo[st], ex->ubo[st], c0_dim, c0_binst);
        memcpy(&b.addr, bank + off, 8);
        size_t ub = nvn2_c0_part(c0_lay, 1), end = nvn2_c0_part(c0_lay, 3);
        if (off >= ub && off < end && (off - ub) % 16 == 0) {
            uint32_t bytes;
            memcpy(&bytes, bank + off + 8, 4);
            b.size = bytes;   // preserve a typed table's declared extent, not the remainder of its pool
            // KAR_GUARDED_FULL=1 (diagnostic): bind up to the end of the backing instead, as the console's unbounded
            // global stores reach past the declared extent (Kirby Air Riders' light-record writer binds 0x80 bytes).
            static int full = -1;
            if (full < 0) full = getenv("KAR_GUARDED_FULL") && *getenv("KAR_GUARDED_FULL") == '1';
            if (full) b.size = 0;
            static int64_t gmin = -1;   // KAR_GUARDED_MIN=<bytes> (diagnostic): a smaller declared extent binds that much
            if (gmin < 0) gmin = getenv("KAR_GUARDED_MIN") ? strtoll(getenv("KAR_GUARDED_MIN"), NULL, 0) : 0;
            if (gmin > 0 && b.size && b.size < (uint64_t)gmin) b.size = (uint64_t)gmin;
        }
    } else if (r->cbuf_slot == 1 && ex->prog) {
        const uint8_t *ctl, *code;
        uint32_t len, c1;
        if (!find_stage(ex->prog, st, &ctl, &code, &len)) return b;
        memcpy(&c1, ctl + 0x700, 4);
        if (c1 > len || off > len - c1 || len - c1 - off < 8) return b;
        memcpy(&b.addr, code + c1 + off, 8);
    } else if (r->cbuf_slot == 17) {
        // ubo_info binds the synthetic compute push bank here, before its user-UBO mapping. Unwritten words are zero.
        if (off > sizeof ex->push[1][0] - 8) return b;
        uint32_t words[2] = {0};
        for (unsigned i = 0; i < 2; i++) {
            unsigned word = r->handle + i;
            if (ex->push_set[1][0] >> word & 1) words[i] = ex->push[1][0][word];
        }
        memcpy(&b.addr, words, 8);
    } else if (ubo_ptr_res(r)) {
        b.addr = ubo_ptr(ex, st, r);
    }
    return b;
}

static uint64_t kirby_watch[2];   // KAR_DIAG_SSBO_WRITE=kirby: address and size, set by diag_kirby_vs

static VkDescriptorBufferInfo ssbo_info(VkExec *ex, int st, const GfxShaderRes *r, int written) {
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    uint64_t max = LIM.sbo_max, align = LIM.sbo_align;
    VkDescriptorBufferInfo bi;
    int ubo = 0, slot = -1;
    p_bind_buf up = {0};
    const p_bind_buf *b = NULL;
    int guarded = !!(r->flags & GFX_RES_SSBO_GUARDED);
    if (!guarded) slot = ssbo_slot(r, st, &ubo);
    if (guarded) up = guarded_storage_binding(ex, st, r), b = &up;
    else if (slot >= 0 && slot < 16) b = ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot];
    // KAR_ALT_SSBO=<frame> (diagnostic): from that frame, in steps of 100 frames, step k binds the zero buffer for
    // fragment storage slot k during its second 50 frames (k = 0..15), so a buffer that causes a fault switches it off
    // and on in step, at the same track position.
    // KAR_ALT_SSBO=<frame>:<slot>: that one slot only, off in every second block of 50 frames from <frame> on.
    static int64_t alt_at = -2;
    static int alt_slot = -1;
    if (alt_at == -2) {
        const char *e = getenv("KAR_ALT_SSBO");
        alt_at = e ? atoll(e) : -1;
        if (e && strchr(e, ':')) alt_slot = atoi(strchr(e, ':') + 1);
    }
    static int alt_st = -1;   // KAR_ALT_SSBO_ST=<nvn stage> (default fragment): the stage KAR_ALT_SSBO acts on
    if (alt_st < 0) alt_st = getenv("KAR_ALT_SSBO_ST") ? atoi(getenv("KAR_ALT_SSBO_ST")) : NVN_STAGE_FS;
    if (alt_at >= 0 && st == alt_st && !ubo && !guarded && slot >= 0) {
        int64_t t = (int64_t)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) - alt_at;
        if (alt_slot >= 0 ? t >= 0 && slot == alt_slot && t / 50 % 2 : t >= 0 && t / 100 == slot && t % 100 >= 50)
            return zero_info(max);
    }
    else if (ubo_ptr_res(r)) up.addr = ubo_ptr(ex, st, r), b = &up;   // size 0: up to the end of the backing
    // KAR_SSBO_MIN=<bytes> (diagnostic): a storage buffer the game binds smaller is bound at least that large (bind_range
    // clamps to the backing). NVN 2 shaders read storage buffers with plain LDG, which has no bound: Kirby Air Riders'
    // character shader reads light-zone records at zone * 320 from a buffer the game binds 320 bytes large.
    static int64_t ssbo_min = -1;
    if (ssbo_min < 0) {
        const char *e = getenv("KAR_SSBO_MIN");
        ssbo_min = e ? strtoll(e, NULL, 0) : 0;
    }
    if (ssbo_min > 0 && b && !ubo && !guarded && b->size && b->size < (uint64_t)ssbo_min) {
        if (b != &up) up = *b, b = &up;
        up.size = (uint64_t)ssbo_min;
    }
    if (b && !written && !guarded && !ubo && b->size && (st == NVN_STAGE_VS || st == NVN_STAGE_FS)) {
        // read-only vertex / fragment storage: the copy taken at the submit call (vk_usnap.c KAR_SSBO_SNAPSHOT)
        uint64_t lead = vk_caps.sbuf_align ? b->addr & (vk_caps.sbuf_align - 1) : 0;
        if (vk_usnap_find(b->addr - lead, b->size + lead < max ? b->size + lead : max, align, &bi)) return bi;
    }
    if (b) {
        // vk_caps.c: shaders translated for sbuf_align address the buffer from its address rounded down to it.
        uint64_t lead = vk_caps.sbuf_align ? b->addr & (vk_caps.sbuf_align - 1) : 0;
        if (bind_range(ex, b->addr - lead, b->size ? b->size + lead : 0, align, max, ubo ? VK_MUSE_UBO : VK_MUSE_SSBO,
                       written, guarded, &bi)) {
            if (written && bi.buffer != VK_NULL_HANDLE) {
                // KAR_DIAG_SSBO_WRITE=<hex address> (diagnostic): log once per program each one that binds a written
                // storage buffer covering that address (who fills Kirby Air Riders' light-zone table).
                // KAR_DIAG_SSBO_WRITE=kirby: the buffer Kirby's body vertex shader last bound at vertex slot 6 (its
                // per-object local light records, diag_kirby_vs), whose address changes between runs.
                static uint64_t dw = 1, dw_len = 1;
                if (dw == 1) {
                    const char *e = getenv("KAR_DIAG_SSBO_WRITE");
                    dw = !e ? 0 : *e == 'k' ? 2 : strtoull(e, NULL, 16);
                    if (e && *e != 'k' && strchr(e, ':')) dw_len = strtoull(strchr(e, ':') + 1, NULL, 16);   // <addr>:<len>
                }
                uint64_t w0 = dw == 2 ? __atomic_load_n(&kirby_watch[0], __ATOMIC_RELAXED) : dw;
                uint64_t w1 = dw == 2 ? w0 + __atomic_load_n(&kirby_watch[1], __ATOMIC_RELAXED) : dw + dw_len;
                if (w0 && w0 < b->addr - lead + bi.range && b->addr - lead < w1 && ex->prog) {
                    static uint64_t seen[64];
                    static int nseen;
                    int k = 0;
                    while (k < nseen && seen[k] != ex->prog->serial) k++;
                    if (k == nseen && nseen < 64) {
                        seen[nseen++] = ex->prog->serial;
                        rt_log("ssbo write: program %llu stage %d%s binds %#llx+%#llx written (covers %#llx), frame %llu",
                               (unsigned long long)ex->prog->serial, st, guarded ? " guarded" : "", (unsigned long long)b->addr,
                               (unsigned long long)b->size, (unsigned long long)w0,
                               (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
                    }
                }
                note_written(ex, b->addr - lead, bi.range);
                const uint8_t *ctl, *code;
                uint32_t len;
                if (vk_sync_noting() && ex->prog && find_stage(ex->prog, st, &ctl, &code, &len))
                    vk_sync_note_write(b->addr - lead, bi.range, nvn2_origin(code));
            }
            return bi;
        }
        if (b->addr && !guarded) warn_once(W_SSBO_ALIGN, "storage buffer %#llx is not backed or not aligned: a zero buffer is bound",
                               (unsigned long long)b->addr);
    } else {
        warn_once(W_SSBO_SLOT, "a storage buffer whose address is at c%d+%#x (stage %d) is not replayed: a zero buffer is bound",
                  r->cbuf_slot, r->handle * 4, st);
    }
    bi = zero_info(max);
    // Vulkan requires a nonzero descriptor range. One byte is zero elements of the shader's uint runtime array;
    // the guarded operation branches away without touching it. Unguarded users retain the existing 64 KiB dummy.
    if (guarded) bi.range = 1;
    return bi;
}

// ---- feedback loops (maxwell.md 1.9, vk.h VkCtx.feedback)
// TotK's G-buffer and terrain shaders sample targets of the pass that renders them, at their own pixel, after earlier
// draws of the pass wrote them (albedo, normals, R32F linear depth). Maxwell reads see those writes; in Vulkan the read
// is undefined without a barrier, and on the RX 9070 it showed as dark screen-aligned blocks on the island ground
// and hid Tulin's sage spirit. Before a draw whose fragment shader samples an attachment of
// the open pass that a draw or clear of the pass wrote since the pass began or the last barrier:
// - VK_FEEDBACK_LAYOUT: the image is learned (VkTexImage.fb_learned; it has the feedback usage since its creation), the
//   pass restarts once with it in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT, and from then on its passes attach it in that
//   layout, its descriptors carry that layout, the draws that sample it use a pipeline with the feedback flag
//   (VkPipeState.feedback) and a by-region barrier precedes such a draw.
// - VK_FEEDBACK_RESTART: the pass ends and restarts behind the full barrier (vk_exec_pass_begin).
static int in_fbl(const VkExec *ex, VkImage img) {
    for (int i = 0; i < ex->nfbl; i++)
        if (ex->fbl_img[i] == img) return 1;
    return 0;
}

// The image and the levels [l0, l1) and layers [a0, a1) a texture's view covers.
typedef struct { VkImage image; uint32_t l0, l1, a0, a1; } SubRange;

static SubRange sub_of(const VkTexImage *img, const view_t *v, int hv) {
    SubRange r = {img->image, 0, img->levels, 0, img->layers};
    if (hv && v->set_levels) r.l0 = v->base_level, r.l1 = v->base_level + (v->nlevels ? v->nlevels : img->levels);
    if (hv && v->set_layers) r.a0 = v->min_layer, r.a1 = v->min_layer + (v->nlayers ? v->nlayers : img->layers);
    return r;
}

// Attachment slots of the open pass (bit 8 depth) whose subresource r covers.
static uint32_t attached_as(const VkExec *ex, const SubRange *r) {
    uint32_t m = 0;
    if (!r->image) return 0;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || t->image != r->image) continue;
        if (t->level < r->l0 || t->level >= r->l1) continue;
        if (!t->is3d && (t->base_layer + t->layers <= r->a0 || t->base_layer >= r->a1)) continue;
        m |= 1u << i;
    }
    return m;
}

// TOTK_TEX_MEMO (tex_info, feedback_scan): 0 off, 1 on, 2 (=check) computes every hit again and stops on a difference.
static int td_mode(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_value("TOTK_TEX_MEMO");
        on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
    }
    return on;
}

// The subresource a texture handle samples, per handle, kept while its slot resolves the same (slot_still): a draw's
// texture bindings change the whole-scan memo below, but the handles they bind were mostly looked up before, in this
// pass or an earlier one. Direct-mapped.
#define FM_N 8192
static struct { uint64_t context, h, tok; uint32_t gen; SubRange r; } FM[FM_N];

static SubRange sampled_sub(const VkExec *ex, uint64_t h) {   // texture lock held
    view_t v;
    int hv;
    const VkTexImage *img = vk_texture_by_id_locked(&ex->tex_pool, (int)(h & 0xfffff), &v, &hv);
    return img ? sub_of(img, &v, hv) : (SubRange){0};
}

// Attachments of the open pass the fragment shader fs samples; kept while the shader, the texture bindings and the
// pass stay the same.
static uint32_t feedback_scan(VkExec *ex, const GfxShader *fs) {
    descriptor_state(ex);
    if (ex->fb_fs == fs && ex->fb_serial == ex->tex_serial && ex->fb_pass == ex->pass_serial) return ex->fb_sampled;
    const GfxShaderRes *res;
    int n = gfx_shader_resources(fs, GFX_RES_TEX, &res);
    int st = nvn_stage_of(gfx_shader_stage(fs));
    if (st < 0) return 0;
    uint32_t m = 0, gen = vk_texture_slots_gen();   // before any lookup
    int memo = td_mode(), locked = 0;
    for (int r = 0; r < n; r++)
        for (int k = 0; k < (res[r].count ? res[r].count : 1); k++) {
            int slot = res[r].slot + k;
            uint64_t h = slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0;
            if (!h) continue;
            typeof(FM[0]) *f = &FM[(h * 0x9E3779B97F4A7C15ull) >> 51];
            if (memo && f->context == descriptor_serial && f->h == h && slot_still(ex, &f->gen, f->tok, h, gen)) {
                if (memo == 2) {
                    if (!locked) vk_texture_lock(), locked = 1;
                    SubRange c = sampled_sub(ex, h);
                    if (memcmp(&c, &f->r, sizeof c))
                        rt_fatal("vk_exec: feedback memo of handle %#llx differs from a lookup", (unsigned long long)h);
                }
                m |= attached_as(ex, &f->r);
                continue;
            }
            uint64_t tok = token_of(ex, h);   // before the lookup
            if (!locked) vk_texture_lock(), locked = 1;
            SubRange sr = sampled_sub(ex, h);
            m |= attached_as(ex, &sr);
            if (tok) *f = (typeof(*f)){.context = descriptor_serial, .h = h, .tok = tok, .gen = gen, .r = sr};
            else f->h = 0;
        }
    if (locked) vk_texture_unlock();
    ex->fb_fs = fs, ex->fb_serial = ex->tex_serial, ex->fb_pass = ex->pass_serial, ex->fb_sampled = m;
    return m;
}

static int geometry_feedback_safe(VkExec *ex, const GfxShader *vs, const GfxShader *gs) {
    // The existing in-pass feedback dependency is fragment-only. Pre-raster reads of an attachment need a different
    // synchronization contract; reject them rather than reuse that by-region fragment barrier.
    if (gs && (feedback_scan(ex, vs) || feedback_scan(ex, gs))) {
        warn_once(W_GEOMETRY, "experimental geometry draw samples an attachment before rasterization: skipped");
        return 0;
    }
    return 1;
}

// Before the pipeline lookup of a draw with fragment shader fs: learns and restarts as the mode says. Returns the
// sampled attachments in the feedback layout (the pipeline's feedback flag and feedback_barrier).
static uint32_t feedback_prepare(VkExec *ex, const GfxShader *fs) {
    int mode = vk_ctx()->feedback;
    if (mode == VK_FEEDBACK_OFF || !fs || !ex->in_pass) return 0;
    uint32_t m = feedback_scan(ex, fs);
    uint32_t d = m & ex->fb_dirty;
    if (d) vk_exec_frame.fb_draws++;
    if (d & ~(mode == VK_FEEDBACK_LAYOUT ? ex->fbl_mask : 0)) {
        if (mode == VK_FEEDBACK_LAYOUT)
            for (uint32_t i = 0; i < 9; i++) {
                const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
                if ((d >> i & 1) && t->img && t->img->fb_usage) ((VkTexImage *)t->img)->fb_learned = 1;
            }
        vk_exec_pass_end(ex);
        vk_exec_pass_begin(ex);   // learned targets now in the feedback layout; the full barrier cleared fb_dirty
        vk_exec_frame.fb_restarts++;
    }
    return mode == VK_FEEDBACK_LAYOUT ? m & ex->fbl_mask : 0;
}

// By-region barrier inside the pass: earlier attachment writes become visible to this draw's fragment shader reads.
static void feedback_barrier(VkExec *ex) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
                           .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
    ex->fb_dirty = 0;
    vk_exec_frame.fb_barriers++;
}

// Marks the attachments the draw with state s writes: colour targets with a nonzero effective write mask, depth with
// depth writes or the stencil test on.
static void feedback_written(VkExec *ex, const VkPipeState *s) {
    if (vk_ctx()->feedback == VK_FEEDBACK_OFF) return;
    if (ex->depth.image && !(ex->fb_dirty & 0x100) && ((s->ds.dt && s->ds.dw) || s->ds.st)) ex->fb_dirty |= 0x100;
    for (uint32_t i = 0; i < ex->pass_ncolor; i++)
        if (ex->color[i].image && !(ex->fb_dirty >> i & 1) && vk_pipeline_target_wmask(s, (int)i)) ex->fb_dirty |= 1u << i;
}

// A 1x1 depth image at 1.0 for compare samplers whose texture is not a depth format. Maxwell compares the red channel
// of any texture; Vulkan leaves a compare on a format without SAMPLED_IMAGE_DEPTH_COMPARISON undefined
// (VUID-vkCmdDraw-None-06479), and the NVIDIA driver returned values that changed from run to run: Kirby Air Riders'
// material arrays are declared both 2D and 2D-shadow over the same handle table (fp_t2ds_cb196613_...), so their
// colour textures sat in the shadow declaration too, and about every second run drew random rainbow surfaces. A depth
// of 1.0 compares as "lit" for the usual less-or-equal test. Views: 2D, 2D array, cube, cube array (6 layers).
// shadow_dummy_at(vt, 1) is a second stand-in at 0.0, used only by the KAR_ALT_SHADOW diagnostic.
static struct { VkImage img; VmaAllocation mem; VkImageView v[4]; int tried; } SZS[2];
static VkImageView shadow_dummy_at(VkImageViewType vt, int zero) {
#define SZ SZS[zero]
    if (!SZ.tried) {
        SZ.tried = 1;
        VkCtx *c = vk_ctx();
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT, .extent = {1, 1, 1},
                                .mipLevels = 1, .arrayLayers = 6, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &SZ.img, &SZ.mem, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
        static const VkImageViewType vts[4] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE,
                                               VK_IMAGE_VIEW_TYPE_CUBE_ARRAY};
        for (int k = 0; k < 4; k++) {
            VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = SZ.img, .viewType = vts[k],
                                        .format = VK_FORMAT_D32_SFLOAT,
                                        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, k == 0 ? 1u : k == 1 ? 1u : 6u}};
            if (vkCreateImageView(c->device, &vi, NULL, &SZ.v[k]) != VK_SUCCESS) SZ.v[k] = VK_NULL_HANDLE;
        }
        VkCommandBuffer cb = vk_upload_begin();
        VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = SZ.img, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 6}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
        vkCmdPipelineBarrier2(cb, &di);
        VkClearDepthStencilValue one = {zero ? 0.0f : 1.0f, 0};
        VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 6};
        vkCmdClearDepthStencilImage(cb, SZ.img, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &rg);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT};
        VkDependencyInfo d2 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &d2);
        vk_upload_end();
        rt_log("vk_exec: compare samplers over colour textures read a 1x1 depth stand-in at %s", zero ? "0.0" : "1.0");
    }
    int k = vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? 1 : vt == VK_IMAGE_VIEW_TYPE_CUBE ? 2 : vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? 3 :
            vt == VK_IMAGE_VIEW_TYPE_2D ? 0 : -1;
    return k >= 0 ? SZ.v[k] : VK_NULL_HANDLE;
#undef SZ
}
static VkImageView shadow_dummy(VkImageViewType vt) { return shadow_dummy_at(vt, 0); }

static void tex_info_full(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate,
                          VkDescriptorImageInfo *out, VkImage *image);

// Sampled-texture descriptors per (handle, resource type, separate), kept while nothing they derive from changed: the
// slot's resolution and texture entry (slot_still) and the samplers (vk_sampler_gen); the layout is set again on every
// hit from the images in the feedback layout (in_fbl of the image kept with it). The first binding of a slot in a submission (vk_texture_epoch) still makes the slot lookup
// for its storage check; a re-upload writes the same image, so the descriptor stays. Draws bind the same textures
// thousands of times a frame (15,000 lookups a frame in the monsters fight); a hit skips the slot lookup, the view key
// with its hash probe and the sampler lookup. Storage images, which note their binding per submission, are
// not kept. Direct-mapped; texture lock held. TOTK_TEX_MEMO=0 turns it off; =check computes every hit again and stops
// on a difference.
#define TD_N 8192
static struct {
    uint64_t context, h, tok;
    uint32_t gen, epoch, sgen;
    uint16_t type;
    uint8_t separate, valid;
    VkImage image;
    VkDescriptorImageInfo out;
} TD[TD_N];

static void tex_info_lookup(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate,
                            VkDescriptorImageInfo *out) {
    descriptor_state(ex);
    int td_on = td_mode();
    VkImage image;
    if (!h || storage || !td_on) return tex_info_full(ex, r, h, storage, separate, out, &image);
    uint32_t gen = vk_texture_slots_gen(), ep = vk_texture_epoch(), sg = vk_sampler_gen();   // before any lookup
    typeof(TD[0]) *m = &TD[(h * 0x9E3779B97F4A7C15ull ^ (uint64_t)r->type << 7 ^ (uint64_t)separate) >> 51];
    if (m->valid && m->context == descriptor_serial && m->h == h && m->type == r->type && m->separate == separate && m->sgen == sg &&
        slot_still(ex, &m->gen, m->tok, h, gen)) {
        if (m->epoch != ep) {   // the slot's first binding in this submission: only its storage check is due
            vk_texture_by_id_locked(&ex->tex_pool, (int)(h & 0xfffff), NULL, NULL);
            m->epoch = ep;
        }
        *out = m->out;
        out->imageLayout = m->image && ex->nfbl && in_fbl(ex, m->image) ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                                          : VK_IMAGE_LAYOUT_GENERAL;
        if (td_on == 2) {
            VkDescriptorImageInfo full;
            tex_info_full(ex, r, h, storage, separate, &full, &image);
            if (full.sampler != out->sampler || full.imageView != out->imageView || full.imageLayout != out->imageLayout)
                rt_fatal("vk_exec: texture descriptor memo of handle %#llx type %#x differs from a lookup",
                         (unsigned long long)h, r->type);
        }
        return;
    }
    uint64_t tok = token_of(ex, h);   // before the lookup
    tex_info_full(ex, r, h, storage, separate, out, &image);
    *m = (typeof(*m)){.context = descriptor_serial, .h = h, .tok = tok, .gen = gen, .epoch = ep, .sgen = sg, .type = r->type,
                      .separate = (uint8_t)separate, .valid = 1, .image = image, .out = *out};
}

static void tex_info_full(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate,
                          VkDescriptorImageInfo *out, VkImage *image) {
    int ms = (r->type & 0x200) != 0;
    *image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler samp = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    if (h) {
        view_t v;
        int hv;
        const VkTexImage *img = vk_texture_by_id_locked(&ex->tex_pool, (int)(h & 0xfffff), &v, &hv);
        if (img) view = get_view(ex, img, &v, hv, r->type, storage), *image = img->image;
        // KAR_DIAG_PROBE=1 (diagnostic; run with TOTK_TEX_MEMO=0): every change of the 12x12 cube a shader samples
        // (Kirby Air Riders' character light probe), with its storage address, to see whether the game switches
        // probes between areas (the cave glow).
        static int diag_probe = -1;
        if (diag_probe < 0) diag_probe = getenv("KAR_DIAG_PROBE") != NULL;
        if (diag_probe && img && !storage && img->width == 12 && img->height == 12 && img->layers >= 6) {
            static uint64_t last;
            static volatile int32_t told;
            if (img->addr != last && plat_inc32(&told) <= 200) {
                last = img->addr;
                rt_log("vk_exec: probe cube now tex %u at %#llx (format %d, mirrored %u gpu_owned %u), frame %llu",
                       (unsigned)(h & 0xfffff), (unsigned long long)img->addr, (int)img->format, img->mirrored,
                       img->gpu_owned, (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
            }
        }
        if (img && storage) vk_texture_storage_bound_locked(img), vk_exec_rt_note_storage(img);
        if (img && !storage && ex->nfbl && in_fbl(ex, img->image)) layout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
        if (!storage && !separate) {
            // A depth texture sampled without a compare reads one texel on the Switch, whatever the sampler's filter:
            // blending neighbouring depths puts reconstructed positions in mid-air at object edges (shadow masks).
            int sid = (int)(h >> 20 & 0xfff);
            int point = img && (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && !(r->type & 0x400) &&
                        vk_exec_view_aspect(img, &v, hv) != VK_IMAGE_ASPECT_STENCIL_BIT;
            samp = point ? vk_sampler_point_by_id_locked(&ex->samp_pool, sid) : vk_sampler_by_id_locked(&ex->samp_pool, sid);
        }
    }
    // A compare (shadow) declaration over a colour image or no image: the depth stand-in (shadow_dummy). The compute
    // path's depth twin (tex_info) may still replace it with the texels of an R32F/R16 image afterwards.
    if (!storage && !separate && (r->type & 0x400) && !ms) {
        const VkTexImage *img = h ? vk_texture_by_id_locked(&ex->tex_pool, (int)(h & 0xfffff), NULL, NULL) : NULL;
        if (!img || !(img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
            VkImageView dv = shadow_dummy(res_view_type(r->type));
            if (dv) view = dv, layout = VK_IMAGE_LAYOUT_GENERAL;
        }
    }
    // KAR_ALT_SHADOW=<frame>:<p|c>[:<width>] (diagnostic): from <frame>, in blocks of 50 frames (KAR_ALT_SHADOW_BLOCK)
    // that cycle real /
    // 1.0 / 0.0, a depth image read through a plain (p) or compare (c) declaration gets the 1.0 or the 0.0 depth
    // stand-in instead; with <width>, only depth images of that width. Shows whether a depth map drives a shader's
    // light (Kirby Air Riders' 1024x1024 map in fs slot 2: the cave glow). Run it with TOTK_TEX_MEMO=0, so a kept
    // descriptor does not skip the swap.
    static int64_t as_at = -2;
    static int as_cmp;
    static uint32_t as_w;
    static int as_blk = 50;
    if (as_at == -2) {
        const char *e = getenv("KAR_ALT_SHADOW");
        as_at = e ? atoll(e) : -1;
        const char *c1 = e ? strchr(e, ':') : NULL, *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        as_cmp = c1 && c1[1] == 'c';
        as_w = c2 ? (uint32_t)atoi(c2 + 1) : 0;
        const char *bl = getenv("KAR_ALT_SHADOW_BLOCK");   // block length in frames (default 50)
        as_blk = bl && atoi(bl) > 0 ? atoi(bl) : 50;
    }
    if (as_at >= 0 && view && h && !storage && !separate && !ms && !!(r->type & 0x400) == as_cmp) {
        int64_t t = (int64_t)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) - as_at;
        const VkTexImage *img = vk_texture_by_id_locked(&ex->tex_pool, (int)(h & 0xfffff), NULL, NULL);
        int phase = t >= 0 ? (int)(t / as_blk % 3) : 0;
        if (phase && img && (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && (!as_w || img->width == as_w)) {
            VkImageView dv = shadow_dummy_at(res_view_type(r->type), phase == 2);
            if (dv) view = dv, layout = VK_IMAGE_LAYOUT_GENERAL;
        }
    }
    // KAR_ALT_GPUBC=<frame> (diagnostic): from <frame>, in every second block of 50 frames, a block-compressed texture
    // the CPU does not mirror (filled by GPU copies) is replaced by the dummy texture.
    static int64_t ab_at = -2;
    if (ab_at == -2) ab_at = getenv("KAR_ALT_GPUBC") ? atoll(getenv("KAR_ALT_GPUBC")) : -1;
    if (ab_at >= 0 && view && h && !storage) {
        int64_t t = (int64_t)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) - ab_at;
        const VkTexImage *img = vk_texture_by_id_locked(&ex->tex_pool, (int)(h & 0xfffff), NULL, NULL);
        if (t >= 0 && t / 50 % 2 && img && !img->mirrored && img->format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK &&
            img->format <= VK_FORMAT_BC7_SRGB_BLOCK)
            view = VK_NULL_HANDLE;
    }
    if (!view) {
        int di = dummy_index(res_view_type(r->type), ms);
        view = storage ? storage_dummy(di, r->format) : Z.img[di].view;
    }
    if (!samp) samp = r->type & 0x400 ? Z.samp_shadow : Z.samp;
    *out = (VkDescriptorImageInfo){separate || storage ? VK_NULL_HANDLE : samp, view, layout};
}

// ---- depth twins: colour images under a compare sampler in a compute shader
// TotK's compute passes sample an R32_SFLOAT texture through a shadow sampler. Maxwell compares any single-channel
// float or unorm texel; Vulkan only formats with SAMPLED_IMAGE_DEPTH_COMPARISON (VUID-vkCmdDispatch-None-06479), which
// AMD's drivers do not give R32_SFLOAT, so the result there is undefined. Such an image gets a depth twin of the same
// size and bits (R32_SFLOAT -> D32_SFLOAT, R16_UNORM -> D16_UNORM), copied through a buffer right before each dispatch
// that samples it (a dispatch is outside any render pass), and the dispatch samples the twin. Draws keep the colour
// image: a copy cannot go inside their pass. Design: Savage Wind Recomp runtime/nvn/vulkan/exec.c depth_twin_update
// (https://github.com/Revivedx/SavageWindRecomp, MIT); the code is ours. TOTK_DEPTH_TWIN=0: off.
#define TWINS 8
typedef struct { VkImage src, img; VmaAllocation mem; VkImageView view[2]; uint32_t w, h, layers; VkFormat fmt; } Twin;
static Twin twins[TWINS];
static unsigned twin_next;
static int twin_dispatch;                // op_dispatch*: the resources being bound are a dispatch's
static VkBuffer twin_buf;
static VmaAllocation twin_buf_mem;
static VkDeviceSize twin_buf_cap;

static void twin_free(void *arg) {   // completion thread, after the last submission that may sample it
    Twin *t = arg;
    VkCtx *c = vk_ctx();
    for (int k = 0; k < 2; k++)
        if (t->view[k]) vkDestroyImageView(c->device, t->view[k], NULL);
    vmaDestroyImage(c->vma, t->img, t->mem);
    free(t);
}

typedef struct { VkBuffer buf; VmaAllocation mem; } TwinBuf;
static void twin_buf_free(void *arg) {   // completion thread, after the last submission that may copy through it
    TwinBuf *b = arg;
    vmaDestroyBuffer(vk_ctx()->vma, b->buf, b->mem);
    free(b);
}

static VkFormat twin_format(const VkTexImage *img) {
    static int on = -1;
    if (on < 0) on = rt_knob("DEPTH_TWIN", "1");
    if (!on || (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || img->samples > 1 || img->depth > 1) return VK_FORMAT_UNDEFINED;
    VkFormat df = img->format == VK_FORMAT_R32_SFLOAT ? VK_FORMAT_D32_SFLOAT
                : img->format == VK_FORMAT_R16_UNORM  ? VK_FORMAT_D16_UNORM : VK_FORMAT_UNDEFINED;
    if (!df) return df;
    static int compares[2] = {-1, -1};   // per source format: the driver compares it as it is
    int i = df == VK_FORMAT_D16_UNORM;
    if (compares[i] < 0) {
        VkFormatProperties3 p3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &p3};
        vkGetPhysicalDeviceFormatProperties2(vk_ctx()->phys, img->format, &p2);
        compares[i] = (p3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT) != 0;
        rt_log("vk_exec: %s under a compare sampler: %s", i ? "R16_UNORM" : "R32_SFLOAT",
               compares[i] ? "compared by the driver" : "sampled through a depth twin in compute passes");
    }
    return compares[i] ? VK_FORMAT_UNDEFINED : df;
}

static void twins_release(VkCtx *c) {
    for (int i = 0; i < TWINS; i++) {
        for (int k = 0; k < 2; k++)
            if (twins[i].view[k]) vkDestroyImageView(c->device, twins[i].view[k], NULL);
        if (twins[i].img) vmaDestroyImage(c->vma, twins[i].img, twins[i].mem);
    }
    memset(twins, 0, sizeof twins);
    if (twin_buf) vmaDestroyBuffer(c->vma, twin_buf, twin_buf_mem);
    twin_buf = VK_NULL_HANDLE, twin_buf_mem = NULL, twin_buf_cap = 0;
}

// The twin of img holding img's current texels, the copies recorded into ex->cb; NULL when img needs none or it cannot
// be made.
static Twin *twin_update(VkExec *ex, const VkTexImage *img) {
    VkFormat df = twin_format(img);
    if (!df) return NULL;
    VkCtx *c = vk_ctx();
    uint32_t bpp = df == VK_FORMAT_D32_SFLOAT ? 4 : 2, layers = img->layers ? img->layers : 1;
    VkDeviceSize bytes = (VkDeviceSize)img->width * img->height * layers * bpp;
    if (bytes > (64u << 20)) return NULL;
    if (bytes > twin_buf_cap) {   // one copy buffer, grown; the old one goes after the submission being recorded
        VkDeviceSize want = twin_buf_cap ? twin_buf_cap * 2 : 1u << 16;
        while (want < bytes) want *= 2;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = want,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer nb;
        VmaAllocation nm;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &nb, &nm, NULL) != VK_SUCCESS) return NULL;
        if (twin_buf) {
            TwinBuf *old = malloc(sizeof *old);
            if (!old) rt_fatal("vk: out of memory (depth twin)");
            *old = (TwinBuf){twin_buf, twin_buf_mem};
            vk_completion_add(vk_sync_issued() + 1, twin_buf_free, old);
        }
        twin_buf = nb, twin_buf_mem = nm, twin_buf_cap = want;
    }
    Twin *t = NULL;
    for (int i = 0; i < TWINS && !t; i++)
        if (twins[i].img && twins[i].src == img->image && twins[i].w == img->width && twins[i].h == img->height &&
            twins[i].layers == layers && twins[i].fmt == df)
            t = &twins[i];
    if (!t) {
        t = &twins[twin_next++ % TWINS];
        if (t->img) {   // the submission being recorded may still sample the replaced twin
            Twin *old = malloc(sizeof *old);
            if (!old) rt_fatal("vk: out of memory (depth twin)");
            *old = *t;
            vk_completion_add(vk_sync_issued() + 1, twin_free, old);
        }
        *t = (Twin){.src = img->image, .w = img->width, .h = img->height, .layers = layers, .fmt = df};
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = df,
                                .extent = {img->width, img->height, 1}, .mipLevels = 1, .arrayLayers = layers,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &t->img, &t->mem, NULL) != VK_SUCCESS) {
            *t = (Twin){0};
            return NULL;
        }
        for (int k = 0; k < 2; k++) {
            VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img,
                                        .viewType = k ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, .format = df,
                                        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, k ? layers : 1}};
            if (vkCreateImageView(c->device, &vi, NULL, &t->view[k]) != VK_SUCCESS) t->view[k] = VK_NULL_HANDLE;
        }
        static int nlog;
        if (nlog++ < 8)
            rt_log("vk_exec: depth twin %ux%ux%u %s for a compute pass's compare sampler", img->width, img->height,
                   layers, df == VK_FORMAT_D32_SFLOAT ? "D32_SFLOAT" : "D16_UNORM");
    }
    // The source's earlier writes and the twin's earlier readers before the copies; the twin's old texels are dropped.
    VkMemoryBarrier2 m0 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = t->img, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers}};
    VkDependencyInfo d0 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m0,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(ex->cb, &d0);
    VkBufferImageCopy src = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers},
                             .imageExtent = {img->width, img->height, 1}};
    vkCmdCopyImageToBuffer(ex->cb, img->image, VK_IMAGE_LAYOUT_GENERAL, twin_buf, 1, &src);
    VkMemoryBarrier2 m1 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo d1 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m1};
    vkCmdPipelineBarrier2(ex->cb, &d1);
    VkBufferImageCopy dst = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, layers},
                             .imageExtent = {img->width, img->height, 1}};
    vkCmdCopyBufferToImage(ex->cb, twin_buf, t->img, VK_IMAGE_LAYOUT_GENERAL, 1, &dst);
    // The twin's texels before the dispatch; the source's next writers and the buffer's next copy after these copies.
    VkMemoryBarrier2 m2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d2 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m2};
    vkCmdPipelineBarrier2(ex->cb, &d2);
    ex->recorded += 5;
    return t;
}

// The image descriptor of slot h; in a dispatch, a compare-sampled colour image the driver cannot compare is swapped
// for its depth twin.
// Missing-upscale fallback (Kirby Air Riders; KAR_UPSCALE_FALLBACK=0 turns it off). The game's final composite samples
// the output of an upscaler the host does not run (DLSS: a 1920x1080 rgb10a2 texture nothing ever writes, from the
// 1536x864 rgb10a2 scene). A sampled colour image that no GPU work, upload or copy ever wrote is filled, once per frame,
// by a linear blit of the newest smaller render target of its format and aspect ratio from the last two frames.
static int upscale_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("KAR_UPSCALE_FALLBACK");
        on = e ? *e != '0' : rt_game_is_kar();
    }
    return on;
}

static void upscale_fill(VkExec *ex, uint64_t h) {   // texture lock held
    // Handles already found to be no candidate under the current slot generation are skipped without a lookup.
    // frame: UINT64_MAX for a permanent answer (mirrored, wrong kind), else the frame a recent draw was seen in.
    static struct { uint64_t h, frame; uint32_t gen; } no[1024];
    uint32_t gen = vk_texture_slots_gen();
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    typeof(no[0]) *n = &no[(h * 0x9E3779B97F4A7C15ull) >> 54];
    if (n->h == h && n->gen == gen && (n->frame == UINT64_MAX || n->frame == now)) return;
    const VkTexImage *img = vk_texture_by_id_locked(&ex->tex_pool, (int)(h & 0xfffff), NULL, NULL);
    // Candidates: not drawn into by the replay (attachment, storage image, copy) in the last 8 frames. Clears do not
    // count, and an earlier draw (the race's output texture is written during the load) does not disqualify it.
    // Only what can be the DLSS output: rgb10a2, 16:9, at least 1280 wide. Any other never-drawn image (a shadow map,
    // reflection or minimap target the game renders only now and then) was overwritten with an unrelated render
    // target of the same format (a 1024x1024 one every frame in the race), which put wrong pictures on the track.
    // KAR_UPSCALE_ANY=1 restores the wide match for diagnosis.
    static int any = -1;
    if (any < 0) any = getenv("KAR_UPSCALE_ANY") && *getenv("KAR_UPSCALE_ANY") == '1';
    int dlss_like = img && img->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && img->width >= 1280 &&
                    (uint64_t)img->width * 9 >= (uint64_t)img->height * 16 * 98 / 100 &&
                    (uint64_t)img->width * 9 <= (uint64_t)img->height * 16 * 102 / 100;
    if (!img || img->mirrored || img->samples != 1 || img->type != VK_IMAGE_TYPE_2D || (!any && !dlss_like) ||
        img->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !(img->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        n->h = h, n->gen = gen, n->frame = UINT64_MAX;
        return;
    }
    if (vk_exec_was_drawn(img->image)) {
        n->h = h, n->gen = gen, n->frame = now;
        return;
    }
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    static struct { VkImage dst; uint64_t frame; } done[8];
    for (int i = 0; i < 8; i++)
        if (done[i].dst == img->image && done[i].frame == frame) return;
    uint32_t sw, sh;
    // Only a target drawn in this frame (its passes are stamped frames + 1, vk_exec.c vk_exec_pass_end): it is alive
    // in the submission the blit goes into.
    VkImage src = vk_exec_rt_newest(img->format, img->width, img->width, img->height, frame + 1, &sw, &sh);
    if (!src) return;
    static int slot;
    done[slot].dst = img->image, done[slot].frame = frame, slot = (slot + 1) % 8;
    static int told;
    if (!told++)
        rt_log("vk_exec: upscale fallback: %ux%u format %d filled from a %ux%u render target (the host runs no DLSS)",
               img->width, img->height, (int)img->format, sw, sh);
    int reopen = ex->in_pass;
    vk_exec_barrier(ex);   // ends the pass; earlier writes of the source are visible to the transfer
    // FSR 1 EASU plus the linear decode the game's composite expects (vk_kar_upscale.c); a plain linear blit when it
    // is off or the output is not rgb10a2.
    if (vk_kar_upscale_record(ex->cb, src, img->format, sw, sh, img->image, img->format, img->width, img->height)) {
        B.pipe[1] = VK_NULL_HANDLE;   // the replay's next dispatch binds its own pipeline again
    } else {
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}};
        bl.srcOffsets[1] = (VkOffset3D){(int32_t)sw, (int32_t)sh, 1};
        bl.dstOffsets[1] = (VkOffset3D){(int32_t)img->width, (int32_t)img->height, 1};
        vkCmdBlitImage(ex->cb, src, VK_IMAGE_LAYOUT_GENERAL, img->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    }
    ex->recorded++;
    vk_exec_barrier(ex);   // the blit lands before the draw samples it
    if (reopen) vk_exec_pass_begin(ex);
}

static void tex_info(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate, VkDescriptorImageInfo *out) {
    if (h && !storage && upscale_on()) upscale_fill(ex, h);
    tex_info_lookup(ex, r, h, storage, separate, out);
    if (!twin_dispatch || storage || separate || !h || !(r->type & 0x400)) return;
    VkImageViewType vt = res_view_type(r->type);
    if (vt != VK_IMAGE_VIEW_TYPE_2D && vt != VK_IMAGE_VIEW_TYPE_2D_ARRAY) return;
    const VkTexImage *img = vk_texture_by_id_locked(&ex->tex_pool, (int)(h & 0xfffff), NULL, NULL);
    Twin *t = img ? twin_update(ex, img) : NULL;
    VkImageView v = t ? t->view[vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY] : VK_NULL_HANDLE;
    if (v) out->imageView = v, out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
}

// A Direct pool array (GfxShaderRes.pool_arr): the shader indexes it with (id ^ (id >> 5)) & 31 (gfx_shader.c
// pool_patch), so that element
// gets the texture (or, for a sampler array, the sampler) of each c2 handle the shader reads. Elements no
// candidate names keep the dummy texture or default sampler.
static void pool_fill(VkExec *ex, int st, const GfxShaderRes *r, uint32_t cnt, VkDescSlot *o) {
    uint32_t id1[32] = {0};   // id + 1, 0 empty
    int sampler = !(r->type & 0xff);
    // On by default; KAR_POOL_ARR=0 keeps the dummy texture and default sampler. It was off while the rainbow runs
    // seemed to follow it; their real cause was a skipped texture clear (vk_exec_xfer.c clear_tex), and with that fixed
    // 4 of 4 runs with the fill were clean (2026-11-10).
    static int on = -1;
    if (on < 0) on = !(getenv("KAR_POOL_ARR") && *getenv("KAR_POOL_ARR") == '0');
    if (!on) {
        for (uint32_t k = 0; k < cnt && k < 32; k++) {
            if (sampler) o[k].image = (VkDescriptorImageInfo){Z.samp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            else tex_info(ex, r, 0, 0, 1, &o[k].image);
            o[k].w[2] = (uint32_t)o[k].image.imageLayout;
        }
        return;
    }
    static int clash_told;
#define POOL_PUT(id) do { uint32_t id_ = (uint32_t)(id), k_ = (id_ ^ (id_ >> 5)) & 31; \
        if (id1[k_] && id1[k_] != id_ + 1 && clash_told++ < 8) \
            rt_log("vk_exec: Direct pool array: ids %u and %u share element %u", id1[k_] - 1, id_, k_); \
        id1[k_] = id_ + 1; } while (0)
    for (int s = 0; s < 32; s++)
        if (r->pool_comb >> s & 1) {
            uint64_t v = ex->tex[st][s];
            if (v) POOL_PUT(sampler ? v >> 20 & 0xfff : v & 0xfffff);
        }
    if (!sampler)
        for (int s = 0; s < 128; s++)
            if (r->pool_sep[s / 64] >> (s % 64) & 1 && ex->tex[st][s]) POOL_PUT(ex->tex[st][s] & 0xfffff);
    if (sampler)
        for (int s = 0; s < 32; s++)
            if (r->pool_samp >> s & 1 && ex->samp[st][s]) POOL_PUT(ex->samp[st][s] >> 20 & 0xfff);
#undef POOL_PUT
    for (uint32_t k = 0; k < cnt && k < 32; k++) {
        if (sampler) {
            VkSampler sm = id1[k] ? vk_sampler_by_id_locked(&ex->samp_pool, (int)(id1[k] - 1)) : VK_NULL_HANDLE;
            o[k].image = (VkDescriptorImageInfo){sm ? sm : Z.samp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
        } else {
            tex_info(ex, r, id1[k] ? id1[k] - 1 : 0, 0, 1, &o[k].image);
        }
        o[k].w[2] = (uint32_t)o[k].image.imageLayout;
    }
}

// KAR_C0_TEX=1 (experiment, off by default): an NVN 2 compute texture whose handle word lies in c0 outside the
// texture and image tables (GfxShaderRes.c0_byte; 82 Kirby Air Riders computes, handle 0x100f8038 at byte 0xe0, in
// the storage-buffer table) takes the 32-bit word there as its texture handle, when that names a texture in the pool;
// otherwise the dummy stays. The first values are logged so a run shows whether the word is a handle at all.
static uint64_t c0_tex_handle(VkExec *ex, int st, const GfxShaderRes *r) {
    static int on = -1;
    if (on < 0) on = getenv("KAR_C0_TEX") && *getenv("KAR_C0_TEX") == '1';
    if (!on || !c0_lay) return 0;
    uint8_t bank[VK_C0_BYTES];
    vk_c0_fill(bank, st, ex->ssbo[st], ex->ubo[st], c0_dim, c0_binst);
    uint32_t w;
    memcpy(&w, bank + r->c0_byte, 4);
    const VkTexImage *img = w ? vk_texture_by_id_locked(&ex->tex_pool, (int)(w & 0xfffff), NULL, NULL) : NULL;
    static int told;
    if (told++ < 16)
        rt_log("vk_exec: KAR_C0_TEX: c0+%#x = %#x -> %s", r->c0_byte, w,
               img ? "a texture" : "no texture (dummy kept)");
    return img ? w : 0;
}

// KAR_ARR_SCHED=<frame>:<what>,... (diagnostic, bisection inside one run): from each frame on, the material arrays
// of only the constant buffer named (5..20), of none (0) or of all ("all") get their textures; the others get the
// dummy. A broken run keeps its fault to the end, so screenshots after each step show which buffer's arrays cause it.
// Example: KAR_ARR_SCHED=3500:0,3700:5,3900:6,4100:7,4300:8,4500:all
static int arr_sched_allows(int cbuf) {
    static int parsed, n;
    static struct { uint64_t frame; int what; } st[16];   // what: -1 all, 0 none, else the constant buffer
    if (!parsed) {
        parsed = 1;
        const char *e = getenv("KAR_ARR_SCHED");
        while (e && *e && n < 16) {
            char *q;
            uint64_t f = strtoull(e, &q, 10);
            if (*q != ':') break;
            q++;
            int w = !strncmp(q, "all", 3) ? -1 : (int)strtol(q, &q, 10);
            if (w == -1) q += 3;
            st[n].frame = f, st[n].what = w, n++;
            e = *q == ',' ? q + 1 : q;
        }
        if (n) rt_log("vk_exec: KAR_ARR_SCHED: %d steps", n);
    }
    if (!n) return 1;
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int what = -1;
    for (int i = 0; i < n; i++)
        if (now >= st[i].frame) what = st[i].what;
    static int last = -2;
    if (what != last) {
        last = what;
        rt_log("vk_exec: KAR_ARR_SCHED at frame %llu: material arrays %s%d", (unsigned long long)now,
               what < 0 ? "all (" : what ? "only c" : "none (", what);
    }
    return what < 0 || what == cbuf;
}

// KAR_DIAG_UBO=<fs hash>:<cbuf>[,<cbuf>...] (diagnostic): for the fragment shader with that hash (the FNV-1a of its
// SPIR-V, as vk_dump names it), every 30 frames at its first draw, logs vec4 0..7 of the named uniform buffers as the
// shader reads them (the submit-time copy, ubo_word). Follows parameters such as exposure over a whole run.
// KAR_DIAG_SSBO=<fs hash>[,<fs hash>...] (diagnostic): for those fragment shaders, every 60 frames at their first
// draw, logs each bound fragment and vertex storage buffer: address, size, FNV-1a of its whole contents in guest
// memory (GPU writes land there too) and its first 8 floats. Load-time buffers that differ between a clean and a
// rainbow run show up as different hashes at the same address.
static int readable_range(uint64_t a, uint64_t lim) {
    uint64_t end;
    while (a < lim) {
        int st = plat_mem_state(a, lim, &end);
        if (st != PLAT_PROT_RW && st != PLAT_PROT_READ) return 0;
        a = end;
    }
    return 1;
}
static void diag_ssbo(VkExec *ex, const VkPipeState *s) {
    static int parsed, nwant;
    static uint64_t want[8], last[8];
    if (!parsed) {
        parsed = 1;
        const char *e = getenv("KAR_DIAG_SSBO");
        char *q = (char *)e;
        while (e && *q && nwant < 8) {
            want[nwant] = strtoull(q, &q, 16), last[nwant++] = ~0ull;
            if (*q == ',') q++;
            else break;
        }
    }
    if (!nwant || !s->fs) return;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), h = gfx_shader_spirv_fnv(s->fs);
    int w = -1;
    for (int i = 0; i < nwant; i++)
        if (want[i] == h) w = i;
    if (w < 0 || frame % 60 || frame == last[w]) return;
    last[w] = frame;
    for (int st = NVN_STAGE_VS; st <= NVN_STAGE_FS; st++) {
        if (st != NVN_STAGE_VS && st != NVN_STAGE_FS) continue;
        for (int k = 0; k < 16; k++) {
            const p_bind_buf *b = &ex->ssbo[st][k];
            if (!b->addr || !b->size) continue;
            uint64_t n = b->size < (64ull << 20) ? b->size : (64ull << 20);
            if (!readable_range(b->addr, b->addr + n)) {
                rt_log("ssbo: frame %llu fs %016llx st %d s%d %#llx+%#llx unreadable", (unsigned long long)frame,
                       (unsigned long long)h, st, k, (unsigned long long)b->addr, (unsigned long long)b->size);
                continue;
            }
            const uint8_t *m = (const uint8_t *)(uintptr_t)b->addr;
            uint64_t f = 0xcbf29ce484222325ull;
            for (uint64_t i = 0; i < n; i++) f = (f ^ m[i]) * 0x100000001b3ull;
            float v[8] = {0};
            memcpy(v, m, n < 32 ? n : 32);
            rt_log("ssbo: frame %llu fs %016llx st %d s%d %#llx+%#llx hash %016llx: %g %g %g %g %g %g %g %g",
                   (unsigned long long)frame, (unsigned long long)h, st, k, (unsigned long long)b->addr,
                   (unsigned long long)b->size, (unsigned long long)f, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        }
    }
}

// KAR_DIAG_SSBO_WRITE=kirby (diagnostic): at Kirby's body draw (fs 0432bb881b7a70ab) note the vertex slot 6 buffer
// (per-object local light records, 128 bytes each, chosen by the object's index at slot 0 +460) for ssbo_info, and
// every 60 frames log that object's record as guest memory holds it: 8 x {rgb, weight}.
static void diag_kirby_vs(VkExec *ex, const VkPipeState *s) {
    static int on = -1;
    static uint64_t want = 0x0432bb881b7a70abull;   // KAR_DIAG_SSBO_WRITE=kirby:<fs hash> picks another draw
    if (on < 0) {
        const char *e = getenv("KAR_DIAG_SSBO_WRITE");
        on = e && *e == 'k';
        if (on && strchr(e, ':')) want = strtoull(strchr(e, ':') + 1, NULL, 16);
    }
    if (!on || !s->fs || gfx_shader_spirv_fnv(s->fs) != want) return;
    const p_bind_buf *b6 = &ex->ssbo[NVN_STAGE_VS][6], *b0 = &ex->ssbo[NVN_STAGE_VS][0];
    __atomic_store_n(&kirby_watch[0], b6->addr, __ATOMIC_RELAXED);
    __atomic_store_n(&kirby_watch[1], b6->size, __ATOMIC_RELAXED);
    static uint64_t last = ~0ull;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    static int64_t watch_at = -2;   // KAR_WATCH_WRITE=<frame>: from then, log the code that writes this record buffer
    if (watch_at == -2) watch_at = getenv("KAR_WATCH_WRITE") ? atoll(getenv("KAR_WATCH_WRITE")) : -1;
    if (watch_at >= 0 && (int64_t)frame >= watch_at && b6->addr && b6->size) plat_diag_watch_write(b6->addr, 0x2000);
    if (frame % 60 || frame == last || !b0->addr || !b6->addr) return;
    last = frame;
    if (!readable_range(b0->addr + 456, b0->addr + 464)) return;
    uint32_t idx = ((const uint32_t *)(uintptr_t)(b0->addr + 456))[1];
    uint64_t at = b6->addr + (uint64_t)idx * 128;
    if (!readable_range(at, at + 128)) return;
    const float *f = (const float *)(uintptr_t)at;
    char t[700];
    size_t o = 0;
    for (int k = 0; k < 8; k++) o += (size_t)snprintf(t + o, sizeof t - o, " [%g %g %g %g]", f[4 * k], f[4 * k + 1], f[4 * k + 2], f[4 * k + 3]);
    rt_log("kirby vs: frame %llu idx %u s6 %#llx+%#llx%s:%s", (unsigned long long)frame, idx, (unsigned long long)b6->addr,
           (unsigned long long)b6->size, (uint64_t)idx * 128 + 128 > b6->size ? " (past bound)" : "", t);
}

static void diag_ubo(VkExec *ex, const VkPipeState *s) {
    static int parsed, ncb;
    static uint64_t want;
    static int cbs[8];
    static uint64_t last_frame = ~0ull;
    if (!parsed) {
        parsed = 1;
        const char *e = getenv("KAR_DIAG_UBO");
        if (e && *e) {
            char *q;
            want = strtoull(e, &q, 16);
            while (*q == ':' || *q == ',') {
                if (ncb < 8) cbs[ncb++] = (int)strtol(q + 1, &q, 10);
                else break;
            }
        }
    }
    if (!want || !s->fs) return;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (frame % 30 || frame == last_frame || gfx_shader_spirv_fnv(s->fs) != want) return;
    last_frame = frame;
    int st = NVN_STAGE_FS;
    static int nvec = -1;   // KAR_DIAG_UBO_N: vec4s logged per buffer (default 8), 8 per line
    if (nvec < 0) nvec = getenv("KAR_DIAG_UBO_N") ? atoi(getenv("KAR_DIAG_UBO_N")) : 8;
    for (int c = 0; c < ncb; c++)
    for (int v0 = 0; v0 < nvec; v0 += 8) {
        char b[900];
        size_t o = 0;
        for (int v = v0; v < v0 + 8 && v < nvec; v++) {
            uint64_t lo = ubo_word(ex, st, cbs[c], 16ull * v), hi = ubo_word(ex, st, cbs[c], 16ull * v + 8);
            float f[4];
            memcpy(f, &lo, 8), memcpy(f + 2, &hi, 8);
            o += (size_t)snprintf(b + o, sizeof b - o, " [%d] %g %g %g %g", v, f[0], f[1], f[2], f[3]);
        }
        rt_log("ubo: frame %llu c%d:%s", (unsigned long long)frame, cbs[c], b);
    }
}

static VkDescriptorSet alloc_set(VkExec *ex, VkDescriptorSetLayout layout) {
    CbRes *r = cb_res(ex);
    VkCtx *c = vk_ctx();
    for (int attempt = 0; attempt < 2; attempt++) {
        if (r->npools) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = r->pools[r->npools - 1], .descriptorSetCount = 1,
                                              .pSetLayouts = &layout};
            VkDescriptorSet set;
            VkResult res = vkAllocateDescriptorSets(c->device, &ai, &set);
            if (res == VK_SUCCESS) return set;
            if (res != VK_ERROR_OUT_OF_POOL_MEMORY && res != VK_ERROR_FRAGMENTED_POOL) rt_fatal("vk: descriptor set allocation failed (%d)", (int)res);
        }
        static const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 256},
            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 256},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}};   // separate samplers: without it validation reports the pool
        VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 256,
                                         .poolSizeCount = sizeof sizes / sizeof sizes[0], .pPoolSizes = sizes};
        if (r->npools == r->cpools) r->pools = grow(r->pools, &r->cpools, sizeof *r->pools);
        VkDescriptorPool pool = VK_NULL_HANDLE;
        plat_mutex_lock(&spare_lock);
        if (nspare_pool) pool = spare_pool[--nspare_pool];
        plat_mutex_unlock(&spare_lock);
        if (!pool && vkCreateDescriptorPool(c->device, &pi, NULL, &pool) != VK_SUCCESS) rt_fatal("vk: descriptor pool failed");
        r->pools[r->npools++] = pool;
    }
    rt_fatal("vk: descriptor set allocation failed twice");
    return VK_NULL_HANDLE;
}

static void support_in(const VkExec *ex, float in[3]) {   // what support_fill reads: viewport size, point size
    in[0] = ex->in_pass && ex->has_vp ? ex->vp[0][2] : (float)ex->pass_w;
    in[1] = ex->in_pass && ex->has_vp ? ex->vp[0][3] : (float)ex->pass_h;
    in[2] = ex->point_size > 0 ? ex->point_size : 1.0f;
}

static void support_fill(uint8_t *b, float w, float h, float point_size) {
    memset(b, 0, SUPPORT_BYTES);
    float inv[4] = {w > 0 ? 2.0f / w : 0, h > 0 ? 2.0f / h : 0, 0, 0}, size[4] = {w, h, 0, 0};
    memcpy(b + 144, inv, 16);
    memcpy(b + 160, size, 16);
    const float one = 1.0f;
    memcpy(b + 1392, &point_size, 4);   // point_size (translator patch 0008): vertex shaders that write none output it
    for (int i = 0; i < 73; i++) memcpy(b + 192 + 16 * i, &one, 4);   // render scale 1: the host renders at guest size
}

// TOTK_BIND_STATS=1: bindings and descriptors per draw of each stage (Task 2.2's choice of the pushed stage), and how
// often a draw repeats the previous one, logged every 20000 draws: pushes recorded and skipped, allocated sets written
// and reused, stage texture-type recomputations (answers_for), draws with the previous draw's program, bound state
// and textures (shaders and pipeline would be the same) and those that also have its buffers and viewport (all
// descriptors would be the same). BS is declared beside DS.
static int bind_stats_on(void) {
    if (BS.on < 0) {
        const char *e = cfg_get("TOTK_BIND_STATS");
        BS.on = e && *e && strcmp(e, "0");
    }
    return BS.on;
}

static void bind_stats(const VkPipeInfo *info) {
    if (!bind_stats_on() || info->bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        int fs = info->n_sets > 1 ? b->set : !(b->stages & VK_SHADER_STAGE_VERTEX_BIT);
        BS.bind[fs]++, BS.desc[fs] += b->count ? b->count : 1;
        if (b->res && b->res->arr_ubo) {   // material arrays (bindless 16.2); those whose sampler is in the handle
            BS.arr_desc += b->count ? b->count : 1;
            if (b->res->samp_slot < 0) BS.arr_own_samp += b->count ? b->count : 1;
        }
        else if (b->res && b->res->pool_arr) BS.pool_desc += b->count ? b->count : 1;
    }
    if (++BS.draws % 20000 == 0) {
        double d = (double)BS.draws;
        rt_log("vk_exec: per draw over %llu draws: vertex %.2f bindings %.2f descriptors, fragment %.2f bindings %.2f descriptors",
               (unsigned long long)BS.draws, BS.bind[0] / d, BS.desc[0] / d, BS.bind[1] / d, BS.desc[1] / d);
        rt_log("vk_exec: per draw: pushes %.3f skipped %.3f, sets written %.3f reused %.3f, texture types %.3f; "
               "same program and state as the draw before %.3f, also same buffers %.3f; descriptor builds skipped %.3f",
               BS.push / d, BS.push_skip / d, BS.set_new / d, BS.set_reuse / d, BS.ac_miss / d, BS.same_prog / d,
               BS.same_all / d, BS.bind_skip / d);
        rt_log("vk_exec: per draw: descriptors from material arrays %.2f (sampler in the handle %.2f), from pool arrays %.2f",
               BS.arr_desc / d, BS.arr_own_samp / d, BS.pool_desc / d);
    }
}

// The bound state a draw reads, compared with the previous draw's (TOTK_BIND_STATS only).
static void repeat_stats(const VkExec *ex, const VkPipeInfo *info) {
    static struct {
        const prog_t *prog;
        VkPipeState pipe;
        uint64_t tex[2][128], img[2][32];
        const VkPipeInfo *info;
        p_bind_buf ubo[2][18], ssbo[2][16];
        float vp[6], point_size;
    } last, cur;
    if (!bind_stats_on()) return;
    memset(&cur, 0, sizeof cur);
    cur.prog = ex->prog, cur.pipe = ex->pipe, cur.info = info;
    const int st[2] = {NVN_STAGE_VS, NVN_STAGE_FS};
    for (int k = 0; k < 2; k++) {
        memcpy(cur.tex[k], ex->tex[st[k]], sizeof cur.tex[k]), memcpy(cur.img[k], ex->img[st[k]], sizeof cur.img[k]);
        memcpy(cur.ubo[k], ex->ubo[st[k]], sizeof cur.ubo[k]), memcpy(cur.ssbo[k], ex->ssbo[st[k]], sizeof cur.ssbo[k]);
    }
    memcpy(cur.vp, ex->vp[0], sizeof cur.vp), cur.point_size = ex->point_size;
    size_t prog_part = offsetof(typeof(cur), info);
    if (!memcmp(&cur, &last, prog_part)) {
        BS.same_prog++;
        if (!memcmp((const char *)&cur + prog_part, (const char *)&last + prog_part, sizeof cur - prog_part)) BS.same_all++;
    }
    last = cur;
}

static int push_skip_on(void) {   // TOTK_PUSH_SKIP=0: every graphics draw pushes its set again
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("TOTK_PUSH_SKIP");
    }
    return on;
}

// A graphics draw whose descriptor inputs equal the previous graphics draw's in this command buffer skips building its
// sets: every write would be the same, the push would be skipped and the allocated set reused without a bind (about a
// quarter of field and monsters2 draws, TOTK_BIND_STATS). The inputs, compared and stored in place, are what
// bind_resources and its helpers read beyond the pipeline info's fixed bindings and Z's stand-ins:
// - the info (bindings, layouts), the program and its serial (the compiler-constant range of UBO slot 1);
// - vk_texture_slots_gen, vk_texture_epoch (a slot's image, view and once-per-submit storage check), vk_sampler_gen;
// - vk_mem_gen (buffer and offset of a span, mirror creation and per-submission sync);
// - the images in the feedback layout (the sampled layout);
// - per binding: the support inputs, c0's storage table and base instance, the bound uniform and storage ranges, the
//   texture and image handles (RsList; compared only when a bind op ran since the last draw, bind_gen);
// - storage buffer addresses read from user uniform blocks (RsList.p, ubo_ptr): compared on every draw, since the
//   block's contents change without a bind op.
// The side effects of a build repeat as no-ops in the same command buffer and submission: c0 and support buffers kept
// in B, note_written's ranges, vk_mem_gpu_wrote's write-backs, the per-submission mirror sync and dirty census note,
// the views cached per submission. The census counts are added again. A build that copied guest bytes into transient
// memory (an unaligned host range) is never skipped after: the copy may differ. B.res_valid says the sets bound hold
// the last build; vk_exec_draw_new_cb's memset clears it, and every build clears it until it ends.
// TOTK_BIND_SKIP=0 builds every draw; =check builds every draw and stops when a draw with equal inputs gets other
// writes, census counts or descriptor commands.
static int bind_skip_on(void) {
    if (RS.on < 0) {
        const char *e = cfg_value("TOTK_BIND_SKIP");
        RS.on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
        if (RS.on != 1) rt_log("vk_exec: descriptor build skip %s", RS.on ? "check" : "off");
    }
    return RS.on;
}

static void rs_add(RsRange *r, uint32_t *n, const VkExec *ex, const void *at, size_t len) {
    uint32_t off = (uint32_t)((const uint8_t *)at - (const uint8_t *)ex);
    if (*n && r[*n - 1].off + r[*n - 1].len == off) r[*n - 1].len += (uint32_t)len;
    else r[(*n)++] = (RsRange){off, (uint32_t)len};
}

// A material array's inputs as one number: its constant buffer binding, every element handle it reads (as the replay
// fills them, ubo_word) and the separate sampler it combines them with. Equal numbers give equal descriptors.
// The words only change with the binding, the submit-time snapshot (vk_usnap_gen) or an inline update in this
// command buffer (UPD), so the hash is cached on those: reading 256 words per array per draw cost half the frame.
static uint64_t arr_sig_words(const VkExec *ex, int st, const GfxShaderRes *r, const p_bind_buf *u);
static uint64_t arr_sig(const VkExec *ex, int st, const GfxShaderRes *r) {
    const p_bind_buf *u = r->cbuf_slot >= 3 && r->cbuf_slot - 3 < 18 ? &ex->ubo[st][r->cbuf_slot - 3] : NULL;
    static struct { const GfxShaderRes *r; uint64_t addr, size, gen, upd, h; int st; } cache[512];
    uint64_t gen = vk_usnap_gen() + vk_mem_gen(), upd = (uint64_t)UPD.n << 32 ^ UPD.fence;
    uint64_t addr = u ? u->addr : 0, size = u ? u->size : 0;
    unsigned slot = (unsigned)(((uintptr_t)r >> 4) * 2654435761u + (unsigned)st) & 511;
    if (cache[slot].r == r && cache[slot].st == st && cache[slot].addr == addr && cache[slot].size == size &&
        cache[slot].gen == gen && cache[slot].upd == upd) {
        uint64_t h = cache[slot].h;
        if (r->samp_slot >= 0 && r->samp_slot < 128)
            h = (h ^ ex->samp[st][r->samp_slot] ^ (r->samp_tbl ? ex->tex[st][r->samp_slot] << 1 : 0)) * 0x100000001b3ull;
        return h;
    }
    uint64_t w = arr_sig_words(ex, st, r, u);
    cache[slot].r = r, cache[slot].st = st, cache[slot].addr = addr, cache[slot].size = size, cache[slot].gen = gen;
    cache[slot].upd = upd, cache[slot].h = w;
    if (r->samp_slot >= 0 && r->samp_slot < 128)
        w = (w ^ ex->samp[st][r->samp_slot] ^ (r->samp_tbl ? ex->tex[st][r->samp_slot] << 1 : 0)) * 0x100000001b3ull;
    return w;
}
static uint64_t arr_sig_words(const VkExec *ex, int st, const GfxShaderRes *r, const p_bind_buf *u) {
    uint64_t h = 0xcbf29ce484222325ull;
    h = (h ^ (u ? u->addr : 0)) * 0x100000001b3ull;
    h = (h ^ (u ? u->size : 0)) * 0x100000001b3ull;
    static uint64_t w[1024];
    uint32_t cnt = r->bindless ? r->bl_n : r->count;   // a bindless array keeps one element but reads bl_n handles
    uint32_t n = cnt < 1024 ? cnt : 1024;
    ubo_block(ex, st, r->cbuf_slot, 0, n, w);
    for (uint32_t k = 0; k < n; k++) h = (h ^ w[k]) * 0x100000001b3ull;
    for (uint32_t k = n; k < cnt; k++) h = (h ^ ubo_word(ex, st, r->cbuf_slot, 8ull * k)) * 0x100000001b3ull;
    return h;
}

static const RsList *rs_list(const VkExec *ex, const VkPipeInfo *info) {
    if (info == rs_last) return rs_last_l;
    if (!rs_lists.klen) nvn_hm_init(&rs_lists, sizeof info);
    RsList *l = nvn_hm_get(&rs_lists, &info);
    if (!l) {
        static RsRange tmp[MAX_DESC];
        RsPtr ptr[RS_PTRS];
        uint32_t n = 0, nd = 0, np = 0;
        int sup = 0, c0 = 0, ok = 1;
        for (uint32_t i = 0; i < info->n_bindings && ok; i++) {
            const VkPipeBinding *b = &info->bindings[i];
            const GfxShaderRes *r = b->res;
            uint32_t cnt = b->count ? b->count : 1;
            if ((nd += cnt) > MAX_DESC) { ok = 0; break; }   // the build stops on it
            int st = nvn_stage_of(b->stage);
            if (st < 0) rt_fatal("vk_exec: unsupported descriptor stage %u", b->stage);
            if (b->kind == VK_PIPE_SUPPORT) {
                sup = 1;
            } else if (b->kind == GFX_RES_UBO) {
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    if (slot == 0 && c0_on()) c0 = 1, rs_add(tmp, &n, ex, ex->ssbo[st], sizeof ex->ssbo[st]);
                    else if (slot >= 3 && slot - 3 < 18) rs_add(tmp, &n, ex, &ex->ubo[st][slot - 3], sizeof(p_bind_buf));
                }   // slot 1: the program (bind_sig); others: the zero buffer
            } else if (b->kind == GFX_RES_SSBO) {   // every descriptor of the binding is the same range
                int ubo, slot = ssbo_slot(r, st, &ubo);
                if (slot >= 0 && slot < 16) {
                    rs_add(tmp, &n, ex, ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot], sizeof(p_bind_buf));
                } else if (ubo_ptr_res(r)) {
                    if (np == RS_PTRS) { ok = 0; break; }
                    rs_add(tmp, &n, ex, &ex->ubo[st][r->cbuf_slot - 3], sizeof(p_bind_buf));
                    ptr[np++] = (RsPtr){st, r};
                }
            } else if ((b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
                       b->type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                int img = b->kind == GFX_RES_IMG, cap = img ? 32 : 128;
                // material arrays: their element handles come from a constant buffer, so the signature takes a hash
                // of those words (arr_sig) instead of bound slots; pool and c0 handles stay out of it (no skip)
                if (r->arr_ubo && !img) {
                    if (np == RS_PTRS) { ok = 0; break; }
                    ptr[np++] = (RsPtr){st, r};
                    continue;
                }
                if (r->arr_ubo || r->pool_arr || r->c0_byte) { ok = 0; break; }   // handles not in the signature: no skip
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    if (slot >= 0 && slot < cap && (img || r->from_img) && slot < 32) rs_add(tmp, &n, ex, &ex->img[st][slot], 8);
                    else if (slot >= 0 && slot < cap && !r->from_img) rs_add(tmp, &n, ex, &ex->tex[st][slot], 8);
                }
            }
        }
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < n; i++) bytes += tmp[i].len;
        if (bytes > RS_BYTES) ok = 0;
        l = malloc(sizeof *l + sizeof *tmp * n);
        if (!l) rt_fatal("vk: out of memory");
        l->n = n, l->bytes = (uint32_t)bytes, l->sup = (uint8_t)sup, l->c0 = (uint8_t)c0, l->ok = (uint8_t)ok;
        l->np = (uint8_t)np;
        memcpy(l->p, ptr, sizeof *ptr * np);
        memcpy(l->r, tmp, sizeof *tmp * n);
        nvn_hm_put(&rs_lists, &info, l);
    }
    rs_last = info, rs_last_l = l;
    return l;
}

// The signature of a graphics draw's descriptor inputs into RS; 1 when it equals the stored one. The bound-slot bytes
// are compared only when a bind op ran since they were stored or the pipeline changed: otherwise they are the same.
// Check mode compares them anyway and stops when the bind op generation said they could not differ but they do.
static int bind_sig(const VkExec *ex, const VkPipeInfo *info) {
    const RsList *l = rs_list(ex, info);
    if (!l->ok) return RS.hn = 0, RS.info = NULL, 0;
    uint64_t *h = RS.h, diff = 0;
    uint32_t n = 0;
#define PUT(v) do { uint64_t v_ = (uint64_t)(v); diff |= h[n] ^ v_; h[n++] = v_; } while (0)
    PUT((uintptr_t)info);
    PUT((uintptr_t)ex->prog);
    PUT(ex->prog->serial);
    PUT(ex->tex_pool.addr);
    PUT(ex->tex_pool.count);
    PUT(ex->samp_pool.addr);
    PUT(ex->samp_pool.count);
    PUT((uint64_t)vk_texture_slots_gen() << 32 | vk_texture_epoch());
    PUT((uint64_t)vk_sampler_gen() << 32 | (uint32_t)ex->nfbl);
    PUT(vk_mem_gen() + vk_usnap_gen());   // both only grow
    for (int i = 0; i < ex->nfbl && i < 9; i++) PUT((uintptr_t)ex->fbl_img[i]);
    if (l->sup) {
        float in[3];
        support_in(ex, in);
        uint64_t a;
        uint32_t ps;
        memcpy(&a, in, 8), memcpy(&ps, &in[2], 4);
        PUT(a);
        PUT(ps);
    }
    if (l->c0) PUT(c0_binst);
    for (int i = 0; i < l->np; i++) PUT(l->p[i].res->arr_ubo ? arr_sig(ex, l->p[i].st, l->p[i].res) : ubo_ptr(ex, l->p[i].st, l->p[i].res));
#undef PUT
    diff |= n != RS.hn;
    RS.hn = n;
    int fast = RS.info == info && RS.gen == bind_gen;
    if (fast && RS.on != 2) return !diff;
    const uint8_t *e = (const uint8_t *)ex;
    uint8_t *d = RS.body;
    int bd = 0;
    for (uint32_t i = 0; i < l->n; i++) {
        const RsRange *r = &l->r[i];
        if (!bd && memcmp(d, e + r->off, r->len)) bd = 1;
        if (bd) memcpy(d, e + r->off, r->len);
        d += r->len;
    }
    if (fast && bd) rt_fatal("vk_exec: descriptor build skip check: bound slots changed without a bind op");
    RS.info = info, RS.gen = bind_gen;
    return !diff && !bd;
}

// Writes allocated set s from its packed descriptors d: through the layout's update template, or with the write
// structs w[i0..i1) when it has none.
static void write_set(const VkPipeInfo *info, uint32_t s, VkDescriptorSet set, const VkDescSlot *d, VkWriteDescriptorSet *w,
                      uint32_t i0, uint32_t i1) {
    if (i1 == i0) return;
    if (info->tpl[s]) {
        vk_pipeline_update_tpl_fn()(vk_ctx()->device, set, info->tpl[s], d);
        return;
    }
    for (uint32_t k = i0; k < i1; k++) w[k].dstSet = set;
    vkUpdateDescriptorSets(vk_ctx()->device, i1 - i0, w + i0, 0, NULL);
}

// Writes the pipeline's sets from the bound state: set 0, and set 1 with independent layouts. Each is pushed into the
// command buffer, or allocated, written and bound; a graphics set whose contents equal the last one allocated for that
// set number in this command buffer is reused, and bound again only when another layout or a push came between. A
// graphics push equal to the last one at its set number, with no other layout or bind between, is not recorded.
// The descriptors go into one packed array d, a VkDescSlot per descriptor in binding order, which a set's update
// template reads directly (VkPipeInfo.tpl); write structs are filled only when a set has no template.
static int getenv_diag_c8(void) {   // KAR_DIAG_C8 logs every element: keep the per-element path then
    static int on = -1;
    if (on < 0) on = getenv("KAR_DIAG_C8") && *getenv("KAR_DIAG_C8") == '1';
    return on;
}

uint64_t vk_rp_bind_ns;   // KAR_REPLAY_PROF: time in bind_resources (vk_exec.c window line)
static void bind_resources_impl(VkExec *ex, const VkPipeInfo *info);
static void bind_resources(VkExec *ex, const VkPipeInfo *info) {
    static int on = -1;
    if (on < 0) on = getenv("KAR_REPLAY_PROF") && *getenv("KAR_REPLAY_PROF") == '1';
    if (!on) return bind_resources_impl(ex, info);
    uint64_t t0 = plat_now_ns();
    bind_resources_impl(ex, info);
    vk_rp_bind_ns += plat_now_ns() - t0;
}
// Bindless material arrays (gfx_shader.c bindless_patch, 16.2): the shader reads element k's texture and sampler from
// the global set by the handle at byte 8k of its constant buffer. Each handle the array holds is resolved through
// tex_info once per submission (vk_texture_epoch: its lazy upload, storage check and barrier, as before), and the
// global set is written only when the view, layout or sampler differs from what it holds. An array whose sampler sits
// in a bound slot (GfxShaderRes.bl_samp) reads that one sampler from its own binding: its handles are completed with
// the slot's sampler bits as before (the sampler can depend on the texture: point sampling of depth), and the sampler
// of the first element with a handle is returned for that binding.
static VkSampler bindless_resident(VkExec *ex, int st, const GfxShaderRes *r) {
    enum { NIMG = 65792, NSAMP = 4096 };
    static uint64_t aw[1024];
    static uint32_t img_ep[3][NIMG], samp_ep[NSAMP];
    static VkImageView img_v[3][NIMG];
    static VkImageLayout img_l[3][NIMG];
    static VkSampler samp_v[NSAMP];
    uint32_t n = r->bl_n <= 1024 ? r->bl_n : 1024, ep = vk_texture_epoch() + 1, kind = r->bindless - 1u;
    uint64_t sh = r->bl_samp ? ex->samp[st][r->samp_slot] & ~0xfffffull : 0;
    VkSampler first = VK_NULL_HANDLE;
    if (!n || kind > 2 || !arr_sched_allows(r->cbuf_slot)) return first;
    ubo_block(ex, st, r->cbuf_slot, 0, n, aw);
    int put = 0;
    vk_texture_lock();
    for (uint32_t k = 0; k < n; k++) {
        uint64_t h = aw[k] & 0xffffffffull;
        if (h && sh) h = (h & 0xfffff) | sh;
        uint32_t tid = (uint32_t)(h & 0xfffff), sid = (uint32_t)(h >> 20);
        if (r->bl_samp && h && !first) {   // the binding's sampler: always resolved (memoised by tex_info)
            VkDescriptorImageInfo fi;
            tex_info(ex, r, h, 0, 0, &fi);
            first = fi.sampler;
        }
        if (!h || (img_ep[kind][tid] == ep && samp_ep[sid] == ep)) continue;
        VkDescriptorImageInfo ii;
        tex_info(ex, r, h, 0, 0, &ii);
        img_ep[kind][tid] = ep, samp_ep[sid] = ep;
        if (ii.imageView != img_v[kind][tid] || ii.imageLayout != img_l[kind][tid]) {
            vk_bindless_put_image((int)kind, tid, ii.imageView, ii.imageLayout);
            img_v[kind][tid] = ii.imageView, img_l[kind][tid] = ii.imageLayout, put = 1;
        }
        if (ii.sampler && ii.sampler != samp_v[sid]) {
            vk_bindless_put_sampler(sid, ii.sampler);
            samp_v[sid] = ii.sampler, put = 1;
        }
    }
    vk_texture_unlock();
    if (put) vk_bindless_flush();
    return first;
}

static void bind_resources_impl(VkExec *ex, const VkPipeInfo *info) {
    static VkWriteDescriptorSet w[MAX_BINDINGS];
    static VkDescSlot d[MAX_DESC];
    static VkBufferView bv[MAX_DESC];
    if (info->n_bindings > MAX_BINDINGS) rt_fatal("vk: %u descriptor bindings exceed the replay's %d", info->n_bindings, MAX_BINDINGS);
    bind_stats(info);
    int gfx = info->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS;
    int skip = gfx ? bind_skip_on() : 0, same = 0, was_valid = B.res_valid;
    if (skip) {
        same = bind_sig(ex, info);
        if (same && was_valid && skip == 1) {
            for (int i = 0; i < RS.ncnt; i++) vk_mem_count(RS.cnt[i].cls, RS.cnt[i].use, RS.cnt[i].bytes);
            BS.bind_skip++;
            ex->recorded++;
            return;
        }
        B.res_valid = 0;   // until the build below ends
        if (skip == 2) memcpy(RS.pcnt, RS.cnt, sizeof RS.cnt), RS.pncnt = RS.ncnt;
        RS.rec = 1, RS.ncnt = 0, RS.cnt_over = 0, RS.volatile_ = 0, RS.cmds = 0;
    }
    uint32_t n_sets = info->n_sets ? info->n_sets : 1;
    uint32_t lo[2] = {0, 0}, hi[2] = {0, 0}, d0[2] = {0, 0};   // bindings [lo, hi) and first descriptor per set
    int need_w = 0;                                             // a set without a template: fill the write structs
    for (uint32_t s = 0, i0 = 0; s < n_sets; s++) {
        uint32_t i1 = i0;
        while (i1 < info->n_bindings && (n_sets == 1 || info->bindings[i1].set == s)) i1++;
        lo[s] = i0, hi[s] = i1, i0 = i1;
        if (i1 > lo[s] && !info->tpl[s]) need_w = 1;
    }
    uint32_t nd = 0;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        const GfxShaderRes *r = b->res;
        uint32_t cnt = b->count ? b->count : 1;
        if (nd + cnt > MAX_DESC) rt_fatal("vk: %u descriptors exceed the replay's %d", nd + cnt, MAX_DESC);
        if (n_sets == 2 && i == lo[1]) d0[1] = nd;
        VkDescSlot *o = &d[nd];
        int st = nvn_stage_of(b->stage), kind = 0;   // kind: 0 buffer, 1 image, 2 texel buffer view
        if (st < 0) rt_fatal("vk_exec: unsupported descriptor stage %u", b->stage);
        if (b->kind == VK_PIPE_SUPPORT) {
            float in[3];
            support_in(ex, in);
            if (!B.sup_valid || memcmp(in, B.sup_in, sizeof in)) {   // support_fill reads only these
                uint8_t tmp[SUPPORT_BYTES];
                support_fill(tmp, in[0], in[1], in[2]);
                memcpy(B.sup_in, in, sizeof in);
                if (!B.sup_valid || memcmp(tmp, B.support, SUPPORT_BYTES)) {   // a new buffer only when the contents changed
                    VkBuffer sb;
                    VkDeviceSize so;
                    memcpy(scratch(ex, SUPPORT_BYTES, &sb, &so), tmp, SUPPORT_BYTES);
                    memcpy(B.support, tmp, SUPPORT_BYTES);
                    B.sup_buf = sb, B.sup_off = so, B.sup_valid = 1;
                }
            }
            o->buffer = (VkDescriptorBufferInfo){B.sup_buf, B.sup_off, SUPPORT_BYTES};
        } else if (b->kind == GFX_RES_UBO) {
            for (uint32_t k = 0; k < cnt; k++) o[k].buffer = ubo_info(ex, st, r->slot + (int)k);
        } else if (b->kind == GFX_RES_SSBO) {
            for (uint32_t k = 0; k < cnt; k++) o[k].buffer = ssbo_info(ex, st, r, r->flags & 1);
        } else if (b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) {
            int img = b->kind == GFX_RES_IMG;
            if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                kind = 2;
                for (uint32_t k = 0; k < cnt; k++) {   // buffer textures are not replayed yet
                    o[k].w[0] = 0, o[k].w[1] = 0, o[k].w[2] = 0;
                    o[k].view = Z.zview;
                    if (need_w) bv[nd + k] = Z.zview;
                }
            } else if (r->pool_arr) {
                kind = 1;
                vk_texture_lock();
                pool_fill(ex, st, r, cnt, o);
                vk_texture_unlock();
            } else if (r->bindless) {   // read through the global set: one unused element
                kind = 1;
                VkSampler bs = bindless_resident(ex, st, r);
                vk_texture_lock();
                tex_info(ex, r, 0, 0, 0, &o[0].image);
                vk_texture_unlock();
                if (r->bl_samp && bs) o[0].image.sampler = bs;   // a SAMPLER binding: only the sampler counts
                o[0].w[2] = (uint32_t)o[0].image.imageLayout;
            } else {
                kind = 1;
                // A material array: its handles in one read (ubo_block), and the descriptor of an empty element made
                // once and copied (an element without a handle takes the dummy texture whatever its index).
                static uint64_t aw[1024];
                int ablock = r->arr_ubo && !img && cnt <= 1024 && arr_sched_allows(r->cbuf_slot) &&
                             !(getenv("KAR_ARR_UBO") && *getenv("KAR_ARR_UBO") == '0') &&
                             !(getenv("KAR_ARR_BLOCK") && *getenv("KAR_ARR_BLOCK") == '0');   // 0: per word (A/B)
                if (ablock) ubo_block(ex, st, r->cbuf_slot, 0, cnt, aw);
                int zero_at = -1;
                vk_texture_lock();   // tex_info looks up with the texture lock held
                for (uint32_t k = 0; k < cnt; k++) {
                    if (ablock && !(aw[k] & 0xffffffffull) && zero_at >= 0 && !getenv_diag_c8()) {
                        o[k] = o[zero_at];
                        continue;
                    }
                    int slot = r->slot + (int)k;
                    uint64_t h = 0;
                    if (img) h = slot >= 0 && slot < 32 ? ex->img[st][slot] : 0;
                    else if (r->arr_ubo) {
                        static int arr_on = -1;   // KAR_ARR_UBO=0 (diagnostic): material arrays get the dummy texture
                        if (arr_on < 0) arr_on = !getenv("KAR_ARR_UBO") || *getenv("KAR_ARR_UBO") != '0';
                        h = ablock ? aw[k] & 0xffffffffull
                            : arr_on && arr_sched_allows(r->cbuf_slot) ? ubo_word(ex, st, r->cbuf_slot, 8ull * k) & 0xffffffffull
                                                                     : 0;
                        // KAR_ARR_SCAN=1 (diagnostic): once per array fill, read the buffer's words past the array's
                        // end (up to 1024 elements) and log each new highest element holding a handle, so ARR_N can
                        // be chosen from the data rather than by binding huge arrays
                        static int arr_scan = -1;
                        if (arr_scan < 0) arr_scan = getenv("KAR_ARR_SCAN") && *getenv("KAR_ARR_SCAN") == '1';
                        if (arr_scan && k == 0 && r->cbuf_slot >= 3 && r->cbuf_slot - 3 < 18) {
                            static uint32_t scan_top[24];
                            const p_bind_buf *ub = &ex->ubo[st][r->cbuf_slot - 3];
                            uint32_t lim = ub->size / 8 < 1024 ? (uint32_t)(ub->size / 8) : 1024;
                            for (uint32_t e = lim; e-- > scan_top[r->cbuf_slot];)
                                if (ubo_word(ex, st, r->cbuf_slot, 8ull * e) & 0xffffffffull) {
                                    scan_top[r->cbuf_slot] = e;
                                    rt_log("vk_exec: KAR_ARR_SCAN: c%d highest element with a handle %u (array %u, buffer %llu bytes)",
                                           r->cbuf_slot, e, cnt, (unsigned long long)ub->size);
                                    break;
                                }
                        }
                        static uint32_t arr_top;   // highest material array element seen holding a handle
                        if (h && k > arr_top && k >= 31) {
                            arr_top = k;
                            rt_log("vk_exec: material array c%d element %u holds handle %#llx (32 elements reached only "
                                   "0..31)", r->cbuf_slot, k, (unsigned long long)h);
                        }
                    }
                    else if (r->from_img) h = slot >= 0 && slot < 32 ? ex->img[st][slot] : 0;
                    else if (r->c0_byte && st == NVN_STAGE_CS) h = c0_tex_handle(ex, st, r);
                    else h = slot >= 0 && slot < 128 ? tex_or_image(ex, st, slot) : 0;
                    // A combined sampler over a split handle: the sampler bound in the separate-sampler slot
                    // (nvnDeviceGetSeparateSamplerHandle: id << 20) completes the texture handle as a combined one.
                    if (!img && h && !r->separate && r->samp_slot >= 0 && r->samp_slot < 128) {
                        uint64_t sh = ex->samp[st][r->samp_slot];
                        // a compute texture-table entry: the separate sampler may have been bound as a texture handle
                        if (r->samp_tbl && !(sh & ~0xfffffull)) sh = ex->tex[st][r->samp_slot];
                        if (!r->samp_tbl || (sh & ~0xfffffull)) h = (h & 0xfffff) | (sh & ~0xfffffull);
                    }
                    tex_info(ex, r, h, img, r->separate, &o[k].image);
                    o[k].w[2] = (uint32_t)o[k].image.imageLayout;   // zero the tail padding
                    if (ablock && !h && zero_at < 0) zero_at = (int)k;
                    // KAR_DIAG_C8=1: each c8 material array element with a handle: frame, declaration, handle, the
                    // texture it resolves to and the pass's first colour target (the rainbow comes from c8 arrays,
                    // drawn only in some frames: an environment capture is the suspect).
                    static int c8_diag = -1;
                    static int c8_lines;
                    if (c8_diag < 0) c8_diag = getenv("KAR_DIAG_C8") && *getenv("KAR_DIAG_C8") == '1';
                    if (c8_diag && r->arr_ubo && r->cbuf_slot == 8 && h && c8_lines < 400) {
                        const VkTexImage *ti = vk_texture_by_id_locked(&ex->tex_pool, (int)(h & 0xfffff), NULL, NULL);
                        const VkTexImage *ct = ex->color[0].img;
                        c8_lines++;
                        rt_log("c8: frame %llu decl %#x [%u] handle %#llx -> %s fmt %d %ux%u layers %u view %d%s | target %ux%u fmt %d",
                               (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), r->type, k,
                               (unsigned long long)h, ti ? "tex" : "none", ti ? (int)ti->format : 0, ti ? ti->width : 0,
                               ti ? ti->height : 0, ti ? ti->layers : 0, ti ? (int)ti->view_type : -1,
                               ti && ti->gpu_owned ? " gpu" : "", ct ? ct->width : 0, ct ? ct->height : 0,
                               ct ? (int)ct->format : 0);
                    }
                }
                vk_texture_unlock();
            }
        } else {
            rt_fatal("vk: binding %u has an unknown kind %d", (unsigned)b->binding, (int)b->kind);
        }
        if (need_w) {
            w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstBinding = b->binding, .descriptorCount = cnt, .descriptorType = b->type};
            if (kind == 0) w[i].pBufferInfo = &o->buffer;   // a VkDescSlot is as large as either info struct
            else if (kind == 1) w[i].pImageInfo = &o->image;
            else w[i].pTexelBufferView = &bv[nd];
        }
        nd += cnt;
    }
    // Set 1's first descriptor is noted when the loop reaches its first binding. A set 1 without bindings (a fragment
    // stage with no resources) starts at the end instead, so set 0's byte count (d0[1] - d0[0]) covers its descriptors.
    // Left at 0, it pushed set 0's template with 0 bytes copied into the recording, and the driver read the
    // descriptors from whatever followed in the recording.
    if (n_sets == 2 && lo[1] == info->n_bindings) d0[1] = nd;
    // Under the validation layer, a new graphics layout binds set 1 before set 0 is pushed or bound. The layer (SDK
    // 1.4.363, PipelineLayoutCompatDef::operator==) takes any two INDEPENDENT_SETS layouts as compatible, so
    // vkCmdPushDescriptorSetKHR keeps the push set it made for an earlier layout and drops writes to bindings that
    // layout lacks (false VUID-vkCmdDrawIndexed-None-08114, then an access violation inside the layer). A bind at set 1
    // disturbs set 0 by its canonical id, which drops the layer's push set: the push after it gets a new one.
    int set1_first = 0;
    if (gfx && info->layout != B.ds_layout) {   // another layout: sets bound before may be disturbed
        B.ds_layout = info->layout;
        B.ds_bound[0] = B.ds_bound[1] = VK_NULL_HANDLE;
        B.ds_pushed[0] = B.ds_pushed[1] = 0;
        set1_first = n_sets == 2 && vk_ctx()->validation;
    }
    {   // bindless (vk_bindless.c, 16.2): the global set as set 2, again after a new command buffer or layout; bound
        // before sets 0 and 1 are pushed or bound, so it does not disturb their push sets
        static VkCommandBuffer bl_cb[2];
        static VkPipelineLayout bl_layout[2];
        int k = !gfx;
        VkDescriptorSet bls = info->bindless ? vk_bindless_set() : VK_NULL_HANDLE;
        if (bls && (bl_cb[k] != ex->cb || bl_layout[k] != info->layout)) {
            extern VkDescriptorSet vk_pipeline_empty_set(void);
            VkDescriptorSet two[2] = {vk_pipeline_empty_set(), bls};
            if (!gfx && two[0]) vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, 1, 2, two, 0, NULL);
            else vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, 2, 1, &bls, 0, NULL);
            bl_cb[k] = ex->cb, bl_layout[k] = info->layout;
            ex->recorded++;
        }
    }
    for (uint32_t k = 0; k < n_sets; k++) {
        uint32_t s = set1_first ? 1 - k : k, i0 = lo[s], i1 = hi[s];
        const VkDescSlot *ds = &d[d0[s]];
        uint32_t dbytes = (uint32_t)(((s + 1 < n_sets ? d0[s + 1] : nd) - d0[s]) * sizeof *d);
        if (s && i1 == i0) {               // no fragment stage: set 1 is empty and unused
            if (set1_first) {              // bound only to drop the layer's push set (comment above)
                VkDescriptorSet set = alloc_set(ex, info->set_layout1);
                vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, 1, 1, &set, 0, NULL);
                B.ds_bound[1] = set, B.ds_pushed[1] = 0;
                RS.cmds++;
            }
            continue;
        }
        if (s ? info->push1 : info->push) {
            uint32_t n = gfx && push_skip_on() ? dbytes : 0;
            if (n && B.ds_pushed[s] && DS.pn[s] == n && !memcmp(DS.psig[s], ds, n)) {
                BS.push_skip++;
                continue;
            }
            if (info->tpl[s]) rec_push_tpl(vk_pipeline_push_tpl_fn(), ex->cb, info->tpl[s], info->layout, s, ds, dbytes);
            else rec_push(vk_pipeline_push_fn(), ex->cb, info->bind_point, info->layout, s, i1 - i0, w + i0);
            BS.push++;
            RS.cmds++;
            if (gfx) {
                B.ds_bound[s] = VK_NULL_HANDLE, B.ds_pushed[s] = n != 0;
                if (n) DS.pn[s] = n, memcpy(DS.psig[s], ds, n);
            }
        } else if (!gfx || s > 1) {
            VkDescriptorSet set = alloc_set(ex, s ? info->set_layout1 : info->set_layout);
            write_set(info, s, set, ds, w, i0, i1);
            vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &set, 0, NULL);
        } else {
            VkDescriptorSetLayout sl = s ? info->set_layout1 : info->set_layout;
            VkDescriptorSet set = B.ds_last[s];
            if (!set || B.ds_last_layout[s] != sl || DS.n[s] != dbytes || memcmp(DS.sig[s], ds, dbytes)) {
                DsCacheEnt *ce = NULL;
                uint64_t h1 = 0, h2 = 0;
                if (dsc_on()) {
                    CbRes *cr = cb_res(ex);
                    if (!cr->dsc) cr->dsc = calloc(DSC_N, sizeof *cr->dsc);
                    if (cr->dsc) {
                        ds_hash(ds, dbytes, &h1, &h2);
                        ce = &cr->dsc[(h1 ^ (uint64_t)(uintptr_t)sl * 0x9E3779B97F4A7C15ull) & (DSC_N - 1)];
                    }
                }
                if (ce && ce->set && ce->h1 == h1 && ce->h2 == h2 && ce->sl == sl && ce->n == dbytes) {
                    set = ce->set;
                    BS.set_reuse++;
                } else {
                    set = alloc_set(ex, sl);
                    write_set(info, s, set, ds, w, i0, i1);
                    if (ce) *ce = (DsCacheEnt){h1, h2, sl, dbytes, set};
                }
                B.ds_last[s] = set, B.ds_last_layout[s] = sl, DS.n[s] = dbytes;
                memcpy(DS.sig[s], ds, dbytes);
                BS.set_new++;
                RS.cmds++;
            } else {
                BS.set_reuse++;
            }
            if (B.ds_bound[s] != set) {
                vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &set, 0, NULL);
                B.ds_bound[s] = set, B.ds_pushed[s] = 0;
                RS.cmds++;
            }
        }
    }
    if (skip) {
        RS.rec = 0;
        // An equal next draw records nothing when every push set holds its push (TOTK_PUSH_SKIP on) and nothing made
        // the build unrepeatable; allocated sets are always bound to the one written last.
        int valid = !RS.volatile_ && !RS.cnt_over && RS.hn;
        for (uint32_t s = 0; s < n_sets; s++)
            if (hi[s] > lo[s] && (s ? info->push1 : info->push) && !B.ds_pushed[s]) valid = 0;
        if (skip == 2) {   // with the pipeline info equal (bind_sig), the packed descriptors determine every write
            uint32_t n = (uint32_t)(nd * sizeof *d);
            int wr = n != RS.ckn || memcmp(RS.ck, d, n);
            int cn = RS.ncnt != RS.pncnt || memcmp(RS.cnt, RS.pcnt, sizeof RS.cnt[0] * (size_t)RS.ncnt);
            if (same && was_valid && (wr || cn || RS.cmds))
                rt_fatal("vk_exec: descriptor build skip check: a draw of program %llu with the inputs of the previous one "
                         "gets %s", (unsigned long long)ex->prog->serial,
                         RS.cmds ? "descriptor commands" : wr ? "other writes" : "other census counts");
            if (same && was_valid) BS.bind_skip++;   // the draws a skip would have spared, compared instead
            RS.ckn = n;
            memcpy(RS.ck, d, n);
        }
        B.res_valid = valid;
    }
    ex->recorded++;
}

// ---- draws

static void bind_pipeline(VkExec *ex, VkPipelineBindPoint bp, VkPipeline p) {
    int k = bp == VK_PIPELINE_BIND_POINT_COMPUTE;
    if (B.pipe[k] == p) return;
    vkCmdBindPipeline(ex->cb, bp, p);
    if (vk_ctx()->has_nv_checkpoints && vk_checkpoints_on()) {   // DKB_GPU_CHECKPOINTS=1: name the shaders the checkpoint's pipeline holds
        uint64_t o[3] = {0};
        const uint8_t *ctl, *code;
        uint32_t len;
        static const int stages[2][3] = {{NVN_STAGE_VS, NVN_STAGE_GS, NVN_STAGE_FS}, {NVN_STAGE_CS, -1, -1}};
        for (int i = 0; i < 3 && ex->prog; i++)
            if (stages[k][i] >= 0 && find_stage(ex->prog, stages[k][i], &ctl, &code, &len)) o[i] = nvn2_origin(code);
        vk_checkpoint_note(p, o[0], o[1], o[2]);
        rec_checkpoint(ex->cb, p);
    }
    ex->recorded++;
    B.pipe[k] = p;
    if (!k) B.st_valid = 0;
}

// Kirby ultrawide (KAR_WIDE=1, RELEASE_PLAN 17.3): the game records its pass rectangles at the 16:9 sizes while the
// karwide patches make the targets wider. A rectangle from (0,0) with a 16:9 size, the target's height and a smaller
// width is widened to the target.
static int kar_wide_on(void) {
    static int on = -1;
    if (on < 0) on = rt_game_is_kar() && kar_wide_active();
    return on;
}
static int kar_widen(float x, float y, float w, float h, uint32_t pw, uint32_t ph) {
    int on = kar_wide_on() && x == 0 && y == 0 && fabsf(h - (float)ph) <= 1 && w < (float)pw && fabsf(w * 9 - h * 16) <= 18;
    static uint64_t seen[32];
    static int nseen;
    uint64_t key = (uint64_t)pw << 32 | (uint64_t)ph << 16 | (uint64_t)w;
    if (on && nseen < 32) {   // each widened (target, rectangle) once, to tell scene passes from interface ones
        int k = 0;
        while (k < nseen && seen[k] != key) k++;
        if (k == nseen) seen[nseen++] = key, rt_log("kar wide: widen %gx%g to the %ux%u target", w, h, pw, ph);
    }
    return on;
}

static void record_fixed(VkExec *ex) {
    VkCommandBuffer cb = ex->cb;
    if (ex->dirty & VK_EXEC_DIRTY_VIEWPORT) {
        float x = 0, y = 0, w = (float)ex->pass_w, h = (float)ex->pass_h;
        if (ex->has_vp) x = ex->vp[0][0], y = ex->vp[0][1], w = ex->vp[0][2], h = ex->vp[0][3];
        if (kar_widen(x, y, w, h, ex->pass_w, ex->pass_h)) {   // interface layer: centre; other targets: stretch
            if (kar_wide_ui(ex->pass_w, ex->pass_h)) x = ((float)ex->pass_w - w) / 2;
            else w = (float)ex->pass_w;
        }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        VkViewport v = {x, y + h, w, -h, ex->depth_range[0], ex->depth_range[1]};   // the y flip: negative height
        vkCmdSetViewportWithCount(cb, 1, &v);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_SCISSOR) {
        VkRect2D r = vk_exec_scissor(ex);
        if (kar_widen((float)r.offset.x, (float)r.offset.y, (float)r.extent.width, (float)r.extent.height, ex->pass_w,
                      ex->pass_h)) {
            if (kar_wide_ui(ex->pass_w, ex->pass_h)) r.offset.x = (int32_t)(ex->pass_w - r.extent.width) / 2;
            else r.extent.width = ex->pass_w;
        }
        vkCmdSetScissorWithCount(cb, 1, &r);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_DEPTH_BIAS) {
        vkCmdSetDepthBias(cb, ex->poly_offset.units, ex->poly_offset.clamp, ex->poly_offset.factor);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_BLEND_CONST) {
        vkCmdSetBlendConstants(cb, ex->blend_color);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_STENCIL) {
        for (int f = 0; f < 2; f++) {
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            vkCmdSetStencilCompareMask(cb, face, ex->st_vmask[f]);
            vkCmdSetStencilWriteMask(cb, face, ex->st_wmask[f]);
            vkCmdSetStencilReference(cb, face, ex->st_ref[f]);
            ex->recorded += 3;
        }
    }
    ex->dirty = 0;
}

static void bind_vertex_buffers(VkExec *ex) {
    VkBuffer vb[17];
    VkDeviceSize vo[17], vsz[17], vstr[17];
    int vcls[17] = {0};
    void *vpool[17] = {0};
    int dyn = vk_pipeline_vertex_input_dynamic();
    // A stream whose range is the one its bound buffer was resolved from, with no pool or mirror change since
    // (vk_mem_gen, read before any resolve), resolves to that buffer again: most draws change one stream or none.
    uint64_t gen = vk_mem_gen();
    int same_gen = B.vb_valid && B.vgen == gen;
    for (int b = 0; b < 17; b++) {
        vb[b] = Z.zbuf, vo[b] = 0, vsz[b] = ZERO_BYTES, vstr[b] = b < 16 ? ex->pipe.vstream[b].stride : 0;
        if (b == VK_PIPE_ZERO_BINDING) continue;
        const NVNbufferRange *r = &ex->vbuf[b];
        if (same_gen && B.vin[b].address == r->address && B.vin[b].size == r->size) {
            vb[b] = B.vb[b], vo[b] = B.vo[b], vsz[b] = B.vsz[b], vcls[b] = B.vcls[b], vpool[b] = B.vpool[b];
            continue;
        }
        VkMemSpan sp;
        if (!r->address || !vk_mem_resolve(r->address, 1, &sp)) continue;
        VK_MEM_USE(sp);
        uint64_t n = r->size && r->size < sp.avail ? r->size : sp.avail;
        vb[b] = sp.buffer, vo[b] = sp.offset, vsz[b] = n, vcls[b] = sp.cls, vpool[b] = sp.pool;
    }
    memcpy(B.vcls, vcls, sizeof vcls), memcpy(B.vpool, vpool, sizeof vpool);
    memcpy(B.vin, ex->vbuf, sizeof B.vin);
    B.vgen = gen;
    if (B.vb_valid && !memcmp(B.vb, vb, sizeof vb) && !memcmp(B.vo, vo, sizeof vo) && !memcmp(B.vsz, vsz, sizeof vsz) &&
        (dyn || !memcmp(B.vstr, vstr, sizeof vstr)))
        return;
    vkCmdBindVertexBuffers2(ex->cb, 0, 17, vb, vo, vsz, dyn ? NULL : vstr);
    ex->recorded++;
    for (int b = 0; b < 17; b++)
        if (vcls[b]) vk_mem_count(vcls[b], VK_MUSE_VB, vsz[b]);
    if (rt_trace & RT_TRACE_PERF)
        for (int b = 0; b < 17; b++)
            if (vpool[b]) vk_mem_bound_note(vpool[b]);
    memcpy(B.vb, vb, sizeof vb), memcpy(B.vo, vo, sizeof vo), memcpy(B.vsz, vsz, sizeof vsz), memcpy(B.vstr, vstr, sizeof vstr);
    B.vb_valid = 1;
}

// TOTK_PIPE_SKIP: which draws are skipped, rather than waited for, while their shaders or pipeline build.
// strict (0): only draws whose pass targets were all rendered in the previous presented frame; a pass with a target
//   that was not counts as one-shot and waits.
// any (1): also draws in a pass with at least one target rendered in the previous frame (a per-frame depth
//   buffer or g-buffer with a colour target the frame before did not use: half-resolution effect buffers in fights).
//   Passes with none (volumes, LUTs, shadow layers seen for the first time) still wait.
// fullscreen (2, default): as any, and full-screen draws into such passes are skipped instead of waiting from their
//   budget. A full-screen pass whose stage libraries are cold in the driver cache took 86 ms on average (up to 194 ms)
//   in a fight on an RX 9070: three frames in a row each spent the 50 ms budget, and some of the draws were skipped
//   after all. Skipping costs the pass for the frames until the pipeline is ready.
static int pipe_skip_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = cfg_value("TOTK_PIPE_SKIP");
        mode = !strcmp(e, "strict") || !strcmp(e, "0") ? 0 : !strcmp(e, "fullscreen") || !strcmp(e, "2") ? 2 : 1;
        static const char *const what[3] = {"only draws into per-frame passes (TOTK_PIPE_SKIP=strict)",
                                            "draws into passes with a target the previous frame rendered",
                                            "those and full-screen draws (TOTK_PIPE_SKIP=fullscreen)"};
        rt_log("vk_exec: draws skipped while their pipeline builds: %s", what[mode]);
    }
    return mode;
}

// The previous draw's shaders, kept while its program, bound pipeline state, the vertex and fragment stages' first 32
// texture and image handles and every slot's resolution (vk_texture_slots_gen, read unlocked: a registration racing
// with it is ordered after this draw, as answers_for's own check would order it) stay the same: answers_for reads
// nothing else, so shader_for would return the same shaders. About 65% of field draws repeat them
// (TOTK_BIND_STATS). TOTK_SHADER_MEMO=0 turns it off; =check looks the shaders up on every hit too, and every slot's
// texture type in answers_for, and stops on a difference.
// tex_serial (bumped by every texture and image bind) equal to the key's means the handles were not rebound since
// they were compared, so the 1 KiB compare is skipped; a rebind to the same handles still matches by the compare.
typedef struct {
    const prog_t *prog;
    uint64_t serial;
    uint32_t gen, tex_serial;
    uint64_t tex[3][32], img[3][32];
    uint8_t geometry_input;
    VkPipeState pipe;
} MemoKey;

// Workaround for the GPU fault after the first guide movie (#9). On NVIDIA (drivers 617.14, 617.42 and 621.01) the
// draws of Bananza's instanced light quads whose vertex stage fetches ten per-instance streams (an input at Location
// 9) and whose fragment stage reads the storage buffer at c0+0x530 fault the device with an invalid instruction
// pointer, or crash the driver's shader compiler. Leaving out one such shader moves the fault to another of the
// family, and the same SPIR-V drawn on its own does not fault, so the cause is still open. Skipping these draws
// loses those lights; the family's quads without an input at Location 9 draw as before. It is a mitigation, not a fix:
// in 600 s runs on an RTX 5060 Ti the fault came minutes later in one run of two (another shader), where without it
// every run faulted right after the first guide movie. TOTK_FIX_VS_FAULT=0 draws them.
//
// The later fault, around seven minutes in, comes from one more quad of the same shape whose fragment stage does not
// read c0+0x530: NVN 2 vertex stage cae0ef8e9fe9147f, nine per-instance RGBA32F streams, one instance. In 9 runs of
// 600 s every run where it drew faulted, with its draw among those in flight at the device loss, and the runs where it
// never drew did not; skipping it gave 0 faults in 4 runs. Quads that never fault have its shape: vertex stages
// 782b2585777d2b4e, 392e2956038a9e64 and 7d85669ac430f01a read Location 9, feed every attribute per instance and carry
// the same integer code (four reads of c9 at a computed index), and skipping every such quad left out ten vertex stages
// that drew in all 17 logged runs. So it is matched by the name of its NVN 2 stage.
static int light_quad_fault_skip(const VkExec *ex, const GfxShader *vs, const GfxShader *fs) {
    static int on = -1;
    if (on < 0) on = !rt_game_version && cfg_bool("TOTK_FIX_VS_FAULT") && vk_ctx()->props.vendorID == 0x10de;   // 0: Bananza
    if (!on || !vs || !fs) return 0;
    const uint8_t *ctl, *code = NULL;
    uint32_t len;
    if (find_stage(ex->prog, NVN_STAGE_VS, &ctl, &code, &len) && nvn2_origin(code) == 0xcae0ef8e9fe9147full) {
        warn_once(W_FAULT_SKIP2, "the light quad of NVN 2 vertex stage cae0ef8e9fe9147f is skipped on NVIDIA (#9)");
        return 1;
    }
    const GfxShaderRes *r;
    int n = gfx_shader_resources(fs, GFX_RES_SSBO, &r), lights = 0;
    for (int i = 0; i < n; i++) lights |= r[i].cbuf_slot == 0 && r[i].handle * 4 == 0x530;
    GfxShaderIo io[32];
    if (!lights || !(gfx_shader_inputs(vs, io) >> 9 & 1)) return 0;
    warn_once(W_FAULT_SKIP, "light-quad draws with ten instance streams are skipped on NVIDIA (#9; TOTK_FIX_VS_FAULT=0 draws them)");
    return 1;
}

// DKB_SKIP_VS / DKB_SKIP_FS (debug only, for bisecting the device loss of #75): comma-separated 16-digit hex NVN 2
// stage origins. A draw whose vertex (fragment) stage is listed in the matching variable is skipped, so a run that
// stops faulting names the culprit shader. Parsed once; unset costs one flag test per draw.
typedef struct { int n; uint64_t list[16]; } SkipList;
static int skip_listed(const VkExec *ex, int stage, SkipList *sl, const char *var, int warn, const char *msg) {
    if (sl->n < 0) {
        sl->n = 0;
        const char *e = cfg_get(var);
        while (e && *e && sl->n < 16) {
            char *end;
            uint64_t v = strtoull(e, &end, 16);
            if (end == e) break;
            sl->list[sl->n++] = v;
            e = *end == ',' ? end + 1 : end;
        }
    }
    if (!sl->n) return 0;
    const uint8_t *ctl, *code = NULL;
    uint32_t len;
    if (!find_stage(ex->prog, stage, &ctl, &code, &len)) return 0;
    uint64_t o = nvn2_origin(code);
    for (int i = 0; i < sl->n; i++)
        if (sl->list[i] == o) return warn_once(warn, msg), 1;
    return 0;
}

static int skip_vs_listed(const VkExec *ex) {
    static SkipList sl = {-1};
    return skip_listed(ex, NVN_STAGE_VS, &sl, "TOTK_SKIP_VS", W_SKIP_VS, "DKB_SKIP_VS is active: draws of the listed NVN 2 vertex stages are skipped");
}

static int skip_fs_listed(const VkExec *ex) {
    static SkipList sl = {-1};
    return skip_listed(ex, NVN_STAGE_FS, &sl, "TOTK_SKIP_FS", W_SKIP_FS, "DKB_SKIP_FS is active: draws of the listed NVN 2 fragment stages are skipped");
}

static struct { int on, valid, has_fs, has_gs; MemoKey k; const GfxShader *vs, *fs, *gs; } SM = {-1};

// Numeric handles are relative to the active descriptor tables. Keep the per-handle memos only while those tables
// and their address mappings stay unchanged. A sampler table switch also changes combined image descriptors.
static void descriptor_state(VkExec *ex) {
    static struct { const VkExec *ex; dpool_ref tex, samp; uint64_t mem_gen; uint32_t slots_gen; } last;
    uint64_t mg = vk_mem_gen();
    uint32_t sg = vk_texture_slots_gen();
    if (last.ex != ex || last.tex.addr != ex->tex_pool.addr || last.tex.count != ex->tex_pool.count ||
        last.samp.addr != ex->samp_pool.addr || last.samp.count != ex->samp_pool.count || last.mem_gen != mg) {
        if (!++descriptor_serial) rt_fatal("vk_exec: descriptor context serial exhausted");
        SM.valid = 0;
        ex->fb_fs = NULL;
        last.ex = ex, last.tex = ex->tex_pool, last.samp = ex->samp_pool, last.mem_gen = mg;
    }
    if (last.slots_gen != sg) ex->fb_fs = NULL;
    last.slots_gen = sg;
}

static void memo_key(const VkExec *ex, uint32_t gen, MemoKey *k) {
    k->prog = ex->prog, k->serial = ex->prog->serial, k->gen = gen, k->tex_serial = ex->tex_serial;
    memcpy(k->tex[0], ex->tex[NVN_STAGE_VS], sizeof k->tex[0]), memcpy(k->tex[1], ex->tex[NVN_STAGE_FS], sizeof k->tex[1]);
    memcpy(k->img[0], ex->img[NVN_STAGE_VS], sizeof k->img[0]), memcpy(k->img[1], ex->img[NVN_STAGE_FS], sizeof k->img[1]);
    k->geometry_input = geometry_input;
    if (geometry_input) {
        memcpy(k->tex[2], ex->tex[NVN_STAGE_GS], sizeof k->tex[2]);
        memcpy(k->img[2], ex->img[NVN_STAGE_GS], sizeof k->img[2]);
    }
    memcpy(&k->pipe, &ex->pipe, sizeof k->pipe);
}

static int memo_match(VkExec *ex, uint32_t gen, MemoKey *k) {
    descriptor_state(ex);
    if (!SM.valid) return 0;
    if (k->prog != ex->prog || k->serial != ex->prog->serial || k->gen != gen || k->geometry_input != geometry_input ||
        memcmp(&k->pipe, &ex->pipe, sizeof k->pipe))
        return 0;
    if (geometry_input && (memcmp(k->tex[2], ex->tex[NVN_STAGE_GS], sizeof k->tex[2]) ||
                           memcmp(k->img[2], ex->img[NVN_STAGE_GS], sizeof k->img[2]))) return 0;
    if (k->tex_serial == ex->tex_serial) return 1;
    if (memcmp(k->tex[0], ex->tex[NVN_STAGE_VS], sizeof k->tex[0]) || memcmp(k->tex[1], ex->tex[NVN_STAGE_FS], sizeof k->tex[1]) ||
        memcmp(k->img[0], ex->img[NVN_STAGE_VS], sizeof k->img[0]) || memcmp(k->img[1], ex->img[NVN_STAGE_FS], sizeof k->img[1]))
        return 0;
    k->tex_serial = ex->tex_serial;
    return 1;
}

static int shader_memo_on(void) {
    if (SM.on < 0) {
        const char *e = cfg_value("TOTK_SHADER_MEMO");
        SM.on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
    }
    return SM.on;
}

static struct { int on, valid; VkPipeState s; const VkPipeInfo *info; } PM = {-1};

static int pipe_memo_on(void) {
    if (PM.on < 0) PM.on = cfg_bool("TOTK_PIPE_MEMO");
    return PM.on;
}

static int experimental_geometry_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_bool("TOTK_EXPERIMENTAL_GEOMETRY");
    return on;
}

// 1: an opted-in ordinary GS is required; 0: preserve the existing VS/FS path; -1: this opted-in draw is unsupported.
// Inspect raw NVN2 markers before any NVN1 control offsets. TOTK_EXPERIMENTAL_GEOMETRY admits no native shader; only
// DKB_NATIVE_GS does, for the passthrough subset checked first.
static int geometry_draw(VkExec *ex, uint32_t prim) {
    geometry_input = 0;
    if (native_gs_on() && ex->prog) {   // native failures keep VS/FS; ordinary stages retain their separate opt-in
        const uint8_t *ctl, *code, *fctl, *fcode;
        uint32_t len, flen, sph[5];
        if (!find_stage(ex->prog, NVN_STAGE_GS, &ctl, &code, &len)) return 0;
        if (nvn2_origin(code)) {
            if (len < 0x30 + 0x60 || !find_stage(ex->prog, NVN_STAGE_FS, &fctl, &fcode, &flen) ||
                vk_geometry_input(prim) != GFX_GEOMETRY_TRIANGLES) return 0;
            memcpy(sph, code + 0x30, sizeof sph);
            if (!(sph[0] & (1u << 24)) || sph[2] >> 24 != 1 || (sph[3] >> 24 & 15) != 7 || (sph[4] & 0xfff) != 1) {
                warn_once(W_GEOMETRY, "native GS: a stage outside the passthrough subset keeps the vertex and fragment path");
                return 0;
            }
            // Metal has no geometry stage (MoltenVK): skip before the pipeline and the lowered GS are built. The vertex
            // and fragment stages alone would draw black, so the draw is dropped and counted in draws skipped.
            if (!vk_geometry_supported()) {
                warn_once(W_GEOMETRY, "native GS: the device has no geometryShader: draws that need one are skipped");
                __atomic_fetch_add(&gfx_counters.draws_skipped, 1, __ATOMIC_RELAXED);
                return -1;
            }
            geometry_input = GFX_GEOMETRY_TRIANGLES;
            return 1;
        }
        // An ordinary GS still follows its own opt-in and validation below.
    }
    if (!experimental_geometry_on() || !ex->prog) return 0;
    int found = 0, portable = 0;
    uint32_t stages = 0;
    if (ex->prog->nstage > 6) goto unsupported;
    for (uint32_t i = 0; i < ex->prog->nstage; i++) {
        const uint8_t *ctl = ex->prog->sd[i].control;
        const uint8_t *code = (const uint8_t *)(uintptr_t)ex->prog->sd[i].data;
        if (!ctl || !code || gfx_shader_is_nvn2(ctl)) goto unsupported;
        uint32_t stage, len;
        memcpy(&stage, ctl + 0x714, 4);
        if (stage == NVN_STAGE_VS || stage == NVN_STAGE_FS) { stages |= 1u << stage; continue; }
        if (stage == NVN_STAGE_CS) continue;
        if (stage != NVN_STAGE_GS || found++) goto unsupported;   // no tessellation, duplicate or unknown stage
        stages |= 1u << stage;
        memcpy(&len, ctl + 0x704, 4);
        if (len < 0x30 + 0x60 || nvn2_origin(code) || c0_on()) goto unsupported;
        uint32_t sph;
        memcpy(&sph, code + 0x30, 4);
        if ((sph >> 10 & 15) != 4) goto unsupported;
        if (sph & (1u << 24)) {
            uint32_t skip[8];
            if (nvn_geometry_control_get(ctl, 0x880, code, len, skip) != 1 ||
                vk_geometry_input(prim) != GFX_GEOMETRY_TRIANGLES) goto unsupported;
            portable = 1;
        }
    }
    if (found) {
        // Replay currently replaces a complete program, not individual stages. Partial GS binds need a separate
        // merging contract; do not execute a GS which this bind did not request.
        if (!(stages & (1u << NVN_STAGE_VS)) || ((uint32_t)ex->prog_stages & stages) != stages) goto unsupported;
        // Portable metadata currently follows the verified whole-program SDK binding path.
        if (portable && ((uint32_t)ex->prog_stages != NVN_SHADER_STAGE_ALL_GRAPHICS_BITS ||
                         !(stages & (1u << NVN_STAGE_FS)))) goto unsupported;
        geometry_input = vk_geometry_input(prim);
        if (!geometry_input) goto unsupported;
    }
    return found;
unsupported:
    geometry_input = 0;
    warn_once(W_GEOMETRY, "experimental geometry draw rejected: ordinary GS, complete stage mask, supported primitive and FIX_C0=0 are required");
    return -1;
}

// Pass, shaders, pipeline, fixed state and resources for a draw. 0: the draw is skipped.
// Why draws were skipped, by reason, logged every 50000 prepare_draw calls (KAR diagnostic).
static volatile uint64_t skip_ct[16], skip_calls;
static const char *const skip_names[16] = {"disabled", "gs", "scissor", "no program", "no vs", "bisect", "shader pending", "listed", "shader failed", "feedback", "pipe pending", "pipe failed", "drawn"};
#define SKIP(r) return (skip_ct[r]++, 0)
static int prepare_draw_(VkExec *ex, int prim, int32_t count, int restart);
static int prepare_draw(VkExec *ex, int prim, int32_t count, int restart) {
    static int clear_on = -1;   // KAR_CLEAR_AT (vk_texture.c vk_texture_clear_gpu_at): before this draw is prepared
    if (clear_on < 0) clear_on = getenv("KAR_CLEAR_AT") != NULL;
    if (clear_on) vk_texture_clear_gpu_at(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
    static int reup_on = -1;   // KAR_REUPLOAD_AT (vk_texture.c vk_texture_reupload_at)
    if (reup_on < 0) reup_on = getenv("KAR_REUPLOAD_AT") != NULL;
    if (reup_on) vk_texture_reupload_at(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
    int r = prepare_draw_(ex, prim, count, restart);
    if (r) skip_ct[12]++;
    // Every 50000 draws, logged when draws were skipped since the last census (or always with KAR_DIAG=1).
    static uint64_t last_skipped;
    static int diag = -1;
    if (diag < 0) diag = getenv("KAR_DIAG") && *getenv("KAR_DIAG") == '1';
    if (++skip_calls % 50000 == 0 && (diag || skip_calls - skip_ct[12] != last_skipped)) {
        last_skipped = skip_calls - skip_ct[12];
        char b[512]; size_t o = 0;
        for (int i = 0; i < 13; i++) o += (size_t)snprintf(b + o, sizeof b - o, " %s %llu", skip_names[i], (unsigned long long)skip_ct[i]);
        rt_log("vk_exec: draw census over %llu:%s", (unsigned long long)skip_calls, b);
    }
    return r;
}
static int prepare_draw_(VkExec *ex, int prim, int32_t count, int restart) {
    if (ex->render_disabled) SKIP(0);
    static int skip_all = -1;   // DKB_SKIP_DRAWS=1: drop every draw (bisecting GPU faults)
    if (skip_all < 0) skip_all = getenv("DKB_SKIP_DRAWS") != NULL;
    if (skip_all) return 0;
    int want_gs = geometry_draw(ex, (uint32_t)prim);
    if (want_gs < 0) SKIP(1);
    cb_res(ex);
    dummies_init();
    if (ex->pass_noatt) {   // no attachment: the pass covers this draw's viewport or scissor, reopened when it needs more
        uint32_t w, h;
        vk_exec_noatt_dims(ex, &w, &h);
        if (ex->in_pass && (w > ex->pass_w || h > ex->pass_h)) vk_exec_pass_end(ex);
        if (!ex->in_pass) ex->rt_resolved = 0;
    }
    if (!ex->in_pass) ex->dirty |= VK_EXEC_DIRTY_VIEWPORT | VK_EXEC_DIRTY_SCISSOR;
    vk_exec_pass_begin(ex);
    VkRect2D sc = vk_exec_scissor(ex);
    if (!sc.extent.width || !sc.extent.height) SKIP(2);
    if (!ex->prog) { warn_once(W_PROGRAM, "a draw without a bound program is skipped"); SKIP(3); }
    int has_vs, has_fs, has_gs = 0, vs_wait = 0, fs_wait = 0, gs_wait = 0, memo = shader_memo_on();
    const GfxShader *vs, *fs, *gs = NULL;
    uint32_t gen = memo ? vk_texture_slots_gen() : 0;   // before shader_for, whose lookups may bump it
    if (memo && SM.valid && memo_match(ex, gen, &SM.k)) {
        vs = SM.vs, fs = SM.fs, gs = SM.gs, has_vs = 1, has_fs = SM.has_fs, has_gs = SM.has_gs;
        if (memo == 2) {
            int hv, hf, hg = 0, wv, wf, wg = 0;
            const GfxShader *v2 = shader_for(ex, NVN_STAGE_VS, &hv, &wv, 0), *f2 = shader_for(ex, NVN_STAGE_FS, &hf, &wf, 0);
            const GfxShader *g2 = want_gs ? shader_for(ex, NVN_STAGE_GS, &hg, &wg, 0) : NULL;
            if (v2 != vs || f2 != fs || g2 != gs || hf != has_fs || hg != has_gs || wv || wf || wg)
                rt_fatal("vk_exec: shader memo of program %llu differs from the lookup", (unsigned long long)ex->prog->serial);
        }
    } else {
        SM.valid = 0;
        vs = shader_for(ex, NVN_STAGE_VS, &has_vs, &vs_wait, 0);
        fs = shader_for(ex, NVN_STAGE_FS, &has_fs, &fs_wait, 0);
        native_gs_vs = vs, native_gs_fs = fs;
        if (want_gs) gs = shader_for(ex, NVN_STAGE_GS, &has_gs, &gs_wait, 0);
        if (memo && vs && !vs_wait && !fs_wait && !gs_wait && (!has_fs || fs) && (!want_gs || (has_gs && gs)))
            memo_key(ex, gen, &SM.k), SM.vs = vs, SM.fs = fs, SM.gs = gs, SM.has_fs = has_fs, SM.has_gs = has_gs, SM.valid = 1;
    }
    if (!has_vs) { warn_once(W_NO_VS, "a draw with a program that has no vertex stage is skipped"); SKIP(4); }
    static int bis_lo = -1, bis_hi = 255;   // DKB_DRAW_BISECT=lo-hi (hex): only draws whose program byte is in range
    if (bis_lo < 0) {
        const char *e = getenv("DKB_DRAW_BISECT");
        bis_lo = 0;
        if (e) sscanf(e, "%x-%x", &bis_lo, &bis_hi);
    }
    if (vs && (bis_lo > 0 || bis_hi < 255)) {
        int b = gfx_shader_input_id(vs)[0] ^ (fs ? gfx_shader_input_id(fs)[0] : 0);
        if (b < bis_lo || b > bis_hi) SKIP(5);
        if (bis_lo == bis_hi) {
            static int told;
            if (told++ < 4) {   // the cache file names (shader-cache/<key>.spv)
                char k[2][65] = {{0}};
                for (int i = 0; i < 32; i++) {
                    sprintf(k[0] + 2 * i, "%02x", gfx_shader_key(vs)[i]);
                    if (fs) sprintf(k[1] + 2 * i, "%02x", gfx_shader_key(fs)[i]);
                }
                const uint8_t *ctl, *vcode = NULL, *fcode = NULL;
                uint32_t len;
                find_stage(ex->prog, NVN_STAGE_VS, &ctl, &vcode, &len), find_stage(ex->prog, NVN_STAGE_FS, &ctl, &fcode, &len);
                rt_log("bisect: draw byte %#x vs %s (NVN 2 dump %016llx) fs %s (%016llx)", b, k[0],
                       (unsigned long long)nvn2_origin(vcode), k[1], (unsigned long long)nvn2_origin(fcode));
            }
        }
    }
    // A draw into a target the previous frame did not render (or into none) waits for a pending shader or pipeline:
    // one-shot draws (volumes and LUTs generated once, later only sampled) would leave their target empty for the
    // session. A draw into per-frame targets is skipped until they are ready and comes back next frame.
    int oneshot = -1, skip = pipe_skip_mode();   // -1: not looked up
    if (vs_wait || fs_wait || gs_wait) {
        if (!(oneshot = !vk_exec_targets_recent(ex, skip >= 1))) { perf_count(PERF_CNT_SHADER_SKIP); SKIP(6); }   // skipped until ready
        perf_count(PERF_CNT_ONESHOT_WAIT);
        if (vs_wait) vs = shader_for(ex, NVN_STAGE_VS, &has_vs, NULL, 1);
        if (fs_wait) fs = shader_for(ex, NVN_STAGE_FS, &has_fs, NULL, 1);
        native_gs_vs = vs, native_gs_fs = fs;
        if (gs_wait) gs = shader_for(ex, NVN_STAGE_GS, &has_gs, NULL, 1);
    }
    if (light_quad_fault_skip(ex, vs, fs) || skip_vs_listed(ex) || skip_fs_listed(ex)) SKIP(7);
    if (!vs || (has_fs && !fs) || (want_gs && (!has_gs || !gs))) SKIP(8);
    if (!geometry_feedback_safe(ex, vs, gs)) SKIP(9);
    uint32_t fbl = feedback_prepare(ex, fs);
    VkPipeState s = ex->pipe;
    s.vs = vs, s.fs = fs, s.gs = gs;
    s.feedback = (uint8_t)((fbl & 0xff ? 1 : 0) | (fbl & 0x100 ? 2 : 0));
    s.prim = (uint8_t)prim;
    s.depth_minus_w = (uint8_t)(nvn_depth_minus_w() != 0);
    s.y_flip = 1;
    s.prim_restart = (uint8_t)(restart != 0);
    const VkPipeInfo *info = NULL;
    int pending;
    // A draw of at most 6 vertices over half the render area or more (a full-screen triangle or quad, a tile) waits for
    // its pipeline: skipping it can leave a black tile. The waits share 50 ms per presented frame
    // (TOTK_PIPE_SKIP=fullscreen skips them instead). A one-shot draw waits without that budget. Everything else is
    // skipped while its pipeline is built.
    static uint64_t wait_frame;
    static uint64_t wait_left_ns;
    uint32_t wait_ms = 0;
    uint64_t t0 = 0;
    // KAR_ONESHOT_BUDGET (default 1, =0 the old way): one-shot draws (targets neither recent nor steady) also wait from
    // the frame's 50 ms pool instead of up to ONESHOT_WAIT_MS each. Waiting 30 s per draw made area changes and the
    // first race frames stall 300-650 ms (frametime "replay"); a skipped one-shot draw comes back the next time its
    // target is drawn, with the pipeline built by a worker meanwhile.
    static int oneshot_budget = -1;
    if (oneshot_budget < 0) oneshot_budget = !getenv("KAR_ONESHOT_BUDGET") || *getenv("KAR_ONESHOT_BUDGET") != '0';
    {
        uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        if (f != wait_frame) wait_frame = f, wait_left_ns = 50000000;
    }
    if (oneshot > 0) {
        wait_ms = oneshot_budget ? (uint32_t)(wait_left_ns / 1000000) : ONESHOT_WAIT_MS;
        if (oneshot_budget && wait_ms) t0 = kern_now_ns();
    } else if (skip < 2 && count <= 6 && (uint64_t)sc.extent.width * sc.extent.height * 2 >= (uint64_t)ex->pass_w * ex->pass_h) {
        wait_ms = (uint32_t)(wait_left_ns / 1000000);
        if (wait_ms) t0 = kern_now_ns();
    }
    // The previous draw's pipeline while its state repeats (PM, TOTK_PIPE_MEMO=0 turns it off): the cache key is made
    // from the state alone, and a pipeline once found only changes from its fast link to its optimised build, read
    // from the info the cache entry keeps.
    VkPipeline p;
    const char *why = NULL;
    uint64_t w0 = 0;
    if (pipe_memo_on() && PM.valid && !memcmp(&PM.s, &s, sizeof s)) {
        info = PM.info, pending = 0;
        p = __atomic_load_n(&info->pipeline, __ATOMIC_ACQUIRE);
    } else {
        w0 = t0 ? t0 : wait_ms ? kern_now_ns() : 0;   // the clock is read only for draws that may wait
        p = vk_pipeline_graphics_async(&s, &info, &pending, wait_ms);
        if (t0) {
            uint64_t spent = kern_now_ns() - t0;
            wait_left_ns = spent < wait_left_ns ? wait_left_ns - spent : 0;
        }
        why = oneshot > 0 ? "one-shot, shader" : wait_ms ? "full-screen" : NULL;
        if (pending && oneshot < 0 && !vk_exec_targets_recent(ex, skip >= 1)) {
            perf_count(PERF_CNT_ONESHOT_WAIT);
            why = "one-shot";
            if (!w0) w0 = kern_now_ns();
            uint64_t o0 = kern_now_ns();
            p = vk_pipeline_graphics_async(&s, &info, &pending,
                                           oneshot_budget ? (uint32_t)(wait_left_ns / 1000000) : ONESHOT_WAIT_MS);
            if (oneshot_budget) {
                uint64_t spent = kern_now_ns() - o0;
                wait_left_ns = spent < wait_left_ns ? wait_left_ns - spent : 0;
            }
        }
        PM.valid = !pending && p && info;
        if (PM.valid) PM.s = s, PM.info = info;
    }
    uint64_t waited = w0 ? kern_now_ns() - w0 : 0;
    static int wait_logs;
    if (why && waited >= 2000000 && wait_logs < 200) {
        char tg[512];
        wait_logs++;
        vk_exec_targets_describe(ex, tg, sizeof tg);
        rt_log("vk_exec: frame %llu: a draw waited %.1f ms for its pipeline (%s%s): %u vertices, pass %ux%u, %s",
               (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1, waited / 1e6, why,
               pending ? ", skipped after all" : "", count, ex->pass_w, ex->pass_h, tg);
    }
    if (pending) SKIP(10);   // a background worker is building it: the draw is skipped until it is ready
    if (!p || !info) { warn_once(W_PIPELINE, "a graphics pipeline could not be created: draws with it are skipped"); SKIP(11); }
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    if (!B.st_valid || memcmp(&B.st, &s, sizeof s)) {
        vk_pipeline_set_dynamic(ex->cb, &s);
        ex->recorded++;
        B.st = s, B.st_valid = 1;
    }
    if (ex->pass_w == 2560 || (kar_wide_on() && ex->pass_w == 3440)) {   // KAR diagnostic: the final composite's fixed state
        static int told;
        if (told++ < (kar_wide_on() ? 60 : 3))
            rt_log("kar: 2560 draw: vp %d [%g %g %g %g %g %g] scissor %d [%d %d %d %d] chmask %#x blend0 en %#x src %d dst %d "
                   "count %d prim %d cull %d front %d mode %d ms en %d samples %d a2c %d", ex->has_vp, ex->vp[0][0], ex->vp[0][1], ex->vp[0][2], ex->vp[0][3], ex->vp[0][4],
                   ex->vp[0][5], ex->has_scissor, ex->scissor.x, ex->scissor.y, ex->scissor.w, ex->scissor.h,
                   s.chmask.mask, s.color.blend_en, (int)s.blend[0].src_rgb, (int)s.blend[0].dst_rgb, count, prim,
                   (int)s.poly.cull, (int)s.poly.front, (int)s.poly.mode, s.ms.enable, s.ms.samples, s.ms.a2c);
    }
    {   // KAR_PASS_STATS=<frame> (diagnostic, ultrawide HUD): each draw of that frame by target size, viewport and blending
        static long at = -2;
        if (at == -2) { const char *e = getenv("KAR_PASS_STATS"); at = e ? atol(e) : -1; }
        if (at >= 0 && (long)gfx_counters.frames == at) {
            static int lines;
            if (lines++ < 5000)
                rt_log("pass stats: target serial %llu %ux%u vp %g,%g %gx%g blend %d count %d", ex->rt.n > 0 && ex->rt.color[0] ? (unsigned long long)ex->rt.color[0]->serial : 0ull, ex->pass_w, ex->pass_h,
                       ex->has_vp ? ex->vp[0][0] : 0, ex->has_vp ? ex->vp[0][1] : 0, ex->has_vp ? ex->vp[0][2] : 0,
                       ex->has_vp ? ex->vp[0][3] : 0, s.color.blend_en != 0, count);
        }
    }
    record_fixed(ex);
    repeat_stats(ex, info);
    bind_resources(ex, info);
    if (fbl & ex->fb_dirty) feedback_barrier(ex);
    feedback_written(ex, &s);
    bind_vertex_buffers(ex);
    diag_ubo(ex, &s);
    diag_ssbo(ex, &s);
    diag_kirby_vs(ex, &s);
    if (vk_dump_armed) vk_dump_draw(ex, &s, p, info);
    vk_exec_oq_draw(ex);
    return 1;
}

// TOTK_TRACE=perf: the draw or dispatch just recorded, with the NVN 2 names of its stages, for the slow-part line
// (vk_sync.c vk_sync_note). kind: 0 draw, 1 indirect draw, 2 dispatch, 3 indirect dispatch.
static void note_work(const VkExec *ex, int kind, uint64_t work) {
    if (kind < 2 && vk_dump_armed && vk_dump_split_draw(ex)) vk_exec_pass_end((VkExec *)ex);   // TOTK_DUMP_DRAWS
    if (!vk_sync_noting() || !ex->prog) return;
    const uint8_t *ctl, *code;
    uint32_t len;
    int cs = kind >= 2;
    uint64_t a = find_stage(ex->prog, cs ? NVN_STAGE_CS : NVN_STAGE_VS, &ctl, &code, &len) ? nvn2_origin(code) : 0;
    uint64_t b = !cs && find_stage(ex->prog, NVN_STAGE_FS, &ctl, &code, &len) ? nvn2_origin(code) : 0;
    vk_sync_note(kind, a, b, work);
}

static void bind_index(VkExec *ex, const void *data, uint64_t bytes, VkIndexType type) {
    VkBuffer b;
    VkDeviceSize o;
    memcpy(scratch(ex, bytes, &b, &o), data, bytes);
    vkCmdBindIndexBuffer(ex->cb, b, o, type);
    ex->recorded++;
}

// The Bananza reset vertex program loses stores through pointers loaded from a list. Match both locally confirmed
// stages and the observed one-instance point-draw form. Other programs and parameter forms keep the shader path.
static int indirect_reset_draw(VkExec *ex, const p_draw *d) {
    // Both settings are fixed for the run (TOTK_FIX_INDIRECT_RESET needs a restart), so read them once: this
    // runs ahead of every draw.
    static int on = -1;
    if (on < 0) on = !rt_game_version && cfg_bool("TOTK_FIX_INDIRECT_RESET") && !getenv("DKB_SKIP_DRAWS");
    if (!on || !c0_on() || ex->render_disabled ||
        !ex->prog || ex->prog->nstage != 2 || d->prim != NVN_DRAW_PRIMITIVE_POINTS ||
        d->count <= 0 || d->first < 0 || d->base_instance || d->instances != 1) return 0;
    const uint8_t *ctl, *code;
    uint32_t length;
    if (!find_stage(ex->prog, NVN_STAGE_VS, &ctl, &code, &length) || nvn2_origin(code) != 0xe78abde51f2d6bfaull ||
        !find_stage(ex->prog, NVN_STAGE_FS, &ctl, &code, &length) || nvn2_origin(code) != 0x57b0d7b4959f03c0ull) return 0;
    const p_bind_buf *list = &ex->ssbo[NVN_STAGE_VS][0];
    // c0_table encodes the binding's size as u32 at +0x118; use the same extent, including its zero rejection.
    if (!vk_exec_reset_indirect(ex, list->addr, (uint32_t)list->size, d->first, d->count)) {
        warn_once(W_RESET, "the matched indirect reset has an unsupported list or record: using its shader");
        return 0;
    }
    return 1;
}

static void op_draw(VkExec *ex, const p_draw *d) {
    c0_binst = (uint32_t)d->base_instance;
    if (indirect_reset_draw(ex, d)) return;
    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, 0)) return;
    vk_exec_frame.draws++;
    uint32_t inst = d->instances > 0 ? (uint32_t)d->instances : 1, first = (uint32_t)d->first, n = (uint32_t)d->count;
    if (d->prim == NVN_DRAW_PRIMITIVE_QUADS && n >= 4) {   // quads become two triangles each
        uint32_t q = n / 4;
        uint32_t *ix = malloc(sizeof *ix * 6 * q);
        if (!ix) rt_fatal("vk: out of memory");
        for (uint32_t i = 0; i < q; i++) {
            uint32_t v = first + 4 * i, k[6] = {0, 1, 2, 0, 2, 3};
            for (int j = 0; j < 6; j++) ix[6 * i + j] = v + k[j];
        }
        bind_index(ex, ix, sizeof *ix * 6 * q, VK_INDEX_TYPE_UINT32);
        free(ix);
        vkCmdDrawIndexed(ex->cb, 6 * q, inst, 0, 0, (uint32_t)d->base_instance);
    } else if (d->prim == NVN_DRAW_PRIMITIVE_LINE_LOOP && n >= 2) {   // a strip that returns to its first vertex
        uint32_t *ix = malloc(sizeof *ix * (n + 1));
        if (!ix) rt_fatal("vk: out of memory");
        for (uint32_t i = 0; i < n; i++) ix[i] = first + i;
        ix[n] = first;
        bind_index(ex, ix, sizeof *ix * (n + 1), VK_INDEX_TYPE_UINT32);
        free(ix);
        vkCmdDrawIndexed(ex->cb, n + 1, inst, 0, 0, (uint32_t)d->base_instance);
    } else {
        vkCmdDraw(ex->cb, n, inst, first, (uint32_t)d->base_instance);
    }
    ex->recorded++;
    note_work(ex, 0, (uint64_t)n * inst);
}

// Primitive restart of an indexed draw. NVN restarts strips wherever an index equals the SetPrimitiveRestart value;
// Vulkan restarts only at the all-ones value of the bound index type, and only strips and fans without
// primitiveTopologyListRestart. Byte indices are widened to 16 bits, so their restart value becomes 0xffff
// (op_draw_idx). Without restart the 0xffff entries of TOTK's terrain and cliff strips fetch vertex 65535 + base and
// the triangles around them drop out.
static int restart_for(const VkExec *ex, int prim, int itype) {
    if (!ex->prim_restart.enable) return 0;
    switch (vk_pipeline_topology(prim)) {
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: break;
    default: return 0;   // lists: a restart index only drops the unfinished primitive
    }
    static const uint32_t all_ones[3] = {0xffu, 0xffffu, 0xffffffffu};
    if (ex->prim_restart.index == all_ones[itype]) return 1;
    warn_once(W_RESTART, "primitive restart index %#x with %d-byte indices is not replayed: strips drawn without restart",
              ex->prim_restart.index, 1 << itype);
    return 0;
}

static void op_draw_idx(VkExec *ex, const p_draw_idx *d) {
    c0_binst = (uint32_t)d->base_instance;
    static const uint32_t isize[3] = {1, 2, 4};
    if (d->itype < 0 || d->itype > 2) return warn_once(W_INDEX, "index type %d is unknown: draw skipped", d->itype);
    int restart = restart_for(ex, d->prim, d->itype);
    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, restart)) return;
    vk_exec_frame.draws++;
    uint64_t bytes = (uint64_t)d->count * isize[d->itype];
    VkMemSpan sp;
    if (!vk_mem_resolve(d->ibuf, bytes, &sp)) return warn_once(W_INDEX, "index buffer %#llx is not backed: draws with it are skipped", (unsigned long long)d->ibuf);
    if (d->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE) {   // no uint8 index type here: widen (host-readable backing only)
        if (sp.alloc) return warn_once(W_INDEX, "byte indices in device-local memory: draw skipped");
        vk_mem_count(sp.cls, VK_MUSE_CPU_READ, bytes);
        vk_mem_count(VK_MCLS_SCRATCH, VK_MUSE_IB, 2 * bytes);
        uint16_t *ix = malloc(sizeof *ix * (size_t)d->count);
        if (!ix) rt_fatal("vk: out of memory");
        for (int i = 0; i < d->count; i++) {
            uint8_t v = ((const uint8_t *)(uintptr_t)d->ibuf)[i];
            ix[i] = restart && v == 0xff ? 0xffff : v;
        }
        bind_index(ex, ix, sizeof *ix * (size_t)d->count, VK_INDEX_TYPE_UINT16);
        free(ix);
    } else if (sp.offset % isize[d->itype]) {
        return warn_once(W_INDEX, "index buffer %#llx is not aligned to its index size: draw skipped", (unsigned long long)d->ibuf);
    } else {
        VK_MEM_USE(sp);
        vkCmdBindIndexBuffer(ex->cb, sp.buffer, sp.offset, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        ex->recorded++;
        vk_mem_count(sp.cls, VK_MUSE_IB, bytes);
        VK_MEM_BOUND(sp);
    }
    vkCmdDrawIndexed(ex->cb, (uint32_t)d->count, d->instances > 0 ? (uint32_t)d->instances : 1, 0, d->base_vertex,
                     (uint32_t)d->base_instance);
    ex->recorded++;
    note_work(ex, 0, (uint64_t)d->count * (uint64_t)(d->instances > 0 ? d->instances : 1));
}

// DrawElementsIndirect: the arguments are read by the GPU from the pool buffer (they are often written by an earlier
// dispatch), as nxnative nvn_mtl.m does. The base instance stays out of c0 (0x644): c0 cannot be patched by a transfer
// inside a pass; gl_BaseInstance of the Vulkan draw is the argument's. Byte indices need the count to be widened: skipped.
static void op_draw_idx_ind(VkExec *ex, const p_draw_idx_ind *d) {
    c0_binst = 0;
    if (d->itype != NVN_INDEX_TYPE_UNSIGNED_SHORT && d->itype != NVN_INDEX_TYPE_UNSIGNED_INT)
        return warn_once(W_INDEX, "indirect draw with index type %d: skipped", d->itype);
    VkMemSpan ib, ip;
    if (!vk_mem_resolve(d->ind, sizeof(VkDrawIndexedIndirectCommand), &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect draw arguments at %#llx are not backed or not aligned: draws skipped", (unsigned long long)d->ind);
    if (!vk_mem_resolve(d->ibuf, 1, &ib) || ib.offset % (d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 4))
        return warn_once(W_INDEX, "index buffer %#llx is not backed or not aligned: indirect draws skipped", (unsigned long long)d->ibuf);
    if (!prepare_draw(ex, d->prim, 0x7fffffff, restart_for(ex, d->prim, d->itype))) return;
    vk_exec_frame.draws++;
    VK_MEM_USE(ip);
    VK_MEM_USE(ib);
    vkCmdBindIndexBuffer(ex->cb, ib.buffer, ib.offset, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexedIndirect(ex->cb, ip.buffer, ip.offset, 1, sizeof(VkDrawIndexedIndirectCommand));
    ex->recorded += 2;
    note_work(ex, 1, 0);
    // An imported pool's GPU address is its host address (bind_range reads it the same way); device memory is not.
    if (!ip.alloc && vk_sync_noting()) vk_sync_note_args(d->ind);
    vk_mem_count(ip.cls, VK_MUSE_IND, sizeof(VkDrawIndexedIndirectCommand));
    VK_MEM_BOUND(ip);
    VK_MEM_BOUND(ib);
}

// DrawArraysIndirect: op_draw_idx_ind without the index buffer; the GPU reads {count, instances, first, base instance}
// (VkDrawIndirectCommand) from the pool buffer.
static void op_draw_ind(VkExec *ex, const p_draw_ind *d) {
    c0_binst = 0;
    VkMemSpan ip;
    if (!vk_mem_resolve(d->ind, sizeof(VkDrawIndirectCommand), &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect draw arguments at %#llx are not backed or not aligned: draws skipped", (unsigned long long)d->ind);
    if (!prepare_draw(ex, d->prim, 0x7fffffff, 0)) return;
    vk_exec_frame.draws++;
    VK_MEM_USE(ip);
    vkCmdDrawIndirect(ex->cb, ip.buffer, ip.offset, 1, sizeof(VkDrawIndirectCommand));
    ex->recorded += 1;
    note_work(ex, 1, 0);
    if (!ip.alloc && vk_sync_noting()) vk_sync_note_args(d->ind);
    vk_mem_count(ip.cls, VK_MUSE_IND, sizeof(VkDrawIndirectCommand));
    VK_MEM_BOUND(ip);
}

// ---- dispatch and uniform buffer updates

// WORKAROUND, MoltenVK only (TOTK_MVK_UPSCALE_BLIT / DKB_MVK_UPSCALE_BLIT, on by default there, 0 turns it off):
// Bananza's own compute upscaler (the dispatch that scales the graded frame to the output size) gives wrong results on
// Metal. Its input is right; its output keeps a dim red channel, little green and blue, and blocks, so every scene is
// dark red and blue. The cause is not found: fast math off (MVK_CONFIG_FAST_MATH_ENABLED=0), a write-only binding of the
// R11G11B10F output and the shared memory size (12 KiB) are ruled out; the shader also uses subgroup shuffles and votes
// and texel fetches. Until it is found the dispatch is replaced by a linear blit of its input (texture slot 0) to its
// output (image slot 1): the picture is right, without the game's own reconstruction and sharpening. The shaders are
// matched by the hash of their SPIR-V (the name TOTK_DUMP prints), so a translator change that alters them turns this
// off by itself. Other drivers never take this path.
// Whole images, mip 0, layer 0: the game makes each input texture at the size it renders (1920x1080 and 1280x720 for
// the intro, the current dynamic resolution such as 2095x1179 in play, a new texture when it changes), so the full
// extent is the rendered area; a run in the mining tunnel showed no stretch or border.
// The upscaler's variants: fixed resolution (the intro: 1920x1080 or 1280x720 in) and dynamic resolution (gameplay
// after the intro, e.g. 2095x1179 in).
static int mvk_upscale_cs(uint64_t h) { return h == 0x3f3390cfdfa1f9e8ull || h == 0x4f9090b273fa7e4full; }

// Whether the device can blit from format f with linear filtering (src) or into it (dst), optimal tiling.
static int blit_ok(VkFormat f, int dst) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, f, &fp);
    VkFormatFeatureFlags want = dst ? VK_FORMAT_FEATURE_BLIT_DST_BIT
                                    : VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (fp.optimalTilingFeatures & want) == want;
}

// Records the blit in place of the dispatch; 0 when this is not the upscaler or the blit cannot be done (the shader
// then runs). The caller has already put a full barrier before it (vk_exec_barrier in op_dispatch) and puts one after.
static int mvk_upscale_blit(VkExec *ex, const GfxShader *cs) {
    static int on = -1;
    if (on < 0) on = vk_caps.moltenvk && cfg_bool("TOTK_MVK_UPSCALE_BLIT");
    // The hash is kept with the shader (gfx_shader_spirv_fnv): a cache of the last shader alone missed on nearly every
    // dispatch, as dispatches alternate between compute shaders, and hashing the whole SPIR-V each time took about 12%
    // of the replaying thread on an M4.
    if (!on || !mvk_upscale_cs(gfx_shader_spirv_fnv(cs))) return 0;
    uint64_t hs = ex->tex[NVN_STAGE_CS][0], hd = ex->img[NVN_STAGE_CS][1];
    view_t vs, vd;
    int hvs = 0, hvd = 0;
    const VkTexImage *s = hs ? vk_texture_by_id(&ex->tex_pool, NVN_HANDLE_TEX(hs), &vs, &hvs) : NULL;
    const VkTexImage *t = hd ? vk_texture_by_id(&ex->tex_pool, NVN_HANDLE_TEX(hd), &vd, &hvd) : NULL;
    const char *why = !s || !t                                                     ? "its images do not resolve"
                    : s->samples > 1 || t->samples > 1                             ? "an image is multisampled"
                    : s->type != VK_IMAGE_TYPE_2D || t->type != VK_IMAGE_TYPE_2D ? "an image is not 2D"
                    : !(s->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !(t->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                        ? "an image lacks transfer usage"
                    : !blit_ok(s->format, 0) || !blit_ok(t->format, 1) ? "the formats cannot be blitted linearly"
                                                                       : NULL;
    if (why) {
        static int warned;
        if (!warned++) rt_log("vk_exec: MoltenVK workaround: the upscaler runs as a shader: %s", why);
        return 0;
    }
    static int logged;
    if (!logged++)
        rt_log("vk_exec: MoltenVK workaround: the game's upscaler dispatches are linear blits (first %ux%u -> %ux%u, "
               "TOTK_MVK_UPSCALE_BLIT=0 runs the shaders)", s->width, s->height, t->width, t->height);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}};
    bl.srcOffsets[1] = (VkOffset3D){(int32_t)s->width, (int32_t)s->height, 1};
    bl.dstOffsets[1] = (VkOffset3D){(int32_t)t->width, (int32_t)t->height, 1};
    vkCmdBlitImage(ex->cb, s->image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    return 1;
}

// The pipeline of a dispatch. Built by the background workers (vk_pipeline_compute_async), but the dispatch waits for
// it: a skipped dispatch changes results (indirect arguments, index lists, the colour LUT), and skipping them while the
// boot prebuild was off lost the GPU device at boot. KAR_CS_SKIP_PENDING=1 restores skipping (*pending = 1 then).
uint64_t vk_rp_sub[4];   // KAR_REPLAY_PROF: dispatch time in shader lookup, compute pipeline, resource binding, the rest
static VkPipeline compute_pipe(const GfxShader *cs, const VkPipeInfo **info, int *pending) {
    static int skip = -1;
    if (skip < 0) skip = getenv("KAR_CS_SKIP_PENDING") && *getenv("KAR_CS_SKIP_PENDING") == '1';
    VkPipeline p = vk_pipeline_compute_async(cs, info, pending);
    if (!*pending || skip) return p;
    uint64_t t0 = plat_now_ns();
    while (*pending) {
        plat_sleep_ns(500000);
        p = vk_pipeline_compute_async(cs, info, pending);
    }
    uint64_t ms = (plat_now_ns() - t0) / 1000000;
    static int told;
    if (ms >= 100 && told++ < 8) rt_log("vk_exec: a dispatch waited %llu ms for its compute pipeline", (unsigned long long)ms);
    return p;
}

// A dispatch whose program has no usable compute stage (an NVN 2 program that did not lower) is skipped. The first
// skip of each program is logged with its caller, so a run lists every compute pass that is missing.
static int cs_skip_first(uint64_t serial) {
    static uint64_t seen[256];
    static int n;
    static PlatMutex mu;   // zero-initialised storage is a valid unlocked lock (platform.h)
    int first = 1;
    plat_mutex_lock(&mu);
    for (int i = 0; i < n; i++)
        if (seen[i] == serial) first = 0;
    if (first && n < 256) seen[n++] = serial;
    plat_mutex_unlock(&mu);
    return first;
}

static void op_dispatch(VkExec *ex, const p_dispatch *d) {
    if (d->x <= 0 || d->y <= 0 || d->z <= 0) return;
    static int skip = -1;   // DKB_SKIP_COMPUTE=1: drop every direct dispatch (bisecting GPU faults)
    if (skip < 0) skip = getenv("DKB_SKIP_COMPUTE") != NULL;
    if (skip) return;
    if (!ex->prog) return warn_once(W_PROGRAM, "a dispatch without a bound program is skipped");
    cb_res(ex);
    dummies_init();
    uint64_t q0 = plat_now_ns();
    int present;
    const GfxShader *cs = shader_for(ex, NVN_STAGE_CS, &present, NULL, 0);   // synchronous: a skipped dispatch changes results
    if (!cs) {   // no compute stage (an NVN 2 program that did not lower is dropped): said, not silent
        if (cs_skip_first(ex->prog->serial))
            rt_log("vk_exec: dispatch %ux%ux%u skipped: program %llu has no usable compute stage (caller %#llx), frame %llu",
                   d->x, d->y, d->z, (unsigned long long)ex->prog->serial, (unsigned long long)d->caller,
                   (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
        return;
    }
    uint64_t q1 = plat_now_ns();
    vk_rp_sub[0] += q1 - q0;
    const VkPipeInfo *info = NULL;
    int cs_pending;
    VkPipeline p = compute_pipe(cs, &info, &cs_pending);
    uint64_t q2 = plat_now_ns();
    vk_rp_sub[1] += q2 - q1;
    if (cs_pending) return;   // KAR_CS_SKIP_PENDING=1 only
    if (!p || !info) return warn_once(W_PIPELINE, "a compute pipeline could not be created: dispatches with it are skipped");
    vk_exec_barrier(ex);   // ends the pass; earlier writes are visible to the shader
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    c0_dim[0] = (uint32_t)d->x, c0_dim[1] = (uint32_t)d->y, c0_dim[2] = (uint32_t)d->z;
    c0_lay_set(ex, cs);
    uint64_t q3 = plat_now_ns();
    twin_dispatch = 1;
    bind_resources(ex, info);
    twin_dispatch = 0;
    uint64_t q4 = plat_now_ns();
    vk_rp_sub[2] += q4 - q3, vk_rp_sub[3] += q3 - q2;
    int blit = mvk_upscale_blit(ex, cs);
    if (!blit) vkCmdDispatch(ex->cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    ex->recorded++;
    // The blit is a transfer write, where what follows expects a compute-shader write: a full barrier makes it visible
    // to every later access (the one before it, vk_exec_barrier above, already covers the transfer reads).
    if (blit) vk_exec_barrier(ex);
    note_work(ex, 2, (uint64_t)d->x * (uint64_t)d->y * (uint64_t)d->z);
    if (vk_dump_armed) vk_dump_dispatch(ex, cs, p, info, d);
}

// DispatchComputeIndirect: vkCmdDispatchIndirect from the pool buffer. NVN 1 shaders read the group counts at c0+0x200,
// so they are copied there on the GPU before dispatch. That NVN 1 offset can hold declared roots or resource tables
// in NVN 2; leave its packed bank unchanged rather than overwrite it with the legacy copy.
static void op_dispatch_ind(VkExec *ex, const p_dispatch_ind *d) {
    if (!ex->prog) return warn_once(W_PROGRAM, "a dispatch without a bound program is skipped");
    VkMemSpan ip;
    if (!vk_mem_resolve(d->ind, sizeof(VkDispatchIndirectCommand), &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect dispatch arguments at %#llx are not backed or not aligned: skipped", (unsigned long long)d->ind);
    cb_res(ex);
    dummies_init();
    uint64_t q0 = plat_now_ns();
    int present;
    const GfxShader *cs = shader_for(ex, NVN_STAGE_CS, &present, NULL, 0);
    if (!cs) {   // as op_dispatch: said, not silent
        if (cs_skip_first(ex->prog->serial))
            rt_log("vk_exec: indirect dispatch skipped: program %llu has no usable compute stage, frame %llu",
                   (unsigned long long)ex->prog->serial,
                   (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
        return;
    }
    uint64_t q1 = plat_now_ns();
    vk_rp_sub[0] += q1 - q0;
    const VkPipeInfo *info = NULL;
    int cs_pending;
    VkPipeline p = compute_pipe(cs, &info, &cs_pending);
    uint64_t q2 = plat_now_ns();
    vk_rp_sub[1] += q2 - q1;
    if (cs_pending) return;   // KAR_CS_SKIP_PENDING=1 only
    if (!p || !info) return warn_once(W_PIPELINE, "a compute pipeline could not be created: dispatches with it are skipped");
    vk_exec_barrier(ex);
    VK_MEM_USE(ip);
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    c0_dim[0] = c0_dim[1] = c0_dim[2] = 0;
    c0_lay_set(ex, cs);
    B.c0_valid[2] = 0;   // a fresh c0 range: the copy below must not change one an earlier dispatch reads
    uint64_t q3 = plat_now_ns();
    twin_dispatch = 1;
    bind_resources(ex, info);
    twin_dispatch = 0;
    uint64_t q4 = plat_now_ns();
    vk_rp_sub[2] += q4 - q3, vk_rp_sub[3] += q3 - q2;
    if (!c0_lay && B.c0_valid[2]) {   // the NVN 1 shader binds c0
        VkBufferCopy cp = {ip.offset, B.c0_off[2] + 0x200, sizeof(VkDispatchIndirectCommand)};
        vkCmdCopyBuffer(ex->cb, ip.buffer, B.c0_buf[2], 1, &cp);
        ex->recorded++;
        vk_exec_barrier(ex);
        B.c0_valid[2] = 0;   // the range no longer holds B.c0[2]
    }
    vkCmdDispatchIndirect(ex->cb, ip.buffer, ip.offset);
    ex->recorded++;
    note_work(ex, 3, 0);
    vk_mem_count(ip.cls, VK_MUSE_IND, sizeof(VkDispatchIndirectCommand));
    VK_MEM_BOUND(ip);
    if (vk_dump_armed) vk_dump_dispatch(ex, cs, p, info, &(p_dispatch){0, 0, 0, d->caller});
}

static void op_update_ub(VkExec *ex, const p_update_ub *u) {
    if (!u->size) return;
    VkMemSpan sp;
    if (!vk_mem_resolve(u->dst, u->size, &sp)) return warn_once(W_UB, "UpdateUniformBuffer to %#llx is not backed: dropped", (unsigned long long)u->dst);
    cb_res(ex);
    vk_exec_barrier(ex);   // earlier draws finished with the old contents
    VkBuffer b;
    VkDeviceSize o;
    memcpy(scratch(ex, u->size, &b, &o), u->data, u->size);
    VK_MEM_PICK(sp);
    VkBufferCopy cp = {o, sp.offset, u->size};
    vkCmdCopyBuffer(ex->cb, b, sp.buffer, 1, &cp);
    vk_mem_gpu_wrote(&sp, u->size);
    vk_usnap_drop(u->dst, u->size);
    ex->recorded++;
    vk_mem_count(sp.cls, VK_MUSE_GPU_WRITE, u->size);
    note_written(ex, u->dst, u->size);
    upd_note(ex, u);
}

// ---- bind ops

static void geometry_context_buffer_log(unsigned record, const char *name, const p_bind_buf *binding) {
    VkMemInspect view = {0};
    int backed = binding->addr && binding->size && vk_mem_inspect(binding->addr, binding->size, &view);
    rt_log("geometry_buffer_context: record=%u range=%s snapshot=passive requested=%llu full_range_backed_now=%d "
           "pool_kind=%u pool_flags=%#x class_before_mirror_pick=%d offset=%llu available=%llu "
           "mirror_state=%d mirror_present=%d shadow_storage=%d buffer_present=%d contents=unread descriptor=unobserved",
           record, name, (unsigned long long)binding->size, backed, view.pool_kind, view.pool_flags, view.cls,
           (unsigned long long)view.offset, (unsigned long long)view.avail,
           view.mirror_state, view.mirror_present, view.shadow_storage, view.buffer_present);
}

// Opt-in draw-ingress metadata only. Replay is serialized; the cap is process-wide and later draws are unobserved.
static void geometry_context_log(const VkExec *ex, uint32_t op, const void *payload) {
    if (op != OP_DRAW && op != OP_DRAW_IDX && op != OP_DRAW_IDX_IND) return;
    static int on = -1, filtered;
    static uint64_t filter;
    static unsigned records;
    if (on < 0) {
        const char *e = getenv("DKB_GEOMETRY_CONTEXT"), *f = getenv("DKB_GEOMETRY_CONTEXT_ORIGIN");
        on = e && !strcmp(e, "1");
        if (on && f) {
            filtered = 1;
            if (f[0] == '0' && (f[1] == 'x' || f[1] == 'X')) f += 2;
            unsigned digits = 0;
            for (; *f; f++) {
                unsigned d = *f >= '0' && *f <= '9' ? (unsigned)(*f - '0') :
                             *f >= 'a' && *f <= 'f' ? (unsigned)(*f - 'a' + 10) :
                             *f >= 'A' && *f <= 'F' ? (unsigned)(*f - 'A' + 10) : 16;
                if (d == 16 || digits == 16) { on = 0; break; }
                filter = (filter << 4) | d;
                digits++;
            }
            if (!digits) on = 0;
            if (!on) rt_log("geometry_context: invalid origin filter; metadata logging disabled");
        }
    }
    if (!on || records == 12 || !ex || !payload) return;

    uint64_t origin[3] = {0};
    unsigned present = 0;
    int metadata = ex->prog && ex->prog->nstage <= 6;
    // Failed native lowering can leave a compact control: do not send it to the legacy-shaped stage getter.
    if (metadata)
        for (uint32_t i = 0; i < ex->prog->nstage; i++) {
            const uint8_t *ctl = ex->prog->sd[i].control;
            if (ctl && gfx_shader_is_nvn2(ctl)) { metadata = 0; break; }
        }
    if (metadata)
        for (int stage = 0; stage < 3; stage++) {
            const uint8_t *ctl, *code;
            uint32_t len;
            if (find_stage(ex->prog, stage, &ctl, &code, &len)) {
                present |= 1u << stage;
                origin[stage] = nvn2_origin(code);   // pointer-to-origin lookup, no shader bytes read
            }
        }
    if (filtered && !((present & 1) && origin[0] == filter) &&
        !((present & 2) && origin[1] == filter) && !((present & 4) && origin[2] == filter)) return;

    const char *kind;
    int prim;
    char draw[256], viewport[192], attachments[512], bindings[384];
    if (op == OP_DRAW) {
        const p_draw *d = payload;
        kind = "direct", prim = d->prim;
        snprintf(draw, sizeof draw, "first=%d count=%d instances_raw=%d base_instance=%d",
                 d->first, d->count, d->instances, d->base_instance);
    } else if (op == OP_DRAW_IDX) {
        const p_draw_idx *d = payload;
        kind = "indexed", prim = d->prim;
        snprintf(draw, sizeof draw, "count=%d instances_raw=%d base_vertex=%d base_instance=%d index_type=%d index_addr=%#llx",
                 d->count, d->instances, d->base_vertex, d->base_instance, d->itype, (unsigned long long)d->ibuf);
    } else {
        const p_draw_idx_ind *d = payload;
        kind = "indexed_indirect", prim = d->prim;
        snprintf(draw, sizeof draw, "index_type=%d index_addr=%#llx indirect_addr=%#llx count=unknown instances=unknown",
                 d->itype, (unsigned long long)d->ibuf, (unsigned long long)d->ind);
    }
    if (ex->has_vp)
        snprintf(viewport, sizeof viewport, "has_vp=1 viewport0_stored=[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g] depth_range=[%.9g,%.9g]",
                 ex->vp[0][0], ex->vp[0][1], ex->vp[0][2], ex->vp[0][3], ex->vp[0][4], ex->vp[0][5],
                 ex->depth_range[0], ex->depth_range[1]);
    else
        snprintf(viewport, sizeof viewport, "has_vp=0 viewport0_stored=unreported depth_range=[%.9g,%.9g]", ex->depth_range[0], ex->depth_range[1]);
    if (ex->rt_resolved) {
        size_t n = (size_t)snprintf(attachments, sizeof attachments, "attachments=resolved pass_noatt=%d pass_layers=%u layer_views=mip:base:count",
                                  ex->pass_noatt, ex->pass_layers);
        for (int i = 0; i < 8; i++)
            if (ex->color[i].image && n < sizeof attachments) {
                const VkExecTarget *t = &ex->color[i];
                n += (size_t)snprintf(attachments + n, sizeof attachments - n, " color%d=%u:%u:%u", i, t->level, t->base_layer, t->layers);
            }
        if (ex->depth.image && n < sizeof attachments)
            snprintf(attachments + n, sizeof attachments - n, " depth=%u:%u:%u", ex->depth.level, ex->depth.base_layer, ex->depth.layers);
    } else {
        snprintf(attachments, sizeof attachments, "attachments=unresolved pass_layers=unknown");
    }
    const p_bind_buf *s0 = &ex->ssbo[NVN_STAGE_GS][0], *s1 = &ex->ssbo[NVN_STAGE_GS][1], *u0 = &ex->ubo[NVN_STAGE_GS][0];
    snprintf(bindings, sizeof bindings, "gs_ranges=recorded_pod_unresolved gs_ssbo0=%#llx:%llu:%s gs_ssbo1=%#llx:%llu:%s gs_ubo0=%#llx:%llu:%s",
             (unsigned long long)s0->addr, (unsigned long long)s0->size, s0->addr && s0->size ? "range" : "empty_or_unset",
             (unsigned long long)s1->addr, (unsigned long long)s1->size, s1->addr && s1->size ? "range" : "empty_or_unset",
             (unsigned long long)u0->addr, (unsigned long long)u0->size, u0->addr && u0->size ? "range" : "empty_or_unset");
    rt_log("geometry_context: record=%u limit=12 kind=%s op=%u prim=%d %s fence=%llu program=%llu "
           "bound_stage_mask=%#x stage_metadata=%s present_stage_mask=%#x vs=%016llx gs=%016llx fs=%016llx "
           "render_disabled=%d render_cond_present=%d raster_discard=%u pass_active=%d %s %s %s "
           "clip_cull=untracked xfb=untracked guest_viewport_count=unknown guest_viewport_mask=unknown viewport0_update_provenance=untracked layer_routing=unknown",
           ++records, kind, op, prim, draw, (unsigned long long)ex->fence,
           ex->prog ? (unsigned long long)ex->prog->serial : 0ull, (unsigned)ex->prog_stages,
           metadata ? "available" : "unavailable", present,
           (unsigned long long)origin[0], (unsigned long long)origin[2], (unsigned long long)origin[1],
           ex->render_disabled, ex->has_render_cond, (unsigned)ex->pipe.raster_discard, ex->in_pass, viewport, attachments, bindings);
    geometry_context_buffer_log(records, "gs_ssbo0", s0);
    geometry_context_buffer_log(records, "gs_ssbo1", s1);
    geometry_context_buffer_log(records, "gs_ubo0", u0);
}

static void bind_handles(uint64_t *dst, int cap, const p_bind_handles *b) {
    if (b->stage < 0 || b->stage >= 6 || b->first < 0 || b->n < 0 || b->n > 32 || b->first + b->n > cap) {
        warn_once(W_BIND, "a bind of %d handles at %d of stage %d is out of range: ignored", b->n, b->first, b->stage);
        return;
    }
    memcpy(&dst[b->first], b->h, sizeof(uint64_t) * (size_t)b->n);
}

// A program made of the stages several bound programs supply, one per distinct combination of their serials (a
// program's serial changes at every nvnProgramSetShaders, so a combination never names stale code). Each composite
// gets a serial of its own, which keys the shader, stage and pipeline caches like any program's. The composites live
// for the process: games bind a bounded set of combinations.
#define COMPOSITE_N 4096
static struct { uint64_t key[5]; prog_t *p; } composites[COMPOSITE_N];

static const prog_t *composite_prog(const prog_t *const sp[5]) {
    uint64_t key[5], h = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 5; i++) key[i] = sp[i] ? sp[i]->serial : 0, h = (h ^ key[i]) * 0x100000001b3ull;
    for (uint32_t k = 0; k < COMPOSITE_N; k++) {
        typeof(composites[0]) *e = &composites[(h + k) & (COMPOSITE_N - 1)];
        if (e->p && !memcmp(e->key, key, sizeof key)) return e->p;
        if (e->p) continue;
        prog_t *p = calloc(1, sizeof *p);
        if (!p) return NULL;
        for (int st = 0; st < 5; st++) {
            const uint8_t *ctl, *code;
            uint32_t len;
            if (!sp[st] || !find_stage_scan(sp[st], st, &ctl, &code, &len) || p->nstage >= 6) continue;
            p->magic = sp[st]->magic;
            p->sd[p->nstage++] = (NVNshaderData){(NVNbufferAddress)(uintptr_t)code, ctl};
        }
        p->serial = nvn_new_serial();
        memcpy(e->key, key, sizeof key), e->p = p;
        static int told;
        if (told++ < 4) rt_log("vk_exec: composite program %llu from per-stage binds (%d stages)", (unsigned long long)p->serial, (int)p->nstage);
        return p;
    }
    static int full;
    if (!full++) rt_log("vk_exec: composite program table full: further mixed-program draws use the vertex program");
    return sp[0];
}

// The program a draw (graphics) or dispatch replays with: the stage programs bound last, composed when they differ.
static void use_prog(VkExec *ex, int graphics) {
    const prog_t *p = NULL;
    int mask = 0;
    if (!graphics) p = ex->sprog[5], mask = p ? NVN_SHADER_STAGE_COMPUTE_BIT : 0;
    else {
        int mixed = 0;
        for (int i = 0; i < 5; i++) {
            if (!ex->sprog[i]) continue;
            mask |= 1 << i;
            if (!p) p = ex->sprog[i];
            else if (p != ex->sprog[i]) mixed = 1;
        }
        if (mixed) p = composite_prog(ex->sprog);
    }
    if (p != ex->prog) ex->prog = p, bind_gen++;
    ex->prog_stages = mask;
}

void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    geometry_context_log(ex, op, payload);
    switch (op) {
    case OP_BIND_PROGRAM: {
        const p_bind_program *b = payload;
        for (int i = 0; i < 6; i++)
            if (b->stages >> i & 1) ex->sprog[i] = b->prog;
        bind_gen++;
        break;
    }
    case OP_BIND_STATE: {
        const p_bind_state *b = payload;
        blend_st bs;
        switch (b->kind) {
        case 0:
            memcpy(&bs, b->bytes, sizeof bs);
            if (bs.target < 8) ex->pipe.blend[bs.target] = bs;
            break;
        case 1: memcpy(&ex->pipe.color, b->bytes, sizeof ex->pipe.color); break;
        case 2: memcpy(&ex->pipe.chmask, b->bytes, sizeof ex->pipe.chmask); break;
        case 3: memcpy(&ex->pipe.ms, b->bytes, sizeof ex->pipe.ms); break;
        case 4: memcpy(&ex->pipe.poly, b->bytes, sizeof ex->pipe.poly); break;
        case 5: memcpy(&ex->pipe.ds, b->bytes, sizeof ex->pipe.ds); break;
        case 6: memcpy(&ex->point_size, b->bytes, sizeof ex->point_size); break;
        case 7: memcpy(&ex->pipe.alpha_ref, b->bytes, sizeof ex->pipe.alpha_ref); break;
        }
        break;
    }
    case OP_BIND_VATTR: {
        const p_bind_vattr *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(ex->pipe.vattr, 0, sizeof ex->pipe.vattr);   // MNG: the new list replaces the old one
        memcpy(&ex->pipe.vattr[b->first], b->st, sizeof(uint32_t) * (size_t)b->n);
        break;
    }
    case OP_BIND_VSTREAM: {
        const p_bind_vstream *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(ex->pipe.vstream, 0, sizeof ex->pipe.vstream);
        memcpy(&ex->pipe.vstream[b->first], b->st, sizeof(vs_st) * (size_t)b->n);
        break;
    }
    case OP_BIND_VBUF: {
        const p_bind_vbuf *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memcpy(&ex->vbuf[b->first], b->r, sizeof(NVNbufferRange) * (size_t)b->n);
        break;
    }
    case OP_BIND_UBO: {
        const p_bind_buf *b = payload;
        if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 18) ex->ubo[b->stage][b->index] = *b;
        bind_gen++;
        break;
    }
    case OP_PUSH_CONST: {
        const p_push_const *p = payload;
        if (p->table < 0 || p->table >= NVN2_ROOT_TABLES || (!p->compute && p->table != 0)) {
            static int warned;
            if (!warned++) rt_log("vk: push constants into root table %d dropped", p->table);
            break;
        }
        int k = p->compute != 0, w0 = p->offset / 4, n = p->bytes / 4;
        memcpy(&ex->push[k][p->table][w0], p->w, 4 * (size_t)n);
        ex->push_set[k][p->table] |= (n >= 64 ? ~0ull : ((1ull << n) - 1)) << w0;
        bind_gen++;
        break;
    }
    case OP_BIND_SSBO: {
        const p_bind_buf *b = payload;
        if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) ex->ssbo[b->stage][b->index] = *b;
        else {   // past the table: dropped (NVN 2 binds more storage buffers per stage than NVN 1's 16)
            static uint64_t seen[6];
            if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 64 && !(seen[b->stage] >> b->index & 1))
                seen[b->stage] |= 1ull << b->index, rt_log("vk: storage buffer %d of stage %d dropped", b->index, b->stage);
        }
        bind_gen++;
        break;
    }
    case OP_BIND_TEX: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < 6 ? ex->tex[b->stage] : NULL, 128, b);
        ex->tex_serial++;
        bind_gen++;
        break;
    }
    case OP_BIND_SAMP: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < 6 ? ex->samp[b->stage] : NULL, 128, b);
        break;
    }
    case OP_BIND_IMG: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < 6 ? ex->img[b->stage] : NULL, 32, b);
        ex->tex_serial++;   // the shader memo's handle check (feedback_scan reads only textures: a rescan at most)
        bind_gen++;
        break;
    }
    case OP_UPDATE_UB: op_update_ub(ex, payload); break;
    case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: case OP_DRAW_IND: use_prog(ex, 1); break;
    case OP_DISPATCH: case OP_DISPATCH_IND: use_prog(ex, 0); break;
    default: break;
    }
    switch (op) {
    case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
    case OP_BIND_UBO: case OP_PUSH_CONST: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG:
    case OP_UPDATE_UB: break;
    case OP_DRAW: op_draw(ex, payload); break;
    case OP_DRAW_IDX: op_draw_idx(ex, payload); break;
    case OP_DISPATCH: op_dispatch(ex, payload); break;
    case OP_DRAW_IDX_IND: op_draw_idx_ind(ex, payload); break;
    case OP_DRAW_IND: op_draw_ind(ex, payload); break;
    case OP_DISPATCH_IND: op_dispatch_ind(ex, payload); break;
    default: rt_fatal("vk: op %s is not a draw op", nvn_op_name(op));
    }
}
