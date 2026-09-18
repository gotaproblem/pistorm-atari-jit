/*
 * joy_usb.c - USB/Bluetooth game controllers as Atari joysticks
 *
 * See joy_usb.h for the contract. Two pads: pad 0 = ST joystick 1 (game
 * port) + STE joypad A, pad 1 = ST joystick 0 (mouse port) + STE joypad B.
 *
 * Mapping (Xbox names; any evdev gamepad with the standard BTN_GAMEPAD
 * codes works the same - wired xpad and Bluetooth hid-microsoft both do):
 *
 *   D-pad or left stick   direction (bits 0-3)
 *   A (or BTN_TRIGGER)    fire (bit 7)            STE pad button A
 *   B                                             STE pad button B
 *   X                                             STE pad button C
 *   Y                                             STE pad button OPTION
 *   Start                 Space bar tap           STE pad PAUSE
 *
 * The stick is read through EVIOCGABS ranges, never assumed: xpad reports
 * -32768..32767, the Bluetooth HID path 0..65535. Engage at 50% of the
 * half-range, release at 35% (hysteresis), so a stick resting near the
 * threshold cannot chatter packets down the 7812.5 bps IKBD link.
 */

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "joy_usb.h"

bool JOY_USB_enabled = false;

/* Older kernel headers lack the gamepad aliases. */
#ifndef BTN_SOUTH
#define BTN_SOUTH BTN_A
#define BTN_EAST  BTN_B
#define BTN_NORTH BTN_X
#define BTN_WEST  BTN_Y
#endif
#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP    0x220
#define BTN_DPAD_DOWN  0x221
#define BTN_DPAD_LEFT  0x222
#define BTN_DPAD_RIGHT 0x223
#endif

/* pressed evdev buttons, input-thread private */
#define B_A      0x0001
#define B_B      0x0002
#define B_X      0x0004
#define B_Y      0x0008
#define B_LB     0x0010
#define B_RB     0x0020
#define B_START  0x0040
#define B_SELECT 0x0080
#define B_DUP    0x0100
#define B_DDOWN  0x0200
#define B_DLEFT  0x0400
#define B_DRIGHT 0x0800
#define B_TRIG   0x1000

typedef struct {
    int  fd;                   /* -1 = slot free                        */
    char name[64];
    struct { int on, off, centre; } ax[2];   /* X, Y thresholds        */
    int  stick[2];             /* -1 / 0 / 1 after hysteresis           */
    int  hat[2];               /* ABS_HAT0X/Y                           */
    unsigned btn;              /* B_* bitmask                            */
    int  dirty;                /* recompute at EV_SYN                    */
    /* published (any thread reads) */
    _Atomic uint8_t joy;       /* STJOY_* byte                           */
    _Atomic uint8_t pad;       /* STPAD_* extras                         */
    /* emission bookkeeping (input thread) */
    int  last_sent;            /* -1 = nothing sent yet                  */
    int  start_sent;
} joypad;

static joypad pads[JOY_USB_MAX_PADS] = {
    { .fd = -1, .last_sent = -1 }, { .fd = -1, .last_sent = -1 },
};
static _Atomic int n_pads;
static _Atomic int resend_wanted;
static joy_usb_emit_hooks hooks;

/* monitoring modes ($17 / $18): what the IKBD reports, and how often */
static _Atomic int mon_mode;
static _Atomic int mon_rate_cs;
static _Atomic int mon_idx;                /* byte parity of real reports */
static uint64_t    mon_next_us;            /* standalone: next report due */

static uint64_t mon_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* $17 report: %000000AB (A joystick 0 fire, B joystick 1 fire), then
 * %LLLLRRRR (joystick 0 directions, joystick 1 directions) */
static uint8_t mon_buttons(void)
{
    return (uint8_t)(((pads[1].joy & STJOY_FIRE) ? 2 : 0) |
                     ((pads[0].joy & STJOY_FIRE) ? 1 : 0));
}
static uint8_t mon_dirs(void)
{
    return (uint8_t)(((pads[1].joy & 0x0F) << 4) | (pads[0].joy & 0x0F));
}
/* $18 report: joystick 1's fire sampled eight times a byte */
static uint8_t mon_fire(void)
{
    return (pads[0].joy & STJOY_FIRE) ? 0xFF : 0x00;
}

