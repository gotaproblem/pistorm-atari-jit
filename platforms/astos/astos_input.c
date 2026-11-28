/*
 * astos_input.c - see astos_input.h.
 *
 * The ST side differs from the setup page in one way: the IKBD is put in
 * relative mouse mode (command $08) instead of having the mouse switched
 * off, so the byte stream carries mouse packets - a header $F8-$FB
 * (bit 1 = left button, bit 0 = right) and two signed deltas - between
 * the key codes. Other IKBD reports are skipped by their length.
 *
 * The 6850 holds ONE received byte and the IKBD sends one every 1.28 ms
 * (7812.5 baud), three per mouse packet. A byte not read in time is
 * overwritten (overrun), and a lost packet header turns the deltas that
 * follow into key codes - a delta of $01 is Esc. So the ACIA is emptied
 * by its own thread every 200 us into a ring, whatever the main loop is
 * doing (drawing, flushing the screen), and parsed from there. The bus
 * accessors take the transaction lock for non-CPU threads, so the reader
 * and the drawing interleave safely.
 */
#include <dirent.h>
#include <errno.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "astos_input.h"
#include "gpio/ps_protocol.h"

#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP 0x220
#endif

#define ACIA_CTRL 0x00FFFC00u
#define ACIA_DATA 0x00FFFC02u
#define ACIA_RDRF 0x01
#define ACIA_TDRE 0x02
#define ACIA_ERRS 0x70

#define MAX_DEV 16

static struct {
    int fd, is_pad, is_kbd, is_mouse;
    char node[16];                      /* "event5"                         */
    int dbg_n;                          /* raw events logged so far         */
    struct { int centre, on, off; } ax[2];
    int stick[2];
} dev[MAX_DEV];
static int ndev, n_kbd, n_mouse, n_pad, have_st, grabbed;
static int in_debug;                   /* PISTORM_ASTOS_INPUT_DEBUG=1      */
static int ino_fd = -1;                /* /dev/input watched: hotplug      */
static int shift_usb, shift_st;
static int usb_buttons;
static struct ai_event pend_key;              /* held back one poll */
static int have_pend;

static struct ai_event none(void)
{
    struct ai_event e;
    memset(&e, 0, sizeof e);
    return e;
}

static struct ai_event key_ev(struct si_event k)
{
    struct ai_event e = none();
    e.type = AI_KEY;
    e.key = k;
    return e;
}

/* ------------------------------------------------------------------ */
/* the ST: ACIA + IKBD                                                 */
/* ------------------------------------------------------------------ */

static void acia_tx(uint8_t v)
{
    for (int i = 0; i < 2000; i++) {
        if (ps_read_8(ACIA_CTRL) & ACIA_TDRE)
            break;
        usleep(10);
    }
    ps_write_8(ACIA_DATA, v);
    usleep(1500);
}

static int st_open(void)
{
    ps_write_8(ACIA_CTRL, 0x03);              /* master reset           */
    usleep(1000);
    ps_write_8(ACIA_CTRL, 0x96);              /* /64, 8N1, RTS low, RIE */
    usleep(1000);
    acia_tx(0x80); acia_tx(0x01);             /* IKBD reset             */
    usleep(300000);
    acia_tx(0x08);                            /* relative mouse         */
    acia_tx(0x1A);                            /* joysticks off          */
    for (int i = 0; i < 64; i++) {            /* drain the reset answer */
        if (!(ps_read_8(ACIA_CTRL) & ACIA_RDRF))
            break;
        ps_read_8(ACIA_DATA);
        usleep(200);
    }
    return 1;
}

/* ---- the reader thread: ACIA -> ring ---- */
#define RING 1024                               /* power of two         */
static uint16_t ring[RING];                     /* byte | 0x100 = error */
static atomic_uint ring_w, ring_r;
static pthread_t reader;
static atomic_int reader_run;
static unsigned long acia_errors;

