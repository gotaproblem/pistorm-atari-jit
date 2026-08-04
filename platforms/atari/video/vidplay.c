/* SPDX-License-Identifier: MIT
 *
 * vidplay.c - host video playback (MP4 / MKV / AVI / ...) for the Atari guest.
 * See vidplay.h for the design summary, VIDEO.md for the user-facing docs.
 *
 * HARD RULE (inherited from avrecord.c / the MP3 work): the emulator never
 * spawns external processes for media work. Children inherit the emulator CPU
 * thread's SCHED_FIFO class and core pinning and starve. So everything here is
 * in-process libav*, on threads that FIRST demote themselves to SCHED_OTHER on
 * every core except the one the 68k lives on.
 *
 * Two threads:
 *   decode  - demux, decode video into the next free DRM scanout buffer (no
 *             colour conversion in the common 4:2:0 case, just row copies),
 *             decode+resample audio into an SDL3 stream bound to the same
 *             device as ST sound and MP3 so SDL mixes everything.
 *   present - pops decoded frames and hands them to the video overlay plane at
 *             their presentation time, using the audio queue as the clock.
 *
 * Env:
 *   PISTORM_VID_HWDEC=0     force software decode (default: hardware H.264 via
 *                           /dev/video10 bcm2835-codec when the file allows)
 *   PISTORM_VID_THREADS=n   software decoder threads (default 3)
 *   PISTORM_VID_CPUS=mask   hex affinity mask for the media threads
 *                           (default: every CPU except CPU 2)
 *   PISTORM_VID_DEBUG=1     per-second decode/present/drop stats
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <drm_fourcc.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "vidplay.h"
#include "vidplane.h"
#include "../et4000/et4000_drm.h"
#include "../audio/dmasnd.h"
#include "../avrecord.h"    /* only to know whether to keep a frame for capture */

#if LIBAVUTIL_VERSION_MAJOR >= 57
#define VP_NEW_CHLAYOUT 1
#endif

/* Scanout buffers. TWO are reserved, not one:
 *
 *   - the buffer the CRTC is scanning RIGHT NOW (the last one committed), and
 *   - the buffer the decoder is writing into.
 *
 * Reserving only one was a tearing bug, and a subtle one. The ring let the
 * decoder write into the slot just behind the read pointer - which is exactly
 * the buffer still on screen, because a committed frame keeps being scanned
 * until the NEXT commit replaces it, not just until the commit latches. So the
 * decoder painted a new frame into the picture the display was reading, and you
 * saw two frames at once: tearing that reads as stutter.
 *
 * This is also why 4K HEVC was the only format unaffected. Hardware frames are
 * dmabufs held by reference (g_scanned keeps the on-screen one alive until
 * something replaces it), so they were never recycled underneath the CRTC. The
 * CPU path had no such protection. */
#define VP_NBUF        5
#define VP_RESERVED    2        /* on-screen + being-written                  */
#define VP_AUDIO_AHEAD 0.50     /* seconds of audio we let run ahead           */
#define VQ_MAX         96       /* queued video packets (~4 s at 25 fps)       */
#define VQ_MAX_BYTES   (16 << 20)

/* ------------------------------------------------------------------ state */

struct vslot {
    int      idx;               /* scanout buffer index, -1 for dmabuf frames */
    uint32_t fb;                /* imported framebuffer id (0 = use idx)      */
    AVFrame *hwframe;           /* ref kept while that fb is alive            */
    double   pts;               /* presentation time, seconds from start      */
};

static pthread_mutex_t g_ctl = PTHREAD_MUTEX_INITIALIZER;  /* play/stop lock  */
static pthread_mutex_t g_rq  = PTHREAD_MUTEX_INITIALIZER;  /* ring lock       */
static pthread_cond_t  g_notfull  = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_notempty = PTHREAD_COND_INITIALIZER;

static struct vslot g_ring[VP_NBUF];
static int g_count, g_rd, g_wr;

/* Demuxed-but-not-yet-decoded video packets.
 *
 * Without this, the demux loop blocked on the scanout ring, and since the same
 * loop is what feeds audio, the audio queue could never get further ahead than
 * the ring is deep - about 0.12 s. Any hiccup drained it, the clock slewed
 * backwards chasing a stalled audio position, video waited for the clock, the
 * ring stayed full, and the whole thing spiralled down: 25 fps, then 19, then
 * 4. Parking video packets here lets the demuxer keep running - and keep audio
 * half a second ahead - while the decoder waits for a free scanout buffer.
 * Only the decode thread touches this, so it needs no lock. */
static AVPacket *g_vq[VQ_MAX];
static int  g_vq_head, g_vq_tail, g_vq_count;
static long g_vq_bytes;

static pthread_t g_th_dec, g_th_pre;
static int       g_have_dec, g_have_pre;

static atomic_int g_on;             /* a file is loaded and not finished     */
static atomic_int g_stop;           /* teardown requested                    */
static atomic_int g_paused;
static atomic_int g_eof;            /* demuxer hit EOF, ring may still drain */
static atomic_int g_seek_req;
static atomic_long g_seek_delta;
static atomic_int g_gen;            /* bumped on every flush/seek            */
static atomic_int g_hidden;         /* overlay off, audio keeps running      */

/* libav */
static AVFormatContext *g_fmt;
static AVCodecContext  *g_vctx, *g_actx;
static int              g_vs = -1, g_as = -1;
static struct SwsContext *g_sws;
static SwrContext      *g_swr;

/* video geometry / plane */
static uint32_t g_fourcc;
static int      g_vw, g_vh;
static int      g_convert;          /* 1 = swscale needed                    */
static double   g_fps;
static int      g_hw;
static int      g_zerocopy;         /* decoder hands us dmabufs, no copies    */
static int      g_degrade;          /* 0..3 quality given up to keep up       */
static AVFrame *g_scanned;          /* frame the CRTC is currently showing    */
/* ...and what was committed to show it, so it can be committed again at a new
 * position while PAUSED - see repaint_last(). One of the two is valid: an
 * imported framebuffer id (hardware path, kept alive by g_scanned) or a ring
 * index (dumb-buffer path, kept off the decoder's hands by VP_RESERVED). */
static uint32_t   g_last_fb;
static int        g_last_idx = -1;
static atomic_int g_geom_gen;       /* bumped by any rect or clip change      */
static int      g_src_w, g_src_h;   /* from the container, known at PLAY time */
static int      g_configured;       /* plane sized from the first frame      */
static long     g_frames_out;       /* frames decoded for the current file   */

/* Shared counters so the PRESENT thread can report on the DECODE thread. Twice
 * now a diagnostic has been placed somewhere the failure itself prevented it
 * from running; the present thread is demonstrably alive whenever the decode
 * thread is stuck, so the watchdog lives there and reads these. */
static atomic_long g_v_pkts, g_a_pkts, g_o_pkts, g_loops;

/* Frames per second actually reaching the screen, x100. Independent of the
 * debug statistics - a front-end wants this whether or not anyone is reading
 * the console, and it is the number that says whether playback is really
 * healthy. The file's nominal rate says what it should be; this says what it
 * is. */
static atomic_int g_fps_shown;

/* Is the decode thread still there, and if not, why did it leave? Every exit
 * path from the decode loop sets a reason, so a thread that quietly falls out
 * of its loop can no longer look identical to one that is stuck inside it. */
static atomic_int g_dec_alive;
static const char *g_dec_exit = "still running";

/* audio */
static SDL_AudioStream *g_astream;
static double   g_a_bps;            /* bytes per second of the stream source */
static _Atomic double g_a_pts;      /* pts of the last audio byte queued     */
static int      g_volume = 100;

/* clock for silent files */
static _Atomic double g_wall_base;  /* monotonic time that maps to pts 0      */
static _Atomic double g_wall_frozen;

/* presentation rect */
static int g_rect_x, g_rect_y, g_rect_w, g_rect_h;   /* 0,0,0,0 = auto       */
static int g_dx, g_dy, g_dw, g_dh;                   /* resolved             */
static int g_min_dw, g_min_dh;   /* smallest rect the HVS will actually take */
/* Visible region of the destination, in display pixels. The overlay is a
 * hardware plane and composites above the ENTIRE Atari screen, so nothing GEM
 * draws can ever appear on top of it. A front-end that knows which parts of its
 * window are actually visible (the AES rectangle list does exactly that) can
 * pass that region here, and the picture is cropped to it - so another window
 * overlapping the video really does sit in front of it. w<=0 means no clip. */
static int g_clip_x, g_clip_y, g_clip_w, g_clip_h;
static int g_want_backdrop;   /* fullscreen with bars: black them out */

/* Last decoded frame, kept for the screen recorder - see vidplay_capture_blend.
 * Only ever populated on the dumb-buffer path: a zero-copy frame is a SAND-
 * tiled dmabuf and there is nothing the CPU can do with it. Held by reference,
 * so this costs a refcount rather than a copy. */
static pthread_mutex_t g_cap_lock = PTHREAD_MUTEX_INITIALIZER;
static AVFrame        *g_cap_frame;
static struct SwsContext *g_cap_sws;
static int g_cap_sw, g_cap_sh, g_cap_sfmt;   /* what g_cap_sws was built for */
static int g_cap_dw, g_cap_dh;

/* Intersect the destination with the clip and take the matching slice of the
 * source, so the visible part is not stretched to fill the smaller rectangle.
 * Returns 0 if nothing is visible. */
static int clip_rect(int *dx, int *dy, int *dw, int *dh,
                     int *sx, int *sy, int *sw, int *sh)
{
    int x1, y1, x2, y2;

    *sx = 0; *sy = 0; *sw = g_vw; *sh = g_vh;
    if (g_clip_w <= 0 || g_clip_h <= 0 || *dw <= 0 || *dh <= 0)
        return 1;

    x1 = *dx > g_clip_x ? *dx : g_clip_x;
    y1 = *dy > g_clip_y ? *dy : g_clip_y;
    x2 = (*dx + *dw) < (g_clip_x + g_clip_w) ? (*dx + *dw) : (g_clip_x + g_clip_w);
    y2 = (*dy + *dh) < (g_clip_y + g_clip_h) ? (*dy + *dh) : (g_clip_y + g_clip_h);
    if (x2 - x1 < 2 || y2 - y1 < 2)
        return 0;                        /* nothing of it is visible */

    *sx = (int)((long)(x1 - *dx) * g_vw / *dw);
    *sy = (int)((long)(y1 - *dy) * g_vh / *dh);
    *sw = (int)((long)(x2 - x1)  * g_vw / *dw);
    *sh = (int)((long)(y2 - y1)  * g_vh / *dh);
    if (*sw < 2) *sw = 2;
    if (*sh < 2) *sh = 2;
    if (*sx + *sw > g_vw) *sw = g_vw - *sx;
    if (*sy + *sh > g_vh) *sh = g_vh - *sy;

    *dx = x1; *dy = y1; *dw = x2 - x1; *dh = y2 - y1;
    return (*sw > 1 && *sh > 1);
}
static int g_clamp_said_w, g_clamp_said_h;   /* last clamp we reported        */

static char   g_meta[3][160];
static long   g_len_s;
static double g_start_off;          /* container start_time in seconds        */
static char   g_path[1024];

static int g_debug;

/* ------------------------------------------------------------------ utils */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_s(double s)
{
    if (s <= 0)
        return;
    struct timespec ts;
    ts.tv_sec  = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

/* Sleep until an ABSOLUTE monotonic instant.
 *
 * Frame pacing was a chain of relative nanosleep() calls, and every one of them
 * can overshoot - a few milliseconds each under load, and the error accumulates
 * within a frame instead of cancelling out. That lands frames a little early or
 * late even when every frame is decoded and presented on time, which is exactly
 * what judder looks like. An absolute deadline cannot drift: overshooting one
 * wake-up does not move the target. */
static void sleep_until(double when)
{
    struct timespec ts;
    double now = now_s();
    if (when <= now)
        return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    {
        double d = when - now;
        long long ns = ts.tv_nsec + (long long)(d * 1e9);
        ts.tv_sec += (time_t)(ns / 1000000000LL);
        ts.tv_nsec = (long)(ns % 1000000000LL);
    }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
}

/* The CPU set the media threads are allowed to run on.
 *
 * Two cores are spoken for and we stay off both:
 *   CPU 2 - the 68k CPU thread, SCHED_FIFO. Anything sharing it is starved.
 *   CPU 0 - the et4000 display render thread, SCHED_IDLE. A nice-5 SCHED_OTHER
 *           decoder would trivially preempt it, and that thread is already
 *           printing "render overrun ... budget=16ms" without our help.
 * That leaves CPUs 1 and 3 on a Pi 4, which is plenty for hardware-decoded
 * H.264 and adequate for software decode. PISTORM_VID_CPUS=<hex> overrides -
 * e.g. PISTORM_VID_CPUS=b adds CPU 0 back if you would rather have smoother
 * software decode than a smooth guest display. */
static void media_cpuset(cpu_set_t *set)
{
    const char *e = getenv("PISTORM_VID_CPUS");
    unsigned long mask = e && *e ? strtoul(e, NULL, 16) : 0;

    CPU_ZERO(set);
    if (!mask) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        for (long i = 0; i < n && i < CPU_SETSIZE; i++)
            if (i != 2 && !(n >= 4 && i == 0))
                CPU_SET((int)i, set);
    } else {
        for (int i = 0; i < 64 && i < CPU_SETSIZE; i++)
            if (mask & (1UL << i))
                CPU_SET(i, set);
    }
    if (CPU_COUNT(set) == 0)
        CPU_SET(0, set);
}

