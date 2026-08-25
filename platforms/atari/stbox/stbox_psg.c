/* SPDX-License-Identifier: MIT
 *
 * stbox_psg.c - the sandbox's YM2149, ym2149.c's pattern one door down:
 * core 3 pushes timestamped register writes into a lock-free ring (arch-
 * timer ticks - reading CNTVCT is register access, so the admission rule
 * holds), and a second emu2149 instance renders them in an SDL3 stream
 * bound to the same device as ST/STE sound, MP3 and vidplay. SDL mixes
 * everything; the main machine's PSG and the box's PSG coexist.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>

#include <SDL3/SDL.h>

#include "stbox.h"
#include "../audio/dmasnd.h"
#include "../audio/emu2149.h"

#define YM_CLOCK 2000000u
#define YM_RATE  250000u
#define STEP_NS  (1000000000ull / YM_RATE)

/* ring lives in stbox.c (producer side); we are the consumer */
extern stbox_psg_ev stbox_psg_ring[STBOX_PSG_RING];
extern volatile unsigned stbox_psg_head;
extern volatile unsigned stbox_psg_tail;

static PSG *g_psg;
static SDL_AudioStream *g_stream;
static atomic_int g_on;
static uint64_t g_render_t;            /* ns, lags wall clock          */
static uint64_t g_lag_ns = 30000000ull;
static int32_t  g_dc_acc;
static uint64_t g_tick_num, g_tick_den;   /* ticks -> ns conversion    */

static uint64_t ticks_to_ns(uint64_t t) { return t * g_tick_num / g_tick_den; }

static uint64_t now_ns(void)
{
    uint64_t t;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
    return ticks_to_ns(t);
}

static void SDLCALL stbox_psg_feed(void *ud, SDL_AudioStream *stream,
                                   int additional, int total)
{
    (void)ud; (void)total;
    if (!g_psg || additional <= 0)
        return;
    int frames = (additional + (int)sizeof(int16_t) - 1) / (int)sizeof(int16_t);

    uint64_t now = now_ns();
    if (g_render_t == 0 || g_render_t > now ||
        now - g_render_t > g_lag_ns + 300000000ull)
        g_render_t = (now > g_lag_ns) ? now - g_lag_ns : 0;

    unsigned t_idx = stbox_psg_tail;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    unsigned h_idx = stbox_psg_head;

    int16_t buf[1024];
    int n = 0;
    while (frames-- > 0) {
        while (t_idx != h_idx &&
               ticks_to_ns(stbox_psg_ring[t_idx & (STBOX_PSG_RING - 1)].ticks)
                   <= g_render_t) {
            stbox_psg_ev *e = &stbox_psg_ring[t_idx & (STBOX_PSG_RING - 1)];
            PSG_writeReg(g_psg, e->reg, e->val);
            t_idx++;
        }
        int32_t s = PSG_calc(g_psg);
        g_dc_acc += ((s << 12) - g_dc_acc) >> 12;
        s -= g_dc_acc >> 12;
        buf[n++] = (int16_t)s;
        if (n == (int)(sizeof(buf) / sizeof(buf[0]))) {
            SDL_PutAudioStreamData(stream, buf, n * (int)sizeof(int16_t));
            n = 0;
        }
        g_render_t += STEP_NS;
    }
    if (n)
        SDL_PutAudioStreamData(stream, buf, n * (int)sizeof(int16_t));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    stbox_psg_tail = t_idx;
}

int stbox_psg_start(void)
{
    if (atomic_load(&g_on))
        return 0;
    SDL_AudioDeviceID dev = (SDL_AudioDeviceID)dmasnd_device_id();
    if (!dev) {
        fprintf(stderr, "[STBOX] no SDL audio device - box is silent\n");
        return -1;
    }
    uint64_t frq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    if (!frq) frq = 54000000ull;
    g_tick_num = 1000000000ull;
    g_tick_den = frq;

    g_psg = PSG_new(YM_CLOCK, YM_RATE);
    if (!g_psg) return -1;
    PSG_setVolumeMode(g_psg, 1);
    PSG_reset(g_psg);

    SDL_AudioSpec dst;
    if (!SDL_GetAudioDeviceFormat(dev, &dst, NULL)) {
        dst.format = SDL_AUDIO_S16LE; dst.channels = 2; dst.freq = 48000;
    }
    SDL_AudioSpec src = { SDL_AUDIO_S16, 1, (int)YM_RATE };
    g_stream = SDL_CreateAudioStream(&src, &dst);
    if (!g_stream) {
        PSG_delete(g_psg); g_psg = NULL;
        return -1;
    }
    SDL_SetAudioStreamGetCallback(g_stream, stbox_psg_feed, NULL);
    if (!SDL_BindAudioStream(dev, g_stream)) {
        SDL_DestroyAudioStream(g_stream); g_stream = NULL;
        PSG_delete(g_psg); g_psg = NULL;
        return -1;
    }
    g_render_t = 0;
    atomic_store(&g_on, 1);
    fprintf(stderr, "[STBOX] PSG -> SDL mixer ready\n");
    return 0;
}

void stbox_psg_stop(void)
{
    if (!atomic_load(&g_on))
        return;
    atomic_store(&g_on, 0);
    SDL_UnbindAudioStream(g_stream);       /* device lock: callback done */
    SDL_DestroyAudioStream(g_stream);
    g_stream = NULL;
    PSG_delete(g_psg);
    g_psg = NULL;
    stbox_psg_tail = stbox_psg_head;       /* drop anything queued */
}
