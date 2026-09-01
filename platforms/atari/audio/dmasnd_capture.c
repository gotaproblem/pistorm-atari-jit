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
#include "../mfp_hub.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <sys/eventfd.h>
#include "dmasnd.h"

#define DMASND_DEBUG 0

/* Opt-in probes:
 *   PISTORM_DMASND_DEBUG=1  verbose - traces the capture chain per event
 *                           (register writes, commits, per-frame boundary
 *                           lines, pump accept/reject). Heavy: the print
 *                           avalanche itself perturbs timing-sensitive
 *                           guests (field case: Bad Apple stalls at its
 *                           loading screen under =1).
 *   PISTORM_DMASND_DEBUG=2  summary - ONE line per second from the pump
 *                           thread (frame events/s, active frame, bps,
 *                           computed duration). Cheap enough to measure
 *                           playback cadence without disturbing it. */
static int dmasnd_dbg_level(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PISTORM_DMASND_DEBUG");
        v = e ? atoi(e) : 0;
        if (v < 0) v = 0;
    }
    return v;
}
static int dmasnd_dbg(void)
{
    return dmasnd_dbg_level() == 1;
}

extern unsigned char *natmem_offset;    /* pistorm_natmem.cpp: flat ST-RAM mmap */
#define ST_RAM_SIZE 0x00400000u

static const unsigned ste_rates[4] = { 6258, 12517, 25033, 50066 };

#define SND_BASE    0x00FF8900u
#define SND_TOP     0x00FF8925u
#define ADDR_MASK   0x003FFFFEu
#define MAX_FRAME   0x00100000u          /* sanity cap on one commit (1 MB).
                                            MiNT players use ~180 KB buffers;
                                            FastBobs loops its entire 345 KB
                                            soundtrack as ONE repeat frame
                                            (was 256 KB: every frame REJECTed
                                            = silence). SDL streams grow
                                            dynamically, so a big one-shot
                                            queue is fine. */
#define PUMP_US     500                  /* FALLBACK poll only (no eventfd) */

static uint8_t      reg[0x26];
static atomic_int   g_enabled = 0;
static atomic_uint  g_gen     = 0;

/* pacing counters: stg = guest stagings of a new frame (start-low $8907
 * writes - Bad Apple's ISR writes end regs then start regs, start-low
 * last, so one $8907 write = one chunk processed by the guest). rep =
 * boundaries that latched an UNCHANGED frame (guest didn't restage in
 * time = replay). stg > ev/s means stagings overwritten before the
 * latch = chunks silently skipped = playback runs fast. */
static atomic_uint g_ct_stage;
static atomic_uint g_ct_rep;
static atomic_int   g_repeat  = 0;   /* current $FF8901 repeat bit */

static pthread_t    pump_tid;
static atomic_int   pump_run = 0;
static int          pump_evfd = -1;  /* commit -> pump wakeup (eventfd) */

static uint32_t start_addr(void)
{ return (((uint32_t)reg[0x03]<<16)|((uint32_t)reg[0x05]<<8)|reg[0x07]) & ADDR_MASK; }
static uint32_t end_addr(void)
{ return (((uint32_t)reg[0x0F]<<16)|((uint32_t)reg[0x11]<<8)|reg[0x13]) & ADDR_MASK; }

static struct timespec g_mw_t0;      /* microwire transfer start - readback side */

/* THE LATCHED FRAME MODEL. Real STE double-buffers the frame registers:
 * start/end (and the rate) written while a frame plays do NOT take
 * effect until the frame boundary - that is the whole basis of buffer
 * chaining (the ISR stages the NEXT buffer while the counter keeps
 * walking the CURRENT one). reg[] holds what the guest wrote (staged);
 * g_act_* hold what is playing (latched at boundaries by
 * frames_advance, ipl_task-owned; readers use the atomics). */
static atomic_uint   g_act_s;        /* active frame start   */
static atomic_uint   g_act_e;        /* active frame end     */
static atomic_uint   g_act_bps;      /* active bytes/second  */
static atomic_ullong g_ft0;          /* active frame start time, ns */
static atomic_int    g_parked;       /* non-repeat frame completed  */

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static atomic_int  g_g7_app;         /* guest vector $13C points into ST RAM */
/* virtual MFP pending/in-service state moved to mfp_hub.c (the shadow
 * here set in-service at IACK but never CHECKED it before re-raising -
 * Paula's Timer A re-entered its own handler; same disease class as the
 * keyboard channel's stack death, fixed once in the hub for all) */