/* THIS IS THE WHOLE BALLGAME, so it gets a long comment.
 *
 * These threads are created from the NatFeat handler, which runs ON the 68k CPU
 * thread: SCHED_FIFO, pinned to core 2, executing a JIT loop that essentially
 * never blocks. A new thread inherits its creator's scheduling policy and
 * affinity by default, so a thread created here is born SCHED_FIFO on core 2 -
 * queued behind a real-time spinner that never yields. It therefore never gets
 * a single timeslice, which means it never reaches the first line of its own
 * body. Demoting itself from inside the thread is useless: that code needs the
 * thread to be running before it can run.
 *
 * This is the same wall the ffmpeg-subprocess approach hit during the MP3 work.
 * A forked child inherits exactly the same way, which is why MP3 ended up
 * decoding in-process inside SDL's own audio thread (already a normal thread,
 * created by SDL long before any of this) and side-stepped the problem rather
 * than solving it. Video has no such thread to borrow, so it has to be solved:
 * the policy and affinity are set on the pthread_attr_t, in the CREATOR, so the
 * thread is born SCHED_OTHER on the right cores and never has to be scheduled
 * on core 2 to fix itself. */
static int spawn_media_thread(pthread_t *th, void *(*fn)(void *),
                              const char *what)
{
    pthread_attr_t attr;
    struct sched_param sp;
    cpu_set_t set;
    int rc;

    media_cpuset(&set);
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = 0;

    if (pthread_attr_init(&attr) != 0)
        return -1;
    /* EXPLICIT_SCHED is the load-bearing call: without it the policy and
     * priority below are ignored and the creator's SCHED_FIFO is inherited. */
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setaffinity_np(&attr, sizeof set, &set);

    rc = pthread_create(th, &attr, fn, (void *)what);
    if (rc != 0) {
        /* Some libc/permission combinations refuse explicit scheduling. Fall
         * back to a default thread; it will demote itself and may stutter, but
         * that beats not playing at all. */
        fprintf(stderr, "[VID] %s thread: explicit SCHED_OTHER refused (%s), "
                        "falling back to inherited scheduling\n",
                what, strerror(rc));
        pthread_attr_destroy(&attr);
        return pthread_create(th, NULL, fn, (void *)what) == 0 ? 0 : -1;
    }
    pthread_attr_destroy(&attr);
    return 0;
}

/* Belt and braces: re-assert policy/affinity from inside the thread too, in
 * case the attr path was refused, and report what we actually ended up with.
 * If this line never appears, the thread is not being scheduled at all. */
static void thread_demote(const char *what)
{
    struct sched_param sp;
    cpu_set_t set;
    char cpus[64];
    int n = 0;

    memset(&sp, 0, sizeof sp);
    sched_setscheduler(0, SCHED_OTHER, &sp);
    media_cpuset(&set);
    sched_setaffinity(0, sizeof set, &set);
    /* The decoder is throughput work and can afford to be nice. The presenter
     * is latency work: it does almost nothing but must wake at the right
     * millisecond, and standing behind the decoder in the run queue shows up
     * directly as judder. */
    if (nice(what && what[0] == 'd' ? 5 : 0) == -1) { /* best effort */ }

    cpus[0] = 0;
    for (int i = 0; i < 16 && i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &set) && n < (int)sizeof(cpus) - 4)
            n += snprintf(cpus + n, sizeof(cpus) - n, "%s%d", n ? "," : "", i);

    fprintf(stderr, "[VID] %s thread running: policy=%s cpus=%s\n", what,
            sched_getscheduler(0) == SCHED_OTHER ? "SCHED_OTHER" : "INHERITED",
            cpus);
}

/* ------------------------------------------------------------------ clock */

/* The clock FREE-RUNS off the monotonic timer and is gently steered toward the
 * audio position. It used to BE the audio position, which deadlocked:
 *
 *   audio clock only advances when the demux loop pushes audio
 *     -> the demux loop blocks when the video scanout ring is full
 *       -> the ring only drains when a frame's pts <= the clock
 *         -> the clock only advances when the demux loop pushes audio
 *
 * Four scanout buffers hold ~0.12 s at 25 fps, so the ring filled almost at
 * once, the loop blocked, the audio queue drained, and the clock froze a
 * fraction of a second in - exactly "plays for a moment then stops".
 *
 * A free-running clock cannot stall, so the ring always drains and the loop
 * always makes progress. Audio still governs A/V sync, but as a correction
 * rather than as the source of time: small errors are slewed out at 2% per
 * call, and anything past half a second (a seek, a long stall) is snapped. */
static double master_clock(void)
{
    if (atomic_load(&g_paused))
        return atomic_load(&g_wall_frozen);
    return now_s() - atomic_load(&g_wall_base);
}

/* Called only from the present thread, so the base is written by one thread. */
static void clock_sync_to_audio(void)
{
    if (!g_astream || g_a_bps <= 0.0 || atomic_load(&g_paused))
        return;

    int q = SDL_GetAudioStreamQueued(g_astream);
    if (q < 0)
        q = 0;
    double audio = atomic_load(&g_a_pts) - (double)q / g_a_bps;
    if (audio < 0.0)
        return;

    double wall = now_s() - atomic_load(&g_wall_base);
    double err  = audio - wall;

    if (fabs(err) > 0.5)
        atomic_store(&g_wall_base, now_s() - audio);          /* snap */
    else
        atomic_store(&g_wall_base, atomic_load(&g_wall_base) - err * 0.02);
}

static void clock_set(double pts)
{
    atomic_store(&g_a_pts, pts);
    atomic_store(&g_wall_base, now_s() - pts);
    atomic_store(&g_wall_frozen, pts);
}

/* ------------------------------------------------------------- rect maths */

static void resolve_rect(void)
{
    int mw = (int)vidplane_mode_w();
    int mh = (int)vidplane_mode_h();

    if (g_rect_w > 0 && g_rect_h > 0) {
        /* The front-end works in guest-image pixels (INFO 6/7); the plane
         * works in display pixels. They differ by the letterbox origin. */
        g_dx = g_rect_x + drmpres_dst_x();
        g_dy = g_rect_y + drmpres_dst_y();
        g_dw = g_rect_w; g_dh = g_rect_h;
        /* A window has its own background; the bars there belong to the app. */
        g_want_backdrop = 0;
        return;
    }

    double sar = 1.0;
    if (g_fmt && g_vs >= 0) {
        AVRational r = av_guess_sample_aspect_ratio(g_fmt, g_fmt->streams[g_vs],
                                                    NULL);
        if (r.num > 0 && r.den > 0)
            sar = (double)r.num / (double)r.den;
    }
    double dar = (double)g_vw * sar / (double)g_vh;
    int w = mw;
    int h = (int)(mw / dar + 0.5);
    if (h > mh) {
        h = mh;
        w = (int)(mh * dar + 0.5);
    }
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    g_dw = w & ~1;
    g_dh = h & ~1;
    g_dx = (mw - g_dw) / 2;
    g_dy = (mh - g_dh) / 2;
    /* Fullscreen, and the film does not fill it. Whatever is left over would
     * otherwise be the Atari desktop showing through above and below the
     * picture, which is not what fullscreen means. */
    g_want_backdrop = (g_dw < mw || g_dh < mh);
}

/* -------------------------------------------------------------- ring queue */

static void vq_clear(void)
{
    while (g_vq_count > 0) {
        AVPacket *p = g_vq[g_vq_head];
        g_vq_head = (g_vq_head + 1) % VQ_MAX;
        g_vq_count--;
        av_packet_free(&p);
    }
    g_vq_head = g_vq_tail = g_vq_count = 0;
    g_vq_bytes = 0;
}

static int vq_push(const AVPacket *pkt)
{
    AVPacket *c;
    if (g_vq_count >= VQ_MAX || g_vq_bytes >= VQ_MAX_BYTES)
        return 0;
    c = av_packet_clone(pkt);
    if (!c)
        return 0;
    g_vq[g_vq_tail] = c;
    g_vq_tail = (g_vq_tail + 1) % VQ_MAX;
    g_vq_count++;
    g_vq_bytes += c->size;
    return 1;
}

static AVPacket *vq_pop(void)
{
    AVPacket *p;
    if (g_vq_count == 0)
        return NULL;
    p = g_vq[g_vq_head];
    g_vq_head = (g_vq_head + 1) % VQ_MAX;
    g_vq_count--;
    g_vq_bytes -= p->size;
    return p;
}

/* Is there room in the scanout ring right now? Never blocks. */
static int ring_has_space(void)
{
    int ok;
    pthread_mutex_lock(&g_rq);
    ok = g_count < VP_NBUF - VP_RESERVED;
    pthread_mutex_unlock(&g_rq);
    return ok;
}

static void ring_reset(void)
{
    pthread_mutex_lock(&g_rq);
    for (int i = 0; i < VP_NBUF; i++)
        if (g_ring[i].hwframe)
            av_frame_free(&g_ring[i].hwframe);
    memset(g_ring, 0, sizeof g_ring);
    g_count = g_rd = g_wr = 0;
    /* Whatever was on the plane is no longer guaranteed to survive: the
     * hardware frames have just been dropped and the dumb buffers are about to
     * be rewritten from a different position in the file. Forget it rather
     * than let repaint_last() put a recycled buffer back on screen. */
    g_last_fb  = 0;
    g_last_idx = -1;
    pthread_cond_broadcast(&g_notfull);
    pthread_cond_broadcast(&g_notempty);
    pthread_mutex_unlock(&g_rq);
}

/* Wait for a writable buffer. Returns the index, or -1 if we should give up.
 * One buffer is always reserved for whatever the CRTC is currently scanning. */
static int ring_acquire(void)
{
    struct timespec ts;
    pthread_mutex_lock(&g_rq);
    while (g_count >= VP_NBUF - VP_RESERVED && !atomic_load(&g_stop) &&
           !atomic_load(&g_seek_req)) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_notfull, &g_rq, &ts);
    }
    int idx = (atomic_load(&g_stop) || atomic_load(&g_seek_req)) ? -1 : g_wr;
    pthread_mutex_unlock(&g_rq);
    return idx;
}

static void ring_publish(int idx, uint32_t fb, AVFrame *hwframe, double pts)
{
    pthread_mutex_lock(&g_rq);
    g_ring[g_wr].idx = idx;
    g_ring[g_wr].fb  = fb;
    g_ring[g_wr].hwframe = hwframe;
    g_ring[g_wr].pts = pts;
    g_wr = (g_wr + 1) % VP_NBUF;
    g_count++;
    pthread_cond_signal(&g_notempty);
    pthread_mutex_unlock(&g_rq);
}

/* ------------------------------------------------------------ frame upload */

static void copy_rows(uint8_t *dst, int dpitch, const uint8_t *src, int spitch,
                      int bytes, int rows)
{
    if (dpitch == spitch && dpitch == bytes) {
        memcpy(dst, src, (size_t)bytes * rows);
        return;
    }
    for (int y = 0; y < rows; y++) {
        memcpy(dst, src, (size_t)bytes);
        dst += dpitch;
        src += spitch;
    }
}

