/*
 * astos.c - ASTOS, the Atari ST Operating System front end (phase 0).
 *
 * Started by run-pistorm.sh in place of the setup page. Halts the 68k,
 * brings the real Shifter up (medium resolution on a colour monitor, high
 * on a mono one), and shows the desktop: an icon for every psctrl.cfg
 * build, a details panel, the taskbar with its start menu, the clock and
 * the Pi's temperature. Launching a build writes its name to
 * /run/astos-launch and exits 44; run-pistorm.sh starts it and brings
 * ASTOS back when it ends.
 *
 *   sudo ./astos [--config FILE] [--safe] [--mono|--colour] [--hdmi]
 *
 * Exit codes (run-pistorm.sh): 44 launch, 42 start ASTOS again (also
 * after the ST's reset button or a power loss), 43 shut the Pi down,
 * 0 leave to the console.
 *
 * Host test (no board, any Linux):
 *   make astos-host && ./astos-host --config configs/psctrl.cfg.default \
 *        --shot out.ppm [--mono] [--keys "rrd"]
 * draws the desktop into out.ppm after the keys (r l u d = arrows,
 * e = Enter, m = start menu, s = shut down dialog, x = Esc) and exits.
 */
#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "astos.h"
#include "astos_input.h"
#include "setup_cfg.h"
#include "setup_hdmi.h"
#include "astos_settings.h"
#include "native_probe.h"
#include "gpio/ps_protocol.h"
#include "gpio/bus_lock.h"

extern uint8_t fc;

/* ---- state ---------------------------------------------------------- */

static struct ss_screen ss;
static struct sc_cfg cfg;
static char cfg_path[512];

static struct env envs[MAX_ENV];
static int nenv, sel;

static int safe_mode, no_board, hdmi_on, hdmi_only, h12, mdy;
static int menu_open, menu_sel;
enum { DLG_NONE, DLG_SHUTDOWN, DLG_MESSAGE, DLG_NAME, DLG_DELETE };
static int dlg, dlg_sel;
static char dlg_text[2][64];
static char name_buf[SC_SEC_LEN];             /* DLG_NAME: what is typed       */
static char name_from[SC_SEC_LEN];            /* "" = new and empty, else copy */
static char name_err[64];

static int autolaunch = -1, countdown_s;      /* index, seconds left */

static int mx = 320, my = 100;                 /* pointer, physical px */
static int mrem_x, mrem_y;
static int buttons_prev;
#define BOUNCE_S 0.06                          /* a press this soon after a */
static double btn_up_t[2] = { -1.0, -1.0 };    /* release is switch bounce  */
static double last_click_t;
static int last_click_env = -1;

static char cpld_text[32] = "-";
static int temp_c10 = -1;
static char clock_s[16], date_s[16];
static int clock_x = AW, date_x = AW;          /* where the taskbar drew them */

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int phys_h(void) { return g_mono() ? 400 : 200; }
static int log_y(int py) { return g_mono() ? py / 2 : py; }

/* ---- layout --------------------------------------------------------- */

#define BAR_Y     188
#define BAR_H     12
#define START_W   72
#define CELL_W    80
#define CELL_H    40
#define GRID_X    8
#define GRID_Y    8
#define GRID_COLS 4
#define PANEL_X   352
#define PANEL_W   280
#define MENU_W    208
#define ROW_H     10

static void cell_rect(int i, int *x, int *y)
{
    *x = GRID_X + (i % GRID_COLS) * CELL_W;
    *y = GRID_Y + (i / GRID_COLS) * CELL_H;
}

/* start menu rows: New environment..., a separator, then Restart /
 * Console / Shut down */
enum { MI_NEW = 0, MI_HDMI, MI_RESTART, MI_CONSOLE, MI_SHUTDOWN, MI_NSYS };
static const char *const sys_label[MI_NSYS] = {
    "New environment...", "HDMI display", "Restart ASTOS", "Exit to console",
    "Shut down...",
};
static int menu_rows(void) { return MI_NSYS; }
static int menu_h(void)    { return 14 + menu_rows() * ROW_H + 10; }
static int menu_y(void)    { return BAR_Y - menu_h(); }
static int menu_row_y(int r)
{
    return menu_y() + 14 + r * ROW_H + (r >= MI_RESTART ? 4 : 0);
}

/* ---- drawing -------------------------------------------------------- */

static void draw_button(int x, int y, int w, const char *label, int hot)
{
    g_fill(x, y, w, 12, hot ? C_ACCENT : C_FACE);
    g_frame(x, y, w, 12, C_TEXT);
    g_text(x + (w - g_text_w(label)) / 2, y + 2, label, hot ? C_WHITE : C_TEXT);
}

/* a window: title bar in accent, white body, black frame */
static void draw_window(int x, int y, int w, int h, const char *title)
{
    g_fill(x + 2, y + 1, w, h, C_TEXT);                 /* shadow */
    g_fill(x, y, w, h, C_WHITE);
    g_fill(x, y, w, 11, C_ACCENT);
    g_frame(x, y, w, h, C_TEXT);
    g_textn(x + 6, y + 2, title, (w - 12) / 8, C_WHITE);
}

static void draw_icons(void)
{
    for (int i = 0; i < nenv; i++) {
        int x, y;
        cell_rect(i, &x, &y);
        int on = i == sel && !menu_open;
        if (on)
            g_fill(x + 18, y - 2, 44, 20, C_FACE);
        g_icon(x + 24, y, envs[i].kind);
        const char *t = envs[i].title;
        int chars = (int)strlen(t) > 10 ? 10 : (int)strlen(t);
        int tw = chars * 8, tx = x + (CELL_W - tw) / 2, ty = y + 22;
        if (g_mono()) {
            g_fill(tx - 2, ty - 1, tw + 4, 10, on ? C_TEXT : C_WHITE);
            g_textn(tx, ty, t, chars, on ? C_WHITE : C_TEXT);
        } else {
            if (on)
                g_fill(tx - 2, ty - 1, tw + 4, 10, C_WHITE);
            g_textn(tx, ty, t, chars, on ? C_TEXT : C_WHITE);
        }
    }
    if (nenv == 0) {
        g_fill(GRID_X, GRID_Y, 320, 22, C_WHITE);
        g_frame(GRID_X, GRID_Y, 320, 22, C_TEXT);
        g_text(GRID_X + 8, GRID_Y + 3, "No environments in psctrl.cfg", C_TEXT);
        g_text(GRID_X + 8, GRID_Y + 12, "Add a [build] section to it", C_DIM);
    }
}

