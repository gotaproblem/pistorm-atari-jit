/*
 * setup_page.c - see setup_page.h.
 *
 * The row list is rebuilt from psctrl.cfg every redraw, so a section
 * switch or an added key needs no bookkeeping of its own:
 *
 *   [psctrl] countdown, and one row per other section = the boot picker
 *   the chosen section's keys, one row each, value editable in place
 *   Save, and Boot now
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "setup_page.h"
#include "setup_input.h"
#include "setup_cfg.h"
#include "setup_enums.h"
#include "setup_hdmi.h"

#define MAX_ROWS   96

enum row_kind { ROW_PICK, ROW_KEY, ROW_SAVE, ROW_BOOT };

struct row {
    enum row_kind kind;
    char text[SC_KEY_LEN];       /* section or key name */
    int  offtopic;               /* not this environment's, or switched off
                                  * by the key it depends on              */
    int  unset;                  /* offered, but not in the .cfg yet      */
};

struct state {
    struct sc_cfg cfg;
    int  list_rows;              /* settings rows that fit, see layout() */
    struct row    row[MAX_ROWS];
    int  nrow, nsec;
    int  sel, top;               /* selection, first visible settings row */
    char sec[SC_SEC_LEN];        /* section being edited / booted */
    int  secs;                   /* countdown, -1 once stopped */
    int  show_all;               /* Tab: list the off-topic rows too */
    int  hidden;                 /* how many are being left out       */
    int  editing;
    int  choosing;               /* picking from a list of known values */
    int  choice;
    char edit[SC_LINE_LEN];
    char msg[64];
};

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* the Pi's day, date and time for the title bar */
static void stamp_now(char *buf, unsigned long n)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, n, "%a %d %b %Y  %H:%M:%S", &tm);
}

/* ------------------------------------------------------------------ */

static void build_rows(struct state *st)
{
    char secs[8][SC_SEC_LEN];
    int ns = sc_sections(&st->cfg, secs, 8);

    st->nrow = 0;
    st->nsec = 0;
    for (int i = 0; i < ns && st->nrow < MAX_ROWS; i++) {
        if (!strcmp(secs[i], "psctrl"))
            continue;                       /* not a machine */
        st->row[st->nrow].kind = ROW_PICK;
        snprintf(st->row[st->nrow].text, SC_KEY_LEN, "%.*s",
                 SC_KEY_LEN - 1, secs[i]);
        st->nrow++;
        st->nsec++;
    }
    if (!st->sec[0] && st->nsec)
        snprintf(st->sec, sizeof st->sec, "%.*s", SC_SEC_LEN - 1, st->row[0].text);

    /* which environment this section is: an unknown name shows everything */
    int env = !strcasecmp(st->sec, "gem")    ? SE_GEM :
              !strcasecmp(st->sec, "apj-os") ? SE_APJ : SE_BOTH;

    char keys[64][SC_KEY_LEN];
    int nk = sc_keys(&st->cfg, st->sec, keys, 64);
    st->hidden = 0;
    for (int i = 0; i < nk && st->nrow < MAX_ROWS - 2; i++) {
        if (se_retired(keys[i]))
            continue;                       /* does nothing: never a row */
        int off = !(se_env(keys[i]) & env) || se_developer(keys[i]);
        const char *needs = se_needs(keys[i]);
        if (!off && needs) {
            /* hidden when the key it hangs off is absent or switched off:
             * the eight network_* rows only matter with `network` on */
            const char *v = sc_get(&st->cfg, st->sec, needs);
            off = !v || !sp_row_on(needs, v);
        }
        if (off && !st->show_all) {
            st->hidden++;
            continue;
        }
        st->row[st->nrow].kind = ROW_KEY;
        st->row[st->nrow].offtopic = off;
        st->row[st->nrow].unset = 0;
        snprintf(st->row[st->nrow].text, SC_KEY_LEN, "%.*s",
                 SC_KEY_LEN - 1, keys[i]);
        st->nrow++;
    }
    /* keys this machine could have but the file does not mention: shown
     * with their default so they can be set, and only written when they
     * are (se_known in setup_enums.c) */
    for (int i = 0; se_known(i) && st->nrow < MAX_ROWS - 2; i++) {
        const char *k = se_known(i);
        if (sc_get(&st->cfg, st->sec, k))
            continue;                       /* already listed above */
        if (!(se_env(k) & env)) {
            st->hidden++;
            continue;
        }
        const char *needs = se_needs(k);
        if (needs) {
            const char *v = sc_get(&st->cfg, st->sec, needs);
            if (!v || !sp_row_on(needs, v)) {
                st->hidden++;
                continue;
            }
        }
        st->row[st->nrow].kind = ROW_KEY;
        st->row[st->nrow].offtopic = 0;
        st->row[st->nrow].unset = 1;
        snprintf(st->row[st->nrow].text, SC_KEY_LEN, "%.*s", SC_KEY_LEN - 1, k);
        st->nrow++;
    }

    st->row[st->nrow].kind = ROW_SAVE;
    snprintf(st->row[st->nrow].text, SC_KEY_LEN, "Save");
    st->nrow++;
    st->row[st->nrow].kind = ROW_BOOT;
    snprintf(st->row[st->nrow].text, SC_KEY_LEN, "Boot now");
    st->nrow++;

    if (st->sel >= st->nrow)
        st->sel = st->nrow - 1;
}

