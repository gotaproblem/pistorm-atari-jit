/*
 * beam_clock.c - where the real ST beam is, for the CPU thread
 *
 * BEAM-TIMED HANDLERS (cpu/events.cpp) put a Timer B raster handler on the
 * beam's clock, and BACK-DATED READS (pistorm_natmem.cpp) answer a video
 * counter read the CPU thread reached late with the value it had at the
 * guest's time. Both need to know where the beam is, with the native
 * Shifter alone - no HDMI. This is that clock:
 *
 *   1. ipl_task stamps every VBL it sees (bc_vbl_sighting, 50 times a
 *      second: one clock read and three stores). Sightings are never early,
 *      so the lower envelope of their residuals on a fixed period is the
 *      true VBL; their spacing and the guest's sync/rez registers give the
 *      frame rate (50 or 60 Hz; 71 Hz mono is not modelled).
 *   2. Once a frame, mid-display (lines 80-180) and outside any interrupt
 *      handler, the CPU thread reads the real video counter
 *      (pistorm_shm_read_counter: four quiet bus reads, ~2-3 us). With the
 *      screen base the guest wrote, the counter says the line and cycle the
 *      beam was on - by the normal layout (160 bytes a line, display enable
 *      from cycle 56 / 52, one word every 4 cycles). That fixes the frame's
 *      origin to the counter's 4-cycle step; a second-order loop then
 *      follows the ST's crystal against the Pi's.
 *   3. A read that does not fit a normal line is not used: an STE line width
 *      or fine scroll, a base the guest changed between the counter reload
 *      and the read (all three candidate bases are tried: as at the last
 *      two VBLs and as now), or a line more than two lines from where the
 *      model expects it. With no read accepted for 0.5 s the clock says
 *      "no model" and the callers do what they did before it existed.
 *
 * The method is the HDMI mirror's (shifter_mirror.c: lock_sighting,
 * cal_process), which places reads with the whole frame's GLUE pass; this
 * one needs none of the picture, so it runs with the mirror off too. The
 * numbers that matter - the VBL-to-origin offset (-86 cycles until it is
 * measured), the counter's read latency (+2 cycles into the word's slot) -
 * are the mirror's, measured on the hardware (2 Nov 2026).
 *
 *   PISTORM_BEAM_CLOCK=0   off: no counter reads, no model
 *   PISTORM_BEAM_DEBUG=1   a summary line every 5 s
 *
 * Host test: tools/beam-clock-test.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(BC_TEST)
#include <stdbool.h>
#include <time.h>
#endif

#include "platforms/atari/video/beam_clock.h"

/* ---- what the clock reads ------------------------------------------------ */
#if !defined(BC_TEST)
#include "platforms/atari/et4000/et4000.h"          /* rtg_s */
extern rtg_s rtg;
extern volatile double pistorm_guest_hz;            /* pistorm_natmem.cpp */
extern bool emulator_config_shifter_ste(void);
int pistorm_shm_read_counter(uint32_t *cnt, uint64_t *t);   /* pistorm_natmem.cpp */
int pistorm_shm_read_sync(uint8_t *v);