static void draw_details(void)
{
    if (nenv == 0)
        return;
    const struct env *e = &envs[sel];
    int x = PANEL_X, y = 8, h = 11 + 6 * ROW_H + 22;
    draw_window(x, y, PANEL_W, h, e->title);
    for (int i = 0; i < 6; i++)
        g_textn(x + 8, y + 15 + i * ROW_H, e->line[i], (PANEL_W - 16) / 8,
                i == 0 && e->metal ? C_ACCENT : C_TEXT);
    g_text(x + 8, y + h - 15, "Double-click or Enter to launch", C_DIM);
}

static void draw_machine(void)
{
    char l[5][48];
    int x = PANEL_X, y = 104, h = 11 + 5 * ROW_H + 6;
    if (ss.shifter)
        snprintf(l[0], sizeof l[0], "ST-RAM  %uK (%uK + %uK)",
                 (ss.bank0 + ss.bank1) >> 10, ss.bank0 >> 10, ss.bank1 >> 10);
    else
        snprintf(l[0], sizeof l[0], "ST-RAM  not answering");
    snprintf(l[1], sizeof l[1], "Screen  %s, %s",
             g_mono() ? "mono 640x400" : "colour 640x200",
             ss.hz50 ? "50 Hz" : "60 Hz");
    snprintf(l[2], sizeof l[2], "Input   %s%s%d kbd, %d mouse, %d pad",
             ai_have_st() ? "ST" : "", ai_have_st() ? " + " : "",
             ai_keyboards(), ai_mice(), ai_pads());
    snprintf(l[3], sizeof l[3], "CPLD    firmware %.30s", cpld_text);
    if (temp_c10 >= 0)
        snprintf(l[4], sizeof l[4], "Pi      %d.%d C", temp_c10 / 10, temp_c10 % 10);
    else
        snprintf(l[4], sizeof l[4], "Pi      -");
    draw_window(x, y, PANEL_W, h, "This machine");
    for (int i = 0; i < 5; i++)
        g_textn(x + 8, y + 15 + i * ROW_H, l[i], (PANEL_W - 16) / 8, C_TEXT);
}

static void draw_taskbar(void)
{
    g_fill(0, BAR_Y, AW, BAR_H, C_FACE);
    g_hline(0, BAR_Y, AW, C_TEXT);
    /* start button */
    g_fill(0, BAR_Y + 1, START_W, BAR_H - 1, menu_open ? C_TEXT : C_ACCENT);
    g_fill(6, BAR_Y + 3, 4, 3, C_WHITE);                  /* the logo: 4 tiles */
    g_fill(12, BAR_Y + 3, 4, 3, C_WHITE);
    g_fill(6, BAR_Y + 7, 4, 3, C_WHITE);
    g_fill(12, BAR_Y + 7, 4, 3, C_WHITE);
    g_text(22, BAR_Y + 3, "ASTOS", C_WHITE);

    int x = START_W + 10;
    if (safe_mode) {
        g_text(x, BAR_Y + 3, "SAFE MODE", C_ACCENT);
        x += 10 * 8;
    }
    if (nenv)
        g_textn(x, BAR_Y + 3, envs[sel].title, 20, C_TEXT);

    /* right to left: clock, date, temperature. The clock and the date
     * are buttons: a click flips 24/12 hour and day/month order */
    char t[16] = "";
    if (temp_c10 >= 0)
        snprintf(t, sizeof t, "%d.%dC", temp_c10 / 10, temp_c10 % 10);
    clock_x = AW - 6 - g_text_w(clock_s);
    date_x = clock_x - 16 - g_text_w(date_s);
    g_text(clock_x, BAR_Y + 3, clock_s, C_TEXT);
    g_text(date_x, BAR_Y + 3, date_s, C_TEXT);
    g_text(date_x - 16 - g_text_w(t), BAR_Y + 3, t, C_TEXT);
}

/* [psctrl] clock 24|12 and date dmy|mdy, as the page writes them */
static void save_clock_keys(void)
{
    sc_set(&cfg, "psctrl", "clock", h12 ? "12" : "24");
    sc_set(&cfg, "psctrl", "date", mdy ? "mdy" : "dmy");
    if (!no_board && sc_save(&cfg, cfg_path) != 0)
        fprintf(stderr, "[ASTOS] could not save %s\n", cfg_path);
    sys_clock(clock_s, sizeof clock_s, h12);
    sys_date(date_s, sizeof date_s, mdy);
}

static void draw_menu(void)
{
    int y = menu_y(), h = menu_h();
    g_fill(2, y + 1, MENU_W, h, C_TEXT);
    g_fill(0, y, MENU_W, h, C_WHITE);
    g_frame(0, y, MENU_W, h, C_TEXT);
    g_text(8, y + 3, "ASTOS " ASTOS_VERSION, C_DIM);
    g_hline(4, y + 12, MENU_W - 8, C_DIM);
    for (int r = 0; r < menu_rows(); r++) {
        int ry = menu_row_y(r);
        int on = r == menu_sel;
        if (r == MI_RESTART)
            g_hline(4, ry - 3, MENU_W - 8, C_DIM);
        if (on)
            g_fill(4, ry - 1, MENU_W - 8, ROW_H, C_ACCENT);
        g_textn(12, ry, sys_label[r], (MENU_W - 24) / 8, on ? C_WHITE : C_TEXT);
        if (r == MI_HDMI)
            g_text(MENU_W - 12 - 8 * 4, ry, hdmi_only ? "only" : hdmi_on ? " on" : "off",
                   on ? C_WHITE : C_DIM);
    }
}

/* the right-click menu on an icon */
enum { CX_LAUNCH = 0, CX_SETTINGS, CX_DUP, CX_DELETE, CX_N };
static const char *const ctx_label[CX_N] = {
    "Launch", "Settings...", "Duplicate...", "Delete...",
};
static int ctx_open, ctx_x, ctx_y, ctx_sel;
#define CTX_W 128
static int ctx_h(void) { return CX_N * ROW_H + 6; }