void joy_usb_monitor_set(int mode, int rate_cs)
{
    atomic_store(&mon_mode, mode);
    atomic_store(&mon_rate_cs, rate_cs < 1 ? 1 : rate_cs);
    atomic_store(&mon_idx, 0);
    mon_next_us = 0;
}

static int has_bit(const unsigned long *bits, int bit)
{
    return (bits[bit / (8 * (int)sizeof(long))] >>
            (bit % (8 * (int)sizeof(long)))) & 1;
}

void joy_usb_set_hooks(const joy_usb_emit_hooks *h)
{
    hooks = *h;
}

/* ------------------------------------------------------------------ */
/* device classification + slots (input thread)                        */
/* ------------------------------------------------------------------ */

int joy_usb_is_gamepad(const unsigned long *evbits,
                       const unsigned long *keybits,
                       const unsigned long *absbits)
{
    if (!has_bit(evbits, EV_KEY) || !has_bit(evbits, EV_ABS))
        return 0;
    /* a gamepad (BTN_GAMEPAD == BTN_SOUTH/BTN_A) or a classic joystick
     * (BTN_JOYSTICK == BTN_TRIGGER) with an X/Y axis pair */
    if (!has_bit(keybits, BTN_GAMEPAD) && !has_bit(keybits, BTN_JOYSTICK))
        return 0;
    return has_bit(absbits, ABS_X) && has_bit(absbits, ABS_Y);
}

static void axis_setup(int fd, int code, joypad *p, int i)
{
    struct input_absinfo ai;
    memset(&ai, 0, sizeof ai);
    if (ioctl(fd, EVIOCGABS(code), &ai) < 0 || ai.maximum <= ai.minimum)
    {
        ai.minimum = -32768;              /* xpad's range, a safe guess */
        ai.maximum =  32767;
    }
    const int half = (ai.maximum - ai.minimum) / 2;
    p->ax[i].centre = ai.minimum + half;
    p->ax[i].on  = half / 2;              /* 50 %                      */
    p->ax[i].off = (half * 35) / 100;     /* 35 %                      */
}

int joy_usb_dev_open(int fd, const char *name)
{
    for (int i = 0; i < JOY_USB_MAX_PADS; i++)
    {
        joypad *p = &pads[i];
        if (p->fd >= 0)
            continue;
        memset(p, 0, sizeof *p);
        p->fd = fd;
        p->last_sent = -1;
        snprintf(p->name, sizeof p->name, "%s", name ? name : "?");
        axis_setup(fd, ABS_X, p, 0);
        axis_setup(fd, ABS_Y, p, 1);
        atomic_store(&p->joy, 0);
        atomic_store(&p->pad, 0);
        atomic_fetch_add(&n_pads, 1);
        printf("[JOY] pad %d: %s -> ST joystick %d%s\n", i, p->name,
               i == 0 ? 1 : 0, i == 0 ? " (game port), STE pad A"
                                      : " (mouse port), STE pad B");
        return i;
    }
    printf("[JOY] %s ignored: both pad slots in use\n", name ? name : "?");
    return -1;
}

void joy_usb_dev_close(int pad)
{
    if (pad < 0 || pad >= JOY_USB_MAX_PADS || pads[pad].fd < 0)
        return;
    joypad *p = &pads[pad];
    /* release everything it was holding */
    p->btn = 0; p->stick[0] = p->stick[1] = p->hat[0] = p->hat[1] = 0;
    atomic_store(&p->joy, 0);
    atomic_store(&p->pad, 0);
    joy_usb_tick();                        /* flush the release        */
    printf("[JOY] pad %d lost (%s)\n", pad, p->name);
    p->fd = -1;
    atomic_fetch_sub(&n_pads, 1);
}

/* ------------------------------------------------------------------ */
/* events                                                              */
/* ------------------------------------------------------------------ */

static void stick_update(joypad *p, int i, int value)
{
    const int d = value - p->ax[i].centre;
    int s = p->stick[i];
    if (s == 0)
    {
        if (d >=  p->ax[i].on) s =  1;
        else if (d <= -p->ax[i].on) s = -1;
    }
    else if (s > 0)
    {
        if (d < p->ax[i].off) s = (d <= -p->ax[i].on) ? -1 : 0;
    }
    else
    {
        if (d > -p->ax[i].off) s = (d >= p->ax[i].on) ? 1 : 0;
    }
    if (s != p->stick[i]) { p->stick[i] = s; p->dirty = 1; }
}

