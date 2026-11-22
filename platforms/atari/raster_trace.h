/* raster_trace.h - see raster_trace.c (PISTORM_RASTER_TRACE) */
#ifndef RASTER_TRACE_H
#define RASTER_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern volatile int rt_on;
void rt_start(void);   /* PISTORM_RASTER_FROM_MARK: begin recording (input_script.c) */
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
void rt_gap_slow(int kind, int64_t ns, uint32_t pc);
/* H records: the CPU thread away from guest code (kind 0, between two
 * governor checks - normally ~2-8 us) or in a beam-sync wait (kind 1) */
static inline void rt_gap(int kind, int64_t ns, uint32_t pc)
{
    if (__builtin_expect(rt_on, 0))
        rt_gap_slow(kind, ns, pc);
}
void rt_dump(void);

/* a guest write to the sync ($FF820A) or resolution ($FF8260) register,
 * after it reached the bus (pistorm_natmem.cpp): S reg value pc, then a K
 * record with the guest clock - border routines time these against the
 * beam, and the K clocks against the real times say how the governor
 * paced the code in between */
/* t_a: the write path entered (after the beam wait), t_b: just before the
 * bus write - both rt_tick(); the S record carries the two spans */
void rt_sync_slow(uint32_t reg, uint32_t v, uint64_t t_a, uint64_t t_b);
static inline uint64_t rt_tick(void)
{
#if defined(__aarch64__)
    uint64_t t;
    if (__builtin_expect(!rt_on, 1))
        return 0;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
    return t;
#else
    return 0;
#endif
}
static inline void rt_sync(uint32_t a, uint32_t v, int size, uint64_t t_a, uint64_t t_b)
{
    if (__builtin_expect(!rt_on, 1))
        return;
    a &= 0x00FFFFFFu;
    if (size == 1 && (a == 0x00FF820Au || a == 0x00FF8260u))
        rt_sync_slow(a & 0xFFu, v & 0xFFu, t_a, t_b);
    else if (size == 2 && (a == 0x00FF820Au || a == 0x00FF8260u))
        rt_sync_slow(a & 0xFFu, (v >> 8) & 0xFFu, t_a, t_b);
    else if (size == 4 && a == 0x00FF8208u)
        rt_sync_slow(0x0Au, (v >> 8) & 0xFFu, t_a, t_b);
    else if (size == 4 && a == 0x00FF8260u)
        rt_sync_slow(0x60u, (v >> 24) & 0xFFu, t_a, t_b);
}

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
