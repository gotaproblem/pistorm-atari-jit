/*
 * beamtest - can the Pi put a write on the real ST beam to one 4-cycle
 * bus slot?
 *
 * Go/no-go for the hybrid STBOX: the 68000, MFP and GLUE timing emulated
 * in the Pi, the real Shifter and YM driven by writes timed to the real
 * beam. If writes land in the right bus slot every time, that works; if
 * they don't, the design doesn't.
 *
 *   sudo systemctl stop pistorm
 *   make beamtest
 *   sudo ./beamtest 2>&1 | tee beamtest.log
 *
 * Plain ST or Mega ST, colour monitor, 50 Hz. Nothing of TOS runs: the
 * Pi is the only bus master (as in setupvtest). About 45 seconds:
 *
 *   A  lock   1 s of video counter reads fix the beam's phase and the
 *             ST's crystal against the Pi's timer; the bus write time
 *             is measured.
 *   B  right border sweep: 9 bands of 15 lines, each band places the
 *             60 Hz write at cycle 376 + offset, offsets -16..+16. Only
 *             cycle 376 itself opens the border (a one-slot window);
 *             early gives a 158-byte line, late a normal one. The video
 *             counter, read between bands, says how many lines of each
 *             band opened, landed early or landed late. One band at
 *             100 % with its neighbours at 0 % is the pass mark, and its
 *             offset is the writes' landing error: the tests after this
 *             one are corrected by it.
 *   C  edge   every line turns colour 15 from blue to yellow at cycle
 *             244: a straight edge mid-screen. Ragged = jitter (one
 *             step = 4 pixels = one bus slot). Visual only.
 *   D  top/bottom border: 60 Hz on line 33 and line 262. The video
 *             counter says, every frame, whether each border opened.
 *   E  left border sweep: hi res for 8 cycles from cycle 0 + offset
 *             (a demo's pulse). Hi res at cycle 4 and low again by 16
 *             opens the border (186 bytes); otherwise the line is normal
 *             or blank. Measured the same way.
 *
 * Run 1 on the Pi 4 (22 Nov 2026): right sweep 100 % in one band, 0 % in
 * every other, the writes landing 8 cycles after where they were aimed;
 * top and bottom open in every frame; the left sweep blanked every line,
 * because it held hi res from cycle 470 of the line before.
 *
 * Options: --secs N (per phase, default 10), --core N (default 3),
 *          --lead NS (override the measured write lead),
 *          --cal CYCLES (fix the correction instead of taking it from B).
 *
 * tools/beamtest/sim.c runs the same code against a model ST on any host
 * (build line at its top): the lock, the scheduling and the counter
 * decoding were checked there - right sweep opens at offset 0 only, left
 * at 0/+4, top and bottom open every frame.
 *
 * GLUE positions are Hatari's STF table (wakestate 1), as stbox.c and
 * shifter_mirror.c use them; the counter model and its +2 cycle read
 * latency are shifter_mirror.c's, measured on a Pi 4 + ST on 2 Nov 2026.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gpio/ps_protocol.h"
#include "gpio/bus_lock.h"
#include "shifter_setup.h"

extern uint8_t fc;
extern volatile _Atomic uint8_t g_buserr;

/* ---- the machine -------------------------------------------------------- */
#define BASE        0x00010000u     /* screen base, 256-byte aligned       */
#define FILL_BYTES  0x00020000u     /* > full overscan (230 x 276)         */
#define LINE_CYC    512             /* 50 Hz line                          */
#define FRAME_LINES 313
#define FRAME_CYC   (LINE_CYC * FRAME_LINES)
#define DE_FIRST    63              /* first displayed line, 50 Hz         */
#define DE_FIRST_TOP 34             /* ... with the top border open        */
#define DE_LAST     262
#define ST_HZ_NOM   8021247.0

#define R_SYNC   0x00FF820Au
#define R_REZ    0x00FF8260u
#define R_PAL0   0x00FF8240u
#define R_PAL15  0x00FF825Eu
#define R_CNTHI  0x00FF8205u
#define R_CNTMI  0x00FF8207u
#define R_CNTLO  0x00FF8209u

