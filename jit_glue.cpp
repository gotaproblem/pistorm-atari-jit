// SPDX-License-Identifier: MIT
//
// jit_glue.cpp — bridge between pistorm (emulator.c / ps_protocol.c) and the
// Amiberry ARM64 JIT CPU core. Implements the entry points emulator.c calls,
// plus the Atari interrupt-acknowledge and bus-error paths that replace
// Amiberry's Amiga (Paula) interrupt model.
//
// Companion: pistorm_natmem.cpp (the single-array guest map + natmem_offset).
//
// Verified against BlitterStudio/amiberry master:
//   init_m68k(), build_cpufunctbl() [build_comp() inside, when cachesize>0],
//   m68k_reset(), m68k_go(int), set_special(), SPCFLAG_BRK (16), SPCFLAG_INT (8),
//   do_interrupt()->intlev_ack(nr)->Exception(nr+24), Exception_normal()'s
//   `newpc = x_get_long(regs.vbr + 4*vector_nr)`, currprefs/changed_prefs fields,
//   JIT_HAS_BUS_ERROR_RECOVERY / jit_bus_error_jmpbuf / jit_in_compiled_code.
//
// CONFIRM against your live tree (/home/pistorm/pistorm-atari-jit/):
//   - exact translation-cache alloc call (check_prefs_changed_comp / alloc_cache)
//   - `fc` declaration, ps_read_ipl()/ps_read_16() prototypes
//   - the one-line Exception_normal patch (shown at the bottom of this file)
//   - that m68k_go(1) is acceptable as the run entry, or whether the bare-CPU
//     build needs a trimmed run loop (see jit_cpu_execute note).

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "uae.h" /* quit_program, UAE_RESET */
#include "jit/compemu.h"

#include <signal.h>

/* Max consecutive top-level run-loop relaunches before jit_cpu_execute gives
 * up (see the safety net at the bottom of jit_cpu_execute). */
#ifndef JITGLUE_MAX_RELAUNCH
#define JITGLUE_MAX_RELAUNCH 16
#endif

static void sigill_handler(int sig, siginfo_t *si, void *uctx)
{
    extern void *pushall_call_handler;
    uint32_t insn = 0;
    if (si->si_addr)
        insn = *(volatile uint32_t *)si->si_addr;
    fprintf(stderr, "[SIGILL] host=%p insn=0x%08x  pushall=%p delta=+0x%lx\n",
            si->si_addr, insn, pushall_call_handler,
            (unsigned long)((char *)si->si_addr - (char *)pushall_call_handler));
    _exit(42);
}

static void crash_handler(int sig, siginfo_t *si, void *uctx)
{
    extern void *pushall_call_handler;
    extern uae_u8 *natmem_offset;
    long guest = (char *)si->si_addr - (char *)natmem_offset;
    fprintf(stderr, "[%s] host=%p natmem=%p guest_addr=0x%lx\n",
            sig == SIGILL ? "SIGILL" : "SIGSEGV",
            si->si_addr, (void *)natmem_offset, guest);
    if (sig == SIGILL)
    {
        uint32_t insn = si->si_addr ? *(volatile uint32_t *)si->si_addr : 0;
        fprintf(stderr, "       insn=0x%08x delta_from_pushall=+0x%lx\n",
                insn, (unsigned long)((char *)si->si_addr - (char *)pushall_call_handler));
    }
    _exit(42);
}

// call once during init (top of main, before the CPU threads start):
static void install_crash_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);

    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
}

extern "C"
{
    /* ps_protocol.c */
    uint8_t ps_read_ipl(void);
    uint16_t ps_read_16(uint32_t);
    extern volatile uint8_t fc; // CONFIRM type/name

    /* pistorm_natmem.cpp */
    void jit_mem_init(void);
    void pistorm_seed_reset_vector(void);

    /* shared bus-error state defined in ps_protocol.c */
    extern volatile uint8_t g_buserr;
    extern volatile uint32_t g_buserr_addr;

    /* entry points emulator.c declares */
    void jit_cpu_init(int cpu_level);
    void jit_cpu_reset(void);
    void jit_cpu_execute(void);
    // void jit_set_irq(int level);
    void jit_signal_buserr(void);
}