static uint32_t frame_bps(void);         /* defined with the readback side */

/* is_enable: 1 = ctrl play-bit enable edge (a frame STARTS: latch the
 * staged registers immediately, hardware-style), 0 = end-register chain
 * write mid-frame (stage only - the latch happens at the next frame
 * boundary in frames_advance; resetting timing here was the bug that
 * teleported the counter into the next buffer 20ms early). */
static void dmasnd_commit(const char *why, int is_enable)
{
    if (dmasnd_dbg())
        fprintf(stderr, "[dmasnd] commit (%s) en=%d rpt=%d\n", why,
                atomic_load(&g_enabled), atomic_load(&g_repeat));
    if (is_enable) {
        atomic_store(&g_act_s, start_addr());
        atomic_store(&g_act_e, end_addr());
        atomic_store(&g_act_bps, frame_bps());
        atomic_store(&g_ft0, now_ns());
        atomic_store(&g_parked, 0);
        if (dmasnd_dbg() && natmem_offset) {
            /* What will the next VBL run? Dump the interrupt vectors and
             * the whole VBL queue at the moment playback starts - the
             * field case dies within one VBL period of this write with
             * no exception logged: a clean jump from somewhere here. */
            #define RL(a) ((uint32_t)natmem_offset[a] << 24 | \
                           (uint32_t)natmem_offset[(a)+1] << 16 | \
                           (uint32_t)natmem_offset[(a)+2] << 8 | \
                           (uint32_t)natmem_offset[(a)+3])
            uint32_t vblq = RL(0x456);
            int i;
            fprintf(stderr, "[dmasnd] vecs: ill(10)=%06X hbl(68)=%06X "
                    "vbl(70)=%06X mfp13C=%06X resval(426)=%08X resvec(42A)=%06X\n",
                    RL(0x10), RL(0x68), RL(0x70), RL(0x13C),
                    RL(0x426), RL(0x42A));
            fprintf(stderr, "[dmasnd] nvbls(454)=%u vblqueue(456)->%06X:",
                    (unsigned)((natmem_offset[0x454] << 8) | natmem_offset[0x455]),
                    vblq);
            if (vblq && vblq < ST_RAM_SIZE - 32)
                for (i = 0; i < 8; i++)
                    fprintf(stderr, " %06X", RL(vblq + i * 4));
            fprintf(stderr, "\n");
            #undef RL
        }
    }
    if (natmem_offset)
    {
        /* Sample the guest's GPIP7 vector ($13C) from the flat ST-RAM
         * mirror (same source the capture pump reads audio from - no
         * legacy Musashi hooks). In ST RAM = app sound handler ->
         * pulses deliverable; ROM = OS monitor-detect -> withheld. */
        const uint8_t *p = natmem_offset + 0x13Cu;
        uint32_t vec = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
        atomic_store(&g_g7_app, (vec != 0 && vec < 0x00400000u) ? 1 : 0);
        if (dmasnd_dbg())
            fprintf(stderr, "[dmasnd] gpip7 vector $13C = 0x%06X (%s)\n",
                    vec, (vec != 0 && vec < 0x00400000u)
                         ? "app/RAM: deliver" : "OS/ROM: withhold");
    }
    /* Capture triggers: ONLY the enable edge queues audio here (first
     * frame). Chain/stage commits no longer trigger capture - the frame
     * is copied when it becomes ACTIVE at the boundary (frames_advance),
     * after the guest's mixer has had the full frame time to fill it. */
    if (is_enable) {
        atomic_fetch_add(&g_gen, 1);
        if (pump_evfd >= 0) {
            uint64_t one = 1;
            ssize_t r = write(pump_evfd, &one, sizeof one);
            (void)r;                     /* EAGAIN = already signalled: fine */
        }
    }
}

/* =================== snoop (cpu_task thread) =========================== */

