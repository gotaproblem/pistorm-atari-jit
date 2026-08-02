/* SPDX-License-Identifier: MIT
 *
 * vidplane.c - second DRM/KMS plane (YUV) for host video playback.
 * See vidplane.h for the design summary.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "vidplane.h"
#include "../et4000/et4000_drm.h"

#define ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((a) - 1))

struct vpbuf {
    uint32_t handle;
    uint32_t fb;
    uint32_t pitch[3];
    uint32_t offset[3];
    uint64_t size;
    uint8_t *map;
};

static int      g_fd = -1;
static uint32_t g_plane_id;
static uint32_t g_crtc_id;
static uint32_t g_mode_w, g_mode_h;
static uint32_t g_mode_hz = 60;
static int      g_crtc_index;
static int      g_vblank_ok = 1;
static unsigned g_busy_commits;
static uint32_t g_formats[64];
static uint32_t g_nformats;

static struct vpbuf g_buf[VIDPLANE_MAXBUF];
static int      g_nbuf;
static uint32_t g_w, g_h, g_fourcc;
static int      g_visible;

/* BACKDROP PLANE.
 *
 * A film whose shape does not match the display leaves bars above and below
 * it, and what shows through them is the guest plane - i.e. the Atari desktop,
 * which is not what anyone means by fullscreen. A second overlay plane holding
 * one full-screen black frame, sitting between the guest and the picture,
 * covers them.
 *
 * It costs display bandwidth like any other plane, but the arithmetic is kind:
 * source and destination are the same size, so the vc4 load tracker's
 * vscale_factor is 1 and the whole thing is w*h*cpp*refresh - about 190 MB/s
 * at 1080p60, under a tenth of the budget. And it is only ever switched on
 * when bars actually exist, which by definition means a film shorter than the
 * display, which is the cheap case for the picture plane too. */
static uint32_t     g_bd_plane;
static uint32_t     g_bd_fourcc;
static struct vpbuf g_bd;
static int          g_bd_on;

/* ------------------------------------------------------------------ utils */

/* Look a property up on an object and report everything we need to decide what
 * to do with it: its id, its current value, whether it is writable, and (for a
 * range property) its limits. Returns 0 if the object has no such property. */
