/* SPDX-License-Identifier: MIT
 *
 * sysconfig.h — pistorm bare-CPU build configuration for the Amiberry AArch64
 * JIT CPU core. Replaces amiberry/src/osdep/sysconfig.h, which is welded to the
 * full app (it pulls uae/string.h -> SDL3). This selects ONLY the 68k CPU + JIT
 * engine: no Amiga chipset, no SDL, no GUI.
 *
 * Include order in the core is always: #include "sysconfig.h" then "sysdeps.h".
 * This file defines the SIZEOF_* macros that uae/types.h needs (normally from
 * the generated config.h) and the target/feature switches, then pulls sysdeps.h.
 *
 * Verified against amiberry/src/include/uae/types.h (width selection),
 * uae/byteswap.h (config.h only under HAVE_CONFIG_H), and src/include/sysdeps.h.
 */

#ifndef PISTORM_SYSCONFIG_H
#define PISTORM_SYSCONFIG_H

/* ------------------------------------------------------------------ *
 * Host type sizes — Raspberry Pi 4, AArch64 Linux (LP64).
 * uae/types.h #errors out if these are wrong/missing.
 * ------------------------------------------------------------------ */
#ifndef SIZEOF_SHORT
#define SIZEOF_SHORT      2
#endif
#ifndef SIZEOF_INT
#define SIZEOF_INT        4
#endif
#ifndef SIZEOF_LONG
#define SIZEOF_LONG       8
#endif
#ifndef SIZEOF_LONG_LONG
#define SIZEOF_LONG_LONG  8
#endif
#ifndef SIZEOF_VOID_P
#define SIZEOF_VOID_P     8
#endif

/* ------------------------------------------------------------------ *
 * Target / feature selection
 * ------------------------------------------------------------------ */
#define UAE             1   /* enables UAE paths; compemu defines USE_JIT under UAE */
#define JIT             1   /* compile the recompiler */
#define USE_JIT             /* empty, matching jit/arm/compemu_arm.h (all uses are #ifdef) */
#define PISTORM_ATARI   1   /* Atari PiStorm build: real bus IACK, no Amiga custom IRQs */
#define AMIBERRY        1   /* empty REGPARAM*, soft uae_p32 warning, inline atomics */
/* Amiberry's sysdeps.h defines BOTH of these on aarch64 (its aarch64 branch
 * does "define CPU_arm 1" then CPU_AARCH64). CPU_arm gates the midfunc
 * declaration includes in jit/arm/compemu_arm.h; without it the opcode handlers
 * in compemu.cpp see no declarations for the jff_, mov_l_ri and dont_care_flags
 * family. arm32-only blocks are separately guarded with !CPU_AARCH64. */
#define CPU_arm         1   /* generic "ARM" flag; required even on AArch64      */
#define CPU_AARCH64     1   /* 64-bit ARM backend: R_MEMSTART = x27, N_REGS = 18  */
#define CPU_64_BIT      1   /* host pointers are 64-bit (uintptr = uae_u64)        */

/* Path limits + target name (were in the replaced osdep/sysconfig.h). */
#include <limits.h>
#ifndef MAX_DPATH
#define MAX_DPATH 4096
#endif
#ifndef TARGET_NAME
#define TARGET_NAME _T("pistorm")
#endif

/* Do NOT define CPU_arm here — that selects the 32-bit ARM midfunc path.        */
/* Do NOT define HAVE_CONFIG_H — uae/byteswap.h then skips config.h and uses     */
/*   __builtin_bswap* directly under __GNUC__.                                   */
/* Do NOT define WORDS_BIGENDIAN — the Pi is little-endian, so the               */
/*   machdep/maccess.h do_get/put_mem_* accessors byteswap, keeping natmem in    */
/*   m68k big-endian order to match what the JIT codegen expects.                */

/* GCC/Clang on the Pi have the byteswap builtins. */
#ifndef HAVE___BUILTIN_BSWAP16
#define HAVE___BUILTIN_BSWAP16 1
#endif
#ifndef HAVE___BUILTIN_BSWAP32
#define HAVE___BUILTIN_BSWAP32 1
#endif
#ifndef HAVE___BUILTIN_BSWAP64
#define HAVE___BUILTIN_BSWAP64 1
#endif

/* ------------------------------------------------------------------ *
 * CPU/FPU/MMU emulation feature set (were in the replaced
 * osdep/sysconfig.h). These gate opcode-table symbols referenced by the
 * cputbls[][] array in newcpu.cpp and the dc030 accessors in
 * cpu_prefetch.h, so the set must match the cpuemu_*.cpp you compile.
 * MMU is compiled (CPUEMU_31..35) but stays inert at runtime (mmu_model=0).
 * Deliberately NOT defining AGA / AUTOCONFIG (Amiga chipset/expansion).
 * ------------------------------------------------------------------ */
#define FPUEMU
#define WITH_SOFTFLOAT  /* fpdata.fpx (floatx80); newcpu.h then pulls softfloat.h */
#define MMUEMU
/* JIT-compiled FPU (comp_fpp_opp in jit/arm/compemu_fpp_arm.cpp): FMOVE/FADD/
 * FMUL/FDIV/FSQRT/FCMP etc. compile to native AArch64 FP instructions instead
 * of interpretive fpuop_arithmetic() calls (transcendentals still fall back
 * per-op). Without this define avoid_fpu is hard-true in
 * compemu_support_arm.cpp and EVERY F-line op is interpreted regardless of
 * compfpu - both "hardware" and "software" FPU modes were interpretive, only
 * the arithmetic backend differed. Requires currprefs.compfpu=1 (jit_glue.cpp)
 * and fpu_mode=0 (fptype=double matches the AArch64 FP registers). Changes no
 * struct layouts, so mixed old/new objects still link. */
#define USE_JIT_FPU

/* JIT-internal config from the replaced osdep/sysconfig.h:
 *  - NOFLAGS_SUPPORT_GENCOMP gates nfctbl/nfcpufunctbl in compemu_support_arm.cpp
 *    (they're used unconditionally there, so the define must be on).
 *  - NATMEM_OFFSET is the symbol codegen_arm64.cpp loads into R_MEMSTART (x27);
 *    it aliases the natmem_offset pointer (defined in pistorm_natmem.cpp) and
 *    enables the direct-addressing block + its extern decl in memory.h.
 * We do NOT enable NOFLAGS_SUPPORT_GENCPU (keeps the handler_ff path). */
#define NOFLAGS_SUPPORT_GENCOMP
#define NATMEM_OFFSET   natmem_offset
#define FULLMMU
#define CPUEMU_0     /* generic 680x0 */
#define CPUEMU_11    /* 68000/010 prefetch */
#define CPUEMU_13    /* 68000/010 cycle-exact */
#define CPUEMU_20    /* 68020 prefetch */
#define CPUEMU_21    /* 68020 cycle-exact */
#define CPUEMU_22    /* 68030 prefetch */
#define CPUEMU_23    /* 68030 cycle-exact */
#define CPUEMU_24    /* 68060 cycle-exact */
#define CPUEMU_25    /* 68040 cycle-exact */
#define CPUEMU_31    /* 68040 MMU */
#define CPUEMU_32    /* 68030 MMU */
#define CPUEMU_33    /* 68060 MMU */
#define CPUEMU_34    /* 68030 MMU + cache */
#define CPUEMU_35    /* 68030 MMU + cache + CE */
#define CPUEMU_40    /* generic + JIT direct */
#define CPUEMU_50    /* generic + JIT indirect */

#include "sysdeps.h"

#endif /* PISTORM_SYSCONFIG_H */
