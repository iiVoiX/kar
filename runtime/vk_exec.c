// The op-stream replay core of the Vulkan back end: walks the command handles of
// one submission into its command buffer, keeps render targets and fixed-function state, opens and closes dynamic
// rendering passes and records clears and barriers. Draws, binds and dispatches go to vk_exec_op_draw
// (vk_exec_draw.c); copies, counters, events and conditional rendering to vk_exec_op_xfer (vk_exec_xfer.c). Images
// stay in VK_IMAGE_LAYOUT_GENERAL (vk_texture.c); full memory barriers order passes, clears and whatever those two
// record between them.
//
// The op loop (END / BEGIN / JUMP / CALL, which ops are pure state, the state defaults, render targets kept until they
// change, clears skipped while rendering is disabled) follows MacNativeGames
// games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m:10588-10700, 7409-7460, 5449-5457
// (https://github.com/ly05010419/MacNativeGames @ b75be8cfa704df3701e86130c8ced268e79d6b11, MIT, Copyright (c) 2026
// jhaizhou). Pass begin and end (GENERAL-layout attachments, LOAD/STORE, render area from the smallest attachment, a
// full barrier before each pass), the scissor clip and attachment clears follow LAS Recomp
// runtime/nvn/vulkan/exec.c:855-1149, 3553-3778 (https://github.com/peiikai/LASRecomp @
// da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai).
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "platform/platform.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"

// The newest depth target a pass used (vk_exec_pass_begin), for vk_exec_depth_newest.
static VkImage dn_image;
static VkFormat dn_format;
static uint32_t dn_w, dn_h;
static uint64_t dn_frame;

#define WEAK __attribute__((weak))
#define CALL_DEPTH 8

typedef struct {
    VkImage image;
    VkFormat format;
    VkImageViewType type;
    VkImageAspectFlags aspect;
    uint32_t level, base, layers;
    VkImageView view;
} ViewEnt;

static VkExec X;
static int x_ready;
static _Thread_local int x_here;   // this thread is inside vk_exec_record (vk_exec_inline_begin)

// ---- TOTK_TRACE=gfx frame counters

VkExecFrame vk_exec_frame;
static struct { const uint8_t *mem; uint32_t w, h, fmt, n; } fw[64];   // textures written this frame
static int nfw;

uint64_t vk_exec_recording_fence(void) { return x_here ? X.fence : 0; }

void vk_exec_frame_written(const tex_t *t) {
    if (!(rt_trace & RT_TRACE_GFX) || !t) return;
    for (int i = 0; i < nfw; i++)
        if (fw[i].mem == t->mem) { fw[i].n++; return; }
    if (nfw == 64) return;
    fw[nfw].mem = t->mem, fw[nfw].w = t->d.width, fw[nfw].h = t->d.height, fw[nfw].fmt = (uint32_t)t->d.format;
    fw[nfw++].n = 1;
}

void vk_exec_frame_log(const tex_t *p, uint64_t frame) {
    if (!(rt_trace & RT_TRACE_GFX)) return;
    VkExecFrame *f = &vk_exec_frame;
    int hit = 0;
    char list[512] = "";
    size_t o = 0;
    for (int i = 0; i < nfw; i++) {
        if (p && fw[i].mem == p->mem) hit = 1;
        if (o < sizeof list - 48)
            o += (size_t)snprintf(list + o, sizeof list - o, " %p:%ux%u/%#x*%u", (const void *)fw[i].mem, fw[i].w,
                                  fw[i].h, fw[i].fmt, fw[i].n);
    }
    rt_log("frame %llu: submits %llu passes %llu draws %llu/%llu clears %llu copies %llu dispatches %llu; present %p "
           "%ux%u/%#x written %d; feedback draws %llu barriers %llu restarts %llu layouts %llu; targets%s",
           (unsigned long long)frame, (unsigned long long)f->submits,
           (unsigned long long)f->passes, (unsigned long long)f->draws, (unsigned long long)f->draws_seen,
           (unsigned long long)f->clears, (unsigned long long)f->copies, (unsigned long long)f->dispatches,
           p ? (const void *)p->mem : NULL, p ? p->d.width : 0, p ? p->d.height : 0, p ? (uint32_t)p->d.format : 0,
           hit, (unsigned long long)f->fb_draws, (unsigned long long)f->fb_barriers, (unsigned long long)f->fb_restarts,
           (unsigned long long)f->fb_layouts, list);
    memset(f, 0, sizeof *f);
    nfw = 0;
}

WEAK void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

WEAK void vk_exec_op_xfer(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

enum { W_UNRENDERABLE, W_LEVEL, W_LAYER, W_VIEW_FORMAT, W_BLOCK_VIEW, W_BLOCK_LAYERS, W_CLEAR_TARGET, W_CLEAR_MASK, W_COUNT };

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

// NVN defaults of the replay state (nvn_mtl.m).
static void state_reset(VkExec *ex) {
    memset(ex, 0, sizeof *ex);
    ex->st_vmask[0] = ex->st_vmask[1] = ex->st_wmask[0] = ex->st_wmask[1] = 0xff;
    ex->depth_range[1] = 1.0f;
    vk_pipe_state_defaults(&ex->pipe);
    ex->barrier_at = -1;
}

// ---- render targets

static uint32_t level_extent(uint32_t v, uint32_t level) {
    v >>= level;
    return v ? v : 1;
}

// A view format other than the texture's is used when both map directly and have the same texel size (the image is
// created MUTABLE_FORMAT for colour formats). blocks: t is a BC texture with block_views, and the view an uncompressed
// format whose texel is one of its blocks.
static int view_format(const tex_t *t, const view_t *v, int blocks, VkFormat *out) {
    TexVkFormat a, b;
    int bw, bh, vbw, vbh;
    if (!tex_vk_format(t->d.format, &a) || a.vk != a.ideal || !tex_vk_format(v->format, &b) || b.vk != b.ideal ||
        b.depth || b.stencil)
        return 0;
    int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh), vbpb = nvn_fmt_bpb(v->format, &vbw, &vbh);
    if (!bpb || bpb != vbpb || (!blocks && (bw != 1 || bh != 1)) || vbw != 1 || vbh != 1) return 0;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, b.vk, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return 0;
    *out = b.vk;
    return 1;
}

