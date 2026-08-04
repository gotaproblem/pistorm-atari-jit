/* SPDX-License-Identifier: MIT
 *
 * avrecord.c - live A/V screen recording, fully in-process.
 *
 * HARD RULE: the emulator never spawns external processes for media work.
 * Children inherit the RT/pinned scheduling of the emulator's threads and
 * starve (proven repeatedly with ffmpeg). So the recorder talks straight to
 * the Pi 4's hardware H.264 encoder via the V4L2 memory-to-memory API
 * (/dev/video11, bcm2835-codec) - a kernel device, not a process - and audio
 * (the SDL3 postmix tap: ST sound + MP3, exactly what you hear) goes to a
 * plain WAV file. Outputs in the capture directory:
 *
 *     capture.h264   raw H.264 elementary stream (hardware encoded)
 *     capture.wav    S16LE audio at the device rate
 *
 * Mux to a normal video file afterwards from a shell (where ffmpeg works):
 *
 *     ffmpeg -framerate 25 -i capture.h264 -i capture.wav -c copy capture.mkv
 *
 * Threading:
 *  - render thread: avrecord_video_frame() row-copies into a "latest" buffer
 *    under a mutex; no I/O, sub-ms.
 *  - SDL audio thread: avrecord_audio_push_f32() float->s16 into a ring.
 *  - writer thread (first act: SCHED_OTHER on all cores): paces a constant
 *    frame rate (duplicating the last frame while the display idles under the
 *    dirty gate), converts BGRA->NV12, runs the V4L2 encode loop, appends
 *    encoded bytes to capture.h264, drains the audio ring to capture.wav, and
 *    self-stops when the requested duration elapses (so recording ends even
 *    if the display is idle and no more frames arrive).
 *
 * Env: PISTORM_REC_FPS (default 25), PISTORM_REC_VB (bits/s, default 4000000),
 *      PISTORM_REC_DEV (default /dev/video11).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "avrecord.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <jpeglib.h>
#include <setjmp.h>

/* audio device format, provided by dmasnd_hdmi.c */
extern int dmasnd_out_freq(void);
extern int dmasnd_out_channels(void);

/* PNG encoder (et4000.c) - used by PNG capture mode */
extern int write_png_rgb(const char *path, const uint32_t *pixels,
                         uint32_t w, uint32_t h, uint32_t stride_px);

/* Capture mode: PISTORM_REC_MODE = mjpg (default) | png | h264.
 * ARM-only modes (mjpg/png, encoded on the nice-10 writer thread) are gentler
 * on the PiStorm bus timing than the VPU encoder, whose SDRAM DMA traffic
 * coincided with MFP vector glitches (exception 71) on heavy captures.
 *  - mjpg: libjpeg-turbo (NEON), ~25fps at FHD, all frames appended to ONE
 *          capture.mjpg file, ~5x smaller than PNG. Visually lossless for
 *          screen content at the default quality (PISTORM_REC_Q, 85).
 *  - png:  lossless frame files; slow (~10fps at FHD) and bulky.
 *  - h264: hardware VPU encode, near-zero CPU, when the system tolerates it.
 * Default fps: mjpg/h264 25, png 10 (PISTORM_REC_FPS overrides). */
enum { REC_MJPG, REC_PNG, REC_H264 };
static int rec_mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PISTORM_REC_MODE");
        if (e && strcasecmp(e, "h264") == 0)      v = REC_H264;
        else if (e && strcasecmp(e, "png") == 0)  v = REC_PNG;
        else                                      v = REC_MJPG;
    }
    return v;
}

/* ------------------------------------------------------------- state ---- */

#define NBUF 4

static atomic_int  g_armed = 0;
static atomic_int  g_running = 0;
static atomic_int  g_ok = 1;
static char        g_dir[512];
static int         g_secs = 10;

static int         g_w, g_h, g_fps;

static uint8_t    *g_latest = NULL;          /* packed BGRA, render thread */
static pthread_mutex_t g_fmx = PTHREAD_MUTEX_INITIALIZER;
/* dirty rect deferred because the writer held the lock (render never blocks) */
static int g_pend_valid = 0;
static int g_pend_x0, g_pend_y0, g_pend_x1, g_pend_y1;