static int upload_frame(AVFrame *f, int idx)
{
    uint8_t *base = vidplane_map(idx);
    if (!base)
        return -1;

    uint8_t *dst[4] = {
        base + vidplane_offset(idx, 0),
        base + vidplane_offset(idx, 1),
        base + vidplane_offset(idx, 2),
        NULL
    };
    int dl[4] = {
        (int)vidplane_pitch(idx, 0),
        (int)vidplane_pitch(idx, 1),
        (int)vidplane_pitch(idx, 2),
        0
    };

    if (!g_convert) {
        if (g_fourcc == DRM_FORMAT_NV12) {
            copy_rows(dst[0], dl[0], f->data[0], f->linesize[0], g_vw, g_vh);
            copy_rows(dst[1], dl[1], f->data[1], f->linesize[1], g_vw,
                      (g_vh + 1) / 2);
        } else {                                   /* YUV420 / I420 */
            copy_rows(dst[0], dl[0], f->data[0], f->linesize[0], g_vw, g_vh);
            copy_rows(dst[1], dl[1], f->data[1], f->linesize[1], (g_vw + 1) / 2,
                      (g_vh + 1) / 2);
            copy_rows(dst[2], dl[2], f->data[2], f->linesize[2], (g_vw + 1) / 2,
                      (g_vh + 1) / 2);
        }
        return 0;
    }

    if (!g_sws) {
        g_sws = sws_getContext(f->width, f->height, (enum AVPixelFormat)f->format,
                               g_vw, g_vh,
                               g_fourcc == DRM_FORMAT_NV12 ? AV_PIX_FMT_NV12
                                                           : AV_PIX_FMT_YUV420P,
                               SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!g_sws) {
            fprintf(stderr, "[VID] sws_getContext failed\n");
            return -1;
        }
    }
    sws_scale(g_sws, (const uint8_t * const *)f->data, f->linesize, 0,
              f->height, dst, dl);
    return 0;
}

/* --------------------------------------------------------------- decoders */

/* Hardware decode is H.264 only, and only inside the BCM2711 decoder's limits.
 *
 * The other v4l2m2m wrappers (mpeg2/mpeg4/vp8) are deliberately NOT used.
 * bcm2835-codec advertises some of those formats but cannot actually decode
 * them on a stock Pi 4 (MPEG-2 needs a licence key that is no longer sold), so
 * asking for them buys nothing and costs a 48-packet stall before the fallback
 * notices. Those codecs are all old and small; an A72 eats them in software.
 *
 * The size limit matters just as much: the decode block tops out at 1920x1088.
 * Handing it 4K footage - which is what a phone or a drone produces - gets you
 * a decoder that opens happily and then never emits a frame. */
#define HW_MAX_W 1920
#define HW_MAX_H 1088

static const AVCodec *hw_decoder_for(enum AVCodecID id, int w, int h)
{
    const char *e = getenv("PISTORM_VID_HWDEC");
    if (e && *e == '0')
        return NULL;
    if (id != AV_CODEC_ID_H264)
        return NULL;
    {   /* PISTORM_VID_HWMAX=<width> lets you try the block beyond its
         * documented 1920x1088 - it will simply fail to open or produce
         * nothing, and the software fallback catches that. */
        const char *m = getenv("PISTORM_VID_HWMAX");
        int maxw = m && *m ? atoi(m) : HW_MAX_W;
        int maxh = maxw > HW_MAX_W ? (maxw * 9 / 16 + 16) : HW_MAX_H;
        if (w > maxw || h > maxh) {
            fprintf(stderr, "[VID] %dx%d is beyond the Pi 4 H.264 decoder "
                            "(%dx%d max) - using software decode. Unlike HEVC "
                            "there is no stateless H.264 block to fall back "
                            "on, so 4K H.264 has no hardware path at all on "
                            "this machine.\n", w, h, maxw, maxh);
            return NULL;
        }
    }
    return avcodec_find_decoder_by_name("h264_v4l2m2m");
}

static enum AVPixelFormat vp_get_format(AVCodecContext *avctx,
                                        const enum AVPixelFormat *fmts)
{
    (void)avctx;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_DRM_PRIME)
            return *p;
    return fmts[0];
}

/* Attach the V4L2 Request hwaccel, which is how the Pi 4's STATELESS decoder
 * block (rpivid, /dev/video19) is reached. That block does HEVC up to 4Kp60,
 * and measured on a Pi 4 it decodes 4K H.265 at about 1.5x realtime using ~15%
 * of one core - against roughly 3 fps and two saturated cores in software.
 *
 * Stock FFmpeg has no such hwaccel, so this is a runtime lookup by NAME: with
 * a distro FFmpeg it simply is not found and we decode in software exactly as
 * before. Nothing here is a build-time dependency. tools/build-rpi-ffmpeg.sh
 * builds a suitable FFmpeg into /opt/rpi-ffmpeg.
 *
 * Only HEVC: the Pi 4's H.264 unit is the STATEFUL bcm2835-codec, already
 * driven perfectly well by h264_v4l2m2m, and there is no stateless H.264
 * device for the request API to find. */
static void attach_v4l2request(AVCodecContext *c, enum AVCodecID id)
{
    const char *e = getenv("PISTORM_VID_HWDEC");
    enum AVHWDeviceType type;
    AVBufferRef *dev = NULL;

    if (id != AV_CODEC_ID_HEVC || (e && *e == '0'))
        return;

    type = av_hwdevice_find_type_by_name("v4l2request");
    if (type == AV_HWDEVICE_TYPE_NONE) {
        fprintf(stderr, "[VID] no v4l2request hwaccel in this FFmpeg - HEVC "
                        "will decode in software. See "
                        "tools/build-rpi-ffmpeg.sh.\n");
        return;
    }
    if (av_hwdevice_ctx_create(&dev, type, NULL, NULL, 0) < 0) {
        fprintf(stderr, "[VID] v4l2request device unavailable (is "
                        "dtoverlay=rpivid-v4l2 set and /dev/video19 present?)"
                        "\n");
        return;
    }
    c->hw_device_ctx = dev;              /* context takes ownership */
    c->get_format    = vp_get_format;
    fprintf(stderr, "[VID] v4l2request hwaccel attached for HEVC\n");
}

static int open_video(int allow_hw)
{
    AVStream *st = g_fmt->streams[g_vs];
    const AVCodec *dec = allow_hw
        ? hw_decoder_for(st->codecpar->codec_id, st->codecpar->width,
                         st->codecpar->height)
        : NULL;
    int is_hw = dec != NULL;
    if (!dec)
        dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        fprintf(stderr, "[VID] no decoder for video codec %d\n",
                (int)st->codecpar->codec_id);
        return -1;
    }

    AVCodecContext *c = avcodec_alloc_context3(dec);
    if (!c)
        return -1;
    if (avcodec_parameters_to_context(c, st->codecpar) < 0)
        goto fail;
    c->pkt_timebase = st->time_base;

    AVDictionary *opts = NULL;
    if (is_hw) {
        /* bcm2835-codec wants a decent capture ring or it stalls on reorder,
         * but every buffer is contiguous CMA - twelve 1080p NV12 frames is
         * ~37 MB on top of our scanout buffers and the guest's framebuffers. */
        int big = (st->codecpar->width * st->codecpar->height) > (1280 * 720);
        av_dict_set(&opts, "num_capture_buffers", big ? "8" : "12", 0);
    } else {
        const char *tn = getenv("PISTORM_VID_THREADS");
        int nth = tn && *tn ? atoi(tn) : 3;
        if (nth < 1) nth = 1;
        if (nth > 4) nth = 4;
        c->thread_count = nth;
        c->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
        c->flags2      |= AV_CODEC_FLAG2_FAST;
    }
    if (!is_hw)
        attach_v4l2request(c, st->codecpar->codec_id);

    int rc = avcodec_open2(c, dec, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        if (is_hw) {
            fprintf(stderr, "[VID] %s unavailable, using software decode\n",
                    dec->name);
            avcodec_free_context(&c);
            return open_video(0);
        }
        goto fail;
    }

    g_vctx = c;
    g_hw   = is_hw;
    fprintf(stderr, "[VID] video decoder: %s (%s)\n", dec->name,
            is_hw ? "hardware" : "software");
    return 0;

fail:
    avcodec_free_context(&c);
    return -1;
}

static int open_audio(void)
{
    AVStream *st = g_fmt->streams[g_as];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        fprintf(stderr, "[VID] no decoder for audio codec %d - playing silent\n",
                (int)st->codecpar->codec_id);
        return -1;
    }
    AVCodecContext *c = avcodec_alloc_context3(dec);
    if (!c)
        return -1;
    if (avcodec_parameters_to_context(c, st->codecpar) < 0 ||
        avcodec_open2(c, dec, NULL) < 0) {
        avcodec_free_context(&c);
        return -1;
    }
    c->pkt_timebase = st->time_base;

    int rate = c->sample_rate;
    if (rate < 8000 || rate > 192000)
        rate = 48000;

#ifdef VP_NEW_CHLAYOUT
    AVChannelLayout out_ch;
    av_channel_layout_default(&out_ch, 2);
    if (swr_alloc_set_opts2(&g_swr, &out_ch, AV_SAMPLE_FMT_S16, rate,
                            &c->ch_layout, c->sample_fmt, c->sample_rate,
                            0, NULL) < 0 || swr_init(g_swr) < 0) {
        av_channel_layout_uninit(&out_ch);
        avcodec_free_context(&c);
        return -1;
    }
    av_channel_layout_uninit(&out_ch);
#else
    int64_t in_ch = c->channel_layout ? c->channel_layout
                  : av_get_default_channel_layout(c->channels);
    g_swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16,
                               rate, in_ch, c->sample_fmt, c->sample_rate,
                               0, NULL);
    if (!g_swr || swr_init(g_swr) < 0) {
        avcodec_free_context(&c);
        return -1;
    }
#endif

    SDL_AudioDeviceID dev = (SDL_AudioDeviceID)dmasnd_device_id();
    if (!dev) {
        fprintf(stderr, "[VID] no SDL audio device - playing silent\n");
        swr_free(&g_swr);
        avcodec_free_context(&c);
        return -1;
    }
    SDL_AudioSpec devspec;
    if (!SDL_GetAudioDeviceFormat(dev, &devspec, NULL)) {
        devspec.format = SDL_AUDIO_S16LE;
        devspec.channels = 2;
        devspec.freq = 48000;
    }
    SDL_AudioSpec src;
    src.format   = SDL_AUDIO_S16LE;
    src.channels = 2;
    src.freq     = rate;
    g_astream = SDL_CreateAudioStream(&src, &devspec);
    if (!g_astream || !SDL_BindAudioStream(dev, g_astream)) {
        fprintf(stderr, "[VID] audio stream bind failed: %s\n", SDL_GetError());
        if (g_astream) { SDL_DestroyAudioStream(g_astream); g_astream = NULL; }
        swr_free(&g_swr);
        avcodec_free_context(&c);
        return -1;
    }
    SDL_SetAudioStreamGain(g_astream, g_volume / 100.0f);

    g_actx  = c;
    g_a_bps = (double)rate * 2.0 * 2.0;     /* stereo S16 */
    fprintf(stderr, "[VID] audio decoder: %s, %d Hz -> SDL mix\n",
            dec->name, rate);
    return 0;
}

/* The codec summary shown by the front-ends. Called once before playback and
 * again once the decode thread knows whether it got hardware or software. */
static void describe_media(void)
{
    const AVCodecDescriptor *vd, *ad;

    if (!g_fmt || g_vs < 0)
        return;
    vd = avcodec_descriptor_get(g_fmt->streams[g_vs]->codecpar->codec_id);
    ad = (g_as >= 0)
        ? avcodec_descriptor_get(g_fmt->streams[g_as]->codecpar->codec_id)
        : NULL;
    snprintf(g_meta[2], sizeof g_meta[2], "%s %dx%d %s%s%s",
             vd ? vd->name : "video",
             g_fmt->streams[g_vs]->codecpar->width,
             g_fmt->streams[g_vs]->codecpar->height,
             g_hw ? "hw" : "sw",
             ad ? " + " : " (silent)",
             ad ? ad->name : "");
}

/* ---------------------------------------------------------- plane config */

