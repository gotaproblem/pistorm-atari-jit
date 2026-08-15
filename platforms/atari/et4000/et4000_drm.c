/* et4000_drm.c - direct DRM/KMS scanout presenter (phase 2).
 * See et4000_drm.h for the design summary.
 *
 * Layout:
 *   - a black mode-size PRIMARY dumb buffer, set once with SetCrtc to light the
 *     display at its preferred mode;
 *   - a scaling PLANE (overlay preferred, primary fallback) onto which we push
 *     the guest image with drmModeSetPlane, src = native guest size, dst = the
 *     whole display, so the vc4 HVS upscales it to fullscreen during scanout;
 *   - TWO native-size source dumb buffers (double buffer). The caller copies the
 *     visible frame into the back one (from the emulator's padded staging
 *     buffer); flip presents it and swaps - the buffer being scanned out is
 *     never written, so no shear.
 *
 * Legacy (non-atomic) KMS: SetCrtc once, then drmModeSetPlane per frame. libdrm
 * turns the setplane into an atomic commit on vc4, latched at vblank.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <stdlib.h>
#include <math.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "et4000_drm.h"

static int g_fd = -1;
static uint32_t g_conn_id;
static uint32_t g_crtc_id;
static int g_crtc_index = -1;
static drmModeModeInfo g_mode;
static drmModeCrtc *g_saved_crtc = NULL;

/* black mode-size primary (for SetCrtc) */
static uint32_t g_bg_handle, g_bg_fb, g_bg_pitch;
static uint64_t g_bg_size;
static uint8_t *g_bg_map = NULL;

/* scaling plane */
static uint32_t g_plane_id;

/* native source buffers. Blocking (legacy) path uses 2 (double buffer). Async
 * (atomic) path uses 3 (triple buffer): scanned + pending + one the render
 * thread can always write without waiting or tearing. */
#define NUM_SRC 3
struct srcbuf {
    uint32_t handle, fb, pitch;
    uint64_t size;
    uint8_t *map;
};
static struct srcbuf g_src[NUM_SRC];
static int g_nsrc;          /* how many buffers we actually allocated (2 or 3) */
static uint32_t g_src_w, g_src_h;
static int g_back;          /* index the caller writes the next frame into */
static int g_have_src;

/* ---- async atomic present (PISTORM_DRM_ASYNC=1) --------------------------- */
static int g_async;              /* atomic async available + enabled */
static int g_flip_pending;       /* an async commit is in flight (awaiting event) */
static int g_first_flip = 1;     /* first atomic commit: blocking + ALLOW_MODESET */
static int g_last_committed = -1;/* buffer index of the previous successful commit */
/* plane atomic property ids */
/* WHERE THE GUEST IMAGE LANDS ON THE DISPLAY.
 *
 * This used to be "the whole thing, always": src 640x400 stretched to
 * 1920x1080. The scaling filter is Nearest Neighbor (deliberately - it keeps
 * pixel art crisp), and 1080/400 is 2.7, so nearest-neighbour duplicates some
 * source rows twice and some three times. On graphics nobody notices. On an
 * 8-pixel-tall font it means some scanlines of a character are twice as thick
 * as others, which is exactly the "distorted, hard to read" text.
 *
 * The cure is to scale by a whole number, so every source pixel becomes the
 * same size square, and centre what is left over. 640x400 goes to 1280x800 in
 * a 1920x1080 frame with a black border; 1280x960 goes 1:1. Text becomes
 * exactly as sharp as the guest drew it.
 *
 * PISTORM_VGA_SCALE picks the policy:
 *   integer   largest whole multiple that fits, centred  (default)
 *   aspect    fit preserving the source aspect ratio, centred
 *   stretch   fill the display - the old behaviour
 */
static int g_dst_x, g_dst_y, g_dst_w, g_dst_h;