static void draw_ctx(void)
{
    int h = ctx_h();
    g_fill(ctx_x + 2, ctx_y + 1, CTX_W, h, C_TEXT);
    g_fill(ctx_x, ctx_y, CTX_W, h, C_WHITE);
    g_frame(ctx_x, ctx_y, CTX_W, h, C_TEXT);
    for (int r = 0; r < CX_N; r++) {
        int ry = ctx_y + 3 + r * ROW_H;
        if (r == ctx_sel)
            g_fill(ctx_x + 4, ry - 1, CTX_W - 8, ROW_H, C_ACCENT);
        g_text(ctx_x + 12, ry, ctx_label[r], r == ctx_sel ? C_WHITE : C_TEXT);
    }
}

static void open_ctx(int x, int y)
{
    ctx_open = 1;
    ctx_sel = 0;
    ctx_x = x > AW - CTX_W - 4 ? AW - CTX_W - 4 : x;
    ctx_y = y > BAR_Y - ctx_h() - 2 ? BAR_Y - ctx_h() - 2 : y;
}

static void draw_dialog(void)
{
    if (dlg == DLG_NAME) {
        int w = 360, h = 62, x = (AW - w) / 2, y = (BAR_Y - h) / 2;
        char t[64], f[40];
        snprintf(t, sizeof t, name_from[0] ? "Duplicate %.20s" : "New environment", name_from);
        draw_window(x, y, w, h, t);
        g_text(x + 10, y + 16, "Name:", C_TEXT);
        snprintf(f, sizeof f, "%.30s_", name_buf);
        g_fill(x + 60, y + 15, w - 70, 10, C_WHITE);
        g_frame(x + 60, y + 15, w - 70, 10, C_ACCENT);
        g_text(x + 64, y + 16, f, C_TEXT);
        g_textn(x + 10, y + 28, name_err[0] ? name_err : "letters, digits, - and _",
                (w - 20) / 8, name_err[0] ? C_ACCENT : C_DIM);
        draw_button(x + w - 200, y + h - 16, 96, "Create", dlg_sel == 0);
        draw_button(x + w - 96, y + h - 16, 88, "Cancel", dlg_sel == 1);
        return;
    }
    int w = 320, h = (dlg == DLG_SHUTDOWN || dlg == DLG_DELETE) ? 58 : 44;
    int x = (AW - w) / 2, y = (BAR_Y - h) / 2;
    draw_window(x, y, w, h, dlg == DLG_SHUTDOWN ? "Shut down" :
                            dlg == DLG_DELETE ? "Delete" : "ASTOS");
    g_textn(x + 10, y + 16, dlg_text[0], (w - 20) / 8, C_TEXT);
    g_textn(x + 10, y + 26, dlg_text[1], (w - 20) / 8, C_DIM);
    if (dlg == DLG_SHUTDOWN || dlg == DLG_DELETE) {
        draw_button(x + w - 200, y + h - 16, 96, dlg == DLG_DELETE ? "Delete" : "Shut down",
                    dlg_sel == 0);
        draw_button(x + w - 96, y + h - 16, 88, "Cancel", dlg_sel == 1);
    }
}

static void draw_toast(void)
{
    if (autolaunch < 0)
        return;
    char t[64];
    snprintf(t, sizeof t, "Starting %s in %d s", envs[autolaunch].title, countdown_s);
    int w = 296, x = 8, y = BAR_Y - 30;
    draw_window(x, y, w, 26, "Auto-launch");
    g_textn(x + 8, y + 14, t, 34, C_TEXT);
    (void)w;
}

static void render(void)
{
    g_clear(C_DESK);
    if (safe_mode) {
        g_fill(0, 0, AW, 10, C_WHITE);
        g_text(8, 1, "ASTOS safe mode: no auto-launch, no HDMI.  Restart ASTOS to leave.",
               C_TEXT);
    }
    if (set_is_open()) {
        set_draw();
        draw_taskbar();
        if (dlg)
            draw_dialog();
        g_pack(&ss);
        return;
    }
    draw_icons();
    draw_details();
    draw_machine();
    draw_toast();
    draw_taskbar();
    if (menu_open)
        draw_menu();
    if (ctx_open)
        draw_ctx();
    if (dlg)
        draw_dialog();
    g_pack(&ss);
}

static void show(void)
{
    g_compose(&ss, mx, my, 1);
    ss_flush(&ss);
    if (hdmi_on && sh_active())
        sh_present(&ss);
}

/* ---- actions -------------------------------------------------------- */

static void message(const char *a, const char *b)
{
    dlg = DLG_MESSAGE;
    snprintf(dlg_text[0], sizeof dlg_text[0], "%.63s", a);
    snprintf(dlg_text[1], sizeof dlg_text[1], "%.63s", b ? b : "");
}

static int launch(int i)
{
    if (i < 0 || i >= nenv)
        return 0;
    if (envs[i].metal && access("./stboxmetal", X_OK) != 0 && !no_board) {
        message("stboxmetal is not built.", "Run make on the Pi first.");
        return 0;
    }
    char t[64];
    snprintf(t, sizeof t, "Starting %s ...", envs[i].title);
    message(t, envs[i].metal ? "Ctrl+Alt+Undo comes back here"
                             : "Ctrl+Alt+Undo or Restart comes back here");
    render();
    show();
    if (no_board) {
        printf("[ASTOS] launch [%s] (no board: not starting it)\n", envs[i].sec);
        return 1;
    }
    if (env_launch(&cfg, cfg_path, envs[i].sec) != 0) {
        message("Could not start it:", "cannot write " ASTOS_LAUNCH_FILE);
        return 0;
    }
    printf("[ASTOS] launching [%s]\n", envs[i].sec);
    return 1;
}

static void reload_envs(const char *keep)
{
    char want[SC_SEC_LEN];
    snprintf(want, sizeof want, "%s", keep ? keep : "");
    if (sc_load(&cfg, cfg_path) != 0)
        fprintf(stderr, "[ASTOS] cannot re-read %s\n", cfg_path);
    nenv = env_load(&cfg, envs, MAX_ENV);
    sel = 0;
    for (int i = 0; i < nenv; i++)
        if (!strcasecmp(envs[i].sec, want))
            sel = i;
}

static void hdmi_start(void);

