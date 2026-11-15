/* beam_clock.h - where the real ST beam is, for the CPU thread (see beam_clock.c)
 *
 *   ipl_task    bc_vbl_sighting()   the GLUE's level-4 line came on
 *   CPU thread  bc_poll()           every governor check outside a handler:
 *                                   takes in the sightings, and once a frame,
 *                                   mid-display, reads the real video counter
 *               bc_de_end()         when the display enable last ended
 *               bc_counter_advance()  bytes the counter fetched between two ticks
 *
 * Independent of the HDMI mirror: works with the native Shifter only.
 */
#ifndef BEAM_CLOCK_H
#define BEAM_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* arch-timer ticks (cntvct) and their rate; any host: CLOCK_MONOTONIC ns */
static inline uint64_t bc_ticks(void)
{
#if defined(__aarch64__) && !defined(BC_TEST)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    extern uint64_t bc_ticks_host(void);
    return bc_ticks_host();
#endif
}

static inline double bc_tick_hz(void)
{
#if defined(__aarch64__) && !defined(BC_TEST)
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? (double)f : 54000000.0;
#else
    return 1e9;
#endif
}

/* ---- ipl_task ---------------------------------------------------------- */
void bc_vbl_sighting(void);

/* ---- CPU thread -------------------------------------------------------- */
extern volatile uint32_t bc_sight_w;   /* sightings written (ipl_task)     */
extern uint32_t          bc_sight_r;   /* ... taken in                     */
extern uint64_t          bc_next;      /* next counter read due (ticks)    */
void bc_poll_slow(uint64_t now);

/* the governor's check, outside interrupt handlers: one clock read and two
 * compares unless there is something to do */
static inline void bc_poll(void)
{
    const uint64_t now = bc_ticks();
    if (now >= bc_next || bc_sight_w != bc_sight_r)
        bc_poll_slow(now);
}

/* the tick the display enable last ended at or before t (cycle 376 / 372
 * of this line or the one before) and ticks per ST cycle. 0 = no model:
 * no 50/60 Hz lock, or no counter read accepted in the last 0.5 s */
int bc_de_end(uint64_t t, double *de, double *tc);

/* bytes the video counter advanced from tick t0 to t1 by the nominal
 * geometry (display enable on lines 63..262 / 34..233, cycles 56..376 /
 * 52..372; 2 bytes per 4 cycles). -1 = unknown: no model, a counter
 * reload in between, or more than two lines apart */
int bc_counter_advance(uint64_t t0, uint64_t t1);

/* 50 / 60 when locked, else 0 */
int bc_locked_hz(void);

#ifdef __cplusplus
}
#endif

#endif
