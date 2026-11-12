// Kirby Air Riders ultrawide (RELEASE_PLAN.md 17.3, experimental). The patch set karwide (recomp/patches-kar458752.txt)
// calls size_hook in f_7000f342c0 right after it fetched its display-info object (x0: a base size 1280x720 at +0x10,
// the output size 2560x1440 at +0x18), before it creates the window textures. With KAR_DISPLAY_RES=WxH the host
// gives its output size (+0x18) the wanted width; the other sizes change in class_hook and window_hook.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "host.h"
#include "dispatch.h"
#include "cpu.h"

#define SLOT_SIZE 0x340
#define SLOT_CLASS 0x350
#define SLOT_WINDOW 0x360

static int want_w, want_h;

static void size_hook(Cpu *c) {
    uint32_t *s = (uint32_t *)(uintptr_t)c->x[0];
    if (!s) return;
    rt_log("kar wide: display info %#llx: %ux%u (w8 %u); words %u %u %u %u | %u %u %u %u %u %u %u %u %u %u",
           (unsigned long long)c->x[0], s[4], s[5], (unsigned)c->x[8], s[0], s[1], s[2], s[3], s[6], s[7], s[8], s[9],
           s[10], s[11], s[12], s[13], s[14], s[15]);
    // +0x18: the output size the final composite covers (2560x1440); +0x10: the base size (1280x720) the game fits
    // into it keeping its aspect (16:9 content centred). Both get the wanted width; w8 holds the base width already.
    if (want_w && want_h && s[7] && s[6] * 9 == s[7] * 16) s[6] = (uint32_t)(((uint64_t)s[7] * want_w / want_h + 8) & ~15u);
    if (want_w && want_h && s[5] && s[4] * 9 == s[5] * 16) {
        s[4] = (uint32_t)(((uint64_t)s[5] * want_w / want_h + 4) & ~7u);
        c->x[8] = s[4];
    }
}

// f_7005b74840 before its epilogue: x0 = width | height << 32 of a render-graph size class. A 16:9 size gets the
// width of the wanted aspect (a multiple of 16), so the scene, the composite and the window targets grow sideways.
static void class_hook(Cpu *c) {
    if (!want_w || !want_h) return;
    uint32_t w = (uint32_t)c->x[0], h = (uint32_t)(c->x[0] >> 32);
    if (!h || w * 9 < h * 16 - 9 || w * 9 > h * 16 + 9) return;   // only 16:9 sizes
    uint32_t nw = (uint32_t)(((uint64_t)h * want_w / want_h + 8) & ~15u);
    if (nw <= w) return;
    static int logged;
    if (logged < 30) { logged++; rt_log("kar wide: render size %ux%u -> %ux%u", w, h, nw, h); }
    c->x[0] = (uint64_t)nw | (uint64_t)h << 32;
}

// f_7004c3e180 after its prologue: x0 is the object whose +0x3d8/+0x3dc hold the size the window textures are created
// with (2560x1440). A 16:9 size gets the wanted width.
static void window_hook(Cpu *c) {
    uint32_t *g = (uint32_t *)(uintptr_t)(c->x[0] + 0x3d8);
    static int logged;
    if (logged < 8) { logged++; rt_log("kar wide: window textures of %#llx: %ux%u", (unsigned long long)c->x[0], g[0], g[1]); }
    if (!want_w || !want_h || !g[1] || g[0] * 9 != g[1] * 16) return;
    g[0] = (uint32_t)(((uint64_t)g[1] * want_w / want_h + 8) & ~15u);
}

void kar_wide_init(void) {
    if (rt_patch_set("karwide") < 0) return;
    const char *e = getenv("KAR_DISPLAY_RES");
    if (!e || sscanf(e, "%dx%d", &want_w, &want_h) != 2) want_w = want_h = 0;
    rt_register_host((uint64_t)(uintptr_t)rt_patch_page() + SLOT_SIZE, size_hook);
    rt_register_host((uint64_t)(uintptr_t)rt_patch_page() + SLOT_CLASS, class_hook);
    rt_register_host((uint64_t)(uintptr_t)rt_patch_page() + SLOT_WINDOW, window_hook);
}
