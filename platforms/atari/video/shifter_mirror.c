/*
 * shifter_mirror.c - the HDMI copy of the ST screen, rebuilt the way the
 * real Shifter drew it: line by line, with borders, overscan and every
 * mid-frame change at the beam position where it actually landed.
 *
 * WHY
 *   The earlier mirror (et4000.c, ST NATIVE SNAPSHOT) copied the screen
 *   once per VBL and placed palette writes on lines by timestamp against a
 *   VBL grid guessed from late sightings. It could not show borders or
 *   overscan, mid-frame resolution/sync/base changes, or anything a game
 *   drew into the visible screen while it was being displayed, and its
 *   raster splits wandered by a line or more.
 *
 * HOW
 *   1. Everything that changes the picture is logged on the CPU thread, in
 *      one ring, stamped with the arch timer at the moment it landed on the
 *      real bus (shm_io_write runs AFTER the bus write):
 *        - palette, sync ($FF820A), resolution ($FF8260), screen base,
 *          STE line width / fine scroll / video counter writes
 *        - ST-RAM writes into the screen being shown or about to be shown,
 *          with the value they overwrote (shm_log_ram, before the store)
 *      The producer never waits: the ring is overwritten, and the reader
 *      checks it was not lapped.
 *   2. Frame timing. ipl_task reports each VBL it sees (shm_vbl_sighting);
 *      sightings are never early, so the lower envelope of them on a fixed
 *      period is the true VBL (same idea as the old vbl_lock). With the
 *      governed 68000 the CPU thread also reads the REAL video counter once
 *      a frame, mid-display (shm_cal_poll): the counter says exactly which
 *      line and cycle the beam was on at that tick, which pins the frame's
 *      origin to the cycle. Without the governor (JIT) the envelope alone
 *      is used.
 *   3. The render thread rebuilds the newest complete frame:
 *        a. replays register events to the frame's start,
 *        b. runs the GLUE line by line - line length, display-enable start
 *           and end, top/bottom border - from the sync/resolution state at
 *           the cycles where the GLUE decides (the decision table is the one
 *           Hatari documents in video.c for the STF, wakestate 1),
 *        c. copies the screen memory once, then undoes every logged write
 *           that landed after the Shifter had fetched that word,
 *        d. draws each line, applying palette writes at their pixel.
 *      Nothing of this runs on core 3 (ipl_task) or costs the CPU thread
 *      more than a ring append per logged write.
 *
 * WHAT IT CANNOT KNOW
 *   The timestamps are the Pi's view of when a bus write completed. Raster
 *   effects that depend on 2-cycle precision (a +2 line made with a switch
 *   on cycle 54) are decided on that view. DMA (floppy/ACSI) into the
 *   screen is not logged. Writes through the unhooked direct ST-RAM window
 *   (stram_direct) are not logged - but those never reach the real Shifter
 *   either.
 *
 * ENVIRONMENT (tuning and diagnosis)
 *   PISTORM_SHM=0            use the old snapshot mirror
 *   PISTORM_SHM_DEBUG=1      one summary line every 5 s (timing lock,
 *                            calibration error, events, border tricks seen,
 *                            render cost). =2 adds a line per calibration.
 *   PISTORM_SHM_X0=24        first cycle shown at the left edge (negative:
 *                            border colour before the line starts)
 *   PISTORM_SHM_W=384        cycles shown (canvas width in low-res pixels)
 *   PISTORM_SHM_PAR=5/4      pixel aspect, width/height (as the monitor
 *                            shows it; 1/1 = square)
 *   PISTORM_SHM_TOP50=34     first frame line shown at 50 Hz (276 rows)
 *   PISTORM_SHM_TOP60=5      first frame line shown at 60 Hz
 *   PISTORM_SHM_PALDX=28     a palette write at cycle c colours pixels from
 *                            cycle c-PALDX on (Shifter pipeline; Hatari's
 *                            spec512 alignment)
 *   PISTORM_SHM_CALK=0       cycles between the counter read stamp and the
 *                            counter value (bus read latency)
 *   PISTORM_SHM_VBLOFS=-86   uncalibrated only: cycles from the VBL
 *                            envelope to the frame origin (measured on a
 *                            Pi 4; the debug line prints what calibration
 *                            measures).
 *   PISTORM_SHM_CAL=0        no video counter reads
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "platforms/atari/video/shifter_mirror.h"
#if !defined(SHM_TEST)
#include "platforms/atari/et4000/et4000.h"          /* rtg_s */
#endif

uint32_t pistorm_stram_top(void);             /* pistorm_natmem.cpp */

