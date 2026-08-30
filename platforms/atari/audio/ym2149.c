/*
 * ym2149.c — emulated YM2149 (PSG) -> HDMI via SDL3 (third stream).
 *
 * Built as C with -DPISTORM_REAL_SDL3 + `pkg-config sdl3 --cflags`, same as
 * dmasnd_hdmi.c. Core is emu2149 (Mitsutaka Okazaki, MIT — see emu2149-LICENSE).
 *
 * MODEL
 *   The guest's PSG writes are snooped on the cpu_task thread (they still go
 *   to the real chip on the motherboard). Each data write is timestamped
 *   (CLOCK_MONOTONIC) and pushed into a lock-free ring. The SDL3 get-callback
 *   drains the ring while generating samples, applying each write at the
 *   sample position matching its timestamp — so timer-driven effects
 *   (digidrums, SID-voice, buzzers) that hammer the volume registers at
 *   6-50 kHz land with ~sample accuracy instead of being quantised to the
 *   callback period.
 *
 * RATES
 *   The core runs at the YM's native step rate: 2 MHz / 8 = 250 kHz, mono
 *   S16 (emu2149's recommended high-accuracy configuration — its internal
 *   rate converter is bypassed because rate == clk/8 makes base_incr exactly
 *   one step per sample). SDL3's stream converter resamples 250 kHz -> device
 *   rate; at mono 250 kHz that is noise on a Pi 4.
 *
 * ENV
 *   PISTORM_YM=0          disable at startup
 *   PISTORM_YM_GAIN=x.y   stream gain (default 1.0, clamped 0..4)
 *   PISTORM_YM_LAG_MS=n   render-behind-realtime margin (default 20, 5..200)
 */

#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "emu2149.h"
#include "ym2149.h"
#include "dmasnd.h"          /* dmasnd_device_id() */

#define YM_CLOCK 2000000u    /* Atari ST PSG master clock */

/* Core sample rate. 250000 (clock/8) is one emu2149 step per sample -
 * the most faithful, and the original default - but it costs a QUARTER
 * MILLION PSG_calc() calls per second of audio, all inside the SDL
 * callback, plus a 250k->48k resample. That fits only while the
 * emulator leaves the Pi idle time; with the CPU unthrottled the
 * callback misses its deadlines and the sound stutters.
 *
 * emu2149 handles an arbitrary output rate internally (it accumulates
 * clock steps per sample), so a lower rate is not "wrong", just less
 * oversampled. 62500 is clock/32 - a quarter of the work, still 31 kHz
 * of bandwidth, well above anything the PSG produces that matters.
 * PISTORM_YM_RATE overrides (48000..250000). */
/* Default 250000 = clock/8, one emu2149 step per sample. With emu2149's
 * quality=0 (the only mode that works here, see PSG_setQuality below)
 * this is the ONLY exact rate: anything lower point-samples the PSG and
 * folds square-wave harmonics back as broadband hash. Lower rates are
 * a CPU-cost escape hatch, not a free choice. */
#define YM_RATE_DEFAULT 250000u
static uint32_t g_ym_rate = YM_RATE_DEFAULT;
static uint64_t g_step_ns = 1000000000ull / YM_RATE_DEFAULT;

/* ---- state ------------------------------------------------------------- */

static PSG             *g_psg = NULL;
static SDL_AudioStream *g_ym  = NULL;
static atomic_int       g_on  = 0;

/* register-select latch: written on the cpu_task thread only */
static uint8_t g_latch = 0;

/* timestamped write ring: producer = cpu_task thread, consumer = SDL audio
 * thread. Single producer / single consumer, so plain acquire/release on the
 * indices is enough. */
/* Depth = write rate x lag. Music that uses DIGIDRUMS drives the volume
 * registers at audio rate - measured at 27000 writes/sec, and with the
 * lag now covering two device periods (~125 ms of events in flight)
 * that is ~3400 entries in the ring. The old 4096 left 15% headroom, so
 * a burst overflowed and silently DROPPED writes, which is heard as
 * noise. 16384 entries is 128 KB and gives 4x margin. */
#define RING_SIZE 16384u                     /* power of two */
#define RING_MASK (RING_SIZE - 1u)
typedef struct { uint64_t t; uint8_t reg, val; } ym_ev;
static ym_ev        g_ring[RING_SIZE];
static atomic_uint  g_head = 0;              /* next slot to write  */
static atomic_uint  g_tail = 0;              /* next slot to read   */

