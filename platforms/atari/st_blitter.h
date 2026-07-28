/*
 * st_blitter.h — emulated Atari ST BLiTTER for pistorm-atari-jit-amiberry
 * platforms/atari/
 *
 * Software emulation of the ST BLiTTER register file + blit engine, used
 * instead of the real chip: the PiStorm CPLD has no bus arbitration, so the
 * real blitter's bus bursts collide with CPU cycles (BERR storms). Register
 * writes land only in the emulated register file; the blit itself runs over
 * the natmem ST-RAM mirror with write-through to the real bus, so the
 * physical shifter displays the result and the real chip is never started.
 *
 * Blits complete synchronously at BUSY-set time: guest restart loops
 * (bset/TAS on $FF8A3C) read BUSY=0 / YCOUNT=0 and fall straight through,
 * for both HOG and shared mode. The blitter has no interrupt, so no timing
 * side-effects are lost.
 *
 * Clean-room implementation from the public Atari BLiTTER documentation
 * (register map $FF8A00-$FF8A3D, HOP/OP/endmask/skew/FXSR/NFSR/smudge
 * semantics). MIT, same as the rest of the tree.
 *
 * st_blitter.c is plain C; callers (pistorm_natmem.cpp) are C++.
 */
#ifndef _ST_BLITTER_H
#define _ST_BLITTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register access from the $FF8A00 page handlers. addr is the folded 24-bit
 * guest address; size is 1/2/4. Offsets 0x00-0x3F decode; the caller is
 * responsible for bus-erroring anything outside that window (real hardware
 * has nothing there either). A write that sets BUSY with YCOUNT>0 executes
 * the entire blit before returning. */
uint32_t st_blitter_reg_read(uint32_t addr, int size);
void     st_blitter_reg_write(uint32_t addr, uint32_t val, int size);

void st_blitter_reset(void);   /* clear register file (machine reset) */

/* Memory hooks, implemented in pistorm_natmem.cpp:
 *   read : any 24-bit address, served from the natmem mirror
 *   write: ST-RAM only; mirror + write-through to the real bus + SMC/snoop */
uint16_t pistorm_blit_read16(uint32_t addr);
void     pistorm_blit_write16(uint32_t addr, uint16_t val);

#ifdef __cplusplus
}
#endif

#endif /* _ST_BLITTER_H */
