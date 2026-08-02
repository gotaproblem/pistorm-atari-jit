/* SPDX-License-Identifier: MIT
 *
 * vidplane.h - a SECOND DRM/KMS plane, YUV, for host video playback.
 *
 * The guest display already owns one scaling plane (et4000_drm.c). Video does
 * NOT go through it: it gets its own overlay plane on the same CRTC, in a YUV
 * format the vc4 HVS understands natively (NV12 or YUV420/I420). That means:
 *
 *   - zero CPU colour conversion. The decoder hands us YUV420P or NV12 and we
 *     memcpy it row-wise straight into a scanout buffer; the HVS does YUV->RGB
 *     during scanout, for free.
 *   - zero CPU scaling. The plane's destination rect is the display (or a
 *     letterboxed sub-rect) and the HVS scales, for free.
 *   - the guest screen is untouched. Video sits ON TOP of the Atari display and
 *     vanishes again on STOP - no framebuffer takeover, no mode change.
 *
 * We deliberately use LEGACY drmModeSetPlane rather than atomic commits: the
 * guest presenter may be running the atomic async path on the same fd, and two
 * independent page-flip-event consumers on one fd steal each other's events.
 * SetPlane generates no event and latches at vblank, so the two paths coexist.
 *
 * All of this shares et4000_drm.c's fd - only the DRM master may touch planes,
 * and the emulator is the master.
 */
#ifndef PISTORM_VIDPLANE_H
#define PISTORM_VIDPLANE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIDPLANE_MAXBUF 6

/* Locate a usable overlay plane on the guest's CRTC (one that is NOT the plane
 * the guest display is using). Returns 0 on success, -1 if the display is not
 * up or no spare YUV-capable overlay exists. Cheap no-op once open. */
int vidplane_open(void);

/* 1 if the plane can scan out this DRM_FORMAT_* fourcc. */
int vidplane_supports(uint32_t fourcc);

/* Allocate nbuf scanout buffers of w x h in the given fourcc (NV12 or YUV420).
 * Frees any previous set. Returns 0 on success. */
int vidplane_alloc(uint32_t fourcc, uint32_t w, uint32_t h, int nbuf);

/* Per-buffer geometry for the decoder to write into. plane = 0..2. */
uint8_t *vidplane_map(int idx);
uint32_t vidplane_pitch(int idx, int plane);
uint32_t vidplane_offset(int idx, int plane);

/* Tell the HVS about the source colour encoding/range of the coming frames
 * (best effort - ignored if the kernel plane has no such properties).
 * enc: 0 = BT.601, 1 = BT.709, 2 = BT.2020.  range: 0 = limited, 1 = full. */
void vidplane_set_colorimetry(int enc, int range);

/* ---- zero-copy path: frames that already live in DMA-BUF memory -----------
 *
 * The V4L2 Request hwaccel (Pi 4 stateless HEVC, /dev/video19) hands back
 * dmabuf handles instead of pixels. Those frames are Broadcom SAND-tiled and
 * CANNOT be copied into a linear buffer - libavutil refuses to download them
 * for exactly that reason - so the only way to display them is to import the
 * dmabuf as a DRM framebuffer, tiling modifier and all, and scan it out. Which
 * is also the fastest possible path: no decode copy, no colour conversion, no
 * scaling, nothing touching the CPU between the decoder and the display.
 *
 * This mirrors AVDRMFrameDescriptor without dragging libav headers in here. */
#define VIDPLANE_MAX_OBJECTS 4
#define VIDPLANE_MAX_PLANES  4

struct vidplane_dmabuf {
    uint32_t width, height;
    uint32_t format;                                  /* DRM fourcc          */
    int      nobjects;
    int      object_fd[VIDPLANE_MAX_OBJECTS];
    uint64_t modifier[VIDPLANE_MAX_OBJECTS];          /* e.g. SAND128        */
    int      nplanes;
    int      plane_object[VIDPLANE_MAX_PLANES];       /* index into objects  */
    uint32_t plane_offset[VIDPLANE_MAX_PLANES];
    uint32_t plane_pitch[VIDPLANE_MAX_PLANES];
};

/* Import (or return a cached) framebuffer id for a dmabuf frame. The caller
 * must keep the frame referenced for as long as the id is in use. 0 on
 * success. */
int vidplane_import(const struct vidplane_dmabuf *d, uint32_t *fb_out);

/* Scan out an imported framebuffer. src_w/src_h are the visible size. */
int vidplane_show_fb(uint32_t fb, uint32_t sx, uint32_t sy,
                     uint32_t src_w, uint32_t src_h,
                     int dx, int dy, int dw, int dh);

/* Drop every cached import (called when playback stops). */
void vidplane_drop_imports(void);

/* Scan out buffer idx into the destination rect (display coordinates).
 * Latched at the next vblank. Returns 0 on success. */
int vidplane_show(int idx, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                  int dx, int dy, int dw, int dh);

/* Turn the plane off - the guest display becomes fully visible again. */
void vidplane_hide(void);

/* Full-screen black on a second overlay plane, underneath the picture and
 * above the Atari screen, so that letterbox bars are black rather than a view
 * of the desktop. Idempotent. Returns -1 if there is no plane for it. */
int vidplane_backdrop(int on);

/* Free the buffers (implies hide). The plane selection is kept. */
void vidplane_free(void);

/* Display geometry, for letterbox maths. */
uint32_t vidplane_mode_w(void);
uint32_t vidplane_mode_hz(void);   /* display refresh, for the load budget */
void vidplane_mode_changed(void);  /* re-read after a refresh-rate switch   */

/* Block until the next vblank on our CRTC; reports its time and the measured
 * period, both in seconds. -1 if the driver will not do it. */
int vidplane_wait_vblank(double *when, double *period);
unsigned vidplane_busy_commits(void);
uint32_t vidplane_mode_h(void);

#ifdef __cplusplus
}
#endif

#endif /* PISTORM_VIDPLANE_H */