static void *acia_reader(void *arg)
{
    (void)arg;
    while (atomic_load(&reader_run)) {
        uint8_t sr = ps_read_8(ACIA_CTRL);
        if (sr & ACIA_RDRF) {
            uint8_t b = ps_read_8(ACIA_DATA);
            unsigned w = atomic_load(&ring_w);
            if (w - atomic_load(&ring_r) < RING) {
                ring[w & (RING - 1)] = (uint16_t)(b | ((sr & ACIA_ERRS) ? 0x100 : 0));
                atomic_store(&ring_w, w + 1);
            }
            continue;                             /* the next may be close */
        }
        usleep(200);
    }
    return NULL;
}

static int ring_get(void)
{
    unsigned r = atomic_load(&ring_r);
    if (r == atomic_load(&ring_w))
        return -1;
    int v = ring[r & (RING - 1)];
    atomic_store(&ring_r, r + 1);
    return v;
}

static int pk_need, pk_have, pk_kind;          /* IKBD packet in progress */
static uint8_t pk[8];

/* one parsed report from the ring, or AI_NONE when it is empty */
static struct ai_event st_poll(void)
{
    int v = ring_get();
    if (v < 0)
        return none();
    uint8_t b = (uint8_t)v;
    if (v & 0x100) {
        acia_errors++;
        pk_need = 0;                          /* resync on the next header */
        return none();
    }

    if (pk_need) {                            /* inside a packet */
        pk[pk_have++] = b;
        if (pk_have < pk_need)
            return none();
        pk_need = 0;
        if (pk_kind == 1) {                   /* relative mouse */
            struct ai_event e = none();
            e.type = AI_MOUSE;
            e.dx = (int8_t)pk[1];
            e.dy = (int8_t)pk[2];
            e.buttons = ((pk[0] & 2) ? 1 : 0) | ((pk[0] & 1) ? 2 : 0);
            return e;
        }
        return none();
    }

    if (b >= 0xF6) {                          /* a report header */
        pk[0] = b; pk_have = 1;
        if (b >= 0xF8 && b <= 0xFB) { pk_need = 3; pk_kind = 1; }
        else if (b == 0xF6)         { pk_need = 8; pk_kind = 0; }
        else if (b == 0xF7)         { pk_need = 6; pk_kind = 0; }
        else if (b == 0xFC)         { pk_need = 7; pk_kind = 0; }
        else if (b == 0xFD)         { pk_need = 3; pk_kind = 0; }
        else                        { pk_need = 2; pk_kind = 0; } /* FE/FF */
        return none();
    }

    int release = b & 0x80;
    uint8_t sc = b & 0x7F;
    if (sc == 0x2A || sc == 0x36) {
        shift_st = !release;
        return none();
    }
    if (release)
        return none();
    struct si_event k = si_map_st(sc, shift_st);
    return k.key == SI_NONE ? none() : key_ev(k);
}

/* ------------------------------------------------------------------ */
/* USB / Bluetooth                                                     */
/* ------------------------------------------------------------------ */

static int has_bit(const unsigned long *b, int n)
{
    return (b[n / (8 * (int)sizeof(long))] >> (n % (8 * (int)sizeof(long)))) & 1;
}

/* a keyboard, mouse or pad at /dev/input/<node>; once only. Devices come
 * and go: a Bluetooth keyboard or mouse connects seconds after boot and
 * again after each of its sleeps, as a new event node, and USB ones can be
 * plugged in at any time - so /dev/input is watched (inotify) and a node
 * that stops reading (ENODEV) is let go. */