extern void alloc_cache(void);              // compemu_arm.h
extern bool check_prefs_changed_comp(bool); // compemu_prefs.cpp

/* ============================================================= */
/* 1. CPU init — model select + JIT enable                       */
/* ============================================================= */
extern "C" void jit_cpu_init(int cpu_level)
{
    /* cpu_level 0..4 -> 68000/010/020/030/040. 020/030/040 are the solid JIT
     * models; clamp anything else to 030. 060 is selectable but experimental. */
    static const int model[] = {68000, 68010, 68020, 68030, 68040, 68060};
    int m = (cpu_level >= 2 && cpu_level < 6) ? model[cpu_level] : 68030;
    // int m = model[cpu_level];

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);

    // install_crash_handler();

    currprefs.cpu_model = changed_prefs.cpu_model = m;
    currprefs.fpu_model = changed_prefs.fpu_model = (m >= 68040) ? m : 68882;

    /* fpu_mode = 1 -> softfloat. REQUIRED on AArch64: the 68881/882/040 FPU is
     * 80-bit extended, which ARM has no native equivalent for. With fpu_mode==0
     * (the default), fpu_reset() takes the fp_init_native() x87 path, which on
     * ARM never assigns the fpp_* function pointers (fpp_get_support_flags etc.),
     * so get_features() then calls through a NULL pointer and SIGSEGVs. Softfloat
     * (WITH_SOFTFLOAT, fpp_softfloat.cpp) sets them all. compfpu=true below lets
     * the JIT compile FPU ops against this softfloat backend. */
#define HW_FPU_ON 1
#if HW_FPU_ON
    currprefs.fpu_mode = changed_prefs.fpu_mode = 0;  // deafult 1 softfloat, 0 hardware fpu for double the performance
    currprefs.compfpu = changed_prefs.compfpu = true; // false;   // default true Pi must emulate the FPU, false if using hw fpu
#else
    currprefs.fpu_mode = changed_prefs.fpu_mode = 1;  // deafult 1 softfloat, 0 hardware fpu for double the performance
    currprefs.compfpu = changed_prefs.compfpu = true; // default true Pi must emulate the FPU, false if using hw fpu
    // currprefs.compfpu   = changed_prefs.fpu_softfloat = false;   // does not exist???
#endif
    /* MMU off and cycle-exact off are REQUIRED for the JIT loop to be selected. */
    currprefs.mmu_model = changed_prefs.mmu_model = 0;
    currprefs.cpu_cycle_exact = changed_prefs.cpu_cycle_exact = false;
    currprefs.cpu_memory_cycle_exact = changed_prefs.cpu_memory_cycle_exact = false;

    /* cpu_compatible=false -> do_interrupt() takes one Exception and returns
     * (no get_ipl/intlev re-poll loop), which is what we want with real pins. */
    currprefs.cpu_compatible = changed_prefs.cpu_compatible = false; // default false, true slows things down a lot

    /* TT-RAM lives at 0x01000000, so we are 32-bit, not 24-bit. */
    currprefs.address_space_24 = changed_prefs.address_space_24 = false;

    currprefs.cachesize = changed_prefs.cachesize = 8192; // KB; >0 enables JIT default 8192KB

    /* DIRECT memory: bang natmem for RAM/ROM/TT. The per-bank jit_read_flag /
     * jit_write_flag set in pistorm_natmem.cpp force I/O + ST-RAM writes onto
     * the handler (bus) path regardless, so comptrust stays 0 for speed. */
    currprefs.comptrustbyte = currprefs.comptrustword =
        currprefs.comptrustlong = currprefs.comptrustnaddr = 0; // default 0
    changed_prefs.comptrustbyte = changed_prefs.comptrustword =
        changed_prefs.comptrustlong = changed_prefs.comptrustnaddr = 0; // default 0

    /* needed for Atari to obviscate Amiga code */
    currprefs.reset_delay = changed_prefs.reset_delay = 0;
    currprefs.cpu_clock_multiplier = changed_prefs.cpu_clock_multiplier = 0; // default 0, increases cpu cycleunit 2 is good
    currprefs.cpu_frequency = changed_prefs.cpu_frequency = 0;

    /* cryptodad optimisations */
    currprefs.turbo_emulation = changed_prefs.turbo_emulation = 0;
    currprefs.m68k_speed = changed_prefs.m68k_speed = -1;
    currprefs.compnf = changed_prefs.compnf = 0;                 // default 0, 1 is faster
    currprefs.comp_constjump = changed_prefs.comp_constjump = 0; // default 0, 1 is faster

    init_m68k(); // prefs_changed_cpu(), init_table68k()
    // NOTE: build_cpufunctbl() is static inside newcpu.cpp and is invoked by
    // m68k_go()'s entry block on the first jit_cpu_execute() call (it runs
    // prefs_changed_cpu(); build_cpufunctbl(); set_x_funcs(); ...). So we do
    // NOT (and cannot) call it here. We only sync the JIT comp prefs/cache.
    check_prefs_changed_comp(false); // alloc/init translation cache

    /* Seed the reset vector. jit_mem_init() (called before us in emulator.c)
     * has already copied the ROM image to ROM_BASE, but the 68k reset reads
     * SSP@0 and PC@4 from low memory. Normally pistorm_seed_reset_vector()
     * does this from jit_cpu_reset(), but that path is disabled in emulator.c,
     * so do it here so the vector is in place before m68k_go's reset reads it. */
    pistorm_seed_reset_vector();

    /* NOTE: the power-on reset (quit_program = UAE_RESET) that drives m68k_go's
     * build/reset block is armed in jit_cpu_execute(), NOT here — anything set
     * here is consumed (cleared to 0) by an init-time m68k_go pass before the
     * CPU thread runs. See jit_cpu_execute(). */
}