#define ARING (1u << 19)
#define AMASK (ARING - 1)
static uint8_t     g_aring[ARING];
static atomic_uint g_ahead = 0, g_atail = 0;

static pthread_t   g_thr;
static int         g_thr_up = 0;

/* V4L2 encoder */
static int         g_vfd = -1;
static struct { void *p; size_t len; } g_obuf[NBUF], g_cbuf[NBUF];
static uint32_t    g_bpl, g_fmt_h, g_osize;

/* files */
static FILE       *g_fh264 = NULL;
static FILE       *g_fwav = NULL;
static FILE       *g_mjpg = NULL;            /* MJPG mode: single stream file */
static unsigned    g_png_frame = 0;          /* PNG mode frame counter */

/* ---- JPEG (libjpeg-turbo) frame append ---------------------------------- */

struct jerr_jmp { struct jpeg_error_mgr mgr; jmp_buf env; };

static void jerr_exit(j_common_ptr c)
{
    struct jerr_jmp *e = (struct jerr_jmp *)c->err;
    longjmp(e->env, 1);
}

/* Compress one BGRX frame and append it to g_mjpg. Concatenated JPEGs are a
 * valid MJPEG stream that ffmpeg reads natively. Returns 0 on success. */
static int mjpg_append(const uint8_t *frame, int w, int h)
{
    struct jpeg_compress_struct c;
    struct jerr_jmp err;
    unsigned char *buf = NULL;
    unsigned long buflen = 0;
    static int quality = -1;

    if (quality < 0) {
        const char *q = getenv("PISTORM_REC_Q");
        quality = q ? atoi(q) : 85;
        if (quality < 30 || quality > 100) quality = 85;
    }

    c.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jerr_exit;
    if (setjmp(err.env)) {
        jpeg_destroy_compress(&c);
        free(buf);
        return -1;
    }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &buf, &buflen);
    c.image_width = (JDIMENSION)w;
    c.image_height = (JDIMENSION)h;
    c.input_components = 4;
    c.in_color_space = JCS_EXT_BGRX;         /* our staging layout, no repack */
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = (JSAMPROW)(frame + (size_t)c.next_scanline * w * 4);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);

    size_t wr = fwrite(buf, 1, buflen, g_mjpg);
    free(buf);
    return wr == buflen ? 0 : -1;
}
static uint32_t    g_wav_bytes = 0;
static int         g_wav_freq = 48000, g_wav_ch = 2;

int avrecord_active(void) { return atomic_load(&g_armed) || atomic_load(&g_running); }
int avrecord_ok(void)     { return atomic_load(&g_ok); }
/* g_fps is only meaningful once the first frame has fixed the geometry and
 * started the writer; before that there is no answer, and 0 says so. */
int avrecord_fps(void)    { return atomic_load(&g_running) ? g_fps : 0; }

void avrecord_arm(const char *dir, int seconds)
{
    snprintf(g_dir, sizeof(g_dir), "%s", dir ? dir : ".");
    g_secs = seconds > 0 ? seconds : 10;
    atomic_store(&g_ok, 1);
    atomic_store(&g_armed, 1);
}

/* ------------------------------------------------------------- audio ---- */

void avrecord_audio_push_f32(const float *buf, int nsamples)
{
    if (!atomic_load(&g_running))
        return;
    unsigned head = atomic_load_explicit(&g_ahead, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&g_atail, memory_order_acquire);
    unsigned freeb = ARING - (head - tail) - 2;
    for (int i = 0; i < nsamples && freeb >= 2; i++, freeb -= 2) {
        float v = buf[i];
        if (v > 1.0f) v = 1.0f;
        else if (v < -1.0f) v = -1.0f;
        int16_t s = (int16_t)(v * 32767.0f);
        g_aring[head & AMASK]       = (uint8_t)(s & 0xff);
        g_aring[(head + 1) & AMASK] = (uint8_t)((s >> 8) & 0xff);
        head += 2;
    }
    atomic_store_explicit(&g_ahead, head, memory_order_release);
}

/* ------------------------------------------------------------- WAV ------ */