static void compute_dst(void)
{
    static int said = 0;
    const char *e = getenv("PISTORM_VGA_SCALE");
    int mw = (int)g_mode.hdisplay, mh = (int)g_mode.vdisplay;
    int sw = (int)g_src_w, sh = (int)g_src_h;
    int mode_integer = 1, mode_stretch = 0;

    if (e && *e) {
        if (!strcasecmp(e, "stretch"))     { mode_stretch = 1; mode_integer = 0; }
        else if (!strcasecmp(e, "aspect")) { mode_integer = 0; }
    }
    if (mw <= 0 || mh <= 0 || sw <= 0 || sh <= 0) {
        g_dst_x = g_dst_y = 0; g_dst_w = mw; g_dst_h = mh;
        return;
    }
    if (mode_stretch) {
        g_dst_x = g_dst_y = 0; g_dst_w = mw; g_dst_h = mh;
    } else if (mode_integer && sw <= mw && sh <= mh) {
        int n = mw / sw;
        if (mh / sh < n) n = mh / sh;
        if (n < 1) n = 1;
        g_dst_w = sw * n; g_dst_h = sh * n;
        g_dst_x = (mw - g_dst_w) / 2; g_dst_y = (mh - g_dst_h) / 2;
    } else {
        /* Source bigger than the display, or aspect mode: fit and centre. */
        long w = mw, h = (long)mw * sh / sw;
        if (h > mh) { h = mh; w = (long)mh * sw / sh; }
        g_dst_w = (int)w; g_dst_h = (int)h;
        g_dst_x = (mw - g_dst_w) / 2; g_dst_y = (mh - g_dst_h) / 2;
    }
    if (!said || 1) {
        static int lw, lh, lx, ly;
        if (lw != g_dst_w || lh != g_dst_h || lx != g_dst_x || ly != g_dst_y) {
            lw = g_dst_w; lh = g_dst_h; lx = g_dst_x; ly = g_dst_y; said = 1;
            fprintf(stderr, "[DRM] guest %ux%u -> %dx%d at %d,%d (%s)\n",
                    g_src_w, g_src_h, g_dst_w, g_dst_h, g_dst_x, g_dst_y,
                    mode_stretch ? "stretch" :
                    (g_dst_w % sw == 0 && g_dst_h % sh == 0) ? "integer" : "aspect");
        }
    }
}

static uint32_t p_fb_id, p_crtc_id, p_crtc_x, p_crtc_y, p_crtc_w, p_crtc_h,
                p_src_x, p_src_y, p_src_w, p_src_h;

/* ---- dumb-buffer helpers -------------------------------------------------- */

static int make_dumb(uint32_t w, uint32_t h, struct srcbuf *b)
{
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof creq);
    creq.width = w;
    creq.height = h;
    creq.bpp = 32;
    if (drmIoctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
        return -1;

    uint32_t handles[4] = { creq.handle, 0, 0, 0 };
    uint32_t pitches[4] = { creq.pitch, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    uint32_t fbid = 0;
    if (drmModeAddFB2(g_fd, w, h, DRM_FORMAT_XRGB8888, handles, pitches,
                      offsets, &fbid, 0) < 0)
        goto destroy;

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof mreq);
    mreq.handle = creq.handle;
    if (drmIoctl(g_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
        goto rmfb;

    uint8_t *m = (uint8_t *)mmap(NULL, creq.size, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, g_fd, mreq.offset);
    if (m == MAP_FAILED)
        goto rmfb;
    memset(m, 0, creq.size);

    b->handle = creq.handle;
    b->pitch  = creq.pitch;
    b->size   = creq.size;
    b->fb     = fbid;
    b->map    = m;
    return 0;

rmfb:
    drmModeRmFB(g_fd, fbid);
destroy:
    {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof dreq);
        dreq.handle = creq.handle;
        drmIoctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
    return -1;
}

static void free_dumb(struct srcbuf *b)
{
    if (b->map)
        munmap(b->map, b->size);
    if (b->fb)
        drmModeRmFB(g_fd, b->fb);
    if (b->handle) {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof dreq);
        dreq.handle = b->handle;
        drmIoctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
    memset(b, 0, sizeof *b);
}

/* ---- plane selection ------------------------------------------------------ */

static int plane_type(int fd, uint32_t plane_id)
{
    drmModeObjectProperties *pr =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!pr)
        return -1;
    int type = -1;
    for (uint32_t i = 0; i < pr->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, pr->props[i]);
        if (p) {
            if (strcmp(p->name, "type") == 0)
                type = (int)pr->prop_values[i];
            drmModeFreeProperty(p);
        }
        if (type >= 0)
            break;
    }
    drmModeFreeObjectProperties(pr);
    return type;
}

static int plane_has_format(drmModePlane *pl, uint32_t fmt)
{
    for (uint32_t i = 0; i < pl->count_formats; i++)
        if (pl->formats[i] == fmt)
            return 1;
    return 0;
}

/* Ask the plane to scale with nearest-neighbour instead of the HVS default
 * smoothing filter, so upscaled low-res guest modes stay crisp (matches SDL's
 * SDL_ScaleModeNearest). Best-effort: warns if the kernel/plane lacks the
 * SCALING_FILTER property, in which case scaling stays soft. */