/* ============================================================= */
/* 2. Reset — seed reset vector into mirror, then reset core      */
/* ============================================================= */
extern "C" void jit_cpu_reset(void)
{
    pistorm_seed_reset_vector(); // ROM[0..7] -> mirror[0..7] (ROM overlay at 0)
    m68k_reset();                // reads SSP@0 / PC@4 from the mirror
}

/* ============================================================= */
/* 3. Execute — run the JIT loop                                 */
/* ============================================================= */
extern "C" void jit_cpu_execute(void)
{
    /* Arm the power-on reset HERE, immediately before m68k_go(1), not in
     * jit_cpu_init: quit_program is consumed (set to 0 at newcpu.cpp:6952) by
     * the init-time m68k_go pass, so by the time the CPU thread reaches this
     * point it is already 0 and m68k_go's `if (quit_program > 0)` reset+build
     * block (build_cpufunctbl -> build_comp -> create_popalls, which generates
     * pushall_call_handler) is skipped. Setting it here guarantees the build
     * runs on the pass that actually feeds m68k_run_jit. build_cpufunctbl has
     * no "already built" guard, so this rebuild always reaches build_comp.
     * Primed once: m68k_go(1) then loops until UAE_QUIT, so a single arming is
     * enough and we avoid re-resetting the CPU if m68k_go ever returns. */
    static bool primed = false;
    if (!primed)
    {
        quit_program = UAE_RESET;
        primed = true;
    }

    /* m68k_go(1) runs the selected (JIT) run loop until quit_program; its
     * do_specialties() services SPCFLAG_INT (raised by ipl_task) and SPCFLAG_BRK
     * (bus error). NOTE: m68k_go also pulls Amiga reset/device scaffolding — the
     * bare-CPU build must either stub those symbols (the "machine-layer" sweep)
     * or substitute a trimmed run loop that calls the JIT block executor. */
    /* Top-level safety net. m68k_run_jit now catches guest faults inside its own
     * dispatch loop (jit_recover_from_fault), so in normal operation nothing
     * reaches here. This only fires if an m68k_exception is thrown OUTSIDE that
     * loop — e.g. the reset/build path, or the pre-loop do_specialties() — where
     * an uncaught C++ throw would otherwise unwind into std::terminate and abort
     * the whole process. Bounded re-entry so a deterministic immediate re-throw
     * can't spin forever. m68k_exception is a bare struct (not std::exception),
     * so the catch-all (...) is what actually traps it here. */
    int relaunch = 0;
    for (;;)
    {
        try
        {
            m68k_go(1);
            return; /* normal exit: quit_program == UAE_QUIT */
        }
        catch (...)
        {
            fprintf(stderr, "[JITGLUE] escaped guest exception caught at top level "
                            "(relaunch %d/%d)\n",
                    relaunch + 1, JITGLUE_MAX_RELAUNCH);
        }
        if (++relaunch >= JITGLUE_MAX_RELAUNCH)
        {
            fprintf(stderr, "[JITGLUE] giving up after %d consecutive top-level faults\n",
                    relaunch);
            return;
        }
    }
}