// ---- 3D BC render targets
// A 3D BC texture has no block views (vk_texture.c create_tex, TOTK_BLOCK_VIEWS_3D): on RDNA2 cards such an image
// made BLOCK_TEXEL_VIEW_COMPATIBLE and 2D_ARRAY_COMPATIBLE took GPU writes past the end of its memory (issue #5). Draws
// into it through an uncompressed view of its blocks (agl's BC4 encoder fills 32x32x32 and 64x64x64 volumes and their
// levels at the logo) go to a stand-in: a 2D array of the blocks of one level in the view's format, one layer per
// depth plane. It starts with the level's content, so a pass that writes part of a plane keeps the rest, and each pass
// end copies the planes it drew into the level (a copy between a compressed and a size-compatible uncompressed format).
typedef struct { VkImage bc, img; VmaAllocation alloc; VkFormat fmt; uint32_t level, w, h, d; } BcStandIn;
#define BC_STANDINS 64   // one per level drawn: 6 volumes of up to 7 levels at the logo
static BcStandIn bcs[BC_STANDINS];
static int nbcs;
static PlatMutex bcs_lock = PLAT_MUTEX_INIT;   // the replay adds and reads; destroy_tex (any thread) removes

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, uint32_t layers) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static void memory_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                           VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// The stand-in of level level of a 3D BC image for draws through view format fmt, made on first use; 0 when none can
// be made (logged). Replay thread.
static int bc_standin(const VkTexImage *img, uint32_t level, VkFormat fmt, BcStandIn *out) {
    static int warned;
    plat_mutex_lock(&bcs_lock);
    int full = nbcs == BC_STANDINS;
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == img->image && bcs[i].level == level && bcs[i].fmt == fmt) {
            *out = bcs[i];
            plat_mutex_unlock(&bcs_lock);
            return 1;
        }
    plat_mutex_unlock(&bcs_lock);
    if (full) {
        if (!warned++) rt_log("vk: more than %d 3D BC render targets: the others are left out", BC_STANDINS);
        return 0;
    }
    uint32_t lw = level_extent(img->width, level), lh = level_extent(img->height, level);
    BcStandIn s = {img->image, VK_NULL_HANDLE, NULL, fmt, level, (lw + 3) / 4, (lh + 3) / 4,
                   level_extent(img->depth, level)};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {s.w, s.h, 1}, .mipLevels = 1, .arrayLayers = s.d, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(vk_ctx()->vma, &ci, &ai, &s.img, &s.alloc, NULL) != VK_SUCCESS) {
        if (!warned++) rt_log("vk: stand-in for a 3D BC render target could not be created: left out");
        return 0;
    }
    // In the upload batch, which goes to the queue before this part: GENERAL, then the texture's blocks.
    VkCommandBuffer cb = vk_upload_begin();
    image_barrier(cb, s.img, VK_IMAGE_LAYOUT_UNDEFINED, s.d);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, s.d},
                      .extent = {lw, lh, s.d}};   // texels of the compressed source
    vkCmdCopyImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, s.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    vk_upload_end();
    plat_mutex_lock(&bcs_lock);
    bcs[nbcs++] = s;
    plat_mutex_unlock(&bcs_lock);
    rt_log("vk: 3D BC render target %ux%ux%u level %u drawn through a %ux%u x %u stand-in (format %d)", img->width,
           img->height, img->depth, level, s.w, s.h, s.d, (int)fmt);
    *out = s;
    return 1;
}

// vk_exec_draw.c vk_exec_image_gone: the stand-in of a BC image that goes, freed once the GPU is done with it.
void vk_exec_bc_gone(VkImage image) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == image) {
            vk_defer_image(bcs[i].img, bcs[i].alloc);
            bcs[i--] = bcs[--nbcs];
        }
    plat_mutex_unlock(&bcs_lock);
}

// At a pass end: the planes the pass drew into a stand-in go to its texture.
static void bc_copy_back(VkExec *ex, const VkExecTarget *t) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++) {
        if (bcs[i].img != t->image) continue;
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->base_layer, t->layers},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, bcs[i].level, 0, 1},
                          .dstOffset = {0, 0, (int32_t)t->base_layer},
                          .extent = {bcs[i].w, bcs[i].h, t->layers}};   // texels of the uncompressed source
        vkCmdCopyImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, bcs[i].bc, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        ex->recorded++;
        break;
    }
    plat_mutex_unlock(&bcs_lock);
}

// A draw into a 3D BC image without block views through an uncompressed view of its blocks: the target is the
// image's stand-in (bc_standin). 1 when o is filled (target or left out), 0 for any other target.
static int bc_target(const tex_t *t, const view_t *v, int has_v, const VkTexImage *img, VkExecTarget *o) {
    int bw, bh;
    if (img->type != VK_IMAGE_TYPE_3D || img->block_views || !nvn_fmt_bpb(t->d.format, &bw, &bh) || bw != 4 || bh != 4)
        return 0;
    VkFormat fmt;
    BcStandIn s;
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return 1;
    }
    if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt) || !bc_standin(img, level, fmt, &s)) {
        warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                  t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
        return 1;
    }
    uint32_t base = 0, layers = s.d;
    if (v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < s.d ? s.d - base : 0);
    }
    if (base >= s.d || !layers) return 1;
    if (base + layers > s.d) layers = s.d - base;
    o->image = s.img;   // not img->image: view_of makes this command buffer's view of the stand-in
    o->img = img;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = 0, o->base_layer = base, o->layers = layers;
    o->width = s.w, o->height = s.h, o->samples = 1;
    o->nvn_format = v->format;
    return 1;
}

static void resolve_target(const tex_t *t, const view_t *v, int has_v, int depth_slot, VkExecTarget *o) {
    memset(o, 0, sizeof *o);
    o->tex = t;
    if (!t) return;
    const VkTexImage *img = vk_texture_get(t);
    if (img && !depth_slot && bc_target(t, v, has_v, img, o)) return;   // a 3D BC image: its stand-in
    VkImageUsageFlags need = depth_slot ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    int is3d = img && img->type == VK_IMAGE_TYPE_3D && !depth_slot;   // slices drawn through 2D views (2D_ARRAY_COMPATIBLE)
    if (!img || !(img->usage & need) || (img->type != VK_IMAGE_TYPE_2D && !is3d)) {
        warn_once(W_UNRENDERABLE, "render target format %#x target %d (%ux%u) is not renderable here: left out",
                  t->d.format, t->d.target, t->d.width, t->d.height);
        return;
    }
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return;
    }
    // A 3D level's depth planes take the place of layers (Vulkan views them as the array layers of a 2D array view).
    uint32_t nlay = is3d ? level_extent(img->depth, level) : img->layers;
    uint32_t base = 0, layers = nlay;
    if (has_v && v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < nlay ? nlay - base : 0);
    }
    if (base >= nlay || !layers) {
        warn_once(W_LAYER, "render target layer %u of a %u-layer texture: left out", base, nlay);
        return;
    }
    if (base + layers > nlay) layers = nlay - base;
    VkFormat fmt = img->format;
    int32_t nvn_fmt = t->d.format;
    int blocks = !depth_slot && img->block_views;
    if (blocks) {   // a BC texture takes draws only through an uncompressed view of its blocks
        if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt)) {
            warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                      t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
            return;
        }
        nvn_fmt = v->format;
        if (layers > 1) {   // an uncompressed view of a BC image has one layer (no maintenance6 here)
            warn_once(W_BLOCK_LAYERS, "render target format %#x: block view of %u layers, first layer only", t->d.format,
                      layers);
            layers = 1;
        }
    } else if (!depth_slot && has_v && v->set_format && v->format != t->d.format) {
        if (view_format(t, v, 0, &fmt)) nvn_fmt = v->format;
        else warn_once(W_VIEW_FORMAT, "render target view format %#x of a format %#x texture: texture format used",
                       v->format, t->d.format);
    }
    o->image = img->image;
    o->img = img;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = depth_slot ? img->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                           : VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = level, o->base_layer = base, o->layers = layers;
    o->is3d = (uint8_t)is3d;
    o->blocks = (uint8_t)blocks;
    o->width = level_extent(img->width, level), o->height = level_extent(img->height, level);
    if (blocks) o->width = (o->width + 3) / 4, o->height = (o->height + 3) / 4;   // the view's extent: 4x4 blocks
    o->samples = img->samples ? img->samples : 1;
    o->nvn_format = nvn_fmt;
    if (!o->aspect) o->image = VK_NULL_HANDLE;
}

/* The size of a pass without attachments: the far corner of viewport 0, else of the scissor, 1..16384, as
   MacNativeGames and nxnative size it. TotK draws a fragment shader that only writes a storage buffer into a 1x1
   viewport; a pass sized by the viewport's width alone puts an offset viewport outside the scissor, and the cave rock
   loses its ambient light. */
void vk_exec_noatt_dims(const VkExec *ex, uint32_t *w, uint32_t *h) {
    double x1 = 0, y1 = 0;
    if (ex->has_vp) {
        x1 = ceil(fmax(ex->vp[0][0], ex->vp[0][0] + ex->vp[0][2]));
        y1 = ceil(fmax(ex->vp[0][1], ex->vp[0][1] + ex->vp[0][3]));
    }
    if ((x1 < 1 || y1 < 1) && ex->has_scissor) x1 = (double)ex->scissor.x + ex->scissor.w, y1 = (double)ex->scissor.y + ex->scissor.h;
    *w = x1 < 1 ? 1 : x1 > 16384 ? 16384 : (uint32_t)x1;
    *h = y1 < 1 ? 1 : y1 > 16384 ? 16384 : (uint32_t)y1;
}

