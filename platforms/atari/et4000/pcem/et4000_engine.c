/* ============================================================================
 * et4000_engine.c  —  glue between your PiStorm/Musashi/SDL front-end and the
 *                     vendored PCem ET4000AX engine.
 *
 * This REPLACES your hand-rolled register decode + blits. Your et4000.c keeps
 * only: the SDL backend (sdl_open/sdl_set_logical/sdl_present), render_frame,
 * the PNG dump, and the native-ST path. Wire the five entry points below into
 * the matching spots in your et4000.c (see INTEGRATION.md).
 *
 * Engine handle is the et4000_t* returned by et4000_init(); its first member
 * is the svga_t, so (svga_t*)dev aliases &et4000->svga.
 * ============================================================================ */
#include "pcem_shim.h"
#include "vid_svga.h"

/* from the vendored engine */
extern void   *pcem_et4000_init(void);
extern void    et4000_out(uint16_t addr, uint8_t val, void *p);
extern uint8_t et4000_in (uint16_t addr, void *p);
extern void    svga_write(uint32_t addr, uint8_t val, void *p);
extern uint8_t svga_read (uint32_t addr, void *p);
extern void    svga_write_linear(uint32_t addr, uint8_t val, void *p);
extern uint8_t svga_read_linear (uint32_t addr, void *p);
extern void    svga_recalctimings(svga_t *svga);
extern void    svga_poll(void *p);           /* PCem's per-line frame state machine */
extern void    svga_render_blank(svga_t *svga);  /* for the diagnostic line only */

/* from pcem_shim.c */
extern void pcem_buffer32_alloc(int max_w, int max_h);

static void   *g_dev  = NULL;     /* et4000_t* */
static svga_t *g_svga = NULL;

/* --------------------------------------------------------------------------
 * Init — call once (e.g. from your et4000_init(), after SDL is up).
 * et4000_init() does only rom_init (BIOS, stubbed no-op) + io_sethandler
 * (stubbed) + svga_init, so it runs as-is against the shim.
 * -------------------------------------------------------------------------- */
void et4000_engine_init(void)
{
    pcem_shim_init_tables();           /* build 15/16bpp LUTs before any render */
    g_dev  = pcem_et4000_init();
    g_svga = (svga_t *)g_dev;          /* svga is the first member of et4000_t */
    /* The bounded render loop uses displine 0..H-1 (H<=1024); 1088 rows give a
     * comfortable margin. (~6 MB — far less than the old 12 MB attempt.) */
    pcem_buffer32_alloc(1280, 1088);
}

/* --------------------------------------------------------------------------
 * Register I/O — route your Musashi callbacks straight in.
 * port is (addr & 0xFFFF) exactly as your old handlers received it.
 * -------------------------------------------------------------------------- */
int et4k_io_log = 0;          /* front-end sets this when the guest enables VGA */
static int io_log_n = 0;
int et4k_acl_log = 1;         /* log gated-out (ACL/W32/unknown) ports so we can see if the desktop uses the blitter */
static int acl_log_n = 0;
static uint8_t g_3c6_last  = 0xFF;   /* last value written to 0x3C6 (the original's dac_mask) */
static int     g_3c6_first = 1;      /* first 0x3C6 read returns the 0xE0 RAMDAC id */

/* --- TEMP diagnostics: where does the guest write, and does it read back wild
 *     pointers out of VRAM? (delete once the picture is stable) --------------- */
static unsigned long g_vw_count = 0;            /* VRAM writes since last frame  */
static uint32_t      g_vw_min   = 0xFFFFFFFFu;  /* min guest addr written        */
static uint32_t      g_vw_max   = 0;            /* max guest addr written        */
static inline void vw_track(uint32_t a) {
    g_vw_count++;
    if (a < g_vw_min) g_vw_min = a;
    if (a > g_vw_max) g_vw_max = a;
}

/* Which ports does an ET4000AX actually decode? VGA/EGA/CGA + ET4000 extensions
 * live in 0x3B0-0x3DF; 0x46E8/0x42E8/0x4AE8 are VGA/8514 subsystem-enable
 * latches; 0x92E8/0x9AE8 are the 8514/ACL status registers. Everything else the
 * NOVA driver pokes (notably the 0x21E.. W32 accelerator block) is hardware we
 * don't emulate: accept and ignore, never fault — exactly what your original's
 * default case did. */
static inline int et4k_port_known(uint16_t port) {
    if (port >= 0x3B0 && port <= 0x3DF) return 1;
    if (port == 0x46E8 || port == 0x42E8 || port == 0x4AE8) return 1;
    if (port == 0x92E8 || port == 0x9AE8) return 1;   /* ACL/GE status (stubbed idle) */
    return 0;
}

#define NOVA_IO_START (0x00D00000u)
#define NOVA_IO_END (0x00D10000u)

