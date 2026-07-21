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

/* native double-buffered source */
struct srcbuf {
    uint32_t handle, fb, pitch;
    uint64_t size;
    uint8_t *map;
};
static struct srcbuf g_src[2];
static uint32_t g_src_w, g_src_h;
static int g_back;          /* index the caller writes the next frame into */
static int g_have_src;

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
        free_dumb(&g_src[0]);
        free_dumb(&g_src[1]);
        g_have_src = 0;
    }
    if (make_dumb(w, h, &g_src[0]) < 0)
        return -1;
    if (make_dumb(w, h, &g_src[1]) < 0) {
        free_dumb(&g_src[0]);
        return -1;
    }
    g_src_w = w;
    g_src_h = h;
    g_back = 0;
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

    int back = g_back;

    /* present the current back buffer, scaled to fill the display.
     * src rect is 16.16 fixed point. */
    if (drmModeSetPlane(g_fd, g_plane_id, g_crtc_id, g_src[back].fb, 0,
                        0, 0, g_mode.hdisplay, g_mode.vdisplay,
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

void drmpres_close(void)
{
    if (g_fd < 0)
        return;

    if (g_have_src) {
        free_dumb(&g_src[0]);
        free_dumb(&g_src[1]);
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