static void plane_set_nearest(int fd, uint32_t plane_id)
{
    drmModeObjectProperties *pr =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!pr)
        return;
    for (uint32_t i = 0; i < pr->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, pr->props[i]);
        if (!p)
            continue;
        if (strcmp(p->name, "SCALING_FILTER") == 0) {
            uint64_t val = 0;
            int found = 0;
            for (int e = 0; e < p->count_enums; e++)
                if (strcmp(p->enums[e].name, "Nearest Neighbor") == 0) {
                    val = p->enums[e].value;
                    found = 1;
                    break;
                }
            if (found &&
                drmModeObjectSetProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE,
                                         p->prop_id, val) == 0)
                fprintf(stderr, "[DRM] plane scaling filter = Nearest Neighbor\n");
            else
                fprintf(stderr, "[DRM] SCALING_FILTER present but set failed: %s\n",
                        strerror(errno));
            drmModeFreeProperty(p);
            drmModeFreeObjectProperties(pr);
            return;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(pr);
    fprintf(stderr, "[DRM] no SCALING_FILTER property - scaled modes use the "
                    "HVS default (smooth) filter\n");
}

/* Pick a plane usable on our CRTC that accepts XRGB8888: overlay preferred
 * (scales, sits above the black primary); primary as fallback. 0 if none. */
static uint32_t pick_plane(int fd, int crtc_index)
{
    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    if (!pr)
        return 0;
    uint32_t overlay = 0, primary = 0;
    for (uint32_t i = 0; i < pr->count_planes; i++) {
        drmModePlane *pl = drmModeGetPlane(fd, pr->planes[i]);
        if (!pl)
            continue;
        if ((pl->possible_crtcs & (1u << crtc_index)) &&
            plane_has_format(pl, DRM_FORMAT_XRGB8888)) {
            int t = plane_type(fd, pl->plane_id);
            if (t == DRM_PLANE_TYPE_OVERLAY && !overlay)
                overlay = pl->plane_id;
            else if (t == DRM_PLANE_TYPE_PRIMARY && !primary)
                primary = pl->plane_id;
        }
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pr);
    return overlay ? overlay : primary;
}

/* ---- public API ----------------------------------------------------------- */

/* ---- async atomic helpers ------------------------------------------------- */

static uint32_t plane_prop_id(int fd, uint32_t plane, const char *name)
{
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(fd, plane, DRM_MODE_OBJECT_PLANE);
    uint32_t id = 0;
    if (props) {
        for (uint32_t i = 0; i < props->count_props && !id; i++) {
            drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
            if (p) {
                if (!strcmp(p->name, name))
                    id = p->prop_id;
                drmModeFreeProperty(p);
            }
        }
        drmModeFreeObjectProperties(props);
    }
    return id;
}

static void page_flip_handler(int fd, unsigned int seq, unsigned int tv_sec,
                              unsigned int tv_usec, unsigned int crtc_id, void *data)
{
    (void)fd; (void)seq; (void)tv_sec; (void)tv_usec; (void)crtc_id; (void)data;
    g_flip_pending = 0;
}

/* Non-blocking: consume any completed-flip events without ever waiting. */
static void drmpres_drain_flip(void)
{
    if (!g_flip_pending)
        return;
    struct pollfd pfd = { .fd = g_fd, .events = POLLIN };
    while (g_flip_pending && poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        drmEventContext ev;
        memset(&ev, 0, sizeof ev);
        ev.version = 3;
        ev.page_flip_handler2 = page_flip_handler;
        drmHandleEvent(g_fd, &ev);
    }
}

/* Enable atomic + cache the plane property ids. Best-effort: on any failure we
 * leave g_async=0 and the caller keeps the blocking legacy path. */
