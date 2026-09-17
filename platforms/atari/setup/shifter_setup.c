/*
 * shifter_setup.c - see shifter_setup.h.
 */
#include <string.h>
#include "shifter_setup.h"
#include "gpio/ps_protocol.h"
#include "psfont8x8.h"

extern uint8_t fc;                        /* gpio/ps_protocol.c */

#define REG_MEMCFG   0x00FF8001u
#define REG_VBASE_HI 0x00FF8201u
#define REG_VBASE_MI 0x00FF8203u
#define REG_SYNC     0x00FF820Au
#define REG_PAL0     0x00FF8240u
#define REG_REZ      0x00FF8260u
#define REG_GPIP     0x00FFFA01u

/* --- RAM probe: the same method as stram_alias_init() in
 * pistorm_natmem.cpp (both ST and STE fold offsets, ~250-word pattern),
 * duplicated because that one is static and runs inside the emulator. */
static int pattern_present(uint32_t addr)
{
    uint16_t v = 0;
    for (uint32_t a = addr; a < addr + 0x1F8; a += 2) {
        if (ps_read_16(a) != v)
            return 0;
        v = (uint16_t)(v + 0xFA54);
    }
    return 1;
}

static uint32_t probe_bank(uint32_t base)
{
    uint16_t v = 0;
    for (uint32_t a = base + 0x008; a < base + 0x200; a += 2) {
        ps_write_16(a, v);
        v = (uint16_t)(v + 0xFA54);
    }
    if (pattern_present(base + 0x40008) || pattern_present(base + 0x00208))
        return 128u << 10;
    if (pattern_present(base + 0x80008) || pattern_present(base + 0x00408))
        return 512u << 10;
    if (pattern_present(base + 0x008))
        return 2048u << 10;
    return 0;
}

/* $FF8001: bank 0 in bits 3-2, bank 1 in bits 1-0; 00 128K, 01 512K, 10 2M */
static uint8_t bank_bits(uint32_t size)
{
    if (size >= (2048u << 10)) return 2;
    if (size >= (512u << 10))  return 1;
    return 0;
}

void ss_palette(const uint16_t pal[4])
{
    for (int i = 0; i < 4; i++)
        ps_write_16(REG_PAL0 + 2u * (uint32_t)i, pal[i] & 0x0777);
}

int ss_bringup(struct ss_screen *ss, enum ss_mode force, int hz50)
{
    memset(ss, 0, sizeof *ss);
    fc = 0x5;                              /* supervisor data: GLUE ignores FC 0 */

    ps_write_8(REG_MEMCFG, 0x0A);          /* 2M|2M while probing */
    ss->bank0 = probe_bank(0x000000u);
    ss->bank1 = probe_bank(0x200000u);
    if (!ss->bank0)
        return -1;
    ss->memcfg = (uint8_t)((bank_bits(ss->bank0) << 2) | bank_bits(ss->bank1));
    ps_write_8(REG_MEMCFG, ss->memcfg);

    ss->gpip7 = (ps_read_8(REG_GPIP) >> 7) & 1;
    if (force == SS_MODE_AUTO)
        ss->mode = ss->gpip7 ? SS_MODE_COLOUR : SS_MODE_MONO;
    else
        ss->mode = force;

    if (ss->mode == SS_MODE_MONO) {
        ss->width = 640; ss->height = 400; ss->planes = 1;
    } else {
        ss->width = 640; ss->height = 200; ss->planes = 2;
    }

    /* base: ST has high + mid bytes only; low byte is always 0 */
    ps_write_8(REG_VBASE_HI, (SS_SCREEN_BASE >> 16) & 0xFF);
    ps_write_8(REG_VBASE_MI, (SS_SCREEN_BASE >> 8) & 0xFF);
    ss->hz50 = hz50 ? 1 : 0;
    ps_write_8(REG_SYNC, hz50 ? 0x02 : 0x00);   /* bit 1: 1 = 50 Hz */

    /* TOS convention: colour 0 = paper $777. Mono reads bit 0 of it. */
    static const uint16_t pal[4] = { 0x0777, 0x0700, 0x0070, 0x0000 };
    ss_palette(pal);
    ps_write_8(REG_REZ, ss->mode == SS_MODE_MONO ? 2 : 1);

    /* nothing on the board is known yet: force the first flush to write all */
    memset(ss->shown, 0xA5, sizeof ss->shown);
    return 0;
}

void ss_write_full(struct ss_screen *ss, enum ss_width w)
{
    uint32_t a = SS_SCREEN_BASE;
    const uint8_t *p = ss->shadow;
    const uint8_t *end = p + SS_SCREEN_BYTES;

    switch (w) {
    case SS_W8:
        for (; p < end; p++, a++)
            ps_write_8(a, *p);
        break;
    case SS_W16:
        for (; p < end; p += 2, a += 2)
            ps_write_16(a, (uint16_t)((p[0] << 8) | p[1]));
        break;
    case SS_W32:
        for (; p < end; p += 4, a += 4)
            ps_write_32(a, ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                           ((uint32_t)p[2] << 8) | p[3]);
        break;
    }
    memcpy(ss->shown, ss->shadow, SS_SCREEN_BYTES);
}

