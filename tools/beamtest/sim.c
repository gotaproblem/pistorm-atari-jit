/*
 * sim.c - a model ST behind beamtest's bus calls, to check the lock, the
 * scheduling and the counter decoding on any host before the Pi run.
 *
 *   cc -O2 -DBEAMTEST_SIM -Igpio -I. -Iplatforms/atari/setup \
 *      tools/beamtest/beamtest.c tools/beamtest/sim.c -o beamsim -lm
 *   ./beamsim --secs 2 --core 0
 *
 * The ST: a crystal SIM_PPM off nominal (default +60), started at a random
 * phase. The GLUE decides each line the way stbox.c's hdecide() and its
 * vertical rules do (Hatari's STF table). A write takes SIM_DUR_NS (600)
 * and lands SIM_LAT_NS (150) after it is issued, +/- SIM_JIT_NS (50),
 * snapped up to the 4-cycle bus grid. A byte read takes 500 ns and samples
 * at its middle. Expected with the defaults: the right sweep opens in the
 * offset 0 band only, the left sweep in the -4, 0 and +4 bands.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "shifter_setup.h"

uint8_t fc;
volatile _Atomic uint8_t g_buserr;

#define LINE_CYC 512
#define FRAME_LINES 313
#define FRAME_CYC (LINE_CYC * FRAME_LINES)
#define BASE 0x00010000u

static double st_hz, org_ns, lat_ns = 150, jit_ns = 50, dur_ns = 600;

/* the model's own work (deciding lines) is taken out of the clock, so it
 * costs beamtest nothing: now_ns() is host time less that work */
static double paused_ns;
static double raw_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}
static double now_ns(void) { return raw_ns() - paused_ns; }
uint64_t sim_ticks(void) { return (uint64_t)(now_ns() * 0.054); }
uint64_t sim_hz(void)    { return 54000000u; }
static void spin_until(double ns) { while (now_ns() < ns) ; }
/* SIM_JUMP_S=n: n seconds in, the real frame jumps one line ahead (what a
 * late write can do to the real GLUE) - for stboxmetal's re-sync */
static double jump_at_ns;
static inline double abs_cyc(double ns)
{
    if (jump_at_ns > 0 && ns > jump_at_ns) { org_ns -= 512.0 / st_hz * 1e9; jump_at_ns = 0; }
    return (ns - org_ns) * st_hz * 1e-9;
}

/* register histories: (abs cycle, value), in landing order */
typedef struct { int64_t c; uint8_t v; } chg_t;
#define NCH (1 << 22)
static chg_t *syn, *rez, *bhi, *bmi;     /* sync, rez, video base hi/mid */
static int nsyn, nrez, nbhi, nbmi;

static uint8_t val_at(const chg_t *a, int n, int64_t c)
{
    int lo = 0, hi = n - 1, r = 0;              /* a[0] is the reset value */
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (a[m].c <= c) { r = m; lo = m + 1; } else hi = m - 1;
    }
    return a[r].v;
}
static int freq_at(int64_t c)
{
    if (val_at(rez, nrez, c) & 2) return 71;
    return (val_at(syn, nsyn, c) & 2) ? 50 : 60;
}

/* stbox.c hdecide(), on the histories */
static int64_t g_ls;
static inline int f_at(int c) { return freq_at(g_ls + c); }
static int hdecide(int left_in, int *fstart, int *left_full)
{
    *left_full = 0;
    int left_off = f_at(4) == 71 || left_in;
    int no_de = 0, start, bytes = 0;
    if (left_off && f_at(56) == 71 && f_at(164) == 71) { no_de = 1; start = 4; }
    else {
        if (!left_off) {
            if (f_at(28) == 71) no_de = 1;
            else if (f_at(56) == 71) no_de = 1;
        }
        if (left_off) start = 4;
        else if (f_at(52) == 60) start = 52;
        else if (f_at(56) == 50) start = 56;
        else { start = 56; no_de = 1; }
    }
    if (!no_de) {
        bytes = 160;
        if (left_off) bytes += 26;
        else if (start == 52) bytes += 2;
        if (f_at(164) == 71) bytes -= 106;
        else if (f_at(372) == 60) bytes -= 2;
        else if (f_at(376) == 50) ;
        else {
            bytes += 44;
            if (f_at(376) == 71 || f_at(LINE_CYC - 50) == 71) { bytes += 22; *left_full = 1; }
        }
    }
    if (val_at(rez, nrez, g_ls + (left_off ? 16 : start)) & 2) no_de = 1;
    *fstart = start;
    return no_de ? 0 : bytes;
}

