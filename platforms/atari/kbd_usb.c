/*
 * kbd_usb.c - USB/Bluetooth keyboard & mouse -> Atari ST IKBD injection
 *
 * Design (hybrid, real IKBD kept alive):
 *
 *   evdev thread                     CPU thread                ipl_task
 *   ------------                     ----------                --------
 *   scan /dev/input                  hw_bget($FFFC00/02)       poll
 *   keys  -> ST scancodes    ---->   shadow: merge virtual     kbd_usb_irq_wanted()
 *   mouse -> IKBD rel pkts   ring    RDRF/byte with real bus   -> g_irq = 6
 *   hotplug via inotify              writes snooped (IKBD
 *                                    command state machine)
 *
 * Merge rules (both directions protected against packet interleave):
 *   - a new injected packet only starts when the real RX stream has been
 *     quiet for >= 2 byte-times (real IKBD sends nothing when idle);
 *   - once an injected packet has started, its remaining bytes take
 *     priority over real RX until the packet completes;
 *   - injected bytes are paced at the real 7812.5 bps serial rate
 *     (~1.28 ms/byte) so guest handlers written against real timing hold up.
 *
 * Interrupts: the real MFP cannot latch an interrupt for us (its GPIP4 pin
 * is wired to the real ACIA), so the level-6 raise and the vector (0x46)
 * are synthesised host-side - see hooks in emulator.c / jit_glue.cpp.
 * The guest's IERB/IMRB writes are snooped so masking works.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "kbd_usb.h"

bool KBD_USB_enabled = false;

volatile uint32_t kbd_usb_stat_injected_bytes;
volatile uint32_t kbd_usb_stat_virtual_iacks;
volatile uint32_t kbd_usb_stat_dropped_bytes;

/* ------------------------------------------------------------------ */
/* timing                                                              */
/* ------------------------------------------------------------------ */

#define IKBD_BYTE_US      1280      /* 7812.5 bps, 10 bits/byte          */
#define REAL_QUIET_US     2560      /* 2 byte-times: real pkt gap guard  */
#define MOUSE_TICK_US     8000      /* mouse packet generation ~125 Hz   */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

/* ------------------------------------------------------------------ */
/* SPSC ring of IKBD bytes; bit 8 marks "start of packet"              */
/* ------------------------------------------------------------------ */

#define RING_SIZE 1024              /* power of two                      */
#define RING_MASK (RING_SIZE - 1)
#define PKT_START 0x100

static uint16_t         ring[RING_SIZE];
static _Atomic uint32_t ring_head;  /* consumer (CPU thread)             */
static _Atomic uint32_t ring_tail;  /* producer (input thread)           */

/* consumer-side presentation state (CPU thread owns, ipl_task reads)   */
static _Atomic uint64_t next_ready_us;   /* serial pacing gate           */
static _Atomic uint64_t real_quiet_us;   /* no new pkt before this       */
static _Atomic int      in_packet;       /* mid-injected-packet flag     */

/* guest MFP mask shadow: assume keyboard irq enabled until told else   */
static _Atomic uint8_t  mfp_ierb = 0xFF;
static _Atomic uint8_t  mfp_imrb = 0xFF;

static int ring_used(void)
{
    return (int)((atomic_load_explicit(&ring_tail, memory_order_acquire) -
                  atomic_load_explicit(&ring_head, memory_order_acquire)) & RING_MASK);
}

static int ring_free(void)
{
    return RING_SIZE - 1 - ring_used();
}

