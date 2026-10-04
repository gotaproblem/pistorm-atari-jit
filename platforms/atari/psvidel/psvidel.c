// SPDX-License-Identifier: MIT
/*
 * PSVIDEL - Falcon Videl + SuperVidel, emulated on the host, HDMI only.
 * See psvidel.h for the thread contract and PSVIDEL.md for the design.
 *
 * Three ways to reach a picture, one model underneath:
 *
 *   XBIOS    PSVIDEL.PRG turns VsetMode/VsetScreen/VsetRGB/... into NatFeat
 *            calls. VsetMode fills the register file with what TOS 4.04
 *            would have programmed, so a program that reads the registers
 *            back (to save them, or to nudge one) sees a real Falcon.
 *   Registers  demos poke $FF8266 (SPSHIFT), $FF8210 (line width), the
 *            vertical display window ($FF82A8/AA) and $FF9800 (palette)
 *            directly. Those registers do not exist on an ST, so they are
 *            owned here outright; the screen base ($FF8201/03/0D) and the
 *            ST shift ($FF8260) still go to the real chip and are snooped.
 *   SuperVidel  VsetMode with SVEXT picks 640x480..1920x1200 in 8-bit
 *            chunky, 16 or 32 bit; the frame lives either in ST-RAM (seen
 *            through the 0xA0000000 alias, which is what SV software does
 *            with Mxalloc(ST)|0xA0000000) or in video RAM at 0xA1000000+.
 *
 * Every path ends in recompute(), which derives ONE display description
 * from the register file (+ the SV extension) and publishes it to the
 * render thread under a seqlock.
 */
#include "psvidel.h"
#include "../../../config_file/config_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

extern volatile uint16_t st_palette[16];   /* snooped $FF8240..$FF825E */

/* ------------------------------------------------------------------ */
/* Falcon mode word (XBIOS VsetMode)                                   */
/* ------------------------------------------------------------------ */
#define VM_BPPMASK   0x0007u
#define VM_COL80     0x0008u
#define VM_VGA       0x0010u
#define VM_PAL       0x0020u
#define VM_OVERSCAN  0x0040u
#define VM_STMODES   0x0080u
#define VM_VERTFLAG  0x0100u
#define VM_VALID     0x01FFu
#define VM_SVEXT     0x4000u
#define VM_SVRES(m)  (((m) >> 9) & 0xFu)

#define BPS1   0
#define BPS2   1
#define BPS4   2
#define BPS8   3
#define BPS16  4
#define BPS32  5   /* SuperVidel only */
#define BPS8C  7   /* SuperVidel only */

enum { SRC_NONE = 0, SRC_FALCON, SRC_SV };

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static struct {
    int      configured;
    int      armed;
    uint8_t *st_ram;
    uint32_t st_limit;

    uint8_t  regs[0x100];        /* $FF8200..$FF82FF, big-endian bytes   */
    uint8_t  pal[0x400];         /* $FF9800..$FF9BFF: R, G, 0, B          */
    uint8_t  r8006, r8007;       /* monitor type / bus control            */

    int      src;                /* SRC_*                                  */
    int      sp16;               /* SPSHIFT last written as 16 colours     */
    uint32_t bpp_override;       /* XBIOS modes: bpp from the mode word    */
    uint32_t cur_mode;           /* what VsetMode(-1) answers              */
    uint32_t sv_w, sv_h, sv_bpp, sv_fmt;

    uint8_t  base_hi, base_mid, base_lo;
    uint32_t phys;               /* 32-bit display base                    */

    uint32_t gen;                /* model generation                       */
} S;

/* published display description */
static volatile uint32_t g_seq;
static psvidel_frame_t   g_pub;
static int               g_pub_active;

/* ...and as it stood at the last real VBL (psvidel_vbl, ipl_task): what
 * the Videl shows, since it takes a new base or mode only at the start
 * of a frame. BOR's VBL handler writes $FF8201/03 (which clear the low
 * byte) and then $FF820D; a renderer reading the live registers between
 * the two showed the screen 64 pixels off. */
static volatile uint32_t g_vseq;
static psvidel_frame_t   g_vpub;
static int               g_vpub_active;
static volatile uint64_t g_vbl_ns;
static uint32_t          g_base_changes;    /* screen flips seen (diag)  */

/* ...and its ST-RAM CONTENT as it stood at that VBL. The render thread
 * runs at its own rate and read the guest's screen live, so a game that
 * erases and redraws its sprites right after the VBL was sampled mid-
 * draw: two or three hedgehogs. The VBL copies the visible bytes (76 KB
 * for 320x240x8, ipl_task, ~20 us) into one of three buffers and the
 * renderer converts from that. Three, so the writer never touches the
 * one the renderer is reading. VRAM sources have dirty pages and are
 * left alone. */
#define PSV_SNAP_MAX (512u * 1024u)
typedef struct {
    uint8_t *buf;
    uint32_t base, bytes;               /* what the copy is of */
} psv_snap_t;
static psv_snap_t        g_snap[3];
static volatile int      g_snap_pub = -1;   /* last complete copy */
static volatile int      g_snap_busy = -1;  /* the renderer is reading it */

/* the palette as the renderer wants it */
static uint32_t          g_pal_xrgb[256];
static volatile uint32_t g_pal_gen;

/* ------------------------------------------------------------------ */
/* Video RAM                                                           */
/* ------------------------------------------------------------------ */
#define PSV_PAGE_SHIFT 12
#define PSV_PAGES      (PSV_VRAM_SIZE >> PSV_PAGE_SHIFT)
static uint8_t *g_vram;
static uint64_t g_dirty[PSV_PAGES / 64];

#define PSV_MAX_BLOCKS 512
typedef struct { uint32_t off, len; int used; } vblk_t;
static vblk_t   g_blk[PSV_MAX_BLOCKS];
static int      g_nblk;
static uint32_t g_screen_alloc;          /* guest address, 0 = none */

static void vram_alloc_reset(void)
{
    g_nblk = 1;
    g_blk[0].off = 0;
    g_blk[0].len = PSV_VRAM_SIZE;
    g_blk[0].used = 0;
    g_screen_alloc = 0;
}

static uint32_t vram_alloc(uint32_t bytes)
{
    if (!g_vram || bytes == 0 || bytes > PSV_VRAM_SIZE)
        return 0;
    bytes = (bytes + 4095u) & ~4095u;
    for (int i = 0; i < g_nblk; i++) {
        if (g_blk[i].used || g_blk[i].len < bytes)
            continue;
        if (g_blk[i].len > bytes) {
            if (g_nblk >= PSV_MAX_BLOCKS)
                return 0;
            memmove(&g_blk[i + 2], &g_blk[i + 1],
                    (size_t)(g_nblk - i - 1) * sizeof g_blk[0]);
            g_nblk++;
            g_blk[i + 1].off = g_blk[i].off + bytes;
            g_blk[i + 1].len = g_blk[i].len - bytes;
            g_blk[i + 1].used = 0;
            g_blk[i].len = bytes;
        }
        g_blk[i].used = 1;
        return PSV_VRAM_BASE + g_blk[i].off;
    }
    return 0;
}

static uint32_t vram_largest(void)
{
    uint32_t best = 0;
    for (int i = 0; i < g_nblk; i++)
        if (!g_blk[i].used && g_blk[i].len > best)
            best = g_blk[i].len;
    return best;
}