/* Called once, when the first decoded frame tells us the real pixel format. */
static int configure_plane(AVFrame *f)
{
    enum AVPixelFormat pf = (enum AVPixelFormat)f->format;
    int dispw = (int)vidplane_mode_w();
    int disph = (int)vidplane_mode_h();

    if (f->width <= 0 || f->height <= 0) {
        fprintf(stderr, "[VID] decoded frame has no size (%dx%d, format %d)\n",
                f->width, f->height, f->format);
        return -1;
    }

    /* Hardware frames arrive as dmabuf handles in a tiled layout we neither
     * can nor need to touch: no scanout buffers to allocate, no downscale to
     * apply, no format choice to make. Just record the geometry and let the
     * import path hand the decoder's own memory to the CRTC. */
    /* Now that we know the frame rate for certain, ask the display to run at a
     * refresh it divides into. 24 fps on 60 Hz is a 2:3 cadence and 25 fps is
     * 2,2,3 - judder that correct pacing cannot remove because it is
     * arithmetic. 48 or 50 Hz removes it at the source. Restored on stop. */
    if (g_fps > 1.0 && drmpres_match_refresh(g_fps) > 0)
        vidplane_mode_changed();

    if (pf == AV_PIX_FMT_DRM_PRIME) {
        g_vw = f->width;
        g_vh = f->height;
        g_zerocopy = 1;
        g_hw = 1;
        g_convert = 0;
        vidplane_set_colorimetry(g_vh <= 576 ? 0 : 1,
                                 f->color_range == AVCOL_RANGE_JPEG);
        resolve_rect();
        fprintf(stderr, "[VID] %dx%d hardware frames (dmabuf, zero copy) -> "
                        "dst %dx%d @%d,%d\n",
                g_vw, g_vh, g_dw, g_dh, g_dx, g_dy);
        return 0;
    }

    g_vw = f->width;
    g_vh = f->height;

    /* A scanout buffer bigger than the display is pointless and, on vc4, often
     * fatal: the HVS sizes its line buffer from the plane's SOURCE width, so a
     * 3840-wide plane scaled down to a 1080p screen is rejected outright and
     * you get a perfectly black picture with no other complaint. Cap the source
     * at the display size - aspect preserved - and let swscale do the shrink.
     * Nothing is lost: the display cannot show more pixels than it has. */
    if (dispw > 0 && disph > 0 && (g_vw > dispw || g_vh > disph)) {
        long sw = (long)dispw * f->height;
        long sh = (long)disph * f->width;
        if (sw < sh) {                       /* width is the binding limit */
            g_vw = dispw;
            g_vh = (int)((long)f->height * dispw / f->width);
        } else {
            g_vh = disph;
            g_vw = (int)((long)f->width * disph / f->height);
        }
        g_vw &= ~1;
        g_vh &= ~1;
        if (g_vw < 2) g_vw = 2;
        if (g_vh < 2) g_vh = 2;
        fprintf(stderr, "[VID] source %dx%d is larger than the %dx%d display - "
                        "downscaling to %dx%d on the CPU. Transcoding the file "
                        "to %d-wide H.264 would be far cheaper.\n",
                f->width, f->height, dispw, disph, g_vw, g_vh, dispw);
    }

    int same_size = (g_vw == f->width && g_vh == f->height);

    g_convert = 1;
    g_fourcc  = DRM_FORMAT_NV12;

    if (same_size && (pf == AV_PIX_FMT_YUV420P || pf == AV_PIX_FMT_YUVJ420P) &&
        vidplane_supports(DRM_FORMAT_YUV420)) {
        g_fourcc  = DRM_FORMAT_YUV420;
        g_convert = 0;
    } else if (same_size && pf == AV_PIX_FMT_NV12 &&
               vidplane_supports(DRM_FORMAT_NV12)) {
        g_fourcc  = DRM_FORMAT_NV12;
        g_convert = 0;
    } else if (!vidplane_supports(DRM_FORMAT_NV12)) {
        g_fourcc = DRM_FORMAT_YUV420;
    }

    if (vidplane_alloc(g_fourcc, (uint32_t)g_vw, (uint32_t)g_vh, VP_NBUF) != 0)
        return -1;

    int enc = 1, range = 0;
    if (f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M)
        enc = 0;
    else if (f->colorspace == AVCOL_SPC_BT2020_NCL)
        enc = 2;
    else if (f->colorspace == AVCOL_SPC_UNSPECIFIED)
        enc = (g_vh <= 576) ? 0 : 1;
    if (f->color_range == AVCOL_RANGE_JPEG || pf == AV_PIX_FMT_YUVJ420P)
        range = 1;
    vidplane_set_colorimetry(enc, range);

    resolve_rect();
    fprintf(stderr, "[VID] %dx%d %s -> %s plane, dst %dx%d @%d,%d%s\n",
            g_vw, g_vh, av_get_pix_fmt_name(pf),
            g_fourcc == DRM_FORMAT_NV12 ? "NV12" : "YUV420",
            g_dw, g_dh, g_dx, g_dy, g_convert ? " (swscale)" : " (direct)");
    return 0;
}

/* ------------------------------------------------------------ audio push */

static void push_audio(AVFrame *f, double pts)
{
    if (!g_astream || !g_swr)
        return;

    int out_max = (int)av_rescale_rnd(swr_get_delay(g_swr, g_actx->sample_rate) +
                                      f->nb_samples,
                                      (int64_t)(g_a_bps / 4.0),
                                      g_actx->sample_rate, AV_ROUND_UP) + 64;
    uint8_t *buf = NULL;
    if (av_samples_alloc(&buf, NULL, 2, out_max, AV_SAMPLE_FMT_S16, 0) < 0)
        return;
    int got = swr_convert(g_swr, &buf, out_max,
                          (const uint8_t **)f->extended_data, f->nb_samples);
    if (got > 0) {
        SDL_PutAudioStreamData(g_astream, buf, got * 4);
        atomic_store(&g_a_pts, pts + (double)got / (g_a_bps / 4.0));
    }
    av_freep(&buf);
}

/* --------------------------------------------------------- decode thread */

/* Decoder errors used to be swallowed whole, which is how "nothing happens"
 * became the failure mode. Report each distinct error once - a stream that is
 * failing tends to fail on every packet. */
static void vp_report_err(const char *what, int err)
{
    static int last = 0;
    char buf[128];
    if (err == last)
        return;
    last = err;
    av_strerror(err, buf, sizeof buf);
    fprintf(stderr, "[VID] %s: %s\n", what, buf);
}

static double stream_pts(int stream, int64_t ts)
{
    if (ts == AV_NOPTS_VALUE)
        return -1.0;
    double t = ts * av_q2d(g_fmt->streams[stream]->time_base) - g_start_off;
    return t < 0.0 ? 0.0 : t;
}

static void do_seek(void)
{
    long delta = atomic_load(&g_seek_delta);
    atomic_store(&g_seek_delta, 0);
    double target = master_clock() + (double)delta;
    if (target < 0) target = 0;
    if (g_len_s > 0 && target > (double)g_len_s - 1.0)
        target = (double)g_len_s - 1.0;
    if (target < 0) target = 0;

    int64_t ts = (int64_t)((target + g_start_off) * AV_TIME_BASE);
    if (av_seek_frame(g_fmt, -1, ts, AVSEEK_FLAG_BACKWARD) < 0)
        fprintf(stderr, "[VID] seek to %.1fs failed\n", target);

    vq_clear();
    if (g_vctx) avcodec_flush_buffers(g_vctx);   /* g_configured stays set */
    if (g_actx) avcodec_flush_buffers(g_actx);
    if (g_astream) SDL_ClearAudioStream(g_astream);
    ring_reset();
    atomic_fetch_add(&g_gen, 1);
    atomic_store(&g_eof, 0);
    clock_set(target);
    atomic_store(&g_seek_req, 0);
}

/* Publish one decoded frame into the scanout ring.
 * Returns 1 published, 0 skipped (stopping/seeking), -1 fatal. */
static int publish_frame(AVFrame *frm)
{
    int idx;
    double pts;

    if (!g_configured) {
        if (configure_plane(frm) != 0)
            return -1;
        g_configured = 1;
        if (!g_actx)
            clock_set(0.0);
    }
    idx = ring_acquire();
    if (idx < 0)
        return 0;
    pts = stream_pts(g_vs, frm->best_effort_timestamp);

    if (g_zerocopy) {
        const AVDRMFrameDescriptor *desc =
            (const AVDRMFrameDescriptor *)frm->data[0];
        struct vidplane_dmabuf d;
        AVFrame *keep;
        uint32_t fb = 0;
        int i;

        if (!desc || desc->nb_objects <= 0 || desc->nb_layers <= 0)
            return -1;

        memset(&d, 0, sizeof d);
        d.width  = (uint32_t)frm->width;
        d.height = (uint32_t)frm->height;
        d.format = desc->layers[0].format;
        d.nobjects = desc->nb_objects < VIDPLANE_MAX_OBJECTS
                   ? desc->nb_objects : VIDPLANE_MAX_OBJECTS;
        for (i = 0; i < d.nobjects; i++) {
            d.object_fd[i] = desc->objects[i].fd;
            d.modifier[i]  = desc->objects[i].format_modifier;
        }
        d.nplanes = desc->layers[0].nb_planes < VIDPLANE_MAX_PLANES
                  ? desc->layers[0].nb_planes : VIDPLANE_MAX_PLANES;
        for (i = 0; i < d.nplanes; i++) {
            d.plane_object[i] = desc->layers[0].planes[i].object_index;
            d.plane_offset[i] = (uint32_t)desc->layers[0].planes[i].offset;
            d.plane_pitch[i]  = (uint32_t)desc->layers[0].planes[i].pitch;
        }

        if (vidplane_import(&d, &fb) != 0)
            return -1;

        /* The CRTC will be reading this memory, so the frame has to stay
         * referenced until something else is on screen. */
        keep = av_frame_alloc();
        if (!keep)
            return -1;
        if (av_frame_ref(keep, frm) < 0) {
            av_frame_free(&keep);
            return -1;
        }
        ring_publish(-1, fb, keep, pts >= 0 ? pts : master_clock());
        return 1;
    }

    /* When software decode cannot keep up, throwing away frames that nothing
     * else references buys real time back. Engaged only once we are properly
     * behind and released as soon as we catch up, so a brief hiccup does not
     * cost picture quality. Hardware decode never needs it. */
    /* Software decode that cannot keep up: give away picture quality in
     * stages rather than just falling further behind. Deblocking goes first -
     * it is a large share of H.264 decode time and costs the least visually -
     * then B-frames, then every non-reference frame. Restored the moment we
     * catch up, so a brief stall does not permanently degrade the picture. */
    if (!g_hw && g_vctx && pts >= 0) {
        double lag = master_clock() - pts;
        int want = g_degrade;

        if (lag > 3.0)       want = 3;
        else if (lag > 1.5)  want = 2;
        else if (lag > 0.5)  want = 1;
        else if (lag < 0.3)  want = 0;

        if (want != g_degrade) {
            static const char *what[4] = {
                "full quality",
                "deblocking off",
                "deblocking off + non-reference frames skipped",
                "deblocking off + B-frames skipped"
            };
            g_vctx->skip_loop_filter = want >= 1 ? AVDISCARD_ALL
                                                 : AVDISCARD_DEFAULT;
            g_vctx->skip_frame = want >= 3 ? AVDISCARD_BIDIR
                              : want >= 2 ? AVDISCARD_NONREF
                                          : AVDISCARD_DEFAULT;
            fprintf(stderr, "[VID] %.1fs behind -> %s\n", lag, what[want]);
            g_degrade = want;
        }
    }
    if (upload_frame(frm, idx) == 0) {
        /* Keep a reference for the screen recorder. A refcount, not a copy,
         * and only on this path - the zero-copy branch above returns before
         * here because its frames are unreadable by the CPU. */
        if (avrecord_active()) {
            pthread_mutex_lock(&g_cap_lock);
            if (!g_cap_frame)
                g_cap_frame = av_frame_alloc();
            if (g_cap_frame) {
                av_frame_unref(g_cap_frame);
                if (av_frame_ref(g_cap_frame, frm) < 0)
                    av_frame_unref(g_cap_frame);
            }
            pthread_mutex_unlock(&g_cap_lock);
        }
        ring_publish(idx, 0, NULL, pts >= 0 ? pts : master_clock());
    }
    return 1;
}

/* Pull every frame the decoder currently has. Returns how many came out, or
 * -1 on a fatal error. */
static int drain_video(AVFrame *frm)
{
    int got = 0, rc, r;

    while ((rc = avcodec_receive_frame(g_vctx, frm)) == 0) {
        g_frames_out++;
        got++;
        r = publish_frame(frm);
        av_frame_unref(frm);
        if (r < 0)
            return -1;
    }
    if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF)
        vp_report_err("receive_frame", rc);
    return got;
}

/* Feed one packet to the video decoder.
 *
 * The subtlety that matters: avcodec_send_packet() returning EAGAIN does NOT
 * mean "skip this packet". It means "I have output waiting, drain me and then
 * offer the SAME packet again". Dropping it instead - which is what this code
 * used to do - quietly deletes parts of the bitstream. A codec with a shallow
 * pipeline mostly survives that; MPEG-2, where a dropped packet can take out a
 * slice a sequence header depends on, can end up never assembling a single
 * complete picture and simply reports "need more data" forever. */