#define C_BLACK  0x000
#define C_BLUE   0x007
#define C_YELLOW 0x770
#define C_GREEN  0x070

/* ---- time ---------------------------------------------------------------- */
#ifdef BEAMTEST_SIM                 /* tools/beamtest/sim.c: a model ST    */
uint64_t sim_ticks(void);
uint64_t sim_hz(void);
static inline uint64_t ticks(void) { return sim_ticks(); }
static inline uint64_t tick_hz(void) { return sim_hz(); }
#else
static inline uint64_t ticks(void)
{
    uint64_t t;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t) :: "memory");
    return t;
}
static inline uint64_t tick_hz(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}
#endif

static double HZ;           /* Pi timer ticks per second                   */
static double NSPT;         /* ns per tick                                 */
static double TPC;          /* ticks per ST cycle (follows the lock)       */
static double O;            /* tick of frame 0, line 0, cycle 0            */
static double P;            /* ticks per frame                             */
static double LEAD8, LEAD16;/* issue a write this many ticks early         */
static int    lead_forced = -1;
static int    g_cal;        /* cycles: aim writes this far from target   */
static int    cal_forced;
static volatile sig_atomic_t g_stop;

static void on_sigint(int s) { (void)s; g_stop = 1; }

/* ---- histograms (10 ns bins to 50 us) ----------------------------------- */
#define HB 5001
typedef struct { uint32_t b[HB]; uint64_t n; double max, sum; } hist_t;
static void h_add(hist_t *h, double ns)
{
    int i = ns < 0 ? 0 : (int)(ns / 10.0);
    if (i >= HB) i = HB - 1;
    h->b[i]++; h->n++; h->sum += ns;
    if (ns > h->max) h->max = ns;
}
static double h_pct(const hist_t *h, double p)
{
    if (!h->n) return 0;
    uint64_t k = (uint64_t)ceil(p * (double)h->n), s = 0;
    if (k < 1) k = 1;
    for (int i = 0; i < HB; i++) { s += h->b[i]; if (s >= k) return i * 10.0; }
    return h->max;
}
static void h_print(const char *what, const hist_t *h)
{
    printf("  %-22s n=%-8llu p50 %6.0f  p99 %6.0f  p99.9 %6.0f  max %7.0f ns\n",
           what, (unsigned long long)h->n, h_pct(h, 0.5), h_pct(h, 0.99),
           h_pct(h, 0.999), h->max);
}

/* ---- bus ----------------------------------------------------------------- */
/* As pistorm_shm_read_counter: hi, mid, lo, mid again; stamp = middle of
 * the lo read. */
static int read_counter(uint32_t *cnt, uint64_t *t)
{
    g_buserr = 0;
    uint32_t hi  = ps_read_8(R_CNTHI);
    uint32_t mid = ps_read_8(R_CNTMI);
    uint64_t t0  = ticks();
    uint32_t lo  = ps_read_8(R_CNTLO);
    uint64_t t1  = ticks();
    uint32_t mid2 = ps_read_8(R_CNTMI);
    if (g_buserr || mid2 != mid)
        return 0;
    *cnt = (hi << 16) | (mid << 8) | lo;
    *t = t0 + (t1 - t0) / 2;
    return 1;
}

/* frame cycle a counter read saw, on a normal 160-byte line whose display
 * starts at cycle 56 (shifter_mirror.c fetch_tick + 2) */
static inline double cnt_cycle(int line, int off)
{
    return (double)line * LINE_CYC + 56.0 + 2.0 * off + 2.0;
}

/* ---- events -------------------------------------------------------------- */
enum { EV_W8, EV_W16, EV_READ };
enum { RD_TRACK = 0, RD_BOT = 1, RD_BAND0 = 2 };   /* read slots          */
#define NRD 16
typedef struct { int32_t cyc; uint8_t kind, timed; uint16_t slot; uint32_t addr; uint16_t val; } ev_t;
#define NEV 4096
static ev_t ev[NEV];
static int nev;