/* consumer-side (audio thread) */
static uint64_t g_render_t = 0;              /* timestamp of next sample */
static int32_t  g_dc_acc   = 0;              /* DC-blocker integrator    */
static uint64_t g_lag_ns   = 20000000ull;    /* render this far behind now */
static int      g_oversample = 5;            /* core steps per output sample */
static int32_t  g_os_acc;                    /* box-filter accumulator      */
static int      g_os_n;

/* Stream level = user's fixed trim x the emulated LMC1992's current level. */
static float g_user_gain = 1.0f;
static float g_lmc_gain  = 1.0f;

static void ym_apply_gain(void)
{
    if (g_ym)
        SDL_SetAudioStreamGain(g_ym, g_user_gain * g_lmc_gain);
}

void ym2149_set_gain(float lmc_gain)
{
    if (lmc_gain < 0.0f) lmc_gain = 0.0f;
    if (lmc_gain > 1.0f) lmc_gain = 1.0f;
    g_lmc_gain = lmc_gain;
    ym_apply_gain();          /* no-op until the stream exists */
}

/* ---- diagnostics (PISTORM_YM_DEBUG=1) ----------------------------------
 * One line a second from the audio thread. What each number means:
 *   wr    PSG register writes snooped from the guest this second. Music
 *         driven from a 50 Hz VBL or a 200 Hz timer lands in the low
 *         hundreds; ~0 means the snoop is not seeing the guest at all.
 *   drop  writes lost because the ring was full (should always be 0).
 *   cb    audio-thread callbacks; frames rendered at the 250 kHz core.
 *   late  hard resyncs - the renderer fell more than lag+300ms behind
 *         wall clock and jumped. Any nonzero value here is audible and
 *         means the callback is not keeping up.
 * ------------------------------------------------------------------- */
static atomic_uint g_dbg_wr, g_dbg_drop, g_dbg_cb, g_dbg_late;
static atomic_uint g_dbg_reg[16];            /* writes per PSG register */
static atomic_uint g_dbg_r0max, g_dbg_r0sum, g_dbg_r0n; /* chA period */
static uint64_t    g_dbg_frames;

/* PISTORM_YM_SELFTEST=1: ignore the guest and hold a steady 440 Hz tone
 * on channel A. This splits the problem cleanly in two. If the tone is
 * clean, everything from PSG_calc through the DC blocker, SDL's
 * resampler and the HDMI sink is fine, and the fault is in the write
 * stream we capture from the guest (timestamps, ordering, a missed
 * select latch). If the tone itself is grubby, the guest is irrelevant
 * and the fault is in the synth or the output path. */
static int g_selftest;