/*
 * Layout, 25 rows:
 *   0  title bar: name, the .cfg path, and the clock
 *   1  the movement/action help, centred
 *   2  "Boot:"  then one row per section
 *      blank
 *      "[section] settings:"  with "n-m of k" on the right
 *      the settings list - whatever is left
 *      blank
 *      Save / Boot now
 *  24  message, or the countdown
 */
#define ROW_HELP      1
#define ROW_BOOT_HDR  2
#define FOOTER_ROWS   3          /* blank, actions, message */

static int list_top_row(const struct state *st)
{
    return ROW_BOOT_HDR + 1 + st->nsec + 1 + 1;
}

static void layout(struct state *st)
{
    st->list_rows = SS_ROWS - list_top_row(st) - FOOTER_ROWS;
    if (st->list_rows < 1)
        st->list_rows = 1;
}

/* first and last settings row, for scrolling */
static int first_key_row(const struct state *st) { return st->nsec; }
static int n_key_rows(const struct state *st)    { return st->nrow - st->nsec - 2; }

static void scroll_to_sel(struct state *st)
{
    int i = st->sel - first_key_row(st);
    if (i < 0) {
        st->top = 0;
        return;
    }
    if (i < st->top)
        st->top = i;
    if (i >= st->top + st->list_rows)
        st->top = i - st->list_rows + 1;
    int max = n_key_rows(st) - st->list_rows;
    if (max < 0) max = 0;
    if (st->top > max) st->top = max;
}

/* ------------------------------------------------------------------ */

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