// Resolves ex->rt to this command buffer's images and derives the pass parameters and the pipeline's target formats.
static void resolve_rt(VkExec *ex) {
    if (ex->rt_resolved) return;
    ex->rt_resolved = 1;
    const p_set_rt *r = &ex->rt;
    int n = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n;
    uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX, samples = 0;
    for (int i = 0; i < 8; i++) {
        VkExecTarget *t = &ex->color[i];
        resolve_target(i < n ? r->color[i] : NULL, &r->cview[i], i < n && r->has_cview[i], 0, t);
        ex->pass_color[i] = t->image ? t->format : VK_FORMAT_UNDEFINED;
        ex->pipe.color_fmt[i] = t->image ? t->nvn_format : 0;
        if (!t->image) continue;
        if (t->width < w) w = t->width;
        if (t->height < h) h = t->height;
        if (t->layers < layers) layers = t->layers;
        if (!samples) samples = t->samples;
    }
    VkExecTarget *d = &ex->depth;
    resolve_target(r->depth, &r->dview, r->has_dview, 1, d);
    ex->pass_depth = d->image && (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pass_stencil = d->image && (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pipe.depth_fmt = d->image ? d->nvn_format : 0;
    if (d->image && d->img && (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && d->samples == 1 && d->level == 0 &&
        d->layers == 1 && !d->is3d) {   // frame generation (vk_fg_mv.c): the newest single-sample 2D depth target
        dn_image = d->img->image, dn_format = d->img->format, dn_w = d->img->width, dn_h = d->img->height;
        dn_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    }
    if (d->image) {
        if (d->width < w) w = d->width;
        if (d->height < h) h = d->height;
        if (d->layers < layers) layers = d->layers;
        if (!samples) samples = d->samples;
    }
    ex->pass_noatt = w == UINT32_MAX;
    if (ex->pass_noatt) {
        vk_exec_noatt_dims(ex, &w, &h);
        layers = 1;
        static int logged;
        if (logged < 8 && ex->has_vp && (ex->vp[0][0] != 0 || ex->vp[0][1] != 0 || ex->vp[0][2] != w || ex->vp[0][3] != h))
            logged++, rt_log("vk_exec: pass without attachments %ux%u for viewport %g,%g %gx%g", w, h, ex->vp[0][0],
                             ex->vp[0][1], ex->vp[0][2], ex->vp[0][3]);
    }
    ex->pass_w = w, ex->pass_h = h, ex->pass_layers = layers;
    ex->pass_ncolor = (uint32_t)n;
    ex->pass_samples = samples ? samples : 1;
    ex->pipe.samples = (uint8_t)ex->pass_samples;
}

// The attachment view of a target. A registered texture's view lives as long as its image (vk_exec_target_view); any
// other image's view is made for this command buffer and freed at its fence.
static VkImageView view_of(VkExec *ex, const VkExecTarget *t) {
    if (t->img && t->img->image == t->image)
        return vk_exec_target_view(t->img, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, t->blocks);
    ViewEnt *v = ex->views;
    for (int i = 0; i < ex->nviews; i++)
        if (v[i].image == t->image && v[i].format == t->format && v[i].type == t->view_type && v[i].aspect == t->aspect &&
            v[i].level == t->level && v[i].base == t->base_layer && v[i].layers == t->layers)
            return v[i].view;
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image,
                                .viewType = t->view_type, .format = t->format,
                                .subresourceRange = {t->aspect, t->level, 1, t->base_layer, t->layers}};
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    if (t->blocks) ci.pNext = &use;   // not the BC image's sampling and transfer usages
    VkImageView view;
    if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS)
        rt_fatal("vk: render target view (format %d, %ux%u) could not be created", (int)t->format, t->width, t->height);
    if (ex->nviews == ex->cap_views) {
        ex->cap_views = ex->cap_views ? ex->cap_views * 2 : 16;
        ex->views = realloc(ex->views, sizeof(ViewEnt) * (size_t)ex->cap_views);
        if (!ex->views) rt_fatal("vk: out of memory");
        v = ex->views;
    }
    v[ex->nviews++] = (ViewEnt){t->image, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, view};
    return view;
}

typedef struct { int n; VkImageView v[]; } ViewList;

static void free_views(void *arg) {   // completion thread, once the submission that used them is done
    ViewList *l = arg;
    VkDevice dev = vk_ctx()->device;
    for (int i = 0; i < l->n; i++) vkDestroyImageView(dev, l->v[i], NULL);
    free(l);
}

// ---- target history
// The presented frame (gfx_counters.frames + 1; 0: never) each target subresource was last rendered into, and the one
// before. prepare_draw lets a draw into a target the previous frame did not render (a one-shot draw: a volume or LUT
// generated once) wait for its shaders and pipeline instead of skipping it. Per subresource, not per image: a volume
// may be generated a few slices per frame. Direct-mapped: a slot another subresource took reads as not rendered, so a
// collision makes a draw wait rather than skip.
typedef struct { VkImage image; uint32_t level, layer; uint64_t last, prev; } TargetFrames;
static TargetFrames tfr[1024];

static TargetFrames *tfr_slot(const VkExecTarget *t) {
    uint64_t h = ((uint64_t)(uintptr_t)t->image ^ (uint64_t)t->level << 56 ^ (uint64_t)t->base_layer << 40) *
                 0x9E3779B97F4A7C15ull;
    return &tfr[h >> 54];
}

static int tfr_match(const TargetFrames *e, const VkExecTarget *t) {
    return e->image == t->image && e->level == t->level && e->layer == t->base_layer;
}

static void tfr_note(const VkExecTarget *t, uint64_t stamp) {
    TargetFrames *e = tfr_slot(t);
    if (!tfr_match(e, t)) *e = (TargetFrames){t->image, t->level, t->base_layer, 0, 0};
    if (e->last != stamp) e->prev = e->last, e->last = stamp;
}

static int tfr_recent(const VkExecTarget *t, uint64_t stamp) {
    const TargetFrames *e = tfr_slot(t);
    return tfr_match(e, t) && (e->last == stamp ? e->prev : e->last) == stamp - 1;
}

int vk_exec_targets_recent(VkExec *ex, int any) {
    resolve_rt(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    int n = 0, recent = 0;
    for (int i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if (!t->image) continue;
        n++;
        if (tfr_recent(t, stamp)) recent++;
        else if (!any) return 0;
    }
    return any ? recent > 0 : n > 0;
}

// One line naming each target of the current pass: format, extent, layer and the two frames it was last rendered in
// (prepare_draw's pipeline wait log).
void vk_exec_targets_describe(VkExec *ex, char *buf, size_t cap) {
    resolve_rt(ex);
    size_t n = 0;
    buf[0] = 0;
    for (int i = 0; i < 9 && n < cap; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if (!t->image) continue;
        const TargetFrames *e = tfr_slot(t);
        int m = tfr_match(e, t);
        int w = snprintf(buf + n, cap - n, "%s%c%d fmt %d %ux%u L%u seen %llu/%llu", n ? "; " : "", i < 8 ? 'c' : 'd',
                         i < 8 ? i : 0, (int)t->format, t->width, t->height, t->base_layer,
                         (unsigned long long)(m ? e->last : 0), (unsigned long long)(m ? e->prev : 0));
        if (w < 0) break;
        n += (size_t)w;
    }
    if (!n) snprintf(buf, cap, "no targets");
}

// ---- passes and barriers

static int has_image(const VkImage *list, int n, VkImage img) {
    for (int i = 0; i < n; i++)
        if (list[i] == img) return 1;
    return 0;
}

static VkImageMemoryBarrier2 layout_barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    return (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                                   .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
}

// Full barrier. The images in the feedback layout become want[] (pass begin) or none (every other barrier): the others
// go back to GENERAL and the new ones leave it, in the same command.
static void barrier_to(VkExec *ex, const VkImage *want, const VkImageAspectFlags *wasp, int nwant) {
    vk_exec_pass_end(ex);
    VkImageMemoryBarrier2 ib[18];
    uint32_t n = 0;
    for (int i = 0; i < ex->nfbl; i++)
        if (!has_image(want, nwant, ex->fbl_img[i]))
            ib[n++] = layout_barrier(ex->fbl_img[i], ex->fbl_aspect[i], VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
                                     VK_IMAGE_LAYOUT_GENERAL);
    for (int i = 0; i < nwant; i++)
        if (!has_image(ex->fbl_img, ex->nfbl, want[i]))
            ib[n++] = layout_barrier(want[i], wasp[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT);
    for (int i = 0; i < nwant; i++) ex->fbl_img[i] = want[i], ex->fbl_aspect[i] = wasp[i];
    ex->nfbl = nwant;
    if (ex->barrier_at == ex->recorded && !n) return;
    vk_exec_frame.fb_layouts += n;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = n, .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->barrier_at = ++ex->recorded;
}

void vk_exec_barrier(VkExec *ex) { barrier_to(ex, NULL, NULL, 0); }

void vk_exec_pass_begin(VkExec *ex) {
    if (ex->in_pass) return;
    resolve_rt(ex);
    // Learned targets (VkTexImage.fb_learned) are attached in the feedback layout
    VkImage want[9];
    VkImageAspectFlags wasp[9];
    int nwant = 0;
    uint32_t fbl = 0;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || !t->image || !t->img || !t->img->fb_learned) continue;
        fbl |= 1u << i;
        if (has_image(want, nwant, t->image)) continue;
        want[nwant] = t->image, wasp[nwant++] = i < 8 ? VK_IMAGE_ASPECT_COLOR_BIT : t->img->aspect;
    }
    barrier_to(ex, want, wasp, nwant);   // passes, clears, copies and dispatches see each other's results
    ex->fbl_mask = fbl;
    ex->fb_dirty = 0;
    ex->pass_serial++;
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    for (uint32_t i = 0; i < ex->pass_ncolor; i++) {
        const VkExecTarget *t = &ex->color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!t->image) continue;
        ca[i].imageView = view_of(ex, t);
        ca[i].imageLayout = fbl >> i & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;    }
    const VkExecTarget *d = &ex->depth;
    if (d->image) {
        VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view_of(ex, d),
                                       .imageLayout = fbl >> 8 & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                                   : VK_IMAGE_LAYOUT_GENERAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        if (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) da = a;
        if (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) sa = a;
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {ex->pass_w, ex->pass_h}},
                          .layerCount = ex->pass_layers, .colorAttachmentCount = ex->pass_ncolor,
                          .pColorAttachments = ca,
                          .pDepthAttachment = ex->pass_depth ? &da : NULL,
                          .pStencilAttachment = ex->pass_stencil ? &sa : NULL};
    vkCmdBeginRendering(ex->cb, &ri);
    vk_exec_frame.passes++;
    ex->recorded++;
    ex->in_pass = 1;
}