static int ym_debug(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("PISTORM_YM_DEBUG");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- snoops (cpu_task thread) ------------------------------------------ */
/* ST decode: PSG answers on the upper data byte at even addresses in the
 * $FF88xx page, mirrored every 4 bytes. Offset bit1 = 0 -> select (also the
 * read port, but reads come from the real chip), bit1 = 1 -> data write. */

void ym2149_snoop8(uint32_t addr, uint8_t val)
{
    if (!atomic_load_explicit(&g_on, memory_order_relaxed))
        return;
    uint32_t a = addr & 0x00FFFFFFu;
    if (a < 0x00FF8800u || a > 0x00FF88FFu || (a & 1u))
        return;                              /* odd byte: PSG not connected */
    if ((a & 2u) == 0) {
        g_latch = val & 0x0F;                /* YM2149 decodes 4 addr bits */
        return;
    }
    if (g_selftest)
        return;                              /* bench tone owns the PSG */

    /* data write to the latched register */
    unsigned h = atomic_load_explicit(&g_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&g_tail, memory_order_acquire);
    if (h - t >= RING_SIZE) {
        atomic_fetch_add_explicit(&g_dbg_drop, 1, memory_order_relaxed);
        return;                              /* full — drop (never in practice) */
    }
    atomic_fetch_add_explicit(&g_dbg_wr, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_dbg_reg[g_latch & 0x0F], 1,
                              memory_order_relaxed);
    if ((g_latch & 0x0F) <= 5) {             /* tone period registers */
        unsigned cur = atomic_load_explicit(&g_dbg_r0max, memory_order_relaxed);
        if (g_latch <= 1 && val > cur)
            atomic_store_explicit(&g_dbg_r0max, val, memory_order_relaxed);
        if (g_latch <= 1) {
            atomic_fetch_add_explicit(&g_dbg_r0sum, val, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_dbg_r0n, 1, memory_order_relaxed);
        }
    }
    g_ring[h & RING_MASK].t   = now_ns();
    g_ring[h & RING_MASK].reg = g_latch;
    g_ring[h & RING_MASK].val = val;
    atomic_store_explicit(&g_head, h + 1, memory_order_release);
}

void ym2149_snoop16(uint32_t addr, uint16_t val)
{
    /* word write: the PSG sees the upper byte at the even address */
    ym2149_snoop8(addr, (uint8_t)(val >> 8));
}

void ym2149_snoop32(uint32_t addr, uint32_t val)
{
    /* long write = two word cycles: select at a, data at a+2
     * (the classic move.l #$RR00VV00,$FF8800.w trick) */
    ym2149_snoop8(addr,     (uint8_t)(val >> 24));
    ym2149_snoop8(addr + 2, (uint8_t)(val >> 8));
}

/* The guest's current PSG select latch, tracked by the snoop above. The
 * real-FDC errand pump (stbox_realfdc.c) restores it after temporarily
 * selecting reg 14 for drive/side control - both run on the CPU thread,
 * so this value is exact at any block boundary. */
uint8_t ym2149_selected_reg(void) { return g_latch; }

/* ---- render (SDL audio thread) ----------------------------------------- */

static void SDLCALL ym_feed_cb(void *ud, SDL_AudioStream *stream,
                               int additional, int total)
{
    (void)ud; (void)total;
    if (!g_psg || additional <= 0)
        return;
    /* Keep the render clock ~lag behind wall time so every event we render
     * already carries its final timestamp; hard-resync on gross drift
     * (startup, underrun, suspend). */
    uint64_t now = now_ns();
    if (g_render_t == 0 || g_render_t > now ||
        now - g_render_t > g_lag_ns + 300000000ull) {
        if (g_render_t)
            atomic_fetch_add_explicit(&g_dbg_late, 1, memory_order_relaxed);
        g_render_t = (now > g_lag_ns) ? now - g_lag_ns : 0;
    }

    /* How much to render: the amount of CORE-RATE audio that wall time
     * has advanced by, NOT `additional`.
     *
     * `additional` is a byte count SDL derives from the device buffer,
     * and it does not scale with our source rate - it came out as a
     * flat 2048 frames per callback whatever the core ran at. Feeding
     * 2048 samples per callback (49152/s at 24 callbacks/s) into a
     * stream that consumes at the core rate starves it by the ratio
     * between them, which is heard as stutter. Rendering to the clock
     * keeps exactly one second of core audio produced per second of
     * wall time, at any rate and any buffer size. Cap a single burst at
     * 250 ms so a delayed callback cannot spin here forever. */
    int frames = 0;
    {
        uint64_t target = (now > g_lag_ns) ? now - g_lag_ns : 0;
        if (target > g_render_t) {
            uint64_t n = (target - g_render_t) / g_step_ns;
            uint64_t cap = g_ym_rate / 4;
            if (n > cap) n = cap;
            frames = (int)n;
        }
    }
    if (ym_debug()) {
        static uint64_t next_report;
        atomic_fetch_add_explicit(&g_dbg_cb, 1, memory_order_relaxed);
        g_dbg_frames += (uint64_t)frames;
        if (!next_report)
            next_report = now + 1000000000ull;
        else if (now >= next_report) {
            unsigned wr   = atomic_exchange(&g_dbg_wr, 0);
            unsigned drop = atomic_exchange(&g_dbg_drop, 0);
            unsigned cb   = atomic_exchange(&g_dbg_cb, 0);
            unsigned late = atomic_exchange(&g_dbg_late, 0);
            fprintf(stderr, "[ym2149] wr=%u drop=%u cb=%u frames=%llu late=%u "
                    "backlog=%u\n", wr, drop, cb,
                    (unsigned long long)g_dbg_frames, late,
                    atomic_load(&g_head) - atomic_load(&g_tail));
            /* Which registers the guest is driving. Sane chip music is
             * mostly r0-r5 (tone) and r8-r10 (volume); digidrums hammer
             * r8-r10 at audio rate. A big count on r7 (mixer) or r6
             * (noise period) is the signature of writes landing in the
             * WRONG register - i.e. our select latch is out of step with
             * the guest - and enabling noise is exactly what "white
             * noise added to the music" sounds like. */
            fprintf(stderr, "[ym2149] reg:");
            for (int r = 0; r < 16; r++) {
                unsigned c = atomic_exchange(&g_dbg_reg[r], 0);
                if (c)
                    fprintf(stderr, " r%d=%u", r, c);
            }
            {
                /* Channel A tone period. The SID-voice/digidrum trick
                 * drives this to 0-5, putting the carrier above 24 kHz
                 * where the real chip is inaudible but a point-sampled
                 * emulation aliases it back as hash. Musical periods are
                 * in the hundreds. This tells the two apart. */
                unsigned mx = atomic_exchange(&g_dbg_r0max, 0);
                unsigned sm = atomic_exchange(&g_dbg_r0sum, 0);
                unsigned n  = atomic_exchange(&g_dbg_r0n, 0);
                if (n)
                    fprintf(stderr, " | chA period max=%u avg=%u", mx, sm / n);
            }
            fprintf(stderr, "\n");
            g_dbg_frames = 0;
            next_report = now + 1000000000ull;
        }
    }

    if (frames <= 0)
        return;                       /* clock has not advanced a sample */

    unsigned t_idx = atomic_load_explicit(&g_tail, memory_order_relaxed);
    unsigned h_idx = atomic_load_explicit(&g_head, memory_order_acquire);

    int16_t buf[1024];
    int n = 0;
    while (frames-- > 0) {
        /* apply every write that is due at this sample position */
        while (t_idx != h_idx && g_ring[t_idx & RING_MASK].t <= g_render_t) {
            PSG_writeReg(g_psg, g_ring[t_idx & RING_MASK].reg,
                                g_ring[t_idx & RING_MASK].val);
            t_idx++;
        }
        /* Oversample and average. The PSG steps at the core rate; we
         * emit one sample per g_oversample steps, averaging them.
         *
         * This is the anti-aliasing that was missing. A digidrum player
         * parks a channel's tone period at 0-5, so the carrier sits at
         * ~125 kHz and the volume writes are the sample data. Emitting
         * every core step hands SDL a 125 kHz square sitting right at
         * the 250 kHz Nyquist, which folds down into the audible band
         * as hash. Averaging is a box filter: the carrier integrates
         * away to its mean, the volume envelope - the actual music -
         * comes through untouched. Muting the channel (freq_limit)
         * would remove the music along with the carrier. */
        g_os_acc += PSG_calc(g_psg);
        if (++g_os_n >= g_oversample) {
            int32_t s = g_os_acc / g_os_n;
            g_os_acc = 0;
            g_os_n = 0;
            /* one-pole DC blocker: the YM output is unipolar */
            g_dc_acc += ((s << 12) - g_dc_acc) >> 12;
            s -= g_dc_acc >> 12;
            buf[n++] = (int16_t)s;
            if (n == (int)(sizeof(buf) / sizeof(buf[0]))) {
                SDL_PutAudioStreamData(stream, buf, n * (int)sizeof(int16_t));
                n = 0;
            }
        }
        g_render_t += g_step_ns;
    }
    if (n)
        SDL_PutAudioStreamData(stream, buf, n * (int)sizeof(int16_t));
    atomic_store_explicit(&g_tail, t_idx, memory_order_release);
}

/* ---- lifecycle (main thread) ------------------------------------------- */

int ym2149_active(void) { return atomic_load(&g_on); }

int ym2149_init(void)
{
    const char *e = getenv("PISTORM_YM");
    if (e && *e == '0')
        return 1;                            /* explicitly disabled */

    SDL_AudioDeviceID dev = (SDL_AudioDeviceID)dmasnd_device_id();
    if (!dev) {
        fprintf(stderr, "[ym2149] no SDL audio device (dmasnd not running)\n");
        return -1;
    }

    {
        const char *r = getenv("PISTORM_YM_RATE");
        long v = r ? atol(r) : (long)YM_RATE_DEFAULT;
        if (v < 48000)  v = 48000;
        if (v > 250000) v = 250000;
        g_ym_rate = (uint32_t)v;
        g_step_ns = 1000000000ull / g_ym_rate;
    }

    g_psg = PSG_new(YM_CLOCK, g_ym_rate);
    if (!g_psg)
        return -1;
    PSG_setVolumeMode(g_psg, 1);             /* YM2149 32-step DAC table */

    /* Anti-aliasing. emu2149 defaults to quality=0, which advances the
     * PSG by (clock/8)/rate steps and samples the output ONCE - point
     * sampling. That is exact only when rate == clock/8 (250 kHz), the
     * rate this code originally used; at anything lower the square-wave
     * harmonics fold back as broadband hash, which is heard as white
     * noise added to the music. quality=1 runs the PSG at the true
     * master clock and averages down to the output rate, so it is right
     * at any rate - and at 62.5 kHz it costs about what quality=0 cost
     * at 250 kHz, while saving SDL the 250k->48k resample.
     * OFF BY DEFAULT: enabling it silenced the HDMI output entirely on
     * this build, so the vendored emu2149's quality path is not usable
     * as-is and needs investigating before it can be trusted. Opt in
     * with PISTORM_YM_QUALITY=1 to experiment. */
    {
        const char *q = getenv("PISTORM_YM_QUALITY");
        if (q && *q == '1')
            PSG_setQuality(g_psg, 1);
    }

    /* Ultrasonic-tone guard.
     *
     * The "SID voice" technique drives a channel's tone period down to
     * a couple of counts so the carrier is ~125 kHz - inaudible on real
     * hardware, and filtered away by the ST's analogue output - and then
     * uses the VOLUME register as sample data. The register trace of a
     * digidrum player shows exactly this: r0 written at the same audio
     * rate as r8/r9/r10.
     *
     * emu2149 has a guard for it (update_output mutes a tone-only
     * channel whose period is at or below freq_limit) but internal_refresh
     * only sets freq_limit when quality==1; at quality 0 it is zeroed, so
     * the carrier is point-sampled and aliases back down as broadband
     * hash over the music. Size the limit to the DEVICE rate, not our
     * core rate: anything above the 24 kHz Nyquist of the 48 kHz output
     * cannot be represented. period <= clock/16/24000 = 5.
     *
     * MUTING IS THE WRONG ANSWER, though, and proving it cost a build:
     * setting freq_limit=5 silenced the machine completely, because
     * those ultrasonic channels are precisely the ones carrying the
     * digidrum samples - the volume writes ARE the audio. Killing the
     * channel kills the music. The carrier has to be FILTERED OUT while
     * the volume envelope is kept, which is what the oversample-and-
     * average below does. Default 0 = guard off;
     * PISTORM_YM_FREQLIMIT sets it if you want to experiment. */
    {
        const char *f = getenv("PISTORM_YM_FREQLIMIT");
        g_psg->freq_limit = (f && *f) ? (uint32_t)atol(f) : 0;
    }
    PSG_reset(g_psg);

    SDL_AudioSpec dst;
    if (!SDL_GetAudioDeviceFormat(dev, &dst, NULL)) {
        dst.format = SDL_AUDIO_S16LE; dst.channels = 2; dst.freq = 48000;
    }
    SDL_AudioSpec src;
    src.format   = SDL_AUDIO_S16;            /* native-endian S16, mono */
    src.channels = 1;
    {   /* one emitted sample per g_oversample core steps (see ym_feed_cb).
         * 250000/5 = 50 kHz out, comfortably above the 48 kHz sink, with
         * the ultrasonic digidrum carrier averaged away rather than
         * folded down. PISTORM_YM_OVERSAMPLE=1 restores the old
         * point-sampled behaviour for comparison. */
        const char *o = getenv("PISTORM_YM_OVERSAMPLE");
        int os = o && *o ? atoi(o) : 5;
        if (os < 1)  os = 1;
        if (os > 16) os = 16;
        g_oversample = os;
    }
    src.freq     = (int)(g_ym_rate / (uint32_t)g_oversample);
    g_ym = SDL_CreateAudioStream(&src, &dst);
    if (!g_ym) {
        fprintf(stderr, "[ym2149] CreateAudioStream: %s\n", SDL_GetError());
        PSG_delete(g_psg); g_psg = NULL;
        return -1;
    }

    {   /* fixed trim (relative level vs the STE DMA stream) */
        const char *g = getenv("PISTORM_YM_GAIN");
        float gain = g ? (float)atof(g) : 1.0f;
        if (gain < 0.0f) gain = 0.0f;
        if (gain > 4.0f) gain = 4.0f;
        g_user_gain = gain;
        ym_apply_gain();      /* folds in whatever the LMC shadow already holds */
    }
    {
        const char *l = getenv("PISTORM_YM_LAG_MS");
        long ms = l ? atol(l) : 20;
        if (ms < 5) ms = 5;
        if (ms > 200) ms = 200;
        g_lag_ns = (uint64_t)ms * 1000000ull;

        /* The lag must cover the DEVICE PERIOD, or we are permanently
         * behind the consumer: the render clock only ever runs `lag`
         * ahead, but SDL asks for a whole buffer at a time. The default
         * 20 ms was tuned when the device buffer was SDL's 1024 frames
         * (21 ms); dmasnd now asks for 2048 (43 ms), so 20 ms cannot
         * fill one gulp - which is heard as stutter. Take two periods
         * plus the requested lag. */
        int dev_frames = 0;
        SDL_AudioSpec dspec;
        if (SDL_GetAudioDeviceFormat(dev, &dspec, &dev_frames) &&
            dspec.freq > 0 && dev_frames > 0) {
            uint64_t period_ns = (uint64_t)dev_frames * 1000000000ull /
                                 (uint64_t)dspec.freq;
            uint64_t need = period_ns * 2;
            if (g_lag_ns < need) {
                fprintf(stderr, "[ym2149] lag %llu ms -> %llu ms "
                        "(device period %llu ms x2)\n",
                        (unsigned long long)(g_lag_ns / 1000000ull),
                        (unsigned long long)(need / 1000000ull),
                        (unsigned long long)(period_ns / 1000000ull));
                g_lag_ns = need;
            }
        }
    }

    SDL_SetAudioStreamGetCallback(g_ym, ym_feed_cb, NULL);
    if (!SDL_BindAudioStream(dev, g_ym)) {
        fprintf(stderr, "[ym2149] BindAudioStream: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(g_ym); g_ym = NULL;
        PSG_delete(g_psg); g_psg = NULL;
        return -1;
    }

    {
        const char *s = getenv("PISTORM_YM_SELFTEST");
        if (s && *s == '1') {
            /* period = clock/16/freq: 2000000/16/440 = 284 = $11C */
            PSG_writeReg(g_psg, 0, 0x1C);    /* channel A fine   */
            PSG_writeReg(g_psg, 1, 0x01);    /* channel A coarse */
            PSG_writeReg(g_psg, 7, 0x3E);    /* tone A only, no noise */
            PSG_writeReg(g_psg, 8, 12);      /* channel A volume */
            g_selftest = 1;
            fprintf(stderr, "[ym2149] SELFTEST: holding 440 Hz on channel A, "
                            "guest writes ignored\n");
        }
    }

    atomic_store(&g_on, 1);
    fprintf(stderr, "[ym2149] emulated PSG -> HDMI ready (emu2149, %u Hz core, "
                    "/%d -> %u Hz out)\n",
            g_ym_rate, g_oversample, g_ym_rate / (uint32_t)g_oversample);
    return 0;
}

void ym2149_reset(void)
{
    if (!g_ym)
        return;
    SDL_LockAudioStream(g_ym);               /* serialise with ym_feed_cb */
    atomic_store(&g_tail, atomic_load(&g_head));   /* drop queued writes */
    PSG_reset(g_psg);
    if (g_selftest) {                        /* keep the bench tone alive */
        PSG_writeReg(g_psg, 0, 0x1C);
        PSG_writeReg(g_psg, 1, 0x01);
        PSG_writeReg(g_psg, 7, 0x3E);
        PSG_writeReg(g_psg, 8, 12);
    }
    g_latch    = 0;
    g_dc_acc   = 0;
    g_render_t = 0;
    SDL_UnlockAudioStream(g_ym);
    SDL_ClearAudioStream(g_ym);              /* drop already-buffered PCM */
}

void ym2149_close(void)
{
    atomic_store(&g_on, 0);
    if (g_ym) {
        /* Unbind first: takes the device lock, so the callback is not running
         * when the core is torn down underneath it (same as the MP3 path). */
        SDL_UnbindAudioStream(g_ym);
        SDL_DestroyAudioStream(g_ym);
        g_ym = NULL;
    }
    if (g_psg) {
        PSG_delete(g_psg);
        g_psg = NULL;
    }
}
