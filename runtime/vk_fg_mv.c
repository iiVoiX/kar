// Frame generation camera motion vectors (RELEASE_PLAN.md 16.7 B2). The game renders none (its DLSS class is never
// built), so they come from the scene's depth and the camera: kar_fg_cam.c keeps the scene camera's current and
// previous view-projection, and shaders/kar_fg_mv.comp reprojects every depth pixel into the previous frame.
//
// vk_fg_mv_record runs in the replay right after the DLSS stand-in's upscale (vk_exec_draw.c upscale_fill), where the
// scene's depth target of the upscale source's size is still this frame's. It writes into one of three slots (motion
// vectors rg16f in pixels, depth copy r32f); vk_backend.c takes the newest slot's number when the game presents and
// hands it to vk_fg.cpp with the present job. Three slots: the present thread may still interpolate the previous
// frame while this one is recorded, and queue order puts this recording after that frame's interpolation was submitted.
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "vk_exec.h"
#include "host.h"
#include "nvn_api.h"
#include "kar_fg_mv_spv.h"

int vk_fg_on(void);
int kar_fg_camera(uint32_t w, uint32_t h, float reproj[16], float dz[2], float *fov, float *nearv, float *farv);

#define SLOTS 3
static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler sampler;
    struct { VkImage image; VkFormat format; VkImageView view; } dview[4];
    int dnext;
    struct {
        VkImage mv, depth, dist;
        VmaAllocation mv_mem, depth_mem, dist_mem;
        VkImageView mv_view, depth_view, dist_view;
        uint32_t w, h;
        int fresh;
        VkFgMv info;
    } s[SLOTS];
    int next;
    volatile int32_t latest;   // slot + 1 of the newest recording, 0: none
} M;

static int init(VkCtx *c) {
    M.tried = 1;
    M.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!M.push) return 0;
    VkDescriptorSetLayoutBinding b[5] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 5, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 88};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &M.dsl) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &M.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &M.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &M.sampler) != VK_SUCCESS)
        return 0;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof kar_fg_mv_spv,
                                   .pCode = kar_fg_mv_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = M.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &M.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) return 0;
    rt_log("fg: camera motion vectors ready");
    return M.ok = 1;
}

static int make(VkCtx *c, VkImage *img, VmaAllocation *mem, VkImageView *view, VkFormat f, uint32_t w, uint32_t h) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, img, mem, NULL) != VK_SUCCESS) return *img = VK_NULL_HANDLE, 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, view) == VK_SUCCESS;
}

static int slot_images(VkCtx *c, int k, uint32_t w, uint32_t h) {
    typeof(M.s[0]) *s = &M.s[k];
    if (s->mv && s->w == w && s->h == h) return 1;
    if (s->mv) {   // a new size: the old images retire through the deferred destruction list
        vk_defer_view(s->mv_view), vk_defer_view(s->depth_view), vk_defer_view(s->dist_view);
        vk_defer_image(s->mv, s->mv_mem), vk_defer_image(s->depth, s->depth_mem), vk_defer_image(s->dist, s->dist_mem);
        memset(s, 0, sizeof *s);
    }
    if (!make(c, &s->mv, &s->mv_mem, &s->mv_view, VK_FORMAT_R16G16_SFLOAT, w, h) ||
        !make(c, &s->depth, &s->depth_mem, &s->depth_view, VK_FORMAT_R32_SFLOAT, w, h) ||
        !make(c, &s->dist, &s->dist_mem, &s->dist_view, VK_FORMAT_R32_SFLOAT, w, h))
        return 0;
    s->w = w, s->h = h, s->fresh = 1;
    return 1;
}

