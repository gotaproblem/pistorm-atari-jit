/* ============================================================================
 * pcem_shim.c  —  bodies for the PCem framework stubs + render-target plumbing
 * ============================================================================ */
#include "pcem_shim.h"
#include "vid_svga.h"

/* ---- globals the engine expects ------------------------------------------ */
VIDEO_BITMAP *buffer32     = NULL;
int           changeframecount = 1;

/* Backing store for buffer32. PCem renderers write at an x-offset of +32
 * (overscan border), so each row is sized W + 64. */
static uint32_t *bmp_store = NULL;
static int       bmp_w = 0, bmp_h = 0;

/* Allocate/resize the render bitmap. Call once (or on max-mode change). */
void pcem_buffer32_alloc(int max_w, int max_h)
{
    int rw = max_w + 64;
    if (max_h > PCEM_MAX_H) max_h = PCEM_MAX_H;
    if (buffer32 && rw == bmp_w && max_h == bmp_h) return;

    free(bmp_store);
    free(buffer32);
    bmp_store = (uint32_t*)calloc((size_t)rw * max_h, sizeof(uint32_t));
    buffer32  = (VIDEO_BITMAP*)calloc(1, sizeof(VIDEO_BITMAP));
    if (!bmp_store || !buffer32) {        /* OOM: fail safe rather than write through NULL */
        fprintf(stderr, "[et4k] FATAL: buffer32 alloc failed (%dx%d)\n", rw, max_h);
        free(bmp_store); bmp_store = NULL;
        free(buffer32);  buffer32  = NULL;
        bmp_w = bmp_h = 0;
        return;
    }
    buffer32->w = rw; buffer32->h = max_h;
    for (int y = 0; y < max_h; y++)
        buffer32->line[y] = (uint8_t*)(bmp_store + (size_t)y * rw);
    /* Safety: any line[] beyond max_h (up to the array size) points at the last
     * valid row, so an out-of-range displine can never write through NULL. */
    for (int y = max_h; y < PCEM_MAX_H; y++)
        buffer32->line[y] = (uint8_t*)(bmp_store + (size_t)(max_h - 1) * rw);
    bmp_w = rw; bmp_h = max_h;
}

void pcem_buffer32_point_at(uint32_t *visible, int pitch_px, int visible_w,
                            int visible_h, int left_pad_px)
{
    if (!buffer32 || !visible || pitch_px <= 0 || visible_w <= 0 || visible_h <= 0)
        return;
    if (visible_h > PCEM_MAX_H)
        visible_h = PCEM_MAX_H;

    buffer32->w = visible_w + 64;
    buffer32->h = visible_h;
    for (int y = 0; y < visible_h; y++)
        buffer32->line[y] = (uint8_t *)(visible + (size_t)y * pitch_px - left_pad_px);
    for (int y = visible_h; y < PCEM_MAX_H; y++)
        buffer32->line[y] = buffer32->line[visible_h - 1];
}

/* ---- framework stubs: all no-ops (we drive the engine ourselves) ---------- */
void pclog(const char *fmt, ...)                                   { (void)fmt; }
void rom_init(rom_t *r, char *fn, uint32_t a, int s, int m, int o, int f)
                                                                   { (void)r;(void)fn;(void)a;(void)s;(void)m;(void)o;(void)f; }
void loadfont(char *fn, int fmt)                                   { (void)fn;(void)fmt; }
void io_sethandler(uint16_t b, int s, uint8_t(*i8)(uint16_t,void*), uint16_t(*i16)(uint16_t,void*), uint32_t(*i32)(uint16_t,void*),
                   void(*o8)(uint16_t,uint8_t,void*), void(*o16)(uint16_t,uint16_t,void*), void(*o32)(uint16_t,uint32_t,void*), void *p)
                                                                   { (void)b;(void)s;(void)i8;(void)i16;(void)i32;(void)o8;(void)o16;(void)o32;(void)p; }
void io_removehandler(uint16_t b, int s, uint8_t(*i8)(uint16_t,void*), uint16_t(*i16)(uint16_t,void*), uint32_t(*i32)(uint16_t,void*),
                   void(*o8)(uint16_t,uint8_t,void*), void(*o16)(uint16_t,uint16_t,void*), void(*o32)(uint16_t,uint32_t,void*), void *p)
                                                                   { (void)b;(void)s;(void)i8;(void)i16;(void)i32;(void)o8;(void)o16;(void)o32;(void)p; }
