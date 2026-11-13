// Shader service client: turns an NVN program (control section, code, c1 constants and the
// translator's answers) into SPIR-V plus reflection through `totk_xlate.exe --serve` children, started lazily with
// pipes and kept for the life of the process (a pool: TOTK_XLATE_PROCS at run time, more for the boot prewarm).
// Results are cached in memory and on disk (shader-cache/<key>.spv and <key>.res.json next to totk.exe), so a shader
// is translated once per machine and translator.
//
// Keys. The code hash is sha256 over stage (u32), the 0x880-byte control section, code_size (u64), the code, the c1
// length (u32) and bytes. GS additionally hashes its versioned input-topology suffix. Per code hash and translator
// the cache holds a use mask (<sha256("TKSU", xlate id, code
// hash)>.use): the texture/image slots the translator queried (.res.json tex_queried, img_queried), the vertex
// attributes a VS declares and the colour targets an FS declares (SPIR-V Locations). The input id is sha256 over "TKSI",
// IN_VERSION, the code hash and the answers with everything outside the mask zeroed (fixed little-endian layout), so
// stale bindings in slots the shader never reads do not make new ids; it does not depend on the translator. The cache
// key is sha256 over "TKSK", KEY_VERSION, the translator identity and the input id. A code hash without a known mask is
// translated under a provisional key ("TKSP", full answers); its files are renamed to the cache key and the mask stored.
//
// Translator identity: build/xlate/totk_xlate.id, sha256 hex that tools/build_xlate.py writes over the sources it
// builds from (Program.cs, the csproj, the patches, the vendor tree); without it the sha256 of the exe. TOTK_XLATE_ID
// (64 hex digits) overrides it. A translator change therefore changes every cache key without a hand-made KEY_VERSION
// bump; KEY_VERSION is for changes on this side (key layout, how the output is used).
//
// Pack: tools/pack_shader_cache.py folds the .spv, .res.json, .use and in/*.in files into <cache>/shader-cache.pack
// (one file instead of four per shader: ~117,000 files after an install) and deletes them; the installer runs it after
// preparing the shaders. The pack is read-only here. Every read looks in the pack first and then for the loose file,
// and new translations are written as loose files as before, until the tool runs again (section "cache pack").
//
// Stored inputs: every translation also writes <cache>/in/<input id>.in (magic "TKSI"; stage, the full answers, the
// control section, the code up to its length, c1), so a pipeline key record (vk_pipeline.c), which names its shaders by
// input id, can be translated again under a new translator without guest memory (gfx_shader_load_input,
// gfx_shader_prewarm).
//
// Reflection: every shader that enters the cache has its .res.json parsed into GfxShaderRes arrays and its SPIR-V
// scanned for interface Locations and the workgroup size (gfx.h accessors at the end of the file).
//
// Provenance: the c2 handle ranges of handle_slot are those of MacNativeGames' totk_xlate Service.cs (MIT).
// The code layout the translator sees (graphics: code[0x30..], compute: code[0..]; c1 = code[c1..length]
// with c1 and length at control+0x700) follows tools/shader_census.py, itself MacNativeGames' totk shader tooling (MIT).
#include <ctype.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_geometry_control.h"
#include "platform/platform.h"

#define CONTROL_SIZE 0x880
// NVN 2 compute c0: a 16-byte driver head, eight root tables padded separately to 16 bytes (word counts +0xa9..0xb0),
// uniform/storage tables (+0xb1/+0xb2), then texture/image handles (+0xb3/+0xb6). nvn2_lower.cpp writes DKC1,
// the four resource counts, shared size and eight root counts. Both translator and replay use that same layout.
#define NVN2_C0_LAYOUT 0x860

// Shared memory bytes of a lowered NVN 2 compute stage (nvn2_lower.cpp, after the bank counts), 0 otherwise.
static uint32_t nvn2_shared(const uint8_t *control) {
    uint32_t n = 0;
    if (!memcmp(control + NVN2_C0_LAYOUT, "DKC1", 4)) memcpy(&n, control + NVN2_C0_LAYOUT + 8, 4);
    return n;
}
#define CODE_SKIP 0x30                                     // graphics code starts after the first 0x30 bytes of the program
#define BUCKETS 4096
#define LINE_MAX_LEN 4096
#define KEY_VERSION 5                                      // in every cache key; bump when the key layout or the use of the
                                                           // output changes here (5: guarded storage binding contract);
                                                           // translator changes are covered by its identity
#define IN_VERSION 1                                       // legacy input-ID schema and non-GS .in storage version
#define USE_MAGIC 0x4d554b54u                              // "TKUM", .use files
#define USE_VERSION 2                                      // 2: include every array location in VS-input / FS-output masks
#define IN_MAGIC 0x49534b54u                               // "TKSI", .in files
#define GS_IN_VERSION 2                                   // GS only: explicit packed answers plus input topology
#define ANSWERS_PACKED 147                                // legacy hash/record codec, independent of C struct padding
#define GS_ANS_PACKED (ANSWERS_PACKED + 1)
#define LEGACY_ANS_SIZE 152                               // version-1 .in ABI; do not replace with a new sizeof
#define MAX_SVC 32                                         // translator processes

typedef struct {                                           // which answers a shader's translation depends on
    uint32_t slots;                                        // tex_type, img_type, img_format of these c2 slots
    uint32_t attrs;                                        // attr_type (VS input Locations)
    uint32_t fouts;                                        // frag_out_type (FS output Locations)
} UseMask;

typedef struct UseEnt {                                    // code hash -> use mask; known 0 caches a disk miss
    struct UseEnt *next;
    uint8_t code_hash[32];
    int known;
    UseMask m;
} UseEnt;

struct GfxShader {
    struct GfxShader *next;
    uint8_t key[32];
    uint8_t iid[32];                                       // input id (see the keys at the top)
    uint32_t *spirv;
    uint64_t words;
    char *reflection;                                      // the service's .res.json text, parsed into the fields below
    uint64_t reflection_size;
    // Reflection (reflect(), once per shader before it enters the cache)
    int stage;                                             // GFX_STAGE_*
    GfxShaderRes *res[GFX_RES_KINDS];
    int nres[GFX_RES_KINDS];
    uint32_t in_mask, out_mask;                            // user Locations of Input / Output variables
    GfxShaderIo in[32], out[32];
    uint32_t local[3];
    GfxShaderGeometry geometry;
    uint32_t geometry_skip[8];                           // source mask, independent of queried draw answers
    int geometry_source_passthrough;
    unsigned geometry_position;                         // v2: Position input/output, malformed interface bit 4
    Nvn2C0Layout c0;                                       // NVN 2 compute parameter-block layout, when has_c0
    int has_c0;
    uint64_t origin;                                       // NVN 2 stage origin, set by note_origin (DKB_DUMP_SPV naming only)
    uint64_t spirv_fnv;                                    // FNV-1a 64 of spirv; 0 until gfx_shader_spirv_fnv fills it
    volatile int32_t used_noted;                           // compute: entered in this session's used list (cs-used.bin)
};

static PlatMutex cache_lock = PLAT_MUTEX_INIT;                  // memory cache
static GfxShader *buckets[BUCKETS];
static UseEnt *use_buckets[BUCKETS];                       // under cache_lock
static volatile int64_t n_requests, n_hits, n_failures, n_keys, n_xlated;   // n_keys: memory cache entries

typedef struct {                                           // one translator child
    PlatChild *child;                                      // NULL: not started
    int busy;                                              // a request owns it (under S.lock)
    size_t in_len;
    char inbuf[LINE_MAX_LEN * 2];
} Svc;

static struct {
    PlatMutex lock;
    PlatCond freed;
    int limit;                                             // children in use at most (0: svc_runtime_limit)
    Svc s[MAX_SVC];
} S = {PLAT_MUTEX_INIT, PLAT_COND_INIT};

static struct {                                            // translations in flight: cache key, or code hash while the
    PlatMutex lock;                                          // use mask is unknown; one translation per id at a time
    PlatCond done;
    uint8_t id[2 * MAX_SVC][32];
    int n;
} F = {PLAT_MUTEX_INIT, PLAT_COND_INIT};

#define PATH_CAP 1024                                  // bytes of a UTF-8 path
static char cache_dir[PATH_CAP];
static char xlate_exe[PATH_CAP];
static uint8_t xlate_id[32];                               // translator identity (see the keys at the top)
static int host_f64 = 1, host_set;                         // gfx_shader_set_host
static uint32_t host_sbuf, host_sg[3] = {32, 32, 32}, host_gather;
static uint32_t host_gs_sg;                               // must be supplied explicitly by the host
static PlatOnce paths_once = PLAT_ONCE_INIT;

// ---- small helpers

#define file_exists plat_file_exists
#define mkdir_p plat_file_mkdir_p
#define read_file plat_file_read
#define write_file plat_file_write

// ---- cache pack (<cache>/shader-cache.pack, written by tools/pack_shader_cache.py)
// Little-endian: "TKSPACK1", u32 count, u32 0, then count entries sorted by (kind, name), then the data. name is the
// 32 bytes the loose file's 64 hex digits spell. The index is read once (48 bytes a file: ~5.6 MB after an install);
// the data is read per lookup with positioned reads on one handle, safe from every thread. A pack that fails a check is
// ignored as a whole and the loose files are used.
enum { PACK_SPV = 1, PACK_RES = 2, PACK_USE = 3, PACK_IN = 4 };
typedef struct { uint8_t name[32]; uint32_t kind, size; uint64_t offset; } PackEnt;
_Static_assert(sizeof(PackEnt) == 48, "shader-cache.pack entry layout");
static PlatFile *pack_file;
static PackEnt *pack_ix;
static uint32_t pack_n;

static int pack_cmp(const PackEnt *a, uint32_t kind, const uint8_t name[32]) {
    if (a->kind != kind) return a->kind < kind ? -1 : 1;
    return memcmp(a->name, name, 32);
}

static int pack_read_at(uint64_t offset, void *out, uint32_t n) {
    return plat_file_pread(pack_file, out, n, offset) == (int64_t)n;
}

// Opens <cache>/shader-cache.pack and reads its index (init_paths). Nothing happens when there is none.
static void pack_open(void) {
    char p[PATH_CAP + 32];
    snprintf(p, sizeof p, "%s/shader-cache.pack", cache_dir);
    PlatFile *h = plat_file_open(p);
    if (!h) return;
    PlatFileId id;
    uint8_t head[16];
    uint32_t n = 0;
    const char *why = NULL;
    PackEnt *ix = NULL;
    pack_file = h;
    if (!plat_file_id(h, &id) || !pack_read_at(0, head, sizeof head) || memcmp(head, "TKSPACK1", 8)) why = "header";
    if (!why) {
        memcpy(&n, head + 8, 4);
        if (n > (1u << 24) || (uint64_t)n * sizeof *ix + sizeof head > id.size ||
            !(ix = malloc((size_t)n * sizeof *ix + 1)))
            why = "index size";
        else if (!pack_read_at(sizeof head, ix, n * (uint32_t)sizeof *ix)) why = "index read";
    }
    for (uint32_t i = 0; !why && i < n; i++) {
        if (ix[i].offset + ix[i].size > id.size) why = "entry out of bounds";
        else if (i && pack_cmp(&ix[i - 1], ix[i].kind, ix[i].name) >= 0) why = "index order";
    }
    if (why) {
        rt_log("gfx_shader: %s is not a usable cache pack (%s); reading the loose files", p, why);
        free(ix);
        plat_file_close(h);
        pack_file = NULL;
        return;
    }
    pack_ix = ix, pack_n = n;
    rt_log("gfx_shader: cache pack %s: %u files", p, n);
}

