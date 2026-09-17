/*
 * harness.c - host test for shifter_setup.c. No Pi, no Atari: the bus
 * calls land in a fake board (1 MB, 512K + 512K, colour monitor) and the
 * screen is decoded back out of the planar layout and compared with what
 * was asked for. Run through run.sh.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "shifter_setup.h"
#include "psfont8x8.h"
#include "setup_input.h"
#include <linux/input.h>

uint8_t fc;
static uint8_t ram[0x100000], reg[0x10000];
static long txns;
static int gpip_mono;

/* 512K bank 0 and bank 1, STE fold: a 512K chip repeats every 0x80000 */
static int map(uint32_t a, uint32_t *out)
{
    if (a < 0x200000u) { *out = a % 0x80000u; return 1; }
    if (a < 0x400000u) { *out = 0x80000u + (a - 0x200000u) % 0x80000u; return 1; }
    return 0;
}

static void w8(uint32_t a, uint8_t v)
{
    a &= 0xFFFFFFu;
    if (a >= 0xFF0000u) { reg[a - 0xFF0000u] = v; return; }
    uint32_t m;
    if (map(a, &m)) ram[m] = v;
}

static uint8_t r8(uint32_t a)
{
    a &= 0xFFFFFFu;
    if (a == 0xFFFA01u) return gpip_mono ? 0x00 : 0x80;
    if (a >= 0xFF0000u) return reg[a - 0xFF0000u];
    uint32_t m;
    return map(a, &m) ? ram[m] : 0xFF;
}

uint8_t  ps_read_8  (uint32_t a) { return r8(a); }
uint16_t ps_read_16 (uint32_t a) { return (uint16_t)((r8(a) << 8) | r8(a + 1)); }
uint32_t ps_read_32 (uint32_t a) { return ((uint32_t)ps_read_16(a) << 16) | ps_read_16(a + 2); }
void ps_write_8  (uint32_t a, uint16_t v) { txns++; w8(a, (uint8_t)v); }
void ps_write_16 (uint32_t a, uint16_t v) { txns++; w8(a, (uint8_t)(v >> 8)); w8(a + 1, (uint8_t)v); }
void ps_write_32 (uint32_t a, uint32_t v) { txns++;
    w8(a, (uint8_t)(v >> 24)); w8(a + 1, (uint8_t)(v >> 16));
    w8(a + 2, (uint8_t)(v >> 8)); w8(a + 3, (uint8_t)v); }

static struct ss_screen s;
static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* decode a pixel straight out of the shadow, the way the shifter reads it */
static int getpix(int x, int y)
{
    if (s.planes == 1)
        return (s.shadow[y * 80 + (x >> 3)] >> (7 - (x & 7))) & 1;
    int g = x >> 4, bit = 15 - (x & 15), c = 0;
    for (int p = 0; p < 2; p++) {
        uint16_t w = (uint16_t)((s.shadow[y * 160 + g * 4 + p * 2] << 8) |
                                 s.shadow[y * 160 + g * 4 + p * 2 + 1]);
        c |= ((w >> bit) & 1) << p;
    }
    return c;
}

static int text_page_bad(int ink, int paper)
{
    int bad = 0, dup = s.planes == 1 ? 2 : 1;
    for (int row = 0; row < SS_ROWS; row++)
        for (int col = 0; col < SS_COLS; col++)
            ss_putc(&s, col, row, (unsigned char)(33 + (row * SS_COLS + col) % 94),
                    ink, paper);
    for (int row = 0; row < SS_ROWS; row++)
        for (int col = 0; col < SS_COLS; col++) {
            const uint8_t *g = ps_font8x8[33 + (row * SS_COLS + col) % 94];
            for (int i = 0; i < 8; i++)
                for (int d = 0; d < dup; d++) {
                    int y = row * 8 * dup + i * dup + d;
                    for (int b = 0; b < 8; b++)
                        bad += getpix(col * 8 + b, y) !=
                               (((g[i] >> (7 - b)) & 1) ? ink : paper);
                }
        }
    return bad;
}