// The newest depth target a pass used (replay thread), for frame generation's motion vectors (vk_fg_mv.c): the
// scene's depth is the one of the upscale source's size in the frame of the upscale.
VkImage vk_exec_depth_newest(uint32_t w, uint32_t h, VkFormat *format) {
    if (!dn_image || dn_w != w || dn_h != h || dn_frame != __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED)) return VK_NULL_HANDLE;
    *format = dn_format;
    return dn_image;
}

// The newest single-sample colour render target per format (vk_exec_rt_newest), noted when its pass ends: the
// missing-upscale fallback of vk_exec_draw.c (upscale_fill) blits from it.
#define RT_NEWEST_N 16
static struct { VkImage image; VkFormat format; uint32_t w, h; uint64_t stamp; uint8_t storage; } rt_newest[RT_NEWEST_N];

// Images the replay has drawn into (colour attachments, storage images, copy destinations; clears do not count), with
// the gfx frame of the latest draw. The missing-upscale fallback leaves alone what was drawn in the last few frames.
// Open addressing over the VkImage handle; a full table stops adding new images.
#define DRAWN_N (1u << 16)
static struct { uint64_t h, frame; } drawn_set[DRAWN_N];
static uint32_t drawn_count;

static uint32_t drawn_find(uint64_t h, int add) {
    for (uint32_t i = (uint32_t)((h * 0x9E3779B97F4A7C15ull) >> 48), n = 0; n < DRAWN_N; i = (i + 1) & (DRAWN_N - 1), n++) {
        if (drawn_set[i].h == h) return i;
        if (!drawn_set[i].h) {
            if (!add || drawn_count >= DRAWN_N - DRAWN_N / 4) return UINT32_MAX;
            drawn_set[i].h = h, drawn_count++;
            return i;
        }
    }
    return UINT32_MAX;
}

void vk_exec_note_drawn(VkImage image) {
    uint64_t h = (uint64_t)(uintptr_t)image;
    uint32_t i = h ? drawn_find(h, 1) : UINT32_MAX;
    if (i != UINT32_MAX) drawn_set[i].frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
}

int vk_exec_was_drawn(VkImage image) {   // within the last 8 frames
    uint64_t h = (uint64_t)(uintptr_t)image;
    uint32_t i = h ? drawn_find(h, 0) : UINT32_MAX;
    if (i == UINT32_MAX) return 0;
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    return now - drawn_set[i].frame <= 8;
}