void dmasnd_snoop8(uint32_t addr, uint8_t val)
{
    uint32_t a = addr & 0x00FFFFFFu;
    if (a < SND_BASE || a > SND_TOP) return;
    uint32_t off = a - SND_BASE;
    reg[off] = val;
    if (off == 0x07)
        atomic_fetch_add(&g_ct_stage, 1);

    if (dmasnd_dbg()) {
        static int wlogged = 0;
        if (wlogged < 48) {
            wlogged++;
            fprintf(stderr, "[dmasnd] W $%06X=%02X\n", a, val);
        }
    }

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
                dmasnd_commit(off == 0x00 ? "ctrl0-enable" : "ctrl1-enable", 1);
        } else if (was_enabled) {
            atomic_store(&g_enabled, 0);
        }
    } else if (off == 0x13 && atomic_load(&g_enabled)) {
        dmasnd_commit(atomic_load(&g_repeat) ? "repeat-end" : "end-low", 0);
    } else if (off == 0x22 || off == 0x23) {
        /* Microwire data register ($FF8922): both halves are captured in
         * reg[] above, so decode once the word is complete. Decoding twice
         * for a word write is harmless - the command is idempotent. The
         * timestamp starts the emulated ~20us shift the readback side
         * plays back (EmuTOS/TOS busy-wait on it - see mw_data_now). */
        clock_gettime(CLOCK_MONOTONIC, &g_mw_t0);
        dmasnd_microwire_write((uint16_t)((reg[0x22] << 8) | reg[0x23]));
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

/* =================== register readback (cpu thread) ==================== */
/* On a plain ST the $FF89xx range bus-errors - the STE DMA-sound hardware
 * only exists host-side. So READS of the range must be answered here too,
 * or hardware probes (SysInfo etc.) conclude the hardware is absent, and
 * players that poll the play bit / frame counter die on the probe.
 *
 * The shadow reg[] already holds every byte the guest wrote. Two values
 * are live and synthesized from wall-clock time at the current STE byte
 * rate: the frame counter ($FF8909/0B/0D) walking start..end, and the
 * play bit ($FF8901 bit 0), which self-clears at frame end in non-repeat
 * mode exactly like the real hardware. */

/* Microwire transfer emulation. On real hardware a write to the data
 * register starts a ~20us transfer during which the data register
 * shifts left one bit per microwire clock (reading 0 once all 16 bits
 * have gone) and the mask register ROTATES, returning to its written
 * value at completion. EmuTOS's write_microwire() busy-waits for data
 * == 0; TOS waits for the mask to read back its original value - both
 * idioms complete against this model. A static shadow hangs EmuTOS at
 * the version screen (infinite poll), hence the timestamping. */

#define MW_NS_PER_BIT 1400u              /* ~1.4 us per bit, 16 bits */

static unsigned mw_bits_elapsed(void)
{
    struct timespec now;
    uint64_t ns;

    clock_gettime(CLOCK_MONOTONIC, &now);
    ns = (uint64_t)(now.tv_sec - g_mw_t0.tv_sec) * 1000000000ull
       + (uint64_t)(now.tv_nsec - g_mw_t0.tv_nsec);
    ns /= MW_NS_PER_BIT;
    return ns > 16 ? 16 : (unsigned)ns;
}

static uint16_t mw_data_now(void)
{
    unsigned n = mw_bits_elapsed();
    uint16_t d = (uint16_t)(((uint16_t)reg[0x22] << 8) | reg[0x23]);

    return (n >= 16) ? 0 : (uint16_t)(d << n);
}

static uint16_t mw_mask_now(void)
{
    unsigned n = mw_bits_elapsed() & 15;
    uint16_t m = (uint16_t)(((uint16_t)reg[0x24] << 8) | reg[0x25]);

    return (uint16_t)((m << n) | (m >> (16 - n)));
}

static uint32_t frame_bps(void)
{
    unsigned rate = ste_rates[reg[0x21] & 3];
    return (reg[0x21] & 0x80) ? rate : rate * 2;    /* bit7: 1 = mono */
}

static int playing_now(void)
{
    return atomic_load(&g_enabled) && !atomic_load(&g_parked);
}

/* readback counters: does the guest form its idea of time by READING the
 * hardware back? rdc = ctrl ($8901), rda = address counter ($8909/0B/0D).
 * A player that never reads the counter cannot be misled by it. */
static atomic_uint g_ct_rd_ctrl;
static atomic_uint g_ct_rd_cnt;

/* IPL latch counters (emulator.c ipl_task, single writer): interrupt
 * requests presented to the CPU per level. i4 is the guest's VBL rate -
 * the field case that motivated them: phantom level-4s from unsynchron-
 * ized IPL line sampling paced Bad Apple ~12% fast. */
extern volatile unsigned pistorm_ipl_lat2, pistorm_ipl_lat4,
                         pistorm_ipl_lat6;

/* Real-bus GPIP sample for the =2 summary (gp=): reads the PHYSICAL MFP
 * GPIP register once per second - bit 7 is the real MonoDetect wire,
 * which on an ST also feeds the GLUE's MONOMON timing select. Field
 * question this answers: the GLUE runs 71.47Hz (mono timing) with
 * REZ=0 the moment Bad Apple starts - is the physical line actually
 * being pulled low? ps_read_8 takes the bus lock; one read/s from the
 * pump thread is harmless and GPIP reads have no side effects. */
extern uint8_t ps_read_8(uint32_t address);

/* counter walks the ACTIVE (latched) frame - staged chain writes must
 * NOT move it (that divergence corrupted chaining players) */
static uint32_t cur_counter(void)
{
    uint32_t s = atomic_load(&g_act_s), e = atomic_load(&g_act_e);
    uint32_t bps = atomic_load(&g_act_bps);
    uint64_t off;

    if (!playing_now() || e <= s || !bps)
        return e;                                   /* idle: holds end */
    off = (now_ns() - atomic_load(&g_ft0)) / 1000u * bps / 1000000u;
    if (off >= (e - s))
        return e;               /* boundary imminent; advance is ipl-side */
    return (s + (uint32_t)off) & ~1u;
}

int dmasnd_owns(uint32_t addr)
{
    uint32_t a = addr & 0x00FFFFFFu;
    return a >= SND_BASE && a <= SND_TOP;
}

uint8_t dmasnd_reg_read8(uint32_t addr)
{
    uint32_t off = (addr & 0x00FFFFFFu) - SND_BASE; /* caller checked range */
    uint32_t c;

    if (dmasnd_dbg() && (off == 0x01 || off == 0x09 || off == 0x0B || off == 0x0D)) {
        static int logged = 0;
        if (logged < 12) {
            logged++;
            fprintf(stderr, "[dmasnd] read $%06X (ctrl/cnt) en=%d\n",
                    (unsigned)(SND_BASE + off), atomic_load(&g_enabled));
        }
    }

    switch (off) {
    case 0x01:                  /* ctrl: live play bit, repeat as written */
        atomic_fetch_add(&g_ct_rd_ctrl, 1);
        return (uint8_t)((reg[0x01] & 0x02) | (playing_now() ? 0x01 : 0x00));
    case 0x09: atomic_fetch_add(&g_ct_rd_cnt, 1);
               c = cur_counter(); return (uint8_t)(c >> 16);
    case 0x0B: atomic_fetch_add(&g_ct_rd_cnt, 1);
               c = cur_counter(); return (uint8_t)(c >> 8);
    case 0x0D: atomic_fetch_add(&g_ct_rd_cnt, 1);
               c = cur_counter(); return (uint8_t)c;
    case 0x22: return (uint8_t)(mw_data_now() >> 8);    /* microwire data */
    case 0x23: return (uint8_t)mw_data_now();
    case 0x24: return (uint8_t)(mw_mask_now() >> 8);    /* microwire mask */
    case 0x25: return (uint8_t)mw_mask_now();
    default:
        return (off < sizeof reg) ? reg[off] : 0xFF;
    }
}

uint16_t dmasnd_reg_read16(uint32_t addr)
{
    return (uint16_t)(((uint16_t)dmasnd_reg_read8(addr) << 8) |
                       dmasnd_reg_read8(addr + 1));
}

uint32_t dmasnd_reg_read32(uint32_t addr)
{
    return ((uint32_t)dmasnd_reg_read16(addr) << 16) |
            dmasnd_reg_read16(addr + 2);
}

/* =================== synthesized frame interrupt ====================== */
/* On a real STE the DMA sound engine signals end-of-frame to the MFP:
 * Timer A in event-count mode counts frame-done pulses, and GPIP7 (which
 * on the STE is XSINT, not just monochrome-detect) sees the same event.
 * Interrupt-driven players (buffer chaining) depend on one or the other.
 * The real MFP on a plain ST never sees these events, so - exactly like
 * the USB keyboard's GPIP4 - the level-6 raise and the vector are
 * synthesised host-side from the frame timing model above. The guest's
 * IERA/IMRA/VR/TACR/TADR writes are snooped so masking, the vector base
 * and the Timer A count behave as programmed. */

/* IERA/IMRA/VR/TACR/TADR shadows, the Timer A event countdown, pending
 * latches and the IACK vector all live in mfp_hub.c now; this file just
 * reports frame-end events (mfp_hub_timer_a_event / mfp_hub_raise). */

/* delivery counters for the =2 summary */
static atomic_uint g_ct_g7;              /* GPIP7 raised at a boundary    */

/* (MFP register snooping moved wholesale to mfp_hub_write_snoop.) */


/* GPIP7 delivery policy. Field-verified both ways: FastBobs installs
 * its own GPIP7 sound handler (in ST RAM) and needs the pulses; Paula
 * leaves the OS's ROM monitor-detect handler on vector $13C, which
 * treats a sound pulse as a monitor swap and REBOOTS (trace: IACK vec
 * 0x4F -> immediate RESET at ROM e00088). Discriminator: at each sound
 * commit the CPU thread samples the guest's vector $13C - a handler in
 * ST RAM is an app's sound handler (deliver), a ROM address is the OS
 * monitor-detect (stay silent). PISTORM_DMASND_GPIP7=0 forces off,
 * =1 forces always-on, unset = this auto policy. */
static int g7_mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PISTORM_DMASND_GPIP7");
        v = (e && *e == '0') ? 0 : (e && *e == '1') ? 1 : 2;
    }
    return v;
}

