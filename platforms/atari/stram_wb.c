/* ST-RAM WRITE-BEHIND FOR INTERRUPT HANDLERS (PISTORM_WRBEHIND=1; off by
 * default while it is being proven).
 *
 * THE PROBLEM. Every access to the real ST bus costs ~0.95 us on firmware
 * 1.1b (a real 68000: 0.5 us), and with stram_cache off - how games run -
 * every RAM write goes there. Instruction fetches cost us nothing, so
 * ordinary code keeps 8 MHz; but the first instructions of an interrupt
 * handler are almost all bus: the exception frame (3 words), a movem of
 * saved registers (12 words for d0-d5), vectors, counters. That stretch
 * runs at about half an ST's speed.
 *
 * Turrican II's title, 12 Nov 2026 ([games], 1.1b, measured with
 * PISTORM_RASTER_TRACE against Hatari 2.4 on the same disk): the band's
 * Timer B handler has to begin polling Timer B's count ($FFFA21) before
 * the next line ends, 444 cycles (55.5 us) after its IACK on an ST. An ST
 * gets there in 272 cycles (34 us). We took 60 us (median; deadline 61):
 *     IACK + frame + movem.l d0-d5,-(sp) + sf $fa1b   ST 11 us   us 25.5
 *     two MFP writes                                     5.5        3.8
 *     move.l #,$120 + four clr.l to the palette + poll  17.5       30.7
 * so 738 of 1685 frames started polling after the count had moved on and
 * drew the band one line late - the jitter, on the Shifter itself. The
 * IPL sampling period (2 us instead of 15) and posted writes (the CPU not
 * waiting for a write to finish) were both tried and measured: neither is
 * the bulk of it. The bulk is the number of bus cycles.
 *
 * THE CHANGE. While an interrupt handler is running (newcpu.cpp's
 * pistorm_handler_running(): from the interrupt being taken until its RTE
 * or 2000 cycles, the window in which the governor already does not
 * pause), a RAM write-through is put in a FIFO instead of on the bus. The
 * CPU's own copy (natmem) is written at once as always, so the guest sees
 * no difference. The FIFO is emptied onto the bus, oldest first:
 *   - at the governor's checks outside a handler (every 32 cycles),
 *   - at the first RAM write outside a handler,
 * one entry at a time, stopping whenever an interrupt is waiting to be
 * taken (the next handler must not pay for the last one's stack), and
 *   - all of it, before anything else reads or writes real ST-RAM or can
 *     start real DMA: blitter and DMA stores, the screen shadow, accesses
 *     to the DMA/FDC registers $FF8600-$FF860F and to the video base
 *     registers, reset.
 * While entries wait, EVERY RAM write-through joins the FIFO (in or out
 * of a handler), so two writes to one address can never swap. When the
 * FIFO is full a new entry pushes the oldest out to the bus: the cost of
 * a plain write, never a burst.
 *
 * WHAT IS LATE. Only the real RAM, only for writes made in a handler's
 * first 250 us, and only until the handler ends. What reads real RAM is
 * the Shifter (the picture) and real DMA. I/O is not touched: MFP, YM,
 * palette, sync, the video counter all go to the bus at once, in order.
 * A handler that draws into the visible screen at a chosen raster line
 * would see those pixels appear at its RTE instead - usually microseconds,
 * and such drawing is by its nature not beam-exact to the pixel.
 *
 * CPU thread only. */
#include <stdio.h>
#include <stdlib.h>
#include "stram_wb.h"

extern void ps_write_8(uint32_t address, uint16_t data);
extern void ps_write_16(uint32_t address, uint16_t data);
extern uint8_t fc;
#ifdef STRAM_WB_TEST
extern uint8_t g_buserr;
#else
extern volatile uint8_t g_buserr;
#endif
extern int stram_wb_irq_due(void);      /* an interrupt the CPU would take now */

#define WB_CAP 256u                 /* > the words a handler can write in
                                     * its 2000-cycle window (8 cycles each) */
struct wb_e { uint32_t a; uint16_t v; uint8_t size, fc; };
static struct wb_e wb_q[WB_CAP];
static unsigned wb_head;            /* oldest entry */

int      stram_wb_on;
unsigned stram_wb_n;
unsigned long stram_wb_deferred, stram_wb_direct_after, stram_wb_pushed,
              stram_wb_flushes, stram_wb_max;

void stram_wb_init(void)
{
    const char *e = getenv("PISTORM_WRBEHIND");
    stram_wb_on = e && *e == '1';
    if (stram_wb_on)
        fprintf(stderr, "[STRAM] write-behind on (PISTORM_WRBEHIND=1): RAM writes made "
                "in an interrupt handler reach the real bus when it ends\n");
}

static inline void wb_bus(uint32_t a, uint32_t v, int size, uint8_t wfc)
{
    const uint8_t old_fc = fc;
    const uint8_t old_be = g_buserr;
    fc = wfc;
    if (size == 1)
        ps_write_8(a, (uint16_t)(v & 0xFFu));
    else
        ps_write_16(a, (uint16_t)v);
    fc = old_fc;
    g_buserr = old_be;              /* a RAM write-through reports nothing */
}

static inline void wb_pop(void)
{
    const struct wb_e *e = &wb_q[wb_head];
    wb_head = (wb_head + 1u) % WB_CAP;
    stram_wb_n--;
    wb_bus(e->a, e->v, e->size, e->fc);
}

static inline void wb_push(uint32_t a, uint32_t v, int size, uint8_t wfc)
{
    if (stram_wb_n == WB_CAP) {     /* full: the oldest goes out now */
        stram_wb_pushed++;
        wb_pop();
    }
    struct wb_e *e = &wb_q[(wb_head + stram_wb_n) % WB_CAP];
    e->a = a; e->v = (uint16_t)v; e->size = (uint8_t)size; e->fc = wfc;
    stram_wb_n++;
    if (stram_wb_n > stram_wb_max)
        stram_wb_max = stram_wb_n;
}

void stram_wb_drain(void)
{
    while (stram_wb_n && !stram_wb_irq_due())
        wb_pop();
}

void stram_wb_flush_slow(void)
{
    stram_wb_flushes++;
    while (stram_wb_n)
        wb_pop();
}

void stram_wb_slow(uint32_t bus_a, uint32_t v, int size, uint8_t wfc, int in_handler)
{
    if (in_handler) {
        stram_wb_deferred++;
        wb_push(bus_a, v, size, wfc);
        return;
    }
    if (stram_wb_n == 0) {          /* the normal case: straight to the bus */
        wb_bus(bus_a, v, size, wfc);
        return;
    }
    /* older writes are still waiting: this one goes behind them, then as
     * much as an idle interrupt line allows goes out (normally all) */
    stram_wb_direct_after++;
    wb_push(bus_a, v, size, wfc);
    stram_wb_drain();
}
