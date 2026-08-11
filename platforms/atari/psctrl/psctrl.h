// SPDX-License-Identifier: MIT
//
// PSCTRL — read-only PiStorm status NatFeat (phase 1 of PSCTRL-DESIGN.md /
// PSMON-DESIGN.md). Host-side sampler + indexed PS_GETINT value namespace.
//
// The sampler runs on its own thread at a fixed 500 ms wall-clock tick and
// publishes finished, pre-divided values; the NatFeat read is O(1) and free
// of side effects, so guest poll rate cannot distort the numbers (see
// PSMON-DESIGN.md section 1). Everything here is read-only: no JIT state is
// ever mutated from a NatFeat handler (see the JIT invariant comment in
// atari_natfeat.cpp).

#ifndef PSCTRL_H
#define PSCTRL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSCTRL_API_VERSION 1

/* PSCTRL sub-operations (low 20 bits of the NatFeat id) */
enum psctrl_subop {
  PSCTRL_VERSION = 0,           /* -> API version                     */
  PSCTRL_GETINT  = 1,           /* p0 = index -> value, see below     */
  PSCTRL_FX      = 2            /* p0 = 1|2: desk-slide transition
                                 * (old frame exits left|right); a
                                 * presentation-only effect, snapshot
                                 * taken synchronously in the handler */
};

/* PS_GETINT index namespace.
 * 0..31   configuration (read once, static per run)
 * 32..63  guest/JIT statistics sampled every 500 ms
 * 64..95  host (Raspberry Pi) statistics sampled every 500 ms
 * Unknown indices return 0xFFFFFFFF. */
enum psctrl_stat_index {
  PS_CFG_JIT_ENABLED    = 0,    /* 1 = JIT translation active          */
  PS_CFG_CACHE_SIZE_KB  = 1,    /* configured translation cache, KB    */
  PS_CFG_CPU_MODEL      = 2,    /* e.g. 68030                          */
  PS_CFG_FPU_MODEL      = 3,    /* e.g. 68882, 0 = none                */
  PS_CFG_TTRAM_SIZE     = 4,    /* configured TT-RAM bytes, 0 = off    */

  PS_STAT_EPOCH         = 32,   /* snapshot serial, bumps every 500 ms */
  PS_STAT_CACHE_USED    = 39,   /* translation cache bytes in use      */
  PS_STAT_CACHE_TOTAL   = 40,   /* translation cache bytes total       */
  PS_STAT_COMPILES      = 41,   /* blocks compiled in last window      */
  PS_STAT_FLUSHES       = 42,   /* hard cache flushes in last window   */
  PS_STAT_INTERP_CALLS  = 44,   /* execute_normal() calls, last window */
  PS_STAT_STOP_ITERS    = 46,   /* STOP-state iterations, last window
                                 * (nonzero = guest idling under MiNT) */

  PS_HOST_SOC_TEMP_MC   = 64,   /* SoC temperature, millidegrees C     */
  PS_HOST_ARM_FREQ_KHZ  = 65,   /* current ARM core clock, kHz         */
  PS_HOST_LOADAVG_X100  = 66,   /* 1-minute load average x 100         */
  PS_HOST_UPTIME_S      = 67,   /* host uptime, seconds                */
  PS_HOST_TIME_DOS      = 68,   /* Pi local time, GEMDOS packed format
                                 * (hhhhhmmm mmmsssss, seconds / 2)    */
  PS_HOST_DATE_DOS      = 69,   /* Pi local date, GEMDOS packed format
                                 * (yyyyyyym mmmddddd, year - 1980)    */
  PS_HOST_THROTTLED     = 70,   /* firmware get_throttled register:
                                 * bit0 undervoltage now, bit1 freq
                                 * capped now, bit2 throttled now,
                                 * bit3 soft temp limit now; bits
                                 * 16-19 = same, has occurred         */

  /* Phase 4: JIT engine figures, computed by the sampler from state the
   * emulator already maintains (zero new work on the hot path - see the
   * project's phase4-impact-report). Speed is cycle-accounted against
   * the 8 MHz ST reference (one 68k cycle = CYCLE_UNIT/2 clock units),
   * so in governed m68k_speed=0 mode it honestly reads ~8000. */
  PS_JIT_EFF_KHZ        = 71,   /* effective 68k speed, kHz            */
  PS_JIT_HITRATE_X10    = 72,   /* cycle-weighted JIT hit rate,
                                 * tenths of a percent (0-1000)        */
  PS_JIT_IDLE_X10       = 73,   /* true idle (STOP) share of the
                                 * clock, tenths of a percent          */

  /* Taskbar v2: board identity (static, from the revision word in
   * /proc/cpuinfo - new-style codes only, 0 when undecodable) and two
   * more JIT health figures for the click-open JIT panel. */
  PS_PI_MODEL           = 74,   /* board type code, revision bits 4-11:
                                 * 0x08=3B 0x0d=3B+ 0x0e=3A+ 0x11=4B
                                 * 0x13=400 0x14=CM4 0x17=5            */
  PS_PI_RAM_MB          = 75,   /* board RAM, MB (256 << rev bits
                                 * 20-22)                              */
  PS_STAT_FLUSHES_TOTAL = 76,   /* hard cache flushes since boot       */
  PS_STAT_SMC_INV       = 77    /* blocks invalidated (self-modifying
                                 * code / checksum fail), last window  */
};

/* Idempotent; spawns the sampler thread on first use (called lazily from
 * the NatFeat handler, so a system that never probes PSCTRL pays nothing). */
void psctrl_sampler_start(void);

/* O(1) snapshot read; safe to call from the CPU thread inside a translated
 * block. */
uint32_t psctrl_getint(uint32_t index);

/* Free-running event counters, bumped from the CPU/JIT core.
 * Single writer (the CPU thread); the sampler only reads and differences
 * them, so plain volatile 32-bit stores are sufficient on AArch64. */
extern volatile uint32_t psctrl_ctr_compiles;
extern volatile uint32_t psctrl_ctr_flushes;
extern volatile uint32_t psctrl_ctr_interp_calls;
extern volatile uint32_t psctrl_ctr_stop_iters;
extern volatile uint32_t psctrl_ctr_interp_cycles;	/* CYCLE_UNIT-weighted */
extern volatile uint32_t psctrl_ctr_smc_inv;	/* invalidate_block() calls */

/* Implemented in jit/arm/compemu_support_arm.cpp */
uint32_t psctrl_jit_cache_used(void);
uint32_t psctrl_jit_cache_total(void);
int psctrl_jit_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* PSCTRL_H */