static int g7_deliver(void)
{
    int m = g7_mode();
    if (m == 2) {
        /* AUTO policy, sampled FRESH at every frame boundary (50Hz x a
         * 4-byte natmem read - free). The old cached verdict was taken
         * at the sound-ENABLE commit; Paula's STE mode enables DMA
         * first and installs its $13C handler + IERA bit7 AFTER, so
         * the cache said "OS/ROM: withhold" forever and the frame
         * interrupt never came - the guest looped one buffer (rep=51/s,
         * stg=0, position frozen). PISTORM_DMASND_GPIP7=1 confirmed:
         * forced delivery = full-rate playback. Reading live removes
         * the ordering assumption entirely; the reboot protection
         * (ROM monitor-detect handler = withhold) is preserved. */
        const uint8_t *p = natmem_offset + 0x13Cu;
        uint32_t vec = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
        return (vec != 0 && vec < 0x00400000u);
    }
    return m;
}

/* one frame boundary: report the events; the hub owns the Timer A
 * event countdown (TACR=8), the pending latches, masking, in-service
 * gating and the vector */
static void frame_end_event(unsigned frameno)
{
    mfp_hub_timer_a_event();                         /* ch 13 when due */
    if (g7_deliver()) {
        mfp_hub_raise(15);                           /* GPIP7 / XSINT  */
        atomic_fetch_add(&g_ct_g7, 1);
    }
    if (dmasnd_dbg()) {
        /* uncapped and self-describing: length, bps and the computed
         * frame duration make clock errors directly measurable */
        uint32_t s2 = atomic_load(&g_act_s), e2 = atomic_load(&g_act_e);
        uint32_t b2 = atomic_load(&g_act_bps);
        fprintf(stderr, "[dmasnd] frame#%u act=%05X..%05X len=%u bps=%u "
                "dur=%uus\n",
                frameno, s2, e2, e2 - s2, b2,
                (unsigned)((uint64_t)(e2 - s2) * 1000000u / (b2 ? b2 : 1)));
    }
}

