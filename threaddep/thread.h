/* SPDX-License-Identifier: MIT
 *
 * thread.h — pistorm replacement for amiberry/src/threaddep/thread.h, which is
 * SDL3-based. This is pthread/POSIX, and is the canonical home of the atomic_*()
 * ops (matching Amiberry's AMIBERRY layout). uae_atomic itself comes from
 * sysdeps.h. Pulls the (clean) Amiberry commpipe.h for smp_comm_pipe.
 *
 * Put this on the include path AHEAD of amiberry/src/threaddep so it wins.
 */

#ifndef PISTORM_THREAD_H
#define PISTORM_THREAD_H

#include "sysconfig.h"
#include "sysdeps.h"

#include <pthread.h>
#include <semaphore.h>
#include <sched.h>

/* ------------------------------------------------------------------ *
 * Semaphores (POSIX). uae_sem_t is a POINTER (like the SDL original) so
 * commpipe.h's `p->lock = 0` initialisations are valid. Callers pass &sem.
 * ------------------------------------------------------------------ */
typedef sem_t *uae_sem_t;

static inline int uae_sem_init(uae_sem_t *s, int dummy, int initial_state)
{ (void)dummy; *s = (sem_t *)malloc(sizeof(sem_t)); return *s ? sem_init(*s, 0, (unsigned)initial_state) : -1; }
static inline void uae_sem_destroy(uae_sem_t *s)
{ if (*s) { sem_destroy(*s); free(*s); *s = 0; } }

#define uae_sem_post(PSEM)     sem_post(*(PSEM))
#define uae_sem_unpost(PSEM)   sem_post(*(PSEM))
#define uae_sem_wait(PSEM)     sem_wait(*(PSEM))
#define uae_sem_trywait(PSEM)  sem_trywait(*(PSEM))

static inline int uae_sem_getvalue(uae_sem_t *s)
{ int v = 0; if (*s) sem_getvalue(*s, &v); return v; }

/* ------------------------------------------------------------------ *
 * Threads (pthread). uae_thread_function is pthread-style void*(void*).
 * ------------------------------------------------------------------ */
typedef pthread_t uae_thread_id;
typedef void *(*uae_thread_function)(void *);

static inline int uae_start_thread(const char *name, uae_thread_function fn,
                                   void *arg, uae_thread_id *tid)
{
    (void)name;
    uae_thread_id t;
    int r = pthread_create(&t, NULL, fn, arg);
    if (r == 0 && tid) *tid = t;
    return r == 0;
}
static inline int uae_start_thread_fast(uae_thread_function fn, void *arg, uae_thread_id *tid)
{ return uae_start_thread(NULL, fn, arg, tid); }

static inline int  uae_wait_thread(uae_thread_id *tid) { return pthread_join(*tid, NULL); }
static inline void uae_end_thread(uae_thread_id *tid)  { (void)tid; }
static inline void uae_set_thread_priority(uae_thread_id *tid, int pri) { (void)tid; (void)pri; }
static inline void uae_set_thread_priority2(uae_thread_id tid, int pri) { (void)tid; (void)pri; }

/* ------------------------------------------------------------------ *
 * Atomics now live in sysdeps.h (included above), which is included before
 * this file everywhere. Kept here under the same guard only as a fallback
 * for any TU that reaches thread.h without sysdeps.h first.
 * ------------------------------------------------------------------ */
#ifndef PISTORM_ATOMICS_DEFINED
#define PISTORM_ATOMICS_DEFINED 1
static inline uae_atomic atomic_and(volatile uae_atomic *p, uae_u32 v)
{ return __atomic_and_fetch(p, v, __ATOMIC_SEQ_CST); }
static inline uae_atomic atomic_or(volatile uae_atomic *p, uae_u32 v)
{ return __atomic_or_fetch(p, v, __ATOMIC_SEQ_CST); }
static inline uae_atomic atomic_inc(volatile uae_atomic *p)
{ return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline uae_atomic atomic_dec(volatile uae_atomic *p)
{ return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline uae_u32 atomic_bit_test_and_reset(volatile uae_atomic *p, uae_u32 v)
{ uae_u32 mask = (1u << v); uae_u32 res = __atomic_fetch_and(p, ~mask, __ATOMIC_SEQ_CST); return (res & mask); }
static inline void atomic_set(volatile uae_atomic *p, uae_u32 v)
{ __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
#endif /* PISTORM_ATOMICS_DEFINED */

#include "commpipe.h"

#endif /* PISTORM_THREAD_H */