static int send_video_packet(AVPacket *pkt, AVFrame *frm)
{
    int tries = 0;

    for (;;) {
        int rc = avcodec_send_packet(g_vctx, pkt);
        if (rc != AVERROR(EAGAIN)) {
            if (rc < 0)
                vp_report_err("send_packet", rc);
            break;
        }
        if (drain_video(frm) < 0)
            return -1;
        if (atomic_load(&g_stop) || atomic_load(&g_seek_req))
            return 0;
        if (++tries > 64) {              /* wedged: give up on this packet */
            vp_report_err("send_packet stuck on EAGAIN", AVERROR(EAGAIN));
            break;
        }
    }
    return drain_video(frm) < 0 ? -1 : 1;
}

static void *decode_thread(void *arg)
{
    thread_demote(arg ? (const char *)arg : "decode");
    g_dec_exit = "still running";
    atomic_store(&g_dec_alive, 1);

    /* THE CODECS ARE OPENED HERE, AND THAT IS DELIBERATE.
     *
     * avcodec_open2() on a multi-threaded software decoder spawns its own
     * worker threads, and those workers inherit the scheduling of whoever
     * called it. Called from vidplay_play() - which runs on the 68k CPU thread,
     * SCHED_FIFO, pinned to core 2 - every worker is born onto the core the JIT
     * loop owns, and is never scheduled. The first avcodec_send_packet() then
     * waits for a worker that will never run, and the decode thread blocks
     * forever: no frames, no error, nothing.
     *
     * That is the same inheritance trap that killed the ffmpeg subprocess
     * during the MP3 work and then killed these two threads, one level further
     * down. libavformat already knows about it, which is why
     * avformat_find_stream_info() forces "threads=1" on its probe decoders with
     * the comment "Ensure non-blocking operation".
     *
     * Opening here means the workers inherit THIS thread: SCHED_OTHER on CPUs
     * 1 and 3. Container opening stays in vidplay_play() so that a missing or
     * unplayable file can still be reported to the guest as PLAY -> -1. */
    if (open_video(1) != 0) {
        fprintf(stderr, "[VID] video decoder would not open\n");
        g_dec_exit = "video decoder would not open";
        atomic_store(&g_eof, 1);
        atomic_store(&g_dec_alive, 0);
        pthread_mutex_lock(&g_rq);
        pthread_cond_broadcast(&g_notempty);
        pthread_mutex_unlock(&g_rq);
        return NULL;
    }
    if (g_as >= 0 && open_audio() != 0)
        g_as = -1;                       /* silent playback, wall clock */
    describe_media();                    /* now that hw vs sw is known */

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();
    int hw_probe_pkts = 0;
    long n_last = 0;
    int  diagnosed = 0, announced_pkt = 0;
    double stat_t = now_s(), slow_t = now_s();
    long slow_frames = 0;

    while (!atomic_load(&g_stop)) {
        atomic_fetch_add(&g_loops, 1);
        if (atomic_load(&g_seek_req))
            do_seek();
        if (atomic_load(&g_paused)) {
            sleep_s(0.010);
            continue;
        }
        /* Feed the decoder from the packet queue for as long as there is
         * somewhere to put the output. This is the only thing that touches the
         * scanout ring, and it is happy to stop early. */
        while (g_vq_count > 0 && ring_has_space() && !atomic_load(&g_stop)) {
            AVPacket *vp = vq_pop();
            int r = send_video_packet(vp, frm);
            av_packet_free(&vp);
            if (r < 0) {
                g_dec_exit = "fatal error publishing a decoded frame";
                goto done;
            }
        }

        /* Back off only when BOTH buffers are comfortable. The audio limit is
         * half a second; the packet queue holds seconds of video, so audio is
         * never the thing that runs dry first. */
        int audio_ahead = g_astream && g_a_bps > 0 &&
            SDL_GetAudioStreamQueued(g_astream) > (int)(g_a_bps * VP_AUDIO_AHEAD);
        if (g_vq_count >= VQ_MAX || g_vq_bytes >= VQ_MAX_BYTES ||
            (audio_ahead && g_vq_count > 8)) {
            sleep_s(0.005);
            continue;
        }

        int rc = av_read_frame(g_fmt, pkt);
        if (rc < 0) {
            char eb[128];
            av_strerror(rc, eb, sizeof eb);
            g_dec_exit = (rc == AVERROR_EOF) ? "end of file"
                                             : "av_read_frame failed";
            if (rc != AVERROR_EOF)
                fprintf(stderr, "[VID] av_read_frame: %s\n", eb);
            /* drain whatever is still queued before flushing the decoder */
            while (g_vq_count > 0 && !atomic_load(&g_stop)) {
                AVPacket *vp = vq_pop();
                send_video_packet(vp, frm);
                av_packet_free(&vp);
            }
            if (g_vctx) {
                avcodec_send_packet(g_vctx, NULL);   /* flush */
                drain_video(frm);
            }
            if (g_frames_out == 0)
                fprintf(stderr, "[VID] end of file and NOT ONE frame decoded "
                                "(video packets seen: %ld). The decoder never "
                                "assembled a picture from this stream.\n",
                        atomic_load(&g_v_pkts));
            atomic_store(&g_eof, 1);
            break;
        }

        if (pkt->stream_index == g_vs && g_vctx) {
            long v_pkts = atomic_fetch_add(&g_v_pkts, 1) + 1;
            hw_probe_pkts++;
            if (!vq_push(pkt))
                fprintf(stderr, "[VID] video packet queue overflow\n");
            if (!announced_pkt) {
                announced_pkt = 1;
                fprintf(stderr, "[VID] first video packet: stream %d, %d bytes, "
                                "pts %lld\n", pkt->stream_index, pkt->size,
                        (long long)pkt->pts);
            }

            /* Hardware decoder that never produces anything: fall back once. */
            if (g_hw && g_frames_out == 0 && hw_probe_pkts > 48) {
                fprintf(stderr, "[VID] hardware decoder produced no frames - "
                                "falling back to software\n");
                avcodec_free_context(&g_vctx);
                g_hw = 0;
                if (open_video(0) != 0) {
                    g_dec_exit = "software decoder would not open either";
                    av_packet_unref(pkt);
                    break;
                }
                av_seek_frame(g_fmt, -1, (int64_t)(g_start_off * AV_TIME_BASE),
                              AVSEEK_FLAG_BACKWARD);
                hw_probe_pkts = 0;
            }

            /* Still nothing after a good run of packets: say everything we
             * know about the stream, once. Silence is the worst failure mode. */
            if (!diagnosed && g_frames_out == 0 && v_pkts > 120) {
                AVCodecParameters *cp = g_fmt->streams[g_vs]->codecpar;
                diagnosed = 1;
                fprintf(stderr,
                    "[VID] %ld video packets in, zero frames out.\n"
                    "[VID]   stream %d of %u, codec %s, %dx%d, extradata %d B\n"
                    "[VID]   last packet: %d bytes, pts %lld, dts %lld, flags 0x%x\n"
                    "[VID]   try PISTORM_VID_HWDEC=0, and 'ffprobe' the file on the Pi\n",
                    v_pkts, g_vs, g_fmt->nb_streams,
                    avcodec_get_name(cp->codec_id), cp->width, cp->height,
                    cp->extradata_size, pkt->size,
                    (long long)pkt->pts, (long long)pkt->dts, pkt->flags);
            }
        } else if (pkt->stream_index == g_as && g_actx) {
            atomic_fetch_add(&g_a_pkts, 1);
            if (avcodec_send_packet(g_actx, pkt) >= 0) {
                while (avcodec_receive_frame(g_actx, frm) == 0) {
                    double pts = stream_pts(g_as, frm->best_effort_timestamp);
                    push_audio(frm, pts >= 0 ? pts : atomic_load(&g_a_pts));
                    av_frame_unref(frm);
                }
            }
        } else {
            atomic_fetch_add(&g_o_pkts, 1);
        }
        av_packet_unref(pkt);

        /* Unconditional, rate-limited: if the machine simply cannot decode
         * this file, say so in plain terms rather than letting it look like a
         * bug. A Pi 4 has no HEVC path through stock FFmpeg's v4l2m2m wrapper,
         * so 4K H.265 lands entirely on two A72 cores plus a 4K->1080p
         * downscale, and that is not a winnable fight. */
        if (now_s() - slow_t >= 5.0) {
            double elapsed = now_s() - slow_t;
            double fps = (double)(g_frames_out - slow_frames) / elapsed;
            if (g_frames_out > 0 && g_fps > 1.0 && fps < g_fps * 0.6 &&
                !atomic_load(&g_eof) && !atomic_load(&g_paused)) {
                AVCodecParameters *cp = g_fmt->streams[g_vs]->codecpar;
                fprintf(stderr,
                    "[VID] DECODE CANNOT KEEP UP: %.1f fps decoded, %.1f fps "
                    "needed (%dx%d %s, %s).\n"
                    "[VID] More cores may help: PISTORM_VID_CPUS=b "
                    "PISTORM_VID_THREADS=4 (costs guest display smoothness).\n"
                    "[VID] Otherwise this file is beyond software decode here; "
                    "H.265 at this size WOULD use the hardware block.\n",
                    fps, g_fps, cp->width, cp->height,
                    avcodec_get_name(cp->codec_id),
                    g_hw ? "hardware" : "software");
            }
            slow_t = now_s();
            slow_frames = g_frames_out;
        }

        if (g_debug && now_s() - stat_t >= 1.0) {
            fprintf(stderr, "[VID] %ld frames/s (total %ld), packets v=%ld "
                            "a=%ld other=%ld, vq %d, ring %d, audio %dms, "
                            "clock %.2f\n",
                    g_frames_out - n_last, g_frames_out,
                    atomic_load(&g_v_pkts), atomic_load(&g_a_pkts),
                    atomic_load(&g_o_pkts), g_vq_count, g_count,
                    (g_astream && g_a_bps > 0)
                        ? (int)(SDL_GetAudioStreamQueued(g_astream) * 1000.0 / g_a_bps)
                        : 0,
                    master_clock());
            n_last = g_frames_out;
            stat_t = now_s();
        }
    }

done:
    if (atomic_load(&g_stop) && !strcmp(g_dec_exit, "still running"))
        g_dec_exit = "stop requested";
    vq_clear();
    atomic_store(&g_dec_alive, 0);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    pthread_mutex_lock(&g_rq);
    pthread_cond_broadcast(&g_notempty);
    pthread_mutex_unlock(&g_rq);
    return NULL;
}

/* Everything we know, printed unconditionally, when 3 seconds pass without a
 * frame reaching the screen. NOT gated on PISTORM_VID_DEBUG - note that `sudo`
 * resets the environment by default, so a debug flag exported in your shell
 * never reaches the emulator unless you use `sudo -E` or put the assignment
 * after `sudo`. */
static void vp_stall_report(void)
{
    fprintf(stderr,
        "[VID] ---- STALL: 3s, no frame on screen ----------------------\n"
        "[VID] decode thread          : %s (%s)\n"
        "[VID] decode loop iterations : %ld\n"
        "[VID] packets   video=%ld  audio=%ld  other=%ld\n"
        "[VID] frames decoded         : %ld   ring queue: %d\n"
        "[VID] state  paused=%d seek=%d eof=%d hidden=%d hw=%d\n"
        "[VID] selected streams: video=%d audio=%d  of %u\n",
        atomic_load(&g_dec_alive) ? "ALIVE" : "GONE", g_dec_exit,
        atomic_load(&g_loops), atomic_load(&g_v_pkts), atomic_load(&g_a_pkts),
        atomic_load(&g_o_pkts), g_frames_out, g_count,
        atomic_load(&g_paused), atomic_load(&g_seek_req), atomic_load(&g_eof),
        atomic_load(&g_hidden), g_hw, g_vs, g_as,
        g_fmt ? g_fmt->nb_streams : 0);

    if (g_fmt) {
        for (unsigned i = 0; i < g_fmt->nb_streams; i++) {
            AVCodecParameters *cp = g_fmt->streams[i]->codecpar;
            fprintf(stderr, "[VID]   stream %u: %s %s %dx%d\n", i,
                    av_get_media_type_string(cp->codec_type)
                        ? av_get_media_type_string(cp->codec_type) : "?",
                    avcodec_get_name(cp->codec_id), cp->width, cp->height);
        }
    }
    if (!atomic_load(&g_dec_alive))
        fprintf(stderr, "[VID] the decode thread has EXITED - see the reason "
                        "above; it is not stuck, it is gone.\n");
    else if (atomic_load(&g_loops) < 10)
        fprintf(stderr, "[VID] the decode thread is BLOCKED, not slow - it has "
                        "barely gone round its loop.\n");
    else if (atomic_load(&g_v_pkts) == 0)
        fprintf(stderr, "[VID] reading packets but NONE belong to the selected "
                        "video stream - the stream index is the problem, not "
                        "the decoder.\n");
    fprintf(stderr, "[VID] -----------------------------------------------\n");
}

