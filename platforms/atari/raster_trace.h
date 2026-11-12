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
void rt_mfp_read_slow(uint32_t addr, uint32_t val);
void rt_vcount_slow(uint32_t raw, uint32_t given, uint32_t dur_ns);
/* a guest read of the video counter's low byte ($FF8209): what the bus
 * returned, what the guest was given (pistorm_natmem.cpp, BACK-DATED
 * READS) and how long the bus read took */
static inline void rt_beam_read(uint32_t raw, uint32_t given, uint32_t dur_ns)
{
    if (__builtin_expect(rt_on, 0))
        rt_vcount_slow(raw, given, dur_ns);
}
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

/* a guest read of Timer B's data register ($FFFA21): what a raster
 * handler's "wait for the next line" poll saw, and when */
static inline void rt_mfp_read(uint32_t addr, uint32_t val)
{
    if (__builtin_expect(rt_on, 0) && (addr & 0x00FFFFFFu) == 0x00FFFA21u)
        rt_mfp_read_slow(addr, val);
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
