// Frame generation (RELEASE_PLAN.md 16.7, way B1): one interpolated picture between two of the game's, from AMD
// FidelityFX optical flow + frame interpolation (SDK 1.1.4, MIT, built by tools/build_ffx.py). KAR_FG=1 turns it on.
//
// The game renders no motion vectors (its DLSS class is never built), so this first stage feeds the interpolation
// constant inputs: motion vectors 0 and a far depth. The motion then comes from the optical flow on the finished
// picture alone. Camera motion vectors (B2) and a picture without the HUD (B3) improve the inputs later.
//
// vk_fg_interpolate runs on the present thread before the real picture's blit: it copies the game's picture (the
// crop the present shows) into an own image, records optical flow and interpolation into its own command buffer and
// submits it to gfx_queue. Queue order makes the game's writes visible (a barrier from all commands, as the present
// blit does), and later submissions that write the game's image wait for the copy (barrier after it). The result is
// the picture halfway between the previous call's picture and this one, in GENERAL layout, for vk_present_blit.
// Without the FidelityFX SDK in the build (no build/ffx-src/bin/ffx_sdk), vk_fg_interpolate returns VK_NULL_HANDLE.
extern "C" {
#include <malloc.h>   // _aligned_malloc
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "kar_fg_hudless_spv.h"
}

// On with KAR_FG=1 in the environment or frame_gen = 1 in dkb.ini (TOTK_FRAME_GEN); KAR_FG=0 turns it off for a run.
extern "C" int vk_fg_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("KAR_FG"), *c = cfg_get("TOTK_FRAME_GEN");
        on = e && *e ? *e == '1' : c && *c && strcmp(c, "0") != 0;
    }
    return on;
}

#if __has_include(<FidelityFX/host/ffx_frameinterpolation.h>)
#include <bit>      // the SDK headers use std::popcount and wide-string functions without including them (MSVC does)
#include <cmath>
#include <cwchar>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>

