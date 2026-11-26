// External upscaler add-on interface (RELEASE_PLAN.md 17.5). This file is also available under the MIT licence
// (LICENSE-MIT), so an add-on may carry a copy of these declarations under its own terms. An optional DLL beside the exe (kar_upscaler.dll) can
// take the place of the built-in upscalers in Kirby's upscale slot. It is a separate program: it exports one function,
// ext_upscaler_get, returning the table below, and the exe talks to it through this interface alone. The add-on keeps
// its own copy of these declarations; the layout is versioned by EXT_UPSCALER_VERSION and only ever grows at the end.
//
// Calls, all from the exe:
//   instance_extensions  before the Vulkan instance exists: instance extensions the add-on needs.
//   device_extensions    before the device exists: device extensions it needs on that physical device.
//   init                 once the device exists; 0 means the add-on cannot run (the exe keeps its own upscalers).
//   evaluate             in the replay, outside a render pass, on the replay's command buffer: records the upscale of
//                        one frame. Every image is in VK_IMAGE_LAYOUT_GENERAL before the call and must be again after.
//   shutdown             before the device goes away.
// Version 2 adds optional frame generation by the add-on (RELEASE_PLAN 17.6); an add-on that leaves these NULL has none:
//   fg_preinit           before the Vulkan instance exists, when the user asked for the add-on's frame generation: 1 when
//                        it can run (its libraries loaded); the exe then creates instance and device through fg_vk_proc.
//   fg_vk_proc           the add-on's own version of a Vulkan entry point by name (instance and device creation, the
//                        physical-device enumeration,
//                        surface, swapchain, acquire, present and wait-idle calls), NULL for the exe's normal one.
//   fg_device_ready      once the device exists: 1 when frame generation is supported on it.
// Version 3, per presented game picture on the present thread:
//   fg_frame             before the picture's blit to the swapchain: its depth, motion vectors and camera (below);
//                        2 asks the exe to build the swapchain again (frame generation was just switched on).
//   fg_marker            EXT_FG_PRESENT_START just before the present, EXT_FG_PRESENT_END after it.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#define EXT_UPSCALER_VERSION 3
#define EXT_UPSCALER_ENTRY "ext_upscaler_get"
#define EXT_UPSCALER_DLL "kar_upscaler.dll"

typedef struct ExtUpscalerSetup {
    uint32_t version;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    PFN_vkGetInstanceProcAddr get_instance_proc;
    PFN_vkGetDeviceProcAddr get_device_proc;
    const wchar_t *data_dir;   // a folder the add-on may write logs and caches into
} ExtUpscalerSetup;

typedef struct ExtUpscalerFrame {
    VkCommandBuffer cb;
    VkImage color;           // the scene at render size, colour as the game encodes it (not HDR)
    VkFormat color_format;
    VkImage depth;           // device depth at render size, r32 float; near is 1 and far 0 when depth_inverted
    VkImage motion;          // rg16 float at render size, pixels from the current to the previous frame
    VkImage output;          // output size, rgba16 float, written as a storage image
    VkFormat output_format;
    uint32_t render_w, render_h, output_w, output_h;
    float jitter_x, jitter_y;   // the subpixel offset of this frame, render pixels (+x right, +y down)
    float motion_scale_x, motion_scale_y;
    int32_t reset;           // the history does not belong to this frame (cut, first frame)
    int32_t depth_inverted;
    float frame_ms, near_z, far_z, fov_y;
} ExtUpscalerFrame;

// One game picture for the add-on's frame generation. Images are in VK_IMAGE_LAYOUT_GENERAL and stay valid until the
// picture's present; depth and motion are at render size.
typedef struct ExtFgFrame {
    uint64_t frame;          // the game's frame number
    VkImage depth;           // r32 float, near 1 and far 0 when depth_inverted; VK_NULL_HANDLE: none this picture
    VkImage motion;          // rg16 float, pixels from the current to the previous picture
    uint32_t render_w, render_h, output_w, output_h;   // output: the swapchain extent
    float fov_y, near_z, far_z;   // radians; the camera's planes
    int32_t depth_inverted, reset;
} ExtFgFrame;
enum { EXT_FG_PRESENT_START = 4, EXT_FG_PRESENT_END = 5 };

typedef struct ExtUpscalerApi {
    uint32_t version;
    const char *name;        // shown in the port menu
    int (*instance_extensions)(const char *const **names, uint32_t *count);
    int (*device_extensions)(VkInstance instance, VkPhysicalDevice phys, PFN_vkGetInstanceProcAddr gipa,
                             const char *const **names, uint32_t *count);
    int (*init)(const ExtUpscalerSetup *setup);
    int (*evaluate)(const ExtUpscalerFrame *frame);   // 1 when recorded
    void (*shutdown)(void);
    // version 2
    int (*fg_preinit)(const wchar_t *data_dir);
    PFN_vkVoidFunction (*fg_vk_proc)(const char *name);
    int (*fg_device_ready)(VkInstance instance, VkPhysicalDevice phys, VkDevice device);
    // version 3
    int (*fg_frame)(const ExtFgFrame *frame);
    void (*fg_marker)(int marker);
} ExtUpscalerApi;

typedef const ExtUpscalerApi *(*ExtUpscalerGetFn)(void);

#ifdef __cplusplus
extern "C" {
#endif
// The exe side (ext_upscaler.c).
const char *ext_upscaler_name(void);   // NULL without a usable add-on
uint32_t ext_upscaler_instance_ext(const char **out, uint32_t room);
uint32_t ext_upscaler_device_ext(VkInstance instance, VkPhysicalDevice phys, const VkExtensionProperties *have,
                                 uint32_t nhave, const char **out, uint32_t room);
void ext_upscaler_device_ready(VkInstance instance, VkPhysicalDevice phys, VkDevice device);
int ext_upscaler_evaluate(const ExtUpscalerFrame *frame);
int ext_fg_preinit(void);                    // 1 when the add-on's frame generation is wanted (TOTK_FRAME_GEN=dlss) and loads
PFN_vkVoidFunction ext_fg_proc(const char *name);   // its version of a Vulkan call, or NULL
void ext_fg_device_ready(VkInstance instance, VkPhysicalDevice phys, VkDevice device);
int ext_fg_supported(void);                  // after the device: frame generation can run
int ext_fg_frame(const ExtFgFrame *frame);   // version 3 calls; nothing without a supported add-on
void ext_fg_marker(int marker);
int ext_fg_requested(void);   // the add-on's frame generation is asked for (settings only, before any device)
void vk_present_extent(int *w, int *h);      // gfx_present.c: the swapchain's size
// The add-on's version of a Vulkan call when its frame generation runs, else the normal one (cached per name).
// EXT_FG_VK(vkQueuePresentKHR)(queue, &info) at every call site of the calls the add-on lends.
PFN_vkVoidFunction ext_fg_resolve(const char *name, PFN_vkVoidFunction normal);
#define EXT_FG_VK(fn) ((PFN_##fn)ext_fg_resolve(#fn, (PFN_vkVoidFunction)fn))
#ifdef __cplusplus
}
#endif
