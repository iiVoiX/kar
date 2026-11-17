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
#include "platform/platform.h"
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
#define TITLE_CTX 0xf00                // a resolver context for f_7004d01668 in the patch page (0x50 bytes)
#define SEL_MOVED 0x7002df110cull   // f_7002df110c(selector, 1): what LEFT/RIGHT call right after SEL_SET
#define SEL_SET 0x7002df08c0ull   // f_7002df08c0(selector, index): what LEFT/RIGHT call after +0x260 = index
#define TITLE_UPDATE 0x7004d01668ull   // f_7004d01668(component, context): looks the label (+0x70) up again
#define TITLE_CALLER 0x7004d016d8ull   // where f_7004d01668 calls the prefab text function

// PC mode of the display page (Anzeige): rows bound to a port setting (pc_rows) show it instead. Labels answered with
// our own text while pc_mode is on; the page is closed and opened again on a switch (L/R), so it is built with them.
// The tab "Allgemein" becomes "PC-Grafik". Rows not bound yet keep the game's own option.
static struct { const char *label; const char *text; uint64_t at; } pc_texts[64];   // made from pc_rows (kar_menu_init)
static size_t NPC;
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
    if (now - rec_frame > 30 && (page_obj != rec_page || !page_obj)) nrec = 0;   // a new build of the page
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
    uint64_t ret = *(const uint64_t *)(uintptr_t)(c->x[29] + 8);   // only the display page's value texts: the build
    if (managed(label, n) && (ret == 0x700300cb08ull || ret == 0x700300e1c8ull))   // loop and the value handler
        remember(c->x[0], c->x[1], 0, label, n);
}

static struct { uint64_t comp; char label[64]; } titles[16];
static PlatMutex title_lock = PLAT_MUTEX_INIT;   // several workers build the page's components in one frame
static int ntitle;
static uint64_t title_frame;

