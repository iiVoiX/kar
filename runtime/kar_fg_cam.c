// Kirby Air Riders camera data for frame generation (RELEASE_PLAN.md 16.7, way B2). f_7005384740 fills one camera's
// constant block (x1, 0x210 bytes: 3x3 and 4x4 matrices from +0x60, render size +0x1e0, near/far +0x1e8, 1/size
// +0x1f0) from the camera object (x0) for the GPU. The patch set karfg (recomp/patches-kar458752.txt, on with
// KAR_FG=1) calls cam_hook on its last store before the return, with x0 and x1 unchanged.
//
// KAR_FG_CAMLOG=<frame> (diagnostic): from that frame, two frames of calls: camera object, field of view, aspect,
// render size, near/far and the six 4x4 matrices (rows as the block holds them), to tell which camera draws the scene
// and which matrix is which.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "dispatch.h"
#include "cpu.h"
#include "gfx.h"
#include "nvn_api.h"
#include "platform/platform.h"

#define SLOT_CAMERA 0x3c0

// Scene camera candidates: perspective (projection row 3 = 0 0 -1 0), field of view in radians 0.5..2.1, a 16:9 render
// size of at least 1280 wide other than 1920x1080 (the interface's 3D cameras). The race also has a narrow second
// camera of the scene's size (field of view ~0.34), so each frame keeps the candidate with the widest field of view.
// Frames are the present counter at the time of the call: the replay of a frame runs before its present, so the
// motion vectors of frame n use the cameras kept under n (current) and n - 1 (previous).
typedef struct { float vp[16], ivp[16], dz[2], fov, nearv, farv; uint32_t w, h; uint64_t frame; int ok; } SceneCam;
#define HIST 4
static PlatMutex lock = PLAT_MUTEX_INIT;
static SceneCam hist[HIST];   // by frame % HIST

static void keep_scene_camera(Cpu *c, uint64_t fr) {
    const float *cam = (const float *)(uintptr_t)c->x[0];
    const float *b = (const float *)(uintptr_t)c->x[1];
    float fov = cam[0x31c / 4], w = b[0x1e0 / 4], h = b[0x1e4 / 4];
    const float *proj = b + 0x60 / 4;
    if (proj[12] != 0 || proj[13] != 0 || proj[14] != -1 || proj[15] != 0) return;
    if (!(fov > 0.5f && fov < 2.1f) || w < 1280 || (w == 1920 && h == 1080) || w * 9 < h * 16 * 0.98f ||
        w * 9 > h * 16 * 1.02f)
        return;
    plat_mutex_lock(&lock);
    SceneCam *e = &hist[fr % HIST];
    if (!e->ok || e->frame != fr || fov >= e->fov) {
        memcpy(e->vp, b + 0xe0 / 4, sizeof e->vp);
        memcpy(e->ivp, b + 0x120 / 4, sizeof e->ivp);
        e->dz[0] = proj[10], e->dz[1] = proj[11];   // projection row 2: ndc z = (A z + B) / -z
        e->fov = fov, e->nearv = b[0x1e8 / 4], e->farv = b[0x1ec / 4], e->w = (uint32_t)w, e->h = (uint32_t)h;
        e->frame = fr, e->ok = 1;
    }
    plat_mutex_unlock(&lock);
}

// reproj = previous view-projection x inverse current view-projection (rows), for the scene camera of size w x h of
// this frame and the one before; 0 when either is missing.
int kar_fg_camera(uint32_t w, uint32_t h, float reproj[16], float dz[2], float *fov, float *nearv, float *farv) {
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    plat_mutex_lock(&lock);
    // the newest kept frame (the game's logic may already be one frame ahead of the replay) and the one before it
    const SceneCam *cur = NULL, *prev = NULL;
    for (int i = 0; i < HIST; i++)
        if (hist[i].ok && hist[i].frame <= fr + 1 && (!cur || hist[i].frame > cur->frame)) cur = &hist[i];
    if (cur) prev = &hist[(cur->frame + HIST - 1) % HIST];
    int ok = cur && prev->ok && prev->frame + 1 == cur->frame && cur->frame + 2 >= fr && cur->w == w && cur->h == h &&
             prev->w == w && prev->h == h;
    if (ok) {
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) {
                float sum = 0;
                for (int j = 0; j < 4; j++) sum += prev->vp[r * 4 + j] * cur->ivp[j * 4 + k];
                reproj[r * 4 + k] = sum;
            }
        *fov = cur->fov, *nearv = cur->nearv, *farv = cur->farv, dz[0] = cur->dz[0], dz[1] = cur->dz[1];
    }
    {   // KAR_FG_MVLOG=1: the frames kept, for the first failures
        static int on = -1, n;
        if (on < 0) on = getenv("KAR_FG_MVLOG") && *getenv("KAR_FG_MVLOG") == '1';
        if (on && !ok && n < 20 && cur) {
            n++;
            rt_log("fg: no camera pair at frame %llu: newest %llu (%ux%u), before it %llu ok %d", (unsigned long long)fr,
                   (unsigned long long)cur->frame, cur->w, cur->h, (unsigned long long)prev->frame, prev->ok);
        }
    }
    plat_mutex_unlock(&lock);
    static int told;
    if (ok && !told++) rt_log("fgcam: scene camera %ux%u, fov %g, near %g far %g", w, h, *fov, *nearv, *farv);
    return ok;
}

static void cam_hook(Cpu *c) {
    keep_scene_camera(c, __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
    static long long from = -2;
    if (from == -2) from = getenv("KAR_FG_CAMLOG") ? atoll(getenv("KAR_FG_CAMLOG")) : -1;
    if (from < 0) return;
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (fr < (uint64_t)from || fr > (uint64_t)from + 1) return;
    const float *cam = (const float *)(uintptr_t)c->x[0];
    const float *b = (const float *)(uintptr_t)c->x[1];
    rt_log("fgcam: f%llu cam %#llx aspect %g fov %g size %gx%g nearfar %g %g", (unsigned long long)fr,
           (unsigned long long)c->x[0], cam[0x318 / 4], cam[0x31c / 4], b[0x1e0 / 4], b[0x1e4 / 4], b[0x1e8 / 4],
           b[0x1ec / 4]);
    for (int m = 0; m < 6; m++) {
        const float *r = b + (0x60 + m * 0x40) / 4;
        rt_log("fgcam:   m%d @%#x [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]", m, 0x60 + m * 0x40, r[0],
               r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    }
}

void kar_fg_cam_init(void) {
    if (rt_patch_set("karfg") < 0) return;
    rt_register_host((uint64_t)(uintptr_t)rt_patch_page() + SLOT_CAMERA, cam_hook);
}