static int vram_free(uint32_t addr)
{
    if (addr < PSV_VRAM_BASE)
        return -1;
    uint32_t off = addr - PSV_VRAM_BASE;
    for (int i = 0; i < g_nblk; i++) {
        if (!g_blk[i].used || g_blk[i].off != off)
            continue;
        g_blk[i].used = 0;
        /* merge with the free neighbours */
        if (i + 1 < g_nblk && !g_blk[i + 1].used) {
            g_blk[i].len += g_blk[i + 1].len;
            memmove(&g_blk[i + 1], &g_blk[i + 2],
                    (size_t)(g_nblk - i - 2) * sizeof g_blk[0]);
            g_nblk--;
        }
        if (i > 0 && !g_blk[i - 1].used) {
            g_blk[i - 1].len += g_blk[i].len;
            memmove(&g_blk[i], &g_blk[i + 1],
                    (size_t)(g_nblk - i - 1) * sizeof g_blk[0]);
            g_nblk--;
        }
        return 0;
    }
    return -1;
}

void psvidel_vram_dirty(uint32_t off, uint32_t len)
{
    if (!len || off >= PSV_VRAM_SIZE)
        return;
    if (len > PSV_VRAM_SIZE - off)
        len = PSV_VRAM_SIZE - off;
    uint32_t p0 = off >> PSV_PAGE_SHIFT;
    uint32_t p1 = (off + len - 1) >> PSV_PAGE_SHIFT;
    for (uint32_t p = p0; p <= p1; p++) {
        uint64_t bit = 1ull << (p & 63);
        uint64_t *w = &g_dirty[p >> 6];
        /* plain load first: nearly every write lands on a page that is
         * already dirty this frame, and the atomic is an LDXR/STXR loop
         * on the A72 */
        if (!(__atomic_load_n(w, __ATOMIC_RELAXED) & bit))
            __atomic_fetch_or(w, bit, __ATOMIC_RELAXED);
    }
}

uint8_t *psvidel_vram(void) { return g_vram; }
int psvidel_configured(void) { return S.configured; }

/* $FF8006 as psvidel_enable() sets it: monitor type in bits 7-6 and, for
 * Falcon TOS which sizes ST-RAM from it, the memory bits (5, 4, 1) and
 * the ROM wait bits. Default: VGA, memory bits 0 (the TSR case - ST TOS
 * never reads it). */
static uint8_t g_r8006 = 0x80;
void psvidel_set_sysconfig(uint8_t r8006) { g_r8006 = r8006; }
void psvidel_cold(void) { S.r8007 &= (uint8_t)~0x40; }

/* ------------------------------------------------------------------ */
/* Register file helpers                                               */
/* ------------------------------------------------------------------ */
static inline uint32_t rw(uint32_t off)
{
    return ((uint32_t)S.regs[off & 0xFF] << 8) | S.regs[(off + 1) & 0xFF];
}

static inline void ww(uint32_t off, uint32_t v)
{
    S.regs[off & 0xFF] = (uint8_t)(v >> 8);
    S.regs[(off + 1) & 0xFF] = (uint8_t)v;
}

static void pal_entry_update(uint32_t i)
{
    const uint8_t *p = &S.pal[i * 4];
    g_pal_xrgb[i] = 0xFF000000u | ((uint32_t)p[0] << 16) |
                    ((uint32_t)p[1] << 8) | p[3];
}

/* f.base of a frame whose vertical window is empty (VDE <= VDB): the
 * Videl fetches no lines and the whole screen is border, colour 0.
 * DSPBench closes the window around every timed test (no video DMA on
 * the bus) and puts it back after; dropped as "no picture", the HDMI
 * fell back to the ST path and flipped to 320x200 each time. */
#define PSV_BASE_BLANK 0xFFFFFFFFu

static void publish(const psvidel_frame_t *f, int active)
{
    __atomic_add_fetch(&g_seq, 1, __ATOMIC_ACQ_REL);   /* odd: writing */
    g_pub = *f;
    g_pub_active = active;
    __atomic_add_fetch(&g_seq, 1, __ATOMIC_ACQ_REL);   /* even: stable */
}

/* The one place the display is derived. */
static void recompute(void)
{
    psvidel_frame_t f;
    memset(&f, 0, sizeof f);
    S.gen++;
    f.gen = S.gen;

    if (!S.armed || S.src == SRC_NONE) {
        publish(&f, 0);
        return;
    }

    uint32_t vwrap = rw(0x10);
    uint32_t off   = rw(0x0E) & 0x01FFu;
    uint32_t vdb   = rw(0xA8);
    uint32_t vde   = rw(0xAA);
    uint32_t vco   = rw(0xC2);
    uint32_t sp    = rw(0x66);
    /* Only lines inside the vertical blank window (VBE..VBB) reach the
     * screen: a VDB above VBE fetches lines nobody sees. DSPBench closes
     * the window VDB first (VDB=1 with VDE still $3FF for a moment): taken
     * raw that was a 511-line picture and the HDMI changed mode twice per
     * test. Lines skipped at the top still move the first visible one. */
    uint32_t vbb = rw(0xA4) & 0x7FFu, vbe = rw(0xA6) & 0x7FFu;
    uint32_t top = vdb, bot = vde, skip = 0;
    if (vbe && vbb > vbe) {
        if (top < vbe) { skip = vbe - top; top = vbe; }
        if (bot > vbb) bot = vbb;
    }
    uint32_t lines = bot > top ? bot - top : 0;
    if (!(vco & 0x02)) {        /* not interlaced: counts are half-lines */
        lines >>= 1;
        skip >>= 1;
    }
    if (vco & 0x01) {           /* line doubling                          */
        lines >>= 1;
        skip >>= 1;
    }

    f.hscroll = S.regs[0x65] & 0x0Fu;
    f.base = S.phys;

    if (S.src == SRC_SV) {
        f.w = S.sv_w;
        f.bpp = S.sv_bpp;
        f.fmt = S.sv_fmt;
        f.h = (lines && lines <= S.sv_h) ? lines : S.sv_h;
        f.pitch = S.sv_w * S.sv_bpp / 8u + off * 2u;
        f.ste_pal = 0;
    } else {
        uint32_t bpp = S.bpp_override;
        if (!bpp)
            bpp = (sp & 0x400) ? 1 : (sp & 0x100) ? 16 : (sp & 0x010) ? 8 : 4;
        vwrap &= 0x03FFu;
        if (!vwrap) {
            publish(&f, 0);
            return;
        }
        f.bpp = bpp;
        f.fmt = bpp == 16 ? PSV_FMT_RGB565 : PSV_FMT_PLANAR;
        f.w = vwrap * 16u / bpp;
        f.pitch = (vwrap + off) * 2u;
        /* With HSCROLL set the Videl fetches one more 16-pixel group per
         * line (bpp words), exactly like the STE shifter; software that
         * scrolls sets the line offset for that and expects the stride to
         * grow by the group. Without it every row started 16 bytes early
         * and the picture sheared as soon as a game scrolled. */
        if (f.hscroll)
            f.pitch += bpp * 2u;
        f.h = lines;
        f.base += skip * f.pitch;
        f.ste_pal = (bpp == 2);
        if (f.fmt == PSV_FMT_PLANAR)
            f.w &= ~15u;
        /* window closed: keep the last picture's size, all border */
        static uint32_t last_w, last_h;
        if (!lines && last_h && f.w == last_w) {
            f.h = last_h;
            f.base = PSV_BASE_BLANK;
        } else if (lines) {
            last_w = f.w;
            last_h = lines;
        }
    }

    if (f.w > PSV_MAX_W)
        f.w = PSV_MAX_W;
    if (f.h > PSV_MAX_H)
        f.h = PSV_MAX_H;
    publish(&f, f.w >= 16 && f.h >= 1);
}

