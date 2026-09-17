/*
 * harness.c - host test for shifter_setup.c. No Pi, no Atari: the bus
 * calls land in a fake board (1 MB, 512K + 512K, colour monitor) and the
 * screen is decoded back out of the planar layout and compared with what
 * was asked for. Run through run.sh.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "shifter_setup.h"
#include "psfont8x8.h"
#include "setup_input.h"
#include "setup_cfg.h"
#include "setup_page.h"
#include "setup_enums.h"
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

    /* the pad's exit button: X, and the others that mean the same */
    CHECK(si_map_pad(BTN_NORTH).key == SI_F10, "pad X does not finish");
    CHECK(si_map_pad(BTN_START).key == SI_F10, "pad Start does not finish");
    CHECK(si_map_pad(BTN_SOUTH).key == SI_ENTER, "pad A is not Enter");
    CHECK(si_map_pad(BTN_EAST).key == SI_ESC, "pad B is not Esc");
    CHECK(si_map_pad(BTN_NORTH).src == SI_SRC_PAD, "wrong source tag on a pad key");

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

/* psctrl.cfg: sections, values, and the promise that a save changes only
 * the lines it owns. */
static const char cfg_text[] =
    "# a comment at the top\n"
    "\n"
    "[psctrl]\n"
    "countdown 5\n"
    "boot apj-os\n"
    "\n"
    "[gem]\n"
    "cpu 68030\n"
    "fpu\n"
    "# keep me exactly as I am\n"
    "rom ../roms/emutos-aranym.rom\n"
    "\n"
    "[apj-os]\n"
    "cpu 68040\n"
    "vga ET4000AX FVDI\n"
    "hostfs S /home/pistorm/atari-share\n";

