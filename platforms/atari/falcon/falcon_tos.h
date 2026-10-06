// SPDX-License-Identifier: MIT
/*
 * Falcon-only registers a plain ST does not have, answered while Falcon
 * TOS owns the machine (emulator_falcon_tos(): TOS 4.0x on `machine
 * falcon`). Found by booting TOS 4.04 as a Falcon in Hatari with I/O
 * tracing: everything else it touches at boot is the ST's own chips,
 * PSVIDEL (Videl, $FF8006/7) or falcon_hw (DSP, sound matrix).
 *
 *   $FF8961/$FF8963  MC146818 RTC + 50 bytes NVRAM (index / data)
 *   $FF8C80-$FF8C87  Z85C30 SCC: present, idle (no serial behind it)
 *   $FF860E/F        floppy density (HD) register: absorbed
 *   $FF8604/5        while the DMA mode ($FF8606) selects the HDC
 *                    side (bit 3 set, bit 4 clear): the Falcon's NCR
 *                    5380 SCSI controller, modelled as an empty bus.
 *                    On an ST that mode reaches the ACSI port, and TOS
 *                    4.04's SCSI probe (select IDs, poll BSY for
 *                    250ms each) must not drive the ST's ACSI bus.
 *
 * CPU thread only.
 */
#ifndef FALCON_TOS_H
#define FALCON_TOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int      emulator_falcon_tos(void);         /* emulator.c                */

int      falcon_tos_owns(uint32_t a);
uint32_t falcon_tos_read(uint32_t a, int size);
void     falcon_tos_write(uint32_t a, uint32_t v, int size);
void     falcon_tos_snoop(uint32_t a, uint32_t v, int size); /* hw writes */
void     falcon_tos_reset(void);            /* RESET line: RTC int bits  */

#ifdef __cplusplus
}
#endif

#endif /* FALCON_TOS_H */
