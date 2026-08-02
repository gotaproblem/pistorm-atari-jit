/* SPDX-License-Identifier: MIT
 *
 * vidplay.h - host video playback (MP4 / MKV / AVI ...) for the Atari guest.
 *
 * The MP3PLAY story, one level up: the 68k asks for a file, the Pi does all the
 * work. Demux and decode are IN-PROCESS via libavformat/libavcodec (never a
 * child process - children inherit the emulator thread's SCHED_FIFO + core
 * pinning and starve; that lesson is already written into avrecord.c), video
 * lands on its own DRM overlay plane in YUV (no CPU colour conversion, HVS
 * scaling), audio joins the existing SDL3 device next to ST/STE sound and the
 * MP3 stream so SDL mixes all of it.
 *
 * The 68k cost of playing a 1080p movie is a handful of NatFeat traps.
 */
#ifndef PISTORM_VIDPLAY_H
#define PISTORM_VIDPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* host_path is a real Pi path (the NatFeat layer maps the GEMDOS/MiNT form of
 * a HOSTFS drive to it, exactly as MP3PLAY does). 0 on success. */
int  vidplay_play(const char *host_path);
void vidplay_stop(void);
int  vidplay_active(void);      /* 1 while a file is loaded and not finished  */
void vidplay_pause(int on);
int  vidplay_is_paused(void);
long vidplay_pos_s(void);       /* current position in seconds, -1 unknown    */
long vidplay_len_s(void);       /* duration in seconds, 0 unknown             */
void vidplay_seek_rel(long delta_s);

/* 0=title 1=artist/author 2=codec summary ("H.264 1920x1080 hw + AAC 2ch") */
const char *vidplay_meta(int which);

/* 0=width 1=height 2=fps*100 3=has audio 4=hardware decode 5=volume % */
long vidplay_info(int what);

/* Destination rect in DISPLAY pixels. All zero (or w/h <= 0) restores the
 * default: aspect-correct letterbox filling the screen. */
void vidplay_set_rect(int x, int y, int w, int h);

void vidplay_set_volume(int percent);   /* 0..200, default 100 */

/* The visible part of the destination rect, in display pixels. The overlay is
 * a hardware plane above the whole Atari screen, so a front-end must tell the
 * host which parts of its window are not covered by other windows if anything
 * is to appear in front of the picture. w or h <= 0 = no clip. */
void vidplay_set_clip(int x, int y, int w, int h);

void vidplay_shutdown(void);            /* called from the emulator teardown */

#ifdef __cplusplus
}
#endif

#endif /* PISTORM_VIDPLAY_H */
