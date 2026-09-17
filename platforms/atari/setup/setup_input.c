/*
 * setup_input.c - see setup_input.h.
 *
 * ST keyboard: the 6850 ACIA at $FFFC00 (status/control) and $FFFC02
 * (data), read over the bus with the 68k halted. The IKBD is reset and
 * told to stop reporting the mouse and joysticks, so the only traffic
 * left is key make/break codes.
 *
 * USB: evdev nodes under /dev/input, classified the same way kbd_usb.c
 * and joy_usb.c classify them (KEY_A+KEY_Z = keyboard; BTN_GAMEPAD or
 * BTN_JOYSTICK with ABS_X/ABS_Y = gamepad).
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "setup_input.h"
#include "gpio/ps_protocol.h"

#ifndef BTN_SOUTH
#define BTN_SOUTH BTN_A
#define BTN_EAST  BTN_B
#endif

#define ACIA_CTRL 0x00FFFC00u
#define ACIA_DATA 0x00FFFC02u
#define ACIA_RDRF 0x01
#define ACIA_TDRE 0x02
#define ACIA_ERRS 0x70                 /* OVRN | FE | PE */

#define MAX_DEV 16

static struct {
    int  fd;
    int  is_pad;
} dev[MAX_DEV];
static int ndev, n_kbd, n_pad, have_st, grabbed;
static int shift_usb, shift_st;

/* ------------------------------------------------------------------ */
/* keymaps                                                            */
/* ------------------------------------------------------------------ */

/* The ST's scancodes follow the IBM PC XT set, which is why this table
 * and the evdev one below look alike. Index = scancode, two entries:
 * unshifted and shifted (US/UK layout as far as they agree). */
static const char st_ascii[0x54][2] = {
    [0x02] = { '1', '!' }, [0x03] = { '2', '"' }, [0x04] = { '3', '#' },
    [0x05] = { '4', '$' }, [0x06] = { '5', '%' }, [0x07] = { '6', '^' },
    [0x08] = { '7', '&' }, [0x09] = { '8', '*' }, [0x0A] = { '9', '(' },
    [0x0B] = { '0', ')' }, [0x0C] = { '-', '_' }, [0x0D] = { '=', '+' },
    [0x10] = { 'q', 'Q' }, [0x11] = { 'w', 'W' }, [0x12] = { 'e', 'E' },
    [0x13] = { 'r', 'R' }, [0x14] = { 't', 'T' }, [0x15] = { 'y', 'Y' },
    [0x16] = { 'u', 'U' }, [0x17] = { 'i', 'I' }, [0x18] = { 'o', 'O' },
    [0x19] = { 'p', 'P' }, [0x1A] = { '[', '{' }, [0x1B] = { ']', '}' },
    [0x1E] = { 'a', 'A' }, [0x1F] = { 's', 'S' }, [0x20] = { 'd', 'D' },
    [0x21] = { 'f', 'F' }, [0x22] = { 'g', 'G' }, [0x23] = { 'h', 'H' },
    [0x24] = { 'j', 'J' }, [0x25] = { 'k', 'K' }, [0x26] = { 'l', 'L' },
    [0x27] = { ';', ':' }, [0x28] = { '\'', '@' }, [0x29] = { '#', '~' },
    [0x2B] = { '\\', '|' },
    [0x2C] = { 'z', 'Z' }, [0x2D] = { 'x', 'X' }, [0x2E] = { 'c', 'C' },
    [0x2F] = { 'v', 'V' }, [0x30] = { 'b', 'B' }, [0x31] = { 'n', 'N' },
    [0x32] = { 'm', 'M' }, [0x33] = { ',', '<' }, [0x34] = { '.', '>' },
    [0x35] = { '/', '?' },
};

static struct si_event ev_make(enum si_key k, enum si_source s, char ch)
{
    struct si_event e = { k, s, ch };
    return e;
}