static void wav_write_header(FILE *f, int freq, int ch, uint32_t data_bytes)
{
    uint32_t byterate = (uint32_t)freq * ch * 2;
    uint16_t align = (uint16_t)(ch * 2);
    uint32_t riff = 36 + data_bytes;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmtlen = 16; memcpy(h + 16, &fmtlen, 4);
    uint16_t pcm = 1;     memcpy(h + 20, &pcm, 2);
    uint16_t nch = (uint16_t)ch; memcpy(h + 22, &nch, 2);
    uint32_t fr = (uint32_t)freq; memcpy(h + 24, &fr, 4);
    memcpy(h + 28, &byterate, 4);
    memcpy(h + 32, &align, 2);
    uint16_t bits = 16;   memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

static void drain_audio(void)
{
    unsigned tail = atomic_load_explicit(&g_atail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&g_ahead, memory_order_acquire);
    while (tail != head) {
        unsigned chunk = head - tail;
        unsigned lin = ARING - (tail & AMASK);
        if (chunk > lin) chunk = lin;
        fwrite(&g_aring[tail & AMASK], 1, chunk, g_fwav);
        g_wav_bytes += chunk;
        tail += chunk;
    }
    atomic_store_explicit(&g_atail, tail, memory_order_release);
}

/* ------------------------------------------------------------- V4L2 ----- */

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static int enc_open(int w, int h, int fps)
{
    const char *dev = getenv("PISTORM_REC_DEV");
    if (!dev || !*dev) dev = "/dev/video11";

    g_vfd = open(dev, O_RDWR | O_NONBLOCK);
    if (g_vfd < 0) {
        fprintf(stderr, "[AVREC] open %s: %s (no HW encoder? "
                "set PISTORM_REC_DEV or use PISTORM_RECORD_PNG=1)\n",
                dev, strerror(errno));
        return -1;
    }

    /* Stateful-encoder API order matters: the coded CAPTURE format must be
     * set FIRST (setting it resets the OUTPUT format as a side effect - doing
     * it the other way round silently wiped the NV12 geometry and the VPU
     * component then failed at STREAMON with ENOENT). */
    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width = (uint32_t)w;
    f.fmt.pix_mp.height = (uint32_t)h;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = 2 * 1024 * 1024;
    if (xioctl(g_vfd, VIDIOC_S_FMT, &f) != 0) {
        fprintf(stderr, "[AVREC] S_FMT(H264): %s\n", strerror(errno));
        return -1;
    }

    /* OUTPUT = the raw frames we feed, straight from the ARGB staging buffer
     * (memory bytes B,G,R,X) with no pixel conversion - the codec's ISP does
     * the CSC. The 32-bit RGB fourccs are the V4L2 legacy-ambiguity minefield
     * (VideoCore has historic R/B swaps), so the fourcc is env-selectable:
     * PISTORM_REC_FMT=XXXX (4 chars, e.g. BGR4 or AB24). */
    uint32_t reqfmt = v4l2_fourcc('A', 'B', '2', '4');  /* + R/B swap in copy */
    {
        const char *e = getenv("PISTORM_REC_FMT");
        if (e && strlen(e) == 4)
            reqfmt = v4l2_fourcc(e[0], e[1], e[2], e[3]);
    }
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.width = (uint32_t)w;
    f.fmt.pix_mp.height = (uint32_t)h;
    f.fmt.pix_mp.pixelformat = reqfmt;
    f.fmt.pix_mp.num_planes = 1;
    if (xioctl(g_vfd, VIDIOC_S_FMT, &f) != 0) {
        fprintf(stderr, "[AVREC] S_FMT(%.4s): %s\n",
                (const char *)&reqfmt, strerror(errno));
        return -1;
    }
    if (f.fmt.pix_mp.pixelformat != reqfmt) {
        fprintf(stderr, "[AVREC] driver refused %.4s input (got %.4s)\n",
                (const char *)&reqfmt, (const char *)&f.fmt.pix_mp.pixelformat);
        return -1;
    }
    g_bpl = f.fmt.pix_mp.plane_fmt[0].bytesperline;
    g_fmt_h = f.fmt.pix_mp.height;
    g_osize = f.fmt.pix_mp.plane_fmt[0].sizeimage;

    /* bitrate + frame rate */
    {
        const char *vb = getenv("PISTORM_REC_VB");
        struct v4l2_control c;
        memset(&c, 0, sizeof(c));
        c.id = V4L2_CID_MPEG_VIDEO_BITRATE;
        c.value = vb && *vb ? atoi(vb) : 4000000;
        if (c.value < 100000) c.value = 4000000;
        xioctl(g_vfd, VIDIOC_S_CTRL, &c);

        struct v4l2_streamparm sp;
        memset(&sp, 0, sizeof(sp));
        sp.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        sp.parm.output.timeperframe.numerator = 1;
        sp.parm.output.timeperframe.denominator = (uint32_t)fps;
        xioctl(g_vfd, VIDIOC_S_PARM, &sp);
    }

    /* buffers, both queues, mmap'd */
    for (int q = 0; q < 2; q++) {
        uint32_t type = q ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                          : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        struct v4l2_requestbuffers rb;
        memset(&rb, 0, sizeof(rb));
        rb.count = NBUF;
        rb.type = type;
        rb.memory = V4L2_MEMORY_MMAP;
        if (xioctl(g_vfd, VIDIOC_REQBUFS, &rb) != 0) {
            fprintf(stderr, "[AVREC] REQBUFS: %s\n", strerror(errno));
            return -1;
        }
        for (uint32_t i = 0; i < rb.count && i < NBUF; i++) {
            struct v4l2_buffer b;
            struct v4l2_plane pl;
            memset(&b, 0, sizeof(b));
            memset(&pl, 0, sizeof(pl));
            b.type = type;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            b.length = 1;
            b.m.planes = &pl;
            if (xioctl(g_vfd, VIDIOC_QUERYBUF, &b) != 0) {
                fprintf(stderr, "[AVREC] QUERYBUF(%s %u): %s\n",
                        q ? "cap" : "out", i, strerror(errno));
                return -1;
            }
            void *p = mmap(NULL, pl.length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, g_vfd, pl.m.mem_offset);
            if (p == MAP_FAILED) {
                fprintf(stderr, "[AVREC] mmap(%s %u, %u bytes): %s\n",
                        q ? "cap" : "out", i, pl.length, strerror(errno));
                return -1;
            }
            if (q) {
                g_cbuf[i].p = p; g_cbuf[i].len = pl.length;
                b.m.planes = &pl;                 /* pre-queue capture bufs */
                if (xioctl(g_vfd, VIDIOC_QBUF, &b) != 0) {
                    fprintf(stderr, "[AVREC] QBUF(cap %u): %s\n", i, strerror(errno));
                    return -1;
                }
            } else {
                g_obuf[i].p = p; g_obuf[i].len = pl.length;
            }
        }
    }

    /* Match ffmpeg's working sequence: STREAMON the capture side now, but the
     * OUTPUT side only after the first frame is queued (enc_feed) - streaming
     * an empty output queue is where the VPU component creation was failing
     * with ENOENT. */
    uint32_t t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(g_vfd, VIDIOC_STREAMON, &t) != 0) {
        fprintf(stderr, "[AVREC] STREAMON(cap): %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int g_out_streaming = 0;

static void enc_close(void)
{
    if (g_vfd < 0)
        return;
    uint32_t t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(g_vfd, VIDIOC_STREAMOFF, &t);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(g_vfd, VIDIOC_STREAMOFF, &t);
    for (int i = 0; i < NBUF; i++) {
        if (g_obuf[i].p) { munmap(g_obuf[i].p, g_obuf[i].len); g_obuf[i].p = NULL; }
        if (g_cbuf[i].p) { munmap(g_cbuf[i].p, g_cbuf[i].len); g_cbuf[i].p = NULL; }
    }
    close(g_vfd);
    g_vfd = -1;
    g_out_streaming = 0;
}

/* Copy a packed BGRX frame into the encoder buffer honoring its stride,
 * swapping R and B on the way: hardware tests showed the VideoCore consumes
 * AB24 as literal R,G,B,X (and mislabels BGR4 too), so no 32-bit fourcc
 * matches our B,G,R,X staging layout directly. The swap rides the copy pass
 * (NEON de-interleave/re-interleave), so it costs ~1 extra shuffle, not an
 * extra pass. PISTORM_REC_SWAP=0 disables it for firmwares that differ. */
/* Hardware-verified on Pi 4 (bcm2835-codec): AB24 consumes our staging bytes
 * (B,G,R,X) as-is - colours correct with NO swap, so the default is a pure
 * memcpy feed. PISTORM_REC_SWAP=1 enables an R/B swap for firmwares that
 * interpret the fourcc the other way. */
static int rec_swap_rb(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PISTORM_REC_SWAP");
        v = (e && *e == '1') ? 1 : 0;
    }
    return v;
}

static void row_copy_swap(uint8_t *dst, const uint8_t *src, size_t rowbytes)
{
    size_t x = 0;
#if defined(__aarch64__)
    for (; x + 64 <= rowbytes; x += 64) {
        uint8x16x4_t v = vld4q_u8(src + x);      /* planes B,G,R,X */
        uint8x16_t t = v.val[0];                 /* swap B <-> R   */
        v.val[0] = v.val[2];
        v.val[2] = t;
        vst4q_u8(dst + x, v);
    }
#endif
    for (; x + 4 <= rowbytes; x += 4) {
        dst[x]     = src[x + 2];
        dst[x + 1] = src[x + 1];
        dst[x + 2] = src[x];
        dst[x + 3] = src[x + 3];
    }
}

static void frame_to_encbuf(const uint8_t *src, uint8_t *dst, int w, int h)
{
    size_t rowbytes = (size_t)w * 4;
    if (!rec_swap_rb()) {
        if (g_bpl == rowbytes) {
            memcpy(dst, src, rowbytes * (size_t)h);
            return;
        }
        for (int y = 0; y < h; y++)
            memcpy(dst + (size_t)y * g_bpl, src + (size_t)y * rowbytes, rowbytes);
        return;
    }
    for (int y = 0; y < h; y++)
        row_copy_swap(dst + (size_t)y * g_bpl, src + (size_t)y * rowbytes, rowbytes);
}

/* Reap any finished encoded buffers -> capture.h264, requeue them. */
static void enc_drain(void)
{
    for (;;) {
        struct v4l2_buffer b;
        struct v4l2_plane pl;
        memset(&b, 0, sizeof(b));
        memset(&pl, 0, sizeof(pl));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.length = 1;
        b.m.planes = &pl;
        if (xioctl(g_vfd, VIDIOC_DQBUF, &b) != 0)
            return;                                   /* EAGAIN: none ready */
        if (pl.bytesused)
            fwrite(g_cbuf[b.index].p, 1, pl.bytesused, g_fh264);
        memset(&pl, 0, sizeof(pl));
        b.m.planes = &pl;
        xioctl(g_vfd, VIDIOC_QBUF, &b);               /* recycle */
    }
}

static int enc_feed(const uint8_t *bgra, int w, int h, int index)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl;

    frame_to_encbuf(bgra, (uint8_t *)g_obuf[index].p, w, h);

    memset(&b, 0, sizeof(b));
    memset(&pl, 0, sizeof(pl));
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = (uint32_t)index;
    b.length = 1;
    pl.bytesused = g_osize;
    b.m.planes = &pl;
    if (xioctl(g_vfd, VIDIOC_QBUF, &b) != 0) {
        static int qerr = 0;
        if (!qerr) { qerr = 1;
            fprintf(stderr, "[AVREC] QBUF(out): %s\n", strerror(errno)); }
        return -1;
    }
    if (!g_out_streaming) {           /* first frame queued: start the input side */
        uint32_t t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (xioctl(g_vfd, VIDIOC_STREAMON, &t) != 0) {
            fprintf(stderr, "[AVREC] STREAMON(out): %s\n", strerror(errno));
            atomic_store(&g_ok, 0);
            return -1;
        }
        g_out_streaming = 1;
    }
    return 0;
}

/* ------------------------------------------------------------- writer --- */

static void *writer_thread(void *arg)
{
    (void)arg;
    {   /* escape inherited pinning / RT class */
        cpu_set_t all; CPU_ZERO(&all);
        long n = sysconf(_SC_NPROCESSORS_CONF);
        if (n < 1) n = 4;
        for (long i = 0; i < n && i < CPU_SETSIZE; i++) CPU_SET((int)i, &all);
        sched_setaffinity(0, sizeof(all), &all);
        struct sched_param sp; memset(&sp, 0, sizeof(sp));
        sched_setscheduler(0, SCHED_OTHER, &sp);
    }

    /* be a background citizen: normal class, low priority */
    setpriority(PRIO_PROCESS, 0, 10);

    struct timespec next, start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    next = start;
    const long step_ns = 1000000000L / (g_fps > 0 ? g_fps : 25);
    int oidx = 0, queued = 0;

    while (atomic_load(&g_running)) {
        next.tv_nsec += step_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        if (rec_mode() != REC_H264) {
            /* ARM-encode modes: compress on this (nice-10) thread while
             * holding the frame lock - the render thread uses trylock and
             * just defers its rect, so a long encode never stalls rendering. */
            int rc;
            if (rec_mode() == REC_MJPG) {
                pthread_mutex_lock(&g_fmx);
                rc = mjpg_append(g_latest, g_w, g_h);
                pthread_mutex_unlock(&g_fmx);
                if (rc == 0)
                    g_png_frame++;              /* frame counter (any mode) */
                else
                    fprintf(stderr, "[AVREC] JPEG append failed\n");
            } else {
                char fp[640];
                snprintf(fp, sizeof(fp), "%s/frame_%04u.png", g_dir, g_png_frame);
                pthread_mutex_lock(&g_fmx);
                rc = write_png_rgb(fp, (const uint32_t *)g_latest,
                                   (uint32_t)g_w, (uint32_t)g_h, (uint32_t)g_w);
                pthread_mutex_unlock(&g_fmx);
                if (rc == 0)
                    g_png_frame++;
                else
                    fprintf(stderr, "[AVREC] PNG write failed: %s\n", fp);
            }
            if (rc != 0) {
                atomic_store(&g_ok, 0);
                atomic_store(&g_running, 0);
            }
            drain_audio();
            struct timespec pnow;
            clock_gettime(CLOCK_MONOTONIC, &pnow);
            if (pnow.tv_sec - start.tv_sec >= g_secs &&
                (pnow.tv_sec - start.tv_sec > g_secs || pnow.tv_nsec >= start.tv_nsec))
                break;
            continue;
        }

        /* free a source buffer if the encoder is holding them all */
        if (queued >= NBUF) {
            struct v4l2_buffer b; struct v4l2_plane pl;
            struct pollfd pf = { .fd = g_vfd, .events = POLLOUT | POLLIN };
            poll(&pf, 1, 100);
            memset(&b, 0, sizeof(b)); memset(&pl, 0, sizeof(pl));
            b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            b.memory = V4L2_MEMORY_MMAP;
            b.length = 1; b.m.planes = &pl;
            if (xioctl(g_vfd, VIDIOC_DQBUF, &b) == 0)
                queued--;
        } else {
            struct v4l2_buffer b; struct v4l2_plane pl;
            memset(&b, 0, sizeof(b)); memset(&pl, 0, sizeof(pl));
            b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            b.memory = V4L2_MEMORY_MMAP;
            b.length = 1; b.m.planes = &pl;
            while (xioctl(g_vfd, VIDIOC_DQBUF, &b) == 0) {
                queued--;
                memset(&pl, 0, sizeof(pl)); b.m.planes = &pl;
            }
        }
        if (queued < NBUF) {
            /* feed straight from g_latest under the lock; the render thread
             * uses trylock and defers its rect on contention, so holding it
             * for the encoder copy never stalls rendering */
            pthread_mutex_lock(&g_fmx);
            int fed = enc_feed(g_latest, g_w, g_h, oidx);
            pthread_mutex_unlock(&g_fmx);
            if (fed == 0) {
                oidx = (oidx + 1) % NBUF;
                queued++;
            }
        }
        enc_drain();
        drain_audio();

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= g_secs &&
            (now.tv_sec - start.tv_sec > g_secs || now.tv_nsec >= start.tv_nsec))
            break;                                    /* duration reached */
    }

    /* final reap so the tail of the stream lands in the file */
    usleep(100000);
    enc_drain();
    drain_audio();
    atomic_store(&g_running, 0);
    return NULL;
}