/* advance the latched frame clock over any elapsed boundaries.
 * ipl_task ONLY - single writer of g_act_* / g_ft0 / g_parked while
 * playing (dmasnd_commit's enable-latch happens when nothing plays). */
static void frames_advance(void)
{
    static unsigned frameno;
    uint32_t s = atomic_load(&g_act_s), e = atomic_load(&g_act_e);
    uint32_t bps = atomic_load(&g_act_bps);
    uint64_t t0 = atomic_load(&g_ft0);
    uint64_t now = now_ns();
    uint64_t dur;
    int guard = 0;

    if (e <= s || !bps) {
        atomic_store(&g_parked, 1);
        return;
    }
    dur = (uint64_t)(e - s) * 1000000000ull / bps;
    while (now >= t0 + dur && guard++ < 64) {
        uint32_t ps = s, pe = e;
        t0 += dur;
        /* frame boundary: latch the STAGED registers (STE hardware
         * double-buffering - this is where chain writes take effect) */
        s = start_addr();
        e = end_addr();
        bps = frame_bps();
        if (s == ps && e == pe)              /* guest didn't restage:  */
            atomic_fetch_add(&g_ct_rep, 1);  /* replay of same frame   */
        atomic_store(&g_act_s, s);
        atomic_store(&g_act_e, e);
        atomic_store(&g_act_bps, bps);
        atomic_store(&g_ft0, t0);
        /* the just-latched frame is now what hardware would fetch:
         * wake the pump to capture it (complete - the guest's mixer
         * had the whole previous frame's duration to fill it) */
        atomic_fetch_add(&g_gen, 1);
        if (pump_evfd >= 0) {
            uint64_t one = 1;
            ssize_t r = write(pump_evfd, &one, sizeof one);
            (void)r;
        }
        frame_end_event(++frameno);
        if (!atomic_load(&g_repeat)) {
            atomic_store(&g_parked, 1);              /* single-shot done */
            break;
        }
        if (e <= s || !bps) {
            atomic_store(&g_parked, 1);
            break;
        }
        dur = (uint64_t)(e - s) * 1000000000ull / bps;
    }
}