static void run_cfg(void)
{
    static struct sc_cfg c;
    printf("psctrl.cfg:\n");
    CHECK(sc_load_text(&c, cfg_text) == 0, "could not parse the file");

    char secs[8][SC_SEC_LEN];
    int ns = sc_sections(&c, secs, 8);
    CHECK(ns == 3, "found %d sections, wanted 3", ns);
    CHECK(ns == 3 && !strcmp(secs[0], "psctrl") && !strcmp(secs[1], "gem") &&
          !strcmp(secs[2], "apj-os"), "sections came back in the wrong order");

    CHECK(sc_get_int(&c, "psctrl", "countdown", -1) == 5, "countdown wrong");
    const char *v = sc_get(&c, "apj-os", "cpu");
    CHECK(v && !strcmp(v, "68040"), "apj-os cpu reads \"%s\"", v ? v : "(null)");
    v = sc_get(&c, "gem", "cpu");
    CHECK(v && !strcmp(v, "68030"), "the two sections' cpu keys are confused");
    v = sc_get(&c, "apj-os", "hostfs");
    CHECK(v && !strcmp(v, "S /home/pistorm/atari-share"),
          "a value with spaces was cut: \"%s\"", v ? v : "(null)");
    v = sc_get(&c, "gem", "fpu");
    CHECK(v && !*v, "a bare key should read as present and empty");
    CHECK(sc_get(&c, "gem", "vga") == NULL, "found a key the section lacks");
    CHECK(sc_get(&c, "nosuch", "cpu") == NULL, "found a key in no section");
    CHECK(!c.dirty, "a fresh file is already dirty");

    /* edit, add, remove */
    CHECK(sc_set(&c, "gem", "cpu", "68000") == 0, "set failed");
    CHECK(!strcmp(sc_get(&c, "gem", "cpu"), "68000"), "set did not take");
    CHECK(c.dirty, "an edit did not mark the file dirty");
    CHECK(!strcmp(sc_get(&c, "apj-os", "cpu"), "68040"),
          "editing [gem] changed [apj-os]");
    CHECK(sc_set(&c, "gem", "fps", "50") == 0, "add failed");
    CHECK(!strcmp(sc_get(&c, "gem", "fps"), "50"), "added key not readable");
    CHECK(sc_set(&c, "gem", "fpu", NULL) == 0, "remove failed");
    CHECK(sc_get(&c, "gem", "fpu") == NULL, "removed key still there");
    CHECK(sc_set(&c, "stbox", "stbox_tos", "../roms/tos104.img") == 0,
          "new section failed");
    CHECK(!strcmp(sc_get(&c, "stbox", "stbox_tos"), "../roms/tos104.img"),
          "key in a new section not readable");

    /* save, reload, and diff against the original */
    char path[256];
    snprintf(path, sizeof path, "%s/sc_harness_%d.cfg",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    CHECK(sc_save(&c, path) == 0, "save failed");
    CHECK(!c.dirty, "still dirty after a save");

    static struct sc_cfg r;
    CHECK(sc_load(&r, path) == 0, "could not read the file back");
    CHECK(!strcmp(sc_get(&r, "gem", "cpu"), "68000"), "edit did not survive");
    CHECK(!strcmp(sc_get(&r, "gem", "fps"), "50"), "addition did not survive");
    CHECK(sc_get(&r, "gem", "fpu") == NULL, "removal did not survive");
    CHECK(!strcmp(sc_get(&r, "apj-os", "hostfs"), "S /home/pistorm/atari-share"),
          "an untouched value changed");

    int comment = 0, top = 0;
    for (int i = 0; i < r.n; i++) {
        if (!strcmp(r.line[i].text, "# keep me exactly as I am")) comment++;
        if (!strcmp(r.line[i].text, "# a comment at the top")) top++;
    }
    CHECK(comment == 1 && top == 1, "comments were lost or duplicated");

    char bak[300];
    snprintf(bak, sizeof bak, "%s.bak", path);
    /* first save had no previous file, so no .bak yet; a second one makes it */
    CHECK(sc_save(&c, path) == 0, "second save failed");
    CHECK(access(bak, F_OK) == 0, "no .bak after overwriting a file");
    unlink(path); unlink(bak);
}

/* sc_locate: the file must be found without $HOME, which is /root under
 * sudo. Everything happens inside a scratch tree, and ".." is inside it
 * too, so a stray write cannot land anywhere real. */
static void run_locate(void)
{
    char root[256], work[320], cwd[512], path[512];
    int created = 0;

    printf("psctrl.cfg lookup:\n");
    if (!getcwd(cwd, sizeof cwd)) { CHECK(0, "getcwd failed"); return; }
    snprintf(root, sizeof root, "%s/sc_loc_%d",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    snprintf(work, sizeof work, "%s/work", root);
    char cmd[900];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/configs %s/configs",
             root, root, work);
    CHECK(system(cmd) == 0, "could not make the scratch tree");
    CHECK(chdir(work) == 0, "could not enter the scratch tree");

    /* 1. an existing repo-local config wins over nothing else existing */
    FILE *f = fopen("configs/psctrl.cfg", "w");
    if (f) { fputs("[psctrl]\nboot gem\n", f); fclose(f); }
    CHECK(sc_locate(path, sizeof path, &created) == 0, "lookup failed");
    CHECK(!strcmp(path, "configs/psctrl.cfg"), "found \"%s\"", path);
    CHECK(!created, "reported creating a file that was already there");

    /* 2. the runtime tree beside the repo wins over the repo-local one */
    f = fopen("../configs/psctrl.cfg", "w");
    if (f) { fputs("[psctrl]\nboot apj-os\n", f); fclose(f); }
    CHECK(sc_locate(path, sizeof path, &created) == 0, "lookup failed");
    CHECK(!strcmp(path, "../configs/psctrl.cfg"), "found \"%s\"", path);

    /* 3. nothing there, but the tree has a default: it gets copied */
    CHECK(unlink("../configs/psctrl.cfg") == 0, "cleanup failed");
    CHECK(unlink("configs/psctrl.cfg") == 0, "cleanup failed");
    f = fopen("configs/psctrl.cfg.default", "w");
    if (f) { fputs("[psctrl]\ncountdown 5\nboot gem\n", f); fclose(f); }
    CHECK(sc_locate(path, sizeof path, &created) == 0, "lookup failed");
    CHECK(created, "did not report creating the file");
    CHECK(access(path, R_OK) == 0, "the created file is not readable");
    static struct sc_cfg c;
    CHECK(sc_load(&c, path) == 0, "the created file does not parse");
    CHECK(sc_get_int(&c, "psctrl", "countdown", -1) == 5,
          "the created file lost its contents");

    CHECK(chdir(cwd) == 0, "could not leave the scratch tree");
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    CHECK(system(cmd) == 0, "could not clean up");
}

/* switch rows read as enabled/disabled, never "(on)" */
static void run_switches(void)
{
    printf("switch values:\n");
    static const char *on[]  = { "", "1", "on", "yes", "true", "enabled",
                                 "enable", "ENABLED" };
    static const char *off[] = { "0", "off", "no", "false", "disabled",
                                 "disable", "OFF" };
    for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++) {
        CHECK(sp_is_switch(on[i]), "\"%s\" is not read as a switch", on[i]);
        CHECK(!strcmp(sp_switch_text(on[i]), "enabled"),
              "\"%s\" shows as %s", on[i], sp_switch_text(on[i]));
    }
    for (unsigned i = 0; i < sizeof off / sizeof off[0]; i++) {
        CHECK(sp_is_switch(off[i]), "\"%s\" is not read as a switch", off[i]);
        CHECK(!strcmp(sp_switch_text(off[i]), "disabled"),
              "\"%s\" shows as %s", off[i], sp_switch_text(off[i]));
    }
    static const char *not_switch[] = { "68040", "ET4000AX FVDI", "128M",
                                        "S /home/pistorm/atari-share", "usb" };
    for (unsigned i = 0; i < sizeof not_switch / sizeof not_switch[0]; i++) {
        CHECK(!sp_is_switch(not_switch[i]), "\"%s\" taken for a switch",
              not_switch[i]);
        CHECK(!strcmp(sp_switch_text(not_switch[i]), not_switch[i]),
              "\"%s\" was rewritten as \"%s\"", not_switch[i],
              sp_switch_text(not_switch[i]));
    }
    /* what a toggle writes must survive the emulator's own parser rules:
     * "disabled" is in its false list, "enabled" is not */
    CHECK(!strcmp(sp_switch_text("disabled"), "disabled") &&
          !strcmp(sp_switch_text("enabled"), "enabled"),
          "the words written back are not stable");
}