static void rt_note_image(const VkTexImage *img, uint64_t stamp, int storage) {
    if (img->samples != 1 || !(img->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) return;
    int slot = -1;
    for (int i = 0; i < RT_NEWEST_N && slot < 0; i++)
        if (rt_newest[i].format == img->format && rt_newest[i].w == img->width && rt_newest[i].h == img->height) slot = i;
    if (slot < 0) {   // the oldest entry goes
        slot = 0;
        for (int i = 1; i < RT_NEWEST_N; i++)
            if (rt_newest[i].stamp < rt_newest[slot].stamp) slot = i;
    }
    __atomic_store_n(&rt_newest[slot].image, VK_NULL_HANDLE, __ATOMIC_RELEASE);
    rt_newest[slot].format = img->format;
    rt_newest[slot].w = img->width, rt_newest[slot].h = img->height, rt_newest[slot].stamp = stamp;
    rt_newest[slot].storage = (uint8_t)storage;
    __atomic_store_n(&rt_newest[slot].image, img->image, __ATOMIC_RELEASE);
}

static void rt_note_newest(const VkExecTarget *t, uint64_t stamp) {
    if (t->img) vk_exec_note_drawn(t->img->image);
    if (t->image) vk_exec_note_drawn(t->image);
    if (t->img && t->image == t->img->image) rt_note_image(t->img, stamp, 0);
}

// A storage image bound for a dispatch or draw counts as written now. Kirby Air Riders' scene can end in a compute
// pass (a resolve writing the 1536x864 rgb10a2 image); noting only render passes left the newest target of that size
// a G-buffer image (normals in rgb10a2), and the upscale fallback showed it as a rainbow picture for the whole run.
void vk_exec_rt_note_storage(const VkTexImage *img) {
    vk_exec_note_drawn(img->image);
    if (img->type == VK_IMAGE_TYPE_2D) rt_note_image(img, __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1, 1);
}

// A texture's image goes to the dead list when the game drops it (vk_memory.c vk_defer_image, any thread): it must
// not stay a blit source, or a later fallback blit reads a destroyed image (device lost at boot, where textures come
// and go). The slot's handle is cleared atomically; the replay thread re-reads it before use.
void vk_exec_rt_forget(VkImage image) {
    for (int i = 0; i < RT_NEWEST_N; i++)
        if (__atomic_load_n(&rt_newest[i].image, __ATOMIC_ACQUIRE) == image)
            __atomic_store_n(&rt_newest[i].image, VK_NULL_HANDLE, __ATOMIC_RELEASE);
}

VkImage vk_exec_rt_newest(VkFormat format, uint32_t max_w, uint32_t dst_w, uint32_t dst_h, uint64_t min_stamp,
                          uint32_t *w, uint32_t *h) {
    int best = -1;
    for (int i = 0; i < RT_NEWEST_N; i++) {
        if (!__atomic_load_n(&rt_newest[i].image, __ATOMIC_ACQUIRE) || rt_newest[i].format != format ||
            rt_newest[i].w >= max_w || rt_newest[i].stamp < min_stamp)
            continue;
        // same aspect ratio within 2 %
        double a = (double)rt_newest[i].w * dst_h, b = (double)rt_newest[i].h * dst_w;
        if (a > b * 1.02 || b > a * 1.02) continue;
        if (best < 0 || rt_newest[i].stamp > rt_newest[best].stamp ||
            (rt_newest[i].stamp == rt_newest[best].stamp && rt_newest[i].w > rt_newest[best].w)) best = i;
    }
    if (best < 0) return VK_NULL_HANDLE;
    *w = rt_newest[best].w, *h = rt_newest[best].h;
    static VkImage last;   // diagnostic: each change of source, the first 24
    static int changes;
    VkImage cur = __atomic_load_n(&rt_newest[best].image, __ATOMIC_ACQUIRE);
    if (cur != last && changes++ < 24)
        rt_log("vk_exec: upscale source %p %ux%u written by %s, frame %llu", (void *)cur, *w, *h,
               rt_newest[best].storage ? "a storage bind" : "a render pass",
               (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
    last = cur;
    return __atomic_load_n(&rt_newest[best].image, __ATOMIC_ACQUIRE);
}

void vk_exec_pass_end(VkExec *ex) {
    if (!ex->in_pass) return;
    vk_exec_oq_pass_end(ex);
    vkCmdEndRendering(ex->cb);
    ex->recorded++;
    ex->in_pass = 0;
    for (int i = 0; i < ex->pass_ncolor && i < 8; i++)   // stand-ins of 3D BC targets go to their textures
        if (ex->color[i].image && ex->color[i].img && ex->color[i].img->image != ex->color[i].image)
            bc_copy_back(ex, &ex->color[i]);
    if (vk_dump_armed) vk_dump_pass_end(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    for (int i = 0; i < 8; i++)
        if (ex->color[i].image) vk_texture_gpu_written(ex->color[i].tex), tfr_note(&ex->color[i], stamp),
                                rt_note_newest(&ex->color[i], stamp);
    if (ex->depth.image) vk_texture_gpu_written(ex->depth.tex), tfr_note(&ex->depth, stamp);
}

VkCommandBuffer vk_exec_inline_begin(int *reopen) {
    VkExec *ex = &X;
    *reopen = 0;
    if (!x_here || !ex->cb) return VK_NULL_HANDLE;
    *reopen = ex->in_pass;
    vk_exec_barrier(ex);   // ends the pass; feedback-layout targets go back to GENERAL
    return ex->cb;
}

void vk_exec_inline_end(int reopen) {
    X.recorded++;
    if (reopen) vk_exec_pass_begin(&X);
}

typedef struct { VkBuffer b; VmaAllocation a; } InlineBuf;
static void free_inline_buf(void *arg) {   // completion thread
    InlineBuf *t = arg;
    vmaDestroyBuffer(vk_ctx()->vma, t->b, t->a);
    free(t);
}

VkBuffer vk_exec_inline_buffer(VkDeviceSize size) {
    InlineBuf *t = calloc(1, sizeof *t);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!t || vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &t->b, &t->a, NULL) != VK_SUCCESS) { free(t); return VK_NULL_HANDLE; }
    vk_completion_add(X.fence, free_inline_buf, t);
    return t->b;
}

VkRect2D vk_exec_scissor(const VkExec *ex) {
    int64_t w = ex->pass_w, h = ex->pass_h;
    if (!ex->has_scissor) return (VkRect2D){{0, 0}, {(uint32_t)w, (uint32_t)h}};
    int64_t x0 = ex->scissor.x, y0 = ex->scissor.y;
    int64_t x1 = x0 + ex->scissor.w, y1 = y0 + ex->scissor.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return (VkRect2D){{0, 0}, {0, 0}};
    return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

// ---- clears

// Outside a pass, a clear of a whole target (no narrower scissor, the target as large as the render area) clears the
// image directly instead of opening a pass for it.
static int whole_target(const VkExec *ex, const VkExecTarget *t) {
    // vkCmdClear*Image addresses a 3D level whole, not one depth plane: slices of a 3D target clear in a pass; it
    // cannot clear a compressed image: views of BC blocks clear in a pass too
    if (ex->in_pass || t->is3d || t->blocks || t->width != ex->pass_w || t->height != ex->pass_h) return 0;
    if (!ex->has_scissor) return 1;
    const p_scissor *s = &ex->scissor;
    return s->x <= 0 && s->y <= 0 && (int64_t)s->x + s->w >= t->width && (int64_t)s->y + s->h >= t->height;
}

static void clear_attachment(VkExec *ex, const VkClearAttachment *a) {
    vk_exec_pass_begin(ex);
    VkClearRect cr = {vk_exec_scissor(ex), 0, ex->pass_layers};
    if (!cr.rect.extent.width || !cr.rect.extent.height) return;
    vkCmdClearAttachments(ex->cb, 1, a, 1, &cr);
    ex->recorded++;
    ex->fb_dirty |= a->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT ? 1u << a->colorAttachment : 0x100u;
}

static void clear_color(VkExec *ex, const p_clear_color *c) {
    vk_exec_frame.clears++;
    if (ex->render_disabled) return;
    resolve_rt(ex);
    int i = c->index;
    if (i < 0 || i >= (int)ex->pass_ncolor || !ex->color[i].image) {
        warn_once(W_CLEAR_TARGET, "colour clear of target %d, which is not bound or not renderable: skipped", i);
        return;
    }
    uint32_t mask = (uint32_t)c->mask & 0xf;
    if (!mask) return;
    if (mask != 0xf) {   // vkCmdClearAttachments ignores write masks; a masked clear needs a draw (vk_exec_draw.c)
        warn_once(W_CLEAR_MASK, "colour clear with channel mask %#x is not replayed: skipped", mask);
        return;
    }
    const VkExecTarget *t = &ex->color[i];
    VkClearColorValue cv;
    memcpy(cv.float32, c->rgba, sizeof cv.float32);   // integer targets: the payload's bits as the front end stored them
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, 1, t->base_layer, t->layers};
        vkCmdClearColorImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)i};
    a.clearValue.color = cv;
    clear_attachment(ex, &a);
}

