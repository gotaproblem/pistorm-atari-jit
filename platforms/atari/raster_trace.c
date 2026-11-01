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
 *
 * Off costs one load of a zero flag at each palette write and interrupt.
 * On, each palette write costs three bus reads (~2-3 us) - small against a
 * 64 us line, and the same for every write, so the spread it measures is
 * the emulator's, not the trace's. Recording stops when the buffer is full
 * (about 60 s of a busy raster screen) and says so.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platforms/atari/raster_trace.h"

uint8_t ps_read_8(uint32_t address);          /* gpio/ps_protocol.c */

#define RT_MAX (1u << 20)                     /* events, 16 bytes each */

typedef struct {
    uint64_t t;                               /* arch timer ticks      */
    uint8_t  kind;                            /* 'P' palette, 'I' irq, 'M' MFP write */
    uint8_t  a;                               /* P: index  I: level  M: reg ($FFFAxx) */
    uint16_t v;                               /* P/M: value  I(4): TBCR<<8 | TBDR count */
    uint32_t c;                               /* P/M: video counter, I(4): screen base */
} rt_ev_t;

volatile int rt_on;
static rt_ev_t *rt_buf;
static rt_ev_t *rt_last_irq;               /* gets its vector after the IACK */
static uint32_t rt_n;
static int rt_full_said;
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
    rt_on = 1;
    fprintf(stderr, "[RASTER] tracing palette writes and interrupts -> %s "
                    "(written when the emulator stops)\n", rt_path);
}

static inline rt_ev_t *rt_slot(void)
{
    if (rt_n >= RT_MAX) {
        if (!rt_full_said) {
            rt_full_said = 1;
            fprintf(stderr, "[RASTER] buffer full - recording stopped; stop the "
                            "emulator to write %s\n", rt_path);
        }
        return NULL;
    }
    return &rt_buf[rt_n++];
}

static uint32_t rt_vcount(void)
{
    return ((uint32_t)ps_read_8(0x00FF8205u) << 16) |
           ((uint32_t)ps_read_8(0x00FF8207u) << 8) |
            (uint32_t)ps_read_8(0x00FF8209u);
}

void rt_palette_slow(unsigned idx, uint16_t val)
{
    rt_ev_t *e = rt_slot();

    if (!e)
        return;
    e->t = rt_ticks();
    e->kind = 'P';
    e->a = (uint8_t)idx;
    e->v = val;
    e->c = rt_vcount();
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
    if (level == 4) {
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
        rt_last_irq = NULL;
    }
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
    e->c = rt_vcount();
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
    t0 = rt_n ? rt_buf[0].t : 0;
    fprintf(f, "# raster trace: t_us kind ...\n"
               "#   P idx value video_counter   (palette write, real counter after it)\n"
               "#   I level vector              (interrupt taken, vector from the IACK)\n"
               "#   I 4 screen_base TBCR TBDR   (VBL: base, Timer B control and count)\n"
               "#   M FAxx value video_counter  (guest write to an MFP register)\n");
    for (uint32_t i = 0; i < rt_n; i++) {
        const rt_ev_t *e = &rt_buf[i];
        double us = (double)(e->t - t0) * 1e6 / (double)hz;

        if (e->kind == 'P')
            fprintf(f, "%.1f P %u %03X %06X\n", us, e->a, e->v & 0xFFFu, e->c);
        else if (e->kind == 'M')
            fprintf(f, "%.1f M FA%02X %02X %06X\n", us, e->a, e->v & 0xFFu, e->c);
        else if (e->a == 4)
            fprintf(f, "%.1f I 4 %06X %02X %02X\n", us, e->c, e->v >> 8, e->v & 0xFFu);
        else
            fprintf(f, "%.1f I %u %02X\n", us, e->a, e->c & 0xFFu);
    }
    fclose(f);
    fprintf(stderr, "[RASTER] %u events written to %s\n", rt_n, rt_path);
}
