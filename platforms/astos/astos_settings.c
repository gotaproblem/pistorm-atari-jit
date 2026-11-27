/*
 * astos_settings.c - ASTOS's settings window for one environment.
 *
 * The rows, rules and pickers are the setup page's own (its editor as a
 * model: spm_* in setup_page.c), so a value set here is exactly what the
 * page would have written. Only the presentation is ASTOS's: the page's
 * tabs are one drop-down list (the category), and every setting's value is
 * a drop-down list too - a list key its choices, a switch On / Off, a ROM
 * or disk the files in rom_path / disk_path / fdd_path - or, for a typed
 * value, an edit field in place.
 */
#include <stdio.h>
#include <string.h>

#include "astos.h"
#include "astos_settings.h"
#include "setup_enums.h"
#include "setup_page.h"

/* ---- layout (logical 640x200) -------------------------------------- */
#define WX      4
#define WY      1
#define WW      632
#define WH      186
#define CAT_X   96
#define CAT_Y   15
#define CAT_W   144
#define LIST_Y  30
#define ROW_H   10
#define NVIS    13                      /* rows on screen               */
#define NAME_X  16
#define VAL_X   212
#define VAL_W   264
#define TAIL_X  484
#define FOOT_Y  (WY + WH - 18)
#define BTN_W   80
#define POP_MAX 10                      /* items on screen in a list    */

static int open_, sel, top;
static char title[32];

enum pop { POP_NONE = 0, POP_CAT, POP_VALUE, POP_EDIT };
enum act { A_CHOICE = 0, A_FILE, A_TYPE, A_REMOVE, A_ON, A_OFF, A_TAB };
static int pop, pop_sel, pop_top, pop_n, pop_row;
static struct { char label[48]; int act, arg; } item[160];
static char edit[256];
static int asked_close;

static int nrows(void) { return spm_rows(); }

static int row_usable(int i)
{
    struct spm_row r;
    spm_row(i, &r);
    return r.kind != SPM_LABEL && r.usable;
}

static void clamp(void)
{
    int n = nrows();
    if (sel >= n) sel = n - 1;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + NVIS) top = sel - NVIS + 1;
    if (top > n - NVIS) top = n - NVIS;
    if (top < 0) top = 0;
}

/* first row that can be used, at or after i (dir +1) / before (dir -1) */
static void move(int dir)
{
    for (int i = sel + dir; i >= 0 && i < nrows(); i += dir) {
        struct spm_row r;
        spm_row(i, &r);
        if (r.kind != SPM_LABEL && r.usable) { sel = i; break; }
    }
    clamp();
}

static void first_row(void)
{
    sel = -1;
    move(+1);
    if (sel < 0) sel = 0;
    top = 0;
    clamp();
}

int set_open(const char *cfg_path, const char *section, const char *ttl)
{
    if (spm_open(cfg_path, section) != 0)
        return -1;
    snprintf(title, sizeof title, "%s", ttl ? ttl : section);
    pop = POP_NONE;
    asked_close = 0;
    open_ = 1;
    first_row();
    return 0;
}

int set_is_open(void) { return open_; }

/* ---- the drop-down lists ------------------------------------------- */

static void add_item(const char *label, int act, int arg)
{
    if (pop_n >= (int)(sizeof item / sizeof *item))
        return;
    snprintf(item[pop_n].label, sizeof item[pop_n].label, "%s", label);
    item[pop_n].act = act;
    item[pop_n].arg = arg;
    pop_n++;
}

static void pop_scroll(void)
{
    if (pop_sel < pop_top) pop_top = pop_sel;
    if (pop_sel >= pop_top + POP_MAX) pop_top = pop_sel - POP_MAX + 1;
    if (pop_top < 0) pop_top = 0;
}

static void open_cat(void)
{
    pop = POP_CAT;
    pop_n = 0;
    for (int t = 0; t < SE_TAB_N; t++)
        add_item(se_tab_name(t), A_TAB, t);
    pop_sel = spm_tab();
    pop_top = 0;
    pop_scroll();
}