static void clear_ds(VkExec *ex, const p_clear_ds *c) {
    if (ex->render_disabled) return;
    resolve_rt(ex);
    const VkExecTarget *t = &ex->depth;
    if (!t->image) return;
    VkImageAspectFlags asp = 0;
    if (c->depth_mask && (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    uint32_t sm = (uint32_t)c->stencil_mask & 0xff;
    if (sm && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
        if (sm == 0xff) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        else {
            VkRect2D scissor = vk_exec_scissor(ex);
            if (scissor.extent.width && scissor.extent.height) {
                // ClearAttachments ignores the write mask. Draw into each layer with ALWAYS/REPLACE instead,
                // preserving the depth plane and the stencil bits outside sm. A separate pass also excludes the
                // clear's fragments from the game's occlusion queries and avoids touching its colour targets.
                vk_exec_barrier(ex);
                VkExecTarget layer = *t;
                layer.layers = 1;
                layer.view_type = VK_IMAGE_VIEW_TYPE_2D;
                for (uint32_t i = 0; i < t->layers; i++) {
                    layer.base_layer = t->base_layer + i;
                    vk_clear_stencil_masked(ex->cb, t->format, t->samples, t->aspect, view_of(ex, &layer),
                                             ex->pass_w, ex->pass_h, scissor, sm, (uint32_t)c->stencil);
                    ex->recorded += 9;
                    vk_exec_frame.passes++;
                }
                vk_texture_gpu_written(t->tex);
                vk_exec_draw_new_cb();
                ex->dirty = VK_EXEC_DIRTY_ALL;
                vk_exec_barrier(ex);
            }
        }
    }
    if (!asp) return;
    float d = c->depth < 0.0f ? 0.0f : c->depth > 1.0f ? 1.0f : c->depth;
    VkClearDepthStencilValue v = {d, (uint32_t)c->stencil & 0xff};
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {asp, t->level, 1, t->base_layer, t->layers};
        vkCmdClearDepthStencilImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = asp};
    a.clearValue.depthStencil = v;
    clear_attachment(ex, &a);
}

// ---- the op loop

static void set_rt(VkExec *ex, const p_set_rt *r) {
    if (!memcmp(&ex->rt, r, sizeof *r)) return;   // the same targets again: the pass goes on
    vk_exec_pass_end(ex);
    ex->rt = *r;
    ex->rt_resolved = 0;
}

// ---- command buffer parts
// NVN starts the GPU as soon as commands are submitted; one Vulkan command buffer per submission starts it only after
// the whole replay. Between passes, once TOTK_SPLIT_DRAWS (64; 0 off) draws and dispatches were recorded since the
// last part, the part goes to the queue (vk_sync_split) and the replay continues in a new command buffer. Only
// command-buffer state is lost there (bindings and dynamic state: vk_exec_draw_new_cb, ex->dirty); barriers, layouts and
// queries are not: a barrier's first scope is everything earlier in submission order, image layouts belong to the image,
// and occlusion queries end with their pass. Fences, completion callbacks and reports stay per submission: only the last
// part signals the value. nxnative commits every 64 draws between passes; MacNativeGames after 30, then every 40.
// A part also spans at least TOTK_SPLIT_US (1000) of replay time: a split costs about 40 us of the render thread
// (vkQueueSubmit2, a command buffer, the mirror prologue), and a fight frame of 2,000 draws made 15 parts on the 7900X;
// a millisecond of recording is a fraction of a millisecond of GPU work, so the GPU still starts early.
static int split_n = -1, split_draws, part_start;
static uint64_t split_ns, part_t0;
static uint64_t n_parts, n_subs;

static void maybe_split(VkExec *ex) {
    if (split_n < 0) {
        split_n = cfg_int("TOTK_SPLIT_DRAWS");
        if (split_n < 0) split_n = 0;
        int us = cfg_int("TOTK_SPLIT_US");
        split_ns = us > 0 ? (uint64_t)us * 1000 : 0;
        rt_log("vk: replay command buffer split every %d draws between passes, parts of at least %d us%s", split_n,
               us > 0 ? us : 0, split_n ? "" : " (off)");
    }
    // KAR_SPLIT_PASS=1 (16.6b): also inside a render pass, which is ended here and begun again by the next draw (its
    // attachments load what the part stored). A frame's scene pass is long; split only between passes, the GPU got it
    // at the end of the replay and the frame waited for all of its GPU time.
    static int in_pass_ok = -1;
    if (in_pass_ok < 0) in_pass_ok = getenv("KAR_SPLIT_PASS") && *getenv("KAR_SPLIT_PASS") == '1';
    if (!split_n || (ex->in_pass && !in_pass_ok) || split_draws < split_n) return;
    if (split_ns && kern_now_ns() - part_t0 < split_ns) return;   // split_draws stays: the next draw asks again
    split_draws = 0;
    if (ex->recorded == part_start) return;
    if (ex->in_pass) vk_exec_pass_end(ex);
    ex->cb = vk_sync_split();
    part_start = ex->recorded;
    part_t0 = kern_now_ns();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    n_parts++;
}

// An ordered read of the GPU-visible backing at this replay position. In particular, imported guest bytes can be
// older than their shadow or VRAM mirror. Read through the same resolver/pick path as a buffer copy, never the VA.
int vk_exec_read_buffer(VkExec *ex, uint64_t addr, uint64_t size, void *out) {
    if (!x_here || ex != &X || !ex->cb || !out || !addr || !size || size > (8ull << 20) ||
        addr > UINT64_MAX - size || (addr & 3) || (size & 3)) return 0;
    VkMemSpan source;
    if (!vk_mem_resolve(addr, size, &source) || !source.buffer || source.avail < size || (source.offset & 3)) return 0;
    VK_MEM_PICK(source);   // choose an existing mirror; a read does not create a new one
    if (!source.buffer) return 0;
    VkCtx *c = vk_ctx();
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = NULL;
    VmaAllocationInfo mapped = {0};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    if (vmaCreateBuffer(c->vma, &bi, &ai, &buffer, &allocation, &mapped) != VK_SUCCESS) return 0;
    if (!mapped.pMappedData) { vmaDestroyBuffer(c->vma, buffer, allocation); return 0; }
    vk_exec_barrier(ex);   // end rendering and order all earlier writes before the transfer
    VkBufferCopy copy = {.srcOffset = source.offset, .dstOffset = 0, .size = size};
    vkCmdCopyBuffer(ex->cb, source.buffer, buffer, 1, &copy);
    VkBufferMemoryBarrier2 host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer, .offset = 0, .size = size};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                           .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded += 2;
    vk_mem_count(source.cls, VK_MUSE_COPY_SRC, size);
    VK_MEM_BOUND(source);
    ex->cb = vk_sync_split_wait();   // this prefix, not ex->fence (only the final part signals that value)
    split_draws = 0;
    part_start = ex->recorded;
    part_t0 = kern_now_ns();
    ex->barrier_at = -1;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    n_parts++;
    VkResult result = vmaInvalidateAllocation(c->vma, allocation, 0, size);
    if (result == VK_SUCCESS) memcpy(out, mapped.pMappedData, (size_t)size);
    // split_wait is fatal on submission/wait failure, so this allocation is never freed while the copy is in flight.
    vmaDestroyBuffer(c->vma, buffer, allocation);
    return result == VK_SUCCESS;
}

