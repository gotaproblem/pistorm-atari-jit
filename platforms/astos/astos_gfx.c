/*
 * astos_gfx.c - the logical canvas, its icons, and packing it onto the
 * real Shifter's screen (see astos.h).
 *
 * Medium (colour monitor): 640x200, 4 pens. Mono: 640x400, 1 plane; each
 * logical row is drawn twice and the desktop is the classic GEM 50 %
 * dither. Packing writes ss->shadow; ss_flush() then sends only the
 * longwords that differ from what the board already shows, so a menu
 * opening or the pointer moving costs a few hundred bytes on the bus.
 */
#include <stdio.h>
#include <string.h>
#include "astos.h"
#include "psfont8x8.h"

uint8_t g_px[AW * AH];

static int mono;

/* medium: logical colour -> pen. Pens: 0 face grey, 1 black, 2 accent
 * blue, 3 white. TOS keeps pen 0 as the border colour, which is why the
 * face grey is pen 0: the border then matches the taskbar. */
static const uint8_t pen_med[C_NCOL] = {
    [C_FACE] = 0, [C_TEXT] = 1, [C_ACCENT] = 2, [C_WHITE] = 3,
    [C_DESK] = 2, [C_DIM] = 2,
};
/* mono: 0 white, 1 black, 2 = 50 % dither */
static const uint8_t pen_mono[C_NCOL] = {
    [C_FACE] = 0, [C_TEXT] = 1, [C_ACCENT] = 1, [C_WHITE] = 0,
    [C_DESK] = 2, [C_DIM] = 1,
};

/* Fluent-ish on 3 bits a gun: #DBDBDB face, black, #006DB6 accent, white */
static const uint16_t st_pal[4]  = { 0x0666, 0x0000, 0x0035, 0x0777 };
static const uint32_t rgb_pal[4] = { 0xDBDBDB, 0x000000, 0x006DB6, 0xFFFFFF };

void g_set_mode(int m) { mono = m; }

/* the palette index a logical colour ends up as on the screen, as
 * setup_hdmi.c's sh_present reads it (mono: ink 3, paper 0) */
int g_pen(int c)
{
    if (mono)
        return pen_mono[c] == 1 ? 3 : 0;
    return pen_med[c];
}
int  g_mono(void)      { return mono; }

void g_palette_st(uint16_t pal[4])
{
    if (mono) {                             /* bit 0 of pen 0 = paper white */
        pal[0] = 0x0777; pal[1] = pal[2] = pal[3] = 0x0000;
        return;
    }
    memcpy(pal, st_pal, sizeof st_pal);
}

void g_palette_rgb(uint32_t rgb[4])
{
    if (mono) {
        rgb[0] = 0xFFFFFF; rgb[1] = rgb[2] = rgb[3] = 0x000000;
        return;
    }
    memcpy(rgb, rgb_pal, sizeof rgb_pal);
}

/* ---- drawing ------------------------------------------------------- */

void g_fill(int x, int y, int w, int h, int c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > AW) w = AW - x;
    if (y + h > AH) h = AH - y;
    if (w <= 0 || h <= 0)
        return;
    for (int j = y; j < y + h; j++)
        memset(g_px + j * AW + x, c, (size_t)w);
}

void g_clear(int c)                      { g_fill(0, 0, AW, AH, c); }
void g_hline(int x, int y, int w, int c) { g_fill(x, y, w, 1, c); }
/* Medium pixels are twice as tall as wide, so a vertical line is 2 px
 * wide to weigh the same as a 1 px horizontal one. */
void g_vline(int x, int y, int h, int c) { g_fill(x, y, 2, h, c); }

void g_frame(int x, int y, int w, int h, int c)
{
    g_hline(x, y, w, c);
    g_hline(x, y + h - 1, w, c);
    g_vline(x, y, h, c);
    g_vline(x + w - 2, y, h, c);
}

void g_textn(int x, int y, const char *s, int max_chars, int c)
{
    for (int n = 0; *s && n < max_chars; s++, n++, x += 8) {
        const uint8_t *gl = ps_font8x8[(unsigned char)*s & 0x7F];
        for (int r = 0; r < 8; r++) {
            int yy = y + r;
            if ((unsigned)yy >= AH)
                continue;
            for (int b = 0; b < 8; b++)
                if ((gl[r] << b) & 0x80) {
                    int xx = x + b;
                    if ((unsigned)xx < AW)
                        g_px[yy * AW + xx] = (uint8_t)c;
                }
        }
    }
}