static const PackEnt *pack_find(uint32_t kind, const uint8_t name[32]) {
    size_t lo = 0, hi = pack_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = pack_cmp(&pack_ix[mid], kind, name);
        if (!c) return &pack_ix[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

// A packed file as plat_file_read returns a loose one: malloc'd, NUL after the last byte; NULL when absent or empty.
static uint8_t *pack_read(uint32_t kind, const uint8_t name[32], uint64_t *size) {
    const PackEnt *e = pack_find(kind, name);
    uint8_t *buf = e && e->size ? malloc((size_t)e->size + 1) : NULL;
    if (!buf) return NULL;
    if (!pack_read_at(e->offset, buf, e->size)) {
        free(buf);
        return NULL;
    }
    buf[e->size] = 0;
    *size = e->size;
    return buf;
}

// Appends a UTF-8 string as a JSON string body: ASCII as is, quotes and backslashes escaped, the rest as \uXXXX
// (UTF-16 units), so the service's stdin encoding does not matter.
static void json_esc(char **o, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p++;
        if (c >= 0x80) {
            int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            c &= 0x3f >> more;
            while (more-- && (*p & 0xc0) == 0x80) c = c << 6 | (*p++ & 0x3f);
        }
        if (c == '"' || c == '\\') *(*o)++ = '\\', *(*o)++ = (char)c;
        else if (c >= 0x20 && c < 0x7f) *(*o)++ = (char)c;
        else if (c >= 0x10000) {
            c -= 0x10000;
            *o += sprintf(*o, "\\u%04x\\u%04x", 0xd800 + (c >> 10), 0xdc00 + (c & 0x3ff));
        } else *o += sprintf(*o, "\\u%04x", c);
    }
}

// ---- paths and the translator identity

static int hex32(const char *h, uint8_t out[32]) {        // 64 hex digits, then nothing but white space
    for (int i = 0; i < 64; i++) {
        char c = h[i] >= 'A' && h[i] <= 'F' ? (char)(h[i] | 32) : h[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return 0;
        out[i / 2] = (uint8_t)(i & 1 ? out[i / 2] | v : v << 4);
    }
    for (const char *p = h + 64; *p; p++)
        if (*p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') return 0;
    return 1;
}

// TOTK_XLATE_ID (64 hex digits, or any other text, which is hashed), else <translator>.id next to the exe
// (tools/build_xlate.py), else the sha256 of the exe.
static void xlate_identity(void) {
    char text[256] = "";
    const char *how = "TOTK_XLATE_ID";
    const char *id_env = cfg_get("TOTK_XLATE_ID");
    if (id_env && *id_env && strlen(id_env) < sizeof text) {
        strcpy(text, id_env);
        if (!hex32(text, xlate_id)) rt_sha256(text, strlen(text), xlate_id);
    } else {
        char p[PATH_CAP + 8];
        strcpy(p, xlate_exe);
        char *dot = strrchr(p, '.'), *sep = plat_last_sep(p);
        if (dot && dot > sep) *dot = 0;
        strcat(p, ".id");
        uint64_t size = 0;
        char *f = (char *)read_file(p, &size);
        how = "totk_xlate.id";
        if (!f || !hex32(f, xlate_id)) {
            uint8_t *exe = read_file(xlate_exe, &size);
            how = exe ? "sha256 of the exe; tools/build_xlate.py writes totk_xlate.id" : "none: no translator";
            if (exe) rt_sha256(exe, (size_t)size, xlate_id);
            free(exe);
        }
        free(f);
    }
    rt_log("gfx_shader: translator identity %02x%02x%02x%02x%02x%02x%02x%02x (%s)", xlate_id[0], xlate_id[1], xlate_id[2],
           xlate_id[3], xlate_id[4], xlate_id[5], xlate_id[6], xlate_id[7], how);
}

// The repository root is the nearest parent of totk.exe that holds build/xlate/totk_xlate.exe. The shader cache lives
// next to totk.exe, so every build dir has its own (as the pipeline cache, vk_pipeline.c cache_path_init).
static void use_preload_start(void);

static void init_paths(void *param) {
    (void)param;
    char exe[PATH_CAP], root[PATH_CAP] = "", exe_dir[PATH_CAP] = "";
    if (plat_exe_path(exe, sizeof exe)) {
        char *last = plat_last_sep(exe);
        if (last) snprintf(exe_dir, sizeof exe_dir, "%.*s", (int)(last - exe), exe);
        for (char *cut = plat_last_sep(exe); cut; cut = plat_last_sep(exe)) {
            *cut = 0;
            char probe[PATH_CAP + 32];
            snprintf(probe, sizeof probe, "%s/build/xlate/totk_xlate" PLAT_EXE_SUFFIX, exe);
            if (file_exists(probe)) {
                strcpy(root, exe);
                break;
            }
        }
    }
    const char *xlate_cfg = cfg_get("TOTK_XLATE"), *cache_cfg = cfg_get("TOTK_SHADER_CACHE");
    char beside[PATH_CAP + 32];   // a packaged folder (tools/package_build.py) keeps the translator next to totk.exe
    if (exe_dir[0]) snprintf(beside, sizeof beside, "%s/totk_xlate" PLAT_EXE_SUFFIX, exe_dir);
    if (xlate_cfg && *xlate_cfg && strlen(xlate_cfg) < sizeof xlate_exe) strcpy(xlate_exe, xlate_cfg);
    else if (exe_dir[0] && file_exists(beside)) strcpy(xlate_exe, beside);
    else if (root[0]) snprintf(xlate_exe, sizeof xlate_exe, "%s/build/xlate/totk_xlate" PLAT_EXE_SUFFIX, root);
    if (cache_cfg && *cache_cfg && strlen(cache_cfg) < sizeof cache_dir) strcpy(cache_dir, cache_cfg);
    else if (exe_dir[0]) snprintf(cache_dir, sizeof cache_dir, "%s/shader-cache", exe_dir);
    xlate_identity();
    if (cache_dir[0]) pack_open();
    use_preload_start();
    return;
}

static void paths(void) { plat_once(&paths_once, init_paths, NULL); }

// Host answers (vk_caps.c, at device creation, before any translation). Answers other than the defaults (Float64,
// exact storage offsets, 32 lanes, no gather bias) are mixed into the translator identity, so they get their own cache
// keys.
void gfx_shader_set_host(int float64, uint32_t sbuf_align, const uint32_t subgroup[3], uint32_t gather_bits) {
    paths();
    if (host_set++) return;
    host_f64 = float64, host_sbuf = sbuf_align, host_gather = gather_bits;
    memcpy(host_sg, subgroup, sizeof host_sg);
    if (float64 && !sbuf_align && subgroup[0] == 32 && subgroup[1] == 32 && subgroup[2] == 32 && !gather_bits) return;
    uint8_t buf[32 + 96];
    int n = snprintf((char *)buf + 32, 96, "TKHO f64=%d sbuf=%u sg=%u,%u,%u", float64, sbuf_align, subgroup[0], subgroup[1],
                     subgroup[2]);
    if (gather_bits) n += snprintf((char *)buf + 32 + n, (size_t)(96 - n), " gather=%u", gather_bits);
    memcpy(buf, xlate_id, 32);
    rt_sha256(buf, 32 + (size_t)n, xlate_id);
    rt_log("gfx_shader: host options %s: own shader cache keys", (char *)buf + 37);
}

void gfx_shader_set_geometry_host(uint32_t subgroup) {
    if (!subgroup || (subgroup & (subgroup - 1))) rt_fatal("gfx_shader: invalid geometry subgroup size %u", subgroup);
    if (host_gs_sg == subgroup) return;
    if (host_gs_sg || n_requests || n_keys) rt_fatal("gfx_shader: geometry host answers changed after initialization");
    paths();
    host_gs_sg = subgroup;
    if (subgroup == 32) return;                           // the managed service's default; preserve legacy output keys
    // A separate capability tag preserves the existing three-stage host API. Like other host options this changes
    // output keys and use-mask namespaces, not code hashes or input IDs with the same learned dependencies.
    uint8_t buf[40];
    memcpy(buf, xlate_id, 32), memcpy(buf + 32, "TKHG", 4), memcpy(buf + 36, &subgroup, 4);
    rt_sha256(buf, sizeof buf, xlate_id);
}

// ---- the translator children

static void child_stop(Svc *v) {
    plat_child_stop(v->child);
    v->child = NULL;
    v->in_len = 0;
}

static int child_start(Svc *v) {
    paths();
    if (!xlate_exe[0] || !file_exists(xlate_exe)) {
        rt_log("gfx_shader: translator not found (%s); set TOTK_XLATE", xlate_exe[0] ? xlate_exe : "<none>");
        return 0;
    }
    uint32_t err = 0;
#ifndef _WIN32
    // An archive unpacked without Unix modes leaves the translator unexecutable (issue #122: error 13 for every
    // shader, so full-screen draws were dropped); make it executable once instead of failing every request.
    if (access(xlate_exe, X_OK) != 0) {
        if (chmod(xlate_exe, 0755) == 0) rt_log("gfx_shader: %s was not executable; fixed", xlate_exe);
        else rt_log("gfx_shader: %s is not executable and chmod failed", xlate_exe);
    }
#endif
    v->child = plat_child_start(xlate_exe, "--serve", &err);
    if (!v->child) {
        rt_log("gfx_shader: cannot start %s (error %lu)", xlate_exe, (unsigned long)err);
        return 0;
    }
    v->in_len = 0;
    return 1;
}

// One line out, one line in (no newline in the reply). 0 when the pipe broke.
static int child_exchange_raw(Svc *v, const char *line, char *reply, size_t reply_size) {
    if (!v->child && !child_start(v)) return 0;
    plat_inc64(&n_requests);
    size_t n = strlen(line);
    if (!plat_child_write(v->child, line, n)) return 0;
    if (!plat_child_write(v->child, "\n", 1)) return 0;
    for (;;) {
        char *nl = memchr(v->inbuf, '\n', v->in_len);
        if (nl) {
            size_t len = (size_t)(nl - v->inbuf), keep = len < reply_size - 1 ? len : reply_size - 1;
            memcpy(reply, v->inbuf, keep);
            while (keep && reply[keep - 1] == '\r') keep--;
            reply[keep] = 0;
            memmove(v->inbuf, nl + 1, v->in_len - len - 1);
            v->in_len -= len + 1;
            return 1;
        }
        if (v->in_len == sizeof v->inbuf) v->in_len = 0;  // an overlong line: drop it, wait for its end
        long got = plat_child_read(v->child, v->inbuf + v->in_len, sizeof v->inbuf - v->in_len);
        if (got <= 0) return 0;
        v->in_len += (size_t)got;
    }
}

static int child_exchange(Svc *v, const char *line, char *reply, size_t reply_size) {
    uint64_t p0 = perf_begin();   // TOTK_TRACE=perf: render-thread time spent translating
    int ok = child_exchange_raw(v, line, reply, reply_size);
    perf_end(PERF_XLATE, p0);
    return ok;
}

// Children allowed at run time: TOTK_XLATE_PROCS, default 2.
static int svc_runtime_limit(void) {
    static volatile int32_t n;
    if (!n) {
        const char *e = cfg_get("TOTK_XLATE_PROCS");
        int v = e && *e ? atoi(e) : 2;
        plat_xchg32(&n, v < 1 ? 1 : v > MAX_SVC ? MAX_SVC : v);
    }
    return (int)n;
}

static int svc_used(void) {                                // caller holds S.lock
    int used = 0;
    for (int i = 0; i < MAX_SVC; i++) used += S.s[i].busy || S.s[i].child;
    return used;
}

// A child for one request: an idle running one, else a new slot while fewer than S.limit are in use, else waits.
static Svc *svc_get(void) {
    plat_mutex_lock(&S.lock);
    if (!S.limit) S.limit = svc_runtime_limit();
    Svc *v;
    for (;;) {
        Svc *empty = NULL;
        v = NULL;
        for (int i = 0; i < MAX_SVC && !v; i++) {
            Svc *c = &S.s[i];
            if (!c->busy && c->child) v = c;
            else if (!c->busy && !empty) empty = c;
        }
        if (!v && empty && svc_used() < S.limit) v = empty;
        if (v) break;
        plat_cond_wait(&S.freed, &S.lock);
    }
    v->busy = 1;
    plat_mutex_unlock(&S.lock);
    return v;
}

// Returns a child; one over the limit (svc_set_limit lowered it) is stopped.
static void svc_put(Svc *v) {
    plat_mutex_lock(&S.lock);
    int stop = v->child && svc_used() > S.limit;
    if (!stop) v->busy = 0;
    plat_mutex_unlock(&S.lock);
    if (stop) {
        child_stop(v);
        plat_mutex_lock(&S.lock);
        v->busy = 0;
        plat_mutex_unlock(&S.lock);
    }
    plat_cond_broadcast(&S.freed);
}

// Sets the number of children; idle ones over it are stopped now, busy ones when they are returned.
static void svc_set_limit(int n) {
    Svc *stop[MAX_SVC];
    int k = 0;
    plat_mutex_lock(&S.lock);
    S.limit = n < 1 ? 1 : n > MAX_SVC ? MAX_SVC : n;
    for (int i = 0, used = svc_used(); i < MAX_SVC && used > S.limit; i++)
        if (!S.s[i].busy && S.s[i].child) S.s[i].busy = 1, stop[k++] = &S.s[i], used--;
    plat_mutex_unlock(&S.lock);
    for (int i = 0; i < k; i++) child_stop(stop[i]);
    plat_mutex_lock(&S.lock);
    for (int i = 0; i < k; i++) stop[i]->busy = 0;
    plat_mutex_unlock(&S.lock);
    plat_cond_broadcast(&S.freed);
}

// One request on a child; a dead child is restarted once. 0 when the translator is unavailable. The caller has given
// up its virtual core (kern_core_away): the wait for a child and the translation are host time.
static int svc_request(const char *line, char *reply, size_t reply_size) {
    Svc *v = svc_get();
    int ok = 0;
    for (int attempt = 0; attempt < 2 && !ok; attempt++)
        if (!(ok = child_exchange(v, line, reply, reply_size))) child_stop(v);
    svc_put(v);
    return ok;
}

// One translation per id at a time (F); F.id full also waits.
static void flight_enter(const uint8_t id[32]) {
    plat_mutex_lock(&F.lock);
    for (;;) {
        int busy = F.n == 2 * MAX_SVC;
        for (int i = 0; i < F.n && !busy; i++) busy = !memcmp(F.id[i], id, 32);
        if (!busy) break;
        plat_cond_wait(&F.done, &F.lock);
    }
    memcpy(F.id[F.n++], id, 32);
    plat_mutex_unlock(&F.lock);
}

static void flight_leave(const uint8_t id[32]) {
    plat_mutex_lock(&F.lock);
    for (int i = 0; i < F.n; i++)
        if (!memcmp(F.id[i], id, 32)) {
            memcpy(F.id[i], F.id[--F.n], 32);
            break;
        }
    plat_mutex_unlock(&F.lock);
    plat_cond_broadcast(&F.done);
}

// ---- cache

static GfxShader *mem_find(const uint8_t key[32]) {
    plat_mutex_lock_shared(&cache_lock);
    GfxShader *s = buckets[(key[0] | key[1] << 8) % BUCKETS];
    while (s && memcmp(s->key, key, 32)) s = s->next;
    plat_mutex_unlock_shared(&cache_lock);
    return s;
}

static void shader_free(GfxShader *s) {
    for (int k = 0; k < GFX_RES_KINDS; k++) free(s->res[k]);
    free(s->spirv), free(s->reflection), free(s);
}

enum { REFLECT_REQUIRE_C0 = 1, REFLECT_LOWERED_BODY = 2 };   // flags of reflect, disk_load_from and disk_load
static int reflect(GfxShader *s, int flags);

// Inserts s, or frees it and returns the entry another thread inserted first.
static GfxShader *mem_insert(GfxShader *s) {
    plat_mutex_lock(&cache_lock);
    GfxShader **head = &buckets[(s->key[0] | s->key[1] << 8) % BUCKETS], *r = *head;
    while (r && memcmp(r->key, s->key, 32)) r = r->next;
    if (r) {
        shader_free(s);
        s = r;
    } else {
        s->next = *head;
        *head = s;
        n_keys++;
    }
    plat_mutex_unlock(&cache_lock);
    return s;
}

static void key_hex(const uint8_t key[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[i * 2] = hex[key[i] >> 4], out[i * 2 + 1] = hex[key[i] & 15];
    out[64] = 0;
}

// A .res.json the translator wrote: one JSON object that names its stage, no NUL inside. A file an interrupted write
// left zero-filled (3647 of 21924 in one cache were) fails this; reflect() would take it for a vertex shader with
// no resources, and its pipeline layout would lack the shader's buffers and textures.
static int reflection_valid(const char *res, uint64_t size) {
    if (strlen(res) != size) return 0;
    const char *b = res, *e = res + size;
    while (b < e && isspace((unsigned char)*b)) b++;
    while (e > b && isspace((unsigned char)e[-1])) e--;
    if (e - b < 2 || *b != '{' || e[-1] != '}') return 0;
    const char *st = strstr(b, "\"stage\"");
    if (!st) return 0;
    st = strchr(st + 7, '"');
    return st && (!strncmp(st, "\"vs\"", 4) || !strncmp(st, "\"ps\"", 4) || !strncmp(st, "\"cs\"", 4) ||
                  !strncmp(st, "\"gs\"", 4));
}

// Every instruction after the header has a word count and the last one ends at the module's end: a module with
// zero-filled clusters fails.
static int spirv_complete(const uint32_t *w, uint64_t n) {
    uint64_t i = 5;
    while (i < n) {
        uint32_t len = w[i] >> 16;
        if (!len) return 0;
        i += len;
    }
    return i == n;
}

// Reads <key>.spv and <key>.res.json from the pack (packed 1) or the loose files; NULL if either is missing or
// malformed. iid: the input id, NULL when the caller sets it.
static __thread int rp_cs;   // KAR_REPLAY_PROF: get_one is loading a compute stage (gfx_rp[8..9]: read, check+reflect)
extern uint64_t gfx_rp[10];
static GfxShader *disk_load_from(const uint8_t key[32], const uint8_t *iid, int packed, int flags) {
    uint64_t q0 = rp_cs ? plat_now_ns() : 0;
    static volatile int32_t n_bad;
    char hex[65], p[PATH_CAP + 80];
    key_hex(key, hex);
    uint64_t spv_size = 0, res_size = 0;
    snprintf(p, PATH_CAP + 80, "%s/%s.res.json", cache_dir, hex);
    uint8_t *res = packed ? pack_read(PACK_RES, key, &res_size) : read_file(p, &res_size);
    if (!res) return NULL;
    if (!reflection_valid((const char *)res, res_size)) {
        int32_t n = plat_inc32(&n_bad);
        if (n <= 3 || !(n & (n - 1))) rt_log("gfx_shader: %s.res.json is malformed, translating again (%d so far)", hex, n);
        free(res);
        return NULL;
    }
    snprintf(p, PATH_CAP + 80, "%s/%s.spv", cache_dir, hex);
    uint8_t *spv = packed ? pack_read(PACK_SPV, key, &spv_size) : read_file(p, &spv_size);
    uint64_t q1 = q0 ? plat_now_ns() : 0;
    if (q0) gfx_rp[8] += q1 - q0;
    uint32_t magic = 0;
    if (spv && spv_size >= 20) memcpy(&magic, spv, 4);
    if (!spv || spv_size % 4 || magic != 0x07230203 || !spirv_complete((const uint32_t *)spv, spv_size / 4)) {
        free(res), free(spv);
        return NULL;
    }
    GfxShader *s = calloc(1, sizeof *s);
    memcpy(s->key, key, 32);
    if (iid) memcpy(s->iid, iid, 32);
    s->spirv = (uint32_t *)spv;
    s->words = spv_size / 4;
    s->reflection = (char *)res;
    s->reflection_size = res_size;
    int ok = reflect(s, flags);
    if (q0) gfx_rp[9] += plat_now_ns() - q1;
    if (!ok) { shader_free(s); return NULL; }
    return s;
}

// The pack, then the loose files (a newer translation of a key the pack holds a bad copy of); NULL when neither has a
// valid pair (the caller translates again and writes loose files).
static GfxShader *disk_load(const uint8_t key[32], const uint8_t *iid, int flags) {
    GfxShader *s = pack_n ? disk_load_from(key, iid, 1, flags) : NULL;
    return s ? s : disk_load_from(key, iid, 0, flags);
}

// ---- request building

static const char *const tex_names[] = {NULL, "1d", "1darray", "buffer", "2d", "2darray", "2dms", "2dmsarray", "3d",
                                        "cube", "cubearray"};   // GfxShaderAnswers tex_type / img_type codes
static const char *const fmt_names[] = {   // img_format codes: Ryujinx TextureFormat enum order
    NULL, "R8Unorm", "R8Snorm", "R8Uint", "R8Sint", "R16Float", "R16Unorm", "R16Snorm", "R16Uint", "R16Sint", "R32Float",
    "R32Uint", "R32Sint", "R8G8Unorm", "R8G8Snorm", "R8G8Uint", "R8G8Sint", "R16G16Float", "R16G16Unorm", "R16G16Snorm",
    "R16G16Uint", "R16G16Sint", "R32G32Float", "R32G32Uint", "R32G32Sint", "R8G8B8A8Unorm", "R8G8B8A8Snorm",
    "R8G8B8A8Uint", "R8G8B8A8Sint", "R16G16B16A16Float", "R16G16B16A16Unorm", "R16G16B16A16Snorm", "R16G16B16A16Uint",
    "R16G16B16A16Sint", "R32G32B32A32Float", "R32G32B32A32Uint", "R32G32B32A32Sint", "R10G10B10A2Unorm",
    "R10G10B10A2Uint", "R11G11B10Float"};
#define COUNT(a) ((int)(sizeof a / sizeof *a))

static const char *attr_name(uint8_t t) { return t == 1 ? "sint" : t == 2 ? "uint" : "float"; }

// ---- reflection: the .res.json the translator writes (tools/xlate/Program.cs Translate) and the SPIR-V itself

// `"name":` inside [o, e): the first byte after the colon and its spaces, NULL when absent.
static const char *json_key(const char *o, const char *e, const char *name) {
    char pat[40];
    int n = snprintf(pat, sizeof pat, "\"%s\":", name);
    for (const char *p = o; p + n <= e; p++)
        if (*p == '"' && !memcmp(p, pat, (size_t)n)) {
            p += n;
            while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
            return p;
        }
    return NULL;
}

static long json_int(const char *o, const char *e, const char *name, long def) {
    const char *p = json_key(o, e, name);
    return p ? strtol(p, NULL, 10) : def;
}

// Copies a string value into buf (empty when absent or not a string).
static void json_str(const char *o, const char *e, const char *name, char *buf, size_t n) {
    const char *p = json_key(o, e, name);
    size_t k = 0;
    if (p && *p == '"')
        for (p++; p < e && *p != '"' && k + 1 < n; p++) buf[k++] = *p;
    buf[k] = 0;
}

static const char *json_space(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

// Bounded JSON token traversal for the versioned geometry contract. The older resource reader below is intentionally
// unchanged; strtol alone cannot distinguish integer fields from floats, strings or truncated values.
static const char *json_token_end(const char *p, const char *end, unsigned depth) {
    p = json_space(p, end);
    if (p == end || depth > 64) return NULL;
    if (*p == '"') {
        for (p++; p < end; p++) {
            if (*p == '"') return p + 1;
            if ((unsigned char)*p < 32) return NULL;
            if (*p != '\\') continue;
            if (++p == end) return NULL;
            if (*p == 'u') {
                for (int i = 0; i < 4; i++) if (++p == end || !isxdigit((unsigned char)*p)) return NULL;
            } else if (!strchr("\"\\/bfnrt", *p)) return NULL;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        int object = *p == '{';
        char close = object ? '}' : ']';
        p = json_space(p + 1, end);
        if (p < end && *p == close) return p + 1;
        for (;;) {
            if (object) {
                if (p == end || *p != '"' || !(p = json_token_end(p, end, depth + 1))) return NULL;
                p = json_space(p, end);
                if (p == end || *p != ':') return NULL;
                p++;
            }
            if (!(p = json_token_end(p, end, depth + 1))) return NULL;
            p = json_space(p, end);
            if (p == end) return NULL;
            if (*p == close) return p + 1;
            if (*p != ',') return NULL;
            p = json_space(p + 1, end);
        }
    }
    for (int i = 0; i < 3; i++) {
        const char *literal = i == 0 ? "true" : i == 1 ? "false" : "null";
        size_t n = strlen(literal);
        if ((size_t)(end - p) >= n && !memcmp(p, literal, n)) return p + n;
    }
    if (*p == '-') p++;
    if (p == end || !isdigit((unsigned char)*p)) return NULL;
    if (*p == '0') p++;
    else while (p < end && isdigit((unsigned char)*p)) p++;
    if (p < end && *p == '.') {
        if (++p == end || !isdigit((unsigned char)*p)) return NULL;
        while (p < end && isdigit((unsigned char)*p)) p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        if (p == end || !isdigit((unsigned char)*p)) return NULL;
        while (p < end && isdigit((unsigned char)*p)) p++;
    }
    return p;
}

// NULL: absent; end: malformed object or duplicate/escaped keys. Object keys emitted by our service are plain ASCII.
static const char *json_member(const char *o, const char *end, const char *name) {
    const char *p = json_space(o, end), *found = NULL;
    size_t size = strlen(name);
    if (p == end || *p++ != '{') return end;
    for (;;) {
        p = json_space(p, end);
        if (p < end && *p == '}') return found;
        const char *key = p, *ke = json_token_end(p, end, 0);
        if (!ke || *p != '"' || memchr(p, '\\', (size_t)(ke - p))) return end;
        p = json_space(ke, end);
        if (p == end || *p++ != ':') return end;
        p = json_space(p, end);
        if ((size_t)(ke - key) == size + 2 && !memcmp(key + 1, name, size)) {
            if (found) return end;
            found = p;
        }
        if (!(p = json_token_end(p, end, 0))) return end;
        p = json_space(p, end);
        if (p < end && *p == '}') return found;
        if (p == end || *p++ != ',') return end;
    }
}

static int json_string_is(const char *p, const char *end, const char *value) {
    size_t n = strlen(value);
    return p && p < end && (size_t)(end - p) >= n + 2 && *p == '"' && !memcmp(p + 1, value, n) && p[n + 1] == '"';
}

static int json_uint(const char *o, const char *end, const char *name, uint32_t *out) {
    const char *p = json_member(o, end, name), *q;
    if (!p || p == end || !isdigit((unsigned char)*p) || !(q = json_token_end(p, end, 0))) return 0;
    uint32_t v = 0;
    for (; p < q; p++) {
        if (!isdigit((unsigned char)*p) || v > (UINT32_MAX - (uint32_t)(*p - '0')) / 10) return 0;
        v = v * 10 + (uint32_t)(*p - '0');
    }
    *out = v;
    return 1;
}

static const char *const geometry_names[] = {NULL, "points", "lines", "lines_adjacency", "triangles", "triangles_adjacency"};
static const uint32_t geometry_vertices[] = {0, 1, 2, 4, 3, 6};
static const char *const geometry_outputs[] = {NULL, "points", "line_strip", "triangle_strip"};

static int geometry_skip_json(const char *js, const char *end, uint32_t skip[8]) {
    const char *p = json_member(js, end, "gs_passthrough_skip");
    if (!p || p == end || *p++ != '[') return 0;
    for (unsigned i = 0; i < 8; i++) {
        p = json_space(p, end);
        const char *q = json_token_end(p, end, 0);
        if (!q || p == q || !isdigit((unsigned char)*p)) return 0;
        uint32_t v = 0;
        for (; p < q; p++) {
            if (!isdigit((unsigned char)*p) || v > (UINT32_MAX - (uint32_t)(*p - '0')) / 10) return 0;
            v = v * 10 + (uint32_t)(*p - '0');
        }
        skip[i] = v;
        p = json_space(p, end);
        if (p == end || *p++ != (i == 7 ? ']' : ',')) return 0;
    }
    return nvn_geometry_control_mask_valid(skip);
}

static int geometry_json(const char *js, const char *end, GfxShaderGeometry *g, uint32_t skip[8], int *source) {
    memset(g, 0, sizeof *g);
    memset(skip, 0, 32);
    *source = 0;
    if (!json_uint(js, end, "geometry_version", &g->version) ||
        (g->version != GFX_GEOMETRY_VERSION && g->version != GFX_GEOMETRY_PASSTHROUGH_VERSION)) return 0;
    const char *input = json_member(js, end, "gs_input"), *o = json_member(js, end, "geometry");
    for (unsigned i = 1; i < COUNT(geometry_names); i++)
        if (json_string_is(input, end, geometry_names[i])) g->input = i;
    if (!g->input || !o || o == end || *o != '{') return 0;
    const char *oe = json_token_end(o, end, 0);
    if (!oe) return 0;
    const char *pass = json_member(o, oe, "passthrough");
    if (!pass || pass == oe || oe - pass < 5 || memcmp(pass, "false", 5)) return 0;
    if (!json_uint(o, oe, "input_vertices", &g->input_vertices) || g->input_vertices != geometry_vertices[g->input] ||
        !json_uint(o, oe, "invocations", &g->invocations) || !g->invocations || g->invocations > 255 ||
        !json_uint(o, oe, "max_output_vertices", &g->max_output_vertices) || !g->max_output_vertices || g->max_output_vertices > 4095)
        return 0;
    const char *output = json_member(o, oe, "output_topology");
    for (unsigned i = 1; i < COUNT(geometry_outputs); i++)
        if (json_string_is(output, oe, geometry_outputs[i])) g->output = i;
    const char *src = json_member(o, oe, "source_passthrough");
    if (g->version == GFX_GEOMETRY_PASSTHROUGH_VERSION) {
        if (!src || src == oe || oe - src < 4 || memcmp(src, "true", 4) || !geometry_skip_json(js, end, skip) ||
            g->input != GFX_GEOMETRY_TRIANGLES || g->input_vertices != 3 || g->invocations != 1 ||
            g->max_output_vertices != 3 || g->output != GFX_GEOMETRY_OUTPUT_TRIANGLE_STRIP) return 0;
        *source = 1;
    } else if (src || json_member(js, end, "gs_passthrough_skip")) return 0;
    return g->output != 0;
}

// The objects of the flat array `"name": [ {...}, ... ]`: calls fn for each object's [start, end). Returns the count.
static int json_objects(const char *js, const char *name, void (*fn)(GfxShader *, int, const char *, const char *),
                        GfxShader *s, int kind) {
    const char *end = js + strlen(js), *p = json_key(js, end, name);
    if (!p || *p != '[') return 0;
    int n = 0;
    for (p++; p < end && *p != ']'; p++) {
        if (*p != '{') continue;
        const char *q = memchr(p, '}', (size_t)(end - p));
        if (!q) break;
        if (fn) fn(s, kind, p, q);
        n++;
        p = q;
    }
    return n;
}

static int fmt_code(const char *name) {
    for (int i = 1; i < COUNT(fmt_names); i++)
        if (!strcmp(fmt_names[i], name)) return i;
    return 0;
}

// c2 slot of a texture or image handle (Service.cs of MNG's totk_xlate: combined handles at 0x20, 32 slots; images at
// 0x120; separate textures at 0x168), -1 outside those ranges or for a handle in another constant buffer.
static int handle_slot(int kind, int cbuf_slot, int handle, int separate) {
    if (cbuf_slot >= 0 && cbuf_slot != 2) return -1;
    int bytes = (handle & 0x3fff) * 4;
    if (kind == GFX_RES_IMG) return bytes >= 0x120 && bytes < 0x168 ? (bytes - 0x120) / 8 : -1;
    if (separate) return bytes >= 0x168 && bytes < 0x568 ? (bytes - 0x168) / 8 : -1;
    return bytes >= 0x20 && bytes < 0x120 ? (bytes - 0x20) / 8 : -1;
}

static void add_res(GfxShader *s, int kind, const char *o, const char *e) {
    GfxShaderRes *r = &s->res[kind][s->nres[kind]++];
    r->set = 0;
    r->binding = (uint16_t)json_int(o, e, "binding", 0);
    if (kind == GFX_RES_UBO || kind == GFX_RES_SSBO) {
        r->slot = (int16_t)json_int(o, e, "slot", -1);
        r->count = 1;
        if (kind == GFX_RES_SSBO) {
            r->cbuf_slot = (int8_t)json_int(o, e, "cb_slot", -1);
            r->handle = (uint16_t)json_int(o, e, "cb_offset", 0);
            r->flags = (uint8_t)json_int(o, e, "flags", 0);
            // NVN 2 compute: the address's place in the packed bank, as the NVN 1 compute offset of the same buffer
            // (uniform buffer u at 0x210 + 16u, storage buffer k at 0x310 + 16k), which is what vk_exec_draw.c's
            // ssbo_slot reads. vk_c0_fill puts the addresses at the packed offsets the shader reads.
            int off = r->handle * 4, ub = (int)nvn2_c0_part(&s->c0, 1), sb = (int)nvn2_c0_part(&s->c0, 2),
                end = (int)nvn2_c0_part(&s->c0, 3);
            if (!(r->flags & GFX_RES_SSBO_GUARDED) && s->has_c0 && r->cbuf_slot == 0 &&
                off >= ub && off < end && (off - ub) % 16 == 0)
                r->handle = (uint16_t)((off < sb ? 0x210 + (off - ub) : 0x310 + (off - sb)) / 4);
        }
        return;
    }
    char buf[32];
    json_str(o, e, "format", buf, sizeof buf);
    const char *sep = json_key(o, e, "separate");
    r->type = (uint16_t)json_int(o, e, "type", 0);
    r->format = (uint8_t)fmt_code(buf);
    r->flags = (uint8_t)json_int(o, e, "flags", 0);
    // cbuf_slot is Ryujinx's TextureHandle.PackSlots: the texture buffer in the low 16 bits, the sampler buffer + 1
    // above (0: the same buffer). -1 stays -1.
    long long packed = json_int(o, e, "cbuf_slot", -1);
    int tex_cb = packed < 0 ? -1 : (int)(packed & 0xffff), samp_cb = packed < 0 ? -1 : (int)(packed >> 16) - 1;
    if (packed >= 0 && samp_cb < 0) samp_cb = tex_cb;
    r->cbuf_slot = (int8_t)tex_cb;
    int handle = (int)json_int(o, e, "handle", 0);
    r->handle = (uint16_t)handle;
    r->count = (uint16_t)json_int(o, e, "array", 1);
    r->separate = sep && *sep == 't';
    r->arr_ubo = 0, r->from_img = 0, r->pool_arr = 0, r->c0_byte = 0, r->samp_tbl = 0;
    // Ryujinx's "Direct" handle (TextureHandle 0x40000000, QueryTextureArrayLengthFromPool = 32): the whole texture or
    // sampler pool as an array, indexed by the handle's id. Kirby Air Riders has 41 fragment shaders whose handle is a
    // phi of c2 handles. pool_patch makes the index id & 31 and finds the c2 words the shader reads; the replay fills
    // element id & 31 from them (vk_exec_draw.c bind_resources).
    if (kind == GFX_RES_TEX && handle == 0x40000000 && r->count == 32) {
        r->pool_arr = 1, r->slot = -1, r->samp_slot = -1;
        return;
    }
    // Ryujinx BindlessToArray (BindlessToArray.cs RunPass): a texture indexed through handles in an ordinary constant
    // buffer. Kirby Air Riders' materials keep up to 32 handles in c5..c8; the sampler, when separate, is a c2 word
    // (handle type SeparateSamplerHandle at bit 28, its word offset in bits 14..27). The replay reads the element
    // handles from the bound buffer (vk_exec_draw.c bind_resources).
    if (kind == GFX_RES_TEX && tex_cb >= 3 && tex_cb - 3 < 18) {
        r->arr_ubo = 1;
        r->samp_slot = -1;
        int type = (int)((unsigned)handle >> 28), sb = ((handle >> 14) & 0x3fff) * 4;
        if (type == 1 && samp_cb == 2 && sb >= 0x568 && sb < 0x668) r->samp_slot = (int8_t)((sb - 0x568) / 8);
        // NVN 2 compute: the separate sampler handle is a c0 word in the packed bank's texture table (Kirby Air Riders'
        // local-light compute, handle 0x10090000: sampler at c0 byte 0x90). Its entry index is the bind slot the
        // replay reads the sampler from (vk_exec_draw.c bind_resources); without this the array got sampler 0.
        if (type == 1 && samp_cb == 0 && s->has_c0) {
            int at = (int)nvn2_c0_part(&s->c0, 3), n = s->c0.textures;
            if (sb >= at && sb < at + 8 * n && (sb - at) % 8 == 0) r->samp_slot = (int8_t)((sb - at) / 8), r->samp_tbl = 1;
            else {
                static int told;
                if (told++ < 8) rt_log("gfx_shader: array sampler at c0 byte %#x outside the texture table", sb);
            }
        }
        r->slot = -1;
        return;
    }
    // A handle with a second offset above bit 14 (and a handle type at bit 28) names a texture and a separate sampler:
    // its texture half is a separate-texture entry (c2 0x168 and up), whether or not the reflection marked it separate.
    // Kirby Air Riders' NVN 2 programs read textures this way (handle 0x1056805a: texture at c2 0x168).
    // The two 14-bit halves come in either order (0x1056805a: texture 0x168 low, sampler 0x568 high; 0x1016815a: the
    // reverse), so the half in the separate-texture range names the texture.
    // The descriptor type must stay as the SPIR-V declares it (r->separate from the reflection): a combined sampler
    // declared over a split handle takes its texture from the separate-texture slot and its sampler from the
    // separate-sampler slot (r->samp_slot), which the replay looks up (vk_exec_draw.c bind_resources).
    r->samp_slot = -1;
    int split_tex = 0;
    if (kind != GFX_RES_IMG) {
        int lo = (handle & 0x3fff) * 4, hi = ((handle >> 14) & 0x3fff) * 4;
        int lo_tex = lo >= 0x168 && lo < 0x568, hi_tex = hi >= 0x168 && hi < 0x568;
        int lo_samp = lo >= 0x568 && lo < 0x668, hi_samp = hi >= 0x568 && hi < 0x668;
        if (!lo_tex && hi_tex) {
            if (lo_samp) r->samp_slot = (int8_t)((lo - 0x568) / 8);
            handle = (handle >> 14) & 0x3fff, split_tex = 1;
        } else if (lo_tex) {
            if (hi_samp) r->samp_slot = (int8_t)((hi - 0x568) / 8);
            split_tex = 1;
        }
    }
    r->slot = (int16_t)handle_slot(kind, r->cbuf_slot, handle, r->separate || split_tex);
    if (r->slot < 0) {   // KAR diagnostic: where the shader reads a handle no c2 slot covers
        static int told;
        if (told++ < 24)
            rt_log("gfx_shader: %s handle outside the c2 slots: cbuf_slot %d handle %#x (byte %#x) separate %d has_c0 %d",
                   kind == GFX_RES_IMG ? "image" : "texture", r->cbuf_slot, handle, (handle & 0x3fff) * 4, r->separate,
                   s->has_c0);
    }
    if (s->has_c0 && (r->cbuf_slot <= 0 || r->cbuf_slot == 2)) {   // NVN 2 compute: packed-bank handles, 8 bytes each
        int bytes = (handle & 0x3fff) * 4, at = (int)nvn2_c0_part(&s->c0, kind == GFX_RES_IMG ? 4 : 3);
        int n = kind == GFX_RES_IMG ? s->c0.images : s->c0.textures;
        r->slot = (int16_t)(bytes >= at && bytes < at + 8 * n && (bytes - at) % 8 == 0 ? (bytes - at) / 8 : -1);
        // A texture handle in the image range (the translator's TexSlot accepts it there too): the texture path reads
        // that image, as on the console where images register in the texture pool.
        int ia = (int)nvn2_c0_part(&s->c0, 4);
        if (kind == GFX_RES_TEX && r->slot < 0 && bytes >= ia && bytes < ia + 8 * s->c0.images && (bytes - ia) % 8 == 0)
            r->slot = (int16_t)((bytes - ia) / 8), r->from_img = 1;
        // Still unmatched: remember where the handle word is in c0, for the KAR_C0_TEX experiment.
        if (kind == GFX_RES_TEX && r->slot < 0 && r->cbuf_slot <= 0 && bytes > 0 && bytes + 4 <= (int)NVN2_C0_BYTES)
            r->c0_byte = (uint16_t)bytes;
        if (r->slot < 0) {   // KAR diagnostic: the packed layout this handle missed
            static int told2;
            if (told2++ < 40)
                rt_log("gfx_shader: NVN 2 %s handle byte %#x unmatched: roots %u %u %u %u %u %u %u %u ubos %u ssbos %u "
                       "textures %u (at %#zx) images %u (at %#zx)", kind == GFX_RES_IMG ? "image" : "texture", bytes,
                       s->c0.roots[0], s->c0.roots[1], s->c0.roots[2], s->c0.roots[3], s->c0.roots[4], s->c0.roots[5],
                       s->c0.roots[6], s->c0.roots[7], s->c0.ubos, s->c0.ssbos, s->c0.textures, nvn2_c0_part(&s->c0, 3),
                       s->c0.images, nvn2_c0_part(&s->c0, 4));
        }
    }
}

// "Direct" pool arrays (add_res, GfxShaderRes.pool_arr). The translator indexes them with UMin(id, 31), so any id
// above 31 read pool entry 31. This rewrites each such UMin whose result (through a Bitcast) indexes a pool array into
// BitwiseAnd(id, 31), and records which c2 handle words the shader reads (constant-index access chains into its c2
// uniform buffer): the phi's candidates are among them, and the replay puts each candidate's texture and sampler at
// element id & 31. Two candidates whose ids agree in the low 5 bits would share an element (logged by the replay).
// Returns 0 when the module does not have the expected shape (the arrays then stay unfilled, as before).
static int pool_patch(GfxShader *s) {
    enum { OP_NAME_ = 5, OP_EXT_INST = 12, OP_CONST = 43, OP_VAR = 59, OP_ACCESS_CHAIN = 65, OP_BITCAST = 124,
           OP_BITWISE_AND = 199, OP_DECOR = 71, DEC_BINDING = 33, GLSL_UMIN = 38 };
    int any = 0, c2_binding = -1;
    for (int i = 0; i < s->nres[GFX_RES_TEX]; i++)
        any |= s->res[GFX_RES_TEX][i].pool_arr && !s->res[GFX_RES_TEX][i].pool_bl;   // pool_bl: done by pool_bindless_patch
    if (!any) return 1;
    for (int i = 0; i < s->nres[GFX_RES_UBO]; i++)
        if (s->res[GFX_RES_UBO][i].slot == 2) c2_binding = s->res[GFX_RES_UBO][i].binding;
    const uint32_t *w = s->spirv;
    uint64_t n = s->words;
    uint32_t bound = w[3];
    if (n < 5 || bound > (1u << 22)) return 0;
    // Per id: 1 the c2 variable, 2 a pool array variable; constant values; bitcast sources; UMin results to rewrite.
    uint8_t *role = calloc(bound, 1), *is_const = calloc(bound, 1), *rewrite = calloc(bound, 1);
    uint32_t *val = calloc(bound, sizeof *val), *cast = calloc(bound, sizeof *cast);
    uint32_t *out = malloc(n * sizeof *out);
    if (!role || !is_const || !rewrite || !val || !cast || !out) rt_fatal("gfx_shader: out of memory patching a shader");
    uint32_t comb = 0, samp = 0;
    uint64_t sep[2] = {0, 0};
    for (uint64_t i = 5; i < n;) {   // pass 1: bindings, constants, bitcasts
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || i + len > n) goto bad;
        if (op == OP_DECOR && len == 4 && w[i + 2] == DEC_BINDING && w[i + 1] < bound) {
            uint32_t id = w[i + 1], b = w[i + 3];
            if ((int)b == c2_binding) role[id] = 1;
            for (int k = 0; k < s->nres[GFX_RES_TEX]; k++)
                if (s->res[GFX_RES_TEX][k].pool_arr && s->res[GFX_RES_TEX][k].binding == b) role[id] = 2;
        } else if (op == OP_CONST && len == 4 && w[i + 2] < bound) {
            is_const[w[i + 2]] = 1, val[w[i + 2]] = w[i + 3];
        } else if (op == OP_BITCAST && len == 4 && w[i + 2] < bound) {
            cast[w[i + 2]] = w[i + 3];
        }
        i += len;
    }
    for (uint64_t i = 5; i < n;) {   // pass 2: access chains
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == OP_ACCESS_CHAIN && len >= 5 && w[i + 3] < bound) {
            uint32_t base = w[i + 3];
            if (role[base] == 1 && len >= 6) {   // c2.member0[vec4][component]
                int ok = 1;
                for (uint32_t k = 4; k < len; k++) ok &= w[i + k] < bound && is_const[w[i + k]];
                if (ok) {
                    uint32_t lo = val[w[i + 5]] * 16, hi = lo + (len >= 7 ? val[w[i + 6]] * 4 : 12);
                    for (uint32_t b = lo - lo % 4; b <= hi; b += 4) {
                        if (b % 8) continue;
                        if (b >= 0x20 && b < 0x120) comb |= 1u << ((b - 0x20) / 8);
                        else if (b >= 0x168 && b < 0x568) sep[(b - 0x168) / 8 / 64] |= 1ull << ((b - 0x168) / 8 % 64);
                        else if (b >= 0x568 && b < 0x668) samp |= 1u << ((b - 0x568) / 8);
                    }
                }
            } else if (role[base] == 2 && w[i + 4] < bound && cast[w[i + 4]] < bound) {
                rewrite[cast[w[i + 4]]] = 1;
            }
        }
        i += len;
    }
    // The index becomes (id ^ (id >> 5)) & 31: plain id & 31 put the two candidates of a common shader (ids 321 and
    // 641, 320 apart) in one element. A constant 5 of the 31's type follows each such 31; new ids go past the bound.
    uint32_t *five = calloc(bound, sizeof *five), nb = bound;
    if (!five) rt_fatal("gfx_shader: out of memory patching a shader");
    uint64_t n_umin = 0, n_five = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == OP_EXT_INST && len == 7 && w[i + 4] == GLSL_UMIN && w[i + 2] < bound && rewrite[w[i + 2]] &&
            w[i + 6] < bound && is_const[w[i + 6]] && val[w[i + 6]] == 31) {
            n_umin++;
            if (!five[w[i + 6]]) five[w[i + 6]] = nb++, n_five++;
        }
        i += len;
    }
    // Each rewrite turns 7 words into 15; each new constant adds 4.
    uint32_t *grown = realloc(out, (n + 8 * n_umin + 4 * n_five + 8) * sizeof *out);
    if (!grown) rt_fatal("gfx_shader: out of memory patching a shader");
    out = grown;
    uint64_t m = 0;
    int patched = 0;
    for (uint64_t i = 0; i < 5; i++) out[m++] = w[i];
    for (uint64_t i = 5; i < n;) {   // pass 3: copy; UMin(x, 31) → BitwiseAnd(x ^ (x >> 5), 31) where marked
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == OP_EXT_INST && len == 7 && w[i + 4] == GLSL_UMIN && w[i + 2] < bound && rewrite[w[i + 2]] &&
            w[i + 6] < bound && five[w[i + 6]]) {
            uint32_t type = w[i + 1], x = w[i + 5], sh = nb++, xr = nb++;
            out[m++] = 5u << 16 | 194, out[m++] = type, out[m++] = sh, out[m++] = x, out[m++] = five[w[i + 6]];
            out[m++] = 5u << 16 | 198, out[m++] = type, out[m++] = xr, out[m++] = x, out[m++] = sh;
            out[m++] = 5u << 16 | OP_BITWISE_AND, out[m++] = type, out[m++] = w[i + 2], out[m++] = xr, out[m++] = w[i + 6];
            patched++;
        } else {
            memcpy(out + m, w + i, len * sizeof *w), m += len;
            if (op == OP_CONST && len == 4 && w[i + 2] < bound && five[w[i + 2]])
                out[m++] = 4u << 16 | OP_CONST, out[m++] = w[i + 1], out[m++] = five[w[i + 2]], out[m++] = 5;
        }
        i += len;
    }
    out[3] = nb;
    free(five);
    for (int k = 0; k < s->nres[GFX_RES_TEX]; k++) {
        GfxShaderRes *r = &s->res[GFX_RES_TEX][k];
        if (r->pool_arr) r->pool_comb = comb, r->pool_samp = samp, r->pool_sep[0] = sep[0], r->pool_sep[1] = sep[1];
    }
    free(s->spirv);
    s->spirv = out, s->words = m;
    free(role), free(is_const), free(rewrite), free(val), free(cast);
    static int told;
    if (told++ < 4)
        rt_log("gfx_shader: Direct pool arrays: %d index clamps rewritten; c2 candidates: combined %#x, separate "
               "textures %#llx%016llx, samplers %#x", patched, comb, (unsigned long long)sep[1],
               (unsigned long long)sep[0], samp);
    return patched > 0;
bad:
    free(role), free(is_const), free(rewrite), free(val), free(cast), free(out);
    return 0;
}

// Material texture arrays (GfxShaderRes.arr_ubo) as Eden builds them. The translator (Ryujinx BindlessToArray)
// declares 32 elements and clamps the index with UMin(i, 31); Eden, which plays Kirby Air Riders, reads up to 1024
// descriptors per array at the shader's own stride (refs/eglantine src/shader_recompiler/ir_opt/texture_pass.cpp
// DynamicDescriptorCount, src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp read_handle). Kirby's materials keep
// their handles 16 bytes apart, so 32 eight-byte elements reach only 16 textures and every later one read element 31.
// This retypes each such array variable to ARR_N elements (a new length constant, array and pointer type after the
// old pointer type) and raises the clamp to ARR_N - 1 where a UMin result indexes it; GfxShaderRes.count follows.
// KAR_ARR_N sets ARR_N; the default 32 keeps the translator's arrays (a 128-element run logged no handle past element
// 31, so Kirby's materials fit in 32). Returns 0 when the module does not have the expected shape.
static uint32_t arr_n(void) {
    static uint32_t n;
    if (!n) {
        const char *e = getenv("KAR_ARR_N");
        // default 512: Kirby Air Riders' local-light compute indexes c4 handles past element 31 (elements 32, 34, 36
        // hold handles in the race); with 32 its probe atlas reads hit the clamped dummy and characters went dark
        long v = e && *e ? strtol(e, NULL, 10) : 512;   // KAR_ARR_SCAN: handles up to element 416 in the race
        n = v >= 32 && v <= 1024 ? (uint32_t)v : 32;
    }
    return n;
}


// KAR_ARR_DIMS=1 (diagnostic, bindless 16.2): the image types behind the material arrays (GfxShaderRes.arr_ubo), each
// distinct (dim, arrayed, depth, sampled type, ms) once: a global descriptor array holds one image type, so this tells
// how many global arrays bindless needs. The arrays are found by their binding decoration (GfxShaderRes.binding).
static void arr_dims(const GfxShader *s) {
    static int on = -1;
    if (on < 0) on = getenv("KAR_ARR_DIMS") && *getenv("KAR_ARR_DIMS") == '1';
    if (!on) return;
    enum { OP_TYPE_INT_ = 21, OP_TYPE_FLOAT_ = 22, OP_TYPE_IMAGE_ = 25, OP_TYPE_SAMPLED_IMAGE_ = 27, OP_TYPE_ARRAY_ = 28,
           OP_TYPE_POINTER_ = 32, OP_VAR = 59, OP_DECOR = 71, DEC_BINDING = 33 };
    const uint32_t *w = s->spirv;
    uint64_t n = s->words;
    if (!w || n < 5) return;
    uint32_t bound = w[3];
    if (bound > (1u << 22)) return;
    uint32_t *binding = calloc(bound, 4), *ptr_to = calloc(bound, 4), *arr_of = calloc(bound, 4), *simg_of = calloc(bound, 4);
    uint32_t (*img)[6] = calloc(bound, sizeof *img);   // sampled type, dim, depth, arrayed, ms, sampled
    uint8_t *num = calloc(bound, 1);                   // 1 float, 2 signed int, 3 unsigned int
    if (!binding || !ptr_to || !arr_of || !simg_of || !img || !num) goto done;
    for (int pass = 0; pass < 2; pass++)
        for (uint64_t i = 5; i < n;) {
            uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
            if (!len || i + len > n) break;
            if (pass == 0) {
                if (op == OP_DECOR && len >= 4 && w[i + 2] == DEC_BINDING && w[i + 1] < bound) binding[w[i + 1]] = w[i + 3] + 1;
                else if (op == OP_TYPE_FLOAT_ && w[i + 1] < bound) num[w[i + 1]] = 1;
                else if (op == OP_TYPE_INT_ && len >= 4 && w[i + 1] < bound) num[w[i + 1]] = w[i + 3] ? 2 : 3;
                else if (op == OP_TYPE_IMAGE_ && len >= 9 && w[i + 1] < bound)
                    for (int k = 0; k < 6; k++) img[w[i + 1]][k] = w[i + 2 + k];
                else if (op == OP_TYPE_SAMPLED_IMAGE_ && len >= 3 && w[i + 1] < bound) simg_of[w[i + 1]] = w[i + 2];
                else if (op == OP_TYPE_ARRAY_ && len >= 4 && w[i + 1] < bound) arr_of[w[i + 1]] = w[i + 2];
                else if (op == OP_TYPE_POINTER_ && len >= 4 && w[i + 1] < bound) ptr_to[w[i + 1]] = w[i + 3];
            } else if (op == OP_VAR && len >= 4 && w[i + 2] < bound) {
                uint32_t id = w[i + 2], b = binding[id];
                if (!b) goto next;
                for (int r = 0; r < s->nres[GFX_RES_TEX]; r++) {
                    const GfxShaderRes *res = &s->res[GFX_RES_TEX][r];
                    if (!res->arr_ubo || res->binding != b - 1) continue;
                    uint32_t t = ptr_to[w[i + 1]], a = t < bound ? arr_of[t] : 0, si = a && a < bound ? simg_of[a] : 0;
                    if (!si || si >= bound) continue;
                    const uint32_t *m = img[si];
                    uint32_t key = m[1] | m[2] << 4 | m[3] << 6 | m[4] << 7 | (m[0] < bound ? num[m[0]] : 0) << 8 |
                                   (uint32_t)(res->samp_slot >= 0) << 12 | (uint32_t)res->separate << 13;
                    static uint32_t seen[64];
                    static int nseen;
                    int k = 0;
                    while (k < nseen && seen[k] != key) k++;
                    if (k == nseen && nseen < 64) {
                        seen[nseen++] = key;
                        char kh[65];
                        for (int q = 0; q < 32; q++) snprintf(kh + 2 * q, 3, "%02x", s->key[q]);
                        rt_log("arr dims: dim %u depth %u arrayed %u ms %u type %s samp_slot %d separate %d (stage %d binding %d) %s", m[1], m[2],
                               m[3], m[4], (m[0] < bound && num[m[0]] == 1) ? "float" : (m[0] < bound && num[m[0]] == 2) ? "int" : "uint",
                               res->samp_slot, res->separate, s->stage, res->binding, kh);
                    }
                }
            }
        next:
            i += len;
        }
done:
    free(binding), free(ptr_to), free(arr_of), free(simg_of), free(img), free(num);
}

static int arr_patch(GfxShader *s) {
    enum { OP_EXT_INST = 12, OP_TYPE_ARRAY_ = 28, OP_TYPE_POINTER_ = 32, OP_CONST = 43, OP_VAR = 59,
           OP_ACCESS_CHAIN = 65, OP_BITCAST = 124, OP_DECOR = 71, DEC_BINDING = 33, GLSL_UMIN = 38 };
    uint32_t N = arr_n();
    int any = 0;
    for (int i = 0; i < s->nres[GFX_RES_TEX]; i++) any |= s->res[GFX_RES_TEX][i].arr_ubo;
    if (!any || N == 32) return 1;
    const uint32_t *w = s->spirv;
    uint64_t n = s->words;
    uint32_t bound = w[3];
    if (n < 5 || bound > (1u << 22)) return 0;
    uint8_t *is_arr_var = calloc(bound, 1), *is_const = calloc(bound, 1), *rewrite = calloc(bound, 1);
    uint32_t *val = calloc(bound, 4), *ctype = calloc(bound, 4), *arr_len = calloc(bound, 4), *pointee = calloc(bound, 4),
             *vtype = calloc(bound, 4), *cast = calloc(bound, 4), *newptr = calloc(bound, 4), *binding = calloc(bound, 4);
    if (!is_arr_var || !is_const || !rewrite || !val || !ctype || !arr_len || !pointee || !vtype || !cast || !newptr ||
        !binding)
        rt_fatal("gfx_shader: out of memory patching a shader");
    for (uint64_t i = 5; i < n;) {   // pass 1: bindings, constants, array and pointer types, variables, bitcasts
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || i + len > n) goto bad;
        if (op == OP_DECOR && len == 4 && w[i + 2] == DEC_BINDING && w[i + 1] < bound) {
            for (int k = 0; k < s->nres[GFX_RES_TEX]; k++)
                if (s->res[GFX_RES_TEX][k].arr_ubo && s->res[GFX_RES_TEX][k].binding == w[i + 3])
                    is_arr_var[w[i + 1]] = 1, binding[w[i + 1]] = w[i + 3];
        } else if (op == OP_CONST && len == 4 && w[i + 2] < bound) {
            is_const[w[i + 2]] = 1, val[w[i + 2]] = w[i + 3], ctype[w[i + 2]] = w[i + 1];
        } else if (op == OP_TYPE_ARRAY_ && len == 4 && w[i + 1] < bound) {
            arr_len[w[i + 1]] = w[i + 3];
        } else if (op == OP_TYPE_POINTER_ && len == 4 && w[i + 1] < bound) {
            pointee[w[i + 1]] = w[i + 3];
        } else if (op == OP_VAR && len >= 4 && w[i + 2] < bound) {
            vtype[w[i + 2]] = w[i + 1];
        } else if (op == OP_BITCAST && len == 4 && w[i + 2] < bound) {
            cast[w[i + 2]] = w[i + 3];
        }
        i += len;
    }
    // Which pointer types to duplicate: those of the material array variables over a 32-element array.
    uint32_t nb = bound, uint_t = 0, c_n = 0, c_max = 0, nptr = 0;
    for (uint32_t v = 0; v < bound; v++) {
        if (!is_arr_var[v]) continue;
        uint32_t pt = vtype[v], at = pt < bound ? pointee[pt] : 0, lc = at < bound ? arr_len[at] : 0;
        if (!pt || !at || !lc || lc >= bound || !is_const[lc] || val[lc] != 32) { is_arr_var[v] = 0; continue; }
        if (!uint_t) uint_t = ctype[lc];
        if (ctype[lc] != uint_t) { is_arr_var[v] = 0; continue; }
        if (!newptr[pt]) newptr[pt] = nb + 3, nb += 4, nptr++;   // per pointer type: const N, max, array, pointer
    }
    if (!nptr) goto bad;
    for (uint64_t i = 5; i < n;) {   // pass 2: UMin results that index a material array
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == OP_ACCESS_CHAIN && len >= 5 && w[i + 3] < bound && is_arr_var[w[i + 3]] && w[i + 4] < bound &&
            cast[w[i + 4]] < bound)
            rewrite[cast[w[i + 4]]] = 1;
        i += len;
    }
    uint32_t *out = malloc((n + 16 * nptr + 8) * sizeof *out);
    if (!out) rt_fatal("gfx_shader: out of memory patching a shader");
    uint64_t m = 0;
    int clamps = 0, vars = 0;
    for (uint64_t i = 0; i < 5; i++) out[m++] = w[i];
    for (uint64_t i = 5; i < n;) {   // pass 3: copy with the new types after each duplicated pointer type
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == OP_VAR && len >= 4 && w[i + 2] < bound && is_arr_var[w[i + 2]]) {
            memcpy(out + m, w + i, len * sizeof *w);
            out[m + 1] = newptr[w[i + 1]];
            m += len, vars++;
        } else if (op == OP_EXT_INST && len == 7 && w[i + 4] == GLSL_UMIN && w[i + 2] < bound && rewrite[w[i + 2]] &&
                   w[i + 6] < bound && is_const[w[i + 6]] && val[w[i + 6]] == 31 && c_max) {
            memcpy(out + m, w + i, len * sizeof *w);
            out[m + 6] = c_max;
            m += len, clamps++;
        } else {
            memcpy(out + m, w + i, len * sizeof *w), m += len;
            if (op == OP_TYPE_POINTER_ && len == 4 && w[i + 1] < bound && newptr[w[i + 1]]) {
                uint32_t np = newptr[w[i + 1]], cn = np - 3, cm = np - 2, na = np - 1;
                uint32_t elem = 0;   // the old array's element type
                for (uint64_t j = 5; j < i && !elem;) {
                    uint32_t l2 = w[j] >> 16;
                    if ((w[j] & 0xffff) == OP_TYPE_ARRAY_ && l2 == 4 && w[j + 1] == w[i + 3]) elem = w[j + 2];
                    j += l2;
                }
                if (!elem) { free(out); goto bad; }
                out[m++] = 4u << 16 | OP_CONST, out[m++] = uint_t, out[m++] = cn, out[m++] = N;
                out[m++] = 4u << 16 | OP_CONST, out[m++] = uint_t, out[m++] = cm, out[m++] = N - 1;
                out[m++] = 4u << 16 | OP_TYPE_ARRAY_, out[m++] = na, out[m++] = elem, out[m++] = cn;
                out[m++] = 4u << 16 | OP_TYPE_POINTER_, out[m++] = np, out[m++] = w[i + 2], out[m++] = na;
                if (!c_max) c_max = cm;
            }
        }
        i += len;
    }
    out[3] = nb;
    for (int k = 0; k < s->nres[GFX_RES_TEX]; k++) {   // the layout and the replay bind N elements
        GfxShaderRes *r = &s->res[GFX_RES_TEX][k];
        for (uint32_t v = 0; v < bound && r->arr_ubo; v++)
            if (is_arr_var[v] && binding[v] == r->binding) { r->count = (uint16_t)N; break; }
    }
    free(s->spirv);
    s->spirv = out, s->words = m;
    free(is_arr_var), free(is_const), free(rewrite), free(val), free(ctype), free(arr_len), free(pointee), free(vtype);
    free(cast), free(newptr), free(binding);
    static int told;
    if (told++ < 3)
        rt_log("gfx_shader: material arrays widened to %u elements: %d variables, %d index clamps", N, vars, clamps);
    return 1;