void    et4000_engine_io_write(uint32_t port, uint8_t val) {
    if (port < NOVA_IO_START || port > NOVA_IO_END) {
        fprintf (stderr, "et4000_engine_io_write8(): 0x%X address out of bounds\n", port);
        return;
    }

    port &= 0x0000FFFF;
    if (!et4k_port_known(port)) {                     /* ACL / unknown -> no-op, but record it */
        if (et4k_acl_log && acl_log_n < 4000) {
            fprintf(stderr, "[acl] W %04X = %02X\n", port, val); acl_log_n++;
        }
        return;
    }
/*
    if (et4k_io_log && port != 0x3DA && port != 0x3BA && io_log_n < 6000) {
        fprintf(stderr, "[io] W %04X = %02X\n", port, val); io_log_n++;
    }
*/
    if (port == 0x3C6) g_3c6_last = val;     /* track like the original's dac_mask */
    if (port == 0x92E8 || port == 0x9AE8) return;     /* ACL status is read-only to us */
    et4000_out(port, val, g_dev);            /* PCem still sets bpp via 4-read+write protocol */
}

uint8_t et4000_engine_io_read (uint32_t port) {
    if (port < NOVA_IO_START || port > NOVA_IO_END) {
        fprintf (stderr, "et4000_engine_io_read8(): 0x%X address out of bounds\n", port);
        return 0xFF;
    }

    port &= 0x0000FFFF;
    uint8_t v;
    if (port == 0x92E8 || port == 0x9AE8) {  /* accelerator: report not-busy / ready */
        v = 0x00;
    } else if (!et4k_port_known(port)) {
        if (et4k_acl_log && acl_log_n < 4000) {
            fprintf(stderr, "[acl] R %04X\n", port); acl_log_n++;
        }
        return 0xFF;                         /* unknown port -> open bus, never fault */
    } else {
        v = et4000_in(port, g_dev);          /* advance PCem's RAMDAC state machine */
        if (port == 0x3C6) {
            /* Return what the NOVA driver expects (matching your original
             * et4000_io_read8) rather than PCem's Sierra hidden-register pattern,
             * which the driver mis-identifies into a wild pointer. PCem's internal
             * state still advanced above, so command writes set bpp correctly. */
            v = g_3c6_first ? 0xE0 : g_3c6_last;
            g_3c6_first = 0;
        }
    }
/*
    if (et4k_io_log && port != 0x3DA && port != 0x3BA && io_log_n < 6000) {
        fprintf(stderr, "[io] R %04X = %02X\n", port, v); io_log_n++;
    }
*/
    return v;
}

/* Wide IO — split/assemble big-endian (68k order), every byte through the
 * tolerant path above, so any port is safe and non-VGA ports just no-op. */
void et4000_engine_io_write16(uint32_t port, uint16_t val) {
    if (port < NOVA_IO_START || port > NOVA_IO_END) {
        fprintf (stderr, "et4000_engine_io_write16(): 0x%X address out of bounds\n", port);
        return;
    }
    et4000_engine_io_write(port,     (uint8_t)(val >> 8));
    et4000_engine_io_write(port + 1, (uint8_t)(val & 0xFF));
}

void et4000_engine_io_write32(uint32_t port, uint32_t val) {
    if (port < NOVA_IO_START || port > NOVA_IO_END) {
        fprintf (stderr, "et4000_engine_io_write32(): 0x%X address out of bounds\n", port);
        return;
    }
    for (int i = 0; i < 4; i++)
        et4000_engine_io_write(port + i, (uint8_t)((val >> (24 - 8 * i)) & 0xFF));
}

uint16_t et4000_engine_io_read16(uint32_t port) {
    if (port < NOVA_IO_START || port > NOVA_IO_END)
        fprintf (stderr, "et4000_engine_io_read16(): 0x%X address out of bounds\n", port);
    return (uint16_t)((et4000_engine_io_read(port) << 8) | et4000_engine_io_read(port + 1));
}
uint32_t et4000_engine_io_read32(uint32_t port) {
    if (port < NOVA_IO_START || port > NOVA_IO_END)
        fprintf (stderr, "et4000_engine_io_read32(): 0x%X address out of bounds\n", port);
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | et4000_engine_io_read(port + i);
    return v;
}

/* --------------------------------------------------------------------------
 * VRAM access — your JIT routes the NOVA/XVDI VGA aperture here via
 * vga_bget/wget/lget/bput/wput/lput. The address `a` is the FULL guest address
 * (e.g. 0xC00000+offset for NOVA, 0xA00000+offset for XVDI); mask it down to the
 * 1 MB aperture so it lands in VRAM (PCem drops anything >= vram_max, which is
 * why unmasked writes vanished and reads came back 0xFF). Stored little-endian
 * because PCem's renderer reads VRAM little-endian.
 * -------------------------------------------------------------------------- */