/* ------------------------------------------------------------------ */
/* Mode tables                                                          */
/* ------------------------------------------------------------------ */
/* What TOS 4.04 programs for each VGA mode (the monitor is always VGA
 * here). Keyed on the mode with OVERSCAN, VERTFLAG and PAL masked off.
 * hht hbb hbe hdb hde hss | vft vbb vbe vdb vde vss                     */
typedef struct {
    uint16_t mode;
    uint16_t h[6];
    uint16_t v[6];
} vga_regs_t;

static const vga_regs_t k_vga[] = {
    { 0x0010, {0x0c6,0x08d,0x015,0x22d,0x011,0x096}, {0x419,0x3ff,0x03f,0x03f,0x3ff,0x415} },
    { 0x0011, {0x017,0x012,0x001,0x20a,0x009,0x011}, {0x419,0x3ff,0x03f,0x03f,0x3ff,0x415} },
    { 0x0012, {0x0c6,0x08d,0x015,0x28a,0x06b,0x096}, {0x419,0x3ff,0x03f,0x03f,0x3ff,0x415} },
    { 0x0013, {0x0c6,0x08d,0x015,0x29a,0x07b,0x096}, {0x419,0x3ff,0x03f,0x03f,0x3ff,0x415} },
    { 0x0014, {0x0c6,0x08d,0x015,0x2ac,0x091,0x096}, {0x419,0x3ff,0x03f,0x03f,0x3ff,0x415} },
    { 0x0018, {0x0c6,0x08d,0x015,0x273,0x050,0x096}, {0x419,0x3ff,0x03f,0x03f,0x3ff,0x415} },
    { 0x0019, {0x017,0x012,0x001,0x20e,0x00d,0x011}, {0x419,0x3ff,0x03f,0x03f,0x3ff,0x415} },
    { 0x001a, {0x0c6,0x08d,0x015,0x2a3,0x07c,0x096}, {0x419,0x3ff,0x03f,0x03f,0x3ff,0x415} },
    { 0x001b, {0x0c6,0x08d,0x015,0x2ab,0x084,0x096}, {0x419,0x3ff,0x03f,0x03f,0x3ff,0x415} },
    { 0x001c, {0x18e,0x11d,0x02d,0x000,0x119,0x12e}, {0x419,0x3ff,0x03f,0x03f,0x3ff,0x415} },
    { 0x0090, {0x0c6,0x08d,0x015,0x22d,0x011,0x096}, {0x419,0x3af,0x08f,0x08f,0x3af,0x415} },
    { 0x0091, {0x017,0x012,0x001,0x20e,0x00d,0x011}, {0x419,0x3af,0x08f,0x08f,0x3af,0x415} },
    { 0x0092, {0x017,0x012,0x001,0x20e,0x00d,0x011}, {0x419,0x3af,0x08f,0x08f,0x3af,0x415} },
    { 0x0098, {0x0c6,0x08d,0x015,0x273,0x050,0x096}, {0x419,0x3af,0x08f,0x08f,0x3af,0x415} },
    { 0x0099, {0x017,0x012,0x001,0x20e,0x00d,0x011}, {0x419,0x3af,0x08f,0x08f,0x3af,0x415} },
    { 0x009a, {0x017,0x012,0x001,0x20e,0x00d,0x011}, {0x419,0x3af,0x08f,0x08f,0x3af,0x415} },
};

static const vga_regs_t *vga_lookup(uint32_t mode)
{
    uint32_t key = mode & (VM_VGA | VM_COL80 | VM_STMODES | VM_BPPMASK);
    /* compat 8/16 bpp do not exist; compat 1/2/4 share rows */
    for (size_t i = 0; i < sizeof k_vga / sizeof k_vga[0]; i++)
        if (k_vga[i].mode == key)
            return &k_vga[i];
    if (key & VM_STMODES) {
        key = (key & ~VM_BPPMASK) | BPS1;
        for (size_t i = 0; i < sizeof k_vga / sizeof k_vga[0]; i++)
            if (k_vga[i].mode == key)
                return &k_vga[i];
    }
    return NULL;
}

/* SuperVidel base resolutions (SVEXT_BASERES), plain and with OVERSCAN */
static const uint16_t k_sv_res[5][2][2] = {
    { {  640,  480 }, { 1280,  720 } },
    { {  800,  600 }, { 1680, 1050 } },
    { { 1024,  768 }, { 1920, 1080 } },
    { { 1280, 1024 }, { 1920, 1200 } },
    { { 1600, 1200 }, { 2560, 1440 } },
};

typedef struct {
    int      valid, compat, sv, strez;
    uint32_t w, h, bpp, fmt, pitch, linewidth_words, vctl, spshift;
} mode_geom_t;

uint32_t psvidel_fixmode(uint32_t mode)
{
    mode &= 0xFFFFu;
    if (mode & VM_SVEXT)
        return mode | VM_VGA;            /* SV is a VGA-class output */
    mode &= VM_VALID;
    /* PAL follows the current mode, as TOS does */
    if (S.cur_mode & VM_PAL) mode |= VM_PAL; else mode &= ~VM_PAL;
    /* the monitor is VGA: a TV mode gets VGA and its vertical flipped */
    if (!(mode & VM_VGA))
        mode ^= (VM_VERTFLAG | VM_VGA);
    if (mode & VM_STMODES) {
        if ((mode & VM_BPPMASK) == BPS1)
            mode &= ~VM_VERTFLAG;        /* ST high: 400 real lines   */
        else
            mode |= VM_VERTFLAG;         /* ST low/medium: doubled    */
    }
    return mode;
}

static void mode_geometry(uint32_t mode, mode_geom_t *g)
{
    memset(g, 0, sizeof *g);
    uint32_t bc = mode & VM_BPPMASK;

    if (mode & VM_SVEXT) {
        uint32_t r = VM_SVRES(mode);
        if (r > 4)
            return;
        g->w = k_sv_res[r][(mode & VM_OVERSCAN) ? 1 : 0][0];
        g->h = k_sv_res[r][(mode & VM_OVERSCAN) ? 1 : 0][1];
        if (g->w > PSV_MAX_W || g->h > PSV_MAX_H)
            return;                      /* beyond the render stage */
        switch (bc) {
        case BPS1:  g->bpp = 1;  g->fmt = PSV_FMT_PLANAR;  g->spshift = 0x400; break;
        case BPS2:  g->bpp = 2;  g->fmt = PSV_FMT_PLANAR;  break;
        case BPS4:  g->bpp = 4;  g->fmt = PSV_FMT_PLANAR;  break;
        case BPS8:  g->bpp = 8;  g->fmt = PSV_FMT_PLANAR;  g->spshift = 0x010; break;
        case BPS16: g->bpp = 16; g->fmt = PSV_FMT_RGB565;  g->spshift = 0x100; break;
        case BPS32: g->bpp = 32; g->fmt = PSV_FMT_ARGB32;  g->spshift = 0x100; break;
        case BPS8C: g->bpp = 8;  g->fmt = PSV_FMT_CHUNKY8; g->spshift = 0x010; break;
        default: return;
        }
        g->sv = 1;
        g->pitch = g->w * g->bpp / 8u;
        g->linewidth_words = g->pitch / 2u;
        g->vctl = 0x0008;
        g->valid = 1;
        return;
    }

    if (bc > BPS16)
        return;
    if (mode & VM_STMODES) {
        g->compat = 1;
        if (bc == BPS4)      { g->w = 320; g->h = 200; g->bpp = 4; g->strez = 0; }
        else if (bc == BPS2) { g->w = 640; g->h = 200; g->bpp = 2; g->strez = 1; }
        else if (bc == BPS1) { g->w = 640; g->h = 400; g->bpp = 1; g->strez = 2; }
        else return;
        g->fmt = PSV_FMT_PLANAR;
        g->pitch = g->w * g->bpp / 8u;
        g->linewidth_words = g->pitch / 2u;
        g->vctl = (bc == BPS1) ? 0x0008 : (bc == BPS2) ? 0x0009 : 0x0005;
        g->valid = 1;
        return;
    }

    g->w = (mode & VM_COL80) ? 640 : 320;
    g->h = (mode & VM_VERTFLAG) ? 240 : 480;
    g->bpp = 1u << bc;
    g->fmt = bc == BPS16 ? PSV_FMT_RGB565 : PSV_FMT_PLANAR;
    g->linewidth_words = ((mode & VM_COL80) ? 40u : 20u) << bc;
    g->pitch = g->linewidth_words * 2u;
    g->vctl = ((mode & VM_COL80) ? 0x08 : 0x04) | ((mode & VM_VERTFLAG) ? 0x01 : 0);
    g->spshift = bc == BPS1 ? 0x400 : bc == BPS8 ? 0x010 : bc == BPS16 ? 0x100 : 0;
    g->valid = 1;
}

