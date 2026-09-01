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
 *
 * DIAGNOSTIC TAPS (for the HDMI quality investigation - measure, don't guess)
 *   PISTORM_YM_TAP=path    dump the exact PCM handed to SDL: raw S16LE mono
 *                          250000 Hz. Play with:
 *                            ffplay -f s16le -ar 250000 -ch_layout mono path
 *                          This is the pipeline's midpoint: a clean tap with
 *                          bad HDMI output puts the fault in SDL's resampler
 *                          or the device; a dirty tap puts it in the register
 *                          timing or the emu2149 core.
 *   PISTORM_YM_EVLOG=path  binary log of register writes AS APPLIED by the
 *                          renderer: {u64 t_ns, u64 sample_idx, u8 reg,
 *                          u8 val, u16 pad} x N. Lets the same write stream
 *                          be re-rendered offline and compared to the tap.
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
#define YM_RATE  250000u     /* clock/8: one emu2149 step per sample */
#define STEP_NS  (1000000000ull / YM_RATE)   /* 4000 ns/sample */

/* ---- state ------------------------------------------------------------- */

static PSG             *g_psg = NULL;
static SDL_AudioStream *g_ym  = NULL;
static atomic_int       g_on  = 0;

/* register-select latch: written on the cpu_task thread only */
static uint8_t g_latch = 0;

/* timestamped write ring: producer = cpu_task thread, consumer = SDL audio
 * thread. Single producer / single consumer, so plain acquire/release on the
 * indices is enough. */
#define RING_SIZE 4096u                      /* power of two */
#define RING_MASK (RING_SIZE - 1u)
typedef struct { uint64_t t; uint8_t reg, val; } ym_ev;
static ym_ev        g_ring[RING_SIZE];
static atomic_uint  g_head = 0;              /* next slot to write  */
static atomic_uint  g_tail = 0;              /* next slot to read   */

/* consumer-side (audio thread) */
static uint64_t g_render_t = 0;              /* timestamp of next sample */
static int32_t  g_dc_acc   = 0;              /* DC-blocker integrator    */
static uint64_t g_lag_ns   = 20000000ull;    /* render this far behind now */

/* Stream level = user's fixed trim x the emulated LMC1992's current level. */
static float g_user_gain = 1.0f;
static float g_lmc_gain  = 1.0f;

/* diagnostic taps (audio thread only; buffered, NULL when disarmed) */
static FILE    *g_tap   = NULL;              /* raw S16LE mono 250 kHz  */
static FILE    *g_evlog = NULL;              /* applied register writes */
static uint64_t g_sample_idx = 0;            /* samples rendered so far */
typedef struct { uint64_t t, sample; uint8_t reg, val; uint16_t pad; } ym_evrec;

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
    /* data write to the latched register */
    unsigned h = atomic_load_explicit(&g_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&g_tail, memory_order_acquire);
    if (h - t >= RING_SIZE)
        return;                              /* full — drop (never in practice) */
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
    int frames = (additional + (int)sizeof(int16_t) - 1) / (int)sizeof(int16_t);

    /* Keep the render clock ~lag behind wall time so every event we render
     * already carries its final timestamp.
     *
     * MEASURED (ym.pcm/ym.ev tap, 97s digidrum capture): the original
     * hard-resync (g_render_t > now -> slam back by lag) fought SDL's
     * queue refill in a ~1 Hz limit cycle - +19 ms rewind, then ~4
     * catch-up steps of -5.4 ms - warping register-event application by
     * +-5..20 ms continuously. On a 4 kHz volume-write stream that IS
     * the audible flutter/stutter. Fine jitter between lurches was
     * 0.047 ms, so only the clock discipline was at fault.
     *
     * So: never jump the clock (except first call / >1s gross desync,
     * e.g. suspend). Slew toward (now - lag) by at most 0.5% of this
     * callback's span - drift between CLOCK_MONOTONIC and the device
     * crystal becomes an inaudible rate bias instead of a lurch. */
    uint64_t now = now_ns();
    uint64_t target = (now > g_lag_ns) ? now - g_lag_ns : 0;
    if (g_render_t == 0 ||
        (g_render_t > target ? g_render_t - target
                             : target - g_render_t) > 1000000000ull) {
        g_render_t = target;
    } else {
        int64_t diff = (int64_t)target - (int64_t)g_render_t;
        int64_t max_slew = (int64_t)((uint64_t)frames * STEP_NS) / 200;
        if (diff >  max_slew) diff =  max_slew;
        if (diff < -max_slew) diff = -max_slew;
        g_render_t += diff;
    }

    unsigned t_idx = atomic_load_explicit(&g_tail, memory_order_relaxed);
    unsigned h_idx = atomic_load_explicit(&g_head, memory_order_acquire);

    int16_t buf[1024];
    int n = 0;
    while (frames-- > 0) {
        /* apply every write that is due at this sample position */
        while (t_idx != h_idx && g_ring[t_idx & RING_MASK].t <= g_render_t) {
            PSG_writeReg(g_psg, g_ring[t_idx & RING_MASK].reg,
                                g_ring[t_idx & RING_MASK].val);
            if (g_evlog) {
                ym_evrec r = { g_ring[t_idx & RING_MASK].t, g_sample_idx,
                               g_ring[t_idx & RING_MASK].reg,
                               g_ring[t_idx & RING_MASK].val, 0 };
                fwrite(&r, sizeof r, 1, g_evlog);
            }
            t_idx++;
        }
        int32_t s = PSG_calc(g_psg);
        /* one-pole DC blocker (~10 Hz at 250 kHz): the YM output is unipolar */
        g_dc_acc += ((s << 12) - g_dc_acc) >> 12;
        s -= g_dc_acc >> 12;
        buf[n++] = (int16_t)s;
        g_sample_idx++;
        if (n == (int)(sizeof(buf) / sizeof(buf[0]))) {
            SDL_PutAudioStreamData(stream, buf, n * (int)sizeof(int16_t));
            if (g_tap)
                fwrite(buf, sizeof(int16_t), (size_t)n, g_tap);
            n = 0;
        }
        g_render_t += STEP_NS;
    }
    if (n) {
        SDL_PutAudioStreamData(stream, buf, n * (int)sizeof(int16_t));
        if (g_tap)
            fwrite(buf, sizeof(int16_t), (size_t)n, g_tap);
    }
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

    g_psg = PSG_new(YM_CLOCK, YM_RATE);
    if (!g_psg)
        return -1;
    PSG_setVolumeMode(g_psg, 1);             /* YM2149 32-step DAC table */
    PSG_reset(g_psg);

    SDL_AudioSpec dst;
    if (!SDL_GetAudioDeviceFormat(dev, &dst, NULL)) {
        dst.format = SDL_AUDIO_S16LE; dst.channels = 2; dst.freq = 48000;
    }
    SDL_AudioSpec src;
    src.format   = SDL_AUDIO_S16;            /* native-endian S16, mono */
    src.channels = 1;
    src.freq     = (int)YM_RATE;
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
        /* Default raised 20 -> 100ms. MEASURED (tap + evlog): SDL's pull
         * cadence bursts up to ~25ms ahead of real time in a ~1Hz limit
         * cycle; any burst that outruns the render-behind margin applies
         * pending register events early/bunched = audible flutter on
         * digidrum-class streams. 20ms was inside the burst; field
         * results: 50ms usable, 100ms clean. Cost is fixed audio
         * latency, fine for music; PISTORM_YM_LAG_MS tunes it down for
         * latency-sensitive use. */
        const char *l = getenv("PISTORM_YM_LAG_MS");
        long ms = l ? atol(l) : 100;
        if (ms < 5) ms = 5;
        if (ms > 200) ms = 200;
        g_lag_ns = (uint64_t)ms * 1000000ull;
    }
    {   /* diagnostic taps: big stdio buffers so the audio-thread fwrite is
         * a memcpy; the kernel flush happens on fclose or buffer-full. At
         * 500 KB/s (250 kHz S16) a 4 MB buffer flushes every ~8 s. */
        const char *tp = getenv("PISTORM_YM_TAP");
        const char *ev = getenv("PISTORM_YM_EVLOG");
        if (tp && *tp) {
            g_tap = fopen(tp, "wb");
            if (g_tap)
                setvbuf(g_tap, NULL, _IOFBF, 4u << 20);
            fprintf(stderr, "[ym2149] TAP %s: %s (S16LE mono 250000 Hz)\n",
                    tp, g_tap ? "armed" : "OPEN FAILED");
        }
        if (ev && *ev) {
            g_evlog = fopen(ev, "wb");
            if (g_evlog)
                setvbuf(g_evlog, NULL, _IOFBF, 1u << 20);
            fprintf(stderr, "[ym2149] EVLOG %s: %s\n",
                    ev, g_evlog ? "armed" : "OPEN FAILED");
        }
    }

    SDL_SetAudioStreamGetCallback(g_ym, ym_feed_cb, NULL);
    if (!SDL_BindAudioStream(dev, g_ym)) {
        fprintf(stderr, "[ym2149] BindAudioStream: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(g_ym); g_ym = NULL;
        PSG_delete(g_psg); g_psg = NULL;
        return -1;
    }

    atomic_store(&g_on, 1);
    fprintf(stderr, "[ym2149] emulated PSG -> HDMI ready (emu2149, %u Hz core, slew clock v2)\n",
            YM_RATE);
    return 0;
}

void ym2149_reset(void)
{
    if (!g_ym)
        return;
    SDL_LockAudioStream(g_ym);               /* serialise with ym_feed_cb */
    atomic_store(&g_tail, atomic_load(&g_head));   /* drop queued writes */
    PSG_reset(g_psg);
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
    if (g_tap)   { fclose(g_tap);   g_tap   = NULL; }
    if (g_evlog) { fclose(g_evlog); g_evlog = NULL; }
}
