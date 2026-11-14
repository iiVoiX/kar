// Kirby Air Riders: the game's own 60 fps limiter off (RELEASE_PLAN 16.6a). f_7000fb22fc, run by the job threads each
// frame, sleeps until 1/target fps has passed since the last frame when one of two flags of its pacing object is set:
// the replay after a race and the menus, never the race itself (60 there). The patch set karpace (KAR_PACE, on) calls
// pace_hook right after d2 = the time left to wait: zero skips the sleep, the function still stores the measured frame
// delta as valid, and the runtime's frame limit paces instead.
#include "host.h"

#define SLOT_PACE 0x3e0

static void pace_hook(Cpu *c) { c->v[2] = 0; }

void kar_pace_init(void) {
    if (rt_patch_set("karpace") < 0) return;
    rt_register_host((uint64_t)(uintptr_t)rt_patch_page() + SLOT_PACE, pace_hook);
}
