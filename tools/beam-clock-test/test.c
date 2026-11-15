/*
 * tools/beam-clock-test/test.c - host test for platforms/atari/video/beam_clock.c
 *
 * No Pi, no ST: a simulated beam (50 or 60 Hz, crystal on or off nominal)
 * drives the clock through the calls the emulator makes - ipl_task's VBL
 * sightings (0-16 us late, now and then much later), the governor's poll
 * every ~4 us, the real video counter read - on a fake clock, and what the
 * clock answers (bc_de_end, bc_counter_advance) is checked against the
 * simulated beam.
 *
 *   sh tools/beam-clock-test/run.sh
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platforms/atari/video/beam_clock.h"

/* ---- the simulated machine ------------------------------------------ */
static double   NS_PER_CYC;                /* ST cycle, ns (fake ticks are ns) */
static int      HZ = 50, LC, TOTAL, L0, C0, DE_END, RESTART;
static double   P, T0 = 1e9;
static uint64_t fake_t;
static uint32_t base_reg = 0x078000u;      /* what the guest last wrote      */
static uint32_t base_latched[4000];        /* frame k shows base_latched[k]  */
static int      ste_extras, torn_always;
static unsigned seed = 271828;
static uint64_t reads_ok;

uint64_t bc_ticks_host(void) { return fake_t; }
uint32_t bc_test_base(void) { return base_reg; }
int      bc_test_ste_extras(void) { return ste_extras; }
int      bc_test_reg_cls(void) { return HZ; }
int      bc_test_read_sync(uint8_t *v) { *v = HZ == 50 ? 0x02 : 0x00; return 1; }

static double origin(int k) { return T0 + (double)k * P; }
static uint64_t tick(int k, int L, double c) { return (uint64_t)llround(origin(k) + ((double)L * LC + c) * NS_PER_CYC); }

/* the counter at fake_t, as the Shifter's would read */
int bc_test_read_counter(uint32_t *cnt, uint64_t *t)
{
    if (torn_always)
        return 0;
    const double cyc = ((double)fake_t - T0) / NS_PER_CYC;
    const int k = (int)floor(cyc / (TOTAL * (double)LC));
    const double fc = cyc - (double)k * TOTAL * LC;
    const int L = (int)(fc / LC);
    const double c = fc - (double)L * LC;
    uint32_t a;
    if (L > RESTART || (L == RESTART && c >= 56))
        a = base_latched[k + 1];                  /* reloaded for the next frame */
    else {
        a = base_latched[k];
        int lines = L - L0;
        if (lines < 0) lines = 0;
        if (lines > 200) lines = 200;
        a += (uint32_t)lines * 160u;
        if (L >= L0 && L < L0 + 200 && c >= C0) {
            int b = (int)((c - C0) / 2.0) & ~1;
            if (b > 160) b = 160;
            a += (uint32_t)b;
        }
    }
    *cnt = a;
    *t = fake_t;
    reads_ok++;
    return 1;
}

/* ---- checks ----------------------------------------------------------- */
static int fails, checks;
static void expect(const char *what, int ok)
{
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL %s\n", what);
    }
}

/* base writes per frame: 0 none, 1 double buffer written at line 150 (before
 * the reload: the next frame shows it), 2 written at line 312 (after the
 * reload, before the VBL: the frame after next shows it) */
static int dbuf;
/* polls held off during the display enable of lines 80-180 (a handler
 * running there): every read lands in a border, where the counter shows a
 * line's first word */
static int hold_de;

