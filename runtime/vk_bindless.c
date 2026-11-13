// Global descriptor set for bindless textures (RELEASE_PLAN.md 16.2, Kirby Air Riders). Kirby's material arrays put
// about 171 of the ~187 descriptors a draw binds (KAR_POOL_STATS / TOTK_BIND_STATS); writing them per draw is most of
// the replay's bind time. This set mirrors the game's texture and sampler pools instead: one sampled-image array per
// image type the material arrays use (2D, 2D array, cube; 2D depth aliases the 2D array) indexed by the texture id of
// the handle (h & 0xfffff), and one sampler array indexed by the sampler id (h >> 20). An entry is written once, when a
// draw first resolves that id through the existing path (tex_info), and again only when what it resolved to changes,
// so the per-draw path keeps its lazy upload, storage checks and barriers but stops writing descriptors.
//
// Step 1 (this file): the set, its layout and the writes, behind KAR_BINDLESS=1 for kar* targets on devices with
// descriptor indexing; nothing uses it yet.
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define BL_IMAGES 65792u    // Kirby's texture pool (pool stats: texture pool of 65792)
#define BL_SAMPLERS 4096u   // its sampler pool is 768; NVN allows 4096
#define BL_KINDS 3          // 0 2D (and 2D depth), 1 2D array, 2 cube
#define BL_BATCH 512

static struct {
    int state;   // 0 not tried, 1 active, -1 off or unsupported
    VkDescriptorSetLayout layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    PlatMutex mu;
    VkDescriptorImageInfo info[BL_BATCH];
    VkWriteDescriptorSet w[BL_BATCH];
    int n;
    uint64_t writes;
} BL;

static int supported(VkCtx *c) {
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    const char *miss = !f12.descriptorBindingPartiallyBound ? "descriptorBindingPartiallyBound"
                     : !f12.descriptorBindingSampledImageUpdateAfterBind ? "descriptorBindingSampledImageUpdateAfterBind"
                     : !f12.descriptorBindingUpdateUnusedWhilePending ? "descriptorBindingUpdateUnusedWhilePending"
                     : !f12.shaderSampledImageArrayNonUniformIndexing ? "shaderSampledImageArrayNonUniformIndexing"
                     : p12.maxDescriptorSetUpdateAfterBindSampledImages < BL_IMAGES * BL_KINDS ? "sampled images limit"
                     : p12.maxDescriptorSetUpdateAfterBindSamplers < BL_SAMPLERS ? "samplers limit"
                     : p12.maxPerStageDescriptorUpdateAfterBindSampledImages < BL_IMAGES * BL_KINDS ? "per-stage images limit"
                     : NULL;
    if (miss) rt_log("vk bindless: off, the device lacks %s", miss);
    return !miss;
}

static int create(VkCtx *c) {
    VkDescriptorSetLayoutBinding b[BL_KINDS + 1];
    VkDescriptorBindingFlags fl[BL_KINDS + 1];
    for (int k = 0; k <= BL_KINDS; k++) {
        b[k] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)k,
                                              .descriptorType = k < BL_KINDS ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                                             : VK_DESCRIPTOR_TYPE_SAMPLER,
                                              .descriptorCount = k < BL_KINDS ? BL_IMAGES : BL_SAMPLERS,
                                              .stageFlags = VK_SHADER_STAGE_ALL};
        fl[k] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = BL_KINDS + 1,
        .pBindingFlags = fl};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bf,
                                          .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                          .bindingCount = BL_KINDS + 1, .pBindings = b};
    if (vkCreateDescriptorSetLayout(c->device, &li, NULL, &BL.layout) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, BL_IMAGES * BL_KINDS},
                                  {VK_DESCRIPTOR_TYPE_SAMPLER, BL_SAMPLERS}};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, .maxSets = 1,
                                     .poolSizeCount = 2, .pPoolSizes = ps};
    if (vkCreateDescriptorPool(c->device, &pi, NULL, &BL.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = BL.pool,
                                      .descriptorSetCount = 1, .pSetLayouts = &BL.layout};
    return vkAllocateDescriptorSets(c->device, &ai, &BL.set) == VK_SUCCESS;
}

// Whether the bindless set exists (first call creates it). KAR_BINDLESS=1, kar* only, after vk_init.
int vk_bindless_active(void) {
    if (BL.state) return BL.state > 0;
    VkCtx *c = vk_ctx();
    if (!c) return 0;
    const char *e = getenv("KAR_BINDLESS");
    BL.state = -1;
    if (!rt_game_is_kar() || !e || *e != '1' || !supported(c)) return 0;
    plat_mutex_init(&BL.mu);
    if (!create(c)) {
        rt_log("vk bindless: off, creating the set failed");
        return 0;
    }
    BL.state = 1;
    rt_log("vk bindless: global set with %u images x %d types and %u samplers", BL_IMAGES, BL_KINDS, BL_SAMPLERS);
    return 1;
}

VkDescriptorSetLayout vk_bindless_layout(void) { return vk_bindless_active() ? BL.layout : VK_NULL_HANDLE; }
VkDescriptorSet vk_bindless_set(void) { return vk_bindless_active() ? BL.set : VK_NULL_HANDLE; }

static void flush_locked(void) {
    if (!BL.n) return;
    vkUpdateDescriptorSets(vk_ctx()->device, (uint32_t)BL.n, BL.w, 0, NULL);
    BL.writes += (uint64_t)BL.n;
    BL.n = 0;
}

static void put(uint32_t binding, uint32_t index, VkDescriptorType type, VkDescriptorImageInfo ii) {
    plat_mutex_lock(&BL.mu);
    if (BL.n == BL_BATCH) flush_locked();
    BL.info[BL.n] = ii;
    BL.w[BL.n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = BL.set,
                                        .dstBinding = binding, .dstArrayElement = index, .descriptorCount = 1,
                                        .descriptorType = type, .pImageInfo = &BL.info[BL.n]};
    BL.n++;
    plat_mutex_unlock(&BL.mu);
}

// Texture id `id` of image type `kind` (0 2D, 1 2D array, 2 cube) now resolves to view/layout.
void vk_bindless_put_image(int kind, uint32_t id, VkImageView view, VkImageLayout layout) {
    if (!vk_bindless_active() || kind < 0 || kind >= BL_KINDS || id >= BL_IMAGES || !view) return;
    put((uint32_t)kind, id, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, (VkDescriptorImageInfo){VK_NULL_HANDLE, view, layout});
}

void vk_bindless_put_sampler(uint32_t id, VkSampler s) {
    if (!vk_bindless_active() || id >= BL_SAMPLERS || !s) return;
    put(BL_KINDS, id, VK_DESCRIPTOR_TYPE_SAMPLER, (VkDescriptorImageInfo){s, VK_NULL_HANDLE, 0});
}

// Before a submission that may read the set: the pending writes land (update-after-bind allows it after binding).
void vk_bindless_flush(void) {
    if (BL.state <= 0) return;
    plat_mutex_lock(&BL.mu);
    flush_locked();
    plat_mutex_unlock(&BL.mu);
}

uint64_t vk_bindless_writes(void) { return BL.writes; }