/* the start menu's HDMI display switch: [psctrl] hdmi on|off, live */
static void toggle_hdmi(void)
{
    if (hdmi_only) {
        message("HDMI is on: hdmi_only is set", "(in [psctrl] or the last environment)");
        return;
    }
    hdmi_on = !hdmi_on;
    sc_set(&cfg, "psctrl", "hdmi", hdmi_on ? "on" : "off");
    if (sc_save(&cfg, cfg_path) != 0)
        fprintf(stderr, "[ASTOS] could not save %s\n", cfg_path);
    if (no_board)
        return;
    if (hdmi_on)
        hdmi_start();
    else {
        sh_close();
        printf("[ASTOS] HDMI off\n");
    }
}

/* the settings window for one environment */
static void open_settings(int i)
{
    if (i < 0 || i >= nenv)
        return;
    if (set_open(cfg_path, envs[i].sec, envs[i].title) != 0)
        message("Could not read psctrl.cfg", cfg_path);
    else
        printf("[ASTOS] settings: [%s]\n", envs[i].sec);
}

static void settings_closed(void)
{
    char keep[SC_SEC_LEN];
    snprintf(keep, sizeof keep, "%s", set_section());
    reload_envs(keep);
}

/* New / Duplicate: ask for the name */
static void ask_name(const char *from)
{
    dlg = DLG_NAME;
    dlg_sel = 0;
    name_buf[0] = name_err[0] = '\0';
    snprintf(name_from, sizeof name_from, "%s", from ? from : "");
}

/* the same rule as the page: lower case, letters digits - _, unique */
static int name_ok(void)
{
    if (!name_buf[0]) { snprintf(name_err, sizeof name_err, "a name is needed"); return 0; }
    for (char *p = name_buf; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
            snprintf(name_err, sizeof name_err, "'%c' cannot be in a name", *p);
            return 0;
        }
    }
    if (!strcmp(name_buf, "psctrl") || !strcmp(name_buf, "astos")) {
        snprintf(name_err, sizeof name_err, "[%s] is reserved", name_buf);
        return 0;
    }
    char secs[64][SC_SEC_LEN];
    int n = sc_sections(&cfg, secs, 64);
    for (int i = 0; i < n; i++)
        if (!strcasecmp(secs[i], name_buf)) {
            snprintf(name_err, sizeof name_err, "there is already a [%.20s]", name_buf);
            return 0;
        }
    return 1;
}

static void create_env(void)
{
    if (!name_ok())
        return;
    int r = name_from[0] ? sc_copy_section(&cfg, name_from, name_buf)
                         : sc_add_section(&cfg, name_buf);
    if (r != 0 || sc_save(&cfg, cfg_path) != 0) {
        snprintf(name_err, sizeof name_err, "could not save %.40s", cfg_path);
        return;
    }
    printf("[ASTOS] %s [%s]\n", name_from[0] ? "duplicated as" : "new environment", name_buf);
    char made[SC_SEC_LEN];
    snprintf(made, sizeof made, "%s", name_buf);
    dlg = DLG_NONE;
    reload_envs(made);
    if (!strcasecmp(envs[sel].sec, made))
        open_settings(sel);                      /* straight to its settings */
}

static void ask_delete(int i)
{
    if (i < 0 || i >= nenv)
        return;
    dlg = DLG_DELETE;
    dlg_sel = 1;
    snprintf(dlg_text[0], sizeof dlg_text[0], "Delete %.40s?", envs[i].title);
    snprintf(dlg_text[1], sizeof dlg_text[1], "Its section goes from psctrl.cfg.");
}

static void delete_env(void)
{
    char gone[SC_SEC_LEN];
    snprintf(gone, sizeof gone, "%s", envs[sel].sec);
    dlg = DLG_NONE;
    if (sc_del_section(&cfg, gone) != 0) {
        message("Could not delete it", gone);
        return;
    }
    static const char *const refs[] = { "astos_last", "astos_autolaunch", "boot" };
    for (unsigned k = 0; k < sizeof refs / sizeof *refs; k++) {
        const char *v = sc_get(&cfg, "psctrl", refs[k]);
        if (v && !strcasecmp(v, gone))
            sc_set(&cfg, "psctrl", refs[k], NULL);
    }
    if (sc_save(&cfg, cfg_path) != 0) {
        message("Could not save psctrl.cfg", sc_save_error(&cfg));
        return;
    }
    printf("[ASTOS] deleted [%s]\n", gone);
    reload_envs(NULL);
}

static int menu_activate(int r, int *exit_code)
{
    menu_open = 0;
    switch (r) {
    case MI_NEW:      ask_name(NULL); return 0;
    case MI_HDMI:     toggle_hdmi(); return 0;
    case MI_RESTART:  *exit_code = ASTOS_EXIT_RESTART; return 1;
    case MI_CONSOLE:  *exit_code = 0; return 1;
    case MI_SHUTDOWN:
        dlg = DLG_SHUTDOWN; dlg_sel = 1;
        snprintf(dlg_text[0], sizeof dlg_text[0], "Shut the Pi down?");
        snprintf(dlg_text[1], sizeof dlg_text[1], "Switch the Atari off when it is down.");
        return 0;
    }
    return 0;
}

static void screenshot(void)
{
    char path[600];
    const struct passwd *pw = sc_invoking_user();
    const char *home = pw && pw->pw_dir ? pw->pw_dir : ".";
    for (int n = 1; n < 1000; n++) {
        snprintf(path, sizeof path, "%s/astos-shot-%d.ppm", home, n);
        if (access(path, F_OK) != 0)
            break;
    }
    if (g_screenshot(&ss, path) == 0) {
        if (pw)
            (void)!chown(path, pw->pw_uid, pw->pw_gid);
        printf("[ASTOS] screenshot %s\n", path);
    }
}

static void cancel_autolaunch(void) { autolaunch = -1; }

static int ctx_do(int r, int *exit_code)
{
    switch (r) {
    case CX_LAUNCH:
        if (launch(sel)) { *exit_code = ASTOS_EXIT_LAUNCH; return 1; }
        return 0;
    case CX_SETTINGS: open_settings(sel); return 0;
    case CX_DUP:      ask_name(envs[sel].sec); return 0;
    case CX_DELETE:   ask_delete(sel); return 0;
    }
    return 0;
}