static void draw(struct ss_screen *ss, const struct state *st)
{
    int ink = ss->planes == 1 ? 1 : 3, paper = 0, hi = ss->planes == 1 ? 1 : 2;
    char line[SC_LINE_LEN];
    char stamp[32];
    int row = 0;

    ss_clear(ss, paper);
    ss_clear_row(ss, 0, ink);
    ss_puts(ss, 2, 0, "PSCTRL PiSTorm Setup", paper, ink);
    stamp_now(stamp, sizeof stamp);
    ss_puts(ss, SS_COLS - 2 - (int)strlen(stamp), 0, stamp, paper, ink);

    /* the file being edited, between the name and the clock; a long path
     * keeps its tail, which is the part that identifies it */
    {
        int at = 2 + 20 + 2;
        int room = SS_COLS - 2 - (int)strlen(stamp) - 1 - at;
        const char *p = st->cfg.path;
        int len = (int)strlen(p);
        if (room > 4) {
            if (room > (int)sizeof line - 1)
                room = (int)sizeof line - 1;
            if (len > room)
                snprintf(line, sizeof line, "...%.*s", room - 3,
                         p + len - (room - 3));
            else
                snprintf(line, sizeof line, "%.*s", room, p);
            ss_puts(ss, at, 0, line, paper, ink);
        }
    }

    /* the help, centred, directly under the title bar */
    {
        const char *help =
            st->choosing ? "Up/Down choose   Enter accept   Esc cancel" :
            st->editing  ? "type a value   Enter accept   Esc cancel" :
            "Up/Down move   Left/Right pick   Enter toggle, choose or edit"
            "   Esc/X leave";
        int at = (SS_COLS - (int)strlen(help)) / 2;
        ss_puts(ss, at < 0 ? 0 : at, ROW_HELP, help, ink, paper);
    }

    row = ROW_BOOT_HDR;
    ss_puts(ss, 2, row++, "Boot:", ink, paper);
    for (int i = 0; i < st->nsec; i++) {
        int on = st->sel == i;
        snprintf(line, sizeof line, " %s %-30.30s",
                 strcmp(st->sec, st->row[i].text) ? " " : "*", st->row[i].text);
        ss_puts(ss, 4, row++, line, on ? paper : ink, on ? hi : paper);
    }

    row++;
    snprintf(line, sizeof line, "[%.15s] settings:", st->sec);
    ss_puts(ss, 2, row++, line, ink, paper);

    int fk = first_key_row(st), nk = n_key_rows(st);
    for (int i = 0; i < st->list_rows; i++) {
        int k = st->top + i;
        ss_clear_row(ss, row, paper);
        if (k < nk) {
            const char *key = st->row[fk + k].text;
            const char *val = sc_get(&st->cfg, st->sec, key);
            int unset = st->row[fk + k].unset;
            if (unset && !val)
                val = se_known_default(key);
            int on = st->sel == fk + k;
            int off = st->row[fk + k].offtopic;
            char vbuf[SC_LINE_LEN];
            if (on && st->choosing) {
                const char *c = se_label(key, st->choice);
                snprintf(line, sizeof line, "%-20.20s %d/%d  %-40.40s",
                         sp_row_label(key, val), st->choice + 1,
                         se_count(key), c ? c : "");
            } else if (on && st->editing)
                snprintf(line, sizeof line, "%-20.20s %-48.48s",
                         sp_row_label(key, val), st->edit);
            else
                snprintf(line, sizeof line, "%-20.20s %-38.38s%s",
                         sp_row_label(key, val),
                         sp_row_value(key, val, vbuf, sizeof vbuf),
                         unset ? "(not set)" : "");
            ss_puts(ss, 4, row, line,
                    on ? paper : (off ? hi : ink), on ? hi : paper);
        }
        row++;
    }
    if (nk > st->list_rows) {
        snprintf(line, sizeof line, "%d-%d of %d", st->top + 1,
                 st->top + st->list_rows > nk ? nk : st->top + st->list_rows, nk);
        ss_puts(ss, SS_COLS - 2 - (int)strlen(line), list_top_row(st) - 1,
                line, ink, paper);
    }

    row = SS_ROWS - 2;
    for (int i = st->nrow - 2; i < st->nrow; i++) {
        int on = st->sel == i;
        snprintf(line, sizeof line, " %-14s",
                 st->row[i].kind == ROW_SAVE ?
                     (st->cfg.dirty ? "Save *" : "Save") : "Boot now");
        ss_puts(ss, 4 + (i == st->nrow - 1 ? 20 : 0), row, line,
                on ? paper : ink, on ? hi : paper);
    }

    if (st->hidden || st->show_all) {
        if (st->show_all)
            snprintf(line, sizeof line, "Tab: hide unused");
        else
            snprintf(line, sizeof line, "Tab: show %d hidden", st->hidden);
        ss_puts(ss, SS_COLS - 2 - (int)strlen(line), SS_ROWS - 2, line,
                hi, paper);
    }

    ss_clear_row(ss, SS_ROWS - 1, paper);
    if (st->msg[0])
        snprintf(line, sizeof line, "%.48s", st->msg);
    else if (st->secs > 0)
        snprintf(line, sizeof line, "Booting %.15s in %d...", st->sec, st->secs);
    else
        snprintf(line, sizeof line, "countdown stopped");
    ss_puts(ss, 2, SS_ROWS - 1, line, ink, paper);
    if (st->cfg.dirty)
        ss_puts(ss, SS_COLS - 17, SS_ROWS - 1, "unsaved changes", hi, paper);
}

/* ------------------------------------------------------------------ */

