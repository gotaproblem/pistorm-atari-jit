/*
 * raster_trace.c - where on the REAL screen a palette write lands
 *
 * For raster effects on the Shifter output (sky bands, Timer B colour
 * splits): every write to $FF8240-$FF825E is logged with the time and the
 * real Shifter's video address counter ($FF8205/07/09) read straight after
 * it - so the log says which screen line the colour actually changed on.
 * Every interrupt the CPU takes is logged too (level 4 = VBL, 6 = MFP:
 * Timer B lives there), and at each VBL the screen base ($FF8201/03) and
 * the real MFP's Timer B control and current count ($FFFA1B/$FFFA21 -
 * reading the data register returns the running counter). Guest writes
 * to Timer B's registers and to the MFP interrupt registers are logged
 * with the video counter too, so a raster chain that starts on the wrong
 * line shows where its count came from.
 * Bands that jump show up as the same write landing on different lines
 * from frame to frame; tools/raster-trace/analyse.py prints that.
 *
 *   PISTORM_RASTER_TRACE=/home/pistorm/raster.txt   on, writes the file
 *                                                   when the emulator stops
 * Guest reads of Timer B's data register ($FFFA21) are logged too (R): a
 * handler that waits for the next line by polling it shows which count
 * its first read found, so a poll that started after the line it was
 * waiting for had already ended (and so waited one line more) is seen.
 *
 *   PISTORM_RASTER_VCOUNT=1                         also read the real video
 *       counter at each palette/MFP write and Timer B at each VBL. Those are
 *       bus reads inside the guest's handlers: a game whose handlers nest
 *       (Shadow of the Beast II) crashed with them on. Off by default: the
 *       timing - IPL seen, interrupt taken, colour written - needs none.
 *
 * Off costs one load of a zero flag at each palette write and interrupt.
 * On, each palette write costs three bus reads (~2-3 us) - small against a
 * 64 us line, and the same for every write, so the spread it measures is
 * the emulator's, not the trace's. The buffer is a ring holding the most
 * recent 2M events (about 30 s of a busy raster screen with sample music):
 * stop the emulator while the screen of interest is showing and the trace
 * ends on it, however long the game took to get there.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platforms/atari/raster_trace.h"

uint8_t ps_read_8(uint32_t address);          /* gpio/ps_protocol.c */

/* Timer B window counters, kept by the CPU thread's bus lock path
 * (gpio/ps_protocol.c) while ps_rt_win is set; arch-timer ticks */
extern volatile int ps_rt_win;
extern uint64_t ps_rt_bus_n, ps_rt_bus_ticks, ps_rt_bus_max, ps_rt_yield_ticks,
                ps_rt_gap_max, ps_rt_last_end;
uint64_t ps_rt_now(void);
uint64_t pistorm_guest_clocks(void);          /* cpu/events.cpp */

#define RT_MAX (1u << 21)                     /* events, 16 bytes each: 32 MB */

typedef struct {
    uint64_t t;                               /* arch timer ticks      */
    uint8_t  kind;                            /* 'P' palette, 'I' irq, 'M' MFP write */
    uint8_t  a;                               /* P: index  I: level  M: reg ($FFFAxx) */
    uint16_t v;                               /* P/M: value  I(4): TBCR<<8 | TBDR count */
    uint32_t c;                               /* P/M: video counter, I(4): screen base */
} rt_ev_t;

volatile int rt_on;
static int rt_vc;                           /* 1 = read the real counters (bus) */
static rt_ev_t *rt_buf;
static rt_ev_t *rt_last_irq;               /* gets its vector after the IACK */
static uint64_t rt_irq6_t;                  /* when the last level-6 was taken */
static uint64_t rt_win_ticks;               /* 100 us in arch-timer ticks      */
static volatile uint64_t rt_n;            /* events ever logged */
static uint64_t rt_w_t0, rt_w_clk0, rt_w_gov;  /* the open Timer B window */
static const char *rt_path;

