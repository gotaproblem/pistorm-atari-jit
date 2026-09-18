/*
 * setup_page.c - the pre-boot setup page, revision 2: the checklist.
 *
 * Builds are the .cfg's [sections]. The editor lists EVERY key the
 * emulator accepts (setup_enums.c's catalogue), on seven tabs, and a row
 * is ticked when the build has that line:
 *
 *   [x] key     value          the line is in the build
 *   [ ] key     empty          it is not written at all
 *
 * Unticking removes the line - never "writes it disabled" - because the
 * emulator treats an absent key and a disabled one differently. Ticking
 * writes the key's tick value (a switch: enabled; a list: its usual
 * choice; a text key: it asks for the value first, and stays unticked
 * if none is given). Space or pad Y ticks; Enter edits the value with
 * the chooser, the switch flip or the text editor; Left/Right changes
 * tab. A row whose parent is off (network_* under network) is greyed and
 * cannot be ticked.
 *
 * Repeated keys (hdd, acsi, hostfs) are one row here; the Drives tab
 * expands them into numbered slots in step 3.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "setup_page.h"
#include "setup_input.h"
#include "setup_cfg.h"
#include "setup_enums.h"
#include "setup_hdmi.h"

#define MAX_ROWS   96
#define MAX_BUILDS 16

enum screen { SCR_BUILDS = 0, SCR_EDIT };
enum row_kind { ROW_KEY = 0, ROW_SAVE, ROW_BOOT };

struct row {
    enum row_kind kind;
    char text[SC_KEY_LEN];       /* the key                                   */
    int  ord;                    /* nth occurrence (repeated keys)            */
    int  ticked;                 /* the build has the line                    */
    int  blocked;                /* parent off: greyed, cannot be ticked      */
    const char *why;             /* cpu rule fails: "needs 68020+", or NULL   */
};

struct state {
    struct sc_cfg cfg;
    enum screen   screen;
    /* builds screen */
    char builds[MAX_BUILDS][SC_SEC_LEN];
    int  nbuilds, bsel;
    char boot[SC_SEC_LEN];       /* [psctrl] boot                              */
    int  secs;                   /* countdown, -1 once a key arrived           */
    /* editor */
    char sec[SC_SEC_LEN];        /* the build being edited                     */
    int  tab;
    struct row row[MAX_ROWS];
    int  nrow, sel, top, list_rows;
    int  editing, choosing, choice;
    char edit[SC_LINE_LEN];
    char msg[64];
};

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* dd/mm/yy hh:mm - 14 characters, no seconds, so the bar has room */
static void stamp_now(char *buf, unsigned long n)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, n, "%d/%m/%y %H:%M", &tm);
}

/* A switch is a key whose value is empty (present = on, the .cfg's own
 * shorthand) or one of the words the emulator's parser treats as a
 * boolean. They read and write as enabled / disabled, never "(on)":
 * config_file.c's get_bool_default_true() takes "disabled" as false and
 * anything else, including a bare key, as true. */
static const char *false_words[] = { "0", "off", "no", "false", "disabled",
                                     "disable" };
static const char *true_words[]  = { "1", "on", "yes", "true", "enabled",
                                     "enable" };

static int word_n_in(const char *v, unsigned long len, const char **list, int n)
{
    for (int i = 0; i < n; i++)
        if (strlen(list[i]) == len && !strncasecmp(v, list[i], len))
            return 1;
    return 0;
}

/* Values like `kbd usb nograb`, `kbd disabled`, `usb gamepad off` carry
 * the device in the first word and the state (and any options) after it.
 * These read the first word only: `disabled nograb` is false, `nograb`
 * on its own is not a boolean at all. */
static const char *rest_after_word(const char *val, unsigned long n)
{
    const char *rest = val + n;
    while (*rest == ' ' || *rest == '\t')
        rest++;
    return rest;
}

static int word_in_false_first(const char *v)
{
    return word_n_in(v, strcspn(v, " \t"), false_words, 6);
}

static int first_word_is_bool(const char *v)
{
    unsigned long len = strcspn(v, " \t");
    return word_n_in(v, len, false_words, 6) || word_n_in(v, len, true_words, 6);
}

/* 1 = the value names the device (kbd usb..., usb gamepad...), 2 = kbd
 * with a plain boolean word, 0 = an ordinary key. */