bad:
    free(is_arr_var), free(is_const), free(rewrite), free(val), free(ctype), free(arr_len), free(pointee), free(vtype);
    free(cast), free(newptr), free(binding);
    return 0;
}

// Direct pool arrays through the global bindless set (KAR_BINDLESS, RELEASE_PLAN 2.6): the translator indexes a pool
// array with UMin(id, 31) and pool_patch hashes the id into 32 elements, so two textures of one draw whose ids hash
// alike share an element and one of them reads the other (the main menu's speech bubble and the title's prompt: the
// text's second texture read the glyph atlas, alpha 0, every fragment discarded). Here each access chain into a pool
// array of 2D, 2D array or cube float images or of samplers indexes the global set's array by the whole id instead
// (vk_bindless.c: images at bindings 0-2 by texture id, samplers at binding 3 by sampler id), and the UMin becomes a
// copy of the id. GfxShaderRes.pool_bl tells the layout to add set 2 and the replay to make the ids resident.
static int pool_bindless_patch(GfxShader *s) {
    enum { OP_EXTENSION_ = 10, OP_CAPABILITY_ = 17, OP_TYPE_INT_ = 21, OP_TYPE_FLOAT_ = 22, OP_TYPE_IMAGE_ = 25,
           OP_TYPE_SAMPLER_ = 26, OP_TYPE_ARRAY_ = 28, OP_TYPE_POINTER_ = 32, OP_CONST = 43, OP_FUNCTION_ = 54,
           OP_VAR = 59, OP_LOAD_ = 61, OP_ACCESS_CHAIN = 65, OP_DECOR = 71, OP_COPY_OBJECT_ = 83, OP_BITCAST = 124,
           OP_EXT_INST = 12, DEC_BINDING = 33, DEC_SET = 34, DEC_NONUNIFORM = 5300, CAP_NONUNIFORM = 5301,
           CAP_SAMPLED_NU = 5307, SC_UNIFORM_CONSTANT = 0, GLSL_UMIN = 38 };
    static int on = -1;
    if (on < 0) on = !(getenv("KAR_BINDLESS") && *getenv("KAR_BINDLESS") == '0') &&
                     getenv("KAR_POOL_BL") && *getenv("KAR_POOL_BL") == '1';   // opt-in until tested in the game
    if (!on) return 0;
    int any = 0;
    for (int k = 0; k < s->nres[GFX_RES_TEX]; k++) any |= s->res[GFX_RES_TEX][k].pool_arr;
    if (!any) return 0;
    const uint32_t *w = s->spirv;
    uint64_t n = s->words;
    uint32_t bound = w[3];
    if (n < 5 || bound > (1u << 22)) return 0;
    uint32_t *binding = calloc(bound, 4), *vtype = calloc(bound, 4), *ptr_to = calloc(bound, 4), *ptr_sc = calloc(bound, 4),
             *arr_elem = calloc(bound, 4), *cast = calloc(bound, 4), *umin_x = calloc(bound, 4), *gvar_of = calloc(bound, 4),
             *chain_of = calloc(bound, 4);
    uint8_t *kind_of = calloc(bound, 1), *img_kind = calloc(bound, 1), *is_31 = calloc(bound, 1), *nu = calloc(bound, 1),
            *is_umin = calloc(bound, 1);
    if (!binding || !vtype || !ptr_to || !ptr_sc || !arr_elem || !cast || !umin_x || !gvar_of || !chain_of || !kind_of ||
        !img_kind || !is_31 || !nu || !is_umin)
        rt_fatal("gfx_shader: out of memory patching a shader");
    int moved = 0, have_nu = 0, have_snu = 0;
    uint32_t t_uint = 0, t_sampler = 0, t_float = 0, nb = bound;
    uint64_t first_func = 0, first_decor = 0, last_cap = 0;
    for (uint64_t i = 5; i < n;) {   // pass 1: types, decorations, variables, constants, casts, UMin(x, 31)
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || i + len > n) goto out;
        if (op == OP_CAPABILITY_) { last_cap = i + len; have_nu |= w[i + 1] == CAP_NONUNIFORM; have_snu |= w[i + 1] == CAP_SAMPLED_NU; }
        if (op == OP_DECOR && !first_decor) first_decor = i;
        if (op == OP_FUNCTION_ && !first_func) first_func = i;
        if (op == OP_DECOR && len == 4 && w[i + 2] == DEC_BINDING && w[i + 1] < bound) binding[w[i + 1]] = w[i + 3] + 1;
        else if (op == OP_TYPE_INT_ && len == 4 && w[i + 2] == 32 && !w[i + 3]) t_uint = w[i + 1];
        else if (op == OP_TYPE_FLOAT_ && len == 3 && w[i + 2] == 32) t_float = w[i + 1];
        else if (op == OP_TYPE_SAMPLER_ && len == 2 && w[i + 1] < bound) t_sampler = w[i + 1], img_kind[w[i + 1]] = 4;
        else if (op == OP_TYPE_IMAGE_ && len >= 9 && w[i + 1] < bound && w[i + 2] == t_float && w[i + 6] == 0 &&
                 w[i + 7] == 1)   // float, single sample, sampled: 2D 1, 2D array 2, cube 3
            img_kind[w[i + 1]] = w[i + 3] == 1 ? (w[i + 5] ? 2 : 1) : w[i + 3] == 3 && !w[i + 5] ? 3 : 0;
        else if (op == OP_TYPE_ARRAY_ && len == 4 && w[i + 1] < bound) arr_elem[w[i + 1]] = w[i + 2];
        else if (op == OP_TYPE_POINTER_ && len == 4 && w[i + 1] < bound) ptr_sc[w[i + 1]] = w[i + 2], ptr_to[w[i + 1]] = w[i + 3];
        else if (op == OP_VAR && len >= 4 && w[i + 2] < bound) vtype[w[i + 2]] = w[i + 1];
        else if (op == OP_CONST && len == 4 && w[i + 2] < bound && w[i + 3] == 31) is_31[w[i + 2]] = 1;
        else if (op == OP_BITCAST && len == 4 && w[i + 2] < bound) cast[w[i + 2]] = w[i + 3];
        else if (op == OP_EXT_INST && len == 7 && w[i + 4] == GLSL_UMIN && w[i + 2] < bound && w[i + 6] < bound &&
                 is_31[w[i + 6]])
            umin_x[w[i + 2]] = w[i + 5], is_umin[w[i + 2]] = 1;
        i += len;
    }
    if (!t_uint || !first_func || !first_decor || !last_cap) goto out;
    // The pool array variables: binding of a pool_arr resource, an array of a supported image type or of samplers.
    uint32_t c_nimg = nb++, c_nsamp = nb++, gv[5] = {0}, ga[5] = {0}, gp[5] = {0}, gelem[5] = {0};
    for (uint32_t v = 0; v < bound; v++) {
        if (!binding[v] || !vtype[v] || vtype[v] >= bound || ptr_sc[vtype[v]] != SC_UNIFORM_CONSTANT) continue;
        uint32_t at = ptr_to[vtype[v]], el = at && at < bound ? arr_elem[at] : 0;
        if (!el || el >= bound || !img_kind[el]) continue;
        for (int k = 0; k < s->nres[GFX_RES_TEX]; k++) {
            const GfxShaderRes *r = &s->res[GFX_RES_TEX][k];
            if (!r->pool_arr || r->binding != binding[v] - 1) continue;
            uint32_t kd = img_kind[el];   // 1..3 images, 4 samplers
            if (gelem[kd] && gelem[kd] != el) break;   // two element types of one kind: left as it is
            if (!gv[kd]) gelem[kd] = el, ga[kd] = nb++, gp[kd] = nb++, gv[kd] = nb++;
            kind_of[v] = (uint8_t)kd, gvar_of[v] = gv[kd];
            break;
        }
    }
    for (uint64_t i = first_func; i < n;) {   // pass 2: access chains into those arrays indexed by a cast UMin
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == OP_ACCESS_CHAIN && len == 5 && w[i + 3] < bound && kind_of[w[i + 3]] && w[i + 4] < bound &&
            w[i + 2] < bound) {
            uint32_t idx = w[i + 4], u = cast[idx] < bound && is_umin[cast[idx]] ? cast[idx] : is_umin[idx] ? idx : 0;
            if (!u) kind_of[w[i + 3]] = 0;   // another index form: this array stays a pool array
            else chain_of[w[i + 2]] = w[i + 3], umin_x[w[i + 2]] = u, nu[w[i + 2]] = 1;   // umin_x: the chain's UMin
        } else if (op == OP_LOAD_ && len >= 4 && w[i + 3] < bound && chain_of[w[i + 3]] && w[i + 2] < bound) {
            nu[w[i + 2]] = 1;
        }
        i += len;
    }
    int nchain = 0;
    // Only the UMins of chains into arrays that move become copies; is_umin keeps that mark from here on.
    uint8_t *conv = calloc(bound, 1);
    if (!conv) rt_fatal("gfx_shader: out of memory patching a shader");
    for (uint32_t x = 0; x < bound; x++)
        if (chain_of[x] && kind_of[chain_of[x]]) nchain++, conv[umin_x[x]] = 1;
    memcpy(is_umin, conv, bound);
    free(conv);
    if (!nchain) goto out;
    uint32_t *o = malloc((n + 128 + 4 * (uint64_t)bound / 64 + 8 * (uint64_t)nchain + 16) * sizeof *o);
    if (!o) rt_fatal("gfx_shader: out of memory patching a shader");
    uint64_t m = 0;
    for (int k = 0; k < 5; k++) o[m++] = w[k];
    for (uint64_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (i == first_decor) {
            for (int kd = 1; kd <= 4; kd++)
                if (gv[kd] && gelem[kd]) {
                    o[m++] = 4u << 16 | OP_DECOR, o[m++] = gv[kd], o[m++] = DEC_SET, o[m++] = 2;
                    o[m++] = 4u << 16 | OP_DECOR, o[m++] = gv[kd], o[m++] = DEC_BINDING, o[m++] = kd == 4 ? 3 : kd - 1;
                }
            for (uint32_t x = 0; x < bound; x++)
                if (nu[x] && (!chain_of[x] || kind_of[chain_of[x]]))
                    o[m++] = 3u << 16 | OP_DECOR, o[m++] = x, o[m++] = DEC_NONUNIFORM;
        }
        if (i == first_func) {
            o[m++] = 4u << 16 | OP_CONST, o[m++] = t_uint, o[m++] = c_nimg, o[m++] = 65792;
            o[m++] = 4u << 16 | OP_CONST, o[m++] = t_uint, o[m++] = c_nsamp, o[m++] = 4096;
            for (int kd = 1; kd <= 4; kd++)
                if (gv[kd]) {
                    o[m++] = 4u << 16 | OP_TYPE_ARRAY_, o[m++] = ga[kd], o[m++] = gelem[kd], o[m++] = kd == 4 ? c_nsamp : c_nimg;
                    o[m++] = 4u << 16 | OP_TYPE_POINTER_, o[m++] = gp[kd], o[m++] = SC_UNIFORM_CONSTANT, o[m++] = ga[kd];
                    o[m++] = 4u << 16 | OP_VAR, o[m++] = gp[kd], o[m++] = gv[kd], o[m++] = SC_UNIFORM_CONSTANT;
                }
        }
        if (op == OP_EXT_INST && len == 7 && w[i + 2] < bound && is_umin[w[i + 2]]) {   // UMin(x, 31) -> x
            o[m++] = 4u << 16 | OP_COPY_OBJECT_, o[m++] = w[i + 1], o[m++] = w[i + 2], o[m++] = w[i + 5];
            i += len;
            continue;
        }
        if (op == OP_ACCESS_CHAIN && len == 5 && w[i + 2] < bound && chain_of[w[i + 2]] && kind_of[chain_of[w[i + 2]]]) {
            memcpy(o + m, w + i, len * sizeof *w);
            o[m + 3] = gvar_of[chain_of[w[i + 2]]];
            m += len, i += len, moved++;
            continue;
        }
        memcpy(o + m, w + i, len * sizeof *w), m += len;
        if (i + len == last_cap) {
            if (!have_nu) o[m++] = 2u << 16 | OP_CAPABILITY_, o[m++] = CAP_NONUNIFORM;
            if (!have_snu) o[m++] = 2u << 16 | OP_CAPABILITY_, o[m++] = CAP_SAMPLED_NU;
            if (w[1] < 0x00010500 && !have_nu) {
                static const char ext[] = "SPV_EXT_descriptor_indexing";
                uint32_t words = (uint32_t)(sizeof ext + 3) / 4;
                o[m++] = (1u + words) << 16 | OP_EXTENSION_;
                memset(o + m, 0, words * 4), memcpy(o + m, ext, sizeof ext), m += words;
            }
        }
        i += len;
    }
    o[3] = nb;
    for (uint32_t v = 0; v < bound; v++)   // the pool arrays moved: the replay makes their ids resident
        if (kind_of[v])
            for (int k = 0; k < s->nres[GFX_RES_TEX]; k++) {
                GfxShaderRes *r = &s->res[GFX_RES_TEX][k];
                if (r->pool_arr && r->binding == binding[v] - 1) r->pool_bl = kind_of[v];
            }
    free(s->spirv);
    s->spirv = o, s->words = m;
    static int told;
    if (told++ < 3) rt_log("gfx_shader: Direct pool arrays read through the bindless set (%d access chains)", moved);
out:
    free(binding), free(vtype), free(ptr_to), free(ptr_sc), free(arr_elem), free(cast), free(umin_x), free(gvar_of);
    free(chain_of), free(kind_of), free(img_kind), free(is_31), free(nu), free(is_umin);
    (void)t_sampler;
    return moved;
}

// Bindless material arrays (RELEASE_PLAN 16.2, KAR_BINDLESS=1). A material array whose sampler sits in the handle
// (GfxShaderRes.samp_slot < 0) is read through the global set (vk_bindless.c, set 2) instead of the array: each
// `%p = OpAccessChain %arr %i` + `%r = OpLoad %T %p` becomes
//   h = bitcast<uint>(cbuf.data[i >> 1][(i & 1) * 2])   (element i's handle at byte 8i of its constant buffer)
//   %r = OpSampledImage %T (globalImage[h & 0xfffff]) (globalSampler[h >> 20])
// with NonUniform decorations, so the array is no longer used and its binding keeps one element. Arrays whose
// sampler comes from a separate-sampler slot, other image kinds and modules of another shape stay as they are.
// Returns the number of arrays moved.
// KAR_BL_WHY=1: counts, per material array of every shader, why bindless_patch left it in place (diagnostic).
static void bl_why(const GfxShader *s, int bail, const uint32_t *img_dim) {
    static int on = -1, why[8], nsh;
    static const char *name[8] = {"moved", "sampler-slot", "separate", "no-cbuf", "image-kind", "cbuf-shape", "module", "?"};
    if (on < 0) on = getenv("KAR_BL_WHY") && *getenv("KAR_BL_WHY") == '1';
    if (!on) return;
    for (int k = 0; k < s->nres[GFX_RES_TEX]; k++) {
        const GfxShaderRes *r = &s->res[GFX_RES_TEX][k];
        if (!r->arr_ubo) continue;
        int c = r->bindless ? 0 : r->samp_slot >= 0 ? 1 : r->separate ? 2 : r->cbuf_slot <= 0 ? 3 : bail ? bail : 7;
        if (c == 7 && img_dim) c = img_dim[r->binding & 63] ? 5 : 4;
        why[c]++;
    }
    if (++nsh % 200 == 0)
        rt_log("gfx_shader: bindless why (%d shaders): %s %d, %s %d, %s %d, %s %d, %s %d, %s %d, %s %d, %s %d", nsh,
               name[0], why[0], name[1], why[1], name[2], why[2], name[3], why[3], name[4], why[4], name[5], why[5],
               name[6], why[6], name[7], why[7]);
}