/* producer only */
static void ring_push_packet(const uint8_t *bytes, int n)
{
    if (ring_free() < n)
    {
        kbd_usb_stat_dropped_bytes += (uint32_t)n;  /* whole packet or none */
        return;
    }
    uint32_t t = atomic_load_explicit(&ring_tail, memory_order_relaxed);
    for (int i = 0; i < n; i++)
        ring[(t + (uint32_t)i) & RING_MASK] = (uint16_t)bytes[i] | (i == 0 ? PKT_START : 0);
    atomic_store_explicit(&ring_tail, t + (uint32_t)n, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* IKBD state machine (snooped from guest command writes)              */
/* ------------------------------------------------------------------ */

enum { MOUSE_REL, MOUSE_ABS, MOUSE_KEYCODE, MOUSE_OFF };

static struct {
    pthread_mutex_t lock;
    int  mouse_mode;
    int  y0_top;             /* 1: y increases downward (TOS default)    */
    int  paused;
    int  joy_event;          /* joystick event reporting on              */
    /* command parameter consumption */
    int  pending_cmd;
    int  pending_params;     /* params still to swallow                  */
    int  memload_left;       /* extra counted bytes for cmd 0x20         */
} ikbd = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .mouse_mode = MOUSE_REL, .y0_top = 1, .paused = 0, .joy_event = 1,
};

/* total parameter bytes following each IKBD command opcode */
static int ikbd_param_len(uint8_t cmd)
{
    switch (cmd)
    {
        case 0x80: return 1;                /* RESET (0x01)              */
        case 0x07: return 1;                /* set mouse button action   */
        case 0x09: return 4;                /* set absolute positioning  */
        case 0x0A: return 2;                /* set keycode mouse         */
        case 0x0B: return 2;                /* set threshold             */
        case 0x0C: return 2;                /* set scale                 */
        case 0x0E: return 5;                /* load mouse position       */
        case 0x17: return 1;                /* joystick monitoring       */
        case 0x19: return 6;                /* keycode joystick          */
        case 0x1B: return 6;                /* set time-of-day           */
        case 0x20: return 3;                /* memory load (+ counted)   */
        case 0x21: return 2;                /* memory read               */
        case 0x22: return 2;                /* controller execute        */
        default:   return 0;
    }
}

static void ikbd_apply(uint8_t cmd)
{
    switch (cmd)
    {
        case 0x08: ikbd.mouse_mode = MOUSE_REL;              break;
        case 0x09: ikbd.mouse_mode = MOUSE_ABS;              break;
        case 0x0A: ikbd.mouse_mode = MOUSE_KEYCODE;          break;
        case 0x12: ikbd.mouse_mode = MOUSE_OFF;              break;
        case 0x0F: ikbd.y0_top = 0;                          break;
        case 0x10: ikbd.y0_top = 1;                          break;
        case 0x11: ikbd.paused = 0;                          break;
        case 0x13: ikbd.paused = 1;                          break;
        case 0x14: ikbd.joy_event = 1;                       break;
        case 0x15: case 0x1A: ikbd.joy_event = 0;            break;
        default: break;
    }
}

static void ikbd_reset_state(void)
{
    ikbd.mouse_mode = MOUSE_REL;
    ikbd.y0_top = 1;
    ikbd.paused = 0;
    ikbd.joy_event = 1;
    ikbd.pending_cmd = 0;
    ikbd.pending_params = 0;
    ikbd.memload_left = 0;
    /* flush anything queued under the old mode */
    atomic_store(&ring_head, atomic_load(&ring_tail));
    atomic_store(&in_packet, 0);
}

void kbd_usb_tx_snoop(uint8_t v)
{
    if (!KBD_USB_enabled)
        return;

    pthread_mutex_lock(&ikbd.lock);

    if (ikbd.memload_left > 0)
    {
        ikbd.memload_left--;
    }
    else if (ikbd.pending_params > 0)
    {
        ikbd.pending_params--;
        if (ikbd.pending_cmd == 0x80 && v == 0x01)
            ikbd_reset_state();
        if (ikbd.pending_cmd == 0x20 && ikbd.pending_params == 0)
            ikbd.memload_left = v;          /* 3rd param = byte count    */
        if (ikbd.pending_params == 0 && ikbd.pending_cmd != 0x20)
            ikbd.pending_cmd = 0;
    }
    else
    {
        int n = ikbd_param_len(v);
        if (n > 0)
        {
            ikbd.pending_cmd = v;
            ikbd.pending_params = n;
        }
        else
        {
            ikbd_apply(v);
        }
    }

    pthread_mutex_unlock(&ikbd.lock);
}

void kbd_usb_ctrl_snoop(uint8_t v)
{
    if (!KBD_USB_enabled)
        return;
    if ((v & 0x03) == 0x03)                 /* 6850 master reset         */
    {
        pthread_mutex_lock(&ikbd.lock);
        atomic_store(&ring_head, atomic_load(&ring_tail));
        atomic_store(&in_packet, 0);
        pthread_mutex_unlock(&ikbd.lock);
    }
}

void kbd_usb_mfp_snoop(uint32_t addr, uint32_t value, int is_word)
{
    if (!KBD_USB_enabled)
        return;
    uint32_t a = addr & 0x00FFFFFFu;
    uint8_t  b = (uint8_t)(value & 0xFF);   /* low byte lands on odd addr */
    if (is_word)
        a |= 1;
    if (a == 0x00FFFA09u)                   /* IERB */
        atomic_store_explicit(&mfp_ierb, b, memory_order_relaxed);
    else if (a == 0x00FFFA15u)              /* IMRB */
        atomic_store_explicit(&mfp_imrb, b, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* consumer side (CPU thread) + observer (ipl_task)                    */
/* ------------------------------------------------------------------ */

int kbd_usb_rx_priority(void)
{
    return KBD_USB_enabled &&
           atomic_load_explicit(&in_packet, memory_order_relaxed) &&
           ring_used() > 0;
}

int kbd_usb_rx_ready(void)
{
    if (!KBD_USB_enabled || ring_used() == 0)
        return 0;

    uint64_t t = now_us();
    if (t < atomic_load_explicit(&next_ready_us, memory_order_relaxed))
        return 0;                            /* serial pacing             */

    if (!atomic_load_explicit(&in_packet, memory_order_relaxed))
    {
        /* starting a new packet: require the real stream to be quiet    */
        if (t < atomic_load_explicit(&real_quiet_us, memory_order_relaxed))
            return 0;
    }
    return 1;
}

uint8_t kbd_usb_rx_read(void)
{
    uint32_t h = atomic_load_explicit(&ring_head, memory_order_relaxed);
    if (((atomic_load_explicit(&ring_tail, memory_order_acquire) - h) & RING_MASK) == 0)
        return 0;                            /* defensive: empty          */

    uint16_t e = ring[h & RING_MASK];
    atomic_store_explicit(&ring_head, h + 1, memory_order_release);

    /* peek: are we now mid-packet? (next entry exists, not pkt start)   */
    uint32_t t2 = atomic_load_explicit(&ring_tail, memory_order_acquire);
    int mid = ((t2 - (h + 1)) & RING_MASK) != 0 &&
              !(ring[(h + 1) & RING_MASK] & PKT_START);
    atomic_store_explicit(&in_packet, mid, memory_order_relaxed);

    atomic_store_explicit(&next_ready_us, now_us() + IKBD_BYTE_US,
                          memory_order_relaxed);
    kbd_usb_stat_injected_bytes++;
    return (uint8_t)e;
}

void kbd_usb_note_real_rx(void)
{
    if (!KBD_USB_enabled)
        return;
    atomic_store_explicit(&real_quiet_us, now_us() + REAL_QUIET_US,
                          memory_order_relaxed);
}

/* ---- shared ACIA/GPIP shims (used by pistorm_natmem.cpp and the ---- */
/* ---- legacy emulator.c memory handlers)                          ---- */

extern uint8_t ps_read_8(uint32_t address);   /* gpio/ps_protocol.h      */

#define KBD_ACIA_CTRL_ADDR 0x00FFFC00u
#define KBD_ACIA_DATA_ADDR 0x00FFFC02u

uint8_t kbd_usb_acia_status_shim(uint8_t real)
{
    if (kbd_usb_rx_priority() || (!(real & 0x01) && kbd_usb_rx_ready()))
        real |= 0x81;                        /* RDRF | IRQ                */
    return real;
}

uint8_t kbd_usb_acia_data_shim(void)
{
    if (kbd_usb_rx_priority())
        return kbd_usb_rx_read();

    /* fresh real status decides whose byte the guest gets */
    uint8_t rs = ps_read_8(KBD_ACIA_CTRL_ADDR);
    if (rs & 0x01)
    {
        uint8_t v = ps_read_8(KBD_ACIA_DATA_ADDR);
        kbd_usb_note_real_rx();
        return v;
    }
    if (kbd_usb_rx_ready())
        return kbd_usb_rx_read();
    return ps_read_8(KBD_ACIA_DATA_ADDR);
}

uint8_t kbd_usb_gpip_shim(uint8_t real)
{
    if (kbd_usb_rx_ready() || kbd_usb_rx_priority())
        real &= (uint8_t)~0x10;              /* GPIP4 low (active low)    */
    return real;
}

int kbd_usb_irq_wanted(void)
{
    if (!kbd_usb_rx_ready() && !kbd_usb_rx_priority())
        return 0;
    /* respect the guest's MFP keyboard interrupt mask (GPIP4 = bit 6)   */
    uint8_t en = atomic_load_explicit(&mfp_ierb, memory_order_relaxed) &
                 atomic_load_explicit(&mfp_imrb, memory_order_relaxed);
    return (en & 0x40) != 0;
}

/* ------------------------------------------------------------------ */
/* Linux keycode -> Atari ST scancode                                  */
/* ------------------------------------------------------------------ */

#define ST_NONE 0x00

static const uint8_t st_scan[KEY_MAX + 1] = {
    [KEY_ESC]        = 0x01,
    [KEY_1] = 0x02, [KEY_2] = 0x03, [KEY_3] = 0x04, [KEY_4] = 0x05,
    [KEY_5] = 0x06, [KEY_6] = 0x07, [KEY_7] = 0x08, [KEY_8] = 0x09,
    [KEY_9] = 0x0A, [KEY_0] = 0x0B,
    [KEY_MINUS]      = 0x0C,
    [KEY_EQUAL]      = 0x0D,
    [KEY_BACKSPACE]  = 0x0E,
    [KEY_TAB]        = 0x0F,
    [KEY_Q] = 0x10, [KEY_W] = 0x11, [KEY_E] = 0x12, [KEY_R] = 0x13,
    [KEY_T] = 0x14, [KEY_Y] = 0x15, [KEY_U] = 0x16, [KEY_I] = 0x17,
    [KEY_O] = 0x18, [KEY_P] = 0x19,
    [KEY_LEFTBRACE]  = 0x1A,
    [KEY_RIGHTBRACE] = 0x1B,
    [KEY_ENTER]      = 0x1C,
    [KEY_LEFTCTRL]   = 0x1D,
    [KEY_A] = 0x1E, [KEY_S] = 0x1F, [KEY_D] = 0x20, [KEY_F] = 0x21,
    [KEY_G] = 0x22, [KEY_H] = 0x23, [KEY_J] = 0x24, [KEY_K] = 0x25,
    [KEY_L] = 0x26,
    [KEY_SEMICOLON]  = 0x27,
    [KEY_APOSTROPHE] = 0x28,
    [KEY_GRAVE]      = 0x29,
    [KEY_LEFTSHIFT]  = 0x2A,
    [KEY_BACKSLASH]  = 0x2B,
    [KEY_Z] = 0x2C, [KEY_X] = 0x2D, [KEY_C] = 0x2E, [KEY_V] = 0x2F,
    [KEY_B] = 0x30, [KEY_N] = 0x31, [KEY_M] = 0x32,
    [KEY_COMMA]      = 0x33,
    [KEY_DOT]        = 0x34,
    [KEY_SLASH]      = 0x35,
    [KEY_RIGHTSHIFT] = 0x36,
    [KEY_KPASTERISK] = 0x66,
    [KEY_LEFTALT]    = 0x38,
    [KEY_SPACE]      = 0x39,
    [KEY_CAPSLOCK]   = 0x3A,
    [KEY_F1] = 0x3B, [KEY_F2] = 0x3C, [KEY_F3]  = 0x3D, [KEY_F4]  = 0x3E,
    [KEY_F5] = 0x3F, [KEY_F6] = 0x40, [KEY_F7]  = 0x41, [KEY_F8]  = 0x42,
    [KEY_F9] = 0x43, [KEY_F10] = 0x44,
    [KEY_NUMLOCK]    = 0x63,     /* ST keypad '('                       */
    [KEY_SCROLLLOCK] = 0x64,     /* ST keypad ')'                       */
    [KEY_KP7] = 0x67, [KEY_KP8] = 0x68, [KEY_KP9] = 0x69,
    [KEY_KPMINUS]    = 0x4A,
    [KEY_KP4] = 0x6A, [KEY_KP5] = 0x6B, [KEY_KP6] = 0x6C,
    [KEY_KPPLUS]     = 0x4E,
    [KEY_KP1] = 0x6D, [KEY_KP2] = 0x6E, [KEY_KP3] = 0x6F,
    [KEY_KP0]        = 0x70,
    [KEY_KPDOT]      = 0x71,
    [KEY_102ND]      = 0x60,     /* ISO <> key                          */
    [KEY_KPENTER]    = 0x72,
    [KEY_RIGHTCTRL]  = 0x1D,     /* ST has one Control                  */
    [KEY_KPSLASH]    = 0x65,
    [KEY_RIGHTALT]   = 0x38,     /* ST has one Alternate                */
    [KEY_HOME]       = 0x47,     /* ClrHome                             */
    [KEY_UP]         = 0x48,
    [KEY_PAGEUP]     = 0x62,     /* -> HELP (no PgUp on ST)             */
    [KEY_LEFT]       = 0x4B,
    [KEY_RIGHT]      = 0x4D,
    [KEY_END]        = 0x61,     /* -> UNDO (no End on ST)              */
    [KEY_DOWN]       = 0x50,
    [KEY_PAGEDOWN]   = 0x61,     /* -> UNDO                             */
    [KEY_INSERT]     = 0x52,
    [KEY_DELETE]     = 0x53,
};

/* ------------------------------------------------------------------ */
/* evdev input thread                                                  */
/* ------------------------------------------------------------------ */

#define MAX_DEVS 16

typedef struct {
    int  fd;
    int  is_mouse;
    char node[64];
} in_dev;

static struct {
    pthread_t tid;
    _Atomic int running;
    int grab_wanted;
    _Atomic int grab_active;
    in_dev dev[MAX_DEVS];
    int ndev;
    int ifd;                    /* inotify on /dev/input                 */
    /* accumulated mouse state (thread-local use only) */
    int dx, dy;
    int buttons;                /* bit1 = left, bit0 = right (IKBD)      */
    int last_sent_buttons;
    uint64_t next_mouse_us;
} in_state;

static int has_bit(const unsigned long *bits, int bit)
{
    return (bits[bit / (8 * (int)sizeof(long))] >>
            (bit % (8 * (int)sizeof(long)))) & 1;
}

static void dev_try_open(const char *name)
{
    if (strncmp(name, "event", 5) != 0 || in_state.ndev >= MAX_DEVS)
        return;

    char path[64];
    snprintf(path, sizeof path, "/dev/input/%s", name);

    for (int i = 0; i < in_state.ndev; i++)
        if (!strcmp(in_state.dev[i].node, path))
            return;                          /* already open              */

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return;

    unsigned long evbits[(EV_MAX / (8 * sizeof(long))) + 1];
    unsigned long keybits[(KEY_MAX / (8 * sizeof(long))) + 1];
    unsigned long relbits[(REL_MAX / (8 * sizeof(long))) + 1];
    memset(evbits, 0, sizeof evbits);
    memset(keybits, 0, sizeof keybits);
    memset(relbits, 0, sizeof relbits);
    ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits);

    int is_kbd = 0, is_mouse = 0;
    if (has_bit(evbits, EV_KEY))
    {
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
        if (has_bit(keybits, KEY_A) && has_bit(keybits, KEY_Z))
            is_kbd = 1;
        if (has_bit(keybits, BTN_LEFT))
            is_mouse = 1;
    }
    if (has_bit(evbits, EV_REL))
    {
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbits), relbits);
        if (has_bit(relbits, REL_X) && has_bit(relbits, REL_Y) && is_mouse)
            is_mouse = 1;
        else if (!is_kbd)
            is_mouse = 0;
    }
    else
    {
        is_mouse = 0;
    }

    if (!is_kbd && !is_mouse)
    {
        close(fd);
        return;
    }

    if (in_state.grab_wanted && atomic_load(&in_state.grab_active))
        ioctl(fd, EVIOCGRAB, (void *)1);

    in_dev *d = &in_state.dev[in_state.ndev++];
    d->fd = fd;
    d->is_mouse = is_mouse;
    snprintf(d->node, sizeof d->node, "%s", path);

    char dname[64] = "?";
    ioctl(fd, EVIOCGNAME(sizeof dname), dname);
    printf("[KBD] using %s (%s)%s\n", path, dname,
           is_mouse ? " [mouse]" : " [keyboard]");
}