#if !defined(__aarch64__)
uint64_t bc_ticks_host(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

static uint32_t in_base(void)                       /* the base as the guest wrote it */
{
    uint32_t b = ((uint32_t)rtg.high << 16) | ((uint32_t)rtg.mid << 8);
    if (emulator_config_shifter_ste())
        b |= (uint32_t)(rtg.low & 0xFEu);
    return b;
}
static int in_ste_extras(void)
{
    return emulator_config_shifter_ste() && (rtg.linewidth || (rtg.hscroll & 15u));
}
static int in_reg_cls(void)                         /* 50 / 60 / 71, 0 = never written */
{
    if ((rtg.hw_rez & 3u) == 2u)
        return 71;
    const double hz = pistorm_guest_hz;
    return (hz > 55.0 && hz < 65.0) ? 60 : (hz > 45.0 && hz <= 55.0) ? 50 :
           (hz > 65.0) ? 71 : 0;
}
#define in_read_counter pistorm_shm_read_counter
#define in_read_sync    pistorm_shm_read_sync
#else
uint32_t bc_test_base(void);
int      bc_test_ste_extras(void);
int      bc_test_reg_cls(void);
int      bc_test_read_counter(uint32_t *cnt, uint64_t *t);
int      bc_test_read_sync(uint8_t *v);
#define in_base         bc_test_base
#define in_ste_extras   bc_test_ste_extras
#define in_reg_cls      bc_test_reg_cls
#define in_read_counter bc_test_read_counter
#define in_read_sync    bc_test_read_sync
#endif

/* ---- VBL sightings: ipl_task -> CPU thread ------------------------------- */
#define SIGHT_N 64u
static uint64_t          s_t[SIGHT_N];
static uint8_t           s_cls[SIGHT_N];   /* the registers' rate at the sighting */
static uint32_t          s_base[SIGHT_N];  /* the base as written at the sighting */
volatile uint32_t        bc_sight_w;
uint32_t                 bc_sight_r;
uint64_t                 bc_next = UINT64_MAX;

void bc_vbl_sighting(void)
{
    const uint32_t w = bc_sight_w;
    s_t[w % SIGHT_N] = bc_ticks();
    s_cls[w % SIGHT_N] = (uint8_t)in_reg_cls();
    s_base[w % SIGHT_N] = in_base();
    __atomic_store_n(&bc_sight_w, w + 1u, __ATOMIC_RELEASE);
}

/* ---- the model (CPU thread only) ----------------------------------------- */
static int      on = -1, dbg;
static int      L_cls;                   /* 0 / 50 / 60 (71: no lock)        */
static double   L_ema, L_P, L_A;         /* interval avg, period, VBL anchor  */
static int      L_have, L_wn;
static uint64_t L_last;
static double   L_win[32];
static double   C_A;                     /* a calibrated frame origin         */
static uint64_t C_when;                  /* tick of the read it came from     */
static int      C_good;                  /* reads accepted since a re-anchor  */
static double   C_ofs;                   /* origin - VBL envelope, measured   */
static int      C_ofs_n, rej_run;
static int      glue_cls, glue_reads;    /* the GLUE's rate, read when unwritten */
static uint32_t base1, base2;            /* the base at the last two VBLs    */
static uint64_t bc_hi;                   /* the read window's end            */

static struct {
    uint64_t t0, sightings, tries, ok, rej_frame, rej_edge, rej_ste, rej_far, torn;
    uint64_t q_ok, q_none;
    double   emin, emax, esum;
} st;

static double frame_cycles(int c)
{
    return c == 50 ? 313.0 * 512.0 : 263.0 * 508.0;
}

/* the registers' rate over the last 8 sightings: 6 needed, else 0 */
static int reg_vote(int c)
{
    static uint8_t h[8];
    static unsigned n;
    h[n++ & 7u] = (uint8_t)c;
    int v50 = 0, v60 = 0, v71 = 0;
    for (int i = 0; i < 8; i++)
        v50 += h[i] == 50, v60 += h[i] == 60, v71 += h[i] == 71;
    return v50 >= 6 ? 50 : v60 >= 6 ? 60 : v71 >= 6 ? 71 : 0;
}

/* shifter_mirror.c lock_sighting(), without the HDMI refresh */
static void lock_sighting(uint64_t t, int sight_cls)
{
    const double hz = bc_tick_hz();
    st.sightings++;
    if (L_last) {
        const double iv = (double)(t - L_last);
        if (iv > hz / 250.0 && iv < hz / 25.0)
            L_ema = L_ema > 0 ? L_ema + (iv - L_ema) / 32.0 : iv;
    }
    L_last = t;
    const double us = L_ema * 1e6 / hz;
    int cls = (us > 18000 && us < 22000) ? 50 : (us > 15000 && us <= 18000) ? 60 :
              (us > 12500 && us <= 15000) ? 71 : 0;
    /* the GLUE's rate is the registers'; the intervals only confirm it
     * (a re-latched level 4 sighted mid-frame must not make a 50 Hz
     * screen 60) */
    const int reg = reg_vote(sight_cls);
    if (!reg)
        cls = L_cls;
    else if (cls != reg)
        cls = (L_cls == reg) ? L_cls : 0;
    if (cls == 71)
        cls = 0;                             /* mono: not modelled */
    if (cls != L_cls) {
        L_cls = cls;
        L_have = 0;
        C_when = 0;
        C_good = 0;
        if (!cls)
            return;
        L_P = frame_cycles(cls) * hz / 8021247.0;   /* refined below */
    }
    if (!cls)
        return;
    if (!L_have) {
        L_A = (double)t;
        L_have = 1;
        L_wn = 0;
        return;
    }
    /* sightings land in [-P/8, 7P/8) of their VBL */
    const double rel = (double)t - L_A + L_P / 8.0;
    const double k = floor(rel / L_P);
    L_win[L_wn++] = (double)t - (L_A + k * L_P);
    if (L_wn >= 32) {
        /* second smallest residual: the lower envelope, one outlier allowed */
        double m1 = 1e300, m2 = 1e300;
        for (int i = 0; i < L_wn; i++) {
            if (L_win[i] < m1) { m2 = m1; m1 = L_win[i]; }
            else if (L_win[i] < m2) m2 = L_win[i];
        }
        L_A += m2;
        if (!C_when || (double)(t - C_when) > 0.5 * hz)
            L_P += 0.5 * m2 / 32.0;          /* drift - unless the counter steers */
        L_wn = 0;
        const double kk = floor(((double)t - L_A) / L_P);
        if (kk > 0)
            L_A += kk * L_P;
    }
}

/* origin of the frame containing tick x (or the last one before it);
 * *cal = 1 when it comes from a counter read in the last 0.5 s */
static double origin_at(double x, int *cal)
{
    const double hz = bc_tick_hz();
    if (C_when && fabs(x - (double)C_when) < 0.5 * hz) {
        *cal = 1;
        return C_A + floor((x - C_A) / L_P) * L_P;
    }
    *cal = 0;
    const double ofs = C_ofs_n ? C_ofs : -86.0 * L_P / frame_cycles(L_cls);
    const double a = L_A + ofs;
    return a + floor((x - a) / L_P) * L_P;
}

/* the next read window: lines 80-180 of the frame at `from`, or the next.
 * It opens at a different cycle of line 80 each frame, inside the display
 * enable: the first poll after it opening is the read, and a read in the
 * border shows a line's first word, which says nothing about the cycle */
static unsigned sched_n;
static void schedule_(uint64_t from, int next_frame)
{
    if (!L_have || (L_cls != 50 && L_cls != 60)) {
        bc_next = UINT64_MAX;
        return;
    }
    int cal;
    const double lc = L_cls == 50 ? 512.0 : 508.0, tc = L_P / frame_cycles(L_cls);
    const double c0 = L_cls == 50 ? 56.0 : 52.0;
    const double O = origin_at((double)from, &cal);
    const double phase = c0 + 8.0 + (double)((sched_n++ * 53u) % 200u);
    double lo = O + (80.0 * lc + phase) * tc, hi = O + 180.0 * lc * tc;
    if ((double)from > hi || next_frame) {
        lo += L_P;
        hi += L_P;
    }
    bc_next = (uint64_t)lo;
    bc_hi = (uint64_t)hi;
}
static void schedule(uint64_t from) { schedule_(from, 0); }

/* a counter read at tick t: where does it put the frame's origin? */
static void cal_apply(uint64_t t, uint32_t cnt)
{
    if (in_ste_extras()) {
        st.rej_ste++;
        return;
    }
    const int cls = L_cls;
    const double lc = cls == 50 ? 512.0 : 508.0, c0 = cls == 50 ? 56.0 : 52.0;
    const double l0 = cls == 50 ? 63.0 : 34.0;
    const double tc = L_P / frame_cycles(cls);
    int cal;
    const double O = origin_at((double)t, &cal);
    const uint32_t now_base = in_base();
    const uint32_t cand[3] = { base1, base2, now_base };
    double best = 1e300;
    int found = 0, edge = 0;
    uint32_t best_b = 0;
    for (int i = 0; i < 3; i++) {
        if ((i == 1 && cand[1] == cand[0]) || (i == 2 && (cand[2] == cand[0] || cand[2] == cand[1])))
            continue;
        const uint32_t off = cnt - cand[i];
        if (off >= 200u * 160u)
            continue;
        const uint32_t inl = off % 160u;
        if (inl == 0) {                      /* a line's first or last word: ambiguous */
            edge = 1;
            continue;
        }
        const double L = l0 + (double)(off / 160u);
        const double pred = O + (L * lc + c0 + 2.0 * (double)inl + 2.0) * tc;
        const double err = (double)t - pred;
        if (fabs(err) < fabs(best)) {
            best = err;
            best_b = cand[i];
            found = 1;
        }
    }
    if (!found) {
        if (edge) st.rej_edge++; else st.rej_frame++;
        return;
    }
    const double errc = best / tc;
    if (fabs(errc) > 2.0 * lc) {
        /* more than two lines out. The first read, or the model has gone
         * for good (8 in a row): take the counter's word for it - but only
         * with a base that has not changed over the last two VBLs */
        st.rej_far++;
        if ((!C_when || ++rej_run >= 8) && best_b == base1 && base1 == base2 &&
            base1 == now_base) {
            C_A = O + best;
            C_when = t;
            C_good = 0;
            rej_run = 0;
        }
        return;
    }
    rej_run = 0;
    st.ok++;
    st.esum += errc;
    if (st.ok == 1 || errc < st.emin) st.emin = errc;
    if (st.ok == 1 || errc > st.emax) st.emax = errc;
    /* one read is good to the counter's 4-cycle step: small errors are
     * averaged in, larger ones are a real move */
    C_A = O + (fabs(errc) > 8.0 ? best : best / 4.0);
    if (C_when) {
        const double nf = floor(((double)t - (double)C_when) / L_P + 0.5);
        if (nf >= 1.0 && nf < 50.0)
            L_P += best / 16.0 / nf;
    }
    C_when = t;
    C_good++;
    double rel = C_A - L_A;
    rel -= floor(rel / L_P) * L_P;
    if (rel > L_P / 2) rel -= L_P;
    C_ofs = C_ofs_n ? C_ofs + (rel - C_ofs) / 16.0 : rel;
    C_ofs_n++;
}

static void stats_print(uint64_t now)
{
    const double hz = bc_tick_hz();
    if (!st.t0) { st.t0 = now; return; }
    if ((double)(now - st.t0) < 5.0 * hz)
        return;
    fprintf(stderr, "[BEAM] %d Hz P=%.1fus sightings=%llu | counter reads %llu: used %llu "
            "(err %.1f..%.1f avg %.2f cyc), not used: no line %llu, line edge %llu, "
            "STE width/scroll %llu, >2 lines out %llu, torn %llu | asked %llu: model %llu, none %llu\n",
            L_have ? L_cls : 0, L_P * 1e6 / hz, (unsigned long long)st.sightings,
            (unsigned long long)st.tries, (unsigned long long)st.ok,
            st.ok ? st.emin : 0.0, st.ok ? st.emax : 0.0, st.ok ? st.esum / (double)st.ok : 0.0,
            (unsigned long long)st.rej_frame, (unsigned long long)st.rej_edge,
            (unsigned long long)st.rej_ste, (unsigned long long)st.rej_far,
            (unsigned long long)st.torn,
            (unsigned long long)(st.q_ok + st.q_none), (unsigned long long)st.q_ok,
            (unsigned long long)st.q_none);
    memset(&st, 0, sizeof st);
    st.t0 = now;
}

void bc_poll_slow(uint64_t now)
{
    if (on < 0) {
        const char *e = getenv("PISTORM_BEAM_CLOCK");
        on = !(e && *e == '0');
        e = getenv("PISTORM_BEAM_DEBUG");
        dbg = e && *e == '1';
    }
    const uint32_t w = __atomic_load_n(&bc_sight_w, __ATOMIC_ACQUIRE);
    if (!on) {
        bc_sight_r = w;
        bc_next = UINT64_MAX;
        return;
    }
    if (w - bc_sight_r > SIGHT_N)
        bc_sight_r = w - SIGHT_N;
    if (bc_sight_r != w) {
        const int had = L_have ? L_cls : 0;
        for (; bc_sight_r != w; bc_sight_r++) {
            const uint32_t i = bc_sight_r % SIGHT_N;
            int c = s_cls[i];
            if (!c) {
                /* the guest has never written SYNC or REZ (a 60 Hz TOS
                 * never does): the GLUE's own rate, read once */
                if (!glue_cls && glue_reads < 64) {
                    uint8_t v;
                    glue_reads++;
                    if (in_read_sync(&v))
                        glue_cls = (v & 2u) ? 50 : 60;
                }
                c = glue_cls;
            }
            lock_sighting(s_t[i], c);
            base2 = base1;
            base1 = s_base[i];
        }
        if ((L_have ? L_cls : 0) != had || bc_next == UINT64_MAX)
            schedule(now);
    }
    if (now >= bc_next) {
        if (now > bc_hi) {                   /* missed it: the next frame's */
            schedule(now);
            if (now < bc_next)
                goto out;
        }
        uint32_t cnt;
        uint64_t t;
        st.tries++;
        if (in_read_counter(&cnt, &t))
            cal_apply(t, cnt);
        else
            st.torn++;
        schedule_(now, 1);                   /* the frame after this read's */
    }
out:
    if (dbg)
        stats_print(now);
}

/* ---- answers ------------------------------------------------------------- */
/* the frame geometry around tick t, only with a trusted calibration */
static int model(uint64_t t, double *O, double *tc, int *cls)
{
    if (!L_have || (L_cls != 50 && L_cls != 60) || C_good < 2) {
        st.q_none++;
        return 0;
    }
    int cal;
    const double o = origin_at((double)t, &cal);
    if (!cal) {
        st.q_none++;
        return 0;
    }
    st.q_ok++;
    *O = o;
    *tc = L_P / frame_cycles(L_cls);
    *cls = L_cls;
    return 1;
}

int bc_locked_hz(void)
{
    return L_have ? L_cls : 0;
}

int bc_de_end(uint64_t t, double *de, double *tc)
{
    double O, k;
    int cls;
    if (!model(t, &O, &k, &cls))
        return 0;
    const double lc = cls == 50 ? 512.0 : 508.0;
    const double e  = cls == 50 ? 376.0 : 372.0;
    const double c  = ((double)t - O) / k;          /* cycles into the frame */
    double L = floor(c / lc);
    if (c - L * lc < e)
        L -= 1.0;                                    /* this line's has not ended */
    *de = O + (L * lc + e) * k;
    *tc = k;
    return 1;
}

/* display-enable cycles from the frame origin to cycle c of the frame */
static double de_cycles_to(double c, int cls)
{
    const double lc = cls == 50 ? 512.0 : 508.0;
    const double c0 = cls == 50 ? 56.0 : 52.0, c1 = c0 + 320.0;
    const double l0 = cls == 50 ? 63.0 : 34.0, l1 = l0 + 200.0;
    if (c <= 0.0)
        return 0.0;
    const double L = floor(c / lc), x = c - L * lc;
    const double n = L < l0 ? 0.0 : (L > l1 ? l1 : L) - l0;
    double d = n * 320.0;
    if (L >= l0 && L < l1)
        d += (x < c0 ? c0 : x > c1 ? c1 : x) - c0;
    return d;
}

int bc_counter_advance(uint64_t t0, uint64_t t1)
{
    double O, k;
    int cls;
    if (t1 < t0 || !model(t1, &O, &k, &cls))
        return -1;
    const double lc = cls == 50 ? 512.0 : 508.0;
    if ((double)(t1 - t0) > 2.0 * lc * k)
        return -1;                                   /* too far back to trust */
    const double a = ((double)t0 - O) / k, b = ((double)t1 - O) / k;
    if (a < 0.0) {
        /* t0 in the frame before: nothing is fetched in its last lines,
         * but the GLUE reloads the counter from the base at line 310 / 260,
         * cycle 56 - across that the value jumps */
        const int total = cls == 50 ? 313 : 263, restart = cls == 50 ? 310 : 260;
        const double reload = -((double)(total - restart) * lc - 56.0);
        if (a <= reload)
            return -1;
        return 2 * (int)floor(de_cycles_to(b, cls) / 4.0);
    }
    return 2 * (int)floor(de_cycles_to(b, cls) / 4.0) -
           2 * (int)floor(de_cycles_to(a, cls) / 4.0);
}