static void run_mode(enum ss_mode mode, const char *name)
{
    printf("%s:\n", name);
    memset(ram, 0, sizeof ram);
    memset(reg, 0, sizeof reg);
    CHECK(ss_bringup(&s, mode, 1) == 0, "bringup failed");

    CHECK(s.bank0 == (512u << 10) && s.bank1 == (512u << 10),
          "probe read %uK + %uK, board is 512K + 512K", s.bank0 >> 10, s.bank1 >> 10);
    CHECK(s.memcfg == 0x05, "memcfg $%02X, expected $05", s.memcfg);
    CHECK(reg[0x8260] == (s.mode == SS_MODE_MONO ? 2 : 1), "wrong resolution");
    CHECK(reg[0x820A] == 0x02, "sync should be 50 Hz");
    CHECK(((uint32_t)reg[0x8201] << 16 | (uint32_t)reg[0x8203] << 8) == SS_SCREEN_BASE,
          "screen base wrong");
    CHECK(s.height * (s.planes == 1 ? 80 : 160) == (int)SS_SCREEN_BYTES,
          "mode geometry is not 32000 bytes");
    CHECK(SS_ROWS * 8 * (s.planes == 1 ? 2 : 1) == s.height,
          "80x25 grid does not fill the screen");

    /* pixels: every colour of the mode, read back out of the planes */
    int maxc = s.planes == 1 ? 1 : 3;
    int bad = 0;
    for (int y = 0; y < s.height; y += 7)
        for (int x = 0; x < 640; x += 3) {
            int c = (x * 7 + y) % (maxc + 1);
            ss_pixel(&s, x, y, c);
            bad += getpix(x, y) != c;
        }
    CHECK(bad == 0, "%d pixels came back a different colour", bad);

    /* out of range must not write anything */
    uint8_t before[SS_SCREEN_BYTES];
    memcpy(before, s.shadow, sizeof before);
    ss_pixel(&s, -1, 0, 1); ss_pixel(&s, 640, 0, 1); ss_pixel(&s, 0, s.height, 1);
    ss_putc(&s, -1, 0, 'x', 1, 0); ss_putc(&s, 0, SS_ROWS, 'x', 1, 0);
    CHECK(memcmp(before, s.shadow, sizeof before) == 0,
          "an out-of-range draw changed the screen");

    /* a string running off the right edge is cut, not wrapped */
    int ink = s.planes == 1 ? 1 : 3;
    for (int row = 0; row < SS_ROWS; row++)
        ss_clear_row(&s, row, 0);
    ss_puts(&s, SS_COLS - 1, 3, "AB", ink, 0);
    int cell = 8 * (s.planes == 1 ? 2 : 1), wrapped = 0;
    for (int y = 4 * cell; y < 5 * cell; y++)
        for (int x = 0; x < 640; x++)
            wrapped += getpix(x, y) != 0;
    CHECK(wrapped == 0, "%d pixels wrapped onto the next row", wrapped);

    /* text, both polarities */
    CHECK((bad = text_page_bad(s.planes == 1 ? 1 : 3, 0)) == 0,
          "%d bad pixels drawing text", bad);
    CHECK((bad = text_page_bad(0, s.planes == 1 ? 1 : 2)) == 0,
          "%d bad pixels drawing reversed text", bad);

    /* whole-screen write, then read back off the fake board */
    ss_write_full(&s, SS_W32);
    CHECK(ss_verify(&s) == 0, "screen did not read back");

    /* changed-only flush: one cell, then nothing */
    ss_putc(&s, 10, 10, 'Q', s.planes == 1 ? 1 : 3, 0);
    txns = 0;
    uint32_t sent = ss_flush(&s);
    CHECK(sent > 0 && sent <= 256, "one cell flushed %u bytes", sent);
    CHECK(ss_verify(&s) == 0, "flush did not reach the board");
    CHECK(ss_flush(&s) == 0, "a second flush still found changes");
    printf("  one cell: %u bytes in %ld transactions\n", sent, txns);
}

/* The keymaps, checked without any hardware: the ST's scancodes and
 * evdev's codes must reach the same key for the same physical key. */
