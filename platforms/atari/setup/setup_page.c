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
 *
 * The builds screen makes and removes builds: N asks for a name (it
 * becomes the section [name]) and what to copy into it - an existing
 * build, or nothing - then opens it in the editor; D deletes the build
 * under the cursor after a Y; S saves. The last build cannot be
 * deleted, so there is always something to boot.
 */
#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
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
#define MAX_FILES  128
#define MAX_FNAME  64

enum screen { SCR_BUILDS = 0, SCR_EDIT };
/* ROW_LABEL: a heading the cursor skips (acsi, fdd, hostfs groups);
 * ROW_SLOT: the nth line of a repeated key (hdd 0..7, acsi 0..7, fdd A:/B:,
 * hostfs S:) - ticked when that line exists; ROW_ADD: "add a hostfs
 * drive" - ticking it opens the editor for a new line */
enum row_kind { ROW_KEY = 0, ROW_LABEL, ROW_SLOT, ROW_ADD, ROW_SAVE, ROW_BOOT };

struct row {
    enum row_kind kind;
    char text[SC_KEY_LEN];       /* the key                                   */
    int  ord;                    /* nth occurrence (repeated keys)            */
    int  ticked;                 /* the build has the line                    */
    int  blocked;                /* parent off: greyed, cannot be ticked      */
    const char *why;             /* cpu rule fails: "needs 68020+", or NULL   */
    /* Drives tab: two columns of cells */
    int  col, line;              /* 0 left / 1 right, line within the list    */
    char label[8];               /* "0".."7", "A:", "S:", "+"                 */
    int  slot;                   /* pinned slot for a disk row; label is what
                                    the user sees and is not always a number
                                    (the floppy shows A:/B:)                  */
};

struct state {
    struct sc_cfg cfg;
    enum screen   screen;
    /* builds screen */
    char builds[MAX_BUILDS][SC_SEC_LEN];
    int  nbuilds, bsel;
    char boot[SC_SEC_LEN];       /* [psctrl] boot                              */
    int  secs;                   /* countdown, -1 once a key arrived           */
    int  naming;                 /* N: typing the new build's name into edit   */
    int  copying;                /* then choosing what to copy: 0 empty, 1.. builds */
    int  asking;                 /* D: "delete [x]? Y/N" is up                 */
    int  warned;                 /* ESC with unsaved changes: said so once     */
    /* editor */
    char sec[SC_SEC_LEN];        /* the build being edited                     */
    int  tab;
    struct row row[MAX_ROWS];
    int  nrow, sel, top, list_rows;
    int  editing, choosing, choice;
    char edit[SC_LINE_LEN];
    char msg[64];
    /* the image picker: the files in rom_path / disk_path / fdd_path */
    int  picking, pick, ptop, nfiles;
    char files[MAX_FILES][MAX_FNAME];
    char pickdir[512];
    const char *pickvar;
};

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* The clock in the title bar: 29 Apr 2026 23:45. Two switches, kept in
 * [psctrl] so they survive: `clock 12` puts the hour on a 12-hour clock
 * (11:45pm), `date mdy` puts the month first (Apr 29 2026). No seconds,
 * so the bar has room. */
static int clock_12h, date_mdy;

static void stamp_now(char *buf, unsigned long n)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, n, date_mdy ? (clock_12h ? "%b %d %Y %I:%M%P" : "%b %d %Y %H:%M")
                              : (clock_12h ? "%d %b %Y %I:%M%P" : "%d %b %Y %H:%M"), &tm);
    if (clock_12h) {                 /* %I pads with a zero: 03:05pm -> 3:05pm */
        char *h = strrchr(buf, ' ');
        if (h && h[1] == '0')
            memmove(h + 1, h + 2, strlen(h + 2) + 1);
    }
}

static void clock_load(const struct sc_cfg *c)
{
    const char *v = sc_get(c, "psctrl", "clock");
    clock_12h = v && !strcmp(v, "12");
    v = sc_get(c, "psctrl", "date");
    date_mdy = v && !strcasecmp(v, "mdy");
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

/* N: the name typed so far becomes the section [name]. The parser
 * lower-cases section names and the emulator keeps 31 characters, the
 * page 15; psctrl is the page's own block. */
static int name_ok(struct state *st)
{
    char *s = st->edit;
    if (!*s) { snprintf(st->msg, sizeof st->msg, "a name is needed"); return 0; }
    for (char *p = s; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
            snprintf(st->msg, sizeof st->msg, "'%c' cannot be in a build name", *p);
            return 0;
        }
    }
    if (!strcmp(s, "psctrl")) {
        snprintf(st->msg, sizeof st->msg, "[psctrl] is the page's own block");
        return 0;
    }
    for (int i = 0; i < st->nbuilds; i++)
        if (!strcmp(st->builds[i], s)) {
            snprintf(st->msg, sizeof st->msg, "there is already a [%.15s]", s);
            return 0;
        }
    return 1;
}

