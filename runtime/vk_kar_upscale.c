// Kirby Air Riders' stand-in for DLSS, recorded into the replay's command buffer by the missing-upscale fallback
// (vk_exec_draw.c upscale_fill). The game feeds DLSS an sRGB-encoded rgb10a2 image and composites DLSS's output in
// linear light (fs bc440c9e blends it with the sRGB-decoded UI, then takes the square root for the display). This pass
// upscales with AMD FSR 1 EASU on the encoded values and writes them sRGB-decoded, linear, into the game's output
// texture as a storage image (runtime/shaders/kar_upscale.comp). KAR_UPSCALE=blit falls back to the plain blit;
// KAR_UPSCALE_DECODE=0 skips the decode, =sq squares instead. Without the decode the start line's brightness matches
// console video (mean 182 vs 199; decoded 132) but the colours wash out (saturation 0.25 vs the console's 0.38; decoded
// 0.43): the console's extra brightness comes from before this pass (2026-11-11), so the decode stays. With the port menu's sharpness (TOTK_SHARPNESS, 1..100, live) above 0 it is two passes instead: EASU into an
// rgba16f intermediate, still encoded (kar_easu16.comp), then FSR 1 RCAS sharpens, decodes and stores (kar_rcas.comp),
// with the stops mapping of the present-time FSR in vk_fsr.c.
//
// Views: one sampled view per source image and one storage view per destination image, kept in small tables. A view
// whose image is replaced in its table goes to the dead list (vk_defer_view), which frees it once the GPU is past the
// submissions that used it.
#include "vk.h"

#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "cfg.h"
#include "nvn_api.h"
#include "ext_upscaler.h"
#include "kar_easu16_spv.h"
#include "kar_rcas_spv.h"
#include "kar_upscale_spv.h"

#define VIEWS 8

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe, easu16, rcas;
    VkSampler sampler;
    struct { VkImage image; VmaAllocation mem; VkImageView sampled, storage; uint32_t w, h; int fresh; } mid;
    struct { VkImage image; VkFormat format; VkImageView view; } src[VIEWS], dst[VIEWS];
    int src_next, dst_next;
} U;

static VkPipeline make_pipe(VkCtx *c, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = U.layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(c->device, m, NULL);
    return p;
}

// The rgba16f intermediate of the two-pass path, w x h; replaced (the old one retired) when the size changes.
static int mid_image(VkCtx *c, uint32_t w, uint32_t h) {
    if (U.mid.image && U.mid.w == w && U.mid.h == h) return 1;
    if (U.mid.image) {
        vk_defer_view(U.mid.sampled), vk_defer_view(U.mid.storage);
        vk_defer_image(U.mid.image, U.mid.mem);
        memset(&U.mid, 0, sizeof U.mid);
    }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &U.mid.image, &U.mid.mem, NULL) != VK_SUCCESS) {
        memset(&U.mid, 0, sizeof U.mid);
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = U.mid.image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &U.mid.sampled) != VK_SUCCESS ||
        vkCreateImageView(c->device, &vi, NULL, &U.mid.storage) != VK_SUCCESS)
        return 0;
    U.mid.w = w, U.mid.h = h, U.mid.fresh = 1;
    return 1;
}

static void bind_pair(VkCommandBuffer cb, VkImageView in, VkImageView out) {
    VkDescriptorImageInfo ii = {U.sampler, in, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    rec_push(U.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, U.layout, 0, 2, w);
}

static int init(VkCtx *c) {
    U.tried = 1;
    U.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!U.push) return 0;
    VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &U.dsl) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &U.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &U.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &U.sampler) != VK_SUCCESS)
        return 0;
    if (!(U.pipe = make_pipe(c, kar_upscale_spv, sizeof kar_upscale_spv))) return 0;
    U.easu16 = make_pipe(c, kar_easu16_spv, sizeof kar_easu16_spv);   // sharpening only: may fail on its own
    U.rcas = make_pipe(c, kar_rcas_spv, sizeof kar_rcas_spv);
    rt_log("vk: DLSS stand-in: FSR 1 EASU with linear decode ready (KAR_UPSCALE=blit for the plain blit)");
    return U.ok = 1;
}

// The view of `image` in table t (created when missing; the entry it replaces is retired).
static VkImageView view_of(VkCtx *c, int dst, VkImage image, VkFormat format) {
    typeof(U.src[0]) *t = dst ? U.dst : U.src;
    int *next = dst ? &U.dst_next : &U.src_next;
    for (int i = 0; i < VIEWS; i++)
        if (t[i].image == image && t[i].format == format && t[i].view) return t[i].view;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageViewUsageCreateInfo usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                        .usage = dst ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT};
    vi.pNext = &usage;
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(c->device, &vi, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    int slot = *next;
    *next = (*next + 1) % VIEWS;
    if (t[slot].view) vk_defer_view(t[slot].view);
    t[slot].image = image, t[slot].format = format, t[slot].view = v;
    return v;
}