static void drmpres_atomic_setup(void)
{
    const char *e = getenv("PISTORM_DRM_ASYNC");
    if (!(e && *e && strcmp(e, "0") != 0))
        return;
    if (drmSetClientCap(g_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        fprintf(stderr, "[DRM] atomic cap unavailable; async present off\n");
        return;
    }
    p_fb_id   = plane_prop_id(g_fd, g_plane_id, "FB_ID");
    p_crtc_id = plane_prop_id(g_fd, g_plane_id, "CRTC_ID");
    p_crtc_x  = plane_prop_id(g_fd, g_plane_id, "CRTC_X");
    p_crtc_y  = plane_prop_id(g_fd, g_plane_id, "CRTC_Y");
    p_crtc_w  = plane_prop_id(g_fd, g_plane_id, "CRTC_W");
    p_crtc_h  = plane_prop_id(g_fd, g_plane_id, "CRTC_H");
    p_src_x   = plane_prop_id(g_fd, g_plane_id, "SRC_X");
    p_src_y   = plane_prop_id(g_fd, g_plane_id, "SRC_Y");
    p_src_w   = plane_prop_id(g_fd, g_plane_id, "SRC_W");
    p_src_h   = plane_prop_id(g_fd, g_plane_id, "SRC_H");
    g_async = p_fb_id && p_crtc_id && p_crtc_x && p_crtc_y && p_crtc_w &&
              p_crtc_h && p_src_x && p_src_y && p_src_w && p_src_h;
    fprintf(stderr, "[DRM] async atomic present: %s\n",
            g_async ? "ON (triple buffer, non-blocking)" : "unavailable (missing props)");
}

int drmpres_open(void)
{
    char card[32] = "";

    for (int i = 0; i < 4 && g_fd < 0; i++) {
        snprintf(card, sizeof card, "/dev/dri/card%d", i);
        int fd = open(card, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        drmModeRes *r = drmModeGetResources(fd);
        if (r && r->count_crtcs > 0 && r->count_connectors > 0) {
            g_fd = fd;                    /* vc4 display device */
            drmModeFreeResources(r);
            break;
        }
        if (r)
            drmModeFreeResources(r);
        close(fd);                        /* v3d render node etc. */
    }
    if (g_fd < 0) {
        fprintf(stderr, "[DRM] no display-capable /dev/dri/cardN\n");
        return -1;
    }

    /* see overlay/primary planes and use them via setplane */
    drmSetClientCap(g_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    uint64_t cap = 0;
    if (drmGetCap(g_fd, DRM_CAP_DUMB_BUFFER, &cap) < 0 || !cap) {
        fprintf(stderr, "[DRM] dumb buffers not supported\n");
        goto fail_fd;
    }

    drmModeRes *res = drmModeGetResources(g_fd);
    if (!res)
        goto fail_fd;

    /* connected connector + its preferred (first) mode */
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(g_fd, res->connectors[i]);
        if (!c)
            continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            conn = c;
            break;
        }
        drmModeFreeConnector(c);
    }
    if (!conn) {
        fprintf(stderr, "[DRM] no connected connector\n");
        drmModeFreeResources(res);
        goto fail_fd;
    }
    g_conn_id = conn->connector_id;
    g_mode = conn->modes[0];

    /* CRTC: prefer the connector's current encoder/crtc, else first free */
    g_crtc_id = 0;
    if (conn->encoder_id) {
        drmModeEncoder *e = drmModeGetEncoder(g_fd, conn->encoder_id);
        if (e) {
            g_crtc_id = e->crtc_id;
            drmModeFreeEncoder(e);
        }
    }
    if (!g_crtc_id) {
        for (int i = 0; i < conn->count_encoders && !g_crtc_id; i++) {
            drmModeEncoder *e = drmModeGetEncoder(g_fd, conn->encoders[i]);
            if (!e)
                continue;
            for (int j = 0; j < res->count_crtcs; j++) {
                if (e->possible_crtcs & (1u << j)) {
                    g_crtc_id = res->crtcs[j];
                    break;
                }
            }
            drmModeFreeEncoder(e);
        }
    }
    drmModeFreeConnector(conn);

    g_crtc_index = -1;
    for (int j = 0; j < res->count_crtcs; j++)
        if (res->crtcs[j] == g_crtc_id) {
            g_crtc_index = j;
            break;
        }
    drmModeFreeResources(res);
    if (!g_crtc_id || g_crtc_index < 0) {
        fprintf(stderr, "[DRM] no usable CRTC\n");
        goto fail_fd;
    }

    /* Reuse the mode the console/firmware already set, rather than forcing the
     * EDID-preferred mode. The active mode already has working HDMI audio and
     * the sink's HDMI-vs-DVI/infoframe decision baked in; renegotiating the
     * link to a different mode can drop HDMI audio. Falls back to the preferred
     * mode (already in g_mode) if the CRTC has no valid active mode. */
    g_saved_crtc = drmModeGetCrtc(g_fd, g_crtc_id);   /* restore on close */
    if (g_saved_crtc && g_saved_crtc->mode_valid)
        g_mode = g_saved_crtc->mode;

    /* ...unless asked to do otherwise.
     *
     * Inheriting the console mode is the safe default, but it means the guest
     * is scaled by the DISPLAY whenever the console is not already at the
     * panel's native resolution - a 1920x1080 console on a 3440x1440 monitor
     * gets stretched by the monitor's own scaler, and no amount of care on our
     * side survives that. Fixing it in cmdline.txt works but names one
     * resolution, so it has to be edited every time the machine is plugged
     * into a different screen.
     *
     *   PISTORM_VGA_MODE=preferred   use whatever the attached display says it
     *                                prefers (its native mode). Follows the
     *                                monitor around; nothing to edit.
     *   PISTORM_VGA_MODE=3440x1440   force one specific mode.
     *
     * Left unset, behaviour is exactly as before. The caution in the comment
     * above is real: changing the mode renegotiates the link and CAN drop HDMI
     * audio on some sinks, which is why this is opt-in and why it says loudly
     * what it did. */
    {
        const char *want = getenv("PISTORM_VGA_MODE");
        if (want && *want && conn->count_modes > 0) {
            drmModeModeInfo *pick = NULL;
            unsigned w = 0, h = 0;
            int i;
            if (!strcasecmp(want, "preferred") || !strcasecmp(want, "native")) {
                for (i = 0; i < conn->count_modes; i++)
                    if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
                        pick = &conn->modes[i]; break;
                    }
                if (!pick)
                    pick = &conn->modes[0];   /* EDID lists preferred first */
            } else if (sscanf(want, "%ux%u", &w, &h) == 2) {
                for (i = 0; i < conn->count_modes; i++)
                    if (conn->modes[i].hdisplay == w &&
                        conn->modes[i].vdisplay == h) {
                        /* highest refresh that matches, first match wins */
                        pick = &conn->modes[i]; break;
                    }
                if (!pick)
                    fprintf(stderr, "[DRM] PISTORM_VGA_MODE=%s is not offered "
                                    "by this display; keeping %ux%u\n",
                            want, g_mode.hdisplay, g_mode.vdisplay);
            }
            if (pick && (pick->hdisplay != g_mode.hdisplay ||
                         pick->vdisplay != g_mode.vdisplay ||
                         pick->vrefresh != g_mode.vrefresh)) {
                fprintf(stderr, "[DRM] switching %ux%u@%u -> %ux%u@%u "
                                "(PISTORM_VGA_MODE=%s). If HDMI audio stops, "
                                "this is why - unset it and set the console "
                                "mode in cmdline.txt instead.\n",
                        g_mode.hdisplay, g_mode.vdisplay, g_mode.vrefresh,
                        pick->hdisplay, pick->vdisplay, pick->vrefresh, want);
                g_mode = *pick;
            }
        }
    }

    /* black mode-size primary for SetCrtc (built via a temp srcbuf) */
    {
        struct srcbuf bg;
        memset(&bg, 0, sizeof bg);
        if (make_dumb(g_mode.hdisplay, g_mode.vdisplay, &bg) < 0) {
            fprintf(stderr, "[DRM] bg create failed: %s\n", strerror(errno));
            goto fail_fd;
        }
        g_bg_handle = bg.handle;
        g_bg_fb     = bg.fb;
        g_bg_pitch  = bg.pitch;
        g_bg_size   = bg.size;
        g_bg_map    = bg.map;
    }

    if (drmModeSetCrtc(g_fd, g_crtc_id, g_bg_fb, 0, 0,
                       &g_conn_id, 1, &g_mode) < 0) {
        fprintf(stderr, "[DRM] SetCrtc failed: %s (is another DRM master "
                        "active? run without X/Wayland/SDL-KMSDRM)\n",
                strerror(errno));
        goto fail_bg;
    }

    g_plane_id = pick_plane(g_fd, g_crtc_index);
    if (!g_plane_id) {
        fprintf(stderr, "[DRM] no XRGB8888 plane usable on this CRTC\n");
        goto fail_crtc;
    }
    plane_set_nearest(g_fd, g_plane_id);   /* crisp upscaling like SDL */
    drmpres_atomic_setup();                 /* opt-in async atomic present */

    fprintf(stderr, "[DRM] phase2 scanout up: %ux%u@%u plane=%u(%s) (%s)\n",
            g_mode.hdisplay, g_mode.vdisplay, g_mode.vrefresh, g_plane_id,
            plane_type(g_fd, g_plane_id) == DRM_PLANE_TYPE_OVERLAY
                ? "overlay" : "primary", card);
    return 0;

fail_crtc:
    if (g_saved_crtc) {
        drmModeSetCrtc(g_fd, g_saved_crtc->crtc_id, g_saved_crtc->buffer_id,
                       g_saved_crtc->x, g_saved_crtc->y,
                       &g_conn_id, 1, &g_saved_crtc->mode);
        drmModeFreeCrtc(g_saved_crtc);
        g_saved_crtc = NULL;
    }
fail_bg:
    if (g_bg_map)
        munmap(g_bg_map, g_bg_size);
    if (g_bg_fb)
        drmModeRmFB(g_fd, g_bg_fb);
    if (g_bg_handle) {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof dreq);
        dreq.handle = g_bg_handle;
        drmIoctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
    g_bg_map = NULL;
    g_bg_fb = g_bg_handle = 0;
fail_fd:
    close(g_fd);
    g_fd = -1;
    return -1;
}

