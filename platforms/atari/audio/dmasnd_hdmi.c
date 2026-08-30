/*
 * dmasnd_hdmi.c — STE DMA sound -> HDMI via SDL3 audio.
 *
 * Built as C. Compile with -DPISTORM_REAL_SDL3 and `pkg-config sdl3 --cflags`
 * (so include/SDL3/SDL.h forwards to the real SDL3 header), link with
 * `pkg-config sdl3 --libs`.
 *
 * Only the audio subsystem is used (SDL_INIT_AUDIO) - no SDL video, the DRM
 * display path is untouched. SDL owns one output device and does the format /
 * rate conversion, so we just hand it the raw signed-8-bit STE samples at the
 * current STE rate and it plays them on the HDMI sink. This replaces the whole
 * hand-rolled ALSA ring / resample / drift / mixer machinery.
 *
 * Capture side (dmasnd_capture.c) is unchanged: it calls dmasnd_set_mode() when
 * the ST changes DMA rate/stereo, then dmasnd_write_bytes() with the samples.
 */

#include <SDL3/SDL.h>
#include <mpg123.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

#include <math.h>

#include "dmasnd.h"
#include "ym2149.h"
#include "../avrecord.h"

/* One SDL audio stream carrying the STE sound. src format is set to match the
 * current STE mode (signed 8-bit, mono/stereo, STE rate); SDL converts to the
 * device format. */
static SDL_AudioDeviceID g_dev = 0;      /* opened plain so we can bind >1 stream */
static SDL_AudioSpec     g_devspec;      /* actual device format (stream dst)     */
static SDL_AudioStream *g_ste = NULL;
static atomic_uint  cur_rate   = 0;
static atomic_int   cur_stereo = -1;

/* MP3 host-offload: a second stream bound to the same device, decoded
 * IN-PROCESS with libmpg123 inside the SDL get-callback, auto-mixed by SDL3
 * with the STE stream. No subprocess, no pipes - nothing to starve or wire. */
static SDL_AudioStream *g_mp3 = NULL;
static mpg123_handle   *g_mh  = NULL;
static atomic_int       mp3_on = 0;
static atomic_int       mp3_paused = 0;
static long             mp3_rate_hz = 0;    /* track sample rate (frames/s)   */
static long             mp3_len_s = 0;      /* track length in seconds        */
static char             mp3_meta[3][128];   /* 0=title 1=artist 2=album       */

/* ---------------------------------------------------- lifecycle --------- */

static void SDLCALL dmasnd_postmix(void *ud, const SDL_AudioSpec *spec,
                                   float *buffer, int buflen);

/* libasound prints "snd_pcm_recover: underrun occurred" straight to stderr
 * (bypassing SDL's log system entirely), which floods the console thousands
 * of times under heavy guest load (DOSBox). Install a silent error handler.
 * libasound is loaded dynamically by SDL, so resolve the setter at runtime -
 * no link dependency on -lasound. */
static void alsa_quiet_handler(const char *file, int line, const char *func,
                               int err, const char *fmt, ...)
{
    (void)file; (void)line; (void)func; (void)err; (void)fmt;
}

static void alsa_silence_errors(void)
{
    typedef void (*alsa_err_h)(const char *, int, const char *, int, const char *, ...);
    typedef int (*set_h)(alsa_err_h);
    void *h = dlopen("libasound.so.2", RTLD_LAZY | RTLD_GLOBAL);
    if (!h)
        return;
    set_h set = (set_h)dlsym(h, "snd_lib_error_set_handler");
    if (set)
        set(alsa_quiet_handler);
    /* keep the handle open: the handler must outlive us anyway */
}