// f_70011333c0 + 0xc: x0 the label as a string object (byte 0x17 < 0: pointer at +0, length at +8; else inline,
// length in byte 0x17), x1 the text field. Prefab texts: the row names, the tab, "Zurücksetzen".
static void static_record(Cpu *c) {
    if (resetting || !c->x[0]) return;
    const uint8_t *s = (const uint8_t *)(uintptr_t)c->x[0];
    const char *p = (const char *)s;
    size_t n = 23 - (size_t)s[0x17];   // inline: byte 0x17 holds the free room of the 23 inline bytes
    if ((int8_t)s[0x17] < 0) { p = (const char *)(uintptr_t)*(const uint64_t *)s; n = *(const uint64_t *)(s + 8); }
    if (log_match) {   // every call: the raw string object, to see how row titles arrive
        const uint64_t *q = (const uint64_t *)s;
        fprintf(stderr, "kar_menu: f%llu static x0 %016llx %016llx %016llx dst %#llx parsed '%.*s'\n",
                (unsigned long long)frame_now(), (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
                (unsigned long long)c->x[1], (int)(p && n < 64 ? n : 0), p ? p : "");
    }
    // A row title: the engine's text component (x0 - 0x70 holds the label at +0x70) asks for its text. Kept, so a
    // switch of PC mode can make the component look its label up again (titles_refresh).
    if (p && n < 64 && managed(p, n) && *(const uint64_t *)(uintptr_t)(c->x[29] + 8) == TITLE_CALLER) {
        uint64_t now = frame_now();
        plat_mutex_lock(&title_lock);
        if (now - title_frame > 30) ntitle = 0;
        title_frame = now;
        if (ntitle < 16) titles[ntitle].comp = c->x[0] - 0x70, snprintf(titles[ntitle].label, sizeof titles[0].label, "%.*s", (int)n, p), ntitle++;
        plat_mutex_unlock(&title_lock);
    }
    // Prefab texts are not recorded: x1 is a local of the caller on a worker's stack (the caller copies the text into
    // the row itself), so setting it again later writes into a dead stack slot.
}


// PC rows (RELEASE_PLAN 15.4): a row of the display page that shows a port setting while the page is in PC mode. The
// page object (recorded by page_hook) keeps its rows at +0xe48 (0x30 bytes each: +0xc the value index, +0x18 the
// table of the value labels) and the focused row at +0xe90. Entering PC mode puts the setting's index into the row and
// sets its value text; a change (value_hook, f_700300e100) goes to the setting; leaving PC mode (L/R or B) puts the
// game's own index back, so the game's option stays as it was.
typedef struct { const char *game_label, *cfg_val, *text, *help_label, *help; } PcVal;   // the game's label and help label for a value, the setting's value, our texts
static struct {
    int row;                        // the row of the display page
    const char *key;                // the port setting
    const char *title_label, *title;   // the row's title label and what it shows in PC mode
    PcVal vals[4];
    const char *help_label, *help;  // a help text for the whole row (rows whose help does not change with the value)
    int orig, entered, applied, bound;   // the game's index, the index PC mode started with, the index last given to
                                         // the setting, bound this time (not set by the environment)
} pc_rows[] = {
    // Fullscreen sits on the checklist-tips row: moving a value with LEFT/RIGHT also runs the game's own change
    // callback (selector +0x2a0), and on the text-size row that made the menu text large until the page closed (pv1).
    {2, "TOTK_FULLSCREEN", "Mnu_Opt_Disp_Checker", "Vollbild",
     {{"Mnu_Opt_Disp_Checker_Off", "0", "Aus", "Mnu_Opt_Disp_Checker_Off_Help", "Zeigt das Spiel in einem Fenster."},
      {"Mnu_Opt_Disp_Checker_On", "1", "Ein", "Mnu_Opt_Disp_Checker_On_Help", "Zeigt das Spiel im Vollbild."}},
     "Mnu_Opt_Disp_Checker_Help", "Zeigt das Spiel im Fenster oder im Vollbild."},
    {1, "TOTK_HUD_POSITION", "Mnu_Opt_Disp_NoviceGuide", "HUD-Position",
     {{"Mnu_Opt_Disp_NoviceGuide_Off", "centered", "Mitte", "Mnu_Opt_Disp_NoviceGuide_Off_Help",
       "Zeigt die Rennanzeigen im mittleren 16:9-Bereich."},
      {"Mnu_Opt_Disp_NoviceGuide_On", "edges", "Rand", "Mnu_Opt_Disp_NoviceGuide_On_Help",
       "Setzt die Rennanzeigen an die Bildschirmr\xc3\xa4nder (Breitbild)."}}},
};
#define NROWS (sizeof pc_rows / sizeof *pc_rows)

static uint8_t *row_at(int row) { return (uint8_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)(page_obj + 0xe48) + (uint64_t)row * 0x30); }
static const char *row_label(int row, int idx) {   // the label of value idx, e.g. Mnu_Opt_Disp_NoviceGuide_On
    uint64_t beg = *(uint64_t *)(row_at(row) + 0x18), end = *(uint64_t *)(row_at(row) + 0x20);   // the labels' vector
    int n = beg && end > beg && (end - beg) / 8 <= 16 ? (int)((end - beg) / 8) : 0;
    return idx >= 0 && idx < n ? (const char *)(uintptr_t)((const uint64_t *)(uintptr_t)beg)[idx] : NULL;
}

// The page object and what identified it when it was built: its vtable word and row 1's hash. After the page is
// freed the memory may be reused or unmapped, so page_alive checks every pointer it follows (crash in pcA).
static uint64_t page_vt, page_hash1;
static int readable(uint64_t a, uint64_t n) {
    RtMemoryInfo mi;
    if (a < 0x10000 || mem_query(a, &mi) || !mi.state || !(mi.perm & 1)) return 0;
    return a + n <= mi.base + mi.size;
}

static void page_hook(Cpu *c) {
    uint64_t pg = c->x[19];
    if (c->x[22] == 0) nrec = 0;   // the row loop starts again (a build, or a refresh after a value change): it records
                                   // the value text fields anew, so none of an earlier build stays; PC mode stays
    if (pg == page_obj && page_vt) return;
    page_obj = pg;
    page_vt = *(const uint64_t *)(uintptr_t)pg;
    uint64_t rows = *(const uint64_t *)(uintptr_t)(pg + 0xe48);
    page_hash1 = rows ? *(const uint64_t *)(uintptr_t)(rows + 0x30) : 0;
}

