/* ============================================================================
 * pcem_shim.h  —  minimal PCem framework shim for the standalone ET4000 engine
 *
 * The vendored PCem files (vid_svga.c, vid_svga_render.c, vid_et4000.c,
 * vid_unk_ramdac.c) expect a handful of framework primitives. We don't want
 * PCem's mem-mapping / I/O / timer / ROM / device subsystems, because in this
 * project the Musashi bus drives the engine directly and SDL owns the display.
 * So every one of those primitives is stubbed to a no-op here, except the
 * render target (buffer32) which the glue points at our own frame buffer.
 *
 * The tiny stub headers (ibm.h, mem.h, io.h, rom.h, device.h, video.h,
 * timer.h, viewer.h, plat.h) all just #include this file, so the vendored
 * .c compile unmodified.
 * ============================================================================ */
#ifndef PCEM_SHIM_H
#define PCEM_SHIM_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>      /* vendored svga_add_status_info() uses sprintf */

/* Generic globals below are marked weak: if your emulator core already defines
 * a symbol of the same common name (e.g. `cycles`), the linker keeps yours and
 * there is no multiple-definition error. */
#define PCEM_WEAK __attribute__((weak))

/* ---- VGA palette type + colour packing (PCem video.h) -------------------- */
typedef struct { uint8_t r, g, b; } PALETTE_ENTRY;
typedef PALETTE_ENTRY PALETTE[256];          /* type of svga_t.vgapal */
#define makecol32(r,g,b) (((uint32_t)(uint8_t)(r) << 16) | \
                          ((uint32_t)(uint8_t)(g) <<  8) | \
                          ((uint32_t)(uint8_t)(b)))

/* ---- clock / timer constants (only feed the unused poll timing) ---------- */
#define VGACONST1 1.0
#define VGACONST2 1.0
#define TIMER_USEC 1

/* ---- CPU-cycle + video-timing counters the vendored read/write paths touch.
 *      No cycle-accurate bus here, so these exist only so `cycles -=
 *      video_timing_*` compiles and does nothing meaningful. ---------------- */
extern int cycles, cycles_lost;
extern int egareads, egawrites;
extern int video_timing_read_b,  video_timing_read_w,  video_timing_read_l;
extern int video_timing_write_b, video_timing_write_w, video_timing_write_l;
extern int readflash;
extern int xsize, ysize;     /* svga_doblit blit extent (path unused) */
extern int vid_resize;       /* =1 so svga_doblit never calls updatewindowsize */

/* ---- debug "viewer" hooks: referenced in svga_init/svga_poll, all no-ops -- */
typedef struct { int unused; } viewer_t;
extern viewer_t viewer_palette, viewer_palette_16, viewer_font, viewer_vram;
void viewer_add(const char *name, viewer_t *v, void *p);
void viewer_update(viewer_t *v, void *p);

/* ---- 15/16bpp -> 32bpp lookup tables (filled by pcem_shim_init_tables) ---- */
extern uint32_t video_15to32[65536];
extern uint32_t video_16to32[65536];
extern uint8_t  edatlookup[4][4];  /* planar 4bpp -> packed expansion (filled by init) */
extern int cpuclock;             /* feeds svga->clock — unused poll timing only */
int  rom_present(char *fn);      /* only reached by unused device *_available probes */
void pcem_shim_init_tables(void); /* call once before the first render */

/* ---- KSC5601 Korean font ROM: present only so the (never-selected on the
 *      Atari NOVA) Korean text + ET4000K paths compile & link. Zero-filled,
 *      never read at runtime. -------------------------------------------- */
extern uint8_t fontdatksc5601[16384][72];
extern uint8_t fontdatksc5601_user[192][72];

/* ---- framework types embedded inside svga_t / et4000_t -------------------- */
typedef struct mem_mapping_t { int enable; uint32_t base, size; void *p; } mem_mapping_t;
typedef struct pc_timer_t    { int enabled; uint64_t next; void (*cb)(void*); void *p; } pc_timer_t;
typedef struct rom_t         { uint8_t *rom; uint32_t mask; mem_mapping_t mapping; } rom_t;

/* device_t: the et4000_*_device tables in vid_et4000.c are file-scope globals
 * that must compile but are never used here. Either #if 0 that block (see
 * INTEGRATION.md) or rely on this permissive definition. */
typedef struct device_t {
    const char *name;
    uint32_t    flags;
    void       *fn[8];     /* init/close/available/... — never called */
    void       *config;
} device_t;

/* ---- render target: PCem renders into buffer32->line[y] as uint32_t ARGB --- */
#ifndef PCEM_MAX_H
#define PCEM_MAX_H 2400
#endif
typedef struct VIDEO_BITMAP {
    int w, h;
    uint8_t *line[PCEM_MAX_H];     /* line[y] -> start of row y (uint32_t pixels) */
} VIDEO_BITMAP;

extern VIDEO_BITMAP *buffer32;     /* defined in pcem_shim.c, pointed at our buffer */
extern int changeframecount;

#define MEM_MAPPING_EXTERNAL 1     /* used by rom_init() call; value irrelevant */

/* ---- stubbed framework primitives (bodies in pcem_shim.c) ----------------- */
void  pclog(const char *fmt, ...);
void  rom_init(rom_t *rom, char *fn, uint32_t addr, int sz, int mask, int off, int flags);
void  loadfont(char *fn, int format);
void  io_sethandler(uint16_t base, int size,
                    uint8_t  (*inb)(uint16_t, void*), uint16_t (*inw)(uint16_t, void*), uint32_t (*inl)(uint16_t, void*),
                    void     (*outb)(uint16_t, uint8_t, void*), void (*outw)(uint16_t, uint16_t, void*), void (*outl)(uint16_t, uint32_t, void*),
                    void *p);
void  io_removehandler(uint16_t base, int size,
                    uint8_t  (*inb)(uint16_t, void*), uint16_t (*inw)(uint16_t, void*), uint32_t (*inl)(uint16_t, void*),
                    void     (*outb)(uint16_t, uint8_t, void*), void (*outw)(uint16_t, uint16_t, void*), void (*outl)(uint16_t, uint32_t, void*),
                    void *p);
void  mem_mapping_add(mem_mapping_t *m, uint32_t base, uint32_t size, void *r8, void *r16, void *r32,
                      void *w8, void *w16, void *w32, uint8_t *exec, int flags, void *p);
void  mem_mapping_set_addr(mem_mapping_t *m, uint32_t base, uint32_t size);
void  timer_add(pc_timer_t *t, void (*cb)(void*), void *p, int start);
void  timer_advance_u64(pc_timer_t *t, uint64_t delay);
void  video_blit_memtoscreen(int x, int y, int y1, int y2, int w, int h);
void  video_wait_for_buffer(void);
void  updatewindowsize(int x, int y);

#endif /* PCEM_SHIM_H */