static void add(int32_t cyc, int kind, uint32_t addr, uint16_t val, int timed, int slot)
{
    if (nev >= NEV) { fprintf(stderr, "event list full\n"); exit(1); }
    ev[nev++] = (ev_t){ cyc, (uint8_t)kind, (uint8_t)timed, (uint16_t)slot, addr, val };
}
static int ev_cmp(const void *a, const void *b)
{
    return ((const ev_t *)a)->cyc - ((const ev_t *)b)->cyc;
}

/* ---- the sweep ----------------------------------------------------------- */
#define NBAND     9
#define BAND_L0   100               /* first band's clean (read) line      */
#define BAND_H    16                /* 1 clean line + 15 trick lines        */
#define BAND_TL   (BAND_H - 1)
static const uint16_t band_col[NBAND] =
    { 0x700, 0x740, 0x770, 0x070, 0x077, 0x007, 0x707, 0x777, 0x444 };
static const char *band_name[NBAND] =
    { "red", "orange", "yellow", "green", "cyan", "blue", "magenta", "white", "grey" };
static inline int band_ofs(int b) { return -16 + 4 * b; }

/* ---- per-phase results --------------------------------------------------- */
typedef struct {
    hist_t late, dur8, dur16;       /* timed writes: issue lateness, time  */
    uint64_t frames, skipped, gaps, late1us;
    /* lock tracking (cycles) */
    uint64_t trk_n, trk_rej, trk_relock;
    double trk_sum, trk_sq, trk_min, trk_max;
    /* phase C */
    uint64_t top_open, top_shut, top_bad, bot_open, bot_shut, bot_bad;
    /* phases D/E, per band */
    uint64_t hit[NBAND], early[NBAND], late_[NBAND], undec[NBAND];
    double   dsum[NBAND];
    uint64_t dn[NBAND];
} res_t;
static res_t R;

/* the read results of the current frame */
static uint32_t rd_cnt[NRD];
static uint64_t rd_t[NRD];
static uint8_t  rd_ok[NRD];

/* where in the frame a tick is, in cycles, for frame n */
static inline double frame_cyc(uint64_t t, long n) { return ((double)t - (O + (double)n * P)) / TPC; }

/* Lock tracking from the line-80 read: the frame origin follows each read
 * (a quarter of the error, all of it beyond 8 cycles), the period a
 * sixteenth - shifter_mirror.c's second-order loop. de_first = the line
 * display starts on (63, or 34 with the top border open). */
static double track(long n, int de_first)
{
    double cm = frame_cyc(rd_t[RD_TRACK], n);
    long off = (long)rd_cnt[RD_TRACK] - (long)BASE - (long)(80 - de_first) * 160;
    if (off <= 0 || off >= 160) {
        R.trk_rej++;
        return 1e9;
    }
    double e = cm - cnt_cycle(80, (int)off);    /* + = model is late        */
    if (fabs(e) > 64.0) {
        R.trk_rej++;
        static int run;
        if (++run >= 8) { O += e * TPC; run = 0; R.trk_relock++; }
        return e;
    }
    /* move this frame's origin, then change the period about it: O is
     * frame 0's origin, so a new P must not move frame n's */
    double On = O + (double)n * P + (fabs(e) > 8.0 ? e : e / 4.0) * TPC;
    P += e * TPC / 16.0;
    TPC = P / FRAME_CYC;
    O = On - (double)n * P;
    R.trk_n++; R.trk_sum += e; R.trk_sq += e * e;
    if (R.trk_n == 1 || e < R.trk_min) R.trk_min = e;
    if (R.trk_n == 1 || e > R.trk_max) R.trk_max = e;
    return e;
}

/* bytes the Shifter fetched before the line of read slot s, given the
 * frame model: the counter less the offset into its (clean) line */
static int bytes_before(int s, long n, int line)
{
    double cin = frame_cyc(rd_t[s], n) - (double)line * LINE_CYC;
    long off = lround((cin - 58.0) / 2.0);
    long v = (long)rd_cnt[s] - (long)BASE - off;
    return (int)(v & ~1L);          /* the counter is even                 */
}

/* Split a band's byte surplus into lines opened / landed early / late.
 * right: hit +44, early -2, late 0.   left: hit +26, early 0, late -160. */
