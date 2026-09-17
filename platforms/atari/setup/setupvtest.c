/*
 * setupvtest - step 1 of the pre-boot setup page: bring up the real
 * shifter from the Pi, detect mono / colour, draw a test pattern, and
 * time how fast the screen can be written over the bus.
 *
 *   sudo systemctl stop pistorm     (the emulator must not own the bus)
 *   ./setupvtest [--mono|--colour] [--60hz] [--hold SECONDS] [--pattern]
 *                [--no-st-kbd] [--input-debug] [--input-only]
 *                [--cfg PATH] [--timings]
 *
 * By default it brings the screen up and runs the real setup page against
 * ~/configs/psctrl.cfg (or --cfg). --timings runs the bus measurements and
 * the layout mock-up instead, --pattern leaves the step 1 test card up.
 *
 * Output is a short report on the console; the pattern stays on the ST
 * monitor until the hold ends (default: until Enter).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>

#include "gpio/ps_protocol.h"
#include "gpio/bus_lock.h"
#include "shifter_setup.h"
#include "setup_input.h"
#include "setup_page.h"

static struct ss_screen scr;

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void pattern(struct ss_screen *ss, int phase)
{
    int w = ss->width, h = ss->height;
    int ink = ss->planes == 1 ? 1 : 3;

    ss_clear(ss, 0);
    ss_fill(ss, 0, 0, w, 2, ink);           /* 2 px border */
    ss_fill(ss, 0, h - 2, w, 2, ink);
    ss_fill(ss, 0, 0, 2, h, ink);
    ss_fill(ss, w - 2, 0, 2, h, ink);

    if (ss->planes == 2) {                  /* four colour bars, top half */
        for (int c = 0; c < 4; c++)
            ss_fill(ss, 16 + c * 152, 8, 144, h / 2 - 16, c);
    } else {                                /* 1, 2, 4, 8 px stripes */
        for (int s = 0; s < 4; s++) {
            int stripe = 1 << s;
            for (int x = 0; x < 144; x++)
                if ((x / stripe) & 1)
                    ss_fill(ss, 16 + s * 152 + x, 8, 1, h / 2 - 16, ink);
        }
    }

    int cell = ss->planes == 1 ? 16 : 8;    /* 16 px wide; 8 rows in colour = square on a colour monitor */
    for (int y = h / 2; y < h - 8; y += cell)
        for (int x = 16; x < w - 16; x += 16)
            if ((((x - 16) / 16) + ((y - h / 2) / cell) + phase) & 1)
                ss_fill(ss, x, y, 16, cell, ink);
}

/* Day, date and time from the Pi, right-aligned in the title bar. */
static void ss_clock(struct ss_screen *ss)
{
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%a %d %b %Y  %H:%M:%S", &tm);
    ss_puts(ss, SS_COLS - 2 - (int)strlen(stamp), 0, stamp, 0,
            ss->planes == 1 ? 1 : 3);
}

/* A mock-up of the setup page: a boot picker and a few settings rows,
 * driven by whichever keyboard or gamepad is attached. */
struct page {
    int sel;            /* 0,1 = boot choices; 2.. = settings rows */
    int boot;           /* 0 GEM, 1 APJ-OS                         */
    int jit_power;
    int cache_mb;
    int secs;           /* countdown, -1 once a key has arrived    */
    char last[48];
};

#define PAGE_ROWS 6     /* 2 boot choices + 4 settings rows        */