// The recorded page is still the display page: same vtable and row 1 hash, and row 1's first value label is one of
// Einsteigertipps (Mnu_Opt_Disp_NoviceGuide_*). Otherwise it is forgotten.
static int page_alive(void) {
    if (!page_obj) return 0;
    uint64_t rows = 0, tab = 0, l = 0;
    int ok = readable(page_obj, 0xe98) && *(const uint64_t *)(uintptr_t)page_obj == page_vt &&
             (rows = *(const uint64_t *)(uintptr_t)(page_obj + 0xe48)) && readable(rows, 0x60) &&
             *(const uint64_t *)(uintptr_t)(rows + 0x30) == page_hash1 &&
             (tab = *(const uint64_t *)(uintptr_t)(rows + 0x30 + 0x18)) && readable(tab, 8) &&
             (l = *(const uint64_t *)(uintptr_t)tab) && readable(l, 25) &&
             !strncmp((const char *)(uintptr_t)l, "Mnu_Opt_Disp_NoviceGuide", 24);
    if (!ok) page_obj = 0, page_vt = 0;
    return ok;
}

static void value_hook(Cpu *c) {   // diagnostic only (KAR_MENU_LOG): rows_leave takes the value
    if (!log_match || !pc_mode || c->x[0] != page_obj || !page_obj) return;
    fprintf(stderr, "kar_menu: value call row %d index %d\n", *(int32_t *)(uintptr_t)(page_obj + 0xe90), (int)(int32_t)c->x[1]);
}

// Sets row's value index and points the recorded value text field of that row at the label of idx; the text loop
// in menu_tick sets every recorded field right after, through text_lookup (our text in PC mode, the game's after).
// The game's own option of the focused row back to idx, with its live effect (a larger menu text for the text
// size): the selector setter with +0xc still on the old value, then f_7002df110c, the way LEFT/RIGHT do (pr1).
static void row_effect(Cpu *c, int row, int idx) {
    if (*(const int32_t *)(uintptr_t)(page_obj + 0xe90) != row) return;
    *(int32_t *)(uintptr_t)(page_obj + 0xb90 + 0x260) = idx;
    *(int32_t *)(uintptr_t)(page_obj + 0xb90 + 0x1f4) = idx;
    rt_call(c, SEL_SET, 2, page_obj + 0xb90, (uint64_t)(uint32_t)idx);
    rt_call(c, SEL_MOVED, 2, page_obj + 0xb90, (uint64_t)1);
    if (log_match) fprintf(stderr, "kar_menu: f%llu row %d game value back to %d\n", (unsigned long long)frame_now(), row, idx);
}

static void row_show(Cpu *c, int row, int idx) {
    const char *want = row_label(row, idx);
    if (!want) return;
    // +0xc first: the page then takes idx without the option's live effect (pr0), so PC mode never changes the
    // game's option. The page has one selector (+0xb90) for the focused row; focusing a row loads it from the row's
    // +0xc (f_700300c5f4), so only the focused row needs it set, the way LEFT/RIGHT do (+0x260, then the setter).
    *(int32_t *)(row_at(row) + 0xc) = idx;
    if (*(const int32_t *)(uintptr_t)(page_obj + 0xe90) == row) {
        *(int32_t *)(uintptr_t)(page_obj + 0xb90 + 0x260) = idx;
        *(int32_t *)(uintptr_t)(page_obj + 0xb90 + 0x1f4) = idx;
        rt_call(c, SEL_SET, 2, page_obj + 0xb90, (uint64_t)(uint32_t)idx);
        if (log_match) fprintf(stderr, "kar_menu: f%llu row %d selector set %d, %d texts\n", (unsigned long long)frame_now(), row, idx, nrec);
    }
    for (size_t r = 0; r < NROWS; r++)
        if (pc_rows[r].row == row) pc_rows[r].applied = idx;   // our own setting, not the player's
    for (int i = 0; i < nrec; i++)
        for (int k = 0; k < 8; k++)
            if (!rec[i].fixed && row_label(row, k) && rec[i].label == (uint64_t)(uintptr_t)row_label(row, k)) {
                rec[i].label = (uint64_t)(uintptr_t)want;
                break;
            }
}

