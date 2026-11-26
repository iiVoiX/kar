// Loader of the external upscaler add-on (ext_upscaler.h, RELEASE_PLAN.md 17.5). kar_upscaler.dll beside the exe is
// loaded once, before the Vulkan instance is made; without it, or when the add-on cannot run on this device, nothing
// changes. KAR_EXT_UPSCALER=0 skips the add-on.
#include <stdlib.h>
#include <string.h>
#include "cfg.h"
#include "ext_upscaler.h"
#include "host.h"
#ifdef _WIN32
#include <windows.h>
#endif

static struct {
    int tried, ready;
    const ExtUpscalerApi *api;
    const char *dev_ext[16];
    uint32_t ndev_ext;
} X;

static const ExtUpscalerApi *api(void) {
    if (X.tried) return X.api;
    X.tried = 1;
#ifdef _WIN32
    const char *e = getenv("KAR_EXT_UPSCALER");
    if (e && *e == '0') return NULL;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return NULL;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash || (size_t)(slash + 1 - path) + 32 >= MAX_PATH) return NULL;
    wcscpy(slash + 1, L"" EXT_UPSCALER_DLL);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return NULL;
    HMODULE m = LoadLibraryW(path);
    if (!m) { rt_log("ext upscaler: %s present but not loadable (%lu)", EXT_UPSCALER_DLL, GetLastError()); return NULL; }
    ExtUpscalerGetFn get = (ExtUpscalerGetFn)(void *)GetProcAddress(m, EXT_UPSCALER_ENTRY);
    const ExtUpscalerApi *a = get ? get() : NULL;
    if (!a || a->version < 1 || !a->init || !a->evaluate) {   // version 1 add-ons load too (no frame generation)
        rt_log("ext upscaler: %s has no usable interface", EXT_UPSCALER_DLL);
        return NULL;
    }
    X.api = a;
    rt_log("ext upscaler: loaded %s (%s)", EXT_UPSCALER_DLL, a->name ? a->name : "?");
#endif
    return X.api;
}

uint32_t ext_upscaler_instance_ext(const char **out, uint32_t room) {
    const ExtUpscalerApi *a = api();
    const char *const *names = NULL;
    uint32_t n = 0, k = 0;
    if (!a || !a->instance_extensions || !a->instance_extensions(&names, &n)) return 0;
    for (uint32_t i = 0; i < n && k < room; i++) out[k++] = names[i];
    return k;
}

uint32_t ext_upscaler_device_ext(VkInstance instance, VkPhysicalDevice phys, const VkExtensionProperties *have,
                                 uint32_t nhave, const char **out, uint32_t room) {
    const ExtUpscalerApi *a = api();
    const char *const *names = NULL;
    uint32_t n = 0, k = 0;
    if (!a || !a->device_extensions || !a->device_extensions(instance, phys, vkGetInstanceProcAddr, &names, &n)) return 0;
    for (uint32_t i = 0; i < n; i++) {
        int present = 0;
        for (uint32_t j = 0; j < nhave && !present; j++) present = !strcmp(have[j].extensionName, names[i]);
        if (!present) {   // the add-on cannot run here: enable none of its extensions
            rt_log("ext upscaler: device lacks %s; add-on off", names[i]);
            X.api = NULL;
            return 0;
        }
    }
    for (uint32_t i = 0; i < n && k < room; i++) out[k++] = names[i];
    return k;
}

void ext_upscaler_device_ready(VkInstance instance, VkPhysicalDevice phys, VkDevice device) {
    const ExtUpscalerApi *a = X.api;
    if (!a) return;
    ExtUpscalerSetup s = {EXT_UPSCALER_VERSION, instance, phys, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, L"."};
    X.ready = a->init(&s);
    rt_log("ext upscaler: %s %s", a->name ? a->name : "add-on", X.ready ? "ready" : "failed to start; off");
}

const char *ext_upscaler_name(void) { return X.ready && X.api ? (X.api->name ? X.api->name : "External") : NULL; }

int ext_upscaler_evaluate(const ExtUpscalerFrame *f) { return X.ready && X.api->evaluate(f); }

// ---- frame generation by the add-on (17.6)
static int fg_on, fg_ok;
int ext_fg_preinit(void) {
    const char *set = cfg_get("TOTK_FRAME_GEN");
    if (!set || strcmp(set, "dlss")) return 0;
    // experimental: Streamline's device creation crashed on the first try (RELEASE_PLAN 17.6); KAR_DLSSG=1 to test
    if (!getenv("KAR_DLSSG") || *getenv("KAR_DLSSG") != '1') {
        rt_log("frame generation: dlss is experimental (set KAR_DLSSG=1 to try); none this run");
        return 0;
    }
    const ExtUpscalerApi *a = api();
    if (!a || a->version < 2 || !a->fg_preinit || !a->fg_vk_proc) {
        rt_log("frame generation: dlss asked for, but %s has none", EXT_UPSCALER_DLL);
        return 0;
    }
    wchar_t dir[260] = L".";
    fg_on = a->fg_preinit(dir);
    rt_log("frame generation: add-on %s", fg_on ? "loaded; instance and device through it" : "could not start");
    return fg_on;
}
PFN_vkVoidFunction ext_fg_proc(const char *name) { return fg_on ? X.api->fg_vk_proc(name) : NULL; }
void ext_fg_device_ready(VkInstance instance, VkPhysicalDevice phys, VkDevice device) {
    if (!fg_on || !X.api->fg_device_ready) return;
    fg_ok = X.api->fg_device_ready(instance, phys, device);
    rt_log("frame generation: %s on this device", fg_ok ? "supported" : "not supported (see the add-on's log)");
}
int ext_fg_supported(void) { return fg_ok; }