static void text_page(struct ss_screen *ss, const struct page *p)
{
    int ink = ss->planes == 1 ? 1 : 3, paper = 0, hi = ss->planes == 1 ? 1 : 2;
    char line[SS_COLS + 1];

    ss_clear(ss, paper);
    ss_clear_row(ss, 0, ink);
    ss_puts(ss, 2, 0, "PSCTRL PiSTorm Setup", paper, ink);

    ss_puts(ss, 2, 2, "Boot:", ink, paper);
    for (int i = 0; i < 2; i++) {
        int on = p->sel == i;
        snprintf(line, sizeof line, " %s %-36s",
                 p->boot == i ? "*" : " ", i == 0 ? "GEM" : "APJ-OS");
        ss_clear_row(ss, 3 + i, paper);
        ss_puts(ss, 4, 3 + i, line, on ? paper : ink, on ? hi : paper);
    }

    ss_puts(ss, 2, 6, "Settings:", ink, paper);
    static const char *names[4] = { "JIT power", "Translation cache",
                                    "TT-RAM", "Blitter bus cost" };
    for (int i = 0; i < 4; i++) {
        char val[24];
        switch (i) {
        case 0: snprintf(val, sizeof val, "%d", p->jit_power); break;
        case 1: snprintf(val, sizeof val, "%d K", p->cache_mb * 1024); break;
        case 2: snprintf(val, sizeof val, "128 M"); break;
        default: snprintf(val, sizeof val, "instant"); break;
        }
        int on = p->sel == 2 + i, grey = i == 3;   /* row 3 is GEM only */
        snprintf(line, sizeof line, "%-20s%-10s%-14s", names[i], val,
                 i == 0 ? "live" : i == 1 ? "restart" :
                 i == 2 ? "boot" : "(GEM only)");
        ss_clear_row(ss, 7 + i, paper);
        ss_puts(ss, 4, 7 + i, line, on ? paper : (grey ? hi : ink),
                on ? hi : paper);
    }

    snprintf(line, sizeof line, "input: %s%d USB keyboard%s, %d gamepad%s",
             si_have_st() ? "ST keyboard, " : "", si_usb_keyboards(),
             si_usb_keyboards() == 1 ? "" : "s", si_gamepads(),
             si_gamepads() == 1 ? "" : "s");
    ss_puts(ss, 2, SS_ROWS - 5, line, ink, paper);
    snprintf(line, sizeof line, "last key: %-40s", p->last);
    ss_puts(ss, 2, SS_ROWS - 4, line, ink, paper);
    ss_puts(ss, 2, SS_ROWS - 3,
            "Up/Down select  Left/Right change  Enter/A pick  F10, Esc or pad X finish",
            ink, paper);
    ss_clear_row(ss, SS_ROWS - 2, paper);
    if (p->secs >= 0)
        snprintf(line, sizeof line, "Booting %s in %d...",
                 p->boot ? "APJ-OS" : "GEM", p->secs);
    else
        snprintf(line, sizeof line, "countdown stopped - %s selected",
                 p->boot ? "APJ-OS" : "GEM");
    ss_puts(ss, 2, SS_ROWS - 2, line, ink, paper);
    ss_clock(ss);
}

static void countdown_block(struct ss_screen *ss, int n)
{
    /* a text-line-sized strip: 8 rows colour / 16 rows mono = 1280 bytes */
    int rows = ss->planes == 1 ? 16 : 8;
    int y = ss->height / 2 - rows - 2;
    ss_fill(ss, 16, y, ss->width - 32, rows, 0);
    ss_fill(ss, 16 + (n % 38) * 16, y, 16, rows, 1);
}