static int device_key(const char *key, const char *val, const char **rest)
{
    if (!val)
        return 0;
    if (!strcasecmp(key, "kbd")) {
        if (!strncasecmp(val, "usb", 3)) {
            *rest = rest_after_word(val, 3);
            return 1;
        }
        *rest = val;
        return 2;
    }
    if (!strcasecmp(key, "usb") && !strncasecmp(val, "gamepad", 7)) {
        *rest = rest_after_word(val, 7);
        return 1;
    }
    return 0;
}

int sp_is_switch(const char *val)
{
    if (!val)
        return 0;
    if (!*val)
        return 1;                            /* a bare key is "on" */
    return first_word_is_bool(val);
}

static int switch_on(const char *val)
{
    if (!val || !*val)
        return 1;
    return !word_in_false_first(val);
}

/* Is this ROW a switch? The device keys are, whatever their value looks
 * like (`kbd usb nograb` is on, `kbd disabled nograb` is off), and so is
 * any key whose value is a boolean word or empty. */
int sp_row_is_switch(const char *key, const char *val)
{
    const char *rest;
    if (device_key(key, val, &rest))
        return 1;
    return sp_is_switch(val);
}

/* and is it on? */
int sp_row_on(const char *key, const char *val)
{
    const char *rest;
    int dev = device_key(key, val, &rest);
    if (dev == 1)
        return !*rest || !word_in_false_first(rest);
    if (dev == 2)
        return !word_in_false_first(rest);
    return switch_on(val);
}


/*
 * A few .cfg keys read badly as key + value, because the key names the
 * subsystem and the value names what is being switched on:
 *
 *   kbd usb            -> "usb kbd/mouse    usb"   (config_file.c: kbd usb
 *                         [nograb] [merge|standalone] injects the USB
 *                         keyboard AND mouse into the IKBD stream)
 *   usb gamepad        -> "usb gamepad      enabled"
 *   hostfs S /path     -> "hostfs           S: /path"
 *
 * Only the display changes; the file keeps the emulator's own spelling.
 */
const char *sp_row_label(const char *key, const char *val)
{
    if (!strcasecmp(key, "kbd"))
        return "usb kbd/mouse";
    if (!strcasecmp(key, "usb") && val && !strncasecmp(val, "gamepad", 7))
        return "usb gamepad";
    return key;
}


/* hostfs drive letters read as a drive: "S /path" -> "S: /path" */
static int hostfs_drive(const char *val)
{
    return val && ((*val >= 'A' && *val <= 'Z') || (*val >= 'a' && *val <= 'z')) &&
           (val[1] == ' ' || val[1] == '\t' || val[1] == ':');
}

const char *sp_row_value(const char *key, const char *val, char *buf,
                         unsigned long n)
{
    const char *rest;
    /* a key with a known list reads as that list's label; an absent key
     * lands on the "" choice when the list has one (stram_size: not set) */
    if (se_count(key)) {
        int i = se_index(key, val ? val : "");
        if (i >= 0)
            return se_label(key, i);
    }
    if (!val)
        return "";
    int dev = device_key(key, val, &rest);
    if (dev) {
        int on = sp_row_on(key, val);
        const char *opts = first_word_is_bool(rest) ?
                           rest_after_word(rest, strcspn(rest, " \t")) : rest;
        if (*opts)
            snprintf(buf, n, "%s (%s)", on ? "enabled" : "disabled", opts);
        else
            snprintf(buf, n, "%s", on ? "enabled" : "disabled");
        return buf;
    }
    if (!strcasecmp(key, "hostfs") && hostfs_drive(val)) {
        const char *rest = val + 1;
        if (*rest == ':')
            rest++;
        while (*rest == ' ' || *rest == '\t')
            rest++;
        snprintf(buf, n, "%c: %s", *val, rest);
        return buf;
    }
    return sp_switch_text(val);
}

/* What a typed value is written back as: the page shows "S: /path", the
 * file keeps "S /path", and either spelling may be typed. */
const char *sp_value_from_edit(const char *key, const char *typed, char *buf,
                               unsigned long n)
{
    if (!strcasecmp(key, "hostfs") && hostfs_drive(typed)) {
        const char *rest = typed + 1;
        if (*rest == ':')
            rest++;
        while (*rest == ' ' || *rest == '\t')
            rest++;
        snprintf(buf, n, "%c %s", *typed, rest);
        return buf;
    }
    return typed;
}

