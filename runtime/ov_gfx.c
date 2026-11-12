// Adapted from MacNativeGames games/totk/runtime/frontend.m (nn::vi, lines 345-359) and gfx_bridge.c (nv, lines
// 74-86) @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames contributors). The nvnBootstrapLoader stop takes the place
// of MacNativeGames' NVN entry (third_party/d2r_gfx/gfx/nvn/hle_nvn.c:115-118); no code of it is copied.
// Graphics bring-up:
//   nv::*        record the game's allocator callbacks and graphics work memory (guest addresses) for the NVN layer
//   nn::vi::*    display, layer and native window identities; the vsync event is bound here
//   nvnBootstrapLoader   the end of --boot (rt_boot_event "nvn"); otherwise the synthetic address of an NVN proc (nvn_procs.c)
#include <stdint.h>
#include <stdlib.h>

#include "gfx.h"
#include "host.h"
#include "ov.h"
#include "platform/platform.h"

#define VI_R_INVALID 0xf601u   // MacNativeGames' result for a call on an unknown display or layer

static OvNvState nv;
static PlatMutex lock = PLAT_MUTEX_INIT;

const OvNvState *ov_nv_state(void) { return &nv; }

static void set_allocator(Cpu *c, OvNvAllocator *a) {
    plat_mutex_lock(&lock);
    *a = (OvNvAllocator){arg_x(c, 0), arg_x(c, 1), arg_x(c, 2), arg_x(c, 3)};
    plat_mutex_unlock(&lock);
}

// void nv::SetGraphicsAllocator(AllocateFunction, FreeFunction, ReallocateFunction, void *user)
static void ov_graphics_allocator(Cpu *c) { set_allocator(c, &nv.graphics); }
// void nv::SetGraphicsDevtoolsAllocator(AllocateFunction, FreeFunction, ReallocateFunction, void *user)
static void ov_devtools_allocator(Cpu *c) { set_allocator(c, &nv.devtools); }

// void nv::InitializeGraphics(void *memory, size_t size). TotK hands over a work buffer. Donkey Kong Bananza (Switch 2)
// passes (0, 0) and leaves the driver to its allocator callbacks; nothing on the host reads the buffer, so that is fine.
static void ov_initialize_graphics(Cpu *c) {
    uint64_t mem = arg_x(c, 0), size = arg_x(c, 1);
    if (!mem != !size) rt_fatal("nv::InitializeGraphics: work memory %#llx with size %#llx", (unsigned long long)mem,
                                (unsigned long long)size);
    plat_mutex_lock(&lock);
    nv.memory = mem;
    nv.memory_size = size;
    nv.initialized = 1;
    plat_mutex_unlock(&lock);
    rt_log("nv: graphics work memory %#llx+%#llx recorded", (unsigned long long)mem,
           (unsigned long long)size);
}

// void nv::InitCheck(): aborts unless nv::InitializeGraphics ran, and that call is the host's (above), so the SDK's own
// flag is never set. Bananza calls it before nn::codec::video::Initialize when a tutorial guide movie starts.
static void ov_init_check(Cpu *c) { (void)c; }

// ---- nn::vi: the identities are small zeroed blocks in the guest-visible kernel region, in case SDK code reads them
static int vi_ready;
static void *display, *layer, *window;

// void nn::vi::Initialize()
static void ov_vi_initialize(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    if (!display) {
        display = mem_kernel_alloc(64, 16);
        layer = mem_kernel_alloc(64, 16);
        window = mem_kernel_alloc(64, 16);
    }
    vi_ready = 1;
    plat_mutex_unlock(&lock);
}

static int vi_up(void) {
    plat_mutex_lock_shared(&lock);
    int r = vi_ready;
    plat_mutex_unlock_shared(&lock);
    return r;
}

// Result nn::vi::OpenDefaultDisplay(Display **out)
static void ov_open_display(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up()) { ret_x(c, VI_R_INVALID); return; }
    *out = display;
    ret_x(c, 0);
}

// Result nn::vi::CreateLayer(Layer **out, Display *display)
static void ov_create_layer(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up() || arg_p(c, 1) != display) { ret_x(c, VI_R_INVALID); return; }
    *out = layer;
    ret_x(c, 0);
}

// Result nn::vi::GetNativeWindow(void **out, Layer *layer): a placeholder the NVN layer recognises
static void ov_native_window(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up() || arg_p(c, 1) != layer) { ret_x(c, VI_R_INVALID); return; }
    *out = window;
    ret_x(c, 0);
}

// Result nn::vi::GetDisplayVsyncEvent(os::SystemEventType *event, Display *display): auto-clear system event
static void ov_vsync_event(Cpu *c) {
    uint64_t ev = arg_x(c, 0);
    if (!ev || !vi_up() || arg_p(c, 1) != display) { ret_x(c, VI_R_INVALID); return; }
    kern_bind_system_event(c, ev, 1);
    gfx_vsync_bind(ev);
    ret_x(c, 0);
}