static int decode(int delta, int right, int *h, int *e, int *l)
{
    int best = 1 << 30;
    for (int hh = 0; hh <= BAND_TL; hh++)
        for (int ff = 0; ff + hh <= BAND_TL; ff++) {
            int v = right ? 44 * hh - 2 * ff : 26 * hh - 160 * ff;
            int err = abs(v - delta);
            if (err < best) {
                best = err; *h = hh;
                if (right) { *e = ff; *l = BAND_TL - hh - ff; }
                else       { *l = ff; *e = BAND_TL - hh - ff; }
            }
        }
    return best <= 3;
}

/* ---- running a phase ----------------------------------------------------- */
enum { PH_EDGE, PH_TOPBOT, PH_RIGHT, PH_LEFT };

static void build(int ph)
{
    nev = 0;
    add(80 * LINE_CYC + 120, EV_READ, 0, 0, 0, RD_TRACK);
    switch (ph) {
    case PH_EDGE:
        for (int L = DE_FIRST; L <= DE_LAST; L++) {
            add(L * LINE_CYC + 244, EV_W16, R_PAL15, C_YELLOW, 1, 0);
            add(L * LINE_CYC + 440, EV_W16, R_PAL15, C_BLUE, 0, 0);
        }
        break;
    case PH_TOPBOT:
        add(33 * LINE_CYC + 400, EV_W8, R_SYNC, 0, 1, 0);
        add(34 * LINE_CYC + 16,  EV_W8, R_SYNC, 2, 1, 0);
        add(262 * LINE_CYC + 440, EV_W8, R_SYNC, 0, 1, 0);
        add(263 * LINE_CYC + 16,  EV_W8, R_SYNC, 2, 1, 0);
        add(290 * LINE_CYC + 120, EV_READ, 0, 0, 0, RD_BOT);
        break;
    case PH_RIGHT:
    case PH_LEFT:
        for (int b = 0; b <= NBAND; b++) {
            int Lc = BAND_L0 + b * BAND_H;            /* clean read line  */
            add(Lc * LINE_CYC + 120, EV_READ, 0, 0, 0, RD_BAND0 + b);
            if (b == NBAND)
                break;
            add(Lc * LINE_CYC + 20, EV_W16, R_PAL15, band_col[b], 0, 0);
            for (int L = Lc + 1; L < Lc + BAND_H; L++) {
                if (ph == PH_RIGHT) {
                    add(L * LINE_CYC + 376 + band_ofs(b), EV_W8, R_SYNC, 0, 1, 0);
                    add(L * LINE_CYC + 420, EV_W8, R_SYNC, 2, 0, 0);
                } else {
                    /* a demo's pulse: hi res for 8 cycles at the line
                     * start (held from cycle 470 of the line before, as
                     * the first Pi run did, blanked every line) */
                    add(L * LINE_CYC + band_ofs(b), EV_W8, R_REZ, 2, 1, 0);
                    add(L * LINE_CYC + band_ofs(b) + 8, EV_W8, R_REZ, 0, 1, 0);
                }
            }
        }
        break;
    }
    qsort(ev, (size_t)nev, sizeof ev[0], ev_cmp);
}

