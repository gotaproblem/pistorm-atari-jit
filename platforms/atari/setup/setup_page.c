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
#include <time.h>

#include "setup_page.h"
#include "setup_input.h"
#include "setup_cfg.h"

#define MAX_ROWS   96
#define LIST_ROWS  12            /* visible settings rows */

enum row_kind { ROW_PICK, ROW_KEY, ROW_SAVE, ROW_BOOT };

struct row {
    enum row_kind kind;
    char text[SC_KEY_LEN];       /* section or key name */
};

struct state {
    struct sc_cfg cfg;
    struct row    row[MAX_ROWS];
    int  nrow, nsec;
    int  sel, top;               /* selection, first visible settings row */
    char sec[SC_SEC_LEN];        /* section being edited / booted */
    int  secs;                   /* countdown, -1 once stopped */
    int  editing;
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

    char keys[64][SC_KEY_LEN];
    int nk = sc_keys(&st->cfg, st->sec, keys, 64);
    for (int i = 0; i < nk && st->nrow < MAX_ROWS - 2; i++) {
        st->row[st->nrow].kind = ROW_KEY;
        snprintf(st->row[st->nrow].text, SC_KEY_LEN, "%.*s",
                 SC_KEY_LEN - 1, keys[i]);
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
    if (i >= st->top + LIST_ROWS)
        st->top = i - LIST_ROWS + 1;
    int max = n_key_rows(st) - LIST_ROWS;
    if (max < 0) max = 0;
    if (st->top > max) st->top = max;
}

/* ------------------------------------------------------------------ */

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

    row = 2;
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
    for (int i = 0; i < LIST_ROWS; i++) {
        int k = st->top + i;
        ss_clear_row(ss, row, paper);
        if (k < nk) {
            const char *key = st->row[fk + k].text;
            const char *val = sc_get(&st->cfg, st->sec, key);
            int on = st->sel == fk + k;
            if (on && st->editing)
                snprintf(line, sizeof line, "%-20.20s %-48.48s", key, st->edit);
            else
                snprintf(line, sizeof line, "%-20.20s %-48.48s", key,
                         val && *val ? val : "(on)");
            ss_puts(ss, 4, row, line, on ? paper : ink, on ? hi : paper);
        }
        row++;
    }
    if (nk > LIST_ROWS) {
        snprintf(line, sizeof line, "%d-%d of %d", st->top + 1,
                 st->top + LIST_ROWS > nk ? nk : st->top + LIST_ROWS, nk);
        ss_puts(ss, SS_COLS - 2 - (int)strlen(line), row - LIST_ROWS - 1,
                line, ink, paper);
    }

    row++;
    for (int i = st->nrow - 2; i < st->nrow; i++) {
        int on = st->sel == i;
        snprintf(line, sizeof line, " %-14s",
                 st->row[i].kind == ROW_SAVE ?
                     (st->cfg.dirty ? "Save *" : "Save") : "Boot now");
        ss_puts(ss, 4 + (i == st->nrow - 1 ? 20 : 0), row, line,
                on ? paper : ink, on ? hi : paper);
    }

    snprintf(line, sizeof line, "%.50s%s", st->cfg.path,
             st->cfg.dirty ? "   (unsaved changes)" : "");
    ss_puts(ss, 2, SS_ROWS - 4, line, ink, paper);
    ss_puts(ss, 2, SS_ROWS - 3, st->editing ?
            "type a value   Enter accept   Esc cancel" :
            "Up/Down move  Enter pick or edit  Esc/X leave", ink, paper);

    ss_clear_row(ss, SS_ROWS - 2, paper);
    if (st->msg[0])
        ss_puts(ss, 2, SS_ROWS - 2, st->msg, ink, paper);
    else if (st->secs > 0) {
        snprintf(line, sizeof line, "Booting %s in %d...", st->sec, st->secs);
        ss_puts(ss, 2, SS_ROWS - 2, line, ink, paper);
    }
    else
        ss_puts(ss, 2, SS_ROWS - 2, "countdown stopped", ink, paper);
}

/* ------------------------------------------------------------------ */

static void commit_edit(struct state *st)
{
    const char *key = st->row[st->sel].text;
    if (sc_set(&st->cfg, st->sec, key, st->edit) == 0)
        snprintf(st->msg, sizeof st->msg, "%.20s = %.35s", key,
                 *st->edit ? st->edit : "(on)");
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
    for (int i = 0; i < st.nsec; i++)          /* start on the boot choice */
        if (!strcmp(st.row[i].text, st.sec))
            st.sel = i;

    double tick = now_ms();
    for (;;) {
        build_rows(&st);
        scroll_to_sel(&st);
        draw(ss, &st);
        ss_flush(ss);

        struct si_event e = si_poll(100);
        if (e.key != SI_NONE && st.secs > 0)
            st.secs = -1;                      /* any key stops the countdown */

        if (st.editing) {
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
                    const char *v = sc_get(&st.cfg, st.sec, st.row[st.sel].text);
                    snprintf(st.edit, sizeof st.edit, "%s", v ? v : "");
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
