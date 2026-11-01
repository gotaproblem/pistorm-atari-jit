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
void rt_mfp_slow(uint32_t addr, uint32_t val, int word);
void rt_irq_vector_slow(unsigned vec);
void rt_ipl_slow(int level);
void rt_gov_slow(int64_t ns, int skipped);
void rt_dump(void);

static inline void rt_palette(unsigned idx, uint16_t val)
{
    if (__builtin_expect(rt_on, 0))
        rt_palette_slow(idx, val);
}

static inline void rt_mfp(uint32_t addr, uint32_t val, int word)
{
    if (__builtin_expect(rt_on, 0))
        rt_mfp_slow(addr, val, word);
}

static inline void rt_irq_vector(unsigned vec)
{
    if (__builtin_expect(rt_on, 0))
        rt_irq_vector_slow(vec);
}

static inline void rt_ipl(int level)
{
    if (__builtin_expect(rt_on, 0))
        rt_ipl_slow(level);
}

static inline void rt_gov(int64_t ns, int skipped)
{
    if (__builtin_expect(rt_on, 0))
        rt_gov_slow(ns, skipped);
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
