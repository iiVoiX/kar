// Kirby Air Riders: PC settings inside the game's own options menu (RELEASE_PLAN.md 15.x, way B). The patch set
// karmenu (recomp/patches-kar458752.txt) sends both message lookups of the text fetch f_70011302c0 to text_lookup,
// which calls the original lookup f_7000001300 (x0 message set, x1 NUL-terminated label; returns a UTF-16 text or 0)
// and can answer a label with its own text, kept in the patch page from +0x400 (guest addresses equal host ones).
// Two more sites record where the display page puts its texts and give a per-frame tick that switches the page's
// PC mode with L/R and sets those texts again (RELEASE_PLAN.md 15.1, plan (2)).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "dispatch.h"
#include "cpu.h"
#include "nvn_api.h"
#include "input.h"

#define LOOKUP 0x7000001300ull    // f_7000001300(message set, label) -> UTF-16 text or 0
#define SET_TEXT 0x7001127350ull  // f_7001127350(text field, label, length, 0): a UI text from a message label
#define SLOT_LOOKUP 0x300
#define SLOT_RECORD 0x310
#define SLOT_TICK 0x320
#define SLOT_RECORD_STATIC 0x330
#define SET_STATIC 0x70011333c0ull  // f_70011333c0(label string, text field): a prefab text (fixed label)
#define TEXTS 0x400

// PC mode of the display page (Anzeige): the same rows show the port's settings. Labels answered with our own text
// while pc_mode is on. The tab "Allgemein" becomes "PC-Grafik".
static struct { const char *label; const char *text; uint64_t at; } pc_texts[] = {
    {"Mnu_Opt_Disp_OptMode_Cmn", "PC-Grafik", 0},
    {"Mnu_Opt_Disp_TextSize", "Aufl\xc3\xb6sung", 0},
    {"Mnu_Opt_Disp_NoviceGuide", "Bildrate", 0},
    {"Mnu_Opt_Disp_Checker", "Hochskalierung", 0},
    {"Mnu_Opt_Disp_Sep", "V-Sync", 0},
    {"Mnu_Opt_Disp_ColorFilter", "Fenstermodus", 0},
};
#define NPC (sizeof pc_texts / sizeof *pc_texts)
static volatile int pc_mode;
static int log_left;
static const char *log_match;   // KAR_MENU_LOG=<label substring>: every matching lookup with its frame (diagnostic)

// Text fields of the display page, recorded when the page sets them (one page build at a time).
#define NREC 32
static struct { uint64_t dst, label; int fixed; uint8_t snap[32]; int snapped; } rec[NREC];   // fixed: label is a string object
static int nrec, resetting;
static uint64_t rec_frame;

static uint64_t frame_now(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED); }