/* Program the register file the way VsetMode would. */
static void fill_regs(uint32_t mode, const mode_geom_t *g)
{
    const vga_regs_t *t = vga_lookup(g->sv ? (VM_VGA | VM_COL80 | BPS8) : mode);
    static const uint16_t hoff[6] = { 0x82, 0x84, 0x86, 0x88, 0x8A, 0x8C };
    static const uint16_t voff[6] = { 0xA2, 0xA4, 0xA6, 0xA8, 0xAA, 0xAC };
    if (t) {
        for (int i = 0; i < 6; i++) {
            ww(hoff[i], t->h[i]);
            ww(voff[i], t->v[i]);
        }
    }
    if (g->sv) {
        /* SV scans the display window in VGA half-lines too; programs
         * shrink it by moving VDB/VDE (ScummVM does, for 640x400) */
        uint32_t vdb = 0x3F, vde = vdb + g->h * 2u;
        ww(0xA2, vde + 0x1A);   /* vft */
        ww(0xA4, vde);          /* vbb */
        ww(0xA6, vdb);          /* vbe */
        ww(0xA8, vdb);
        ww(0xAA, vde);
        ww(0xAC, vde + 0x16);   /* vss */
    } else if (g->compat) {
        /* keep the table's window; the vctl doubling gives 200 lines */
    }
    ww(0x0E, 0);
    ww(0x10, g->linewidth_words);
    ww(0xC2, g->vctl);
    ww(0xC0, 0x0186);
    ww(0x66, g->spshift);
    S.regs[0x65] = 0;
}

/* ------------------------------------------------------------------ */
/* NatFeat side                                                         */
/* ------------------------------------------------------------------ */
static void palette_defaults(void)
{
    /* the 16 VDI colours, then a 6x6x6 cube, then a grey ramp */
    static const uint32_t vdi16[16] = {
        0xFFFFFF, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF,
        0x00FFFF, 0xAAAAAA, 0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
        0x5555FF, 0xFF55FF, 0x55FFFF, 0x000000
    };
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t rgb;
        if (i < 16)
            rgb = vdi16[i];
        else if (i < 232) {
            uint32_t c = i - 16;
            rgb = ((c / 36) * 51u) << 16 | (((c / 6) % 6) * 51u) << 8 | ((c % 6) * 51u);
        } else {
            uint32_t v = 8u + (i - 232u) * 10u;
            rgb = v << 16 | v << 8 | v;
        }
        psvidel_set_rgb(i, rgb);
    }
}

uint32_t psvidel_enable(uint32_t initial_mode)
{
    if (!S.configured)
        return 0;
    mode_geom_t g;
    S.cur_mode = psvidel_fixmode(initial_mode);
    mode_geometry(S.cur_mode, &g);
    if (g.valid)
        fill_regs(S.cur_mode, &g);
    S.r8006 = g_r8006;             /* VGA monitor: vmontype() == 2 */
    /* bit 6 is Falcon TOS's warm-start flag: it sets it once booted and
     * looks at it after a reset. It survives a re-arm (a warm reset);
     * psvidel_cold() clears it (power-on, hard reset). */
    S.r8007 = (uint8_t)(0x01 | (S.r8007 & 0x40));
    S.src = SRC_NONE;
    S.sp16 = 0;
    S.bpp_override = 0;
    palette_defaults();
    S.armed = 1;
    recompute();
    fprintf(stderr, "[PSVIDEL] armed, mode $%04X, VRAM %s\n",
            (unsigned)S.cur_mode, g_vram ? "112MB at $A1000000" : "none (24-bit bus)");
    return 1u | (g_vram ? 2u : 0u);
}

void psvidel_disable(void)
{
    S.armed = 0;
    S.src = SRC_NONE;
    S.sp16 = 0;
    recompute();
}

uint32_t psvidel_setmode(uint32_t mode)
{
    mode &= 0xFFFFu;
    if (!S.armed || mode == 0xFFFFu)
        return S.cur_mode;

    uint32_t m = psvidel_fixmode(mode);
    mode_geom_t g;
    mode_geometry(m, &g);
    if (!g.valid) {
        fprintf(stderr, "[PSVIDEL] VsetMode($%04X) rejected\n", (unsigned)mode);
        return S.cur_mode | PSV_SM_REJECT;
    }

    uint32_t old = S.cur_mode;
    S.cur_mode = m;
    fill_regs(m, &g);
    S.sp16 = 0;

    if (g.compat) {
        S.src = SRC_NONE;
        S.bpp_override = 0;
    } else if (g.sv) {
        S.src = SRC_SV;
        S.sv_w = g.w;
        S.sv_h = g.h;
        S.sv_bpp = g.bpp;
        S.sv_fmt = g.fmt;
    } else {
        S.src = SRC_FALCON;
        S.bpp_override = g.bpp;   /* 2bpp has no SPSHIFT encoding */
    }
    recompute();
    fprintf(stderr, "[PSVIDEL] VsetMode($%04X) -> %ux%u %u-bit%s%s\n",
            (unsigned)m, (unsigned)g.w, (unsigned)g.h, (unsigned)g.bpp,
            g.sv ? " SuperVidel" : "", g.compat ? " (ST compatible)" : "");
    return old | (g.compat ? (PSV_SM_COMPAT | ((uint32_t)g.strez << 17)) : 0u);
}

uint32_t psvidel_getsize(uint32_t mode)
{
    mode_geom_t g;
    mode_geometry(psvidel_fixmode(mode), &g);
    if (!g.valid)
        return 0;
    return g.pitch * g.h;
}

void psvidel_set_rgb(uint32_t idx, uint32_t rgb)
{
    if (idx > 255)
        return;
    uint8_t *p = &S.pal[idx * 4];
    p[0] = (uint8_t)(rgb >> 16);
    p[1] = (uint8_t)(rgb >> 8);
    p[2] = 0;
    p[3] = (uint8_t)rgb;
    pal_entry_update(idx);
    __atomic_add_fetch(&g_pal_gen, 1, __ATOMIC_RELEASE);
}

