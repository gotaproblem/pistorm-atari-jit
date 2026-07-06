/*
 * et4000.c  —  Tseng Labs ET4000AX emulator for PiStorm Atari ST
 *
 * Interfaces:
 *   - Musashi 68000 memory callbacks (read/write hooks)
 *   - SDL2 window + streaming texture for display output
 *
 * Supported modes:
 *   - 1bpp mono  (NOVA/EmuTOS: flat linear bitmap, 1 byte = 8 pixels)
 *   - 4bpp planar 16-colour (EGA/VGA planar)
 *   - 8bpp packed 256-colour (SVGA linear)
 *   - 16bpp packed HiColor
 *   - 32bpp packed TrueColor
 *
 * Display:
 *   - SDL2 KMSDRM: renders directly to the HDMI connector via DRM/KMS, with
 *     no X11/Wayland/window-manager — the SDL2 replacement for the old
 *     /dev/fb0 mmap path.
 *   - Each blit decodes VRAM and scales the ET4000 logical content into a
 *     CPU-side ARGB8888 staging buffer (s->fb_mem) sized to the HDMI output.
 *   - One streaming texture is uploaded and presented per frame on the render
 *     thread (the only thread that touches SDL).
 *
 * Memory map (Atari ST / PiStorm):
 *   0xA00000-0xBFFFFF  VRAM alt aperture (mono)
 *   0xC00000-0xCFFFFF  VRAM main aperture (1MB)
 *   0xD00000-0xD0FFFF  I/O registers (port = addr & 0xFFFF)
 *
 * Known facts from testing:
 *   - NOVA mono writes flat 1bpp to 0xC00000 linearly
 *   - bytes_per_row = screen_width / 8
 *   - crtc[0x13] = 0x28 = 40; bpl = 40*8 = 320; plane_stride = 80
 *   - fb_stride must be read back from kernel after mode set
 *   - dirty flag unreliable across threads — always blit
 *   - mutex required: fb_set_mode vs blit thread race
 */

#include "et4000.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <linux/fb.h>

#include <SDL2/SDL.h>
#include <zlib.h>

#ifdef __cplusplus
extern "C" {
#endif
extern volatile uint8_t g_irq;
extern volatile uint8_t g_irq_mask;
#ifdef __cplusplus
}
#endif

/* -----------------------------------------------------------------------
 * Internal state
 * ----------------------------------------------------------------------- */

ET4000State *g_et4000 = NULL; /* non-static: used by Musashi callbacks */
uint8_t *fb_ptr;
uint8_t *vram_ptr;
volatile int et4000_thread_ready = 0;
pthread_mutex_t et4000_engine_mutex = PTHREAD_MUTEX_INITIALIZER;

/* -----------------------------------------------------------------------
 * SDL2 output backend
 *
 * The window/renderer/texture are owned by the render thread (render_frame),
 * which is also the only thread that calls et4000_init and et4000_update_
 * display, so every SDL call happens on one thread as SDL requires.
 *
 * s->fb_mem is a plain ARGB8888 staging buffer sized to the renderer output;
 * the blit functions fill it exactly as they filled the mmap'd framebuffer.
 * ----------------------------------------------------------------------- */
static SDL_Window *g_sdl_win = NULL;
static SDL_Renderer *g_sdl_ren = NULL;
static SDL_Texture *g_sdl_tex = NULL;
static int g_sdl_tex_has_frame = 0;

/* Native-resolution staging: the blits decode 1:1 into g_logical at the
 * mode's source resolution; the texture is sized to match; the GPU does the
 * upscale + aspect-correct letterbox to the HDMI output via the renderer's
 * logical size. Sized for the largest mode we support (1024x768). */
#define ET4K_MAX_LW 1280
#define ET4K_MAX_LH 1024
#define ET4K_ROW_PAD 64
#define ET4K_STAGE_PITCH (ET4K_MAX_LW + ET4K_ROW_PAD * 2)
static uint32_t *g_logical = NULL;          /* padded ET4K_STAGE_PITCH * ET4K_MAX_LH XRGB */
static uint32_t g_tex_w = 0, g_tex_h = 0;   /* current texture (native) size */
static uint32_t g_disp_w = 0, g_disp_h = 0; /* current renderer logical size  */

static int g_fbdev_mode = 0;
static int g_fbdev_fd = -1;
static uint8_t *g_fbdev_mem = NULL;
static size_t g_fbdev_size = 0;
static uint32_t g_fbdev_w = 0, g_fbdev_h = 0, g_fbdev_stride = 0;

/* Screendump is requested from any thread but serviced on the render thread
 * (inside sdl_present, between RenderCopy and RenderPresent) so the readback
 * is valid and SDL is only touched from its owning thread. */
static volatile int g_screendump_req = 0;
static int save_png_rgb(const char *path, const uint32_t *pixels,
                        uint32_t w, uint32_t h, uint32_t stride_px);

// static pthread_mutex_t fb_mutex = PTHREAD_MUTEX_INITIALIZER;

static void et4000_configure_render_thread(void)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    setpriority(PRIO_PROCESS, 0, 19);
#ifdef SCHED_IDLE
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    sched_setscheduler(0, SCHED_IDLE, &param);
#endif
}

static int et4000_frame_interval_us(void)
{
    int fps = 25;
    const char *env = getenv("PISTORM_VGA_FPS");
    if (env && *env)
    {
        int requested = atoi(env);
        if (requested >= 10 && requested <= 60)
            fps = requested;
    }
    return 1000000 / fps;
}

static int et4000_present_divisor(void)
{
    int divisor = 1;
    const char *env = getenv("PISTORM_VGA_PRESENT_DIV");
    if (env && *env)
    {
        int requested = atoi(env);
        if (requested >= 1 && requested <= 10)
            divisor = requested;
    }
    return divisor;
}

static int et4000_no_render(void)
{
    const char *env = getenv("PISTORM_VGA_NORENDER");
    return env && *env && strcmp(env, "0") != 0;
}

static int et4000_irq_backoff_enabled(void)
{
    const char *env = getenv("PISTORM_VGA_IRQ_BACKOFF");
    return env && *env && strcmp(env, "0") != 0;
}

static inline int et4000_irq_pending_for_cpu(void)
{
    uint8_t irq = g_irq;
    uint8_t mask = g_irq_mask;

    return irq != 0 && irq > mask;
}

static int et4000_direct_texture(void)
{
    const char *env = getenv("PISTORM_VGA_DIRECT_TEXTURE");
    return env && *env && strcmp(env, "0") != 0;
}

typedef struct {
    uint64_t calls;
    uint64_t total_ns;
    uint64_t max_ns;
} et4000_prof_counter_t;