/* one key; returns 1 with *exit_code set when ASTOS should exit */
static int on_key(struct si_event k, int *exit_code)
{
    cancel_autolaunch();
    if (k.key == SI_SNAP) {
        screenshot();
        return 0;
    }
    if (dlg == DLG_MESSAGE) {
        if (k.key == SI_ENTER || k.key == SI_ESC || k.key == SI_SPACE)
            dlg = DLG_NONE;
        return 0;
    }
    if (dlg == DLG_NAME) {
        size_t n = strlen(name_buf);
        switch (k.key) {
        case SI_CHAR:
            if (n + 1 < sizeof name_buf) { name_buf[n] = k.ch; name_buf[n + 1] = '\0'; }
            name_err[0] = '\0';
            break;
        case SI_BACKSPACE: if (n) name_buf[n - 1] = '\0'; break;
        case SI_TAB: dlg_sel ^= 1; break;
        case SI_ENTER: if (dlg_sel == 0) create_env(); else dlg = DLG_NONE; break;
        case SI_ESC: dlg = DLG_NONE; break;
        default: break;
        }
        return 0;
    }
    if (dlg == DLG_DELETE) {
        switch (k.key) {
        case SI_LEFT: case SI_RIGHT: case SI_TAB: dlg_sel ^= 1; break;
        case SI_ESC: dlg = DLG_NONE; break;
        case SI_ENTER: case SI_SPACE:
            if (dlg_sel == 0) delete_env(); else dlg = DLG_NONE;
            break;
        default: break;
        }
        return 0;
    }
    if (set_is_open()) {
        if (set_key(k))
            settings_closed();
        return 0;
    }
    if (ctx_open) {
        switch (k.key) {
        case SI_UP:   ctx_sel = (ctx_sel + CX_N - 1) % CX_N; break;
        case SI_DOWN: ctx_sel = (ctx_sel + 1) % CX_N; break;
        case SI_ESC: ctx_open = 0; break;
        case SI_ENTER: case SI_SPACE:
            ctx_open = 0;
            return ctx_do(ctx_sel, exit_code);
        default: break;
        }
        return 0;
    }
    if (dlg == DLG_SHUTDOWN) {
        switch (k.key) {
        case SI_LEFT: case SI_RIGHT: case SI_TAB: dlg_sel ^= 1; break;
        case SI_ESC: dlg = DLG_NONE; break;
        case SI_ENTER: case SI_SPACE:
            if (dlg_sel == 0) { *exit_code = ASTOS_EXIT_HALT; return 1; }
            dlg = DLG_NONE;
            break;
        default: break;
        }
        return 0;
    }
    if (menu_open) {
        switch (k.key) {
        case SI_UP:   menu_sel = (menu_sel + menu_rows() - 1) % menu_rows(); break;
        case SI_DOWN: menu_sel = (menu_sel + 1) % menu_rows(); break;
        case SI_ESC: case SI_F1: case SI_TAB: menu_open = 0; break;
        case SI_ENTER: case SI_SPACE:
            return menu_activate(menu_sel, exit_code);
        default: break;
        }
        return 0;
    }
    switch (k.key) {
    case SI_LEFT:  if (sel > 0) sel--; break;
    case SI_RIGHT: if (sel + 1 < nenv) sel++; break;
    case SI_UP:    if (sel - GRID_COLS >= 0) sel -= GRID_COLS; break;
    case SI_DOWN:  if (sel + GRID_COLS < nenv) sel += GRID_COLS; break;
    case SI_HOME:  sel = 0; break;
    case SI_F1: case SI_TAB: case SI_ESC:
        menu_open = 1; menu_sel = 0; break;
    case SI_F2:                                         /* its settings */
        if (nenv) open_settings(sel);
        break;
    case SI_ENTER: case SI_F10:
        if (launch(sel)) { *exit_code = ASTOS_EXIT_LAUNCH; return 1; }
        break;
    case SI_CHAR:                                       /* jump by letter */
        for (int n = 1; n <= nenv; n++) {
            int i = (sel + n) % nenv;
            char c = envs[i].title[0];
            if ((c | 0x20) == (k.ch | 0x20)) { sel = i; break; }
        }
        break;
    default: break;
    }
    return 0;
}

static int inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

/* a left click at logical (x, y); dbl = second click on the same icon */
static int on_click(int x, int y, int *exit_code)
{
    cancel_autolaunch();
    if (dlg == DLG_MESSAGE) { dlg = DLG_NONE; return 0; }
    if (dlg == DLG_SHUTDOWN || dlg == DLG_DELETE) {
        int w = 320, h = 58, dx = (AW - w) / 2, dy = (BAR_Y - h) / 2;
        if (inside(x, y, dx + w - 200, dy + h - 16, 96, 12)) {
            if (dlg == DLG_DELETE) { delete_env(); return 0; }
            *exit_code = ASTOS_EXIT_HALT;
            return 1;
        }
        if (inside(x, y, dx + w - 96, dy + h - 16, 88, 12))
            dlg = DLG_NONE;
        return 0;
    }
    if (dlg == DLG_NAME) {
        int w = 360, h = 62, dx = (AW - w) / 2, dy = (BAR_Y - h) / 2;
        if (inside(x, y, dx + w - 200, dy + h - 16, 96, 12)) create_env();
        else if (inside(x, y, dx + w - 96, dy + h - 16, 88, 12)) dlg = DLG_NONE;
        return 0;
    }
    if (set_is_open()) {
        if (set_click(x, y))
            settings_closed();
        return 0;
    }
    if (ctx_open) {
        if (inside(x, y, ctx_x, ctx_y, CTX_W, ctx_h())) {
            int r = (y - ctx_y - 2) / ROW_H;
            ctx_open = 0;
            return r >= 0 && r < CX_N ? ctx_do(r, exit_code) : 0;
        }
        ctx_open = 0;
        return 0;
    }
    if (y >= BAR_Y && x >= clock_x - 4) {             /* the clock: 24 <-> 12 h */
        h12 = !h12;
        save_clock_keys();
        printf("[ASTOS] clock %s hour\n", h12 ? "12" : "24");
        return 0;
    }
    if (y >= BAR_Y && x >= date_x - 4 && x < clock_x - 8) {   /* the date */
        mdy = !mdy;
        save_clock_keys();
        printf("[ASTOS] date %s\n", mdy ? "month/day/year" : "day/month/year");
        return 0;
    }
    if (inside(x, y, 0, BAR_Y, START_W, BAR_H)) {
        menu_open = !menu_open;
        menu_sel = 0;
        return 0;
    }
    if (menu_open) {
        if (inside(x, y, 0, menu_y(), MENU_W, menu_h())) {
            for (int r = 0; r < menu_rows(); r++)
                if (y >= menu_row_y(r) - 1 && y < menu_row_y(r) - 1 + ROW_H)
                    return menu_activate(r, exit_code);
            return 0;
        }
        menu_open = 0;
        return 0;
    }
    for (int i = 0; i < nenv; i++) {
        int cx, cy;
        cell_rect(i, &cx, &cy);
        if (inside(x, y, cx + 8, cy - 2, CELL_W - 16, CELL_H - 6)) {
            double t = now_s();
            int dbl = last_click_env == i && t - last_click_t < 0.45;
            sel = i;
            last_click_env = i;
            last_click_t = t;
            if (dbl) {
                last_click_env = -1;
                if (launch(i)) { *exit_code = ASTOS_EXIT_LAUNCH; return 1; }
            }
            return 0;
        }
    }
    return 0;
}