/* the rows that read badly as key + value */
static void run_labels(void)
{
    char buf[SC_LINE_LEN];
    printf("row labels:\n");

    CHECK(!strcmp(sp_row_label("kbd", "usb"), "usb kbd/mouse"),
          "kbd reads as \"%s\"", sp_row_label("kbd", "usb"));
    CHECK(!strcmp(sp_row_label("usb", "gamepad"), "usb gamepad"),
          "usb gamepad reads as \"%s\"", sp_row_label("usb", "gamepad"));
    CHECK(!strcmp(sp_row_label("cpu", "68040"), "cpu"), "cpu was relabelled");
    CHECK(!strcmp(sp_row_label("hostfs", "S /x"), "hostfs"),
          "hostfs was relabelled");

    /* kbd is a switch: enabled / disabled, with any options in brackets */
    CHECK(!strcmp(sp_row_value("kbd", "usb", buf, sizeof buf), "enabled"),
          "kbd usb reads \"%s\"", sp_row_value("kbd", "usb", buf, sizeof buf));
    CHECK(!strcmp(sp_row_value("kbd", "disabled", buf, sizeof buf), "disabled"),
          "kbd disabled reads \"%s\"",
          sp_row_value("kbd", "disabled", buf, sizeof buf));
    CHECK(!strcmp(sp_row_value("kbd", "usb nograb merge", buf, sizeof buf),
                  "enabled (nograb merge)"), "kbd options were lost: \"%s\"",
          sp_row_value("kbd", "usb nograb merge", buf, sizeof buf));
    CHECK(!strcmp(sp_row_value("kbd", "disabled nograb", buf, sizeof buf),
                  "disabled (nograb)"), "kbd off with options reads \"%s\"",
          sp_row_value("kbd", "disabled nograb", buf, sizeof buf));
    CHECK(sp_row_is_switch("kbd", "usb") && sp_row_is_switch("kbd", "usb nograb"),
          "kbd is not a switch row");
    CHECK(!sp_is_switch("usb"), "\"usb\" alone should not be a boolean word");

    /* usb gamepad is a switch, whichever way it is written */
    CHECK(!strcmp(sp_row_value("usb", "gamepad", buf, sizeof buf), "enabled"),
          "usb gamepad reads \"%s\"", sp_row_value("usb", "gamepad", buf, sizeof buf));
    CHECK(!strcmp(sp_row_value("usb", "gamepad off", buf, sizeof buf), "disabled"),
          "usb gamepad off reads \"%s\"",
          sp_row_value("usb", "gamepad off", buf, sizeof buf));
    CHECK(sp_row_is_switch("usb", "gamepad") &&
          sp_row_is_switch("usb", "gamepad disabled"),
          "usb gamepad is not a switch row");
    CHECK(!strcmp(sp_row_value("usb", "gamepad off nograb", buf, sizeof buf),
                  "disabled (nograb)"), "usb gamepad options reads \"%s\"",
          sp_row_value("usb", "gamepad off nograb", buf, sizeof buf));

    /* hostfs shows a drive letter as a drive, and writes back the
     * emulator's own spelling whichever way it is typed */
    CHECK(!strcmp(sp_row_value("hostfs", "S /home/pistorm/atari-share",
                               buf, sizeof buf), "S: /home/pistorm/atari-share"),
          "hostfs reads \"%s\"",
          sp_row_value("hostfs", "S /home/pistorm/atari-share", buf, sizeof buf));
    CHECK(!strcmp(sp_value_from_edit("hostfs", "S: /media/films", buf, sizeof buf),
                  "S /media/films"), "a typed S: was not converted back");
    CHECK(!strcmp(sp_value_from_edit("hostfs", "S /media/films", buf, sizeof buf),
                  "S /media/films"), "a typed S was changed");
    CHECK(!strcmp(sp_value_from_edit("cpu", "68030", buf, sizeof buf), "68030"),
          "an ordinary value was rewritten");
    /* a path that is not <letter> <path> must be left alone */
    CHECK(!strcmp(sp_row_value("hostfs", "/home/pistorm", buf, sizeof buf),
                  "/home/pistorm"), "a bare hostfs path was mangled");
}

