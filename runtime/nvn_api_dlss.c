// NVN 2 DLSS (nvnDlssNetworkBuilder*, nvnDlssNetwork*, nvnDlssParameters*, nvnCommandBufferEvaluateDlss) for Kirby Air
// Riders, which upscales its 3D view with DLSS (1536x864 rgb10a2 -> 1920x1080 rgb10a2 in the attract demo). No public
// header gives these signatures, so this is a recording stand-in: every setter keeps its raw arguments per object
// (the builder, network or parameters pointer in x0), every getter answers from what the matching setter kept, the
// network initializes, and every call is logged (KAR_DLSS_LOG=n logs the first n calls, default 400) so a run shows
// the game's sequence. EvaluateDlss replays as an upscale of the input colour into the output colour
// (vk side: OP_DLSS), once the argument shapes are confirmed from the log.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_obj.h"
#include "platform/platform.h"

enum { MAX_OBJ = 64, MAX_KEY = 64 };
typedef struct { uint64_t obj; uint8_t set[MAX_KEY]; uint64_t v[MAX_KEY][4]; float f[MAX_KEY][2]; } DlssObj;
static DlssObj objs[MAX_OBJ];
static int nobjs;
static PlatMutex lock = PLAT_MUTEX_INIT;

static DlssObj *obj_of(uint64_t p) {
    for (int i = 0; i < nobjs; i++)
        if (objs[i].obj == p) return &objs[i];
    if (nobjs == MAX_OBJ) return &objs[MAX_OBJ - 1];
    DlssObj *o = &objs[nobjs++];
    memset(o, 0, sizeof *o);
    o->obj = p;
    return o;
}

static int log_left(void) {
    static int left = -1;
    if (left < 0) {
        const char *e = getenv("KAR_DLSS_LOG");
        left = e ? atoi(e) : 400;
    }
    return left > 0 ? left-- : 0;
}