int drmpres_set_source(uint32_t w, uint32_t h)
{
    if (g_fd < 0 || w == 0 || h == 0)
        return -1;
    if (g_have_src && w == g_src_w && h == g_src_h)
        return 0;                               /* unchanged */

    if (g_have_src) {
        for (int i = 0; i < g_nsrc; i++)
            free_dumb(&g_src[i]);
        g_have_src = 0;
    }
    int n = g_async ? NUM_SRC : 2;   /* triple buffer only for async */
    for (int i = 0; i < n; i++) {
        if (make_dumb(w, h, &g_src[i]) < 0) {
            for (int j = 0; j < i; j++)
                free_dumb(&g_src[j]);
            return -1;
        }
    }
    g_nsrc = n;
    g_src_w = w;
    g_src_h = h;
    g_back = 0;
    g_last_committed = -1;
    g_flip_pending = 0;
    g_first_flip = 1;
    g_have_src = 1;
    return 0;
}

uint8_t *drmpres_backbuffer(void)
{
    return g_have_src ? g_src[g_back].map : NULL;
}

uint32_t drmpres_src_pitch(void)
{
    return g_have_src ? g_src[g_back].pitch : 0;
}

uint32_t drmpres_src_w(void) { return g_src_w; }
uint32_t drmpres_src_h(void) { return g_src_h; }