uint32_t ss_flush(struct ss_screen *ss)
{
    uint32_t sent = 0;
    for (uint32_t i = 0; i < SS_SCREEN_BYTES; i += 4) {
        if (memcmp(ss->shadow + i, ss->shown + i, 4) == 0)
            continue;
        const uint8_t *p = ss->shadow + i;
        ps_write_32(SS_SCREEN_BASE + i,
                    ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                    ((uint32_t)p[2] << 8) | p[3]);
        memcpy(ss->shown + i, p, 4);
        sent += 4;
    }
    return sent;
}

uint32_t ss_verify(const struct ss_screen *ss)
{
    uint32_t bad = 0;
    for (uint32_t i = 0; i < SS_SCREEN_BYTES; i += 4) {
        uint32_t v = ps_read_32(SS_SCREEN_BASE + i);
        const uint8_t *p = ss->shown + i;
        uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                         (uint8_t)(v >> 8), (uint8_t)v };
        for (int k = 0; k < 4; k++)
            bad += (b[k] != p[k]);
    }
    return bad;
}

void ss_clear(struct ss_screen *ss, int colour)
{
    ss_fill(ss, 0, 0, ss->width, ss->height, colour);
}

/* mono: 80 bytes a line, bit 7 = leftmost, 1 = ink.
 * colour: 160 bytes a line, 40 groups of (plane0 word, plane1 word). */
void ss_pixel(struct ss_screen *ss, int x, int y, int colour)
{
    if ((unsigned)x >= (unsigned)ss->width || (unsigned)y >= (unsigned)ss->height)
        return;
    if (ss->planes == 1) {
        uint8_t *b = ss->shadow + y * 80 + (x >> 3);
        uint8_t m = (uint8_t)(0x80 >> (x & 7));
        if (colour & 1) *b |= m; else *b &= (uint8_t)~m;
        return;
    }
    int group = x >> 4, bit = 15 - (x & 15);
    uint8_t *w = ss->shadow + y * 160 + group * 4;
    for (int pl = 0; pl < 2; pl++) {
        uint8_t *b = w + pl * 2 + (bit >= 8 ? 0 : 1);
        uint8_t m = (uint8_t)(1u << (bit >= 8 ? bit - 8 : bit));
        if ((colour >> pl) & 1) *b |= m; else *b &= (uint8_t)~m;
    }
}

/* One cell: 8 pixels wide, so every scanline of it is exactly one byte
 * per plane and no shifting or masking is needed.
 *   mono   byte = y * 80 + col
 *   colour byte = y * 160 + (col >> 1) * 4 + plane * 2 + (col & 1)
 * A plane's byte is all-ones, all-zeros or the glyph (or its inverse),
 * depending on whether ink and paper differ in that plane's bit. */
void ss_putc(struct ss_screen *ss, int col, int row, unsigned char c,
             int ink, int paper)
{
    if ((unsigned)col >= SS_COLS || (unsigned)row >= SS_ROWS)
        return;
    const uint8_t *glyph = ps_font8x8[c & 0x7F];
    int dup = ss->planes == 1 ? 2 : 1;          /* mono doubles each row */
    int cell_h = 8 * dup;

    for (int i = 0; i < 8; i++) {
        uint8_t g = glyph[i];
        for (int d = 0; d < dup; d++) {
            int y = row * cell_h + i * dup + d;
            for (int pl = 0; pl < ss->planes; pl++) {
                int ib = (ink >> pl) & 1, pb = (paper >> pl) & 1;
                uint8_t v = ib == pb ? (uint8_t)(ib ? 0xFF : 0x00)
                                     : (uint8_t)(ib ? g : ~g);
                if (ss->planes == 1)
                    ss->shadow[y * 80 + col] = v;
                else
                    ss->shadow[y * 160 + (col >> 1) * 4 + pl * 2 + (col & 1)] = v;
            }
        }
    }
}

void ss_puts(struct ss_screen *ss, int col, int row, const char *s,
             int ink, int paper)
{
    for (; *s && col < SS_COLS; s++, col++)
        ss_putc(ss, col, row, (unsigned char)*s, ink, paper);
}

void ss_clear_row(struct ss_screen *ss, int row, int paper)
{
    for (int col = 0; col < SS_COLS; col++)
        ss_putc(ss, col, row, ' ', paper, paper);
}

void ss_fill(struct ss_screen *ss, int x, int y, int w, int h, int colour)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            ss_pixel(ss, i, j, colour);
}