static const PcVal *val_of(int i, const char *label) {
    for (int j = 0; j < 4 && pc_rows[i].vals[j].game_label; j++)
        if (label && !strcmp(label, pc_rows[i].vals[j].game_label)) return &pc_rows[i].vals[j];
    return NULL;
}

// Entering PC mode: each bound row shows its setting (the index whose value is the setting's, else the game's stays).
static void rows_enter(Cpu *c) {
    if (!page_obj) return;
    if (log_match) {   // diagnostic: the row records and the recorded text fields, to find the visible row titles
        for (int r = 0; r < 5; r++) {
            for (int k = 0; k < 8 && row_label(r, k); k++) fprintf(stderr, "kar_menu: row %d label %d %s\n", r, k, row_label(r, k));
            const uint64_t *q = (const uint64_t *)(uintptr_t)row_at(r);
            fprintf(stderr, "kar_menu: row %d: %016llx %016llx %016llx %016llx %016llx %016llx\n", r,
                    (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3],
                    (unsigned long long)q[4], (unsigned long long)q[5]);
        }
        for (int i = 0; i < nrec; i++)
            fprintf(stderr, "kar_menu: rec %d dst %#llx label %s\n", i, (unsigned long long)rec[i].dst,
                    rec[i].fixed ? "(prefab)" : (const char *)(uintptr_t)rec[i].label);
        const uint64_t *pg = (const uint64_t *)(uintptr_t)page_obj;
        const uint64_t *vt = (const uint64_t *)(uintptr_t)pg[0];
        fprintf(stderr, "kar_menu: page %#llx vtable %#llx: %#llx %#llx %#llx %#llx\n", (unsigned long long)page_obj,
                (unsigned long long)pg[0], (unsigned long long)vt[0], (unsigned long long)vt[1], (unsigned long long)vt[2],
                (unsigned long long)vt[3]);
        for (int k = 0x1c0; k < 0x1d4; k++)   // page words 0xe00..0xea0
            fprintf(stderr, "kar_menu: page+%#x %016llx\n", k * 8, (unsigned long long)pg[k]);
    }
    for (size_t i = 0; i < NROWS; i++) {
        int row = pc_rows[i].row, idx = -1;
        pc_rows[i].orig = *(int32_t *)(row_at(row) + 0xc);
        pc_rows[i].bound = !cfg_from_env(pc_rows[i].key);   // the environment fixes it: the row stays the game's
        if (!pc_rows[i].bound) continue;
        const char *want = cfg_value(pc_rows[i].key);
        for (int k = 0; k < 8 && row_label(row, k) && idx < 0; k++) {
            const PcVal *v = val_of((int)i, row_label(row, k));
            if (v && !strcmp(v->cfg_val, want)) idx = k;
        }
        if (idx < 0) idx = pc_rows[i].orig;
        pc_rows[i].entered = pc_rows[i].applied = idx;
        row_show(c, row, idx);
    }
}

// Leaving PC mode: a bound row whose index the player changed gives its value to the setting (a value not in the table
// is left alone when the row was only visited); then the game's own index goes back.
static void rows_leave(Cpu *c) {
    if (!page_obj) return;
    for (size_t i = 0; i < NROWS; i++) {
        if (!pc_rows[i].bound) continue;
        int row = pc_rows[i].row, cur = *(int32_t *)(row_at(row) + 0xc);
        const PcVal *v = cur != pc_rows[i].entered ? val_of((int)i, row_label(row, cur)) : NULL;
        if (v) {
            if (strcmp(cfg_value(pc_rows[i].key), v->cfg_val)) cfg_set(pc_rows[i].key, v->cfg_val);
            cfg_save();
        }
        if (log_match) fprintf(stderr, "kar_menu: row %d leaves at %d (entered %d): %s = %s\n", row, cur, pc_rows[i].entered,
                               pc_rows[i].key, cfg_value(pc_rows[i].key));
        row_show(c, row, pc_rows[i].orig);
    }
}

