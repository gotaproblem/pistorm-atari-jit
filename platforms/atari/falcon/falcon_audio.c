// SPDX-License-Identifier: MIT
/*
 * falcon_audio.c - the Falcon CODEC DAC on the HDMI audio device.
 *
 * Built like ym2149.c: C with -DPISTORM_REAL_SDL3 and the sdl3 cflags. A
 * fourth SDL3 stream on dmasnd's device, S16 stereo at whatever rate the
 * Falcon matrix runs (49170 Hz for most software); SDL resamples.
 *
 * The engine thread (falcon_hw.c) pushes frames into a lock-free ring; the
 * SDL get-callback drains it and wakes the engine, so the ring's fill level
 * is what paces the emulated sample clock. With no audio device the ring
 * drains itself at the nominal rate against CLOCK_MONOTONIC instead, so the
 * DSP and sound DMA still run in real time - silently.
 */
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "falcon.h"
#include "../audio/dmasnd.h"          /* dmasnd_device_id() */

#define RING      16384u              /* frames, power of two */
#define TARGET_MS 40u                 /* how far ahead the engine renders */

static int16_t           g_ring[RING * 2];
static _Atomic uint32_t  g_head, g_tail;
static SDL_AudioStream  *g_st;
static _Atomic unsigned  g_rate = 49170;
static _Atomic int       g_virtual = 1;
static uint64_t          g_virt_ns;

static pthread_mutex_t   g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int       g_kicked;
static _Atomic unsigned  g_underruns;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

unsigned falcon_audio_underruns(void)
{
    return atomic_load(&g_underruns);
}

void falcon_audio_kick(void)
{
    if (!atomic_exchange(&g_kicked, 1)) {
        pthread_mutex_lock(&g_mx);
        pthread_cond_signal(&g_cv);
        pthread_mutex_unlock(&g_mx);
    }
}

void falcon_audio_wait(unsigned us)
{
    if (atomic_exchange(&g_kicked, 0))
        return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)us * 1000L;
    while (ts.tv_nsec >= 1000000000L) { ts.tv_nsec -= 1000000000L; ts.tv_sec++; }
    pthread_mutex_lock(&g_mx);
    if (!atomic_load(&g_kicked))
        pthread_cond_timedwait(&g_cv, &g_mx, &ts);
    pthread_mutex_unlock(&g_mx);
    atomic_store(&g_kicked, 0);
}

/* no device: consume at the nominal rate */
static void virtual_drain(void)
{
    uint64_t t = now_ns();
    if (!g_virt_ns) { g_virt_ns = t; return; }
    uint64_t due = (t - g_virt_ns) * atomic_load(&g_rate) / 1000000000ull;
    if (!due)
        return;
    g_virt_ns += due * 1000000000ull / atomic_load(&g_rate);
    uint32_t h = atomic_load(&g_head), tl = atomic_load(&g_tail);
    uint32_t have = h - tl;
    atomic_store(&g_tail, tl + (uint32_t)(due < have ? due : have));
}

unsigned falcon_audio_fill(void)
{
    if (atomic_load(&g_virtual))
        virtual_drain();
    return atomic_load(&g_head) - atomic_load(&g_tail);
}

unsigned falcon_audio_room(void)
{
    unsigned target = atomic_load(&g_rate) * TARGET_MS / 1000u;
    unsigned fill = falcon_audio_fill();
    return fill >= target ? 0 : target - fill;
}

void falcon_audio_push(const int16_t *lr, unsigned frames)
{
    uint32_t h = atomic_load(&g_head);
    for (unsigned i = 0; i < frames; i++) {
        if (h - atomic_load(&g_tail) >= RING)
            break;                                  /* full: drop the rest */
        g_ring[(h % RING) * 2] = lr[i * 2];
        g_ring[(h % RING) * 2 + 1] = lr[i * 2 + 1];
        h++;
    }
    atomic_store(&g_head, h);
}

static void SDLCALL feed_cb(void *ud, SDL_AudioStream *stream, int additional, int total)
{
    (void)ud; (void)total;
    if (additional <= 0)
        return;
    int want = additional / 4;                      /* frames */
    int16_t buf[2048];
    while (want > 0) {
        int n = want > 1024 ? 1024 : want;
        uint32_t tl = atomic_load(&g_tail), h = atomic_load(&g_head);
        uint32_t have = h - tl;
        int i = 0;
        for (; i < n && (uint32_t)i < have; i++) {
            buf[i * 2] = g_ring[((tl + i) % RING) * 2];
            buf[i * 2 + 1] = g_ring[((tl + i) % RING) * 2 + 1];
        }
        atomic_store(&g_tail, tl + (uint32_t)i);
        if (i < n) {
            atomic_fetch_add(&g_underruns, 1);
            memset(buf + i * 2, 0, (size_t)(n - i) * 4);
        }
        SDL_PutAudioStreamData(stream, buf, n * 4);
        want -= n;
    }
    falcon_audio_kick();
}

int falcon_audio_open(void)
{
    SDL_AudioDeviceID dev = (SDL_AudioDeviceID)dmasnd_device_id();
    if (!dev) {
        atomic_store(&g_virtual, 1);
        return 1;
    }
    SDL_AudioSpec dst;
    if (!SDL_GetAudioDeviceFormat(dev, &dst, NULL)) {
        dst.format = SDL_AUDIO_S16LE; dst.channels = 2; dst.freq = 48000;
    }
    SDL_AudioSpec src;
    src.format = SDL_AUDIO_S16;
    src.channels = 2;
    src.freq = (int)atomic_load(&g_rate);
    g_st = SDL_CreateAudioStream(&src, &dst);
    if (!g_st) {
        fprintf(stderr, "[FALCON] CreateAudioStream: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetAudioStreamGetCallback(g_st, feed_cb, NULL);
    if (!SDL_BindAudioStream(dev, g_st)) {
        fprintf(stderr, "[FALCON] BindAudioStream: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(g_st);
        g_st = NULL;
        return -1;
    }
    atomic_store(&g_virtual, 0);
    return 0;
}

void falcon_audio_rate(unsigned hz)
{
    if (hz < 1000 || hz > 100000)
        return;
    atomic_store(&g_rate, hz);
    if (g_st) {
        SDL_AudioSpec src;
        src.format = SDL_AUDIO_S16;
        src.channels = 2;
        src.freq = (int)hz;
        SDL_SetAudioStreamFormat(g_st, &src, NULL);
    }
}

void falcon_audio_close(void)
{
    if (g_st) {
        SDL_UnbindAudioStream(g_st);
        SDL_DestroyAudioStream(g_st);
        g_st = NULL;
    }
}