int main(int argc, char **argv)
{
    enum ss_mode force = SS_MODE_AUTO;
    int hz50 = 1, hold = -1, pattern_only = 0, no_st_kbd = 0;
    int input_debug = 0, input_only = 0, timings = 0;
    const char *cfg = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mono"))        force = SS_MODE_MONO;
        else if (!strcmp(argv[i], "--colour")) force = SS_MODE_COLOUR;
        else if (!strcmp(argv[i], "--60hz"))   hz50 = 0;
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pattern")) pattern_only = 1;
        else if (!strcmp(argv[i], "--no-st-kbd")) no_st_kbd = 1;
        else if (!strcmp(argv[i], "--input-debug")) input_debug = 1;
        else if (!strcmp(argv[i], "--input-only")) input_only = input_debug = 1;
        else if (!strcmp(argv[i], "--timings")) timings = 1;
        else if (!strcmp(argv[i], "--cfg") && i + 1 < argc) cfg = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--mono|--colour] [--60hz] [--hold SECONDS]"
                            " [--pattern] [--no-st-kbd]\n"
                            "       %s --input-only [--input-debug]"
                            "   (keys only, screen untouched)\n",
                            argv[0], argv[0]);
            return 2;
        }
    }

    if (input_debug)
        si_set_debug(1);

    /* --input-only: no bus, no screen - just say what was found and
     * print every key as it arrives. For diagnosing a quiet gamepad. */
    if (input_only) {
        int n = si_open(0, 0);
        printf("%d device(s) kept: %d USB keyboard(s), %d gamepad(s)\n",
               n, si_usb_keyboards(), si_gamepads());
        for (int i = 0; i < si_device_count(); i++) {
            int is_pad = 0;
            const char *nm = si_device_name(i, &is_pad);
            printf("  %-9s %s\n", is_pad ? "gamepad" : "keyboard", nm);
        }
        puts("press keys or move the pad; ^C to stop");
        for (;;) {
            struct si_event e = si_poll(500);
            if (e.key != SI_NONE) {
                char buf[48];
                printf("key: %s\n", si_key_name(&e, buf, sizeof buf));
                fflush(stdout);
            }
        }
    }

    if (pistorm_bus_lock("setupvtest") != 0)
        return 1;

    ps_setup_protocol();
    ps_pulse_halt();
    ps_reset_state_machine();
    ps_pulse_reset();
    usleep(250000);                          /* let GLUE/MFP settle */

    if (ss_bringup(&scr, force, hz50) != 0) {
        fprintf(stderr, "bank 0 not detected - check the board\n");
        return 1;
    }
    printf("RAM      bank0 %uK  bank1 %uK  memcfg $%02X\n",
           scr.bank0 >> 10, scr.bank1 >> 10, scr.memcfg);
    printf("monitor  GPIP7=%d -> %s%s, %s\n", scr.gpip7,
           scr.mode == SS_MODE_MONO ? "mono 640x400" : "colour 640x200",
           force ? " (forced)" : "", hz50 ? "50 Hz" : "60 Hz");

    if (!timings && !pattern_only) {
        char path[512], chosen[32] = "";
        if (!cfg) {
            const char *home = getenv("HOME");
            snprintf(path, sizeof path, "%s/configs/psctrl.cfg",
                     home ? home : "/home/pistorm");
            cfg = path;
        }
        int n = si_open(!no_st_kbd, 1);
        printf("input: %d source(s) - ST keyboard %s, %d USB keyboard(s), "
               "%d gamepad(s)\n", n, si_have_st() ? "yes" : "no",
               si_usb_keyboards(), si_gamepads());
        printf("config: %s\n", cfg);

        enum sp_result r = sp_run(&scr, cfg, chosen, sizeof chosen);
        si_close();
        switch (r) {
        case SP_BOOT:
            printf("boot: [%s]\n", chosen);
            return 0;
        case SP_QUIT:
            printf("left without booting (section was [%s])\n", chosen);
            return 0;
        default:
            printf("could not read %s\n", cfg);
            return 1;
        }
    }

    static const struct { enum ss_width w; const char *name; } widths[] = {
        { SS_W8, "8-bit " }, { SS_W16, "16-bit" }, { SS_W32, "32-bit" },
    };
    printf("\nfull screen, 32000 bytes:\n");
    for (unsigned k = 0; k < 3; k++) {
        pattern(&scr, (int)k);
        double t0 = now_ms();
        ss_write_full(&scr, widths[k].w);
        double ms = now_ms() - t0;
        uint32_t bad = ss_verify(&scr);
        printf("  %s writes  %7.1f ms  %6.0f KB/s  verify: %u bad bytes\n",
               widths[k].name, ms, 32000.0 / 1024.0 / (ms / 1000.0), bad);
    }

    printf("\nchanged-only flush (ss_flush):\n");
    pattern(&scr, 1);
    double t0 = now_ms();
    uint32_t sent = ss_flush(&scr);
    printf("  checker phase flip   %6u bytes  %7.1f ms\n", sent, now_ms() - t0);

    const int N = 100;
    uint32_t total = 0;
    t0 = now_ms();
    for (int n = 0; n < N; n++) {
        countdown_block(&scr, n);
        total += ss_flush(&scr);
    }
    double per = (now_ms() - t0) / N;
    printf("  one text line x%d    %6u bytes  %7.2f ms each\n", N, total / N, per);
    printf("  verify after flushes: %u bad bytes\n", ss_verify(&scr));

    struct page pg = { 0, 0, 4, 16, 5, "-" };
    if (pattern_only) {
        printf("\n--pattern: leaving the test pattern up\n");
        si_open(!no_st_kbd, 1);
        goto hold_it;
    }

    printf("\ntext page (80x25 grid, 8x%d cells):\n", scr.planes == 1 ? 16 : 8);
    text_page(&scr, &pg);
    t0 = now_ms();
    ss_write_full(&scr, SS_W32);
    printf("  full page            %6u bytes  %7.1f ms\n", SS_SCREEN_BYTES,
           now_ms() - t0);
    t0 = now_ms();
    total = 0;
    for (int n = 5; n > 0; n--) {
        pg.secs = n;
        text_page(&scr, &pg);
        total += ss_flush(&scr);
    }
    printf("  countdown tick x5    %6u bytes  %7.2f ms each\n", total / 5,
           (now_ms() - t0) / 5);
    printf("  verify: %u bad bytes\n", ss_verify(&scr));

    int nsrc = si_open(!no_st_kbd, 1);
    printf("\ninput: %d source(s) - ST keyboard %s, %d USB keyboard(s), %d gamepad(s)\n",
           nsrc, si_have_st() ? "yes" : "no", si_usb_keyboards(), si_gamepads());