static void dev_scan_all(void)
{
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return;
    struct dirent *e;
    while ((e = readdir(dir)))
        dev_try_open(e->d_name);
    closedir(dir);
}

static void dev_close(int idx)
{
    printf("[KBD] lost %s\n", in_state.dev[idx].node);
    close(in_state.dev[idx].fd);
    in_state.dev[idx] = in_state.dev[--in_state.ndev];
}

static void grab_set(int on)
{
    atomic_store(&in_state.grab_active, on);
    for (int i = 0; i < in_state.ndev; i++)
        ioctl(in_state.dev[i].fd, EVIOCGRAB, (void *)(long)(on ? 1 : 0));
    printf("[KBD] input grab %s (F12 toggles)\n", on ? "ON" : "OFF");
}

static void send_key(uint8_t scan, int pressed)
{
    pthread_mutex_lock(&ikbd.lock);
    int ok = !ikbd.paused;
    pthread_mutex_unlock(&ikbd.lock);
    if (!ok)
        return;
    uint8_t b = pressed ? scan : (uint8_t)(scan | 0x80);
    ring_push_packet(&b, 1);
}

static void mouse_flush(void)
{
    pthread_mutex_lock(&ikbd.lock);
    int mode   = ikbd.mouse_mode;
    int y0top  = ikbd.y0_top;
    int paused = ikbd.paused;
    pthread_mutex_unlock(&ikbd.lock);

    if (paused || mode != MOUSE_REL)
    {
        /* not injectable in abs/keycode/off modes - drop deltas so they
         * don't burst out when relative mode returns */
        in_state.dx = in_state.dy = 0;
        in_state.last_sent_buttons = in_state.buttons;
        return;
    }

    while (in_state.dx || in_state.dy ||
           in_state.buttons != in_state.last_sent_buttons)
    {
        int sx = in_state.dx, sy = in_state.dy;
        if (sx > 127)  sx = 127;
        if (sx < -128) sx = -128;
        if (sy > 127)  sy = 127;
        if (sy < -128) sy = -128;
        in_state.dx -= sx;
        in_state.dy -= sy;
        in_state.last_sent_buttons = in_state.buttons;

        if (!y0top)
            sy = -sy;

        uint8_t pkt[3];
        pkt[0] = (uint8_t)(0xF8 | (in_state.buttons & 0x03));
        pkt[1] = (uint8_t)(int8_t)sx;
        pkt[2] = (uint8_t)(int8_t)sy;
        ring_push_packet(pkt, 3);

        if (ring_free() < 8)
            break;
    }
}

