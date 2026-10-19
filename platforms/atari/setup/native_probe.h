/*
 * native_probe.h - the [native] build: the real machine, configured
 * automatically.
 *
 * On a psctrl.cfg with sections but no [native] block, the emulator asks
 * the board what it is - over the PiSTorm bus, with the 68k halted and
 * before any build is loaded - and writes a [native] build describing it:
 *
 *   cpu 68000
 *   machine st | ste | megast        from the chips that answer
 *   stram_size <bank0 + bank1>       from the DRAM fold probe
 *   shifter st | ste                 $FF8901 (STE DMA sound) answers
 *   blitter real | blitter disabled  $FF8A00 - the real chip, nothing emulated
 *   rom native-tos.rom               the real TOS, copied into rom_path
 *
 * The copy of the ROM is byte-exact (ps_read_16 over the whole image), so
 * the build boots the TOS the machine has, from the Pi, with no .img to
 * find. The block is written once: an existing [native] is never touched,
 * so the page's edits to it stick. Delete the block (or the key you
 * want re-probed) and it is regenerated at the next start.
 */
#ifndef NATIVE_PROBE_H
#define NATIVE_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct np_result {
    uint32_t bank0, bank1;      /* bytes; 0 = absent                      */
    int ste_shifter;            /* $FF8265 answered                        */
    int dma_sound;              /* $FF8901 answered (STE/MSTE)             */
    int blitter;                /* $FF8A00 answered                        */
    int mste_ctrl;              /* $FF8E21 answered (Mega STE)             */
    int mega_rtc;               /* $FFFC21 answered (Mega ST / Mega STE)   */
    int tt_shifter;             /* $FF8400 answered (TT)                   */
    uint32_t rom_base;          /* 0 = no TOS header found                 */
    uint32_t rom_size;          /* 192K / 256K / 512K                      */
    uint16_t tos_version;       /* header +2, e.g. 0x0104                  */
    char machine[8];            /* "st" "ste" "megast"                     */
};

/* Probe the board. Board must be reset with the protocol up and the 68k
 * halted (as it is before the setup page). Leaves $FF8001 programmed
 * for the banks found. Always fills r; returns 0, or -1 when bank 0 did
 * not answer (no board, or no ST-RAM: nothing to describe). */
int np_probe(struct np_result *r);

/* Copy the real ROM (r->rom_base, r->rom_size) to `path`. 0 or -1. */
int np_copy_rom(const struct np_result *r, const char *path);

/* The whole thing: if cfg_path has no [native] section, probe, copy the
 * ROM to <rom_path>/native-tos.rom (rom_path from [psctrl], ~ expanded
 * to the invoking user's home, relative to the emulator's directory) and
 * add the section, and set [psctrl] boot to native so it boots first.
 * Returns 1 when the file was changed, 0 when there was
 * nothing to do, -1 on failure (logged). */
int np_ensure_native_section(const char *cfg_path);

#ifdef __cplusplus
}
#endif

#endif
