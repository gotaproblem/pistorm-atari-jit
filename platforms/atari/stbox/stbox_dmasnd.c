/* SPDX-License-Identifier: MIT
 *
 * stbox_dmasnd.c - the sandbox STE's DMA sound output, stbox_psg.c's
 * pattern: core 3 (stbox.c, dma_advance) fetches the guest's sample bytes
 * in step with guest cycles and pushes S16 stereo frames at the 50066 Hz
 * master rate into a lock-free ring; this file drains the ring into an
 * SDL3 stream bound to the shared audio device, so SDL resamples to the
 * device rate and mixes it with the box PSG, the main machine and MP3.
 *
 * Latency policy: prime on ~20 ms of audio, then follow the guest; if the
 * ring runs empty (guest stopped, or the box is starved) output silence
 * and re-prime; if it backs up past ~160 ms (host hiccup) skip ahead so
 * the sound never drifts seconds behind the picture.
 *
 * LMC1992 (microwire) volume: master 0-40 = -80..0 dB in 2 dB steps,
 * left/right 0-20 = -40..0 dB, as dmasnd_hdmi.c models it for the main
 * machine. Applied per channel here, in the copy.
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdatomic.h>

#include <SDL3/SDL.h>

#include "stbox.h"
#include "../audio/dmasnd.h"

#define DMA_RATE   50066
#define PRIME_FR   1000        /* ~20 ms */
#define MAX_FR     8000        /* ~160 ms: skip ahead beyond this */
#define KEEP_FR    2000        /* ...to this */

static SDL_AudioStream *g_stream;
static atomic_int g_on;
static int g_primed;

static float db_gain(float db) { return powf(10.0f, db / 20.0f); }

static void SDLCALL stbox_dma_feed(void *ud, SDL_AudioStream *stream,
                                   int additional, int total)
{
    (void)ud; (void)total;
    if (additional <= 0)
        return;
    int frames = (additional + 3) / 4;               /* S16 stereo */

    unsigned t = stbox_dma_tail;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    unsigned h = stbox_dma_head;
    unsigned avail = h - t;

    if (avail == 0) {
        g_primed = 0;
    } else if (!g_primed && avail >= PRIME_FR) {
        g_primed = 1;
    }
    if (g_primed && avail > MAX_FR) {                /* backed up: catch up */
        t = h - KEEP_FR;
        avail = KEEP_FR;
    }

    /* LMC1992 gains, read once per callback (the guest may be poking
     * them from core 3 meanwhile; a one-callback lag is fine) */
    int m = stbox_dma_lmc_master, l = stbox_dma_lmc_left, r = stbox_dma_lmc_right;
    float gm = (m >= 40) ? 1.0f : db_gain(-80.0f + 2.0f * (float)(m < 0 ? 0 : m));
    float gl = gm * ((l >= 20) ? 1.0f : db_gain(-40.0f + 2.0f * (float)(l < 0 ? 0 : l)));
    float gr = gm * ((r >= 20) ? 1.0f : db_gain(-40.0f + 2.0f * (float)(r < 0 ? 0 : r)));

    int16_t buf[512 * 2];
    int n = 0;
    while (frames-- > 0) {
        int16_t sl = 0, sr = 0;
        if (g_primed && (h - t) > 0) {
            unsigned i = (t & (STBOX_DMA_RING - 1)) * 2;
            sl = (int16_t)((float)stbox_dma_ring[i]     * gl);
            sr = (int16_t)((float)stbox_dma_ring[i + 1] * gr);
            t++;
        }
        buf[n++] = sl; buf[n++] = sr;
        if (n == (int)(sizeof(buf) / sizeof(buf[0]))) {
            SDL_PutAudioStreamData(stream, buf, n * (int)sizeof(int16_t));
            n = 0;
        }
    }
    if (n)
        SDL_PutAudioStreamData(stream, buf, n * (int)sizeof(int16_t));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    stbox_dma_tail = t;
}

int stbox_dmasnd_start(void)
{
    if (atomic_load(&g_on))
        return 0;
    SDL_AudioDeviceID dev = (SDL_AudioDeviceID)dmasnd_device_id();
    if (!dev) {
        fprintf(stderr, "[STBOX] no SDL audio device - STE DMA sound is silent\n");
        return -1;
    }
    SDL_AudioSpec dst;
    if (!SDL_GetAudioDeviceFormat(dev, &dst, NULL)) {
        dst.format = SDL_AUDIO_S16LE; dst.channels = 2; dst.freq = 48000;
    }
    SDL_AudioSpec src = { SDL_AUDIO_S16, 2, DMA_RATE };
    g_stream = SDL_CreateAudioStream(&src, &dst);
    if (!g_stream)
        return -1;
    SDL_SetAudioStreamGetCallback(g_stream, stbox_dma_feed, NULL);
    if (!SDL_BindAudioStream(dev, g_stream)) {
        SDL_DestroyAudioStream(g_stream); g_stream = NULL;
        return -1;
    }
    g_primed = 0;
    stbox_dma_tail = stbox_dma_head;
    atomic_store(&g_on, 1);
    fprintf(stderr, "[STBOX] STE DMA sound -> SDL mixer ready\n");
    return 0;
}

void stbox_dmasnd_stop(void)
{
    if (!atomic_load(&g_on))
        return;
    atomic_store(&g_on, 0);
    SDL_UnbindAudioStream(g_stream);       /* device lock: callback done */
    SDL_DestroyAudioStream(g_stream);
    g_stream = NULL;
    stbox_dma_tail = stbox_dma_head;       /* drop anything queued */
}