struct si_event si_map_st(unsigned char sc, int shift)
{
    switch (sc) {
    case 0x01: return ev_make(SI_ESC, SI_SRC_ST, 0);
    case 0x0E: return ev_make(SI_BACKSPACE, SI_SRC_ST, 0);
    case 0x0F: return ev_make(SI_TAB, SI_SRC_ST, 0);
    case 0x1C:
    case 0x72: return ev_make(SI_ENTER, SI_SRC_ST, 0);   /* 0x72 = keypad Enter */
    case 0x39: return ev_make(SI_SPACE, SI_SRC_ST, ' ');
    case 0x48: return ev_make(SI_UP, SI_SRC_ST, 0);
    case 0x50: return ev_make(SI_DOWN, SI_SRC_ST, 0);
    case 0x4B: return ev_make(SI_LEFT, SI_SRC_ST, 0);
    case 0x4D: return ev_make(SI_RIGHT, SI_SRC_ST, 0);
    case 0x47: return ev_make(SI_HOME, SI_SRC_ST, 0);
    case 0x52: return ev_make(SI_INSERT, SI_SRC_ST, 0);
    case 0x53: return ev_make(SI_DELETE, SI_SRC_ST, 0);
    default: break;
    }
    if (sc >= 0x3B && sc <= 0x44)                        /* F1..F10 */
        return ev_make((enum si_key)(SI_F1 + (sc - 0x3B)), SI_SRC_ST, 0);
    if (sc < 0x54 && st_ascii[sc][0])
        return ev_make(SI_CHAR, SI_SRC_ST, st_ascii[sc][shift ? 1 : 0]);
    return ev_make(SI_NONE, SI_SRC_ST, 0);
}

static const char ev_ascii[KEY_SLASH + 1][2] = {
    [KEY_1] = { '1', '!' }, [KEY_2] = { '2', '"' }, [KEY_3] = { '3', '#' },
    [KEY_4] = { '4', '$' }, [KEY_5] = { '5', '%' }, [KEY_6] = { '6', '^' },
    [KEY_7] = { '7', '&' }, [KEY_8] = { '8', '*' }, [KEY_9] = { '9', '(' },
    [KEY_0] = { '0', ')' }, [KEY_MINUS] = { '-', '_' },
    [KEY_EQUAL] = { '=', '+' },
    [KEY_Q] = { 'q', 'Q' }, [KEY_W] = { 'w', 'W' }, [KEY_E] = { 'e', 'E' },
    [KEY_R] = { 'r', 'R' }, [KEY_T] = { 't', 'T' }, [KEY_Y] = { 'y', 'Y' },
    [KEY_U] = { 'u', 'U' }, [KEY_I] = { 'i', 'I' }, [KEY_O] = { 'o', 'O' },
    [KEY_P] = { 'p', 'P' }, [KEY_LEFTBRACE] = { '[', '{' },
    [KEY_RIGHTBRACE] = { ']', '}' },
    [KEY_A] = { 'a', 'A' }, [KEY_S] = { 's', 'S' }, [KEY_D] = { 'd', 'D' },
    [KEY_F] = { 'f', 'F' }, [KEY_G] = { 'g', 'G' }, [KEY_H] = { 'h', 'H' },
    [KEY_J] = { 'j', 'J' }, [KEY_K] = { 'k', 'K' }, [KEY_L] = { 'l', 'L' },
    [KEY_SEMICOLON] = { ';', ':' }, [KEY_APOSTROPHE] = { '\'', '@' },
    [KEY_GRAVE] = { '`', '~' }, [KEY_BACKSLASH] = { '\\', '|' },
    [KEY_Z] = { 'z', 'Z' }, [KEY_X] = { 'x', 'X' }, [KEY_C] = { 'c', 'C' },
    [KEY_V] = { 'v', 'V' }, [KEY_B] = { 'b', 'B' }, [KEY_N] = { 'n', 'N' },
    [KEY_M] = { 'm', 'M' }, [KEY_COMMA] = { ',', '<' }, [KEY_DOT] = { '.', '>' },
    [KEY_SLASH] = { '/', '?' },
};

struct si_event si_map_evdev(int code, int shift)
{
    switch (code) {
    case KEY_ESC:        return ev_make(SI_ESC, SI_SRC_USB, 0);
    case KEY_BACKSPACE:  return ev_make(SI_BACKSPACE, SI_SRC_USB, 0);
    case KEY_TAB:        return ev_make(SI_TAB, SI_SRC_USB, 0);
    case KEY_ENTER:
    case KEY_KPENTER:    return ev_make(SI_ENTER, SI_SRC_USB, 0);
    case KEY_SPACE:      return ev_make(SI_SPACE, SI_SRC_USB, ' ');
    case KEY_UP:         return ev_make(SI_UP, SI_SRC_USB, 0);
    case KEY_DOWN:       return ev_make(SI_DOWN, SI_SRC_USB, 0);
    case KEY_LEFT:       return ev_make(SI_LEFT, SI_SRC_USB, 0);
    case KEY_RIGHT:      return ev_make(SI_RIGHT, SI_SRC_USB, 0);
    case KEY_HOME:       return ev_make(SI_HOME, SI_SRC_USB, 0);
    case KEY_INSERT:     return ev_make(SI_INSERT, SI_SRC_USB, 0);
    case KEY_DELETE:     return ev_make(SI_DELETE, SI_SRC_USB, 0);
    default: break;
    }
    if (code >= KEY_F1 && code <= KEY_F10)
        return ev_make((enum si_key)(SI_F1 + (code - KEY_F1)), SI_SRC_USB, 0);
    if (code >= 0 && code <= KEY_SLASH && ev_ascii[code][0])
        return ev_make(SI_CHAR, SI_SRC_USB, ev_ascii[code][shift ? 1 : 0]);
    return ev_make(SI_NONE, SI_SRC_USB, 0);
}