static uint32_t prop_info(uint32_t obj, uint32_t type, const char *name,
                          uint64_t *cur, int *mutable_, uint64_t *lo,
                          uint64_t *hi)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(g_fd, obj, type);
    uint32_t id = 0;
    if (!props)
        return 0;
    for (uint32_t i = 0; i < props->count_props && !id; i++) {
        drmModePropertyRes *p = drmModeGetProperty(g_fd, props->props[i]);
        if (!p)
            continue;
        if (!strcmp(p->name, name)) {
            id = p->prop_id;
            if (cur)     *cur = props->prop_values[i];
            if (mutable_) *mutable_ = (p->flags & DRM_MODE_PROP_IMMUTABLE) ? 0 : 1;
            if (lo)      *lo = (p->count_values > 0) ? p->values[0] : 0;
            if (hi)      *hi = (p->count_values > 1) ? p->values[1] : 0;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

/* Current zpos of a plane, or -1 if it has none. */
static long plane_zpos(uint32_t plane)
{
    uint64_t cur = 0;
    return prop_info(plane, DRM_MODE_OBJECT_PLANE, "zpos", &cur, NULL, NULL, NULL)
           ? (long)cur : -1;
}

/* Set an enum property by the name of one of its values. Best effort. */
static int set_enum_prop(uint32_t plane, const char *pname, const char *vname)
{
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(g_fd, plane, DRM_MODE_OBJECT_PLANE);
    int done = 0;
    if (!props)
        return 0;
    for (uint32_t i = 0; i < props->count_props && !done; i++) {
        drmModePropertyRes *p = drmModeGetProperty(g_fd, props->props[i]);
        if (!p)
            continue;
        if (!strcmp(p->name, pname)) {
            for (int e = 0; e < p->count_enums; e++) {
                if (!strcmp(p->enums[e].name, vname)) {
                    done = drmModeObjectSetProperty(g_fd, plane,
                                                    DRM_MODE_OBJECT_PLANE,
                                                    p->prop_id,
                                                    p->enums[e].value) == 0;
                    break;
                }
            }
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return done;
}

/* defined further down, next to the import cache they share state with */
static void atomic_setup(void);
static int  commit_plane(uint32_t fb, uint32_t sx, uint32_t sy,
                         uint32_t sw, uint32_t sh,
                         int dx, int dy, int dw, int dh);

static int plane_type_of(uint32_t plane_id)
{
    drmModeObjectProperties *pr =
        drmModeObjectGetProperties(g_fd, plane_id, DRM_MODE_OBJECT_PLANE);
    int type = -1;
    if (!pr)
        return -1;
    for (uint32_t i = 0; i < pr->count_props && type < 0; i++) {
        drmModePropertyRes *p = drmModeGetProperty(g_fd, pr->props[i]);
        if (p) {
            if (!strcmp(p->name, "type"))
                type = (int)pr->prop_values[i];
            drmModeFreeProperty(p);
        }
    }
    drmModeFreeObjectProperties(pr);
    return type;
}

static int has_fmt(drmModePlane *pl, uint32_t f)
{
    for (uint32_t i = 0; i < pl->count_formats; i++)
        if (pl->formats[i] == f)
            return 1;
    return 0;
}

/* --------------------------------------------------------------- plane pick */

int vidplane_open(void)
{
    if (g_fd >= 0)
        return 0;

    int fd = drmpres_fd();
    if (fd < 0) {
        fprintf(stderr, "[VID] DRM display is not up - video needs the DRM "
                        "scanout path (PISTORM_VGA_DRM=1, the default)\n");
        return -1;
    }
    g_fd     = fd;
    g_crtc_id = drmpres_crtc_id();
    g_mode_w  = drmpres_mode_w();
    g_mode_h  = drmpres_mode_h();
    {   /* the load budget below is per SECOND, so the refresh rate matters */
        drmModeCrtc *c = drmModeGetCrtc(g_fd, g_crtc_id);
        if (c) {
            if (c->mode_valid && c->mode.vrefresh)
                g_mode_hz = c->mode.vrefresh;
            drmModeFreeCrtc(c);
        }
    }
    int crtc_index      = drmpres_crtc_index();
    g_crtc_index = crtc_index;
    uint32_t guest_plane = drmpres_plane_id();

    drmModePlaneRes *pr = drmModeGetPlaneResources(g_fd);
    if (!pr) {
        fprintf(stderr, "[VID] drmModeGetPlaneResources: %s\n", strerror(errno));
        g_fd = -1;
        return -1;
    }

    /* Walk the plane list and take a free overlay that can do NV12 or YUV420 on
     * our CRTC. We print every candidate: when video does not appear, this list
     * (and the zpos numbers below) is the thing that tells you why.
     * PISTORM_VID_PLANE=<id> forces a specific plane. */
    const char *force = getenv("PISTORM_VID_PLANE");
    uint32_t want = force && *force ? (uint32_t)strtoul(force, NULL, 0) : 0;

    uint32_t chosen = 0;
    fprintf(stderr, "[VID] planes on CRTC %u (guest uses plane %u):\n",
            g_crtc_id, guest_plane);
    for (uint32_t i = 0; i < pr->count_planes; i++) {
        drmModePlane *pl = drmModeGetPlane(g_fd, pr->planes[i]);
        if (!pl)
            continue;
        int usable_crtc = (pl->possible_crtcs & (1u << crtc_index)) != 0;
        int type = plane_type_of(pl->plane_id);
        int nv12 = has_fmt(pl, DRM_FORMAT_NV12);
        int yuv  = has_fmt(pl, DRM_FORMAT_YUV420);
        fprintf(stderr, "[VID]   plane %u type=%s zpos=%ld crtc=%s NV12=%s "
                        "YUV420=%s%s\n",
                pl->plane_id,
                type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" :
                type == DRM_PLANE_TYPE_CURSOR  ? "CURSOR"  : "OVERLAY",
                plane_zpos(pl->plane_id), usable_crtc ? "yes" : "no",
                nv12 ? "yes" : "no", yuv ? "yes" : "no",
                pl->plane_id == guest_plane ? "  <- guest" : "");

        int candidate = pl->plane_id != guest_plane && usable_crtc &&
                        type == DRM_PLANE_TYPE_OVERLAY && (nv12 || yuv);
        if (candidate && (want ? pl->plane_id == want : !chosen)) {
            chosen = pl->plane_id;
            g_nformats = pl->count_formats < 64 ? pl->count_formats : 64;
            memcpy(g_formats, pl->formats, g_nformats * sizeof(uint32_t));
        } else if (candidate && !g_bd_plane) {
            /* Second usable overlay: keep it for the letterbox backdrop. Taken
             * on the way past because the plane list is only walked once. */
            g_bd_plane  = pl->plane_id;
            g_bd_fourcc = yuv ? DRM_FORMAT_YUV420 : DRM_FORMAT_NV12;
        }
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pr);
    if (want && chosen != want)
        fprintf(stderr, "[VID] PISTORM_VID_PLANE=%u is not a usable candidate\n",
                want);

    if (!chosen) {
        fprintf(stderr, "[VID] no spare YUV-capable overlay plane on CRTC %u "
                        "(guest plane %u). Run ./drmprobe to list planes.\n",
                g_crtc_id, guest_plane);
        g_fd = -1;
        return -1;
    }
    g_plane_id = chosen;

    /* Sit ABOVE the guest plane. This is the single most important thing in
     * this file: the guest plane is an opaque full-screen XRGB8888 surface, so
     * if video ends up underneath it the picture is simply invisible - the
     * classic "sound plays, screen stays black" symptom.
     *
     * zpos on vc4 is a RANGE property with real limits, so writing a big
     * constant like 127 just fails and leaves the default ordering in place.
     * Read the range, take the top of it, and if that still does not put us
     * above the guest plane, push the GUEST plane down instead (we are the DRM
     * master, both planes are ours). */
    {
        uint64_t cur = 0, lo = 0, hi = 0;
        int mut = 0;
        uint32_t zp = prop_info(g_plane_id, DRM_MODE_OBJECT_PLANE, "zpos",
                                &cur, &mut, &lo, &hi);
        if (zp && mut && hi > cur)
            drmModeObjectSetProperty(g_fd, g_plane_id, DRM_MODE_OBJECT_PLANE,
                                     zp, hi);

        long mine = plane_zpos(g_plane_id);
        long theirs = plane_zpos(guest_plane);
        if (mine >= 0 && theirs >= 0 && mine <= theirs) {
            uint64_t gcur = 0, glo = 0, ghi = 0;
            int gmut = 0;
            uint32_t gzp = prop_info(guest_plane, DRM_MODE_OBJECT_PLANE, "zpos",
                                     &gcur, &gmut, &glo, &ghi);
            if (gzp && gmut && glo < (uint64_t)mine)
                drmModeObjectSetProperty(g_fd, guest_plane,
                                         DRM_MODE_OBJECT_PLANE, gzp, glo);
            theirs = plane_zpos(guest_plane);
        }
        fprintf(stderr, "[VID] zpos: video plane %u = %ld, guest plane %u = %ld"
                        " (range %llu..%llu, %s)\n",
                g_plane_id, mine, guest_plane, theirs,
                (unsigned long long)lo, (unsigned long long)hi,
                mut ? "writable" : "read-only");
        if (mine >= 0 && theirs >= 0 && mine <= theirs)
            fprintf(stderr, "[VID] WARNING: the video plane is NOT above the "
                            "guest plane - the picture will be hidden behind "
                            "the Atari screen. Try PISTORM_VID_PLANE=<id> with "
                            "a higher-zpos plane from the list above.\n");
    }
    /* The backdrop has to sit strictly between the two: above the Atari screen
     * so it hides it, below the picture so it does not hide that. One step
     * under the video plane is the whole requirement. If there is no room -
     * the guest is already directly beneath us - there is nowhere to put it
     * and letterbox bars will keep showing the desktop; say so once. */
    if (g_bd_plane) {
        long mine = plane_zpos(g_plane_id);
        long theirs = plane_zpos(guest_plane);
        uint64_t cur = 0, lo = 0, hi = 0;
        int mut = 0;
        uint32_t zp = prop_info(g_bd_plane, DRM_MODE_OBJECT_PLANE, "zpos",
                                &cur, &mut, &lo, &hi);
        long want_z = mine - 1;
        if (zp && mut && mine > theirs + 1 &&
            want_z >= (long)lo && want_z <= (long)hi &&
            drmModeObjectSetProperty(g_fd, g_bd_plane, DRM_MODE_OBJECT_PLANE,
                                     zp, (uint64_t)want_z) == 0) {
            fprintf(stderr, "[VID] letterbox backdrop on plane %u at zpos %ld "
                            "(between guest %ld and video %ld)\n",
                    g_bd_plane, want_z, theirs, mine);
        } else {
            fprintf(stderr, "[VID] no usable zpos between guest (%ld) and video "
                            "(%ld) - letterbox bars will show the Atari "
                            "screen.\n", theirs, mine);
            g_bd_plane = 0;
        }
    } else {
        fprintf(stderr, "[VID] only one spare overlay plane - letterbox bars "
                        "will show the Atari screen.\n");
    }

    /* Bilinear (the HVS default) is what you want for video - unlike the guest
     * plane, which forces Nearest Neighbor for crisp pixel art. */
    set_enum_prop(g_plane_id, "SCALING_FILTER", "Default");
    atomic_setup();

    fprintf(stderr, "[VID] video overlay plane %u on CRTC %u, display %ux%u\n",
            g_plane_id, g_crtc_id, g_mode_w, g_mode_h);
    return 0;
}

int vidplane_supports(uint32_t fourcc)
{
    for (uint32_t i = 0; i < g_nformats; i++)
        if (g_formats[i] == fourcc)
            return 1;
    return 0;
}

void vidplane_set_colorimetry(int enc, int range)
{
    if (g_fd < 0)
        return;
    const char *e = enc >= 2 ? "ITU-R BT.2020 YCbCr"
                  : enc == 1 ? "ITU-R BT.709 YCbCr"
                             : "ITU-R BT.601 YCbCr";
    set_enum_prop(g_plane_id, "COLOR_ENCODING", e);
    set_enum_prop(g_plane_id, "COLOR_RANGE",
                  range ? "YCbCr full range" : "YCbCr limited range");
}

/* ---------------------------------------------------------------- buffers */

static void free_one(struct vpbuf *b)
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

void vidplane_free(void)
{
    if (g_fd < 0)
        return;
    vidplane_hide();
    /* The backdrop is switched off but its buffer is KEPT: this is also called
     * from vidplane_alloc() when a new film needs differently sized scanout
     * buffers, and the backdrop is display-sized, so it is still correct. It
     * is only thrown away if the mode itself changes. */
    vidplane_backdrop(0);
    vidplane_drop_imports();
    for (int i = 0; i < g_nbuf; i++)
        free_one(&g_buf[i]);
    g_nbuf = 0;
    g_w = g_h = 0;
    g_fourcc = 0;
}

/* A dumb buffer is a flat 8-bit allocation; we lay the Y and chroma planes out
 * inside it by hand and describe that layout to AddFB2 with offsets. Width is
 * rounded up to 64 so the chroma pitch stays sane and the HVS is happy; the
 * extra columns are simply not sampled (SRC_W crops them). */
int vidplane_alloc(uint32_t fourcc, uint32_t w, uint32_t h, int nbuf)
{
    if (g_fd < 0 || w == 0 || h == 0)
        return -1;
    if (nbuf < 2) nbuf = 2;
    if (nbuf > VIDPLANE_MAXBUF) nbuf = VIDPLANE_MAXBUF;

    if (g_nbuf && g_fourcc == fourcc && g_w == w && g_h == h && g_nbuf == nbuf)
        return 0;
    vidplane_free();

    if (fourcc != DRM_FORMAT_NV12 && fourcc != DRM_FORMAT_YUV420) {
        fprintf(stderr, "[VID] unsupported plane fourcc 0x%08x\n", fourcc);
        return -1;
    }

    uint32_t aw = ALIGN_UP(w, 64);
    uint32_t ah = ALIGN_UP(h, 2);
    uint32_t rows = ah + ah / 2;            /* 4:2:0, both layouts */

    for (int i = 0; i < nbuf; i++) {
        struct drm_mode_create_dumb creq;
        memset(&creq, 0, sizeof creq);
        creq.width  = aw;
        creq.height = rows;
        creq.bpp    = 8;
        if (drmIoctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
            fprintf(stderr, "[VID] CREATE_DUMB %ux%u: %s\n", aw, rows,
                    strerror(errno));
            goto fail;
        }
        struct vpbuf *b = &g_buf[i];
        b->handle = creq.handle;
        b->size   = creq.size;

        uint32_t P = creq.pitch ? creq.pitch : aw;
        uint32_t handles[4] = { creq.handle, creq.handle, creq.handle, 0 };
        uint32_t pitches[4] = { 0, 0, 0, 0 };
        uint32_t offsets[4] = { 0, 0, 0, 0 };

        if (fourcc == DRM_FORMAT_NV12) {
            b->pitch[0]  = P;         b->offset[0] = 0;
            b->pitch[1]  = P;         b->offset[1] = P * ah;
            b->pitch[2]  = 0;         b->offset[2] = 0;
            handles[2] = 0;
        } else {                                        /* YUV420 (I420) */
            b->pitch[0]  = P;         b->offset[0] = 0;
            b->pitch[1]  = P / 2;     b->offset[1] = P * ah;
            b->pitch[2]  = P / 2;     b->offset[2] = P * ah + (P / 2) * (ah / 2);
        }
        pitches[0] = b->pitch[0]; offsets[0] = b->offset[0];
        pitches[1] = b->pitch[1]; offsets[1] = b->offset[1];
        pitches[2] = b->pitch[2]; offsets[2] = b->offset[2];

        if (drmModeAddFB2(g_fd, w, h, fourcc, handles, pitches, offsets,
                          &b->fb, 0) < 0) {
            fprintf(stderr, "[VID] AddFB2 %ux%u fourcc 0x%08x: %s\n", w, h,
                    fourcc, strerror(errno));
            goto fail;
        }

        struct drm_mode_map_dumb mreq;
        memset(&mreq, 0, sizeof mreq);
        mreq.handle = creq.handle;
        if (drmIoctl(g_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
            fprintf(stderr, "[VID] MAP_DUMB: %s\n", strerror(errno));
            goto fail;
        }
        b->map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      g_fd, mreq.offset);
        if (b->map == MAP_FAILED) {
            b->map = NULL;
            fprintf(stderr, "[VID] mmap: %s\n", strerror(errno));
            goto fail;
        }
        /* Y = 16, chroma = 128 -> a clean black frame, not green snow. */
        memset(b->map, 16, P * ah);
        memset(b->map + P * ah, 128, (size_t)(creq.size - P * ah));
        g_nbuf = i + 1;
    }

    g_w = w;
    g_h = h;
    g_fourcc = fourcc;
    fprintf(stderr, "[VID] %d scanout buffers %ux%u (%s), luma pitch %u\n",
            g_nbuf, w, h,
            fourcc == DRM_FORMAT_NV12 ? "NV12" : "YUV420", g_buf[0].pitch[0]);
    return 0;

fail:
    vidplane_free();
    return -1;
}

/* One black frame the size of the display, for the backdrop plane. Same dumb
 * buffer recipe as above; it is written once and never touched again. */
static int backdrop_alloc(void)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    uint32_t aw, ah, rows, P;
    uint32_t handles[4] = { 0, 0, 0, 0 };
    uint32_t pitches[4] = { 0, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };

    if (g_bd.fb)
        return 0;
    aw   = ALIGN_UP(g_mode_w, 64);
    ah   = ALIGN_UP(g_mode_h, 2);
    rows = ah + ah / 2;

    memset(&creq, 0, sizeof creq);
    creq.width  = aw;
    creq.height = rows;
    creq.bpp    = 8;
    if (drmIoctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        fprintf(stderr, "[VID] backdrop CREATE_DUMB %ux%u: %s\n", aw, rows,
                strerror(errno));
        return -1;
    }
    g_bd.handle = creq.handle;
    g_bd.size   = creq.size;
    P = creq.pitch ? creq.pitch : aw;

    handles[0] = handles[1] = creq.handle;
    if (g_bd_fourcc == DRM_FORMAT_NV12) {
        g_bd.pitch[0] = P;     g_bd.offset[0] = 0;
        g_bd.pitch[1] = P;     g_bd.offset[1] = P * ah;
    } else {
        handles[2] = creq.handle;
        g_bd.pitch[0] = P;     g_bd.offset[0] = 0;
        g_bd.pitch[1] = P / 2; g_bd.offset[1] = P * ah;
        g_bd.pitch[2] = P / 2; g_bd.offset[2] = P * ah + (P / 2) * (ah / 2);
    }
    pitches[0] = g_bd.pitch[0]; offsets[0] = g_bd.offset[0];
    pitches[1] = g_bd.pitch[1]; offsets[1] = g_bd.offset[1];
    pitches[2] = g_bd.pitch[2]; offsets[2] = g_bd.offset[2];

    if (drmModeAddFB2(g_fd, g_mode_w, g_mode_h, g_bd_fourcc, handles, pitches,
                      offsets, &g_bd.fb, 0) < 0) {
        fprintf(stderr, "[VID] backdrop AddFB2: %s\n", strerror(errno));
        goto fail;
    }
    memset(&mreq, 0, sizeof mreq);
    mreq.handle = creq.handle;
    if (drmIoctl(g_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        fprintf(stderr, "[VID] backdrop MAP_DUMB: %s\n", strerror(errno));
        goto fail;
    }
    g_bd.map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    g_fd, mreq.offset);
    if (g_bd.map == MAP_FAILED) {
        g_bd.map = NULL;
        fprintf(stderr, "[VID] backdrop mmap: %s\n", strerror(errno));
        goto fail;
    }
    /* Y = 16, chroma = 128: black in studio range, and clamped to black in
     * full range too, so it matches the bars of the film either way. */
    memset(g_bd.map, 16, P * ah);
    memset(g_bd.map + P * ah, 128, (size_t)(creq.size - P * ah));
    return 0;

fail:
    free_one(&g_bd);
    return -1;
}

/* Show or hide the full-screen black frame underneath the picture. Committed
 * with the legacy blocking SetPlane on purpose: it changes at most twice per
 * film, so there is nothing to gain from the atomic path and one less thing to
 * interleave with the per-frame commits. */
int vidplane_backdrop(int on)
{
    if (g_fd < 0 || !g_bd_plane)
        return -1;
    if (!on) {
        if (g_bd_on) {
            drmModeSetPlane(g_fd, g_bd_plane, g_crtc_id, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0);
            g_bd_on = 0;
        }
        return 0;
    }
    if (g_bd_on)
        return 0;
    if (backdrop_alloc() != 0)
        return -1;
    if (drmModeSetPlane(g_fd, g_bd_plane, g_crtc_id, g_bd.fb, 0,
                        0, 0, g_mode_w, g_mode_h,
                        0, 0, (uint32_t)g_mode_w << 16,
                        (uint32_t)g_mode_h << 16) < 0) {
        fprintf(stderr, "[VID] backdrop SetPlane %ux%u on plane %u: %s\n",
                g_mode_w, g_mode_h, g_bd_plane, strerror(errno));
        return -1;
    }
    g_bd_on = 1;
    return 0;
}

uint8_t *vidplane_map(int idx)
{
    return (idx >= 0 && idx < g_nbuf) ? g_buf[idx].map : NULL;
}

uint32_t vidplane_pitch(int idx, int plane)
{
    if (idx < 0 || idx >= g_nbuf || plane < 0 || plane > 2)
        return 0;
    return g_buf[idx].pitch[plane];
}

uint32_t vidplane_offset(int idx, int plane)
{
    if (idx < 0 || idx >= g_nbuf || plane < 0 || plane > 2)
        return 0;
    return g_buf[idx].offset[plane];
}

/* ---------------------------------------------------- dmabuf import cache -- */
/* The decoder recycles a small pool of dmabufs, so the same file descriptors
 * come round again and again. Importing on every frame would leak GEM handles
 * and cost an ioctl pair per frame, so keep a tiny cache keyed on the fd. */

#define VP_IMPORTS 16

struct vpimport {
    int      fd;                 /* dmabuf fd as seen this session */
    uint32_t fb;
    uint32_t handles[VIDPLANE_MAX_OBJECTS];
    int      nhandles;
    int      used;
};
static struct vpimport g_imp[VP_IMPORTS];

/* ---- non-blocking presentation ------------------------------------------- *
 * drmModeSetPlane is a BLOCKING commit: it waits for the vblank before
 * returning. With the guest's presenter committing to the same CRTC, the two
 * take alternate vblanks and each waits on the other - which is what turns a
 * 16 ms guest render budget into the 33 ms overruns in the logs, and what makes
 * video presentation land on whichever vblank it can get rather than the one it
 * wanted. An atomic commit with ATOMIC_NONBLOCK and NO page-flip event returns
 * immediately and generates no event, so neither side blocks and there is
 * nothing for the two event consumers on this shared fd to steal from each
 * other. PISTORM_VID_ATOMIC=0 falls back to the blocking path. */
static int g_atomic;
static uint32_t pr_fb, pr_crtc, pr_cx, pr_cy, pr_cw, pr_chh,
                pr_sx, pr_sy, pr_sw, pr_sh;

static uint32_t plane_prop(uint32_t plane, const char *name)
{
    return prop_info(plane, DRM_MODE_OBJECT_PLANE, name, NULL, NULL, NULL, NULL);
}

static void atomic_setup(void)
{
    const char *e = getenv("PISTORM_VID_ATOMIC");
    if (e && *e == '0')
        return;
    if (drmSetClientCap(g_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
        return;
    pr_fb   = plane_prop(g_plane_id, "FB_ID");
    pr_crtc = plane_prop(g_plane_id, "CRTC_ID");
    pr_cx   = plane_prop(g_plane_id, "CRTC_X");
    pr_cy   = plane_prop(g_plane_id, "CRTC_Y");
    pr_cw   = plane_prop(g_plane_id, "CRTC_W");
    pr_chh  = plane_prop(g_plane_id, "CRTC_H");
    pr_sx   = plane_prop(g_plane_id, "SRC_X");
    pr_sy   = plane_prop(g_plane_id, "SRC_Y");
    pr_sw   = plane_prop(g_plane_id, "SRC_W");
    pr_sh   = plane_prop(g_plane_id, "SRC_H");
    g_atomic = pr_fb && pr_crtc && pr_cx && pr_cy && pr_cw && pr_chh &&
               pr_sx && pr_sy && pr_sw && pr_sh;
    fprintf(stderr, "[VID] present: %s\n", g_atomic
            ? "non-blocking atomic (no vblank wait)"
            : "blocking SetPlane (atomic properties unavailable)");
}

/* Returns 0 presented, 1 skipped (a commit is still in flight), -1 error. */
static int commit_plane(uint32_t fb, uint32_t sx, uint32_t sy,
                        uint32_t sw, uint32_t sh,
                        int dx, int dy, int dw, int dh)
{
    drmModeAtomicReq *req;
    int r;

    if (!g_atomic)
        return drmModeSetPlane(g_fd, g_plane_id, g_crtc_id, fb, 0,
                               dx, dy, (uint32_t)dw, (uint32_t)dh,
                               sx << 16, sy << 16, sw << 16, sh << 16) < 0
               ? -1 : 0;

    req = drmModeAtomicAlloc();
    if (!req)
        return -1;
    drmModeAtomicAddProperty(req, g_plane_id, pr_fb,   fb);
    drmModeAtomicAddProperty(req, g_plane_id, pr_crtc, g_crtc_id);
    drmModeAtomicAddProperty(req, g_plane_id, pr_cx,   dx);
    drmModeAtomicAddProperty(req, g_plane_id, pr_cy,   dy);
    drmModeAtomicAddProperty(req, g_plane_id, pr_cw,   dw);
    drmModeAtomicAddProperty(req, g_plane_id, pr_chh,  dh);
    drmModeAtomicAddProperty(req, g_plane_id, pr_sx,   (uint64_t)sx << 16);
    drmModeAtomicAddProperty(req, g_plane_id, pr_sy,   (uint64_t)sy << 16);
    drmModeAtomicAddProperty(req, g_plane_id, pr_sw,   (uint64_t)sw << 16);
    drmModeAtomicAddProperty(req, g_plane_id, pr_sh,   (uint64_t)sh << 16);
    r = drmModeAtomicCommit(g_fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);

    /* EBUSY means a commit is already in flight on this CRTC - almost always
     * the guest's presenter. Returning "fine" here and moving on, which is what
     * this did at first, silently DROPS the frame: the counters say presented,
     * the screen says otherwise, and it looks exactly like judder. Wait for the
     * pending commit instead by repeating the request as a blocking one. */
    if (r != 0 && errno == EBUSY) {
        g_busy_commits++;
        r = drmModeAtomicCommit(g_fd, req, 0, NULL);
    }
    drmModeAtomicFree(req);
    return r == 0 ? 0 : -1;
}

unsigned vidplane_busy_commits(void) { return g_busy_commits; }

/* Block until the next vblank on our CRTC and report when it happened.
 * Returns 0 on success. */
int vidplane_wait_vblank(double *when, double *period)
{
    drmVBlank vbl;
    static double last = 0.0;

    if (g_fd < 0 || !g_vblank_ok)
        return -1;
    memset(&vbl, 0, sizeof vbl);
    vbl.request.type = (drmVBlankSeqType)(_DRM_VBLANK_RELATIVE |
        (g_crtc_index << _DRM_VBLANK_HIGH_CRTC_SHIFT));
    vbl.request.sequence = 1;
    if (drmWaitVBlank(g_fd, &vbl) != 0) {
        g_vblank_ok = 0;
        fprintf(stderr, "[VID] vblank wait unavailable (%s) - falling back to "
                        "timed presentation\n", strerror(errno));
        return -1;
    }
    if (when) {
        double t = (double)vbl.reply.tval_sec + vbl.reply.tval_usec / 1e6;
        *when = t;
        if (period) {
            double d = t - last;
            *period = (last > 0.0 && d > 0.002 && d < 0.2) ? d
                                                           : 1.0 / (double)g_mode_hz;
        }
        last = t;
    }
    return 0;
}

static void drop_import(struct vpimport *e)
{
    if (!e->used)
        return;
    if (e->fb)
        drmModeRmFB(g_fd, e->fb);
    for (int i = 0; i < e->nhandles; i++)
        if (e->handles[i]) {
            struct drm_gem_close gc;
            memset(&gc, 0, sizeof gc);
            gc.handle = e->handles[i];
            drmIoctl(g_fd, DRM_IOCTL_GEM_CLOSE, &gc);
        }
    memset(e, 0, sizeof *e);
}

void vidplane_drop_imports(void)
{
    if (g_fd < 0)
        return;
    for (int i = 0; i < VP_IMPORTS; i++)
        drop_import(&g_imp[i]);
}

int vidplane_import(const struct vidplane_dmabuf *d, uint32_t *fb_out)
{
    uint32_t handles[4] = { 0, 0, 0, 0 };
    uint32_t pitches[4] = { 0, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    uint64_t mods[4]    = { 0, 0, 0, 0 };
    struct vpimport *slot = NULL;
    uint32_t obj_handle[VIDPLANE_MAX_OBJECTS] = { 0 };
    int i;

    if (g_fd < 0 || !d || !fb_out || d->nobjects <= 0 || d->nplanes <= 0)
        return -1;

    /* already imported? */
    for (i = 0; i < VP_IMPORTS; i++)
        if (g_imp[i].used && g_imp[i].fd == d->object_fd[0]) {
            *fb_out = g_imp[i].fb;
            return 0;
        }
    for (i = 0; i < VP_IMPORTS && !slot; i++)
        if (!g_imp[i].used)
            slot = &g_imp[i];
    if (!slot) {                       /* pool bigger than expected: recycle */
        drop_import(&g_imp[0]);
        slot = &g_imp[0];
    }

    for (i = 0; i < d->nobjects && i < VIDPLANE_MAX_OBJECTS; i++) {
        if (drmPrimeFDToHandle(g_fd, d->object_fd[i], &obj_handle[i]) != 0) {
            fprintf(stderr, "[VID] drmPrimeFDToHandle: %s\n", strerror(errno));
            goto fail;
        }
    }

    for (i = 0; i < d->nplanes && i < 4; i++) {
        int o = d->plane_object[i];
        if (o < 0 || o >= d->nobjects)
            goto fail;
        handles[i] = obj_handle[o];
        pitches[i] = d->plane_pitch[i];
        offsets[i] = d->plane_offset[i];
        mods[i]    = d->modifier[o];
    }

    if (drmModeAddFB2WithModifiers(g_fd, d->width, d->height, d->format,
                                   handles, pitches, offsets, mods,
                                   &slot->fb, DRM_MODE_FB_MODIFIERS) != 0) {
        fprintf(stderr, "[VID] AddFB2WithModifiers %ux%u fourcc 0x%08x "
                        "modifier 0x%016llx: %s\n",
                d->width, d->height, d->format,
                (unsigned long long)mods[0], strerror(errno));
        goto fail;
    }

    slot->fd = d->object_fd[0];
    slot->nhandles = d->nobjects;
    for (i = 0; i < d->nobjects && i < VIDPLANE_MAX_OBJECTS; i++)
        slot->handles[i] = obj_handle[i];
    slot->used = 1;

    {
        static int announced = 0;
        if (!announced) {
            announced = 1;
            fprintf(stderr, "[VID] zero-copy: imported a %ux%u dmabuf "
                            "(fourcc 0x%08x, modifier 0x%016llx) - no frame "
                            "copies at all from here on\n",
                    d->width, d->height, d->format,
                    (unsigned long long)mods[0]);
        }
    }
    *fb_out = slot->fb;
    return 0;

fail:
    for (i = 0; i < d->nobjects && i < VIDPLANE_MAX_OBJECTS; i++)
        if (obj_handle[i]) {
            struct drm_gem_close gc;
            memset(&gc, 0, sizeof gc);
            gc.handle = obj_handle[i];
            drmIoctl(g_fd, DRM_IOCTL_GEM_CLOSE, &gc);
        }
    return -1;
}

int vidplane_show_fb(uint32_t fb, uint32_t sx, uint32_t sy,
                     uint32_t src_w, uint32_t src_h,
                     int dx, int dy, int dw, int dh)
{
    if (g_fd < 0 || !fb || dw <= 0 || dh <= 0 || src_w == 0 || src_h == 0)
        return -1;
    if (commit_plane(fb, sx, sy, src_w, src_h, dx, dy, dw, dh) < 0) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[VID] SetPlane (dmabuf %ux%u -> %dx%d @%d,%d): "
                            "%s\n", src_w, src_h, dw, dh, dx, dy,
                    strerror(errno));
            if (src_w > 1920)
                fprintf(stderr, "[VID] the source is wider than 1920 - if this "
                                "is EINVAL the HVS cannot scale a plane this "
                                "wide and the picture cannot be shown at full "
                                "resolution.\n");
        }
        return -1;
    }
    g_visible = 1;
    return 0;
}

/* ---------------------------------------------------------------- present */

int vidplane_show(int idx, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                  int dx, int dy, int dw, int dh)
{
    if (g_fd < 0 || idx < 0 || idx >= g_nbuf || dw <= 0 || dh <= 0 ||
        sw == 0 || sh == 0)
        return -1;

    static int announced = 0;
    if (commit_plane(g_buf[idx].fb, sx, sy, sw, sh, dx, dy, dw, dh) < 0) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[VID] SetPlane (%ux%u -> %dx%d @%d,%d): %s\n",
                    g_w, g_h, dw, dh, dx, dy, strerror(errno));
        }
        return -1;
    }
    if (!announced) {
        announced = 1;
        fprintf(stderr, "[VID] first frame on plane %u: src %ux%u -> dst %dx%d "
                        "@%d,%d (SetPlane OK)\n",
                g_plane_id, g_w, g_h, dw, dh, dx, dy);
    }
    g_visible = 1;
    return 0;
}

void vidplane_hide(void)
{
    if (g_fd < 0 || !g_visible)
        return;
    drmModeSetPlane(g_fd, g_plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    g_visible = 0;
}

/* Re-read the mode after someone changes the refresh rate under us. */
void vidplane_mode_changed(void)
{
    drmModeCrtc *c;
    if (g_fd < 0)
        return;
    c = drmModeGetCrtc(g_fd, g_crtc_id);
    if (c) {
        if (c->mode_valid && c->mode.vrefresh)
            g_mode_hz = c->mode.vrefresh;
        drmModeFreeCrtc(c);
    }
    if (g_mode_w != drmpres_mode_w() || g_mode_h != drmpres_mode_h()) {
        vidplane_backdrop(0);          /* wrong size now - build a new one */
        free_one(&g_bd);
    }
    g_mode_w = drmpres_mode_w();
    g_mode_h = drmpres_mode_h();
}

uint32_t vidplane_mode_hz(void) { return g_mode_hz; }
uint32_t vidplane_mode_w(void) { return g_mode_w; }
uint32_t vidplane_mode_h(void) { return g_mode_h; }
