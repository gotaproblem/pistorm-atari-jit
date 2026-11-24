/*
 * shifter_blank.h - wait for the real beam to be in the vertical blank.
 *
 * Why: the Shifter fetches a line's words in groups (4 in low res, 2 in
 * medium) and keeps its place in the group across lines and frames.
 * A resolution or sync change from the Pi lands at a random beam
 * position; mid-display it can leave the Shifter one word out of step,
 * which shows as every bitplane drawn 16 pixels off its neighbour (text
 * doubled in two colours) until the next power cycle. In the vertical
 * blank the MMU sends no LOADs, so the change cannot slip the group.
 *
 * Test: the video address counter ($FF8205/07/09) moves only while a
 * line is displayed. Unchanged for longer than a whole line (64 us at
 * 50/60 Hz) means the beam is in the top or bottom border. Gives up
 * after `limit_ms` (no RAM, no video) and returns 0; 1 when in blank.
 */
#ifndef SHIFTER_BLANK_H
#define SHIFTER_BLANK_H
#include <stdint.h>
#include <time.h>
#include "gpio/ps_protocol.h"

static inline uint64_t sb_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline int shifter_wait_blank(unsigned limit_ms)
{
    const uint64_t t_end = sb_now_ns() + (uint64_t)limit_ms * 1000000ull;
    uint64_t still_since = sb_now_ns();
    uint32_t last = ps_read_8(0x00FF8209u) | (ps_read_8(0x00FF8207u) << 8);
    for (;;) {
        const uint64_t t = sb_now_ns();
        if (t > t_end) return 0;
        const uint32_t c = ps_read_8(0x00FF8209u) | (ps_read_8(0x00FF8207u) << 8);
        if (c != last) { last = c; still_since = t; continue; }
        if (t - still_since > 90000ull) return 1;   /* > 1 line unchanged */
    }
}
#endif