/* enabled / disabled for a switch, or the value itself */
const char *sp_switch_text(const char *val)
{
    if (sp_is_switch(val))
        return switch_on(val) ? "enabled" : "disabled";
    return val;
}

/* ================================================================== */
/* colours: paper 0; on colour ink 3, hi (green) 2, key (red) 1;      */
/* on mono there is only ink 1 - keys go reverse instead              */
/* ================================================================== */
static int c_ink(const struct ss_screen *ss)  { return ss->planes == 1 ? 1 : 3; }
static int c_hi(const struct ss_screen *ss)   { return ss->planes == 1 ? 1 : 2; }
static int c_key(const struct ss_screen *ss)  { return ss->planes == 1 ? 1 : 1; }
static int mono(const struct ss_screen *ss)   { return ss->planes == 1; }

/* the help line: [key, action] pairs, keys in red (reverse on mono) */
struct help { const char *key, *act; };
static void draw_help(struct ss_screen *ss, int row, const struct help *h, int n)
{
    int len = 0;
    for (int i = 0; i < n; i++)
        len += (int)strlen(h[i].key) + 1 + (int)strlen(h[i].act) + (i ? 3 : 0);
    int col = (SS_COLS - len) / 2;
    if (col < 0) col = 0;
    ss_clear_row(ss, row, 0);
    for (int i = 0; i < n; i++) {
        if (i) col += 3;
        if (mono(ss))
            ss_puts(ss, col, row, h[i].key, 0, 1);          /* reverse   */
        else
            ss_puts(ss, col, row, h[i].key, c_key(ss), 0);  /* red       */
        col += (int)strlen(h[i].key);
        ss_putc(ss, col++, row, ' ', c_ink(ss), 0);
        ss_puts(ss, col, row, h[i].act, c_ink(ss), 0);
        col += (int)strlen(h[i].act);
    }
}

static void draw_bar(struct ss_screen *ss, const char *build)
{
    char stamp[24], left[64];
    int ink = c_ink(ss);
    ss_clear_row(ss, 0, ink);
    snprintf(left, sizeof left, " PSCTRL PiSTorm Setup%s%s%s%s",
             build ? "   [" : "", build ? build : "", build ? "]" : "", "");
    ss_puts(ss, 0, 0, left, 0, ink);
    stamp_now(stamp, sizeof stamp);
    ss_puts(ss, SS_COLS - 1 - (int)strlen(stamp), 0, stamp, 0, ink);
}

/* folder tabs, three rows, ASCII: the open tab has no line under it and
 * its name is green */
#define ROW_TABS   2
#define ROW_LIST   5
#define FOOTER     3
static void draw_tabs(struct ss_screen *ss, int active)
{
    int ink = c_ink(ss), hi = c_hi(ss);
    char t1[SS_COLS + 1], t3[SS_COLS + 1];
    int p1 = 0, p3 = 0;
    memset(t1, ' ', SS_COLS); memset(t3, '-', SS_COLS);
    t1[SS_COLS] = t3[SS_COLS] = '\0';
    p1 = 1; p3 = 1;
    ss_clear_row(ss, ROW_TABS + 1, 0);
    int col = 1;
    for (int i = 0; i < SE_TAB_N; i++) {
        const char *nm = se_tab_name(i);
        int w = (int)strlen(nm) + 2;
        /* top: .-----. */
        if (p1 + w + 3 <= SS_COLS) {
            t1[p1] = '.'; memset(t1 + p1 + 1, '-', w); t1[p1 + w + 1] = '.';
        }
        /* middle: | name | */
        ss_putc(ss, col, ROW_TABS + 1, '|', ink, 0);
        ss_putc(ss, col + 1, ROW_TABS + 1, ' ', ink, 0);
        ss_puts(ss, col + 2, ROW_TABS + 1, nm, i == active ? hi : ink, 0);
        ss_putc(ss, col + 2 + (int)strlen(nm), ROW_TABS + 1, ' ', ink, 0);
        ss_putc(ss, col + w + 1, ROW_TABS + 1, '|', ink, 0);
        /* base: '-----' closed, or '     ' open under the active tab */
        if (p3 + w + 3 <= SS_COLS) {
            t3[p3] = '\''; memset(t3 + p3 + 1, i == active ? ' ' : '-', w);
            t3[p3 + w + 1] = '\'';
        }
        p1 += w + 3; p3 += w + 3; col += w + 3;
    }
    ss_puts(ss, 0, ROW_TABS, t1, ink, 0);
    ss_puts(ss, 0, ROW_TABS + 2, t3, ink, 0);
}