/* ------------------------------------------------------------- frames --- */

void avrecord_video_frame(const void *fb, int stride_bytes, int w, int h,
                          int dx0, int dy0, int dx1, int dy1)
{
    if (!fb || w <= 0 || h <= 0)
        return;

    if (!atomic_load(&g_running)) {
        if (!atomic_load(&g_armed))
            return;
        int mode = rec_mode();
        const char *fe = getenv("PISTORM_REC_FPS");
        g_fps = fe ? atoi(fe) : (mode == REC_PNG ? 10 : 25);
        if (g_fps < 1 || g_fps > 60) g_fps = mode == REC_PNG ? 10 : 25;
        g_w = w; g_h = h;
        g_png_frame = 0;

        char path[600];
        g_fh264 = NULL;
        g_mjpg = NULL;
        if (mode == REC_H264) {
            snprintf(path, sizeof(path), "%s/capture.h264", g_dir);
            g_fh264 = fopen(path, "wb");
            if (!g_fh264)
                fprintf(stderr, "[AVREC] fopen %s: %s\n", path, strerror(errno));
        } else if (mode == REC_MJPG) {
            snprintf(path, sizeof(path), "%s/capture.mjpg", g_dir);
            g_mjpg = fopen(path, "wb");
            if (!g_mjpg)
                fprintf(stderr, "[AVREC] fopen %s: %s\n", path, strerror(errno));
        }
        snprintf(path, sizeof(path), "%s/capture.wav", g_dir);
        g_fwav = fopen(path, "wb");
        if (!g_fwav)
            fprintf(stderr, "[AVREC] fopen %s: %s\n", path, strerror(errno));
        g_latest = malloc((size_t)w * h * 4);
        if (!g_latest)
            fprintf(stderr, "[AVREC] malloc %dx%d frame failed\n", w, h);
        g_wav_freq = dmasnd_out_freq() > 0 ? dmasnd_out_freq() : 48000;
        g_wav_ch = dmasnd_out_channels() > 0 ? dmasnd_out_channels() : 2;
        g_wav_bytes = 0;

        if ((mode == REC_H264 && !g_fh264) || (mode == REC_MJPG && !g_mjpg) ||
            !g_fwav || !g_latest ||
            (mode == REC_H264 && enc_open(w, h, g_fps) != 0)) {
            fprintf(stderr, "[AVREC] recorder init failed\n");
            if (g_fh264) { fclose(g_fh264); g_fh264 = NULL; }
            if (g_mjpg)  { fclose(g_mjpg);  g_mjpg = NULL; }
            if (g_fwav)  { fclose(g_fwav);  g_fwav = NULL; }
            free(g_latest); g_latest = NULL;
            enc_close();
            atomic_store(&g_armed, 0);
            atomic_store(&g_ok, 0);
            return;
        }
        wav_write_header(g_fwav, g_wav_freq, g_wav_ch, 0);  /* patched at stop */
        memset(g_latest, 0, (size_t)w * h * 4);
        atomic_store(&g_armed, 0);
        atomic_store(&g_running, 1);
        atomic_store(&g_ahead, 0);
        atomic_store(&g_atail, 0);
        if (pthread_create(&g_thr, NULL, writer_thread, NULL) != 0) {
            atomic_store(&g_running, 0);
            atomic_store(&g_ok, 0);
            return;
        }
        g_thr_up = 1;
        g_pend_valid = 1;                       /* first copy must be full */
        g_pend_x0 = 0; g_pend_y0 = 0;
        g_pend_x1 = w - 1; g_pend_y1 = h - 1;
        fprintf(stderr, "[AVREC] recording %dx%d@%dfps (%s) + %dHz WAV -> %s\n",
                w, h, g_fps,
                mode == REC_MJPG ? "MJPEG" : mode == REC_PNG ? "PNG frames" : "HW H.264",
                g_wav_freq, g_dir);
    }

    if (w != g_w || h != g_h) {
        fprintf(stderr, "[AVREC] frame size changed (%dx%d -> %dx%d), stopping\n",
                g_w, g_h, w, h);
        avrecord_stop();
        return;
    }

    /* clamp this frame's dirty rect */
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 >= w || dx1 < dx0) dx1 = w - 1;
    if (dy1 >= h || dy1 < dy0) dy1 = h - 1;

    /* NEVER block the render thread: if the writer holds the buffer (it holds
     * it only for the encoder copy), defer this rect and merge it next time.
     * The IKBD/mouse constraint is absolute - starving the CPU thread with
     * render-side stalls loses ACIA bytes and desyncs the mouse protocol. */
    if (pthread_mutex_trylock(&g_fmx) != 0) {
        if (!g_pend_valid) {
            g_pend_x0 = dx0; g_pend_y0 = dy0;
            g_pend_x1 = dx1; g_pend_y1 = dy1;
            g_pend_valid = 1;
        } else {
            if (dx0 < g_pend_x0) g_pend_x0 = dx0;
            if (dy0 < g_pend_y0) g_pend_y0 = dy0;
            if (dx1 > g_pend_x1) g_pend_x1 = dx1;
            if (dy1 > g_pend_y1) g_pend_y1 = dy1;
        }
        return;
    }

    /* merge any deferred rect, then copy only the dirty region */
    if (g_pend_valid) {
        if (g_pend_x0 < dx0) dx0 = g_pend_x0;
        if (g_pend_y0 < dy0) dy0 = g_pend_y0;
        if (g_pend_x1 > dx1) dx1 = g_pend_x1;
        if (g_pend_y1 > dy1) dy1 = g_pend_y1;
        g_pend_valid = 0;
    }
    {
        const uint8_t *src = (const uint8_t *)fb;
        uint8_t *dst = g_latest;
        size_t xoff = (size_t)dx0 * 4;
        size_t xlen = (size_t)(dx1 - dx0 + 1) * 4;
        for (int y = dy0; y <= dy1; y++)
            memcpy(dst + (size_t)y * w * 4 + xoff,
                   src + (size_t)y * stride_bytes + xoff, xlen);
    }
    pthread_mutex_unlock(&g_fmx);
}