// ---- Switch 2 vi calls (Donkey Kong Bananza): the host presents SDR with a fixed swap interval and no VRR.
// Result QueryDisplayHdrStatus(bool *enabled, HdrOutputFormat *format, u16 *?, const Display *): HDR off
// All three outputs are written: left as they were, the format and the u16 (a luminance in nits, read by Kirby Air
// Riders' colour grading) kept stack garbage, and the grading LUT mapped to a ~2000-nit HDR range that the SDR
// output then clipped (a white, glowing Kirby in dark scenes). SDR: format 0 and 100 nits; KAR_DISPLAY_NITS overrides.
static void ov_hdr_status(Cpu *c) {
    uint8_t *on = arg_p(c, 0);
    uint32_t *fmt = arg_p(c, 1);
    uint16_t *nits = arg_p(c, 2);
    const char *e = getenv("KAR_DISPLAY_NITS");
    uint16_t v = e && atoi(e) > 0 ? (uint16_t)atoi(e) : 100;
    static int told;
    if (!told++)
        rt_log("vi: QueryDisplayHdrStatus: SDR, %u nits (before: format %#x, u16 %u)", v, fmt ? *fmt : 0, nits ? *nits : 0);
    if (on) *on = 0;
    if (fmt) *fmt = 0;
    if (nits) *nits = v;
    ret_x(c, 0);
}
// Diagnostic: the content luminance the game announces per frame (first few calls).
static void ov_hdr_lum(Cpu *c) {
    static int told;
    if (told++ < 4) rt_log("vi: SetLayerNextHdr*ContentLuminanceRangeMetadata(%u, %u)", (unsigned)(arg_x(c, 1) & 0xffff),
                           (unsigned)(arg_x(c, 2) & 0xffff));
    ret_x(c, 0);
}
// Result QueryDisplayHdrPaperWhite(u16 *nits, const Display *): the BT.2408 reference white, 203 nits
static void ov_hdr_paper_white(Cpu *c) {
    uint16_t *nits = arg_p(c, 0);
    if (nits) *nits = 203;
    ret_x(c, 0);
}
// Result QueryDisplayVrrSupported(bool *out, const Display *): no
static void ov_vrr_supported(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, 0);
}
// Result SetDisplayVrrCapability(Display *, bool), Set/ClearLayerExtendedSwapInterval(Layer *[, int, int]),
// SetLayerNextHdr{Extended,Standard}ContentLuminanceRangeMetadata(Layer *, u16, u16): accepted, nothing changes
static void ov_vi_accept(Cpu *c) { ret_x(c, 0); }

HOST_OVERRIDE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", ov_hdr_status);
HOST_OVERRIDE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", ov_hdr_paper_white);
HOST_OVERRIDE("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", ov_vrr_supported);
HOST_OVERRIDE("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", ov_vi_accept);
HOST_OVERRIDE("_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", ov_vi_accept);
HOST_OVERRIDE("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", ov_vi_accept);
HOST_OVERRIDE("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", ov_hdr_lum);
HOST_OVERRIDE("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", ov_hdr_lum);
HOST_OVERRIDE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", ov_vi_accept);
HOST_OVERRIDE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", ov_vi_accept);

// void *nvnBootstrapLoader(const char *name): the first NVN call
static void ov_nvn_bootstrap(Cpu *c) {
    const char *name = arg_p(c, 0);
    if (!name) name = "(null)";
    static int32_t first = 1;
    if (rt_config()->boot && plat_xchg32(&first, 0)) rt_boot_event("nvn", c, name);   // ends --boot; returns
                                                                                                // when --seconds goes on
    ret_x(c, nvn_proc_address(name));
}

// The NVN procs need their synthetic addresses before hostfn_seal; the loader seals at the end of rt_boot, so the
// registration runs at start-up instead of through boot.c.
__attribute__((constructor)) static void ov_nvn_procs_register(void) { nvn_procs_register(); }

HOST_OVERRIDE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_graphics_allocator);
HOST_OVERRIDE("_ZN2nv18InitializeGraphicsEPvm", ov_initialize_graphics);
HOST_OVERRIDE("_ZN2nv9InitCheckEv", ov_init_check);
HOST_OVERRIDE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_devtools_allocator);
HOST_OVERRIDE("_ZN2nn2vi10InitializeEv", ov_vi_initialize);
HOST_OVERRIDE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", ov_open_display);
HOST_OVERRIDE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", ov_create_layer);
HOST_OVERRIDE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", ov_native_window);
HOST_OVERRIDE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", ov_vsync_event);
HOST_OVERRIDE("nvnBootstrapLoader", ov_nvn_bootstrap);