static void frame_result(int ph, long n)
{
    int de_first = DE_FIRST;
    if (!rd_ok[RD_TRACK]) { R.trk_rej++; return; }
    if (ph == PH_TOPBOT) {
        /* which first line fits the counter better: that is the top border */
        double cm = frame_cyc(rd_t[RD_TRACK], n);
        long o63 = (long)rd_cnt[RD_TRACK] - BASE - (80 - DE_FIRST) * 160;
        long o34 = (long)rd_cnt[RD_TRACK] - BASE - (80 - DE_FIRST_TOP) * 160;
        double e63 = (o63 > 0 && o63 < 160) ? fabs(cm - cnt_cycle(80, (int)o63)) : 1e9;
        double e34 = (o34 > 0 && o34 < 160) ? fabs(cm - cnt_cycle(80, (int)o34)) : 1e9;
        if (e34 < 64.0 && e34 < e63)      { R.top_open++; de_first = DE_FIRST_TOP; }
        else if (e63 < 64.0)              R.top_shut++;
        else                              R.top_bad++;
        /* bottom: shut = the counter stopped at the end of line 262 */
        if (rd_ok[RD_BOT]) {
            long end = (long)(DE_LAST + 1 - de_first) * 160;
            long v = (long)rd_cnt[RD_BOT] - BASE;
            long run = (long)(290 - de_first) * 160;
            if (v == end)                         R.bot_shut++;
            else if (v > run && v < run + 160)    R.bot_open++;
            else                                  R.bot_bad++;
        } else
            R.bot_bad++;
    }
    track(n, de_first);
    if (ph == PH_RIGHT || ph == PH_LEFT) {
        int bb[NBAND + 1];
        for (int b = 0; b <= NBAND; b++) {
            if (!rd_ok[RD_BAND0 + b]) return;
            bb[b] = bytes_before(RD_BAND0 + b, n, BAND_L0 + b * BAND_H);
        }
        for (int b = 0; b < NBAND; b++) {
            int delta = bb[b + 1] - bb[b] - BAND_H * 160;
            int h, e, l;
            R.dsum[b] += delta; R.dn[b]++;
            if (decode(delta, ph == PH_RIGHT, &h, &e, &l)) {
                R.hit[b] += (uint64_t)h; R.early[b] += (uint64_t)e; R.late_[b] += (uint64_t)l;
            } else
                R.undec[b]++;
        }
    }
}

static void run_phase(int ph, double secs)
{
    build(ph);
    memset(&R, 0, sizeof R);
    const long frames = (long)(secs * 50.0);
    const uint64_t gap = (uint64_t)(20e-6 * HZ);     /* a 20 us stall      */
    long n = (long)floor(((double)ticks() - O) / P) + 1;

    for (long f = 0; f < frames && !g_stop; f++, n++) {
        /* behind by more than the first event? start again a frame on */
        double first = O + (double)n * P + ev[0].cyc * TPC - LEAD16;
        if ((double)ticks() > first) {
            long m = (long)floor(((double)ticks() - O) / P) + 1;
            R.skipped += (uint64_t)(m - n);
            n = m;
        }
        memset(rd_ok, 0, sizeof rd_ok);
        for (int i = 0; i < nev; i++) {
            const ev_t *e = &ev[i];
            double lead = e->kind == EV_W8 ? LEAD8 : e->kind == EV_W16 ? LEAD16 : 0.0;
            double T = O + (double)n * P +
                       (e->cyc + (e->kind == EV_READ ? 0 : g_cal)) * TPC - lead;
            uint64_t Ti = (uint64_t)T, t = ticks(), prev = t;
            while (t < Ti) {
                prev = t; t = ticks();
                if (t - prev > gap) R.gaps++;
            }
            if (e->kind == EV_READ) {
                rd_ok[e->slot] = (uint8_t)read_counter(&rd_cnt[e->slot], &rd_t[e->slot]);
                continue;
            }
            if (e->kind == EV_W8) ps_write_8(e->addr, e->val);
            else                  ps_write_16(e->addr, e->val);
            uint64_t t1 = ticks();
            if (e->timed) {
                double late = ((double)t - T) * NSPT;
                h_add(&R.late, late);
                if (late > 1000.0) R.late1us++;
                h_add(e->kind == EV_W8 ? &R.dur8 : &R.dur16, (double)(t1 - t) * NSPT);
            }
        }
        frame_result(ph, n);
        R.frames++;
    }
    /* leave the machine plain */
    ps_write_8(R_SYNC, 2);
    ps_write_8(R_REZ, 0);
}