namespace {
struct Img {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageCreateInfo ci = {};
};

struct Fg {
    int failed = 0, w = 0, h = 0;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    void *scratch = nullptr;
    FfxInterface iface = {};
    FfxUInt32 shared_id = 0;
    // The SDK casts these opaque uint32_t arrays to its private structures, which Clang accesses with 16-byte
    // aligned SSE moves (see tools/build_ffx.py)
    alignas(64) FfxOpticalflowContext of = {};
    alignas(64) FfxFrameInterpolationContext fi = {};
    int have_ctx = 0;
    FfxResourceInternal res[5] = {};   // optical flow vector, scene change; dilated depth, dilated motion, prev depth
    int nres = 0;
    Img cur, out, depth, mv, hud;   // hud: the scene without the interface at the output size (B3)
    VkImageView hud_view = VK_NULL_HANDLE;
    struct { VkImage image; VkImageView view; } hsrc[4] = {};   // sampled views of vk_fg_mv.c's DLSS output copies
    int hsrc_next = 0;
    VkDescriptorSetLayout hl_dsl = VK_NULL_HANDLE;
    VkPipelineLayout hl_layout = VK_NULL_HANDLE;
    VkPipeline hl_pipe = VK_NULL_HANDLE;
    VkSampler hl_sampler = VK_NULL_HANDLE;
    PFN_vkCmdPushDescriptorSetKHR push = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint64_t frame_id = 0, calls = 0, last_ns = 0;
    int cleared = 0;
} G;

VkFormat storage_format(VkFormat f) {   // the interpolation writes its output as a storage image: no sRGB views
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

int make_image(VkCtx *c, Img *m, VkFormat fmt, int w, int h) {
    m->ci = {};
    m->ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    m->ci.imageType = VK_IMAGE_TYPE_2D;
    m->ci.format = fmt;
    m->ci.extent = {(uint32_t)w, (uint32_t)h, 1};
    m->ci.mipLevels = 1;
    m->ci.arrayLayers = 1;
    m->ci.samples = VK_SAMPLE_COUNT_1_BIT;
    m->ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    m->ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    m->ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    return vmaCreateImage(c->vma, &m->ci, &ai, &m->image, &m->alloc, nullptr) == VK_SUCCESS;
}

void free_image(VkCtx *c, Img *m) {
    if (m->image) vmaDestroyImage(c->vma, m->image, m->alloc);
    *m = Img{};
}

FfxResource res_of(Img *m, const wchar_t *name) {
    return ffxGetResourceVK((void *)m->image, ffxGetImageResourceDescriptionVK(m->image, m->ci, FFX_RESOURCE_USAGE_UAV),
                            name, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
}

void barrier(VkCommandBuffer cb, VkImage img, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
             VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = ss, b.srcAccessMask = sa, b.dstStageMask = ds, b.dstAccessMask = da;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d = {};
    d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

void to_general(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier2 b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d = {};
    d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

// The SDK loads some functions by their extension names (vkGetBufferMemoryRequirements2KHR ...). The device has
// them as Vulkan 1.1-1.3 core functions without enabling those extensions, where the KHR name may return NULL and
// the SDK would call through it: retry under the core name, and log what stays missing.
PFN_vkVoidFunction VKAPI_CALL proc_addr(VkDevice d, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(d, name);
    size_t n = strlen(name);
    if (!f && n > 3 && n < 120 && !strcmp(name + n - 3, "KHR")) {
        char core[128];
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        f = vkGetDeviceProcAddr(d, core);
    }
    if (!f) rt_log("fg: the FidelityFX back end found no %s", name);
    return f;
}

// The back end reports fp16 support from the extension list alone; this device does not enable shaderFloat16
// (vk_device.c), so the SDK must pick its 32-bit shader permutations.
FfxGetDeviceCapabilitiesFunc caps_orig;
FfxErrorCode caps_no_fp16(FfxInterface *i, FfxDeviceCapabilities *d) {
    FfxErrorCode e = caps_orig(i, d);
    d->fp16Supported = false;
    return e;
}

// KAR_FG_DUMP=<n> (diagnostic): from the n-th call on, six calls in a row write the copied real picture
// (fg_<k>_real.png) and the interpolated one shown before it (fg_<k>_mid.png, halfway between real k-1 and k) into
// KAR_FG_DUMP_DIR (default the working directory). Waits for the GPU each time: only for looking at the pictures.
int m_fmt_ok(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM; }

void dump_image(VkCtx *c, Img *m, const char *path) {
    uint32_t w = m->ci.extent.width, h = m->ci.extent.height;
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = (VkDeviceSize)w * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buf;
    VmaAllocation al;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &buf, &al, &info) != VK_SUCCESS) return;
    vkWaitForFences(c->device, 1, &G.fence, VK_TRUE, 5000000000ull);
    vkResetFences(c->device, 1, &G.fence);
    vkResetCommandBuffer(G.cb, 0);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    vkBeginCommandBuffer(G.cb, &cbi);
    barrier(G.cb, m->image, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy bc = {};
    bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bc.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(G.cb, m->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bc);
    vkEndCommandBuffer(G.cb);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &G.cb;
    vk_queue_lock();
    vkQueueSubmit(c->gfx_queue, 1, &si, G.fence);
    vk_queue_unlock();
    vkWaitForFences(c->device, 1, &G.fence, VK_TRUE, 5000000000ull);
    vmaInvalidateAllocation(c->vma, al, 0, VK_WHOLE_SIZE);
    uint8_t *px = (uint8_t *)info.pMappedData;
    if (m->ci.format == VK_FORMAT_B8G8R8A8_UNORM)
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t t = px[4 * i];
            px[4 * i] = px[4 * i + 2], px[4 * i + 2] = t;
        }
    for (size_t i = 0; i < (size_t)w * h; i++) px[4 * i + 3] = 255;
    rt_write_png(path, px, w, h);
    vmaDestroyBuffer(c->vma, buf, al);
    rt_log("fg: wrote %s", path);
}

void dump_pair(VkCtx *c, int mid) {
    static long long from = -2;
    if (from == -2) from = getenv("KAR_FG_DUMP") ? atoll(getenv("KAR_FG_DUMP")) : -1;
    if (from < 0 || G.calls < (uint64_t)from || G.calls >= (uint64_t)from + 6) return;
    if (m_fmt_ok(G.fmt) == 0) return;
    const char *dir = getenv("KAR_FG_DUMP_DIR");
    char path[600];
    snprintf(path, sizeof path, "%s/fg_%llu_real.png", dir && *dir ? dir : ".", (unsigned long long)G.calls);
    dump_image(c, &G.cur, path);
    if (mid) {
        snprintf(path, sizeof path, "%s/fg_%llu_mid.png", dir && *dir ? dir : ".", (unsigned long long)G.calls);
        dump_image(c, &G.out, path);
        if (G.hud_view) {
            snprintf(path, sizeof path, "%s/fg_%llu_hud.png", dir && *dir ? dir : ".", (unsigned long long)G.calls);
            dump_image(c, &G.hud, path);
        }
    }
    G.last_ns = kern_now_ns();   // the dump's wait is no pause of the game: no reset for it
}

void destroy_ctx(VkCtx *c) {
    if (G.fence) vkWaitForFences(c->device, 1, &G.fence, VK_TRUE, 5000000000ull);
    if (G.have_ctx) {
        for (int i = 0; i < G.nres; i++) G.iface.fpDestroyResource(&G.iface, G.res[i], G.shared_id);
        G.iface.fpDestroyBackendContext(&G.iface, G.shared_id);
        ffxFrameInterpolationContextDestroy(&G.fi);
        ffxOpticalflowContextDestroy(&G.of);
        G.have_ctx = 0, G.nres = 0;
    }
    if (G.hud_view) vkDestroyImageView(c->device, G.hud_view, nullptr), G.hud_view = VK_NULL_HANDLE;
    for (auto &h : G.hsrc)
        if (h.view) vkDestroyImageView(c->device, h.view, nullptr), h = {};
    free_image(c, &G.cur), free_image(c, &G.out), free_image(c, &G.depth), free_image(c, &G.mv), free_image(c, &G.hud);
    _aligned_free(G.scratch);
    G.scratch = nullptr;
    G.w = G.h = 0;
}

// The pipeline of shaders/kar_fg_hudless.comp (once; 0 when it cannot be made: no picture without the HUD then).
int hudless_pipe(VkCtx *c) {
    static int tried;
    if (tried) return G.hl_pipe != VK_NULL_HANDLE;
    tried = 1;
    G.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!G.push) return 0;
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0].binding = 0, b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, b[0].descriptorCount = 1;
    b[1].binding = 1, b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, b[1].descriptorCount = 1;
    b[0].stageFlags = b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dci.bindingCount = 2, dci.pBindings = b;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 1, li.pSetLayouts = &G.hl_dsl, li.pushConstantRangeCount = 1, li.pPushConstantRanges = &pr;
    VkSamplerCreateInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, nullptr, &G.hl_dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, nullptr, &G.hl_layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, nullptr, &G.hl_sampler) != VK_SUCCESS)
        return 0;
    VkShaderModuleCreateInfo mi = {};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = sizeof kar_fg_hudless_spv, mi.pCode = kar_fg_hudless_spv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, nullptr, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, ci.stage.module = m, ci.stage.pName = "main";
    ci.layout = G.hl_layout;
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, nullptr, &G.hl_pipe) != VK_SUCCESS)
        G.hl_pipe = VK_NULL_HANDLE;
    vkDestroyShaderModule(c->device, m, nullptr);
    return G.hl_pipe != VK_NULL_HANDLE;
}

