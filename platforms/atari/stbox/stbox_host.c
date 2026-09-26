/* SPDX-License-Identifier: MIT
 *
 * stbox_host.c - the normal-core half of the ST sandbox: ROM/RAM loading,
 * DRM overlay presentation and lifecycle. Everything with a syscall in it
 * lives here; the core-3 slice engine in stbox.c never calls this file.
 *
 * Presentation follows vidplane.c's hard-won rules on the shared DRM fd:
 *   - legacy/atomic coexistence: non-blocking atomic commits with NO
 *     page-flip event, falling back to blocking SetPlane (see vidplane.c
 *     for why two event consumers on one fd steal each other's events).
 *   - plane choice: walk the plane list, skip the guest's plane and any
 *     plane that already has a framebuffer attached (vidplay's plane shows
 *     up busy, so the two features never fight over an overlay).
 *
 * The render thread runs SCHED_OTHER on cores 0/1 only - cores 2 (JIT CPU,
 * SCHED_FIFO) and 3 (ipl_task + the sandbox CPU) are off-limits, same
 * lesson as avrecord.c/vidplay.c.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <dirent.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "stbox.h"
#include "../et4000/et4000_drm.h"
#include "../../../third_party/musashi/m68k.h"   /* m68k_disassemble */

/* config_file.h can't be included here: its M68K_CPU_TYPE_* enum (the
 * JIT-side copy) collides with the identical names in Musashi's m68k.h
 * above. Declare the two accessors we need instead. */
extern const char *emulator_config_stbox_tos(void);   /* "" if unset */
extern int emulator_config_stbox_plane(void);         /* 0 if unset  */

/* ------------------------------------------------------------------ */
/* state                                                              */
/* ------------------------------------------------------------------ */
static uint8_t *g_ram;                 /* sandbox ST-RAM (malloc'd)     */
static uint8_t *g_disk_cur;            /* drive A image buffer we own   */
static stbox_cfg_t g_cfg;
static volatile int g_run;             /* render thread keepalive       */
static pthread_t g_thread;
static int g_started;

/* geometry from the GEM front-end (display pixels), plus focus */
static volatile int g_rx, g_ry, g_rw = -1, g_rh = -1;
static volatile int g_cx, g_cy, g_cw = -1, g_ch = -1;
static volatile int g_focus;
static volatile int g_route = 1;        /* kbd_usb.c ESC toggle, for logs */

/* DRM */
static int g_fd = -1;
static uint32_t g_crtc, g_plane;
static int g_mode_w, g_mode_h;
static uint32_t g_fourcc;

#define NBUF 2
struct sbuf {
    uint32_t handle, fb, pitch;
    uint8_t *map;
    size_t   size;
};
static struct sbuf g_buf[NBUF];
static int g_cur;

/* atomic props */
static int g_atomic;
static uint32_t pr_fb, pr_crtc, pr_cx, pr_cy, pr_cw, pr_ch,
                pr_sx, pr_sy, pr_sw, pr_sh;

#define SRC_W 640                      /* buffer is always 640x400      */
#define SRC_H 400

/* ------------------------------------------------------------------ */
/* plane + buffers                                                    */
/* ------------------------------------------------------------------ */
/* Property lookup with current value and range - needed for zpos, which on
 * vc4 is a RANGE property with real limits (see vidplane.c). */
static uint32_t prop_find(uint32_t plane, const char *name, uint64_t *cur,
                          int *mutable_, uint64_t *lo, uint64_t *hi)
{
    drmModeObjectProperties *pr =
        drmModeObjectGetProperties(g_fd, plane, DRM_MODE_OBJECT_PLANE);
    uint32_t id = 0;
    if (!pr) return 0;
    for (uint32_t i = 0; i < pr->count_props && !id; i++) {
        drmModePropertyRes *p = drmModeGetProperty(g_fd, pr->props[i]);
        if (p) {
            if (!strcmp(p->name, name)) {
                id = p->prop_id;
                if (cur) *cur = pr->prop_values[i];
                if (mutable_) *mutable_ = !(p->flags & DRM_MODE_PROP_IMMUTABLE);
                if ((p->flags & DRM_MODE_PROP_RANGE) && p->count_values >= 2) {
                    if (lo) *lo = p->values[0];
                    if (hi) *hi = p->values[1];
                }
            }
            drmModeFreeProperty(p);
        }
    }
    drmModeFreeObjectProperties(pr);
    return id;
}

static uint32_t plane_prop(uint32_t plane, const char *name)
{
    return prop_find(plane, name, NULL, NULL, NULL, NULL);
}

static long plane_zpos_of(uint32_t plane)
{
    uint64_t cur = 0;
    return prop_find(plane, "zpos", &cur, NULL, NULL, NULL) ? (long)cur : -1;
}

/* Sit ABOVE the guest plane - vidplane.c's hard-won lesson, quoted: the
 * guest plane is an opaque full-screen XRGB8888 surface, so a picture
 * underneath it is simply invisible. Raise ours to the top of its range;
 * if that still leaves us at or below the guest plane, push the GUEST
 * plane down (we are the DRM master, both planes are ours). */
static void raise_above_guest(uint32_t guest_plane)
{
    uint64_t cur = 0, lo = 0, hi = 0;
    int mut = 0;
    uint32_t zp = prop_find(g_plane, "zpos", &cur, &mut, &lo, &hi);
    if (zp && mut && hi > cur)
        drmModeObjectSetProperty(g_fd, g_plane, DRM_MODE_OBJECT_PLANE, zp, hi);

    long mine = plane_zpos_of(g_plane);
    long theirs = plane_zpos_of(guest_plane);
    if (mine >= 0 && theirs >= 0 && mine <= theirs) {
        uint64_t gcur = 0, glo = 0, ghi = 0;
        int gmut = 0;
        uint32_t gzp = prop_find(guest_plane, "zpos", &gcur, &gmut, &glo, &ghi);
        if (gzp && gmut && glo < (uint64_t)mine)
            drmModeObjectSetProperty(g_fd, guest_plane, DRM_MODE_OBJECT_PLANE,
                                     gzp, glo);
        theirs = plane_zpos_of(guest_plane);
    }
    fprintf(stderr, "[STBOX] zpos: box plane %u = %ld, guest plane %u = %ld\n",
            g_plane, mine, guest_plane, theirs);
    if (mine >= 0 && theirs >= 0 && mine <= theirs)
        fprintf(stderr, "[STBOX] WARNING: still at or below the guest plane - "
                        "the picture will be invisible\n");
}