static void report_common(void)
{
    printf("  frames %llu, skipped %llu, stalls >20us while waiting %llu, writes >1us late %llu\n",
           (unsigned long long)R.frames, (unsigned long long)R.skipped,
           (unsigned long long)R.gaps, (unsigned long long)R.late1us);
    h_print("issue lateness", &R.late);
    if (R.dur8.n)  h_print("byte write time", &R.dur8);
    if (R.dur16.n) h_print("word write time", &R.dur16);
    if (R.trk_n) {
        double m = R.trk_sum / R.trk_n, sd = sqrt(fmax(0.0, R.trk_sq / R.trk_n - m * m));
        printf("  lock (line 80 read)    n=%-8llu mean %+5.2f  sd %4.2f  min %+6.1f  max %+6.1f cycles"
               "   rejected %llu, re-locks %llu\n",
               (unsigned long long)R.trk_n, m, sd, R.trk_min, R.trk_max,
               (unsigned long long)R.trk_rej, (unsigned long long)R.trk_relock);
    } else
        printf("  lock: NO counter read accepted (rejected %llu)\n", (unsigned long long)R.trk_rej);
    printf("  ST clock now %.1f Hz\n", FRAME_CYC * HZ / P);
}

static void report_sweep(int right)
{
    printf("  band     offset  cycle   opened  %s  %s   undecoded  mean surplus\n",
           right ? " early " : "normal", right ? "normal" : " blank");
    for (int b = 0; b < NBAND; b++) {
        double tot = (double)(R.hit[b] + R.early[b] + R.late_[b]);
        if (tot < 1) tot = 1;
        printf("  %-8s  %+4d   %4d   %5.1f%%  %5.1f%%  %5.1f%%   %6llu    %+8.1f bytes\n",
               band_name[b], band_ofs(b), (right ? 376 : 0) + band_ofs(b),
               100.0 * R.hit[b] / tot, 100.0 * R.early[b] / tot, 100.0 * R.late_[b] / tot,
               (unsigned long long)R.undec[b], R.dn[b] ? R.dsum[b] / R.dn[b] : 0.0);
    }
    if (right)
        printf("  (cycle = where the 60 Hz write was aimed. opened: 204-byte line;\n"
               "   early: 158-byte line, 60 Hz seen at 372; normal: 60 Hz came after 376)\n");
    else
        printf("  (cycle = where the hi res write was aimed, low res 8 later. opened:\n"
               "   186-byte line, hi res at 4 and low again by 16; normal: hi res missed\n"
               "   cycle 4; blank: hi res still on at 16 or seen at 28/56)\n");
}

/* ---- the lock ------------------------------------------------------------ */
static int lock(void)
{
    enum { NS = 400000 };
    uint64_t *T = malloc(NS * sizeof *T);
    uint32_t *C = malloc(NS * sizeof *C);
    uint8_t  *bad = calloc(NS, 1);
    int ns = 0, torn = 0;
    if (!T || !C || !bad) return -1;

    uint64_t end = ticks() + (uint64_t)HZ;
    while (ticks() < end && ns < NS) {
        if (read_counter(&C[ns], &T[ns])) ns++; else torn++;
    }

    double tpc = HZ / ST_HZ_NOM, Pf = FRAME_CYC * tpc, ref = 0, a = 0, slope = Pf, sd = 0;
    int have_ref = 0, used = 0;
    for (int pass = 0; pass < 3; pass++) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0; used = 0;
        for (int i = 0; i < ns; i++) {
            long ot = (long)C[i] - (long)BASE;
            if (bad[i] || ot < 0 || ot >= (DE_LAST + 1 - DE_FIRST) * 160) { bad[i] = 1; continue; }
            int off = (int)(ot % 160);
            if (off == 0) { bad[i] = 1; continue; }
            double est = (double)T[i] - cnt_cycle(DE_FIRST + (int)(ot / 160), off) * tpc;
            if (!have_ref) { ref = est; have_ref = 1; }
            double x = (double)llround((est - ref) / Pf), y = est - ref;
            if (pass > 0) {
                double r = (y - (a + slope * x)) / tpc;
                if (fabs(r) > fmax(4.0 * sd, 6.0)) { bad[i] = 1; continue; }
            }
            sx += x; sy += y; sxx += x * x; sxy += x * y; used++;
        }
        if (used < 1000) break;
        double d = used * sxx - sx * sx;
        if (fabs(d) < 1e-9) break;
        slope = (used * sxy - sx * sy) / d;
        a = (sy - slope * sx) / used;
        Pf = slope; tpc = Pf / FRAME_CYC;
        double s2 = 0;
        for (int i = 0; i < ns; i++) {
            if (bad[i]) continue;
            long ot = (long)C[i] - (long)BASE;
            double est = (double)T[i] - cnt_cycle(DE_FIRST + (int)(ot / 160), (int)(ot % 160)) * tpc;
            double x = (double)llround((est - ref) / Pf);
            double r = (est - ref - (a + slope * x)) / tpc;
            s2 += r * r;
        }
        sd = sqrt(s2 / used);
    }
    printf("  counter reads %d (torn %d), used for the fit %d\n", ns, torn, used);
    free(T); free(C); free(bad);
    if (used < 1000) {
        printf("  LOCK FAILED: too few reads on displayed lines - is the screen up at 50 Hz?\n");
        return -1;
    }
    O = ref + a; P = slope; TPC = P / FRAME_CYC;
    printf("  ST clock %.1f Hz (PAL ST nominal 8021247), frame %.3f ms, fit residual sd %.2f cycles\n",
           FRAME_CYC * HZ / P, P * NSPT / 1e6, sd);
    return 0;
}

