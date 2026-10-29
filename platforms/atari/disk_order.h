/* disk_order.h - byte order of hard-disk images (IDE / ACSI).
 *
 * Some images are stored with the bytes of every 16-bit word swapped -
 * ARAnyM writes its disks little-endian, a twisted-cable reader dumps them
 * that way. disk_order_detect() looks at sector 0 of the file and says
 * which way round it is; the IDE and ACSI drivers then present a swapped
 * file unswapped (see doc/DISK-BYTEORDER.md). */
#ifndef DISK_ORDER_H
#define DISK_ORDER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DISK_ORDER_AUTO = 0, DISK_ORDER_NATIVE = 1, DISK_ORDER_SWAPPED = 2, DISK_ORDER_BOOTSWAP = 3 };

/* 1 = the file is stored byte-swapped, 0 = native (big-endian, as an ST
 * writes it), 2 = native partition table but 68000 boot code stored
 * word-swapped (PP-prepared images: the boot driver expects the twisted-
 * cable view), -1 = cannot tell (blank / unknown sector 0). `why` gets a
 * short reason for the log. Reads with pread: the file offset is kept. */
int disk_order_detect (int fd, char *why, size_t why_len);

/* cfg overrides: `hdd_order <slot> auto|native|swapped`,
 * `acsi_order <id> auto|native|swapped` (slot / ID 0-7) */
void disk_order_set_hdd  (int slot, int mode);
void disk_order_set_acsi (int id, int mode);
int  disk_order_get_hdd  (int slot);
int  disk_order_get_acsi (int id);
int  disk_order_parse_mode (const char *s);       /* -1 if not understood */
const char *disk_order_mode_name (int mode);

/* Decide for one image: the result of disk_order_detect (auto) or the
 * override (native -> 0, swapped -> 1). Logs one line with `tag`.
 * Returns 0, 1 or 2 as disk_order_detect; unknown (-1) comes back as 0. */
int disk_order_resolve (int fd, int mode, const char *tag, const char *path);

static inline void disk_swap512 (uint8_t *b)
{
  for (int i = 0; i < 512; i += 2) {
    uint8_t t = b[i]; b[i] = b[i + 1]; b[i + 1] = t;
  }
}

#ifdef __cplusplus
}
#endif
#endif