uint32_t psvidel_get_rgb(uint32_t idx)
{
    if (idx > 255)
        return 0;
    const uint8_t *p = &S.pal[idx * 4];
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[3];
}

/* TOS 4 keeps 4-colour modes and ST low on the STE palette registers */
int psvidel_ste_palette_mode(void)
{
    uint32_t bc = S.cur_mode & VM_BPPMASK;
    if (S.cur_mode & VM_SVEXT)
        return 0;
    return bc == BPS2 || ((S.cur_mode & VM_STMODES) && bc == BPS4);
}

uint16_t psvidel_rgb_to_ste(uint32_t rgb)
{
#define F2S(a) ((((a) >> 1) & 0x08) | (((a) >> 5) & 0x07))
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (uint16_t)((F2S(r) << 8) | (F2S(g) << 4) | F2S(b));
#undef F2S
}

void psvidel_setphys(uint32_t addr)
{
    S.phys = addr;
    recompute();
}

uint32_t psvidel_getphys(void) { return S.phys; }
int psvidel_active(void) { return S.armed && S.src != SRC_NONE; }

uint32_t psvidel_vmalloc(uint32_t mode, uint32_t value)
{
    if (!g_vram)
        return mode == 0 ? 0u : 0xFFFFFFFFu;
    if (mode == 0) {
        if (value == 0xFFFFFFFFu)
            return vram_largest();
        return vram_alloc(value);
    }
    if (mode == 1)
        return vram_free(value) == 0 ? 0u : 0xFFFFFFFFu;
    return 0xFFFFFFFFu;
}

uint32_t psvidel_screen_alloc(uint32_t bytes)
{
    if (!g_vram)
        return 0;
    if (g_screen_alloc) {
        vram_free(g_screen_alloc);
        g_screen_alloc = 0;
    }
    uint32_t a = vram_alloc(bytes);
    if (a) {
        uint32_t off = a - PSV_VRAM_BASE;
        memset(g_vram + off, 0, bytes);
        psvidel_vram_dirty(off, bytes);
        g_screen_alloc = a;
    }
    return a;
}

