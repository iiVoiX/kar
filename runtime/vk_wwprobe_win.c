// KAR_WW_PROBE=1 (diagnostic, RELEASE_PLAN 16.4): does a GPU write into imported host memory mark its pages in the
// OS write watch? The VRAM mirror (vk_memory.c) writes GPU results back into imported guest memory and finds dirty
// pages with GetWriteWatch; if the device's writes count as writes there, every write-back is uploaded again.
// Once, from the present path: 2 MiB of write-watched memory, imported as a buffer, filled by the GPU, then queried.
#include <windows.h>

#include <stdint.h>

#include "host.h"
#include "vk.h"

typedef struct { VkBuffer buf; } Probe;

static int probe_record(VkCommandBuffer cb, uint64_t fence, uint64_t ticks, void *context) {
    (void)fence, (void)ticks;
    const Probe *p = context;
    vkCmdFillBuffer(cb, p->buf, 0, 1u << 20, 0x12345678u);   // the first MiB: 256 pages
    return 1;
}

static uint64_t watched(uint8_t *mem, SIZE_T size, int reset) {
    static PVOID pages[1024];
    ULONG_PTR n = 1024;
    DWORD gran;
    if (GetWriteWatch(reset ? WRITE_WATCH_FLAG_RESET : 0, mem, size, pages, &n, &gran)) return UINT64_MAX;
    return n;
}

void vk_ww_probe(void) {
    static int done;
    if (done++ || !getenv("KAR_WW_PROBE")) return;
    VkCtx *c = vk_ctx();
    const SIZE_T size = 2u << 20;
    uint8_t *mem = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!mem) { rt_log("ww probe: VirtualAlloc failed"); return; }
    memset(mem, 0, size);   // committed and touched: every page written once
    uint64_t before = watched(mem, size, 1), after_reset = watched(mem, size, 0);
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(c->device, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!props || props(c->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, mem, &hp) != VK_SUCCESS) {
        rt_log("ww probe: host pointer not importable");
        return;
    }
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    Probe p = {0};
    if (vkCreateBuffer(c->device, &bi, NULL, &p.buf) != VK_SUCCESS) { rt_log("ww probe: vkCreateBuffer failed"); return; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c->device, p.buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(c->phys, &mp);
    int type = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type < 0; i++)
        if ((req.memoryTypeBits & hp.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            type = (int)i;
    VkImportMemoryHostPointerInfoEXT im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                           .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                           .pHostPointer = mem};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &im, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)type};
    VkDeviceMemory dm;
    if (type < 0 || vkAllocateMemory(c->device, &ai, NULL, &dm) != VK_SUCCESS ||
        vkBindBufferMemory(c->device, p.buf, dm, 0) != VK_SUCCESS) {
        rt_log("ww probe: import failed");
        return;
    }
    uint64_t after_import = watched(mem, size, 1);
    uint64_t f = vk_sync_submit_record(probe_record, &p, 0);
    vk_sync_fence_wait(f, UINT64_MAX);
    uint64_t after_gpu = watched(mem, size, 1);
    int ok = *(volatile uint32_t *)mem == 0x12345678u;
    rt_log("ww probe: pages marked written: after memset %llu (reset), right after reset %llu, after import %llu, after "
           "a GPU fill of 256 pages %llu (fill %s)", (unsigned long long)before, (unsigned long long)after_reset,
           (unsigned long long)after_import, (unsigned long long)after_gpu, ok ? "visible" : "NOT visible");
}