hold_it:
    printf("the page is on the ST monitor. Drive it with any keyboard or the\n"
           "gamepad; F10, Esc or Enter here finishes.\n");
    pg.secs = 5;
    double tick = now_ms();
    for (double t_end = now_ms() + (hold < 0 ? 1e12 : hold * 1000.0);
         now_ms() < t_end; ) {
        struct si_event e = si_poll(100);
        if (e.key != SI_NONE) {
            si_key_name(&e, pg.last, sizeof pg.last);
            pg.secs = -1;                     /* any key stops the countdown */
            switch (e.key) {
            case SI_UP:    pg.sel = (pg.sel + PAGE_ROWS - 1) % PAGE_ROWS; break;
            case SI_DOWN:  pg.sel = (pg.sel + 1) % PAGE_ROWS;             break;
            case SI_LEFT:
            case SI_RIGHT: {
                int d = e.key == SI_RIGHT ? 1 : -1;
                if (pg.sel == 0 || pg.sel == 1) pg.boot = pg.sel;
                else if (pg.sel == 2) pg.jit_power = (pg.jit_power + d + 7) % 7;
                else if (pg.sel == 3) pg.cache_mb = pg.cache_mb == 16 ? 8 : 16;
                break;
            }
            case SI_ENTER: if (pg.sel < 2) pg.boot = pg.sel;              break;
            case SI_ESC:
            case SI_F10:   goto done;
            default: break;
            }
        }
        /* stdin ends it too, for a session over ssh */
        struct timeval tv = { 0, 0 };
        fd_set r;
        FD_ZERO(&r); FD_SET(0, &r);
        if (select(1, &r, NULL, NULL, &tv) > 0)
            break;
        if (pattern_only) {
            ss_clock(&scr);
            ss_flush(&scr);
            continue;
        }
        if (pg.secs > 0 && now_ms() - tick >= 1000.0) {
            tick = now_ms();
            if (--pg.secs == 0)
                pg.secs = 5;                  /* demo: just wraps round */
        }
        text_page(&scr, &pg);
        ss_flush(&scr);
    }
done:
    si_close();
    return 0;
}