static inline uint64_t rt_ticks(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

static inline uint64_t rt_hz(void)
{
#if defined(__aarch64__)
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? f : 54000000ull;
#else
    return 1;
#endif
}

__attribute__((constructor)) static void rt_init(void)
{
    rt_path = getenv("PISTORM_RASTER_TRACE");
    if (!rt_path || !*rt_path)
        return;
    rt_buf = (rt_ev_t *)calloc(RT_MAX, sizeof *rt_buf);
    if (!rt_buf) {
        fprintf(stderr, "[RASTER] no memory for the trace - off\n");
        return;
    }
    {
        const char *v = getenv("PISTORM_RASTER_VCOUNT");
        rt_vc = v && *v == '1';
    }
    rt_on = 1;
    fprintf(stderr, "[RASTER] tracing palette writes and interrupts -> %s "
                    "(written when the emulator stops)%s\n", rt_path,
            rt_vc ? "; reading the video counter at each write (bus reads)"
                  : "; no bus reads");
}

/* Several threads log: the CPU thread (palette, interrupts, MFP writes,
 * governor waits) and ipl_task (IPL levels it sees). Slots are claimed
 * atomically from a ring; the dump runs after the emulator has stopped. */
static inline rt_ev_t *rt_slot(void)
{
    uint64_t n = __atomic_fetch_add(&rt_n, 1u, __ATOMIC_RELAXED);

    return &rt_buf[n & (RT_MAX - 1u)];
}

/* four adjacent slots (the W record), so no other thread's event lands
 * inside it */
static inline rt_ev_t *rt_slot4(void)
{
    uint64_t n = __atomic_fetch_add(&rt_n, 4u, __ATOMIC_RELAXED);

    if (((n + 3u) & (RT_MAX - 1u)) < (n & (RT_MAX - 1u)))
        return NULL;                          /* would wrap: drop this one */
    return &rt_buf[n & (RT_MAX - 1u)];
}

static uint32_t rt_ns(uint64_t ticks)
{
    uint64_t ns = ticks * 1000000000ull / rt_hz();
    return ns > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)ns;
}

static uint16_t rt_ns16(uint64_t ticks)
{
    uint32_t ns = rt_ns(ticks);
    return ns > 0xFFFFu ? 0xFFFFu : (uint16_t)ns;
}

/* Timer B taken: from here to its colour write, time the CPU thread */
static void rt_win_open(uint64_t t)
{
    ps_rt_bus_n = ps_rt_bus_ticks = ps_rt_bus_max = 0;
    ps_rt_yield_ticks = ps_rt_gap_max = 0;
    ps_rt_last_end = t;
    rt_w_t0 = t;
    rt_w_gov = 0;
    rt_w_clk0 = pistorm_guest_clocks();
    __atomic_store_n(&ps_rt_win, 1, __ATOMIC_RELAXED);
}

/* the colour write: one W record of four slots -
 *   W  total_ns  guest_clocks  bus_n  bus_ns  bus_max_ns  yield_ns  gap_max_ns  gov_ns */
static void rt_win_close(void)
{
    uint64_t now = ps_rt_now();
    uint64_t clk = pistorm_guest_clocks() - rt_w_clk0;
    rt_ev_t *e;

    __atomic_store_n(&ps_rt_win, 0, __ATOMIC_RELAXED);
    if (now - ps_rt_last_end > ps_rt_gap_max)
        ps_rt_gap_max = now - ps_rt_last_end;
    if (!(e = rt_slot4()))
        return;
    e[0].t = now; e[0].kind = 'W'; e[0].a = 0;
    e[0].v = (uint16_t)(clk > 0xFFFFu ? 0xFFFFu : clk);
    e[0].c = rt_ns(now - rt_w_t0);
    e[1].t = now; e[1].kind = 'w'; e[1].a = 1;
    e[1].v = (uint16_t)(ps_rt_bus_n > 0xFFFFu ? 0xFFFFu : ps_rt_bus_n);
    e[1].c = rt_ns(ps_rt_bus_ticks);
    e[2].t = now; e[2].kind = 'w'; e[2].a = 2;
    e[2].v = rt_ns16(ps_rt_bus_max);
    e[2].c = rt_ns(ps_rt_yield_ticks);
    e[3].t = now; e[3].kind = 'w'; e[3].a = 3;
    e[3].v = rt_ns16(ps_rt_gap_max);
    e[3].c = rt_w_gov > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)rt_w_gov;
}

static uint32_t rt_vcount(void)
{
    return ((uint32_t)ps_read_8(0x00FF8205u) << 16) |
           ((uint32_t)ps_read_8(0x00FF8207u) << 8) |
            (uint32_t)ps_read_8(0x00FF8209u);
}

void rt_palette_slow(unsigned idx, uint16_t val)
{
    rt_ev_t *e;

    if (ps_rt_win)
        rt_win_close();
    e = rt_slot();

    if (!e)
        return;
    e->t = rt_ticks();
    e->kind = 'P';
    e->a = (uint8_t)idx;
    e->v = val;
    e->c = rt_vc ? rt_vcount() : 0xFFFFFFFFu;
}