/* ============================================================= */
/* 4. Interrupts — real pins, no injection                       */
/*    ipl_task() owns ps_read_ipl(); it raises SPCFLAG_INT to     */
/*    bail the JIT block. The core then samples + acknowledges.   */
/* ============================================================= */

/* Replaces Amiberry's Amiga intlev() (custom.cpp not compiled). Returns the
 * real level latched off the pins. Atari uses 2/4/6 only. */
/* C++ linkage (NOT extern "C"): newcpu.cpp references the mangled _Z6intlevv.
 * Wrapping it in extern "C" emits an unmangled `intlev`, leaving newcpu's call
 * unresolved at link. Same applies to intlev_ack below. */
extern volatile uint8_t g_irq;
int intlev(void)
{
    if (g_irq)
    {
        set_special (SPCFLAG_DOINT);
    }

    return 0;
}

/* Captured by intlev_ack(), consumed by the Exception_normal patch below. */
uae_u16 pistorm_iack_vector = 0;

/* Uniform 16-bit IACK read at FC=7. Hardware returns the vector number for all
 * three levels (no autovector branch). do_interrupt() calls this before
 * Exception(nr+24); the patch swaps in this vector. */
void intlev_ack(uint8_t nr)
{
    uint8_t old_fc = fc;    // save current fc bits
    fc = 7;                 // interrupt acknowledge
    pistorm_iack_vector = ps_read_16(0xFFFFFFF1u | (nr << 1));
    fc = old_fc;            // restore fc bits
}

/* jit_set_irq is now just a bail nudge: ipl_task observed a maskable pin change,
 * so force the JIT out of its block to go sample intlev(). NO level injection. */
// extern "C" void jit_set_irq(int level)
//{
//    (void)level;
//    set_special(SPCFLAG_INT);
//}

/* NOTE: doint() is NOT defined here. newcpu.cpp provides it; defining it here
 * too caused a "multiple definition of doint()" link error. IPL is sampled via
 * intlev(), so newcpu's own doint() (which sets the interrupt special-flag) is
 * exactly what we want. */

/* ============================================================= */
/* 5. Bus error — dedicated bail, NOT SPCFLAG_DOINT               */
/* ============================================================= */
extern "C" void jit_signal_buserr(void)
{
    /* Called from ps_protocol.c the instant the real BERR pin asserts.
     * SPCFLAG_BRK breaks out of the compiled block with no interrupt semantics
     * (the bug we hit last time was abusing SPCFLAG_DOINT for this). */
    set_special (SPCFLAG_BRK);

#if defined(JIT_HAS_BUS_ERROR_RECOVERY)
    /* If we faulted inside translated code, unwind to the dispatcher's setjmp
     * site (in compemu_support_arm.cpp). That site must, on return, raise
     * Exception(2) using g_buserr_addr. */
    if (jit_in_compiled_code)
        longjmp(jit_bus_error_jmpbuf, 1);
#endif
}

/* =============================================================
 * REQUIRED one-line core patch — in newcpu.cpp, Exception_normal():
 *
 *   Replace:
 *       newpc = x_get_long (regs.vbr + 4 * vector_nr);
 *   With:
 *       extern uae_u8 pistorm_iack_vector;
 *       int eff_vec = interrupt ? pistorm_iack_vector : vector_nr;
 *       newpc = x_get_long (regs.vbr + 4 * eff_vec);
 *
 * `interrupt` is already true on the do_interrupt() path; regs.intmask is still
 * set from (nr-24) so masking is unaffected. This is the only change that makes
 * the autovector path use the hardware-supplied vector instead.
 * ============================================================= */
