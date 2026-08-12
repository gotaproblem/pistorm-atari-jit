/*
 * machine_cookie.c - host-side _MCH cookie forcing ("machine ste" etc.)
 *
 * Retires AUTO\SETMCH.PRG for emulator boots: instead of a guest program
 * patching the cookie jar after the OS builds it, the host watches the
 * jar come into existence and patches it directly in guest RAM.
 *
 * Mechanism, grounded in field-verified facts:
 *  - _p_cookies lives at $5A0; the jar is {tag,value} pairs (big-endian
 *    longs) terminated by {0, capacity-in-slots}.
 *  - Host writes to natmem_offset are guest-visible (every IDE-loaded
 *    program reaches the guest exactly that way).
 *  - The OS ADDS its own _MCH while building the jar, so patching must
 *    happen after the jar settles; and the jar is rebuilt on every warm
 *    boot, so the watcher re-arms when the pointer at $5A0 changes.
 *
 * Runs as a poll from ipl_task's housekeeping slot (bounded memory work,
 * no syscalls, no locks) via machine_cookie_tick(), stride-gated by the
 * caller. States: wait for a plausible jar pointer -> let it settle
 * (unchanged across two looks + terminator present) -> patch (replace
 * _MCH, or append if room) -> verify each look, re-patch if the OS
 * rewrote it, re-arm if $5A0 moved.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern unsigned char *natmem_offset;

#define ST_RAM_SIZE   0x00400000u
#define COOKIE_PTR    0x5A0u
#define C_MCH         0x5F4D4348u   /* '_MCH' */
#define JAR_MAX_SLOTS 64u           /* sanity bound while walking */

static uint32_t g_forced_mch;       /* value+1 semantics: 0 = feature off
                                       (so forcing plain ST (0) works) */
static uint32_t g_seen_ptr;         /* last jar pointer observed */
static int      g_settled;          /* pointer unchanged across looks */
static int      g_logged;

static uint32_t rl(uint32_t a)
{
    const unsigned char *p = natmem_offset + a;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void wl(uint32_t a, uint32_t v)
{
    unsigned char *p = natmem_offset + a;
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

void machine_cookie_set(uint32_t mch_value)
{
    g_forced_mch = mch_value + 1u;  /* off-by-one encoding, see above */
}

void machine_cookie_tick(void)
{
    uint32_t jar, a, i, want;

    if (!g_forced_mch || !natmem_offset)
        return;
    want = g_forced_mch - 1u;

    jar = rl(COOKIE_PTR);
    if (jar != g_seen_ptr) {        /* new/moved jar (boot or reset) */
        g_seen_ptr = jar;
        g_settled = 0;
        return;
    }
    if (jar == 0 || jar >= ST_RAM_SIZE - JAR_MAX_SLOTS * 8u ||
        (jar & 1u))
        return;                     /* no plausible jar yet */

    /* require a terminator within bounds before considering it settled */
    a = jar;
    for (i = 0; i < JAR_MAX_SLOTS && rl(a) != 0; i++)
        a += 8;
    if (i >= JAR_MAX_SLOTS)
        return;                     /* unterminated: still being built */
    if (g_settled < 2) {            /* two consecutive stable looks */
        g_settled++;
        return;
    }

    /* patch: replace an existing _MCH... */
    a = jar;
    for (i = 0; i < JAR_MAX_SLOTS && rl(a) != 0; i++, a += 8) {
        if (rl(a) == C_MCH) {
            if (rl(a + 4) != want) {
                wl(a + 4, want);
                if (!g_logged) {
                    g_logged = 1;
                    fprintf(stderr, "[MCH] _MCH cookie forced to 0x%08X "
                            "(replaced, jar @0x%06X)\n", want, jar);
                }
            }
            return;
        }
    }
    /* ...or append if the jar has room ({0,cap} slot moves down one) */
    {
        uint32_t used = i;          /* entries excluding terminator */
        uint32_t cap = rl(a + 4);
        if (cap > used + 1u && cap <= JAR_MAX_SLOTS) {
            wl(a + 8, 0);           /* new terminator first */
            wl(a + 12, cap);
            wl(a, C_MCH);           /* then the cookie */
            wl(a + 4, want);
            if (!g_logged) {
                g_logged = 1;
                fprintf(stderr, "[MCH] _MCH cookie forced to 0x%08X "
                        "(appended, jar @0x%06X)\n", want, jar);
            }
        } else if (!g_logged) {
            g_logged = 1;
            fprintf(stderr, "[MCH] cookie jar full (cap %u) - cannot "
                    "force _MCH\n", cap);
        }
    }
}