void rt_irq_slow(int level)
{
    rt_ev_t *e = rt_slot();

    if (!e)
        return;
    e->t = rt_ticks();
    e->kind = 'I';
    e->a = (uint8_t)level;
    e->v = 0;
    e->c = 0;
    rt_last_irq = level == 4 ? NULL : e;
    if (level == 4 && rt_vc) {
        e->c = ((uint32_t)ps_read_8(0x00FF8201u) << 16) |
               ((uint32_t)ps_read_8(0x00FF8203u) << 8);
        e->v = (uint16_t)(((uint16_t)ps_read_8(0x00FFFA1Bu) << 8) |
                          ps_read_8(0x00FFFA21u));
    }
}

/* the vector the IACK returned, for the interrupt just logged (MFP:
 * $40 + channel - Timer B is $48, Timer A $4D, Timer C $45) */
void rt_irq_vector_slow(unsigned vec)
{
    if (rt_last_irq) {
        rt_last_irq->c = vec & 0xFFFFu;
        if ((vec & 0xFFu) == 0x48u) {      /* Timer B: watch the governor */
            rt_irq6_t = rt_last_irq->t;
            rt_win_open(ps_rt_now());       /* and time it to the colour write */
        }
        rt_last_irq = NULL;
    }
}

/* ipl_task: it now sees IPL level 6 on the bus (before any confirm).
 * Only entries into 6 - the analyser pairs each with the level-6
 * interrupt the CPU then takes; the other levels would only fill the ring. */
void rt_ipl_slow(int level)
{
    rt_ev_t *e;

    if (level != 6)
        return;
    e = rt_slot();
    e->t = rt_ticks();
    e->kind = 'L';
    e->a = (uint8_t)level;
    e->v = 0;
    e->c = 0;
}

/* the governor: a pause of ns, and whether it was skipped because an
 * interrupt handler was running (skipped = 1, ns = what it would have been).
 * Only pauses within 100 us of a Timer B interrupt are logged: the governor
 * checks every ~31 us, and logging them all (first try, 31 Oct 2026) filled
 * the buffer in 30 s, before the game reached its raster screen; within
 * 100 us of every level-6 interrupt is still all of them with an 11 kHz
 * sample timer running. */
void rt_gov_slow(int64_t ns, int skipped)
{
    uint64_t now = rt_ticks();
    rt_ev_t *e;

    if (!rt_win_ticks)
        rt_win_ticks = rt_hz() / 10000u;
    if (ps_rt_win && !skipped && ns > 0)
        rt_w_gov += (uint64_t)ns;
    if (now - rt_irq6_t > rt_win_ticks)
        return;
    if (!(e = rt_slot()))
        return;
    e->t = now;
    e->kind = 'G';
    e->a = (uint8_t)skipped;
    e->v = 0;
    e->c = ns < 0 ? 0u : ns > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)ns;
}

/* MFP registers worth seeing for a Timer B raster chain */
static int rt_mfp_reg(uint32_t a)
{
    switch (a) {
    case 0x00FFFA07u: case 0x00FFFA0Bu: case 0x00FFFA0Fu: case 0x00FFFA13u:
                                          /* IERA IPRA ISRA IMRA (bit 0 = Timer B) */
    case 0x00FFFA17u:                     /* VR (end-of-interrupt mode)        */
    case 0x00FFFA1Bu: case 0x00FFFA21u:   /* TBCR TBDR                          */
        return 1;
    }
    return 0;
}

void rt_vcount_slow(uint32_t raw, uint32_t given, uint32_t dur_ns)
{
    rt_ev_t *e = rt_slot();
    if (!e)
        return;
    e->t = rt_ticks();
    e->kind = 'V';
    e->a = (uint8_t)raw;
    e->v = (uint16_t)(given & 0xFFu);
    e->c = dur_ns;
}

void rt_mfp_read_slow(uint32_t addr, uint32_t val)
{
    rt_ev_t *e = rt_slot();
    (void)addr;
    if (!e)
        return;
    e->t = rt_ticks();
    e->kind = 'R';
    e->a = 0x21;
    e->v = (uint16_t)(val & 0xFFu);
    e->c = 0;
}

void rt_mfp_slow(uint32_t addr, uint32_t val, int word)
{
    uint32_t a = addr & 0x00FFFFFFu;
    rt_ev_t *e;

    if (word)
        a |= 1u;                          /* the MFP sits on the odd byte */
    if (!rt_mfp_reg(a) || !(e = rt_slot()))
        return;
    e->t = rt_ticks();
    e->kind = 'M';
    e->a = (uint8_t)(a & 0xFFu);
    e->v = (uint16_t)(val & 0xFFu);
    e->c = rt_vc ? rt_vcount() : 0xFFFFFFFFu;
}