void mem_mapping_add(mem_mapping_t *m, uint32_t b, uint32_t s, void *r8,void *r16,void *r32,
                     void *w8,void *w16,void *w32, uint8_t *e, int f, void *p)
                                                                   { (void)m;(void)b;(void)s;(void)r8;(void)r16;(void)r32;(void)w8;(void)w16;(void)w32;(void)e;(void)f;(void)p; }
void mem_mapping_set_addr(mem_mapping_t *m, uint32_t b, uint32_t s){ (void)m;(void)b;(void)s; }
void timer_add(pc_timer_t *t, void(*cb)(void*), void *p, int st)   { (void)t;(void)cb;(void)p;(void)st; }
void timer_advance_u64(pc_timer_t *t, uint64_t d)                  { (void)t;(void)d; }
void video_blit_memtoscreen(int x,int y,int y1,int y2,int w,int h) { (void)x;(void)y;(void)y1;(void)y2;(void)w;(void)h; }
void video_wait_for_buffer(void)                                   { }
void updatewindowsize(int x, int y)                                { (void)x;(void)y; }

/* ---- counters / flags the vendored bus paths reference (inert values).
 *      Weak so they yield to any same-named symbol in your emulator core. --- */
PCEM_WEAK int cycles = 0;
PCEM_WEAK int cycles_lost = 0;
PCEM_WEAK int egareads = 0;
PCEM_WEAK int egawrites = 0;
PCEM_WEAK int video_timing_read_b  = 0;
PCEM_WEAK int video_timing_read_w  = 0;
PCEM_WEAK int video_timing_read_l  = 0;
PCEM_WEAK int video_timing_write_b = 0;
PCEM_WEAK int video_timing_write_w = 0;
PCEM_WEAK int video_timing_write_l = 0;
PCEM_WEAK int readflash = 0;
PCEM_WEAK int xsize = 640;
PCEM_WEAK int ysize = 480;
PCEM_WEAK int vid_resize = 1;

/* ---- debug viewers: defined so &viewer_* resolves; updates do nothing ----- */
viewer_t viewer_palette, viewer_palette_16, viewer_font, viewer_vram;
void viewer_add(const char *name, viewer_t *v, void *p)            { (void)name;(void)v;(void)p; }
void viewer_update(viewer_t *v, void *p)                           { (void)v;(void)p; }

/* ---- depth-conversion tables + clock ------------------------------------- */
uint32_t video_15to32[65536];
uint32_t video_16to32[65536];
uint8_t  edatlookup[4][4];
PCEM_WEAK int cpuclock = 14318180;

int rom_present(char *fn) { (void)fn; return 0; }   /* no BIOS ROMs in this build */

void pcem_shim_init_tables(void)
{
    for (int c = 0; c < 65536; c++) {
        int r5 = (c >> 10) & 0x1F, g5 = (c >> 5) & 0x1F, b5 = c & 0x1F;   /* 5-5-5 */
        video_15to32[c] = makecol32((r5<<3)|(r5>>2), (g5<<3)|(g5>>2), (b5<<3)|(b5>>2));
        int r = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b = c & 0x1F;     /* 5-6-5 */
        video_16to32[c] = makecol32((r<<3)|(r>>2),  (g6<<2)|(g6>>4),  (b<<3)|(b>>2));
    }
    for (int c = 0; c < 4; c++)
        for (int d = 0; d < 4; d++) {          /* PCem planar-expansion table */
            edatlookup[c][d] = 0;
            if (c & 1) edatlookup[c][d] |= 1;
            if (d & 1) edatlookup[c][d] |= 2;
            if (c & 2) edatlookup[c][d] |= 0x10;
            if (d & 2) edatlookup[c][d] |= 0x20;
        }
}

/* ---- KSC5601 Korean font ROM (inert; never read for Atari NOVA modes) ----- */
uint8_t fontdatksc5601[16384][72];
uint8_t fontdatksc5601_user[192][72];