static int plane_type_of(uint32_t plane)
{
    drmModeObjectProperties *pr =
        drmModeObjectGetProperties(g_fd, plane, DRM_MODE_OBJECT_PLANE);
    int type = -1;
    if (!pr) return -1;
    for (uint32_t i = 0; i < pr->count_props && type < 0; i++) {
        drmModePropertyRes *p = drmModeGetProperty(g_fd, pr->props[i]);
        if (p) {
            if (!strcmp(p->name, "type")) type = (int)pr->prop_values[i];
            drmModeFreeProperty(p);
        }
    }
    drmModeFreeObjectProperties(pr);
    return type;
}

static int pick_plane(void)
{
    int fd = drmpres_fd();
    if (fd < 0) {
        fprintf(stderr, "[STBOX] DRM display not up - sandbox video needs "
                        "the DRM scanout path\n");
        return -1;
    }
    g_fd = fd;
    g_crtc = drmpres_crtc_id();
    g_mode_w = drmpres_mode_w();
    g_mode_h = drmpres_mode_h();
    int crtc_index = drmpres_crtc_index();
    uint32_t guest_plane = drmpres_plane_id();

    /* forced plane: env overrides cfg overrides auto-pick */
    const char *force = getenv("PISTORM_STBOX_PLANE");
    uint32_t want = force && *force ? (uint32_t)strtoul(force, NULL, 0)
                                    : (uint32_t)emulator_config_stbox_plane();

    drmModePlaneRes *pr = drmModeGetPlaneResources(g_fd);
    if (!pr) return -1;
    uint32_t chosen = 0, chosen_fmt = 0;
    for (uint32_t i = 0; i < pr->count_planes && !chosen; i++) {
        drmModePlane *pl = drmModeGetPlane(g_fd, pr->planes[i]);
        if (!pl) continue;
        int usable = (pl->possible_crtcs & (1u << crtc_index)) &&
                     pl->plane_id != guest_plane &&
                     plane_type_of(pl->plane_id) == DRM_PLANE_TYPE_OVERLAY &&
                     pl->fb_id == 0;              /* not in use (vidplay) */
        uint32_t fmt = 0;
        for (uint32_t f = 0; f < pl->count_formats && !fmt; f++)
            if (pl->formats[f] == DRM_FORMAT_XRGB8888 ||
                pl->formats[f] == DRM_FORMAT_ARGB8888)
                fmt = pl->formats[f];
        if (usable && fmt && (!want || pl->plane_id == want)) {
            chosen = pl->plane_id;
            chosen_fmt = fmt;
        }
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pr);
    if (!chosen) {
        fprintf(stderr, "[STBOX] no free RGB-capable overlay plane on CRTC %u"
                        " - is a video playing? (PISTORM_STBOX_PLANE forces)\n",
                g_crtc);
        return -1;
    }
    g_plane = chosen;
    g_fourcc = chosen_fmt;
    raise_above_guest(guest_plane);

    if (drmSetClientCap(g_fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0) {
        pr_fb = plane_prop(g_plane, "FB_ID");
        pr_crtc = plane_prop(g_plane, "CRTC_ID");
        pr_cx = plane_prop(g_plane, "CRTC_X");
        pr_cy = plane_prop(g_plane, "CRTC_Y");
        pr_cw = plane_prop(g_plane, "CRTC_W");
        pr_ch = plane_prop(g_plane, "CRTC_H");
        pr_sx = plane_prop(g_plane, "SRC_X");
        pr_sy = plane_prop(g_plane, "SRC_Y");
        pr_sw = plane_prop(g_plane, "SRC_W");
        pr_sh = plane_prop(g_plane, "SRC_H");
        g_atomic = pr_fb && pr_crtc && pr_cx && pr_cy && pr_cw && pr_ch &&
                   pr_sx && pr_sy && pr_sw && pr_sh;
    }
    fprintf(stderr, "[STBOX] plane %u (%s), present: %s\n", g_plane,
            g_fourcc == DRM_FORMAT_XRGB8888 ? "XRGB8888" : "ARGB8888",
            g_atomic ? "non-blocking atomic" : "blocking SetPlane");
    return 0;
}

static int buf_alloc(struct sbuf *b)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

    memset(&creq, 0, sizeof creq);
    creq.width = SRC_W; creq.height = SRC_H; creq.bpp = 32;
    if (drmIoctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) return -1;
    b->handle = creq.handle; b->pitch = creq.pitch; b->size = creq.size;

    handles[0] = creq.handle; pitches[0] = creq.pitch;
    if (drmModeAddFB2(g_fd, SRC_W, SRC_H, g_fourcc, handles, pitches,
                      offsets, &b->fb, 0) < 0) return -1;
    memset(&mreq, 0, sizeof mreq);
    mreq.handle = creq.handle;
    if (drmIoctl(g_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) return -1;
    b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  g_fd, mreq.offset);
    if (b->map == MAP_FAILED) { b->map = NULL; return -1; }
    memset(b->map, 0, b->size);
    return 0;
}

