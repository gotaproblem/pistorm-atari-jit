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
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
extern pthread_mutex_t et4000_engine_mutex;

static void   *g_dev  = NULL;     /* et4000_t* */
static svga_t *g_svga = NULL;

typedef struct {
    uint64_t calls;
    uint64_t total_ns;
    uint64_t max_ns;
} et4000_render_prof_counter_t;

enum {
    ET4K_RENDER_PROF_SCANLINES,
    ET4K_RENDER_PROF_COPY,
    ET4K_RENDER_PROF_COUNT
};

static et4000_render_prof_counter_t et4000_render_prof[ET4K_RENDER_PROF_COUNT];

static int et4000_render_profile_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("PISTORM_VGA_PROFILE");
        enabled = env && *env && strcmp(env, "0") != 0;
    }
    return enabled;
}

static uint64_t et4000_render_profile_now_ns(void)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void et4000_render_profile_add(unsigned idx, uint64_t ns)
{
    static uint64_t next_print_ns;
    static const char *names[ET4K_RENDER_PROF_COUNT] = {
        "scanlines", "copy"
    };
    uint64_t now;

    if (!et4000_render_profile_enabled() || idx >= ET4K_RENDER_PROF_COUNT)
        return;

    et4000_render_prof[idx].calls++;
    et4000_render_prof[idx].total_ns += ns;
    if (ns > et4000_render_prof[idx].max_ns)
        et4000_render_prof[idx].max_ns = ns;

    now = et4000_render_profile_now_ns();
    if (!next_print_ns)
        next_print_ns = now + 1000000000ULL;
    if (now < next_print_ns)
        return;

    fprintf(stderr, "[VGARENDER/s]");
    for (unsigned i = 0; i < ET4K_RENDER_PROF_COUNT; i++) {
        uint64_t calls = et4000_render_prof[i].calls;
        if (!calls)
            continue;
        fprintf(stderr, " %s n=%llu avg=%lluns max=%lluns total=%lluus",
                names[i],
                (unsigned long long)calls,
                (unsigned long long)(et4000_render_prof[i].total_ns / calls),
                (unsigned long long)et4000_render_prof[i].max_ns,
                (unsigned long long)(et4000_render_prof[i].total_ns / 1000ULL));
        memset(&et4000_render_prof[i], 0, sizeof(et4000_render_prof[i]));
    }
    fprintf(stderr, "\n");
    next_print_ns = now + 1000000000ULL;
}

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
int et4k_acl_log = 0;         /* optional ACL/W32/unknown port trace; keep off during timing tests */
static int acl_log_n = 0;
static uint8_t g_3c6_last  = 0xFF;   /* last value written to 0x3C6 (the original's dac_mask) */
static int     g_3c6_first = 1;      /* first 0x3C6 read returns the 0xE0 RAMDAC id */
static int     g_3c6_reads = 0;

static void et4k_apply_ramdac_ctrl(uint8_t val)
{
    int oldbpp;

    if (!g_svga || val == 0xFF)
        return;

    oldbpp = g_svga->bpp;
    switch ((val & 1) | ((val & 0xC0) >> 5)) {
        case 0:
            g_svga->bpp = 8;
            break;
        case 2:
        case 3:
            g_svga->bpp = (val & 0x20) ? 24 : 32;
            break;
        case 4:
        case 5:
            g_svga->bpp = 15;
            break;
        case 6:
            g_svga->bpp = 16;
            break;
        case 7:
            if (val & 4)
                g_svga->bpp = (val & 0x20) ? 24 : 32;
            else
                g_svga->bpp = 16;
            break;
        default:
            break;
    }

    if (oldbpp != g_svga->bpp)
        svga_recalctimings(g_svga);
}

static int et4k_synth_status_enabled(void)
{
    const char *env = getenv("PISTORM_VGA_SYNTH_STATUS");
    return env && *env && strcmp(env, "0") != 0;
}

static uint8_t et4k_synth_input_status(uint8_t value)
{
    struct timespec ts;
    uint64_t ns;
    uint64_t phase;

#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    phase = ns % 16666667ULL; /* ~60Hz VGA status pulse */

    if (phase >= 15500000ULL)
        value |= 0x08;        /* vertical retrace */
    else
        value &= (uint8_t)~0x08;

    return value;
}

/* --- TEMP diagnostics: where does the guest write, and does it read back wild
 *     pointers out of VRAM? (delete once the picture is stable) --------------- */
static volatile unsigned long g_vw_count = 0;   /* VRAM writes since last frame  */
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

#define ET4K_IO_NOVA_START (0x00D00000u)
#define ET4K_IO_NOVA_END   (0x00E00000u)
#define ET4K_IO_XVDI_START (0x00B00000u)
#define ET4K_IO_XVDI_END   (0x00C00000u)