/* ================================================================== */
/* the model                                                           */
/* ================================================================== */

static void load_builds(struct state *st)
{
    char secs[MAX_BUILDS + 1][SC_SEC_LEN];
    int ns = sc_sections(&st->cfg, secs, MAX_BUILDS + 1);
    st->nbuilds = 0;
    for (int i = 0; i < ns && st->nbuilds < MAX_BUILDS; i++)
        if (strcasecmp(secs[i], "psctrl"))
            snprintf(st->builds[st->nbuilds++], SC_SEC_LEN, "%.*s", SC_SEC_LEN - 1, secs[i]);
    const char *b = sc_get(&st->cfg, "psctrl", "boot");
    snprintf(st->boot, sizeof st->boot, "%.*s", SC_SEC_LEN - 1,
             (b && *b && strcasecmp(b, "psctrl")) ? b :
             (st->nbuilds ? st->builds[0] : ""));
    st->bsel = 0;
    for (int i = 0; i < st->nbuilds; i++)
        if (!strcasecmp(st->builds[i], st->boot))
            st->bsel = i;
}

/* is this row's parent (se_needs) on? absent or off = blocked */
static int parent_off(const struct state *st, const char *key)
{
    const char *needs = se_needs(key);
    if (!needs)
        return 0;
    const char *v = sc_get(&st->cfg, st->sec, needs);
    return !v || !sp_row_on(needs, v);
}

/* a list whose value is its "disabled" choice (blitter, ttram) is off:
 * the box shows it, the chooser never offers it */
static int list_off(const char *key, const char *val)
{
    int i = se_index(key, val);
    return i >= 0 && !strcasecmp(se_choice(key, i), "disabled");
}

static int choice_is_off(const char *key, int i)
{
    const char *c = se_choice(key, i);
    return c && !strcasecmp(c, "disabled");
}

/* a row the cursor may land on and the user may tick */
static int usable(const struct row *r)
{
    return r->kind != ROW_KEY || (!r->blocked && !r->why);
}

/* step the cursor, skipping rows that cannot be used; stays put at the ends */
static void move_sel(struct state *st, int dir)
{
    for (int i = st->sel + dir; i >= 0 && i < st->nrow; i += dir)
        if (usable(&st->row[i])) { st->sel = i; return; }
}

/* after a rebuild the cursor may sit on a row that just became unusable */
static void settle(struct state *st)
{
    if (usable(&st->row[st->sel]))
        return;
    int keep = st->sel;
    move_sel(st, +1);
    if (st->sel == keep) move_sel(st, -1);
}

/* the cpu changed: keys its rule now forbids leave the build */
static void drop_by_rules(struct state *st)
{
    const char *cpu = sc_get(&st->cfg, st->sec, "cpu");
    char gone[48] = "";
    for (int t = 0; t < SE_TAB_N; t++)
        for (int i = 0; i < se_tab_count(t); i++) {
            const char *k = se_tab_key(t, i);
            if (!se_cpu_rule(k, cpu) || !sc_get(&st->cfg, st->sec, k))
                continue;
            sc_set(&st->cfg, st->sec, k, NULL);
            snprintf(gone + strlen(gone), sizeof gone - strlen(gone), "%s%.14s",
                     *gone ? ", " : "", k);
        }
    if (*gone)
        snprintf(st->msg, sizeof st->msg, "cpu %.8s: dropped %.40s", cpu ? cpu : "68000", gone);
}