// UTF-8 to NUL-terminated UTF-16 at dst (BMP only); returns the bytes written.
static size_t put_utf16(uint8_t *dst, const char *s) {
    uint16_t *o = (uint16_t *)dst;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned cp = *p++;
        if (cp >= 0xe0 && p[0] && p[1]) { cp = (cp & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
        else if (cp >= 0xc0 && p[0]) { cp = (cp & 31) << 6 | (p[0] & 63); p++; }
        *o++ = (uint16_t)cp;
    }
    *o++ = 0;
    return (size_t)((uint8_t *)o - dst);
}

// KAR_MENU_LOG: the first characters of a UTF-16 text (ASCII as is, others as ?).
static const char *kar_menu_txt(uint64_t at) {
    static char t[48];
    int k = 0;
    for (; at && k < 40; k++) {
        uint16_t ch = ((const uint16_t *)(uintptr_t)at)[k];
        if (!ch) break;
        t[k] = ch < 0x20 ? '^' : ch < 0x7f ? (char)ch : '?';
    }
    t[k] = 0;
    return t;
}

static void text_lookup(Cpu *c) {
    const char *label = (const char *)(uintptr_t)c->x[1];
    int hit = 0;
    if (pc_mode && label)
        for (size_t i = 0; i < NPC; i++)
            if (pc_texts[i].at && !strcmp(label, pc_texts[i].label)) {
                c->x[0] = pc_texts[i].at;
                hit = 1;
                break;
            }
    if (!hit) {
        c->pc = LOOKUP;
        rt_dispatch(c);
    }
    if (log_match && log_left > 0 && label && strstr(label, log_match)) {
        log_left--;
        uint64_t fp = c->x[29], chain[5] = {0};
        for (int k = 0; k < 5 && fp && !(fp & 7); k++) {   // frame records: [fp] caller's fp, [fp+8] return address
            chain[k] = *(const uint64_t *)(uintptr_t)(fp + 8);
            fp = *(const uint64_t *)(uintptr_t)fp;
        }
        fprintf(stderr, "kar_menu: f%llu %s -> %#llx \"%s\"%s < %#llx < %#llx < %#llx < %#llx < %#llx\n",
                (unsigned long long)frame_now(), label, (unsigned long long)c->x[0], kar_menu_txt(c->x[0]), hit ? " (own text)" : "",
                (unsigned long long)chain[0], (unsigned long long)chain[1], (unsigned long long)chain[2],
                (unsigned long long)chain[3], (unsigned long long)chain[4]);
    }
}

static int managed(const char *label, size_t n) {
    for (size_t i = 0; i < NPC; i++)
        if (strlen(pc_texts[i].label) == n && !memcmp(label, pc_texts[i].label, n)) return 1;
    return 0;
}

static void remember(uint64_t dst, uint64_t label, int fixed, const char *name, size_t n) {
    uint64_t now = frame_now();
    if (now - rec_frame > 30) { nrec = 0; pc_mode = 0; }   // a new build of the page starts in its own mode
    rec_frame = now;
    for (int i = 0; i < nrec; i++)
        if (rec[i].dst == dst) { rec[i].label = label; rec[i].fixed = fixed; rec[i].snapped = 0; return; }
    if (nrec < NREC) rec[nrec++] = (typeof(rec[0])){.dst = dst, .label = label, .fixed = fixed};
    if (log_match) fprintf(stderr, "kar_menu: f%llu record %.*s at %#llx%s\n", (unsigned long long)now, (int)n, name,
                           (unsigned long long)dst, fixed ? " (prefab)" : "");
}

// f_7001127350 + 0xc (prologue done, arguments untouched): x0 text field, x1 NUL-terminated label.
static void text_record(Cpu *c) {
    const char *label = (const char *)(uintptr_t)c->x[1];
    if (resetting || !label) return;
    size_t n = strlen(label);
    if (managed(label, n)) remember(c->x[0], c->x[1], 0, label, n);
}

// f_70011333c0 + 0xc: x0 the label as a string object (byte 0x17 < 0: pointer at +0, length at +8; else inline,
// length in byte 0x17), x1 the text field. Prefab texts: the row names, the tab, "Zurücksetzen".
static void static_record(Cpu *c) {
    if (resetting || !c->x[0]) return;
    const uint8_t *s = (const uint8_t *)(uintptr_t)c->x[0];
    const char *p = (const char *)s;
    size_t n = 23 - (size_t)s[0x17];   // inline: byte 0x17 holds the free room of the 23 inline bytes
    if ((int8_t)s[0x17] < 0) { p = (const char *)(uintptr_t)*(const uint64_t *)s; n = *(const uint64_t *)(s + 8); }
    if (log_match && (int8_t)s[0x17] >= 0)
        fprintf(stderr, "kar_menu: short string %02x %02x | %.24s\n", s[0], s[0x17], (const char *)s);
    if (p && n < 64 && managed(p, n)) remember(c->x[1], c->x[0], 1, p, n);
}

static int still_there(int i) { return !memcmp(rec[i].snap, (const void *)(uintptr_t)rec[i].dst, sizeof rec[i].snap); }

// 0x7002fe7230 + 0x10, every frame on the game thread: L/R on the display page switch the mode and set its texts again.
void kar_wide_tick(void);
static void menu_tick(Cpu *c) {
    kar_wide_tick();
    if (!nrec) return;
    for (int i = 0; i < nrec; i++)   // the frame after a set: what the field holds now identifies it later
        if (!rec[i].snapped) { memcpy(rec[i].snap, (const void *)(uintptr_t)rec[i].dst, sizeof rec[i].snap); rec[i].snapped = 1; }
    static uint64_t prev;
    uint64_t b = input_game_buttons();
    uint64_t down = b & ~prev;
    prev = b;
    if (!(down & (1u << 6 | 1u << 7))) return;   // L or R
    for (int i = 0; i < nrec; i++)
        if (!still_there(i)) {   // the page is gone (or rebuilt elsewhere): forget it
            if (log_match) {
                const uint8_t *d = (const uint8_t *)(uintptr_t)rec[i].dst;
                fprintf(stderr, "kar_menu: page gone at field %d, %d fields dropped:", i, nrec);
                for (int k = 0; k < 32; k++) fprintf(stderr, " %02x/%02x", rec[i].snap[k], d[k]);
                fprintf(stderr, "\n");
            }
            nrec = 0;
            return;
        }
    Cpu save;
    memcpy(save.x, c->x, sizeof save.x);
    memcpy(save.v, c->v, sizeof save.v);
    save.nzcv = c->nzcv;
    pc_mode = !pc_mode;
    resetting = 1;
    for (int i = 0; i < nrec; i++) {
        if (rec[i].fixed) rt_call(c, SET_STATIC, 2, rec[i].label, rec[i].dst);
        else rt_call(c, SET_TEXT, 4, rec[i].dst, rec[i].label, (uint64_t)strlen((const char *)(uintptr_t)rec[i].label), (uint64_t)0);
        memcpy(rec[i].snap, (const void *)(uintptr_t)rec[i].dst, sizeof rec[i].snap);
    }
    resetting = 0;
    memcpy(c->x, save.x, sizeof save.x);
    memcpy(c->v, save.v, sizeof save.v);
    c->nzcv = save.nzcv;
    if (log_match) fprintf(stderr, "kar_menu: f%llu pc mode %d, %d texts set\n", (unsigned long long)frame_now(), pc_mode, nrec);
}

void kar_menu_init(void) {
    if (rt_patch_set("karmenu") < 0) return;   // another game or generated code predating the patch
    uint8_t *page = rt_patch_page();
    size_t off = TEXTS;
    for (size_t i = 0; i < NPC; i++) {
        pc_texts[i].at = (uint64_t)(uintptr_t)(page + off);
        off += put_utf16(page + off, pc_texts[i].text);
        off = (off + 7) & ~(size_t)7;
    }
    log_match = getenv("KAR_MENU_LOG");
    log_left = 3000;
    uint64_t base = (uint64_t)(uintptr_t)page;
    rt_register_host(base + SLOT_LOOKUP, text_lookup);
    rt_register_host(base + SLOT_RECORD, text_record);
    rt_register_host(base + SLOT_TICK, menu_tick);
    rt_register_host(base + SLOT_RECORD_STATIC, static_record);
}
