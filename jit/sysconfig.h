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
#define USE_JIT         1
#define AMIBERRY        1   /* empty REGPARAM*, soft uae_p32 warning, inline atomics */
#define CPU_AARCH64     1   /* 64-bit ARM backend: R_MEMSTART = x27, N_REGS = 18  */
#define CPU_64_BIT      1   /* host pointers are 64-bit (uintptr = uae_u64)        */

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

#include "sysdeps.h"

#endif /* PISTORM_SYSCONFIG_H */