void g_text(int x, int y, const char *s, int c) { g_textn(x, y, s, 1 << 20, c); }
int  g_text_w(const char *s)                    { return 8 * (int)strlen(s); }

/* ---- icons: 32x16 logical (32x32 on a mono screen, square there;
 * on medium the 2:1 pixels make them look 32x32 too) ---------------- */

static void icon_screen(int x, int y, int bezel, int glass)
{
    g_fill(x + 2, y, 28, 12, bezel);          /* case                     */
    g_frame(x + 2, y, 28, 12, C_TEXT);
    g_fill(x + 6, y + 2, 20, 8, glass);       /* screen                   */
    g_fill(x + 12, y + 12, 8, 2, C_TEXT);     /* neck                     */
    g_fill(x + 6, y + 14, 20, 2, C_TEXT);     /* foot                     */
}

void g_icon(int x, int y, int kind)
{
    switch (kind) {
    case K_TOS:                               /* ST monitor, GEM desktop  */
        icon_screen(x, y, C_FACE, C_WHITE);
        g_fill(x + 6, y + 2, 20, 1, C_ACCENT);   /* menu bar               */
        g_fill(x + 8, y + 4, 4, 2, C_TEXT);      /* two desktop icons      */
        g_fill(x + 8, y + 7, 4, 2, C_TEXT);
        break;
    case K_HDMI:                              /* flat panel, wide         */
        g_fill(x, y + 1, 32, 11, C_TEXT);
        g_fill(x + 2, y + 2, 28, 9, C_ACCENT);
        g_fill(x + 4, y + 9, 24, 1, C_WHITE);    /* taskbar                */
        g_fill(x + 14, y + 12, 4, 2, C_TEXT);
        g_fill(x + 8, y + 14, 16, 1, C_TEXT);
        break;
    case K_APJ:                               /* a window on a desktop    */
        icon_screen(x, y, C_FACE, C_ACCENT);
        g_fill(x + 9, y + 3, 12, 5, C_WHITE);
        g_fill(x + 9, y + 3, 12, 1, C_TEXT);
        g_fill(x + 6, y + 9, 20, 1, C_FACE);     /* taskbar                */
        break;
    case K_METAL:                             /* a joystick               */
        g_fill(x + 6, y + 10, 20, 5, C_TEXT);    /* base                   */
        g_fill(x + 8, y + 11, 16, 3, C_FACE);
        g_fill(x + 15, y + 3, 2, 8, C_TEXT);     /* stick                  */
        g_fill(x + 12, y + 0, 8, 4, C_ACCENT);   /* knob                   */
        g_frame(x + 12, y + 0, 8, 4, C_TEXT);
        g_fill(x + 20, y + 9, 4, 2, C_ACCENT);   /* fire button            */
        break;
    case K_NATIVE:                            /* the ST itself: keyboard  */
    default:
        g_fill(x, y + 4, 32, 10, C_TEXT);
        g_fill(x + 2, y + 5, 28, 8, C_FACE);
        for (int r = 0; r < 3; r++)              /* key rows               */
            for (int k = 0; k < 6; k++)
                g_fill(x + 4 + k * 4, y + 6 + r * 2, 2, 1, C_TEXT);
        g_fill(x + 8, y + 12, 14, 1, C_TEXT);    /* space bar              */
        g_fill(x + 27, y + 6, 2, 1, C_ACCENT);   /* power light            */
        break;
    }
}

/* ---- packing onto the Shifter ------------------------------------- */

static uint8_t base[SS_SCREEN_BYTES];    /* the packed canvas, no pointer */

static const uint8_t *raw_screen;
static const uint32_t *live_rgb;

void g_live_rgb(const uint32_t *rgb) { live_rgb = rgb; }

void g_raw(const uint8_t *screen) { raw_screen = screen; }
int  g_raw_active(void) { return raw_screen != NULL; }