/* Enter (or a click) on a row: what it opens depends on the row */
static void open_value(int i)
{
    struct spm_row r;
    spm_row(i, &r);
    sel = i;
    clamp();
    pop_row = i;
    pop_n = 0;
    pop_top = 0;
    if (r.kind == SPM_LABEL)
        return;
    if (!r.usable) {                    /* the model says why, in spm_msg */
        spm_activate(i);
        return;
    }
    if (r.is_switch) {
        add_item("On", A_ON, 0);
        add_item("Off", A_OFF, 0);
        pop_sel = r.on ? 0 : 1;
        pop = POP_VALUE;
        return;
    }
    enum spm_mode m = spm_activate(i);
    if (m == SPM_CHOOSE) {
        int n = spm_choices(), has_unset = 0;
        for (int c = 0; c < n; c++) {
            if (spm_choice_off(c))
                continue;
            const char *lab = spm_choice(c);
            if (!lab || !*lab) { lab = "not set"; has_unset = 1; }
            if (c == spm_choice_cur())
                pop_sel = pop_n;
            add_item(lab, A_CHOICE, c);
        }
        if (r.on && !has_unset)
            add_item("(remove from this environment)", A_REMOVE, 0);
        pop = POP_VALUE;
    } else if (m == SPM_PICK) {
        int n = spm_files(), cur = spm_file_cur();
        pop_sel = 0;
        for (int f = 0; f < n; f++) {
            if (f == cur)
                pop_sel = pop_n;
            add_item(spm_file(f), A_FILE, f);
        }
        add_item("(type a name...)", A_TYPE, 0);
        if (r.on)
            add_item("(remove from this environment)", A_REMOVE, 0);
        pop = POP_VALUE;
    } else if (m == SPM_EDIT) {
        snprintf(edit, sizeof edit, "%s", spm_edit_text());
        pop = POP_EDIT;
    }
    pop_scroll();
}

static void close_pop(void)
{
    if (pop == POP_VALUE || pop == POP_EDIT)
        spm_cancel();
    pop = POP_NONE;
}

static void accept(int k)
{
    if (k < 0 || k >= pop_n)
        return;
    int act = item[k].act, arg = item[k].arg, row = pop_row;
    struct spm_row r;
    pop = POP_NONE;
    switch (act) {
    case A_TAB:
        spm_set_tab(arg);
        first_row();
        return;
    case A_CHOICE:
        spm_choose(arg);
        break;
    case A_FILE:
        if (spm_pick(arg) == SPM_EDIT) {     /* refused: type it */
            snprintf(edit, sizeof edit, "%s", spm_edit_text());
            pop = POP_EDIT;
        }
        break;
    case A_TYPE:
        spm_pick(-1);
        snprintf(edit, sizeof edit, "%s", spm_edit_text());
        pop = POP_EDIT;
        break;
    case A_REMOVE:
        spm_cancel();
        spm_toggle(row);
        break;
    case A_ON:
    case A_OFF:
        spm_row(row, &r);
        if (r.on != (act == A_ON))
            spm_toggle(row);
        break;
    }
    clamp();
}

/* ---- drawing -------------------------------------------------------- */

static void box(int x, int y, int w, const char *text, int arrow, int hot, int grey)
{
    if (grey) {
        g_textn(x + 4, y + 1, text, (w - 8) / 8, C_DIM);
        return;
    }
    g_fill(x, y, w, 10, hot ? C_ACCENT : C_WHITE);
    g_frame(x, y, w, 10, C_TEXT);
    g_textn(x + 4, y + 1, text, (w - 8 - (arrow ? 12 : 0)) / 8, hot ? C_WHITE : C_TEXT);
    if (arrow) {                                  /* a small down arrow */
        int ax = x + w - 12, c = hot ? C_WHITE : C_TEXT;
        g_fill(ax, y + 3, 7, 1, c);
        g_fill(ax + 1, y + 4, 5, 1, c);
        g_fill(ax + 2, y + 5, 3, 1, c);
        g_fill(ax + 3, y + 6, 1, 1, c);
    }
}

static void button(int x, int y, const char *label, int hot)
{
    g_fill(x, y, BTN_W, 12, hot ? C_ACCENT : C_FACE);
    g_frame(x, y, BTN_W, 12, C_TEXT);
    g_text(x + (BTN_W - g_text_w(label)) / 2, y + 2, label, hot ? C_WHITE : C_TEXT);
}

static int row_y(int i) { return LIST_Y + (i - top) * ROW_H; }

/* where the open list is drawn: under its box, or above it near the end */
static void pop_rect(int *x, int *y, int *w, int *h)
{
    int vis = pop_n < POP_MAX ? pop_n : POP_MAX;
    *h = vis * ROW_H + 4;
    if (pop == POP_CAT) {
        *x = CAT_X; *w = CAT_W; *y = CAT_Y + 11;
        return;
    }
    *x = VAL_X; *w = VAL_W;
    int by = row_y(pop_row);
    *y = by + 11;
    if (*y + *h > FOOT_Y - 2)
        *y = by - *h - 1;
    if (*y < WY + 12)
        *y = WY + 12;
}