/* the keys with a known set of values: every choice must be one the
 * emulator's own parser accepts, and the current value must be found */
static void run_enums(void)
{
    printf("value lists:\n");
    CHECK(se_count("cpu") == 6, "cpu has %d choices", se_count("cpu"));
    CHECK(se_index("cpu", "68040") == 4, "68040 not found in the cpu list");
    CHECK(se_index("cpu", "68030") == 3, "68030 not found in the cpu list");
    CHECK(se_index("cpu", "68070") == -1, "an invented cpu was accepted");
    CHECK(!strcmp(se_choice("cpu", 0), "68000"), "the cpu list starts wrong");
    CHECK(se_choice("cpu", 6) == NULL, "the cpu list runs past its end");
    CHECK(se_choice("cpu", -1) == NULL, "a negative index returned a value");

    CHECK(se_index("machine", "ste") == 1, "machine ste not found");
    CHECK(se_index("shifter", "ST") == 0, "shifter should ignore case");
    CHECK(se_index("blitter", "real") == 2, "blitter real not found");
    CHECK(se_index("monitor", "auto") == 0, "monitor auto not found");
    CHECK(se_index("vga", "ET4000AX FVDI") == 0, "the vga pair was not found");
    CHECK(se_index("jit_cache", "16384") == 3, "jit_cache 16384 not found");
    CHECK(se_index("ttram", "128M") == 3, "ttram 128M not found");

    /* spellings the parser accepts for the same choice, including a bare
     * key: `blitter` alone is enabled, `ttram` alone is 128M */
    CHECK(se_index("blitter", "") == se_index("blitter", "enabled"),
          "a bare blitter is not read as enabled");
    CHECK(se_index("blitter", "off") == se_index("blitter", "disabled"),
          "blitter off is not read as disabled");
    CHECK(se_index("ttram", "") == se_index("ttram", "128M"),
          "a bare ttram is not read as 128M");
    CHECK(se_index("ttram", "enabled") == se_index("ttram", "128M"),
          "ttram enabled is not read as 128M");
    CHECK(se_index("monitor", "color") == se_index("monitor", "colour"),
          "monitor color/colour disagree");
    CHECK(se_index("monitor", "sm124") == se_index("monitor", "mono"),
          "monitor sm124 is not mono");
    CHECK(se_index("machine", "mst") == se_index("machine", "megast"),
          "machine mst is not megast");

    CHECK(se_count("hostfs") == 0, "hostfs should be free text");
    CHECK(se_count("rom") == 0, "rom should be free text");
    CHECK(se_count("fps") == 0, "fps should be a typed number");

    /* every value in the default config must be in its list, or the page
     * would open the chooser on choice 1 and silently offer to change it */
    static struct sc_cfg c;
    if (sc_load(&c, "configs/psctrl.cfg.default") == 0 ||
        sc_load(&c, "../configs/psctrl.cfg.default") == 0) {
        static const char *secs[2] = { "gem", "apj-os" };
        for (int s2 = 0; s2 < 2; s2++) {
            char keys[64][SC_KEY_LEN];
            int nk = sc_keys(&c, secs[s2], keys, 64);
            for (int i = 0; i < nk; i++) {
                if (!se_count(keys[i]))
                    continue;
                const char *v = sc_get(&c, secs[s2], keys[i]);
                CHECK(se_index(keys[i], v) >= 0,
                      "[%s] %s = \"%s\" is not in its own list",
                      secs[s2], keys[i], v ? v : "(null)");
            }
        }
    } else {
        printf("  (psctrl.cfg.default not found from here - skipped)\n");
    }
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
    run_cfg();
    run_locate();
    run_switches();
    run_labels();
    run_enums();

    printf(fails ? "\nFAIL (%d)\n" : "\nPASS (%d failures)\n", fails);
    return fails != 0;
}