static int bindless_patch(GfxShader *s) {
    enum { OP_EXTENSION_ = 10, OP_CAPABILITY_ = 17, OP_TYPE_INT_ = 21, OP_TYPE_FLOAT_ = 22, OP_TYPE_VECTOR_ = 23,
           OP_TYPE_IMAGE_ = 25, OP_TYPE_SAMPLER_ = 26, OP_TYPE_SAMPLED_IMAGE_ = 27, OP_TYPE_ARRAY_ = 28,
           OP_TYPE_STRUCT_ = 30, OP_TYPE_POINTER_ = 32, OP_CONST = 43, OP_FUNCTION_ = 54, OP_VAR = 59, OP_LOAD_ = 61,
           OP_ACCESS_CHAIN = 65, OP_DECOR = 71, OP_SAMPLED_IMAGE_ = 86, OP_BITCAST = 124, OP_SHR = 194, OP_SHL = 196,
           OP_AND = 199, OP_EXT_INST_ = 12, OP_IADD_ = 128, OP_COPY_OBJECT_ = 83, DEC_BINDING = 33, DEC_SET = 34, DEC_NONUNIFORM = 5300, CAP_NONUNIFORM = 5301,
           CAP_SAMPLED_NU = 5307, SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SEQ = 13, OP_MEMBER_DECOR = 72, DEC_BLOCK = 2,
           DEC_ARRAY_STRIDE = 6, DEC_OFFSET = 35 };
    static int on = -1;
    if (on < 0) on = !(getenv("KAR_BINDLESS") && *getenv("KAR_BINDLESS") == '0');   // on unless KAR_BINDLESS=0
    if (!on) return 0;
    int want = 0;
    for (int k = 0; k < s->nres[GFX_RES_TEX]; k++) {
        const GfxShaderRes *r = &s->res[GFX_RES_TEX][k];
        want |= r->arr_ubo && (r->samp_slot < 0 || (!r->samp_tbl && r->samp_slot < 128)) && !r->separate && r->cbuf_slot > 0;
    }
    if (!want) { bl_why(s, 0, NULL); return 0; }
    const uint32_t *w = s->spirv;
    uint64_t n = s->words;
    uint32_t bound = w[3];
    if (n < 5 || bound > (1u << 22)) return 0;
    uint32_t *binding = calloc(bound, 4), *ptr_to = calloc(bound, 4), *ptr_sc = calloc(bound, 4), *arr_elem = calloc(bound, 4),
             *simg_img = calloc(bound, 4), *vtype = calloc(bound, 4), *struct_m0 = calloc(bound, 4), *vec_of = calloc(bound, 4),
             *chain_var = calloc(bound, 4), *chain_idx = calloc(bound, 4), *load_base = calloc(bound, 4),
             *kind_of = calloc(bound, 4), *cbvar_of = calloc(bound, 4), *gvar = calloc(bound, 4), *gptr = calloc(bound, 4),
             *rtype = calloc(bound, 4);
    uint8_t *img_ok = calloc(bound, 1), *samp_of = calloc(bound, 1), *load_samp = calloc(bound, 1),
            *used = calloc(bound, 1);
    uint32_t *one_arr = calloc(bound, 4);   // an array the shader never reads: its 1-element array type (+1 pointer)
    uint32_t (*img)[3] = calloc(bound, sizeof *img);   // dim, depth, arrayed
    if (!binding || !ptr_to || !ptr_sc || !arr_elem || !simg_img || !vtype || !struct_m0 || !vec_of || !chain_var ||
        !chain_idx || !load_base || !kind_of || !cbvar_of || !gvar || !gptr || !img_ok || !img || !rtype || !samp_of || !load_samp || !used || !one_arr)
        rt_fatal("gfx_shader: out of memory patching a shader");
    int moved = 0, nloads = 0, bail = 6;
    uint32_t img_dim[64] = {0};   // per binding (KAR_BL_WHY): 1 when the image kind qualified
    // A handle buffer the shader does not declare (it reads only the array): declared here as a uniform block
    // { vec4 data[N]; } with a new binding, and added to the uniform buffers the runtime binds (slot = cbuf_slot).
    uint32_t syn_var[32] = {0}, syn_n[32] = {0}, syn_bind[32] = {0}, t_vec4 = 0, max_bind = 0;
    uint32_t t_uint = 0, t_int = 0, t_float = 0, t_sampler = 0, nb = bound, nimg_types = 0, img_types[8];
    uint64_t first_func = 0, first_decor = 0, last_cap = 0;
    for (uint64_t i = 5; i < n;) {   // pass 1: types, decorations, variables
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || i + len > n) goto out;
        if (op == OP_CAPABILITY_) last_cap = i + len;
        if (op == OP_DECOR && !first_decor) first_decor = i;
        if (op == OP_FUNCTION_ && !first_func) first_func = i;
        if (op == OP_DECOR && len == 4 && w[i + 2] == DEC_BINDING && w[i + 1] < bound) binding[w[i + 1]] = w[i + 3] + 1;
        else if (op == OP_TYPE_INT_ && len == 4 && w[i + 2] == 32) { if (w[i + 3]) t_int = w[i + 1]; else t_uint = w[i + 1]; }
        else if (op == OP_TYPE_FLOAT_ && len == 3 && w[i + 2] == 32) t_float = w[i + 1];
        else if (op == OP_TYPE_SAMPLER_ && len == 2) t_sampler = w[i + 1];
        else if (op == OP_TYPE_VECTOR_ && len == 4 && w[i + 1] < bound && w[i + 3] == 4) {
            vec_of[w[i + 1]] = w[i + 2];
            if (!t_vec4 && t_float && w[i + 2] == t_float) t_vec4 = w[i + 1];
        }
        else if (op == OP_TYPE_IMAGE_ && len >= 9 && w[i + 1] < bound) {
            img[w[i + 1]][0] = w[i + 3], img[w[i + 1]][1] = w[i + 4], img[w[i + 1]][2] = w[i + 5];
            img_ok[w[i + 1]] = w[i + 2] == t_float && w[i + 6] == 0 && w[i + 7] == 1;   // float, single sample, sampled
        } else if (op == OP_TYPE_SAMPLED_IMAGE_ && len == 3 && w[i + 1] < bound) simg_img[w[i + 1]] = w[i + 2];
        else if (op == OP_TYPE_ARRAY_ && len == 4 && w[i + 1] < bound) arr_elem[w[i + 1]] = w[i + 2];
        else if (op == OP_TYPE_STRUCT_ && len >= 3 && w[i + 1] < bound) struct_m0[w[i + 1]] = w[i + 2];
        else if (op == OP_TYPE_POINTER_ && len == 4 && w[i + 1] < bound) ptr_sc[w[i + 1]] = w[i + 2], ptr_to[w[i + 1]] = w[i + 3];
        else if (op == OP_VAR && len >= 4 && w[i + 2] < bound) vtype[w[i + 2]] = w[i + 1];
        if ((op == OP_BITCAST || op == OP_EXT_INST_ || (op >= OP_SHR && op <= OP_AND) || op == OP_IADD_) && len >= 3 &&
            w[i + 2] < bound)
            rtype[w[i + 2]] = w[i + 1];   // result types of the instructions an index comes from
        i += len;
    }
    if (!t_uint || !t_int || !t_float || !first_func || !first_decor || !last_cap) goto out;
    // The arrays to move: binding of a qualifying resource, element a 2D / 2D array / cube float image, its constant
    // buffer declared as a struct whose first member is an array of float vec4.
    uint32_t c_nimg = nb++, c_nsamp = nb++, c_mask = nb++, c_20 = nb++, c_i0 = nb++, c_i1 = nb++, p_ufloat = nb++;
    uint32_t t_samp = t_sampler ? t_sampler : nb++, a_samp = nb++, pa_samp = nb++, p_samp = nb++, g_samp = nb++;
    for (uint32_t v = 0; v < bound; v++) {
        if (!binding[v] || !vtype[v] || vtype[v] >= bound || ptr_sc[vtype[v]] != SC_UNIFORM_CONSTANT) continue;
        uint32_t at = ptr_to[vtype[v]], st = at < bound ? arr_elem[at] : 0, it = st && st < bound ? simg_img[st] : 0;
        if (!it || it >= bound || !img_ok[it]) continue;
        uint32_t kind = img[it][0] == 1 ? (img[it][2] ? 2 : 1) : img[it][0] == 3 && !img[it][2] ? 3 : 0;   // 2D, 2D array, cube
        if (!kind) continue;
        img_dim[(binding[v] - 1) & 63] = 1;
        for (int k = 0; k < s->nres[GFX_RES_TEX]; k++) {
            GfxShaderRes *r = &s->res[GFX_RES_TEX][k];
            if (!r->arr_ubo || r->separate || r->binding != binding[v] - 1 || r->cbuf_slot <= 0) continue;
            if (r->samp_slot >= 0 && (r->samp_tbl || r->samp_slot >= 128)) continue;
            uint32_t cb = 0;   // the variable of constant buffer cbuf_slot
            for (int u = 0; u < s->nres[GFX_RES_UBO] && !cb; u++)
                if (s->res[GFX_RES_UBO][u].slot == r->cbuf_slot)
                    for (uint32_t x = 0; x < bound && !cb; x++)
                        if (binding[x] == (uint32_t)s->res[GFX_RES_UBO][u].binding + 1 && vtype[x] && vtype[x] < bound &&
                            ptr_sc[vtype[x]] == SC_UNIFORM)
                            cb = x;
            if (!cb && t_vec4 && r->cbuf_slot < 32) {
                uint32_t sl = (uint32_t)r->cbuf_slot, need = ((r->count ? r->count : 32) + 1) / 2;
                if (!max_bind)
                    for (int kk = 0; kk < GFX_RES_KINDS; kk++)
                        for (int q = 0; q < s->nres[kk]; q++)
                            if (s->res[kk][q].binding + 1u > max_bind) max_bind = s->res[kk][q].binding + 1u;
                if (!syn_var[sl]) syn_var[sl] = nb + 4, nb += 5, syn_bind[sl] = max_bind++;   // const N, array, struct, pointer, variable
                if (need > syn_n[sl]) syn_n[sl] = need;
                cb = syn_var[sl];
            } else if (!cb) break;
            if (cb < bound) {
                uint32_t sty = ptr_to[vtype[cb]], m0 = sty < bound ? struct_m0[sty] : 0, ve = m0 && m0 < bound ? arr_elem[m0] : 0;
                if (!ve || ve >= bound || vec_of[ve] != t_float) break;
            }
            int have = 0;
            for (uint32_t q = 0; q < nimg_types; q++) have |= img_types[q] == it;
            if (!have) {
                if (nimg_types == 8) break;
                img_types[nimg_types++] = it, gvar[it] = nb, nb += 3, gptr[it] = nb++;   // array, its pointer, variable; element pointer
            }
            kind_of[v] = kind, cbvar_of[v] = cb, samp_of[v] = r->samp_slot >= 0;
            break;
        }
    }
    for (uint64_t i = first_func; i < n;) {   // pass 2: access chains into moved arrays and their loads
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == OP_ACCESS_CHAIN && len == 5 && w[i + 3] < bound && kind_of[w[i + 3]] && w[i + 2] < bound)
            chain_var[w[i + 2]] = w[i + 3], chain_idx[w[i + 2]] = w[i + 4], used[w[i + 3]] = 1;
        else if (op == OP_LOAD_ && len >= 4 && w[i + 3] < bound && chain_var[w[i + 3]] && w[i + 2] < bound)
            load_base[w[i + 2]] = nb, nb += SEQ, nloads++, load_samp[w[i + 2]] = samp_of[chain_var[w[i + 3]]];
        i += len;
    }
    bail = 0;
    uint32_t c_u1 = nb++, nunused = 0;
    for (uint32_t v = 0; v < bound; v++)
        if (kind_of[v] && !used[v]) {   // a qualifying array no instruction reads through: shrunk to one element
            for (uint64_t i = first_func; i < n && !used[v];) {
                uint32_t len = w[i] >> 16;
                for (uint32_t a = 1; a < len; a++) used[v] |= w[i + a] == v;
                i += len;
            }
            if (!used[v]) one_arr[v] = nb, nb += 2, nunused++, samp_of[v] = 0;
        }
    if (!nloads && nunused) goto rewrite;
    if (!nloads) {
        static int told_nl;
        if (getenv("KAR_BL_WHY") && told_nl++ < 4) {
            rt_log("gfx_shader: bindless: no loads through a qualifying array");
            for (int k = 0; k < s->nres[GFX_RES_TEX]; k++) {
                const GfxShaderRes *r = &s->res[GFX_RES_TEX][k];
                int ub = 0;
                for (int u = 0; u < s->nres[GFX_RES_UBO]; u++) ub |= s->res[GFX_RES_UBO][u].slot == r->cbuf_slot;
                if (r->arr_ubo)
                    rt_log("gfx_shader: bindless:   res binding %d samp_slot %d tbl %d cbuf %d ubo %d stage %d", r->binding,
                           r->samp_slot, r->samp_tbl, r->cbuf_slot, ub, (int)s->stage);
            }
            for (uint64_t i = first_func; i < n;) {
                uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
                for (uint32_t a = 1; a < len; a++)
                    if (w[i + a] < bound && kind_of[w[i + a]]) rt_log("gfx_shader: bindless:   array used by op %u len %u at %u", op, len, a);
                i += len;
            }
        }
        bail = 6;
        goto out;
    }
    // Every chain into a moved array must feed a load rewritten here (any other use would keep the array in use).
    for (uint64_t i = first_func; i < n;) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op != OP_ACCESS_CHAIN)
            for (uint32_t a = 1; a < len; a++)
                if (w[i + a] < bound && chain_var[w[i + a]] && !(op == OP_LOAD_ && a == 3)) {
                    static int told_use;
                    if (getenv("KAR_BL_WHY") && told_use++ < 12) rt_log("gfx_shader: bindless: chain used by op %u len %u at operand %u", op, len, a);
                    bail = 6;
                    goto out;
                }
        i += len;
    }
rewrite:;
    uint32_t *o = malloc((n + 96 + 24 * (uint64_t)nimg_types + (uint64_t)nloads * 80 + 12 * (uint64_t)nunused + 32 * 40) * sizeof *o);
    if (!o) rt_fatal("gfx_shader: out of memory patching a shader");
    uint64_t m = 0;
    for (int k = 0; k < 5; k++) o[m++] = w[k];
    for (uint64_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (i == first_decor) {   // the global variables' set and binding, NonUniform on every per-use value
            for (uint32_t q = 0; q < nimg_types; q++) {
                uint32_t it = img_types[q], v = gvar[it] + 2, kind = img[it][0] == 3 ? 2 : img[it][2] ? 1 : 0;
                o[m++] = 4u << 16 | OP_DECOR, o[m++] = v, o[m++] = DEC_SET, o[m++] = 2;
                o[m++] = 4u << 16 | OP_DECOR, o[m++] = v, o[m++] = DEC_BINDING, o[m++] = kind;
            }
            for (uint32_t sl = 0; sl < 32; sl++)
                if (syn_var[sl]) {
                    uint32_t v = syn_var[sl], arr = v - 3, st = v - 2;
                    o[m++] = 3u << 16 | OP_DECOR, o[m++] = st, o[m++] = DEC_BLOCK;
                    o[m++] = 5u << 16 | OP_MEMBER_DECOR, o[m++] = st, o[m++] = 0, o[m++] = DEC_OFFSET, o[m++] = 0;
                    o[m++] = 4u << 16 | OP_DECOR, o[m++] = arr, o[m++] = DEC_ARRAY_STRIDE, o[m++] = 16;
                    o[m++] = 4u << 16 | OP_DECOR, o[m++] = v, o[m++] = DEC_SET, o[m++] = 0;
                    o[m++] = 4u << 16 | OP_DECOR, o[m++] = v, o[m++] = DEC_BINDING, o[m++] = syn_bind[sl];
                }
            o[m++] = 4u << 16 | OP_DECOR, o[m++] = g_samp, o[m++] = DEC_SET, o[m++] = 2;
            o[m++] = 4u << 16 | OP_DECOR, o[m++] = g_samp, o[m++] = DEC_BINDING, o[m++] = 3;
            for (uint32_t x = 0; x < bound; x++)
                if (load_base[x]) {
                    for (uint32_t d = 6; d <= 11; d++)   // a sampler from the binding: no sid, chain or global load
                        if (!load_samp[x] || d == 6 || d == 8 || d == 9)
                            o[m++] = 3u << 16 | OP_DECOR, o[m++] = load_base[x] + d, o[m++] = DEC_NONUNIFORM;
                    o[m++] = 3u << 16 | OP_DECOR, o[m++] = x, o[m++] = DEC_NONUNIFORM;
                }
        }
        if (i == first_func) {   // types, constants and variables of the global set
            o[m++] = 4u << 16 | OP_CONST, o[m++] = t_uint, o[m++] = c_nimg, o[m++] = 65792;
            o[m++] = 4u << 16 | OP_CONST, o[m++] = t_uint, o[m++] = c_nsamp, o[m++] = 4096;
            o[m++] = 4u << 16 | OP_CONST, o[m++] = t_uint, o[m++] = c_mask, o[m++] = 0xfffff;
            o[m++] = 4u << 16 | OP_CONST, o[m++] = t_uint, o[m++] = c_20, o[m++] = 20;
            o[m++] = 4u << 16 | OP_CONST, o[m++] = t_int, o[m++] = c_i0, o[m++] = 0;
            o[m++] = 4u << 16 | OP_CONST, o[m++] = t_int, o[m++] = c_i1, o[m++] = 1;
            o[m++] = 4u << 16 | OP_TYPE_POINTER_, o[m++] = p_ufloat, o[m++] = SC_UNIFORM, o[m++] = t_float;
            if (!t_sampler) o[m++] = 2u << 16 | OP_TYPE_SAMPLER_, o[m++] = t_samp;
            o[m++] = 4u << 16 | OP_TYPE_ARRAY_, o[m++] = a_samp, o[m++] = t_samp, o[m++] = c_nsamp;
            o[m++] = 4u << 16 | OP_TYPE_POINTER_, o[m++] = pa_samp, o[m++] = SC_UNIFORM_CONSTANT, o[m++] = a_samp;
            o[m++] = 4u << 16 | OP_TYPE_POINTER_, o[m++] = p_samp, o[m++] = SC_UNIFORM_CONSTANT, o[m++] = t_samp;
            o[m++] = 4u << 16 | OP_VAR, o[m++] = pa_samp, o[m++] = g_samp, o[m++] = SC_UNIFORM_CONSTANT;
            o[m++] = 4u << 16 | OP_CONST, o[m++] = t_uint, o[m++] = c_u1, o[m++] = 1;
            for (uint32_t sl = 0; sl < 32; sl++)
                if (syn_var[sl]) {
                    uint32_t v = syn_var[sl], cn = v - 4, arr = v - 3, st = v - 2, pt = v - 1;
                    o[m++] = 4u << 16 | OP_CONST, o[m++] = t_uint, o[m++] = cn, o[m++] = syn_n[sl];
                    o[m++] = 4u << 16 | OP_TYPE_ARRAY_, o[m++] = arr, o[m++] = t_vec4, o[m++] = cn;
                    o[m++] = 3u << 16 | OP_TYPE_STRUCT_, o[m++] = st, o[m++] = arr;
                    o[m++] = 4u << 16 | OP_TYPE_POINTER_, o[m++] = pt, o[m++] = SC_UNIFORM, o[m++] = st;
                    o[m++] = 4u << 16 | OP_VAR, o[m++] = pt, o[m++] = v, o[m++] = SC_UNIFORM;
                }
            for (uint32_t x = 0; x < bound; x++)   // an array never read: one element of its sampled-image type
                if (one_arr[x]) {
                    uint32_t ste = arr_elem[ptr_to[vtype[x]]];
                    o[m++] = 4u << 16 | OP_TYPE_ARRAY_, o[m++] = one_arr[x], o[m++] = ste, o[m++] = c_u1;
                    o[m++] = 4u << 16 | OP_TYPE_POINTER_, o[m++] = one_arr[x] + 1, o[m++] = SC_UNIFORM_CONSTANT, o[m++] = one_arr[x];
                    o[m++] = 4u << 16 | OP_VAR, o[m++] = one_arr[x] + 1, o[m++] = x, o[m++] = SC_UNIFORM_CONSTANT;
                }
            for (uint32_t x = 0; x < bound; x++)   // an array with its sampler in a bound slot: now that one sampler
                if (kind_of[x] && samp_of[x]) o[m++] = 4u << 16 | OP_VAR, o[m++] = p_samp, o[m++] = x, o[m++] = SC_UNIFORM_CONSTANT;
            for (uint32_t q = 0; q < nimg_types; q++) {
                uint32_t it = img_types[q], a = gvar[it], pa = a + 1, v = a + 2;
                o[m++] = 4u << 16 | OP_TYPE_ARRAY_, o[m++] = a, o[m++] = it, o[m++] = c_nimg;
                o[m++] = 4u << 16 | OP_TYPE_POINTER_, o[m++] = pa, o[m++] = SC_UNIFORM_CONSTANT, o[m++] = a;
                o[m++] = 4u << 16 | OP_TYPE_POINTER_, o[m++] = gptr[it], o[m++] = SC_UNIFORM_CONSTANT, o[m++] = it;
                o[m++] = 4u << 16 | OP_VAR, o[m++] = pa, o[m++] = v, o[m++] = SC_UNIFORM_CONSTANT;
            }
        }
        if (op == OP_ACCESS_CHAIN && len == 5 && w[i + 2] < bound && chain_var[w[i + 2]]) { i += len; continue; }
        if (op == OP_VAR && len >= 4 && w[i + 2] < bound && kind_of[w[i + 2]] && (samp_of[w[i + 2]] || one_arr[w[i + 2]])) {
            i += len;
            continue;
        }
        if (op == OP_LOAD_ && len >= 4 && w[i + 2] < bound && load_base[w[i + 2]]) {
            uint32_t res = w[i + 2], st = w[i + 1], p = w[i + 3], v = chain_var[p], idx = chain_idx[p], b = load_base[res];
            uint32_t it = simg_img[arr_elem[ptr_to[vtype[v]]]], cb = cbvar_of[v];
            uint32_t vi = b, bit = b + 1, ci = b + 2, pf = b + 3, hf = b + 4, h = b + 5, tid = b + 6, sid = b + 7, pi = b + 8,
                     im = b + 9, ps = b + 10, sm = b + 11, iidx = b + 12;
            o[m++] = 4u << 16 | (rtype[idx] == t_int ? OP_COPY_OBJECT_ : OP_BITCAST), o[m++] = t_int, o[m++] = iidx, o[m++] = idx;
            o[m++] = 5u << 16 | OP_SHR, o[m++] = t_int, o[m++] = vi, o[m++] = iidx, o[m++] = c_i1;
            o[m++] = 5u << 16 | OP_AND, o[m++] = t_int, o[m++] = bit, o[m++] = iidx, o[m++] = c_i1;
            o[m++] = 5u << 16 | OP_SHL, o[m++] = t_int, o[m++] = ci, o[m++] = bit, o[m++] = c_i1;
            o[m++] = 7u << 16 | OP_ACCESS_CHAIN, o[m++] = p_ufloat, o[m++] = pf, o[m++] = cb, o[m++] = c_i0, o[m++] = vi, o[m++] = ci;
            o[m++] = 4u << 16 | OP_LOAD_, o[m++] = t_float, o[m++] = hf, o[m++] = pf;
            o[m++] = 4u << 16 | OP_BITCAST, o[m++] = t_uint, o[m++] = h, o[m++] = hf;
            o[m++] = 5u << 16 | OP_AND, o[m++] = t_uint, o[m++] = tid, o[m++] = h, o[m++] = c_mask;
            if (!samp_of[v]) o[m++] = 5u << 16 | OP_SHR, o[m++] = t_uint, o[m++] = sid, o[m++] = h, o[m++] = c_20;
            o[m++] = 5u << 16 | OP_ACCESS_CHAIN, o[m++] = gptr[it], o[m++] = pi, o[m++] = gvar[it] + 2, o[m++] = tid;
            o[m++] = 4u << 16 | OP_LOAD_, o[m++] = it, o[m++] = im, o[m++] = pi;
            if (samp_of[v]) o[m++] = 4u << 16 | OP_LOAD_, o[m++] = t_samp, o[m++] = sm, o[m++] = v;
            else {
                o[m++] = 5u << 16 | OP_ACCESS_CHAIN, o[m++] = p_samp, o[m++] = ps, o[m++] = g_samp, o[m++] = sid;
                o[m++] = 4u << 16 | OP_LOAD_, o[m++] = t_samp, o[m++] = sm, o[m++] = ps;
            }
            o[m++] = 5u << 16 | OP_SAMPLED_IMAGE_, o[m++] = st, o[m++] = res, o[m++] = im, o[m++] = sm;
            i += len;
            continue;
        }
        memcpy(o + m, w + i, len * sizeof *w), m += len;
        if (i + len == last_cap) {   // the capabilities, and the extension descriptor indexing needs before SPIR-V 1.5
            o[m++] = 2u << 16 | OP_CAPABILITY_, o[m++] = CAP_NONUNIFORM;
            o[m++] = 2u << 16 | OP_CAPABILITY_, o[m++] = CAP_SAMPLED_NU;
            if (w[1] < 0x00010500) {
                static const char ext[] = "SPV_EXT_descriptor_indexing";
                uint32_t words = (uint32_t)(sizeof ext + 3) / 4;
                o[m++] = (1u + words) << 16 | OP_EXTENSION_;
                memset(o + m, 0, words * 4), memcpy(o + m, ext, sizeof ext), m += words;
            }
        }
        i += len;
    }
    o[3] = nb;
    for (uint32_t v = 0; v < bound; v++)   // the moved arrays' resources: one unused element, read through the set
        if (kind_of[v])
            for (int k = 0; k < s->nres[GFX_RES_TEX]; k++) {
                GfxShaderRes *r = &s->res[GFX_RES_TEX][k];
                if (r->arr_ubo && r->binding == binding[v] - 1) {
                    r->bindless = (uint8_t)kind_of[v], r->bl_samp = samp_of[v], r->bl_n = one_arr[v] ? 0 : r->count ? r->count : 32;
                    r->count = 1, moved++;
                    break;
                }
            }
    free(s->spirv);
    s->spirv = o, s->words = m;
    for (uint32_t sl = 0; sl < 32; sl++)   // the handle buffers declared here: bound like the shader's own
        if (syn_var[sl]) {
            GfxShaderRes *nr = realloc(s->res[GFX_RES_UBO], (s->nres[GFX_RES_UBO] + 1) * sizeof *nr);
            if (!nr) rt_fatal("gfx_shader: out of memory patching a shader");
            s->res[GFX_RES_UBO] = nr, nr += s->nres[GFX_RES_UBO]++;
            memset(nr, 0, sizeof *nr);
            nr->set = 0, nr->binding = (uint16_t)syn_bind[sl], nr->slot = (int16_t)sl, nr->count = 1, nr->cbuf_slot = -1;
        }
    static int told;
    if (told++ < 3) rt_log("gfx_shader: bindless: %d material arrays moved to the global set (%d loads)", moved, nloads);
out:
    bl_why(s, bail, img_dim);
    free(binding), free(ptr_to), free(ptr_sc), free(arr_elem), free(simg_img), free(vtype), free(struct_m0), free(vec_of);
    free(chain_var), free(chain_idx), free(load_base), free(kind_of), free(cbvar_of), free(gvar), free(gptr), free(img_ok);
    free(img), free(rtype), free(samp_of), free(load_samp), free(used), free(one_arr);
    return moved;
}

// SPIR-V opcodes and enumerants used below (SPIR-V 1.6 specification, section 3).
enum { OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
       OP_TYPE_ARRAY = 28, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_CONSTANT = 43, OP_VARIABLE = 59, OP_DECORATE = 71,
       OP_MEMBER_DECORATE = 72,
       DEC_BUILTIN = 11, DEC_LOCATION = 30, DEC_COMPONENT = 31, SC_INPUT = 1, SC_OUTPUT = 3, EM_LOCAL_SIZE = 17 };

// The service emits one ordinary GS main with four execution modes. Validate those against JSON on every disk
// load, including private/by-IID loads which have no original request. This is protocol validation, not spirv-val.
static int geometry_spirv(const GfxShader *s) {
    const uint32_t *w = s->spirv;
    uint32_t entry = 0, modes = 0, capability = 0;
    int gs = s->stage == GFX_STAGE_GS;
    if (s->words < 5) return !gs;
    for (uint64_t i = 5; i < s->words;) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || len > s->words - i) return 0;
        const uint32_t *a = w + i + 1;
        if (op == 17 && len >= 2) {                      // OpCapability
            if (a[0] == 5251) return 0;                  // GeometryShaderPassthroughNV
            if (a[0] == 2) capability = 1;               // Geometry
        } else if (op == 10 && len >= 2) {               // OpExtension
            static const char pass[] = "SPV_NV_geometry_shader_passthrough";
            if ((len - 1) * 4 >= sizeof pass && !memcmp(a, pass, sizeof pass)) return 0;
        } else if ((op == 71 && len >= 3 && a[1] == 5250) ||
                   (op == 72 && len >= 4 && a[2] == 5250)) return 0;   // PassthroughNV decoration/member decoration
        else if (op == OP_ENTRY_POINT) {
            if (len < 4 || (!gs && a[0] == 3)) return 0;
            if (gs) {
                const char *name = (const char *)(a + 2);
                if (entry || a[0] != 3 || !a[1] || a[1] >= w[3] ||
                    (len - 3) * 4 < 5 || memcmp(name, "main", 5)) return 0;
                entry = a[1];
            }
        } else if (gs && op == OP_EXECUTION_MODE) {
            if (len < 3 || !entry || a[0] != entry) return 0;
            uint32_t bit = 0;
            if (a[1] >= 19 && a[1] <= 23) {
                bit = 1;
                if (len != 3 || a[1] != 18 + s->geometry.input) return 0;
            } else if (a[1] == 0) {                       // Invocations
                bit = 2;
                if (len != 4 || a[2] != s->geometry.invocations) return 0;
            } else if (a[1] == 26) {                      // OutputVertices
                bit = 4;
                if (len != 4 || a[2] != s->geometry.max_output_vertices) return 0;
            } else if (a[1] >= 27 && a[1] <= 29) {
                bit = 8;
                if (len != 3 || a[1] != 26 + s->geometry.output) return 0;
            } else return 0;
            if (modes & bit) return 0;
            modes |= bit;
        }
        i += len;
    }
    return !gs || (entry && capability && modes == 15);
}

// Interface Locations and the compute workgroup size from the module. Ids beyond 2^20 are ignored (Ryujinx modules stay
// far below).
static void reflect_spirv(GfxShader *s) {
    const uint32_t *w = s->spirv;
    uint64_t n = s->words;
    s->local[0] = s->local[1] = s->local[2] = 1;
    if (n < 5) return;
    uint32_t bound = w[3] < (1u << 20) ? w[3] : (1u << 20);
    // per id: location + 1 (0 = none), component flag, scalar/vector description of types, pointee of pointers
    int32_t *loc = calloc(bound, sizeof *loc);
    uint8_t *packed = calloc(bound, 1), *ttype = calloc(bound, 1), *tcomps = calloc(bound, 1);
    uint8_t *width = calloc(bound, 1), *span = calloc(bound, 1), *length = calloc(bound, 1);
    uint8_t *array_element_span = calloc(bound, 1);
    uint32_t *pointee = calloc(bound, sizeof *pointee);
    int portable = s->geometry_source_passthrough;
    uint8_t *posdesc = portable ? calloc(bound, 1) : NULL, *poshas = portable ? calloc(bound, 1) : NULL;
    uint8_t *posvar = portable ? calloc(bound, 1) : NULL;
    uint32_t *posmember = portable ? calloc(bound, sizeof *posmember) : NULL;
    s->geometry_position = 0;
    if (!loc || !packed || !ttype || !tcomps || !width || !span || !length || !array_element_span || !pointee) goto out;
    if (portable && (!posdesc || !poshas || !posvar || !posmember)) goto out;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        const uint32_t *a = w + i + 1;
        uint32_t id = len > 1 ? a[0] : 0;
        if (op == OP_EXECUTION_MODE && len >= 6 && a[1] == EM_LOCAL_SIZE) {
            s->local[0] = a[2], s->local[1] = a[3], s->local[2] = a[4];
        } else if (op == OP_DECORATE && len >= 4 && id < bound) {
            if (a[1] == DEC_LOCATION) loc[id] = a[2] < 32 ? (int32_t)a[2] + 1 : 0;
            else if (a[1] == DEC_COMPONENT) packed[id] = 1;
            else if (portable && a[1] == DEC_BUILTIN && a[2] == 0) posvar[id] = 1;
            if (portable && a[1] == DEC_LOCATION && a[2] >= 32) s->geometry_position |= 4;
        } else if (portable && op == OP_MEMBER_DECORATE && len >= 5 && id < bound && a[2] == DEC_BUILTIN && a[3] == 0) {
            posmember[id] = !posmember[id] && a[1] < bound - 1 ? a[1] + 1 : UINT32_MAX;
        } else if (op == OP_TYPE_INT && len >= 4 && id < bound) {
            ttype[id] = a[2] ? GFX_TYPE_SINT : GFX_TYPE_UINT, tcomps[id] = 1;
            width[id] = a[1] <= UINT8_MAX ? (uint8_t)a[1] : 0, span[id] = 1;
        } else if (op == OP_TYPE_FLOAT && len >= 3 && id < bound) {
            ttype[id] = GFX_TYPE_FLOAT, tcomps[id] = 1;
            width[id] = a[1] <= UINT8_MAX ? (uint8_t)a[1] : 0, span[id] = 1;
        } else if (op == OP_TYPE_VECTOR && len >= 4 && id < bound && a[1] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = (uint8_t)a[2];
            width[id] = width[a[1]], span[id] = span[a[1]] && a[2] >= 2 && a[2] <= 4;
            if (portable && ttype[id] == GFX_TYPE_FLOAT && width[id] == 32 && a[2] == 4) posdesc[id] = 1;
        } else if (portable && op == OP_TYPE_STRUCT && id < bound && posmember[id]) {
            poshas[id] = 1;
            uint32_t member = posmember[id];
            if (member < len - 1 && a[member] < bound && posdesc[a[member]] == 1) posdesc[id] = 2;
        } else if (op == OP_CONSTANT && len >= 4 && a[0] < bound && a[1] < bound) {
            uint32_t type = a[0], value = a[2];
            if (ttype[type] != GFX_TYPE_FLOAT && tcomps[type] == 1 &&
                ((width[type] == 32 && len == 4 && value &&
                  (ttype[type] == GFX_TYPE_UINT || !(value >> 31))) ||
                 (width[type] == 64 && len == 5 && (value || a[3]) &&
                  (ttype[type] == GFX_TYPE_UINT || !(a[3] >> 31)))))
                length[a[1]] = (width[type] == 64 && a[3]) || value > 32 ? 32 : (uint8_t)value;
        } else if (op == OP_TYPE_ARRAY && len >= 4 && id < bound && a[1] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = tcomps[a[1]], width[id] = width[a[1]];
            // A scalar/vector array consumes one location per element, including nested arrays (Vulkan Shader
            // Interfaces, Location and Component Assignment). Cap each factor before multiplying: only 32 slots
            // fit our reflection. Unknown lengths and element widths GfxShaderIo cannot describe stay unsupported.
            if (width[id] == 32 && a[2] < bound) {
                array_element_span[id] = span[a[1]];
                uint32_t slots = (uint32_t)span[a[1]] * length[a[2]];
                span[id] = slots > 32 ? 32 : (uint8_t)slots;
            }
            if (portable) {
                poshas[id] = poshas[a[1]];
                if (a[2] < bound && length[a[2]] == 3 && (posdesc[a[1]] == 1 || posdesc[a[1]] == 2))
                    posdesc[id] = (uint8_t)(posdesc[a[1]] * 4);
            }
        } else if (op == OP_TYPE_POINTER && len >= 4 && id < bound) {
            pointee[id] = a[2];
        } else if (op == OP_VARIABLE && len >= 4) {
            uint32_t type = a[0], var = a[1], sc = a[2];
            if (portable && (sc == SC_INPUT || sc == SC_OUTPUT) && var < bound && type < bound) {
                uint32_t t = pointee[type] < bound ? pointee[type] : 0;
                if (posvar[var] || poshas[t]) {
                    unsigned bit = sc == SC_INPUT ? 1 : 2;
                    unsigned expected = posvar[var] ? (sc == SC_INPUT ? 4 : 1) : (sc == SC_INPUT ? 8 : 2);
                    if (posdesc[t] != expected || (s->geometry_position & bit)) s->geometry_position |= 4;
                    else s->geometry_position |= bit;
                }
            }
            if ((sc == SC_INPUT || sc == SC_OUTPUT) && var < bound && type < bound && loc[var] > 0 && loc[var] <= 32) {
                int l = loc[var] - 1;
                uint32_t t = pointee[type] < bound ? pointee[type] : 0;
                GfxShaderIo io = {ttype[t], tcomps[t] ? tcomps[t] : 4, packed[var], width[t]};
                uint32_t slots = span[t];
                if (portable && ((sc == SC_INPUT ? (slots != 3 || array_element_span[t] != 1) :
                                                  (slots != 1 || array_element_span[t] != 0)) ||
                    ((sc == SC_INPUT ? s->in_mask : s->out_mask) >> l & 1))) s->geometry_position |= 4;
                // A GS input's outer fixed array indexes vertices, not Locations. Keep its element span rather
                // than divide the saturated total: inner arrays still consume Locations. Unsupported arrays stay so.
                if (sc == SC_INPUT && s->stage == GFX_STAGE_GS && slots && array_element_span[t])
                    slots = array_element_span[t];
                for (uint32_t j = 0; j < slots && l + j < 32; j++) {
                    if (sc == SC_INPUT) s->in_mask |= 1u << (l + j), s->in[l + j] = io;
                    else s->out_mask |= 1u << (l + j), s->out[l + j] = io;
                }
            }
        }
        i += len;
    }