static void draw_pop(void)
{
    int x, y, w, h;
    pop_rect(&x, &y, &w, &h);
    g_fill(x + 2, y + 1, w, h, C_TEXT);
    g_fill(x, y, w, h, C_WHITE);
    g_frame(x, y, w, h, C_TEXT);
    for (int k = pop_top; k < pop_n && k < pop_top + POP_MAX; k++) {
        int iy = y + 2 + (k - pop_top) * ROW_H;
        int on = k == pop_sel;
        if (on)
            g_fill(x + 2, iy, w - 4, ROW_H, C_ACCENT);
        g_textn(x + 6, iy + 1, item[k].label, (w - 20) / 8, on ? C_WHITE : C_TEXT);
    }
    if (pop_n > POP_MAX) {                         /* a scroll position bar */
        int bh = h - 4, th = bh * POP_MAX / pop_n;
        int ty = y + 2 + (bh - th) * pop_top / (pop_n - POP_MAX);
        g_fill(x + w - 6, y + 2, 2, bh, C_FACE);
        g_fill(x + w - 6, ty, 2, th < 4 ? 4 : th, C_TEXT);
    }
}

void set_draw(void)
{
    char line[96];
    /* the window */
    g_fill(WX + 2, WY + 1, WW, WH, C_TEXT);
    g_fill(WX, WY, WW, WH, C_WHITE);
    g_fill(WX, WY, WW, 11, C_ACCENT);
    g_frame(WX, WY, WW, WH, C_TEXT);
    snprintf(line, sizeof line, "Settings - %s", title);
    g_textn(WX + 6, WY + 2, line, 60, C_WHITE);

    /* the category: the page's tabs as one drop-down list */
    g_text(16, CAT_Y + 1, "Category", C_TEXT);
    box(CAT_X, CAT_Y, CAT_W, se_tab_name(spm_tab()), 1, pop == POP_CAT, 0);
    g_textn(CAT_X + CAT_W + 12, CAT_Y + 1, spm_msg(), (WW - CAT_W - CAT_X - 20) / 8, C_ACCENT);
    g_hline(WX + 4, LIST_Y - 3, WW - 8, C_FACE);

    /* the settings, each with its value as a drop-down */
    int n = nrows();
    for (int i = top; i < n && i < top + NVIS; i++) {
        struct spm_row r;
        spm_row(i, &r);
        int y = row_y(i), on = i == sel && pop != POP_CAT;
        if (r.kind == SPM_LABEL) {
            g_text(NAME_X - 4, y + 1, r.name, C_ACCENT);
            continue;
        }
        int grey = !r.usable;
        if (on)
            g_fill(NAME_X - 4, y, VAL_X - NAME_X - 4, ROW_H, grey ? C_FACE : C_ACCENT);
        g_textn(NAME_X, y + 1, r.name, (VAL_X - NAME_X - 8) / 8,
                on && !grey ? C_WHITE : grey ? C_DIM : C_TEXT);
        if (pop == POP_EDIT && i == pop_row) {
            char e[120];
            snprintf(e, sizeof e, "%.100s_", edit);
            int len = (int)strlen(e), maxc = (VAL_W - 8) / 8;
            g_fill(VAL_X, y, VAL_W, 10, C_WHITE);
            g_frame(VAL_X, y, VAL_W, 10, C_ACCENT);
            g_textn(VAL_X + 4, y + 1, len > maxc ? e + len - maxc : e, maxc, C_TEXT);
        } else {
            const char *v = r.value;
            char vb[112];
            if (!r.on && !r.is_switch && r.usable && strcmp(v, "empty") &&
                !strstr(v, "default") && !strstr(v, "not set")) {
                snprintf(vb, sizeof vb, "%s (default)", v);   /* not in the file */
                v = vb;
            }
            box(VAL_X, y, VAL_W, v, !grey, 0, grey);
        }
        const char *tail = r.why ? r.why : r.hint;
        if (tail)
            g_textn(TAIL_X + 4, y + 1, tail, (WX + WW - TAIL_X - 12) / 8, C_DIM);
    }
    if (top > 0)
        g_text(WX + WW - 20, LIST_Y, "^", C_ACCENT);
    if (top + NVIS < n)
        g_text(WX + WW - 20, LIST_Y + (NVIS - 1) * ROW_H, "v", C_ACCENT);

    /* the footer */
    g_hline(WX + 4, FOOT_Y - 3, WW - 8, C_FACE);
    button(16, FOOT_Y, "Save", spm_dirty());
    button(16 + BTN_W + 8, FOOT_Y, "Close", 0);
    if (spm_dirty())
        g_text(16 + 2 * (BTN_W + 8) + 4, FOOT_Y + 2, "unsaved changes", C_ACCENT);
    g_textn(420, FOOT_Y + 2, "Enter change  Space on/off", 26, C_DIM);

    if (pop == POP_CAT || pop == POP_VALUE)
        draw_pop();
}