// The row titles of the page again (after pc_mode changed): f_7004d01668 resolves each component's label through
// text_lookup and swaps the text in when it differs. Its context only has to say which function resolves (+0x48,
// the prefab text function; +0x40 not 0); that function reads x0 and x1 only.
static int readable(uint64_t a, uint64_t n);
static void titles_refresh(Cpu *c) {
    uint8_t *ctx = rt_patch_page() + TITLE_CTX;
    memset(ctx, 0, 0x50);
    *(uint64_t *)(ctx + 0x40) = 1;
    *(uint64_t *)(ctx + 0x48) = SET_STATIC;
    for (int i = 0; i < ntitle; i++) {
        uint64_t comp = titles[i].comp;
        if (!readable(comp, 0x248) || *(const uint8_t *)(uintptr_t)(comp + 0x244)) continue;
        const uint8_t *str = (const uint8_t *)(uintptr_t)(comp + 0x70);   // the label, as a string object
        const char *lp = (const char *)str;
        size_t ln = 23 - (size_t)str[0x17];
        if ((int8_t)str[0x17] < 0) lp = (const char *)(uintptr_t)*(const uint64_t *)str, ln = *(const uint64_t *)(str + 8);
        if (ln != strlen(titles[i].label) || !readable((uint64_t)(uintptr_t)lp, ln) || memcmp(lp, titles[i].label, ln)) continue;
        resetting = 1;
        rt_call(c, TITLE_UPDATE, 2, comp, (uint64_t)(uintptr_t)ctx);
        resetting = 0;
        if (log_match) fprintf(stderr, "kar_menu: title %s refreshed at %#llx\n", titles[i].label, (unsigned long long)comp);
    }
}

// 0x7002fe7230 + 0x10, every frame on the game thread. L/R on the display page switch PC mode in place: entering puts
// the settings into the bound rows, leaving takes the rows' values into the settings and puts the game's values back;
// the recorded value texts are set again. B in PC mode (the player leaves the page) does the same as leaving.
// The row titles keep the game's text until the page is built again (RELEASE_PLAN 15.4: being worked on).
void kar_wide_tick(void);

static void set_texts(Cpu *c) {   // the recorded value texts again, through text_lookup
    resetting = 1;
    for (int i = 0; i < nrec; i++)
        if (!rec[i].fixed)
            rt_call(c, SET_TEXT, 4, rec[i].dst, rec[i].label, (uint64_t)strlen((const char *)(uintptr_t)rec[i].label), (uint64_t)0);
    resetting = 0;
}