/* Smallest destination rectangle the display pipeline will actually scan out
 * for the current source, per the vc4 load tracker. See the long note in
 * hw_min_dst_note below. Returns 0,0 when there is no constraint (software
 * frames are pre-scaled, so they never hit it).
 *
 * From drivers/gpu/drm/vc4/vc4_plane.c:
 *   vscale_factor = DIV_ROUND_UP(src_h, crtc_h);
 *   membus_load  += src_w * src_h * vscale_factor * cpp;
 *   membus_load  *= vrefresh;
 * and vc4_kms.c rejects above 1.5 GB/s ("absolute limit is 2Gbyte/sec, but
 * let's take a margin"). Downscaling forces the HVS to read every source line
 * within one output line's time, so halving the destination height doubles the
 * read bandwidth. The factor is an integer ceiling, so the allowed sizes come
 * in steps rather than a smooth range.
 *
 * On this kernel the tracker is not enforcing, so exceeding it returns no
 * error at all - the HVS underruns and the plane comes out black. That is why
 * this has to be computed up front instead of detected. */
static void hw_min_dst(int *out_w, int *out_h)
{
    static double forced = -1.0;
    int mw = (int)vidplane_mode_w();
    int mh = (int)vidplane_mode_h();
    double budget;
    int min_w, min_h, hz, ch;

    *out_w = *out_h = 0;
    if (!g_zerocopy || g_vw <= 0 || g_vh <= 0 || mw <= 0 || mh <= 0)
        return;

    if (forced < 0.0) {
        const char *e = getenv("PISTORM_VID_MAXDOWN");
        forced = e && *e ? atof(e) : 0.0;
    }
    hz = (int)vidplane_mode_hz();
    if (hz <= 0)
        hz = 60;
    /* MEASURED, not assumed. vc4_kms.c refuses above 1.5 GB/s, but on this
     * kernel the load tracker is not enforcing, so that figure is the driver
     * being cautious rather than a hardware wall. A 3840x2160 source drawn at
     * 1280x720 works in practice, and that costs 1.99 GB/s - so the real
     * ceiling is at least the 2 GB/s the driver comments call "the absolute
     * limit". Using it means a 4K film gets a 720p window instead of being
     * fullscreen-only. PISTORM_VID_BUDGET (in GB/s) tunes it; drop it back to
     * 1.5 if you see the picture flicker or vanish under load. */
    {
        const char *be = getenv("PISTORM_VID_BUDGET");
        double gbs = be && *be ? atof(be) : 2.0;
        if (gbs < 0.25) gbs = 0.25;
        budget = gbs * 1e9 / (double)hz;
    }

    min_h = mh;
    if (forced > 0.0) {
        min_h = (int)(g_vh / forced + 0.5);
    } else {
        for (ch = 8; ch <= mh; ch += 2) {
            int v0 = (g_vh + ch - 1) / ch;
            int v1 = (g_vh / 2 + ch - 1) / ch;
            double load = (double)g_vw * g_vh * v0
                        + (double)(g_vw / 2) * (g_vh / 2) * v1 * 2.0;
            if (load <= budget) {
                min_h = ch;
                break;
            }
        }
    }
    if (min_h > mh) min_h = mh;
    if (min_h < 2)  min_h = 2;
    min_w = (int)((long)g_vw * min_h / g_vh) & ~1;
    min_h &= ~1;
    if (min_w > mw) { min_h = (int)((long)min_h * mw / min_w) & ~1; min_w = mw; }
    if (min_w < 2)  min_w = 2;

    *out_w = min_w;
    *out_h = min_h;
}

/* Show a hardware frame, working around the HVS downscale limit.
 *
 * The vc4 scaler will not reduce a plane by an arbitrary factor: 3840 -> 1920
 * (2x) is fine, 3840 -> 257 (15x) is rejected outright, which is why a 4K film
 * in a small GEM window came up black while fullscreen worked. For software
 * frames this never arose because the source is already capped to the display
 * size; a tiled dmabuf cannot be shrunk on the CPU first, so the destination
 * rectangle is the only lever we have.
 *
 * Rather than hard-code a limit that varies with format and hardware, find it:
 * on rejection, grow the rect (aspect preserved, centred on where the guest
 * asked for it) until the hardware accepts, and remember that size as the floor
 * for this file. The picture may then overflow a small window - worth it,
 * because the alternative is a black rectangle. */
static int show_hw_frame(uint32_t fb)
{
    int mw = (int)vidplane_mode_w();
    int mh = (int)vidplane_mode_h();
    int dx = g_dx, dy = g_dy, dw = g_dw, dh = g_dh;
    int first_w, first_h, grew = 0;
    int i;

    if (mw <= 0 || mh <= 0 || dw <= 0 || dh <= 0)
        return -1;

    {
        int min_w, min_h;
        hw_min_dst(&min_w, &min_h);
        if (min_w > 0 && (dw < min_w || dh < min_h)) {
            if (g_clamp_said_w != dw || g_clamp_said_h != dh) {
                g_clamp_said_w = dw;
                g_clamp_said_h = dh;
                fprintf(stderr, "[VID] asked for %dx%d, giving %dx%d: "
                                "scanning out a %dx%d source costs the memory "
                                "bus src_h/dst_h times its base load and the "
                                "vc4 budget is 1.5 GB/s.%s\n",
                        dw, dh, min_w, min_h, g_vw, g_vh,
                        (min_w >= mw && min_h >= mh)
                            ? " That is the whole display, so this file can "
                              "only be shown fullscreen." : "");
            }
            dx += (dw - min_w) / 2;
            dy += (dh - min_h) / 2;
            dw = min_w;
            dh = min_h;
        }
    }

    /* Apply the floor we already learned, keeping the guest's centre. */
    if (g_min_dw > 0 && dw < g_min_dw) {
        dx += (dw - g_min_dw) / 2;
        dy += (dh - g_min_dh) / 2;
        dw = g_min_dw;
        dh = g_min_dh;
    }

    first_w = dw;
    first_h = dh;
    for (i = 0; i < 10; i++) {
        int cx, cy;
        if (dw > mw) dw = mw;
        if (dh > mh) dh = mh;
        cx = dx; cy = dy;
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (cx + dw > mw) cx = mw - dw;
        if (cy + dh > mh) cy = mh - dh;

        {
            int cdx = cx, cdy = cy, cdw = dw, cdh = dh, sx, sy, sw, sh;
            if (!clip_rect(&cdx, &cdy, &cdw, &cdh, &sx, &sy, &sw, &sh)) {
                vidplane_hide();          /* completely covered */
                return 0;
            }
            if (vidplane_show_fb(fb, (uint32_t)sx, (uint32_t)sy,
                                 (uint32_t)sw, (uint32_t)sh,
                                 cdx, cdy, cdw, cdh) == 0) {
            /* Only interesting if we actually had to GROW after a real
             * rejection. The proactive clamp above already reports its own
             * decision, and claiming the overlay "refused" a size we never
             * offered it would simply be untrue. */
            if (g_min_dw == 0 && grew) {
                g_min_dw = dw;
                g_min_dh = dh;
                fprintf(stderr, "[VID] the overlay rejected %dx%d outright; "
                                "smallest size it actually accepted is %dx%d. "
                                "PISTORM_VID_MAXDOWN is set too high for this "
                                "hardware.\n", first_w, first_h, dw, dh);
            }
            return 0;
            }
        }
        if (dw >= mw || dh >= mh)
            break;                          /* already as big as the display */

        {   /* grow ~1.5x, aspect preserved, never past the display */
            double sc = 1.5;
            int nw, nh;
            if (dw * sc > mw) sc = (double)mw / dw;
            if (dh * sc > mh) sc = (double)mh / dh;
            nw = (int)(dw * sc) & ~1;
            nh = (int)(dh * sc) & ~1;
            if (nw <= dw && nh <= dh)
                break;
            grew = 1;
            dx -= (nw - dw) / 2;
            dy -= (nh - dh) / 2;
            dw = nw;
            dh = nh;
        }
    }
    return -1;
}

/* WHAT IS ON THE PLANE RIGHT NOW.
 *
 * The overlay only ever moves as a side effect of presenting a frame, which is
 * fine while the film is running and useless the moment it is not: pause, drag
 * the window, and the picture stays where the window used to be until playback
 * resumes and the next frame carries it across. Same for the guest hiding the
 * overlay while paused - the file selector would open behind a stranded
 * picture that no longer has anything to move it.
 *
 * So remember the last thing committed and be able to commit it again. For the
 * hardware path that is a framebuffer id, kept alive by the g_scanned
 * reference; for the dumb-buffer path it is a ring index, kept off the
 * decoder's hands by VP_RESERVED. Both stay valid for exactly as long as the
 * picture is on screen, which is exactly as long as this is useful. */
static void repaint_last(void)
{
    if (atomic_load(&g_hidden)) {
        vidplane_backdrop(0);
        return;
    }
    vidplane_backdrop(g_want_backdrop);
    if (g_last_fb) {
        show_hw_frame(g_last_fb);
    } else if (g_last_idx >= 0) {
        int cdx = g_dx, cdy = g_dy, cdw = g_dw, cdh = g_dh;
        int sx, sy, sw, sh;
        if (clip_rect(&cdx, &cdy, &cdw, &cdh, &sx, &sy, &sw, &sh))
            vidplane_show(g_last_idx, (uint32_t)sx, (uint32_t)sy,
                          (uint32_t)sw, (uint32_t)sh, cdx, cdy, cdw, cdh);
        else
            vidplane_hide();
    }
}

/* -------------------------------------------------------- present thread */