out:
    free(loc), free(packed), free(ttype), free(tcomps), free(width), free(span), free(length), free(array_element_span), free(pointee);
    free(posdesc), free(poshas), free(posvar), free(posmember);
}

// This ABI field is an exact JSON integer, not a numeric prefix such as 1.5 or 1e1. Missing means legacy version 0.
static int storage_contract_version(const char *js, const char *end) {
    const char *p = json_member(js, end, "storage_candidates_version");
    if (!p) return 0;
    uint32_t version;
    return json_uint(js, end, "storage_candidates_version", &version) && version <= 1 ? (int)version : -1;
}

static int reflect(GfxShader *s, int flags) {
    const int require_c0 = flags & REFLECT_REQUIRE_C0;
    static const char *const kinds[GFX_RES_KINDS] = {"cbufs", "sbufs", "textures", "images"};
    const char *js = s->reflection;
    const char *end = js + strlen(js);
    const char *whole = json_token_end(js, end, 0), *stage = json_member(js, end, "stage");
    if (!whole || json_space(whole, end) != end) return 0;
    s->stage = json_string_is(stage, end, "vs") ? GFX_STAGE_VS : json_string_is(stage, end, "ps") ? GFX_STAGE_FS :
               json_string_is(stage, end, "cs") ? GFX_STAGE_CS : json_string_is(stage, end, "gs") ? GFX_STAGE_GS : -1;
    if (s->stage < 0) return 0;
    if (s->stage == GFX_STAGE_GS) {
        if (!host_gs_sg || !geometry_json(js, end, &s->geometry, s->geometry_skip, &s->geometry_source_passthrough)) return 0;
    } else if (json_member(js, end, "geometry_version") || json_member(js, end, "gs_input") ||
               json_member(js, end, "geometry") || json_member(js, end, "gs_passthrough_skip")) return 0;
    if (!geometry_spirv(s)) return 0;
    // Driver contracts are unique root fields. Extra nested properties are metadata, not alternate layouts.
    const char *c0 = json_member(js, end, "c0"), *roots = json_member(js, end, "c0_roots"),
               *c0_version = json_member(js, end, "c0_version");
    if (c0 == end || roots == end || c0_version == end) return 0;
    if (s->stage == GFX_STAGE_GS && (require_c0 || c0 || roots || c0_version)) return 0;
    if (!c0 && (roots || c0_version)) return 0;
    if (c0) {
        uint32_t version;
        if (*c0 != '[' || !json_uint(js, end, "c0_version", &version) || version != 2 || !roots || *roots != '[') {
            rt_log("gfx_shader: NVN 2 reflection lacks root layout version 2; rebuild the shader translator");
            return 0;   // an old translator must not silently use image counts as root-table sizes
        }
        for (int a = 0; a < 2; a++) {
            char *q = (char *)(a ? roots : c0) + 1;
            uint8_t *dst = a ? s->c0.roots : &s->c0.images;
            int n = a ? NVN2_ROOT_TABLES : 4;
            for (int i = 0; i < n; i++) {
                while (isspace((unsigned char)*q)) q++;
                if (!isdigit((unsigned char)*q)) return 0;
                unsigned long v = strtoul(q, &q, 10);
                if (v > 255) return 0;
                dst[i] = (uint8_t)v;
                while (isspace((unsigned char)*q)) q++;
                if (*q++ != (i + 1 == n ? ']' : ',')) return 0;
            }
        }
        if (!nvn2_c0_valid(&s->c0, NVN2_C0_BYTES)) return 0;
        s->has_c0 = 1;
    }
    if (require_c0 && !s->has_c0) {
        rt_log("gfx_shader: translator omitted the NVN 2 root layout; rebuild the shader translator");
        return 0;
    }
    int storage_version = storage_contract_version(js, end);
    if ((require_c0 && storage_version != GFX_STORAGE_CANDIDATES_VERSION) ||
        (storage_version && (storage_version != GFX_STORAGE_CANDIDATES_VERSION ||
                             !s->has_c0 || s->stage != GFX_STAGE_CS))) {
        rt_log("gfx_shader: incompatible guarded storage contract; rebuild the shader translator");
        return 0;
    }
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        int count = json_objects(js, kinds[k], NULL, s, k);
        s->res[k] = calloc(count ? count : 1, sizeof *s->res[k]);
        json_objects(js, kinds[k], add_res, s, k);
    }
    for (int i = 0; i < s->nres[GFX_RES_SSBO]; i++)
        if ((s->res[GFX_RES_SSBO][i].flags & GFX_RES_SSBO_GUARDED) &&
            storage_version != GFX_STORAGE_CANDIDATES_VERSION) {
            rt_log("gfx_shader: guarded storage resource without its versioned binding contract");
            return 0;
        }
    reflect_spirv(s);
    pool_bindless_patch(s);   // before pool_patch: reads the UMin(id, 31) the translator left
    if (!pool_patch(s)) rt_log("gfx_shader: a Direct pool array shader did not have the expected shape; left as is");
    if (!arr_patch(s)) rt_log("gfx_shader: a material array shader did not have the expected shape; left at 32");
    arr_dims(s);
    bindless_patch(s);
    if (s->geometry_source_passthrough) {
        if (s->geometry_position != 3) return 0;
        uint32_t locations = 0;
        for (unsigned i = 0; i < 32; i++)
            if (!(s->geometry_skip[1 + i / 8] >> ((i % 8) * 4) & 15)) locations |= 1u << i;
        // The mask describes forwarded outputs, but a shader may read an input it does not forward (Bananza has one).
        // A lowered native body may also declare additional outputs; a raw passthrough shader must match them exactly.
        if (flags & REFLECT_LOWERED_BODY) {
            if ((s->in_mask & locations) != locations || (s->out_mask & locations) != locations) return 0;
        } else if ((s->in_mask & locations) != locations || s->out_mask != locations) return 0;
        for (unsigned i = 0; i < 32; i++) if (locations >> i & 1) {
            const GfxShaderIo *a = &s->in[i], *b = &s->out[i];
            if (a->scalar_bits != 32 || b->scalar_bits != 32 || a->comps != 4 || b->comps != 4 ||
                a->packed || b->packed || a->type > GFX_TYPE_UINT || a->type != b->type) return 0;
        }
    }
    return 1;
}

// ---- use masks (see the cache key at the top)

// The slots of the int array `"name": [...]` below 32 as a mask.
static uint32_t json_slot_mask(const char *js, const char *name) {
    const char *end = js + strlen(js), *p = json_key(js, end, name);
    uint32_t m = 0;
    if (!p || *p != '[') return 0;
    for (p++; p < end && *p != ']';) {
        char *q;
        long v = strtol(p, &q, 10);
        if (q == p) {
            p++;
            continue;
        }
        if (v >= 0 && v < 32) m |= 1u << v;
        p = q;
    }
    return m;
}

static UseMask use_of(const GfxShader *s) {
    UseMask m = {json_slot_mask(s->reflection, "tex_queried") | json_slot_mask(s->reflection, "img_queried"), 0, 0};
    if (s->stage == GFX_STAGE_VS) m.attrs = s->in_mask;
    else if (s->stage == GFX_STAGE_FS) m.fouts = s->out_mask & 0xff;
    return m;
}

// <sha256("TKSU", translator identity, code hash)>.use: masks are per translator.
static void use_name(const uint8_t h[32], uint8_t name[32]) {
    uint8_t buf[4 + 32 + 32];
    memcpy(buf, "TKSU", 4), memcpy(buf + 4, xlate_id, 32), memcpy(buf + 36, h, 32);
    rt_sha256(buf, sizeof buf, name);
}

static void use_path_of(const uint8_t name[32], char *p) {
    char hex[65];
    key_hex(name, hex);
    snprintf(p, PATH_CAP + 80, "%s/%s.use", cache_dir, hex);
}

static void use_path(const uint8_t h[32], char *p) {
    uint8_t name[32];
    use_name(h, name);
    use_path_of(name, p);
}

// Every .use file of the cache, read by a background thread when the paths are set (use_preload), by file name: a
// lookup on the render thread then opens no file. Opening one took 0.1 ms alone but 11-40 ms in play, behind the
// translator's and the pipeline cache's writes and flushes on the same volume, and each new shader of a draw opened one.
// The table is complete and read-only once use_files_ready is 1; masks found later are in use_buckets.
#define USE_FILE_BUCKETS 65536
typedef struct UseFile { struct UseFile *next; uint8_t name[32]; UseMask m; } UseFile;
static UseFile *use_files[USE_FILE_BUCKETS];
static volatile int32_t use_files_ready;

static UseFile **use_file_slot(const uint8_t name[32]) {
    UseFile **e = &use_files[(name[0] | name[1] << 8) % USE_FILE_BUCKETS];
    while (*e && memcmp((*e)->name, name, 32)) e = &(*e)->next;
    return e;
}

static int hex_byte(const char *c) {
    int v = 0;
    for (int i = 0; i < 2; i++) {
        int d = isdigit((unsigned char)c[i]) ? c[i] - '0' : c[i] >= 'a' && c[i] <= 'f' ? c[i] - 'a' + 10 : -1;
        if (d < 0) return -1;
        v = v << 4 | d;
    }
    return v;
}

typedef struct { int n, bad; } UsePreload;

// One <64 hex digits>.use file of the cache directory into the table.
static int use_preload_file(const char *name, void *ctx) {
    UsePreload *st = ctx;
    char p[PATH_CAP + 80];
    UseFile e = {0};
    int ok = strlen(name) == 68;
    for (int i = 0; ok && i < 32; i++) {
        int v = hex_byte(name + 2 * i);
        ok = v >= 0, e.name[i] = (uint8_t)v;
    }
    uint64_t len = 0;
    uint32_t *u = NULL;
    if (ok) {
        snprintf(p, sizeof p, "%s/%s", cache_dir, name);
        u = (uint32_t *)read_file(p, &len);
        ok = u && len == 20 && u[0] == USE_MAGIC && u[1] == USE_VERSION;
    }
    UseFile **slot = ok ? use_file_slot(e.name) : NULL, *x;
    if (slot && !*slot && (x = malloc(sizeof *x))) {
        e.m.slots = u[2], e.m.attrs = u[3], e.m.fouts = u[4];
        *x = e, *slot = x, st->n++;
    } else if (!ok) {
        st->bad++;
    }
    free(u);
    return 1;
}

static void use_preload(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    uint64_t t0 = plat_now_ns();
    int n = 0, bad = 0;
    for (uint32_t i = 0; i < pack_n; i++) {   // the packed masks; the loose files below add the masks found since
        uint32_t u[5];
        UseFile **slot, *x;
        if (pack_ix[i].kind != PACK_USE) continue;
        if (pack_ix[i].size != sizeof u || !pack_read_at(pack_ix[i].offset, u, sizeof u) || u[0] != USE_MAGIC ||
            u[1] != USE_VERSION) {
            bad++;
            continue;
        }
        if (!*(slot = use_file_slot(pack_ix[i].name)) && (x = malloc(sizeof *x))) {
            memcpy(x->name, pack_ix[i].name, 32);
            x->next = NULL, x->m.slots = u[2], x->m.attrs = u[3], x->m.fouts = u[4];
            *slot = x, n++;
        }
    }
    UsePreload st = {0};
    plat_file_list(cache_dir, ".use", use_preload_file, &st);
    n += st.n, bad += st.bad;
    plat_xchg32(&use_files_ready, 1);
    rt_log("gfx_shader: %d use masks read in %.0f ms on a background thread (%d unreadable)", n,
           (plat_now_ns() - t0) / 1e6, bad);
    return;
}

static void use_preload_start(void) {
    if (!cache_dir[0]) return;
    plat_thread_spawn(use_preload, NULL, "use preload", 0);
}

static UseEnt **use_slot(const uint8_t h[32]) {         // caller holds cache_lock
    UseEnt **e = &use_buckets[(h[0] | h[1] << 8) % BUCKETS];
    while (*e && memcmp((*e)->code_hash, h, 32)) e = &(*e)->next;
    return e;
}

// Sets or adds the entry for h. Caller holds cache_lock exclusively.
static void use_put(const uint8_t h[32], int known, const UseMask *m) {
    UseEnt **e = use_slot(h);
    if (!*e && !(*e = calloc(1, sizeof **e))) return;
    memcpy((*e)->code_hash, h, 32);
    if (known) (*e)->known = 1, (*e)->m = *m;
}

// 1 with *m when the use mask of h is known (memory, then <hash>.use; a disk miss is remembered).
static int use_find(const uint8_t h[32], UseMask *m) {
    plat_mutex_lock_shared(&cache_lock);
    UseEnt *e = *use_slot(h);
    int seen = e != NULL, known = e && e->known;
    if (known) *m = e->m;
    plat_mutex_unlock_shared(&cache_lock);
    if (seen) return known;
    uint8_t name[32];
    use_name(h, name);
    if (plat_cas32(&use_files_ready, 0, 0)) {   // complete: a name not in it has no file
        UseFile *u = *use_file_slot(name);
        if ((known = u != NULL)) *m = u->m;
    } else {
        char p[PATH_CAP + 80];
        use_path_of(name, p);
        uint64_t n = 0;
        uint32_t *f = (uint32_t *)pack_read(PACK_USE, name, &n);
        if (!f) f = (uint32_t *)read_file(p, &n);
        known = f && n == 20 && f[0] == USE_MAGIC && f[1] == USE_VERSION;
        if (known) m->slots = f[2], m->attrs = f[3], m->fouts = f[4];
        free(f);
    }
    plat_mutex_lock(&cache_lock);
    use_put(h, known, m);
    plat_mutex_unlock(&cache_lock);
    return known;
}

static void use_store(const uint8_t h[32], const UseMask *m) {
    char p[PATH_CAP + 80];
    use_path(h, p);
    uint32_t f[5] = {USE_MAGIC, USE_VERSION, m->slots, m->attrs, m->fouts};
    plat_file_write_atomic(p, f, sizeof f);
    plat_mutex_lock(&cache_lock);
    use_put(h, 1, m);
    plat_mutex_unlock(&cache_lock);
}

static void put_u32(uint8_t **p, uint32_t v) { memcpy(*p, &v, 4), *p += 4; }

// Fixed-layout answers for hashing; returns the byte count (at most 256).
static size_t pack_answers(uint8_t *o, const GfxShaderAnswers *a) {
    uint8_t *p = o;
    GfxShaderAnswers zero = {0};
    if (!a) a = &zero;
    memcpy(p, a->tex_type, 32), p += 32;
    memcpy(p, a->img_type, 32), p += 32;
    memcpy(p, a->img_format, 32), p += 32;
    memcpy(p, a->attr_type, 32), p += 32;
    memcpy(p, a->frag_out_type, 8), p += 8;
    for (int i = 0; i < 3; i++) *p++ = (uint8_t)a->local[i], *p++ = (uint8_t)(a->local[i] >> 8);
    put_u32(&p, a->shared_bytes);
    *p++ = a->depth_minus_w;
    return (size_t)(p - o);
}

static void unpack_answers(const uint8_t *p, GfxShaderAnswers *a);

// Versioned GS storage/record codec. Hashing deliberately retains the exact legacy 147-byte sequence.
static size_t pack_geometry_answers(uint8_t *o, const GfxShaderAnswers *a) {
    size_t n = pack_answers(o, a);
    o[n++] = a ? a->geometry_input : 0;
    return n;
}

static void unpack_geometry_answers(const uint8_t *p, GfxShaderAnswers *a) {
    unpack_answers(p, a);
    a->geometry_input = p[ANSWERS_PACKED];
}

// The original .in format stores a 152-byte little-endian C ABI: two padding bytes before shared_bytes and three
// after depth_minus_w. Name neither padding as a new answer nor depend on the current struct's sizeof/offsets.
static void pack_legacy_input_answers(uint8_t out[LEGACY_ANS_SIZE], const GfxShaderAnswers *a) {
    uint8_t packed[ANSWERS_PACKED];
    pack_answers(packed, a);
    memset(out, 0, LEGACY_ANS_SIZE);
    memcpy(out, packed, 142), memcpy(out + 144, packed + 142, 4), out[148] = packed[146];
}

static void unpack_legacy_input_answers(const uint8_t in[LEGACY_ANS_SIZE], GfxShaderAnswers *a) {
    uint8_t packed[ANSWERS_PACKED];
    memcpy(packed, in, 142), memcpy(packed + 142, in + 144, 4), packed[146] = in[148];
    unpack_answers(packed, a);
}

static int build_code_hash(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                           const uint8_t *c1, uint32_t c1_len, uint8_t geometry_input, uint8_t hash[32]) {
    if ((stage == GFX_STAGE_GS && (!geometry_input || geometry_input >= COUNT(geometry_names))) ||
        (stage != GFX_STAGE_GS && geometry_input)) return 0;
    size_t total = 4 + CONTROL_SIZE + 8 + (size_t)code_size + 4 + c1_len + (stage == GFX_STAGE_GS ? 12 : 0);
    uint8_t *buf = malloc(total), *p = buf;
    if (!buf) return 0;
    put_u32(&p, (uint32_t)stage);
    memcpy(p, control, CONTROL_SIZE), p += CONTROL_SIZE;
    memcpy(p, &code_size, 8), p += 8;
    memcpy(p, code, (size_t)code_size), p += code_size;
    put_u32(&p, c1_len);
    if (c1_len) memcpy(p, c1, c1_len);
    p += c1_len;
    if (stage == GFX_STAGE_GS) {
        memcpy(p, "TKGS", 4), p += 4;
        int metadata = nvn_geometry_control_get(control, CONTROL_SIZE, code, code_size, NULL);
        if (metadata < 0) return free(buf), 0;
        put_u32(&p, metadata ? GFX_GEOMETRY_PASSTHROUGH_VERSION : GFX_GEOMETRY_VERSION), put_u32(&p, geometry_input);
    }
    rt_sha256(buf, total, hash);
    free(buf);
    return 1;
}

// The answers with everything outside the mask zeroed; the translator ignores depth_minus_w (Program.cs).
static void mask_answers(const GfxShaderAnswers *a, const UseMask *m, GfxShaderAnswers *o) {
    memset(o, 0, sizeof *o);
    if (!a) return;
    for (int i = 0; i < 32; i++) {
        if (m->slots >> i & 1) o->tex_type[i] = a->tex_type[i], o->img_type[i] = a->img_type[i], o->img_format[i] = a->img_format[i];
        if (m->attrs >> i & 1) o->attr_type[i] = a->attr_type[i];
    }
    for (int i = 0; i < 8; i++)
        if (m->fouts >> i & 1) o->frag_out_type[i] = a->frag_out_type[i];
    memcpy(o->local, a->local, sizeof o->local);
    o->shared_bytes = a->shared_bytes;
}

// The cache key of an input id under the current translator.
static void key_of_input(const uint8_t iid[32], uint8_t key[32]) {
    uint8_t buf[8 + 32 + 32], *p = buf;
    memcpy(p, "TKSK", 4), p += 4;
    put_u32(&p, KEY_VERSION);
    memcpy(p, xlate_id, 32), p += 32;
    memcpy(p, iid, 32), p += 32;
    rt_sha256(buf, (size_t)(p - buf), key);
}

// The input id and cache key (m given), or the provisional key (m NULL: full answers; iid untouched).
static void build_key(const uint8_t code_hash[32], const GfxShaderAnswers *a, const UseMask *m, uint8_t key[32],
                      uint8_t iid[32]) {
    GfxShaderAnswers masked;
    uint8_t buf[8 + 32 + 32 + 256], *p = buf;
    if (m) {
        memcpy(p, "TKSI", 4), p += 4;
        put_u32(&p, IN_VERSION);
        memcpy(p, code_hash, 32), p += 32;
        mask_answers(a, m, &masked);
        p += pack_answers(p, &masked);
        rt_sha256(buf, (size_t)(p - buf), iid);
        key_of_input(iid, key);
        return;
    }
    memcpy(p, "TKSP", 4), p += 4;
    put_u32(&p, KEY_VERSION);
    memcpy(p, xlate_id, 32), p += 32;
    memcpy(p, code_hash, 32), p += 32;
    p += pack_answers(p, a);
    rt_sha256(buf, (size_t)(p - buf), key);
}

static size_t build_request(char *o0, const char *hex, const char *stage, const char *code_path, const uint8_t *c1,
                            uint32_t c1_len, const GfxShaderAnswers *a, const uint8_t *control) {
    char *o = o0;
    o += sprintf(o, "{\"job\":{\"key\":\"");
    json_esc(&o, hex);
    o += sprintf(o, "\",\"stage\":\"%s\",\"debug\":false,\"code\":\"", stage);
    json_esc(&o, code_path);
    o += sprintf(o, "\",\"c1\":\"");
    for (uint32_t i = 0; i < c1_len; i++) o += sprintf(o, "%02x", c1[i]);
    o += sprintf(o, "\"");
    if (!strcmp(stage, "gs")) {
        uint32_t skip[8];
        // req_init already validated the envelope/code. Decode the canonical payload here without guest code.
        int pass = !memcmp(control + NVN_GEOMETRY_CONTROL_OFFSET, "TKGSMETA", 8);
        o += sprintf(o, ",\"geometry_version\":%u,\"gs_input\":\"%s\"",
                     pass ? GFX_GEOMETRY_PASSTHROUGH_VERSION : GFX_GEOMETRY_VERSION, geometry_names[a->geometry_input]);
        if (pass) {
            o += sprintf(o, ",\"gs_passthrough_skip\":[");
            for (unsigned i = 0; i < 8; i++) {
                skip[i] = nvn_geometry_control_u32(control + NVN_GEOMETRY_CONTROL_OFFSET + 24 + 4 * i);
                o += sprintf(o, "%s%u", i ? "," : "", skip[i]);
            }
            o += sprintf(o, "]");
        }
    }
    if (a) {
        const char *sep = "";
        o += sprintf(o, ",\"tex\":{");
        for (int i = 0; i < 32; i++)
            if (a->tex_type[i] && a->tex_type[i] < COUNT(tex_names)) o += sprintf(o, "%s\"%d\":\"%s\"", sep, i, tex_names[a->tex_type[i]]), sep = ",";
        sep = "";
        o += sprintf(o, "},\"img\":{");
        for (int i = 0; i < 32; i++)
            if (a->img_type[i] && a->img_type[i] < COUNT(tex_names)) {
                const char *f = a->img_format[i] && a->img_format[i] < COUNT(fmt_names) ? fmt_names[a->img_format[i]] : "R8G8B8A8Unorm";
                o += sprintf(o, "%s\"%d\":{\"type\":\"%s\",\"fmt\":\"%s\"}", sep, i, tex_names[a->img_type[i]], f), sep = ",";
            }
        sep = "";
        o += sprintf(o, "},\"attr\":{");
        for (int i = 0; i < 32; i++)
            if (a->attr_type[i]) o += sprintf(o, "%s\"%d\":\"%s\"", sep, i, attr_name(a->attr_type[i])), sep = ",";
        sep = "";
        o += sprintf(o, "},\"fout\":{");
        for (int i = 0; i < 8; i++)
            if (a->frag_out_type[i]) o += sprintf(o, "%s\"%d\":\"%s\"", sep, i, attr_name(a->frag_out_type[i])), sep = ",";
        o += sprintf(o, "},\"shared\":%u", a->shared_bytes > nvn2_shared(control) ? a->shared_bytes : nvn2_shared(control));
    }
    if (!a && nvn2_shared(control)) o += sprintf(o, ",\"shared\":%u", nvn2_shared(control));
    if (!strcmp(stage, "cs")) {
        uint32_t l[3] = {1, 1, 1};
        if (a && (a->local[0] | a->local[1] | a->local[2])) l[0] = a->local[0], l[1] = a->local[1], l[2] = a->local[2];
        else memcpy(l, control + 0x748, 12);
        o += sprintf(o, ",\"local\":[%u,%u,%u]", l[0], l[1], l[2]);
        const uint8_t *lay = control + NVN2_C0_LAYOUT;
        if (!memcmp(lay, "DKC1", 4)) {
            o += sprintf(o, ",\"storage_candidates_version\":%u,\"c0\":[%u,%u,%u,%u],\"c0_version\":2,\"c0_roots\":[",
                         GFX_STORAGE_CANDIDATES_VERSION, lay[4], lay[5], lay[6], lay[7]);
            for (int i = 0; i < NVN2_ROOT_TABLES; i++) o += sprintf(o, "%s%u", i ? "," : "", lay[12 + i]);
            o += sprintf(o, "]");
        }
    }
    /* Control byte +0x718 of a fragment shader: early fragment tests (the NVN driver's early-Z-force, MacNativeGames
       ae8f4f4). Without it a shader that stores to memory is depth-tested late, and the G-buffer draws' EQUAL test
       after the depth prepass fails along contour lines. Already part of the code hash (the whole control section). */
    if (!strcmp(stage, "ps") && (control[0x718] & 1)) o += sprintf(o, ",\"early_fragment_tests\":true");
    int si = !strcmp(stage, "vs") ? 0 : !strcmp(stage, "cs") ? 2 : 1;   // legacy gfx_shader_set_host answers
    uint32_t subgroup = !strcmp(stage, "gs") ? host_gs_sg : host_sg[si];
    if (!host_f64) o += sprintf(o, ",\"softfloat64\":true");
    if (host_sbuf) o += sprintf(o, ",\"sbuf_align\":%u", host_sbuf);
    if (subgroup != 32) o += sprintf(o, ",\"subgroup\":%u", subgroup);
    if (host_gather) o += sprintf(o, ",\"gather_bias\":%u", host_gather);
    o += sprintf(o, "},\"out\":\"");
    json_esc(&o, cache_dir);
    o += sprintf(o, "\"}");
    return (size_t)(o - o0);
}

// ---- public

// Raw line exchange for the probe: one line to the service, its reply in `reply`. Starts the service if needed.
int gfx_shader_raw_request(const char *line, char *reply, size_t reply_size) {
    int away = kern_core_away();
    Svc *v = svc_get();
    int ok = child_exchange(v, line, reply, reply_size);
    if (!ok) child_stop(v);
    svc_put(v);
    kern_core_back(&away);
    return ok;
}

uint64_t gfx_shader_failures(void) { return (uint64_t)n_failures; }
uint64_t gfx_shader_requests(void) { return (uint64_t)n_requests; }   // lines sent to the service
uint64_t gfx_shader_cache_hits(void) { return (uint64_t)n_hits; }     // gets answered from memory or disk

// Drops the memory cache (the probe uses it to prove the disk cache). Pointers handed out before are invalid.
void gfx_shader_drop_memory(void) {
    plat_mutex_lock(&cache_lock);
    for (int i = 0; i < BUCKETS; i++) {
        for (GfxShader *s = buckets[i], *n; s; s = n) {
            n = s->next;
            shader_free(s);
        }
        buckets[i] = NULL;
        n_keys = 0;
        for (UseEnt *e = use_buckets[i], *n; e; e = n) n = e->next, free(e);
        use_buckets[i] = NULL;
    }
    // the preloaded .use table misses the masks this run wrote, which only use_buckets held: back to the disk
    plat_xchg32(&use_files_ready, 0);
    for (int i = 0; i < USE_FILE_BUCKETS; i++) {
        for (UseFile *e = use_files[i], *n; e; e = n) n = e->next, free(e);
        use_files[i] = NULL;
    }
    plat_mutex_unlock(&cache_lock);
}

static const GfxShader *fail(const char *why, const char *hex) {
    plat_inc64(&n_failures);
    rt_log("gfx_shader: translation failed (%s): %s", hex ? hex : "-", why);
    return NULL;
}

// A validated request: the program's code length, its c1 constants and the cache key. 0 (failure counted) when invalid.
typedef struct {
    const char *stage_name;
    int stage;
    uint8_t geometry_input;
    GfxShaderGeometry geometry;
    uint32_t geometry_skip[8];
    int geometry_source_passthrough;
    int lowered_body;                                      // nvn_geometry_control_is_lowered
    uint32_t length, c1_len;
    const uint8_t *c1;
    uint8_t code_hash[32];
    uint8_t key[32];                                       // cache key, or the provisional one with final 0
    uint8_t iid[32];                                       // input id with final 1
    int final, require_c0;                                 // DKC1 inputs must have versioned NVN 2 reflection
} Req;

// NVN 2 programs (Donkey Kong Bananza): control magic 0x98761234, version 0xC9 at +4, control size at +0x30, code
// size at +0x38 (docs/bananza-shaders.md).
int gfx_shader_is_nvn2(const uint8_t *control) {
    uint32_t magic, version;
    memcpy(&magic, control, 4), memcpy(&version, control + 4, 4);
    return magic == 0x98761234u && version == 0xC9u;
}

// With DKB_SHADER_DUMP=<folder>, gfx_shader_nvn2_dump writes each distinct NVN 2 stage there as <fnv>.ctl (control,
// size at +0x30) and <fnv>.bin (code, size at +0x38), the hash taken over both; nvn_api_obj.c calls it for every
// program the game makes. The files are game data and stay out of the repository.
// An NVN 2 stage's control size and code size (the code with its inline constants, c[0x1]); 0 when implausible.
static int nvn2_sizes(const uint8_t *control, uint32_t *ctl_size, uint32_t *len) {
    uint32_t c1_end;
    memcpy(ctl_size, control + 0x30, 4), memcpy(len, control + 0x38, 4), memcpy(&c1_end, control + 0x44, 4);
    // Some stages leave the control size at 0 (182 in Bananza's first 3D scene, its final composite among them);
    // 0xd8 is the size of all but two of the 4820 others.
    if (!*ctl_size) *ctl_size = 0xd8;
    if (c1_end > *len && c1_end - *len <= 0x10000) *len = c1_end;
    return *ctl_size >= 0x40 && *ctl_size <= 0x10000 && *len && *len <= 0x400000;
}

// FNV-1a over an NVN 2 stage's control and code: the DKB_SHADER_DUMP file name. 0 when it is not one.
uint64_t gfx_shader_nvn2_hash(const uint8_t *control, const uint8_t *code) {
    uint32_t ctl_size, len;
    if (!control || !code || !gfx_shader_is_nvn2(control) || !nvn2_sizes(control, &ctl_size, &len)) return 0;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < ctl_size; i++) h = (h ^ control[i]) * 0x100000001b3ull;
    for (uint32_t i = 0; i < len; i++) h = (h ^ code[i]) * 0x100000001b3ull;
    return h;
}