uint32_t psvidel_info(uint32_t what)
{
    uint32_t s;
    psvidel_frame_t f;
    int act;
    do {
        s = __atomic_load_n(&g_seq, __ATOMIC_ACQUIRE);
        f = g_pub;
        act = g_pub_active;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while ((s & 1) || s != __atomic_load_n(&g_seq, __ATOMIC_RELAXED));
    switch (what) {
    case 0: return act ? f.w : 0;
    case 1: return act ? f.h : 0;
    case 2: return act ? f.bpp : 0;
    case 3: return f.base == PSV_BASE_BLANK ? S.phys : f.base;
    case 4: return S.cur_mode;
    case 5: return (uint32_t)S.src;
    case 6: return g_vram ? vram_largest() : 0;
    case 7: return __atomic_load_n(&g_base_changes, __ATOMIC_RELAXED);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* CPU thread: hardware registers                                       */
/* ------------------------------------------------------------------ */
int psvidel_hw_owns(uint32_t a)
{
    if (!S.armed)
        return 0;
    if (a >= 0xFF9800u && a < 0xFF9C00u) return 1;        /* palette   */
    if (a >= 0xFF8280u && a < 0xFF82C4u) return 1;        /* timing    */
    if (a >= 0xFF8264u && a < 0xFF8268u) return 1;        /* hscroll, SPSHIFT */
    if (a >= 0xFF820Eu && a < 0xFF8212u) return 1;        /* offset, VWRAP */
    if (a == 0xFF8006u || a == 0xFF8007u) return 1;       /* monitor   */
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* the beam, for software that polls the counters to find a line */
static uint32_t beam_vfc(void)
{
    uint32_t vft = rw(0xA2);
    if (vft < 0x100)
        vft = 0x419;
    const uint64_t period = 16683333ull;          /* ~59.94 Hz */
    return (uint32_t)((now_ns() % period) * vft / period);
}

static uint32_t beam_hhc(void)
{
    uint32_t hht = rw(0x82) + 1u;
    const uint64_t period = 31778ull;             /* 31.47 kHz */
    return (uint32_t)((now_ns() % period) * hht / period);
}

static uint8_t reg_read8(uint32_t a)
{
    if (a >= 0xFF9800u && a < 0xFF9C00u)
        return S.pal[a - 0xFF9800u];
    if (a == 0xFF8006u) return S.r8006;
    if (a == 0xFF8007u) return S.r8007;
    uint32_t o = a & 0xFFu;
    if (o == 0x80 || o == 0x81) {
        uint32_t v = beam_hhc();
        return (uint8_t)(o == 0x80 ? v >> 8 : v);
    }
    if (o == 0xA0 || o == 0xA1) {
        uint32_t v = beam_vfc();
        return (uint8_t)(o == 0xA0 ? v >> 8 : v);
    }
    return S.regs[o];
}

uint32_t psvidel_hw_read(uint32_t a, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v = (v << 8) | reg_read8(a + (uint32_t)i);
    return v;
}

/* Visible lines from the vertical window, as recompute() counts them */
static uint32_t vis_lines(void)
{
    uint32_t vdb = rw(0xA8), vde = rw(0xAA), vco = rw(0xC2);
    uint32_t lines = vde > vdb ? vde - vdb : 0;
    if (!(vco & 0x02))
        lines >>= 1;
    if (vco & 0x01)
        lines >>= 1;
    return lines;
}

/* SPSHIFT holds the 16-colour value: a Falcon 16-colour screen, or the
 * registers of an ST-compatible one being saved/restored? Only an ST
 * geometry (80 or 40 words a line, 200 or 400 lines) stays "ST": ACE
 * Tracker pokes 640x480x16 (160 words, 480 lines) straight into the
 * registers and wants the Falcon shifter. Re-decided on every geometry
 * write, since SPSHIFT usually goes in before VWRAP and the window. */
static void sp16_decide(void)
{
    uint32_t vwrap = rw(0x10) & 0x03FFu, lines = vis_lines();
    int st = (vwrap == 80u || vwrap == 40u) && (lines == 200u || lines == 400u);
    S.src = (vwrap && !st) ? SRC_FALCON : SRC_NONE;
}

/* Side effects of one register write, once its bytes are stored. */
static void reg_effect(uint32_t o, int *mode_dirty)
{
    switch (o & ~1u) {
    case 0x66:
        /* SPSHIFT written last selects the Falcon shifter. A value with
         * none of the 2/256/65536-colour bits is the 16-colour mode -
         * which is also what a register save/restore of an ST-compatible
         * screen looks like, so it only takes the HDMI when the geometry
         * is not an ST one (sp16_decide). */
        if (rw(0x66) & 0x0510u) {
            S.src = SRC_FALCON;
            S.sp16 = 0;
        } else {
            S.sp16 = 1;
            sp16_decide();
        }
        S.bpp_override = 0;
        *mode_dirty = 1;
        break;
    case 0x0E: case 0x10: case 0x64:
    case 0xA8: case 0xAA: case 0xC2:
        if (S.sp16)
            sp16_decide();
        *mode_dirty = 1;
        break;
    default:
        break;
    }
}

int psvidel_hw_write(uint32_t a, uint32_t v, int size)
{
    int mode_dirty = 0, pal_dirty = 0;
    for (int i = 0; i < size; i++) {
        uint32_t b = a + (uint32_t)i;
        uint8_t x = (uint8_t)(v >> (8 * (size - 1 - i)));
        if (b >= 0xFF9800u && b < 0xFF9C00u) {
            uint32_t po = b - 0xFF9800u;
            if ((po & 3) == 2)
                x = 0;                     /* unused byte reads as 0 */
            S.pal[po] = x;
            pal_entry_update(po >> 2);
            pal_dirty = 1;
        } else if (b == 0xFF8006u) {
            /* monitor type is read-only */
        } else if (b == 0xFF8007u) {
            S.r8007 = x;
        } else if ((b & 0xFFFF00u) == 0xFF8200u) {
            uint32_t o = b & 0xFFu;
            if (o == 0x80 || o == 0x81 || o == 0xA0 || o == 0xA1)
                continue;                  /* counters are read-only */
            S.regs[o] = x;
            reg_effect(o, &mode_dirty);
        }
    }
    if (pal_dirty)
        __atomic_add_fetch(&g_pal_gen, 1, __ATOMIC_RELEASE);
    if (mode_dirty)
        recompute();

    /* $FF820E/F and $FF8264/5 are real on an STE: keep the STE's own
     * shifter (and its HDMI mirror) in step for word/byte writes */
    if (size <= 2 && emulator_config_shifter_ste() &&
        ((a >= 0xFF820Eu && a + (uint32_t)size <= 0xFF8210u) ||
         (a >= 0xFF8264u && a + (uint32_t)size <= 0xFF8266u)))
        return 1;
    return 0;
}

void psvidel_video_snoop(uint32_t a, uint32_t v, int size)
{
    int changed = 0;
    /* split into bytes: base regs are on odd addresses */
    for (int i = 0; i < size; i++) {
        uint32_t b = a + (uint32_t)i;
        uint8_t x = (uint8_t)(v >> (8 * (size - 1 - i)));
        switch (b) {
        case 0xFF8201u: S.base_hi = x;  S.base_lo = 0; changed = 1; break;
        case 0xFF8203u: S.base_mid = x; S.base_lo = 0; changed = 1; break;
        case 0xFF820Du: S.base_lo = x & 0xFEu;         changed = 1; break;
        case 0xFF8260u:
            /* ST shift written last: the Falcon is an ST shifter again */
            S.sp16 = 0;
            if (S.armed && S.src != SRC_NONE) {
                S.src = SRC_NONE;
                S.bpp_override = 0;
                recompute();
            }
            break;
        default:
            break;
        }
    }
    if (changed) {
        /* a register-written base is 24-bit; in SV modes it addresses the
         * SV RAM, whose first 16MB mirror ST-RAM - the same bytes */
        uint32_t np = ((uint32_t)S.base_hi << 16) | ((uint32_t)S.base_mid << 8) |
                      S.base_lo;
        if (np != S.phys)
            __atomic_add_fetch(&g_base_changes, 1, __ATOMIC_RELAXED);
        S.phys = np;
        if (S.armed && S.src != SRC_NONE)
            recompute();
    }
}

/* ------------------------------------------------------------------ */
/* SuperVidel register window: SuperBlitter + firmware version          */
/* ------------------------------------------------------------------ */
#define SVB_SRC1   0x58
#define SVB_SRC2   0x5C
#define SVB_DST    0x60
#define SVB_COUNT  0x64
#define SVB_S1OFF  0x68
#define SVB_S2OFF  0x6C
#define SVB_DOFF   0x70
#define SVB_LINES  0x74
#define SVB_CTRL   0x78
#define SVB_VER    0x7C
#define SVB_FIFO   0x80
#define SV_FW_VERSION 9            /* >= 9: the command FIFO exists */

static uint32_t g_svb[0x100 / 4];
static uint32_t g_fifo[9];
static int      g_fifo_n;
static uint32_t (*g_rd8)(uint32_t);
static void     (*g_wr8)(uint32_t, uint32_t);

void psvidel_set_mem_hooks(uint32_t (*rd8)(uint32_t),
                           void (*wr8)(uint32_t, uint32_t))
{
    g_rd8 = rd8;
    g_wr8 = wr8;
}

/* SV RAM offset (bits 26:0) -> host pointer into VRAM, or NULL for the
 * low 16MB, which is reached through the ST-RAM banks */
static uint8_t *sv_ptr(uint32_t off, uint32_t len)
{
    if (off < PSV_LO_SIZE || !g_vram)
        return NULL;
    off -= PSV_LO_SIZE;
    if (off >= PSV_VRAM_SIZE || len > PSV_VRAM_SIZE - off)
        return NULL;
    return g_vram + off;
}

static inline uint8_t sv_get(uint32_t off)
{
    uint8_t *p = sv_ptr(off, 1);
    if (p) return *p;
    if (off < PSV_LO_SIZE && g_rd8) return (uint8_t)g_rd8(off);
    return 0;
}

static inline void sv_put(uint32_t off, uint8_t v)
{
    uint8_t *p = sv_ptr(off, 1);
    if (p) { *p = v; psvidel_vram_dirty(off - PSV_LO_SIZE, 1); return; }
    if (off < PSV_LO_SIZE && g_wr8) g_wr8(off, v);
}

static void svblit_run(void)
{
    uint32_t s1 = g_svb[SVB_SRC1 / 4] & 0x07FFFFFFu;
    uint32_t s2 = g_svb[SVB_SRC2 / 4] & 0x07FFFFFFu;
    uint32_t d  = g_svb[SVB_DST / 4]  & 0x07FFFFFFu;
    uint32_t n  = (g_svb[SVB_COUNT / 4] & 0x7FFu) + 1u;
    uint32_t o1 = g_svb[SVB_S1OFF / 4], o2 = g_svb[SVB_S2OFF / 4];
    uint32_t od = g_svb[SVB_DOFF / 4];
    uint32_t lines = g_svb[SVB_LINES / 4] & 0xFFFu;
    uint32_t mode = (g_svb[SVB_CTRL / 4] >> 1) & 0xFu;

    for (uint32_t y = 0; y < lines; y++) {
        uint8_t *ps1 = sv_ptr(s1, n), *ps2 = sv_ptr(s2, n), *pd = sv_ptr(d, n);
        if (mode == 0 && ps1 && pd) {
            memmove(pd, ps1, n);
            psvidel_vram_dirty(d - PSV_LO_SIZE, n);
        } else if (mode == 1 && ps1 && ps2 && pd) {
            for (uint32_t x = 0; x < n; x++)
                if (ps2[x])
                    pd[x] = ps1[x];
            psvidel_vram_dirty(d - PSV_LO_SIZE, n);
        } else {
            for (uint32_t x = 0; x < n; x++) {
                if (mode == 1 && !sv_get(s2 + x))
                    continue;
                sv_put(d + x, sv_get(s1 + x));
            }
        }
        s1 += o1; s2 += o2; d += od;
    }
    g_svb[SVB_CTRL / 4] &= ~1u;                   /* never busy */
}

uint32_t psvidel_svreg_read(uint32_t a, int size)
{
    uint32_t o = a & 0xFFFFu;
    uint32_t lv;
    if ((o & ~3u) == SVB_VER)
        lv = SV_FW_VERSION;
    else if ((o & ~3u) == SVB_FIFO)
        lv = 1;                                   /* empty, not full */
    else if ((o & ~3u) == SVB_CTRL)
        lv = g_svb[SVB_CTRL / 4] & ~1u;           /* idle */
    else if (o < 0x100)
        lv = g_svb[o / 4];
    else
        lv = 0;
    if (size == 4)
        return lv;
    uint32_t sh = (uint32_t)(4 - size - (int)(o & 3)) * 8u;
    return (lv >> sh) & (size == 2 ? 0xFFFFu : 0xFFu);
}

void psvidel_svreg_write(uint32_t a, uint32_t v, int size)
{
    uint32_t o = a & 0xFFFFu;
    if (o >= 0x100)
        return;
    if (size != 4) {
        /* merge a partial write into its long */
        uint32_t sh = (uint32_t)(4 - size - (int)(o & 3)) * 8u;
        uint32_t m = (size == 2 ? 0xFFFFu : 0xFFu) << sh;
        uint32_t cur = g_svb[o / 4];
        v = (cur & ~m) | ((v << sh) & m);
        o &= ~3u;
    }
    if (o == SVB_FIFO) {
        g_fifo[g_fifo_n++] = v;
        if (g_fifo_n == 9) {
            static const uint32_t order[9] = {
                SVB_SRC1, SVB_SRC2, SVB_DST, SVB_COUNT, SVB_S1OFF,
                SVB_S2OFF, SVB_DOFF, SVB_LINES, SVB_CTRL };
            for (int i = 0; i < 9; i++)
                g_svb[order[i] / 4] = g_fifo[i];
            g_fifo_n = 0;
            if (g_svb[SVB_CTRL / 4] & 1u)
                svblit_run();
        }
        return;
    }
    if (o == SVB_VER)
        return;
    g_svb[o / 4] = v;
    if (o == SVB_CTRL && (v & 1u))
        svblit_run();
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                           */
/* ------------------------------------------------------------------ */
int psvidel_init(uint8_t *st_ram, uint32_t st_limit, int want_vram)
{
    memset(&S, 0, sizeof S);
    S.configured = 1;
    S.st_ram = st_ram;
    S.st_limit = st_limit;
    g_vram = NULL;
    if (want_vram) {
        void *p = mmap(NULL, PSV_VRAM_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED)
            perror("[PSVIDEL] VRAM mmap");
        else
            g_vram = (uint8_t *)p;
    }
    vram_alloc_reset();
    recompute();
    printf("[INIT] PSVIDEL Falcon/SuperVidel on HDMI%s\n",
           g_vram ? ", VRAM $A1000000-$A7FFFFFF" : " (24-bit bus: no VRAM window)");
    return g_vram != NULL;
}

void psvidel_reset(void)
{
    if (!S.configured)
        return;
    S.armed = 0;
    S.src = SRC_NONE;
    S.bpp_override = 0;
    S.sp16 = 0;
    S.cur_mode = 0;
    g_fifo_n = 0;
    vram_alloc_reset();
    recompute();
}

/* ------------------------------------------------------------------ */
/* Render thread                                                        */
/* ------------------------------------------------------------------ */
static int pub_read(psvidel_frame_t *f)
{
    uint32_t s;
    int act;
    do {
        s = __atomic_load_n(&g_seq, __ATOMIC_ACQUIRE);
        *f = g_pub;
        act = g_pub_active;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while ((s & 1) || s != __atomic_load_n(&g_seq, __ATOMIC_RELAXED));
    return act;
}

/* ipl_task, at the real VBL: latch the display for the frame starting */
void psvidel_vbl(void)
{
    psvidel_frame_t f;
    if (!S.configured)
        return;
    int act = pub_read(&f);
    __atomic_add_fetch(&g_vseq, 1, __ATOMIC_ACQ_REL);
    g_vpub = f;
    g_vpub_active = act;
    __atomic_add_fetch(&g_vseq, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_vbl_ns, now_ns(), __ATOMIC_RELEASE);

    /* snapshot an ST-RAM frame (see g_snap) */
    if (act && S.st_ram) {
        uint32_t a = f.base;
        uint64_t bytes = (uint64_t)f.pitch * f.h + 64u;
        if (a >= PSV_LO_BASE && a < PSV_LO_BASE + PSV_LO_SIZE)
            a -= PSV_LO_BASE;
        if (a < PSV_VRAM_BASE && a < S.st_limit && bytes <= S.st_limit - a &&
            bytes <= PSV_SNAP_MAX) {
            int busy = __atomic_load_n(&g_snap_busy, __ATOMIC_ACQUIRE);
            int pub  = __atomic_load_n(&g_snap_pub, __ATOMIC_ACQUIRE);
            int i = 0;
            while (i == busy || i == pub)
                i++;
            psv_snap_t *sn = &g_snap[i];
            if (!sn->buf)
                sn->buf = (uint8_t *)malloc(PSV_SNAP_MAX);
            if (sn->buf) {
                memcpy(sn->buf, S.st_ram + a, (size_t)bytes);
                sn->base = f.base;
                sn->bytes = (uint32_t)bytes;
                __atomic_store_n(&g_snap_pub, i, __ATOMIC_RELEASE);
            }
        }
    }
}

int psvidel_frame_begin(psvidel_frame_t *f)
{
    if (!S.configured)
        return 0;
    /* no VBL seen lately (masked, or a machine without one reaching
     * ipl_task): fall back to the live registers */
    uint64_t v = __atomic_load_n(&g_vbl_ns, __ATOMIC_ACQUIRE);
    if (!v || now_ns() - v > 100000000ull)
        return pub_read(f);
    uint32_t s;
    int act;
    do {
        s = __atomic_load_n(&g_vseq, __ATOMIC_ACQUIRE);
        *f = g_vpub;
        act = g_vpub_active;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while ((s & 1) || s != __atomic_load_n(&g_vseq, __ATOMIC_RELAXED));
    return act;
}

/* source bytes of the frame, or NULL if the base points nowhere useful */
static const uint8_t *frame_source(const psvidel_frame_t *f, int *in_vram,
                                   uint32_t *vram_off)
{
    uint32_t a = f->base;
    uint64_t bytes = (uint64_t)f->pitch * f->h + 64u;
    *in_vram = 0;
    if (a == PSV_BASE_BLANK)
        return NULL;
    if (a >= PSV_LO_BASE && a < PSV_LO_BASE + PSV_LO_SIZE)
        a -= PSV_LO_BASE;
    if (a >= PSV_VRAM_BASE && g_vram) {
        uint32_t off = a - PSV_VRAM_BASE;
        if (off >= PSV_VRAM_SIZE || bytes > PSV_VRAM_SIZE - off)
            return NULL;
        *in_vram = 1;
        *vram_off = off;
        return g_vram + off;
    }
    if (S.st_ram && a < S.st_limit && bytes <= S.st_limit - a) {
        /* the VBL's copy of this frame, if there is one (render thread:
         * mark it busy so the next VBL writes elsewhere) */
        int i = __atomic_load_n(&g_snap_pub, __ATOMIC_ACQUIRE);
        if (i >= 0 && g_snap[i].base == f->base && g_snap[i].bytes == bytes) {
            __atomic_store_n(&g_snap_busy, i, __ATOMIC_RELEASE);
            return g_snap[i].buf;
        }
        return S.st_ram + a;
    }
    return NULL;
}

/* 8 source bits -> 8 bytes of 0/1, MSB first */
static uint64_t g_spread[256];
static int g_spread_ok;

static void spread_init(void)
{
    for (int b = 0; b < 256; b++) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++)
            if (b & (0x80 >> i))
                v |= 1ull << (8 * i);           /* byte i = pixel i */
        g_spread[b] = v;
    }
    g_spread_ok = 1;
}

static inline uint32_t ste_xrgb(uint16_t c)
{
    uint32_t r = (c >> 8) & 0xF, g = (c >> 4) & 0xF, b = c & 0xF;
    r = ((r & 7) << 1) | (r >> 3);
    g = ((g & 7) << 1) | (g >> 3);
    b = ((b & 7) << 1) | (b >> 3);
    return 0xFF000000u | (r * 17u) << 16 | (g * 17u) << 8 | (b * 17u);
}

static void row_planar(uint32_t *out, const uint8_t *src, uint32_t w,
                       uint32_t bpp, uint32_t skip, const uint32_t *pal)
{
    uint8_t idx[16];
    uint32_t x = 0, px = 0;
    uint32_t groups = (w + skip + 15u) / 16u;
    for (uint32_t gi = 0; gi < groups; gi++) {
        const uint8_t *g = src + (size_t)gi * bpp * 2u;
        uint64_t lo = 0, hi = 0;
        for (uint32_t p = 0; p < bpp; p++) {
            lo |= g_spread[g[p * 2]] << p;
            hi |= g_spread[g[p * 2 + 1]] << p;
        }
        memcpy(idx, &lo, 8);
        memcpy(idx + 8, &hi, 8);
        for (int i = 0; i < 16; i++, px++) {
            if (px < skip)
                continue;
            if (x >= w)
                return;
            out[x++] = pal[idx[i]];
        }
    }
}

static void row_convert(const psvidel_frame_t *f, uint32_t *out,
                        const uint8_t *row, const uint32_t *pal)
{
    uint32_t w = f->w, hs = f->hscroll;
    switch (f->fmt) {
    case PSV_FMT_PLANAR:
        row_planar(out, row, w, f->bpp, hs, pal);
        break;
    case PSV_FMT_CHUNKY8:
        row += hs;
        for (uint32_t x = 0; x < w; x++)
            out[x] = pal[row[x]];
        break;
    case PSV_FMT_RGB565:
        row += hs * 2u;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t p = ((uint32_t)row[x * 2] << 8) | row[x * 2 + 1];
            uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
            out[x] = 0xFF000000u | ((r << 3) | (r >> 2)) << 16 |
                     ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2));
        }
        break;
    case PSV_FMT_ARGB32:
        row += hs * 4u;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = row + x * 4u;
            out[x] = 0xFF000000u | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | p[3];
        }
        break;
    }
}

/* Shadow of the last converted ST-RAM frame: ST-RAM is written straight
 * into natmem by the JIT, so there is no dirty map - compare instead. */
static uint8_t *g_shadow;
static size_t   g_shadow_cap;

int psvidel_frame_draw(const psvidel_frame_t *f, uint32_t *dst,
                       uint32_t dst_pitch_px, int force_full,
                       uint32_t *y0, uint32_t *y1, int *full)
{
    static uint32_t last_gen = 0, last_pal = 0;
    int in_vram = 0;
    uint32_t voff = 0;

    if (!g_spread_ok)
        spread_init();

    const uint8_t *src = frame_source(f, &in_vram, &voff);
    uint32_t pal_gen = __atomic_load_n(&g_pal_gen, __ATOMIC_ACQUIRE);
    int redraw_all = force_full || f->gen != last_gen ||
                     (f->fmt != PSV_FMT_RGB565 && f->fmt != PSV_FMT_ARGB32 &&
                      pal_gen != last_pal);

    /* the palette the frame uses */
    uint32_t pal[256];
    if (f->ste_pal) {
        for (int i = 0; i < 16; i++)
            pal[i] = ste_xrgb(st_palette[i]);
        for (int i = 16; i < 256; i++)
            pal[i] = 0xFF000000u;
        /* the STE registers change without a generation - always redraw
         * a 4-colour frame (they are small) */
        redraw_all = 1;
    } else {
        memcpy(pal, g_pal_xrgb, sizeof pal);
    }

    last_gen = f->gen;
    last_pal = pal_gen;

    /* Window closed (VDB/VDE, see PSV_BASE_BLANK): draw nothing - the
     * screen keeps the last picture. DSPBench inverts colour 0 every 1024
     * host words during its timed tests; filling the screen with it
     * strobed white/blue. */
    if (f->base == PSV_BASE_BLANK)
        return 0;
    if (!src) {
        /* base points at nothing: black, once */
        if (!redraw_all)
            return 0;
        for (uint32_t y = 0; y < f->h; y++)
            memset(dst + (size_t)y * dst_pitch_px, 0, (size_t)f->w * 4u);
        *y0 = 0; *y1 = f->h - 1; *full = 1;
        return 1;
    }

    uint32_t lo = UINT32_MAX, hi = 0;
    size_t need = (size_t)f->pitch * f->h;

    if (in_vram) {
        /* rows whose pages were written since the last frame */
        uint32_t p0 = voff >> PSV_PAGE_SHIFT;
        uint32_t p1 = (uint32_t)((voff + need - 1) >> PSV_PAGE_SHIFT);
        uint8_t pagedirty_any = 0;
        static uint8_t pdirty[(PSV_MAX_W * 4u * PSV_MAX_H) / 4096u + 4u];
        uint32_t np = p1 - p0 + 1;
        if (np > sizeof pdirty)
            np = sizeof pdirty, redraw_all = 1;
        for (uint32_t i = 0; i < np; i++) {
            uint32_t p = p0 + i;
            uint64_t bit = 1ull << (p & 63);
            uint64_t *wd = &g_dirty[p >> 6];
            uint8_t d = 0;
            if (__atomic_load_n(wd, __ATOMIC_RELAXED) & bit)
                d = (__atomic_fetch_and(wd, ~bit, __ATOMIC_ACQ_REL) & bit) != 0;
            pdirty[i] = d;
            pagedirty_any |= d;
        }
        for (uint32_t y = 0; y < f->h; y++) {
            int dirty = redraw_all;
            if (!dirty && pagedirty_any) {
                uint32_t a0 = voff + y * f->pitch;
                uint32_t a1 = a0 + f->pitch - 1;
                for (uint32_t p = a0 >> PSV_PAGE_SHIFT;
                     p <= (a1 >> PSV_PAGE_SHIFT) && !dirty; p++)
                    if (p - p0 < np && pdirty[p - p0])
                        dirty = 1;
            }
            if (!dirty)
                continue;
            row_convert(f, dst + (size_t)y * dst_pitch_px,
                        src + (size_t)y * f->pitch, pal);
            if (y < lo) lo = y;
            hi = y;
        }
    } else {
        if (g_shadow_cap < need) {
            free(g_shadow);
            g_shadow = (uint8_t *)malloc(need);
            g_shadow_cap = g_shadow ? need : 0;
            redraw_all = 1;
        }
        for (uint32_t y = 0; y < f->h; y++) {
            const uint8_t *row = src + (size_t)y * f->pitch;
            uint8_t *sh = g_shadow ? g_shadow + (size_t)y * f->pitch : NULL;
            if (!redraw_all && sh && memcmp(sh, row, f->pitch) == 0)
                continue;
            if (sh)
                memcpy(sh, row, f->pitch);
            row_convert(f, dst + (size_t)y * dst_pitch_px, row, pal);
            if (y < lo) lo = y;
            hi = y;
        }
    }

    if (lo == UINT32_MAX)
        return 0;
    *y0 = lo;
    *y1 = hi;
    *full = redraw_all;
    return 1;
}
