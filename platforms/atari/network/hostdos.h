// SPDX-License-Identifier: MIT
/*
 * HOSTDOS - HOSTFS drives for plain TOS / EmuTOS (no MiNT).
 *
 * The guest half is atari-tools/hostdos/HOSTDOS.PRG, an AUTO-folder TSR
 * that hooks trap #1. For each GEMDOS call that names a HOSTFS drive (by
 * path, by one of our file handles, or by a DTA we filled) it hands the
 * caller's argument block to the HOSTDOS NatFeat, which runs the call on
 * the Pi's directory and returns the GEMDOS result. Everything else goes
 * straight on to the ROM, untouched.
 *
 * The engine below knows nothing about the CPU core: guest memory comes
 * and goes through the hostdos_mem_t callbacks, so the same file builds
 * into the emulator (atari_natfeat.cpp) and into tools/hostdos-test.
 */
#ifndef PISTORM_HOSTDOS_H
#define PISTORM_HOSTDOS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define HOSTDOS_API_VERSION 1u

/* special DISPATCH results - never a real GEMDOS result: errors are small
 * negatives and counts / positions are below 2 GB */
#define HOSTDOS_PASS  0x80000000u  /* not ours: guest jumps to the ROM */
#define HOSTDOS_PEXEC 0x80000001u  /* Pexec 0/3 of a program on our drive */

/* NatFeat sub-ops */
enum {
  HOSTDOS_OP_VERSION  = 0,  /* -> HOSTDOS_API_VERSION */
  HOSTDOS_OP_INSTALL  = 1,  /* fresh boot: reset state -> A..Z drive bits */
  HOSTDOS_OP_DISPATCH = 2,  /* p0 args, p1 drive, p2 basepage, p3 DTA */
  HOSTDOS_OP_PTERM    = 3,  /* p0 basepage: close the handles it owns */
  HOSTDOS_OP_PRGFLAGS = 4,  /* -> prgflags of the pending Pexec */
  HOSTDOS_OP_LOAD     = 5   /* p0 basepage from Pexec 5/7: load + relocate */
};

/* first handle number we give out; TOS's own stay well below it */
#define HOSTDOS_HANDLE_BASE 64
#define HOSTDOS_MAX_FILES   32

/* magic in the first four reserved DTA bytes of a search we own */
#define HOSTDOS_DTA_MAGIC 0x48444F53u /* "HDOS" */

/* a drive as the cfg's `hostfs X path [ro]` line gave it */
#define HOSTDOS_DRIVES   26     /* A..Z: what a GEMDOS path can name */
#define HOSTDOS_PATH_MAX 256

typedef struct hostdos_drive {
  bool enabled;
  bool readonly;
  char path[HOSTDOS_PATH_MAX];
} hostdos_drive_t;

typedef struct hostdos_mem {
  void (*read)(uint32_t addr, void *dst, uint32_t n);
  void (*write)(uint32_t addr, const void *src, uint32_t n);
} hostdos_mem_t;

#ifdef __cplusplus
extern "C" {
#endif

/* drives[HOSTDOS_DRIVES], index = drive number (copied) */
void hostdos_init(const hostdos_drive_t *drives,
                  const hostdos_mem_t *mem,
                  int (*debug_enabled)(void));

/* hostfs.xfs (MiNT) has mounted a drive: from now on MiNT owns trap #1
 * and the TSR, if it is still hooked underneath, must stay out of it */
void hostdos_note_mint(void);

uint32_t hostdos_call(uint32_t subid, const uint32_t *p);

/* GEMDOS path on a HOSTDOS drive ("S:\MUSIC\LONGNA~1.MP3") -> host path,
 * through the same 8.3 mapping the guest sees. 1 = ok, 0 = not ours. */
int hostdos_gemdos_to_host(const char *gem, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif
