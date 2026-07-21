/*
 * dmasnd_capture.c — STE DMA sound capture for pistorm-atari-jit-amiberry
 * platforms/atari/audio/ — built as C (gcc).
 *
 * SNAPSHOT-AT-COMMIT model:
 *   Some players (e.g. the MOD driver seen here) refill ONE fixed buffer in
 *   place every VBL and re-trigger it. Storing the buffer address and reading
 *   it later is wrong — by then the player has overwritten it, so you replay
 *   stale content. Instead, the moment the player commits a buffer we COPY its
 *   bytes into the output ring, capturing that frame's audio while it's still
 *   there. Each commit is captured exactly once, in order.
 *
 *   Cushion against output jitter comes from the consumer-side pre-roll in
 *   dmasnd_hdmi.c (the ring buffers ~0.5 s before ALSA starts draining). That
 *   is the only place slack can come from for a real-time source with no
 *   lookahead — we can neither read ahead (data doesn't exist yet) nor
 *   re-read (that repeats audio).
 *
 * Commit = $FF8901 enable edge, or (in repeat mode) an $FF8913 end-low write.
 * Set DMASND_DEBUG 0 once happy.
 */

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include "dmasnd.h"

#define DMASND_DEBUG 0

/* Opt-in probe (PISTORM_DMASND_DEBUG=1): traces the capture chain so we can see
 * where DMA sound stalls - whether the guest ever writes the $FF89xx registers,
 * whether commits fire, and whether the pump accepts or rejects each buffer. */
static int dmasnd_dbg(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PISTORM_DMASND_DEBUG");
        v = (e && *e == '1') ? 1 : 0;
    }
    return v;
}

extern unsigned char *natmem_offset;    /* pistorm_natmem.cpp: flat ST-RAM mmap */
#define ST_RAM_SIZE 0x00400000u

static const unsigned ste_rates[4] = { 6258, 12517, 25033, 50066 };

#define SND_BASE    0x00FF8900u
#define SND_TOP     0x00FF8925u
#define ADDR_MASK   0x003FFFFEu
#define MAX_FRAME   0x00040000u          /* sanity cap on one commit (256 KB) -
                                            MiNT players use ~180 KB buffers */
#define PUMP_US     500                  /* poll faster than the 20 ms VBL */

static uint8_t      reg[0x26];
static atomic_int   g_enabled = 0;
static atomic_uint  g_gen     = 0;
static atomic_int   g_repeat  = 0;   /* current $FF8901 repeat bit */

static pthread_t    pump_tid;
static atomic_int   pump_run = 0;

static uint32_t start_addr(void)
{ return (((uint32_t)reg[0x03]<<16)|((uint32_t)reg[0x05]<<8)|reg[0x07]) & ADDR_MASK; }
static uint32_t end_addr(void)
{ return (((uint32_t)reg[0x0F]<<16)|((uint32_t)reg[0x11]<<8)|reg[0x13]) & ADDR_MASK; }

static void dmasnd_commit(const char *why)
{
    if (dmasnd_dbg())
        fprintf(stderr, "[dmasnd] commit (%s) en=%d rpt=%d\n", why,
                atomic_load(&g_enabled), atomic_load(&g_repeat));
    atomic_fetch_add(&g_gen, 1);
}

/* =================== snoop (cpu_task thread) =========================== */

void dmasnd_snoop8(uint32_t addr, uint8_t val)
{
    uint32_t a = addr & 0x00FFFFFFu;
    if (a < SND_BASE || a > SND_TOP) return;
    uint32_t off = a - SND_BASE;
    reg[off] = val;

    if (dmasnd_dbg()) {
        static int first = 1;
        if (first) {
            first = 0;
            fprintf(stderr, "[dmasnd] first sound-reg write seen: $%06X = 0x%02X\n",
                    a, val);
        }
    }

    if (off == 0x00 || off == 0x01) {
        int was_enabled = atomic_load(&g_enabled);
        atomic_store(&g_repeat, (val & 0x02) ? 1 : 0);
        if (val & 0x01) {
            atomic_store(&g_enabled, 1);
            if (!was_enabled)
                dmasnd_commit(off == 0x00 ? "ctrl0-enable" : "ctrl1-enable");
        } else if (was_enabled) {
            atomic_store(&g_enabled, 0);
        }
    } else if (off == 0x13 && atomic_load(&g_enabled)) {
        dmasnd_commit(atomic_load(&g_repeat) ? "repeat-end" : "end-low");
    }
}