static void menu_tick(Cpu *c) {
    kar_wide_tick();
    static uint64_t prev;
    uint64_t b = input_game_buttons(), down = b & ~prev;
    prev = b;
    if (log_match && page_alive()) {   // diagnostic: every value index change of the five rows, as it lands
        static int32_t seen[5] = {-9, -9, -9, -9, -9};
        for (int r = 0; r < 5; r++) {
            int32_t v = *(const int32_t *)(row_at(r) + 0xc);
            if (v != seen[r]) fprintf(stderr, "kar_menu: f%llu row %d index %d -> %d\n", (unsigned long long)frame_now(), r, seen[r], v), seen[r] = v;
        }
        static int32_t focus_seen = -9;
        int32_t f = *(const int32_t *)(uintptr_t)(page_obj + 0xe90);
        if (f != focus_seen) fprintf(stderr, "kar_menu: f%llu focus %d -> %d\n", (unsigned long long)frame_now(), focus_seen, f), focus_seen = f;
    }
    if (log_match && (down & (1u << 0 | 1u << 1)))   // diagnostic: A/B with the state that decides what they do
        fprintf(stderr, "kar_menu: f%llu %s pc_mode %d nrec %d alive %d\n", (unsigned long long)frame_now(),
                down & 1u ? "A" : "B", pc_mode, nrec, page_obj ? page_alive() : 0);
    if (pc_mode) {
        if (!page_alive()) {   // the page died without A/B/X: PC texts must not show anywhere else
            pc_mode = 0;
            if (log_match) fprintf(stderr, "kar_menu: f%llu pc mode 0 (page gone)\n", (unsigned long long)frame_now());
            return;
        }
        for (size_t i = 0; i < NROWS; i++) {   // a value moved with LEFT/RIGHT: the setting follows at once
            if (!pc_rows[i].bound) continue;
            int cur = *(const int32_t *)(row_at(pc_rows[i].row) + 0xc);
            if (cur == pc_rows[i].applied) continue;
            pc_rows[i].applied = cur;
            const PcVal *v = val_of((int)i, row_label(pc_rows[i].row, cur));
            if (v && strcmp(cfg_value(pc_rows[i].key), v->cfg_val)) cfg_set(pc_rows[i].key, v->cfg_val);
            if (log_match) fprintf(stderr, "kar_menu: f%llu row %d applied %d: %s = %s\n", (unsigned long long)frame_now(),
                                   pc_rows[i].row, cur, pc_rows[i].key, cfg_value(pc_rows[i].key));
            // LEFT/RIGHT also applied the game's own option (its live effect): put that back on the game's value,
            // then show cur again without the effect
            Cpu save;
            memcpy(save.x, c->x, sizeof save.x), memcpy(save.v, c->v, sizeof save.v), save.nzcv = c->nzcv;
            row_effect(c, pc_rows[i].row, pc_rows[i].orig);
            row_show(c, pc_rows[i].row, cur);
            set_texts(c);
            memcpy(c->x, save.x, sizeof save.x), memcpy(c->v, save.v, sizeof save.v), c->nzcv = save.nzcv;
        }
    }
    const uint64_t LEAVE = 1u << 0 | 1u << 1 | 1u << 2;   // A, B, X (X resets the page's values)
    if (!(down & (LEAVE | 1u << 6 | 1u << 7)) || !page_alive()) return;
    if (!pc_mode && !nrec) return;   // entering needs the page's value texts
    // A or B in PC mode: PC mode ends first. B leaves the page; A on a card opens a sub page, and the page does not
    // say whether a card or a row has the focus (+0xe90 stays on the last row, ms4), while A on a row does nothing.
    if ((down & LEAVE) && !(down & (1u << 6 | 1u << 7)) && !pc_mode) return;
    Cpu save;
    memcpy(save.x, c->x, sizeof save.x), memcpy(save.v, c->v, sizeof save.v), save.nzcv = c->nzcv;
    if ((down & LEAVE) && !(down & (1u << 6 | 1u << 7))) {   // A, B or X: PC mode ends
        rows_leave(c);
        pc_mode = 0;
        set_texts(c);
        titles_refresh(c);
        if (log_match) fprintf(stderr, "kar_menu: f%llu pc mode 0 (leaving the page)\n", (unsigned long long)frame_now());
    } else {   // L or R
        if (pc_mode) rows_leave(c);
        pc_mode = !pc_mode;
        if (pc_mode) rows_enter(c);
        set_texts(c);
        titles_refresh(c);
        if (log_match) fprintf(stderr, "kar_menu: f%llu pc mode %d, %d texts set\n", (unsigned long long)frame_now(), pc_mode, nrec);
    }
    memcpy(c->x, save.x, sizeof save.x), memcpy(c->v, save.v, sizeof save.v), c->nzcv = save.nzcv;
}

void kar_menu_init(void) {
    if (rt_patch_set("karmenu") < 0) return;   // another game or generated code predating the patch
    uint8_t *page = rt_patch_page();
    NPC = 0;
    pc_texts[NPC++] = (typeof(pc_texts[0])){"Mnu_Opt_Disp_OptMode_Cmn", "PC-Grafik", 0};
    for (size_t i = 0; i < NROWS; i++) {
        pc_texts[NPC++] = (typeof(pc_texts[0])){pc_rows[i].title_label, pc_rows[i].title, 0};
        if (pc_rows[i].help_label) pc_texts[NPC++] = (typeof(pc_texts[0])){pc_rows[i].help_label, pc_rows[i].help, 0};
        for (int j = 0; j < 4 && pc_rows[i].vals[j].game_label; j++) {
            const PcVal *v = &pc_rows[i].vals[j];
            if (NPC + 2 > sizeof pc_texts / sizeof *pc_texts) rt_fatal("kar_menu: more PC texts than pc_texts holds");
            pc_texts[NPC++] = (typeof(pc_texts[0])){v->game_label, v->text, 0};
            if (v->help_label) pc_texts[NPC++] = (typeof(pc_texts[0])){v->help_label, v->help, 0};
        }
    }
    size_t off = TEXTS;
    for (size_t i = 0; i < NPC; i++) {
        if (off + 2 * strlen(pc_texts[i].text) + 2 > TITLE_CTX) rt_fatal("kar_menu: the PC texts reach the title context at +0xf00");
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