/* ---- input ---------------------------------------------------------- */

static int try_close(void)
{
    if (spm_dirty() && !asked_close) {
        asked_close = 1;
        return 0;                         /* the footer says unsaved; again = go */
    }
    open_ = 0;
    pop = POP_NONE;
    return 1;
}

int set_key(struct si_event k)
{
    if (pop == POP_EDIT) {
        size_t n = strlen(edit);
        switch (k.key) {
        case SI_CHAR: case SI_SPACE:
            if (n + 1 < sizeof edit) { edit[n] = k.ch ? k.ch : ' '; edit[n + 1] = '\0'; }
            break;
        case SI_BACKSPACE: if (n) edit[n - 1] = '\0'; break;
        case SI_ENTER:
            if (spm_commit(edit) == SPM_DONE)
                pop = POP_NONE;           /* else refused: keep typing, see msg */
            break;
        case SI_ESC: close_pop(); break;
        default: break;
        }
        return 0;
    }
    if (pop == POP_CAT || pop == POP_VALUE) {
        switch (k.key) {
        case SI_UP:   if (pop_sel > 0) pop_sel--; pop_scroll(); break;
        case SI_DOWN: if (pop_sel < pop_n - 1) pop_sel++; pop_scroll(); break;
        case SI_HOME: pop_sel = 0; pop_scroll(); break;
        case SI_ENTER: case SI_SPACE: accept(pop_sel); break;
        case SI_ESC: close_pop(); break;
        default: break;
        }
        return 0;
    }
    if (k.key != SI_ESC)
        asked_close = 0;
    switch (k.key) {
    case SI_UP:    move(-1); break;
    case SI_DOWN:  move(+1); break;
    case SI_HOME:  first_row(); break;
    case SI_LEFT:  spm_set_tab(spm_tab() - 1); first_row(); break;
    case SI_RIGHT: spm_set_tab(spm_tab() + 1); first_row(); break;
    case SI_TAB:   open_cat(); break;
    case SI_ENTER: open_value(sel); break;
    case SI_SPACE: if (row_usable(sel)) spm_toggle(sel); else spm_activate(sel); clamp(); break;
    case SI_F10:   spm_save(); break;
    case SI_ESC:   return try_close();
    default: break;
    }
    return 0;
}

static int inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

int set_click(int x, int y)
{
    if (pop == POP_CAT || pop == POP_VALUE) {
        int px, py, pw, ph;
        pop_rect(&px, &py, &pw, &ph);
        if (inside(x, y, px, py, pw, ph)) {
            int k = pop_top + (y - py - 2) / ROW_H;
            if (k >= pop_top && k < pop_n)
                accept(k);
            return 0;
        }
        close_pop();
        return 0;
    }
    if (pop == POP_EDIT) {                 /* clicking away commits */
        if (spm_commit(edit) == SPM_DONE)
            pop = POP_NONE;
        return 0;
    }
    if (inside(x, y, CAT_X, CAT_Y, CAT_W, 10)) { open_cat(); return 0; }
    if (inside(x, y, 16, FOOT_Y, BTN_W, 12))  { spm_save(); asked_close = 0; return 0; }
    if (inside(x, y, 16 + BTN_W + 8, FOOT_Y, BTN_W, 12)) return try_close();
    asked_close = 0;
    int n = nrows();
    for (int i = top; i < n && i < top + NVIS; i++) {
        int ry = row_y(i);
        if (y >= ry && y < ry + ROW_H) {
            struct spm_row r;
            spm_row(i, &r);
            if (r.kind == SPM_LABEL)
                return 0;
            sel = i;
            if (x >= VAL_X)              /* the value: open its list */
                open_value(i);
            return 0;
        }
    }
    /* the scroll marks */
    if (inside(x, y, WX + WW - 24, LIST_Y, 16, 10) && top > 0) { top--; if (sel >= top + NVIS) sel = top + NVIS - 1; }
    if (inside(x, y, WX + WW - 24, LIST_Y + (NVIS - 1) * ROW_H, 16, 10) && top + NVIS < n) {
        top++;
        if (sel < top) sel = top;
    }
    return 0;
}

int set_hover(int x, int y)
{
    if (pop != POP_CAT && pop != POP_VALUE)
        return 0;
    int px, py, pw, ph;
    pop_rect(&px, &py, &pw, &ph);
    if (!inside(x, y, px, py + 2, pw, ph - 4))
        return 0;
    int k = pop_top + (y - py - 2) / ROW_H;
    if (k >= 0 && k < pop_n && k != pop_sel) {
        pop_sel = k;
        return 1;
    }
    return 0;
}

const char *set_section(void) { return spm_section(); }