static void run_keymaps(void)
{
    static const struct { unsigned char st; int ev; enum si_key key; char ch, sh; } t[] = {
        { 0x01, KEY_ESC,       SI_ESC,       0,    0   },
        { 0x1C, KEY_ENTER,     SI_ENTER,     0,    0   },
        { 0x0F, KEY_TAB,       SI_TAB,       0,    0   },
        { 0x0E, KEY_BACKSPACE, SI_BACKSPACE, 0,    0   },
        { 0x39, KEY_SPACE,     SI_SPACE,     ' ',  ' ' },
        { 0x48, KEY_UP,        SI_UP,        0,    0   },
        { 0x50, KEY_DOWN,      SI_DOWN,      0,    0   },
        { 0x4B, KEY_LEFT,      SI_LEFT,      0,    0   },
        { 0x4D, KEY_RIGHT,     SI_RIGHT,     0,    0   },
        { 0x3B, KEY_F1,        SI_F1,        0,    0   },
        { 0x44, KEY_F10,       SI_F10,       0,    0   },
        { 0x1E, KEY_A,         SI_CHAR,      'a',  'A' },
        { 0x32, KEY_M,         SI_CHAR,      'm',  'M' },
        { 0x0B, KEY_0,         SI_CHAR,      '0',  ')' },
        { 0x33, KEY_COMMA,     SI_CHAR,      ',',  '<' },
    };
    printf("keymaps:\n");
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        struct si_event a = si_map_st(t[i].st, 0), b = si_map_evdev(t[i].ev, 0);
        struct si_event as = si_map_st(t[i].st, 1), bs = si_map_evdev(t[i].ev, 1);
        CHECK(a.key == t[i].key, "ST $%02X gave key %d, wanted %d",
              t[i].st, a.key, t[i].key);
        CHECK(b.key == t[i].key, "evdev %d gave key %d, wanted %d",
              t[i].ev, b.key, t[i].key);
        CHECK(a.ch == t[i].ch && b.ch == t[i].ch, "unshifted char differs");
        CHECK(as.ch == t[i].sh && bs.ch == t[i].sh, "shifted char differs");
        CHECK(a.src == SI_SRC_ST && b.src == SI_SRC_USB, "wrong source tag");
    }
    /* every printable ASCII a person can type must come from some ST key */
    int reachable[128] = { 0 };
    for (int sc = 1; sc < 0x80; sc++)
        for (int sh = 0; sh < 2; sh++) {
            struct si_event e = si_map_st((unsigned char)sc, sh);
            if (e.key == SI_CHAR) reachable[(int)(unsigned char)e.ch] = 1;
        }
    for (char c = 'a'; c <= 'z'; c++) CHECK(reachable[(int)c], "no ST key for '%c'", c);
    for (char c = 'A'; c <= 'Z'; c++) CHECK(reachable[(int)c], "no ST key for '%c'", c);
    for (char c = '0'; c <= '9'; c++) CHECK(reachable[(int)c], "no ST key for '%c'", c);
    CHECK(si_map_st(0x00, 0).key == SI_NONE, "scancode 0 produced a key");
    CHECK(si_map_st(0x7F, 0).key == SI_NONE, "unmapped scancode produced a key");
    CHECK(si_map_evdev(KEY_MAX, 0).key == SI_NONE, "unmapped evdev code produced a key");

    /* stick hysteresis: commits at 50 % of half-range, releases at 35 % */
    int centre = 0, on = 8192, off = 5734, st = 0;
    st = si_stick_state(st, 100, centre, on, off);
    CHECK(st == 0, "a stick near centre moved");
    st = si_stick_state(st, 9000, centre, on, off);
    CHECK(st == 1, "a stick pushed right did not commit");
    st = si_stick_state(st, 7000, centre, on, off);
    CHECK(st == 1, "a committed stick released too early");
    st = si_stick_state(st, 2000, centre, on, off);
    CHECK(st == 0, "a stick returned to centre stayed committed");
    st = si_stick_state(st, -30000, centre, on, off);
    CHECK(st == -1, "a stick thrown the other way did not follow");
    st = si_stick_state(st, 30000, centre, on, off);
    CHECK(st == 1, "a stick flicked across did not cross over");

    char buf[48];
    struct si_event e = si_map_st(0x1E, 1);
    CHECK(strcmp(si_key_name(&e, buf, sizeof buf), "'A' (ST)") == 0,
          "key name reads \"%s\"", buf);
}

int main(void)
{
    gpip_mono = 1;
    run_mode(SS_MODE_AUTO, "auto-detect on a mono monitor");
    CHECK(s.mode == SS_MODE_MONO, "mono monitor detected as colour");
    gpip_mono = 0;
    run_mode(SS_MODE_AUTO, "auto-detect on a colour monitor");
    CHECK(s.mode == SS_MODE_COLOUR, "colour monitor detected as mono");
    run_mode(SS_MODE_MONO, "forced mono");
    run_keymaps();

    printf(fails ? "\nFAIL (%d)\n" : "\nPASS (%d failures)\n", fails);
    return fails != 0;
}