void gfx_shader_nvn2_dump(const uint8_t *control, const uint8_t *code) {
    const char *dir = getenv("DKB_SHADER_DUMP");
    uint32_t ctl_size, len;
    uint64_t h = dir && *dir ? gfx_shader_nvn2_hash(control, code) : 0;
    if (!h || !nvn2_sizes(control, &ctl_size, &len)) return;
    static PlatMutex dump_lock = PLAT_MUTEX_INIT;
    char path[1024];
    plat_mutex_lock(&dump_lock);
    mkdir_p(dir);   // a missing folder would otherwise make every fopen below fail without a word
    for (int part = 0; part < 2; part++) {
        snprintf(path, sizeof path, "%s/%016llx.%s", dir, (unsigned long long)h, part ? "bin" : "ctl");
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); continue; }
        if ((f = fopen(path, "wb"))) {
            fwrite(part ? code : control, 1, part ? len : ctl_size, f);
            fclose(f);
        }
    }
    plat_mutex_unlock(&dump_lock);
}

static int req_init(Req *r, int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size, const uint8_t *c1,
                    const GfxShaderAnswers *a) {
    memset(r, 0, sizeof *r);
    if (!control) return fail("missing control section", NULL), 0;
    // Reading an NVN 2 control block at the NVN 1 offsets (+0x700, +0x704) runs off its end and yields a bogus code
    // length, which once crashed the Vulkan path.
    if (control && gfx_shader_is_nvn2(control)) return fail("NVN 2 program (Ampere code): no translator yet", NULL), 0;
    if (control && stage == GFX_STAGE_CS && !memcmp(control + NVN2_C0_LAYOUT, "DKC0", 4))
        return fail("old NVN 2 compute control: regenerate shader inputs with root layout version 2", NULL), 0;
    r->require_c0 = stage == GFX_STAGE_CS && !memcmp(control + NVN2_C0_LAYOUT, "DKC1", 4);
    r->stage = stage;
    r->geometry_input = a ? a->geometry_input : 0;
    r->stage_name = stage == GFX_STAGE_VS ? "vs" : stage == GFX_STAGE_FS ? "ps" : stage == GFX_STAGE_CS ? "cs" :
                    stage == GFX_STAGE_GS ? "gs" : NULL;
    if (!r->stage_name) return fail("stage is not translated", NULL), 0;
    uint32_t c1_off;
    memcpy(&c1_off, control + 0x700, 4), memcpy(&r->length, control + 0x704, 4);
    uint64_t skip = stage == GFX_STAGE_CS ? 0 : CODE_SKIP;
    if (r->length > 0x400000 || c1_off > 0x400000) {   // not an NVN 1 control at all (an NVN 2 one of another version)
        uint32_t w[4];
        memcpy(w, control, sizeof w);
        static int told;
        if (told++ < 8)
            rt_log("gfx_shader: stage %d control %08x %08x %08x %08x is not NVN 1 shaped (length %#x)", stage, w[0], w[1],
                   w[2], w[3], r->length);
        return fail("control is not NVN 1 shaped", NULL), 0;
    }
    if (!code || r->length > code_size || c1_off > r->length || skip >= r->length) return fail("bad control section", NULL), 0;
    if (stage == GFX_STAGE_GS) {
        uint32_t sph[5];
        if (!host_gs_sg) return fail("geometry host answers are not initialized", NULL), 0;
        if (!r->geometry_input || r->geometry_input >= COUNT(geometry_names) ||
            c1_off < CODE_SKIP + 0x60 || (c1_off - CODE_SKIP) % 8 || (r->length - CODE_SKIP) % 8)
            return fail("incomplete geometry request", NULL), 0;
        memcpy(sph, code + CODE_SKIP, sizeof sph);
        uint32_t output = sph[3] >> 24 & 15;
        if ((sph[0] >> 10 & 15) != 4 || !(sph[2] >> 24) || !(sph[4] & 0xfff) ||
            (output != 1 && output != 6 && output != 7)) return fail("unsupported geometry SPH", NULL), 0;
        int metadata = nvn_geometry_control_get(control, CONTROL_SIZE, code, r->length, r->geometry_skip);
        int pass = (sph[0] & (1u << 24)) != 0;
        if (metadata < 0 || pass != metadata || (pass && r->geometry_input != GFX_GEOMETRY_TRIANGLES))
            return fail("unsupported geometry source metadata", NULL), 0;
        if (!memcmp(control + NVN2_C0_LAYOUT, "DKC1", 4) || !memcmp(control + NVN2_C0_LAYOUT, "DKC0", 4))
            return fail("compute root layout supplied for geometry", NULL), 0;
        r->geometry = (GfxShaderGeometry){.version = GFX_GEOMETRY_VERSION, .input = r->geometry_input,
            .input_vertices = geometry_vertices[r->geometry_input], .output = output == 1 ? 1 : output == 6 ? 2 : 3,
            .invocations = sph[2] >> 24, .max_output_vertices = sph[4] & 0xfff};
        if (pass) {
            r->geometry.version = GFX_GEOMETRY_PASSTHROUGH_VERSION;
            r->geometry.max_output_vertices = 3;
            r->geometry_source_passthrough = 1;
            r->lowered_body = nvn_geometry_control_is_lowered(control);
        }
    } else if (r->geometry_input || !memcmp(control + NVN_GEOMETRY_CONTROL_OFFSET, "TKGSMETA", 8))
        return fail("geometry input supplied for another stage", NULL), 0;
    r->c1_len = r->length - c1_off;
    r->c1 = c1 ? c1 : code + c1_off;
    if (!build_code_hash(stage, control, code, r->length, r->c1, r->c1_len, r->geometry_input, r->code_hash))
        return fail("out of memory", NULL), 0;
    paths();
    UseMask m;
    r->final = use_find(r->code_hash, &m);
    build_key(r->code_hash, a, r->final ? &m : NULL, r->key, r->iid);
    return 1;
}

static int shader_matches_request(const GfxShader *s, const Req *r) {
    return s->stage == r->stage && (r->stage != GFX_STAGE_GS ||
        (!memcmp(&s->geometry, &r->geometry, sizeof r->geometry) &&
         s->geometry_source_passthrough == r->geometry_source_passthrough &&
         !memcmp(s->geometry_skip, r->geometry_skip, sizeof r->geometry_skip)));
}

static GfxShader *request_disk_load(const Req *r, const uint8_t key[32], const uint8_t *iid) {
    for (int packed = pack_n ? 1 : 0; packed >= 0; packed--) {
        GfxShader *s = disk_load_from(key, iid, packed, (r->require_c0 ? REFLECT_REQUIRE_C0 : 0) |
                                                        (r->lowered_body ? REFLECT_LOWERED_BODY : 0));
        if (!s) continue;
        if (shader_matches_request(s, r)) return s;
        shader_free(s);
    }
    return NULL;
}

static const GfxShader *translate(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                  const GfxShaderAnswers *a);

static void ans_note(const Req *r, int stage, const uint8_t *control, const uint8_t *code, const GfxShaderAnswers *a,
                     const GfxShader *s);
static const GfxShader *get_async_one(Req r, int stage, const uint8_t *control, const uint8_t *code,
                                      const GfxShaderAnswers *a, int *pending);

static int known_failed(const uint8_t key[32]);
static void note_failed(const uint8_t key[32]);

// The synchronous path (compute dispatches, awaited stages) remembers failures as the background one does
// (X.waits): a shader that does not translate is not sent to the translator again on every dispatch or draw.
static void pf_note_miss(const uint8_t *control, const uint8_t *code, uint32_t length, const uint8_t *iid, int final, const uint8_t *rkey);
static const GfxShader *get_one(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                const GfxShaderAnswers *a) {
    GfxShader *s = mem_find(r->key);
    if (s) return shader_matches_request(s, r) ? (plat_inc64(&n_hits), s) : fail("cached stage contract differs from request", NULL);
    if (known_failed(r->key)) return NULL;
    paths();
    if (!cache_dir[0]) return fail("no cache directory", NULL);
    uint64_t q = stage == GFX_STAGE_CS ? plat_now_ns() : 0;
    rp_cs = q != 0;
    if (q) pf_note_miss(control, code, r->length, r->iid, r->final, r->key);
    s = r->final ? request_disk_load(r, r->key, r->iid) : NULL;
    rp_cs = 0;
    if (s) {
        if (q) gfx_rp[2] += plat_now_ns() - q, gfx_rp[6]++;
        return plat_inc64(&n_hits), mem_insert(s);
    }
    if (q) q = plat_now_ns();
    const GfxShader *t = translate(r, stage, control, code, a);
    if (q) gfx_rp[3] += plat_now_ns() - q, gfx_rp[7]++;
    if (!t) note_failed(r->key);
    return t;
}

// DKB_DUMP_SPV only: remember which NVN 2 stage a shader was got for (the lowered code pointer is only known here).
// Idempotent aligned 64-bit store; a shader shared by two stages keeps the last one.
static void note_origin(const GfxShader *s, const uint8_t *code) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_DUMP_SPV") != NULL;
    if (on && s) ((GfxShader *)s)->origin = nvn2_origin(code);
}
uint64_t gfx_shader_origin(const GfxShader *s) { return s->origin; }

// KAR_REPLAY_PROF (vk_exec.c), compute stages only: ns in request setup, get_one, disk load, translation; then counts.
uint64_t gfx_rp[10];
const GfxShader *gfx_shader_get(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                const uint8_t *c1, const GfxShaderAnswers *a) {
    Req r;
    uint64_t q = stage == GFX_STAGE_CS ? plat_now_ns() : 0;
    if (!req_init(&r, stage, control, code, code_size, c1, a)) return NULL;
    if (q) gfx_rp[0] += plat_now_ns() - q, gfx_rp[4]++, q = plat_now_ns();
    const GfxShader *s = get_one(&r, stage, control, code, a);
    if (q) gfx_rp[1] += plat_now_ns() - q;
    note_origin(s, code);
    if (s) ans_note(&r, stage, control, code, a, s);
    return s;
}

// ---- stored inputs (<cache>/in/<input id>.in, see the top)

typedef struct {
    uint32_t magic, version, stage, length, c1_len, answers_size;
} InHeader;                                                // then GfxShaderAnswers, control, code[length], c1[c1_len]

typedef struct {
    uint8_t *buf;                                          // the file; the pointers below point into it
    int stage;
    uint32_t length, c1_len;
    const uint8_t *control, *code, *c1;
    GfxShaderAnswers a;
} Inputs;

static void in_path(const uint8_t iid[32], char *p) {
    char hex[65];
    key_hex(iid, hex);
    snprintf(p, PATH_CAP + 80, "%s/in/%s.in", cache_dir, hex);
}

// Stores the inputs of a translation unless they are stored already; the file appears whole (written, then renamed).
static void in_store(const uint8_t iid[32], int stage, const uint8_t *control, const uint8_t *code, const Req *r,
                     const GfxShaderAnswers *a) {
    if (nvn_geometry_control_get(control, CONTROL_SIZE, code, r->length, NULL) < 0) return;
    char p[PATH_CAP + 80], tmp[PATH_CAP + 120];
    in_path(iid, p);
    if (pack_find(PACK_IN, iid) || file_exists(p)) return;
    int gs = stage == GFX_STAGE_GS;
    InHeader h = {IN_MAGIC, gs ? GS_IN_VERSION : IN_VERSION, (uint32_t)stage, r->length, r->c1_len,
                  gs ? GS_ANS_PACKED : LEGACY_ANS_SIZE};
    size_t n = sizeof h + h.answers_size + CONTROL_SIZE + r->length + r->c1_len;
    uint8_t *buf = malloc(n), *o = buf;
    if (!buf) return;
    memcpy(o, &h, sizeof h), o += sizeof h;
    if (gs) pack_geometry_answers(o, a);
    else pack_legacy_input_answers(o, a);
    o += h.answers_size;
    memcpy(o, control, CONTROL_SIZE), o += CONTROL_SIZE;
    memcpy(o, code, r->length), o += r->length;
    if (r->c1_len) memcpy(o, r->c1, r->c1_len);
    snprintf(tmp, PATH_CAP + 120, "%s/in", cache_dir);
    mkdir_p(tmp);
    plat_file_write_atomic(p, buf, n);
    free(buf);
}

// Reads stored inputs into *in (free in->buf); 0 when absent or malformed.
static int in_load_from(const uint8_t iid[32], Inputs *in, int packed) {
    char p[PATH_CAP + 80];
    in_path(iid, p);
    uint64_t n = 0;
    uint8_t *f = packed ? pack_read(PACK_IN, iid, &n) : read_file(p, &n);
    InHeader h;
    if (!f || n < sizeof h) return free(f), 0;
    memcpy(&h, f, sizeof h);
    int gs = h.stage == GFX_STAGE_GS;
    if (h.magic != IN_MAGIC || (gs ? h.version != GS_IN_VERSION || h.answers_size != GS_ANS_PACKED
                                 : h.version != IN_VERSION || h.answers_size != LEGACY_ANS_SIZE) ||
        (h.stage != GFX_STAGE_VS && h.stage != GFX_STAGE_FS && h.stage != GFX_STAGE_CS && !gs) ||
        h.c1_len > h.length || n != sizeof h + h.answers_size + CONTROL_SIZE + (uint64_t)h.length + h.c1_len)
        return free(f), 0;
    const uint8_t *o = f + sizeof h;
    if (gs) unpack_geometry_answers(o, &in->a);
    else unpack_legacy_input_answers(o, &in->a);
    if (gs && (!in->a.geometry_input || in->a.geometry_input >= COUNT(geometry_names))) return free(f), 0;
    o += h.answers_size;
    in->control = o, o += CONTROL_SIZE;
    uint32_t length, c1_at;
    memcpy(&length, in->control + 0x704, 4), memcpy(&c1_at, in->control + 0x700, 4);
    if (length != h.length || c1_at > length || length - c1_at != h.c1_len) return free(f), 0;
    in->code = o, o += h.length;
    in->c1 = o;
    in->buf = f, in->stage = (int)h.stage, in->length = h.length, in->c1_len = h.c1_len;
    return 1;
}

static int in_load(const uint8_t iid[32], Inputs *in) {
    return (pack_n && in_load_from(iid, in, 1)) || in_load_from(iid, in, 0);
}

// Portable v2 caches must retain their original inputs, even on the memory fast
// path. Reflection declares a result; it cannot establish the source's provenance.
static int input_matches_cached(const GfxShader *s, const uint8_t iid[32]) {
    int portable = s->stage == GFX_STAGE_GS && s->geometry.version == GFX_GEOMETRY_PASSTHROUGH_VERSION;
    int requires_provenance = portable;
    for (int packed = pack_n ? 1 : 0; packed >= 0; packed--) {
        Inputs in;
        if (!in_load_from(iid, &in, packed)) continue;
        // Classify the source as well as the output: a wrong-stage/v1 cached
        // result must not hide a portable or native passthrough input.
        int tagged = !memcmp(in.control + NVN_GEOMETRY_CONTROL_OFFSET, "TKGSMETA", 8) ||
                     (in.stage == GFX_STAGE_GS && in.length >= CODE_SKIP + 4 &&
                      (nvn_geometry_control_u32(in.code + CODE_SKIP) & (1u << 24)));
        requires_provenance |= tagged;
        if (!portable || !tagged) { free(in.buf); continue; }
        Req r;
        int ok = req_init(&r, in.stage, in.control, in.code, in.length, in.c1, &in.a) &&
                 shader_matches_request(s, &r);
        if (ok && !r.lowered_body) {
            // Another cache layer may have allowed extra outputs. The source must grant that policy; a raw portable
            // shader may read extra inputs, but its outputs still match the passthrough mask exactly.
            uint32_t locations = 0;
            for (unsigned i = 0; i < 32; i++)
                if (!(r.geometry_skip[1 + i / 8] >> ((i % 8) * 4) & 15)) locations |= 1u << i;
            ok = (s->in_mask & locations) == locations && s->out_mask == locations;
        }
        if (ok) {
            UseMask m = use_of(s);
            uint8_t key[32], rebuilt[32];
            build_key(r.code_hash, &in.a, &m, key, rebuilt);
            ok = !memcmp(iid, rebuilt, 32) && !memcmp(s->key, key, 32);
        }
        free(in.buf);
        if (ok) return 1;
    }
    return !requires_provenance;
}

static GfxShader *input_disk_load(const uint8_t key[32], const uint8_t iid[32]) {
    // Parse either interface policy; input_matches_cached must establish the exact source/input/cache identities
    // and require the strict interface unless that same matching input grants the lowered-body policy.
    GfxShader *s = disk_load(key, iid, REFLECT_LOWERED_BODY);
    if (!s || input_matches_cached(s, iid)) return s;
    shader_free(s);
    // A valid JSON/SPIR-V pair in the pack may still describe different inputs.
    // Preserve loose replacement fallback after the stronger v2 admission check.
    s = pack_n ? disk_load_from(key, iid, 0, REFLECT_LOWERED_BODY) : NULL;
    if (s && !input_matches_cached(s, iid)) { shader_free(s); s = NULL; }
    return s;
}

// ---- translation

// Opt-in local repro packets. The existing request serializer includes the exact draw and host answers and c1;
// retain the same lowered bytes the child read, not a reconstructed default shader variant. These are game data.
static void dump_failed_request(const char *hex, const char *request, const char *reply,
                                const uint8_t *code, size_t size) {
    const char *dir = getenv("DKB_SHADER_FAILURE_DUMP");
    if (!dir || !*dir) return;
    static volatile int64_t sequence;
    char packet[PATH_CAP + 128], json[PATH_CAP + 208], binary[PATH_CAP + 208], error[PATH_CAP + 208];
    int claimed = 0;
    mkdir_p(dir);
    // mkdir is an exclusive claim across runtimes and path aliases. Never replace another writer's packet;
    // a reused process id or an incomplete older capture gets a new sequence instead.
    for (int attempt = 0; attempt < 1024; attempt++) {
        int n = snprintf(packet, sizeof packet, "%s/capture-%u-%llu", dir, (unsigned)plat_process_id(),
                         (unsigned long long)plat_inc64(&sequence));
        if (n < 0 || (size_t)n >= sizeof packet) break;
        int status = plat_fs_mkdir(packet);
        if (status == PLAT_FS_OK) { claimed = 1; break; }
        if (status != PLAT_FS_EXISTS) break;
    }
    if (!claimed) {
        rt_log("gfx_shader: cannot claim a failed-request packet directory in %s", dir);
        return;
    }
    snprintf(json, sizeof json, "%s/%s.request.json", packet, hex);
    // This must differ from translate_one's temporary <key>.code even if dir aliases the shader cache.
    snprintf(binary, sizeof binary, "%s/%s.failed.code", packet, hex);
    snprintf(error, sizeof error, "%s/%s.error.txt", packet, hex);
    // Publish the JSON last: its presence means both the code and original error were written completely.
    if (!plat_file_write_atomic(binary, code, size) ||
        !plat_file_write_atomic(error, reply, strlen(reply)) ||
        !plat_file_write_atomic(json, request, strlen(request))) {
        rt_log("gfx_shader: cannot save failed translation %s in %s", hex, packet);
        return;
    }
    rt_log("gfx_shader: saved failed translation %s in %s (game data: keep local; tools/replay_shader_failure.py replays it)",
           hex, packet);
}

static const GfxShader *translate_one(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                      const GfxShaderAnswers *a) {
    const uint8_t *key = r->key;
    GfxShader *s;
    UseMask m;
    uint8_t mk[32], miid[32];
    // Another thread may have translated it while this one waited in flight_enter.
    const uint8_t *look = r->final ? key : NULL, *look_iid = r->iid;
    if (!r->final && use_find(r->code_hash, &m)) build_key(r->code_hash, a, &m, mk, miid), look = mk, look_iid = miid;
    if (look && (s = mem_find(look)))
        return shader_matches_request(s, r) ? (plat_inc64(&n_hits), s) : fail("cached stage contract differs from request", NULL);
    if (look && (s = request_disk_load(r, look, look_iid))) return plat_inc64(&n_hits), mem_insert(s);

    uint64_t skip = stage == GFX_STAGE_CS ? 0 : CODE_SKIP;
    char hex[65], code_path[PATH_CAP + 80], spv_path[PATH_CAP + 80], res_path[PATH_CAP + 80];
    key_hex(key, hex);
    mkdir_p(cache_dir);
    snprintf(code_path, PATH_CAP + 80, "%s/%s.code", cache_dir, hex);
    snprintf(spv_path, PATH_CAP + 80, "%s/%s.spv", cache_dir, hex);
    snprintf(res_path, PATH_CAP + 80, "%s/%s.res.json", cache_dir, hex);
    if (!write_file(code_path, code + skip, (size_t)(r->length - skip))) return fail("cannot write the code file", hex);
    size_t cap = 2 * PATH_CAP * 6 + 2 * r->c1_len + 8192;
    char *req = malloc(cap), reply[LINE_MAX_LEN];
    if (!req) return plat_file_delete(code_path), fail("out of memory", hex);
    build_request(req, hex, r->stage_name, code_path, r->c1, r->c1_len, a, control);
    int ok = svc_request(req, reply, sizeof reply);
    if (ok && strncmp(reply, "ok ", 3)) dump_failed_request(hex, req, reply, code + skip, (size_t)(r->length - skip));
    free(req);
    plat_file_delete(code_path);
    if (!ok) return fail("translator unavailable", hex);
    if (strncmp(reply, "ok ", 3)) {
        plat_file_delete(spv_path), plat_file_delete(res_path);
        return fail(reply, hex);
    }
    plat_file_flush(spv_path), plat_file_flush(res_path);   // the translator wrote them; on disk before use or rename
    if (!(s = request_disk_load(r, key, r->iid))) return fail("translator output missing or malformed", hex);
    if (!r->final) {                                       // name the files after the cache key, remember the mask
        m = use_of(s);
        build_key(r->code_hash, a, &m, mk, miid);
        char mhex[65], to[PATH_CAP + 80];
        key_hex(mk, mhex);
        snprintf(to, PATH_CAP + 80, "%s/%s.spv", cache_dir, mhex);
        int moved = plat_file_replace(spv_path, to);
        snprintf(to, PATH_CAP + 80, "%s/%s.res.json", cache_dir, mhex);
        moved = moved && plat_file_replace(res_path, to);
        if (moved) {
            use_store(r->code_hash, &m);
        } else {                                           // keep the mask for this run only
            plat_file_delete(spv_path), plat_file_delete(res_path), rt_log("gfx_shader: cannot rename %s", hex);
            plat_mutex_lock(&cache_lock);
            use_put(r->code_hash, 1, &m);
            plat_mutex_unlock(&cache_lock);
        }
        memcpy(s->key, mk, 32), memcpy(s->iid, miid, 32);
    }
    in_store(s->iid, stage, control, code, r, a);
    plat_inc64(&n_xlated);
    return mem_insert(s);
}

// The service round trip for a request that is in neither cache. One translation per cache key (per code hash while
// the use mask is unknown) runs at a time; different shaders run in parallel on the children.
// DKB_ANSWER_STATS=1: why a translation was needed in play. A shader whose answers, inside its use mask, are the
// preparation's defaults (2D textures, float attributes and outputs) is in the shader cache after --prepare-shaders, so
// each line names what differed: the texture slots and their types, the attributes, the colour outputs.
static void answer_stats_note(const Req *r, int stage, const uint8_t *code, const GfxShaderAnswers *a) {
    static int on = -1;
    static volatile int64_t lines;
    if (on < 0) on = cfg_get("DKB_ANSWER_STATS") != NULL;
    if (!on || !a) return;
    UseMask m = {0};
    int known = r->final;
    if (known) use_find(r->code_hash, &m);
    char tex[160] = "", att[96] = "", out[64] = "";
    size_t tn = 0, an = 0, on_ = 0;
    for (int i = 0; i < 32; i++) {
        if ((m.slots >> i & 1) && (a->tex_type[i] != GFX_TEX_2D || a->img_type[i]) && tn < sizeof tex - 24)
            tn += (size_t)snprintf(tex + tn, sizeof tex - tn, " %d:t%d/i%d", i, a->tex_type[i], a->img_type[i]);
        if ((m.attrs >> i & 1) && a->attr_type[i] != GFX_TYPE_FLOAT && an < sizeof att - 12)
            an += (size_t)snprintf(att + an, sizeof att - an, " %d:%d", i, a->attr_type[i]);
    }
    for (int i = 0; i < 8; i++)
        if ((m.fouts >> i & 1) && a->frag_out_type[i] != GFX_TYPE_FLOAT && on_ < sizeof out - 12)
            on_ += (size_t)snprintf(out + on_, sizeof out - on_, " %d:%d", i, a->frag_out_type[i]);
    if (plat_inc64(&lines) > 400) return;
    rt_log("answers: translated in play: stage %d, use mask %s, NVN 2 stage %016llx; non-default textures [%s] attributes [%s] "
           "outputs [%s]", stage, known ? "known" : "unknown", (unsigned long long)nvn2_origin(code), tex, att, out);
}

static const GfxShader *translate(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                  const GfxShaderAnswers *a) {
    answer_stats_note(r, stage, code, a);
    int away = kern_core_away();
    const uint8_t *id = r->final ? r->key : r->code_hash;
    flight_enter(id);
    const GfxShader *s = translate_one(r, stage, control, code, a);
    flight_leave(id);
    kern_core_back(&away);
    return s;
}

// ---- background translation (gfx_shader_get_async)
// svc_runtime_limit() workers, one per translator child. A job owns copies of the control section and the code, so the
// guest may free or rewrite its program while the job waits. A key stays in X.waits while its job is queued or running,
// and for good when the translation failed, so a failing shader is not sent again on every draw.
typedef struct XJob {
    struct XJob *next;
    int stage;
    Req r;                                                 // r.c1 points into code
    uint8_t *control, *code;
    GfxShaderAnswers a;
} XJob;

typedef struct XWait {
    struct XWait *next;
    uint8_t key[32];
    int failed;
} XWait;

static struct {
    PlatMutex lock;
    PlatCond work, done;                         // done: a job finished (gfx_shader_await)
    XJob *head, *tail;
    XWait *waits[256];
    int started;                                           // 1 running, -1 no thread could be created
} X = {PLAT_MUTEX_INIT, PLAT_COND_INIT, PLAT_COND_INIT};

static volatile int64_t n_async;

// Caller holds X.lock.
static XWait **wait_slot(const uint8_t key[32]) {
    XWait **w = &X.waits[key[0]];
    while (*w && memcmp((*w)->key, key, 32)) w = &(*w)->next;
    return w;
}

static int known_failed(const uint8_t key[32]) {
    plat_mutex_lock(&X.lock);
    XWait *w = *wait_slot(key);
    int f = w && w->failed;
    plat_mutex_unlock(&X.lock);
    return f;
}

static void note_failed(const uint8_t key[32]) {
    plat_mutex_lock(&X.lock);
    XWait **w = wait_slot(key);
    if (!*w && (*w = calloc(1, sizeof **w))) memcpy((*w)->key, key, 32);
    if (*w) (*w)->failed = 1;
    plat_mutex_unlock(&X.lock);
}

static void xlate_worker(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&X.lock);
        while (!X.head) plat_cond_wait(&X.work, &X.lock);
        XJob *j = X.head;
        if (!(X.head = j->next)) X.tail = NULL;
        plat_mutex_unlock(&X.lock);
        const GfxShader *s = translate(&j->r, j->stage, j->control, j->code, &j->a);
        plat_mutex_lock(&X.lock);
        XWait **w = wait_slot(j->r.key), *e = *w;
        if (e && s) *w = e->next, free(e);
        else if (e) e->failed = 1;
        plat_mutex_unlock(&X.lock);
        plat_cond_broadcast(&X.done);
        if (s) plat_inc64(&n_async);
        free(j->control), free(j->code), free(j);
    }
    return;
}
const GfxShader *gfx_shader_get_async(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                      const GfxShaderAnswers *a, int *pending) {
    *pending = 0;
    Req r;
    if (!req_init(&r, stage, control, code, code_size, NULL, a)) return NULL;
    const GfxShader *s = get_async_one(r, stage, control, code, a, pending);
    note_origin(s, code);
    if (s) ans_note(&r, stage, control, code, a, s);
    return s;
}

static const GfxShader *get_async_one(Req r, int stage, const uint8_t *control, const uint8_t *code,
                                      const GfxShaderAnswers *a, int *pending) {
    GfxShader *s = mem_find(r.key);
    if (s) return shader_matches_request(s, &r) ? (plat_inc64(&n_hits), s) : fail("cached stage contract differs from request", NULL);
    plat_mutex_lock_shared(&X.lock);
    XWait *w = *wait_slot(r.key);
    int failed = w && w->failed;
    plat_mutex_unlock_shared(&X.lock);
    if (w) return *pending = !failed, NULL;
    paths();
    if (!cache_dir[0]) return fail("no cache directory", NULL);
    if (r.final && (s = request_disk_load(&r, r.key, r.iid))) return plat_inc64(&n_hits), mem_insert(s);

    XJob *j = calloc(1, sizeof *j);
    XWait *e = calloc(1, sizeof *e);
    uint8_t *ctl = malloc(CONTROL_SIZE), *cd = malloc(r.length);
    plat_mutex_lock(&X.lock);
    if (!X.started) {
        int n = 0;
        for (int i = 0; i < svc_runtime_limit(); i++) {
            if (plat_thread_spawn(xlate_worker, NULL, "xlate", 0)) n++;
        }
        X.started = n ? 1 : -1;
        if (n) rt_log("gfx_shader: graphics shaders are translated by %d background workers (TOTK_XLATE_PROCS)", n);
        else rt_log("gfx_shader: cannot start the translation workers; translating synchronously");
    }
    XWait **slot = wait_slot(r.key);
    int raced = *slot != NULL;                             // another thread queued it between the checks
    int queued = !raced && X.started == 1 && j && e && ctl && cd;
    if (queued) {
        memcpy(ctl, control, CONTROL_SIZE), memcpy(cd, code, r.length);
        *j = (XJob){.stage = stage, .r = r, .control = ctl, .code = cd, .a = *a};
        j->r.c1 = cd + (r.length - r.c1_len);
        memcpy(e->key, r.key, 32);
        *slot = e;
        if (X.tail) X.tail->next = j;
        else X.head = j;
        X.tail = j;
        plat_cond_signal(&X.work);
    }
    plat_mutex_unlock(&X.lock);
    if (queued) return *pending = 1, NULL;
    free(j), free(e), free(ctl), free(cd);
    if (raced) return *pending = 1, NULL;
    return translate(&r, stage, control, code, a);         // no worker or out of memory
}

const GfxShader *gfx_shader_await(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                  const GfxShaderAnswers *a) {
    int pending;
    const GfxShader *s = gfx_shader_get_async(stage, control, code, code_size, a, &pending);
    Req r;
    if (!pending || !req_init(&r, stage, control, code, code_size, NULL, a)) return s;
    uint64_t p0 = perf_begin();   // the wait counts as the render thread's translation time
    plat_mutex_lock(&X.lock);
    for (XJob *j = X.head, *prev = NULL; j; prev = j, j = j->next)
        if (!memcmp(j->r.key, r.key, 32)) {                // queued: to the front, the worker takes it next
            if (prev) {
                prev->next = j->next;
                if (X.tail == j) X.tail = prev;
                j->next = X.head, X.head = j;
            }
            break;
        }
    XWait *w;
    while ((w = *wait_slot(r.key)) && !w->failed) plat_cond_wait(&X.done, &X.lock);
    plat_mutex_unlock(&X.lock);
    perf_end(PERF_XLATE, p0);
    if (w) return NULL;                                    // failed: logged by the worker
    s = gfx_shader_get_async(stage, control, code, code_size, a, &pending);
    return pending ? NULL : s;
}

uint64_t gfx_shader_async_done(void) { return (uint64_t)n_async; }

void gfx_shader_counts(uint64_t *keys, uint64_t *translated) { *keys = (uint64_t)n_keys, *translated = (uint64_t)n_xlated; }

const uint8_t *gfx_shader_key(const GfxShader *s) { return s->key; }

const uint8_t *gfx_shader_input_id(const GfxShader *s) { return s->iid; }

