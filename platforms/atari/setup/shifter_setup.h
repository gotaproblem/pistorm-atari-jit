/*
 * shifter_setup.h - drive the real ST shifter from the Pi with the 68k
 * halted, for the pre-boot setup page (pistorm-setup-page-design.md).
 *
 * Nothing here depends on a .cfg: only registers every ST/STE has are
 * touched, and the RAM configuration is probed from the board itself.
 *
 * Screen: 640x400 mono (1 plane) or 640x200 colour (2 planes, medium
 * resolution). Both are exactly 32000 bytes. The caller draws into
 * ss->shadow; ss_flush() writes only the bytes that differ from what is
 * already on the board.
 */
#ifndef SHIFTER_SETUP_H
#define SHIFTER_SETUP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define SS_SCREEN_BYTES  32000u
#define SS_SCREEN_BASE   0x00010000u   /* fits even a 128K bank 0 */

enum ss_mode    { SS_MODE_AUTO = 0, SS_MODE_MONO, SS_MODE_COLOUR };
enum ss_width   { SS_W8 = 1, SS_W16 = 2, SS_W32 = 4 };

struct ss_screen {
    enum ss_mode mode;          /* resolved: MONO or COLOUR               */
    int          width, height; /* 640x400 or 640x200                     */
    int          planes;        /* 1 or 2                                 */
    int          gpip7;         /* raw MFP GPIP bit 7 (0 = mono monitor)  */
    int          hz50;          /* 1 = 50 Hz, 0 = 60 Hz (colour only)     */
    uint32_t     bank0, bank1;  /* probed bytes (0 = absent)              */
    uint8_t      memcfg;        /* value written to $FF8001               */
    uint8_t      shadow[SS_SCREEN_BYTES]; /* what the page wants          */
    uint8_t      shown[SS_SCREEN_BYTES];  /* what the board holds         */
};

/* Probe RAM, program $FF8001, detect the monitor (or use `force`),
 * program base / sync / resolution / palette. Board must already be
 * reset with the protocol up. Returns 0, or -1 if bank 0 is not found. */
int  ss_bringup(struct ss_screen *ss, enum ss_mode force, int hz50);

/* 4 colours, ST 0x0RGB (3 bits a gun). Mono uses bit 0 of colour 0. */
void ss_palette(const uint16_t pal[4]);

/* Write the whole shadow with one transaction width; marks it shown. */
void ss_write_full(struct ss_screen *ss, enum ss_width w);

/* Write only changed longwords, coalesced into runs. Returns bytes sent. */
uint32_t ss_flush(struct ss_screen *ss);

/* Read the board back and count bytes that differ from `shown`. */
uint32_t ss_verify(const struct ss_screen *ss);

/* Text: both modes are an 80x25 grid of 8-pixel-wide cells (8 scanlines
 * in colour, 16 in mono - the font's rows are doubled). Colours are
 * 0..1 in mono, 0..3 in colour. */
#define SS_COLS 80
#define SS_ROWS 25

void ss_putc(struct ss_screen *ss, int col, int row, unsigned char c,
             int ink, int paper);
void ss_puts(struct ss_screen *ss, int col, int row, const char *s,
             int ink, int paper);
void ss_clear_row(struct ss_screen *ss, int row, int paper);

/* Pixel helpers on the shadow. Colour 0..1 (mono) or 0..3 (colour). */
void ss_clear(struct ss_screen *ss, int colour);
void ss_pixel(struct ss_screen *ss, int x, int y, int colour);
void ss_fill(struct ss_screen *ss, int x, int y, int w, int h, int colour);

#ifdef __cplusplus
}
#endif

#endif
