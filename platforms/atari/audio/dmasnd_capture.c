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
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <sys/eventfd.h>
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
static atomic_uint g_vipra;          /* virtual MFP pending bits (irq side)   */
static atomic_uint g_visra;          /* virtual MFP in-service bits           */

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
        return (uint8_t)((reg[0x01] & 0x02) | (playing_now() ? 0x01 : 0x00));
    case 0x09: c = cur_counter(); return (uint8_t)(c >> 16);
    case 0x0B: c = cur_counter(); return (uint8_t)(c >> 8);
    case 0x0D: c = cur_counter(); return (uint8_t)c;
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

static atomic_uint g_mfp_iera = 0;
static atomic_uint g_mfp_imra = 0;
static atomic_uint g_mfp_vr   = 0x40;    /* TOS/EmuTOS default base */
static atomic_uint g_mfp_tacr = 0;
static atomic_uint g_mfp_tadr = 256;     /* TADR: 0 means 256 */
static atomic_uint g_ta_count = 256;     /* virtual Timer A main counter */
static atomic_int  g_irq_ta = 0;         /* pending: Timer A (ch 13) */
static atomic_int  g_irq_g7 = 0;         /* pending: GPIP7   (ch 15) */

void dmasnd_mfp_snoop(uint32_t addr, uint32_t value, int is_word)
{
    uint32_t a = addr & 0x00FFFFFFu;
    uint8_t  b = (uint8_t)(value & 0xFF); /* low byte lands on odd addr */

    if (is_word)
        a |= 1;
    switch (a) {
    case 0x00FFFA07u: atomic_store(&g_mfp_iera, b); break;
    case 0x00FFFA0Bu: atomic_fetch_and(&g_vipra, b); break;  /* IPRA: 0 clears */
    case 0x00FFFA0Fu: atomic_fetch_and(&g_visra, b); break;  /* ISRA: 0 clears */
    case 0x00FFFA13u: atomic_store(&g_mfp_imra, b); break;
    case 0x00FFFA17u: atomic_store(&g_mfp_vr,   b); break;
    case 0x00FFFA19u:                    /* TACR: reprogram re-arms count */
        atomic_store(&g_mfp_tacr, b & 0x0Fu);
        atomic_store(&g_ta_count, atomic_load(&g_mfp_tadr));
        break;
    case 0x00FFFA1Fu:                    /* TADR: 0 counts as 256 */
        atomic_store(&g_mfp_tadr, b ? b : 256u);
        atomic_store(&g_ta_count, b ? b : 256u);
        break;
    default: break;
    }
}


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
    if (m == 2)
        return atomic_load(&g_g7_app);
    return m;
}

