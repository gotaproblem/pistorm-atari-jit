/* SPDX-License-Identifier: MIT
 *
 * stbox_render.c - one STBOX scanline capture to XRGB8888.
 *
 * Each captured row carries what the Shifter showed on that line: the
 * bytes it fetched, the cycle of its first pixel (56 normally, 52 on a
 * 60 Hz line, 0 with the left border removed), its resolution and STE fine
 * scroll, the palette at the line's start and every palette write made
 * during the line with its cycle. Rows are built on a half-cycle grid
 * covering the display window (cycles 56..376: 640 half-cycles = 320 low
 * or 640 medium pixels), colour 0 wherever the line fetched nothing, and a
 * palette write at cycle c colours pixels from c - STBOX_PALDX on (the
 * Shifter's pipeline; the same 28 the HDMI mirror uses, Hatari's spec512
 * alignment). Overscan pixels outside the window are clipped: the GEM
 * window shows the display area, now with the right bytes in it.
 */
#include <string.h>
#include "stbox_render.h"

#define WIN_X0     56                  /* first cycle shown                 */
#define WIN_HW     640                 /* half-cycles shown (320 cycles)    */
#define STBOX_PALDX 28

static const uint8_t lvl[8] = { 0, 36, 73, 109, 146, 182, 219, 255 };

static inline uint32_t rgb(uint16_t p, int ste)
{
    if (ste) {
        #define STE_LVL(n) ((uint8_t)(((((n) & 7) << 1) | (((n) >> 3) & 1)) * 17))
        return 0xFF000000u | ((uint32_t)STE_LVL(p >> 8) << 16) |
               ((uint32_t)STE_LVL(p >> 4) << 8) | STE_LVL(p);
        #undef STE_LVL
    }
    return 0xFF000000u | ((uint32_t)lvl[(p >> 8) & 7] << 16) |
           ((uint32_t)lvl[(p >> 4) & 7] << 8) | lvl[p & 7];
}

/* one colour row onto the half-cycle grid: idx[] palette indices */
static void decode_row(const stbox_cap_t *c, int y, uint8_t *idx)
{
    const stbox_cap_line_t *l = &c->ln[y];
    memset(idx, 0, WIN_HW);
    if (!l->bytes || l->rez > 1)
        return;
    const uint8_t *src = c->pix + (size_t)y * STBOX_CAP_PITCH;
    const int planes = l->rez == 0 ? 4 : 2;
    const int total  = l->bytes + l->extra;
    const int groups = (total + planes * 2 - 1) / (planes * 2);
    const int shown  = l->rez == 0 ? l->bytes * 2 : l->bytes * 4;   /* pixels */
    const int hx0    = 2 * (l->pstart - WIN_X0);
    int p = -(int)l->hs;                       /* fine scroll drops pixels */
    for (int g = 0; g < groups; g++) {
        const uint8_t *gp = src + g * planes * 2;
        if ((gp - src) + planes * 2 > STBOX_CAP_PITCH)
            break;
        uint16_t pw[4] = { 0, 0, 0, 0 };
        for (int k = 0; k < planes; k++)
            pw[k] = (uint16_t)((gp[2 * k] << 8) | gp[2 * k + 1]);
        for (int b = 15; b >= 0; b--, p++) {
            if (p < 0 || p >= shown)
                continue;
            uint8_t ci = 0;
            for (int k = 0; k < planes; k++)
                ci |= (uint8_t)(((pw[k] >> b) & 1u) << k);
            if (l->rez == 0) {
                const int hx = hx0 + 2 * p;
                if (hx >= 0 && hx + 1 < WIN_HW) { idx[hx] = ci; idx[hx + 1] = ci; }
            } else {
                const int hx = hx0 + p;
                if (hx >= 0 && hx < WIN_HW) idx[hx] = ci;
            }
        }
    }
}

int stbox_render_cap(const stbox_cap_t *c, int ste, uint32_t *dst,
                     uint32_t pitch, int *w, int *h)
{
    int rows = c->rows;
    if (rows <= 0)
        return 0;
    if (rows > STBOX_CAP_ROWS)
        rows = STBOX_CAP_ROWS;

    if (c->mono) {                             /* 71 Hz: 1 bit per pixel  */
        for (int y = 0; y < rows; y++) {
            const stbox_cap_line_t *l = &c->ln[y];
            uint32_t fg = 0xFF000000u, bg = 0xFFFFFFFFu;
            if (!(c->pal[y][0] & 1)) { fg = bg; bg = 0xFF000000u; }
            uint32_t *d = dst + (size_t)y * pitch;
            const uint8_t *s = c->pix + (size_t)y * STBOX_CAP_PITCH;
            const int n = l->bytes > 80 ? 80 : l->bytes;
            for (int x = 0; x < 640; x++)
                d[x] = (x >> 3) < n && ((s[x >> 3] >> (7 - (x & 7))) & 1) ? fg : bg;
        }
        *w = 640; *h = rows;
        return 1;
    }

    const int wide = c->any_med;
    uint8_t idx[WIN_HW];
    for (int y = 0; y < rows; y++) {
        uint32_t pal[16];
        for (int i = 0; i < 16; i++)
            pal[i] = rgb(c->pal[y][i], ste);
        decode_row(c, y, idx);
        uint32_t *d = dst + (size_t)y * pitch;
        int hx = 0;
        const int n = c->ln[y].npev;
        for (int e = 0; e <= n; e++) {
            int ex = WIN_HW;
            uint32_t ev = 0;
            if (e < n) {
                ev = c->pev[y][e];
                ex = 2 * ((int)(ev >> 16) - STBOX_PALDX - WIN_X0);
                if (ex < 0) ex = 0;
                if (ex > WIN_HW) ex = WIN_HW;
            }
            if (wide)
                for (; hx < ex; hx++) d[hx] = pal[idx[hx]];
            else
                for (; hx < ex; hx++) if (!(hx & 1)) d[hx >> 1] = pal[idx[hx]];
            if (e < n)
                pal[(ev >> 12) & 15] = rgb((uint16_t)(ev & 0xFFF), ste);
        }
    }
    *w = wide ? 640 : 320;
    *h = rows;
    return 1;
}