// By input id: memory, then the disk cache under the current translator, then (xlate 1) a translation from the stored
// inputs. NULL when none has it; a failed translation is counted as a failure.
const GfxShader *gfx_shader_load_input(const uint8_t iid[32], int xlate) {
    paths();
    if (!cache_dir[0]) return NULL;
    uint8_t key[32];
    key_of_input(iid, key);
    GfxShader *s = mem_find(key);
    if (s) return input_matches_cached(s, iid) ? s : NULL;
    if ((s = input_disk_load(key, iid))) return mem_insert(s);
    Inputs in;
    if (!xlate || !in_load(iid, &in)) return NULL;
    const GfxShader *t = gfx_shader_get(in.stage, in.control, in.code, in.length, in.c1, &in.a);
    free(in.buf);
    // A changed stored input may resolve successfully under a different name.
    // This lookup promises the requested input, not another cached translation.
    return t && !memcmp(t->iid, iid, 32) ? t : NULL;
}

// From the disk cache only, as a shader the caller owns and releases with gfx_shader_release_private (the boot library
// prebuild warms the driver's pipeline cache with it without keeping 40 KB per shader in memory). When the memory
// cache has it, that entry is returned with *owned 0 and must not be released. NULL when not cached.
const GfxShader *gfx_shader_load_private(const uint8_t iid[32], int *owned) {
    *owned = 0;
    paths();
    if (!cache_dir[0]) return NULL;
    uint8_t key[32];
    key_of_input(iid, key);
    GfxShader *s = mem_find(key);
    if (s) return input_matches_cached(s, iid) ? s : NULL;
    if ((s = input_disk_load(key, iid))) *owned = 1;
    return s;
}

void gfx_shader_release_private(const GfxShader *s) {
    if (s) shader_free((GfxShader *)s);
}

// ---- boot prewarm (gfx_shader_prewarm)

typedef struct {
    const uint8_t (*named)[32];                            // loaded into memory, results in out
    size_t n_named;
    const GfxShader **out;
    uint8_t (*other)[32];                                  // other stored inputs: translated when not cached, not loaded
    size_t n_other;
    volatile int64_t next, done;
    volatile int32_t missing, other_cached;
} Warm;

static void warm_worker(void *arg) {
    Warm *w = arg;
    for (;;) {
        size_t i = (size_t)plat_inc64(&w->next) - 1;
        if (i >= w->n_named + w->n_other) return;
        if (i < w->n_named) {
            if (!(w->out[i] = gfx_shader_load_input(w->named[i], 1))) plat_inc32(&w->missing);
        } else {
            const uint8_t *iid = w->other[i - w->n_named];
            uint8_t key[32];
            char hex[65], p[PATH_CAP + 80];
            key_of_input(iid, key);
            key_hex(key, hex);
            snprintf(p, PATH_CAP + 80, "%s/%s.spv", cache_dir, hex);
            if (pack_find(PACK_SPV, key) || file_exists(p)) plat_inc32(&w->other_cached);
            else gfx_shader_load_input(iid, 1);
        }
        plat_inc64(&w->done);
    }
}

static int iid_cmp(const void *x, const void *y) { return memcmp(x, y, 32); }

typedef struct {
    const uint8_t (*named)[32];
    size_t n_named, n, cap;
    uint8_t (*v)[32];
} InList;

static int in_collect(const char *file, void *ctx) {
    InList *l = ctx;
    char name[80];
    uint8_t iid[32];
    if (strlen(file) != 67) return 1;                      // 64 hex digits + ".in"
    memcpy(name, file, 64);
    name[64] = 0;
    if (!hex32(name, iid) || (l->n_named && bsearch(iid, l->named, l->n_named, 32, iid_cmp))) return 1;
    if (l->n == l->cap) {
        size_t c = l->cap ? l->cap * 2 : 1024;
        uint8_t (*t)[32] = realloc(l->v, c * 32);
        if (!t) return 0;
        l->v = t, l->cap = c;
    }
    memcpy(l->v[l->n++], iid, 32);
    return 1;
}

// The stored inputs not in named (sorted): ids parsed from the in/ file names.
static size_t other_inputs(const uint8_t (*named)[32], size_t n_named, uint8_t (**out)[32]) {
    char dir[PATH_CAP + 80];
    snprintf(dir, sizeof dir, "%s/in", cache_dir);
    InList l = {.named = named, .n_named = n_named};
    char name[68];
    for (uint32_t i = 0; i < pack_n; i++) {   // the packed inputs, under the name their loose file would have
        if (pack_ix[i].kind != PACK_IN) continue;
        key_hex(pack_ix[i].name, name);
        memcpy(name + 64, ".in", 4);
        if (!in_collect(name, &l)) break;
    }
    plat_file_list(dir, ".in", in_collect, &l);   // in_store writes no loose file for a packed input: no duplicates
    return *out = l.v, l.n;
}

static Warm *volatile warm_live;   // the prewarm running (gfx_shader_prewarm_progress)
static volatile size_t warm_total;
void gfx_shader_prewarm(const uint8_t (*iids)[32], size_t n, const GfxShader **out, int threads, int procs) {
    paths();
    double t0 = (double)plat_now_ms();
    memset(out, 0, n * sizeof *out);
    if (!cache_dir[0]) return;
    uint64_t xl0 = (uint64_t)n_xlated, fail0 = (uint64_t)n_failures;
    uint8_t (*sorted)[32] = n ? malloc(n * 32) : NULL;
    if (sorted) memcpy(sorted, iids, n * 32), qsort(sorted, n, 32, iid_cmp);
    Warm w = {.named = iids, .n_named = n, .out = out};
    w.n_other = other_inputs(sorted, sorted ? n : 0, &w.other);
    free(sorted);
    size_t total = w.n_named + w.n_other;
    threads = threads < 1 ? 1 : threads > 64 ? 64 : threads;
    if ((size_t)threads > total) threads = (int)(total ? total : 1);
    svc_set_limit(procs);
    PlatThread *t[64];
    int k = 0;
    for (int i = 0; i < threads; i++)
        if ((t[k] = plat_thread_new(warm_worker, &w, "shader prewarm"))) k++;
    warm_live = &w, warm_total = total;
    if (!k) warm_worker(&w);
    for (int m = 0; m < k; m++)
        while (!plat_thread_join_ns(t[m], 2000000000u))
            rt_log("gfx_shader: prewarm: %lld of %zu shaders, %llu translated, %.0f ms", (long long)w.done, total,
                   (unsigned long long)((uint64_t)n_xlated - xl0), (double)plat_now_ms() - t0);
    svc_set_limit(svc_runtime_limit());
    rt_log("gfx_shader: prewarm: %zu shaders named by pipeline keys (%ld not available), %zu other stored inputs (%ld "
           "cached); %llu translated, %llu failed, with up to %d translators and %d threads, in %.0f ms", w.n_named,
           (long)w.missing, w.n_other, (long)w.other_cached, (unsigned long long)((uint64_t)n_xlated - xl0),
           (unsigned long long)((uint64_t)n_failures - fail0), procs, k, (double)plat_now_ms() - t0);
    warm_live = NULL;
    free(w.other);
}

// The boot screen's progress (boot_screen.c): shaders of the boot prewarm done, of all; 0 of 0 outside it.
void gfx_shader_prewarm_progress(uint64_t *done, uint64_t *total) {
    const Warm *w = warm_live;
    *done = w ? (uint64_t)w->done : 0, *total = w ? warm_total : 0;
}
// A compute shader's SPIR-V (~1 MB in Kirby Air Riders) is needed only to build its one pipeline; the compute prefetch
// keeps thousands in memory, so the back end drops it once the pipeline is built (gfx_shader_drop_spirv) and a later
// reader (a dump, a rebuild) gets it loaded again from the disk cache, patched as at load. KAR_CS_DROP_SPIRV=0 keeps it.
static volatile int64_t pf_resident;   // bytes of prefetched compute SPIR-V still held (the prefetch cap)
static PlatMutex spirv_lock;   // zero-initialised storage is a valid unlocked lock (platform.h)

// A dropped buffer is freed 5 s later, not at once: a pipeline builder on another thread may still be reading the
// pointer gfx_shader_spirv gave it (the dropping build itself is done by then; a parallel one of the same shader is rare).
#define GRAVE_N 16384
static struct { uint32_t *p; uint64_t at; } grave[GRAVE_N];
static uint32_t grave_head, grave_tail;   // spirv_lock held

static void grave_sweep_locked(uint64_t now) {
    while (grave_head != grave_tail && (now - grave[grave_head % GRAVE_N].at > 5000000000ull ||
                                        grave_tail - grave_head >= GRAVE_N))
        free(grave[grave_head % GRAVE_N].p), grave_head++;
}

static void drop_locked(GfxShader *s, uint64_t now) {
    if (!s->spirv) return;
    grave_sweep_locked(now);
    grave[grave_tail % GRAVE_N].p = s->spirv, grave[grave_tail % GRAVE_N].at = now, grave_tail++;
    if (plat_add64(&pf_resident, -(int64_t)(s->words * 4)) < 0) pf_resident = 0;   // a shader the prefetch did not count
    s->spirv = NULL;
}

static int drop_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("KAR_CS_DROP_SPIRV") && *getenv("KAR_CS_DROP_SPIRV") == '0');
    return on;
}

void gfx_shader_drop_spirv(const GfxShader *cs) {
    if (!drop_on() || !cs || cs->stage != GFX_STAGE_CS) return;
    gfx_shader_spirv_fnv(cs);   // the hash is kept; it is taken from the SPIR-V
    plat_mutex_lock(&spirv_lock);
    drop_locked((GfxShader *)cs, plat_now_ns());
    plat_mutex_unlock(&spirv_lock);
}

// After the boot prewarm: every shader in memory gives its SPIR-V back (graphics ones too; a later pipeline of one
// loads it again). The prewarm's ~4700 graphics shaders held ~0.5 GB that only their pipeline builds needed.
void gfx_shader_drop_all_spirv(void) {
    if (!drop_on()) return;
    uint64_t now = plat_now_ns(), n = 0, bytes = 0;
    plat_mutex_lock_shared(&cache_lock);
    for (size_t i = 0; i < BUCKETS; i++)
        for (GfxShader *s = buckets[i]; s; s = s->next) {
            if (!s->spirv) continue;
            gfx_shader_spirv_fnv(s);
            plat_mutex_lock(&spirv_lock);
            if (s->spirv) n++, bytes += s->words * 4, drop_locked(s, now);
            plat_mutex_unlock(&spirv_lock);
        }
    plat_mutex_unlock_shared(&cache_lock);
    rt_log("gfx_shader: %llu shaders gave back %.1f MiB of SPIR-V after the prewarm (loaded again when needed)",
           (unsigned long long)n, bytes / 1048576.0);
}

const uint32_t *gfx_shader_spirv(const GfxShader *s, uint64_t *words) {
    if (words) *words = s->words;
    if (__atomic_load_n(&s->spirv, __ATOMIC_ACQUIRE)) return s->spirv;
    plat_mutex_lock(&spirv_lock);
    if (!s->spirv) {
        GfxShader *d = disk_load(s->key, s->iid, REFLECT_LOWERED_BODY);
        if (!d) d = disk_load(s->key, s->iid, 0);
        if (d && d->words == s->words) {
            __atomic_store_n(&((GfxShader *)s)->spirv, d->spirv, __ATOMIC_RELEASE);
            plat_add64(&pf_resident, (int64_t)(s->words * 4));
            d->spirv = NULL;
        } else {
            rt_log("gfx_shader: the dropped SPIR-V of a compute shader could not be loaded again");
        }
        if (d) shader_free(d);
    }
    plat_mutex_unlock(&spirv_lock);
    return s->spirv;
}

// The SPIR-V never changes after the shader is made, so the hash is taken on the first call and kept with the shader.
// Threads that race on the first call compute the same value; 0 marks "not taken yet" (a real hash of 0 is just taken
// again each time).
uint64_t gfx_shader_spirv_fnv(const GfxShader *s) {
    uint64_t h = __atomic_load_n(&s->spirv_fnv, __ATOMIC_RELAXED);
    if (h) return h;
    h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)s->spirv;
    for (uint64_t i = 0; b && i < s->words * 4; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    __atomic_store_n(&((GfxShader *)s)->spirv_fnv, h, __ATOMIC_RELAXED);
    return h;
}

int gfx_shader_stage(const GfxShader *s) { return s->stage; }
const GfxShaderGeometry *gfx_shader_geometry(const GfxShader *s) { return s->stage == GFX_STAGE_GS ? &s->geometry : NULL; }

int gfx_shader_resources(const GfxShader *s, int kind, const GfxShaderRes **res) {
    if (kind < 0 || kind >= GFX_RES_KINDS) return 0;
    if (res) *res = s->res[kind];
    return s->nres[kind];
}

const Nvn2C0Layout *gfx_shader_c0_layout(const GfxShader *s) { return s && s->has_c0 ? &s->c0 : NULL; }

// The slots of the use mask: answers outside it select the same translation (mask_answers), so a caller may leave
// their texture and image types zero. Parses the reflection: callers keep the result.
uint32_t gfx_shader_slot_mask(const GfxShader *s) { return use_of(s).slots; }

uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->in, sizeof s->in);
    return s->in_mask;
}

uint32_t gfx_shader_outputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->out, sizeof s->out);
    return s->out_mask;
}

void gfx_shader_local_size(const GfxShader *s, uint32_t local[3]) { memcpy(local, s->local, sizeof s->local); }

// ---- answers met in play
// The translated-shader key includes the answers a draw's state implies, so a translation made without play (the
// census) does not match the keys play asks for. Every (program, answers) that a gfx_shader_get* call resolves is
// recorded next to pipeline-keys.bin, so the first-start preparation (Task 2.6, here the TOTK_TEST_PREPARE_SHADERS hook)
// translates exactly the variants play needs, from these files alone:
//   shader-answers.bin  "TOTKSANS", version, record size; records: stage (u32), code hash, input id, packed full answers,
//                       GS input selector (v2), priority. One record per input id: answers outside the shader's use
//                       mask do not change its SPIR-V, are out of its input id and cache key (mask_answers) and so do
//                       not make records of their own (a fight run meets 4126 full pairs, 1238 input ids). The full
//                       answers are kept so a translator with a wider mask still gets them. Under a new translator
//                       the masks, hence the input ids, are new, and play appends records for them.
//   shader-programs.bin "TOTKSPRG", version, 0; records: stage, length, c1 length (u32 each), code hash, control section,
//                       code[length], c1. One per code hash; written before the answers that name it. Game content:
//                       it stays in the ignored pipeline cache directory.
// A file with another header is rewritten; a torn tail (killed process) is cut off at open.
#define ANS_VERSION 2                                     // independent GS input selector; readers retain version 1
#define ANS_PACKED GS_ANS_PACKED
#define ANS_REC (4 + 32 + 32 + ANS_PACKED + 1)
#define ANS_REC_V1 (4 + 32 + 32 + ANSWERS_PACKED + 1)
#define PRG_VERSION 1
#define PRG_HEAD (12 + 32 + CONTROL_SIZE)
typedef struct { char magic[8]; uint32_t version, rec_size; } RecHeader;
typedef struct { uint64_t *v; uint32_t cap, n; } Set64;    // open addressing, 0 empty
typedef struct { uint8_t *p; size_t n, cap; } Bytes;

static struct {
    PlatMutex lock;
    volatile int32_t on;
    char ans_path[PATH_CAP], prg_path[PATH_CAP];
    Set64 pairs, progs;                                    // input ids, code hashes seen (in the files or this run)
    Bytes ans, prg;                                        // pending
    uint64_t pairs_new;                                    // added this run
} RC = {PLAT_MUTEX_INIT};

static int set_add(Set64 *s, uint64_t h) {                 // 1 if new
    h = h ? h : 1;
    if ((s->n + 1) * 2 > s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 4096;
        uint64_t *t = calloc(cap, sizeof *t);
        if (!t) return 0;
        for (uint32_t i = 0; i < s->cap; i++)
            if (s->v[i]) {
                uint32_t j = (uint32_t)s->v[i] & (cap - 1);
                while (t[j]) j = (j + 1) & (cap - 1);
                t[j] = s->v[i];
            }
        free(s->v), s->v = t, s->cap = cap;
    }
    uint32_t j = (uint32_t)h & (s->cap - 1);
    for (; s->v[j]; j = (j + 1) & (s->cap - 1))
        if (s->v[j] == h) return 0;
    s->v[j] = h, s->n++;
    return 1;
}

static int bytes_put(Bytes *b, const void *p, size_t n) {
    if (!n) return 1;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->n + n) cap *= 2;
        uint8_t *t = realloc(b->p, cap);
        if (!t) return 0;
        b->p = t, b->cap = cap;
    }
    memcpy(b->p + b->n, p, n), b->n += n;
    return 1;
}

static uint64_t hash8(const uint8_t *sha) {
    uint64_t v;
    memcpy(&v, sha, 8);
    return v;
}

static void unpack_answers(const uint8_t *p, GfxShaderAnswers *a) {
    memset(a, 0, sizeof *a);
    memcpy(a->tex_type, p, 32), p += 32;
    memcpy(a->img_type, p, 32), p += 32;
    memcpy(a->img_format, p, 32), p += 32;
    memcpy(a->attr_type, p, 32), p += 32;
    memcpy(a->frag_out_type, p, 8), p += 8;
    for (int i = 0; i < 3; i++) a->local[i] = (uint16_t)(p[0] | p[1] << 8), p += 2;
    memcpy(&a->shared_bytes, p, 4), p += 4;
    a->depth_minus_w = *p;
}

// Normalize both answer record versions to v2 in memory. The final byte is seed priority, never spare padding.
// Whole legacy records retain every byte and gain a zero selector. A torn final record is the only discarded data.
static int answers_decode(const uint8_t *file, size_t size, uint8_t **body, size_t *n, int *rewrite) {
    *body = NULL, *n = 0, *rewrite = 0;
    if (!size) { *rewrite = 1; return 1; }
    RecHeader h;
    if (size < sizeof h) return 0;
    memcpy(&h, file, sizeof h);
    int legacy = h.version == 1 && h.rec_size == ANS_REC_V1;
    if (memcmp(h.magic, "TOTKSANS", 8) || (!legacy && (h.version != ANS_VERSION || h.rec_size != ANS_REC))) return 0;
    size_t bytes = size - sizeof h, count = bytes / h.rec_size;
    if (count > SIZE_MAX / ANS_REC) return 0;
    uint8_t *out = calloc(count ? count : 1, ANS_REC);
    if (!out) return 0;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *from = file + sizeof h + i * h.rec_size;
        uint8_t *to = out + i * ANS_REC;
        if (legacy) {
            memcpy(to, from, ANS_REC_V1 - 1);
            to[ANS_REC - 1] = from[ANS_REC_V1 - 1];
        } else memcpy(to, from, ANS_REC);
        uint32_t stage;
        memcpy(&stage, to, 4);
        uint8_t input = to[68 + ANSWERS_PACKED];
        if (stage == GFX_STAGE_GS ? (!input || input >= COUNT(geometry_names)) :
            (input || (stage != GFX_STAGE_VS && stage != GFX_STAGE_FS && stage != GFX_STAGE_CS))) {
            free(out);
            return 0;
        }
    }
    *body = out, *n = count * ANS_REC;
    *rewrite = legacy || bytes % h.rec_size;
    return 1;
}

// Read-only users normalize without modifying disk. Recording upgrades atomically before it can append a v2 record;
// an allocation/write/format failure leaves the old file intact and prevents recording into a mixed-version file.
static int answers_read(const char *path, uint8_t **body, size_t *n, int migrate) {
    *body = NULL, *n = 0;
    uint64_t size = 0;
    uint8_t *file = read_file(path, &size);
    if (!file) return 0;
    int rewrite = 0, ok = size <= SIZE_MAX && answers_decode(file, (size_t)size, body, n, &rewrite);
    free(file);
    if (ok && migrate && rewrite) {
        RecHeader h = {{'T','O','T','K','S','A','N','S'}, ANS_VERSION, ANS_REC};
        if (*n > SIZE_MAX - sizeof h) ok = 0;
        uint8_t *out = ok ? malloc(sizeof h + *n) : NULL;
        if (out) {
            memcpy(out, &h, sizeof h);
            if (*n) memcpy(out + sizeof h, *body, *n);
            ok = plat_file_write_atomic(path, out, sizeof h + *n);
            free(out);
        } else ok = 0;
    }
    if (!ok) {
        free(*body), *body = NULL, *n = 0;
        rt_log("gfx_shader: cannot read or upgrade %s; existing answers were retained", path);
    }
    return ok;
}

// Reads a record file's body (after its header) into *body; 0 when absent, and a file with another header is removed.
static int rec_read(const char *path, const char *magic, uint32_t version, uint32_t rec_size, uint8_t **body, size_t *n) {
    *body = NULL, *n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    RecHeader h;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    int ok = len >= (long)sizeof h && fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, magic, 8) &&
             h.version == version && h.rec_size == rec_size;
    if (ok && len > (long)sizeof h) {
        *n = (size_t)len - sizeof h;
        ok = (*body = malloc(*n)) && fread(*body, 1, *n, f) == *n;
    }
    fclose(f);
    if (!ok) {
        free(*body), *body = NULL, *n = 0;
        if (len > 0) rt_log("gfx_shader: %s has another format or version; it is rewritten", path), remove(path);
    }
    return ok;
}

// Walks shader-programs.bin records; returns the byte length of the whole records. fn may be NULL.
static size_t prg_walk(const uint8_t *b, size_t n, void (*fn)(const uint8_t *rec, void *ctx), void *ctx) {
    size_t o = 0;
    while (n - o >= PRG_HEAD) {
        uint32_t len, c1;
        memcpy(&len, b + o + 4, 4), memcpy(&c1, b + o + 8, 4);
        size_t size = PRG_HEAD + (size_t)len + c1;
        if (len > (64u << 20) || c1 > len || size > n - o) break;
        if (fn) fn(b + o, ctx);
        o += size;
    }
    return o;
}

static void prg_seen(const uint8_t *rec, void *ctx) { set_add(ctx, hash8(rec + 12)); }

static void truncate_to(const char *path, size_t size) {
    if (!plat_file_truncate(path, size)) return;
    rt_log("gfx_shader: %s had a torn tail; cut to %zu bytes", path, size);
}

void gfx_shader_record_at(const char *keys_path) {
    if (!keys_path || !keys_path[0] || RC.on) return;
    plat_mutex_lock(&RC.lock);
    const char *slash = strrchr(keys_path, '\\'), *fs = strrchr(keys_path, '/');
    if (fs > slash) slash = fs;
    int dir = slash ? (int)(slash + 1 - keys_path) : 0;
    snprintf(RC.ans_path, sizeof RC.ans_path, "%.*sshader-answers.bin", dir, keys_path);
    snprintf(RC.prg_path, sizeof RC.prg_path, "%.*sshader-programs.bin", dir, keys_path);
    uint8_t *b;
    size_t n;
    if (answers_read(RC.ans_path, &b, &n, 1)) {
        for (size_t o = 0; o + ANS_REC <= n; o += ANS_REC) set_add(&RC.pairs, hash8(b + o + 36));
    } else if (file_exists(RC.ans_path)) {
        plat_mutex_unlock(&RC.lock);
        return;                                          // do not append a different format after a failed migration
    }
    free(b);
    if (rec_read(RC.prg_path, "TOTKSPRG", PRG_VERSION, 0, &b, &n)) {
        size_t whole = prg_walk(b, n, prg_seen, &RC.progs);
        if (whole != n) truncate_to(RC.prg_path, sizeof(RecHeader) + whole);
    }
    free(b);
    rt_log("gfx_shader: recording answers met in play to %s (%u records, %u programs recorded before)", RC.ans_path,
           RC.pairs.n, RC.progs.n);
    plat_xchg32(&RC.on, 1);
    plat_mutex_unlock(&RC.lock);
}

// A get resolved (r, a) to s: queue the record when its input id is new, and its program when new. Cost: a set
// lookup under a lock per call; gets happen per (program, answers) change in vk_exec_draw's shader table, not per draw.
static void ans_note(const Req *r, int stage, const uint8_t *control, const uint8_t *code, const GfxShaderAnswers *a,
                     const GfxShader *s) {
    if (!RC.on) return;
    if (nvn_geometry_control_get(control, CONTROL_SIZE, code, r->length, NULL) < 0) return;
    uint32_t st = (uint32_t)stage;
    plat_mutex_lock(&RC.lock);
    if (set_add(&RC.pairs, hash8(s->iid))) {
        uint8_t rec[ANS_REC] = {0};
        memcpy(rec, &st, 4), memcpy(rec + 4, r->code_hash, 32), memcpy(rec + 36, s->iid, 32);
        pack_geometry_answers(rec + 68, a);
        if (set_add(&RC.progs, hash8(r->code_hash))) {
            uint32_t head[3] = {st, r->length, r->c1_len};
            bytes_put(&RC.prg, head, 12), bytes_put(&RC.prg, r->code_hash, 32);
            bytes_put(&RC.prg, control, CONTROL_SIZE), bytes_put(&RC.prg, code, r->length);
            bytes_put(&RC.prg, r->c1, r->c1_len);
        }
        bytes_put(&RC.ans, rec, sizeof rec);
        RC.pairs_new++;
    }
    plat_mutex_unlock(&RC.lock);
}

static int rec_append(const char *path, const char *magic, uint32_t version, uint32_t rec_size, const Bytes *b) {
    FILE *f = fopen(path, "ab");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    int ok = 1;
    if (ftell(f) == 0) {
        RecHeader h = {{0}, version, rec_size};
        memcpy(h.magic, magic, 8);
        ok = fwrite(&h, sizeof h, 1, f) == 1;
    }
    ok = ok && fwrite(b->p, 1, b->n, f) == b->n;
    return !fclose(f) && ok;
}

static PlatMutex rec_io = PLAT_MUTEX_INIT;   // the record files: appends in order, no read during an append

void gfx_shader_record_flush(int force) {
    static volatile int64_t last;
    int64_t now = (int64_t)plat_now_ms();
    if (!RC.on || (!force && now - last < 2000)) return;
    last = now;
    // The files are written outside RC.lock, which the render thread takes to record a new answer.
    plat_mutex_lock(&rec_io);
    plat_mutex_lock(&RC.lock);
    Bytes ans = RC.ans, prg = RC.prg;
    RC.ans = (Bytes){0}, RC.prg = (Bytes){0};
    uint64_t total = RC.pairs.n, added = RC.pairs_new;
    plat_mutex_unlock(&RC.lock);
    if (prg.n && !rec_append(RC.prg_path, "TOTKSPRG", PRG_VERSION, 0, &prg)) rt_log("gfx_shader: cannot write %s", RC.prg_path);
    if (ans.n && !rec_append(RC.ans_path, "TOTKSANS", ANS_VERSION, ANS_REC, &ans)) rt_log("gfx_shader: cannot write %s", RC.ans_path);
    plat_mutex_unlock(&rec_io);
    if (ans.n) rt_log("gfx_shader: answers: %zu new records appended (%llu new this run, %llu recorded)", ans.n / ANS_REC,
                      (unsigned long long)added, (unsigned long long)total);
    free(ans.p), free(prg.p);
}

// ---- preparation from record files: the answers met in play (TOTK_TEST_PREPARE_SHADERS=1) or the seed that
// tools/prepare_shaders.py writes from the MacNativeGames shader list (totk.exe --prepare-shaders). Both use the record
// layouts above; a seed record has a zero input id (the use mask, hence the id, is the translator's to find) and its
// pad byte holds the list priority (0 title, 1 warm-up, 2 rest).
typedef struct { const uint8_t *rec; } PrgIdx;
typedef struct { PrgIdx *idx; size_t n; } PrgList;
typedef struct {
    PrgList progs;
    const uint8_t *ans;
    size_t n_ans;
    uint8_t (*iid)[32];                                    // per answers record: the resolved input id, zero on failure
    volatile int64_t next, done, missing, failed;
} Prep;

static int prg_cmp(const void *x, const void *y) { return memcmp(((const PrgIdx *)x)->rec + 12, ((const PrgIdx *)y)->rec + 12, 32); }

static void prg_collect(const uint8_t *rec, void *ctx) {
    PrgList *l = ctx;
    l->idx[l->n++].rec = rec;
}

static void prep_worker(void *arg) {
    Prep *p = arg;
    for (;;) {
        size_t i = (size_t)plat_inc64(&p->next) - 1;
        if (i >= p->n_ans) return;
        const uint8_t *r = p->ans + i * ANS_REC;
        uint8_t key_rec[12 + 32];
        memcpy(key_rec + 12, r + 4, 32);
        PrgIdx k = {key_rec}, *e = bsearch(&k, p->progs.idx, p->progs.n, sizeof k, prg_cmp);
        if (!e) {
            plat_inc64(&p->missing), plat_inc64(&p->done);
            continue;
        }
        uint32_t st, len;
        memcpy(&st, e->rec, 4), memcpy(&len, e->rec + 4, 4);
        const uint8_t *control = e->rec + 44, *code = control + CONTROL_SIZE, *c1 = code + len;
        GfxShaderAnswers a;
        unpack_geometry_answers(r + 68, &a);
        uint32_t answer_stage;
        memcpy(&answer_stage, r, 4);
        if (answer_stage != st) {
            plat_inc64(&p->failed), plat_inc64(&p->done);
            continue;
        }
        if (!memcmp(control + NVN_GEOMETRY_CONTROL_OFFSET, "TKGSMETA", 8)) {
            Req rebuilt;
            if (!req_init(&rebuilt, (int)st, control, code, len, c1, &a) ||
                memcmp(rebuilt.code_hash, e->rec + 12, 32)) {
                plat_inc64(&p->failed), plat_inc64(&p->done);
                continue;
            }
        }
        const GfxShader *s = gfx_shader_get((int)st, control, code, len, c1, &a);
        if (!s) plat_inc64(&p->failed);
        else if (p->iid) memcpy(p->iid[i], s->iid, 32);
        plat_inc64(&p->done);
    }
}

// Translates every record of ans_path whose program is in prg_path on `threads` threads with up to `procs` translators.
// *ans_out, *iid_out (optional; free them): the answers records and the input id each resolved to (zero: failed).
// Returns the record count, -1 without the files.
static long long prepare_files(const char *ans_path, const char *prg_path, int threads, int procs, const char *what,
                               uint8_t **ans_out, uint8_t (**iid_out)[32]) {
    paths();
    double t0 = (double)plat_now_ms();
    uint8_t *ab = NULL, *pb = NULL;
    size_t an = 0, pn = 0;
    if (!answers_read(ans_path, &ab, &an, 0) ||
        !rec_read(prg_path, "TOTKSPRG", PRG_VERSION, 0, &pb, &pn)) {
        rt_log("gfx_shader: prepare: no %s (%s)", what, ans_path);
        free(ab), free(pb);
        return -1;
    }
    Prep p = {.progs = {calloc(pn / PRG_HEAD + 1, sizeof(PrgIdx)), 0}, .ans = ab, .n_ans = an / ANS_REC};
    if (iid_out) p.iid = calloc(p.n_ans + 1, 32);
    if (!p.progs.idx || (iid_out && !p.iid)) return free(p.progs.idx), free(p.iid), free(ab), free(pb), -1;
    prg_walk(pb, pn, prg_collect, &p.progs);
    qsort(p.progs.idx, p.progs.n, sizeof *p.progs.idx, prg_cmp);
    uint64_t xl0 = (uint64_t)n_xlated;
    threads = threads < 1 ? 1 : threads > 64 ? 64 : threads;
    svc_set_limit(procs);
    PlatThread *t[64];
    int k = 0;
    for (int i = 0; i < threads; i++)
        if ((t[k] = plat_thread_new(prep_worker, &p, "shader prepare"))) k++;
    if (!k) prep_worker(&p);
    for (int m = 0; m < k; m++)
        while (!plat_thread_join_ns(t[m], 5000000000u))
            rt_log("gfx_shader: prepare: %lld of %zu pairs, %llu translated, %.0f ms", (long long)p.done, p.n_ans,
                   (unsigned long long)((uint64_t)n_xlated - xl0), (double)plat_now_ms() - t0);
    svc_set_limit(svc_runtime_limit());
    rt_log("gfx_shader: prepare: %zu %s of %zu programs: %llu translated, %lld failed, %lld without a program, the rest "
           "cached; %.0f ms", p.n_ans, what, p.progs.n, (unsigned long long)((uint64_t)n_xlated - xl0), (long long)p.failed,
           (long long)p.missing, (double)plat_now_ms() - t0);
    free(p.progs.idx), free(pb);
    if (ans_out) *ans_out = ab;
    else free(ab);
    if (iid_out) *iid_out = p.iid;
    return (long long)p.n_ans;
}