/* one frame boundary: fire the enabled synthesized channels */
static void frame_end_event(unsigned frameno)
{
    uint8_t en = (uint8_t)(atomic_load(&g_mfp_iera) & atomic_load(&g_mfp_imra));

    if ((en & 0x20u) && atomic_load(&g_mfp_tacr) == 8u) {
        unsigned c = atomic_load(&g_ta_count);       /* event count mode */
        if (c <= 1u) {
            atomic_store(&g_ta_count, atomic_load(&g_mfp_tadr));
            atomic_store(&g_irq_ta, 1);
            atomic_fetch_or(&g_vipra, 0x20u);        /* pending: ch 13 */
        } else
            atomic_store(&g_ta_count, c - 1u);
    }
    if ((en & 0x80u) && g7_deliver()) {
        atomic_store(&g_irq_g7, 1);
        atomic_fetch_or(&g_vipra, 0x80u);            /* pending: ch 15 */
    }
    if (dmasnd_dbg()) {
        /* uncapped and self-describing: length, bps and the computed
         * frame duration make clock errors directly measurable */
        uint32_t s2 = atomic_load(&g_act_s), e2 = atomic_load(&g_act_e);
        uint32_t b2 = atomic_load(&g_act_bps);
        fprintf(stderr, "[dmasnd] frame#%u act=%05X..%05X len=%u bps=%u "
                "dur=%uus ta=%d g7=%d tacnt=%u\n",
                frameno, s2, e2, e2 - s2, b2,
                (unsigned)((uint64_t)(e2 - s2) * 1000000u / (b2 ? b2 : 1)),
                atomic_load(&g_irq_ta), atomic_load(&g_irq_g7),
                atomic_load(&g_ta_count));
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
        t0 += dur;
        /* frame boundary: latch the STAGED registers (STE hardware
         * double-buffering - this is where chain writes take effect) */
        s = start_addr();
        e = end_addr();
        bps = frame_bps();
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

/* observer: ipl_task. Cheap when idle; one clock_gettime while playing. */
int dmasnd_irq_wanted(void)
{
    if (atomic_load(&g_irq_ta) || atomic_load(&g_irq_g7))
        return 1;
    if (!atomic_load(&g_enabled) || atomic_load(&g_parked))
        return 0;
    frames_advance();
    return atomic_load(&g_irq_ta) || atomic_load(&g_irq_g7);
}

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
        return (uint8_t)(real ^ 0x80u);
    return real;
}

/* Virtual IPRA/ISRA bits for the synthesized channels (bit7 = GPIP7,
 * bit5 = Timer A). A real MFP latches the pending bit when the event
 * arrives and - in software-EOI mode (VR bit 3) - sets the in-service
 * bit during the IACK; handlers identify their interrupt by exactly
 * these bits and clear ISRA before RTE. Our virtual interrupts leave
 * the real MFP blank, so any handler that introspects it concludes
 * "no interrupt happened" and chains to the old vector (field case:
 * Paula -> OS monitor handler -> reset). These shadows are OR-ed into
 * guest reads of IPRA/ISRA and cleared by the guest's own writes
 * (MFP semantics: written 0 bits clear, 1 bits leave). */

uint8_t dmasnd_mfp_read_shim(uint32_t addr, uint8_t real)
{
    switch (addr & 0x00FFFFFFu) {
    case 0x00FFFA01u: return dmasnd_gpip_shim(real);
    case 0x00FFFA0Bu: return (uint8_t)(real | atomic_load(&g_vipra));
    case 0x00FFFA0Fu: return (uint8_t)(real | atomic_load(&g_visra));
    default:          return real;
    }
}

/* consumer: virtual IACK (CPU thread). GPIP7 is the higher MFP channel. */
uint8_t dmasnd_iack_vector(void)
{
    uint8_t base = (uint8_t)(atomic_load(&g_mfp_vr) & 0xF0u);
    uint8_t vec;

    if (atomic_load(&g_irq_g7)) {
        atomic_store(&g_irq_g7, 0);
        atomic_fetch_and(&g_vipra, ~0x80u);          /* pending -> taken   */
        if (atomic_load(&g_mfp_vr) & 0x08u)
            atomic_fetch_or(&g_visra, 0x80u);        /* software-EOI mode  */
        vec = (uint8_t)(base | 15u);
    } else {
        atomic_store(&g_irq_ta, 0);
        atomic_fetch_and(&g_vipra, ~0x20u);
        if (atomic_load(&g_mfp_vr) & 0x08u)
            atomic_fetch_or(&g_visra, 0x20u);
        vec = (uint8_t)(base | 13u);
    }
    if (dmasnd_dbg()) {
        static int logged = 0;
        if (logged < 12) {
            logged++;
            fprintf(stderr, "[dmasnd] IACK -> vector 0x%02X (table @0x%X)\n",
                    vec, (unsigned)vec * 4u);
        }
        /* one-shot: dump the handler the guest will jump to, so a failing
         * handler can be disassembled from the log instead of guessed at */
        if (logged == 1 && natmem_offset) {
            const uint8_t *t = natmem_offset + (unsigned)vec * 4u;
            uint32_t h = ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) |
                         ((uint32_t)t[2] << 8)  |  (uint32_t)t[3];
            if (h >= 0x40u && h < ST_RAM_SIZE - 256u) {
                /* start 0x40 BEFORE the handler: the busy-guard's failure
                 * branch and the saved old-vector cell live there */
                const uint8_t *p = natmem_offset + (h - 0x40u);
                int i;
                fprintf(stderr, "[dmasnd] handler-0x40 @0x%06X:", h - 0x40u);
                for (i = 0; i < 320; i++)
                    fprintf(stderr, "%s%02X", (i & 15) ? "" :
                            "\n[dmasnd]   ", p[i]);
                fprintf(stderr, "\n");
            }
        }
    }
    return vec;
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
            if (behind > 8u) behind = 8u;      /* cap pathological deficits */
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
    atomic_store(&g_irq_ta, 0);          /* no stale virtual interrupts */
    atomic_store(&g_irq_g7, 0);
    atomic_store(&g_vipra, 0);
    atomic_store(&g_visra, 0);
    dmasnd_output_reset();
}