static void note(Cpu *c, const char *name, uint64_t ret) {
    if (!log_left()) return;
    rt_log("dlss: %s(%#llx, %#llx, %#llx, %#llx, %#llx) s0 %g s1 %g -> %#llx", name, (unsigned long long)c->x[0],
           (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
           (unsigned long long)c->x[4], arg_s(c, 0), arg_s(c, 1), (unsigned long long)ret);
}

// A setter: keeps x1..x4 and s0..s1 of the call under key k of the object in x0.
static void set(Cpu *c, const char *name, int k) {
    plat_mutex_lock(&lock);
    DlssObj *o = obj_of(c->x[0]);
    o->set[k] = 1;
    for (int i = 0; i < 4; i++) o->v[k][i] = c->x[i + 1];
    o->f[k][0] = arg_s(c, 0), o->f[k][1] = arg_s(c, 1);
    plat_mutex_unlock(&lock);
    note(c, name, 0);
}

static int plausible_ptr(uint64_t p) { return p >= 0x1000000ull && p < (1ull << 48) && !(p & 3); }

// A getter of key k: returns the first kept argument, and writes the kept arguments through pointer arguments
// (out parameters) when the call passes some: (obj, &a, &b) gets a and b; float pairs come back in s0/s1 too.
static void get(Cpu *c, const char *name, int k) {
    plat_mutex_lock(&lock);
    DlssObj *o = obj_of(c->x[0]);
    uint64_t r = o->set[k] ? o->v[k][0] : 0;
    int outs = 0;
    for (int i = 1; i <= 2 && plausible_ptr(c->x[i]); i++) outs++;
    if (o->set[k] && outs) {
        for (int i = 0; i < outs; i++) {
            uint32_t w = (uint32_t)o->v[k][i];
            if (!w && o->f[k][i] != 0.0f) memcpy(&w, &o->f[k][i], 4);
            memcpy((void *)(uintptr_t)c->x[1 + i], &w, 4);
        }
    }
    float f0 = o->set[k] ? o->f[k][0] : 0.0f, f1 = o->set[k] ? o->f[k][1] : 0.0f;
    plat_mutex_unlock(&lock);
    note(c, name, r);
    ret_x(c, r);
    uint64_t v0 = 0, v1 = 0;
    memcpy(&v0, &f0, 4), memcpy(&v1, &f1, 4);
    c->v[0] = v0, c->v[1] = v1;
}

// Keys: one per setter/getter pair.
enum {
    K_DEVICE, K_FLAGS, K_OUTRES, K_METHOD, K_PRESET, K_STORAGE, K_TEXIDS, K_MAXEVAL,
    K_DEBUG, K_FULLSCREEN, K_NANS, K_JITTERDBG, K_IN, K_OUT, K_MV, K_DEPTH, K_BIASMASK, K_JITTER, K_PREEXP, K_EXPTEX,
    K_RESET, K_MVSCALE, K_EVALIDX, K_INSIZE, K_INBASE, K_DEPTHBASE, K_MVBASE, K_BIASBASE, K_OUTBASE, K_RESIZE,
};

#define SETTER(n, k) NVN_API(n) { set(c, #n, k); }
#define GETTER(n, k) NVN_API(n) { get(c, #n, k); }

// ---- Network builder
NVN_API(nvnDlssNetworkBuilderSetDefaults) {
    plat_mutex_lock(&lock);
    DlssObj *o = obj_of(c->x[0]);
    memset(o->set, 0, sizeof o->set);
    plat_mutex_unlock(&lock);
    note(c, "nvnDlssNetworkBuilderSetDefaults", 0);
}
SETTER(nvnDlssNetworkBuilderSetDevice, K_DEVICE)
GETTER(nvnDlssNetworkBuilderGetDevice, K_DEVICE)
SETTER(nvnDlssNetworkBuilderSetFlags, K_FLAGS)
GETTER(nvnDlssNetworkBuilderGetFlags, K_FLAGS)
SETTER(nvnDlssNetworkBuilderSetOutputResolution, K_OUTRES)
GETTER(nvnDlssNetworkBuilderGetOutputResolution, K_OUTRES)
SETTER(nvnDlssNetworkBuilderSetMethod, K_METHOD)
GETTER(nvnDlssNetworkBuilderGetMethod, K_METHOD)
SETTER(nvnDlssNetworkBuilderSetPreset, K_PRESET)
GETTER(nvnDlssNetworkBuilderGetPreset, K_PRESET)
SETTER(nvnDlssNetworkBuilderSetStorage, K_STORAGE)
GETTER(nvnDlssNetworkBuilderGetMemory, K_STORAGE)
SETTER(nvnDlssNetworkBuilderSetTextureIDs, K_TEXIDS)
GETTER(nvnDlssNetworkBuilderGetTexturePool, K_TEXIDS)
SETTER(nvnDlssNetworkBuilderSetMaxEvaluationEntries, K_MAXEVAL)
GETTER(nvnDlssNetworkBuilderGetMaxEvaluationEntries, K_MAXEVAL)
// size_t nvnDlssNetworkBuilderGetTextureIDs(...): the second kept argument of SetTextureIDs
NVN_API(nvnDlssNetworkBuilderGetTextureIDs) {
    plat_mutex_lock(&lock);
    DlssObj *o = obj_of(c->x[0]);
    uint64_t r = o->set[K_TEXIDS] ? o->v[K_TEXIDS][1] : 0;
    plat_mutex_unlock(&lock);
    note(c, "nvnDlssNetworkBuilderGetTextureIDs", r);
    ret_x(c, r);
}
// The memory the network wants: the host evaluates without any, but a 0 size might fail the game's pool creation, so
// one 64 KiB page each (KAR_DLSS_STORAGE overrides).
static uint64_t storage_size(void) {
    const char *e = getenv("KAR_DLSS_STORAGE");
    return e ? strtoull(e, NULL, 0) : 0x10000;
}
NVN_API(nvnDlssNetworkBuilderGetStorageSize) { note(c, "nvnDlssNetworkBuilderGetStorageSize", storage_size()); ret_x(c, storage_size()); }
NVN_API(nvnDlssNetworkBuilderGetMemorySize) { note(c, "nvnDlssNetworkBuilderGetMemorySize", storage_size()); ret_x(c, storage_size()); }
// Optimal settings (input resolution for a quality mode): unknown shape; logged, nothing written.
NVN_API(nvnDlssNetworkBuilderGetOptimalSettings) { note(c, "nvnDlssNetworkBuilderGetOptimalSettings", 0); ret_x(c, 0); }

// ---- Network: always initializes; resize keeps its arguments.
NVN_API(nvnDlssNetworkInitialize) { note(c, "nvnDlssNetworkInitialize", 1); ret_x(c, 1); }
NVN_API(nvnDlssNetworkFinalize) { note(c, "nvnDlssNetworkFinalize", 0); }
NVN_API(nvnDlssNetworkResize) { set(c, "nvnDlssNetworkResize", K_RESIZE); ret_x(c, 1); }

// ---- Parameters
NVN_API(nvnDlssParametersSetDefaults) {
    plat_mutex_lock(&lock);
    DlssObj *o = obj_of(c->x[0]);
    memset(o->set, 0, sizeof o->set);
    plat_mutex_unlock(&lock);
    note(c, "nvnDlssParametersSetDefaults", 0);
}
SETTER(nvnDlssParametersSetOverlayDebugLayer, K_DEBUG)
SETTER(nvnDlssParametersSetOverlayFullScreen, K_FULLSCREEN)
SETTER(nvnDlssParametersSetOverlayShowNans, K_NANS)
SETTER(nvnDlssParametersSetOverlayJitterDebug, K_JITTERDBG)
SETTER(nvnDlssParametersSetInputColor, K_IN)
SETTER(nvnDlssParametersSetOutputColor, K_OUT)
SETTER(nvnDlssParametersSetMotionVectors, K_MV)
SETTER(nvnDlssParametersSetDepth, K_DEPTH)
SETTER(nvnDlssParametersSetBiasCurrentColorMask, K_BIASMASK)
SETTER(nvnDlssParametersSetJitterOffset, K_JITTER)
GETTER(nvnDlssParametersGetJitterOffset, K_JITTER)
SETTER(nvnDlssParametersSetPreExposure, K_PREEXP)
GETTER(nvnDlssParametersGetPreExposure, K_PREEXP)
SETTER(nvnDlssParametersSetExposureTexture, K_EXPTEX)
SETTER(nvnDlssParametersSetHistoryReset, K_RESET)
GETTER(nvnDlssParametersGetHistoryReset, K_RESET)
SETTER(nvnDlssParametersSetMotionVectorScale, K_MVSCALE)
GETTER(nvnDlssParametersGetMotionVectorScale, K_MVSCALE)
SETTER(nvnDlssParametersSetEvaluationIndex, K_EVALIDX)
GETTER(nvnDlssParametersGetEvaluationIndex, K_EVALIDX)
SETTER(nvnDlssParametersSetInputSubrectangleSize, K_INSIZE)
GETTER(nvnDlssParametersGetInputSubrectangleSize, K_INSIZE)
SETTER(nvnDlssParametersSetInputColorSubrectangleBase, K_INBASE)
GETTER(nvnDlssParametersGetInputColorSubrectangleBase, K_INBASE)
SETTER(nvnDlssParametersSetDepthSubrectangleBase, K_DEPTHBASE)
GETTER(nvnDlssParametersGetDepthSubrectangleBase, K_DEPTHBASE)
SETTER(nvnDlssParametersSetMotionVectorSubrectangleBase, K_MVBASE)
GETTER(nvnDlssParametersGetMotionVectorSubrectangleBase, K_MVBASE)
SETTER(nvnDlssParametersSetBiasCurrentColorSubrectangleBase, K_BIASBASE)
GETTER(nvnDlssParametersGetBiasCurrentColorSubrectangleBase, K_BIASBASE)
SETTER(nvnDlssParametersSetOutputSubrectangleBase, K_OUTBASE)
GETTER(nvnDlssParametersGetOutputSubrectangleBase, K_OUTBASE)

// KAR diagnostic: the Aurora engine binds "Runtime.Graphics.Aurora.Device.EnableDLSS" to a byte in main's .bss
// (main+0x9381810 in 1.3.3, read at main+0x4c25084 where it turns the engine's DLSS state on). With KAR_DLSS_WATCH set,
// logged every 300 frames from vk_backend.c with the byte after it (EnableDLSSDebug).
void kar_dlss_watch(uint64_t frame) {
    if (strncmp(rt_game_target, "kar458752", 9) || !ldr_count()) return;
    const RtLoaded *m = ldr_module(0);
    if (!m || m->size < 0x9381812) return;
    const uint8_t *p = (const uint8_t *)(uintptr_t)(m->base + 0x9381810);
    rt_log("dlss: frame %llu EnableDLSS %u EnableDLSSDebug %u", (unsigned long long)frame, p[0], p[1]);
}

// KAR_FORCE_DLSS=1: sets that flag while the game loads its NVN procs (nvn_procs.c calls this per lookup), before the
// renderer setup reads it, so the engine takes its DLSS path through the stand-in above.
void kar_dlss_force(void) {
    static int on = -1;
    if (on < 0) on = getenv("KAR_FORCE_DLSS") && *getenv("KAR_FORCE_DLSS") == '1' && !strncmp(rt_game_target, "kar458752", 9);
    if (!on || !ldr_count()) return;
    const RtLoaded *m = ldr_module(0);
    if (m && m->size >= 0x9381812) *(volatile uint8_t *)(uintptr_t)(m->base + 0x9381810) = 1;
}

// ---- Evaluation: logged with the parameters object's kept input and output (the replay op follows once the shapes
// are known).
NVN_API(nvnCommandBufferEvaluateDlss) {
    note(c, "nvnCommandBufferEvaluateDlss", 0);
    static int told;
    if (told++ < 4) {
        plat_mutex_lock(&lock);
        for (int r = 1; r <= 3; r++) {
            for (int i = 0; i < nobjs; i++) {
                if (objs[i].obj != c->x[r]) continue;
                const DlssObj *o = &objs[i];
                for (int k = 0; k < MAX_KEY; k++)
                    if (o->set[k])
                        rt_log("dlss:   arg x%d obj %#llx key %d: %#llx %#llx %#llx %#llx f %g %g", r,
                               (unsigned long long)o->obj, k, (unsigned long long)o->v[k][0],
                               (unsigned long long)o->v[k][1], (unsigned long long)o->v[k][2],
                               (unsigned long long)o->v[k][3], o->f[k][0], o->f[k][1]);
            }
        }
        plat_mutex_unlock(&lock);
    }
}
