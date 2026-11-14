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
#include "cfg.h"

#define LOOKUP 0x7000001300ull    // f_7000001300(message set, label) -> UTF-16 text or 0
#define SET_TEXT 0x7001127350ull  // f_7001127350(text field, label, length, 0): a UI text from a message label
#define SLOT_LOOKUP 0x300
#define SLOT_RECORD 0x310
#define SLOT_TICK 0x320
#define SLOT_RECORD_STATIC 0x330
#define SLOT_VALUE 0x380   // f_700300e100 (display page: a row's value changed), first instruction
#define SLOT_PAGE 0x390    // the display page build loop (x19 = the page)
#define SET_STATIC 0x70011333c0ull  // f_70011333c0(label string, text field): a prefab text (fixed label)
#define TEXTS 0x400

// PC mode of the display page (Anzeige): the same rows show the port's settings. Labels answered with our own text
// while pc_mode is on. The tab "Allgemein" becomes "PC-Grafik".
static struct { const char *label; const char *text; uint64_t at; } pc_texts[] = {
    {"Mnu_Opt_Disp_OptMode_Cmn", "PC-Grafik", 0},
    {"Mnu_Opt_Disp_TextSize", "Aufl\xc3\xb6sung", 0},
    {"Mnu_Opt_Disp_NoviceGuide", "HUD-Position", 0},
    {"Mnu_Opt_Disp_NoviceGuide_On", "Rand", 0},
    {"Mnu_Opt_Disp_NoviceGuide_Off", "Mitte", 0},
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
static uint64_t page_obj;   // the display page (page_hook)
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
    static uint64_t rec_page;   // a new build of the page starts in its own mode; a value text set on the same page does not
    if (now - rec_frame > 30 && (page_obj != rec_page || !page_obj)) { nrec = 0; pc_mode = 0; }
    rec_page = page_obj;
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

// PC rows (RELEASE_PLAN 15.4): a row of the display page that shows a port setting while the page is in PC mode. The
// page object (recorded by page_hook) keeps its rows at +0xe48 (0x30 bytes each: +0xc the value index, +0x18 the
// table of the value labels) and the focused row at +0xe90. Entering PC mode puts the setting's index into the row and
// sets its value text; a change (value_hook, f_700300e100) goes to the setting; leaving PC mode (L/R or B) puts the
// game's own index back, so the game's option stays as it was.
static struct { int row; const char *key, *on_val, *off_val; int orig, set; } pc_rows[] = {
    {1, "TOTK_HUD_POSITION", "edges", "centered", 0, 0},   // Einsteigertipps: _On = edges
};
#define NROWS (sizeof pc_rows / sizeof *pc_rows)

static uint8_t *row_at(int row) { return (uint8_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)(page_obj + 0xe48) + (uint64_t)row * 0x30); }
static const char *row_label(int row, int idx) {   // the label of value idx, e.g. Mnu_Opt_Disp_NoviceGuide_On
    const uint64_t *tab = (const uint64_t *)(uintptr_t)*(uint64_t *)(row_at(row) + 0x18);
    return tab && idx >= 0 && idx < 8 ? (const char *)(uintptr_t)tab[idx] : NULL;
}
static int label_is_on(const char *l) { size_t n = l ? strlen(l) : 0; return n > 3 && !strcmp(l + n - 3, "_On"); }

static void page_hook(Cpu *c) { page_obj = c->x[19]; }

// The recorded page is still the display page: its rows pointer is set and row 1's first value label is one of
// Einsteigertipps (Mnu_Opt_Disp_NoviceGuide_*).
static int page_alive(void) {
    if (!page_obj || !*(uint64_t *)(uintptr_t)(page_obj + 0xe48)) return 0;
    const char *l = row_label(1, 0);
    return l && !strncmp(l, "Mnu_Opt_Disp_NoviceGuide", 24);
}

static void value_hook(Cpu *c) {   // diagnostic only (KAR_MENU_LOG): rows_leave takes the value
    if (!log_match || !pc_mode || c->x[0] != page_obj || !page_obj) return;
    fprintf(stderr, "kar_menu: value call row %d index %d\n", *(int32_t *)(uintptr_t)(page_obj + 0xe90), (int)(int32_t)c->x[1]);
}

// Sets row's value index and points the recorded value text field of that row at the label of idx; the text loop
// in menu_tick sets every recorded field right after, through text_lookup (our text in PC mode, the game's after).
static void row_show(Cpu *c, int row, int idx) {
    (void)c;
    const char *want = row_label(row, idx);
    if (!want) return;
    *(int32_t *)(row_at(row) + 0xc) = idx;
    for (int i = 0; i < nrec; i++)
        for (int k = 0; k < 8; k++)
            if (!rec[i].fixed && row_label(row, k) && rec[i].label == (uint64_t)(uintptr_t)row_label(row, k)) {
                rec[i].label = (uint64_t)(uintptr_t)want;
                break;
            }
}

static void rows_enter(Cpu *c) {
    if (!page_obj) return;
    for (size_t i = 0; i < NROWS; i++) {
        int row = pc_rows[i].row, want_on = !strcmp(cfg_value(pc_rows[i].key), pc_rows[i].on_val), idx = -1;
        pc_rows[i].orig = *(int32_t *)(row_at(row) + 0xc);
        for (int k = 0; k < 8 && idx < 0; k++)
            if (row_label(row, k) && label_is_on(row_label(row, k)) == want_on) idx = k;
        if (idx >= 0) row_show(c, row, idx);
    }
}

// Leaving PC mode: the row's value index now is the player's choice for the setting (a value hook cannot tell a
// LEFT/RIGHT from the selector laying out its values, both call f_700300e100); then the game's own index goes back.
static void rows_leave(Cpu *c) {
    if (!page_obj) return;
    for (size_t i = 0; i < NROWS; i++) {
        int row = pc_rows[i].row, cur = *(int32_t *)(row_at(row) + 0xc);
        const char *l = row_label(row, cur);
        if (l) {
            const char *v = label_is_on(l) ? pc_rows[i].on_val : pc_rows[i].off_val;
            if (strcmp(cfg_value(pc_rows[i].key), v)) cfg_set(pc_rows[i].key, v), cfg_save();
            if (log_match) fprintf(stderr, "kar_menu: row %d leaves at %d (%s): %s = %s\n", row, cur, l, pc_rows[i].key, v);
        }
        row_show(c, row, pc_rows[i].orig);
    }
}

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
    if (pc_mode && (down & 1u << 1) && page_alive()) {   // B: the page closes, the game's values go back first
        Cpu save_b;
        memcpy(save_b.x, c->x, sizeof save_b.x), memcpy(save_b.v, c->v, sizeof save_b.v), save_b.nzcv = c->nzcv;
        rows_leave(c);
        pc_mode = 0;
        memcpy(c->x, save_b.x, sizeof save_b.x), memcpy(c->v, save_b.v, sizeof save_b.v), c->nzcv = save_b.nzcv;
        return;
    }
    if (!(down & (1u << 6 | 1u << 7))) return;   // L or R
    int alive = page_alive();   // the page object still holds its rows (the text fields change as they animate)
    for (int i = 0; i < nrec && !alive; i++)
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
    if (pc_mode && alive) rows_leave(c);
    pc_mode = !pc_mode;
    if (pc_mode && alive) rows_enter(c);
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
    rt_register_host(base + SLOT_VALUE, value_hook);
    rt_register_host(base + SLOT_PAGE, page_hook);
}