enum {
    ET4K_PROF_VRAM_RD_DIRECT,
    ET4K_PROF_VRAM_WR_DIRECT,
    ET4K_PROF_VRAM_RD_ENGINE,
    ET4K_PROF_VRAM_WR_ENGINE,
    ET4K_PROF_IO_RD_WAIT,
    ET4K_PROF_IO_RD_TOTAL,
    ET4K_PROF_IO_WR_WAIT,
    ET4K_PROF_IO_WR_TOTAL,
    ET4K_PROF_RENDER_BUILD,
    ET4K_PROF_SDL_UPDATE,
    ET4K_PROF_SDL_CLEAR,
    ET4K_PROF_SDL_COPY,
    ET4K_PROF_SDL_PRESENT,
    ET4K_PROF_PRESENT_TOTAL,
    ET4K_PROF_COUNT
};

static et4000_prof_counter_t et4000_prof[ET4K_PROF_COUNT];

static int et4000_profile_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char *env = getenv("PISTORM_VGA_PROFILE");
        enabled = env && *env && strcmp(env, "0") != 0;
    }
    return enabled;
}

static uint64_t et4000_profile_now_ns(void)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void et4000_profile_add(unsigned idx, uint64_t ns)
{
    static uint64_t next_print_ns;
    static const char *names[ET4K_PROF_COUNT] = {
        "vram_rd_direct", "vram_wr_direct",
        "vram_rd_engine", "vram_wr_engine",
        "io_rd_wait", "io_rd_total",
        "io_wr_wait", "io_wr_total",
        "render_build",
        "sdl_update", "sdl_clear", "sdl_copy", "sdl_present",
        "present_total"
    };
    uint64_t now;

    if (!et4000_profile_enabled() || idx >= ET4K_PROF_COUNT)
        return;

    et4000_prof[idx].calls++;
    et4000_prof[idx].total_ns += ns;
    if (ns > et4000_prof[idx].max_ns)
        et4000_prof[idx].max_ns = ns;

    now = et4000_profile_now_ns();
    if (!next_print_ns)
        next_print_ns = now + 1000000000ULL;
    if (now < next_print_ns)
        return;

    fprintf(stderr, "[VGAPROF/s]");
    for (unsigned i = 0; i < ET4K_PROF_COUNT; i++)
    {
        uint64_t calls = et4000_prof[i].calls;
        if (!calls)
            continue;
        fprintf(stderr, " %s n=%llu avg=%lluns max=%lluns total=%lluus",
                names[i],
                (unsigned long long)calls,
                (unsigned long long)(et4000_prof[i].total_ns / calls),
                (unsigned long long)et4000_prof[i].max_ns,
                (unsigned long long)(et4000_prof[i].total_ns / 1000ULL));
        memset(&et4000_prof[i], 0, sizeof(et4000_prof[i]));
    }
    fprintf(stderr, "\n");
    next_print_ns = now + 1000000000ULL;
}