/* hover in the open menu follows the pointer */
static int on_hover(int x, int y)
{
    if (set_is_open() && !dlg)
        return set_hover(x, y);
    if (ctx_open && inside(x, y, ctx_x, ctx_y + 2, CTX_W, CX_N * ROW_H)) {
        int r = (y - ctx_y - 2) / ROW_H;
        if (r >= 0 && r < CX_N && r != ctx_sel) { ctx_sel = r; return 1; }
        return 0;
    }
    if (!menu_open || !inside(x, y, 0, menu_y(), MENU_W, menu_h()))
        return 0;
    for (int r = 0; r < menu_rows(); r++)
        if (y >= menu_row_y(r) - 1 && y < menu_row_y(r) - 1 + ROW_H && menu_sel != r) {
            menu_sel = r;
            return 1;
        }
    return 0;
}

static void tick_status(void)
{
    temp_c10 = no_board ? 512 : sys_temp_c10();
    sys_clock(clock_s, sizeof clock_s, h12);
    sys_date(date_s, sizeof date_s, mdy);
}

/* ---- start-up ------------------------------------------------------- */

static void splash(const char *line)
{
    g_clear(C_DESK);
    int w = 280, h = 40, x = (AW - w) / 2, y = 70;
    draw_window(x, y, w, h, "ASTOS " ASTOS_VERSION);
    g_text(x + 10, y + 16, "Atari ST Operating System", C_TEXT);
    g_textn(x + 10, y + 27, line, 32, C_DIM);
    g_pack(&ss);
    g_compose(&ss, 0, 0, 0);
    ss_flush(&ss);
    if (hdmi_on && sh_active())
        sh_present(&ss);
}

/* the boolean words config_file.c accepts; a bare key is on */
static int cfg_on(const char *v)
{
    return v && (!*v || !strcasecmp(v, "on") || !strcasecmp(v, "1") ||
                 !strcasecmp(v, "yes") || !strcasecmp(v, "true") ||
                 !strcasecmp(v, "enabled") || !strcasecmp(v, "enable"));
}

/* ASTOS on HDMI: the same picture as the ST monitor, scaled */
static void hdmi_start(void)
{
    if (sh_active())
        return;
    if (sh_open() == 0) {
        uint32_t rgb[4];
        g_palette_rgb(rgb);
        sh_set_palette(rgb);
        printf("[ASTOS] HDMI on%s\n", hdmi_only ? " (hdmi_only)" : "");
    } else {
        printf("[ASTOS] HDMI: no display to open\n");
        hdmi_on = 0;
    }
}

/* The CPLD's revision word as the firmware is named: bits 10-9 the kind
 * (01 alpha, 10 beta, else release), bit 8 EPM570, and for an alpha
 * bits 7-0 the number (0.253a), otherwise bits 7-5 major and 4-0 minor
 * ($421 = 1.1b on an EPM240) - as ps_get_firmware_revision prints it. */
static void fw_text(unsigned fw, char *out, size_t n)
{
    if (!fw) {
        snprintf(out, n, "not answering");
        return;
    }
    const char *chip = (fw & 0x100) ? "EPM570" : "EPM240";
    if ((fw & 0x600) == 0x200)
        snprintf(out, n, "0.%ua (%s)", fw & 0xFFu, chip);
    else
        snprintf(out, n, "%u.%u%c (%s)", (fw & 0xE0u) >> 5, fw & 0x1Fu,
                 (fw & 0x600) == 0x400 ? 'b' : 'r', chip);
}