static void build_rows(struct state *st)
{
    const char *cpu = sc_get(&st->cfg, st->sec, "cpu");
    st->nrow = 0;
    int n = se_tab_count(st->tab);
    for (int i = 0; i < n && st->nrow < MAX_ROWS - 2; i++) {
        const char *k = se_tab_key(st->tab, i);
        if (se_retired(k))
            continue;
        /* developer knobs (addr32, jit ...) stay hidden unless the build
         * already carries them */
        if (se_developer(k) && !sc_get(&st->cfg, st->sec, k))
            continue;
        struct row *r = &st->row[st->nrow++];
        r->kind = ROW_KEY;
        snprintf(r->text, sizeof r->text, "%.*s", SC_KEY_LEN - 1, k);
        r->ord = 0;
        const char *v = sc_get(&st->cfg, st->sec, k);
        /* a switch is the box itself: ticked = in the build AND on */
        r->ticked = v && (se_kind(k) == SE_K_SWITCH ? sp_row_on(k, v) :
                          se_kind(k) == SE_K_LIST   ? !list_off(k, v) : 1);
        r->blocked = parent_off(st, k);
        r->why = se_cpu_rule(k, cpu);
    }
    st->row[st->nrow].kind = ROW_SAVE; st->row[st->nrow++].text[0] = '\0';
    st->row[st->nrow].kind = ROW_BOOT; st->row[st->nrow++].text[0] = '\0';
    if (st->sel >= st->nrow) st->sel = st->nrow - 1;
    st->list_rows = SS_ROWS - ROW_LIST - FOOTER;
    int nk = st->nrow - 2;
    if (st->sel < st->top) st->top = st->sel;
    if (st->sel < nk && st->sel >= st->top + st->list_rows) st->top = st->sel - st->list_rows + 1;
    int max = nk - st->list_rows; if (max < 0) max = 0;
    if (st->top > max) st->top = max;
    settle(st);
}

/* what a row shows: the value if ticked, else empty / disabled / default */
static const char *row_value(const struct state *st, const struct row *r,
                             char *buf, unsigned long n)
{
    const char *k = r->text;
    const char *v = sc_get_n(&st->cfg, st->sec, k, r->ord);
    if (se_kind(k) == SE_K_SWITCH)            /* the box is the value */
        return "";
    if (v && !(se_kind(k) == SE_K_LIST && list_off(k, v)))
        return sp_row_value(k, v, buf, n);
    switch (se_kind(k)) {
    case SE_K_LIST: {
        const char *d = se_tick_value(k);
        int i = se_index(k, d);
        return i >= 0 ? se_label(k, i) : d;
    }
    default: return "empty";
    }
}

/* tick: write the key's tick value (text keys ask first); untick: remove */
static void tick(struct state *st, int open_editor_for_text)
{
    struct row *r = &st->row[st->sel];
    if (r->kind != ROW_KEY)
        return;
    if (r->why) {
        snprintf(st->msg, sizeof st->msg, "%.20s: %s", r->text, r->why);
        return;
    }
    if (r->blocked) {
        snprintf(st->msg, sizeof st->msg, "%.20s needs %.20s ticked", r->text,
                 se_needs(r->text) ? se_needs(r->text) : "its parent");
        return;
    }
    if (r->ticked) {
        /* off = out of the build; the two the emulator has on by default
         * must say so */
        sc_set_n(&st->cfg, st->sec, r->text, r->ord,
                 se_absent_on(r->text) ? "disabled" : NULL);
        snprintf(st->msg, sizeof st->msg, "%.20s %s", r->text,
                 se_kind(r->text) == SE_K_SWITCH ? "off" : "removed from the build");
        return;
    }
    const char *tv = se_tick_value(r->text);
    if (se_kind(r->text) == SE_K_TEXT && !*tv) {
        if (open_editor_for_text) {           /* needs a value first */
            st->edit[0] = '\0';
            st->editing = 1;
            snprintf(st->msg, sizeof st->msg, "type a value for %.20s", r->text);
        }
        return;
    }
    sc_set_n(&st->cfg, st->sec, r->text, r->ord, tv);
    if (se_kind(r->text) == SE_K_SWITCH)
        snprintf(st->msg, sizeof st->msg, "%.20s on", r->text);
    else
        snprintf(st->msg, sizeof st->msg, "%.20s = %.30s", r->text, tv);
    if (!strcasecmp(r->text, "cpu"))
        drop_by_rules(st);
}

static void commit_edit(struct state *st)
{
    struct row *r = &st->row[st->sel];
    char raw[SC_LINE_LEN];
    const char *val = sp_value_from_edit(r->text, st->edit, raw, sizeof raw);
    st->editing = 0;
    if (!*val) {                              /* empty = leave it unticked */
        sc_set_n(&st->cfg, st->sec, r->text, r->ord, NULL);
        snprintf(st->msg, sizeof st->msg, "%.20s left empty", r->text);
        return;
    }
    if (se_kind(r->text) == SE_K_INT) {
        char *end; strtol(val, &end, 0);
        if (end == val || *end) {
            snprintf(st->msg, sizeof st->msg, "%.20s must be a number", r->text);
            return;
        }
    }
    if (sc_set_n(&st->cfg, st->sec, r->text, r->ord, val) == 0)
        snprintf(st->msg, sizeof st->msg, "%.20s = %.30s", r->text, val);
}