VkImageView hsrc_view(VkCtx *c, VkImage image, VkFormat f) {
    for (auto &h : G.hsrc)
        if (h.image == image && h.view) return h.view;
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = f;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(c->device, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    auto &h = G.hsrc[G.hsrc_next];
    G.hsrc_next = (G.hsrc_next + 1) % 4;
    if (h.view) vk_defer_view(h.view);   // the GPU may still read it
    h.image = image, h.view = v;
    return v;
}

int create_ctx(VkCtx *c, VkFormat fmt, int w, int h) {
    if (!G.pool) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                      VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, c->gfx_family};
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, VK_NULL_HANDLE,
                                          VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
        if (vkCreateCommandPool(c->device, &pi, nullptr, &G.pool) != VK_SUCCESS) return 0;
        ai.commandPool = G.pool;
        if (vkAllocateCommandBuffers(c->device, &ai, &G.cb) != VK_SUCCESS) return 0;
        if (vkCreateFence(c->device, &fi, nullptr, &G.fence) != VK_SUCCESS) return 0;
    }
    if (!make_image(c, &G.cur, fmt, w, h) || !make_image(c, &G.out, fmt, w, h) ||
        !make_image(c, &G.depth, VK_FORMAT_R32_SFLOAT, w, h) || !make_image(c, &G.mv, VK_FORMAT_R16G16_SFLOAT, w, h) ||
        !make_image(c, &G.hud, fmt, w, h))
        return 0;
    {
        VkImageViewCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = G.hud.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(c->device, &vi, nullptr, &G.hud_view) != VK_SUCCESS) G.hud_view = VK_NULL_HANDLE;
    }
    VkDeviceContext dc = {c->device, c->phys, proc_addr};
    size_t n = ffxGetScratchMemorySizeVK(c->phys, 4);
    G.scratch = _aligned_malloc(n, 64);
    if (G.scratch) memset(G.scratch, 0, n);
    rt_log("fg: creating the FidelityFX contexts for %dx%d", w, h);
    if (!G.scratch || ffxGetInterfaceVK(&G.iface, ffxGetDeviceVK(&dc), G.scratch, n, 4) != FFX_OK) return 0;
    caps_orig = G.iface.fpGetDeviceCapabilities;
    G.iface.fpGetDeviceCapabilities = caps_no_fp16;
    if (G.iface.fpCreateBackendContext(&G.iface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &G.shared_id) != FFX_OK) return 0;
    FfxOpticalflowContextDescription od = {};
    od.backendInterface = G.iface;
    od.resolution = {(uint32_t)w, (uint32_t)h};
    if (ffxOpticalflowContextCreate(&G.of, &od) != FFX_OK) return 0;
    FfxFrameInterpolationContextDescription fd = {};
    fd.backendInterface = G.iface;
    fd.maxRenderSize = fd.displaySize = {(uint32_t)w, (uint32_t)h};
    fd.flags = FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;   // the game's projection: near 1, far 0
    fd.backBufferFormat = fd.previousInterpolationSourceFormat = ffxGetSurfaceFormatVK(fmt);
    if (ffxFrameInterpolationContextCreate(&G.fi, &fd) != FFX_OK) {
        ffxOpticalflowContextDestroy(&G.of);
        return 0;
    }
    G.have_ctx = 1;
    FfxOpticalflowSharedResourceDescriptions os = {};
    FfxFrameInterpolationSharedResourceDescriptions fs = {};
    if (ffxOpticalflowGetSharedResourceDescriptions(&G.of, &os) != FFX_OK ||
        ffxFrameInterpolationGetSharedResourceDescriptions(&G.fi, &fs) != FFX_OK)
        return 0;
    const FfxCreateResourceDescription *d[5] = {&os.opticalFlowVector, &os.opticalFlowSCD, &fs.dilatedDepth,
                                                &fs.dilatedMotionVectors, &fs.reconstructedPrevNearestDepth};
    for (int i = 0; i < 5; i++) {
        if (G.iface.fpCreateResource(&G.iface, d[i], G.shared_id, &G.res[i]) != FFX_OK) return 0;
        G.nres = i + 1;
    }
    G.w = w, G.h = h, G.fmt = fmt, G.frame_id = 0, G.cleared = 0;
    rt_log("fg: frame generation for %dx%d (format %d)", w, h, (int)fmt);
    return 1;
}
}   // namespace