/* run frames [k0, k1): VBL sighting, the governor's polls every 4 us */
static void run(int k0, int k1, double lag_us_max)
{
    for (int k = k0; k < k1; k++) {
        /* the base this frame's reload takes: what was written before
         * line RESTART of the frame before */
        const double vbl = origin(k) + 64.0 * NS_PER_CYC;
        seed = seed * 1103515245u + 12345u;
        double lag = (double)((seed >> 16) % 16000u);
        if (((seed >> 8) & 15) == 0)
            lag = (double)((seed >> 12) % 1000u) * lag_us_max;
        const double sight_t = vbl + lag;
        int sighted = 0;
        const double end = origin(k + 1);
        int wrote150 = 0, wrote312 = 0;
        for (double t = origin(k); t < end; t += 4000.0) {
            const double fc = (t - origin(k)) / NS_PER_CYC;
            const int L = (int)(fc / LC);
            if (dbuf == 1 && !wrote150 && L >= 150) {
                wrote150 = 1;
                base_reg = base_reg == 0x078000u ? 0x070000u : 0x078000u;
            }
            if (dbuf == 2 && !wrote312 && L >= RESTART + 2) {
                wrote312 = 1;
                base_reg = base_reg == 0x078000u ? 0x070000u : 0x078000u;
            }
            if (L == RESTART && fc - (double)L * LC >= 56 && base_latched[k + 1] == 0xFFFFFFFFu)
                base_latched[k + 1] = base_reg;
            if (!sighted && t >= sight_t) {
                fake_t = (uint64_t)sight_t;
                bc_vbl_sighting();
                sighted = 1;
            }
            fake_t = (uint64_t)t;
            const double lc_c = fc - (double)L * LC;
            if (hold_de && L >= 80 && L < 182 && lc_c >= C0 - 8 && lc_c < DE_END + 24)
                continue;
            bc_poll();
        }
    }
}

static void probes(int k, const char *tag)
{
    char w[160];
    double de, tc;
    const int last = L0 + 199;
    /* in the border after the last-but-one line, and early in the last */
    const double pr[2][2] = { { last - 1, DE_END + 74 }, { last, 40 } };
    for (int i = 0; i < 2; i++) {
        const int ok = bc_de_end(tick(k, (int)pr[i][0], pr[i][1]), &de, &tc);
        const double want = (double)tick(k, last - 1, DE_END);
        snprintf(w, sizeof w, "%s: model at line %d cyc %d", tag, (int)pr[i][0], (int)pr[i][1]);
        expect(w, ok);
        snprintf(w, sizeof w, "%s: display end within 4 cycles (off %.1f)", tag,
                 ok ? (de - want) / NS_PER_CYC : 0.0);
        expect(w, ok && fabs(de - want) / NS_PER_CYC <= 4.0);
        if (getenv("BC_TEST_SHOW") && ok)
            printf("  %s: display end off %.2f cyc\n", tag, (de - want) / NS_PER_CYC);
        if (i == 0) {
            snprintf(w, sizeof w, "%s: ticks per cycle", tag);
            expect(w, ok && fabs(tc / NS_PER_CYC - 1.0) < 1e-4);
        }
    }
    int a = bc_counter_advance(tick(k, last - 1, DE_END + 120), tick(k, last, C0 + 44));
    snprintf(w, sizeof w, "%s: advance border -> 44 cycles into display = 22 +/-2 (got %d)", tag, a);
    expect(w, a >= 20 && a <= 24);
    a = bc_counter_advance(tick(k, last - 1, DE_END + 20), tick(k, last - 1, DE_END + 100));
    snprintf(w, sizeof w, "%s: advance inside the border = 0 (got %d)", tag, a);
    expect(w, a == 0);
    a = bc_counter_advance(tick(k, L0 + 50, C0 + 100), tick(k, L0 + 51, C0 + 100));
    snprintf(w, sizeof w, "%s: advance one display line = 160 +/-2 (got %d)", tag, a);
    expect(w, a >= 158 && a <= 162);
    a = bc_counter_advance(tick(k, L0 + 50, 0), tick(k, L0 + 60, 0));
    snprintf(w, sizeof w, "%s: advance over 10 lines = unknown (got %d)", tag, a);
    expect(w, a == -1);
    a = bc_counter_advance(tick(k - 1, RESTART, 40), tick(k, 2, 0));
    snprintf(w, sizeof w, "%s: advance across the counter reload = unknown (got %d)", tag, a);
    expect(w, a == -1);
    a = bc_counter_advance(tick(k - 1, TOTAL - 1, 100), tick(k, 0, 200));
    snprintf(w, sizeof w, "%s: advance from the frame before, after the reload = 0 (got %d)", tag, a);
    expect(w, a == 0);
}