/* the new section, empty or a copy of the chosen build, then the editor */
static void new_build(struct state *st)
{
    int r = st->choice ? sc_copy_section(&st->cfg, st->builds[st->choice - 1], st->edit)
                       : sc_add_section(&st->cfg, st->edit);
    st->copying = 0;
    if (r != 0) {
        snprintf(st->msg, sizeof st->msg, "could not add [%.15s] - the file is full", st->edit);
        return;
    }
    snprintf(st->sec, sizeof st->sec, "%.15s", st->edit);
    load_builds(st);
    st->screen = SCR_EDIT; st->tab = 0; st->sel = 0; st->top = 0;
    snprintf(st->msg, sizeof st->msg, "new build [%s] - Save keeps it", st->sec);
}

static void delete_build(struct state *st)
{
    char name[SC_SEC_LEN];
    snprintf(name, sizeof name, "%s", st->builds[st->bsel]);
    st->asking = 0;
    if (sc_del_section(&st->cfg, name) != 0)
        return;
    if (!strcasecmp(st->boot, name))
        sc_set(&st->cfg, "psctrl", "boot", NULL);
    load_builds(st);
    if (st->bsel >= st->nbuilds && st->nbuilds)
        st->bsel = st->nbuilds - 1;
    snprintf(st->msg, sizeof st->msg, "deleted [%s] - S saves it", name);
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
    if (r->kind == ROW_LABEL)
        return 0;
    if (r->kind == ROW_KEY || r->kind == ROW_SLOT || r->kind == ROW_ADD)
        return !r->blocked && !r->why;
    return 1;
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

/*
 * The Drives tab. Two columns, walked top to bottom, left column first:
 *
 *   [x] ide   (disk_path)              acsi  (disk_path)
 *       [x] 0  apj-os-dev.img              [ ] 0  empty
 *       ...                                ...
 *   ----------------------------------------------------------------
 *   fdd   (fdd_path)                   hostfs
 *       [x] A: 720k.st                     [x] S: /home/pistorm/atari-share
 *                                          [ ] +  add a drive
 *
 * config_file.c numbers hdd lines 0..7 and acsi lines 0..7 in the order
 * they appear (an ID prefix on acsi would defeat disk_path), so slot n
 * is the nth line; ticking an empty slot appends the next line. The
 * floppy has two slots of its own - drives A: and B:, written "0:" and
 * "1:" in the file - and hostfs takes a letter and a path per line.
 */
#define DRIVE_SLOTS 8
#define FDD_SLOTS   2           /* the ST has two floppy drives, A: and B: */

/* "3:image" -> 3 and "image"; no prefix -> -1 and the value as it is
 * (the same rule as config_file.c's slot_prefix) */
static const char *slot_of(const char *val, int *slot)
{
    *slot = -1;
    if (!val) return "";
    while (*val == ' ' || *val == '\t') val++;
    if (val[0] >= '0' && val[0] <= '7' && (val[1] == ':' || val[1] == ' ' || val[1] == '\t')) {
        *slot = val[0] - '0';
        val += 2;
        while (*val == ' ' || *val == '\t') val++;
    }
    return val;
}

/* which line sits in each slot of a repeated key: pinned lines first,
 * then the unpinned ones the way the emulator numbers them (hdd: next
 * slot in file order; acsi: lowest free ID). -1 = empty. */
/* `acsi enabled` / `acsi disabled` is the group's switch, not an image */
static int is_switch_line(const char *key, const char *v)
{
    return !strcasecmp(key, "acsi") && v && sp_is_switch(v);
}

/* the line index of the acsi switch, or -1 */
static int acsi_switch_line(const struct state *st)
{
    int n = sc_count(&st->cfg, st->sec, "acsi");
    for (int i = 0; i < n; i++)
        if (is_switch_line("acsi", sc_get_n(&st->cfg, st->sec, "acsi", i)))
            return i;
    return -1;
}

static void slot_map(const struct state *st, const char *key, int map[DRIVE_SLOTS])
{
    int n = sc_count(&st->cfg, st->sec, key), next = 0;
    for (int i = 0; i < DRIVE_SLOTS; i++) map[i] = -1;
    for (int i = 0; i < n; i++) {
        const char *v = sc_get_n(&st->cfg, st->sec, key, i);
        if (is_switch_line(key, v)) continue;
        int sl; slot_of(v, &sl);
        if (sl >= 0 && map[sl] < 0) map[sl] = i;
    }
    for (int i = 0; i < n; i++) {
        const char *v = sc_get_n(&st->cfg, st->sec, key, i);
        if (is_switch_line(key, v)) continue;
        int sl; slot_of(v, &sl);
        if (sl >= 0) continue;
        if (!strcasecmp(key, "hdd")) {
            if (next < DRIVE_SLOTS) map[next++] = i;
        } else {
            for (int k = 0; k < DRIVE_SLOTS; k++)
                if (map[k] < 0) { map[k] = i; break; }
        }
    }
}

/* rewrite every line of the key in its pinned "n:image" form, so that
 * removing or editing one slot never renumbers the others */
static void pin_all(struct state *st, const char *key)
{
    int map[DRIVE_SLOTS];
    slot_map(st, key, map);
    for (int sl = 0; sl < DRIVE_SLOTS; sl++) {
        if (map[sl] < 0) continue;
        const char *v = sc_get_n(&st->cfg, st->sec, key, map[sl]);
        int have; const char *img = slot_of(v, &have);
        if (have == sl) continue;
        char pinned[SC_LINE_LEN];
        snprintf(pinned, sizeof pinned, "%d:%.*s", sl & 7, SC_LINE_LEN - 16, img);
        sc_set_n(&st->cfg, st->sec, key, map[sl], pinned);
    }
}

static struct row *drow(struct state *st, enum row_kind kind, const char *key,
                        int ord, int col, int line, const char *label)
{
    struct row *r = &st->row[st->nrow++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    snprintf(r->text, sizeof r->text, "%.*s", SC_KEY_LEN - 1, key ? key : "");
    r->ord = ord; r->col = col; r->line = line;
    snprintf(r->label, sizeof r->label, "%.7s", label ? label : "");
    return r;
}

static void build_drive_rows(struct state *st)
{
    int hmap[DRIVE_SLOTS], amap[DRIVE_SLOTS], fmap[DRIVE_SLOTS];
    slot_map(st, "hdd", hmap);
    slot_map(st, "acsi", amap);
    slot_map(st, "fdd", fmap);
    int nf = sc_count(&st->cfg, st->sec, "fdd");
    int ns = sc_count(&st->cfg, st->sec, "hostfs");
    const char *ide = sc_get(&st->cfg, st->sec, "ide");
    int ide_on = ide && sp_row_on("ide", ide);
    int asw = acsi_switch_line(st);
    const char *acsi = asw >= 0 ? sc_get_n(&st->cfg, st->sec, "acsi", asw) : NULL;
    int acsi_on = acsi && sp_row_on("acsi", acsi);
    char num[4];
    struct row *r;

    st->nrow = 0;
    /* left column: ide + hdd 0..7, then fdd A: and B: */
    r = drow(st, ROW_KEY, "ide", 0, 0, 0, "");
    r->ticked = ide_on;
    for (int i = 0; i < DRIVE_SLOTS; i++) {
        snprintf(num, sizeof num, "%d", i);
        r = drow(st, ROW_SLOT, "hdd", hmap[i], 0, 1 + i, num);
        r->ticked = hmap[i] >= 0;
        r->blocked = !ide_on;
        r->slot = i;
    }
    drow(st, ROW_LABEL, "fdd", 0, 0, DRIVE_SLOTS + 2, "");
    /* Drive B had no row at all, which is why a second image could be
     * mounted live from PSCTRL but never from the boot settings. Both
     * drives are slots of the same repeated key now, pinned "0:"/"1:"
     * in the file and shown as A: and B:. */
    for (int i = 0; i < FDD_SLOTS; i++) {
        r = drow(st, ROW_SLOT, "fdd", fmap[i], 0, DRIVE_SLOTS + 3 + i,
                 i ? "B:" : "A:");
        r->ticked = fmap[i] >= 0;
        r->slot = i;
    }
    (void)nf;
    /* right column: acsi 0..7, then hostfs lines and an add row */
    r = drow(st, ROW_KEY, "acsi", asw, 1, 0, "");
    r->ticked = acsi_on;
    for (int i = 0; i < DRIVE_SLOTS; i++) {
        snprintf(num, sizeof num, "%d", i);
        r = drow(st, ROW_SLOT, "acsi", amap[i], 1, 1 + i, num);
        r->ticked = amap[i] >= 0;
        r->blocked = !acsi_on;
        r->slot = i;
    }
    drow(st, ROW_LABEL, "hostfs", 0, 1, DRIVE_SLOTS + 2, "");
    int line = DRIVE_SLOTS + 3, room = SS_ROWS - ROW_LIST - FOOTER;
    for (int i = 0; i < ns && line < room; i++, line++) {
        const char *v = sc_get_n(&st->cfg, st->sec, "hostfs", i);
        char lab[8];
        snprintf(lab, sizeof lab, "%c:", v && *v ? (char)toupper((unsigned char)*v) : '?');
        r = drow(st, ROW_SLOT, "hostfs", i, 1, line, lab);
        r->ticked = 1;
    }
    if (line < room)
        drow(st, ROW_ADD, "hostfs", ns, 1, line, "+");
    st->row[st->nrow].kind = ROW_SAVE; st->row[st->nrow++].text[0] = '\0';
    st->row[st->nrow].kind = ROW_BOOT; st->row[st->nrow++].text[0] = '\0';
    if (st->sel >= st->nrow) st->sel = st->nrow - 1;
    st->list_rows = room;
    st->top = 0;
    settle(st);
}

static void build_rows(struct state *st)
{
    const char *cpu = sc_get(&st->cfg, st->sec, "cpu");
    if (st->tab == SE_TAB_DRIVES) {
        build_drive_rows(st);
        return;
    }
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
        r->why = se_env_rule(k, st->sec);
        if (!r->why) r->why = se_cpu_rule(k, cpu);
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
    if (v && se_kind(k) == SE_K_INT)          /* 0 is a number, not "off" */
        return v;
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
/* ------------------------------------------------------------------ */
/* the image picker                                                    */
/* ------------------------------------------------------------------ */

/* which [psctrl] path a key's files live under, or NULL */
static const char *image_base_key(const char *key)
{
    if (!strcasecmp(key, "hdd") || !strcasecmp(key, "acsi")) return "disk_path";
    if (!strcasecmp(key, "fdd"))                              return "fdd_path";
    if (!strcasecmp(key, "rom") || !strcasecmp(key, "stbox_tos")) return "rom_path";
    return NULL;
}

/* ~ is the invoking user's home, as config_file.c reads it */
static const char *page_home(void)
{
    static char home[512];
    if (home[0]) return home;
    const char *u = getenv("SUDO_USER");
    const struct passwd *pw = (u && *u) ? getpwnam(u) : NULL;
    if (!pw) pw = getpwuid(getuid());
    const char *h = (pw && pw->pw_dir && *pw->pw_dir) ? pw->pw_dir : getenv("HOME");
    snprintf(home, sizeof home, "%s", h ? h : ".");
    return home;
}

static int name_cmp(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

/* read the directory: regular files, no dotfiles, sorted; 0 = nothing
 * to offer (no path set, no such directory, or empty) */
static int open_picker(struct state *st, const char *key, const char *current)
{
    const char *var = image_base_key(key);
    const char *base = var ? sc_get(&st->cfg, "psctrl", var) : NULL;
    if (!base || !*base)
        return 0;
    if (base[0] == '~' && (base[1] == '/' || !base[1]))
        snprintf(st->pickdir, sizeof st->pickdir, "%s%s", page_home(), base + 1);
    else
        snprintf(st->pickdir, sizeof st->pickdir, "%s", base);
    DIR *d = opendir(st->pickdir);
    if (!d) {
        snprintf(st->msg, sizeof st->msg, "%s: cannot read %.40s", var, st->pickdir);
        return 0;
    }
    st->nfiles = 0;
    struct dirent *e;
    while ((e = readdir(d)) && st->nfiles < MAX_FILES) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= MAX_FNAME)
            continue;
        char full[1024]; struct stat sb;
        snprintf(full, sizeof full, "%s/%s", st->pickdir, e->d_name);
        if (stat(full, &sb) != 0 || !S_ISREG(sb.st_mode))
            continue;
        snprintf(st->files[st->nfiles++], MAX_FNAME, "%.*s", MAX_FNAME - 1, e->d_name);
    }
    closedir(d);
    if (!st->nfiles) {
        snprintf(st->msg, sizeof st->msg, "no files in %.50s", st->pickdir);
        return 0;
    }
    qsort(st->files, (unsigned long)st->nfiles, MAX_FNAME, name_cmp);
    st->pickvar = var;
    st->pick = 0;                             /* 0 = type a name */
    for (int i = 0; current && i < st->nfiles; i++)
        if (!strcasecmp(st->files[i], current)) { st->pick = i + 1; break; }
    st->ptop = 0;
    st->picking = 1;
    snprintf(st->msg, sizeof st->msg, "%d file%s", st->nfiles, st->nfiles == 1 ? "" : "s");
    return 1;
}

/* Keys whose lines carry a pinned "n:image" slot number, so that editing
 * or removing one never renumbers the others. The floppy is one of them:
 * its two slots are drive A and drive B. */
static int is_disk(const char *key)
{
    return !strcasecmp(key, "hdd") || !strcasecmp(key, "acsi") ||
           !strcasecmp(key, "fdd");
}

/* after pin_all the line numbers may have moved: find this slot's line */
static void slot_line(struct state *st, struct row *r)
{
    int map[DRIVE_SLOTS], sl = r->slot;   /* not atoi(label): the floppy's
                                             labels are "A:" and "B:"      */
    slot_map(st, r->text, map);
    r->ord = sl >= 0 && sl < DRIVE_SLOTS ? map[sl] : -1;
}

static void tick(struct state *st, int open_editor_for_text)
{
    struct row *r = &st->row[st->sel];
    if (r->kind == ROW_SLOT || r->kind == ROW_ADD) {
        if (r->blocked) {
            snprintf(st->msg, sizeof st->msg, "%.10s %s needs %.10s ticked", r->text, r->label,
                     !strcasecmp(r->text, "hdd") ? "ide" : r->text);
            return;
        }
        if (r->ticked) {
            if (is_disk(r->text)) {           /* the others keep their numbers */
                pin_all(st, r->text);
                slot_line(st, r);
            }
            sc_set_n(&st->cfg, st->sec, r->text, r->ord, NULL);
            snprintf(st->msg, sizeof st->msg, "%.10s %s removed", r->text, r->label);
            return;
        }
        if (open_editor_for_text) {           /* a new line needs its value */
            if (open_picker(st, r->text, NULL))
                return;
            st->edit[0] = '\0';
            st->editing = 1;
            if (!strcasecmp(r->text, "hostfs"))
                snprintf(st->msg, sizeof st->msg, "type letter and path, e.g. S /home/pistorm/atari-share");
            else
                snprintf(st->msg, sizeof st->msg, "type the image for %.10s %s", r->text, r->label);
        }
        return;
    }
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
    int ord = r->ord < 0 ? sc_count(&st->cfg, st->sec, r->text) : r->ord;
    if (r->ticked) {
        /* off = out of the build; the two the emulator has on by default
         * must say so */
        sc_set_n(&st->cfg, st->sec, r->text, ord, se_off_value(r->text));
        snprintf(st->msg, sizeof st->msg, "%.20s %s", r->text,
                 se_kind(r->text) == SE_K_SWITCH ? "off" : "removed from the build");
        return;
    }
    const char *tv = se_tick_value(r->text);
    if (se_kind(r->text) == SE_K_TEXT && !*tv) {
        if (open_editor_for_text) {           /* needs a value first */
            if (open_picker(st, r->text, NULL))
                return;
            st->edit[0] = '\0';
            st->editing = 1;
            snprintf(st->msg, sizeof st->msg, "type a value for %.20s", r->text);
        }
        return;
    }
    sc_set_n(&st->cfg, st->sec, r->text, ord, tv);
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
        if (r->kind != ROW_ADD)
            sc_set_n(&st->cfg, st->sec, r->text, r->ord, NULL);
        snprintf(st->msg, sizeof st->msg, "%.20s left empty", r->text);
        return;
    }
    if (!strcasecmp(r->text, "hostfs") && !hostfs_drive(val)) {
        st->editing = 1;                      /* keep what was typed */
        snprintf(st->msg, sizeof st->msg, "hostfs needs a drive letter first: S /path");
        return;
    }
    char pinned[SC_LINE_LEN];
    if (r->kind == ROW_SLOT && is_disk(r->text)) {
        int typed_slot; const char *img = slot_of(val, &typed_slot);
        snprintf(pinned, sizeof pinned, "%d:%.*s", r->slot & 7, SC_LINE_LEN - 16, img);
        val = pinned;
        pin_all(st, r->text);
        slot_line(st, r);
    }
    int ord = r->ord < 0 ? sc_count(&st->cfg, st->sec, r->text) : r->ord;
    r->ord = ord;
    if (se_kind(r->text) == SE_K_INT) {
        char *end; long lo, hi, n = strtol(val, &end, 0);
        if (end == val || *end) {
            snprintf(st->msg, sizeof st->msg, "%.20s must be a number", r->text);
            return;
        }
        if (se_int_range(r->text, &lo, &hi) && (n < lo || n > hi)) {
            snprintf(st->msg, sizeof st->msg, "%.20s must be %d to %d", r->text, (int)lo, (int)hi);
            return;
        }
    }
    if (sc_set_n(&st->cfg, st->sec, r->text, ord, val) == 0)
        snprintf(st->msg, sizeof st->msg, "%.20s = %.30s", r->text, val);
}

/* Enter: a list opens the chooser, a switch flips, text/int opens the editor */
static void edit_row(struct state *st)
{
    struct row *r = &st->row[st->sel];
    if (r->kind == ROW_SLOT || r->kind == ROW_ADD) {
        if (!r->ticked) { tick(st, 1); return; }
        const char *v = sc_get_n(&st->cfg, st->sec, r->text, r->ord);
        char vbuf[SC_LINE_LEN];
        int sl;
        snprintf(st->edit, sizeof st->edit, "%s",
                 !v ? "" : is_disk(r->text) ? slot_of(v, &sl) :
                 sp_row_value(r->text, v, vbuf, sizeof vbuf));
        if (open_picker(st, r->text, st->edit))
            return;
        st->editing = 1;
        return;
    }
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
    if (open_picker(st, k, st->edit))
        return;
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
    static const struct help h[] = { { "Enter", "boot" }, { "E", "edit" },
        { "N", "new" }, { "D", "delete" }, { "S", "save" }, { "ESC/pad X", "leave" },
        { "F12", "dump" } };
    static const struct help hn[] = { { "Enter", "next" }, { "ESC", "cancel" } };
    static const struct help hc[] = { { "Up/Down", "move" }, { "Enter", "create" },
        { "ESC", "cancel" } };
    ss_clear(ss, 0);
    draw_bar(ss, NULL);
    const struct help hs[] = { { "C", clock_12h ? "12-hour clock" : "24-hour clock" },
        { "M", date_mdy ? "month first" : "day first" } };
    if (st->naming)       draw_help(ss, 1, hn, 2);
    else if (st->copying) draw_help(ss, 1, hc, 3);
    else                { draw_help(ss, 1, h, 7); draw_help(ss, 2, hs, 2); }
    ss_puts(ss, 1, 3, "Builds:", ink, 0);
    for (int i = 0; i < st->nbuilds; i++) {
        int on = i == st->bsel && !st->copying;
        snprintf(line, sizeof line, " %s %-16.16s%s",
                 strcasecmp(st->builds[i], st->boot) ? " " : "*", st->builds[i],
                 strcasecmp(st->builds[i], st->boot) ? "" : "  (boots on countdown)");
        ss_puts(ss, 3, 4 + i, line, on ? 0 : ink, on ? hi : 0);
    }
    if (st->copying) {
        /* the new build's name is in edit; what goes into it is the choice */
        snprintf(line, sizeof line, "New build [%.15s] - copy from:", st->edit);
        ss_puts(ss, 44, 3, line, ink, 0);
        for (int i = 0; i <= st->nbuilds; i++) {
            int on = i == st->choice;
            snprintf(line, sizeof line, " %-30.30s",
                     i ? st->builds[i - 1] : "<empty - every key unticked>");
            ss_puts(ss, 46, 4 + i, line, on ? 0 : ink, on ? hi : 0);
        }
        ss_puts(ss, 44, 6 + st->nbuilds, "The copy opens in the editor.", hi, 0);
        ss_puts(ss, 44, 7 + st->nbuilds, "Nothing is written until you Save.", hi, 0);
    }
    snprintf(line, sizeof line, "rom_path %.16s  disk_path %.16s  fdd_path %.16s",
             sc_get(&st->cfg, "psctrl", "rom_path") ? sc_get(&st->cfg, "psctrl", "rom_path") : "-",
             sc_get(&st->cfg, "psctrl", "disk_path") ? sc_get(&st->cfg, "psctrl", "disk_path") : "-",
             sc_get(&st->cfg, "psctrl", "fdd_path") ? sc_get(&st->cfg, "psctrl", "fdd_path") : "-");
    ss_puts(ss, 1, SS_ROWS - 3, line, hi, 0);
    ss_clear_row(ss, SS_ROWS - 2, 0);
    ss_clear_row(ss, SS_ROWS - 1, 0);
    if (st->naming) {
        ss_puts(ss, 1, SS_ROWS - 2, "becomes the section [name]: letters, digits, - and _, 15 at most",
                hi, 0);
        snprintf(line, sizeof line, "New build name: %.15s_", st->edit);
        ss_puts(ss, 1, SS_ROWS - 1, line, ink, 0);
    } else if (st->msg[0])
        ss_puts(ss, 1, SS_ROWS - 1, st->msg, ink, 0);
    else if (st->secs > 0) {
        snprintf(line, sizeof line, "Booting %.15s in %d...", st->boot, st->secs);
        ss_puts(ss, 1, SS_ROWS - 1, line, ink, 0);
    } else
        ss_puts(ss, 1, SS_ROWS - 1, "countdown stopped", ink, 0);
}

/* the Drives tab: cells in two columns; the value being typed goes on
 * its own line under the list, full width */
#define DCOL_W 39
static void draw_drives(struct ss_screen *ss, const struct state *st)
{
    int ink = c_ink(ss), hi = c_hi(ss);
    char cell[SC_LINE_LEN], vbuf[SC_LINE_LEN];
    for (int i = 0; i < st->list_rows; i++)
        ss_clear_row(ss, ROW_LIST + i, 0);
    /* the rule between the disks and the floppy / hostfs groups */
    memset(cell, '-', SS_COLS - 2); cell[SS_COLS - 2] = '\0';
    ss_puts(ss, 1, ROW_LIST + DRIVE_SLOTS + 1, cell, hi, 0);

    for (int k = 0; k < st->nrow - 2; k++) {
        const struct row *r = &st->row[k];
        int on = st->sel == k, grey = 0;
        const char *v = sc_get_n(&st->cfg, st->sec, r->text, r->ord);
        switch (r->kind) {
        case ROW_KEY:                         /* ide, acsi */
            snprintf(cell, sizeof cell, "%s %-6.6s(disk_path)", r->ticked ? "[x]" : "[ ]", r->text);
            grey = !r->ticked;
            break;
        case ROW_LABEL:
            snprintf(cell, sizeof cell, "    %-6.6s%s", r->text,
                     !strcasecmp(r->text, "fdd")  ? "(fdd_path)" : "");
            break;
        case ROW_SLOT:
        case ROW_ADD: {
            const char *val = "empty";
            if (r->kind == ROW_ADD)
                val = "add a drive";
            else if (r->ticked && v) {
                int sl;
                if (is_disk(r->text))
                    val = slot_of(v, &sl);        /* the slot is the label */
                else {
                    val = sp_row_value(r->text, v, vbuf, sizeof vbuf);
                    if (!strcasecmp(r->text, "hostfs") && val[1] == ':')
                        val += 2 + (val[2] == ' ');  /* the letter is the label */
                }
            }
            snprintf(cell, sizeof cell, "    %s %-3.3s%.27s",
                     (r->blocked || r->why) ? "[-]" : r->ticked ? "[x]" : "[ ]",
                     r->label, val);
            grey = r->blocked || r->why || !r->ticked;
            break;
        }
        default: continue;
        }
        unsigned long len = strlen(cell);
        while (len < DCOL_W - 1) cell[len++] = ' ';
        cell[len] = '\0';
        ss_puts(ss, r->col ? 1 + DCOL_W + 1 : 1, ROW_LIST + r->line, cell,
                on ? 0 : (grey ? hi : ink), on ? hi : 0);
    }
    if (st->editing) {
        const struct row *r = &st->row[st->sel];
        snprintf(cell, sizeof cell, " %.10s %-3.3s %.60s_", r->text, r->label, st->edit);
        ss_puts(ss, 1, SS_ROWS - FOOTER, cell, ink, 0);
    }
}

/* the picker: the directory on the first line, then the files, with
 * "type a name" first for anything not listed */
static void draw_picker(struct ss_screen *ss, const struct state *st)
{
    int ink = c_ink(ss), hi = c_hi(ss);
    char line[SC_LINE_LEN];
    const struct row *r = &st->row[st->sel];
    for (int i = 0; i < st->list_rows; i++)
        ss_clear_row(ss, ROW_LIST + i, 0);
    snprintf(line, sizeof line, " %.10s %-3.3s from %s %.50s", r->text, r->label,
             st->pickvar ? st->pickvar : "", st->pickdir);
    ss_puts(ss, 1, ROW_LIST, line, hi, 0);
    int rows = st->list_rows - 1, n = st->nfiles + 1;
    for (int i = 0; i < rows; i++) {
        int k = st->ptop + i;
        if (k >= n) break;
        int on = k == st->pick;
        snprintf(line, sizeof line, "   %-72.72s", k == 0 ? "(type a name)" : st->files[k - 1]);
        ss_puts(ss, 1, ROW_LIST + 1 + i, line, on ? 0 : ink, on ? hi : 0);
    }
    if (n > rows) {
        snprintf(line, sizeof line, "%d-%d of %d", st->ptop + 1,
                 st->ptop + rows > n ? n : st->ptop + rows, n);
        ss_puts(ss, SS_COLS - 1 - (int)strlen(line), ROW_TABS, line, ink, 0);
    }
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
    static const struct help hp[] = { { "Up/Down", "choose" }, { "Enter", "pick" },
        { "ESC", "cancel" } };
    if (st->editing)       draw_help(ss, 1, he, 3);
    else if (st->choosing) draw_help(ss, 1, hc, 3);
    else if (st->picking)  draw_help(ss, 1, hp, 3);
    else                   draw_help(ss, 1, h, 5);
    draw_tabs(ss, st->tab);

    int nk = st->nrow - 2;
    if (st->picking) {
        draw_picker(ss, st);
        goto footer;
    }
    if (st->tab == SE_TAB_DRIVES) {
        draw_drives(ss, st);
        goto footer;
    }
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
            char whybuf[48];
            long lo, hi;
            if (r->why) {
                snprintf(whybuf, sizeof whybuf, "(%.28s)", r->why);
                tail = whybuf;
            } else if (r->blocked) {
                snprintf(whybuf, sizeof whybuf, "(needs %.20s)",
                         se_needs(r->text) ? se_needs(r->text) : "parent");
                tail = whybuf;
            } else if (se_kind(r->text) == SE_K_INT) {
                /* a typed number shows its default and range */
                if (se_int_range(r->text, &lo, &hi))
                    snprintf(whybuf, sizeof whybuf, "default %.10s  %d to %d",
                             se_tick_value(r->text), (int)lo, (int)hi);
                else
                    snprintf(whybuf, sizeof whybuf, "default %.10s", se_tick_value(r->text));
                tail = whybuf;
            }
            /* [-] = cannot be ticked here; the cursor skips it */
            /* a row that cannot be used here says "empty" unless it has a
             * value; a tail (reason, default and range) follows the value
             * after a short column rather than sitting at the far right */
            if ((r->why || r->blocked) && !r->ticked)
                val = "empty";
            if (*tail)
                snprintf(line, sizeof line, " %s %-20.20s %-14.36s  %s",
                         (r->why || r->blocked) ? "[-]" : r->ticked ? "[x]" : "[ ]",
                         sp_row_label(r->text, sc_get_n(&st->cfg, st->sec, r->text, r->ord)),
                         val, tail);
            else
                snprintf(line, sizeof line, " %s %-20.20s %-36.36s",
                         (r->why || r->blocked) ? "[-]" : r->ticked ? "[x]" : "[ ]",
                         sp_row_label(r->text, sc_get_n(&st->cfg, st->sec, r->text, r->ord)),
                         val);
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
footer:;
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

/* F12 / Help: hand the page to the emulator's screendump writer */
static sp_dump_fn dumper;
void sp_set_dumper(sp_dump_fn fn) { dumper = fn; }

static void snap(struct ss_screen *ss, struct state *st)
{
    static uint32_t px[640 * 400];
    char path[600];
    if (!dumper) {
        snprintf(st->msg, sizeof st->msg, "no screendump writer in this build");
        return;
    }
    ss_export_xrgb(ss, px);
    if (dumper(px, 640, 400, path, sizeof path) != 0) {
        snprintf(st->msg, sizeof st->msg, "screen dump failed");
        return;
    }
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    snprintf(st->msg, sizeof st->msg, "saved %.55s", name);
    printf("[SETUP] screen dump %s\n", path);
}

enum sp_result sp_run(struct ss_screen *ss, const char *cfg_path,
                      char *chosen, unsigned long chosen_len)
{
    static struct state st;
    memset(&st, 0, sizeof st);
    if (sc_load(&st.cfg, cfg_path) != 0)
        return SP_ERROR;
    load_builds(&st);
    clock_load(&st.cfg);
    st.secs = sc_get_int(&st.cfg, "psctrl", "countdown", 5);
    st.screen = SCR_BUILDS;

    /* PISTORM_SETUP_TRACE=1: every key and what it did, on the console -
     * the way to see where a page that stops responding got to */
    int trace = getenv("PISTORM_SETUP_TRACE") != NULL;
    double tick_at = now_ms();
    for (;;) {
        if (st.screen == SCR_EDIT) build_rows(&st);
        if (trace) { printf("[SETUP] draw: screen %d tab %d sel %d/%d\n", st.screen, st.tab, st.sel, st.nrow); fflush(stdout); }
        if (st.screen == SCR_BUILDS) draw_builds(ss, &st); else draw_edit(ss, &st);
        ss_flush(ss);
        sh_present(ss);

        struct si_event e = si_poll(100);
        if (trace && e.key != SI_NONE) {
            char kn[32];
            printf("[SETUP] key %s on row '%s' ord %d -> ", si_key_name(&e, kn, sizeof kn),
                   st.screen == SCR_EDIT ? st.row[st.sel].text : "-", st.row[st.sel].ord);
            fflush(stdout);
        }
        if (e.key != SI_NONE && st.secs > 0)
            st.secs = -1;

        if (e.key == SI_SNAP) {
            snap(ss, &st);
            e.key = SI_NONE;
        }
        if (st.screen == SCR_BUILDS && st.naming) {
            unsigned long n = strlen(st.edit);
            switch (e.key) {
            case SI_CHAR:
                if (n + 1 < SC_SEC_LEN) { st.edit[n] = e.ch; st.edit[n + 1] = '\0'; }
                break;
            case SI_BACKSPACE: if (n) st.edit[n - 1] = '\0'; break;
            case SI_ENTER:
                if (name_ok(&st)) { st.naming = 0; st.copying = 1; st.choice = 0; st.msg[0] = '\0'; }
                break;
            case SI_ESC: st.naming = 0; st.msg[0] = '\0'; break;
            default: break;
            }
        } else if (st.screen == SCR_BUILDS && st.copying) {
            switch (e.key) {
            case SI_UP:    if (st.choice > 0) st.choice--; break;
            case SI_DOWN:  if (st.choice < st.nbuilds) st.choice++; break;
            case SI_ENTER: new_build(&st); break;
            case SI_ESC:   st.copying = 0; st.msg[0] = '\0'; break;
            default: break;
            }
        } else if (st.screen == SCR_BUILDS && st.asking) {
            st.asking = 0; st.msg[0] = '\0';
            if (e.key == SI_CHAR && (e.ch == 'y' || e.ch == 'Y'))
                delete_build(&st);
            else if (e.key != SI_NONE)
                snprintf(st.msg, sizeof st.msg, "not deleted");
        } else if (st.screen == SCR_BUILDS) {
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
                } else if (e.ch == 'n' || e.ch == 'N') {
                    if (st.nbuilds >= MAX_BUILDS)
                        snprintf(st.msg, sizeof st.msg, "%d builds is as many as the page lists", MAX_BUILDS);
                    else { st.naming = 1; st.edit[0] = '\0'; st.msg[0] = '\0'; }
                } else if ((e.ch == 'd' || e.ch == 'D') && st.nbuilds) {
                    if (st.nbuilds == 1)
                        snprintf(st.msg, sizeof st.msg, "the last build stays - make another first");
                    else {
                        st.asking = 1;
                        snprintf(st.msg, sizeof st.msg, "delete [%s] and everything in it? Y/N", st.builds[st.bsel]);
                    }
                } else if (e.ch == 'c' || e.ch == 'C') {
                    clock_12h = !clock_12h;
                    sc_set(&st.cfg, "psctrl", "clock", clock_12h ? "12" : "24");
                } else if (e.ch == 'm' || e.ch == 'M') {
                    date_mdy = !date_mdy;
                    sc_set(&st.cfg, "psctrl", "date", date_mdy ? "mdy" : "dmy");
                } else if (e.ch == 's' || e.ch == 'S') {
                    sc_set(&st.cfg, "psctrl", "boot", st.boot);
                    if (sc_save(&st.cfg, NULL) == 0) {
                        snprintf(st.msg, sizeof st.msg, "saved");
                        st.warned = 0;
                    } else
                        snprintf(st.msg, sizeof st.msg, "SAVE FAILED - %.48s", sc_save_error(&st.cfg));
                }
                break;
            case SI_ESC:
            case SI_F10:
                if (st.cfg.dirty && !st.warned && e.key == SI_ESC) {
                    st.warned = 1;
                    snprintf(st.msg, sizeof st.msg, "unsaved changes - S saves, ESC again leaves them");
                    break;
                }
                snprintf(chosen, chosen_len, "%s", st.boot);
                return SP_QUIT;
            default: break;
            }
        } else if (st.picking) {
            int n = st.nfiles + 1, rows = st.list_rows - 1;
            switch (e.key) {
            case SI_UP:   if (st.pick > 0) st.pick--; break;
            case SI_DOWN: if (st.pick < n - 1) st.pick++; break;
            case SI_ENTER:
                st.picking = 0;
                if (st.pick == 0) {           /* type it instead */
                    st.editing = 1;
                    snprintf(st.msg, sizeof st.msg, "type a name for %.20s", st.row[st.sel].text);
                } else {
                    snprintf(st.edit, sizeof st.edit, "%s", st.files[st.pick - 1]);
                    commit_edit(&st);
                }
                break;
            case SI_ESC:  st.picking = 0; st.msg[0] = '\0'; break;
            case SI_F10:  st.picking = 0; snprintf(chosen, chosen_len, "%s", st.boot); return SP_QUIT;
            default: break;
            }
            if (st.pick < st.ptop) st.ptop = st.pick;
            if (st.pick >= st.ptop + rows) st.ptop = st.pick - rows + 1;
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
                    if (sc_save(&st.cfg, NULL) == 0) {
                        snprintf(st.msg, sizeof st.msg, "saved");
                        st.warned = 0;
                    } else
                        snprintf(st.msg, sizeof st.msg, "SAVE FAILED - %.48s",
                                 sc_save_error(&st.cfg));
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

        if (trace && e.key != SI_NONE) { printf("msg '%s' lines %d\n", st.msg, st.cfg.n); fflush(stdout); }
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
