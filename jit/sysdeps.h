/* SPDX-License-Identifier: MIT
 *
 * sysdeps.h — trimmed, SDL-free system-dependent layer for the pistorm
 * bare-CPU Amiberry AArch64 JIT build. Mirrors the type/macro CONTRACT of
 * amiberry/src/include/sysdeps.h without its uae/string.h -> SDL3 coupling.
 *
 * Relies on the real (clean) amiberry headers still being on the include path:
 *   uae/types.h        — uae_u8..uae_u64, uaecptr, TCHAR  (needs SIZEOF_* from sysconfig.h)
 *   machdep/maccess.h  — do_get/put_mem_* byteswap accessors (pulled via memory.h)
 *   uae/byteswap.h     — __builtin_bswap* under __GNUC__
 *
 * Verified against those three files + amiberry/src/include/sysdeps.h.
 */

#ifndef PISTORM_SYSDEPS_H
#define PISTORM_SYSDEPS_H

/* sysconfig.h must have been included first (defines SIZEOF_* + target macros). */
#ifndef PISTORM_SYSCONFIG_H
#include "sysconfig.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>     /* strcasecmp / strncasecmp */
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ctype.h>
#include <assert.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>

/* uae_u8/s8/u16/s16/u32/s32/u64/s64, uaecptr (=uae_u32), uae_char, TCHAR (=char).
 * SDL-free; only needs the SIZEOF_* macros from sysconfig.h. */
#include "uae/types.h"

#ifndef uae_uintptr
typedef uintptr_t uae_uintptr;
#endif

/* ------------------------------------------------------------------ *
 * Calling convention — AArch64 uses the natural ABI (the AMIBERRY path
 * in upstream sysdeps.h leaves all of these empty).
 * ------------------------------------------------------------------ */
#ifndef REGPARAM
#define REGPARAM
#endif
#ifndef REGPARAM2
#define REGPARAM2
#endif
#ifndef REGPARAM3
#define REGPARAM3
#endif
#ifndef JITCALL
#define JITCALL
#endif

/* ------------------------------------------------------------------ *
 * Inlining
 * ------------------------------------------------------------------ */
#ifndef STATIC_INLINE
#define STATIC_INLINE static inline
#endif
#ifndef ALWAYS_INLINE
#define ALWAYS_INLINE static inline __attribute__((always_inline))
#endif
#ifndef INLINE
#define INLINE inline
#endif

/* ------------------------------------------------------------------ *
 * TCHAR string layer (replaces uae/string.h; TCHAR == char on Linux)
 * ------------------------------------------------------------------ */
#ifndef _T
#define _T(x)   x
#endif
#ifndef TEXT
#define TEXT(x) x
#endif

#define _tcslen     strlen
#define _tcscpy     strcpy
#define _tcsncpy    strncpy
#define _tcscat     strcat
#define _tcsncat    strncat
#define _tcscmp     strcmp
#define _tcsncmp    strncmp
#define _tcsicmp    strcasecmp
#define _tcsnicmp   strncasecmp
#define _tcschr     strchr
#define _tcsrchr    strrchr
#define _tcsstr     strstr
#define _tcstok     strtok
#define _tcstol     strtol
#define _tcstoul    strtoul
#define _tcstod     strtod
#define _tcsdup     strdup
#define _stprintf   sprintf
#define _sntprintf  snprintf
#define _vsntprintf vsnprintf
#define _vstprintf  vsprintf
#define _tcsftime   strftime
#define _tstof      atof
#define _tstol      atol
#define _tstoi      atoi
#define _istspace   isspace
#define _istdigit   isdigit
#define uae_tcslcpy(d, s, n) do { strncpy((d), (s), (n)); (d)[(n) - 1] = 0; } while (0)

/* ------------------------------------------------------------------ *
 * Logging — route to stderr/stdout (replaces Amiberry's write_log).
 * Real inline functions (not macros) so they don't clobber any prototype
 * carried by a kept header. NOTE: jit_abort() is intentionally NOT defined
 * here — it is a real JIT symbol (declared in compemu_arm.h, defined in
 * compemu_support_arm.cpp); defining it here would clash.
 * ------------------------------------------------------------------ */
static inline void write_log(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
static inline void console_out(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap); }
#define console_out_f console_out

/* ------------------------------------------------------------------ *
 * Misc
 * ------------------------------------------------------------------ */
#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* xmalloc family — UAE signature is (type, count), returns type*. */
#define xmalloc(T, n)     ((T *) malloc((size_t)(n) * sizeof(T)))
#define xcalloc(T, n)     ((T *) calloc((size_t)(n), sizeof(T)))
#define xrealloc(T, p, n) ((T *) realloc((p), (size_t)(n) * sizeof(T)))
#define xfree(p)          free((void *)(p))

/* ------------------------------------------------------------------ *
 * Atomics — upstream AMIBERRY inlines these in thread.h, which we don't
 * compile. uae_atomic is a 32-bit signed counter. GCC __atomic builtins.
 * ------------------------------------------------------------------ */
#ifndef uae_atomic
typedef volatile int32_t uae_atomic;
#endif

static inline uae_atomic atomic_and(volatile uae_atomic *p, uae_u32 v)
{ return __atomic_and_fetch(p, (int32_t)v, __ATOMIC_SEQ_CST); }
static inline uae_atomic atomic_or(volatile uae_atomic *p, uae_u32 v)
{ return __atomic_or_fetch(p, (int32_t)v, __ATOMIC_SEQ_CST); }
static inline uae_atomic atomic_inc(volatile uae_atomic *p)
{ return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline uae_atomic atomic_dec(volatile uae_atomic *p)
{ return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline uae_u32 atomic_bit_test_and_reset(volatile uae_atomic *p, uae_u32 v)
{ uae_u32 mask = 1u << v; uae_u32 old = __atomic_fetch_and(p, ~mask, __ATOMIC_SEQ_CST); return (old & mask) != 0; }

#endif /* PISTORM_SYSDEPS_H */