int dmasnd_init(const char *device)
{
    (void)device;
    /* Go straight to the ALSA backend: running as root there is no PipeWire
     * session, so SDL's PipeWire probe just spams pw.conf warnings before
     * falling back to ALSA anyway. Overridable via SDL_AUDIO_DRIVER env. */
    if (!getenv("SDL_AUDIO_DRIVER"))
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "alsa");
    /* And silence SDL's chatty audio-category log (it reports the ALSA period
     * geometry at ERROR level even when everything is fine). */
    SDL_SetLogPriority(SDL_LOG_CATEGORY_AUDIO, SDL_LOG_PRIORITY_CRITICAL);
    alsa_silence_errors();
    /* Deeper device buffer (task: underruns under heavy JIT load, e.g.
     * DOSBox): default 2048 frames (~43ms at 48k) instead of SDL's 1024, so
     * a late audio-thread wakeup no longer drains the device. Latency cost
     * is inaudible for this use. PISTORM_AUDIO_FRAMES overrides (512-8192). */
    {
        const char *f = getenv("PISTORM_AUDIO_FRAMES");
        int n = f ? atoi(f) : 2048;
        if (n < 512 || n > 8192) n = 2048;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", n);
        SDL_SetHint("SDL_AUDIO_DEVICE_SAMPLE_FRAMES", buf);
    }
    /* Real-time audio thread. This device's single thread services EVERY
     * bound stream - STE DMA, YM2149, MP3, vidplay, STBOX - and the YM
     * renderer alone computes 250k samples per second of output. It is
     * created from the main thread, so by default it is plain
     * SCHED_OTHER, while cpu_task runs SCHED_FIFO priority 99 pinned to
     * CPU 2 and (with the CPU no longer throttled) never yields. The
     * audio thread only survived on the slack the emulator used to
     * leave; on a machine where isolcpus does not keep them apart it
     * loses outright, which sounds like breakup rather than silence.
     *
     * SDL promotes its audio thread to SCHED_RR on Linux only when this
     * hint permits it. OFF by default: enabling it unconditionally was
     * seen to get the process SIGKILLed on this hardware, so it is an
     * experiment you opt into with PISTORM_AUDIO_RT=1, not a default. */
    {
        const char *rt = getenv("PISTORM_AUDIO_RT");
        if (rt && *rt == '1')
            SDL_SetHint("SDL_THREAD_FORCE_REALTIME_TIME_CRITICAL", "1");
    }
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        fprintf(stderr, "[dmasnd] SDL_Init(AUDIO) failed: %s\n", SDL_GetError());
        return -1;
    }
    /* Open the device "plain" (not OpenAudioDeviceStream) so we can bind both
     * the STE stream and, later, the MP3 stream to it and let SDL3 mix them. */
    SDL_AudioSpec want;
    want.format   = SDL_AUDIO_S16LE;
    want.channels = 2;
    want.freq     = 48000;
    g_dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want);
    if (!g_dev) {
        fprintf(stderr, "[dmasnd] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }
    if (!SDL_GetAudioDeviceFormat(g_dev, &g_devspec, NULL)) {
        g_devspec.format = SDL_AUDIO_S16LE; g_devspec.channels = 2; g_devspec.freq = 48000;
    }
    /* STE stream: signed 8-bit at the STE rate (updated by dmasnd_set_mode). */
    SDL_AudioSpec ste_src;
    ste_src.format   = SDL_AUDIO_S8;
    ste_src.channels = 2;
    ste_src.freq     = 25033;
    g_ste = SDL_CreateAudioStream(&ste_src, &g_devspec);
    if (!g_ste || !SDL_BindAudioStream(g_dev, g_ste)) {
        fprintf(stderr, "[dmasnd] STE stream bind failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_ResumeAudioDevice(g_dev);
    SDL_SetAudioPostmixCallback(g_dev, dmasnd_postmix, NULL);
    fprintf(stderr, "[dmasnd] SDL3 audio ready\n");
    return 0;
}

/* Final-mix tap: SDL hands us the mixed float output (STE + MP3, exactly what
 * is heard) right before the device. Used by the A/V screen recorder. */
static void SDLCALL dmasnd_postmix(void *ud, const SDL_AudioSpec *spec,
                                   float *buffer, int buflen)
{
    (void)ud; (void)spec;
    if (avrecord_active())
        avrecord_audio_push_f32(buffer, buflen / (int)sizeof(float));
}

/* ------------------------------------------------------- LMC1992 -------- *
 * The STE routes YM2149 and DMA sound through an LMC1992 volume/tone chip,
 * programmed over the Microwire interface at $FF8922/$FF8924. The real chip
 * still does its job for the shifter/monitor audio path; this shadows the
 * same commands onto the HDMI streams so both outputs track the guest's
 * volume - otherwise game fade-outs, volume settings and mutes are simply
 * inaudible on HDMI while the real output obeys them.
 *
 * Command word (11 bits): 10 CCC DDDDDD
 *   CCC = 0  mixing   (bits 1-0: 01/10 route YM, 00/11 DMA only)
 *         1  bass     ) tone controls - NOT modelled here: they need real
 *         2  treble   ) filtering, and only shape the sound rather than
 *                       gate it, so the HDMI copy just stays flat
 *         3  master volume  (0-63, 2 dB steps, 0 = -80 dB, >=40 = 0 dB)
 *         4  right volume   (0-31, 2 dB steps, 0 = -40 dB, >=20 = 0 dB)
 *         5  left volume
 *
 * Defaults are deliberately WIDE OPEN rather than the chip's real power-on
 * state (which is near-muted and relies on TOS to open it up). This shadow
 * must fail open: if we ever miss the guest's setup writes, the worst case
 * is HDMI ignoring a volume change, not HDMI going silent.
 * PISTORM_LMC=0 disables the shadow entirely. */
#define LMC_MASTER_MAX 40
#define LMC_LR_MAX     20
static int lmc_master = LMC_MASTER_MAX;
static int lmc_left   = LMC_LR_MAX;
static int lmc_right  = LMC_LR_MAX;
static int lmc_mixing = 1;              /* 1 = DMA + YM (what TOS programs) */

static int lmc_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("PISTORM_LMC");
        on = (e && *e == '0') ? 0 : 1;
    }
    return on;
}

static float lmc_gain_db(float db) { return powf(10.0f, db / 20.0f); }

static float lmc_master_gain(int idx)
{
    if (idx >= LMC_MASTER_MAX) return 1.0f;
    if (idx < 0) idx = 0;
    return lmc_gain_db(-80.0f + 2.0f * (float)idx);
}

static float lmc_lr_gain(int idx)
{
    if (idx >= LMC_LR_MAX) return 1.0f;
    if (idx < 0) idx = 0;
    return lmc_gain_db(-40.0f + 2.0f * (float)idx);
}

static void lmc_apply(void)
{
    /* SDL stream gain is scalar, so left/right are folded to their mean.
     * Software almost always sets them equal; true L/R balance would need
     * per-channel scaling in the postmix. */
    float g = lmc_master_gain(lmc_master) *
              0.5f * (lmc_lr_gain(lmc_left) + lmc_lr_gain(lmc_right));

    if (g_ste)
        SDL_SetAudioStreamGain(g_ste, g);
    /* mixing 01 = YM full range, 10 = YM via low-pass (audible either way;
     * we don't model the filter). 00/11 leave the YM input unrouted. */
    ym2149_set_gain((lmc_mixing == 1 || lmc_mixing == 2) ? g : 0.0f);
    /* NOTE: the MP3 stream is deliberately untouched - it is a host-side
     * player, not something the guest's volume chip is wired to. */
}

void dmasnd_lmc_reset(void)
{
    lmc_master = LMC_MASTER_MAX;
    lmc_left = lmc_right = LMC_LR_MAX;
    lmc_mixing = 1;
    lmc_apply();
}

void dmasnd_microwire_write(uint16_t data)
{
    if (!lmc_enabled())
        return;
    if (((data >> 9) & 0x3) != 0x2)     /* not addressed to the LMC1992 */
        return;

    switch ((data >> 6) & 0x7) {
        case 0: lmc_mixing = data & 0x3;  break;
        case 3: lmc_master = data & 0x3F; break;
        case 4: lmc_right  = data & 0x1F; break;
        case 5: lmc_left   = data & 0x1F; break;
        default: return;                  /* bass/treble: not modelled */
    }
    lmc_apply();
}

/* Device format accessors for the recorder's ffmpeg arguments. */
int dmasnd_out_freq(void)     { return g_devspec.freq; }
int dmasnd_out_channels(void) { return g_devspec.channels; }

/* Device id for other audio sources (ym2149.c) to bind their own stream to.
 * SDL_AudioDeviceID is a Uint32, so a plain unsigned keeps SDL types out of
 * dmasnd.h. 0 until dmasnd_init() has opened the device. */
unsigned dmasnd_device_id(void) { return (unsigned)g_dev; }

void dmasnd_close(void)
{
    dmasnd_mp3_stop();
    if (g_ste) { SDL_DestroyAudioStream(g_ste); g_ste = NULL; }
    if (g_dev) { SDL_CloseAudioDevice(g_dev); g_dev = 0; }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

/* ---------------------------------------------------- producer API ------ */

void dmasnd_set_mode(unsigned rate_hz, int stereo)
{
    if (!g_ste || rate_hz == 0)
        return;
    if (rate_hz == atomic_load(&cur_rate) && stereo == atomic_load(&cur_stereo))
        return;
    atomic_store(&cur_rate, rate_hz);
    atomic_store(&cur_stereo, stereo);

    SDL_AudioSpec spec;
    spec.format   = SDL_AUDIO_S8;
    spec.channels = stereo ? 2 : 1;
    spec.freq     = (int)rate_hz;
    SDL_SetAudioStreamFormat(g_ste, &spec, NULL);   /* NULL dst = keep device fmt */
}

void dmasnd_write_bytes(const void *src, unsigned n)
{
    if (!g_ste || !src || n == 0)
        return;
    SDL_PutAudioStreamData(g_ste, src, (int)n);
}

unsigned dmasnd_ring_used(void)
{
    return g_ste ? (unsigned)SDL_GetAudioStreamQueued(g_ste) : 0;
}

unsigned dmasnd_xruns(void) { return 0; }

void dmasnd_note_frame_len(unsigned bytes) { (void)bytes; }

void dmasnd_output_reset(void)
{
    if (g_ste)
        SDL_ClearAudioStream(g_ste);
    atomic_store(&cur_rate, 0);
    atomic_store(&cur_stereo, -1);
}

/* ---------------------------------------------------- MP3 (SDL3 mix) ---- */
/* Two SDL3 streams on one device: STE sound + MP3; SDL mixes. The MP3 is
 * decoded IN-PROCESS by libmpg123 inside SDL's get-callback - the subprocess
 * approach (ffmpeg + pipes) was abandoned because children spawned from the
 * emulator's CPU thread inherit its core-2 pinning and SCHED_FIFO class and
 * starve behind the JIT loop (state R, ~0.5% CPU, zero output). In-process
 * decode has no scheduling, no pipes, no external binary: the SDL audio thread
 * decodes ~21ms of MP3 per callback (sub-millisecond of CPU) straight from the
 * file. */

static void SDLCALL mp3_feed_cb(void *ud, SDL_AudioStream *stream,
                                int additional, int total)
{
    (void)ud; (void)total;
    if (!g_mh || additional <= 0)
        return;
    if (atomic_load(&mp3_paused))
        return;                         /* paused: decode nothing, hold position */
    static int announced = 0;
    unsigned char buf[16384];
    int need = additional;
    while (need > 0) {
        size_t want = need < (int)sizeof(buf) ? (size_t)need : sizeof(buf);
        size_t done = 0;
        int rc = mpg123_read(g_mh, buf, want, &done);
        if (done > 0) {
            if (!announced) { announced = 1;
                fprintf(stderr, "[NF] MP3: decoding (first %zu bytes)\n", done); }
            SDL_PutAudioStreamData(stream, buf, (int)done);
            need -= (int)done;
        }
        if (rc == MPG123_DONE) {            /* track finished */
            atomic_store(&mp3_on, 0);
            return;
        }
        if (rc != MPG123_OK && rc != MPG123_NEW_FORMAT && done == 0) {
            fprintf(stderr, "[NF] MP3 decode error: %s\n", mpg123_strerror(g_mh));
            atomic_store(&mp3_on, 0);
            return;
        }
    }
}

int dmasnd_mp3_active(void) { return atomic_load(&mp3_on); }

void dmasnd_mp3_stop(void)
{
    atomic_store(&mp3_on, 0);
    if (g_mp3) {
        /* Unbind first: takes the device lock, so no callback is running when
         * we tear down the decoder underneath it. */
        SDL_UnbindAudioStream(g_mp3);
        SDL_DestroyAudioStream(g_mp3);
        g_mp3 = NULL;
    }
    if (g_mh) {
        mpg123_close(g_mh);
        mpg123_delete(g_mh);
        g_mh = NULL;
    }
}

int dmasnd_mp3_play(const char *host_path)
{
    if (!host_path || !*host_path || !g_dev)
        return -1;
    dmasnd_mp3_stop();                          /* one track at a time */

    static int mpg_ready = 0;
    if (!mpg_ready) {
        mpg123_init();                          /* no-op on modern libmpg123 */
        mpg_ready = 1;
    }

    int err = 0;
    mpg123_handle *mh = mpg123_new(NULL, &err);
    if (!mh) {
        fprintf(stderr, "[NF] MP3 mpg123_new: %s\n", mpg123_plain_strerror(err));
        return -1;
    }
    if (mpg123_open(mh, host_path) != MPG123_OK) {
        fprintf(stderr, "[NF] MP3 open '%s': %s\n", host_path, mpg123_strerror(mh));
        mpg123_delete(mh);
        return -1;
    }
    long rate = 0; int channels = 0, enc = 0;
    if (mpg123_getformat(mh, &rate, &channels, &enc) != MPG123_OK || rate <= 0) {
        fprintf(stderr, "[NF] MP3 format: %s\n", mpg123_strerror(mh));
        mpg123_close(mh); mpg123_delete(mh);
        return -1;
    }
    /* Lock the decoder's output to S16 at the track's native rate/channels;
     * SDL converts from there to the device format. */
    mpg123_format_none(mh);
    mpg123_format(mh, rate, channels, MPG123_ENC_SIGNED_16);

    /* Full scan for an accurate length, then grab ID3 metadata (v2 preferred,
     * v1 fallback, filename as last resort). Done here, before the stream is
     * bound, so the GEM app can read it lock-free afterwards. */
    mpg123_scan(mh);
    {
        off_t frames = mpg123_length(mh);
        mp3_rate_hz = rate;
        mp3_len_s = (frames > 0) ? (long)(frames / rate) : 0;

        mpg123_id3v1 *v1 = NULL; mpg123_id3v2 *v2 = NULL;
        memset(mp3_meta, 0, sizeof(mp3_meta));
        mpg123_id3(mh, &v1, &v2);
        if (v2 && v2->title  && v2->title->p  && v2->title->p[0])
            snprintf(mp3_meta[0], sizeof(mp3_meta[0]), "%s", v2->title->p);
        else if (v1 && v1->title[0])
            snprintf(mp3_meta[0], sizeof(mp3_meta[0]), "%.30s", v1->title);
        if (v2 && v2->artist && v2->artist->p && v2->artist->p[0])
            snprintf(mp3_meta[1], sizeof(mp3_meta[1]), "%s", v2->artist->p);
        else if (v1 && v1->artist[0])
            snprintf(mp3_meta[1], sizeof(mp3_meta[1]), "%.30s", v1->artist);
        if (v2 && v2->album  && v2->album->p  && v2->album->p[0])
            snprintf(mp3_meta[2], sizeof(mp3_meta[2]), "%s", v2->album->p);
        else if (v1 && v1->album[0])
            snprintf(mp3_meta[2], sizeof(mp3_meta[2]), "%.30s", v1->album);
        if (!mp3_meta[0][0]) {
            const char *b = strrchr(host_path, '/');
            snprintf(mp3_meta[0], sizeof(mp3_meta[0]), "%s", b ? b + 1 : host_path);
        }
    }

    SDL_AudioSpec src;
    src.format   = SDL_AUDIO_S16LE;
    src.channels = channels;
    src.freq     = (int)rate;
    g_mp3 = SDL_CreateAudioStream(&src, &g_devspec);
    if (!g_mp3) {
        fprintf(stderr, "[NF] MP3 CreateAudioStream: %s\n", SDL_GetError());
        mpg123_close(mh); mpg123_delete(mh);
        return -1;
    }
    g_mh = mh;                                   /* set before callback can run */
    SDL_SetAudioStreamGetCallback(g_mp3, mp3_feed_cb, NULL);
    if (!SDL_BindAudioStream(g_dev, g_mp3)) {
        fprintf(stderr, "[NF] MP3 BindAudioStream: %s\n", SDL_GetError());
        dmasnd_mp3_stop();
        return -1;
    }

    atomic_store(&mp3_paused, 0);
    atomic_store(&mp3_on, 1);
    fprintf(stderr, "[NF] MP3 play (%ldHz %dch): %s\n", rate, channels, host_path);
    return 0;
}

/* ---- transport controls (GEM app) --------------------------------------- */

void dmasnd_mp3_pause(int on)
{
    atomic_store(&mp3_paused, on ? 1 : 0);
}

int dmasnd_mp3_is_paused(void) { return atomic_load(&mp3_paused); }

long dmasnd_mp3_len_s(void) { return mp3_len_s; }

long dmasnd_mp3_pos_s(void)
{
    if (!g_mp3 || !g_mh || mp3_rate_hz <= 0)
        return -1;
    SDL_LockAudioStream(g_mp3);          /* serialize with the decode callback */
    off_t f = mpg123_tell(g_mh);
    SDL_UnlockAudioStream(g_mp3);
    return f >= 0 ? (long)(f / mp3_rate_hz) : -1;
}

void dmasnd_mp3_seek_rel(long delta_s)
{
    if (!g_mp3 || !g_mh || mp3_rate_hz <= 0)
        return;
    SDL_LockAudioStream(g_mp3);
    off_t cur = mpg123_tell(g_mh);
    off_t tgt = cur + (off_t)delta_s * mp3_rate_hz;
    if (tgt < 0) tgt = 0;
    mpg123_seek(g_mh, tgt, SEEK_SET);
    SDL_UnlockAudioStream(g_mp3);
    SDL_ClearAudioStream(g_mp3);         /* drop already-buffered PCM */
}

const char *dmasnd_mp3_meta(int which)
{
    if (which < 0 || which > 2)
        return "";
    return mp3_meta[which];
}