static void try_dev(const char *node, int grab)
{
    if (ndev >= MAX_DEV)
        return;
    for (int i = 0; i < ndev; i++)
        if (!strcmp(dev[i].node, node))
            return;
    char path[48];
    snprintf(path, sizeof path, "/dev/input/%.30s", node);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return;
    unsigned long evb[EV_MAX / (8 * sizeof(long)) + 1];
    unsigned long keyb[KEY_MAX / (8 * sizeof(long)) + 1];
    unsigned long absb[ABS_MAX / (8 * sizeof(long)) + 1];
    unsigned long relb[REL_MAX / (8 * sizeof(long)) + 1];
    memset(evb, 0, sizeof evb); memset(keyb, 0, sizeof keyb);
    memset(absb, 0, sizeof absb); memset(relb, 0, sizeof relb);
    ioctl(fd, EVIOCGBIT(0, sizeof evb), evb);
    if (has_bit(evb, EV_KEY)) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keyb), keyb);
    if (has_bit(evb, EV_ABS)) ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), absb);
    if (has_bit(evb, EV_REL)) ioctl(fd, EVIOCGBIT(EV_REL, sizeof relb), relb);

    int pad = has_bit(evb, EV_KEY) &&
              (has_bit(keyb, BTN_GAMEPAD) || has_bit(keyb, BTN_JOYSTICK) ||
               has_bit(keyb, BTN_DPAD_UP) || has_bit(keyb, BTN_THUMB)) &&
              (has_bit(absb, ABS_X) || has_bit(absb, ABS_HAT0X) ||
               has_bit(keyb, BTN_DPAD_UP));
    int kbd = !pad && has_bit(keyb, KEY_A) && has_bit(keyb, KEY_Z);
    int mouse = !pad && has_bit(evb, EV_REL) && has_bit(relb, REL_X) &&
                has_bit(relb, REL_Y) && has_bit(keyb, BTN_LEFT);
    if (!pad && !kbd && !mouse) {
        if (in_debug) {
            char nm[80] = "?";
            ioctl(fd, EVIOCGNAME(sizeof nm), nm);
            printf("[ASTOS] input: skip %s \"%s\" ev %lx rel %lx abs %lx BTN_LEFT %d\n", node, nm,
                   evb[0], relb[0], absb[0], has_bit(keyb, BTN_LEFT));
        }
        close(fd);
        return;
    }
    if (grab)
        ioctl(fd, EVIOCGRAB, (void *)1);
    int i = ndev++;
    memset(&dev[i], 0, sizeof dev[i]);
    dev[i].fd = fd;
    dev[i].is_pad = pad;
    dev[i].is_kbd = kbd;
    dev[i].is_mouse = mouse;
    snprintf(dev[i].node, sizeof dev[i].node, "%.15s", node);
    n_pad += pad; n_kbd += kbd; n_mouse += mouse;
    char name[80] = "?";
    struct input_id id;
    memset(&id, 0, sizeof id);
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    ioctl(fd, EVIOCGID, &id);
    if (in_debug)
        printf("[ASTOS] input: %s bits ev %lx rel %lx abs %lx key(BTN_LEFT %d BTN_MOUSE.. %d KEY_A %d BTN_JOYSTICK %d BTN_GAMEPAD %d BTN_THUMB %d)\n",
               node, evb[0], relb[0], absb[0], has_bit(keyb, BTN_LEFT), has_bit(keyb, BTN_MOUSE),
               has_bit(keyb, KEY_A), has_bit(keyb, BTN_JOYSTICK), has_bit(keyb, BTN_GAMEPAD),
               has_bit(keyb, BTN_THUMB));
    printf("[ASTOS] input: + %s \"%s\" (%s) %s\n", node, name,
           id.bustype == BUS_BLUETOOTH ? "bluetooth" : id.bustype == BUS_USB ? "usb" : "other",
           pad ? "pad" : kbd && mouse ? "keyboard+mouse" : kbd ? "keyboard" : "mouse");
    if (pad) {
        static const int code[2] = { ABS_X, ABS_Y };
        for (int a = 0; a < 2; a++) {
            struct input_absinfo ai;
            memset(&ai, 0, sizeof ai);
            if (ioctl(fd, EVIOCGABS(code[a]), &ai) < 0 || ai.maximum <= ai.minimum) {
                ai.minimum = -32768;
                ai.maximum = 32767;
            }
            int half = (ai.maximum - ai.minimum) / 2;
            dev[i].ax[a].centre = ai.minimum + half;
            dev[i].ax[a].on = half / 2;
            dev[i].ax[a].off = (half * 35) / 100;
        }
    }
}

