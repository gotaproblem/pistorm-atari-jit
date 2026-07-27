/*
 * dmasnd.h — STE DMA sound capture + HDMI output (pistorm-atari-jit-amiberry)
 * platforms/atari/audio/
 *
 * Both .c files are built as C. emulator.c is C++, so it includes this header
 * through the extern "C" guard below.
 */
#ifndef _DMASND_H
#define _DMASND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- output (dmasnd_hdmi.c) ---- */
int      dmasnd_init(const char *device);      /* spawns ALSA thread; NULL = default sink */
void     dmasnd_close(void);
void     dmasnd_set_mode(unsigned rate_hz, int stereo);
void     dmasnd_write_bytes(const void *src, unsigned n);
unsigned dmasnd_ring_used(void);
unsigned dmasnd_xruns(void);
void dmasnd_note_frame_len(unsigned bytes);
void dmasnd_output_reset(void);
int  dmasnd_is_repeat(void);

/* ---- host MP3 playback (dmasnd_hdmi.c, libmpg123 + SDL3 mix) ---- */
int  dmasnd_mp3_play(const char *host_path);  /* open + start decoding/mixing */
void dmasnd_mp3_stop(void);                   /* stop + free the current track */
int  dmasnd_mp3_active(void);                 /* 1 while a track is loaded/playing */
void dmasnd_mp3_pause(int on);                /* 1 = pause, 0 = resume */
int  dmasnd_mp3_is_paused(void);
long dmasnd_mp3_pos_s(void);                  /* current position, seconds (-1 n/a) */
long dmasnd_mp3_len_s(void);                  /* track length, seconds (0 unknown) */
void dmasnd_mp3_seek_rel(long delta_s);       /* seek +/- seconds from current */
const char *dmasnd_mp3_meta(int which);       /* 0=title 1=artist 2=album */

/* ---- capture (dmasnd_capture.c) ---- */
void dmasnd_snoop8 (uint32_t addr, uint8_t  val);  /* call from m68k_write_memory_8  */
void dmasnd_snoop16(uint32_t addr, uint16_t val);  /* call from m68k_write_memory_16 */
void dmasnd_snoop32(uint32_t addr, uint32_t val);  /* call from m68k_write_memory_32 */
int  dmasnd_capture_start(void);                    /* spawns the pump thread */
void dmasnd_capture_stop(void);
void dmasnd_pump(void);                             /* one pump step (pump thread only) */
void dmasnd_capture_reset(void);                    /* call on machine reset */

#ifdef __cplusplus
}
#endif

#endif /* _DMASND_H */