static void setup(int hz, double ppm)
{
    HZ = hz;
    LC = hz == 50 ? 512 : 508;
    TOTAL = hz == 50 ? 313 : 263;
    L0 = hz == 50 ? 63 : 34;
    C0 = hz == 50 ? 56 : 52;
    DE_END = hz == 50 ? 376 : 372;
    RESTART = hz == 50 ? 310 : 260;
    NS_PER_CYC = 1e9 / ((hz == 50 ? 8021247.0 : 8010613.0) * (1.0 + ppm * 1e-6));
    P = (double)TOTAL * LC * NS_PER_CYC;
    for (int i = 0; i < 4000; i++)
        base_latched[i] = 0xFFFFFFFFu;
    base_latched[0] = base_reg;
}

int main(int argc, char **argv)
{
    const char *sc = argc > 1 ? argv[1] : "50";
    double ppm = getenv("BC_TEST_PPM") ? atof(getenv("BC_TEST_PPM")) : 0.0;
    if (!strcmp(sc, "50") || !strcmp(sc, "60")) {
        setup(atoi(sc), ppm);
        run(0, 300, 200.0);
        char tag[64];
        snprintf(tag, sizeof tag, "%s Hz%s", sc, ppm ? " off-nominal crystal" : "");
        expect("locked", bc_locked_hz() == HZ);
        probes(299, tag);
        run(300, 600, 200.0);
        probes(599, tag);
    } else if (!strcmp(sc, "dbuf150") || !strcmp(sc, "dbuf312")) {
        setup(50, ppm);
        dbuf = !strcmp(sc, "dbuf150") ? 1 : 2;
        run(0, 300, 200.0);
        expect("locked", bc_locked_hz() == 50);
        probes(299, sc);
    } else if (!strcmp(sc, "ste")) {
        /* an STE line width: no read fits, so no model - never a wrong one */
        setup(50, ppm);
        ste_extras = 1;
        run(0, 300, 200.0);
        double de, tc;
        expect("STE line width: no model", !bc_de_end(tick(299, 200, 400), &de, &tc));
        expect("STE line width: no counter advance", bc_counter_advance(tick(299, 200, 100), tick(299, 200, 200)) == -1);
    } else if (!strcmp(sc, "border")) {
        /* every read in a border: none fits a cycle - no model, never a
         * wrong one */
        setup(50, ppm);
        hold_de = 1;
        run(0, 300, 200.0);
        double de, tc;
        const int ok = bc_de_end(tick(299, 261, 450), &de, &tc);
        const double want = (double)tick(299, 261, DE_END);
        char w[120];
        snprintf(w, sizeof w, "reads in the border only: no model, or a right one (ok %d off %.1f)",
                 ok, ok ? (de - want) / NS_PER_CYC : 0.0);
        expect(w, !ok || fabs(de - want) / NS_PER_CYC <= 4.0);
    } else if (!strcmp(sc, "noread")) {
        /* reads stop (every one torn): the model goes after 0.5 s */
        setup(50, ppm);
        run(0, 200, 200.0);
        double de, tc;
        expect("reads ok: model", bc_de_end(tick(199, 200, 400), &de, &tc));
        torn_always = 1;
        run(200, 240, 200.0);            /* 0.8 s */
        expect("no reads for 0.8 s: no model", !bc_de_end(tick(239, 200, 400), &de, &tc));
        torn_always = 0;
        run(240, 300, 200.0);
        expect("reads back: model again", bc_de_end(tick(299, 200, 400), &de, &tc));
        probes(299, "reads back");
    } else {
        printf("scenarios: 50 60 dbuf150 dbuf312 ste border noread\n");
        return 2;
    }
    printf("%s%s: %d checks, %d failed (%llu counter reads)\n", sc, ppm ? " ppm" : "", checks, fails,
           (unsigned long long)reads_ok);
    return fails ? 1 : 0;
}