#if !defined(__aarch64__) && !defined(SHM_TEST)
uint64_t shm_ticks_host(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

static uint64_t tick_hz(void)
{
#if defined(__aarch64__)
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? f : 54000000ull;
#else
    return 1000000000ull;
#endif
}

/* ======================================================================
 * Shared state
 * ====================================================================== */
#define SHM_RING_BITS 18u                     /* 256K events, 4 MB       */
#define SHM_RING      (1u << SHM_RING_BITS)
#define SHM_MASK      (SHM_RING - 1u)

int shm_on;
volatile uint32_t shm_rlo[3], shm_rlen[3];

static shm_ev_t         *ring;
static volatile uint32_t ring_w;
static const uint8_t    *g_natmem;
static int               g_ste;
static volatile uint32_t g_frame_bytes = 32000u + 512u;  /* render publishes */

/* the CPU thread's view of the registers (old values for the log) */
static uint16_t cpu_pal[16];
static uint8_t  cpu_reg[0x68];

/* VBL sightings: ipl_task -> render */
#define SIGHT_N 64u
static uint64_t          sight[SIGHT_N];
static uint8_t           sight_cls[SIGHT_N];  /* GLUE rate by the registers at the sighting */
static volatile uint32_t sight_w;

/* video counter reads: CPU -> render */
#define CAL_N 16u
typedef struct { uint64_t t; uint32_t cnt; } cal_t;
static cal_t             cal_ring[CAL_N];
static volatile uint32_t cal_w;
volatile uint64_t        shm_cal_lo = UINT64_MAX;
static volatile uint64_t cal_hi, cal_P;
static volatile uint32_t cal_tries;

/* ======================================================================
 * CPU thread
 * ====================================================================== */
static inline void ev_push(uint64_t t, uint32_t a, uint16_t o, uint16_t n)
{
    uint32_t w = ring_w;
    shm_ev_t *e = &ring[w & SHM_MASK];
    e->t = t;
    e->a = a;
    e->o = o;
    e->n = n;
    __atomic_store_n(&ring_w, w + 1u, __ATOMIC_RELEASE);
}

void shm_log_ram(uint32_t a, int size, uint32_t oldv, uint32_t newv)
{
    if (!shm_on)
        return;
    ev_push(shm_ticks(),
            ((size == 2 ? SHM_K_RAM16 : SHM_K_RAM8) << 24) | (a & 0x00FFFFFFu),
            (uint16_t)oldv, (uint16_t)newv);
}

static void base_range_update(void)
{
    uint32_t b = ((uint32_t)cpu_reg[0x01] << 16) | ((uint32_t)cpu_reg[0x03] << 8) |
                 (g_ste ? (uint32_t)(cpu_reg[0x0D] & 0xFEu) : 0u);
    shm_rlo[0]  = b;
    shm_rlen[0] = g_frame_bytes;
}

static void reg_w8(uint64_t t, uint32_t r, uint8_t b)
{
    switch (r) {
    case 0x01: case 0x03: case 0x05: case 0x07: case 0x09:
    case 0x0A: case 0x0D: case 0x0F: case 0x60: case 0x65:
        break;
    default:
        return;                                   /* nothing the picture uses */
    }
    uint8_t old = cpu_reg[r];
    cpu_reg[r] = b;
    ev_push(t, (SHM_K_REG << 24) | r, old, b);
    if (r == 0x01 || r == 0x03) {
        /* the STE clears the low byte on a high/mid write */
        if (g_ste && cpu_reg[0x0D]) {
            ev_push(t, (SHM_K_REG << 24) | 0x0Du, cpu_reg[0x0D], 0);
            cpu_reg[0x0D] = 0;
        }
        base_range_update();
    } else if (r == 0x0D)
        base_range_update();
}

static void pal_w16(uint64_t t, unsigned idx, uint16_t w)
{
    uint16_t old = cpu_pal[idx];
    cpu_pal[idx] = w;
    ev_push(t, (SHM_K_PAL << 24) | idx, old, w);
}

static void io_w8(uint64_t t, uint32_t a, uint8_t b)
{
    if (a >= 0x00FF8240u && a < 0x00FF8260u) {
        unsigned idx = (a - 0x00FF8240u) >> 1;
        uint16_t w = cpu_pal[idx];
        w = (a & 1u) ? (uint16_t)((w & 0xFF00u) | b) : (uint16_t)((w & 0x00FFu) | (b << 8));
        pal_w16(t, idx, w);
    } else
        reg_w8(t, a - 0x00FF8200u, b);
}

static void io_w16(uint64_t t, uint32_t a, uint16_t v)
{
    if (a >= 0x00FF8240u && a < 0x00FF8260u && !(a & 1u))
        pal_w16(t, (a - 0x00FF8240u) >> 1, v);
    else {
        io_w8(t, a, (uint8_t)(v >> 8));
        io_w8(t, a + 1u, (uint8_t)v);
    }
}

void shm_io_write(uint32_t a, uint32_t v, int size)
{
    if (!shm_on)
        return;
    a &= 0x00FFFFFFu;
    if (a < 0x00FF8200u || a >= 0x00FF8266u)
        return;
    uint64_t t = shm_ticks();
    if (size == 4) {
        io_w16(t, a, (uint16_t)(v >> 16));
        io_w16(t, a + 2u, (uint16_t)v);
    } else if (size == 2)
        io_w16(t, a, (uint16_t)v);
    else
        io_w8(t, a, (uint8_t)v);
}

void shm_cpu_vbl_taken(void)
{
    if (!shm_on)
        return;
    /* the screen just latched stays logged one more frame: the renderer
     * copies it after this VBL, and a game redrawing it as its next back
     * buffer must not change the copy behind its back */
    shm_rlo[2]  = shm_rlo[1];
    shm_rlen[2] = shm_rlen[1];
    shm_rlo[1]  = shm_rlo[0];
    shm_rlen[1] = g_frame_bytes;
}

void shm_cal_poll_slow(uint64_t now)
{
    const uint64_t P = cal_P;
    if (!P) {
        shm_cal_lo = UINT64_MAX;
        return;
    }
    uint64_t hi = cal_hi;
    if (now > hi) {                       /* missed it: next frame's window */
        uint64_t k = (now - hi) / P + 1u;
        shm_cal_lo += k * P;
        cal_hi = hi + k * P;
        if (now < shm_cal_lo)
            return;
    }
    cal_tries++;
    uint32_t cnt;
    uint64_t t;
    if (!pistorm_shm_read_counter(&cnt, &t))
        return;                           /* torn: next poll, same window */
    uint32_t w = cal_w;
    cal_ring[w % CAL_N].t = t;
    cal_ring[w % CAL_N].cnt = cnt;
    __atomic_store_n(&cal_w, w + 1u, __ATOMIC_RELEASE);
    shm_cal_lo += P;
    cal_hi += P;
}

/* ======================================================================
 * ipl_task
 * ====================================================================== */
void shm_vbl_sighting(void)
{
    uint32_t w = sight_w;
    sight[w % SIGHT_N] = shm_ticks();
    sight_cls[w % SIGHT_N] = ((cpu_reg[0x60] & 3u) == 2u) ? 71 :
                             (cpu_reg[0x0A] & 2u) ? 50 : 60;
    __atomic_store_n(&sight_w, w + 1u, __ATOMIC_RELEASE);
}

/* ======================================================================
 * Render thread
 * ====================================================================== */
static int env_int(const char *n, int def)
{
    const char *e = getenv(n);
    return (e && *e) ? atoi(e) : def;
}

static int    cfg_x0, cfg_w, cfg_top50, cfg_top60, cfg_paldx, cfg_dbg, cfg_cal, cfg_rows;
static int    cfg_par_n = 1, cfg_par_d = 1;   /* low-res pixel width : height */
static double cfg_calk, cfg_vblofs;
#define ROWS_MAX 276
#define ROWS cfg_rows
#define MAXL 520                       /* 501 mono lines + slack */
#define BUFMAX (256u * 1024u)

int shm_init(const uint8_t *natmem, int ste)
{
    if (shm_on)
        return 0;
    if (env_int("PISTORM_SHM", 1) == 0)
        return -1;
    ring = (shm_ev_t *)calloc(SHM_RING, sizeof *ring);
    if (!ring)
        return -1;
    g_natmem  = natmem;
    g_ste     = ste;
    /* Framed like the ST's RGB output on a real monitor. Photos of the same
     * desktop on the same 1080p panel (2 Nov 2026), Shifter output against
     * HDMI: the monitor shows the 320-pixel display across 82% of the
     * width and 71% of the height, i.e. about 384 cycles by 280 lines,
     * with pixels about 1.28x as wide as tall. So: 384 cycles (24..408,
     * centred on the display's 56..376) by 270 rows, pixels 5:4. That is
     * 480x270 square pixels = 16:9: on 1080p each low-res pixel is 5x4
     * output pixels, every one the same size, and the picture fills the
     * panel - display area 1600x800, as on the monitor.
     *   PISTORM_SHM_PAR=1/1 with X0=-24 W=480: square pixels, wider view
     *   PISTORM_SHM_PAR=1/1 with X0=8 W=416 ROWS=276: Hatari's framing */
    cfg_x0    = env_int("PISTORM_SHM_X0", 24);
    cfg_w     = env_int("PISTORM_SHM_W", 384);
    if (cfg_w < 320 || cfg_w > 512) cfg_w = 384;
    if (cfg_x0 < -64 || cfg_x0 + cfg_w > 520) cfg_x0 = 24;
    {
        const char *e = getenv("PISTORM_SHM_PAR");
        cfg_par_n = 5; cfg_par_d = 4;
        if (e && *e) {
            int n = 0, d = 0;
            if (sscanf(e, "%d/%d", &n, &d) == 2 && n > 0 && d > 0 && n <= 4 * d && d <= 4 * n) {
                cfg_par_n = n; cfg_par_d = d;
            }
        }
    }
    /* 270 rows, not Hatari's 276: 270 x 4 = 1080 exactly, so on a 1080p
     * panel the picture is scaled x4 (1664x1080, full height, every pixel
     * the same size) instead of x3 (1248x828, the small picture of the
     * first hardware test). The 6 rows lost are 3 at the top and 3 at the
     * bottom of the border. PISTORM_SHM_ROWS=276 restores the full frame. */
    cfg_rows  = env_int("PISTORM_SHM_ROWS", 270);
    if (cfg_rows < 200 || cfg_rows > ROWS_MAX) cfg_rows = 270;
    cfg_top50 = env_int("PISTORM_SHM_TOP50", 34 + (ROWS_MAX - cfg_rows) / 2);
    cfg_top60 = env_int("PISTORM_SHM_TOP60", 5 + (ROWS_MAX - cfg_rows) / 2);
    cfg_paldx = env_int("PISTORM_SHM_PALDX", 28);
    cfg_calk  = env_int("PISTORM_SHM_CALK", 0);
    /* -86: measured on a Pi 4 + plain ST, 2 Nov 2026 - the calibrated
     * origin sat 78..98 cycles before the VBL envelope, run after run */
    cfg_vblofs = env_int("PISTORM_SHM_VBLOFS", -86);
    cfg_dbg   = env_int("PISTORM_SHM_DEBUG", 0);
    cfg_cal   = env_int("PISTORM_SHM_CAL", 1);

    /* starting register state: the snoops' copies */
    extern volatile uint16_t st_palette[16];
    for (int i = 0; i < 16; i++)
        cpu_pal[i] = st_palette[i];
#if !defined(SHM_TEST)
    {
        extern volatile double pistorm_guest_hz;
        extern rtg_s rtg;
        cpu_reg[0x01] = rtg.high;
        cpu_reg[0x03] = rtg.mid;
        cpu_reg[0x0D] = ste ? rtg.low : 0;
        cpu_reg[0x0F] = rtg.linewidth;
        cpu_reg[0x65] = rtg.hscroll;
        cpu_reg[0x60] = rtg.hw_rez;
        cpu_reg[0x0A] = (pistorm_guest_hz > 55.0 && pistorm_guest_hz < 65.0) ? 0x00 : 0x02;
    }
#else
    cpu_reg[0x0A] = 0x02;
#endif
    base_range_update();
    __atomic_store_n(&shm_on, 1, __ATOMIC_RELEASE);
    fprintf(stderr, "[SHM] exact HDMI mirror on: %u-event log, canvas cycles %d..%d, "
            "rows %d, shifter %s, video counter calibration %s\n",
            SHM_RING, cfg_x0, cfg_x0 + cfg_w, ROWS, ste ? "STE" : "ST",
            cfg_cal ? "on (governed 68000 only)" : "off");
    return 0;
}

/* ---- register state ---------------------------------------------------- */
typedef struct {
    uint16_t pal[16];
    uint8_t  reg[0x68];
} vstate_t;

static inline int vs_freq(const vstate_t *s)
{
    if (s->reg[0x60] & 2u)
        return 71;
    return (s->reg[0x0A] & 2u) ? 50 : 60;
}

static inline void vs_apply(vstate_t *s, const shm_ev_t *e)
{
    const uint32_t k = SHM_KIND(e->a);
    if (k == SHM_K_REG)
        s->reg[e->a & 0x7Fu] = (uint8_t)e->n;
    else if (k == SHM_K_PAL)
        s->pal[e->a & 15u] = e->n;
}

static vstate_t rs;                      /* state at rs_i (render replay) */
static uint32_t rs_i;                    /* next ring index to apply      */
static int      rs_ok;

static void rs_reset_from_cpu(void)
{
    /* racy against the CPU thread, used only to recover from a lapped ring */
    memcpy(rs.pal, cpu_pal, sizeof rs.pal);
    memcpy(rs.reg, cpu_reg, sizeof rs.reg);
    rs_i = __atomic_load_n(&ring_w, __ATOMIC_ACQUIRE);
    rs_ok = 1;
}

/* ---- frame timing -------------------------------------------------------- */
static int      L_cls;                   /* 0 / 50 / 60 / 71                */
static double   L_ema;                   /* sighting interval, ticks        */
static double   L_P;                     /* frame period, ticks             */
static double   L_A;                     /* envelope anchor (a true VBL)    */
static int      L_have;
static uint64_t L_last;
static double   L_win[32];
static int      L_wn;
static uint64_t L_last_sight;

/* calibrated origin */
static double   C_A;                     /* origin of a calibrated frame    */
static uint64_t C_when;                  /* tick it was measured            */
static double   C_vblofs;                /* learned origin - VBL, ticks     */
static int      C_vblofs_n;

static double frame_cycles(int cls)
{
    return cls == 50 ? 313.0 * 512.0 : cls == 60 ? 263.0 * 508.0 : 501.0 * 224.0;
}

/* The registers' rate over the last 8 sightings: a class needs 6 of
 * them, else 0 (no opinion). Border-removal code holds the sync bit at
 * 60 Hz for a few cycles at a time; one sighting landing there must not
 * count as a 60 Hz screen. */
static int reg_cls_vote(int c)
{
    static uint8_t h[8];
    static unsigned n;
    h[n++ & 7u] = (uint8_t)c;
    int v50 = 0, v60 = 0, v71 = 0;
    for (int i = 0; i < 8; i++)
        v50 += h[i] == 50, v60 += h[i] == 60, v71 += h[i] == 71;
    return v50 >= 6 ? 50 : v60 >= 6 ? 60 : v71 >= 6 ? 71 : 0;
}

static void lock_sighting(uint64_t t, int sight_reg_cls)
{
    const double hz = (double)tick_hz();
    if (L_last) {
        double iv = (double)(t - L_last);
        if (iv > hz / 250.0 && iv < hz / 25.0)
            L_ema = L_ema > 0 ? L_ema + (iv - L_ema) / 32.0 : iv;
    }
    L_last = t;
    L_last_sight = t;
    const double us = L_ema * 1e6 / hz;
    int cls = (us > 18000 && us < 22000) ? 50 : (us > 15000 && us <= 18000) ? 60 :
              (us > 12500 && us <= 15000) ? 71 : 0;
    /* The GLUE's frame rate is set by its registers: mono 71 Hz, else the
     * sync bit (50 / 60). The sighting intervals only CONFIRM it. A
     * level-4 line held while the CPU is slow to take it, pre-empted and
     * re-latched, is sighted again mid-frame; enough of those pull the
     * average under 18 ms (GEMBench, 3 Nov 2026: the lock and the HDMI
     * refresh went to 60 Hz on a 50 Hz screen). So the class changes only
     * when the intervals and the registers agree; otherwise the current
     * class stands if the registers still say it, else there is no lock.
     * The registers are read at each sighting and voted over 8
     * (reg_cls_vote), never at render time. */
    {
        const int reg_cls = reg_cls_vote(sight_reg_cls);
        if (!reg_cls)
            cls = L_cls;                       /* registers undecided: no change */
        else if (cls != reg_cls)
            cls = (L_cls == reg_cls) ? L_cls : 0;
    }
    if (cls != L_cls) {
        L_cls = cls;
        L_have = 0;
        C_when = 0;
        if (!cls)
            return;
        /* nominal at the PAL ST's 8.021247 MHz; refined below */
        L_P = frame_cycles(cls) * hz / 8021247.0;
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
    double rel = (double)t - L_A + L_P / 8.0;
    double k = floor(rel / L_P);
    double r = (double)t - (L_A + k * L_P);
    L_win[L_wn++] = r;
    if (L_wn >= 32) {
        /* second smallest residual: the lower envelope, one outlier allowed */
        double m1 = 1e300, m2 = 1e300;
        for (int i = 0; i < L_wn; i++) {
            if (L_win[i] < m1) { m2 = m1; m1 = L_win[i]; }
            else if (L_win[i] < m2) m2 = L_win[i];
        }
        L_A += m2;
        /* m2 is also the drift accumulated over ~32 frames - unless the
         * video counter is steering the period (cal_process) */
        if (!C_when || (double)(t - C_when) > 0.5 * hz)
            L_P += 0.5 * m2 / 32.0;
        L_wn = 0;
        /* keep the anchor recent so k stays small */
        double kk = floor(((double)t - L_A) / L_P);
        if (kk > 0)
            L_A += kk * L_P;
    }
}

/* origin of the frame containing tick x (or the last one before it) */
static double frame_origin_at(double x, int *calibrated)
{
    const double hz = (double)tick_hz();
    if (C_when && x - (double)C_when < 0.5 * hz) {
        *calibrated = 1;
        return C_A + floor((x - C_A) / L_P) * L_P;
    }
    *calibrated = 0;
    const double ofs = C_vblofs_n ? C_vblofs : cfg_vblofs * L_P / frame_cycles(L_cls);
    const double a = L_A + ofs;
    return a + floor((x - a) / L_P) * L_P;
}

int shm_locked_hz(void)
{
    return L_have ? L_cls : 0;
}

/* ---- the GLUE, one line at a time --------------------------------------- */
typedef struct {
    double   s;          /* line start, ticks from the frame origin */
    uint32_t ev0;        /* first ring index at or after s          */
    uint32_t addr;       /* video counter at the first fetch        */
    uint16_t len;        /* cycles                                  */
    uint16_t bytes;      /* fetched and shown                       */
    uint16_t extra;      /* STE fine-scroll prefetch                */
    int16_t  fstart;     /* first fetch cycle                       */
    int16_t  pstart;     /* first pixel cycle                       */
    uint8_t  vde, vis, de, blank, rez, hs;
    uint8_t  flags;
} line_t;

#define LF_LEFT   0x01   /* left border removed  (+26) */
#define LF_LEFT2  0x02   /* 60 Hz start          (+2)  */
#define LF_RIGHT  0x04   /* right border removed (+44) */
#define LF_RIGHT2 0x08   /* 60 Hz end            (-2)  */
#define LF_MID    0x10   /* stop in the middle   (-106)*/
#define LF_NODE   0x20   /* no display enable    (0)   */
#define LF_FULL   0x40   /* right border to 512  (+22) */

typedef struct { float c; uint8_t f; uint8_t rez; } glue_ch_t;

static line_t   ln[MAXL];
static int      nln;
static uint8_t  snapbuf[BUFMAX + 16];   /* +16: a partial last group reads on */

/* stats for PISTORM_SHM_DEBUG */
static struct {
    uint64_t frames, cal_frames, lapped, ev, ev_ram, undone, render_ticks;
    uint64_t lf[8], top_off, bottom_off;
    double   cal_err_min, cal_err_max, cal_err_sum;
    uint64_t cal_n, cal_rej;
    uint64_t t0;
} st;

static inline int f_at(const glue_ch_t *ch, int n, int f0, double c)
{
    int f = f0;
    for (int i = 0; i < n && ch[i].c <= c; i++)
        f = ch[i].f;
    return f;
}

static inline int rez_at(const glue_ch_t *ch, int n, int r0, double c)
{
    int r = r0;
    for (int i = 0; i < n && ch[i].c <= c; i++)
        r = ch[i].rez;
    return r;
}

#define EV(i) (&ring[(i) & SHM_MASK])

/* ST bus accesses sit on a 4-cycle grid (the GLUE/MMU interleave), so a
 * register write lands on a multiple of 4 cycles into the line. Snapping
 * to it turns a timing error under 2 cycles into none at all. */
static inline double q4(double c)
{
    return 4.0 * floor(c / 4.0 + 0.5);
}

/* Pass 1: geometry of the frame starting at origin O (state *g at O,
 * events from index j). Fills ln[] / nln. Returns the counter span. */
static void glue_frame(vstate_t *g, uint32_t j, uint32_t iend, double O, double tc,
                       int cls, uint32_t *span_lo, uint32_t *span_hi)
{
    const int total = cls == 50 ? 313 : 263;
    const int vis_on  = cls == 50 ? 25 : 16;
    const int vis_off = cls == 50 ? 308 : 258;
    const int nobot   = cls == 50 ? 310 : 263;
    glue_ch_t ch[64];
    double s = 0;
    int vde = 0, next_left_off = 0;
    uint32_t cnt = ((uint32_t)g->reg[0x01] << 16) | ((uint32_t)g->reg[0x03] << 8) |
                   (g_ste ? (uint32_t)(g->reg[0x0D] & 0xFEu) : 0u);
    const uint32_t top = pistorm_stram_top();
    uint32_t lo = UINT32_MAX, hi = 0;

    nln = 0;
    for (int L = 0; L < total && L < MAXL; L++) {
        line_t *l = &ln[nln++];
        memset(l, 0, sizeof *l);
        l->s = s;
        l->ev0 = j;
        const int f0 = vs_freq(g);
        const int r0 = g->reg[0x60] & 3;
        int n = 0, cnt_w = 0;

        /* events up to cycle 56 decide the line length */
        for (int pass = 0; pass < 2; pass++) {
            const double lim = pass ? (vs_freq(g) == 71 ? 224.0 : vs_freq(g) == 60 ? 508.0 : 512.0)
                                    : 56.0;
            if (pass)
                l->len = (uint16_t)lim;
            while (j != iend) {
                const shm_ev_t *e = EV(j);
                const double c = ((double)e->t - O - s) / tc;
                if (c >= lim)
                    break;
                vs_apply(g, e);
                if (SHM_KIND(e->a) == SHM_K_REG) {
                    const uint32_t r = e->a & 0x7Fu;
                    if ((r == 0x60 || r == 0x0A) && n < 64) {
                        ch[n].c = (float)q4(c);
                        ch[n].f = (uint8_t)vs_freq(g);
                        ch[n].rez = g->reg[0x60] & 3;
                        n++;
                    } else if (g_ste && (r == 0x05 || r == 0x07 || r == 0x09))
                        cnt_w = 1;
                }
                j++;
            }
        }

        l->vis = L >= vis_on && L < vis_off;
        l->vde = (uint8_t)vde;

        /* ---- horizontal: Hatari's STF table (video.c, wakestate 1) ---- */
        int left_off = f_at(ch, n, f0, 4) == 71 || next_left_off;
        int no_de = 0, blank = 0, start, flags = 0;
        next_left_off = 0;
        if (left_off && f_at(ch, n, f0, 56) == 71 && f_at(ch, n, f0, 164) == 71) {
            no_de = 1;                     /* a whole hi-res line in a colour frame */
            start = 4;
        } else {
            if (!left_off) {
                if (f_at(ch, n, f0, 28) == 71)      { no_de = 1; blank = 1; }
                else if (f_at(ch, n, f0, 56) == 71) no_de = 1;
            }
            if (left_off)                             start = 4;
            else if (f_at(ch, n, f0, 52) == 60)       start = 52;
            else if (f_at(ch, n, f0, 56) == 50)       start = 56;
            else                                      { start = 56; no_de = 1; }
        }
        int bytes = 0;
        if (!no_de) {
            bytes = 160;
            if (left_off)          { bytes += 26; flags |= LF_LEFT; }
            else if (start == 52)  { bytes += 2;  flags |= LF_LEFT2; }
            if (f_at(ch, n, f0, 164) == 71) {
                bytes -= 106;      flags |= LF_MID;
            } else if (f_at(ch, n, f0, 372) == 60) {
                bytes -= 2;        flags |= LF_RIGHT2;
            } else if (f_at(ch, n, f0, 376) == 50) {
                /* a normal end */
            } else {
                bytes += 44;       flags |= LF_RIGHT;
                if (f_at(ch, n, f0, 376) == 71 ||
                    f_at(ch, n, f0, (double)l->len - 50.0) == 71) {
                    bytes += 22;   flags |= LF_FULL;
                    next_left_off = 1;   /* and no left border on the next line */
                }
            }
        } else
            flags |= LF_NODE;
        /* a whole line at 60 Hz starts at 52 and ends at 372: that is just
         * a normal 60 Hz line, not a +2/-2 trick (bytes stay 160) */
        if ((flags & (LF_LEFT2 | LF_RIGHT2)) == (LF_LEFT2 | LF_RIGHT2))
            flags &= ~(LF_LEFT2 | LF_RIGHT2);

        l->blank  = (uint8_t)blank;
        l->fstart = (int16_t)start;
        l->pstart = (int16_t)(left_off ? 0 : start);
        /* the pixel mode: after a left-border hi/lo switch the Shifter is
         * back in low/medium by cycle 16 (Hatari LINE_LEFT_STAB_LOW) */
        l->rez    = (uint8_t)rez_at(ch, n, r0, left_off ? 16 : start);
        if (l->rez > 1)
            no_de = 1, flags |= LF_NODE;     /* hi-res fetch in a colour frame */
        l->de     = (uint8_t)(vde && !no_de);
        l->bytes  = (uint16_t)(l->de ? bytes : 0);
        if (l->de) {
            if (g_ste) {
                l->hs = g->reg[0x65] & 15u;
                if (l->hs)
                    l->extra = l->rez == 0 ? 8 : 4;
            }
            l->addr = cnt;
            if (cnt + l->bytes + l->extra <= top) {
                if (cnt < lo) lo = cnt;
                if (cnt + l->bytes + l->extra > hi) hi = cnt + l->bytes + l->extra;
            } else
                l->de = 0;
            cnt += l->bytes + l->extra + (g_ste ? 2u * g->reg[0x0F] : 0u);
        }
        if (vde)
            for (int b = 0; b < 7; b++)
                if (flags & (1 << b))
                    st.lf[b]++;
        l->flags = (uint8_t)flags;
        if (cnt_w)
            cnt = ((uint32_t)g->reg[0x05] << 16) | ((uint32_t)g->reg[0x07] << 8) | g->reg[0x09];

        /* ---- vertical: decided at cycle 502 of the line before ---- */
        const int fv = f_at(ch, n, f0, l->len >= 503 ? 502.0 : (double)l->len - 1);
        if (!vde) {
            if (L == 33 && (fv == 60 || fv == 71)) { vde = 1; if (cls == 50) st.top_off++; }
            else if (L == 62 && fv == 50)          vde = 1;
        } else {
            if ((L == 233 && fv == 60) || (L == 262 && fv == 50))
                vde = 0;
            else if (L + 1 == nobot) {
                vde = 0;
                if (cls == 50) st.bottom_off++;
            }
        }
        s += l->len * tc;
    }
    if (hi <= lo) { lo = 0; hi = 0; }
    *span_lo = lo;
    *span_hi = hi;
}

/* ---- pixels -------------------------------------------------------------- */
static inline uint32_t st_rgb(uint16_t w)
{
    uint32_t r, g, b;
    if (g_ste) {
        r = (((w >> 8) & 7u) << 1) | ((w >> 11) & 1u);
        g = (((w >> 4) & 7u) << 1) | ((w >> 7) & 1u);
        b = ((w & 7u) << 1) | ((w >> 3) & 1u);
        r |= r << 4; g |= g << 4; b |= b << 4;
    } else {
        r = (w >> 8) & 7u; g = (w >> 4) & 7u; b = w & 7u;
        r = (r << 5) | (r << 2) | (r >> 1);
        g = (g << 5) | (g << 2) | (g >> 1);
        b = (b << 5) | (b << 2) | (b >> 1);
    }
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* decode one line into palette indices on the half-cycle grid */
static void decode_line(const line_t *l, const uint8_t *src, uint8_t *idx, int hw)
{
    const int planes = l->rez == 0 ? 4 : 2;
    const int ppg    = 16;                         /* pixels per group */
    const int groups = (l->bytes + l->extra + planes * 2 - 1) / (planes * 2);
    const int shown  = l->rez == 0 ? l->bytes * 2 : l->bytes * 4;   /* pixels */
    const int hx0    = 2 * (l->pstart - cfg_x0);   /* half-cycle of pixel 0 */
    int p = -(int)l->hs;                           /* fine scroll drops pixels */
    for (int gi = 0; gi < groups; gi++) {
        const uint8_t *gp = src + gi * planes * 2;
        uint16_t pw[4] = { 0, 0, 0, 0 };
        for (int k = 0; k < planes; k++)
            pw[k] = (uint16_t)((gp[2 * k] << 8) | gp[2 * k + 1]);
        for (int b = 15; b >= 0; b--, p++) {
            if (p < 0 || p >= shown)
                continue;
            uint8_t c = 0;
            for (int k = 0; k < planes; k++)
                c |= (uint8_t)(((pw[k] >> b) & 1u) << k);
            if (l->rez == 0) {
                const int hx = hx0 + 2 * p;
                if (hx >= 0 && hx + 1 < hw) { idx[hx] = c; idx[hx + 1] = c; }
            } else {
                const int hx = hx0 + p;
                if (hx >= 0 && hx < hw) idx[hx] = c;
            }
        }
        (void)ppg;
    }
}

/* Fetch cycle of byte offset `off` in line l, as an absolute tick. */
static inline double fetch_tick(const line_t *l, uint32_t off, double O, double tc)
{
    return O + l->s + ((double)l->fstart + 2.0 * (double)off) * tc;
}

static int find_line(uint32_t a, int mono)
{
    /* lines are in address order unless an STE counter write moved back */
    for (int L = 0; L < nln; L++) {
        const line_t *l = &ln[L];
        if (l->de && a >= l->addr && a < l->addr + l->bytes + l->extra)
            return L;
    }
    (void)mono;
    return -1;
}

/* binary search over DE lines when addresses ascend (the usual case) */
static int dl[MAXL], ndl, dl_sorted;

static int find_line_fast(uint32_t a)
{
    if (!dl_sorted)
        return find_line(a, 0);
    int lo = 0, hi = ndl - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        const line_t *l = &ln[dl[m]];
        if (a < l->addr) hi = m - 1;
        else if (a >= l->addr + l->bytes + l->extra) lo = m + 1;
        else return dl[m];
    }
    return -1;
}

static double  last_origin;

static uint64_t cal_alt;                 /* reads placed via the other buffer */
static void stats_print(void)
{
    const double hz = (double)tick_hz();
    const uint64_t now = shm_ticks();
    if (!st.t0) { st.t0 = now; return; }
    if ((double)(now - st.t0) < 5.0 * hz)
        return;
    const double fr = st.frames ? (double)st.frames : 1.0;
    const double c2us = 1.0 / 8.021247;          /* cycles -> us */
    fprintf(stderr,
            "[SHM] %dHz P=%.1fus frames=%llu cal=%llu/%u err=%.1f..%.1f avg %.2f cyc rej=%llu "
            "alt=%llu vblofs=%.0fcyc(%s) ev/f=%.0f ram/f=%.0f undone/f=%.1f lapped=%llu render=%.0fus | "
            "lines/f left=%.1f +2=%.1f right=%.1f -2=%.1f mid=%.1f full=%.1f node=%.1f top_off=%llu bot_off=%llu\n",
            L_cls, L_P * 1e6 / hz, (unsigned long long)st.frames,
            (unsigned long long)st.cal_n, (unsigned)cal_tries,
            st.cal_n ? st.cal_err_min : 0.0, st.cal_n ? st.cal_err_max : 0.0,
            st.cal_n ? st.cal_err_sum / (double)st.cal_n : 0.0,
            (unsigned long long)st.cal_rej, (unsigned long long)cal_alt,
            C_vblofs_n ? C_vblofs / (L_P / frame_cycles(L_cls)) : cfg_vblofs,
            C_vblofs_n ? "measured" : "cfg",
            (double)st.ev / fr, (double)st.ev_ram / fr, (double)st.undone / fr,
            (unsigned long long)st.lapped, (double)st.render_ticks / fr * 1e6 / hz,
            st.lf[0] / fr, st.lf[1] / fr, st.lf[2] / fr, st.lf[3] / fr, st.lf[4] / fr,
            st.lf[6] / fr, st.lf[5] / fr,
            (unsigned long long)st.top_off, (unsigned long long)st.bottom_off);
    (void)c2us;
    memset(&st, 0, sizeof st);
    st.t0 = now;
    cal_tries = 0;
}

/* Calibration: a counter read inside this frame -> its true origin. */
/* PISTORM_SHM_DEBUG=3: which buffer the mirror draws vs where the real
 * Shifter is. FastBobs, 5 Nov 2026: every counter read rejected (cal=0,
 * rej=all) while balls showed half drawn - is the mirror drawing a buffer
 * the Shifter is not? Capped so a run stays readable. */
/* The base the PREVIOUS rendered frame was drawn from (first display line).
 * A counter read that falls in no line of this frame is tried against it -
 * see OTHER BUFFER in cal_process. */
static uint32_t prev_real_base, cur_real_base;

static int dbg3_left = 600;
static int dbg3_armed;              /* a frame with heavy screen writes seen */
static void dbg3_reject(const char *why, uint32_t cnt, double t, double O,
                        double tc, int L)
{
    if (cfg_dbg < 3 || !dbg3_armed || dbg3_left <= 0)
        return;
    dbg3_left--;
    const double c = (t - O) / tc;
    uint32_t base = 0, end = 0;
    for (int i = 0; i < nln; i++)
        if (ln[i].de) {
            if (!base) base = ln[i].addr;
            end = ln[i].addr + ln[i].bytes + ln[i].extra;
        }
    fprintf(stderr, "[SHM] cal REJECT %s cnt=%06X at line %d cyc %d | mirror "
            "frame %06X..%06X line=%d\n", why, (unsigned)cnt,
            (int)(c / 512.0), (int)fmod(c, 512.0), (unsigned)base,
            (unsigned)end, L);
}

static void cal_process(double O, double tc, double P)
{
    static uint32_t cal_r;
    static int rej_run;
    const uint32_t w = __atomic_load_n(&cal_w, __ATOMIC_ACQUIRE);
    if (w - cal_r > CAL_N)
        cal_r = w - CAL_N;
    for (; cal_r != w; cal_r++) {
        const cal_t c = cal_ring[cal_r % CAL_N];
        const double t = (double)c.t;
        if (t >= O + P)
            break;                           /* a later frame's: keep it */
        if (t < O)
            continue;                        /* a frame not drawn */
        /* WHERE THE BEAM IS, from the counter. The board's Shifter is an
         * ST's: from the base the reload took ($FF8201/03 only) its counter
         * advances by the bytes each line really fetches - no STE line
         * width, no fine-scroll prefetch, no base low byte. With `shifter
         * ste` our lines are laid out the STE way, so the read is walked
         * line by line in ST terms and then placed in our layout. FastBobs
         * (5 Nov 2026, PISTORM_SHM_DEBUG=3) runs with an STE line width of
         * 48 words: read in our layout, its counter put the beam 15-35
         * lines from where it was. Its real base is also tried against the
         * PREVIOUS frame's (OTHER BUFFER): there every read sat in the
         * other of its two buffers, was rejected "not in frame", and with
         * no read accepted nothing could pull the frame origin back -
         * half-drawn balls for as long as it ran. A read placed through
         * the other buffer is a large error, taken by the >2-line path. */
        uint32_t cnt = 0;
        int L = -1;
        for (int b = 0; b < 2 && L < 0; b++) {
            const uint32_t rb = b ? prev_real_base : cur_real_base;
            if (!rb || (b && rb == cur_real_base) || c.cnt < rb)
                continue;
            uint32_t acc = rb;
            for (int i = 0; i < ndl; i++) {
                const line_t *dli = &ln[dl[i]];
                if (c.cnt < acc + dli->bytes) {
                    L = dl[i];
                    cnt = dli->addr + (c.cnt - acc);
                    if (b)
                        cal_alt++;
                    break;
                }
                acc += dli->bytes;
            }
        }
        if (L < 0) { st.cal_rej++; dbg3_reject("not-in-frame", c.cnt, t, O, tc, L); continue; }
        const line_t *l = &ln[L];
        const uint32_t off = cnt - l->addr;
        if (off == 0 || off >= l->bytes) { st.cal_rej++; dbg3_reject("edge", c.cnt, t, O, tc, L); continue; }   /* at an edge */
        /* the counter steps a word every 4 cycles: it reads `off` for the
         * 4 cycles after that word's fetch slot opens - aim at the middle */
        const double pred = fetch_tick(l, off, O, tc) + (2.0 + cfg_calk) * tc;
        const double err = t - pred;                    /* ticks */
        const double errc = err / tc;
        if (fabs(errc) > 2.0 * 512.0) {                 /* > 2 lines: not trusted yet */
            st.cal_rej++;
            dbg3_reject("far", c.cnt, t, O, tc, L);
            /* the first read, or the model has drifted away for good:
             * take the counter's word for it */
            if (!C_when || ++rej_run >= 8) {
                C_A = O + err;
                C_when = c.t;
                rej_run = 0;
            }
            continue;
        }
        rej_run = 0;
        if (cfg_dbg >= 2)
            fprintf(stderr, "[SHM] cal line %d off %u err %.2f cyc\n", L, off, errc);
        st.cal_n++;
        st.cal_err_sum += errc;
        if (st.cal_n == 1 || errc < st.cal_err_min) st.cal_err_min = errc;
        if (st.cal_n == 1 || errc > st.cal_err_max) st.cal_err_max = errc;
        /* this frame's true origin. One read is only good to the counter's
         * 4-cycle step, so small errors are averaged in (1/4 a frame);
         * anything larger is a real move and is taken at once. */
        /* A second-order loop: the phase takes 1/4 of the error, the period
         * 1/16 of it spread over the frames since the last read, so a
         * crystal difference between the ST and the Pi leaves no standing
         * error. */
        C_A = O + (fabs(errc) > 8.0 ? err : err / 4.0);
        if (C_when) {
            double nf = floor(((double)c.t - (double)C_when) / P + 0.5);
            if (nf >= 1.0 && nf < 50.0)
                L_P += err / 16.0 / nf;
        }
        C_when = c.t;
        /* origin relative to the VBL envelope, for when calibration stops */
        double rel = C_A - L_A;
        rel -= floor(rel / L_P) * L_P;
        if (rel > L_P / 2) rel -= L_P;
        C_vblofs = C_vblofs_n ? C_vblofs + (rel - C_vblofs) / 16.0 : rel;
        C_vblofs_n++;
    }
}

/* 71 Hz mono: 640x400, centred on a 960x540 canvas (16:9: x2 = 1920x1080)
 * whose margins are the picture's background colour, not black. */
#define MONO_W 960
#define MONO_H 540
static void render_mono(uint32_t *dst, uint32_t pitch, const vstate_t *s)
{
    uint32_t base = ((uint32_t)s->reg[0x01] << 16) | ((uint32_t)s->reg[0x03] << 8) |
                    (g_ste ? (uint32_t)(s->reg[0x0D] & 0xFEu) : 0u);
    const uint32_t top = pistorm_stram_top();
    const uint32_t fg = (s->pal[0] & 1u) ? 0xFF000000u : 0xFFFFFFFFu;
    const uint32_t bg = fg ^ 0x00FFFFFFu;
    const int ox = (MONO_W - 640) / 2, oy = (MONO_H - 400) / 2;
    for (int y = 0; y < MONO_H; y++) {
        uint32_t *o = dst + (size_t)y * pitch;
        const int sy = y - oy;
        const uint32_t a = base + (uint32_t)sy * (80u + (g_ste ? 2u * s->reg[0x0F] : 0u));
        if (sy < 0 || sy >= 400 || a + 80u > top) {
            for (int x = 0; x < MONO_W; x++) o[x] = bg;
            continue;
        }
        const uint8_t *r = g_natmem + a;
        for (int x = 0; x < ox; x++) o[x] = bg;
        for (int x = 0; x < 640; x++)
            o[ox + x] = ((r[x >> 3] >> (7 - (x & 7))) & 1u) ? fg : bg;
        for (int x = ox + 640; x < MONO_W; x++) o[x] = bg;
    }
}

static void mono_out(uint32_t *w, uint32_t *h, uint32_t *sw, uint32_t *sh)
{
    *w = MONO_W; *h = MONO_H; *sw = MONO_W; *sh = MONO_H;
}

/* copy the frame's screen memory [lo, hi) (+8 for a partial last group) */
static void snap_copy(uint32_t lo, uint32_t *hi)
{
    if (*hi - lo > BUFMAX)
        *hi = lo + BUFMAX;
    if (*hi > lo) {
        uint32_t n = *hi - lo;
        if (*hi + 8u <= pistorm_stram_top())
            n += 8u;
        memcpy(snapbuf, g_natmem + lo, n);
    }
}

/* pass 3: draw lines ln[0..nln) from state *p0, applying ring events
 * [i0, ie) at their beam positions */
static void draw_frame(uint32_t *dst, uint32_t pitch, const vstate_t *p0,
                       uint32_t i0, uint32_t ie, double O, double tc, int cls,
                       uint32_t lo, uint32_t hi)
{
    const int top = cls == 50 ? cfg_top50 : cfg_top60;
    const int hw = 2 * cfg_w;
    static uint8_t idx[2 * 512 + 64];
    vstate_t p = *p0;
    uint32_t pal32[16];
    for (int i = 0; i < 16; i++)
        pal32[i] = st_rgb(p.pal[i]);
    uint32_t j = i0;
    for (int y = 0; y < ROWS; y++) {
        uint32_t *o = dst + (size_t)y * pitch;
        for (int x = 0; x < hw; x++)
            o[x] = 0xFF000000u;
    }
    for (int L = 0; L < nln; L++) {
        const line_t *l = &ln[L];
        const uint32_t jend = (L + 1 < nln) ? ln[L + 1].ev0 : ie;
        const int y = L - top;
        const int drawn = y >= 0 && y < ROWS && l->vis && !l->blank;
        uint32_t *o = drawn ? dst + (size_t)y * pitch : NULL;
        if (drawn) {
            memset(idx, 0, (size_t)hw);
            if (l->de && l->addr >= lo && l->addr + l->bytes + l->extra <= hi)
                decode_line(l, snapbuf + (l->addr - lo), idx, hw);
        }
        int hx = 0;
        for (; j != jend; j++) {
            const shm_ev_t *e = EV(j);
            if (SHM_KIND(e->a) == SHM_K_PAL && drawn) {
                const double c = q4(((double)e->t - O - l->s) / tc);
                int ex = (int)lround(2.0 * (c - cfg_paldx - cfg_x0));
                if (ex > hw) ex = hw;
                for (; hx < ex; hx++)
                    o[hx] = pal32[idx[hx]];
                pal32[e->a & 15u] = st_rgb(e->n);
            } else if (SHM_KIND(e->a) == SHM_K_PAL)
                pal32[e->a & 15u] = st_rgb(e->n);
            vs_apply(&p, e);
        }
        if (drawn)
            for (; hx < hw; hx++)
                o[hx] = pal32[idx[hx]];
    }
}

static void colour_out(uint32_t *w, uint32_t *h, uint32_t *sw, uint32_t *sh)
{
    *w = (uint32_t)(2 * cfg_w);
    *h = ROWS;
    /* the shape the presenter scales: square pixels after the pixel aspect */
    *sw = (uint32_t)((cfg_w * cfg_par_n + cfg_par_d / 2) / cfg_par_d);
    *sh = ROWS;
}

/* No VBL lock yet - the first frames after a reset, or code that runs with
 * the VBL masked - so no beam timing: draw the screen as the registers and
 * memory stand, as a plain frame with its borders in colour 0. Without
 * this the first second of every boot came out in the old borderless
 * mirror, in black bars. At most 50 times a second. */
static int render_static(uint32_t *dst, uint32_t pitch, uint32_t *w, uint32_t *h,
                         uint32_t *sw, uint32_t *sh)
{
    static uint64_t last;
    const uint64_t now = shm_ticks();
    if (last && now - last < tick_hz() / 50u)
        return 0;
    last = now;
    vstate_t live;
    memcpy(live.pal, cpu_pal, sizeof live.pal);
    memcpy(live.reg, cpu_reg, sizeof live.reg);
    const int f = vs_freq(&live);
    if (f == 71) {
        render_mono(dst, pitch, &live);
        mono_out(w, h, sw, sh);
        return 1;
    }
    vstate_t g = live;
    uint32_t lo, hi;
    glue_frame(&g, 0, 0, 0.0, 1.0, f, &lo, &hi);   /* no events */
    snap_copy(lo, &hi);
    draw_frame(dst, pitch, &live, 0, 0, 0.0, 1.0, f, lo, hi);
    colour_out(w, h, sw, sh);
    return 1;
}

int shm_render(uint32_t *dst, uint32_t pitch, uint32_t *w, uint32_t *h,
               uint32_t *sw, uint32_t *sh)
{
    if (!shm_on)
        return -1;
    const uint64_t r0 = shm_ticks();

    /* VBL sightings -> lock */
    int fresh = 0;
    {
        const uint32_t sw_ = __atomic_load_n(&sight_w, __ATOMIC_ACQUIRE);
        static uint32_t sr;
        if (sw_ - sr > SIGHT_N)
            sr = sw_ - SIGHT_N;
        fresh = sr != sw_;
        for (; sr != sw_; sr++)
            lock_sighting(sight[sr % SIGHT_N], sight_cls[sr % SIGHT_N]);
    }
    /* no lock. (A lock that disagrees with the registers - the first
     * sightings after a reset can read as 71 Hz on a colour screen - is
     * no lock: lock_sighting votes the registers at each sighting. They
     * are not read here: left-border code sets 71 Hz mode for a few cycles
     * on every line it opens, and a render landing there lost the lock.) */
    if (!L_have || !L_cls)
        return render_static(dst, pitch, w, h, sw, sh);
    if (cfg_dbg)
        stats_print();

    const double P  = L_P;
    const double tc = P / frame_cycles(L_cls);
    const double now = (double)shm_ticks();
    int calibrated;
    /* The frame to draw: the one a VBL just ended - the middle of the frame
     * before the newest sighting, whenever the render loop woke. With the
     * VBL masked (no sightings: a diagnostic cartridge, a crash) the frame
     * that ended by the clock. */
    const double x = fresh ? (double)L_last_sight - P / 2
                           : now - P - 50e-6 * (double)tick_hz();
    double O = frame_origin_at(x, &calibrated);
    if (O + P > now)
        return 0;                         /* not over yet */
    if (fabs(O - last_origin) < P / 2)
        return 0;
    last_origin = O;

    if (!rs_ok)
        rs_reset_from_cpu();

    /* replay registers up to the frame's origin */
    uint32_t iw = __atomic_load_n(&ring_w, __ATOMIC_ACQUIRE);
    if (iw - rs_i > SHM_RING - 4096u) {
        st.lapped++;
        rs_reset_from_cpu();
        iw = rs_i;
    }
    /* COUNTER RELOAD. The Shifter does not take the base at the frame's
     * origin: the GLUE copies $FF8201/03 into the video counter at line
     * 310 (50 Hz) / 260 (60 Hz), cycle 56 (Hatari video.c,
     * RESTART_VIDEO_COUNTER_*) - a base written after that shows a frame
     * later. FastBobs writes its base at ~line 312 every frame; taken at
     * the origin it showed the buffer still being drawn. The frame is
     * drawn from the base as it stood at the reload; rs keeps the true
     * register state. */
    uint8_t rl_reg[3] = { rs.reg[0x01], rs.reg[0x03], rs.reg[0x0D] };
    double R = O;
    if (L_cls == 50 || L_cls == 60) {
        const double tcr = P / frame_cycles(L_cls);
        const int total = L_cls == 50 ? 313 : 263, restart = L_cls == 50 ? 310 : 260;
        const double lc = L_cls == 50 ? 512.0 : 508.0;
        R = O - ((double)(total - restart) * lc - 56.0) * tcr;
    }
    while (rs_i != iw && (double)EV(rs_i)->t < O) {
        if (SHM_KIND(EV(rs_i)->a) == SHM_K_REG && (double)EV(rs_i)->t < R) {
            const uint32_t r = EV(rs_i)->a & 0x7Fu;
            if (r == 0x01) rl_reg[0] = (uint8_t)EV(rs_i)->n;
            else if (r == 0x03) rl_reg[1] = (uint8_t)EV(rs_i)->n;
            else if (r == 0x0D) rl_reg[2] = (uint8_t)EV(rs_i)->n;
        }
        if (cfg_dbg >= 3 && dbg3_armed && dbg3_left > 0 &&
            SHM_KIND(EV(rs_i)->a) == SHM_K_REG) {
            const uint32_t r = EV(rs_i)->a & 0x7Fu;
            if (r == 0x01 || r == 0x03 || r == 0x0D || r == 0x05 ||
                r == 0x07 || r == 0x09) {
                /* placed in the frame before this one (origin O - P) */
                const double tcx = P / frame_cycles(L_cls);
                const double c = ((double)EV(rs_i)->t - (O - P)) / tcx;
                dbg3_left--;
                fprintf(stderr, "[SHM] reg $FF82%02X=%02X at line %d cyc %d "
                        "(frame before origin %+.0f cyc)\n", (unsigned)r,
                        (unsigned)EV(rs_i)->n, (int)(c / 512.0),
                        (int)fmod(c, 512.0), (O - (double)EV(rs_i)->t) / tcx);
            }
        }
        vs_apply(&rs, EV(rs_i));
        rs_i++;
    }
    const uint32_t i0 = rs_i;
    /* events of this frame end where the next frame begins */
    uint32_t ie = i0;
    while (ie != iw && (double)EV(ie)->t < O + P)
        ie++;

    st.frames++;
    st.ev += ie - i0;

    if (L_cls == 71) {
        render_mono(dst, pitch, &rs);
        mono_out(w, h, sw, sh);
        st.render_ticks += shm_ticks() - r0;
        return 1;
    }

    /* pass 1: geometry */
    vstate_t g = rs;
    g.reg[0x01] = rl_reg[0];             /* the base as the counter reload took it */
    g.reg[0x03] = rl_reg[1];
    g.reg[0x0D] = rl_reg[2];
    uint32_t lo, hi;
    glue_frame(&g, i0, ie, O, tc, L_cls, &lo, &hi);
    ndl = 0;
    dl_sorted = 1;
    for (int L = 0; L < nln; L++)
        if (ln[L].de) {
            if (ndl && ln[L].addr < ln[dl[ndl - 1]].addr + ln[dl[ndl - 1]].bytes + ln[dl[ndl - 1]].extra)
                dl_sorted = 0;
            dl[ndl++] = L;
        }
    prev_real_base = cur_real_base;
    cur_real_base = ((uint32_t)rl_reg[0] << 16) | ((uint32_t)rl_reg[1] << 8);
    if (hi > lo) {
        uint32_t fb = hi - (((uint32_t)rl_reg[0] << 16) | ((uint32_t)rl_reg[1] << 8)) + 512u;
        if (fb >= 32000u && fb < 0x40000u)
            g_frame_bytes = fb;
    }

    /* the frame's video counter read, if the CPU took one */
    if (cfg_cal) {
        cal_process(O, tc, P);
        if (calibrated)
            st.cal_frames++;
        /* ask for one in the middle of the next frame's display */
        const double nxt = O + P;
        cal_P = (uint64_t)P;
        cal_hi = (uint64_t)(nxt + 180.0 * 512.0 * tc);
        shm_cal_lo = (uint64_t)(nxt + 80.0 * 512.0 * tc);
    }

    /* pass 2: screen memory as each word was fetched */
    snap_copy(lo, &hi);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const uint32_t iu = __atomic_load_n(&ring_w, __ATOMIC_ACQUIRE);
    if (iu - i0 >= SHM_RING) {
        st.lapped++;                      /* cannot undo - draw it as copied */
    } else {
        unsigned fr_ram = 0, fr_undone = 0;
        for (uint32_t i = iu; i-- != i0; ) {
            const shm_ev_t *e = EV(i);
            const uint32_t k = SHM_KIND(e->a);
            if (k != SHM_K_RAM16 && k != SHM_K_RAM8)
                continue;
            st.ev_ram++;
            fr_ram++;
            const uint32_t a = SHM_ADDR(e->a);
            if (a < lo || a >= hi)
                continue;
            const int L = find_line_fast(a);
            if (L < 0)
                continue;
            if ((double)e->t <= fetch_tick(&ln[L], a - ln[L].addr, O, tc))
                continue;                 /* landed before the fetch: shown */
            st.undone++;
            fr_undone++;
            if (cfg_dbg >= 3 && dbg3_armed && dbg3_left > 0 && fr_undone <= 3) {
                /* a few undone writes per frame: where, and how late */
                const double fc = (fetch_tick(&ln[L], a - ln[L].addr, O, tc) - O) / tc;
                const double wc = ((double)e->t - O) / tc;
                dbg3_left--;
                fprintf(stderr, "[SHM] undo %06X line %d: fetched line %d cyc %d, "
                        "written line %d cyc %d\n", (unsigned)a, L,
                        (int)(fc / 512.0), (int)fmod(fc, 512.0),
                        (int)(wc / 512.0), (int)fmod(wc, 512.0));
            }
            if (k == SHM_K_RAM16) {
                snapbuf[a - lo] = (uint8_t)(e->o >> 8);
                if (a + 1u < hi)
                    snapbuf[a + 1u - lo] = (uint8_t)e->o;
            } else
                snapbuf[a - lo] = (uint8_t)e->o;
        }
        if (cfg_dbg >= 3) {
            /* armed by SUSTAINED heavy drawing - 50 frames in a row over
             * 3000 writes. One frame was not enough: the boot screen clear
             * tripped it and the 600 lines were gone before the program
             * under test started (FastBobs, 5 Nov). */
            static int heavy_run;
            heavy_run = fr_ram > 3000 ? heavy_run + 1 : 0;
            if (!dbg3_armed && heavy_run >= 50) {
                dbg3_armed = 1;
                fprintf(stderr, "[SHM] debug 3 armed: 50 frames over 3000 screen writes\n");
            }
            if (dbg3_armed && dbg3_left > 0) {
                dbg3_left--;
                fprintf(stderr, "[SHM] frame %06X..%06X (%d lines) ram=%u undone=%u%s\n",
                        (unsigned)lo, (unsigned)hi, nln, fr_ram, fr_undone,
                        calibrated ? " cal" : "");
            }
        }
    }

    /* pass 3: draw */
    draw_frame(dst, pitch, &rs, i0, ie, O, tc, L_cls, lo, hi);
    colour_out(w, h, sw, sh);
    st.render_ticks += shm_ticks() - r0;
    return 1;
}
