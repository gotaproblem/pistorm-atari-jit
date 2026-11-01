/*
 * raster_trace.c - where on the REAL screen a palette write lands
 *
 * For raster effects on the Shifter output (sky bands, Timer B colour
 * splits): every write to $FF8240-$FF825E is logged with the time and the
 * real Shifter's video address counter ($FF8205/07/09) read straight after
 * it - so the log says which screen line the colour actually changed on.
 * Every interrupt the CPU takes is logged too (level 4 = VBL, 6 = MFP:
 * Timer B lives there), and at each VBL the screen base ($FF8201/03).
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
    uint8_t  kind;                            /* 'P' palette, 'I' irq  */
    uint8_t  a;                               /* P: index  I: level    */
    uint16_t v;                               /* P: value              */
    uint32_t c;                               /* P: video counter, I(4): screen base */
} rt_ev_t;

volatile int rt_on;
static rt_ev_t *rt_buf;
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
    e->c = level == 4
         ? (((uint32_t)ps_read_8(0x00FF8201u) << 16) |
            ((uint32_t)ps_read_8(0x00FF8203u) << 8))
         : 0;
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
               "#   I level [screen_base]       (interrupt taken; base at level 4)\n");
    for (uint32_t i = 0; i < rt_n; i++) {
        const rt_ev_t *e = &rt_buf[i];
        double us = (double)(e->t - t0) * 1e6 / (double)hz;

        if (e->kind == 'P')
            fprintf(f, "%.1f P %u %03X %06X\n", us, e->a, e->v & 0xFFFu, e->c);
        else if (e->a == 4)
            fprintf(f, "%.1f I 4 %06X\n", us, e->c);
        else
            fprintf(f, "%.1f I %u\n", us, e->a);
    }
    fclose(f);
    fprintf(stderr, "[RASTER] %u events written to %s\n", rt_n, rt_path);
}