static void *present_thread(void *arg)
{
    thread_demote(arg ? (const char *)arg : "present");

    long shown = 0, dropped = 0, total_shown = 0;
    int  was_hidden = 0;
    int  warned_stall = 0;
    int  hw_show_fails = 0;
    int  use_vsync = 1;
    int  last_geom = atomic_load(&g_geom_gen);
    long fps_acc = 0;
    double fps_t = now_s();
    double stat_t = now_s();
    double t0 = now_s();

    while (!atomic_load(&g_stop)) {
        struct timespec ts;
        pthread_mutex_lock(&g_rq);
        while (g_count == 0 && !atomic_load(&g_stop)) {
            if (atomic_load(&g_eof)) {
                pthread_mutex_unlock(&g_rq);
                goto done;
            }
            /* This check belongs INSIDE the wait, not after it: the case it
             * describes is precisely the one where the loop never exits. */
            /* Only a real stall is worth a page of diagnostics. If the guest
             * has asked for the picture to be put away - a GEM front-end with
             * its playlist showing, or a file selector open over the window -
             * then "no frame on screen" is the requested behaviour, not a
             * fault, and printing the full report sends the reader hunting for
             * a decode problem that is not there. */
            if (!warned_stall && total_shown == 0 && now_s() - t0 > 3.0) {
                warned_stall = 1;
                if (atomic_load(&g_hidden))
                    fprintf(stderr, "[VID] 3s with no picture, but the guest "
                                    "has it hidden - sound only, as asked.\n");
                else
                    vp_stall_report();
            }
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&g_notempty, &g_rq, &ts);
        }
        if (atomic_load(&g_stop)) {
            pthread_mutex_unlock(&g_rq);
            break;
        }
        struct vslot s = g_ring[g_rd];
        int queued = g_count;
        pthread_mutex_unlock(&g_rq);

        if (atomic_load(&g_seek_req)) {
            sleep_s(0.005);
            continue;
        }
        if (atomic_load(&g_paused)) {
            /* Nothing is being presented, so this is the only thing left that
             * can move the plane. React to a geometry change, to the guest
             * hiding the picture, and to it coming back. */
            int gen = atomic_load(&g_geom_gen);
            int hid = atomic_load(&g_hidden);
            if (hid) {
                if (!was_hidden) {
                    vidplane_hide();
                    was_hidden = 1;
                }
            } else if (was_hidden || gen != last_geom) {
                was_hidden = 0;
                last_geom  = gen;
                repaint_last();
            }
            sleep_s(0.010);
            continue;
        }
        last_geom = atomic_load(&g_geom_gen);

        clock_sync_to_audio();
        double delay = s.pts - master_clock();

        /* PACE TO THE VBLANK, NOT TO THE WALL CLOCK.
         *
         * Sleeping until a frame is due and committing then cannot be made to
         * work by being more precise about the sleep, because the display only
         * changes at vblanks: a commit lands on whichever vblank happens to
         * come next, so a millisecond of jitter moves a frame a whole refresh
         * period. Every frame decoded, every frame presented, and it judders -
         * which is exactly what the counters kept showing.
         *
         * So: block until a vblank, then ask whether this frame belongs on the
         * NEXT one. Presentation becomes exactly refresh-quantised and the wall
         * clock only decides which frame, never when. (24 fps on a 60 Hz panel
         * still gets a 2:3 cadence - that is arithmetic, not jitter.) */
        if (use_vsync) {
            double vt = 0.0, period = 1.0 / 60.0, target;
            if (vidplane_wait_vblank(&vt, &period) != 0) {
                use_vsync = 0;              /* driver will not; fall back */
                continue;
            }
            /* What this vblank's commit will show, and therefore which frame
             * belongs on it. */
            target = master_clock() + period;

            /* DRAIN SUPERSEDED FRAMES HERE, in this same vblank.
             *
             * The old code examined exactly one frame per vblank and dropped it
             * if it was more than 100 ms late. That cannot recover: dropping
             * one frame per refresh consumes them at exactly the rate they
             * arrive, so once the presenter is behind it stays behind and drops
             * EVERY frame for ever - "presented 0, dropped 25" in the logs, and
             * a completely frozen picture. It only became visible when the
             * display dropped to 24 Hz, where a single frame per vblank is the
             * entire budget. Discarding every frame that a newer one has
             * already superseded lets it catch up inside one refresh. */
            for (;;) {
                struct vslot old;
                int drop = 0;
                pthread_mutex_lock(&g_rq);
                if (g_count >= 2 &&
                    g_ring[(g_rd + 1) % VP_NBUF].pts <= target) {
                    old = g_ring[g_rd];
                    g_ring[g_rd].hwframe = NULL;
                    g_ring[g_rd].fb      = 0;
                    g_rd = (g_rd + 1) % VP_NBUF;
                    g_count--;
                    pthread_cond_signal(&g_notfull);
                    drop = 1;
                }
                s = g_ring[g_rd];
                queued = g_count;
                pthread_mutex_unlock(&g_rq);
                if (!drop)
                    break;
                if (old.hwframe)
                    av_frame_free(&old.hwframe);
                dropped++;
            }
            if (s.pts > target + period * 0.5)
                continue;                   /* not its turn yet */
            delay = 0.0;                    /* vblank decides timing, not us */
        } else if (delay > 0.020) {
            /* A long way off: nap in short hops so stop/pause/seek stay
             * responsive. */
            sleep_s(0.010);
            continue;
        }
        /* PRESENT ONE VBLANK EARLY.
         *
         * A commit issued now does not appear now - it latches at the NEXT
         * vblank. Waiting until the frame is due therefore puts it on screen a
         * vblank late, and worse, whether it catches vblank N or N+1 depends on
         * a millisecond of wake-up jitter. On a 60 Hz panel that flips the gap
         * between 16.7 ms and 33.3 ms from frame to frame: every frame present,
         * none dropped, and visible judder. Aiming a full vblank early puts the
         * commit comfortably inside the right refresh window and gives the
         * jitter somewhere to go. */
        if (!use_vsync && delay > 0.0) {
            int hz = (int)vidplane_mode_hz();
            double lead = (hz > 0) ? 1.0 / hz : 1.0 / 60.0;
            if (lead > 0.020) lead = 0.020;
            if (delay > lead)
                sleep_until(now_s() + (delay - lead));
        }

        /* CONSUME THE SLOT FIRST, taking ownership of the held frame and
         * clearing it out of the ring in the same locked section.
         *
         * The previous version copied the slot, handed s.hwframe on to
         * g_scanned, and left the identical pointer sitting in g_ring[] - so
         * ring_reset() on stop or seek freed a frame that g_scanned also
         * owned, and glibc aborted with "double free or corruption". A frame
         * must be owned by exactly one place at a time, and the handover has
         * to happen under the lock that protects the ring. */
        pthread_mutex_lock(&g_rq);
        s = g_ring[g_rd];
        g_ring[g_rd].hwframe = NULL;
        g_ring[g_rd].fb      = 0;
        g_rd = (g_rd + 1) % VP_NBUF;
        if (g_count > 0) g_count--;
        pthread_cond_signal(&g_notfull);
        pthread_mutex_unlock(&g_rq);

        if (atomic_load(&g_hidden)) {
            /* The guest asked for the picture to go away (e.g. a GEM app
             * showing its playlist over the window) but the soundtrack keeps
             * playing, so we keep consuming frames on time - just not showing
             * them. Un-hiding then resumes in sync rather than replaying. */
            if (s.hwframe)
                av_frame_free(&s.hwframe);
            if (!was_hidden) {
                vidplane_hide();
                vidplane_backdrop(0);   /* the guest wants its screen back */
                was_hidden = 1;
            }
        } else if (!use_vsync && delay < -0.100 && queued > 1) {
            was_hidden = 0;
            dropped++;                       /* we are behind: skip this one */
            if (s.hwframe)
                av_frame_free(&s.hwframe);
        } else {
            was_hidden = 0;
            /* Cheap no-op once it is in the right state, so it can simply be
             * asserted on every frame rather than tracked separately. */
            vidplane_backdrop(g_want_backdrop);
            if (s.fb) {
                if (show_hw_frame(s.fb) != 0)
                    hw_show_fails++;
                /* SetPlane is a blocking, vblank-latched commit, so once it
                 * returns the PREVIOUS buffer is genuinely off screen and its
                 * frame can go back to the decoder's pool. */
                if (g_scanned)
                    av_frame_free(&g_scanned);
                g_scanned = s.hwframe;
                s.hwframe = NULL;
                g_last_fb  = s.fb;       /* alive as long as g_scanned is */
                g_last_idx = -1;
            } else {
                int cdx = g_dx, cdy = g_dy, cdw = g_dw, cdh = g_dh;
                int sx, sy, sw, sh;
                if (clip_rect(&cdx, &cdy, &cdw, &cdh, &sx, &sy, &sw, &sh))
                    vidplane_show(s.idx, (uint32_t)sx, (uint32_t)sy,
                                  (uint32_t)sw, (uint32_t)sh,
                                  cdx, cdy, cdw, cdh);
                else
                    vidplane_hide();
                g_last_fb  = 0;
                g_last_idx = s.idx;      /* VP_RESERVED keeps it off screen */
            }
            shown++;
            total_shown++;
            fps_acc++;
        }
        if (s.hwframe)                    /* nothing took it: do not leak */
            av_frame_free(&s.hwframe);

        if (hw_show_fails == 8) {
            hw_show_fails++;              /* say this once */
            fprintf(stderr, "[VID] the overlay has rejected every hardware "
                            "frame, at every size up to fullscreen. A %dx%d "
                            "source is beyond what this display pipeline will "
                            "scan out.\n", g_vw, g_vh);
        }

        if (now_s() - fps_t >= 1.0) {
            double el = now_s() - fps_t;
            atomic_store(&g_fps_shown, (int)((fps_acc / el) * 100.0 + 0.5));
            fps_acc = 0;
            fps_t = now_s();
        }

        if (g_debug && now_s() - stat_t >= 1.0) {
            fprintf(stderr, "[VID] presented %ld, dropped %ld%s (busy %u)\n",
                    shown, dropped, use_vsync ? ", vsync" : ", timed",
                    vidplane_busy_commits());
            shown = dropped = 0;
            stat_t = now_s();
        }
    }

done:
    vidplane_hide();
    atomic_store(&g_on, 0);
    return NULL;
}

/* ------------------------------------------------------------ public API */

static void teardown(void)
{
    if (g_have_pre) { pthread_join(g_th_pre, NULL); g_have_pre = 0; }
    if (g_have_dec) { pthread_join(g_th_dec, NULL); g_have_dec = 0; }

    vidplane_hide();
    drmpres_restore_refresh();         /* put the display back as we found it */
    vidplane_mode_changed();
    ring_reset();                      /* releases any held hardware frames */
    if (g_scanned)
        av_frame_free(&g_scanned);
    vidplane_free();                   /* also drops the dmabuf imports     */

    if (g_astream) {
        SDL_UnbindAudioStream(g_astream);
        SDL_DestroyAudioStream(g_astream);
        g_astream = NULL;
    }
    if (g_swr)  swr_free(&g_swr);
    if (g_sws)  { sws_freeContext(g_sws); g_sws = NULL; }
    pthread_mutex_lock(&g_cap_lock);
    if (g_cap_sws)   { sws_freeContext(g_cap_sws); g_cap_sws = NULL; }
    if (g_cap_frame) { av_frame_free(&g_cap_frame); }
    g_cap_sw = g_cap_sh = g_cap_dw = g_cap_dh = 0;
    g_cap_sfmt = -1;
    pthread_mutex_unlock(&g_cap_lock);
    if (g_vctx) avcodec_free_context(&g_vctx);
    if (g_actx) avcodec_free_context(&g_actx);
    if (g_fmt)  avformat_close_input(&g_fmt);

    g_vs = g_as = -1;
    g_a_bps = 0;
    g_vw = g_vh = 0;
    g_min_dw = g_min_dh = 0;
    g_clamp_said_w = g_clamp_said_h = 0;
    g_src_w = g_src_h = 0;
    g_len_s = 0;
    g_start_off = 0;
    g_hw = 0;
    g_zerocopy = 0;
    g_degrade = 0;
    g_configured = 0;
    g_frames_out = 0;
    g_want_backdrop = 0;
    ring_reset();
    atomic_store(&g_on, 0);
    atomic_store(&g_eof, 0);
    atomic_store(&g_paused, 0);
    atomic_store(&g_seek_req, 0);
    atomic_store(&g_hidden, 0);
}

void vidplay_stop(void)
{
    pthread_mutex_lock(&g_ctl);
    if (g_have_dec || g_have_pre) {
        atomic_store(&g_stop, 1);
        pthread_mutex_lock(&g_rq);
        pthread_cond_broadcast(&g_notfull);
        pthread_cond_broadcast(&g_notempty);
        pthread_mutex_unlock(&g_rq);
        teardown();
    }
    atomic_store(&g_stop, 0);
    pthread_mutex_unlock(&g_ctl);
}