// The FidelityFX resource of one of vk_fg_mv.c's images (GENERAL, storage and sampled).
FfxResource res_ext(VkImage image, VkFormat f, uint32_t w, uint32_t h, const wchar_t *name) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    return ffxGetResourceVK((void *)image, ffxGetImageResourceDescriptionVK(image, ci, FFX_RESOURCE_USAGE_READ_ONLY), name,
                            FFX_RESOURCE_STATE_UNORDERED_ACCESS);
}

extern "C" VkImage vk_fg_interpolate(VkImage src, VkFormat src_fmt, int w, int h, const int crop[4], int mv_slot,
                                     VkFormat *out_fmt) {
    VkCtx *c = vk_ctx();
    if (!vk_fg_on() || G.failed || !c) return VK_NULL_HANDLE;
    int cx = 0, cy = 0, cw = w, ch = h;
    if (crop && crop[2] > 0 && crop[3] > 0) cx = crop[0], cy = crop[1], cw = crop[2], ch = crop[3];
    if (cx < 0 || cy < 0 || cx + cw > w || cy + ch > h || cw < 64 || ch < 64) return VK_NULL_HANDLE;
    VkFormat fmt = storage_format(src_fmt);
    uint64_t now = kern_now_ns();
    int reset = 0;
    if (cw != G.w || ch != G.h || fmt != G.fmt) {
        destroy_ctx(c);
        if (!create_ctx(c, fmt, cw, ch)) {
            rt_log("fg: frame generation unavailable for %dx%d format %d; off", cw, ch, (int)fmt);
            destroy_ctx(c);
            G.failed = 1;
            return VK_NULL_HANDLE;
        }
        reset = 1;
    }
    if (G.last_ns && now - G.last_ns > 200000000ull) reset = 1;   // a pause (loading, menu): no blend across it
    float dt_ms = G.last_ns ? (float)((now - G.last_ns) / 1e6) : 16.6f;
    G.last_ns = now;

    vkWaitForFences(c->device, 1, &G.fence, VK_TRUE, 5000000000ull);
    vkResetFences(c->device, 1, &G.fence);
    vkResetCommandBuffer(G.cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    vkBeginCommandBuffer(G.cb, &bi);
    if (!G.cleared) {   // own images to GENERAL; depth far (1) and motion vectors 0 for good
        Img *all[5] = {&G.cur, &G.out, &G.depth, &G.mv, &G.hud};
        for (Img *m : all) to_general(G.cb, m->image);
        VkClearColorValue one = {{0, 0, 0, 0}}, zero = {{0, 0, 0, 0}};   // reversed depth: 0 is far
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(G.cb, G.depth.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &r);
        vkCmdClearColorImage(G.cb, G.mv.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
        barrier(G.cb, G.depth.image, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        barrier(G.cb, G.mv.image, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        G.cleared = 1;
    }
    // the game's picture: earlier submissions' writes visible to the copy; the copy waits for this command buffer's
    // previous reads of cur (last frame's interpolation)
    barrier(G.cb, src, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(G.cb, G.cur.image, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy cp = {};
    cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.srcOffset = {cx, cy, 0};
    cp.extent = {(uint32_t)cw, (uint32_t)ch, 1};
    vkCmdCopyImage(G.cb, src, VK_IMAGE_LAYOUT_GENERAL, G.cur.image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    barrier(G.cb, src, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    barrier(G.cb, G.cur.image, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    barrier(G.cb, G.out.image, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    FfxCommandList cl = ffxGetCommandListVK(G.cb);
    FfxResource of_vec = G.iface.fpGetResource(&G.iface, G.res[0]), of_scd = G.iface.fpGetResource(&G.iface, G.res[1]);
    FfxResource dd = G.iface.fpGetResource(&G.iface, G.res[2]), dm = G.iface.fpGetResource(&G.iface, G.res[3]),
                pd = G.iface.fpGetResource(&G.iface, G.res[4]);
    FfxResource cur = res_of(&G.cur, L"KAR_FG_Current"), out = res_of(&G.out, L"KAR_FG_Output");
    FfxErrorCode e = FFX_OK;

    // B3: the scene without the interface, from this picture's DLSS output copy (vk_fg_mv.c), in the final picture's
    // encoding (KAR_FG_HUDMODE: 0 as stored, 1 square root (default), 2 sRGB decode + square root, -1 off)
    static int hmode = -2;
    if (hmode == -2) hmode = getenv("KAR_FG_HUDMODE") ? atoi(getenv("KAR_FG_HUDMODE")) : 1;
    VkFgMv hv = {};
    int have_hud = hmode >= 0 && mv_slot && vk_fg_mv_get(mv_slot, &hv) && hv.hud && G.hud_view && hudless_pipe(c);
    VkImageView hsv = have_hud ? hsrc_view(c, hv.hud, hv.hud_fmt) : VK_NULL_HANDLE;
    have_hud = have_hud && hsv;
    if (have_hud) {
        barrier(G.cb, G.hud.image, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        VkDescriptorImageInfo ii[2] = {{G.hl_sampler, hsv, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, G.hud_view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet wr[2] = {};
        for (int i = 0; i < 2; i++) {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstBinding = (uint32_t)i, wr[i].descriptorCount = 1, wr[i].pImageInfo = &ii[i];
            wr[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        float pc[4] = {(float)cw, (float)ch, (float)hmode, 0};
        vkCmdBindPipeline(G.cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.hl_pipe);
        G.push(G.cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.hl_layout, 0, 2, wr);
        vkCmdPushConstants(G.cb, G.hl_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(G.cb, (cw + 7) / 8, (ch + 7) / 8, 1);
        barrier(G.cb, G.hud.image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }
    static int told_hud = -1;
    if (have_hud != told_hud) {
        told_hud = have_hud;
        rt_log("fg: %s", have_hud ? "picture without the HUD in use" : "no picture without the HUD");
    }
    FfxResource hud = have_hud ? res_of(&G.hud, L"KAR_FG_HUDLess") : FfxResource{};

    FfxOpticalflowDispatchDescription od = {};
    od.commandList = cl;
    od.color = have_hud ? hud : cur;
    od.opticalFlowVector = of_vec;
    od.opticalFlowSCD = of_scd;
    od.reset = reset;
    od.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    od.minMaxLuminance = {0.0f, 1.0f};
    e |= ffxOpticalflowContextDispatch(&G.of, &od);

    FfxFrameInterpolationPrepareDescription pd_ = {};
    pd_.commandList = cl;
    // camera motion vectors of this picture (vk_fg_mv.c) at the scene's render size, else the constant zero ones
    VkFgMv mv = {};
    int have_mv = mv_slot && vk_fg_mv_get(mv_slot, &mv) && mv.w <= (uint32_t)cw && mv.h <= (uint32_t)ch;
    static int told_mv = -1;
    if (have_mv != told_mv) {
        told_mv = have_mv;
        rt_log("fg: %s", have_mv ? "camera motion vectors in use" : "no camera motion vectors: zero ones");
    }
    pd_.renderSize = have_mv ? FfxDimensions2D{mv.w, mv.h} : FfxDimensions2D{(uint32_t)cw, (uint32_t)ch};
    pd_.motionVectorScale = {1.0f, 1.0f};
    pd_.frameTimeDelta = dt_ms;
    pd_.cameraNear = have_mv ? mv.nearv : 0.1f, pd_.cameraFar = have_mv ? mv.farv : 1000.0f;
    pd_.viewSpaceToMetersFactor = 1.0f;
    pd_.cameraFovAngleVertical = have_mv ? mv.fov : 1.0f;
    pd_.depth = have_mv ? res_ext(mv.depth, VK_FORMAT_R32_SFLOAT, mv.w, mv.h, L"KAR_FG_Depth")
                        : res_of(&G.depth, L"KAR_FG_Depth");
    pd_.motionVectors = have_mv ? res_ext(mv.mv, VK_FORMAT_R16G16_SFLOAT, mv.w, mv.h, L"KAR_FG_MotionVectors")
                                : res_of(&G.mv, L"KAR_FG_MotionVectors");
    pd_.frameID = G.frame_id;
    pd_.dilatedDepth = dd, pd_.dilatedMotionVectors = dm, pd_.reconstructedPrevDepth = pd;
    e |= ffxFrameInterpolationPrepare(&G.fi, &pd_);

    FfxFrameInterpolationDispatchDescription fd = {};
    fd.commandList = cl;
    {   // KAR_FG_DEBUG=1 (diagnostic): the SDK's debug view (motion fields, optical flow, scene change) as output
        static int dbg = -1;
        if (dbg < 0) dbg = getenv("KAR_FG_DEBUG") && *getenv("KAR_FG_DEBUG") == '1';
        if (dbg) fd.flags |= FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW;
    }
    fd.displaySize = {(uint32_t)cw, (uint32_t)ch};
    fd.renderSize = pd_.renderSize;
    fd.currentBackBuffer = cur;
    if (have_hud) fd.currentBackBuffer_HUDLess = hud;
    fd.output = out;
    fd.interpolationRect = {0, 0, cw, ch};
    fd.opticalFlowVector = of_vec;
    fd.opticalFlowSceneChangeDetection = of_scd;
    fd.opticalFlowBufferSize = {of_vec.description.width, of_vec.description.height};
    fd.opticalFlowScale = {1.0f / cw, 1.0f / ch};
    fd.opticalFlowBlockSize = 8;
    fd.cameraNear = pd_.cameraNear, fd.cameraFar = pd_.cameraFar;
    fd.cameraFovAngleVertical = pd_.cameraFovAngleVertical, fd.viewSpaceToMetersFactor = 1.0f;
    fd.frameTimeDelta = dt_ms;
    fd.reset = reset;
    fd.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    fd.minMaxLuminance[0] = 0.0f, fd.minMaxLuminance[1] = 1.0f;
    fd.frameID = G.frame_id;
    fd.dilatedDepth = dd, fd.dilatedMotionVectors = dm, fd.reconstructedPrevDepth = pd;
    e |= ffxFrameInterpolationDispatch(&G.fi, &fd);
    barrier(G.cb, G.out.image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    vkEndCommandBuffer(G.cb);

    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &G.cb;
    vk_queue_lock();
    VkResult r = vkQueueSubmit(c->gfx_queue, 1, &si, G.fence);
    vk_queue_unlock();
    G.frame_id++;
    G.calls++;
    if (e != FFX_OK || r != VK_SUCCESS) {
        rt_log("fg: dispatch failed (ffx %#x, vk %d); off", (unsigned)e, (int)r);
        G.failed = 1;
        return VK_NULL_HANDLE;
    }
    dump_pair(c, !reset && G.calls >= 2);
    if (reset || G.calls < 2) return VK_NULL_HANDLE;   // nothing before this picture to interpolate from
    static uint64_t made;
    if (++made % 1800 == 0) rt_log("fg: %llu interpolated pictures so far", (unsigned long long)made);
    if (out_fmt) *out_fmt = fmt;
    return G.out.image;
}

extern "C" VkImage vk_fg_current(VkFormat *fmt) {
    *fmt = G.fmt;
    return G.cur.image;
}

extern "C" void vk_fg_release(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    destroy_ctx(c);
    if (G.pool) {
        vkDestroyFence(c->device, G.fence, nullptr);
        vkDestroyCommandPool(c->device, G.pool, nullptr);
    }
    G = Fg{};
}

#else
extern "C" VkImage vk_fg_interpolate(VkImage src, VkFormat src_fmt, int w, int h, const int crop[4], int mv_slot,
                                     VkFormat *out_fmt) {
    (void)src, (void)src_fmt, (void)w, (void)h, (void)crop, (void)mv_slot, (void)out_fmt;
    static int told;
    if (vk_fg_on() && !told++) rt_log("fg: KAR_FG=1, but this build has no FidelityFX SDK (tools/build_ffx.py)");
    return VK_NULL_HANDLE;
}
extern "C" void vk_fg_release(void) {}
extern "C" VkImage vk_fg_current(VkFormat *fmt) {
    *fmt = VK_FORMAT_UNDEFINED;
    return VK_NULL_HANDLE;
}
#endif