/* Enter: a list opens the chooser, a switch flips, text/int opens the editor */
static void edit_row(struct state *st)
{
    struct row *r = &st->row[st->sel];
    if (r->kind != ROW_KEY)
        return;
    if (r->blocked || r->why) {
        tick(st, 0);                          /* prints why */
        return;
    }
    const char *k = r->text;
    const char *v = sc_get_n(&st->cfg, st->sec, k, r->ord);
    if (se_count(k)) {
        int at = se_index(k, v ? v : se_tick_value(k));
        st->choice = at < 0 ? 0 : at;
        if (choice_is_off(k, st->choice))
            st->choice = se_index(k, se_tick_value(k));
        st->choosing = 1;
        return;
    }
    if (se_kind(k) == SE_K_SWITCH) {          /* the box is the switch */
        tick(st, 0);
        return;
    }
    char vbuf[SC_LINE_LEN];
    snprintf(st->edit, sizeof st->edit, "%s",
             v ? sp_row_value(k, v, vbuf, sizeof vbuf) : "");
    st->editing = 1;
}

static void choose_accept(struct state *st)
{
    struct row *r = &st->row[st->sel];
    const char *c = se_choice(r->text, st->choice);
    st->choosing = 0;
    if (!c) return;
    if (!*c) {
        sc_set_n(&st->cfg, st->sec, r->text, r->ord, NULL);
        snprintf(st->msg, sizeof st->msg, "%.20s removed (not set)", r->text);
    } else if (sc_set_n(&st->cfg, st->sec, r->text, r->ord, c) == 0) {
        snprintf(st->msg, sizeof st->msg, "%.20s = %.30s", r->text,
                 se_label(r->text, st->choice));
        if (!strcasecmp(r->text, "cpu"))
            drop_by_rules(st);
    }
}

/* ================================================================== */
/* drawing                                                             */
/* ================================================================== */

static void draw_builds(struct ss_screen *ss, const struct state *st)
{
    int ink = c_ink(ss), hi = c_hi(ss);
    char line[SS_COLS + 1];
    static const struct help h[] = { { "Up/Down", "move" }, { "Enter", "boot" },
        { "E", "edit" }, { "ESC/pad X", "leave" } };
    ss_clear(ss, 0);
    draw_bar(ss, NULL);
    draw_help(ss, 1, h, 4);
    ss_puts(ss, 1, 3, "Builds:", ink, 0);
    for (int i = 0; i < st->nbuilds; i++) {
        int on = i == st->bsel;
        snprintf(line, sizeof line, " %s %-16.16s%s",
                 strcasecmp(st->builds[i], st->boot) ? " " : "*", st->builds[i],
                 strcasecmp(st->builds[i], st->boot) ? "" : "  (boots on countdown)");
        ss_puts(ss, 3, 4 + i, line, on ? 0 : ink, on ? hi : 0);
    }
    snprintf(line, sizeof line, "rom_path %.16s  disk_path %.16s  fdd_path %.16s",
             sc_get(&st->cfg, "psctrl", "rom_path") ? sc_get(&st->cfg, "psctrl", "rom_path") : "-",
             sc_get(&st->cfg, "psctrl", "disk_path") ? sc_get(&st->cfg, "psctrl", "disk_path") : "-",
             sc_get(&st->cfg, "psctrl", "fdd_path") ? sc_get(&st->cfg, "psctrl", "fdd_path") : "-");
    ss_puts(ss, 1, SS_ROWS - 3, line, hi, 0);
    ss_clear_row(ss, SS_ROWS - 1, 0);
    if (st->msg[0])
        ss_puts(ss, 1, SS_ROWS - 1, st->msg, ink, 0);
    else if (st->secs > 0) {
        snprintf(line, sizeof line, "Booting %.15s in %d...", st->boot, st->secs);
        ss_puts(ss, 1, SS_ROWS - 1, line, ink, 0);
    } else
        ss_puts(ss, 1, SS_ROWS - 1, "countdown stopped", ink, 0);
}