#define ET4K_VRAM_MASK 0x000FFFFFu   /* 1 MB; strips the 0xC00000 / 0xA00000 base */
#define NOVA_VRAM_START (0x00C00000u)
#define NOVA_VRAM_END (0x00D00000u)

void et4000_engine_vram_write8 (uint32_t a, uint8_t  v) { 
    if (a < NOVA_VRAM_START || a > NOVA_VRAM_END)
        fprintf (stderr, "et4000_engine_vram_write8(): 0x%X address out of bounds\n", a);
    vw_track(a); svga_write_linear(a & ET4K_VRAM_MASK, v, g_dev); 
}
void et4000_engine_vram_write16(uint32_t a, uint16_t v) {
    if (a < NOVA_VRAM_START || a > NOVA_VRAM_END)
        fprintf (stderr, "et4000_engine_vram_write16(): 0x%X address out of bounds\n", a);
    vw_track(a);
    a &= ET4K_VRAM_MASK;
    svga_write_linear(a,     (v >> 8) & 0xFF, g_dev);   /* big-endian: high byte first (68k order) */
    svga_write_linear(a + 1, v & 0xFF, g_dev);
}
void et4000_engine_vram_write32(uint32_t a, uint32_t v) {
    if (a < NOVA_VRAM_START || a > NOVA_VRAM_END)
        fprintf (stderr, "et4000_engine_vram_write32(): 0x%X address out of bounds\n", a);
    vw_track(a);
    a &= ET4K_VRAM_MASK;
    for (int i = 0; i < 4; i++) svga_write_linear(a + i, (v >> (24 - 8 * i)) & 0xFF, g_dev);  /* BE */
}
uint8_t  et4000_engine_vram_read8 (uint32_t a) { 
    if (a < NOVA_VRAM_START || a > NOVA_VRAM_END)
        fprintf (stderr, "et4000_engine_vram_read8(): 0x%X address out of bounds\n", a);
    return svga_read_linear(a & ET4K_VRAM_MASK, g_dev); 
}
uint16_t et4000_engine_vram_read16(uint32_t a) {
    if (a < NOVA_VRAM_START || a > NOVA_VRAM_END)
        fprintf (stderr, "et4000_engine_vram_read16(): 0x%X address out of bounds\n", a);
    a &= ET4K_VRAM_MASK;
    return (uint16_t)((svga_read_linear(a, g_dev) << 8) | svga_read_linear(a + 1, g_dev));  /* BE */
}
uint32_t et4000_engine_vram_read32(uint32_t a) {
    if (a < NOVA_VRAM_START || a > NOVA_VRAM_END)
        fprintf (stderr, "et4000_engine_vram_read32(): 0x%X address out of bounds\n", a);
    uint32_t off = a & ET4K_VRAM_MASK;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | svga_read_linear(off + i, g_dev);   /* BE */
    return v;
}

uint8_t *et4000_engine_vram_ptr(void) { return g_svga ? g_svga->vram : NULL; }

/* (kept for compatibility; no longer called — VRAM now flows through the
 * vga_* callbacks into PCem's own svga->vram, not natmem) */
void et4000_engine_set_vram(uint8_t *base, uint32_t size)
{
    svga_t *s = g_svga;
    if (!s || !base || s->vram == base) return;
    s->vram = base;
    if (size) {
        s->vram_max          = size;
        s->vram_mask         = size - 1;
        s->vram_display_mask = size - 1;
    }
}

/* --------------------------------------------------------------------------
 * Per-frame render. Drives the PCem scanline renderers (the same ones
 * svga_poll calls) into buffer32, then copies the visible area, tightly
 * packed, into your ARGB staging buffer (pitch == width, exactly as your old
 * blits wrote it). Returns the mode's pixel size in *out_w/*out_h.
 *
 * NOTE: this replicates svga_poll's inner render loop. The two things most
 * likely to need a nudge on first run are (a) the +32 overscan x-offset and
 * (b) the per-line svga->ma stepping; both are flagged below.
 * -------------------------------------------------------------------------- */
/* Does the currently-programmed visible region have ANY non-zero pixel? Used by
 * the front-end to tell a live RTG screen (driver has drawn into the aperture)
 * from the boot/menu phase (VGA enabled but the menu is still in ST RAM, so the
 * aperture is empty). Cheap: early-outs on the first non-zero byte. */
