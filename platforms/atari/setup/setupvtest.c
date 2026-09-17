/*
 * setupvtest - step 1 of the pre-boot setup page: bring up the real
 * shifter from the Pi, detect mono / colour, draw a test pattern, and
 * time how fast the screen can be written over the bus.
 *
 *   sudo systemctl stop pistorm     (the emulator must not own the bus)
 *   ./setupvtest [--mono|--colour] [--60hz] [--hold SECONDS]
 *
 * Output is a short report on the console; the pattern stays on the ST
 * monitor until the hold ends (default: until Enter).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gpio/ps_protocol.h"
#include "gpio/bus_lock.h"
#include "shifter_setup.h"

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
    int hz50 = 1, hold = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mono"))        force = SS_MODE_MONO;
        else if (!strcmp(argv[i], "--colour")) force = SS_MODE_COLOUR;
        else if (!strcmp(argv[i], "--60hz"))   hz50 = 0;
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--mono|--colour] [--60hz] [--hold SECONDS]\n", argv[0]);
            return 2;
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

    if (hold < 0) {
        printf("\npattern is on the ST monitor - press Enter to finish\n");
        getchar();
    } else {
        sleep((unsigned)hold);
    }
    return 0;
}
