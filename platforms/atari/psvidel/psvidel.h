// SPDX-License-Identifier: MIT
/*
 * PSVIDEL - a Falcon Videl + SuperVidel for the HDMI output only.
 *
 * The ST under the PiStorm has no Videl. This gives the guest one anyway:
 * the Falcon-only video registers, the Falcon palette, SuperVidel's
 * extended modes and video RAM, and its SuperBlitter - all emulated on
 * the host and shown on the Pi's HDMI through the same render thread and
 * staging buffer as fVDI. The ST's own monitor keeps showing what the
 * real shifter shows. See PSVIDEL.md.
 *
 * Nothing here is visible to the guest until the PSVIDEL.PRG TSR arms it
 * (NatFeat ENABLE), so boot-time hardware probes (EmuTOS looks for a
 * Videl) still find a plain ST.
 *
 * Threads: every psvidel_* call except the psvidel_frame_* pair runs on
 * the CPU thread. The render thread only reads a seqlock-published
 * snapshot, the palette and the VRAM dirty map.
 */
#ifndef PSVIDEL_H
#define PSVIDEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Guest address map (32-bit machines only - a 24-bit CPU cannot reach it) */
#define PSV_LO_BASE     0xA0000000u /* 16MB: alias of the low 16MB (ST-RAM) */
#define PSV_LO_SIZE     0x01000000u
#define PSV_VRAM_BASE   0xA1000000u /* video RAM proper, to 0xA8000000      */
#define PSV_VRAM_SIZE   0x07000000u
#define PSV_SVREG_BASE  0x80010000u /* SuperBlitter / firmware version       */
#define PSV_SVREG_SIZE  0x00010000u

#define PSV_MAX_W 1920u              /* the render staging buffer's limits   */
#define PSV_MAX_H 1200u

/* ---- life cycle (main thread, before the CPU runs) -------------------- */
/* st_ram: natmem base; st_limit: bytes of natmem the renderer may read.
 * want_vram: map the 0xA0000000 window (32-bit addressing only).
 * Returns 1 when the VRAM window exists. */
int      psvidel_init(uint8_t *st_ram, uint32_t st_limit, int want_vram);
int      psvidel_configured(void);        /* cfg `psvidel` was on         */
uint8_t *psvidel_vram(void);              /* NULL without the window      */
void     psvidel_reset(void);             /* guest RESET / hard reset     */

/* ---- CPU thread: the Videl registers ($FF82xx, $FF98xx, $FF8006) ------ */
/* a is the 24-bit folded address. Owned registers exist only while armed. */
int      psvidel_hw_owns(uint32_t a);
uint32_t psvidel_hw_read(uint32_t a, int size);
/* 1 = the write must ALSO go to the real bus (STE-shared registers on an
 * STE host); 0 = absorbed. */
int      psvidel_hw_write(uint32_t a, uint32_t v, int size);
/* every $FF82xx write that goes to the real bus (base, ST shift) */
void     psvidel_video_snoop(uint32_t a, uint32_t v, int size);

/* ---- CPU thread: VRAM and the SuperVidel register window -------------- */
void     psvidel_vram_dirty(uint32_t off, uint32_t len);
uint32_t psvidel_svreg_read(uint32_t a, int size);
void     psvidel_svreg_write(uint32_t a, uint32_t v, int size);
/* how the SuperBlitter reaches the low 16MB alias (the real ST-RAM banks) */
void     psvidel_set_mem_hooks(uint32_t (*rd8)(uint32_t),
                               void (*wr8)(uint32_t, uint32_t));

/* ---- CPU thread: the NatFeat (XBIOS side, see PSVIDEL.PRG) ------------ */
#define PSV_SM_COMPAT   (1u << 16)  /* new mode is ST compatible          */
#define PSV_SM_STREZ(r) (((r) >> 17) & 3u)
#define PSV_SM_REJECT   (1u << 20)  /* invalid mode, nothing changed      */
uint32_t psvidel_enable(uint32_t initial_mode);   /* -> 1 | VRAM ? 2 : 0 */
void     psvidel_disable(void);
uint32_t psvidel_setmode(uint32_t mode);          /* VsetMode           */
uint32_t psvidel_fixmode(uint32_t mode);          /* Vfixmode           */
uint32_t psvidel_getsize(uint32_t mode);          /* VgetSize           */
void     psvidel_set_rgb(uint32_t idx, uint32_t rgb);   /* 0x00RRGGBB   */
uint32_t psvidel_get_rgb(uint32_t idx);
int      psvidel_ste_palette_mode(void);     /* VsetRGB must also set STE */
uint16_t psvidel_rgb_to_ste(uint32_t rgb);
void     psvidel_setphys(uint32_t addr);
uint32_t psvidel_getphys(void);
int      psvidel_active(void);
uint32_t psvidel_vmalloc(uint32_t mode, uint32_t value); /* ct60_vmalloc */
uint32_t psvidel_screen_alloc(uint32_t bytes);  /* Setscreen(0,0,3,mode) */
uint32_t psvidel_info(uint32_t what);

/* ---- render thread ----------------------------------------------------- */
typedef struct {
    uint32_t w, h;          /* output size                                */
    uint32_t bpp;           /* 1 2 4 8 16 32                              */
    uint32_t fmt;           /* PSV_FMT_*                                  */
    uint32_t pitch;         /* source bytes per line                      */
    uint32_t hscroll;       /* pixels skipped at each line start          */
    uint32_t base;          /* guest address of the first line            */
    uint32_t ste_pal;       /* palette: 1 = STE registers, 0 = Falcon     */
    uint32_t gen;           /* bumps on any geometry / register change    */
} psvidel_frame_t;

#define PSV_FMT_PLANAR  0   /* Falcon interleaved bitplanes, 1..8 bpp */
#define PSV_FMT_CHUNKY8 1   /* SuperVidel 8-bit chunky                */
#define PSV_FMT_RGB565  2   /* Falcon / SV 16-bit, big endian         */
#define PSV_FMT_ARGB32  3   /* SuperVidel 32-bit, big endian A,R,G,B  */

/* 0 = PSVIDEL is not showing anything this frame */
int psvidel_frame_begin(psvidel_frame_t *f);
/* Convert into an XRGB8888 staging buffer. force_full redraws every row.
 * Returns 0 when nothing changed; else the drawn row range and whether it
 * was the whole frame. */
int psvidel_frame_draw(const psvidel_frame_t *f, uint32_t *dst,
                       uint32_t dst_pitch_px, int force_full,
                       uint32_t *y0, uint32_t *y1, int *full);

#ifdef __cplusplus
}
#endif

#endif /* PSVIDEL_H */
