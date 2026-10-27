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
#include "platforms/atari/psctrl/psctrl_tunables.h"   /* PS_INFO */

extern unsigned char *natmem_offset;

#define ST_RAM_SIZE   0x00400000u
#define COOKIE_PTR    0x5A0u
#define C_MCH         0x5F4D4348u   /* '_MCH' */
#define JAR_MAX_SLOTS 64u           /* sanity bound while walking */

/* The cookies the host forces: _MCH always (when a `machine` line is
 * present), plus _VDO and _SND for the STE class. Why the extra two:
 * TOS 2.06 (bios/startup.S, th-otto/tos3x) decides "STE" by writing 90
 * to $FF820D and reading it back. On a PiStorm in a plain ST that read
 * reaches the real ST shifter, which has no $FF820D, so TOS records "no
 * STE", writes _VDO = 0 (ST shifter) and, skipping the $FF9200 stereo
 * test, _SND = 1 (YM only) - while the emulator IS giving it DMA sound
 * (`machine ste`). SYSINFO and STE programs then report no DMA sound
 * (field: _MCH $10000, _VDO 0, _SND 1 under TOS 2.06). Making the probe
 * itself pass would also need $FF9200 answered (it bus-errors on an
 * ST) and would turn on all of TOS's STE paths, so the cookies are set
 * here instead, the same way _MCH is. TOS still runs its ST paths
 * internally; those only differ in STE shifter registers the real ST
 * shifter ignores anyway. */
#define MAX_FORCED 3
static struct { uint32_t tag, want; int logged; } g_forced[MAX_FORCED];
static int      g_nforced;          /* 0 = feature off */
static uint32_t g_seen_ptr;         /* last jar pointer observed */
static int      g_settled;          /* pointer unchanged across looks */
static int      g_full_logged;

static uint32_t rl(uint32_t a)
{
    const unsigned char *p = natmem_offset + a;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* A real jar: every tag is four printable ASCII bytes (all TOS/MiNT/EmuTOS
 * cookies are), and the OS's own _CPU or _VDO is present - both are put in
 * by every TOS that has a jar (1.06 on) and by EmuTOS. TOS 1.04 and older
 * have NO jar: $5A0 is then just a long in low RAM that games use freely,
 * and "patching" whatever it points at wrote 16 bytes into running game
 * code/data - random crashes and resets once a game was running (field
 * log: "[MCH] cookie jar full (cap 1320684748)" = $4EB80CCC, a JSR, just
 * before the game jumped to the reset vector). */
#define C_CPU 0x5F435055u   /* '_CPU' */
#define C_VDO 0x5F56444Fu   /* '_VDO' */
#define C_SND 0x5F534E44u   /* '_SND' */

static int jar_looks_real(uint32_t jar, uint32_t *entries)
{
    uint32_t a = jar, i;
    int os_cookie = 0;

    for (i = 0; i < JAR_MAX_SLOTS; i++, a += 8) {
        uint32_t tag = rl(a);
        if (tag == 0)
            break;
        for (int b = 0; b < 32; b += 8) {
            uint32_t c = (tag >> b) & 0xFFu;
            if (c < 0x20u || c > 0x7Eu)
                return 0;               /* not a cookie tag */
        }
        if (tag == C_CPU || tag == C_VDO)
            os_cookie = 1;
    }
    if (i >= JAR_MAX_SLOTS || i == 0 || !os_cookie)
        return 0;
    if (rl(a + 4) > JAR_MAX_SLOTS * 4u || rl(a + 4) <= i)
        return 0;                       /* capacity must be plausible */
    *entries = i;
    return 1;
}

static void wl(uint32_t a, uint32_t v)
{
    unsigned char *p = natmem_offset + a;
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void force_add(uint32_t tag, uint32_t want)
{
    for (int k = 0; k < g_nforced; k++)
        if (g_forced[k].tag == tag) { g_forced[k].want = want; return; }
    if (g_nforced < MAX_FORCED) {
        g_forced[g_nforced].tag = tag;
        g_forced[g_nforced].want = want;
        g_forced[g_nforced].logged = 0;
        g_nforced++;
    }
}

void machine_cookie_set(uint32_t mch_value)
{
    force_add(C_MCH, mch_value);
}

void machine_cookie_set_ste_hw(void)
{
    force_add(C_VDO, 0x00010000u);  /* STE shifter */
    force_add(C_SND, 0x00000003u);  /* YM2149 + stereo 8-bit DMA sound */
}

static void tag_name(uint32_t tag, char out[5])
{
    out[0] = (char)(tag >> 24); out[1] = (char)(tag >> 16);
    out[2] = (char)(tag >> 8);  out[3] = (char)tag; out[4] = '\0';
}

/* one forced cookie: replace it, or append it if the jar has room
 * ({0,cap} slot moves down one). Returns 0 if the jar is full. */
static int force_one(uint32_t jar, int k)
{
    uint32_t a = jar, i;
    uint32_t tag = g_forced[k].tag, want = g_forced[k].want;
    char nm[5];

    for (i = 0; i < JAR_MAX_SLOTS && rl(a) != 0; i++, a += 8) {
        if (rl(a) == tag) {
            if (rl(a + 4) != want) {
                wl(a + 4, want);
                if (!g_forced[k].logged) {
                    g_forced[k].logged = 1;
                    tag_name(tag, nm);
                    PS_INFO("[MCH] %s cookie forced to 0x%08X "
                            "(replaced, jar @0x%06X)\n", nm, want, jar);
                }
            }
            return 1;
        }
    }
    {
        uint32_t used = i;          /* entries excluding terminator */
        uint32_t cap = rl(a + 4);
        if (cap > used + 1u && cap <= JAR_MAX_SLOTS) {
            wl(a + 8, 0);           /* new terminator first */
            wl(a + 12, cap);
            wl(a, tag);             /* then the cookie */
            wl(a + 4, want);
            if (!g_forced[k].logged) {
                g_forced[k].logged = 1;
                tag_name(tag, nm);
                PS_INFO("[MCH] %s cookie forced to 0x%08X "
                        "(appended, jar @0x%06X)\n", nm, want, jar);
            }
            return 1;
        }
        if (!g_full_logged) {
            g_full_logged = 1;
            tag_name(tag, nm);
            fprintf(stderr, "[MCH] cookie jar full (cap %u, %u used) - "
                    "cannot force %s\n", cap, used, nm);
        }
        return 0;
    }
}

void machine_cookie_tick(void)
{
    uint32_t jar;

    if (!g_nforced || !natmem_offset)
        return;

    jar = rl(COOKIE_PTR);
    if (jar != g_seen_ptr) {        /* new/moved jar (boot or reset) */
        g_seen_ptr = jar;
        g_settled = 0;
        return;
    }
    if (jar == 0 || jar >= ST_RAM_SIZE - JAR_MAX_SLOTS * 8u ||
        (jar & 1u))
        return;                     /* no plausible jar yet */

    /* require a REAL jar (terminated, cookie-shaped tags, the OS's own
     * _CPU/_VDO present) before considering it settled - see
     * jar_looks_real(). No jar (TOS < 1.06): never touch anything. */
    {
        uint32_t n;
        if (!jar_looks_real(jar, &n)) {
            g_settled = 0;
            return;
        }
    }
    if (g_settled < 2) {            /* two consecutive stable looks */
        g_settled++;
        return;
    }

    for (int k = 0; k < g_nforced; k++)
        if (!force_one(jar, k))
            break;
}