int vk_fsr3_dispatch(VkCommandBuffer cb, VkImage color, VkFormat cf, uint32_t sw, uint32_t sh, VkImage depth, VkImage mv,
                     VkImage out, VkFormat of, uint32_t dw, uint32_t dh, float jx, float jy, int reset, float nearv,
                     float farv, float fov, uint64_t key);
int vk_fg_mv_compute(VkCommandBuffer cb, uint32_t w, uint32_t h, VkFgMv *out);
int vk_fsr3_jitter(uint64_t index, int rw, int dw, float *x, float *y);
uint64_t vk_fsr3_last_key(void);
static volatile uint32_t kar_fsr3_size[4];   // render width, height, output width, height of the last FSR 3 dispatch

// UPSCALER = fsr3 (port menu, live) or KAR_UPSCALE=fsr3: FSR 3.1 (vk_fg.cpp) instead of EASU; = external: the
// upscaler add-on (ext_upscaler.c) when one is loaded, else FSR 3.1. 0 off, 1 FSR 3.1, 2 add-on.
static int fsr3_mode(void) {
    const char *e = getenv("KAR_UPSCALE"), *c = cfg_get("TOTK_UPSCALER");
    const char *v = e && *e ? e : c;
    if (!v) return 0;
    if (!strcmp(v, "external")) return ext_upscaler_name() ? 2 : 1;
    return !strcmp(v, "fsr3");
}
static uint64_t last_temporal;   // the frame of the newest FSR 3 or add-on upscale (the jitter runs while they do)

// FSR 3 jitter (RELEASE_PLAN.md 17.5). The game renders without one, and its camera matrices could not be found where
// the geometry reads them (the constant block and the camera's own offset fields moved nothing), so the replay shifts
// the viewport of every pass into a depth target of the scene's render size by FSR's subpixel offset of this frame:
// the same as jittering the projection, and the replay knows its frame. KAR_FSR3_JITTER=0 turns it off;
// KAR_FSR3_JOFFSET=<px> (diagnostic) shifts by a fixed amount, x in one 64-frame block and y in the next, also with FSR 1.
static int jitter_of(uint64_t fr, float *jx, float *jy) {
    static int on = -1;
    static float joff;
    if (on < 0) {
        const char *e = getenv("KAR_FSR3_JITTER");
        on = !(e && *e == '0');
        joff = getenv("KAR_FSR3_JOFFSET") ? (float)atof(getenv("KAR_FSR3_JOFFSET")) : 0;
    }
    if (joff > 0) return *jx = (fr / 64) % 2 ? 0 : joff, *jy = (fr / 64) % 2 ? joff : 0, 1;
    uint64_t last = last_temporal;
    if (!on || !last || last + 8 < fr || !kar_fsr3_size[0] || !kar_fsr3_size[2]) return 0;
    return vk_fsr3_jitter(fr, (int)kar_fsr3_size[0], (int)kar_fsr3_size[2], jx, jy);
}

// The viewport shift in pixels for a pass of w x h with a depth target of dw x dh (vk_exec_draw.c record_fixed).
int vk_kar_jitter_viewport(uint32_t w, uint32_t h, uint32_t dw, uint32_t dh, float *jx, float *jy) {
    static uint32_t sw = 1536, sh = 864;   // the scene's render size: the upscale source's, once seen
    if (kar_fsr3_size[0]) sw = kar_fsr3_size[0], sh = kar_fsr3_size[1];
    if (w != sw || h != sh || dw != sw || dh != sh) return 0;
    return jitter_of(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), jx, jy);
}