static void publish(joypad *p)
{
    uint8_t joy = 0, pad = 0;
    const int x = p->hat[0] ? p->hat[0] : p->stick[0];
    const int y = p->hat[1] ? p->hat[1] : p->stick[1];

    if (y < 0 || (p->btn & B_DUP))    joy |= STJOY_UP;
    if (y > 0 || (p->btn & B_DDOWN))  joy |= STJOY_DOWN;
    if (x < 0 || (p->btn & B_DLEFT))  joy |= STJOY_LEFT;
    if (x > 0 || (p->btn & B_DRIGHT)) joy |= STJOY_RIGHT;
    /* opposite directions cancel, as a real stick cannot do both */
    if ((joy & (STJOY_UP | STJOY_DOWN)) == (STJOY_UP | STJOY_DOWN))
        joy &= (uint8_t)~(STJOY_UP | STJOY_DOWN);
    if ((joy & (STJOY_LEFT | STJOY_RIGHT)) == (STJOY_LEFT | STJOY_RIGHT))
        joy &= (uint8_t)~(STJOY_LEFT | STJOY_RIGHT);
    if (p->btn & (B_A | B_TRIG))      joy |= STJOY_FIRE;

    if (p->btn & (B_A | B_TRIG))      pad |= STPAD_A;
    if (p->btn & B_B)                 pad |= STPAD_B;
    if (p->btn & B_X)                 pad |= STPAD_C;
    if (p->btn & B_Y)                 pad |= STPAD_OPTION;
    if (p->btn & B_START)             pad |= STPAD_PAUSE;

    atomic_store_explicit(&p->joy, joy, memory_order_relaxed);
    atomic_store_explicit(&p->pad, pad, memory_order_relaxed);
    p->dirty = 0;
}

void joy_usb_handle_event(int pad, const struct input_event *ev)
{
    if (pad < 0 || pad >= JOY_USB_MAX_PADS)
        return;
    joypad *p = &pads[pad];
    if (p->fd < 0)
        return;

    if (ev->type == EV_ABS)
    {
        switch (ev->code)
        {
            case ABS_X: stick_update(p, 0, ev->value); break;
            case ABS_Y: stick_update(p, 1, ev->value); break;
            case ABS_HAT0X:
                if (p->hat[0] != ev->value) { p->hat[0] = ev->value; p->dirty = 1; }
                break;
            case ABS_HAT0Y:
                if (p->hat[1] != ev->value) { p->hat[1] = ev->value; p->dirty = 1; }
                break;
            default: break;
        }
        return;
    }
    if (ev->type == EV_KEY)
    {
        unsigned bit = 0;
        switch (ev->code)
        {
            case BTN_SOUTH:      bit = B_A;      break;
            case BTN_EAST:       bit = B_B;      break;
            case BTN_NORTH:      bit = B_X;      break;
            case BTN_WEST:       bit = B_Y;      break;
            case BTN_TL:         bit = B_LB;     break;
            case BTN_TR:         bit = B_RB;     break;
            case BTN_START:      bit = B_START;  break;
            case BTN_SELECT:     bit = B_SELECT; break;
            case BTN_DPAD_UP:    bit = B_DUP;    break;
            case BTN_DPAD_DOWN:  bit = B_DDOWN;  break;
            case BTN_DPAD_LEFT:  bit = B_DLEFT;  break;
            case BTN_DPAD_RIGHT: bit = B_DRIGHT; break;
            case BTN_TRIGGER:    bit = B_TRIG;   break;
            default: return;
        }
        if (ev->value == 2)                 /* autorepeat: not for us */
            return;
        const unsigned nb = ev->value ? (p->btn | bit) : (p->btn & ~bit);
        if (nb != p->btn) { p->btn = nb; p->dirty = 1; }
        return;
    }
    if (ev->type == EV_SYN && p->dirty)
        publish(p);
}

/* ------------------------------------------------------------------ */
/* emission (input thread tick)                                        */
/* ------------------------------------------------------------------ */

void joy_usb_resend(void)
{
    atomic_store(&resend_wanted, 1);
}