int vidplay_play(const char *host_path)
{
    if (!host_path || !*host_path)
        return -1;

    vidplay_stop();
    pthread_mutex_lock(&g_ctl);

    g_debug = getenv("PISTORM_VID_DEBUG") != NULL;

    static int av_ready = 0;
    if (!av_ready) {
        av_log_set_level(g_debug ? AV_LOG_WARNING : AV_LOG_ERROR);
#if LIBAVFORMAT_VERSION_MAJOR < 58
        av_register_all();
#endif
        av_ready = 1;
    }

    if (vidplane_open() != 0) {
        pthread_mutex_unlock(&g_ctl);
        return -1;
    }

    snprintf(g_path, sizeof g_path, "%s", host_path);

    if (avformat_open_input(&g_fmt, host_path, NULL, NULL) < 0) {
        fprintf(stderr, "[VID] cannot open '%s'\n", host_path);
        pthread_mutex_unlock(&g_ctl);
        return -1;
    }
    if (avformat_find_stream_info(g_fmt, NULL) < 0) {
        fprintf(stderr, "[VID] no stream info in '%s'\n", host_path);
        goto fail;
    }

    g_vs = av_find_best_stream(g_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    g_as = av_find_best_stream(g_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (g_as < 0)
        g_as = -1;              /* it returns AVERROR_STREAM_NOT_FOUND, not -1 */
    if (g_vs < 0) {
        fprintf(stderr, "[VID] '%s' has no video stream\n", host_path);
        goto fail;
    }

    g_start_off = (g_fmt->start_time != AV_NOPTS_VALUE)
                ? g_fmt->start_time / (double)AV_TIME_BASE : 0.0;
    g_len_s = (g_fmt->duration > 0) ? (long)(g_fmt->duration / AV_TIME_BASE) : 0;

    g_src_w = g_fmt->streams[g_vs]->codecpar->width;
    g_src_h = g_fmt->streams[g_vs]->codecpar->height;
    {
        AVRational fr = av_guess_frame_rate(g_fmt, g_fmt->streams[g_vs], NULL);
        g_fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 25.0;
    }

    /* A decoder must at least EXIST for this codec, so an unplayable file is
     * still reported to the guest as PLAY -> -1. Opening it happens on the
     * decode thread; see the long comment there. */
    if (!avcodec_find_decoder(g_fmt->streams[g_vs]->codecpar->codec_id)) {
        fprintf(stderr, "[VID] no decoder for video codec %s\n",
                avcodec_get_name(g_fmt->streams[g_vs]->codecpar->codec_id));
        goto fail;
    }

    /* metadata for the front-end; the codec summary is refined by the decode
     * thread once it knows whether it got hardware or software decode. */
    memset(g_meta, 0, sizeof g_meta);
    {
        AVDictionaryEntry *e;
        if ((e = av_dict_get(g_fmt->metadata, "title", NULL, 0)) && e->value)
            snprintf(g_meta[0], sizeof g_meta[0], "%s", e->value);
        if (!g_meta[0][0]) {
            const char *b = strrchr(host_path, '/');
            snprintf(g_meta[0], sizeof g_meta[0], "%s", b ? b + 1 : host_path);
        }
        if ((e = av_dict_get(g_fmt->metadata, "artist", NULL, 0)) && e->value)
            snprintf(g_meta[1], sizeof g_meta[1], "%s", e->value);
        else if ((e = av_dict_get(g_fmt->metadata, "author", NULL, 0)) && e->value)
            snprintf(g_meta[1], sizeof g_meta[1], "%s", e->value);
    }
    describe_media();

    g_configured = 0;
    g_frames_out = 0;
    g_zerocopy = 0;
    g_degrade = 0;
    atomic_store(&g_fps_shown, 0);
    g_min_dw = g_min_dh = 0;
    g_clip_w = g_clip_h = 0;
    g_clamp_said_w = g_clamp_said_h = 0;
    atomic_store(&g_v_pkts, 0);
    atomic_store(&g_a_pkts, 0);
    atomic_store(&g_o_pkts, 0);
    atomic_store(&g_loops, 0);
    clock_set(0.0);
    atomic_store(&g_eof, 0);
    atomic_store(&g_paused, 0);
    atomic_store(&g_stop, 0);
    atomic_store(&g_seek_req, 0);
    atomic_store(&g_seek_delta, 0);
    ring_reset();
    atomic_store(&g_on, 1);

    if (spawn_media_thread(&g_th_dec, decode_thread, "decode") != 0)
        goto fail;
    g_have_dec = 1;
    if (spawn_media_thread(&g_th_pre, present_thread, "present") != 0) {
        atomic_store(&g_stop, 1);
        goto fail;
    }
    g_have_pre = 1;

    fprintf(stderr, "[VID] playing %s (%lds, %.2f fps)\n", host_path, g_len_s,
            g_fps);
    for (unsigned si = 0; si < g_fmt->nb_streams; si++) {
        AVCodecParameters *cp = g_fmt->streams[si]->codecpar;
        fprintf(stderr, "[VID]   stream %u: %s %s %dx%d%s%s\n", si,
                av_get_media_type_string(cp->codec_type)
                    ? av_get_media_type_string(cp->codec_type) : "?",
                avcodec_get_name(cp->codec_id), cp->width, cp->height,
                (int)si == g_vs ? "  <- video" : "",
                (int)si == g_as ? "  <- audio" : "");
    }
    pthread_mutex_unlock(&g_ctl);
    return 0;

fail:
    teardown();
    atomic_store(&g_stop, 0);
    pthread_mutex_unlock(&g_ctl);
    return -1;
}

int  vidplay_active(void)    { return atomic_load(&g_on); }
int  vidplay_is_paused(void) { return atomic_load(&g_paused); }
long vidplay_len_s(void)     { return g_len_s; }

void vidplay_pause(int on)
{
    if (!atomic_load(&g_on))
        return;
    int want = on ? 1 : 0;
    if (want == atomic_load(&g_paused))
        return;
    if (want)
        atomic_store(&g_wall_frozen, master_clock());
    else
        atomic_store(&g_wall_base, now_s() - atomic_load(&g_wall_frozen));
    atomic_store(&g_paused, want);
}

long vidplay_pos_s(void)
{
    if (!atomic_load(&g_on))
        return -1;
    double c = master_clock();
    return c < 0 ? -1 : (long)c;
}

void vidplay_seek_rel(long delta_s)
{
    if (!atomic_load(&g_on) || delta_s == 0)
        return;
    atomic_fetch_add(&g_seek_delta, delta_s);
    atomic_store(&g_seek_req, 1);
    pthread_mutex_lock(&g_rq);
    pthread_cond_broadcast(&g_notfull);
    pthread_mutex_unlock(&g_rq);
}

const char *vidplay_meta(int which)
{
    if (which < 0 || which > 2)
        return "";
    return g_meta[which];
}

long vidplay_info(int what)
{
    switch (what) {
        /* From the container, so a front-end can lay out its window the moment
         * PLAY returns - g_vw/g_vh only exist once a frame has been decoded,
         * and a GEM app that asks straight away would get 0x0 and draw a
         * stretched rectangle. */
        case 0: return g_src_w ? g_src_w : g_vw;
        case 1: return g_src_h ? g_src_h : g_vh;
        case 2: return (long)(g_fps * 100.0 + 0.5);
        case 3: return g_astream ? 1 : 0;
        case 4: return g_hw;
        case 5: return g_volume;
        case 6: case 7: {
            /* Display geometry, so a GEM front-end can map its window's work
             * area (in Atari screen pixels) onto the real HDMI pixels the
             * overlay uses. Opens the plane if it is not open yet - cheap and
             * idempotent - so this works before the first PLAY. */
            if (vidplane_mode_w() == 0)
                vidplane_open();
            /* The GUEST IMAGE size, not the display size. A front-end scales
             * its window coordinates by (this / Atari screen size), and the
             * Atari screen is now drawn into a centred sub-rectangle rather
             * than the whole panel. resolve_rect() adds the origin back. */
            return (what == 6) ? (long)drmpres_dst_w()
                               : (long)drmpres_dst_h();
        }
        case 8: return atomic_load(&g_hidden);
        /* Smallest rectangle the overlay can actually scan out. A front-end
         * that asks for less gets it enlarged, which for a 4K source means
         * the whole screen - so it is better to know beforehand and not
         * bury its own controls under the picture. 0 = no constraint. */
        /* frames per second actually being shown, x100 */
        case 13: return atomic_load(&g_fps_shown);
        case 11: case 12: {
            int mw = 0, mh = 0;
            /* -1 = NOT KNOWN YET. The constraint depends on what the plane's
             * source turns out to be, which is only settled by the first
             * decoded frame - a front-end that asks the instant PLAY returns
             * must be told "ask again", not "no limit", or it will happily
             * show a picture that swallows its own controls. */
            if (!g_configured)
                return -1;
            hw_min_dst(&mw, &mh);
            return (what == 11) ? mw : mh;
        }
    }
    return -1;
}

/* x,y,w,h in DISPLAY pixels.
 *   w > 0 && h > 0 : show the picture in exactly that rectangle
 *   w == 0 && h == 0 : auto - aspect-correct letterbox filling the display
 *   w < 0 || h < 0 : hide the overlay, keep playing (audio continues) */
void vidplay_set_rect(int x, int y, int w, int h)
{
    /* Every one of these is a reason for the plane to move, and while paused
     * the present thread is the only thing that can move it - so tell it. */
    atomic_fetch_add(&g_geom_gen, 1);
    if (w < 0 || h < 0) {
        atomic_store(&g_hidden, 1);
        return;
    }
    atomic_store(&g_hidden, 0);
    g_rect_x = x; g_rect_y = y;
    g_rect_w = (w > 0 && h > 0) ? w : 0;
    g_rect_h = (w > 0 && h > 0) ? h : 0;
    if (g_vw > 0)
        resolve_rect();
}



/* x,y,w,h in DISPLAY pixels: the part of the destination that is actually
 * visible. w or h <= 0 removes the clip. */
void vidplay_set_clip(int x, int y, int w, int h)
{
    g_clip_x = x;
    g_clip_y = y;
    g_clip_w = (w > 0 && h > 0) ? w : 0;
    g_clip_h = (w > 0 && h > 0) ? h : 0;
    atomic_fetch_add(&g_geom_gen, 1);
}

/* ------------------------------------------------------------ capture blend */

int vidplay_capture_pending(void)
{
    if (!atomic_load(&g_on) || atomic_load(&g_hidden) || !g_configured)
        return 0;
    /* Distinguished from 0 so the caller can skip the framebuffer copy AND
     * still explain itself once, rather than quietly recording a hole. */
    return g_zerocopy ? -1 : 1;
}

int vidplay_capture_blend(void *dstv, int dst_stride, int dst_w, int dst_h)
{
    int mw = (int)vidplane_mode_w();
    int mh = (int)vidplane_mode_h();
    int dx, dy, dw, dh, sx, sy, sw, sh;
    uint8_t *dst_planes[4] = { NULL, NULL, NULL, NULL };
    int dst_lines[4] = { 0, 0, 0, 0 };
    AVFrame *f;
    int rc = 0;

    if (!dstv || dst_stride <= 0 || dst_w <= 0 || dst_h <= 0)
        return 0;
    if (!vidplay_capture_pending())
        return 0;
    if (g_zerocopy)
        return -1;                     /* SAND-tiled dmabuf: unreadable here */
    if (mw <= 0 || mh <= 0)
        return 0;

    /* Same geometry the plane is using, including the clip, so the recording
     * shows what the screen showed - a window in front of the picture stays in
     * front of it in the capture too. */
    dx = g_dx; dy = g_dy; dw = g_dw; dh = g_dh;
    if (!clip_rect(&dx, &dy, &dw, &dh, &sx, &sy, &sw, &sh))
        return 0;                      /* entirely covered */

    /* Display pixels -> framebuffer pixels. The DRM presenter scales the guest
     * plane across the whole display, so this is a straight ratio. Done in
     * 64-bit because 4K x a framebuffer width overflows 32. */
    {
        long long fx = (long long)dx * dst_w / mw;
        long long fy = (long long)dy * dst_h / mh;
        long long fw = (long long)dw * dst_w / mw;
        long long fh = (long long)dh * dst_h / mh;
        if (fw < 1 || fh < 1)
            return 0;
        if (fx < 0) { fw += fx; fx = 0; }
        if (fy < 0) { fh += fy; fy = 0; }
        if (fx + fw > dst_w) fw = dst_w - fx;
        if (fy + fh > dst_h) fh = dst_h - fy;
        if (fw < 1 || fh < 1)
            return 0;
        dx = (int)fx; dy = (int)fy; dw = (int)fw; dh = (int)fh;
    }

    pthread_mutex_lock(&g_cap_lock);
    f = g_cap_frame;
    if (!f || !f->data[0] || f->width <= 0 || f->height <= 0) {
        pthread_mutex_unlock(&g_cap_lock);
        return 0;
    }

    /* Rebuild the scaler whenever the source or the destination rect changes -
     * moving the window changes the latter on every drag. */
    if (!g_cap_sws || g_cap_sw != f->width || g_cap_sh != f->height ||
        g_cap_sfmt != f->format || g_cap_dw != dw || g_cap_dh != dh) {
        if (g_cap_sws)
            sws_freeContext(g_cap_sws);
        /* AV_PIX_FMT_BGRA is B,G,R,A in memory, which is what a little-endian
         * 0xAARRGGBB word looks like - i.e. the ARGB8888 framebuffer. */
        g_cap_sws = sws_getContext(f->width, f->height,
                                   (enum AVPixelFormat)f->format,
                                   dw, dh, AV_PIX_FMT_BGRA,
                                   SWS_FAST_BILINEAR, NULL, NULL, NULL);
        g_cap_sw = f->width; g_cap_sh = f->height; g_cap_sfmt = f->format;
        g_cap_dw = dw;       g_cap_dh = dh;
    }
    if (g_cap_sws) {
        /* Write straight into the destination rect: offset the pointer, keep
         * the full stride. No intermediate buffer, no second copy. */
        dst_planes[0] = (uint8_t *)dstv + (size_t)dy * dst_stride
                                        + (size_t)dx * 4;
        dst_lines[0]  = dst_stride;
        sws_scale(g_cap_sws, (const uint8_t * const *)f->data, f->linesize,
                  0, f->height, dst_planes, dst_lines);
        rc = 1;
    }
    pthread_mutex_unlock(&g_cap_lock);
    return rc;
}

void vidplay_set_volume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 200) percent = 200;
    g_volume = percent;
    if (g_astream)
        SDL_SetAudioStreamGain(g_astream, percent / 100.0f);
}

void vidplay_shutdown(void)
{
    vidplay_stop();
}