static void draw_edit(struct ss_screen *ss, const struct state *st)
{
    int ink = c_ink(ss), hi = c_hi(ss);
    char line[SC_LINE_LEN], vbuf[SC_LINE_LEN];
    static const struct help h[] = { { "Up/Down", "move" }, { "Left/Right", "tab" },
        { "Space/pad Y", "tick" }, { "Enter", "edit" }, { "ESC/pad X", "back" } };
    static const struct help he[] = { { "type", "a value" }, { "Enter", "accept" },
        { "ESC", "cancel" } };
    static const struct help hc[] = { { "Up/Down", "choose" }, { "Enter", "accept" },
        { "ESC", "cancel" } };
    ss_clear(ss, 0);
    draw_bar(ss, st->sec);
    if (st->editing)       draw_help(ss, 1, he, 3);
    else if (st->choosing) draw_help(ss, 1, hc, 3);
    else                   draw_help(ss, 1, h, 5);
    draw_tabs(ss, st->tab);

    int nk = st->nrow - 2;
    for (int i = 0; i < st->list_rows; i++) {
        int k = st->top + i, row = ROW_LIST + i;
        ss_clear_row(ss, row, 0);
        if (k >= nk) continue;
        const struct row *r = &st->row[k];
        int on = st->sel == k;
        const char *val;
        const char *tail = "";
        if (on && st->choosing) {
            snprintf(line, sizeof line, " %s %-20.20s < %.36s >",
                     r->ticked ? "[x]" : "[ ]", r->text,
                     se_label(r->text, st->choice));
        } else if (on && st->editing) {
            snprintf(line, sizeof line, " %s %-20.20s %-44.44s",
                     r->ticked ? "[x]" : "[ ]", r->text, st->edit);
        } else {
            val = row_value(st, r, vbuf, sizeof vbuf);
            char whybuf[32];
            if (r->why) {
                snprintf(whybuf, sizeof whybuf, "(%.28s)", r->why);
                tail = whybuf;
            } else if (r->blocked) {
                snprintf(whybuf, sizeof whybuf, "(needs %.20s)",
                         se_needs(r->text) ? se_needs(r->text) : "parent");
                tail = whybuf;
            }
            /* [-] = cannot be ticked here; the cursor skips it */
            snprintf(line, sizeof line, " %s %-20.20s %-36.36s%s",
                     (r->why || r->blocked) ? "[-]" : r->ticked ? "[x]" : "[ ]",
                     sp_row_label(r->text, sc_get_n(&st->cfg, st->sec, r->text, r->ord)),
                     val, tail);
        }
        /* pad to the width so the cursor bar is always full */
        unsigned long len = strlen(line);
        while (len < SS_COLS - 2) line[len++] = ' ';
        line[len] = '\0';
        int grey = r->blocked || r->why || !r->ticked;
        ss_puts(ss, 1, row, line, on ? 0 : (grey ? hi : ink), on ? hi : 0);
    }
    if (nk > st->list_rows) {
        snprintf(line, sizeof line, "%d-%d of %d", st->top + 1,
                 st->top + st->list_rows > nk ? nk : st->top + st->list_rows, nk);
        ss_puts(ss, SS_COLS - 1 - (int)strlen(line), ROW_TABS, line, ink, 0);
    }
    /* footer: Save / Boot now as rows, message line */
    int fr = SS_ROWS - 2;
    ss_clear_row(ss, fr, 0);
    int ons = st->sel == nk, onb = st->sel == nk + 1;
    ss_puts(ss, 3, fr, st->cfg.dirty ? " Save * " : " Save   ", ons ? 0 : ink, ons ? hi : 0);
    ss_puts(ss, 22, fr, " Boot now ", onb ? 0 : ink, onb ? hi : 0);
    if (st->cfg.dirty)
        ss_puts(ss, SS_COLS - 16, fr, "unsaved changes", hi, 0);
    ss_clear_row(ss, SS_ROWS - 1, 0);
    if (st->msg[0])
        ss_puts(ss, 1, SS_ROWS - 1, st->msg, ink, 0);
    else
        ss_puts(ss, 1, SS_ROWS - 1, "countdown stopped", ink, 0);
}

/* ================================================================== */
/* the loop                                                            */
/* ================================================================== */

