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

/* SDL postmix tap (audio thread): device-format float samples. */
void avrecord_audio_push_f32(const float *buf, int nsamples);

/* Finish: close pipes, let ffmpeg finalize the file, reap it. */
void avrecord_stop(void);

int avrecord_active(void);   /* armed or running */
int avrecord_ok(void);       /* 0 after an ffmpeg/write failure */

#ifdef __cplusplus
}
#endif

#endif /* PISTORM_AVRECORD_H */