void joy_usb_tick(void)
{
    const int resend = atomic_exchange(&resend_wanted, 0);

    /* monitoring with no real IKBD to do the reporting: report ourselves
     * at the rate the guest asked for */
    const int mon = atomic_load(&mon_mode);
    if (mon != JOY_MON_OFF && hooks.send_raw && hooks.standalone && hooks.standalone())
    {
        for (int i = 0; i < JOY_USB_MAX_PADS; i++)
            if (pads[i].dirty) publish(&pads[i]);
        const uint64_t now = mon_now_us();
        if (now >= mon_next_us)
        {
            mon_next_us = now + (uint64_t)atomic_load(&mon_rate_cs) * 10000u;
            if (mon == JOY_MON_JOY)
            {
                uint8_t r[2] = { mon_buttons(), mon_dirs() };
                hooks.send_raw(r, 2);
            }
            else
            {
                uint8_t r = mon_fire();
                hooks.send_raw(&r, 1);
            }
        }
        return;                            /* nothing else is reported  */
    }

    for (int i = 0; i < JOY_USB_MAX_PADS; i++)
    {
        joypad *p = &pads[i];
        if (p->fd < 0 && p->last_sent <= 0)
            continue;                      /* free and already released */
        if (p->dirty)
            publish(p);                    /* SYN-less device           */

        const int joy = atomic_load_explicit(&p->joy, memory_order_relaxed);
        const int pad = atomic_load_explicit(&p->pad, memory_order_relaxed);
        const int st_port = (i == 0) ? 1 : 0;
        /* the STE extras (B/C/OPTION/PAUSE) only matter to the STBOX STE
         * tier; the main machine reads them straight from joy_usb_ste_* */
        const int key = joy | (pad << 8);

        if (key != p->last_sent || (resend && joy != 0))
        {
            if (i == 0 && hooks.set_joy_rbutton)
                hooks.set_joy_rbutton((joy & STJOY_FIRE) != 0);
            if (!hooks.send_joy ||
                hooks.send_joy(st_port, (uint8_t)joy, (uint8_t)pad))
                p->last_sent = key;
        }

        /* Start -> Space: many ST games start or pause on the space bar */
        const int start = (p->fd >= 0) && (p->btn & B_START);
        if (start != p->start_sent && hooks.send_key)
        {
            hooks.send_key(0x39, start);
            p->start_sent = start;
        }
        if (p->fd < 0)
            p->last_sent = 0;              /* release flushed, slot idle */
    }
}

/* ------------------------------------------------------------------ */
/* CPU-thread readers                                                  */
/* ------------------------------------------------------------------ */

uint8_t joy_usb_state(int st_port)
{
    const joypad *p = &pads[st_port == 1 ? 0 : 1];
    return atomic_load_explicit(&p->joy, memory_order_relaxed);
}

int joy_usb_count(void)
{
    return atomic_load(&n_pads);
}

/* Real-IKBD packet framing: bytes owed after each header. */
static int ikbd_pkt_len(uint8_t h)
{
    switch (h)
    {
        case 0xF6: return 7;               /* status                   */
        case 0xF7: return 5;               /* absolute mouse           */
        case 0xF8: case 0xF9: case 0xFA: case 0xFB: return 2;
        case 0xFC: return 6;               /* clock                    */
        case 0xFD: return 2;               /* joystick interrogation   */
        case 0xFE: case 0xFF: return 1;    /* joystick event           */
        default:   return 0;               /* key                      */
    }
}

uint8_t joy_usb_real_rx_filter(uint8_t v)
{
    static int left, idx;
    static uint8_t hdr;

    if (!JOY_USB_enabled)
        return v;
    /* monitoring: the real IKBD sends nothing but reports, so the pad is
     * ORed into each one - pairs for $17, single bytes for $18 */
    const int mon = atomic_load(&mon_mode);
    if (mon == JOY_MON_JOY)
    {
        const int i = atomic_fetch_xor(&mon_idx, 1);
        return (uint8_t)(v | (i == 0 ? mon_buttons() : mon_dirs()));
    }
    if (mon == JOY_MON_FIRE)
        return (uint8_t)(v | mon_fire());
    if (left == 0)
    {
        hdr  = v;
        left = ikbd_pkt_len(v);
        idx  = 0;
        return v;
    }
    left--;
    idx++;
    if (hdr == 0xFD)                        /* $FD j0 j1: OR our pads in */
        v |= joy_usb_state(idx == 1 ? 0 : 1);
    return v;
}

