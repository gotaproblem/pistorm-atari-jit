/*
 * platform_atari_fdd.h - PiStorm-Atari platform glue for FDD emulator
 *
 * Integration into pistorm-atari emulator.c / platform_atari.c:
 *
 * 1. In your read handlers:
 *      if (fdd_owns_address(addr)) return fdd_io_read(addr, size);
 *
 * 2. In your write handlers:
 *      if (fdd_owns_address(addr)) { fdd_io_write(addr, val, size); return; }
 *
 * 3. In your MFP GPIP read ($FFFA01):
 *      gpip = fdd_gpip(gpip);
 *
 * 4. In your 50Hz VBL timer callback:
 *      fdd_vbl();
 *
 * 5. At startup, after your platform is ready:
 *      platform_fdd_init();
 *
 * 6. Mount images via environment variables:
 *      PISTORM_FDD_A=/path/to/disk.st
 *      PISTORM_FDD_B=/path/to/disk2.st
 *      PISTORM_FDD_A_WP=1  (write protect)
 */

#ifndef PLATFORM_ATARI_FDD_H
#define PLATFORM_ATARI_FDD_H

#include "atari_fdd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once at platform startup - no RAM pointer needed,
 * DMA uses ps_write_16/ps_read_16 directly */
void platform_fdd_init(char*);

#ifdef __cplusplus
}
#endif
#endif /* PLATFORM_ATARI_FDD_H */
