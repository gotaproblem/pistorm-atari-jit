/*
 * et4000.h  —  Tseng Labs ET4000AX emulator for PiStorm Atari ST
 */

#pragma once

#include <stdint.h>
#include <stddef.h>


/* -----------------------------------------------------------------------
 * Register counts
 * ----------------------------------------------------------------------- */

#define NUM_CRTC_REGS    64
#define NUM_SEQ_REGS     8
#define NUM_GC_REGS      16
#define NUM_AC_REGS      32
#define NUM_DAC_ENTRIES  256

/* -----------------------------------------------------------------------
 * VRAM size
 * ----------------------------------------------------------------------- */

#define ET4000_VRAM_SIZE  (2 * 1024 * 1024)   /* 2 MB — needed for 1024x768x16bpp */

/* -----------------------------------------------------------------------
 * Video modes
 * ----------------------------------------------------------------------- */

typedef enum {
    VIDMODE_TEXT         = 0,
    VIDMODE_MONO,
    VIDMODE_PLANAR_4BPP,
    VIDMODE_PACKED_8BPP,
    VIDMODE_HICOLOR_16BPP,
    VIDMODE_16BPP,
    VIDMODE_TRUECOLOR_32BPP
} VideoMode;

/* -----------------------------------------------------------------------
 * DAC entry
 * ----------------------------------------------------------------------- */

typedef struct {
    uint8_t r, g, b;
} DACEntry;

/* -----------------------------------------------------------------------
 * ET4000 state
 * ----------------------------------------------------------------------- */

typedef struct {
    /* Register banks */
    uint8_t  crtc_index;
    uint8_t  crtc[NUM_CRTC_REGS];

    uint8_t  seq_index;
    uint8_t  seq[NUM_SEQ_REGS];

    uint8_t  gc_index;
    uint8_t  gc[NUM_GC_REGS];

    uint8_t  ac_index;
    uint8_t  ac_flip_flop;       /* 0=index, 1=data */
    uint8_t  ac[NUM_AC_REGS];

    uint8_t  misc_output;
    uint8_t  feature_ctrl;
    uint8_t  dac_mask;

    /* DAC */
    uint8_t  dac_state;          /* 0=read, 3=write */
    uint8_t  dac_read_index;
    uint8_t  dac_write_index;
    uint8_t  dac_component;      /* 0=R, 1=G, 2=B */
    DACEntry dac[NUM_DAC_ENTRIES];

    /* VGA */
    uint8_t  video_subsystem;    /* 0x3C3 select video subsystem */
    /* ET4000 extensions */
    uint8_t  segment_select;     /* 0x3CD bank select */
    uint8_t  auxiliary;          /* 0x3CB */

    /* VRAM */
    uint8_t *vram;
    size_t   vram_size;

    /* Framebuffer */
    int      fb_fd;
    uint8_t *fb_mem;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_stride;
    uint32_t fb_bpp;
    size_t   fb_size;

    /* Decoded mode */
    VideoMode current_mode;
    uint32_t  screen_width;
    uint32_t  screen_height;
    uint8_t   bits_per_pixel;
    uint32_t  bytes_per_line;
    uint32_t  start_address;

    /* Dirty tracking */
    int      dirty;
    uint64_t vram_dirty_lo;
    uint64_t vram_dirty_hi;
    int      vram_needs_clear;

    uint8_t  ramdac_state;   /* 0x3C6 consecutive-read counter (0..4) */
    uint8_t  ramdac_ctrl;    /* latched RAMDAC command / pixel-format register */
    uint8_t  vdepth;         /* decoded bits/pixel from the RAMDAC: 8/15/16/24/32 */

} ET4000State;

/* Aperture descriptor — instances live in emulator.c (et4kaddresses[]) */
typedef struct {
    uint32_t io_base;
    uint32_t io_top;
    uint32_t vram_base;
    uint32_t vram_top;
} ET4KADDRESSES_s;

typedef struct {
  uint8_t  PAL;
  uint8_t  shift_mode;
  uint8_t  res_changed;
  uint32_t vram_base;
  uint8_t  low;
  uint8_t  mid;
  uint8_t  high;
  uint8_t  *natmem;
} rtg_s;

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

extern ET4000State *g_et4000;

int  et4000_init(ET4000State *s, const char *fb_device);
void et4000_shutdown(ET4000State *s);
int  et4000_decode_mode(ET4000State *s);
void et4000_update_display(ET4000State *s);

/* VRAM */
/*
uint8_t  et4000_vram_read8  (ET4000State *s, uint32_t offset);
uint16_t et4000_vram_read16 (ET4000State *s, uint32_t offset);
uint32_t et4000_vram_read32 (ET4000State *s, uint32_t offset);
void     et4000_vram_write8 (ET4000State *s, uint32_t offset, uint8_t  val);
void     et4000_vram_write16(ET4000State *s, uint32_t offset, uint16_t val);
void     et4000_vram_write32(ET4000State *s, uint32_t offset, uint32_t val);
*/
/* I/O */
/*
uint8_t et4000_io_read8 (ET4000State *s, uint32_t port);
void    et4000_io_write8(ET4000State *s, uint32_t port, uint8_t val);
*/

/* Musashi callbacks */
unsigned int et4000_musashi_read_mem8  (unsigned int addr);
unsigned int et4000_musashi_read_mem16 (unsigned int addr);
unsigned int et4000_musashi_read_mem32 (unsigned int addr);
void         et4000_musashi_write_mem8 (unsigned int addr, unsigned int val);
void         et4000_musashi_write_mem16(unsigned int addr, unsigned int val);
void         et4000_musashi_write_mem32(unsigned int addr, unsigned int val);