void gfx_shader_prepare_recorded(int threads, int procs) {
    if (!RC.on) {
        rt_log("gfx_shader: prepare: no recorded answers (not recording)");
        return;
    }
    prepare_files(RC.ans_path, RC.prg_path, threads, procs, "recorded pairs", NULL, NULL);
}

// ---- prepared variants (shader-prepared.bin next to pipeline-keys.bin)
// "TOTKSPRP", version, record size; records: stage, priority, 2 zero bytes, input id. gfx_shader_prepare_seed writes
// them in priority order, one per input id; gfx_shader_warm_list reads them at boot.
#define PRP_VERSION 1
#define PRP_REC 36

// ---- NVN 2 seed pack: every stage of the game's shader files (tools/dkb_shader_census.py writes seed-nvn2.bin)
// "DKBN2SD1", u32 count, then per stage: u32 stage (GFX_STAGE_*), u32 control size, u32 code size, the control section
// and the code, each padded to 4 bytes. Each stage is lowered as nvnProgramSetShaders lowers it and translated with the
// default answers (every texture slot a 2D texture, float vertex inputs and colour outputs; a compute stage's local size
// from its control). The answers do not change a shader's SPIR-V for slots it never reads (mask_answers), so a stage
// whose draws bind other types (3D or cube textures, integer attributes) still translates on first use; play records
// those variants and the boot prewarm keeps them.
// Integer vertex attributes: a vertex stage whose draws bind integer formats (bone indices, packed colours) is a variant of
// its default. A game has few vertex layouts, so the variants are patterns of Locations: the ones met in play so far
// (Locations 6, 6 and 7, 3 and 4, all unsigned) and every other pattern the local shader-answers.bin recorded. A stage
// that reads none of a pattern's Locations resolves to the same cache key as its default and costs a lookup.
#define N2_PATS 32
typedef struct { uint8_t attr[16]; } N2Pat;
static const uint8_t n2_builtin_patterns[][16] = {
    {[6] = GFX_TYPE_UINT}, {[6] = GFX_TYPE_UINT, [7] = GFX_TYPE_UINT}, {[3] = GFX_TYPE_UINT, [4] = GFX_TYPE_UINT},
};

typedef struct {
    const uint8_t **rec;
    size_t n;
    N2Pat pat[N2_PATS];
    int npat;
    volatile int64_t next, done, translated, variants, failed, unlowered;
    PlatMutex lock;                                        // guards prp
    uint8_t *prp;                                          // shader-prepared.bin records (PRP_REC bytes), one per translation
    size_t prp_n, prp_cap;
} N2Prep;

// The variant just resolved, for shader-prepared.bin: the driver pipeline libraries of everything listed are built after
// the translation (vk_pipeline.c, lb_warm_prepared), so a draw finds them in the driver's cache. prio 0: a stage with the
// default answers, 1: an integer-attribute variant.
static void n2_note(N2Prep *p, int stage, int prio, const GfxShader *s) {
    plat_mutex_lock(&p->lock);
    if (p->prp_n == p->prp_cap) {
        size_t cap = p->prp_cap ? p->prp_cap * 2 : 4096;
        uint8_t *t = realloc(p->prp, cap * PRP_REC);
        if (!t) {
            plat_mutex_unlock(&p->lock);
            return;
        }
        p->prp = t, p->prp_cap = cap;
    }
    uint8_t *r = p->prp + p->prp_n++ * PRP_REC;
    memset(r, 0, PRP_REC);
    r[0] = (uint8_t)(stage < 6 ? stage : 0), r[1] = (uint8_t)prio;
    memcpy(r + 4, s->iid, 32);
    plat_mutex_unlock(&p->lock);
}

static void n2_add_pattern(N2Prep *p, const uint8_t attr[16]) {
    static const uint8_t zero[16];
    if (!memcmp(attr, zero, 16) || p->npat >= N2_PATS) return;
    for (int i = 0; i < p->npat; i++)
        if (!memcmp(p->pat[i].attr, attr, 16)) return;
    memcpy(p->pat[p->npat++].attr, attr, 16);
}

static void n2_worker(void *arg) {
    N2Prep *p = arg;
    for (;;) {
        size_t i = (size_t)plat_inc64(&p->next) - 1;
        if (i >= p->n) return;
        const uint8_t *r = p->rec[i];
        uint32_t st, ctl_size;
        memcpy(&st, r, 4), memcpy(&ctl_size, r + 4, 4);
        const uint8_t *control = r + 12, *code = control + ((ctl_size + 3) & ~3u), *nctl, *ncode;
        if (!nvn2_lower(control, code, &nctl, &ncode)) {
            plat_inc64(&p->unlowered), plat_inc64(&p->done);
            continue;
        }
        GfxShaderAnswers a;
        memset(&a, 0, sizeof a);
        if (st == GFX_STAGE_CS) {
            for (int k = 0; k < 3; k++) {
                uint32_t l;
                memcpy(&l, nctl + 0x748 + 4 * k, 4);
                a.local[k] = (uint16_t)(l ? l : 1);
            }
        } else {
            memset(a.tex_type, GFX_TEX_2D, sizeof a.tex_type);
        }
        uint32_t length;
        memcpy(&length, nctl + 0x704, 4);
        uint64_t x0 = (uint64_t)n_xlated;
        const GfxShader *s = gfx_shader_get((int)st, nctl, ncode, length, NULL, &a);
        if (!s) plat_inc64(&p->failed);
        else if ((uint64_t)n_xlated != x0) plat_inc64(&p->translated);
        if (s) n2_note(p, (int)st, 0, s);
        if (s && st == GFX_STAGE_VS)
            for (int k = 0; k < p->npat; k++) {
                GfxShaderAnswers b = a;
                memcpy(b.attr_type, p->pat[k].attr, sizeof p->pat[k].attr);
                x0 = (uint64_t)n_xlated;
                const GfxShader *v = gfx_shader_get((int)st, nctl, ncode, length, NULL, &b);
                if (!v) plat_inc64(&p->failed);
                else {
                    if ((uint64_t)n_xlated != x0) plat_inc64(&p->variants);
                    if (memcmp(v->iid, s->iid, 32)) n2_note(p, (int)st, 1, v);   // the same id as the default: listed already
                }
            }
        plat_inc64(&p->done);
    }
}

// 0 when the pack was read (stages that fail to translate are counted, not fatal), 1 when it cannot be read.
static int prepare_nvn2(const char *path, const char *dir, int threads, int procs) {
    double t0 = (double)plat_now_ms();
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = size > 12 ? malloc((size_t)size) : NULL;
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size || memcmp(buf, "DKBN2SD1", 8)) {
        fclose(f), free(buf);
        rt_log("gfx_shader: prepare: %s is not an NVN 2 seed pack", path);
        return 1;
    }
    fclose(f);
    uint32_t count;
    memcpy(&count, buf + 8, 4);
    N2Prep p = {.rec = calloc(count + 1, sizeof *p.rec), .lock = PLAT_MUTEX_INIT};
    for (size_t i = 0; i < sizeof n2_builtin_patterns / sizeof *n2_builtin_patterns; i++) n2_add_pattern(&p, n2_builtin_patterns[i]);
    {   // patterns this machine's play recorded
        char rec_path[PATH_CAP + 32];
        uint8_t *ab = NULL;
        size_t an = 0;
        snprintf(rec_path, sizeof rec_path, "%s/shader-answers.bin", dir);
        if (answers_read(rec_path, &ab, &an, 0))
            for (size_t i = 0; i < an / ANS_REC; i++) {
                const uint8_t *r = ab + i * ANS_REC;
                uint32_t rs;
                memcpy(&rs, r, 4);
                if (rs == GFX_STAGE_VS) n2_add_pattern(&p, r + 68 + 96);   // attr_type[0..15] of the packed answers
            }
        free(ab);
    }
    const uint8_t *at = buf + 12, *end = buf + size;
    for (uint32_t i = 0; p.rec && i < count; i++) {
        uint32_t cl, len;
        if (end - at < 12) break;
        memcpy(&cl, at + 4, 4), memcpy(&len, at + 8, 4);
        size_t total = 12 + (((size_t)cl + 3) & ~(size_t)3) + (((size_t)len + 3) & ~(size_t)3);
        if ((size_t)(end - at) < total) break;
        p.rec[p.n++] = at, at += total;
    }
    threads = threads < 1 ? 1 : threads > 64 ? 64 : threads;
    svc_set_limit(procs);
    PlatThread *t[64];
    int k = 0;
    for (int i = 0; p.rec && i < threads; i++)
        if ((t[k] = plat_thread_new(n2_worker, &p, "nvn2 prepare"))) k++;
    if (!k && p.rec) n2_worker(&p);
    for (int m = 0; m < k; m++)
        while (!plat_thread_join_ns(t[m], 5000000000u))
            rt_log("gfx_shader: prepare: NVN 2 seed %lld of %zu stages, %lld translated, %.0f ms", (long long)p.done, p.n,
                   (long long)p.translated, (double)plat_now_ms() - t0);
    svc_set_limit(svc_runtime_limit());
    {   // shader-prepared.bin: one record per input id, default variants first
        char out[PATH_CAP + 32];
        snprintf(out, sizeof out, "%s/shader-prepared.bin", dir);
        size_t u = 0;
        uint8_t *recs = calloc(p.prp_n + 1, PRP_REC);
        Set64 seen = {0};
        for (int prio = 0; recs && prio < 2; prio++)
            for (size_t i = 0; i < p.prp_n; i++) {
                const uint8_t *r = p.prp + i * PRP_REC;
                if (r[1] != prio || !set_add(&seen, hash8(r + 4))) continue;
                memcpy(recs + u++ * PRP_REC, r, PRP_REC);
            }
        free(seen.v);
        RecHeader h = {{'T', 'O', 'T', 'K', 'S', 'P', 'R', 'P'}, PRP_VERSION, PRP_REC};
        uint8_t *file = recs ? malloc(sizeof h + u * PRP_REC + 1) : NULL;
        if (file) {
            memcpy(file, &h, sizeof h), memcpy(file + sizeof h, recs, u * PRP_REC);
            if (!plat_file_write_atomic(out, file, sizeof h + u * PRP_REC)) rt_log("gfx_shader: cannot write %s", out);
        }
        rt_log("gfx_shader: %zu prepared variants listed in %s", u, out);
        free(file), free(recs);
    }
    free(p.prp);
    char msg[320];
    snprintf(msg, sizeof msg, "gfx_shader: prepared %zu NVN 2 stages: %lld translated, %lld already cached, %lld failed to "
             "translate, %lld did not lower; %lld integer-attribute variants from %d patterns; %.0f ms", p.n,
             (long long)p.translated, (long long)(p.done - p.translated - p.failed - p.unlowered), (long long)p.failed,
             (long long)p.unlowered, (long long)p.variants, p.npat, (double)plat_now_ms() - t0);
    rt_log("%s", msg);
    printf("%s\n", msg);
    free(p.rec), free(buf);
    return 0;
}

int gfx_shader_prepare_seed(const char *dir, int threads, int procs) {
    char ans[PATH_CAP + 32], prg[PATH_CAP + 32], out[PATH_CAP + 32], n2[PATH_CAP + 32];
    snprintf(n2, sizeof n2, "%s/seed-nvn2.bin", dir);
    if (plat_file_exists(n2)) {
        int rc = prepare_nvn2(n2, dir, threads, procs);
        snprintf(ans, sizeof ans, "%s/seed-answers.bin", dir);
        if (rc || !plat_file_exists(ans)) return rc;   // no MacNativeGames-style seed beside it: done
    }
    snprintf(ans, sizeof ans, "%s/seed-answers.bin", dir);
    snprintf(prg, sizeof prg, "%s/seed-programs.bin", dir);
    snprintf(out, sizeof out, "%s/shader-prepared.bin", dir);
    double t0 = (double)plat_now_ms();
    uint64_t fail0 = (uint64_t)n_failures;
    uint8_t *ab = NULL, (*iid)[32] = NULL;
    long long n = prepare_files(ans, prg, threads, procs, "seed variants", &ab, &iid);
    if (n < 0) return 1;
    uint8_t *recs = calloc((size_t)n + 1, PRP_REC);
    static const uint8_t zero[32];
    size_t u = 0, by[3] = {0}, stages[6] = {0};
    Set64 seen = {0};
    for (long long i = 0; recs && i < n; i++) {           // seed records come in priority order: the first id wins
        if (!memcmp(iid[i], zero, 32) || !set_add(&seen, hash8(iid[i]))) continue;
        uint8_t *r = recs + u * PRP_REC;
        const uint8_t *a = ab + (size_t)i * ANS_REC;
        r[0] = a[0] < 6 ? a[0] : 0, r[1] = a[ANS_REC - 1] > 2 ? 2 : a[ANS_REC - 1];
        memcpy(r + 4, iid[i], 32);
        by[r[1]]++, stages[r[0]]++, u++;
    }
    free(seen.v);
    RecHeader h = {{'T', 'O', 'T', 'K', 'S', 'P', 'R', 'P'}, PRP_VERSION, PRP_REC};
    uint8_t *file = recs ? malloc(sizeof h + u * PRP_REC + 1) : NULL;
    int ok = file != NULL;
    if (ok) {
        memcpy(file, &h, sizeof h), memcpy(file + sizeof h, recs, u * PRP_REC);
        ok = plat_file_write_atomic(out, file, sizeof h + u * PRP_REC);
    }
    char msg[640];
    snprintf(msg, sizeof msg, "gfx_shader: prepared %zu shader variants (vertex %zu, fragment %zu, compute %zu; title %zu, "
             "warm-up %zu, other %zu) from %lld seed records, %llu translations failed, in %.0f ms; %s %s", u,
             stages[GFX_STAGE_VS], stages[GFX_STAGE_FS], stages[GFX_STAGE_CS], by[0], by[1], by[2], n,
             (unsigned long long)((uint64_t)n_failures - fail0), (double)plat_now_ms() - t0,
             ok ? "wrote" : "cannot write", out);
    rt_log("%s", msg);
    printf("%s\n", msg);
    free(file), free(recs), free(iid), free(ab);
    return ok ? 0 : 1;
}

// The variants the boot prebuild warms, most likely first: every input id play recorded (shader-answers.bin, priority
// 0), then the prepared ones (shader-prepared.bin, priority 1 + their list priority). One entry per input id.
size_t gfx_shader_warm_list(GfxWarmEnt **out) {
    *out = NULL;
    if (!RC.on) return 0;
    char prp[PATH_CAP + 32];
    snprintf(prp, sizeof prp, "%.*sshader-prepared.bin", (int)(strlen(RC.ans_path) - strlen("shader-answers.bin")),
             RC.ans_path);
    uint8_t *ab = NULL, *pb = NULL;
    size_t an = 0, pn = 0;
    plat_mutex_lock(&rec_io);                      // no append meanwhile
    answers_read(RC.ans_path, &ab, &an, 0);
    plat_mutex_unlock(&rec_io);
    rec_read(prp, "TOTKSPRP", PRP_VERSION, PRP_REC, &pb, &pn);
    size_t na = an / ANS_REC, np = pn / PRP_REC, n = 0;
    GfxWarmEnt *e = calloc(na + np + 1, sizeof *e);
    Set64 seen = {0};
    for (size_t i = 0; e && i < na; i++) {
        const uint8_t *r = ab + i * ANS_REC;
        if (!set_add(&seen, hash8(r + 36))) continue;
        memcpy(e[n].iid, r + 36, 32), e[n].stage = r[0], e[n].prio = 0, n++;
    }
    for (size_t i = 0; e && i < np; i++) {
        const uint8_t *r = pb + i * PRP_REC;
        if (!set_add(&seen, hash8(r + 4))) continue;
        memcpy(e[n].iid, r + 4, 32), e[n].stage = r[0], e[n].prio = (uint8_t)(1 + r[1]), n++;
    }
    free(seen.v), free(ab), free(pb);
    rt_log("gfx_shader: warm list: %zu variants (%zu met in play, %zu prepared)", n, na, np);
    *out = e;
    return n;
}

// ---- compute prefetch (gfx_shader_prefetch_cs)
// A compute stage's first dispatch otherwise loads its shader from the disk cache inside the replay (a compute shader
// is ~1 MB of SPIR-V: 5-20 ms each, 100-650 ms frames in a race) and then waits for its pipeline. Programs are made
// well before they dispatch (nvnProgramSetShaders, mostly while a course loads), and nearly every compute program has
// exactly one stored input, so a background thread finds the stored input with the same control and code, loads it
// into the memory cache and hands it to the back end (gfx_shader_set_prefetch_sink) to build its pipeline. A dispatch
// whose answers differ from the stored ones loads its own shader as before. KAR_CS_PREFETCH=0 turns it off;
// KAR_CS_PREFETCH_MB (default 128: the SPIR-V waiting for its pipeline) caps the SPIR-V it loads.
// ---- compute used list (<cache>/cs-used.bin): which compute shaders dispatches used, per session
// Prefetching every stored compute input built ~5000 pipelines, of which a race used ~100, at ~0.85 MB of driver
// memory each (16.10). So the prefetch takes only the inputs a dispatch used in one of the last CSU_KEEP sessions; a
// shader met for the first time loads at its dispatch as before and is in the list from then on. No file: nothing is
// prefetched (KAR_CS_PREFETCH_ALL=1 takes every stored input). Layout: "KCSU", u32 version 1, u32 session, u32 count,
// then count entries of an input id (32 bytes) and the u32 session that last used it.
#define CSU_KEEP 8
typedef struct { uint8_t iid[32]; uint32_t session; } CsuEnt;
static struct {
    PlatMutex m;
    CsuEnt *v;                                             // the file's entries, sorted by iid, then this session's
    size_t n, n_file, cap;
    uint32_t session;
    int loaded, dirty;
} CSU;

static int csu_cmp(const void *x, const void *y) { return memcmp(x, y, 32); }

static void csu_path(char *p) { snprintf(p, PATH_CAP + 80, "%s/cs-used.bin", cache_dir); }

static void csu_load(void) {
    char p[PATH_CAP + 80];
    csu_path(p);
    uint64_t n = 0;
    uint8_t *f = read_file(p, &n);
    uint32_t head[4] = {0};
    if (f && n >= 16) memcpy(head, f, 16);
    if (f && head[0] == 0x5553434bu && head[1] == 1 && n == 16 + (uint64_t)head[3] * sizeof(CsuEnt)) {
        CSU.v = malloc((head[3] + 1024) * sizeof(CsuEnt));
        if (CSU.v) {
            memcpy(CSU.v, f + 16, (size_t)head[3] * sizeof(CsuEnt));
            CSU.n = CSU.n_file = head[3], CSU.cap = head[3] + 1024, CSU.session = head[2] + 1;
            qsort(CSU.v, CSU.n, sizeof(CsuEnt), csu_cmp);
        }
    } else {
        CSU.session = 1;
    }
    free(f);
    CSU.loaded = 1;
}

static CsuEnt *csu_find_file(const uint8_t iid[32]) {
    return CSU.n_file ? bsearch(iid, CSU.v, CSU.n_file, sizeof(CsuEnt), csu_cmp) : NULL;
}

// A dispatch used cs (vk_exec_draw.c shader_for): its input enters this session's list once.
void gfx_shader_note_cs_used(const GfxShader *cs) {
    if (!cs || cs->stage != GFX_STAGE_CS || cs->used_noted) return;
    if (plat_xchg32(&((GfxShader *)cs)->used_noted, 1)) return;
    plat_mutex_lock(&CSU.m);
    if (CSU.loaded) {
        CsuEnt *e = csu_find_file(cs->iid);
        if (e) {
            if (e->session != CSU.session) e->session = CSU.session, CSU.dirty = 1;
        } else {
            if (CSU.n == CSU.cap) {
                size_t c = CSU.cap ? CSU.cap * 2 : 1024;
                CsuEnt *t = realloc(CSU.v, c * sizeof *t);
                if (t) CSU.v = t, CSU.cap = c;
            }
            if (CSU.n < CSU.cap) memcpy(CSU.v[CSU.n].iid, cs->iid, 32), CSU.v[CSU.n++].session = CSU.session, CSU.dirty = 1;
        }
    }
    plat_mutex_unlock(&CSU.m);
}

// Writes the list (entries used in the last CSU_KEEP sessions) when it changed. The prefetch thread calls it.
static void csu_save(void) {
    plat_mutex_lock(&CSU.m);
    if (!CSU.dirty || !cache_dir[0]) { plat_mutex_unlock(&CSU.m); return; }
    CSU.dirty = 0;
    size_t keep = 0;
    CsuEnt *out = malloc((CSU.n + 1) * sizeof *out);
    for (size_t i = 0; out && i < CSU.n; i++)
        if (CSU.v[i].session + CSU_KEEP > CSU.session) out[keep++] = CSU.v[i];
    uint32_t session = CSU.session;
    plat_mutex_unlock(&CSU.m);
    if (!out) return;
    size_t bytes = 16 + keep * sizeof *out;
    uint8_t *buf = malloc(bytes);
    if (buf) {
        uint32_t head[4] = {0x5553434bu, 1, session, (uint32_t)keep};
        memcpy(buf, head, 16), memcpy(buf + 16, out, keep * sizeof *out);
        char p[PATH_CAP + 80];
        csu_path(p);
        plat_file_write_atomic(p, buf, bytes);
        free(buf);
    }
    free(out);
}

typedef struct { uint64_t h; uint8_t iid[32]; } PfEnt;
static struct {
    PlatMutex m;
    PlatCond c;
    int started, indexed;
    const uint8_t *(*q)[2];                                // control, code: grows, never drops a program
    uint32_t head, tail, cap_q;
    PfEnt *ix;
    size_t n;
    uint64_t bytes, cap, jobs, loaded, missed, capped;
} PF;
static void (*pf_sink)(const GfxShader *);

void gfx_shader_set_prefetch_sink(void (*sink)(const GfxShader *)) { pf_sink = sink; }

static uint64_t pf_hash(const uint8_t *control, const uint8_t *code, uint32_t length) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < CONTROL_SIZE; i++) h = (h ^ control[i]) * 0x100000001b3ull;
    for (uint32_t i = 0; i < length; i++) h = (h ^ code[i]) * 0x100000001b3ull;
    return h;
}

static int pf_cmp(const void *x, const void *y) {
    uint64_t a = ((const PfEnt *)x)->h, b = ((const PfEnt *)y)->h;
    return a < b ? -1 : a > b;
}

static int pf_collect(const char *file, void *ctx) {
    (void)ctx;
    char name[80];
    uint8_t iid[32];
    if (strlen(file) != 67) return 1;
    memcpy(name, file, 64), name[64] = 0;
    if (!hex32(name, iid)) return 1;
    Inputs in;
    if (!in_load(iid, &in)) return 1;
    static int all = -1;
    if (all < 0) all = getenv("KAR_CS_PREFETCH_ALL") && *getenv("KAR_CS_PREFETCH_ALL") == '1';
    CsuEnt *u = all ? NULL : csu_find_file(iid);
    if (in.stage == GFX_STAGE_CS && (all || (u && u->session + CSU_KEEP > CSU.session))) {
        if (PF.n % 1024 == 0) {
            PfEnt *t = realloc(PF.ix, (PF.n + 1024) * sizeof *t);
            if (!t) return free(in.buf), 0;
            PF.ix = t;
        }
        PF.ix[PF.n].h = pf_hash(in.control, in.code, in.length);
        memcpy(PF.ix[PF.n].iid, iid, 32);
        PF.n++;
    }
    free(in.buf);
    return 1;
}

// The stored compute inputs by control and code hash: the pack's, then the loose in/ files.
static void pf_index(void) {
    uint64_t t0 = plat_now_ns();
    char name[68], dir[PATH_CAP + 80];
    for (uint32_t i = 0; i < pack_n; i++) {
        if (pack_ix[i].kind != PACK_IN) continue;
        key_hex(pack_ix[i].name, name);
        memcpy(name + 64, ".in", 4);
        if (!pf_collect(name, NULL)) break;
    }
    snprintf(dir, sizeof dir, "%s/in", cache_dir);
    plat_file_list(dir, ".in", pf_collect, NULL);
    if (PF.n) qsort(PF.ix, PF.n, sizeof *PF.ix, pf_cmp);
    rt_log("gfx_shader: compute prefetch: %zu stored compute inputs indexed in %.0f ms (session %u, %zu in the used list)",
           PF.n, (plat_now_ns() - t0) / 1e6, CSU.session, CSU.n_file);
}

static void pf_worker(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    paths();
    plat_mutex_lock(&CSU.m);
    if (cache_dir[0]) csu_load();
    plat_mutex_unlock(&CSU.m);
    if (cache_dir[0]) pf_index();
    for (;;) {
        plat_mutex_lock(&PF.m);
        while (PF.head == PF.tail) {
            plat_mutex_unlock(&PF.m);
            csu_save();   // every ~5 s while idle, when this session used a compute shader the list did not have
            plat_mutex_lock(&PF.m);
            if (PF.head == PF.tail) plat_cond_wait_ns(&PF.c, &PF.m, 5000000000ull);
        }
        const uint8_t *control = PF.q[PF.head][0], *code = PF.q[PF.head][1];
        if (++PF.head == PF.tail) PF.head = PF.tail = 0;
        plat_mutex_unlock(&PF.m);
        uint32_t length;
        memcpy(&length, control + 0x704, 4);
        PfEnt k = {.h = pf_hash(control, code, length)};
        PfEnt *e = PF.n ? bsearch(&k, PF.ix, PF.n, sizeof k, pf_cmp) : NULL;
        while (e && e > PF.ix && e[-1].h == k.h) e--;
        int found = 0;
        for (; e && e < PF.ix + PF.n && e->h == k.h; e++) {
            // At the cap, wait for the pipeline builders to drop held SPIR-V (gfx_shader_drop_spirv) rather than skip:
            // a skipped program loads its shader inside the replay again. 30 s without progress gives up on it.
            for (int w = 0; pf_resident >= (int64_t)PF.cap && w < 6000; w++) plat_sleep_ns(5000000);
            if (pf_resident >= (int64_t)PF.cap) { PF.capped++; break; }
            const GfxShader *s = gfx_shader_load_input(e->iid, 0);
            if (!s) continue;
            found = 1;
            if (s->spirv) plat_add64(&pf_resident, (int64_t)(s->words * 4));   // the sink may drop it again
            PF.bytes += s->words * 4, PF.loaded++;
            if (pf_sink) pf_sink(s);
        }
        if (!found && !(e && e < PF.ix + PF.n && e->h == k.h)) PF.missed++;
        if (++PF.jobs % 64 == 0)
            rt_log("gfx_shader: compute prefetch: %llu programs, %llu shaders loaded (%.0f MB, %.0f MB still held), %llu without a stored input, %llu given up at the cap",
                   (unsigned long long)PF.jobs, (unsigned long long)PF.loaded, PF.bytes / 1e6, pf_resident / 1e6,
                   (unsigned long long)PF.missed, (unsigned long long)PF.capped);
    }
}

void gfx_shader_prefetch_cs(const uint8_t *control, const uint8_t *code) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("KAR_CS_PREFETCH"), *mb = getenv("KAR_CS_PREFETCH_MB");
        on = !(e && e[0] == '0');
        PF.cap = (mb ? strtoull(mb, NULL, 10) : 128) << 20;
    }
    if (!on || !control || !code) return;
    plat_mutex_lock(&PF.m);
    if (PF.tail == PF.cap_q) {
        uint32_t c = PF.cap_q ? PF.cap_q * 2 : 4096;
        const uint8_t *(*t)[2] = realloc(PF.q, (size_t)c * sizeof *t);
        if (t) PF.q = t, PF.cap_q = c;
    }
    if (PF.tail < PF.cap_q) {
        PF.q[PF.tail][0] = control, PF.q[PF.tail][1] = code;
        PF.tail++;
        if (!PF.started) PF.started = 1, plat_thread_spawn(pf_worker, NULL, "shader prefetch", 0);
        plat_cond_signal(&PF.c);
    }
    plat_mutex_unlock(&PF.m);
}

// KAR_REPLAY_PROF: why a compute dispatch still loads from disk: no stored input with its code, its prefetch job not
// run yet (queued behind others), or run but the dispatch's answers name another shader. Counts in gfx_pf_why[3].
uint64_t gfx_pf_why[3];
static void pf_note_miss(const uint8_t *control, const uint8_t *code, uint32_t length, const uint8_t *iid, int final, const uint8_t *rkey) {
    if (!PF.started || !PF.n) return;
    PfEnt k = {.h = pf_hash(control, code, length)};
    PfEnt *e = bsearch(&k, PF.ix, PF.n, sizeof k, pf_cmp);
    if (!e) { gfx_pf_why[0]++; return; }
    plat_mutex_lock(&PF.m);
    int queued = 0;
    for (uint32_t i = PF.head; i != PF.tail && !queued; i++) queued = PF.q[i][1] == code;
    uint32_t depth = PF.tail - PF.head;
    plat_mutex_unlock(&PF.m);
    gfx_pf_why[queued ? 1 : 2]++;
    static int told;
    int same = 0, iid_known = 0;
    for (PfEnt *f = e; f > PF.ix && f[-1].h == k.h; f--) e = f - 1;
    for (PfEnt *f = e; f < PF.ix + PF.n && f->h == k.h; f++) same++, iid_known |= !memcmp(f->iid, iid, 32);
    for (size_t i = 0; i < PF.n && !iid_known; i++) iid_known = 2 * !memcmp(PF.ix[i].iid, iid, 32);
    uint8_t key[32];
    key_of_input(iid, key);
    if (told < 12) {
        GfxShader *d = disk_load(key, iid, REFLECT_LOWERED_BODY);
        rt_log("gfx_shader: compute miss: request key %s input key; disk_load(input key) %s; input_matches_cached %d",
               memcmp(key, rkey, 32) ? "differs from" : "equals", d ? "ok" : "fails", d ? input_matches_cached(d, iid) : -1);
        if (d) shader_free(d);
    }
    if (told++ < 12)
        rt_log("gfx_shader: compute miss: %s; %d stored inputs with this code; its input id %s; final %d; in memory under its input key %d",
               queued ? "prefetch still queued" : "prefetched", same,
               iid_known == 1 ? "among them" : iid_known == 2 ? "stored under other code" : "not stored", final, mem_find(key) != NULL);
}

// The memory cache's shaders and the SPIR-V bytes they still hold (vk_memory.c census).
void gfx_shader_mem_stats(uint64_t *n, uint64_t *bytes) {
    *n = *bytes = 0;
    plat_mutex_lock_shared(&cache_lock);
    for (size_t i = 0; i < BUCKETS; i++)
        for (const GfxShader *s = buckets[i]; s; s = s->next) {
            (*n)++;
            if (s->spirv) *bytes += s->words * 4;
        }
    plat_mutex_unlock_shared(&cache_lock);
}