void g_pack(struct ss_screen *ss)
{
    (void)ss;
    if (raw_screen) {
        memcpy(base, raw_screen, SS_SCREEN_BYTES);
        return;
    }
    if (mono) {
        for (int yy = 0; yy < 400; yy++) {
            const uint8_t *src = g_px + (yy >> 1) * AW;
            uint8_t *dst = base + yy * 80;
            for (int bx = 0; bx < 80; bx++) {
                uint8_t v = 0;
                for (int b = 0; b < 8; b++) {
                    int x = bx * 8 + b;
                    int p = pen_mono[src[x]];
                    int ink = p == 2 ? ((x ^ yy) & 1) : p;
                    v = (uint8_t)((v << 1) | ink);
                }
                dst[bx] = v;
            }
        }
        return;
    }
    for (int y = 0; y < AH; y++) {
        const uint8_t *src = g_px + y * AW;
        uint8_t *dst = base + y * 160;
        for (int g = 0; g < 40; g++) {
            uint16_t p0 = 0, p1 = 0;
            for (int b = 0; b < 16; b++) {
                int pen = pen_med[src[g * 16 + b]];
                p0 = (uint16_t)((p0 << 1) | (pen & 1));
                p1 = (uint16_t)((p1 << 1) | ((pen >> 1) & 1));
            }
            dst[g * 4 + 0] = (uint8_t)(p0 >> 8);
            dst[g * 4 + 1] = (uint8_t)p0;
            dst[g * 4 + 2] = (uint8_t)(p1 >> 8);
            dst[g * 4 + 3] = (uint8_t)p1;
        }
    }
}

/* The pointer, drawn in PHYSICAL pixels after packing so it keeps its
 * shape on both monitors. '#' = black, 'o' = white. */
static const char *const ptr_mono[] = {
    "#.........", "##........", "#o#.......", "#oo#......", "#ooo#.....",
    "#oooo#....", "#ooooo#...", "#oooooo#..", "#ooooooo#.", "#oooo####.",
    "#o#oo#....", "##.#oo#...", "#...#oo#..", "....#oo#..", ".....##...",
};
static const char *const ptr_med[] = {
    "#.........", "#o#.......", "#ooo#.....", "#ooooo#...", "#ooooooo#.",
    "#o#oo#....", "#...#oo#..", ".....##...",
};

static void phys_pixel(struct ss_screen *ss, int x, int y, int black)
{
    if (mono) {
        if ((unsigned)x >= 640 || (unsigned)y >= 400) return;
        uint8_t *b = ss->shadow + y * 80 + (x >> 3);
        uint8_t m = (uint8_t)(0x80 >> (x & 7));
        if (black) *b |= m; else *b &= (uint8_t)~m;
    } else {
        if ((unsigned)x >= 640 || (unsigned)y >= 200) return;
        int pen = black ? pen_med[C_TEXT] : pen_med[C_WHITE];
        int g = x >> 4, bit = 15 - (x & 15);
        uint8_t *w = ss->shadow + y * 160 + g * 4;
        for (int pl = 0; pl < 2; pl++) {
            uint8_t *b = w + pl * 2 + (bit >= 8 ? 0 : 1);
            uint8_t m = (uint8_t)(1u << (bit >= 8 ? bit - 8 : bit));
            if ((pen >> pl) & 1) *b |= m; else *b &= (uint8_t)~m;
        }
    }
}

void g_compose(struct ss_screen *ss, int mx, int my, int show_ptr)
{
    memcpy(ss->shadow, base, SS_SCREEN_BYTES);
    if (!show_ptr || raw_screen)
        return;
    const char *const *p = mono ? ptr_mono : ptr_med;
    int rows = mono ? (int)(sizeof ptr_mono / sizeof *ptr_mono)
                    : (int)(sizeof ptr_med / sizeof *ptr_med);
    for (int r = 0; r < rows; r++)
        for (int c = 0; p[r][c]; c++)
            if (p[r][c] != '.')
                phys_pixel(ss, mx + c, my + r, p[r][c] == '#');
}

/* the screen as it is on the monitor, 640x400 (medium rows doubled) */
int g_screenshot(const struct ss_screen *ss, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    uint32_t rgb[4];
    g_palette_rgb(rgb);
    if (live_rgb && !mono)
        memcpy(rgb, live_rgb, sizeof rgb);
    fprintf(f, "P6\n640 400\n255\n");
    for (int yo = 0; yo < 400; yo++) {
        for (int x = 0; x < 640; x++) {
            int c;
            if (mono) {
                c = (ss->shadow[yo * 80 + (x >> 3)] >> (7 - (x & 7))) & 1;
            } else {
                int y = yo >> 1, g = x >> 4, bit = 15 - (x & 15);
                c = 0;
                for (int pl = 0; pl < 2; pl++) {
                    uint16_t wd = (uint16_t)((ss->shadow[y * 160 + g * 4 + pl * 2] << 8) |
                                              ss->shadow[y * 160 + g * 4 + pl * 2 + 1]);
                    c |= ((wd >> bit) & 1) << pl;
                }
            }
            uint8_t px[3] = { (uint8_t)(rgb[c] >> 16), (uint8_t)(rgb[c] >> 8),
                              (uint8_t)rgb[c] };
            fwrite(px, 1, 3, f);
        }
    }
    return fclose(f);
}