/* Frame-clock pump: ipl_task ONLY (single writer on the t0+=dur
 * sequence - routing it through intlev_ack once put two writers on the
 * clock and it lurched/stalled: 171 ev/s against a needed 1252/s). The
 * clock must advance REGARDLESS of pending-interrupt state: real STE
 * DMA keeps playing and re-triggering whether or not the CPU has
 * serviced the interrupt (an early-out on pending once starved the DAC
 * 5x). The stride keeps the hot loop from paying a clock read every
 * iteration. Arbitration/pending is the hub's job now - this only
 * FEEDS it (frame_end_event -> mfp_hub_timer_a_event / mfp_hub_raise). */

/* GPIP level shim: on a real STE the XSINT line toggles at each frame
 * end and is XORed onto GPIP7, and handlers confirm "this interrupt is
 * mine" by reading the level ($FFFA01 bit 7). Field case: Paula's own
 * RAM handler read the real (never-moving) bit, concluded "not mine",
 * chained to the saved OS monitor-detect vector - reset. The level here
 * is the parity of frame-end pulses since the commit: exact, stateless,
 * and coherent with the pulse/IACK side. */
uint8_t dmasnd_gpip_shim(uint8_t real)
{
    /* Real STE physics: GPIP7 = MonoDetect XOR SNDINT, and SNDINT idles
     * at the OPPOSITE level while DMA sound is PLAYING - the line reads
     * INVERTED for the whole duration of playback. EmuTOS 1.3's VBL
     * monitor-change check depends on this exactly: it reads GPIP and,
     * if DMA ctrl bit 0 says playing, INVERTS the reading (neg.b d0 at
     * ROM e0ab42 in etos256uk-13) before comparing monitor type.
     *
     * History: a per-frame parity TOGGLE here was wrong (patch 10 -
     * random monitor-swap verdicts), and a pure no-op was also wrong
     * (EmuTOS un-inverted a never-inverted level -> "monitor changed"
     * -> warm boot one VBL after EVERY playback start; that was the
     * Paula reboot - demos that kill the OS VBL never ran the check).
     * The hardware-true model is a steady inversion while playing,
     * coherent with the ctrl-register readback (same playing_now()). */
    if (playing_now())
    {
        uint8_t v = (uint8_t)(real ^ 0x80u);
        /* EmuTOS's detect_monitor_change() un-XORs this byte with NEG,
         * not EOR - and negation has fixed points 0x00 and 0x80. If the
         * byte reads EXACTLY 0x00 (our inversion clears bit7 while the
         * IDE shim holds GPIP5 low, a pending keyboard byte holds GPIP4
         * low, and the serial lines idle low on this machine), -0 == 0
         * defeats the compensation and EmuTOS concludes "mono monitor":
         * it rewrites the shifter and the GLUE jumps to 71.4Hz hi-res.
         * Field case: Bad Apple ~19% fast (VBL/2-gated player at
         * 35.8 chunks/s = 71.6/2), colour monitor loses sync during
         * playback, intermittent per boot (needs several GPIP bits to
         * line up). Real STEs never present 0x00 here - the idle RS232
         * receiver lines pull their GPIP bits high; emulate that
         * pull-up with RI (bit 6, inert) in the one case it matters. */
        if (v == 0x00u)
            v = 0x40u;
        return v;
    }
    return real;
}

/* (Virtual IPRA/ISRA read shims and the IACK vector moved to
 * mfp_hub_read_shim / mfp_hub_iack - one implementation, all channels,
 * with the in-service gating this copy set but never enforced.) */

/* =================== pump: snapshot each commit ======================= */

int  dmasnd_is_repeat(void) { return atomic_load(&g_repeat); }

void dmasnd_pump(void)
{
    static unsigned stride;

    if (atomic_load(&g_enabled) && !atomic_load(&g_parked) &&
        !(++stride & 7u))
        frames_advance();
}