static int board_up(void)
{
    if (pistorm_bus_lock("astos") != 0) {
        fprintf(stderr, "[ASTOS] the PiStorm bus is in use - stop the other process first\n");
        return -1;
    }
    ps_setup_protocol();
    ps_get_firmware_revision();
    if (!ps_fw_rev) {                       /* the ST is off: wait for it */
        ps_power_wait();
        ps_get_firmware_revision();
    }
    fw_text(ps_fw_rev, cpld_text, sizeof cpld_text);
    ps_pulse_halt();
    ps_reset_state_machine();
    ps_pulse_reset();
    usleep(250000);
    for (int i = 0; i < 1000 && ps_st_reset_pin() == 1; i++)   /* button held */
        usleep(10000);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cfg_arg = NULL, *shot = NULL, *keys = NULL;
    enum ss_mode force = SS_MODE_AUTO;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfg_arg = argv[++i];
        else if (!strcmp(argv[i], "--safe"))      safe_mode = 1;
        else if (!strcmp(argv[i], "--mono"))      force = SS_MODE_MONO;
        else if (!strcmp(argv[i], "--colour"))    force = SS_MODE_COLOUR;
        else if (!strcmp(argv[i], "--hdmi"))      hdmi_on = 1;
        else if (!strcmp(argv[i], "--no-board"))  no_board = 1;
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) { shot = argv[++i]; no_board = 1; }
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--config FILE] [--safe] [--mono|--colour] [--hdmi]\n"
                            "       %s --no-board --shot OUT.ppm [--mono] [--keys KEYS]\n",
                    argv[0], argv[0]);
            return 2;
        }
    }

    /* the config: --config, else found as the emulator finds it */
    if (cfg_arg)
        snprintf(cfg_path, sizeof cfg_path, "%s", cfg_arg);
    else {
        int made = 0;
        if (sc_locate(cfg_path, sizeof cfg_path, &made) != 0) {
            fprintf(stderr, "[ASTOS] no psctrl.cfg and no psctrl.cfg.default\n");
            return 1;
        }
    }

    if (no_board) {
        memset(&ss, 0, sizeof ss);
        ss.mode = force == SS_MODE_MONO ? SS_MODE_MONO : SS_MODE_COLOUR;
        ss.width = 640;
        ss.height = ss.mode == SS_MODE_MONO ? 400 : 200;
        ss.planes = ss.mode == SS_MODE_MONO ? 1 : 2;
        ss.hz50 = 1;
        ss.bank0 = ss.bank1 = 2048u << 10;
        ss.shifter = 0;
        fw_text(0x421, cpld_text, sizeof cpld_text);
    } else {
        if (board_up() != 0)
            return 1;
        /* first start on this board: write [native] before listing */
        const char *np = getenv("PISTORM_NATIVE_PROBE");
        if (!(np && *np == '0'))
            np_ensure_native_section(cfg_path);
        ss_bringup(&ss, force, 1);
        if (!ss.shifter)
            printf("[ASTOS] no ST-RAM answered: nothing on the ST monitor\n");
    }
    g_set_mode(ss.mode == SS_MODE_MONO);
    if (ss.shifter) {
        uint16_t pal[4];
        g_palette_st(pal);
        ss_palette(pal);
    }

    if (sc_load(&cfg, cfg_path) != 0) {
        fprintf(stderr, "[ASTOS] cannot read %s\n", cfg_path);
        return 1;
    }
    {
        const char *v = sc_get(&cfg, "psctrl", "clock");
        h12 = v && !strcmp(v, "12");
        v = sc_get(&cfg, "psctrl", "date");
        mdy = v && !strcasecmp(v, "mdy");
    }
    /* HDMI: [psctrl] hdmi (the start menu's switch), hdmi_only there, or
     * the environment launched last being an hdmi_only one - whoever
     * looks only at HDMI must find ASTOS there when the build ends. No ST
     * memory answering at all: HDMI regardless. Safe mode drops only the
     * plain mirror, never an HDMI-only setup. */
    hdmi_only = cfg_on(sc_get(&cfg, "psctrl", "hdmi_only"));
    {
        const char *last = sc_get(&cfg, "psctrl", "astos_last");
        if (last && cfg_on(sc_get(&cfg, last, "hdmi_only")))
            hdmi_only = 1;
    }
    if (cfg_on(sc_get(&cfg, "psctrl", "hdmi")))
        hdmi_on = 1;
    if (!no_board && !ss.shifter)
        hdmi_only = 1;
    if (safe_mode && !hdmi_only)
        hdmi_on = 0;
    if (hdmi_only)
        hdmi_on = 1;
    if (hdmi_on && !no_board)
        hdmi_start();

    nenv = env_load(&cfg, envs, MAX_ENV);
    {
        const char *last = sc_get(&cfg, "psctrl", "astos_last");
        for (int i = 0; last && i < nenv; i++)
            if (!strcasecmp(envs[i].sec, last))
                sel = i;
    }
    tick_status();
    printf("[ASTOS] %s, %d environment(s), %s%s\n",
           g_mono() ? "mono 640x400" : "colour 640x200 (medium)", nenv,
           cfg_path, safe_mode ? ", SAFE MODE" : "");

    if (!no_board) {
        splash("Starting...   (Help = safe mode)");
        ai_open(ss.shifter, 1);
        printf("[ASTOS] input: ST %s, %d keyboard(s), %d mouse/mice, %d pad(s)\n",
               ai_have_st() ? "keyboard+mouse" : "none", ai_keyboards(),
               ai_mice(), ai_pads());
        /* Help (PgUp on a PC keyboard) during the splash: safe mode */
        double until = now_s() + 1.5;
        while (now_s() < until) {
            struct ai_event e = ai_poll(20);
            if (e.type == AI_KEY && e.key.key == SI_SNAP && !safe_mode) {
                safe_mode = 1;
                printf("[ASTOS] Help held: safe mode\n");
                splash("Safe mode");
            }
        }
    }

    /* auto-launch: [psctrl] astos_autolaunch <build> + astos_countdown N */
    if (!safe_mode) {
        const char *al = sc_get(&cfg, "psctrl", "astos_autolaunch");
        int n = sc_get_int(&cfg, "psctrl", "astos_countdown", 0);
        for (int i = 0; al && n > 0 && i < nenv; i++)
            if (!strcasecmp(envs[i].sec, al)) {
                autolaunch = i;
                countdown_s = n;
                sel = i;
            }
    }

    int exit_code = 0;

    if (no_board) {                          /* host test: keys, then a shot */
        for (const char *k = keys; k && *k; k++) {
            struct si_event e = { SI_NONE, SI_SRC_USB, 0 };
            switch (*k) {
            case 'r': e.key = SI_RIGHT; break;
            case 'l': e.key = SI_LEFT; break;
            case 'u': e.key = SI_UP; break;
            case 'd': e.key = SI_DOWN; break;
            case 'e': e.key = SI_ENTER; break;
            case 'm': e.key = SI_F1; break;
            case 'x': e.key = SI_ESC; break;
            case 's': menu_open = 1; menu_sel = MI_SHUTDOWN; e.key = SI_ENTER; break;
            case 'c': open_ctx(100, 40); continue;
            case 'p': e.key = SI_F2; break;
            case 'k': render(); on_click(clock_x + 2, BAR_Y + 4, &exit_code); continue;
            case 'j': render(); on_click(date_x + 2, BAR_Y + 4, &exit_code); continue;
            case 'n': menu_open = 1; menu_sel = MI_NEW; e.key = SI_ENTER; break;
            case ' ': e.key = SI_SPACE; break;
            case 't': e.key = SI_TAB; break;
            default: continue;
            }
            if (on_key(e, &exit_code))
                break;
        }
        mx = 300; my = g_mono() ? 260 : 130;
        render();
        g_compose(&ss, mx, my, 1);
        if (shot && g_screenshot(&ss, shot) != 0) {
            perror(shot);
            return 1;
        }
        printf("[ASTOS] exit %d\n", exit_code);
        return 0;
    }

    render();
    show();
    double next_tick = now_s() + 1.0;
    int power_checks = 0;

    for (;;) {
        struct ai_event e = ai_poll(20);
        int redraw = 0, moved = 0;

        if (e.type == AI_KEY) {
            if (on_key(e.key, &exit_code))
                break;
            redraw = 1;
        } else if (e.type == AI_MOUSE) {
            /* a USB mouse counts much finer than the ST's: halve it */
            int dx = e.usb ? e.dx + mrem_x : e.dx;
            int dy = e.usb ? e.dy + mrem_y : e.dy;
            if (e.usb) { mrem_x = dx % 2; mrem_y = dy % 2; dx /= 2; dy /= 2; }
            if (!g_mono())                    /* medium: rows are 2 px tall */
                dy = e.usb ? dy / 2 : dy;
            int ox = mx, oy = my;
            mx += dx; my += dy;
            if (mx < 0) mx = 0;
            if (mx > 639) mx = 639;
            if (my < 0) my = 0;
            if (my > phys_h() - 1) my = phys_h() - 1;
            moved = mx != ox || my != oy;
            if (moved && on_hover(mx, log_y(my)))
                redraw = 1;
            /* Mouse-button bounce: the ST mouse's microswitches chatter, and
             * the IKBD reports every edge, so a release could arrive as
             * release-press-release - a second click that closed the list
             * the first one opened. A press within BOUNCE_S of that button's
             * last release is taken as the same click, not a new one. */
            double tb = now_s();
            int left_down = (e.buttons & 1) && !(buttons_prev & 1);
            int right_down = (e.buttons & 2) && !(buttons_prev & 2);
            if (!(e.buttons & 1) && (buttons_prev & 1)) {
                btn_up_t[0] = tb;
                if (set_is_open() && !dlg && set_release(mx, log_y(my)))
                    redraw = 1;
            }
            if (!(e.buttons & 2) && (buttons_prev & 2))
                btn_up_t[1] = tb;
            if (left_down && tb - btn_up_t[0] < BOUNCE_S)
                left_down = 0;
            if (right_down && tb - btn_up_t[1] < BOUNCE_S)
                right_down = 0;
            if (left_down) {
                if (on_click(mx, log_y(my), &exit_code))
                    break;
                redraw = 1;
            }
            if (right_down) {
                /* right: on an icon its menu (Launch, Settings), else
                 * the start menu */
                cancel_autolaunch();
                if (set_is_open() && !dlg) {         /* right = Esc there */
                    struct si_event esc = { SI_ESC, SI_SRC_ST, 0 };
                    if (set_key(esc))
                        settings_closed();
                    buttons_prev = e.buttons;
                    render();
                    show();
                    continue;
                }
                int lx = mx, ly = log_y(my), hit = -1;
                for (int i = 0; i < nenv && !menu_open && !dlg; i++) {
                    int cx, cy;
                    cell_rect(i, &cx, &cy);
                    if (inside(lx, ly, cx + 8, cy - 2, CELL_W - 16, CELL_H - 6))
                        hit = i;
                }
                if (hit >= 0) {
                    sel = hit;
                    open_ctx(lx, ly);
                } else if (!dlg) {
                    ctx_open = 0;
                    menu_open = !menu_open;
                    menu_sel = 0;
                }
                redraw = 1;
            }
            buttons_prev = e.buttons;
        }

        double t = now_s();
        if (t >= next_tick) {
            next_tick += 1.0;
            tick_status();
            redraw = 1;
            if (autolaunch >= 0 && --countdown_s <= 0) {
                sel = autolaunch;
                if (launch(sel)) { exit_code = ASTOS_EXIT_LAUNCH; break; }
                autolaunch = -1;
            }
            /* the ST's reset button: start ASTOS afresh */
            if (ps_st_reset_pin() == 1) {
                printf("[ASTOS] Atari reset - restarting\n");
                for (int i = 0; i < 1000 && ps_st_reset_pin() == 1; i++)
                    usleep(10000);
                exit_code = ASTOS_EXIT_RESTART;
                break;
            }
            /* the ST lost power: the relaunch waits for it */
            if (++power_checks >= 2) {
                power_checks = 0;
                if (ps_power_poll() == 1) {
                    printf("[ASTOS] Atari power lost - waiting for it\n");
                    exit_code = ASTOS_EXIT_RESTART;
                    break;
                }
            }
        }

        if (redraw)
            render();
        if (redraw || moved)
            show();
    }

    ai_close();
    if (hdmi_on)
        sh_close();
    if (exit_code != ASTOS_EXIT_LAUNCH && exit_code != ASTOS_EXIT_RESTART)
        printf("[ASTOS] exit %d\n", exit_code);
    if (exit_code == 0 || exit_code == ASTOS_EXIT_HALT) {
        /* Exit to console / Shut down: nothing starts after us, and a reset
         * would point the Shifter at low memory - random patterns for as
         * long as the Atari stays on. Leave a black screen saying what
         * happened instead, and no reset. Black paper: medium pen 0 black;
         * mono pen 0 bit 0 clear = inverted, so 0 bits show black and the
         * ink bits white. */
        if (ss.shifter) {
            static const uint16_t pal[4] = { 0x0000, 0x0777, 0x0444, 0x0777 };
            const char *l1, *l2;
            if (exit_code == ASTOS_EXIT_HALT) {
                l1 = "The Pi is shutting down";
                l2 = "Switch the Atari off when the Pi's activity light stops";
            } else {
                l1 = "ASTOS has exited to the Linux console";
                l2 = "To start it again:  sudo systemctl restart pistorm";
            }
            int dim = g_mono() ? 1 : 2;
            ss_palette(pal);
            memset(ss.shadow, 0, sizeof ss.shadow);
            ss_puts(&ss, (SS_COLS - (int)strlen(l1)) / 2, 11, l1, 1, 0);
            ss_puts(&ss, (SS_COLS - (int)strlen(l2)) / 2, 13, l2, dim, 0);
            ss_flush(&ss);
        }
        return exit_code;
    }
    /* a build or ASTOS starts next: leave the chips as a fresh reset would,
     * it initialises everything itself */
    ps_reset_state_machine();
    ps_pulse_reset();
    return exit_code;
}