/* flip a switch row in place - one keypress, and it works on a gamepad */
static int toggle_switch(struct state *st)
{
    const char *key = st->row[st->sel].text;
    const char *val = sc_get(&st->cfg, st->sec, key);
    if (!val)
        val = se_known_default(key);      /* offered but not in the file */
    if (!sp_row_is_switch(key, val))
        return 0;
    int on = sp_row_on(key, val);
    const char *now = on ? "disabled" : "enabled";
    char with[SC_LINE_LEN];

    /* keep the device word and the options: `kbd usb nograb` turns into
     * `kbd disabled nograb` (config_file.c reads the first word as the
     * boolean and leaves the rest), and back again. */
    if (!strcasecmp(key, "kbd")) {
        const char *rest = !strncasecmp(val, "usb", 3) ?
                           rest_after_word(val, 3) :
                           rest_after_word(val, strcspn(val, " \t"));
        snprintf(with, sizeof with, "%s%s%s", on ? "disabled" : "usb",
                 *rest ? " " : "", rest);
        sc_set(&st->cfg, st->sec, key, with);
    } else if (!strcasecmp(key, "usb") && !strncasecmp(val, "gamepad", 7)) {
        const char *rest = rest_after_word(val, 7);
        if (first_word_is_bool(rest))
            rest = rest_after_word(rest, strcspn(rest, " \t"));
        snprintf(with, sizeof with, "gamepad %s%s%s", now, *rest ? " " : "", rest);
        sc_set(&st->cfg, st->sec, key, with);
    } else {
        sc_set(&st->cfg, st->sec, key, now);
    }
    snprintf(st->msg, sizeof st->msg, "%.20s = %s",
             sp_row_label(key, val), now);
    return 1;
}

static void commit_edit(struct state *st)
{
    const char *key = st->row[st->sel].text;
    char raw[SC_LINE_LEN];
    const char *val = sp_value_from_edit(key, st->edit, raw, sizeof raw);
    if (sc_set(&st->cfg, st->sec, key, val) == 0)
        snprintf(st->msg, sizeof st->msg, "%.20s = %.35s",
                 sp_row_label(key, val), st->edit);
    else
        snprintf(st->msg, sizeof st->msg, "could not set %.30s", key);
    st->editing = 0;
}