static void *pump_thread(void *arg)
{
    (void)arg;
    unsigned last_gen  = atomic_load(&g_gen);
    uint8_t  last_mode = 0xFF;
    uint64_t sum_t0    = now_ns();     /* =2 summary: 1 Hz reporter state */
    unsigned sum_gen   = last_gen;
    unsigned sum_g7    = atomic_load(&g_ct_g7);
    unsigned sum_rc    = atomic_load(&g_ct_rd_ctrl);
    unsigned sum_ra    = atomic_load(&g_ct_rd_cnt);
    unsigned sum_sg    = atomic_load(&g_ct_stage);
    unsigned sum_rp    = atomic_load(&g_ct_rep);
    unsigned sum_i2    = pistorm_ipl_lat2;
    unsigned sum_i4    = pistorm_ipl_lat4;
    unsigned sum_i6    = pistorm_ipl_lat6;

    while (atomic_load(&pump_run)) {
        /* PISTORM_DMASND_DEBUG=2: one summary line per second, printed
         * from this thread (free to syscall - never ipl_task). The 100ms
         * poll timeout below guarantees we pass here even when idle. */
        if (dmasnd_dbg_level() == 2) {
            uint64_t t = now_ns();
            if (t - sum_t0 >= 1000000000ull) {
                unsigned gen = atomic_load(&g_gen);
                unsigned g7  = atomic_load(&g_ct_g7);
                unsigned rc  = atomic_load(&g_ct_rd_ctrl);
                unsigned ra  = atomic_load(&g_ct_rd_cnt);
                unsigned sg  = atomic_load(&g_ct_stage);
                unsigned rp  = atomic_load(&g_ct_rep);
                unsigned i2  = pistorm_ipl_lat2;
                unsigned i4  = pistorm_ipl_lat4;
                unsigned i6  = pistorm_ipl_lat6;
                uint8_t  gp  = ps_read_8(0x00FFFA01u);  /* real GPIP */
                uint32_t s   = atomic_load(&g_act_s);
                uint32_t e   = atomic_load(&g_act_e);
                uint32_t bps = atomic_load(&g_act_bps);
                uint8_t  m   = reg[0x21];
                fprintf(stderr, "[dmasnd] SUM t=%llu.%03llus ev/s=%u "
                        "g7=%u rdc=%u rda=%u stg=%u rep=%u "
                        "i2=%u i4=%u i6=%u gp=%02X%s "
                        "act=%05X..%05X len=%u bps=%u dur=%uus "
                        "mode=0x%02X %s %uHz en=%d parked=%d ring=%u\n",
                        (unsigned long long)(t / 1000000000ull),
                        (unsigned long long)((t / 1000000ull) % 1000ull),
                        gen - sum_gen, g7 - sum_g7,
                        rc - sum_rc, ra - sum_ra, sg - sum_sg, rp - sum_rp,
                        i2 - sum_i2, i4 - sum_i4, i6 - sum_i6,
                        gp, (gp & 0x80u) ? "" : "<MONO!",
                        s, e, (e > s) ? e - s : 0, bps,
                        (e > s && bps) ?
                            (unsigned)((uint64_t)(e - s) * 1000000u / bps) : 0,
                        m, (m & 0x80) ? "mono" : "stereo", ste_rates[m & 3],
                        atomic_load(&g_enabled), atomic_load(&g_parked),
                        dmasnd_ring_used());
                sum_gen = gen;
                sum_g7  = g7;
                sum_rc  = rc;
                sum_ra  = ra;
                sum_sg  = sg;
                sum_rp  = rp;
                sum_i2  = i2;
                sum_i4  = i4;
                sum_i6  = i6;
                sum_t0  = t;
            }
        }
        unsigned gen = atomic_load_explicit(&g_gen, memory_order_acquire);
        if (gen != last_gen) {
            /* Frame became ACTIVE: copy it now. Captures are triggered at
               enable and at each frame BOUNDARY (frames_advance), matching
               real DMA order - NOT at stage time. A chaining player stages
               the next buffer's pointers while its mixer is still filling
               that buffer (hardware won't read it until the boundary, a
               full frame later); capturing at stage time reads half-mixed
               audio = glitches (field case: Paula). At the boundary the
               frame is complete, exactly like the real fetch. */
            /* one copy PER GENERATION, not per wakeup: frames_advance may
               process several overdue boundaries in one call (bumping the
               generation each time); copying once per wake silently DROPPED
               the missed frames (field-measured: 19KB/s pushed against a
               100KB/s DAC = starvation). For a re-triggered loop the extra
               copies are the same buffer again - exactly what real DMA
               would have fetched on each pass. */
            unsigned behind = gen - last_gen;
            if (behind > 64u) behind = 64u;    /* match the advance guard */
            last_gen = gen;
            if (natmem_offset) {
                uint32_t s = atomic_load(&g_act_s), e = atomic_load(&g_act_e);
                uint8_t  m = reg[0x21];
                if (e > s && (e - s) <= MAX_FRAME && e <= ST_RAM_SIZE) {
                    unsigned k;
                    if (m != last_mode) {
                        dmasnd_set_mode(ste_rates[m & 3], (m & 0x80) ? 0 : 1);
                        last_mode = m;
                    }
                    dmasnd_note_frame_len(e - s);
                    for (k = 0; k < behind; k++)
                        dmasnd_write_bytes(&natmem_offset[s], e - s);
                    if (dmasnd_dbg())
                        fprintf(stderr, "[dmasnd] accept s=0x%06X e=0x%06X len=%u x%u "
                                "%s %uHz\n", s, e, e - s, behind,
                                (m & 0x80) ? "mono" : "stereo", ste_rates[m & 3]);
                } else if (dmasnd_dbg()) {
                    fprintf(stderr, "[dmasnd] REJECT s=0x%06X e=0x%06X len=%d mode=0x%02X"
                            " (need e>s, len<=%u, e<=0x%X)\n",
                            s, e, (int)e - (int)s, m, MAX_FRAME, ST_RAM_SIZE);
                }
            }
        }
        /* Block until the next commit signals the eventfd. The 100 ms timeout
         * is a safety net (missed wakeup, shutdown responsiveness), not a
         * poll rate - the real wakeup is the write() in dmasnd_commit().
         * Without an eventfd (creation failed) fall back to the old poll. */
        if (pump_evfd >= 0) {
            struct pollfd pf = { .fd = pump_evfd, .events = POLLIN, .revents = 0 };
            if (poll(&pf, 1, 100) > 0 && (pf.revents & POLLIN)) {
                uint64_t n;
                ssize_t r = read(pump_evfd, &n, sizeof n);   /* clear counter */
                (void)r;
            }
        } else {
            usleep(PUMP_US);
        }
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

    /* PISTORM_DMASND_TONE=1: prove the host-side STE audio stream in
     * isolation. Pushes ~2s of 440Hz square through the exact path the
     * DMA capture uses (dmasnd_set_mode + dmasnd_write_bytes). Audible
     * tone = host stream fine, silence is capture-side; no tone = the
     * STE stream itself is broken and no capture fix can help. */
    {
        const char *e = getenv("PISTORM_DMASND_TONE");
        if (e && *e == '1') {
            static int8_t tone[25033 * 2];       /* 1s stereo @25033 */
            unsigned i;
            for (i = 0; i < sizeof tone; i += 2) {
                int8_t v = ((i / 2 / 28) & 1) ? 100 : -100;  /* ~447Hz */
                tone[i] = v; tone[i + 1] = v;
            }
            dmasnd_set_mode(25033, 1);
            dmasnd_write_bytes(tone, sizeof tone);
            dmasnd_write_bytes(tone, sizeof tone);
            fprintf(stderr, "[dmasnd] SELF-TEST: 2s tone queued on the STE stream\n");
        }
    }
    if (pump_evfd < 0) {
        pump_evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (pump_evfd < 0)
            fprintf(stderr, "[dmasnd] eventfd unavailable - pump falls back "
                            "to %dus polling\n", PUMP_US);
    }
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
    if (pump_evfd >= 0) {                /* wake the pump so join is instant */
        uint64_t one = 1;
        ssize_t r = write(pump_evfd, &one, sizeof one);
        (void)r;
    }
    pthread_join(pump_tid, NULL);
}

void dmasnd_capture_reset(void)
{
    dmasnd_lmc_reset();
    for (unsigned i = 0; i < sizeof reg; i++) reg[i] = 0;
    atomic_store(&g_enabled, 0);
    atomic_store(&g_gen, 0);
    atomic_store(&g_repeat, 0);
    atomic_store(&g_parked, 1);          /* frame clock idle            */
    atomic_store(&g_act_s, 0);
    atomic_store(&g_act_e, 0);
    mfp_hub_reset();                     /* no stale virtual interrupts */
    dmasnd_output_reset();
}
