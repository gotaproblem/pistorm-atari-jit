/* SPDX-License-Identifier: MIT
 *
 * avrecord.h - live A/V screen recording (video frames + mixed SDL3 audio)
 * encoded on the fly by ffmpeg (Pi 4 hardware H.264) into capture.mkv.
 */
#ifndef PISTORM_AVRECORD_H
#define PISTORM_AVRECORD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Arm the recorder: output directory + duration. The V4L2 hardware encoder is
 * opened lazily on the first video frame (when the frame size is known); the
 * writer thread self-stops when the duration elapses, even if the display is
 * idle and no further frames arrive. */
void avrecord_arm(const char *dir, int seconds);

/* Render thread: latest frame (ARGB8888 staging buffer, stride in bytes) plus
 * the dirty rect for this frame (pixel coords; pass 0,0,w-1,h-1 when unknown).
 * Only the rect is copied, and the call NEVER blocks: if the writer holds the
 * buffer, the rect is deferred and merged into the next frame's copy. */
void avrecord_video_frame(const void *fb, int stride_bytes, int w, int h,
                          int dx0, int dy0, int dx1, int dy1);

/* Overlay planes (a film, the ST Box) are not in fb: the display controller
 * composites them at scanout. Pass a blend callback while one is on screen
 * and the recorder calls it on its OWN copy of the frame, right after the
 * dirty-rect copy and under the same lock - no full-framebuffer copy on the
 * render thread. The callback draws into dst (ARGB8888, stride in bytes),
 * returns 1 if it drew and sets rect to the union it wrote (x0, y0, x1, y1
 * inclusive). The recorder re-copies that rect from fb on the next frame, so
 * a picture that moves, shrinks or goes leaves nothing behind. NULL when no
 * overlay is up. While one is, frames are taken no faster than the writer
 * encodes them, so the blend is not paid for frames that would be dropped. */
typedef int (*avrecord_blend_fn)(void *dst, int dst_stride, int w, int h,
                                 int rect[4]);
void avrecord_video_frame_ov(const void *fb, int stride_bytes, int w, int h,
                             int dx0, int dy0, int dx1, int dy1,
                             avrecord_blend_fn blend);

/* SDL postmix tap (audio thread): device-format float samples. */
void avrecord_audio_push_f32(const float *buf, int nsamples);

/* Finish: close pipes, let ffmpeg finalize the file, reap it. */
void avrecord_stop(void);

int avrecord_active(void);   /* armed or running */
int avrecord_ok(void);       /* 0 after an ffmpeg/write failure */

/* Frames per second the writer thread actually encodes at, or 0 before the
 * first frame has settled it. Frames are OFFERED at the RENDER rate, which can
 * be a good deal higher, and everything offered between two writer ticks is
 * simply overwritten - so expensive work done to prepare a frame is worth
 * pacing against this rather than against the render loop. */
int avrecord_fps(void);

#ifdef __cplusplus
}
#endif

#endif /* PISTORM_AVRECORD_H */