enum sp_result sp_run(struct ss_screen *ss, const char *cfg_path,
                      char *chosen, unsigned long chosen_len)
{
    static struct state st;
    memset(&st, 0, sizeof st);

    if (sc_load(&st.cfg, cfg_path) != 0) {
        snprintf(st.msg, sizeof st.msg, "cannot read %.40s", cfg_path);
        return SP_ERROR;
    }
    const char *last = sc_get(&st.cfg, "psctrl", "boot");
    if (last && *last)
        snprintf(st.sec, sizeof st.sec, "%.*s", SC_SEC_LEN - 1, last);
    st.secs = sc_get_int(&st.cfg, "psctrl", "countdown", 5);

    build_rows(&st);
    layout(&st);
    for (int i = 0; i < st.nsec; i++)          /* start on the boot choice */
        if (!strcmp(st.row[i].text, st.sec))
            st.sel = i;

    double tick = now_ms();
    for (;;) {
        build_rows(&st);
        layout(&st);
        scroll_to_sel(&st);
        draw(ss, &st);
        ss_flush(ss);
        sh_present(ss);        /* no-op unless HDMI was opened */

        struct si_event e = si_poll(100);
        if (e.key != SI_NONE && st.secs > 0)
            st.secs = -1;                      /* any key stops the countdown */

        if (st.choosing) {
            const char *key = st.row[st.sel].text;
            int n = se_count(key);
            switch (e.key) {
            case SI_UP:    st.choice = (st.choice + n - 1) % n; break;
            case SI_DOWN:  st.choice = (st.choice + 1) % n;     break;
            case SI_ENTER: {
                const char *c = se_choice(key, st.choice);
                if (c && sc_set(&st.cfg, st.sec, key, c) == 0)
                    snprintf(st.msg, sizeof st.msg, "%.20s = %.35s",
                             sp_row_label(key, c), c);
                st.choosing = 0;
                break;
            }
            case SI_ESC:   st.choosing = 0; st.msg[0] = '\0'; break;
            case SI_F10:
                st.choosing = 0;
                snprintf(chosen, chosen_len, "%s", st.sec);
                return SP_QUIT;
            default: break;
            }
        } else if (st.editing) {
            unsigned long n = strlen(st.edit);
            switch (e.key) {
            case SI_CHAR:
            case SI_SPACE:
                if (n + 1 < sizeof st.edit) {
                    st.edit[n] = e.ch;
                    st.edit[n + 1] = '\0';
                }
                break;
            case SI_BACKSPACE:
                if (n) st.edit[n - 1] = '\0';
                break;
            case SI_ENTER: commit_edit(&st); break;
            case SI_ESC:   st.editing = 0; st.msg[0] = '\0'; break;
            /* a gamepad cannot type, so X must always get out of here */
            case SI_F10:
                st.editing = 0;
                snprintf(chosen, chosen_len, "%s", st.sec);
                return SP_QUIT;
            default: break;
            }
        } else {
            switch (e.key) {
            case SI_UP:   if (st.sel > 0) st.sel--; break;
            case SI_DOWN: if (st.sel < st.nrow - 1) st.sel++; break;
            case SI_LEFT:
            case SI_RIGHT:
                if (st.row[st.sel].kind == ROW_PICK)
                    snprintf(st.sec, sizeof st.sec, "%.*s", SC_SEC_LEN - 1,
                             st.row[st.sel].text);
                break;
            case SI_ENTER:
                switch (st.row[st.sel].kind) {
                case ROW_PICK:
                    snprintf(st.sec, sizeof st.sec, "%.*s", SC_SEC_LEN - 1,
                             st.row[st.sel].text);
                    st.msg[0] = '\0';
                    break;
                case ROW_KEY: {
                    const char *kk = st.row[st.sel].text;
                    if (st.row[st.sel].offtopic) {
                        snprintf(st.msg, sizeof st.msg,
                                 "%.20s is not used by %.10s", kk, st.sec);
                        break;
                    }
                    if (se_count(kk)) {     /* a known set of values */
                        const char *v = sc_get(&st.cfg, st.sec, kk);
                        if (!v)
                            v = se_known_default(kk);
                        int at = se_index(kk, v ? v : "");
                        st.choice = at < 0 ? 0 : at;
                        st.choosing = 1;
                        st.msg[0] = '\0';
                        break;
                    }
                    if (toggle_switch(&st))
                        break;              /* a switch flips, not types */
                    const char *k = st.row[st.sel].text;
                    const char *v = sc_get(&st.cfg, st.sec, k);
                    if (!v)
                        v = se_known_default(k);
                    char vbuf[SC_LINE_LEN];
                    snprintf(st.edit, sizeof st.edit, "%s",
                             v ? sp_row_value(k, v, vbuf, sizeof vbuf) : "");
                    st.editing = 1;
                    break;
                }
                case ROW_SAVE:
                    sc_set(&st.cfg, "psctrl", "boot", st.sec);
                    if (sc_save(&st.cfg, NULL) == 0)
                        snprintf(st.msg, sizeof st.msg, "saved");
                    else
                        snprintf(st.msg, sizeof st.msg, "SAVE FAILED");
                    break;
                case ROW_BOOT:
                    goto boot;
                }
                break;
            case SI_TAB:
                st.show_all = !st.show_all;
                st.sel = st.nsec;           /* the list just changed length */
                st.top = 0;
                snprintf(st.msg, sizeof st.msg, "%s",
                         st.show_all ? "showing every key in the section"
                                     : "showing what this machine uses");
                break;
            case SI_ESC:
            case SI_F10:
                snprintf(chosen, chosen_len, "%s", st.sec);
                return SP_QUIT;
            default: break;
            }
        }

        if (st.secs > 0 && now_ms() - tick >= 1000.0) {
            tick = now_ms();
            if (--st.secs == 0)
                goto boot;
        }
    }

boot:
    sc_set(&st.cfg, "psctrl", "boot", st.sec);
    if (st.cfg.dirty)
        sc_save(&st.cfg, NULL);
    snprintf(chosen, chosen_len, "%s", st.sec);
    return SP_BOOT;
}
