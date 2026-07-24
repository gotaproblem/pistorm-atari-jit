/*
 * dmasnd_hdmi.c — STE DMA sound -> Raspberry Pi HDMI (ALSA)
 * platforms/atari/audio/ — built as C (gcc), link with -lasound.
 *
 * Output side only. Capture (dmasnd_capture.c) feeds raw signed-8-bit STE
 * samples in via dmasnd_write_bytes(); a private thread converts to S16 stereo
 * and writes to the HDMI ALSA sink. ALSA is the clock. The plug layer resamples
 * the STE rates (6258/12517/25033/50066) up to the HDMI hw rate.
 */

#include <alsa/asoundlib.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "dmasnd.h"

#define RING_BYTES (1u << 20)          /* 1 MiB raw-sample FIFO (pow2) - holds a
                                          couple of the ~180 KB MiNT buffers plus
                                          the pre-roll cushion */
#define RING_MASK  (RING_BYTES - 1)
#define MAX_FRAMES 2048                /* per writei() */
#define PRIME_BYTES 24576u             /* pre-roll cushion (~0.5 s) */

static uint8_t      ring[RING_BYTES];
static atomic_uint      g_frame_len = 0;   /* bytes per commit, set by capture */
static atomic_ullong    g_last_commit_us = 0;

static unsigned long long now_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
}

static int dmasnd_out_dbg(void)
{
    /* The once-per-second output-side stats line. Enable it on its own with
     * PISTORM_DMASND_STATS=1 (quiet — no per-frame capture flood), or as part
     * of the full PISTORM_DMASND_DEBUG=1 trace. Keeping the stats light avoids
     * fprintf overhead perturbing the audio timing. */
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PISTORM_DMASND_DEBUG");
        const char *s = getenv("PISTORM_DMASND_STATS");
        v = ((e && *e == '1') || (s && *s == '1')) ? 1 : 0;
    }
    return v;
}

void dmasnd_note_frame_len(unsigned bytes)
{
    atomic_store(&g_frame_len, bytes);
    atomic_store(&g_last_commit_us, now_us());
}
static atomic_uint  r_head;            /* producer (cpu_task) writes only */
static atomic_uint  r_tail;            /* consumer (audio thread) writes only */

static atomic_uint  a_rate   = 0;
static atomic_int   a_stereo = 0;

static snd_pcm_t   *pcm = NULL;
static char         dev_name[64] = "default";
static pthread_t    thr;
static atomic_int   running = 0;
static atomic_uint  xruns   = 0;

/* ---------------------------------------------------- producer-side API -- */

void dmasnd_set_mode(unsigned rate_hz, int stereo)
{
    atomic_store(&a_rate,   rate_hz);
    atomic_store(&a_stereo, stereo ? 1 : 0);
}

void dmasnd_write_bytes(const void *src, unsigned n)
{
    unsigned head = atomic_load_explicit(&r_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&r_tail, memory_order_acquire);
    unsigned freeb = RING_BYTES - (head - tail) - 1;
    if (n > freeb) n = freeb;                      /* overrun: drop excess */
    const uint8_t *p = src;
    for (unsigned i = 0; i < n; i++)
        ring[(head + i) & RING_MASK] = p[i];
    atomic_store_explicit(&r_head, head + n, memory_order_release);
}

unsigned dmasnd_xruns(void) { return atomic_load(&xruns); }

void dmasnd_output_reset(void)
{
    atomic_store_explicit(&r_head, 0, memory_order_release);
    atomic_store_explicit(&r_tail, 0, memory_order_release);
    atomic_store(&g_frame_len, 0);
    atomic_store(&g_last_commit_us, 0);
    atomic_store(&a_rate, 0);
    atomic_store(&a_stereo, 0);
}

unsigned dmasnd_ring_used(void)
{
    unsigned head = atomic_load_explicit(&r_head, memory_order_acquire);
    unsigned tail = atomic_load_explicit(&r_tail, memory_order_acquire);
    return head - tail;
}

/* ---------------------------------------------------- consumer thread ---- */