enum sp_result sp_run(struct ss_screen *ss, const char *cfg_path,
                      char *chosen, unsigned long chosen_len)
{
    static struct state st;
    memset(&st, 0, sizeof st);
    if (sc_load(&st.cfg, cfg_path) != 0)
        return SP_ERROR;
    load_builds(&st);
    st.secs = sc_get_int(&st.cfg, "psctrl", "countdown", 5);
    st.screen = SCR_BUILDS;

    double tick_at = now_ms();
    for (;;) {
        if (st.screen == SCR_EDIT) build_rows(&st);
        if (st.screen == SCR_BUILDS) draw_builds(ss, &st); else draw_edit(ss, &st);
        ss_flush(ss);
        sh_present(ss);

        struct si_event e = si_poll(100);
        if (e.key != SI_NONE && st.secs > 0)
            st.secs = -1;

        if (st.screen == SCR_BUILDS) {
            switch (e.key) {
            case SI_UP:   if (st.bsel > 0) st.bsel--; break;
            case SI_DOWN: if (st.bsel < st.nbuilds - 1) st.bsel++; break;
            case SI_ENTER:
                if (st.nbuilds) snprintf(st.boot, sizeof st.boot, "%s", st.builds[st.bsel]);
                goto boot;
            case SI_CHAR:
                if ((e.ch == 'e' || e.ch == 'E') && st.nbuilds) {
                    snprintf(st.sec, sizeof st.sec, "%s", st.builds[st.bsel]);
                    st.screen = SCR_EDIT; st.tab = 0; st.sel = 0; st.top = 0;
                    st.msg[0] = '\0';
                }
                break;
            case SI_ESC:
            case SI_F10:
                snprintf(chosen, chosen_len, "%s", st.boot);
                return SP_QUIT;
            default: break;
            }
        } else if (st.choosing) {
            int n = se_count(st.row[st.sel].text);
            switch (e.key) {
            case SI_UP:
                do st.choice = (st.choice + n - 1) % n;
                while (choice_is_off(st.row[st.sel].text, st.choice));
                break;
            case SI_DOWN:
                do st.choice = (st.choice + 1) % n;
                while (choice_is_off(st.row[st.sel].text, st.choice));
                break;
            case SI_ENTER: choose_accept(&st); break;
            case SI_ESC:   st.choosing = 0; break;
            case SI_F10:   st.choosing = 0; snprintf(chosen, chosen_len, "%s", st.boot); return SP_QUIT;
            default: break;
            }
        } else if (st.editing) {
            unsigned long n = strlen(st.edit);
            switch (e.key) {
            case SI_CHAR: case SI_SPACE:
                if (n + 1 < sizeof st.edit) { st.edit[n] = e.ch; st.edit[n + 1] = '\0'; }
                break;
            case SI_BACKSPACE: if (n) st.edit[n - 1] = '\0'; break;
            case SI_ENTER: commit_edit(&st); break;
            case SI_ESC:   st.editing = 0; st.msg[0] = '\0'; break;
            case SI_F10:   st.editing = 0; snprintf(chosen, chosen_len, "%s", st.boot); return SP_QUIT;
            default: break;
            }
        } else {
            switch (e.key) {
            case SI_UP:    move_sel(&st, -1); break;
            case SI_DOWN:  move_sel(&st, +1); break;
            case SI_LEFT:  st.tab = (st.tab + SE_TAB_N - 1) % SE_TAB_N; st.sel = st.top = 0; break;
            case SI_RIGHT: st.tab = (st.tab + 1) % SE_TAB_N; st.sel = st.top = 0; break;
            case SI_SPACE:
            case SI_TICK:  tick(&st, 1); break;
            case SI_ENTER:
                if (st.row[st.sel].kind == ROW_SAVE) {
                    sc_set(&st.cfg, "psctrl", "boot", st.boot);
                    snprintf(st.msg, sizeof st.msg, "%s",
                             sc_save(&st.cfg, NULL) == 0 ? "saved" : "SAVE FAILED");
                } else if (st.row[st.sel].kind == ROW_BOOT) {
                    snprintf(st.boot, sizeof st.boot, "%s", st.sec);
                    goto boot;
                } else
                    edit_row(&st);
                break;
            case SI_ESC:
                st.screen = SCR_BUILDS; st.msg[0] = '\0';
                load_builds(&st);
                break;
            case SI_F10:
                snprintf(chosen, chosen_len, "%s", st.boot);
                return SP_QUIT;
            default: break;
            }
        }

        if (st.secs > 0 && now_ms() - tick_at >= 1000.0) {
            tick_at = now_ms();
            if (--st.secs == 0) goto boot;
        }
    }

boot:
    sc_set(&st.cfg, "psctrl", "boot", st.boot);
    if (st.cfg.dirty)
        sc_save(&st.cfg, NULL);
    snprintf(chosen, chosen_len, "%s", st.boot);
    return SP_BOOT;
}