/* ------------------------------------------------------------------ */
/* STE enhanced joypad ports                                           */
/*                                                                     */
/* $FF9202 write: column select, active low. Bits 0-3 pad A, 4-7 pad B; */
/* bit 0/4 = directions + fire A + PAUSE, 1/5 = B + keypad row 1,      */
/* 2/6 = C + row 2, 3/7 = OPTION + row 3 (Jaguar pad; Hatari joy.c).  */
/* $FF9200 read, low byte: bit 1 pad A button (of the selected column), */
/* bit 0 pad A PAUSE; bits 3/2 the same for pad B. High byte = Mega STE */
/* DIP switches, left as the bus returned it.                          */
/* $FF9202 read, high byte: low nibble pad A directions (or keypad row) */
/* for the selected column, high nibble pad B. All active low.         */
/* We only ever clear bits, so a real pad on the real port still works.*/
/* ------------------------------------------------------------------ */

static _Atomic uint16_t ste_select;        /* last $FF9202 write       */

static uint8_t pad_nibble(int pad, int sel)      /* sel = 4 column bits */
{
    const joypad *p = &pads[pad];
    if (p->fd < 0 || (sel & 0x0F) == 0x0F)
        return 0x0F;                        /* no column: all high      */
    if (!(sel & 0x01))
        return (uint8_t)(~atomic_load_explicit(&p->joy, memory_order_relaxed) & 0x0F);
    return 0x0F;                            /* keypad rows: nothing     */
}

static uint8_t pad_buttons(int pad, int sel)     /* 2 bits: fire, pause */
{
    const joypad *p = &pads[pad];
    uint8_t v = 0x03;
    if (p->fd < 0 || (sel & 0x0F) == 0x0F)
        return v;
    const uint8_t b = atomic_load_explicit(&p->pad, memory_order_relaxed);
    if (!(sel & 0x01))
    {
        if (b & STPAD_A)      v &= (uint8_t)~0x02;
        if (b & STPAD_PAUSE)  v &= (uint8_t)~0x01;
    }
    else if (!(sel & 0x02)) { if (b & STPAD_B)      v &= (uint8_t)~0x02; }
    else if (!(sel & 0x04)) { if (b & STPAD_C)      v &= (uint8_t)~0x02; }
    else if (!(sel & 0x08)) { if (b & STPAD_OPTION) v &= (uint8_t)~0x02; }
    return v;
}

uint32_t joy_usb_ste_read(uint32_t a, int size, uint32_t real)
{
    if (!JOY_USB_enabled || atomic_load(&n_pads) == 0)
        return real;

    const int sel = atomic_load_explicit(&ste_select, memory_order_relaxed);
    /* our 4-byte window, active-low, 1 = leave the bus bit alone */
    const uint8_t btn  = (uint8_t)(0xF0 | (pad_buttons(1, sel >> 4) << 2)
                                        |  pad_buttons(0, sel));
    const uint8_t dirs = (uint8_t)((pad_nibble(1, sel >> 4) << 4)
                                  | pad_nibble(0, sel));
    const uint32_t mask = 0xFF000000u | ((uint32_t)btn << 16) |
                          ((uint32_t)dirs << 8) | 0xFFu;   /* 9200.b 9201.b 9202.b 9203.b */

    const int off = (int)(a & 3);
    uint32_t m;
    if (size == 4)      m = mask;
    else if (size == 2) m = (mask >> (16 - off * 8)) & 0xFFFFu;
    else                m = (mask >> (24 - off * 8)) & 0xFFu;
    return real & m;
}

void joy_usb_ste_write(uint32_t a, int size, uint32_t v)
{
    if (!JOY_USB_enabled)
        return;
    const int off = (int)(a & 3);
    uint16_t w;
    if (size == 4)      w = (uint16_t)(v & 0xFFFF);          /* 9202 = low word */
    else if (size == 2) { if (off != 2) return; w = (uint16_t)v; }
    else
    {
        if (off != 2 && off != 3) return;
        uint16_t cur = atomic_load(&ste_select);
        w = off == 2 ? (uint16_t)((cur & 0x00FF) | (v << 8))
                     : (uint16_t)((cur & 0xFF00) | (v & 0xFF));
    }
    atomic_store_explicit(&ste_select, w, memory_order_relaxed);
}