/* the line cursor: lines decided so far */
static int64_t cur_frame = -1;
static int cur_line, cur_vde, cur_left;
static uint32_t cur_acc;

static void decide_line(void)
{
    g_ls = (cur_frame * FRAME_LINES + cur_line) * (int64_t)LINE_CYC;
    if (cur_vde) {
        int fs, lf;
        cur_acc += (uint32_t)hdecide(cur_left, &fs, &lf);
        cur_left = lf;
    } else
        cur_left = 0;
    int fv = f_at(502), L = cur_line;
    if (!cur_vde) {
        if (L == 33 && (fv == 60 || fv == 71)) cur_vde = 1;
        else if (L == 62 && fv == 50) cur_vde = 1;
    } else if ((L == 233 && fv == 60) || (L == 262 && fv == 50) || L + 1 == 310)
        cur_vde = 0;
    if (++cur_line == FRAME_LINES) { cur_frame++; cur_line = 0; cur_acc = 0; cur_vde = 0; cur_left = 0; }
}

static uint32_t counter_at(double ns)
{
    int64_t c = (int64_t)abs_cyc(ns);
    int64_t frame = c / FRAME_CYC;
    int within = (int)(c % FRAME_CYC), L = within / LINE_CYC, cr = within % LINE_CYC;
    if (cur_frame < 0 || cur_frame < frame - 1) { cur_frame = frame; cur_line = 0; cur_acc = 0; cur_vde = 0; cur_left = 0; }
    while (cur_frame < frame || (cur_frame == frame && cur_line < L))
        decide_line();
    uint32_t partial = 0;
    if (cur_vde) {
        int fs, lf;
        g_ls = (frame * FRAME_LINES + L) * (int64_t)LINE_CYC;
        int bytes = hdecide(cur_left, &fs, &lf);
        if (bytes && cr >= fs) {
            int p = 2 * ((cr - fs) / 4);
            partial = (uint32_t)(p < bytes ? p : bytes);
        }
    }
    /* the counter restarts from the base 3 lines before the frame ends
     * (line 310, cycle 56): this frame's base is the one set by then */
    const int64_t rl = (frame * FRAME_LINES - 3) * (int64_t)LINE_CYC + 56;
    const uint32_t base = ((uint32_t)val_at(bhi, nbhi, rl) << 16) | ((uint32_t)val_at(bmi, nbmi, rl) << 8);
    return base + cur_acc + partial;
}

static double rd_ns = 500.0;          /* a bus read's length (SIM_RD_NS)  */
static double jitter(void) { return jit_ns * (2.0 * rand() / RAND_MAX - 1.0); }

static void land(chg_t *a, int *n, double issue, uint8_t v)
{
    int64_t c = (int64_t)abs_cyc(issue + lat_ns + jitter());
    c = (c + 3) & ~(int64_t)3;                  /* up to the bus grid      */
    if (*n < NCH && c >= a[*n - 1].c) a[(*n)++] = (chg_t){ c, v };
}

void ps_write_8(uint32_t addr, uint16_t data)
{
    double t = now_ns();
    addr &= 0xFFFFFF;
    if (addr == 0xFF820A) land(syn, &nsyn, t, (uint8_t)data);
    if (addr == 0xFF8260) land(rez, &nrez, t, (uint8_t)data);
    if (addr == 0xFF8201) land(bhi, &nbhi, t, (uint8_t)data);
    if (addr == 0xFF8203) land(bmi, &nbmi, t, (uint8_t)data);
    spin_until(t + dur_ns);
}
void ps_write_16(uint32_t addr, uint16_t data) { (void)addr; (void)data; spin_until(now_ns() + dur_ns + 50); }
void ps_write_32(uint32_t addr, uint32_t data) { (void)addr; (void)data; }
uint16_t ps_read_16(uint32_t addr) { (void)addr; spin_until(now_ns() + 700); return 0; }   /* no RAM modelled */