/* ------------------------------------------------------------- stop ----- */

void avrecord_stop(void)
{
    atomic_store(&g_armed, 0);
    if (!g_thr_up && g_vfd < 0)
        return;

    atomic_store(&g_running, 0);
    if (g_thr_up) { pthread_join(g_thr, NULL); g_thr_up = 0; }

    enc_close();

    if (g_fwav) {
        wav_write_header(g_fwav, g_wav_freq, g_wav_ch, g_wav_bytes);
        fclose(g_fwav);
        g_fwav = NULL;
    }
    if (g_fh264) { fclose(g_fh264); g_fh264 = NULL; }
    if (g_mjpg)  { fclose(g_mjpg);  g_mjpg = NULL; }

    pthread_mutex_lock(&g_fmx);
    free(g_latest);
    g_latest = NULL;
    pthread_mutex_unlock(&g_fmx);

    /* Completion marker for the external mux watcher (capmux.sh): written
     * LAST, so its presence guarantees capture.h264/capture.wav are final.
     * The watcher (a normal process outside the emulator) does the ffmpeg
     * mux; the emulator itself never spawns anything. */
    {
        char path[600];
        snprintf(path, sizeof(path), "%s/complete", g_dir);
        FILE *m = fopen(path, "w");
        if (m) {
            fprintf(m, "fps=%d\nwidth=%d\nheight=%d\naudio_hz=%d\naudio_ch=%d\n"
                    "format=%s\nframes=%u\n",
                    g_fps > 0 ? g_fps : 25, g_w, g_h, g_wav_freq, g_wav_ch,
                    rec_mode() == REC_MJPG ? "mjpg" :
                    rec_mode() == REC_PNG  ? "png"  : "h264", g_png_frame);
            fclose(m);
        }
    }
    /* The emulator runs under sudo, so everything here is root-owned; hand
     * the capture back to the invoking user so capmux.sh works without sudo. */
    {
        const char *su = getenv("SUDO_UID"), *sg = getenv("SUDO_GID");
        if (su && sg) {
            uid_t u = (uid_t)atoi(su);
            gid_t g = (gid_t)atoi(sg);
            const char *names[] = { "", "/capture.h264", "/capture.mjpg",
                                    "/capture.wav", "/complete" };
            char path[600];
            for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
                snprintf(path, sizeof(path), "%s%s", g_dir, names[i]);
                if (chown(path, u, g) != 0) { /* best effort */ }
            }
            for (unsigned i = 0; i < g_png_frame; i++) {
                snprintf(path, sizeof(path), "%s/frame_%04u.png", g_dir, i);
                if (chown(path, u, g) != 0) { /* best effort */ }
            }
        }
    }
    fprintf(stderr, "[AVREC] done -> %s (capture.h264 + capture.wav)\n", g_dir);
}
