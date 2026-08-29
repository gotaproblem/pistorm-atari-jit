/*
 * acsi.h - emulated ACSI hard-disk target(s) on the Atari DMA port
 *
 * See ACSI-DESIGN.md. Serves image files as ACSI targets with the
 * Adaptec ACB-4000-bridge command flavour (the one AHDI/ICD/PP drivers
 * and Spectre expect; command semantics cross-checked against Hatari's
 * hdc.c). Plugs into atari_fdd.c's existing FDC/HDC routing on the
 * $FF8604/$FF8606 window:
 *
 *   - HDC-selected command bytes whose target ID has an image attached
 *     are handled here; all other IDs pass to the REAL bus untouched,
 *     so emulated and real ACSI devices coexist per-ID.
 *   - Sector data moves through atari_fdd.c's DMA copy helpers, so the
 *     natmem mirror and the real bus stay coherent by construction.
 *   - Command-complete / next-byte requests assert the shared GPIP5
 *     disk interrupt via fdd_gpip(), same line as the WD1772.
 *
 * Image flavours by suffix (see ACSI-DESIGN.md for the rationale):
 *   *.hfs      bare Macintosh HFS volume; sector 0 (an AHDI root sector
 *              with one MAC-type partition starting at sector 1) is
 *              SYNTHESIZED, LBA n>=1 maps to file offset (n-1)*512.
 *              The same file mounts in Basilisk II / hfsutils.
 *   anything   raw Atari ACSI disk image, sectors passed through 1:1.
 */

#ifndef ATARI_ACSI_H
#define ATARI_ACSI_H

#include <stdint.h>
#include <stdbool.h>

#define ACSI_MAX_TARGETS 8

#ifdef __cplusplus
extern "C" {
#endif

/* Attach an image. Accepts an optional "N:" prefix (N = 0..7) pinning
 * the ACSI ID - "acsi 3:disk.img" in the cfg; without it the lowest
 * free ID is used. Returns the ID, or -1 on failure. */
int  acsi_attach(const char *path);
int  acsi_attach_at(int id, const char *path);   /* id -1 = lowest free */

/* Any targets attached at all? (gates the whole subsystem) */
bool acsi_enabled(void);

/* An emulated command is in flight or has unread status: register reads
 * on the DMA port belong to us, not the real bus. */
bool acsi_owns_dma(void);

/* HDC-selected byte written to $FF8604. first_byte = A1 phase (mode
 * register A-bit clear). Returns true if the byte was consumed by an
 * emulated target; false = not ours, forward to the real bus. */
bool acsi_cmd_byte(uint8_t v, bool first_byte);

/* HDC-selected read of $FF8604 (status byte). Clears the IRQ. */
uint8_t acsi_status_read(void);

/* SCREG read while an emulated command owns the DMA: residual count. */
uint16_t acsi_residual_count(void);

/* IRQ line state (GPIP5, active low - true means "assert"). */
bool acsi_irq_active(void);

/* Drop ownership/IRQ - called when the guest addresses a non-emulated
 * target or returns to FDC-selected commands. */
void acsi_release(void);

#ifdef __cplusplus
}
#endif
#endif /* ATARI_ACSI_H */
