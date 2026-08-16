/*
 * dma_snoop.h - keep the JIT mirror coherent with REAL bus-master DMA
 *
 * Needed only when the emulator is NOT emulating the floppy (no "fdd" line
 * in the cfg), i.e. when a real Gotek/drive and/or a real ACSI HDC do their
 * own DMA into the Atari's RAM.
 *
 * The problem: under JIT, ST-RAM READS are served from the natmem mirror
 *
 *     static uae_u32 sr_bget(uaecptr a) { return natmem_offset[a]; }
 *
 * never from the bus. A real DMA controller writes the Atari's real RAM
 * directly, the mirror is never told, and the guest reads stale bytes - so
 * the transferred sectors are invisible to it no matter how well the bus
 * arbitration works. emulator.c already carries a disabled stub for exactly
 * this ("Coherent ST-RAM writer for non-CPU bus masters", inside #if (0)).
 *
 * This snoops the DMA registers on their way to the real hardware - it does
 * NOT intercept them - to learn the transfer window, then copies that window
 * from the real bus into the mirror once the transfer completes.
 *
 *   PISTORM_DMA_SNOOP=1   enable. Unset, nothing changes at all.
 */

#ifndef DMA_SNOOP_H
#define DMA_SNOOP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when snooping is enabled (PISTORM_DMA_SNOOP=1). Cheap: cached env
 * lookup. Call these ONLY from the branch where the emulator is not itself
 * handling the FDD registers - that condition belongs to the call site. */
int  dma_snoop_active(void);

/* Does this address belong to the DMA/FDC register block? */
int  dma_snoop_owns(uint32_t addr);

/* Call AFTER the value has gone to the real bus. */
void dma_snoop_write(uint32_t addr, uint32_t val, int size);

/* Call AFTER the value has been read from the real bus, with the value that
 * was read. May perform the mirror sync. */
void dma_snoop_read(uint32_t addr, uint32_t val, int size);

#ifdef __cplusplus
}
#endif

#endif /* DMA_SNOOP_H */