static VkImageView depth_view(VkCtx *c, VkImage image, VkFormat format) {
    for (int i = 0; i < 4; i++)
        if (M.dview[i].image == image && M.dview[i].format == format && M.dview[i].view) return M.dview[i].view;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                                .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(c->device, &vi, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    int k = M.dnext;
    M.dnext = (M.dnext + 1) % 4;
    if (M.dview[k].view) vk_defer_view(M.dview[k].view);
    M.dview[k].image = image, M.dview[k].format = format, M.dview[k].view = v;
    return v;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkPipelineStageFlags2 ss,
                          VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from,
                               .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// Records the motion vectors for the scene of size w x h into cb (replay, outside a pass, earlier writes visible).
// 1 when recorded (the caller rebinds its own compute pipeline afterwards).
int vk_fg_mv_record(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (!vk_fg_on()) return 0;
    VkFormat dfmt;
    VkImage depth = vk_exec_depth_newest(w, h, &dfmt);
    float reproj[16], dz[2], fov, nearv, farv;
    if (!depth || !kar_fg_camera(w, h, reproj, dz, &fov, &nearv, &farv)) return 0;
    VkCtx *c = vk_ctx();
    if (!M.tried) init(c);
    if (!M.ok) return 0;
    int k = M.next;
    VkImageView dv = depth_view(c, depth, dfmt);
    if (!dv || !slot_images(c, k, w, h)) return 0;
    typeof(M.s[0]) *s = &M.s[k];
    VkImageLayout from = s->fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    image_barrier(cb, s->mv, from, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    image_barrier(cb, s->depth, from, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    image_barrier(cb, s->dist, from, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    s->fresh = 0;
    // last frame's distances: the slot recorded before this one, when it holds the previous frame at this size (else
    // this slot's own image stands in and the check is off)
    typeof(M.s[0]) *ps = &M.s[(k + SLOTS - 1) % SLOTS];
    int prev_ok = ps->dist && ps->w == w && ps->h == h && ps->info.frame + 1 == __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    VkDescriptorImageInfo ii[5] = {{M.sampler, dv, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, s->mv_view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, s->depth_view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, prev_ok ? ps->dist_view : s->dist_view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, s->dist_view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[5];
    for (int i = 0; i < 5; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                       .descriptorCount = 1,
                                       .descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                           : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .pImageInfo = &ii[i]};
    // NDC y = +1 at the top row (NX2navi's convention too; with it the interpolated pictures are as sharp as the real
    // ones, mvF). KAR_FG_MVY=0: at the bottom row (diagnostic).
    static int flip = -1;
    if (flip < 0) flip = !(getenv("KAR_FG_MVY") && *getenv("KAR_FG_MVY") == '0');
    static int zcheck = -1;   // KAR_FG_ZCHECK=0: no depth check (diagnostic)
    if (zcheck < 0) zcheck = !(getenv("KAR_FG_ZCHECK") && *getenv("KAR_FG_ZCHECK") == '0');
    float pc[22];
    memcpy(pc, reproj, sizeof reproj);
    pc[16] = (float)w, pc[17] = (float)h, pc[18] = (float)flip, pc[19] = (float)(prev_ok && zcheck);
    pc[20] = dz[0], pc[21] = dz[1];
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.pipe);
    rec_push(M.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.layout, 0, 5, wr);
    vkCmdPushConstants(cb, M.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    image_barrier(cb, s->mv, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    image_barrier(cb, s->depth, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    image_barrier(cb, s->dist, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    s->info = (VkFgMv){s->mv, s->depth, w, h, fov, nearv, farv,
                       __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED)};
    M.next = (k + 1) % SLOTS;
    __atomic_store_n(&M.latest, k + 1, __ATOMIC_RELEASE);
    static int told;
    if (!told++) rt_log("fg: camera motion vectors %ux%u from depth format %d (fov %g, near %g, far %g)", w, h, (int)dfmt,
                        fov, nearv, farv);
    return 1;
}

// The newest slot when it was recorded in the replay of the frame now presented (its frame counter is one behind the
// present's), else 0: a frame without them (menu, loading) gets the zero motion vectors.
int vk_fg_mv_latest(void) {
    int k = __atomic_load_n(&M.latest, __ATOMIC_ACQUIRE);
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    return k && M.s[k - 1].info.frame + 1 >= now ? k : 0;
}

int vk_fg_mv_get(int slot, VkFgMv *out) {
    if (slot < 1 || slot > SLOTS || !M.s[slot - 1].info.mv) return 0;
    *out = M.s[slot - 1].info;
    return 1;
}
