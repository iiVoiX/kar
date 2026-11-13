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

// The scene camera: perspective (projection row 3 = 0 0 -1 0), field of view in radians 0.3..2.1, a 16:9 render size
// of at least 1280 wide other than 1920x1080 (the interface's 3D cameras). Its view-projection (+0xe0) and inverse
// (+0x120) are kept per frame; the previous frame's view-projection is kept when a new frame's arrives.
static PlatMutex lock = PLAT_MUTEX_INIT;
static struct { float vp[16], ivp[16], fov, nearv, farv; uint32_t w, h; uint64_t frame; int ok; } cur, prev;

static void keep_scene_camera(Cpu *c, uint64_t fr) {
    const float *cam = (const float *)(uintptr_t)c->x[0];
    const float *b = (const float *)(uintptr_t)c->x[1];
    float fov = cam[0x31c / 4], w = b[0x1e0 / 4], h = b[0x1e4 / 4];
    const float *proj = b + 0x60 / 4;
    if (proj[12] != 0 || proj[13] != 0 || proj[14] != -1 || proj[15] != 0) return;
    if (!(fov > 0.3f && fov < 2.1f) || w < 1280 || (w == 1920 && h == 1080) || w * 9 < h * 16 * 0.98f ||
        w * 9 > h * 16 * 1.02f)
        return;
    plat_mutex_lock(&lock);
    if (cur.ok && cur.frame != fr) prev = cur;
    memcpy(cur.vp, b + 0xe0 / 4, sizeof cur.vp);
    memcpy(cur.ivp, b + 0x120 / 4, sizeof cur.ivp);
    cur.fov = fov, cur.nearv = b[0x1e8 / 4], cur.farv = b[0x1ec / 4], cur.w = (uint32_t)w, cur.h = (uint32_t)h;
    cur.frame = fr, cur.ok = 1;
    plat_mutex_unlock(&lock);
    static int told;
    if (!told++) rt_log("fgcam: scene camera %#llx, %ux%u, fov %g, near %g far %g", (unsigned long long)c->x[0], cur.w,
                        cur.h, fov, cur.nearv, cur.farv);
}

// reproj = previous view-projection x inverse current view-projection (rows), for the camera of size w x h; 0 when
// there is no previous frame's camera of that size.
int kar_fg_camera(uint32_t w, uint32_t h, float reproj[16], float *fov, float *nearv, float *farv) {
    plat_mutex_lock(&lock);
    int ok = cur.ok && prev.ok && cur.w == w && cur.h == h && prev.w == w && prev.h == h;
    if (ok) {
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) {
                float s = 0;
                for (int j = 0; j < 4; j++) s += prev.vp[r * 4 + j] * cur.ivp[j * 4 + k];
                reproj[r * 4 + k] = s;
            }
        *fov = cur.fov, *nearv = cur.nearv, *farv = cur.farv;
    }
    plat_mutex_unlock(&lock);
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