int ai_open(int want_st, int grab)
{
    ndev = n_kbd = n_mouse = n_pad = have_st = 0;
    grabbed = grab;
    /* the watch first, so nothing that appears during the scan is missed */
    const char *dbg = getenv("PISTORM_ASTOS_INPUT_DEBUG");
    in_debug = dbg && *dbg && *dbg != '0';
    ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino_fd >= 0 && inotify_add_watch(ino_fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        close(ino_fd);
        ino_fd = -1;
    }
    DIR *d = opendir("/dev/input");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (!strncmp(e->d_name, "event", 5))
                try_dev(e->d_name, grab);
        closedir(d);
    }
    if (want_st) {
        have_st = st_open();
        atomic_store(&ring_r, 0);
        atomic_store(&ring_w, 0);
        atomic_store(&reader_run, 1);
        if (pthread_create(&reader, NULL, acia_reader, NULL) != 0) {
            atomic_store(&reader_run, 0);
            have_st = 0;
        }
    }
    return ndev + have_st;
}

void ai_close(void)
{
    if (atomic_load(&reader_run)) {
        atomic_store(&reader_run, 0);
        pthread_join(reader, NULL);
    }
    if (acia_errors)
        printf("[ASTOS] ST keyboard/mouse: %lu ACIA receive error(s)\n", acia_errors);
    for (int i = 0; i < ndev; i++) {
        if (grabbed)
            ioctl(dev[i].fd, EVIOCGRAB, (void *)0);
        close(dev[i].fd);
    }
    ndev = 0;
    if (ino_fd >= 0) {
        close(ino_fd);
        ino_fd = -1;
    }
}

int ai_have_st(void)   { return have_st; }
int ai_keyboards(void) { return n_kbd; }
int ai_mice(void)      { return n_mouse; }
int ai_pads(void)      { return n_pad; }

static void drop_dev(int i)
{
    printf("[ASTOS] input: - %s\n", dev[i].node);
    close(dev[i].fd);
    n_pad -= dev[i].is_pad; n_kbd -= dev[i].is_kbd; n_mouse -= dev[i].is_mouse;
    dev[i] = dev[--ndev];
}

static void hotplug(void)
{
    if (ino_fd < 0)
        return;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n;
    while ((n = read(ino_fd, buf, sizeof buf)) > 0)
        for (char *p = buf; p < buf + n; ) {
            const struct inotify_event *ie = (const struct inotify_event *)p;
            if (ie->len && !strncmp(ie->name, "event", 5))
                try_dev(ie->name, grabbed);
            p += sizeof *ie + ie->len;
        }
}