static inline int et4k_io_addr_in_range(uint32_t addr)
{
    return (addr >= ET4K_IO_NOVA_START && addr < ET4K_IO_NOVA_END) ||
           (addr >= ET4K_IO_XVDI_START && addr < ET4K_IO_XVDI_END);
}

void    et4000_engine_io_write(uint32_t port, uint8_t val) {
    if (!et4k_io_addr_in_range(port)) {
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
    if (port == 0x3C6) {
        g_3c6_last = val;     /* track like the original's dac_mask */
        if (g_3c6_reads >= 4)
            et4k_apply_ramdac_ctrl(val);
        g_3c6_reads = 0;
    }
    if (port == 0x92E8 || port == 0x9AE8) return;     /* ACL status is read-only to us */
    et4000_out(port, val, g_dev);            /* PCem still sets bpp via 4-read+write protocol */
}

uint8_t et4000_engine_io_read (uint32_t port) {
    if (!et4k_io_addr_in_range(port)) {
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
        if (port == 0x3C6) {
            g_3c6_reads++;
            v = g_3c6_first ? 0xE0 : g_3c6_last;
            g_3c6_first = 0;
            return v;
        }

        v = et4000_in(port, g_dev);          /* advance PCem's register state machine */
        if ((port == 0x3BA || port == 0x3DA) && et4k_synth_status_enabled())
            v = et4k_synth_input_status(v);
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
    if (!et4k_io_addr_in_range(port)) {
        fprintf (stderr, "et4000_engine_io_write16(): 0x%X address out of bounds\n", port);
        return;
    }
    et4000_engine_io_write(port,     (uint8_t)(val >> 8));
    et4000_engine_io_write(port + 1, (uint8_t)(val & 0xFF));
}

void et4000_engine_io_write32(uint32_t port, uint32_t val) {
    if (!et4k_io_addr_in_range(port)) {
        fprintf (stderr, "et4000_engine_io_write32(): 0x%X address out of bounds\n", port);
        return;
    }
    for (int i = 0; i < 4; i++)
        et4000_engine_io_write(port + i, (uint8_t)((val >> (24 - 8 * i)) & 0xFF));
}

uint16_t et4000_engine_io_read16(uint32_t port) {
    if (!et4k_io_addr_in_range(port))
        fprintf (stderr, "et4000_engine_io_read16(): 0x%X address out of bounds\n", port);
    return (uint16_t)((et4000_engine_io_read(port) << 8) | et4000_engine_io_read(port + 1));
}
uint32_t et4000_engine_io_read32(uint32_t port) {
    if (!et4k_io_addr_in_range(port))
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
#define ET4K_VRAM_NOVA_START (0x00C00000u)
#define ET4K_VRAM_NOVA_END   (0x00D00000u)
#define ET4K_VRAM_XVDI_START (0x00A00000u)
#define ET4K_VRAM_XVDI_END   (0x00B00000u)

static inline int et4k_vram_addr_in_range(uint32_t addr)
{
    return (addr >= ET4K_VRAM_NOVA_START && addr < ET4K_VRAM_NOVA_END) ||
           (addr >= ET4K_VRAM_XVDI_START && addr < ET4K_VRAM_XVDI_END);
}

void et4000_engine_vram_write8 (uint32_t a, uint8_t  v) { 
    if (!et4k_vram_addr_in_range(a))
        fprintf (stderr, "et4000_engine_vram_write8(): 0x%X address out of bounds\n", a);
    vw_track(a); svga_write_linear(a & ET4K_VRAM_MASK, v, g_dev); 
}
void et4000_engine_vram_write16(uint32_t a, uint16_t v) {
    if (!et4k_vram_addr_in_range(a))
        fprintf (stderr, "et4000_engine_vram_write16(): 0x%X address out of bounds\n", a);
    vw_track(a);
    a &= ET4K_VRAM_MASK;
    svga_write_linear(a,     (v >> 8) & 0xFF, g_dev);   /* big-endian: high byte first (68k order) */
    svga_write_linear(a + 1, v & 0xFF, g_dev);
}
void et4000_engine_vram_write32(uint32_t a, uint32_t v) {
    if (!et4k_vram_addr_in_range(a))
        fprintf (stderr, "et4000_engine_vram_write32(): 0x%X address out of bounds\n", a);
    vw_track(a);
    a &= ET4K_VRAM_MASK;
    for (int i = 0; i < 4; i++) svga_write_linear(a + i, (v >> (24 - 8 * i)) & 0xFF, g_dev);  /* BE */
}
uint8_t  et4000_engine_vram_read8 (uint32_t a) { 
    if (!et4k_vram_addr_in_range(a))
        fprintf (stderr, "et4000_engine_vram_read8(): 0x%X address out of bounds\n", a);
    return svga_read_linear(a & ET4K_VRAM_MASK, g_dev); 
}
uint16_t et4000_engine_vram_read16(uint32_t a) {
    if (!et4k_vram_addr_in_range(a))
        fprintf (stderr, "et4000_engine_vram_read16(): 0x%X address out of bounds\n", a);
    a &= ET4K_VRAM_MASK;
    return (uint16_t)((svga_read_linear(a, g_dev) << 8) | svga_read_linear(a + 1, g_dev));  /* BE */
}
uint32_t et4000_engine_vram_read32(uint32_t a) {
    if (!et4k_vram_addr_in_range(a))
        fprintf (stderr, "et4000_engine_vram_read32(): 0x%X address out of bounds\n", a);
    uint32_t off = a & ET4K_VRAM_MASK;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | svga_read_linear(off + i, g_dev);   /* BE */
    return v;
}

uint8_t *et4000_engine_vram_ptr(void) { return g_svga ? g_svga->vram : NULL; }
unsigned long et4000_engine_vram_generation(void) { return g_vw_count; }

int et4000_engine_direct_vram_ok(void)
{
    if (!g_svga)
        return 0;
    return g_svga->fb_only || (g_svga->chain4 && g_svga->packed_chain4);
}

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
 * blits wrote it). Returns the mode's pixel size in out_w/out_h.
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
    svga_t *live = g_svga;
    if (!live || !live->vram) return 0;

    svga_t snap;
    pthread_mutex_lock(&et4000_engine_mutex);
    snap = *live;
    svga_recalctimings(&snap);
    pthread_mutex_unlock(&et4000_engine_mutex);
    svga_t *s = &snap;
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

static int et4000_engine_render_offset(const svga_t *s)
{
    if (!s)
        return 32;

    switch (s->bpp) {
    case 0:
    case 1:
    case 2:
    case 3:
        return ((8 - s->scrollcache) << 1) + 16;
    case 8:
    case 15:
    case 16:
    case 24:
        return (8 - (s->scrollcache & 6)) + 24;
    case 32:
        return (8 - ((s->scrollcache & 6) >> 1)) + 24;
    default:
        return 32;
    }
}

int et4000_engine_current_size(int *out_w, int *out_h)
{
    svga_t *live = g_svga;
    if (!live || !live->vram)
        return 0;

    svga_t snap;
    pthread_mutex_lock(&et4000_engine_mutex);
    snap = *live;
    svga_recalctimings(&snap);
    pthread_mutex_unlock(&et4000_engine_mutex);

    int W = snap.hdisp;
    int H = snap.dispend;
    if (W < 1)
        W = 640;
    if (H < 1)
        H = 480;
    if (W > 1280)
        W = 1280;
    if (H > 1024)
        H = 1024;

    if (out_w)
        *out_w = W;
    if (out_h)
        *out_h = H;
    return 1;
}

static void et4000_engine_advance_scanline(svga_t *s, uint32_t mask)
{
    if (s->linedbl && !s->linecountff) {
        s->linecountff = 1;
        s->ma = s->maback;
    } else if (s->sc == s->rowcount) {
        s->linecountff = 0;
        s->sc = 0;
        s->maback += (uint32_t)s->rowoffset << 3;
        if (s->interlace)
            s->maback += (uint32_t)s->rowoffset << 3;
        s->maback &= mask;
        s->ma = s->maback;
    } else {
        s->linecountff = 0;
        s->sc++;
        s->sc &= 31;
        s->ma = s->maback;
    }
}

static void et4000_engine_advance_vc(svga_t *s)
{
    s->vc++;
    s->vc &= 2047;

    if (s->vc == s->split) {
        int reset = 1;
        if (s->line_compare)
            reset = s->line_compare(s);

        if (reset) {
            s->ma = s->maback = 0;
            s->sc = 0;
            if (s->attrregs[0x10] & 0x20)
                s->scrollcache = 0;
        }
    }
}

void et4000_engine_render(uint32_t *argb_dst, int *out_w, int *out_h)
{
    svga_t *live = g_svga;
    if (!live || !buffer32 || !live->vram) { if (out_w) *out_w = 640; if (out_h) *out_h = 480; return; }

    svga_t snap;
    pthread_mutex_lock(&et4000_engine_mutex);
    snap = *live;
    svga_recalctimings(&snap);             /* refresh geometry + s->render from live regs */
    pthread_mutex_unlock(&et4000_engine_mutex);
    svga_t *s = &snap;
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
    int copy_offset = et4000_engine_render_offset(s);

    s->sc = s->crtc[8] & 0x1f;
    s->scrollcache = s->attrregs[0x13] & 7;
    s->linecountff = 0;
    if (s->interlace && s->oddeven)
        s->ma = s->maback = s->ma_latch + (s->rowoffset << 1);
    else
        s->ma = s->maback = s->ma_latch;
    s->ca = ((s->crtc[0xe] << 8) | s->crtc[0xf]) + s->ca_adj;
    s->ma = (s->ma << 2) & mask;
    s->maback = (s->maback << 2) & mask;
    s->ca = (s->ca << 2) & mask;
    s->vc = 0;

    s->firstline_draw = 2000; s->lastline_draw = 0;
    uint64_t t_scan = et4000_render_profile_enabled() ? et4000_render_profile_now_ns() : 0;
    for (int y = 0; y < H; y++) {
        memset(buffer32->line[y], 0, (size_t)buffer32->w * sizeof(uint32_t));
        s->displine = y;
        s->ma &= mask;                               /* renderer reads here, may advance ma */
        if (s->render == svga_render_blank) {
            uint32_t *dst = (uint32_t *)buffer32->line[y] + copy_offset;
            for (int x = 0; x < W; x++)
                dst[x] = 0;
        } else if (s->render) {
            s->render(s);                            /* writes at a renderer-specific x margin */
        }
        et4000_engine_advance_scanline(s, mask);
        et4000_engine_advance_vc(s);
    }
    if (t_scan)
        et4000_render_profile_add(ET4K_RENDER_PROF_SCANLINES, et4000_render_profile_now_ns() - t_scan);

    uint64_t t_copy = et4000_render_profile_enabled() ? et4000_render_profile_now_ns() : 0;
    for (int y = 0; y < H; y++) {
        const uint32_t *src = (const uint32_t *)buffer32->line[y] + copy_offset;
        uint32_t       *dst = argb_dst + (size_t)y * W;
        memcpy(dst, src, (size_t)W * sizeof(uint32_t)); /* XRGB texture ignores top byte */
    }
    if (t_copy)
        et4000_render_profile_add(ET4K_RENDER_PROF_COPY, et4000_render_profile_now_ns() - t_copy);

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

void et4000_engine_render_direct(uint32_t *visible_dst, int pitch_px, int left_pad_px,
                                 int *out_w, int *out_h)
{
    svga_t *live = g_svga;
    if (!live || !buffer32 || !live->vram || !visible_dst) {
        if (out_w) *out_w = 640;
        if (out_h) *out_h = 480;
        return;
    }

    svga_t snap;
    pthread_mutex_lock(&et4000_engine_mutex);
    snap = *live;
    svga_recalctimings(&snap);
    pthread_mutex_unlock(&et4000_engine_mutex);
    svga_t *s = &snap;
    s->fullchange = 3;

    int W = s->hdisp;
    int H = s->dispend;
    if (W < 1) W = 640;
    if (H < 1) H = 480;
    if (W > 1280) W = 1280;
    if (H > 1024) H = 1024;
    if (pitch_px < W || left_pad_px < 32) {
        et4000_engine_render(visible_dst, out_w, out_h);
        return;
    }

    uint32_t mask   = s->vram_display_mask ? s->vram_display_mask
                    : (s->vram_mask ? s->vram_mask : 0xFFFFFu);
    int copy_offset = et4000_engine_render_offset(s);
    if (copy_offset > left_pad_px) {
        et4000_engine_render(visible_dst, out_w, out_h);
        return;
    }

    pcem_buffer32_point_at(visible_dst, pitch_px, W, H, copy_offset);

    s->sc = s->crtc[8] & 0x1f;
    s->scrollcache = s->attrregs[0x13] & 7;
    s->linecountff = 0;
    if (s->interlace && s->oddeven)
        s->ma = s->maback = s->ma_latch + (s->rowoffset << 1);
    else
        s->ma = s->maback = s->ma_latch;
    s->ca = ((s->crtc[0xe] << 8) | s->crtc[0xf]) + s->ca_adj;
    s->ma = (s->ma << 2) & mask;
    s->maback = (s->maback << 2) & mask;
    s->ca = (s->ca << 2) & mask;
    s->vc = 0;

    s->firstline_draw = 2000; s->lastline_draw = 0;
    uint64_t t_scan = et4000_render_profile_enabled() ? et4000_render_profile_now_ns() : 0;
    for (int y = 0; y < H; y++) {
        memset(buffer32->line[y], 0, (size_t)buffer32->w * sizeof(uint32_t));
        s->displine = y;
        s->ma &= mask;
        if (s->render == svga_render_blank) {
            uint32_t *dst = (uint32_t *)buffer32->line[y] + copy_offset;
            for (int x = 0; x < W; x++)
                dst[x] = 0;
        } else if (s->render) {
            s->render(s);
        }
        et4000_engine_advance_scanline(s, mask);
        et4000_engine_advance_vc(s);
    }
    if (t_scan)
        et4000_render_profile_add(ET4K_RENDER_PROF_SCANLINES, et4000_render_profile_now_ns() - t_scan);

    if (out_w) *out_w = W;
    if (out_h) *out_h = H;
}
