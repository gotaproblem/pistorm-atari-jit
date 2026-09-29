// SPDX-License-Identifier: MIT
/*
 * Falcon DSP + sound subsystem for the PiStorm (cfg `falcon_dsp`),
 * HDMI only. See FALCON-DSP.md.
 *
 *   dsp56k.c         the DSP56001 core
 *   falcon_hw.c      host port ($FFA200), SSI, bootstrap, the switching
 *                    matrix + sound DMA ($FF8900-$FF8943), the engine
 *                    thread that clocks it all
 *   falcon_audio.c   the SDL3 stream the DAC plays into (HDMI)
 *
 * Like PSVIDEL, nothing answers the guest until PSVIDEL.PRG arms it, so
 * boot-time probes still find an ST (or STE).
 *
 * Threads: falcon_hw_* and falcon_psg_snoop run on the CPU thread; the
 * DSP and the matrix run on the engine thread (core 1); they meet in
 * lock-free host-port FIFOs and a few atomics.
 */
#ifndef FALCON_H
#define FALCON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* main thread, once, before the CPU runs. guest: natmem; guest_size: the
 * bytes of it the sound DMA may touch (ST-RAM and TT-RAM). */
int      falcon_init(uint8_t *guest, uint32_t guest_size);
int      falcon_configured(void);
void     falcon_shutdown(void);
void     falcon_reset(void);                 /* guest RESET */

/* PSVIDEL.PRG (NatFeat ENABLE) */
void     falcon_arm(void);
void     falcon_disarm(void);
int      falcon_armed(void);
int      falcon_dma_playing(void);           /* $FF8901 bit 0: play on */

/* CPU thread: $FFA200-$FFA207 and $FF8900-$FF8943 once armed */
int      falcon_hw_owns(uint32_t a);
uint32_t falcon_hw_read(uint32_t a, int size);
void     falcon_hw_write(uint32_t a, uint32_t v, int size);
/* every $FF88xx write: PSG port A bit 4 is the DSP reset line */
void     falcon_psg_snoop(uint32_t a, uint32_t v, int size);

/* ipl_task, at the real VBL: timing statistics only */
void     falcon_vbl(void);

/* test harnesses only: run the Falcon side synchronously */
void     falcon_step(unsigned frames);

/* XBIOS locks (Dsp_Lock/Dsp_Unlock, Locksnd/Unlocksnd) */
int32_t  falcon_dsp_lock(int lock);          /* 0 ok, -1 busy / not locked */
int32_t  falcon_snd_lock(int lock);          /* 1 / -129 / 0 / -128 like TOS 4 */
uint32_t falcon_info(uint32_t what);
/* XBIOS 104/105 and 128-141 with their arguments sign-extended; buffptr
 * (141) fills out4[4] for the caller to write back */
int32_t  falcon_sound_xbios(int op, const int32_t *args, uint32_t *out4);

/* falcon_audio.c (SDL3) */
int      falcon_audio_open(void);            /* needs dmasnd's SDL device */
void     falcon_audio_close(void);
void     falcon_audio_rate(unsigned hz);
unsigned falcon_audio_room(void);            /* frames that fit          */
unsigned falcon_audio_fill(void);            /* frames queued            */
void     falcon_audio_push(const int16_t *lr, unsigned frames);
void     falcon_audio_kick(void);            /* wake the engine (callback) */
void     falcon_audio_wait(unsigned us);     /* engine: sleep until kicked */
unsigned falcon_audio_underruns(void);      /* device reads that found the ring short */

#ifdef __cplusplus
}
#endif

#endif /* FALCON_H */