static struct ai_event usb_poll(void)
{
    struct input_event ie;
    hotplug();
    for (int i = 0; i < ndev; i++) {
        int mdx = 0, mdy = 0, mwheel = 0, mouse_changed = 0;
        for (;;) {
            ssize_t got = read(dev[i].fd, &ie, sizeof ie);
            if (got != (ssize_t)sizeof ie) {
                if (got < 0 && errno != EAGAIN && errno != EINTR) {
                    drop_dev(i--);                /* unplugged / gone to sleep */
                    mouse_changed = 0;
                }
                break;
            }
            if (in_debug && ie.type != EV_SYN && dev[i].dbg_n < 60) {
                dev[i].dbg_n++;
                printf("[ASTOS] input: %s type %d code %d value %d\n", dev[i].node, ie.type,
                       ie.code, ie.value);
            }
            if (dev[i].is_pad) {
                if (ie.type == EV_ABS && ie.code == ABS_HAT0X && ie.value)
                    return key_ev((struct si_event){ ie.value < 0 ? SI_LEFT : SI_RIGHT, SI_SRC_PAD, 0 });
                if (ie.type == EV_ABS && ie.code == ABS_HAT0Y && ie.value)
                    return key_ev((struct si_event){ ie.value < 0 ? SI_UP : SI_DOWN, SI_SRC_PAD, 0 });
                if (ie.type == EV_ABS && (ie.code == ABS_X || ie.code == ABS_Y)) {
                    int a = ie.code == ABS_Y;
                    int s = si_stick_state(dev[i].stick[a], ie.value, dev[i].ax[a].centre,
                                           dev[i].ax[a].on, dev[i].ax[a].off);
                    if (s != dev[i].stick[a]) {
                        dev[i].stick[a] = s;
                        if (s < 0) return key_ev((struct si_event){ a ? SI_UP : SI_LEFT, SI_SRC_PAD, 0 });
                        if (s > 0) return key_ev((struct si_event){ a ? SI_DOWN : SI_RIGHT, SI_SRC_PAD, 0 });
                    }
                    continue;
                }
                if (ie.type == EV_KEY && ie.value == 1) {
                    struct si_event k = si_map_pad(ie.code);
                    if (k.key != SI_NONE)
                        return key_ev(k);
                }
                continue;
            }
            if (ie.type == EV_REL) {
                if (ie.code == REL_X) { mdx += ie.value; mouse_changed = 1; }
                if (ie.code == REL_Y) { mdy += ie.value; mouse_changed = 1; }
                if (ie.code == REL_WHEEL) { mwheel += ie.value; mouse_changed = 1; }
                continue;
            }
            if (ie.type == EV_SYN && mouse_changed)
                break;                        /* one report = one event */
            if (ie.type != EV_KEY)
                continue;
            if (ie.code == BTN_LEFT || ie.code == BTN_RIGHT) {
                int bit = ie.code == BTN_LEFT ? 1 : 2;
                usb_buttons = ie.value ? (usb_buttons | bit) : (usb_buttons & ~bit);
                mouse_changed = 1;
                break;
            }
            if (ie.code == KEY_LEFTSHIFT || ie.code == KEY_RIGHTSHIFT) {
                shift_usb = ie.value != 0;
                continue;
            }
            if (ie.value != 1 && ie.value != 2)
                continue;
            if (ie.code == KEY_PAGEUP)        /* PC keyboards: PgUp is Help */
                return key_ev((struct si_event){ SI_SNAP, SI_SRC_USB, 0 });
            struct si_event k = si_map_evdev(ie.code, shift_usb);
            if (k.key != SI_NONE)
                return key_ev(k);
        }
        if (mouse_changed) {
            struct ai_event e = none();
            e.type = AI_MOUSE;
            e.dx = mdx; e.dy = mdy;
            e.wheel = mwheel;
            e.buttons = usb_buttons;
            e.usb = 1;
            return e;
        }
    }
    return none();
}

struct ai_event ai_poll(int timeout_ms)
{
    if (have_pend) {
        have_pend = 0;
        return pend_key;
    }
    for (int waited = 0; ; waited += 2) {
        struct ai_event e = usb_poll();
        if (e.type != AI_NONE)
            return e;
        if (have_st) {
            /* everything the reader has queued; consecutive mouse packets
             * with the same buttons become one movement */
            struct ai_event m = none();
            for (;;) {
                unsigned before = atomic_load(&ring_r);
                e = st_poll();
                if (e.type == AI_NONE) {
                    if (atomic_load(&ring_r) == before)
                        break;                    /* ring empty           */
                    continue;                     /* mid-packet byte      */
                }
                if (e.type == AI_KEY) {
                    if (m.type == AI_MOUSE) {     /* deliver the move first, */
                        pend_key = e;             /* the key next time       */
                        have_pend = 1;
                        return m;
                    }
                    return e;
                }
                if (m.type == AI_MOUSE && m.buttons != e.buttons) {
                    pend_key = e;                 /* a button change ends it */
                    have_pend = 1;
                    return m;
                }
                if (m.type == AI_MOUSE) { m.dx += e.dx; m.dy += e.dy; }
                else m = e;
            }
            if (m.type == AI_MOUSE)
                return m;
        }
        if (waited >= timeout_ms)
            return none();
        usleep(2000);
    }
}