// KAR_REPLAY_PROF=1 (diagnostic): time per op kind; a submission whose replay takes over 100 ms logs its three
// largest kinds plus the memory sync around the walk (record_impl), to find what the frame-time "replay" spikes are.
static int rp_on = -1;
static uint64_t rp_ns[OP_COUNT_ + 2];   // + mirror/usnap begin, mirror/usnap end
static uint32_t rp_n[OP_COUNT_ + 2];
extern uint64_t vk_rp_sub[4], vk_rp_sh[5], gfx_rp[10], gfx_pf_why[3];   // vk_exec_draw.c: op_dispatch split into shader, pipeline, binding, the rest
static void walk(VkExec *ex, const op_hdr *h, int depth) {
    if (rp_on < 0) { rp_on = getenv("KAR_REPLAY_PROF") && *getenv("KAR_REPLAY_PROF") == '1'; extern int vk_rp_kind_on; vk_rp_kind_on = rp_on; }
    for (;;) {
        uint64_t rp_t0 = rp_on ? plat_now_ns() : 0;
        unsigned rp_op = h->op < OP_COUNT_ ? h->op : 0;
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        const void *a = nvn_op_payload(h);
        switch (h->op) {
        case OP_END: return;
        case OP_BEGIN: break;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_CALL:
            if (depth >= CALL_DEPTH) rt_fatal("vk: command buffer calls nested deeper than %d", CALL_DEPTH);
            walk(ex, nvn_op_first(((const p_jump *)a)->target), depth + 1);
            break;
        case OP_SET_RT: set_rt(ex, a); break;
        case OP_CLEAR_COLOR: clear_color(ex, a); break;
        case OP_CLEAR_DS: clear_ds(ex, a); break;
        case OP_VIEWPORTS: {
            const p_viewports *v = a;
            for (int i = 0; i < v->n && v->first + i < 16; i++)
                if (v->first + i >= 0) memcpy(ex->vp[v->first + i], v->v[i], sizeof ex->vp[0]);
            ex->has_vp = 1;
            ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
            break;
        }
        case OP_SCISSOR:
            ex->scissor = *(const p_scissor *)a;
            ex->has_scissor = 1;
            ex->dirty |= VK_EXEC_DIRTY_SCISSOR;
            break;
        case OP_DEPTH_RANGE: {
            const p_depth_range *r = a;
            ex->depth_range[0] = r->n, ex->depth_range[1] = r->f;
            ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
            break;
        }
        case OP_STENCIL: {
            const p_stencil *s = a;
            for (int f = 0; f < 2; f++) {
                if (!(s->face & (1 << f))) continue;
                if (s->which == 0) ex->st_vmask[f] = (uint32_t)s->value_mask;
                else if (s->which == 1) ex->st_wmask[f] = (uint32_t)s->mask;
                else ex->st_ref[f] = (uint32_t)s->ref;
            }
            ex->dirty |= VK_EXEC_DIRTY_STENCIL;
            break;
        }
        case OP_POLY_OFFSET:
            ex->poly_offset = *(const p_poly_offset *)a;
            ex->dirty |= VK_EXEC_DIRTY_DEPTH_BIAS;
            break;
        case OP_BLEND_COLOR:
            memcpy(ex->blend_color, ((const p_blend_color *)a)->rgba, sizeof ex->blend_color);
            ex->dirty |= VK_EXEC_DIRTY_BLEND_CONST;
            break;
        case OP_DEPTH_CLAMP: ex->pipe.depth_clamp = ((const p_flag *)a)->value != 0; break;
        case OP_RASTER_DISCARD: ex->pipe.raster_discard = ((const p_flag *)a)->value != 0; break;
        case OP_PRIM_RESTART: ex->prim_restart = *(const p_prim_restart *)a; break;
        case OP_COPY_STRIDE: ex->copy_stride = ((const p_copy_stride *)a)->stride; break;
        case OP_SET_TEX_POOL: ex->tex_pool = ((const p_set_pool *)a)->pool, ex->tex_serial++; break;
        case OP_SET_SAMP_POOL: ex->samp_pool = ((const p_set_pool *)a)->pool, ex->tex_serial++; break;
        case OP_BARRIER: vk_exec_barrier(ex); break;
        case OP_WAIT_SYNC: break;   // submit preparation waited before any replay/snapshot; source may be gone
        case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH: case OP_UPDATE_UB: case OP_DRAW_IDX_IND: case OP_DISPATCH_IND:
        case OP_DRAW_IND:
            if (h->op == OP_DRAW || h->op == OP_DRAW_IDX || h->op == OP_DRAW_IDX_IND || h->op == OP_DRAW_IND)
                vk_exec_frame.draws_seen++, split_draws++;
            if (h->op == OP_DISPATCH || h->op == OP_DISPATCH_IND) vk_exec_frame.dispatches++, split_draws++;
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_PUSH_CONST:
            vk_exec_op_draw(ex, h->op, a);
            break;
        case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX:
            if (h->op != OP_CLEAR_TEX) vk_exec_frame.copies++;
            if (vk_dump_armed) vk_dump_copy(ex, h->op, a);
            // fall through
        case OP_SIGNAL_EVENT: case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND:
        case OP_CLEAR_BUF:
            vk_exec_op_xfer(ex, h->op, a);
            break;
        default: rt_fatal("vk: op %s has no replay handler", nvn_op_name(h->op));
        }
        if (split_draws >= split_n) maybe_split(ex);
        if (rp_on && rp_op != OP_CALL) rp_ns[rp_op] += plat_now_ns() - rp_t0, rp_n[rp_op]++;
        h = nvn_op_next(h);
    }
}

