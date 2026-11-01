/* raster_trace.h - see raster_trace.c (PISTORM_RASTER_TRACE) */
#ifndef RASTER_TRACE_H
#define RASTER_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern volatile int rt_on;
void rt_palette_slow(unsigned idx, uint16_t val);
void rt_irq_slow(int level);
void rt_dump(void);

static inline void rt_palette(unsigned idx, uint16_t val)
{
    if (__builtin_expect(rt_on, 0))
        rt_palette_slow(idx, val);
}

static inline void rt_irq(int level)
{
    if (__builtin_expect(rt_on, 0))
        rt_irq_slow(level);
}

#ifdef __cplusplus
}
#endif

#endif