static int fbdev_open_output(const char *dev)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    const char *path = dev ? dev : "/dev/fb0";

    g_fbdev_fd = open(path, O_RDWR);
    if (g_fbdev_fd < 0)
    {
        fprintf(stderr, "et4000: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (ioctl(g_fbdev_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(g_fbdev_fd, FBIOGET_FSCREENINFO, &finfo) < 0)
    {
        fprintf(stderr, "et4000: fbdev ioctl failed: %s\n", strerror(errno));
        close(g_fbdev_fd);
        g_fbdev_fd = -1;
        return -1;
    }

    if (vinfo.bits_per_pixel != 32)
        fprintf(stderr, "et4000: fbdev is %ubpp, expected 32bpp\n", vinfo.bits_per_pixel);

    g_fbdev_w = vinfo.xres;
    g_fbdev_h = vinfo.yres;
    g_fbdev_stride = finfo.line_length;
    g_fbdev_size = finfo.smem_len;
    if (g_fbdev_stride == 0 || g_fbdev_w == 0 || g_fbdev_h == 0)
    {
        fprintf(stderr, "et4000: invalid fbdev geometry %ux%u stride=%u\n",
                g_fbdev_w, g_fbdev_h, g_fbdev_stride);
        close(g_fbdev_fd);
        g_fbdev_fd = -1;
        return -1;
    }
    size_t visible_size = (size_t)g_fbdev_stride * g_fbdev_h;
    if (g_fbdev_size == 0 || g_fbdev_size > visible_size)
        g_fbdev_size = visible_size;
    uint32_t mapped_h = (uint32_t)(g_fbdev_size / g_fbdev_stride);
    if (mapped_h < g_fbdev_h)
    {
        fprintf(stderr, "et4000: fbdev mapped height clamped %u->%u size=%zu stride=%u\n",
                g_fbdev_h, mapped_h, g_fbdev_size, g_fbdev_stride);
        g_fbdev_h = mapped_h;
    }
    if (g_fbdev_h == 0)
    {
        fprintf(stderr, "et4000: fbdev mapping too small size=%zu stride=%u\n",
                g_fbdev_size, g_fbdev_stride);
        close(g_fbdev_fd);
        g_fbdev_fd = -1;
        return -1;
    }
    g_fbdev_mem = (uint8_t *)mmap(NULL, g_fbdev_size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, g_fbdev_fd, 0);
    if (g_fbdev_mem == MAP_FAILED)
    {
        fprintf(stderr, "et4000: fbdev mmap failed: %s\n", strerror(errno));
        g_fbdev_mem = NULL;
        close(g_fbdev_fd);
        g_fbdev_fd = -1;
        return -1;
    }

    memset(g_fbdev_mem, 0, g_fbdev_size);
    fprintf(stderr, "et4000: fbdev output %ux%u stride=%u bpp=%u\n",
            g_fbdev_w, g_fbdev_h, g_fbdev_stride, vinfo.bits_per_pixel);
    return 0;
}

static void fbdev_present(ET4000State *s)
{
    if (!g_fbdev_mem || !s->fb_mem || s->fb_width == 0 || s->fb_height == 0)
        return;

    uint32_t src_w = s->fb_width;
    uint32_t src_h = s->fb_height;
    uint32_t dst_w = g_fbdev_w;
    uint32_t dst_h = g_fbdev_h;

    if (dst_w * src_h > dst_h * src_w)
        dst_w = (dst_h * src_w) / src_h;
    else
        dst_h = (dst_w * src_h) / src_w;

    if (dst_w == 0 || dst_h == 0)
        return;

    uint32_t off_x = (g_fbdev_w - dst_w) / 2;
    uint32_t off_y = (g_fbdev_h - dst_h) / 2;
    memset(g_fbdev_mem, 0, g_fbdev_size);

    const uint32_t *src = (const uint32_t *)s->fb_mem;
    uint32_t src_pitch = s->fb_stride ? (uint32_t)(s->fb_stride / 4) : src_w;
    for (uint32_t y = 0; y < dst_h; y++)
    {
        uint32_t sy = (y * src_h) / dst_h;
        size_t dst_off = (size_t)(off_y + y) * g_fbdev_stride + (size_t)off_x * 4;
        if (dst_off + (size_t)dst_w * 4 > g_fbdev_size)
            break;
        uint32_t *dst = (uint32_t *)(g_fbdev_mem + dst_off);
        const uint32_t *row = src + sy * src_pitch;
        for (uint32_t x = 0; x < dst_w; x++)
            dst[x] = row[(x * src_w) / dst_w];
    }
}

static void fbdev_close_output(void)
{
    if (g_fbdev_mem)
    {
        munmap(g_fbdev_mem, g_fbdev_size);
        g_fbdev_mem = NULL;
    }
    if (g_fbdev_fd >= 0)
    {
        close(g_fbdev_fd);
        g_fbdev_fd = -1;
    }
}

/* ===================================================================
 * PCem ET4000AX engine bridge — replaces the hand-rolled core.
 * Implemented in pcem/et4000_engine.c.  See pcem/INTEGRATION.md.
 * =================================================================== */
void et4000_engine_init(void);
void et4000_engine_io_write(uint32_t port, uint8_t val);
uint8_t et4000_engine_io_read(uint32_t port);
void et4000_engine_io_write16(uint32_t port, uint16_t val);
void et4000_engine_io_write32(uint32_t port, uint32_t val);
uint16_t et4000_engine_io_read16(uint32_t port);
uint32_t et4000_engine_io_read32(uint32_t port);
void et4000_engine_vram_write8(uint32_t off, uint8_t v);
void et4000_engine_vram_write16(uint32_t off, uint16_t v);
void et4000_engine_vram_write32(uint32_t off, uint32_t v);
uint8_t et4000_engine_vram_read8(uint32_t off);
uint16_t et4000_engine_vram_read16(uint32_t off);
uint32_t et4000_engine_vram_read32(uint32_t off);
int et4000_engine_current_size(int *out_w, int *out_h);
void et4000_engine_render(uint32_t *argb_dst, int *out_w, int *out_h);
void et4000_engine_render_direct(uint32_t *visible_dst, int pitch_px, int left_pad_px,
                                 int *out_w, int *out_h);
int et4000_engine_visible_nonzero(void);
uint8_t *et4000_engine_vram_ptr(void);
int et4000_engine_direct_vram_ok(void);
void et4000_engine_set_vram(uint8_t *base, uint32_t size);

volatile uint8_t _VSYNC;
/* -----------------------------------------------------------------------
 * Framebuffer
 * ----------------------------------------------------------------------- */

/* -----------------------------------------------------------------------
 * SDL display backend
 * ----------------------------------------------------------------------- */

static int sdl_open(ET4000State *s, const char *unused_dev)
{
    g_fbdev_mode = getenv("PISTORM_VGA_FBDEV") ? 1 : 0;
    if (g_fbdev_mode)
    {
        if (fbdev_open_output(unused_dev) < 0)
            return -1;

        g_logical = (uint32_t *)calloc((size_t)ET4K_STAGE_PITCH * ET4K_MAX_LH, 4);
        if (!g_logical)
        {
            fprintf(stderr, "et4000: staging buffer alloc failed\n");
            fbdev_close_output();
            return -1;
        }

        s->fb_mem = (uint8_t *)(g_logical + ET4K_ROW_PAD);
        s->fb_size = (size_t)ET4K_STAGE_PITCH * ET4K_MAX_LH * 4;
        s->fb_width = 0;
        s->fb_height = 0;
        s->fb_stride = 0;
        s->fb_bpp = 32;
        s->fb_fd = -1;
        fprintf(stderr, "et4000: SDL bypassed, using fbdev output\n");
        return 0;
    }

    /* Let the emulator's SIGSEGV handler see the real faulting PC/address.
     * SDL's default signal handler re-raises faults via libc, which hides the
     * original JIT/native frame behind gsignal in our crash logs. */
    SDL_SetHint("SDL_NO_SIGNAL_HANDLERS", "1");

    /* Direct-to-HDMI, no windowing system — the SDL2 equivalent of the old
     * /dev/fb0 path. KMSDRM drives the HDMI connector through DRM/KMS + GBM,
     * the same kernel display pipeline the framebuffer device sat on top of.
     * 64-bit Pi OS has no DispmanX, so this is THE bare-console path. Force it
     * by default; SDL_VIDEODRIVER only needs overriding for off-target testing. */
    if (!getenv("SDL_VIDEODRIVER"))
        setenv("SDL_VIDEODRIVER", "kmsdrm", 1);

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        fprintf(stderr, "et4000: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Crisp nearest-neighbour upscale for retro content. Set to "1" (linear)
     * if you prefer smoothing. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    /* KMSDRM has no window manager: the surface always covers the whole HDMI
     * connector. FULLSCREEN_DESKTOP takes the connector's current mode (no
     * extra modeset). The 1280x1024 size is a placeholder — the real output
     * size comes back from SDL_GetRendererOutputSize below. */
    g_sdl_win = SDL_CreateWindow("PiStorm ET4000",
                                 SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                 1280, 1024,
                                 SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!g_sdl_win)
    {
        fprintf(stderr, "et4000: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    /* No PRESENTVSYNC: the guest's frame timing (_VSYNC, 50/60/72 Hz) is paced
     * by render_frame, and must not be slaved to the host refresh rate. */
    g_sdl_ren = SDL_CreateRenderer(g_sdl_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_sdl_ren)
        g_sdl_ren = SDL_CreateRenderer(g_sdl_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_sdl_ren)
    {
        fprintf(stderr, "et4000: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_sdl_win);
        SDL_Quit();
        return -1;
    }

    int ow = 1280, oh = 1024;
    SDL_GetRendererOutputSize(g_sdl_ren, &ow, &oh);

    /* Black letterbox bars (RenderClear fills the area outside the logical
     * rect with the draw colour). */
    SDL_SetRenderDrawColor(g_sdl_ren, 0, 0, 0, 255);

    s->fb_bpp = 32;
    s->fb_fd = -1;

    /* One native-resolution staging buffer, reused across modes. The texture
     * and renderer logical size are set per mode in sdl_set_logical(); the GPU
     * scales the small texture up to the HDMI output. */
    g_logical = (uint32_t *)calloc((size_t)ET4K_STAGE_PITCH * ET4K_MAX_LH, 4);
    if (!g_logical)
    {
        fprintf(stderr, "et4000: staging buffer alloc failed\n");
        return -1;
    }
    s->fb_mem = (uint8_t *)(g_logical + ET4K_ROW_PAD);
    s->fb_size = (size_t)ET4K_STAGE_PITCH * ET4K_MAX_LH * 4;
    s->fb_width = 0; /* set on first blit via sdl_set_logical */
    s->fb_height = 0;
    s->fb_stride = 0;

    SDL_ShowCursor(SDL_DISABLE);

    SDL_RendererInfo rinfo;
    memset(&rinfo, 0, sizeof(rinfo));
    SDL_GetRendererInfo(g_sdl_ren, &rinfo);
    fprintf(stderr, "et4000: SDL output %dx%d (video=%s renderer=%s)\n",
            ow, oh, SDL_GetCurrentVideoDriver(),
            rinfo.name ? rinfo.name : "unknown");
    return 0;
}

/* Point the blit target at the native staging buffer at resolution w x h, size
 * the streaming texture to match (recreating only on change), and set the
 * renderer's logical size to the display shape disp_w x disp_h so the GPU
 * scales + aspect-letterboxes to the HDMI output. */
static void sdl_set_logical(ET4000State *s, uint32_t w, uint32_t h,
                            uint32_t disp_w, uint32_t disp_h)
{
    if (w == 0 || h == 0)
        return;
    if (w > ET4K_MAX_LW)
        w = ET4K_MAX_LW;
    if (h > ET4K_MAX_LH)
        h = ET4K_MAX_LH;

    s->fb_mem = (uint8_t *)(g_logical + ET4K_ROW_PAD);
    s->fb_width = w;
    s->fb_height = h;
    s->fb_stride = ET4K_STAGE_PITCH * 4;

    if (g_fbdev_mode)
        return;

    if (w != g_tex_w || h != g_tex_h)
    {
        if (g_sdl_tex)
            SDL_DestroyTexture(g_sdl_tex);
        g_sdl_tex = SDL_CreateTexture(g_sdl_ren, SDL_PIXELFORMAT_XRGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      (int)w, (int)h);
        g_sdl_tex_has_frame = 0;
        if (g_sdl_tex)
            SDL_SetTextureScaleMode(g_sdl_tex, SDL_ScaleModeNearest);
        g_tex_w = w;
        g_tex_h = h;
    }
    if (disp_w != g_disp_w || disp_h != g_disp_h)
    {
        SDL_RenderSetLogicalSize(g_sdl_ren, (int)disp_w, (int)disp_h);
        g_disp_w = disp_w;
        g_disp_h = disp_h;
    }
}

/* Upload the native staging buffer and present; the GPU scales it up. Called
 * once per frame from the render thread. */
static void sdl_present(ET4000State *s)
{
    static uint32_t present_count = 0;
    static int present_divisor = 0;

    if (!present_divisor)
        present_divisor = et4000_present_divisor();

    if (++present_count % (uint32_t)present_divisor)
        return;

    if (g_fbdev_mode)
    {
        fbdev_present(s);
        return;
    }

    if (!g_sdl_ren || !g_sdl_tex || ((!s->fb_mem || s->fb_stride == 0) && !g_sdl_tex_has_frame))
        return;
    uint64_t t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    if (!g_sdl_tex_has_frame)
    {
        SDL_UpdateTexture(g_sdl_tex, NULL, s->fb_mem, (int)s->fb_stride);
        if (t0)
            et4000_profile_add(ET4K_PROF_SDL_UPDATE, et4000_profile_now_ns() - t0);
    }
    t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    SDL_RenderClear(g_sdl_ren);
    if (t0)
        et4000_profile_add(ET4K_PROF_SDL_CLEAR, et4000_profile_now_ns() - t0);
    t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    SDL_RenderCopy(g_sdl_ren, g_sdl_tex, NULL, NULL);
    if (t0)
        et4000_profile_add(ET4K_PROF_SDL_COPY, et4000_profile_now_ns() - t0);

    /* Full-HDMI-frame screendump: read the composed output (scaled image +
     * letterbox bars) BEFORE present, while the backbuffer is still valid.
     * Logical scaling must be dropped so the NULL read covers the whole
     * physical output, not just the content viewport. */
    if (g_screendump_req)
    {
        g_screendump_req = 0;
        int ow = 0, oh = 0;
        SDL_GetRendererOutputSize(g_sdl_ren, &ow, &oh);
        if (ow > 0 && oh > 0)
        {
            uint32_t *buf = (uint32_t *)malloc((size_t)ow * oh * 4);
            if (buf)
            {
                SDL_RenderSetLogicalSize(g_sdl_ren, 0, 0); /* 1:1 readback */
                int rc = SDL_RenderReadPixels(g_sdl_ren, NULL,
                                              SDL_PIXELFORMAT_ARGB8888,
                                              buf, ow * 4);
                SDL_RenderSetLogicalSize(g_sdl_ren, (int)g_disp_w, /* restore */
                                         (int)g_disp_h);
                if (rc == 0)
                {
                    if (save_png_rgb("screendump.png", buf,
                                     (uint32_t)ow, (uint32_t)oh, (uint32_t)ow) == 0)
                        printf("[ET4K] Screendump %dx%d -> screendump.png\n", ow, oh);
                    else
                        fprintf(stderr, "et4000: PNG screendump failed\n");
                }
                else
                {
                    fprintf(stderr, "et4000: RenderReadPixels failed: %s\n",
                            SDL_GetError());
                }
                free(buf);
            }
        }
    }

    t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    SDL_RenderPresent(g_sdl_ren);
    if (t0)
        et4000_profile_add(ET4K_PROF_SDL_PRESENT, et4000_profile_now_ns() - t0);
}

/* Pump the event queue so the window stays responsive; a window close
 * requests emulation shutdown. */
static void sdl_pump(void)
{
    extern volatile int cpu_emulation_running;
    if (g_fbdev_mode)
        return;

    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        if (e.type == SDL_QUIT ||
            (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE &&
             (e.key.keysym.mod & KMOD_CTRL)))
            cpu_emulation_running = 0;
    }
}

static void sdl_close(ET4000State *s)
{
    if (g_fbdev_mode)
    {
        fbdev_close_output();
        if (g_logical)
        {
            free(g_logical);
            g_logical = NULL;
        }
        if (s)
            s->fb_mem = NULL;
        return;
    }

    if (g_sdl_tex)
    {
        SDL_DestroyTexture(g_sdl_tex);
        g_sdl_tex = NULL;
    }
    if (g_sdl_ren)
    {
        SDL_DestroyRenderer(g_sdl_ren);
        g_sdl_ren = NULL;
    }
    if (g_sdl_win)
    {
        SDL_DestroyWindow(g_sdl_win);
        g_sdl_win = NULL;
    }
    SDL_Quit();
    if (g_logical)
    {
        free(g_logical);
        g_logical = NULL;
    }
    if (s)
        s->fb_mem = NULL;
}

/* -----------------------------------------------------------------------
 * Emulation core — delegated entirely to the PCem ET4000 engine.
 * ----------------------------------------------------------------------- */
int et4000_decode_mode(ET4000State *s)
{
    (void)s;
    return 0;
} /* engine decodes live */

/* RTG framebuffer apertures in the 68k / natmem address space:
 *   NOVA: VRAM 0x00C00000, IO 0x00D00000
 *   XVDI: VRAM 0x00A00000, IO 0x00B00000
 * The ET4000AX has 1 MB of VRAM (the apertures sit exactly 1 MB apart). The
 * active card's VRAM base is where the JIT writes pixels and where the engine
 * must read — NOT rtg.vram_base (that's the ST-screen RAM source). */
#define ET4K_NOVA_VRAM 0x00C00000u
#define ET4K_XVDI_VRAM 0x00A00000u
#define ET4K_VRAM_BYTES 0x00100000u /* 1 MB */

void et4000_update_display(ET4000State *s)
{
    if (!s->fb_mem)
        return;

    /* VRAM is NOT in natmem: your JIT routes the NOVA/XVDI aperture through the
     * vga_* callbacks into PCem's own svga->vram, so we just render from there. */
    int w = 0, h = 0;
    int enabled;

    enabled = s->video_subsystem & 0x01;
    if (!enabled)
        return;

    if (et4000_engine_current_size(&w, &h))
        sdl_set_logical(s, (uint32_t)w, (uint32_t)h, (uint32_t)w, (uint32_t)h);

    g_sdl_tex_has_frame = 0;
    if (et4000_direct_texture() && !g_fbdev_mode && g_sdl_tex && w > 0 && h > 0)
    {
        void *pixels = NULL;
        int pitch = 0;
        uint64_t t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
        if (SDL_LockTexture(g_sdl_tex, NULL, &pixels, &pitch) == 0)
        {
            if (t0)
            {
                et4000_profile_add(ET4K_PROF_SDL_UPDATE, et4000_profile_now_ns() - t0);
                t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
            }
            if (pitch == w * 4)
            {
                int rw = 0, rh = 0;
                et4000_engine_render((uint32_t *)pixels, &rw, &rh);
                g_sdl_tex_has_frame = 1;
                w = rw;
                h = rh;
            }
            t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
            SDL_UnlockTexture(g_sdl_tex);
            if (t0)
                et4000_profile_add(ET4K_PROF_SDL_UPDATE, et4000_profile_now_ns() - t0);
        }
        else if (t0)
            et4000_profile_add(ET4K_PROF_SDL_UPDATE, et4000_profile_now_ns() - t0);
    }

    if (!g_sdl_tex_has_frame)
        et4000_engine_render_direct((uint32_t *)s->fb_mem,
                                    s->fb_stride ? (int)(s->fb_stride / 4) : (int)ET4K_STAGE_PITCH,
                                    ET4K_ROW_PAD,
                                    &w, &h); /* PCem render -> padded staging buf */

    sdl_set_logical(s, (uint32_t)w, (uint32_t)h, (uint32_t)w, (uint32_t)h);

    s->screen_width = (uint32_t)w; /* informational */
    s->screen_height = (uint32_t)h;
}

/* VRAM apertures -> framebuffer bytes.
 *
 * Register I/O remains PCem-driven, but the Atari-visible NOVA/XVDI aperture
 * is a linear framebuffer. Keep this path as cheap and deterministic as the
 * addrbank VGA-VRAM handlers in pistorm_natmem.cpp.
 */
#define ET4K_CPU_VRAM_MASK 0x000FFFFFu

static inline uint8_t *et4000_cpu_vram_ptr(uint32_t off)
{
    return et4000_engine_vram_ptr() + (off & ET4K_CPU_VRAM_MASK);
}

static int et4000_vram_mode(void)
{
    static int mode = -1;
    if (mode != -1)
        return mode;

    const char *env = getenv("PISTORM_VGA_VRAM_MODE");
    if (env && strcmp(env, "direct") == 0)
        mode = 1;
    else if (env && strcmp(env, "pcem") == 0)
        mode = 2;
    else
        mode = 2;
    return mode;
}

static inline int et4000_cpu_direct_vram_ok(void)
{
    int mode = et4000_vram_mode();
    if (mode == 2)
        return 0;
    if (!et4000_engine_vram_ptr())
        return 0;
    if (mode == 1)
        return 1;
    return et4000_engine_direct_vram_ok();
}

uint8_t et4000_vram_read8(ET4000State *s, uint32_t off)
{
    uint64_t t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    uint8_t v;
    (void)s;
    if (!et4000_cpu_direct_vram_ok())
    {
        v = et4000_engine_vram_read8(off);
        if (t0)
            et4000_profile_add(ET4K_PROF_VRAM_RD_ENGINE, et4000_profile_now_ns() - t0);
        return v;
    }

    v = *et4000_cpu_vram_ptr(off);
    if (t0)
        et4000_profile_add(ET4K_PROF_VRAM_RD_DIRECT, et4000_profile_now_ns() - t0);
    return v;
}
uint16_t et4000_vram_read16(ET4000State *s, uint32_t off)
{
    uint64_t t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    uint16_t v;
    (void)s;
    if (!et4000_cpu_direct_vram_ok())
    {
        v = et4000_engine_vram_read16(off);
        if (t0)
            et4000_profile_add(ET4K_PROF_VRAM_RD_ENGINE, et4000_profile_now_ns() - t0);
        return v;
    }

    uint8_t *p = et4000_cpu_vram_ptr(off);
    v = (uint16_t)((p[0] << 8) | p[1]);
    if (t0)
        et4000_profile_add(ET4K_PROF_VRAM_RD_DIRECT, et4000_profile_now_ns() - t0);
    return v;
}
uint32_t et4000_vram_read32(ET4000State *s, uint32_t off)
{
    uint64_t t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    uint32_t v;
    (void)s;
    if (!et4000_cpu_direct_vram_ok())
    {
        v = et4000_engine_vram_read32(off);
        if (t0)
            et4000_profile_add(ET4K_PROF_VRAM_RD_ENGINE, et4000_profile_now_ns() - t0);
        return v;
    }

    uint8_t *p = et4000_cpu_vram_ptr(off);
    v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | p[3];
    if (t0)
        et4000_profile_add(ET4K_PROF_VRAM_RD_DIRECT, et4000_profile_now_ns() - t0);
    return v;
}
void et4000_vram_write8(ET4000State *s, uint32_t off, uint8_t v)
{
    uint64_t t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    (void)s;
    if (!et4000_cpu_direct_vram_ok())
    {
        et4000_engine_vram_write8(off, v);
        if (t0)
            et4000_profile_add(ET4K_PROF_VRAM_WR_ENGINE, et4000_profile_now_ns() - t0);
        return;
    }

    *et4000_cpu_vram_ptr(off) = v;
    if (t0)
        et4000_profile_add(ET4K_PROF_VRAM_WR_DIRECT, et4000_profile_now_ns() - t0);
}
void et4000_vram_write16(ET4000State *s, uint32_t off, uint16_t v)
{
    uint64_t t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    (void)s;
    if (!et4000_cpu_direct_vram_ok())
    {
        et4000_engine_vram_write16(off, v);
        if (t0)
            et4000_profile_add(ET4K_PROF_VRAM_WR_ENGINE, et4000_profile_now_ns() - t0);
        return;
    }

    uint8_t *p = et4000_cpu_vram_ptr(off);
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    if (t0)
        et4000_profile_add(ET4K_PROF_VRAM_WR_DIRECT, et4000_profile_now_ns() - t0);
}
void et4000_vram_write32(ET4000State *s, uint32_t off, uint32_t v)
{
    uint64_t t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    (void)s;
    if (!et4000_cpu_direct_vram_ok())
    {
        et4000_engine_vram_write32(off, v);
        if (t0)
            et4000_profile_add(ET4K_PROF_VRAM_WR_ENGINE, et4000_profile_now_ns() - t0);
        return;
    }

    uint8_t *p = et4000_cpu_vram_ptr(off);
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    if (t0)
        et4000_profile_add(ET4K_PROF_VRAM_WR_DIRECT, et4000_profile_now_ns() - t0);
}

/* I/O ports -> engine. Snoop the subsystem-enable port so render_frame's
 * (g_et4000->video_subsystem & 1) gate keeps working. */
static uint8_t et4000_io_read8_unlocked(ET4000State *s, uint32_t port)
{
    (void)s;
    return et4000_engine_io_read(port);
}

static int et4000_io_write8_unlocked(ET4000State *s, uint32_t port, uint8_t val)
{
    if ((port & 0x0000FFFF) == 0x3C3 || (port & 0x0000FFFF) == 0x46E8)
    {
        s->video_subsystem = val;
        if (val & 0x01)
        {
            extern int et4k_io_log;
            et4k_io_log = 1;
        } /* start register log */
    }
    et4000_engine_io_write(port, val);
    return 1;
}

uint8_t et4000_io_read8(ET4000State *s, uint32_t port)
{
    uint64_t t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    uint64_t t1;
    pthread_mutex_lock(&et4000_engine_mutex);
    t1 = t0 ? et4000_profile_now_ns() : 0;
    uint8_t v = et4000_io_read8_unlocked(s, port);
    pthread_mutex_unlock(&et4000_engine_mutex);
    if (t0)
    {
        et4000_profile_add(ET4K_PROF_IO_RD_WAIT, t1 - t0);
        et4000_profile_add(ET4K_PROF_IO_RD_TOTAL, et4000_profile_now_ns() - t0);
    }
    return v;
}

uint16_t et4000_io_read16(ET4000State *s, uint32_t port)
{
    uint64_t t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    uint64_t t1;
    pthread_mutex_lock(&et4000_engine_mutex);
    t1 = t0 ? et4000_profile_now_ns() : 0;
    uint16_t v = ((uint16_t)et4000_io_read8_unlocked(s, port) << 8) |
                 et4000_io_read8_unlocked(s, port + 1);
    pthread_mutex_unlock(&et4000_engine_mutex);
    if (t0)
    {
        et4000_profile_add(ET4K_PROF_IO_RD_WAIT, t1 - t0);
        et4000_profile_add(ET4K_PROF_IO_RD_TOTAL, et4000_profile_now_ns() - t0);
    }
    return v;
}

int et4000_io_write8(ET4000State *s, uint32_t port, uint8_t val)
{
    uint64_t t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    uint64_t t1;
    pthread_mutex_lock(&et4000_engine_mutex);
    t1 = t0 ? et4000_profile_now_ns() : 0;
    int rc = et4000_io_write8_unlocked(s, port, val);
    pthread_mutex_unlock(&et4000_engine_mutex);
    if (t0)
    {
        et4000_profile_add(ET4K_PROF_IO_WR_WAIT, t1 - t0);
        et4000_profile_add(ET4K_PROF_IO_WR_TOTAL, et4000_profile_now_ns() - t0);
    }
    return rc;
}

int et4000_io_write16(ET4000State *s, uint32_t port, uint16_t val)
{
    uint64_t t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    uint64_t t1;
    pthread_mutex_lock(&et4000_engine_mutex);
    t1 = t0 ? et4000_profile_now_ns() : 0;
    et4000_io_write8_unlocked(s, port, (uint8_t)(val >> 8));
    et4000_io_write8_unlocked(s, port + 1, (uint8_t)val);
    pthread_mutex_unlock(&et4000_engine_mutex);
    if (t0)
    {
        et4000_profile_add(ET4K_PROF_IO_WR_WAIT, t1 - t0);
        et4000_profile_add(ET4K_PROF_IO_WR_TOTAL, et4000_profile_now_ns() - t0);
    }
    return 1;
}
int et4000_io_write32(ET4000State *s, uint32_t port, uint32_t val)
{
    uint64_t t0 = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
    uint64_t t1;
    pthread_mutex_lock(&et4000_engine_mutex);
    t1 = t0 ? et4000_profile_now_ns() : 0;
    for (int i = 0; i < 4; i++)
        et4000_io_write8_unlocked(s, port + i, (uint8_t)((val >> (24 - 8 * i)) & 0xFF));
    pthread_mutex_unlock(&et4000_engine_mutex);
    if (t0)
    {
        et4000_profile_add(ET4K_PROF_IO_WR_WAIT, t1 - t0);
        et4000_profile_add(ET4K_PROF_IO_WR_TOTAL, et4000_profile_now_ns() - t0);
    }
    return 1;
}

int et4000_init(ET4000State *s, const char *fb_device)
{
    memset(s, 0, sizeof(*s));
    s->vram_size = ET4000_VRAM_SIZE; /* informational; engine owns real VRAM */

    if (sdl_open(s, fb_device) < 0)
        return -1;

    et4000_engine_init(); /* PCem ET4000 + 2MB VRAM */

    g_et4000 = s;
    fb_ptr = s->fb_mem;
    vram_ptr = et4000_engine_vram_ptr();

    printf("et4000: init complete (PCem engine), fb=%ux%u\n", s->fb_width, s->fb_height);
    return 0;
}

void et4000_shutdown(ET4000State *s)
{
    if (!s)
        return;
    sdl_close(s);
    if (g_et4000 == s)
        g_et4000 = NULL;
}
/* --------------- PISTORM EMULATOR INTERFACE ---------------- */

#include <sys/ioctl.h>
#include <stdbool.h>
#include <sys/time.h>

int kbhit(void);
void screenDump(int, int);

extern volatile int cpu_emulation_running;
extern bool screenGrab;
extern bool Native_HDMI_enabled;

ET4000State et4000_s;
extern volatile uint16_t st_palette[16];
/* -----------------------------------------------------------------------
 * PNG screendump — minimal in-process encoder (zlib for DEFLATE + CRC).
 * No SDL2_image / libpng dependency; zlib is already in the link graph.
 * Source pixels are ARGB8888 (0xAARRGGBB); written as 8-bit RGB.
 * ----------------------------------------------------------------------- */

static void png_write_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    fwrite(b, 1, 4, f);
}

static void png_write_chunk(FILE *f, const char *type,
                            const uint8_t *data, uint32_t len)
{
    png_write_u32(f, len);
    fwrite(type, 1, 4, f);
    if (len)
        fwrite(data, 1, len, f);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (len)
        crc = crc32(crc, (const Bytef *)data, len);
    png_write_u32(f, (uint32_t)crc);
}

static int save_png_rgb(const char *path, const uint32_t *pixels,
                        uint32_t w, uint32_t h, uint32_t stride_px)
{
    size_t row_bytes = 1 + (size_t)w * 3; /* filter byte + RGB */
    size_t raw_size = row_bytes * h;
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    if (!raw)
        return -1;

    for (uint32_t y = 0; y < h; y++)
    {
        uint8_t *o = raw + y * row_bytes;
        *o++ = 0; /* filter: None */
        const uint32_t *src = pixels + (size_t)y * stride_px;
        for (uint32_t x = 0; x < w; x++)
        {
            uint32_t px = src[x];
            *o++ = (px >> 16) & 0xFF; /* R */
            *o++ = (px >> 8) & 0xFF;  /* G */
            *o++ = (px) & 0xFF;       /* B */
        }
    }

    uLongf comp_size = compressBound(raw_size);
    uint8_t *comp = (uint8_t *)malloc(comp_size);
    if (!comp)
    {
        free(raw);
        return -1;
    }
    if (compress2(comp, &comp_size, raw, raw_size, Z_BEST_SPEED) != Z_OK)
    {
        free(raw);
        free(comp);
        return -1;
    }
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f)
    {
        free(comp);
        return -1;
    }

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13] = {
        (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
        8, /* bit depth   */
        2, /* colour type: RGB */
        0, 0, 0};
    png_write_chunk(f, "IHDR", ihdr, 13);
    png_write_chunk(f, "IDAT", comp, (uint32_t)comp_size);
    png_write_chunk(f, "IEND", NULL, 0);

    fclose(f);
    free(comp);

    system("bash ./screendump.sh");
    return 0;
}

void screenDump(int w, int h)
{
    (void)w;
    (void)h;
    /* Request a capture; the render thread services it in sdl_present, reading
     * the full HDMI frame and writing screendump.png. Safe to call from any
     * thread (no SDL access here). */
    g_screendump_req = 1;
}

/* terminal IO */
int kbhit(void)
{
    int ch = getchar();
    if (ch != EOF)
    {
        ungetc(ch, stdin);
        return 1;
    }
    return 0;
}
#define HZ50 20000
#define HZ60 16667
#define HZ72 14045 // Monochrome actually 71.2Hz

/* ST palette: 16 words at $FF8240. STF = 3 bits/gun (0RRR0GGG0BBB). */
static void st_load_palette(uint32_t pal[16])
{
    for (int i = 0; i < 16; i++)
    {
        uint16_t w = st_palette[i]; /* STF: 0RRR0GGG0BBB */
        uint8_t r3 = (w >> 8) & 7, g3 = (w >> 4) & 7, b3 = w & 7;
        uint8_t r = (r3 << 5) | (r3 << 2) | (r3 >> 1);
        uint8_t g = (g3 << 5) | (g3 << 2) | (g3 >> 1);
        uint8_t b = (b3 << 5) | (b3 << 2) | (b3 >> 1);
        pal[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}


/* STE palette: 16 words at $FF8240. STE = 4 bits/gun.
 * Backward-compatible layout: original ST 3 bits hold the HIGH 3 bits of each
 * 4-bit value; the extra LSB lives in the old unused bit. Nibble = [L R2 R1 R0]:
 *   Red   : bits 11..8  (bit 11 = LSB)
 *   Green : bits  7..4  (bit  7 = LSB)
 *   Blue  : bits  3..0  (bit  3 = LSB)
 */
static void ste_load_palette(uint32_t pal[16])
{
    for (int i = 0; i < 16; i++)
    {
        uint16_t w = st_palette[i]; //ste_palette[i];

        /* rebuild each 4-bit gun: high 3 bits from ST position, LSB from extra bit */
        uint8_t r4 = (((w >> 8) & 7) << 1) | ((w >> 11) & 1);
        uint8_t g4 = (((w >> 4) & 7) << 1) | ((w >>  7) & 1);
        uint8_t b4 = (((w >> 0) & 7) << 1) | ((w >>  3) & 1);

        /* expand 4 -> 8 bits by replication (0 -> 0, 15 -> 255) */
        uint8_t r = (r4 << 4) | r4;
        uint8_t g = (g4 << 4) | g4;
        uint8_t b = (b4 << 4) | b4;

        pal[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

static const uint8_t *st_native_frame_source(uint32_t base, const uint8_t *mirror)
{
    (void)base;
    return mirror;
}

static uint32_t st_native_physbase_from_mirror(const uint8_t *st_ram)
{
    if (!st_ram)
        return 0;

    uint32_t base = ((uint32_t)st_ram[0x44e] << 24) |
                    ((uint32_t)st_ram[0x44f] << 16) |
                    ((uint32_t)st_ram[0x450] << 8) |
                    (uint32_t)st_ram[0x451];
    base &= 0x00FFFFFEu;

    if (base && base + 0x8000u < 0x00400000u)
        return base;
    return 0;
}

/* Decode one ST source scanline to flat palette indices (0..15). */
static void st_decode_row(const uint8_t *row, uint32_t src_w,
                          uint32_t planes, uint8_t *idx)
{
    if (planes == 1)
    {
        for (uint32_t x = 0; x < src_w; x++)
            idx[x] = (row[x >> 3] >> (7 - (x & 7))) & 1;
        return;
    }
    uint32_t groups = src_w >> 4; /* 16 px per group */
    for (uint32_t g = 0; g < groups; g++)
    {
        const uint8_t *gp = row + g * planes * 2;
        uint16_t pw[4];

        for (uint32_t p = 0; p < planes; p++)
            pw[p] = (gp[p * 2] << 8) | gp[p * 2 + 1]; /* big-endian plane words */
        uint8_t *o = idx + g * 16;
        for (int b = 15; b >= 0; b--)
        { /* bit 15 = leftmost pixel */
            uint8_t c = 0;
            for (uint32_t p = 0; p < planes; p++)
                c |= ((pw[p] >> b) & 1) << p;
            *o++ = c;
        }
    }
}

/* st_mode & 3:  0=LOW 320x200x4   1=MED 640x200x2   2=HIGH 640x400 mono */
static void blit_st_native(ET4000State *s, const uint8_t *st_ram, int st_mode)
{
    uint32_t src_w, src_h, planes, stride;
    switch (st_mode & 3)
    {
    case 0:
        src_w = 320;
        src_h = 200;
        planes = 4;
        stride = 160;
        break;
    case 1:
        src_w = 640;
        src_h = 200;
        planes = 2;
        stride = 160;
        break;
    default:
        src_w = 640;
        src_h = 400;
        planes = 1;
        stride = 80;
        break;
    }

    /* Decode at the native sample grid; the GPU stretches it to the common
     * 640x400 display shape (so MED's 640x200 fills the same area as LOW/HIGH)
     * and letterboxes to HDMI. */
    sdl_set_logical(s, src_w, src_h, 640, 400);
    uint32_t *dst = (uint32_t *)s->fb_mem;
    uint32_t dst_pitch = s->fb_stride ? (uint32_t)(s->fb_stride / 4) : src_w;

    uint32_t pal[16];
    if (planes == 1)
    {
        pal[0] = 0xFFFFFFFFu;
        pal[1] = 0xFF000000u;
    }
    else
        //st_load_palette(pal);
        ste_load_palette(pal);

    uint8_t idx[640];

    for (uint32_t y = 0; y < src_h; y++)
    {
        const uint8_t *row = st_ram + y * stride;
        uint32_t *o = dst + y * dst_pitch;

        if (planes == 1)
        {
            for (uint32_t x = 0; x < src_w; x++)
                o[x] = pal[(row[x >> 3] >> (7 - (x & 7))) & 1];
        }
        else
        {
            st_decode_row(row, src_w, planes, idx);
            for (uint32_t x = 0; x < src_w; x++)
                o[x] = pal[idx[x]];
        }
    }
}

/*
 * Render to chosen frame rate
 * Need to read Atari env variables to decide this
 */
void *render_frame(void *vptr)
{
    extern rtg_s rtg;
    int took;
    struct timeval stop, start;
    int FRAME_RATE = et4000_frame_interval_us();
    int no_render = et4000_no_render();
    int irq_backoff = et4000_irq_backoff_enabled();
    int8_t prev_shift_mode = -1;
    static int64_t frames = 1;

    et4000_configure_render_thread();

    if (et4000_init(&et4000_s, NULL) < 0)
    {
        et4000_thread_ready = -1;
        return NULL;
    }
    et4000_thread_ready = 1;

    while (!cpu_emulation_running)
        usleep(1000);

    while (cpu_emulation_running)
    {
        uint32_t shifter_base = ((uint32_t)rtg.high << 16) |
                                ((uint32_t)rtg.mid << 8) |
                                ((uint32_t)rtg.low & 0xFEu);
        if (shifter_base)
            rtg.vram_base = shifter_base;
        else
        {
            uint32_t physbase = st_native_physbase_from_mirror(rtg.natmem);
            if (physbase)
                rtg.vram_base = physbase;
        }
        /*
                if (rtg.vram_base) {
                    g_et4000->video_subsystem = 0x01;

                    if (rtg.shift_mode != prev_shift_mode) {
                        printf ("setting new res @ 0x%X\n", rtg.vram_base);
                        //printf ("render_frame natmem %p\n", rtg.natmem);
                        prev_shift_mode = rtg.shift_mode;

                        switch (rtg.shift_mode & 0x03) {
                            case 0: // 320x200 ST-LOW 16-colour
                                g_et4000->crtc[0x01] = 39; // hdisplay 320
                                g_et4000->crtc[0x07] = 0;
                                g_et4000->crtc[0x0C] = 0;
                                g_et4000->crtc[0x0D] = 0;
                                g_et4000->crtc[0x12] = 199; // vdisplay 200
                                g_et4000->crtc[0x13] = 80; // bytes per row?
                                g_et4000->seq[4]     = 0;
                                g_et4000->seq[2]     = 0x0F;
                                g_et4000->gc[6]      = 0x01;
                            break;
                            case 1: // 640x200 ST-MEDIUM 4-colour
                            break;
                            case 2: // 640x400 ST-HIGH Monochrome
                                g_et4000->seq[4] = 0;
                                g_et4000->seq[2] = 0x01;
                            break;
                        }

                    }
                    else {

                        memcpy ((void*)(rtg.natmem + 0xC00000), (void*)(rtg.natmem + rtg.vram_base), 0x8000 );
                    }
                }
        */
        sdl_pump();

        gettimeofday(&start, NULL);

        _VSYNC = 0;

        if (!no_render && !(irq_backoff && et4000_irq_pending_for_cpu()))
        {
            static bool rtg_seen_live = false;
            bool video_enabled = (g_et4000->video_subsystem & 0x01) != 0;
            bool show_rtg = false;
            bool rendered = false;
            uint64_t t_build = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
            uint64_t t_present;

            if (!video_enabled)
                rtg_seen_live = false;
            else if (rtg_seen_live)
                show_rtg = true;
            else
            {
                show_rtg = et4000_engine_visible_nonzero();
                if (show_rtg)
                    rtg_seen_live = true;
            }

            if (show_rtg) {
                et4000_update_display(g_et4000); /* aperture has pixels -> show RTG */
                rendered = true;
            }

            else if (Native_HDMI_enabled && rtg.vram_base && rtg.vram_base + 0x8000 < 0xE00000)
            {
                g_sdl_tex_has_frame = 0;
                blit_st_native(g_et4000,
                               st_native_frame_source(rtg.vram_base, rtg.natmem + rtg.vram_base),
                               rtg.shift_mode); /* Native ST screen, including real blitter DMA writes. */
                rendered = true;
            }

            if (t_build)
                et4000_profile_add(ET4K_PROF_RENDER_BUILD, et4000_profile_now_ns() - t_build);

            if (rendered)
            {
                t_present = et4000_profile_enabled() ? et4000_profile_now_ns() : 0;
                sdl_present(g_et4000);
                if (t_present)
                    et4000_profile_add(ET4K_PROF_PRESENT_TOTAL, et4000_profile_now_ns() - t_present);
            }
        }

        _VSYNC = 1;

        do
        {
            if (screenGrab && kbhit())
            {
                int c = getchar();

                if (c == 's' || c == 'S')
                    screenDump(g_et4000->fb_width, g_et4000->fb_height);
            }

            gettimeofday(&stop, NULL);
            took = ((stop.tv_sec - start.tv_sec) * 1000000) + (stop.tv_usec - start.tv_usec);

            int remaining = FRAME_RATE - took;
            if (remaining > 1000)
                usleep(remaining > 2000 ? 1000 : remaining / 2);
            else
                asm volatile("yield" ::: "memory");
        } while (took < FRAME_RATE);

        if (took > FRAME_RATE)
        {
            if (took - FRAME_RATE > 2000)
                printf("render_frame: overrun %dms\n", took / 1000);
        }

        frames++;
    }

    return NULL;
}