static int record_impl(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence, uint64_t submitted_ticks,
                       VkExecWorkFn work, void *context) {
    VkExec *ex = &X;
    if (!x_ready) state_reset(ex), vk_dump_init(), x_ready = 1;
    ex->cb = cb;
    ex->fence = fence;
    ex->submitted_ticks = submitted_ticks;
    ex->recorded = 0;
    ex->in_pass = 0;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->rt_resolved = 0;   // images may have changed since the last command buffer
    ex->nviews = 0;
    ex->barrier_at = -1;
    ex->nfbl = 0;   // the previous command buffer ended with a full barrier: every image is in GENERAL
    vk_exec_frame.submits++;
    split_draws = part_start = 0;
    part_t0 = kern_now_ns();
    x_here = 1;
    int memory = n || work;
    uint64_t rp_start = rp_on > 0 ? plat_now_ns() : 0;
    if (rp_on > 0) memset(rp_ns, 0, sizeof rp_ns), memset(rp_n, 0, sizeof rp_n), memset(vk_rp_sub, 0, sizeof vk_rp_sub), memset(vk_rp_sh, 0, sizeof vk_rp_sh), memset(gfx_rp, 0, sizeof gfx_rp), memset(gfx_pf_why, 0, sizeof gfx_pf_why);
    if (memory) vk_mem_mirror_begin(), vk_usnap_begin(handles, n, fence);
    if (rp_on > 0) rp_ns[OP_COUNT_] = plat_now_ns() - rp_start;
    extern void vk_dts_mark(VkExec *ex, int tag);
    vk_dts_mark(ex, 10);
    if (work) work(ex, context);
    else {
        for (int i = 0; i < n; i++) {
            if (!handles[i]) rt_fatal("vk: command handle %d of a submission is null", i);
            walk(ex, nvn_op_first(handles[i]), 0);
        }
    }
    vk_exec_pass_end(ex);
    vk_dts_mark(ex, 11);
    x_here = 0;
    if (ex->recorded) vk_exec_barrier(ex);   // later submissions see this one's writes
    uint64_t rp_e0 = rp_on > 0 ? plat_now_ns() : 0;
    if (memory) ex->recorded += vk_mem_mirror_end(ex->cb, fence), vk_usnap_end(fence);   // VRAM mirror bytes the GPU wrote go back to guest memory
    vk_dts_mark(ex, 12);
    if (rp_on > 0) {
        rp_ns[OP_COUNT_ + 1] = plat_now_ns() - rp_e0;
        uint64_t total = plat_now_ns() - rp_start;
        // Window average (every 600 frames): replay ms per frame and how much of it bind_resources took, a measure
        // that does not depend on where in the race a frame number falls (16.4a).
        static uint64_t win_ns, win_f0, win_bind0;
        extern uint64_t vk_rp_bind_ns;
        uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        win_ns += total;
        static uint64_t win_op[OP_COUNT_ + 2], win_opn[OP_COUNT_];
        for (int k = 0; k < OP_COUNT_ + 2; k++) win_op[k] += rp_ns[k];
        for (int k = 0; k < OP_COUNT_; k++) win_opn[k] += rp_n[k];
        static uint64_t win_sub[4];
        for (int k = 0; k < 4; k++) win_sub[k] += vk_rp_sub[k];
        if (!win_f0) win_f0 = f, win_bind0 = vk_rp_bind_ns;
        if (f >= win_f0 + 600) {
            double nf = (double)(f - win_f0);
            char t[512];
            int o = 0;
            for (int j = 0; j < 8; j++) {   // the most expensive op kinds of the window, ms per frame (count per frame)
                int best = -1;
                for (int k = 0; k < OP_COUNT_ + 2; k++)
                    if (win_op[k] && (best < 0 || win_op[k] > win_op[best])) best = k;
                if (best < 0) break;
                const char *nm = best == OP_COUNT_ ? "mem-begin" : best == OP_COUNT_ + 1 ? "mem-end" : nvn_op_name((unsigned)best);
                o += snprintf(t + o, sizeof t - (size_t)o, " %s %.2f (%.0f)", nm, win_op[best] / 1e6 / nf,
                              best < OP_COUNT_ ? win_opn[best] / nf : 1.0);
                win_op[best] = 0;
            }
            extern uint64_t vk_rp_kind[7], vk_rp_kind_n[7];
            rt_log("replay prof: descriptor build per frame (ms, bindings): support %.2f (%.0f), UBO %.2f (%.0f), SSBO %.2f "
                   "(%.0f), TEX %.2f (%.0f), IMG %.2f (%.0f), bindless arrays %.2f (%.0f), other arrays %.2f (%.0f)",
                   vk_rp_kind[0] / 1e6 / nf, vk_rp_kind_n[0] / nf,
                   vk_rp_kind[1] / 1e6 / nf, vk_rp_kind_n[1] / nf, vk_rp_kind[2] / 1e6 / nf, vk_rp_kind_n[2] / nf,
                   vk_rp_kind[3] / 1e6 / nf, vk_rp_kind_n[3] / nf, vk_rp_kind[4] / 1e6 / nf, vk_rp_kind_n[4] / nf,
                   vk_rp_kind[5] / 1e6 / nf, vk_rp_kind_n[5] / nf, vk_rp_kind[6] / 1e6 / nf, vk_rp_kind_n[6] / nf);
            memset(vk_rp_kind, 0, sizeof vk_rp_kind), memset(vk_rp_kind_n, 0, sizeof vk_rp_kind_n);
            extern uint64_t vk_rp_bl[4];
            rt_log("replay prof: bindless residency per frame: block read %.2f ms (%.0f handles), hash %.2f ms, memo hits %.0f",
                   vk_rp_bl[0] / 1e6 / nf, vk_rp_bl[3] / nf, vk_rp_bl[1] / 1e6 / nf, vk_rp_bl[2] / nf);
            memset(vk_rp_bl, 0, sizeof vk_rp_bl);
            static int64_t dg0;
            int64_t dg = __atomic_load_n(&vk_destroy_gen, __ATOMIC_RELAXED);
            uint64_t ph, pm, pe;
            extern void vk_psc_stats(uint64_t *hits, uint64_t *miss, uint64_t *epochs);
            vk_psc_stats(&ph, &pm, &pe);
            rt_log("replay prof: destructions of descriptor objects %.2f per frame; persistent sets: hits %.1f, misses %.1f, "
                   "epochs %.2f per frame", (double)(dg - dg0) / nf, ph / nf, pm / nf, pe / nf);
            dg0 = dg;
            rt_log("replay prof: frames %llu-%llu: replay %.2f ms per frame, bind_resources %.2f ms; by op (ms, count):%s; "
                   "dispatch: shader %.2f pipeline %.2f bind %.2f rest %.2f ms",
                   (unsigned long long)win_f0, (unsigned long long)f, win_ns / 1e6 / nf,
                   (vk_rp_bind_ns - win_bind0) / 1e6 / nf, t, win_sub[0] / 1e6 / nf, win_sub[1] / 1e6 / nf,
                   win_sub[2] / 1e6 / nf, win_sub[3] / 1e6 / nf);
            memset(win_sub, 0, sizeof win_sub);
            memset(win_op, 0, sizeof win_op), memset(win_opn, 0, sizeof win_opn);
            win_ns = 0, win_f0 = f, win_bind0 = vk_rp_bind_ns;
        }
        if (total > 100000000ull) {
            int top[3] = {-1, -1, -1};
            for (int k = 0; k < OP_COUNT_ + 2; k++)
                for (int j = 0; j < 3; j++)
                    if (top[j] < 0 || rp_ns[k] > rp_ns[top[j]]) {
                        for (int m = 2; m > j; m--) top[m] = top[m - 1];
                        top[j] = k;
                        break;
                    }
            char t[300];
            int o = 0;
            for (int j = 0; j < 3; j++) {
                int k = top[j];
                const char *nm = k == OP_COUNT_ ? "mem-begin" : k == OP_COUNT_ + 1 ? "mem-end" : nvn_op_name((unsigned)k);
                o += snprintf(t + o, sizeof t - o, " %s %.1f ms (%u)", nm, rp_ns[k] / 1e6, k < OP_COUNT_ ? rp_n[k] : 1);
            }
            rt_log("replay prof: frame %llu submission %.1f ms:%s | dispatch shader %.1f pipe %.1f bind %.1f rest %.1f | cs answers %.1f lockwait %.1f get %.1f miss %llu/%llu | req %.1f get_one %.1f disk %.1f (%llu) xlate %.1f (%llu) | read %.1f check+reflect %.1f | why nostored %llu queued %llu other %llu", (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), total / 1e6, t,
                   vk_rp_sub[0] / 1e6, vk_rp_sub[1] / 1e6, vk_rp_sub[2] / 1e6, vk_rp_sub[3] / 1e6,
                   vk_rp_sh[0] / 1e6, vk_rp_sh[1] / 1e6, vk_rp_sh[2] / 1e6, (unsigned long long)vk_rp_sh[3], (unsigned long long)vk_rp_sh[4],
                   gfx_rp[0] / 1e6, gfx_rp[1] / 1e6, gfx_rp[2] / 1e6, (unsigned long long)gfx_rp[6], gfx_rp[3] / 1e6, (unsigned long long)gfx_rp[7], gfx_rp[8] / 1e6, gfx_rp[9] / 1e6, (unsigned long long)gfx_pf_why[0], (unsigned long long)gfx_pf_why[1], (unsigned long long)gfx_pf_why[2]);
        }
    }
    if (ex->nviews) {
        ViewList *l = malloc(sizeof *l + sizeof(VkImageView) * (size_t)ex->nviews);
        if (!l) rt_fatal("vk: out of memory");
        l->n = ex->nviews;
        for (int i = 0; i < ex->nviews; i++) l->v[i] = ((ViewEnt *)ex->views)[i].view;
        vk_completion_add(fence, free_views, l);
        ex->nviews = 0;
    }
    ex->cb = VK_NULL_HANDLE;
    if (memory) n_subs++;
    if ((rt_trace & RT_TRACE_PERF) && n_subs >= 240) {
        rt_log("perf: replay command buffer parts per submission %.2f", (double)(n_subs + n_parts) / (double)n_subs);
        n_subs = n_parts = 0;
    }
    return ex->recorded - part_start;   // what the last part holds
}

int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence, uint64_t submitted_ticks) {
    return record_impl(cb, handles, n, fence, submitted_ticks, NULL, NULL);
}

int vk_exec_record_work(VkCommandBuffer cb, uint64_t fence, uint64_t submitted_ticks, VkExecWorkFn work, void *context) {
    if (!work) rt_fatal("vk: work recording without a callback");
    return record_impl(cb, NULL, 0, fence, submitted_ticks, work, context);
}
