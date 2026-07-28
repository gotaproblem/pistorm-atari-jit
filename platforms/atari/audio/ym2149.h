/*
 * ym2149.h — emulated YM2149 (PSG) -> HDMI for pistorm-atari-jit-amiberry
 * platforms/atari/audio/
 *
 * The real YM2149 on the motherboard still receives every register write and
 * keeps playing through the ST's own audio path. This module SHADOWS those
 * writes into an emu2149 core and renders them on a third SDL3 audio stream
 * bound to the same device as the STE DMA sound and MP3 streams, so PSG sound
 * also comes out of HDMI.
 *
 * ym2149.c is built as C with the SDL3 flags (same rule as dmasnd_hdmi.c).
 * emulator.c / pistorm_natmem.cpp are C++, so extern "C" guard below.
 */
#ifndef _YM2149_H
#define _YM2149_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle. ym2149_init() must run AFTER dmasnd_init() has opened the SDL
 * audio device (it binds its stream to that device). Returns 0 on success,
 * 1 if disabled (PISTORM_YM=0 in the environment), -1 on error. */
int  ym2149_init(void);
void ym2149_close(void);
void ym2149_reset(void);       /* call on machine reset; no-op if not running */
int  ym2149_active(void);      /* 1 while rendering */

/* Level from the emulated LMC1992 (STE volume/mix chip), 0.0-1.0. Combined
 * with the user's PISTORM_YM_GAIN. Safe to call before ym2149_init(). */
void ym2149_set_gain(float lmc_gain);

/* Snoops — call from the $FF88xx write paths (cpu_task thread). The write
 * still goes to the real chip; these only shadow it. Cheap no-ops while
 * inactive. Decoding: even addr bit1=0 -> register select, bit1=1 -> data. */
void ym2149_snoop8 (uint32_t addr, uint8_t  val);
void ym2149_snoop16(uint32_t addr, uint16_t val);
void ym2149_snoop32(uint32_t addr, uint32_t val);

#ifdef __cplusplus
}
#endif

#endif /* _YM2149_H */