static int reopen(unsigned rate)
{
    if (pcm) { snd_pcm_drain(pcm); snd_pcm_close(pcm); pcm = NULL; }

    int err = snd_pcm_open(&pcm, dev_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "dmasnd: open %s: %s\n", dev_name, snd_strerror(err));
        pcm = NULL;
        return -1;
    }
    err = snd_pcm_set_params(pcm,
            SND_PCM_FORMAT_S16_LE,
            SND_PCM_ACCESS_RW_INTERLEAVED,
            2,              /* always 2ch out; mono duplicated */
            rate,           /* STE rate; plug layer resamples to hw */
            1,              /* allow soft resample */
            20000);         /* ~20 ms target latency (us) */
    if (err < 0) {
        fprintf(stderr, "dmasnd: set_params: %s\n", snd_strerror(err));
        snd_pcm_close(pcm); pcm = NULL;
        return -1;
    }
    fprintf(stderr, "[dmasnd] HDMI open OK: %s @ %u Hz\n", dev_name, rate);
    return 0;
}

static void *audio_thread(void *arg)
{
    (void)arg;
    unsigned open_rate = 0;
    int      primed = 0;
    int16_t  out[MAX_FRAMES * 2];

    unsigned long long last_commit_seen = 0;
    unsigned commit_interval_us = 40000;        /* seed ~40 ms until measured */

    while (atomic_load(&running)) {
        unsigned rate   = atomic_load(&a_rate);
        int      stereo = atomic_load(&a_stereo);

        if (rate == 0) { usleep(2000); continue; }
        if (rate != open_rate) {
            if (reopen(rate) == 0) open_rate = rate;
            else { usleep(5000); continue; }
        }

        /* Track the gap between commits (accepted buffers). This, not the
           buffer size, is what sets how much slack we need: a bursty player
           (big buffers, long gaps -> MP3/MOD) needs a deep cushion; a fast
           interactive stream (small buffers, short gaps -> Doom) needs almost
           none, so it stays low-latency. */
        unsigned long long lc = atomic_load(&g_last_commit_us);
        if (lc && lc != last_commit_seen) {
            if (last_commit_seen && lc > last_commit_seen) {
                unsigned d = (unsigned)(lc - last_commit_seen);
                if (d > 3000000u) d = 3000000u;                    /* clamp pauses */
                commit_interval_us = (commit_interval_us * 3u + d) / 4u; /* smooth */
            }
            last_commit_seen = lc;
        }

        unsigned tail  = atomic_load_explicit(&r_tail, memory_order_relaxed);
        unsigned head  = atomic_load_explicit(&r_head, memory_order_acquire);
        unsigned avail = head - tail;
        /* Pre-roll: build a cushion before draining, and re-prime after any
           drain to empty. This is the only cushion available for sources with
           no lookahead (a player refilling one buffer in place per frame). */
        /* Cushion = ~2x the measured commit interval worth of samples. */
        /* Two kinds of source need opposite cushions, so choose by buffer size:
             - big-buffer bursty players (MP3/MOD, ~180 KB commits): need ~2
               buffers so the trough between infrequent refills never hits zero.
               Latency is irrelevant for playback.
             - small-buffer interactive streams (Doom, system sounds): cushion
               scales with the commit interval so it stays low-latency. Tune the
               interval factor with PISTORM_DMASND_CUSHION (percent, default 80). */
        static int cushion_pct = -1;
        if (cushion_pct < 0) {
            const char *e = getenv("PISTORM_DMASND_CUSHION");
            int p = e ? atoi(e) : 0;
            cushion_pct = (p >= 25 && p <= 400) ? p : 80;
        }
        /* Big-buffer (MP3/MOD) pre-roll depth, in multiples of one frame.
         * Default 2. Raise with PISTORM_DMASND_BIGMULT (2..8) if a bursty
         * player still underruns; the prime is still capped at half the ring. */
        static int big_mult = -1;
        if (big_mult < 0) {
            const char *e = getenv("PISTORM_DMASND_BIGMULT");
            int m = e ? atoi(e) : 0;
            big_mult = (m >= 2 && m <= 8) ? m : 2;
        }
        unsigned byterate = rate * (unsigned)(stereo ? 2 : 1);   /* bytes/sec */
        unsigned flen = atomic_load(&g_frame_len);
        unsigned prime;
        if (flen >= 32768u) {
            prime = flen * (unsigned)big_mult;          /* bursty big-buffer */
        } else {
            uint64_t cush = ((uint64_t)commit_interval_us * byterate) / 1000000ull;
            prime = (unsigned)(cush * (unsigned)cushion_pct / 100u);   /* stream */
        }
        if (prime < 2048u)   prime = 2048u;
        if (prime > 524288u) prime = 524288u;   /* hard cap (ring is 1 MiB) */

        if (dmasnd_out_dbg()) {
            static unsigned long long last_log = 0;
            unsigned long long tnow = now_us();
            if (tnow - last_log >= 1000000ull) {
                last_log = tnow;
                fprintf(stderr, "[dmasnd] rate=%u intv=%ums cush=%u avail=%u "
                        "frame_len=%u xrun=%u primed=%d\n",
                        rate, commit_interval_us / 1000u, prime, avail,
                        atomic_load(&g_frame_len), atomic_load(&xruns), primed);
            }
        }
        if (!primed) {
            int ready = (avail >= prime);
            if (!ready && avail > 0 && !dmasnd_is_repeat()) {
                /* One-shot sound (system bleep / short sample): it may be
                   smaller than the cushion, so don't wait for a full pre-roll.
                   Flush shortly after commits stop -> low latency. Continuous
                   repeat-mode playback still waits for the full cushion. */
                unsigned long long idle = now_us() - atomic_load(&g_last_commit_us);
                if (idle > 50000ull) ready = 1;
            }
            if (!ready) { usleep(2000); continue; }
            primed = 1;
        }
        if (avail == 0) { primed = 0; usleep(2000); continue; }

        unsigned avail_frames = stereo ? (avail / 2) : avail;
        unsigned out_frames   = avail_frames;
        if (out_frames > MAX_FRAMES) out_frames = MAX_FRAMES;
        if (out_frames == 0) { usleep(1000); continue; }

        /* Closed-loop drift correction. The Atari sample clock and the HDMI
           clock are independent, so over time production and consumption drift
           (e.g. a 25000-frame/s chunk player vs a 25033 Hz ALSA stream). We
           hold the ring near 'prime' by nudging how many input frames we
           consume per output block: ring low -> consume fewer (stretch), ring
           high -> consume more (compress). Nearest-neighbour, capped at ~0.8%,
           so it's inaudible and only acts while drifting. */
        int err  = (int)avail - (int)prime;          /* bytes from target */
        int corr = err / 4096;                        /* frames */
        if (corr >  (int)MAX_FRAMES/128) corr =  (int)MAX_FRAMES/128;
        if (corr < -(int)MAX_FRAMES/128) corr = -(int)MAX_FRAMES/128;
        int in_frames = (int)out_frames + corr;
        if (in_frames < 1) in_frames = 1;
        if ((unsigned)in_frames > avail_frames) in_frames = (int)avail_frames;

        for (unsigned j = 0; j < out_frames; j++) {
            unsigned k = (unsigned)(((uint64_t)j * (unsigned)in_frames) / out_frames);
            if (stereo) {
                int8_t l = (int8_t)ring[(tail + 2*k)     & RING_MASK];
                int8_t r = (int8_t)ring[(tail + 2*k + 1) & RING_MASK];
                out[2*j]     = (int16_t)(l << 8);
                out[2*j + 1] = (int16_t)(r << 8);
            } else {
                int8_t s = (int8_t)ring[(tail + k) & RING_MASK];
                int16_t v = (int16_t)(s << 8);
                out[2*j] = v; out[2*j + 1] = v;
            }
        }
        unsigned consumed = stereo ? (unsigned)in_frames * 2 : (unsigned)in_frames;
        atomic_store_explicit(&r_tail, tail + consumed, memory_order_release);

        unsigned frames = out_frames;

        snd_pcm_sframes_t w = snd_pcm_writei(pcm, out, frames);
        if (w < 0) {
            atomic_fetch_add(&xruns, 1);
            w = snd_pcm_recover(pcm, (int)w, 1);
            if (w < 0)
                fprintf(stderr, "dmasnd: writei: %s\n", snd_strerror((int)w));
        }
    }
    if (pcm) { snd_pcm_drain(pcm); snd_pcm_close(pcm); pcm = NULL; }
    return NULL;
}

/* ---------------------------------------------------- lifecycle --------- */

int dmasnd_init(const char *device)
{
    if (device && *device) {
        strncpy(dev_name, device, sizeof dev_name - 1);
        dev_name[sizeof dev_name - 1] = 0;
    }
    dmasnd_output_reset();
    atomic_store(&running, 1);
    if (pthread_create(&thr, NULL, audio_thread, NULL) != 0) {
        atomic_store(&running, 0);
        return -1;
    }
    return 0;
}

void dmasnd_close(void)
{
    atomic_store(&running, 0);
    pthread_join(thr, NULL);
}
