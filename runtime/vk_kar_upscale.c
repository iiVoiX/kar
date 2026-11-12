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