void dmasnd_snoop16(uint32_t addr, uint16_t val)
{
    dmasnd_snoop8(addr,     (uint8_t)(val >> 8));
    dmasnd_snoop8(addr + 1, (uint8_t)(val & 0xFF));
}
void dmasnd_snoop32(uint32_t addr, uint32_t val)
{
    dmasnd_snoop8(addr,     (uint8_t)(val >> 24));
    dmasnd_snoop8(addr + 1, (uint8_t)(val >> 16));
    dmasnd_snoop8(addr + 2, (uint8_t)(val >>  8));
    dmasnd_snoop8(addr + 3, (uint8_t)(val & 0xFF));
}

/* =================== pump: snapshot each commit ======================= */

int  dmasnd_is_repeat(void) { return atomic_load(&g_repeat); }
void dmasnd_pump(void) { /* logic in thread */ }

static void *pump_thread(void *arg)
{
    (void)arg;
    unsigned last_gen  = atomic_load(&g_gen);
    uint8_t  last_mode = 0xFF;

    while (atomic_load(&pump_run)) {
        unsigned gen = atomic_load_explicit(&g_gen, memory_order_acquire);
        if (gen != last_gen) {
            /* New commit(s): copy the CURRENT buffer once, now, while it still
               holds this frame's audio. We capture one buffer per poll where a
               commit happened; at 50 commits/sec vs a 0.5 ms poll there is one
               commit per poll, so nothing is duplicated or (in practice) missed.
               Reading here (not in the snoop) means the player's pointer-write
               burst has settled, so start/end are coherent. */
            last_gen = gen;
            if (natmem_offset) {
                uint32_t s = start_addr(), e = end_addr();
                uint8_t  m = reg[0x21];
                if (e > s && (e - s) <= MAX_FRAME && e <= ST_RAM_SIZE) {
                    if (m != last_mode) {
                        dmasnd_set_mode(ste_rates[m & 3], (m & 0x80) ? 0 : 1);
                        last_mode = m;
                    }
                    dmasnd_note_frame_len(e - s);
                    dmasnd_write_bytes(&natmem_offset[s], e - s);
                    if (dmasnd_dbg())
                        fprintf(stderr, "[dmasnd] accept s=0x%06X e=0x%06X len=%u "
                                "%s %uHz\n", s, e, e - s,
                                (m & 0x80) ? "mono" : "stereo", ste_rates[m & 3]);
                } else if (dmasnd_dbg()) {
                    fprintf(stderr, "[dmasnd] REJECT s=0x%06X e=0x%06X len=%d mode=0x%02X"
                            " (need e>s, len<=%u, e<=0x%X)\n",
                            s, e, (int)e - (int)s, m, MAX_FRAME, ST_RAM_SIZE);
                }
            }
        }
        usleep(PUMP_US);
    }
    return NULL;
}

/* =================== debug thread (off the audio path) ================= */
#if DMASND_DEBUG
static pthread_t  dbg_tid;
static atomic_int dbg_run = 0;
static void *dbg_thread(void *arg)
{
    (void)arg;
    while (atomic_load(&dbg_run)) {
        usleep(1000000);
        uint8_t m = reg[0x21];
        fprintf(stderr,
            "[dmasnd] dma=%u commits=%u frames=%u xr=%u | "
            "Cstart=0x%06x Cend=0x%06x len=%u ctrl=0x%02x %s %uHz ring=%u en=%d\n",
            atomic_load(&dbg_writes), atomic_load(&dbg_commits),
            atomic_load(&dbg_frames), dmasnd_xruns(),
            dbg_cs, dbg_ce, (dbg_ce > dbg_cs) ? (dbg_ce - dbg_cs) : 0,
            reg[0x01], (m & 0x80) ? "MONO" : "STER",
            ste_rates[m & 3], dmasnd_ring_used(), atomic_load(&g_enabled));
    }
    return NULL;
}
#endif

int dmasnd_capture_start(void)
{
    if (atomic_load(&pump_run)) return 0;
    atomic_store(&pump_run, 1);
    if (pthread_create(&pump_tid, NULL, pump_thread, NULL) != 0) {
        atomic_store(&pump_run, 0); return -1;
    }
#if DMASND_DEBUG
    atomic_store(&dbg_run, 1);
    pthread_create(&dbg_tid, NULL, dbg_thread, NULL);
    fprintf(stderr, "[dmasnd] capture pump thread started\n");
#endif
    return 0;
}

void dmasnd_capture_stop(void)
{
    if (!atomic_load(&pump_run)) return;
#if DMASND_DEBUG
    atomic_store(&dbg_run, 0);
    pthread_join(dbg_tid, NULL);
#endif
    atomic_store(&pump_run, 0);
    pthread_join(pump_tid, NULL);
}

void dmasnd_capture_reset(void)
{
    for (unsigned i = 0; i < sizeof reg; i++) reg[i] = 0;
    atomic_store(&g_enabled, 0);
    atomic_store(&g_gen, 0);
    atomic_store(&g_repeat, 0);
    dmasnd_output_reset();
}