static void handle_event(const struct input_event *ev, int is_mouse)
{
    if (ev->type == EV_REL)
    {
        if (ev->code == REL_X)
            in_state.dx += ev->value;
        else if (ev->code == REL_Y)
            in_state.dy += ev->value;
        else if (ev->code == REL_WHEEL && ev->value != 0)
        {
            /* wheel -> Up/Down arrow taps (Eiffel convention) */
            uint8_t scan = ev->value > 0 ? 0x48 : 0x50;
            int n = ev->value > 0 ? ev->value : -ev->value;
            if (n > 4) n = 4;
            for (int i = 0; i < n; i++)
            {
                send_key(scan, 1);
                send_key(scan, 0);
            }
        }
        return;
    }

    if (ev->type != EV_KEY || ev->value == 2)   /* ignore autorepeat:    */
        return;                                  /* TOS/games do their own */

    int pressed = ev->value == 1;

    if (ev->code == BTN_LEFT)
    {
        in_state.buttons = pressed ? (in_state.buttons | 0x02)
                                   : (in_state.buttons & ~0x02);
        return;
    }
    if (ev->code == BTN_RIGHT)
    {
        in_state.buttons = pressed ? (in_state.buttons | 0x01)
                                   : (in_state.buttons & ~0x01);
        return;
    }
    if (ev->code == BTN_MIDDLE)
        return;

    if (ev->code == KEY_F12)                     /* grab toggle           */
    {
        if (pressed && in_state.grab_wanted)
            grab_set(!atomic_load(&in_state.grab_active));
        return;
    }

    if (ev->code > KEY_MAX)
        return;
    uint8_t scan = st_scan[ev->code];
    if (scan == ST_NONE)
        return;
    (void)is_mouse;
    send_key(scan, pressed);
}