void drmpres_flip(void)
{
    if (g_fd < 0 || !g_have_src)
        return;

    if (g_async) {
        static int stuck = 0;
        drmpres_drain_flip();            /* reap a completed flip, never blocks */
        if (g_flip_pending) {
            /* previous flip still in flight -> drop this frame (g_back unchanged).
             * Failsafe: if a completion event is ever missed, recover after ~8
             * frames instead of freezing the display forever. */
            if (++stuck <= 8)
                return;
            g_flip_pending = 0;
        }
        stuck = 0;

        int back = g_back;
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (!req)
            return;
        drmModeAtomicAddProperty(req, g_plane_id, p_fb_id,   g_src[back].fb);
        drmModeAtomicAddProperty(req, g_plane_id, p_crtc_id, g_crtc_id);
        compute_dst();
        drmModeAtomicAddProperty(req, g_plane_id, p_crtc_x,  g_dst_x);
        drmModeAtomicAddProperty(req, g_plane_id, p_crtc_y,  g_dst_y);
        drmModeAtomicAddProperty(req, g_plane_id, p_crtc_w,  g_dst_w);
        drmModeAtomicAddProperty(req, g_plane_id, p_crtc_h,  g_dst_h);
        drmModeAtomicAddProperty(req, g_plane_id, p_src_x,   0);
        drmModeAtomicAddProperty(req, g_plane_id, p_src_y,   0);
        drmModeAtomicAddProperty(req, g_plane_id, p_src_w,   (uint64_t)g_src_w << 16);
        drmModeAtomicAddProperty(req, g_plane_id, p_src_h,   (uint64_t)g_src_h << 16);
        /* First commit enables the plane on the CRTC: allow-modeset + blocking.
         * All later commits are the fast non-blocking flips with completion events. */
        uint32_t flags = g_first_flip
            ? DRM_MODE_ATOMIC_ALLOW_MODESET
            : (DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT);
        int r = drmModeAtomicCommit(g_fd, req, flags, NULL);
        drmModeAtomicFree(req);
        if (r == 0) {
            if (g_first_flip)
                g_first_flip = 0;   /* blocking commit: no completion event pending */
            else
                g_flip_pending = 1;
            /* next render buffer = the one that is neither the buffer we just
             * committed (back) nor the one before it (g_last_committed, still
             * scanned until this flip latches). With 3 buffers exactly one such
             * index exists, so the render thread never waits and never tears. */
            int next = 0;
            for (int i = 0; i < g_nsrc; i++)
                if (i != back && i != g_last_committed) { next = i; break; }
            g_last_committed = back;
            g_back = next;
        } else {
            static int warned = 0;
            if (!warned) {
                warned = 1;
                fprintf(stderr, "[DRM] atomic commit failed: %s -> reverting to blocking present\n",
                        strerror(errno));
            }
            g_async = 0;
            g_back = 0;                   /* legacy uses buffers 0/1 only */
        }
        return;
    }

    /* legacy blocking present (default): SetPlane latches at vblank. */
    int back = g_back;
    compute_dst();
    if (drmModeSetPlane(g_fd, g_plane_id, g_crtc_id, g_src[back].fb, 0,
                        g_dst_x, g_dst_y, g_dst_w, g_dst_h,
                        0, 0, g_src_w << 16, g_src_h << 16) < 0) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[DRM] SetPlane (scale %ux%u -> %ux%u) failed: %s\n",
                    g_src_w, g_src_h, g_mode.hdisplay, g_mode.vdisplay,
                    strerror(errno));
        }
    }

    g_back = back ^ 1;   /* the buffer being scanned is now off-limits */
}