// FSR 3.1 into the intermediate, then the RCAS pass decodes (and with the menu's sharpness sharpens) into dst. 0 when
// there is no scene camera pair (menus, loading): the caller runs FSR 1. The history resets after such frames.
static int fsr3_record(int backend, VkCtx *c, VkCommandBuffer cb, VkImage src, VkFormat sfmt, uint32_t sw, uint32_t sh,
                       VkImageView out, uint32_t dw, uint32_t dh, int sharp, int decode) {
    static uint64_t last_frame;
    static uint32_t miss;
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (!U.rcas || !mid_image(c, dw, dh)) return 0;
    kar_fsr3_size[0] = sw, kar_fsr3_size[1] = sh, kar_fsr3_size[2] = dw, kar_fsr3_size[3] = dh;
    VkFgMv mv;
    if (!vk_fg_mv_compute(cb, sw, sh, &mv)) return miss++, 0;   // FSR 1 for this frame; the history resets after it
    float jit[2] = {0, 0};
    if (!jitter_of(fr, &jit[0], &jit[1])) jit[0] = jit[1] = 0;
    int reset = !last_frame || last_frame + 1 != fr || miss;
    miss = 0, last_frame = fr;
    if (U.mid.fresh) {
        VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = U.mid.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &ib};
        vkCmdPipelineBarrier2(cb, &di);
        U.mid.fresh = 0;
    }
    if (backend == 2) {   // the add-on records its own commands: after every call the recorder thread has queued
        extern void rec_drain(void);
        static uint64_t t0;
        uint64_t t = kern_now_ns();
        ExtUpscalerFrame f = {.cb = cb, .color = src, .color_format = sfmt, .depth = mv.depth, .motion = mv.mv,
                              .output = U.mid.image, .output_format = VK_FORMAT_R16G16B16A16_SFLOAT, .render_w = sw,
                              .render_h = sh, .output_w = dw, .output_h = dh, .jitter_x = jit[0], .jitter_y = jit[1],
                              .motion_scale_x = 1, .motion_scale_y = 1, .reset = reset, .depth_inverted = 1,
                              .frame_ms = t0 && t > t0 ? (float)((t - t0) / 1e6) : 16.7f, .near_z = mv.nearv,
                              .far_z = mv.farv, .fov_y = mv.fov};
        t0 = t;
        rec_drain();
        if (!ext_upscaler_evaluate(&f)) return 0;
        static int told;
        if (!told++) rt_log("vk: %s upscaling %ux%u -> %ux%u", ext_upscaler_name(), sw, sh, dw, dh);
    } else if (!vk_fsr3_dispatch(cb, src, sfmt, sw, sh, mv.depth, mv.mv, U.mid.image, VK_FORMAT_R16G16B16A16_SFLOAT, dw,
                                 dh, jit[0], jit[1], reset, mv.nearv, mv.farv, mv.fov, fr)) {
        return 0;
    }
    last_temporal = fr;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    float rc[8] = {(float)dw, (float)dh, sharp > 0 ? (float)(100 - sharp) / 50.0f : -1.0f, (float)decode, 0, 0, 0, 0};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, U.rcas);
    bind_pair(cb, U.mid.sampled, out);
    vkCmdPushConstants(cb, U.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, rc);
    vkCmdDispatch(cb, (dw + 15) / 16, (dh + 15) / 16, 1);
    return 1;
}

int vk_kar_upscale_record(VkCommandBuffer cb, VkImage src, VkFormat sfmt, uint32_t sw, uint32_t sh, VkImage dst,
                          VkFormat dfmt, uint32_t dw, uint32_t dh) {
    static int mode = -1, decode = -1;
    if (mode < 0) {
        const char *e = getenv("KAR_UPSCALE");
        mode = e && !strcmp(e, "blit") ? 0 : 1;
        const char *d = getenv("KAR_UPSCALE_DECODE");
        decode = d && *d == '0' ? 0 : d && !strcmp(d, "sq") ? 2 : 1;   // 1 sRGB curve (default), 2 square, 0 none
    }
    // the shader's storage format is rgb10_a2: other outputs keep the blit
    if (!mode || dfmt != VK_FORMAT_A2B10G10R10_UNORM_PACK32) return 0;
    VkCtx *c = vk_ctx();
    if (!U.tried) init(c);
    if (!U.ok) return 0;
    VkImageView in = view_of(c, 0, src, sfmt), out = view_of(c, 1, dst, dfmt);
    if (!in || !out) return 0;
    // Only a sharpness the player set (menu or totk.ini): the table's default of 80 is tuned for the present-time FSR of
    // a lower-resolution TotK frame and made Kirby's picture visibly harsher than before.
    const char *sv = cfg_get("TOTK_SHARPNESS");
    int sharp = sv && *sv ? cfg_int("TOTK_SHARPNESS") : 0;
    sharp = sharp < 0 ? 0 : sharp > 100 ? 100 : sharp;
    int temporal = fsr3_mode();
    if (temporal && fsr3_record(temporal, c, cb, src, sfmt, sw, sh, out, dw, dh, sharp, decode)) return 1;
    int two = sharp > 0 && U.easu16 && U.rcas && mid_image(c, dw, dh);
    float pc[8] = {(float)sw, (float)sh, (float)sw, (float)sh, (float)dw, (float)dh, (float)decode, 0};
    if (two && U.mid.fresh) {   // the intermediate's first use: from UNDEFINED to GENERAL
        VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = U.mid.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &ib};
        vkCmdPipelineBarrier2(cb, &di);
        U.mid.fresh = 0;
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, two ? U.easu16 : U.pipe);
    bind_pair(cb, in, two ? U.mid.storage : out);
    vkCmdPushConstants(cb, U.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pc);
    vkCmdDispatch(cb, (dw + 15) / 16, (dh + 15) / 16, 1);
    if (two) {   // EASU's writes before RCAS reads them; RCAS then sharpens, decodes and stores into the output
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
        float rc[8] = {(float)dw, (float)dh, (float)(100 - sharp) / 50.0f, (float)decode, 0, 0, 0, 0};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, U.rcas);
        bind_pair(cb, U.mid.sampled, out);
        vkCmdPushConstants(cb, U.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, rc);
        vkCmdDispatch(cb, (dw + 15) / 16, (dh + 15) / 16, 1);
    }
    static int told_sharp = -1;
    if (sharp != told_sharp) {
        told_sharp = sharp;
        rt_log("vk: DLSS stand-in: FSR 1 EASU %ux%u -> %ux%u%s%s (sharpness %d)", sw, sh, dw, dh,
               two ? " + RCAS" : "", decode ? ", decoded to linear" : "", sharp);
    }
    return 1;
}