/* d-pad, stick and the face buttons. A = Enter, B = Esc, Start = boot
 * (the page treats SI_F10 as "boot now"). */
static struct si_event map_pad_key(int code)
{
    switch (code) {
    case BTN_DPAD_UP:    return ev_make(SI_UP, SI_SRC_PAD, 0);
    case BTN_DPAD_DOWN:  return ev_make(SI_DOWN, SI_SRC_PAD, 0);
    case BTN_DPAD_LEFT:  return ev_make(SI_LEFT, SI_SRC_PAD, 0);
    case BTN_DPAD_RIGHT: return ev_make(SI_RIGHT, SI_SRC_PAD, 0);
    case BTN_SOUTH:
    case BTN_TRIGGER:    return ev_make(SI_ENTER, SI_SRC_PAD, 0);
    case BTN_EAST:       return ev_make(SI_ESC, SI_SRC_PAD, 0);
    case BTN_START:      return ev_make(SI_F10, SI_SRC_PAD, 0);
    default:             return ev_make(SI_NONE, SI_SRC_PAD, 0);
    }
}

/* ------------------------------------------------------------------ */
/* the ST keyboard                                                     */
/* ------------------------------------------------------------------ */

static void acia_tx(uint8_t v)
{
    for (int i = 0; i < 2000; i++) {          /* ~20 ms at 7812 baud */
        if (ps_read_8(ACIA_CTRL) & ACIA_TDRE)
            break;
        usleep(10);
    }
    ps_write_8(ACIA_DATA, v);
    usleep(1500);                             /* one byte on the wire */
}

static int st_open(void)
{
    ps_write_8(ACIA_CTRL, 0x03);              /* master reset            */
    usleep(1000);
    ps_write_8(ACIA_CTRL, 0x96);              /* /64, 8N1, RTS low, RIE  */
    usleep(1000);

    acia_tx(0x80); acia_tx(0x01);             /* IKBD reset              */
    usleep(300000);                           /* it answers 0xF1         */
    acia_tx(0x12);                            /* mouse off               */
    acia_tx(0x1A);                            /* joysticks off           */

    /* swallow the reset answer and anything left in the register */
    int drained = 0;
    for (int i = 0; i < 64; i++) {
        uint8_t sr = ps_read_8(ACIA_CTRL);
        if (!(sr & ACIA_RDRF))
            break;
        ps_read_8(ACIA_DATA);
        drained++;
        usleep(200);
    }
    return drained >= 0;
}

/* One key, or SI_NONE. Break codes (bit 7) only move the shift state. */
static struct si_event st_poll(void)
{
    uint8_t sr = ps_read_8(ACIA_CTRL);
    if (!(sr & ACIA_RDRF))
        return ev_make(SI_NONE, SI_SRC_ST, 0);
    uint8_t b = ps_read_8(ACIA_DATA);
    if (sr & ACIA_ERRS)
        return ev_make(SI_NONE, SI_SRC_ST, 0);

    int release = b & 0x80;
    uint8_t sc = b & 0x7F;
    if (sc == 0x2A || sc == 0x36) {           /* either shift */
        shift_st = !release;
        return ev_make(SI_NONE, SI_SRC_ST, 0);
    }
    if (release)
        return ev_make(SI_NONE, SI_SRC_ST, 0);
    return si_map_st(sc, shift_st);
}

/* ------------------------------------------------------------------ */
/* USB                                                                 */
/* ------------------------------------------------------------------ */

static int has_bit(const unsigned long *b, int n)
{
    return (b[n / (8 * (int)sizeof(long))] >> (n % (8 * (int)sizeof(long)))) & 1;
}