uint8_t ps_read_8(uint32_t addr)
{
    double t = now_ns(), w0 = raw_ns();
    double ts = t + 250 + jitter();
    uint32_t v = counter_at(ts);
    if (getenv("SIM_CHECK") && nsyn == 1 && nrez == 1) {
        int64_t c = (int64_t)abs_cyc(ts);
        int within = (int)(c % FRAME_CYC), L = within / LINE_CYC, cr = within % LINE_CYC;
        uint32_t x = BASE;
        if (L >= 63 && L <= 262) { int p = cr < 56 ? 0 : 2 * ((cr - 56) / 4); if (p > 160) p = 160; x += (uint32_t)((L - 63) * 160 + p); }
        else if (L > 262) x += 200 * 160;
        if (x != v) fprintf(stderr, "[sim] MISMATCH L %d c %d model %06X sim %06X cur_frame %lld frame %lld cur_line %d\n", L, cr, x, v, (long long)cur_frame, (long long)(c / FRAME_CYC), cur_line);
    }
    paused_ns += raw_ns() - w0;
    uint8_t r = 0;
    switch (addr & 0xFFFFFF) {
    case 0xFF8205: r = (uint8_t)(v >> 16); break;
    case 0xFF8207: r = (uint8_t)(v >> 8);  break;
    case 0xFF8209: r = (uint8_t)v;         break;
    }
    spin_until(t + rd_ns);
    return r;
}

void ps_setup_protocol(void) {}
void ps_bus_cpu_thread(int on) { (void)on; }
void ps_pulse_halt(void) {}
void ps_reset_state_machine(void) {}
void ps_pulse_reset(void) {}
int  pistorm_bus_lock(const char *who) { (void)who; return 0; }
void pistorm_bus_unlock(void) {}
void ps_get_firmware_revision(void) {}
/* the ST's reset button: SIM_RSTBTN_S=T holds RESET low from T s for
 * 0.2 s, once (the variable is cleared, so a restart does not press it) */
int  ps_st_reset_pin(void)
{
    static double t0 = -1, at = -2;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    const double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (t0 < 0) { t0 = now; const char *e = getenv("SIM_RSTBTN_S"); at = e ? atof(e) : -2; }
    if (at < 0) return 0;
    if (now - t0 >= at + 0.2) { unsetenv("SIM_RSTBTN_S"); at = -2; return 0; }
    return now - t0 >= at;
}

int ss_bringup(struct ss_screen *ss, enum ss_mode force, int hz50)
{
    (void)force; (void)hz50;
    const char *e;
    double ppm = (e = getenv("SIM_PPM")) ? atof(e) : 60.0;
    if ((e = getenv("SIM_LAT_NS"))) lat_ns = atof(e);
    if ((e = getenv("SIM_JIT_NS"))) jit_ns = atof(e);
    if ((e = getenv("SIM_DUR_NS"))) dur_ns = atof(e);
    if ((e = getenv("SIM_RD_NS")))  rd_ns  = atof(e);   /* 3A+: ~1.2 us */
    srand((unsigned)now_ns());
    if ((e = getenv("SIM_JUMP_S"))) jump_at_ns = now_ns() + atof(e) * 1e9;
    st_hz = 8021247.0 * (1.0 + ppm * 1e-6);
    org_ns = now_ns() - (double)(rand() % 20000000);
    syn = malloc(NCH * sizeof *syn); rez = malloc(NCH * sizeof *rez);
    syn[0] = (chg_t){ INT64_MIN, 2 }; nsyn = 1;
    bhi = malloc(NCH * sizeof *bhi); bmi = malloc(NCH * sizeof *bmi);
    bhi[0] = (chg_t){ INT64_MIN, (BASE >> 16) & 0xFF }; nbhi = 1;
    bmi[0] = (chg_t){ INT64_MIN, (BASE >> 8) & 0xFF }; nbmi = 1;
    rez[0] = (chg_t){ INT64_MIN, 0 }; nrez = 1;
    ss->shifter = 1; ss->mode = SS_MODE_COLOUR;
    ss->bank0 = 2048u << 10; ss->bank1 = 2048u << 10; ss->memcfg = 0x0A;
    if (getenv("SIM_1MB")) { ss->bank0 = ss->bank1 = 512u << 10; ss->memcfg = 0x05; }   /* a 1040-style ST */
    printf("[sim] ST %.1f Hz (%+.0f ppm), write lands %.0f +/- %.0f ns after issue, takes %.0f ns\n",
           st_hz, ppm, lat_ns, jit_ns, dur_ns);
    return 0;
}