static void *input_thread(void *arg)
{
    (void)arg;

    in_state.ifd = inotify_init1(IN_NONBLOCK);
    if (in_state.ifd >= 0)
        inotify_add_watch(in_state.ifd, "/dev/input", IN_CREATE | IN_ATTRIB);

    dev_scan_all();
    if (in_state.ndev == 0)
        printf("[KBD] no input devices yet - waiting for hotplug "
               "(pair Bluetooth devices from the Pi as usual; they appear "
               "as evdev nodes)\n");

    in_state.next_mouse_us = now_us() + MOUSE_TICK_US;

    while (atomic_load(&in_state.running))
    {
        struct pollfd pfd[MAX_DEVS + 1];
        int n = 0;
        for (int i = 0; i < in_state.ndev; i++)
        {
            pfd[n].fd = in_state.dev[i].fd;
            pfd[n].events = POLLIN;
            n++;
        }
        pfd[n].fd = in_state.ifd;
        pfd[n].events = POLLIN;
        n++;

        (void)poll(pfd, (nfds_t)n, 4 /* ms */);

        /* hotplug */
        if (in_state.ifd >= 0 && (pfd[n - 1].revents & POLLIN))
        {
            char buf[512];
            ssize_t len = read(in_state.ifd, buf, sizeof buf);
            for (char *p = buf; len > 0 && p < buf + len; )
            {
                struct inotify_event *ie = (struct inotify_event *)p;
                if (ie->len)
                    dev_try_open(ie->name);
                p += sizeof(struct inotify_event) + ie->len;
            }
        }

        /* device events */
        for (int i = 0; i < in_state.ndev; i++)
        {
            struct input_event ev;
            for (;;)
            {
                ssize_t r = read(in_state.dev[i].fd, &ev, sizeof ev);
                if (r == (ssize_t)sizeof ev)
                {
                    handle_event(&ev, in_state.dev[i].is_mouse);
                }
                else if (r < 0 && errno == EAGAIN)
                {
                    break;
                }
                else
                {
                    dev_close(i);
                    i--;
                    break;
                }
            }
        }

        /* periodic mouse packet generation */
        uint64_t t = now_us();
        if (t >= in_state.next_mouse_us)
        {
            mouse_flush();
            in_state.next_mouse_us = t + MOUSE_TICK_US;
        }
    }

    for (int i = 0; i < in_state.ndev; i++)
        close(in_state.dev[i].fd);
    if (in_state.ifd >= 0)
        close(in_state.ifd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

int kbd_usb_init(int grab)
{
    if (atomic_load(&in_state.running))
        return 0;

    in_state.grab_wanted = grab;
    atomic_store(&in_state.grab_active, grab);
    atomic_store(&in_state.running, 1);

    if (pthread_create(&in_state.tid, NULL, input_thread, NULL))
    {
        atomic_store(&in_state.running, 0);
        fprintf(stderr, "[KBD] input thread create failed\n");
        return -1;
    }
    printf("[KBD] USB/Bluetooth IKBD injection enabled%s\n",
           grab ? " (devices grabbed, F12 releases)" : "");
    return 0;
}

void kbd_usb_shutdown(void)
{
    if (!atomic_load(&in_state.running))
        return;
    atomic_store(&in_state.running, 0);
    pthread_join(in_state.tid, NULL);
}