static void buf_free(struct sbuf *b)
{
    if (b->map) munmap(b->map, b->size);
    if (b->fb) drmModeRmFB(g_fd, b->fb);
    if (b->handle) {
        struct drm_mode_destroy_dumb dreq = { .handle = b->handle };
        drmIoctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
    memset(b, 0, sizeof *b);
}

static int commit(uint32_t fb, uint32_t sx, uint32_t sy, uint32_t sw,
                  uint32_t sh, int dx, int dy, int dw, int dh)
{
    if (!g_atomic)
        return drmModeSetPlane(g_fd, g_plane, g_crtc, fb, 0, dx, dy,
                               (uint32_t)dw, (uint32_t)dh,
                               sx << 16, sy << 16, sw << 16, sh << 16) < 0
               ? -1 : 0;
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) return -1;
    drmModeAtomicAddProperty(req, g_plane, pr_fb, fb);
    drmModeAtomicAddProperty(req, g_plane, pr_crtc, fb ? g_crtc : 0);
    drmModeAtomicAddProperty(req, g_plane, pr_cx, dx);
    drmModeAtomicAddProperty(req, g_plane, pr_cy, dy);
    drmModeAtomicAddProperty(req, g_plane, pr_cw, dw);
    drmModeAtomicAddProperty(req, g_plane, pr_ch, dh);
    drmModeAtomicAddProperty(req, g_plane, pr_sx, sx << 16);
    drmModeAtomicAddProperty(req, g_plane, pr_sy, sy << 16);
    drmModeAtomicAddProperty(req, g_plane, pr_sw, sw << 16);
    drmModeAtomicAddProperty(req, g_plane, pr_sh, sh << 16);
    int r = drmModeAtomicCommit(g_fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
    drmModeAtomicFree(req);
    if (r == -EBUSY || (r < 0 && errno == EBUSY)) r = 0;  /* in flight */
    return r < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* frame conversion: ST planar -> XRGB8888                            */
/* ------------------------------------------------------------------ */
static const uint8_t lvl[8] = { 0, 36, 73, 109, 146, 182, 219, 255 };

/* Frame conversion, frame tier: one palette/resolution/base for the whole
 * frame (mid-frame changes smear rather than raster). The plain-ST path is
 * the original, untouched, so an ST box is bit-identical to before STE
 * existed; STE adds 4-bit palette, linewidth and hscroll. Line addresses
 * wrap within the (power-of-two) RAM size, so a base near the top of RAM
 * or a transiently wrong one can never read outside the buffer (field
 * report: SIGSEGV loading Xenon 2). */
/* Low/medium resolution from the scanline capture (stbox.h, SCANLINE
 * CAPTURE): one consistent frame, each row in its own palette. Returns 0
 * when there is no capture to use (none yet, or high resolution) and the
 * live path below runs instead. */
static int convert_cap(uint8_t *dst, uint32_t pitch, int *out_w, int *out_h)
{
    uint32_t idx = stbox_shared.cap_idx;
    if (idx >= STBOX_CAP_BUFS)
        return 0;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const stbox_cap_t *c = &stbox_cap[idx];
    const int res = c->res;
    if (res > 1)
        return 0;
    const int ste = stbox_shared.ste;
    const int hs = ste ? (c->hscroll & 15) : 0;
    int rows = res == 0 ? (int)c->rows : 200;
    if (rows < 200) rows = 200;
    if (rows > STBOX_CAP_ROWS) rows = STBOX_CAP_ROWS;

    for (int y = 0; y < rows; y++) {
        uint32_t pal[16];
        for (int i = 0; i < 16; i++) {
            uint16_t p = c->pal[y][i];
            if (ste) {
                #define STE_LVL(n) ((uint8_t)(((((n) & 7) << 1) | (((n) >> 3) & 1)) * 17))
                pal[i] = 0xFF000000u | ((uint32_t)STE_LVL(p >> 8) << 16) |
                         ((uint32_t)STE_LVL(p >> 4) << 8) | STE_LVL(p);
                #undef STE_LVL
            } else {
                pal[i] = 0xFF000000u | ((uint32_t)lvl[(p >> 8) & 7] << 16) |
                         ((uint32_t)lvl[(p >> 4) & 7] << 8) | lvl[p & 7];
            }
        }
        const uint8_t *s = c->pix + (size_t)y * STBOX_CAP_PITCH;
        uint32_t *d = (uint32_t *)(dst + (size_t)y * pitch);
        uint8_t px[656];
        uint8_t *o = px;
        if (res == 0) {
            const int groups = 20 + (hs ? 1 : 0);
            for (int g = 0; g < groups; g++, s += 8) {
                uint16_t p0 = (s[0] << 8) | s[1], p1 = (s[2] << 8) | s[3];
                uint16_t p2 = (s[4] << 8) | s[5], p3 = (s[6] << 8) | s[7];
                for (int b = 15; b >= 0; b--)
                    *o++ = (uint8_t)(((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) |
                                     (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3));
            }
            for (int x = 0; x < 320; x++) *d++ = pal[px[hs + x]];
        } else {
            const int groups = 40 + (hs ? 1 : 0);
            for (int g = 0; g < groups; g++, s += 4) {
                uint16_t p0 = (s[0] << 8) | s[1], p1 = (s[2] << 8) | s[3];
                for (int b = 15; b >= 0; b--)
                    *o++ = (uint8_t)(((p0 >> b) & 1) | (((p1 >> b) & 1) << 1));
            }
            for (int x = 0; x < 640; x++) *d++ = pal[px[hs + x]];
        }
    }
    *out_w = res == 0 ? 320 : 640;
    *out_h = rows;
    return 1;
}

static void convert(uint8_t *dst, uint32_t pitch, int *out_w, int *out_h)
{
    if (convert_cap(dst, pitch, out_w, out_h))
        return;

    /* Low/med visible height: 200, or more when the guest opened the
     * bottom border this frame. Clamped so the linear read below cannot
     * leave the buffer whatever height was published. */
    int vh = (int)stbox_shared.vis_lines;
    if (vh < 200) vh = 200;
    if (vh > 248) vh = 248;

    if (!stbox_shared.ste) {
        uint32_t need = (uint32_t)vh * 160u;   /* low-res bytes for vh lines */
        uint32_t vb = stbox_shared.video_base & (stbox_shared.ram_size - 1);
        if (stbox_shared.ram_size >= need &&
            vb > stbox_shared.ram_size - need)
            vb = stbox_shared.ram_size - need;
        const uint8_t *src = stbox_shared.ram + vb;
        int res = stbox_shared.shift_res;
        uint32_t pal[16];
        for (int i = 0; i < 16; i++) {
            uint16_t p = stbox_shared.palette[i];
            pal[i] = 0xFF000000u | ((uint32_t)lvl[(p >> 8) & 7] << 16) |
                     ((uint32_t)lvl[(p >> 4) & 7] << 8) | lvl[p & 7];
        }
        if (res == 0) {
            for (int y = 0; y < vh; y++) {
                uint32_t *d = (uint32_t *)(dst + y * pitch);
                const uint8_t *s = src + y * 160;
                for (int g = 0; g < 20; g++, s += 8) {
                    uint16_t p0 = (s[0] << 8) | s[1], p1 = (s[2] << 8) | s[3];
                    uint16_t p2 = (s[4] << 8) | s[5], p3 = (s[6] << 8) | s[7];
                    for (int b = 15; b >= 0; b--)
                        *d++ = pal[((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) |
                                   (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3)];
                }
            }
            *out_w = 320; *out_h = vh;
        } else if (res == 1) {
            for (int y = 0; y < 200; y++) {
                uint32_t *d = (uint32_t *)(dst + y * pitch);
                const uint8_t *s = src + y * 160;
                for (int g = 0; g < 40; g++, s += 4) {
                    uint16_t p0 = (s[0] << 8) | s[1], p1 = (s[2] << 8) | s[3];
                    for (int b = 15; b >= 0; b--)
                        *d++ = pal[((p0 >> b) & 1) | (((p1 >> b) & 1) << 1)];
                }
            }
            *out_w = 640; *out_h = 200;
        } else {
            uint32_t fg = 0xFF000000u, bg = 0xFFFFFFFFu;
            if (!(stbox_shared.palette[0] & 1)) { fg = bg; bg = 0xFF000000u; }
            for (int y = 0; y < 400; y++) {
                uint32_t *d = (uint32_t *)(dst + y * pitch);
                const uint8_t *s = src + y * 80;
                for (int g = 0; g < 40; g++, s += 2) {
                    uint16_t p0 = (s[0] << 8) | s[1];
                    for (int b = 15; b >= 0; b--)
                        *d++ = ((p0 >> b) & 1) ? fg : bg;
                }
            }
            *out_w = 640; *out_h = 400;
        }
        return;
    }

    /* STE path */
    const uint8_t *ram = stbox_shared.ram;
    const uint32_t rmask = stbox_shared.ram_size - 1;
    const int res = stbox_shared.shift_res;
    const int hs  = stbox_shared.hscroll & 15;
    const int lw  = stbox_shared.linewidth * 2;
    uint32_t vb = stbox_shared.video_base & rmask;
    uint32_t pal[16];
    for (int i = 0; i < 16; i++) {
        uint16_t p = stbox_shared.palette[i];
        #define STE_LVL(n) ((uint8_t)(((((n) & 7) << 1) | (((n) >> 3) & 1)) * 17))
        pal[i] = 0xFF000000u | ((uint32_t)STE_LVL(p >> 8) << 16) |
                 ((uint32_t)STE_LVL(p >> 4) << 8) | STE_LVL(p);
        #undef STE_LVL
    }
    #define RD(o) ram[((o)) & rmask]
    if (res == 0) {
        const int groups = 20 + (hs ? 1 : 0);
        const uint32_t stride = (uint32_t)groups * 8 + (uint32_t)lw;
        uint8_t px[336];
        for (int y = 0; y < vh; y++) {
            uint32_t la = vb + (uint32_t)y * stride;
            uint8_t *o = px;
            for (int g = 0; g < groups; g++, la += 8) {
                uint16_t p0 = (RD(la)     << 8) | RD(la + 1);
                uint16_t p1 = (RD(la + 2) << 8) | RD(la + 3);
                uint16_t p2 = (RD(la + 4) << 8) | RD(la + 5);
                uint16_t p3 = (RD(la + 6) << 8) | RD(la + 7);
                for (int b = 15; b >= 0; b--)
                    *o++ = (uint8_t)(((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) |
                                     (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3));
            }
            uint32_t *d = (uint32_t *)(dst + y * pitch);
            const uint8_t *s = px + hs;
            for (int x = 0; x < 320; x++) *d++ = pal[s[x]];
        }
        *out_w = 320; *out_h = vh;
    } else if (res == 1) {
        const int groups = 40 + (hs ? 1 : 0);
        const uint32_t stride = (uint32_t)groups * 4 + (uint32_t)lw;
        uint8_t px[656];
        for (int y = 0; y < 200; y++) {
            uint32_t la = vb + (uint32_t)y * stride;
            uint8_t *o = px;
            for (int g = 0; g < groups; g++, la += 4) {
                uint16_t p0 = (RD(la)     << 8) | RD(la + 1);
                uint16_t p1 = (RD(la + 2) << 8) | RD(la + 3);
                for (int b = 15; b >= 0; b--)
                    *o++ = (uint8_t)(((p0 >> b) & 1) | (((p1 >> b) & 1) << 1));
            }
            uint32_t *d = (uint32_t *)(dst + y * pitch);
            const uint8_t *s = px + hs;
            for (int x = 0; x < 640; x++) *d++ = pal[s[x]];
        }
        *out_w = 640; *out_h = 200;
    } else {
        uint32_t fg = 0xFF000000u, bg = 0xFFFFFFFFu;
        if (!(stbox_shared.palette[0] & 1)) { fg = bg; bg = 0xFF000000u; }
        const int groups = 40 + (hs ? 1 : 0);
        const uint32_t stride = (uint32_t)groups * 2 + (uint32_t)lw;
        uint8_t px[656];
        for (int y = 0; y < 400; y++) {
            uint32_t la = vb + (uint32_t)y * stride;
            uint8_t *o = px;
            for (int g = 0; g < groups; g++, la += 2) {
                uint16_t p0 = (RD(la) << 8) | RD(la + 1);
                for (int b = 15; b >= 0; b--) *o++ = (uint8_t)((p0 >> b) & 1);
            }
            uint32_t *d = (uint32_t *)(dst + y * pitch);
            const uint8_t *s = px + hs;
            for (int x = 0; x < 640; x++) *d++ = s[x] ? fg : bg;
        }
        *out_w = 640; *out_h = 400;
    }
    #undef RD
}

/* ------------------------------------------------------------------ */
/* render thread                                                      */
/* ------------------------------------------------------------------ */
static void default_rect(int sw, int sh, int *dx, int *dy, int *dw, int *dh)
{
    /* largest integer scale that fits the display (square pixels for low
     * res means x2 vertical for med, x2 horizontal for high handled by
     * the natural aspect of each mode's pixel grid: just scale the source
     * to 4:3-ish by treating low as 320x200 doubled). */
    int scale = 1;
    while ((sw * (scale + 1)) <= g_mode_w && (sh * (scale + 1)) <= g_mode_h)
        scale++;
    *dw = sw * scale; *dh = sh * scale;
    *dx = (g_mode_w - *dw) / 2; *dy = (g_mode_h - *dh) / 2;
}

static void *render_main(void *arg)
{
    (void)arg;

    uint32_t last_frame = (uint32_t)-1;
    /* health line every 5 s while running: pace, plus the input state -
     * focus/routing and what actually reached the sandbox IKBD - so a
     * "mouse does nothing" report can be read off the log. */
    time_t health_at = time(NULL) + 5;
    uint32_t last_frame_h = stbox_shared.frame;
    while (g_run) {
        if (time(NULL) >= health_at) {
            health_at += 5;
            uint32_t in[6];
            stbox_input_stats(in);
            uint32_t sc[5];
            stbox_core_sched_stats(sc);
            fprintf(stderr, "[STBOX] health: %u frames/5s, %u cps "
                    "(expect ~250, ~8021248) focus=%d route=%d "
                    "in: key=%u mouse=%u joy=%u raw=%u drop=%u acia_rx=%u\n"
                    "[STBOX]   sched: calls/s=%u slices/s=%u stop:cap=%u debt=%u "
                    "burst~%u ns\n",
                    stbox_shared.frame - last_frame_h, stbox_core_cps(),
                    g_focus, g_route,
                    in[0], in[1], in[2], in[3], in[4], in[5],
                    sc[0], sc[1], sc[2], sc[3], sc[4]);
            {
                extern void stbox_core_irq_state(uint32_t out[5]);
                uint32_t is[5];
                stbox_core_irq_state(is);
                fprintf(stderr, "[STBOX]   irq: sr=%04X pc=%06X "
                        "IERB=%02X IMRB=%02X IPRB=%02X ISRB=%02X VR=%02X "
                        "acia sr=%02X cr=%02X gpip=%02X fifo=%u\n",
                        is[0] & 0xFFFFu, is[1] & 0xFFFFFFu,
                        is[2] >> 24, (is[2] >> 16) & 0xFF, (is[2] >> 8) & 0xFF,
                        is[2] & 0xFF, is[3] >> 24, (is[3] >> 16) & 0xFF,
                        (is[3] >> 8) & 0xFF, is[3] & 0xFF, is[4]);
            }
            {
                static unsigned last_tr;
                unsigned tr = stbox_trace_count;
                if (tr != last_tr) {
                    fprintf(stderr, "[STBOX]   trace: %u exceptions (+%u/5s); first at ppc=%06X "
                            "-> handler %06X (pc=%06X sr=%04X a0=%08X sp=%08X)\n", tr, tr - last_tr,
                            stbox_trace_first_ppc, stbox_trace_vec, stbox_trace_first_pc,
                            stbox_trace_first_sr, stbox_trace_first_a0, stbox_trace_first_sp);
                    if (last_tr == 0) {
                        fprintf(stderr, "[STBOX]   trace frame+stack: %08X %08X %08X %08X %08X %08X\n",
                                stbox_trace_first_stack[0], stbox_trace_first_stack[1],
                                stbox_trace_first_stack[2], stbox_trace_first_stack[3],
                                stbox_trace_first_stack[4], stbox_trace_first_stack[5]);
                        uint32_t p = stbox_trace_first_ppc >= 16 ? stbox_trace_first_ppc - 16 : 0;
                        fprintf(stderr, "[STBOX]   around the traced instruction:\n");
                        for (int k = 0; k < 10; k++) {
                            char buf[96]; buf[0] = 0;
                            int len = m68k_disassemble(buf, p, M68K_CPU_TYPE_68000);
                            fprintf(stderr, "[STBOX]   %s%06X  %s\n",
                                    p == stbox_trace_first_ppc ? ">" : " ", p, buf);
                            p += (len > 0) ? (uint32_t)len : 2u;
                        }
                        p = stbox_trace_vec;
                        fprintf(stderr, "[STBOX]   trace handler:\n");
                        for (int k = 0; k < 24 && p; k++) {
                            char buf[96]; buf[0] = 0;
                            int len = m68k_disassemble(buf, p, M68K_CPU_TYPE_68000);
                            fprintf(stderr, "[STBOX]    %06X  %s\n", p, buf);
                            p += (len > 0) ? (uint32_t)len : 2u;
                        }
                    }
                    last_tr = tr;
                }
            }
            static int dbg = -1;
            if (dbg < 0) { const char *e = getenv("PISTORM_STBOX_DBG"); dbg = (e && *e == '1'); }
            if (dbg) {
                uint32_t pc = stbox_dbg_pc;
                char dis[80]; dis[0] = 0;
                m68k_disassemble(dis, pc, M68K_CPU_TYPE_68000);
                fprintf(stderr, "[STBOX]   pc=%06X [%s] pc-window=%06X..%06X "
                        "lastHWread=%06X @pc %06X\n",
                        pc, dis, stbox_dbg_pc_lo, stbox_dbg_pc_hi,
                        stbox_dbg_last_hwr, stbox_dbg_last_hwr_pc);
                stbox_dbg_pc_lo = 0xFFFFFFFFu; stbox_dbg_pc_hi = 0;
            }
            last_frame_h = stbox_shared.frame;
        }
        {
            static unsigned seen;
            unsigned n = stbox_exc_count;
            if (n != seen) {
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                unsigned from = (n - seen > STBOX_EXC_RING) ? n - STBOX_EXC_RING : seen;
                for (unsigned i = from; i != n; i++) {
                    stbox_exc_info_t e = stbox_exc_ring[i & (STBOX_EXC_RING - 1)];
                    char d1[80]; d1[0] = 0;
                    m68k_disassemble(d1, e.ppc, M68K_CPU_TYPE_68000);
                    fprintf(stderr, "[STBOX] EXCEPTION #%u vec %u (%s) at guest cycle %llu:"
                            " ppc=%06X [%s] sr=%04X sp=%08X d0=%08X a0=%08X a1=%08X a6=%08X\n",
                            i + 1, e.vector,
                            e.vector == 2 ? "bus error" : e.vector == 3 ? "ADDRESS ERROR" :
                            e.vector == 4 ? "illegal" : "privilege",
                            (unsigned long long)e.cycles,
                            e.ppc, d1, e.sr, e.sp, e.d0, e.a0, e.a1, e.a6);
                    fprintf(stderr, "[STBOX]   stack: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                            e.stack[0], e.stack[1], e.stack[2], e.stack[3],
                            e.stack[4], e.stack[5], e.stack[6], e.stack[7]);
                    if (e.vector == 3 || e.vector == 4) {
                        /* the road to the fault: disassemble from a little
                         * before ppc (may start mid-instruction; resyncs) */
                        uint32_t p = e.ppc >= 32 ? e.ppc - 32 : 0;
                        fprintf(stderr, "[STBOX]   code before the fault:\n");
                        for (int k = 0; k < 14 && p <= e.ppc + 8; k++) {
                            char buf[96]; buf[0] = 0;
                            int len = m68k_disassemble(buf, p, M68K_CPU_TYPE_68000);
                            fprintf(stderr, "[STBOX]   %s%06X  %s\n",
                                    p == e.ppc ? ">" : " ", p, buf);
                            p += (len > 0) ? (uint32_t)len : 2u;
                        }
                    }
                }
                seen = n;
            }
        }
        if (stbox_core_take_halt_report()) {
            fprintf(stderr, "[STBOX] DOUBLE BUS FAULT - box halted. "
                    "first fault @ %06x, second @ %06x, pc=%06x ppc=%06x "
                    "sr=%04x sp=%08x\n",
                    stbox_halt_info.fault1, stbox_halt_info.fault2,
                    stbox_halt_info.pc, stbox_halt_info.ppc,
                    stbox_halt_info.sr, stbox_halt_info.sp);
            fprintf(stderr, "[STBOX] last PCs (oldest first):");
            unsigned idx = stbox_pc_ring_idx;
            for (unsigned i = 0; i < STBOX_PC_RING; i++)
                fprintf(stderr, "%s%06x", (i % 8) ? " " : "\n[STBOX]   ",
                        stbox_pc_ring[(idx + i) & (STBOX_PC_RING - 1)]);
            fprintf(stderr, "\n");

            /* The box is halted, its RAM is stable, and we link the
             * disassembler: print every 256-byte code page the ring
             * visited, so the report shows what the code DID, not just
             * where it went. */
            /* TOS's crash vault: when the BOMBS handler ran, the REAL
             * exception (number, registers, frame with the true PC) was
             * archived at $380 (proc_lives = $12345678). The PC ring can
             * be all TOS drawing bombs; this is the actual crash. */
            uint8_t *ram = stbox_shared.ram;
            uint32_t crash_pc = 0;
            uint32_t lives = ((uint32_t)ram[0x380] << 24) | (ram[0x381] << 16) |
                             (ram[0x382] << 8) | ram[0x383];
            if (lives == 0x12345678) {
                /* TOS 1.x high-byte-vector trick: the exception vectors
                 * carry the VECTOR NUMBER in the top byte of the handler
                 * address; the 68000 ignores it on the bus and the bomb
                 * handler pops it back out of the return address at $3C4.
                 * So the exception number is that longword's TOP BYTE. */
                uint32_t penum = ram[0x3C4];
                /* proc_stk at $3CC: group-0 frames (bus=2/addr=3) are
                 * [func.w][addr.l][ir.w][sr.w][pc.l] -> pc at +10;
                 * others [sr.w][pc.l] -> pc at +2 */
                uint32_t off = (penum == 2 || penum == 3) ? 0x3CC + 10
                                                          : 0x3CC + 2;
                crash_pc = ((uint32_t)ram[off] << 24) | (ram[off+1] << 16) |
                           (ram[off+2] << 8) | ram[off+3];
                uint32_t fault_addr = ((uint32_t)ram[0x3CE] << 24) |
                                      (ram[0x3CF] << 16) |
                                      (ram[0x3D0] << 8) | ram[0x3D1];
                fprintf(stderr, "[STBOX] TOS crash vault: ORIGINAL exception "
                        "#%u (%s) at pc=%06x%s%06x - the bombs and the "
                        "double fault are aftermath\n",
                        penum,
                        penum == 2 ? "bus error" :
                        penum == 3 ? "address error" :
                        penum == 4 ? "illegal instruction" :
                        penum == 5 ? "divide by zero" : "other",
                        crash_pc & 0xFFFFFF,
                        (penum == 2 || penum == 3) ? ", fault addr " : "",
                        (penum == 2 || penum == 3) ? (fault_addr & 0xFFFFFF) : 0);
            }

            uint32_t pages[5]; int npages = 0;
            if (crash_pc)
                pages[npages++] = (crash_pc & 0xFFFFFF) & ~0xFFu;
            for (unsigned i = 0; i < STBOX_PC_RING && npages < 5; i++) {
                uint32_t pg = (stbox_pc_ring[(idx + i) & (STBOX_PC_RING - 1)]
                              & 0xFFFFFF) & ~0xFFu;
                int seen = 0;
                for (int j = 0; j < npages; j++)
                    if (pages[j] == pg) seen = 1;
                if (!seen) pages[npages++] = pg;
            }
            for (int j = 0; j < npages; j++) {
                fprintf(stderr, "[STBOX] --- disassembly @ %06x ---\n",
                        pages[j]);
                uint32_t pc2 = pages[j];
                char buf[128];
                while (pc2 < pages[j] + 0x100) {
                    int n = m68k_disassemble(buf, pc2, M68K_CPU_TYPE_68000);
                    fprintf(stderr, "[STBOX]   %06x  %s\n", pc2, buf);
                    pc2 += (n > 0) ? (uint32_t)n : 2;
                }
            }
        }
        uint32_t fr = stbox_shared.frame;
        if (fr == last_frame) { usleep(2000); continue; }
        last_frame = fr;

        struct sbuf *b = &g_buf[g_cur];
        int sw, sh;
        int res_dbg = stbox_shared.shift_res;
        convert(b->map, b->pitch, &sw, &sh);

        /* The front-end reports rect/clip in GUEST DESKTOP pixels - all a
         * GEM app can know. The presenter integer-scales and centres the
         * guest image, so map through its geometry every frame (cheap, and
         * it tracks mode changes for free). */
        int gsw = (int)drmpres_src_w(), gsh = (int)drmpres_src_h();
        int ddx = drmpres_dst_x(), ddy = drmpres_dst_y();
        int ddw = drmpres_dst_w(), ddh = drmpres_dst_h();
        if (gsw <= 0 || gsh <= 0) { gsw = g_mode_w; gsh = g_mode_h;
                                    ddx = ddy = 0; ddw = g_mode_w; ddh = g_mode_h; }
        #define MAPX(v) (ddx + (int)((int64_t)(v) * ddw / gsw))
        #define MAPY(v) (ddy + (int)((int64_t)(v) * ddh / gsh))

        int dx, dy, dw, dh;
        if (g_rw > 0 && g_rh > 0) {
            dx = MAPX(g_rx); dy = MAPY(g_ry);
            dw = MAPX(g_rx + g_rw) - dx; dh = MAPY(g_ry + g_rh) - dy;
        }
        else default_rect(sw, sh, &dx, &dy, &dw, &dh);

        /* single clip rect from the front-end: crop dst, scale src */
        uint32_t sx = 0, sy = 0, vw = (uint32_t)sw, vh = (uint32_t)sh;
        if (g_cw >= 0 && g_ch >= 0) {
            int cx = MAPX(g_cx), cy = MAPY(g_cy);
            int cw = MAPX(g_cx + g_cw) - cx, ch = MAPY(g_cy + g_ch) - cy;
            int x0 = dx > cx ? dx : cx;
            int y0 = dy > cy ? dy : cy;
            int x1 = (dx + dw) < (cx + cw) ? (dx + dw) : (cx + cw);
            int y1 = (dy + dh) < (cy + ch) ? (dy + dh) : (cy + ch);
            if (x1 <= x0 || y1 <= y0) {        /* fully covered: hide  */
                commit(0, 0, 0, 0, 0, 0, 0, 0, 0);
                continue;
            }
            sx = (uint32_t)((int64_t)(x0 - dx) * sw / dw);
            sy = (uint32_t)((int64_t)(y0 - dy) * sh / dh);
            vw = (uint32_t)((int64_t)(x1 - x0) * sw / dw);
            vh = (uint32_t)((int64_t)(y1 - y0) * sh / dh);
            if (!vw) vw = 1;
            if (!vh) vh = 1;
            dx = x0; dy = y0; dw = x1 - x0; dh = y1 - y0;
        }
        int cr = commit(b->fb, sx, sy, vw, vh, dx, dy, dw, dh);
        static int commit_log = 3;     /* first three: say what happened */
        if (commit_log > 0) {
            commit_log--;
            fprintf(stderr, "[STBOX] commit fb=%u src=%u,%u %ux%u -> dst=%d,%d "
                    "%dx%d rc=%d errno=%d (frame %u, res %d, vidbase %06x)\n",
                    b->fb, sx, sy, vw, vh, dx, dy, dw, dh, cr,
                    cr < 0 ? errno : 0, fr, res_dbg, stbox_shared.video_base);
        }
        g_cur ^= 1;
    }
    commit(0, 0, 0, 0, 0, 0, 0, 0, 0);         /* hide on exit */
    return NULL;
}

/* ------------------------------------------------------------------ */
/* disk images: .ST is the raw layout the FDC model wants; .MSA is    */
/* decoded here (big-endian header, per-track RLE with $E5 marker).   */
/* ------------------------------------------------------------------ */
/* fopen with case-insensitive fallback: HOSTFS presents names to GEMDOS
 * case-insensitively, so the path the guest hands back may not match the
 * Linux filename's case exactly. If the literal open fails, scan the
 * directory for a case-insensitive match on the basename. */
static FILE *fopen_ci(const char *path, char *resolved, size_t rsz)
{
    FILE *f = fopen(path, "rb");
    if (f) { snprintf(resolved, rsz, "%s", path); return f; }

    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return NULL;
    char dir[1024];
    size_t dl = (size_t)(slash - path);
    if (dl >= sizeof dir) return NULL;
    memcpy(dir, path, dl); dir[dl] = 0;
    const char *base = slash + 1;

    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, base)) {
            snprintf(resolved, rsz, "%s/%s", dir, e->d_name);
            f = fopen(resolved, "rb");
            break;
        }
    }
    closedir(d);
    return f;
}

static uint8_t *disk_load(const char *path, uint32_t *out_size)
{
    char real[1200];
    FILE *f = fopen_ci(path, real, sizeof real);
    if (!f) { fprintf(stderr, "[STBOX] %s: %s (case-insensitive scan "
                      "of the directory also found nothing)\n",
                      path, strerror(errno)); return NULL; }
    path = real;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz < 1024 || fsz > 2 * 1024 * 1024) {
        fprintf(stderr, "[STBOX] %s: size %ld is not a floppy image "
                "(1 KB..2 MB)\n", path, fsz);
        fclose(f);
        return NULL;
    }
    uint8_t *raw = malloc((size_t)fsz);
    size_t got = raw ? fread(raw, 1, (size_t)fsz, f) : 0;
    if (!raw || got != (size_t)fsz) {
        fprintf(stderr, "[STBOX] %s: short read (%zu of %ld)%s\n",
                path, got, fsz, raw ? "" : " (alloc failed)");
        fclose(f); free(raw); return NULL;
    }
    fclose(f);

    if (!(raw[0] == 0x0E && raw[1] == 0x0F)) {     /* plain .ST */
        *out_size = (uint32_t)fsz;
        return raw;
    }

    /* MSA */
    uint16_t spt    = (uint16_t)((raw[2] << 8) | raw[3]);
    uint16_t sides  = (uint16_t)(((raw[4] << 8) | raw[5]) + 1);
    uint16_t tstart = (uint16_t)((raw[6] << 8) | raw[7]);
    uint16_t tend   = (uint16_t)((raw[8] << 8) | raw[9]);
    uint32_t tlen   = 512u * spt;
    uint32_t total  = tlen * sides * (uint32_t)(tend - tstart + 1);
    if (!spt || spt > 12 || sides > 2 || tend < tstart || total > 2048u * 1024) {
        fprintf(stderr, "[STBOX] %s: bad MSA header\n", path);
        free(raw); return NULL;
    }
    uint8_t *img = calloc(1, total);
    if (!img) { free(raw); return NULL; }
    uint32_t src = 10, dst = 0;
    for (uint32_t t = 0; t < (uint32_t)(tend - tstart + 1) * sides; t++) {
        if (src + 2 > (uint32_t)fsz) goto bad;
        uint32_t dlen = (uint32_t)((raw[src] << 8) | raw[src + 1]);
        src += 2;
        if (src + dlen > (uint32_t)fsz || dst + tlen > total) goto bad;
        if (dlen == tlen) {
            memcpy(img + dst, raw + src, tlen);
        } else {                                    /* RLE */
            uint32_t o = dst, e = src + dlen, i = src;
            while (i < e && o < dst + tlen) {
                uint8_t b = raw[i++];
                if (b != 0xE5) { img[o++] = b; continue; }
                if (i + 3 > e) goto bad;
                uint8_t v = raw[i];
                uint32_t n = (uint32_t)((raw[i + 1] << 8) | raw[i + 2]);
                i += 3;
                while (n-- && o < dst + tlen) img[o++] = v;
            }
        }
        src += dlen;
        dst += tlen;
    }
    free(raw);
    *out_size = total;
    return img;
bad:
    fprintf(stderr, "[STBOX] %s: corrupt MSA data\n", path);
    free(raw); free(img);
    return NULL;
}

int stbox_disk_insert_path(const char *path)
{
    uint32_t size = 0;
    uint8_t *img = disk_load(path, &size);
    if (!img) return -1;
    uint8_t *old = stbox_core_disk_insert(img, size);
    free(old);
    g_disk_cur = img;
    fprintf(stderr, "[STBOX] drive A: %s (%u KB)\n", path, size / 1024);
    return 0;
}

/* ------------------------------------------------------------------ */
/* public lifecycle                                                   */
/* ------------------------------------------------------------------ */
int stbox_host_init(void) { return 0; }        /* lazy: work is in start */

int stbox_start(const stbox_cfg_t *cfg)
{
    if (g_started) return -1;
    g_cfg = *cfg;
    if (!g_cfg.ram_kb) g_cfg.ram_kb = 4096;
    uint32_t ram_size = g_cfg.ram_kb * 1024u;

    /* ROM, most explicit first:
     *   1. the path STBOX.PRG passed for THIS launch (a TOS on its
     *      command line);
     *   2. the .cfg's stbox_tos - the user's own choice in PSCTRL, and
     *      the whole point of that field. It only overrode nothing before
     *      because it sat BELOW the env, so the service's install default
     *      won every time and the setting looked ignored;
     *   3. PISTORM_STBOX_TOS - the install default, set in the systemd
     *      unit, used when the user has chosen nothing.
     * emulator_config_stbox_tos() is already resolved against rom_path at
     * load, so a bare "tos104uk.rom" from the picker becomes ~/roms/... */
    const char *path = g_cfg.tos_path[0] ? g_cfg.tos_path : NULL;
    if (!path || !*path)
        path = emulator_config_stbox_tos();
    if (!path || !*path)
        path = getenv("PISTORM_STBOX_TOS");
    if (!path || !*path) {
        fprintf(stderr, "[STBOX] no TOS image (STBOX.PRG path, "
                        "PISTORM_STBOX_TOS, or cfg 'stbox_tos')\n");
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[STBOX] %s: %s\n", path, strerror(errno)); return -1; }
    static uint8_t rom[512 * 1024];
    size_t rsz = fread(rom, 1, sizeof(rom), f);
    fclose(f);

    /* +32K guard slop: no reader with a racy base can fall off the end */
    g_ram = calloc(1, ram_size + 32768);
    if (!g_ram) return -1;

    stbox_core_set_machine(g_cfg.machine_ste);   /* before the first reset */
    if (stbox_core_setup(g_ram, ram_size, rom, (uint32_t)rsz)) {
        fprintf(stderr, "[STBOX] core setup failed (rom %zu bytes)\n", rsz);
        free(g_ram); g_ram = NULL;
        return -1;
    }

    if (pick_plane()) { free(g_ram); g_ram = NULL; return -1; }
    for (int i = 0; i < NBUF; i++)
        if (buf_alloc(&g_buf[i])) {
            fprintf(stderr, "[STBOX] buffer alloc failed\n");
            for (int j = 0; j <= i; j++) buf_free(&g_buf[j]);
            free(g_ram); g_ram = NULL;
            return -1;
        }

    /* stbox_start runs in the NatFeat handler, i.e. ON the CPU thread:
     * SCHED_FIFO 99 pinned to isolated core 2. A thread created here with
     * default attributes is BORN FIFO-99 on core 2, queued behind a
     * real-time spinner that never yields - it never executes its first
     * instruction, so it cannot demote itself from inside. Policy and
     * affinity must be set on the attr, in the creator. vidplay.c calls
     * this "the whole ballgame" and it is. */
    pthread_attr_t attr;
    struct sched_param sp;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    CPU_SET(1, &set);
    memset(&sp, 0, sizeof sp);
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setaffinity_np(&attr, sizeof set, &set);

    g_run = 1;
    int trc = pthread_create(&g_thread, &attr, render_main, NULL);
    pthread_attr_destroy(&attr);
    if (trc) {
        for (int i = 0; i < NBUF; i++) buf_free(&g_buf[i]);
        free(g_ram); g_ram = NULL;
        g_run = 0;
        return -1;
    }

    uint64_t now, frq;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    if (!frq) frq = 54000000ULL;
    stbox_core_arm(now, frq);
    extern void jit_request_cpu_exit(void);
    stbox_cpu_kick = jit_request_cpu_exit;
    g_started = 1;
    stbox_psg_start();
    if (g_cfg.machine_ste)
        stbox_dmasnd_start();
    if (g_cfg.floppy_a[0])
        stbox_disk_insert_path(g_cfg.floppy_a);
    fprintf(stderr, "[STBOX] running: %s, %u KB, %s (build %s %s)\n",
            path, g_cfg.ram_kb, g_cfg.machine_ste ? "STE" : "ST",
            __DATE__, __TIME__);
    return 0;
}

void stbox_stop(void)
{
    if (!g_started) return;
    stbox_rfdc_abort();                    /* no new real-FDC errands */
    stbox_core_disarm();
    g_run = 0;
    pthread_join(g_thread, NULL);
    for (int i = 0; i < NBUF; i++) buf_free(&g_buf[i]);
    free(g_ram); g_ram = NULL;
    free(g_disk_cur); g_disk_cur = NULL;
    stbox_psg_stop();
    stbox_dmasnd_stop();
    g_started = 0;
    fprintf(stderr, "[STBOX] stopped\n");
}

void stbox_reset(void) { stbox_request_reset(); }

int stbox_running(void) { return g_started; }

void stbox_set_rect(int x, int y, int w, int h)
{ g_rx = x; g_ry = y; g_rw = w; g_rh = h; }

void stbox_set_clip(int x, int y, int w, int h)
{ g_cx = x; g_cy = y; g_cw = w; g_ch = h; }

void stbox_set_focus(int focused)
{
    if (g_focus != focused)
        fprintf(stderr, "[STBOX] focus %d -> %d (routing %s)\n",
                g_focus, focused, g_route ? "on" : "off");
    g_focus = focused;
}
void stbox_note_route(int on)     { g_route = on; }
int  stbox_get_focus(void)        { return g_focus; }

void stbox_get_stats(uint32_t out[4])
{
    out[0] = stbox_core_cps();
    out[1] = stbox_shared.frame;
    out[2] = stbox_core_overruns();
    out[3] = (uint32_t)g_started;
}

void stbox_host_shutdown(void) { stbox_stop(); }