/* ---- refresh-rate matching for video playback ------------------------------
 *
 * 24 fps content on a 60 Hz panel gets a 2:3 vblank cadence and 25 fps gets
 * 2,2,3 - judder that no amount of correct pacing removes, because it is
 * arithmetic. The cure is to run the panel at a refresh the frame rate divides
 * into: 48 or 72 Hz for 24 fps, 50 Hz for 25, 60 Hz for 30.
 *
 * Only the REFRESH changes; the resolution stays put, so the guest's plane and
 * everything scaled against it are unaffected. The original mode is kept and
 * restored when playback stops.
 *
 * Caveat, and it is the reason this can be switched off: renegotiating the
 * HDMI link can drop HDMI audio on some sinks - the same reason drmpres_open()
 * deliberately reuses the console's existing mode rather than the EDID
 * preferred one. PISTORM_VID_MODESET=0 disables the whole thing. */
static drmModeModeInfo g_orig_mode;
static int g_mode_switched;

static double refresh_error(int refresh, double fps)
{
    double n;
    if (refresh <= 0 || fps <= 0.0)
        return 1e9;
    n = (double)refresh / fps;
    if (n < 0.99)                 /* refresh below the frame rate is no use */
        return 1e9;
    return fabs(n - (double)((int)(n + 0.5)));
}

int drmpres_match_refresh(double fps)
{
    drmModeConnector *conn;
    drmModeModeInfo best;
    double best_err, cur_err;
    int i, found = 0, min_hz;
    const char *e = getenv("PISTORM_VID_MODESET");

    if (g_fd < 0 || fps <= 0.0 || (e && *e == '0'))
        return 0;

    cur_err = refresh_error(g_mode.vrefresh, fps);
    if (cur_err < 0.01)
        return 0;                 /* already an exact multiple */

    conn = drmModeGetConnector(g_fd, g_conn_id);
    if (!conn)
        return -1;

    /* Don't drop the whole desktop to 24 Hz just to suit one film: the guest
     * display and the mouse run at this rate too. 48 Hz is fine, 24 is not.
     * PISTORM_VID_MINHZ tunes the floor for anyone who disagrees. */
    {
        const char *mh = getenv("PISTORM_VID_MINHZ");
        min_hz = mh && *mh ? atoi(mh) : 48;
    }

    best_err = cur_err;
    for (i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo *m = &conn->modes[i];
        double err;
        if (m->hdisplay != g_mode.hdisplay || m->vdisplay != g_mode.vdisplay)
            continue;             /* refresh only - never change resolution */
        /* Never pick an interlaced mode. EDIDs commonly carry an interlaced
         * twin of the progressive mode at the SAME hdisplay/vdisplay/vrefresh
         * - e.g. CEA VIC 20 (1920x1080i@50) alongside VIC 31 (1920x1080@50),
         * and VIC 5 alongside VIC 16 at 60. Both score identical error here,
         * and the tie-break below only prefers a HIGHER refresh, so with the
         * interlaced entry first in connector order it would win and we would
         * hand the display half the lines per field. */
        if (m->flags & DRM_MODE_FLAG_INTERLACE)
            continue;
        if ((int)m->vrefresh < min_hz)
            continue;
        err = refresh_error(m->vrefresh, fps);
        /* prefer a lower error, then the higher refresh of two equals */
        if (err < best_err - 1e-6 ||
            (found && err < best_err + 1e-6 && m->vrefresh > best.vrefresh)) {
            best = *m;
            best_err = err;
            found = 1;
        }
    }
    /* If nothing suitable exists, say what the sink DOES offer - otherwise the
     * absence of judder improvement looks like a bug rather than an EDID that
     * simply has no 48/50 Hz mode. Printed once per resolution. */
    if (!found || best_err >= 0.01) {
        static int listed = 0;
        if (!listed) {
            char buf[256];
            int n = 0, j;
            listed = 1;
            for (j = 0; j < conn->count_modes && n < (int)sizeof(buf) - 12; j++) {
                drmModeModeInfo *m = &conn->modes[j];
                if (m->hdisplay == g_mode.hdisplay &&
                    m->vdisplay == g_mode.vdisplay)
                    n += snprintf(buf + n, sizeof(buf) - n, "%s%u",
                                  n ? ", " : "", m->vrefresh);
            }
            buf[n] = '\0';
            fprintf(stderr, "[DRM] no refresh rate suits %.2f fps. %ux%u is "
                            "offered at: %s Hz. Staying at %u Hz - expect the "
                            "usual cadence judder for this frame rate.\n",
                    fps, g_mode.hdisplay, g_mode.vdisplay,
                    n ? buf : "(none)", g_mode.vrefresh);
            /* The commonest case by far: the panel DOES offer an exact match,
             * but it is below the floor - and the floor exists to keep the
             * Atari desktop and mouse usable, not because the mode is bad. Say
             * which knob to turn rather than leaving it to be guessed from a
             * list that visibly contains the wanted number. */
            for (j = 0; j < conn->count_modes; j++) {
                drmModeModeInfo *m = &conn->modes[j];
                if (m->hdisplay == g_mode.hdisplay &&
                    m->vdisplay == g_mode.vdisplay &&
                    (int)m->vrefresh < min_hz &&
                    fabs((double)m->vrefresh - fps) < 0.5) {
                    fprintf(stderr, "[DRM] %u Hz would match exactly but is "
                                    "below the %d Hz floor (the desktop runs "
                                    "at this rate too). Launch with "
                                    "PISTORM_VID_MINHZ=%u to allow it.\n",
                            m->vrefresh, min_hz, m->vrefresh);
                    break;
                }
            }
        }
        drmModeFreeConnector(conn);
        return 0;
    }
    drmModeFreeConnector(conn);

    if (!g_mode_switched) {
        g_orig_mode = g_mode;
        g_mode_switched = 1;
    }
    if (drmModeSetCrtc(g_fd, g_crtc_id, g_bg_fb, 0, 0,
                       &g_conn_id, 1, &best) < 0) {
        fprintf(stderr, "[DRM] %uHz modeset failed: %s\n", best.vrefresh,
                strerror(errno));
        return -1;
    }
    fprintf(stderr, "[DRM] display %u -> %u Hz to match %.2f fps video "
                    "(resolution unchanged)\n",
            g_mode.vrefresh, best.vrefresh, fps);
    g_mode = best;
    return (int)best.vrefresh;
}

