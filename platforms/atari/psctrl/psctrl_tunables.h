// SPDX-License-Identifier: MIT
//
// PSCTRL tunables — the knobs that used to be getenv("PISTORM_*") calls.
//
// Every one of these is a plain aligned int that the emulator re-reads
// where it uses it, so a single 32-bit store from the PSCTRL NatFeat
// handler (which runs on the CPU thread, cores 0-2) is all it takes to
// change the running machine. That store is atomic on AArch64, so the
// readers - including ipl_task on core 3 and the ET4000 render thread -
// need no lock and no syscall, and nothing is added to the admission
// budget: those loops already read globals.
//
// psctrl_tunables_init() is called once from main() BEFORE any consumer
// runs, and seeds every value from the environment so that an existing
// PISTORM_* in a launch script keeps working exactly as it did. After
// that the environment is never consulted again: the settings dialog and
// the .cfg are the only writers. This is the "retiring the env vars" step
// of psctrl-live-settings-design.md - the variable names below are the
// .cfg keys the writer emits.
//
// Two of the values here are not read directly by their consumer but are
// pre-converted for it. ipl_task spins on core 3 at SCHED_FIFO and must
// not divide: it reads *_ticks, and the setter (on the CPU thread) does
// the ns -> arch-timer-tick conversion using the frequency ipl_task
// published in psctrl_cntfrq. Writing the ns value alone would do
// nothing; psctrl_tunables_ipl_recalc() is what makes it take.

#ifndef PSCTRL_TUNABLES_H
#define PSCTRL_TUNABLES_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- JIT ---------------------------------------------------------- */
/* Compiled-chain budget as a multiple of CYCLE_UNIT. jit_glue.cpp turns
 * this into pissoff_value; the dialog's "jit_power" 1..6 is 256 << (n-1). */
extern volatile int pst_pissoff_mult;
/* The three JIT translation switches, so a .cfg can set them and PSCTRL
 * can save them. -1 = not set anywhere: jit_glue keeps its build default.
 * The dialog reads currprefs (the live truth) and applies through the
 * deferred queue; these hold what the .cfg said / what was last asked. */
extern volatile int pst_comp_constjump;
extern volatile int pst_compnf;
extern volatile int pst_compfpu;

/* --- video -------------------------------------------------------- */
extern volatile int pst_fps;              /* 10..60, host present pace   */
extern volatile int pst_drm_dirtyband;    /* 0/1                          */
extern volatile int pst_drm_async;        /* 0/1, boot only               */

/* --- blitter ------------------------------------------------------- */
/* ns per bus access; 0 = instant. There is no separate "instant" switch:
 * the design doc's PISTORM_BLIT_INSTANT never existed (a stale comment in
 * st_blitter.c promises it), and instant is simply 0 here. */
extern volatile int pst_blit_timed_ns;

/* --- audio --------------------------------------------------------- */
extern volatile int pst_ym_gain_x100;     /* 0..400, hundredths of unity  */
extern volatile int pst_ym_lag_ms;        /* 5..200                       */
extern volatile int pst_lmc;              /* 0/1 LMC1992 shadow           */
extern volatile int pst_audio_frames;     /* 512..8192, boot only         */

/* --- input --------------------------------------------------------- */
extern volatile int pst_mouse_thresh;     /* 0..15, 0 = off               */
extern volatile int pst_mouse_scale;      /* 1..16                        */

/* --- ST Box -------------------------------------------------------- */
/* Guest cycles per admission slice. Was the compile-time SLICE_CYC; the
 * design doc calls this a nanosecond cap, but the slice loop counts guest
 * cycles and converting per pass would cost a divide on core 3, so the
 * control is in cycles and the dialog prints the ~ns beside it. */
extern volatile int pst_stbox_slice_cyc;
extern volatile int pst_stbox_telemetry;  /* 0/1                          */

/* --- IPL / interrupt timing (core 3 reads the tick forms) ---------- */
extern volatile int pst_ipl_confirm_ns;
extern volatile int pst_vbl_refract_ns;
extern volatile uint64_t pst_ipl_confirm_ticks;
extern volatile uint64_t pst_vbl_refract_ticks;
extern volatile uint64_t psctrl_cntfrq;   /* published by ipl_task        */

/* Recompute the tick forms from the ns forms. Called by the settings
 * handler after either ns value changes, and by ipl_task once it knows
 * cntfrq. Safe to call from any thread: it only stores. */
void psctrl_tunables_ipl_recalc(void);

/* --- console ------------------------------------------------------- */
/* 0: the console carries what a user needs - the config as loaded, the
 * devices found, warnings and errors. 1: every informational line the
 * emulator has always printed as well (thread start-up, JIT internals,
 * hugepage accounting, per-subsystem "ready" lines). cfg: `debug verbose`,
 * env PISTORM_VERBOSE, PSCTRL Debug tab "Verbose console" - live. */
extern volatile int pst_verbose;

/* An informational line: printed only when pst_verbose is set. Errors and
 * warnings do not go through this - they are printed unconditionally. */
#define PS_INFO(...) \
  do { if (pst_verbose) fprintf(stderr, __VA_ARGS__); } while (0)

/* --- behaviour switches -------------------------------------------- */
/* Deliver the emulated FDC/ACSI completion as a REAL MFP interrupt
 * (channel 7 = GPIP5) as well as a level in the GPIP byte. Default off:
 * TOS and EmuTOS POLL the GPIP bit, and everything that works today
 * works by polling. Turn it on for software that waits on the FDC
 * interrupt instead - an interrupt-driven trackloader, a floppy driver
 * that enables IERB bit 7. Nothing is delivered unless the guest has
 * itself enabled and unmasked that channel, so this cannot invent an
 * interrupt the guest did not ask for. */
extern volatile int pst_fdd_mfp_irq;

/* --- debug / trace flags ------------------------------------------- */
extern volatile int pst_dbg_ipl_stats;
extern volatile int pst_dbg_irq_stats;
extern volatile int pst_dbg_mfp;
extern volatile int pst_dbg_mfp_hub;
extern volatile int pst_dbg_blit_trace;   /* a budget of blits, not a flag */
extern volatile int pst_dbg_dmasnd;       /* 0 off, 1 verbose, 2 summary   */
extern volatile int pst_dbg_acsi;
extern volatile int pst_dbg_ide;
extern volatile int pst_dbg_hostfs;       /* -1 = follow the cfg           */
extern volatile int pst_dbg_gemdos;
extern volatile int pst_dbg_stram;
extern volatile int pst_dbg_net;
extern volatile int pst_dbg_ikbd;         /* every IKBD command the guest sends */

void psctrl_tunables_init(void);

#ifdef __cplusplus
}
#endif

#endif /* PSCTRL_TUNABLES_H */
