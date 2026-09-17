/*
 * setup_hdmi.c - see setup_hdmi.h. Same shape as drmpres_open() in
 * platforms/atari/et4000/et4000_drm.c: find the display-capable card,
 * take the connected connector's current mode, one dumb XRGB8888 buffer
 * the size of that mode, drmModeSetCrtc. The old CRTC is saved and put
 * back on close, so the console comes back.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "setup_hdmi.h"

static int       fd = -1;
static uint32_t  conn_id, crtc_id, fb_id, handle;
static uint32_t  pitch;
static uint64_t  size;
static uint8_t  *map_;
static drmModeModeInfo mode;
static drmModeCrtc *saved;

/* ST palette as the page uses it: paper, red, green, ink */
static const uint32_t palette[4] = { 0xFFEEEEEE, 0xFFCC0000, 0xFF00AA00,
                                     0xFF000000 };

int sh_active(void) { return fd >= 0 && map_; }

int sh_open(void)
{
    char card[32];

    for (int i = 0; i < 4 && fd < 0; i++) {
        snprintf(card, sizeof card, "/dev/dri/card%d", i);
        int f = open(card, O_RDWR | O_CLOEXEC);
        if (f < 0)
            continue;
        drmModeRes *r = drmModeGetResources(f);
        if (r && r->count_crtcs > 0 && r->count_connectors > 0) {
            fd = f;
            drmModeFreeResources(r);
            break;
        }
        if (r)
            drmModeFreeResources(r);
        close(f);                         /* a render node, not the display */
    }
    if (fd < 0)
        return -1;

    uint64_t cap = 0;
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) < 0 || !cap)
        goto fail;

    drmModeRes *res = drmModeGetResources(fd);
    if (!res)
        goto fail;

    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors && !conn; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c)
            continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
            conn = c;
        else
            drmModeFreeConnector(c);
    }
    if (!conn) {
        drmModeFreeResources(res);
        goto fail;
    }
    conn_id = conn->connector_id;
    mode = conn->modes[0];

    crtc_id = 0;
    if (conn->encoder_id) {
        drmModeEncoder *e = drmModeGetEncoder(fd, conn->encoder_id);
        if (e) {
            crtc_id = e->crtc_id;
            drmModeFreeEncoder(e);
        }
    }
    for (int i = 0; i < conn->count_encoders && !crtc_id; i++) {
        drmModeEncoder *e = drmModeGetEncoder(fd, conn->encoders[i]);
        if (!e)
            continue;
        for (int j = 0; j < res->count_crtcs && !crtc_id; j++)
            if (e->possible_crtcs & (1u << j))
                crtc_id = res->crtcs[j];
        drmModeFreeEncoder(e);
    }
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    if (!crtc_id)
        goto fail;

    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof creq);
    creq.width  = mode.hdisplay;
    creq.height = mode.vdisplay;
    creq.bpp    = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
        goto fail;
    handle = creq.handle;
    pitch  = creq.pitch;
    size   = creq.size;

    uint32_t handles[4] = { handle, 0, 0, 0 };
    uint32_t pitches[4] = { pitch, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    if (drmModeAddFB2(fd, mode.hdisplay, mode.vdisplay, DRM_FORMAT_XRGB8888,
                      handles, pitches, offsets, &fb_id, 0) < 0)
        goto fail;

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof mreq);
    mreq.handle = handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
        goto fail;
    map_ = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (map_ == MAP_FAILED) {
        map_ = NULL;
        goto fail;
    }
    memset(map_, 0, size);

    saved = drmModeGetCrtc(fd, crtc_id);
    if (drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &conn_id, 1, &mode) < 0)
        goto fail;

    printf("[SETUP] HDMI mirror on %ux%u\n", mode.hdisplay, mode.vdisplay);
    return 0;

fail:
    sh_close();
    return -1;
}

void sh_present(const struct ss_screen *ss)
{
    if (!sh_active())
        return;

    int sw = ss->width, sh_ = ss->height;
    int n = (int)mode.hdisplay / sw;
    if ((int)mode.vdisplay / sh_ < n)
        n = (int)mode.vdisplay / sh_;
    if (n < 1)
        n = 1;
    int dw = sw * n, dh = sh_ * n;
    int ox = ((int)mode.hdisplay - dw) / 2, oy = ((int)mode.vdisplay - dh) / 2;

    for (int y = 0; y < sh_; y++) {
        uint32_t row[640];
        for (int x = 0; x < sw; x++) {
            int c;
            if (ss->planes == 1)
                c = ((ss->shadow[y * 80 + (x >> 3)] >> (7 - (x & 7))) & 1) ? 3 : 0;
            else {
                int g = x >> 4, bit = 15 - (x & 15);
                c = 0;
                for (int p = 0; p < 2; p++) {
                    const uint8_t *b = &ss->shadow[y * 160 + g * 4 + p * 2];
                    uint16_t w = (uint16_t)((b[0] << 8) | b[1]);
                    c |= ((w >> bit) & 1) << p;
                }
            }
            row[x] = palette[c];
        }
        for (int r = 0; r < n; r++) {
            uint32_t *dst = (uint32_t *)(map_ + (size_t)(oy + y * n + r) * pitch)
                            + ox;
            for (int x = 0; x < sw; x++)
                for (int k = 0; k < n; k++)
                    dst[x * n + k] = row[x];
        }
    }
}

void sh_close(void)
{
    if (fd < 0)
        return;
    if (saved) {
        drmModeSetCrtc(fd, saved->crtc_id, saved->buffer_id, saved->x, saved->y,
                       &conn_id, 1, &saved->mode);
        drmModeFreeCrtc(saved);
        saved = NULL;
    }
    if (map_) {
        munmap(map_, size);
        map_ = NULL;
    }
    if (fb_id) {
        drmModeRmFB(fd, fb_id);
        fb_id = 0;
    }
    if (handle) {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof dreq);
        dreq.handle = handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        handle = 0;
    }
    close(fd);
    fd = -1;
}