static void try_dev(const char *path, int grab)
{
    if (ndev >= MAX_DEV)
        return;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return;

    unsigned long evb[EV_MAX / (8 * sizeof(long)) + 1];
    unsigned long keyb[KEY_MAX / (8 * sizeof(long)) + 1];
    unsigned long absb[ABS_MAX / (8 * sizeof(long)) + 1];
    memset(evb, 0, sizeof evb); memset(keyb, 0, sizeof keyb);
    memset(absb, 0, sizeof absb);
    ioctl(fd, EVIOCGBIT(0, sizeof evb), evb);
    if (has_bit(evb, EV_KEY)) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keyb), keyb);
    if (has_bit(evb, EV_ABS)) ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), absb);

    int pad = has_bit(evb, EV_KEY) && has_bit(evb, EV_ABS) &&
              (has_bit(keyb, BTN_GAMEPAD) || has_bit(keyb, BTN_JOYSTICK)) &&
              has_bit(absb, ABS_X) && has_bit(absb, ABS_Y);
    int kbd = !pad && has_bit(evb, EV_KEY) &&
              has_bit(keyb, KEY_A) && has_bit(keyb, KEY_Z);
    if (!pad && !kbd) {
        close(fd);
        return;
    }
    if (grab)
        ioctl(fd, EVIOCGRAB, (void *)1);
    dev[ndev].fd = fd;
    dev[ndev].is_pad = pad;
    ndev++;
    if (pad) n_pad++; else n_kbd++;
}

int si_open(int want_st, int grab)
{
    ndev = n_kbd = n_pad = have_st = 0;
    grabbed = grab;

    DIR *d = opendir("/dev/input");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "event", 5) != 0)
                continue;
            char path[300];
            snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
            try_dev(path, grab);
        }
        closedir(d);
    }
    if (want_st)
        have_st = st_open();
    return ndev + have_st;
}

void si_close(void)
{
    for (int i = 0; i < ndev; i++) {
        if (grabbed)
            ioctl(dev[i].fd, EVIOCGRAB, (void *)0);
        close(dev[i].fd);
    }
    ndev = n_kbd = n_pad = 0;
}

int si_have_st(void)        { return have_st; }
int si_usb_keyboards(void)  { return n_kbd; }
int si_gamepads(void)       { return n_pad; }

static struct si_event usb_poll(void)
{
    struct input_event ie;
    for (int i = 0; i < ndev; i++) {
        while (read(dev[i].fd, &ie, sizeof ie) == (ssize_t)sizeof ie) {
            if (dev[i].is_pad) {
                if (ie.type == EV_ABS && ie.code == ABS_HAT0X && ie.value)
                    return ev_make(ie.value < 0 ? SI_LEFT : SI_RIGHT, SI_SRC_PAD, 0);
                if (ie.type == EV_ABS && ie.code == ABS_HAT0Y && ie.value)
                    return ev_make(ie.value < 0 ? SI_UP : SI_DOWN, SI_SRC_PAD, 0);
                if (ie.type == EV_KEY && ie.value == 1) {
                    struct si_event e = map_pad_key(ie.code);
                    if (e.key != SI_NONE)
                        return e;
                }
                continue;
            }
            if (ie.type != EV_KEY)
                continue;
            if (ie.code == KEY_LEFTSHIFT || ie.code == KEY_RIGHTSHIFT) {
                shift_usb = ie.value != 0;
                continue;
            }
            if (ie.value != 1 && ie.value != 2)   /* press and autorepeat */
                continue;
            struct si_event e = si_map_evdev(ie.code, shift_usb);
            if (e.key != SI_NONE)
                return e;
        }
    }
    return ev_make(SI_NONE, SI_SRC_USB, 0);
}

struct si_event si_poll(int timeout_ms)
{
    /* Both sources have to be polled: the ACIA has no file descriptor to
     * select on, so the loop sleeps in short steps instead. */
    for (int waited = 0; ; waited += 5) {
        struct si_event e = usb_poll();
        if (e.key != SI_NONE)
            return e;
        if (have_st) {
            e = st_poll();
            if (e.key != SI_NONE)
                return e;
        }
        if (waited >= timeout_ms)
            return ev_make(SI_NONE, SI_SRC_USB, 0);
        usleep(5000);
    }
}

const char *si_key_name(const struct si_event *e, char *buf, unsigned long n)
{
    static const char *names[] = {
        "none", "up", "down", "left", "right", "enter", "esc", "tab",
        "backspace", "space", "home", "insert", "delete",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10"
    };
    const char *src = e->src == SI_SRC_ST ? "ST" :
                      e->src == SI_SRC_PAD ? "pad" : "USB";
    if (e->key == SI_CHAR)
        snprintf(buf, n, "'%c' (%s)", e->ch, src);
    else if ((unsigned)e->key < sizeof names / sizeof names[0])
        snprintf(buf, n, "%s (%s)", names[e->key], src);
    else
        snprintf(buf, n, "? (%s)", src);
    return buf;
}