int et4000_engine_visible_nonzero(void)
{
    svga_t *s = g_svga;
    if (!s || !s->vram) return 0;

    svga_recalctimings(s);
    int W = s->hdisp, H = s->dispend;
    if (W < 1 || H < 1) return 0;
    if (W > 1280) W = 1280;
    if (H > 1024) H = 1024;

    uint32_t mask   = s->vram_display_mask ? s->vram_display_mask
                    : (s->vram_mask ? s->vram_mask : 0xFFFFFu);
    uint32_t stride = (uint32_t)s->rowoffset << 3;
    if (stride == 0) stride = (uint32_t)W;
    uint32_t dstart = ((uint32_t)s->ma_latch << 2) & mask;
    uint32_t span   = stride * (uint32_t)H;
    if (span == 0 || span > mask + 1) span = mask + 1;

    for (uint32_t i = 0; i < span; i++)
        if (s->vram[(dstart + i) & mask]) return 1;
    return 0;
}

void et4000_engine_render(uint32_t *argb_dst, int *out_w, int *out_h)
{
    svga_t *s = g_svga;
    if (!s || !buffer32 || !s->vram) { if (out_w) *out_w = 640; if (out_h) *out_h = 480; return; }

    svga_recalctimings(s);                 /* refresh geometry + s->render from live regs */
    s->fullchange = 3;                     /* force every scanline to redraw */

    int W = s->hdisp;                      /* visible pixel width (depth already divided in) */
    int H = s->dispend;                    /* visible scanlines */
    if (W < 1) W = 640;
    if (H < 1) H = 480;
    if (W > 1280) W = 1280;
    if (H > 1024) H = 1024;

    /* Bounded manual frame walk: render exactly H scanlines, displine 0..H-1, so
     * we can never write past buffer32 (unlike svga_poll, whose free-running
     * displine reaches ~1500 and was segfaulting on NULL line pointers). Address
     * setup mirrors svga_poll: CRTC start is in dword units so the byte address
     * is ma_latch<<2; each line advances rowoffset<<3 bytes; all masked. */
    uint32_t mask   = s->vram_display_mask ? s->vram_display_mask
                    : (s->vram_mask ? s->vram_mask : 0xFFFFFu);
    uint32_t stride = (uint32_t)s->rowoffset << 3;
    if (stride == 0) stride = (uint32_t)W;          /* guard against a 0 stride */
    uint32_t ma     = ((uint32_t)s->ma_latch << 2) & mask;

    s->firstline_draw = 2000; s->lastline_draw = 0;
    for (int y = 0; y < H; y++) {
        s->displine = y;
        s->ma = ma & mask;                          /* renderer reads here, advances ma */
        if (s->render) s->render(s);                /* writes buffer32->line[y][x+32] */
        ma += stride;
    }

    for (int y = 0; y < H; y++) {
        const uint32_t *src = (const uint32_t *)buffer32->line[y] + 32;
        uint32_t       *dst = argb_dst + (size_t)y * W;
        for (int x = 0; x < W; x++)
            dst[x] = src[x] | 0xFF000000u;           /* PCem packs 0x00RRGGBB; force alpha */
    }

    *out_w = W;
    *out_h = H;
#if (0)
    /* ---- TEMPORARY DIAGNOSTIC (delete once RTG output is correct) ---- */
    {
        static unsigned dbg = 0;
        if ((dbg++ % 50) == 0) {
            uint32_t dstart = ((uint32_t)s->ma_latch << 2) & mask;
            uint32_t vh     = (H > 0) ? (uint32_t)H : 1;
            uint32_t vis    = stride * vh;
            uint32_t cap    = mask + 1;
            if (vis == 0 || vis > cap) vis = cap;
            unsigned long nz = 0;
            for (uint32_t i = 0; i < vis; i++)
                if (s->vram[(dstart + i) & mask]) nz++;
            const unsigned char *tl = s->vram + dstart;
            uint32_t coff = (dstart + (vh/2)*stride + stride/2) & mask;
            const unsigned char *ce = s->vram + coff;
            fprintf(stderr,
                "[et4k] render=%s bpp=%d %dx%d rowoff=%d malatch=%05X dstart=%06X "
                "nonzero=%lu/%u  TL=%02x%02x%02x%02x%02x%02x%02x%02x  "
                "C=%02x%02x%02x%02x%02x%02x%02x%02x  pal0=%08X pal1=%08X  "
                "vw=%lu [%06X..%06X]\n",
                (s->render == svga_render_blank) ? "BLANK" : "active",
                s->bpp, s->hdisp, s->dispend, s->rowoffset, s->ma_latch, dstart,
                nz, vis,
                tl[0],tl[1],tl[2],tl[3],tl[4],tl[5],tl[6],tl[7],
                ce[0],ce[1],ce[2],ce[3],ce[4],ce[5],ce[6],ce[7],
                s->pallook[0], s->pallook[1],
                g_vw_count,
                (g_vw_min == 0xFFFFFFFFu) ? 0 : g_vw_min, g_vw_max);
            g_vw_count = 0; g_vw_min = 0xFFFFFFFFu; g_vw_max = 0;  /* reset window */
        }
    }
#endif
}