void rt_dump(void)
{
    FILE *f;
    uint64_t hz, t0;

    if (!rt_on || !rt_buf)
        return;
    rt_on = 0;
    f = fopen(rt_path, "w");
    if (!f) {
        fprintf(stderr, "[RASTER] cannot write %s\n", rt_path);
        return;
    }
    hz = rt_hz();
    uint64_t total = rt_n, first = total > RT_MAX ? total - RT_MAX : 0;
    t0 = total ? rt_buf[first & (RT_MAX - 1u)].t : 0;
    fprintf(f, "# raster trace: t_us kind ...\n"
               "#   P idx value video_counter   (palette write, real counter after it)\n"
               "#   I level vector              (interrupt taken, vector from the IACK)\n"
               "#   I 4 screen_base TBCR TBDR   (VBL: base, Timer B control and count)\n"
               "#   M FAxx value video_counter  (guest write to an MFP register)\n"
            "#   R FA21 value                (guest read of Timer B's count)\n"
            "#   V raw given ns              (guest read of $FF8209: bus value, value given, bus read time)\n"
               "#   L level                     (ipl_task saw the IPL lines change)\n"
               "#   G wait|skip ns              (governor pause; skip = not taken,\n"
               "#                               an interrupt handler was running)\n"
               "#   W total_ns guest_clocks bus_n bus_ns bus_max_ns yield_ns gap_max_ns gov_ns\n"
               "#                               (Timer B taken -> this colour write, CPU thread:\n"
               "#                               bus accesses, waits for the bus, longest\n"
               "#                               stretch without one, governor pauses)\n");
    for (uint64_t i = first; i < total; i++) {
        const rt_ev_t *e = &rt_buf[i & (RT_MAX - 1u)];
        double us = (double)(e->t - t0) * 1e6 / (double)hz;

        if (e->kind == 'W' && i + 3 < total) {
            const rt_ev_t *b = &rt_buf[(i + 1) & (RT_MAX - 1u)];
            const rt_ev_t *m = &rt_buf[(i + 2) & (RT_MAX - 1u)];
            const rt_ev_t *g = &rt_buf[(i + 3) & (RT_MAX - 1u)];
            fprintf(f, "%.1f W %u %u %u %u %u %u %u %u\n", us, e->c, e->v,
                    b->v, b->c, m->v, m->c, g->v, g->c);
            i += 3;
            continue;
        }
        if (e->kind == 'w')
            continue;
        if (e->kind == 'P' && e->c == 0xFFFFFFFFu)
            fprintf(f, "%.1f P %u %03X -\n", us, e->a, e->v & 0xFFFu);
        else if (e->kind == 'P')
            fprintf(f, "%.1f P %u %03X %06X\n", us, e->a, e->v & 0xFFFu, e->c);
        else if (e->kind == 'M' && e->c == 0xFFFFFFFFu)
            fprintf(f, "%.1f M FA%02X %02X -\n", us, e->a, e->v & 0xFFu);
        else if (e->kind == 'V')
            fprintf(f, "%.1f V %02X %02X %u\n", us, e->a, e->v & 0xFFu, e->c);
        else if (e->kind == 'R')
            fprintf(f, "%.1f R FA%02X %02X\n", us, e->a, e->v & 0xFFu);
        else if (e->kind == 'L')
            fprintf(f, "%.1f L %u\n", us, e->a);
        else if (e->kind == 'G')
            fprintf(f, "%.1f G %s %u\n", us, e->a ? "skip" : "wait", e->c);
        else if (e->kind == 'M')
            fprintf(f, "%.1f M FA%02X %02X %06X\n", us, e->a, e->v & 0xFFu, e->c);
        else if (e->a == 4 && !rt_vc)
            fprintf(f, "%.1f I 4\n", us);
        else if (e->a == 4)
            fprintf(f, "%.1f I 4 %06X %02X %02X\n", us, e->c, e->v >> 8, e->v & 0xFFu);
        else
            fprintf(f, "%.1f I %u %02X\n", us, e->a, e->c & 0xFFu);
    }
    fclose(f);
    fprintf(stderr, "[RASTER] last %llu of %llu events written to %s\n",
            (unsigned long long)(total - first), (unsigned long long)total, rt_path);
}