void drmpres_restore_refresh(void)
{
    if (g_fd < 0 || !g_mode_switched)
        return;
    if (drmModeSetCrtc(g_fd, g_crtc_id, g_bg_fb, 0, 0,
                       &g_conn_id, 1, &g_orig_mode) < 0)
        fprintf(stderr, "[DRM] could not restore %uHz: %s\n",
                g_orig_mode.vrefresh, strerror(errno));
    else
        fprintf(stderr, "[DRM] display back to %u Hz\n", g_orig_mode.vrefresh);
    g_mode = g_orig_mode;
    g_mode_switched = 0;
}

/* ---- accessors for the host video player (second plane, same master fd) --- */

int      drmpres_fd(void)         { return g_fd; }
uint32_t drmpres_crtc_id(void)    { return g_crtc_id; }
int      drmpres_crtc_index(void) { return g_crtc_index; }
uint32_t drmpres_plane_id(void)   { return g_plane_id; }
/* Where the guest image actually sits on the display. A front-end mapping its
 * own window coordinates onto display pixels has to go through this now that
 * the image no longer fills the screen. */
int drmpres_dst_x(void) { return g_dst_x; }
int drmpres_dst_y(void) { return g_dst_y; }
int drmpres_dst_w(void) { return g_dst_w ? g_dst_w : (int)g_mode.hdisplay; }
int drmpres_dst_h(void) { return g_dst_h ? g_dst_h : (int)g_mode.vdisplay; }

uint32_t drmpres_mode_w(void)     { return g_mode.hdisplay; }
uint32_t drmpres_mode_h(void)     { return g_mode.vdisplay; }

void drmpres_close(void)
{
    if (g_fd < 0)
        return;

    drmpres_restore_refresh();

    if (g_have_src) {
        for (int i = 0; i < g_nsrc; i++)
            free_dumb(&g_src[i]);
        g_have_src = 0;
    }

    if (g_saved_crtc) {
        drmModeSetCrtc(g_fd, g_saved_crtc->crtc_id, g_saved_crtc->buffer_id,
                       g_saved_crtc->x, g_saved_crtc->y,
                       &g_conn_id, 1, &g_saved_crtc->mode);
        drmModeFreeCrtc(g_saved_crtc);
        g_saved_crtc = NULL;
    }

    if (g_bg_map) {
        munmap(g_bg_map, g_bg_size);
        g_bg_map = NULL;
    }
    if (g_bg_fb)
        drmModeRmFB(g_fd, g_bg_fb);
    if (g_bg_handle) {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof dreq);
        dreq.handle = g_bg_handle;
        drmIoctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
    g_bg_fb = g_bg_handle = 0;

    close(g_fd);
    g_fd = -1;
}