static void measure_writes(void)
{
    hist_t h8, h16;
    memset(&h8, 0, sizeof h8); memset(&h16, 0, sizeof h16);
    for (int i = 0; i < 4000; i++) {
        uint64_t t0 = ticks(); ps_write_8(R_SYNC, 2); uint64_t t1 = ticks();
        h_add(&h8, (double)(t1 - t0) * NSPT);
        t0 = ticks(); ps_write_16(R_PAL0, C_BLACK); t1 = ticks();
        h_add(&h16, (double)(t1 - t0) * NSPT);
    }
    h_print("byte write time", &h8);
    h_print("word write time", &h16);
    /* aim the middle of a write where the middle of a counter read was */
    LEAD8  = h_pct(&h8, 0.5)  / 2.0 / NSPT;
    LEAD16 = h_pct(&h16, 0.5) / 2.0 / NSPT;
    if (lead_forced >= 0) LEAD8 = LEAD16 = lead_forced / NSPT;
    printf("  write lead: byte %.0f ns, word %.0f ns%s\n", LEAD8 * NSPT, LEAD16 * NSPT,
           lead_forced >= 0 ? " (--lead)" : "");
}

/* ---- main ---------------------------------------------------------------- */
static struct ss_screen scr;

int main(int argc, char **argv)
{
    double secs = 10.0;
    int core = 3;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--secs") && i + 1 < argc)      secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--core") && i + 1 < argc) core = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lead") && i + 1 < argc) lead_forced = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cal") && i + 1 < argc)  { g_cal = atoi(argv[++i]); cal_forced = 1; }
        else { fprintf(stderr, "usage: %s [--secs N] [--core N] [--lead NS] [--cal CYCLES]\n", argv[0]); return 2; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (pistorm_bus_lock("beamtest") != 0) {
        printf("the emulator owns the bus: sudo systemctl stop pistorm first\n");
        return 1;
    }
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(core, &cs);
    if (sched_setaffinity(0, sizeof cs, &cs) != 0) perror("sched_setaffinity");
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) perror("mlockall");
    signal(SIGINT, on_sigint);

    HZ = (double)tick_hz(); NSPT = 1e9 / HZ;
    printf("beamtest: core %d, Pi timer %.0f Hz (%.1f ns), %.0f s per phase\n", core, HZ, NSPT, secs);

    ps_setup_protocol();
    ps_bus_cpu_thread(1);
    ps_pulse_halt();
    ps_reset_state_machine();
    ps_pulse_reset();
    ss_bringup(&scr, SS_MODE_AUTO, 1);
    if (!scr.shifter) { printf("no ST-RAM answered: is the ST on?\n"); return 1; }
    if (scr.mode != SS_MODE_COLOUR) { printf("mono monitor: this test needs a colour monitor\n"); return 1; }
    if (scr.bank0 < (512u << 10)) { printf("bank 0 is %u K: needs 512 K\n", scr.bank0 >> 10); return 1; }
    printf("ST-RAM bank 0 %u K, bank 1 %u K, MMU $%02X\n", scr.bank0 >> 10, scr.bank1 >> 10, scr.memcfg);

    /* a solid screen in colour 15, black border: any line the Shifter
     * fetches is colour 15 whatever its length, so overscan shows at once */
    fc = 0x5;
    ps_write_8(R_SYNC, 2);
    ps_write_8(R_REZ, 0);
    ps_write_8(0x00FF8201u, (BASE >> 16) & 0xFF);
    ps_write_8(0x00FF8203u, (BASE >> 8) & 0xFF);
    for (int i = 0; i < 16; i++) ps_write_16(R_PAL0 + 2u * i, C_BLUE);
    ps_write_16(R_PAL0, C_BLACK);
    for (uint32_t a = BASE; a < BASE + FILL_BYTES; a += 4) ps_write_32(a, 0xFFFFFFFFu);
    usleep(100000);

    printf("\n[A] LOCK\n");
    if (lock() != 0) return 1;
    measure_writes();

    static const struct { int ph; const char *name, *look; } phases[] = {
        { PH_RIGHT,  "RIGHT BORDER SWEEP (calibrates the tests after it)",
          "9 coloured bands (red, orange, yellow, green, cyan, blue, magenta, white, grey =\n"
          "      offsets -16..+16). Bands reaching the monitor's right edge opened." },
        { PH_EDGE,   "EDGE",
          "blue screen, right half yellow. The edge should be one straight vertical line;\n"
          "      a ragged edge is jitter (one step = 4 pixels = one bus slot)." },
        { PH_TOPBOT, "TOP + BOTTOM BORDER",
          "green from near the top of the monitor to near the bottom." },
        { PH_LEFT,   "LEFT BORDER SWEEP",
          "the same 9 bands, each pulsing hi res for 8 cycles at a different offset\n"
          "      from the line start. Bands reaching the monitor's left edge opened; black\n"
          "      lines in a band = blank lines." },
    };
    for (int k = 0; k < 4 && !g_stop; k++) {
        printf("\n[%c] %s - on the monitor: %s\n", 'B' + k, phases[k].name, phases[k].look);
        if (phases[k].ph == PH_EDGE)   ps_write_16(R_PAL15, C_BLUE);
        if (phases[k].ph == PH_TOPBOT) ps_write_16(R_PAL15, C_GREEN);
        run_phase(phases[k].ph, secs);
        report_common();
        if (phases[k].ph == PH_TOPBOT) {
            printf("  top border:    opened %llu, shut %llu, unreadable %llu frames\n",
                   (unsigned long long)R.top_open, (unsigned long long)R.top_shut,
                   (unsigned long long)R.top_bad);
            printf("  bottom border: opened %llu, shut %llu, unreadable %llu frames\n",
                   (unsigned long long)R.bot_open, (unsigned long long)R.bot_shut,
                   (unsigned long long)R.bot_bad);
        }
        if (phases[k].ph == PH_RIGHT) {
            report_sweep(1);
            /* one band that always opens, with no neighbour that ever
             * does, gives the writes' landing error: correct for it */
            int best = 0;
            for (int b = 1; b < NBAND; b++) if (R.hit[b] > R.hit[best]) best = b;
            double tot = (double)(R.hit[best] + R.early[best] + R.late_[best]);
            int clean = tot > 0 && R.hit[best] >= 0.9 * tot;
            for (int b = 0; b < NBAND; b++)
                if (b != best && R.hit[b] > 0.1 * tot) clean = 0;
            if (cal_forced)
                printf("  calibration: %+d cycles (--cal)\n", g_cal);
            else if (clean) {
                g_cal = band_ofs(best);
                printf("  calibration: writes land %d cycles %s aimed; the tests below aim %+d\n",
                       abs(g_cal), g_cal <= 0 ? "after where they are" : "before where they are", g_cal);
            } else
                printf("  calibration: NO single band opened cleanly - the tests below are uncorrected\n");
        }
        if (phases[k].ph == PH_LEFT)  report_sweep(0);
    }

    ps_write_16(R_PAL15, C_BLUE);
    printf("\nbeamtest done%s\n", g_stop ? " (stopped by Ctrl+C)" : "");
    return 0;
}
