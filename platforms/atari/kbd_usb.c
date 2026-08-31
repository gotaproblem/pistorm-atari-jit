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
#include "fdd/atari_fdd.h"                 /* fdd_toggle_disk (F11) */

/* Per-second [KBD] state line: off unless explicitly built in. */
#ifndef KBD_USB_DIAG
#define KBD_USB_DIAG 0
#endif

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
#define MOUSE_MAX_QUEUE      6      /* bytes in flight: ~2 packets       */
#define MOUSE_CARRY_CLAMP  384      /* max banked counts per axis        */

/* Host-count divisor, "kbd usb mousediv N". 1 = raw host counts. */
int kbd_usb_mouse_div = 1;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

/* ------------------------------------------------------------------ */
/* MPSC ring of IKBD bytes; bit 8 marks "start of packet"              */
/* Producers: evdev input thread, plus the CPU thread when it           */
/* synthesises an IKBD command response in standalone mode - hence the  */
/* push lock. Consumer: CPU thread only.                                */
/* ------------------------------------------------------------------ */

#define RING_SIZE 1024              /* power of two                      */
#define RING_MASK (RING_SIZE - 1)
#define PKT_START 0x100

static uint16_t         ring[RING_SIZE];
static _Atomic uint32_t ring_head;  /* consumer (CPU thread)             */
static _Atomic uint32_t ring_tail;  /* producers (see above)             */
static pthread_mutex_t  ring_push_lock = PTHREAD_MUTEX_INITIALIZER;

/* consumer-side presentation state (CPU thread owns, ipl_task reads)   */
static _Atomic uint64_t next_ready_us;   /* serial pacing gate           */
static _Atomic uint64_t real_quiet_us;   /* no new pkt before this       */
static _Atomic int      in_packet;       /* mid-injected-packet flag     */

/* guest MFP mask shadow: assume keyboard irq enabled until told else   */
static _Atomic uint8_t  mfp_ierb = 0xFF;
static _Atomic uint8_t  mfp_imrb = 0xFF;

/* Virtual GPIP4 in-service state. On a real MFP, IACK of a channel sets
 * its ISRB bit (software-EOI mode), and that bit BLOCKS the channel from
 * re-interrupting until the handler writes the EOI (a 0 into the bit at
 * $FFFA11). kbd_usb_irq_wanted() used to ignore this: with bytes pending
 * it kept level 6 asserted straight through the guest's handler, so the
 * moment the handler lowered its IPL (EmuTOS runs parts at mask 5) we
 * re-entered it - nested until the supervisor stack ran through the
 * sysvars and the machine died on corruption. Field case: Petra/Paula,
 * whose 9-19 kHz replay ISRs widen the window enormously; EXCRING showed
 * 60+ vec=30/EFF=46 entries with a monotonically descending sp, ending
 * in a wild jump.
 * mfp_vr shadows $FFFA17: S bit (0x08) clear = auto-EOI mode, in which
 * the real chip never holds in-service - so neither do we. TOS/EmuTOS
 * use software EOI ($48); default matches. */
static _Atomic uint8_t  kbd_in_service = 0;
static _Atomic uint8_t  mfp_vr = 0x48;

void kbd_usb_virtual_iacked(void)
{
    if (atomic_load_explicit(&mfp_vr, memory_order_relaxed) & 0x08)
        atomic_store_explicit(&kbd_in_service, 1, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* Real IKBD presence state (see the detection section further down)   */
/* ------------------------------------------------------------------ */

extern uint8_t ps_read_8(uint32_t address);   /* gpio/ps_protocol.h      */

#define KBD_ACIA_CTRL_ADDR 0x00FFFC00u
#define KBD_ACIA_DATA_ADDR 0x00FFFC02u

/* 6850 status bits */
#define ACIA_RDRF  0x01
#define ACIA_TDRE  0x02
#define ACIA_FE    0x10
#define ACIA_OVRN  0x20
#define ACIA_PE    0x40
#define ACIA_IRQ   0x80
#define ACIA_ERRS  (ACIA_OVRN | ACIA_FE | ACIA_PE)

enum { IKBD_UNKNOWN = 0, IKBD_PRESENT, IKBD_ABSENT };

static _Atomic int      real_state = IKBD_UNKNOWN;
static _Atomic uint32_t real_err_run;        /* consecutive bad reads     */
static _Atomic uint32_t real_good_run;       /* consecutive clean reads   */
static _Atomic uint64_t reset_probe_due;     /* IKBD reset answer deadline*/
static _Atomic int      reset_probe_armed;
static _Atomic int      probe_saw_clean;     /* real reply during probe   */
static _Atomic uint32_t real_drained;        /* garbage bytes swallowed   */
static _Atomic uint32_t real_rx_total;       /* real bytes consumed       */
static _Atomic uint32_t real_rx_passed;      /* real bytes given to guest */
static _Atomic uint32_t rate_win_bytes;      /* bytes in current window   */
static _Atomic uint64_t rate_win_start;
static _Atomic uint32_t rate_last_bps;       /* last completed window     */
static _Atomic uint32_t rate_hot_wins;       /* consecutive flood windows */
static _Atomic uint8_t  real_last_status;    /* for diagnostics           */

/* forced mode from config: 0 = auto, 1 = force merge, 2 = force standalone */
int kbd_usb_force_mode = 0;

#define ERR_RUN_TO_ABSENT   12   /* noisy line -> quarantine              */
#define GOOD_RUN_TO_PRESENT  4   /* clean bytes -> trust the real IKBD    */
#define RESET_ANSWER_US 400000   /* IKBD reset reply window (0xF1)        */
#define RATE_FLOOD_BPS     400   /* B/s above which it cannot be a human  */
#define RATE_HOT_WINS        2   /* consecutive flood seconds -> absent   */

/* Last control-register value the guest wrote to the keyboard ACIA. The
 * register is write-only (reads return status), so we have to remember it
 * to be able to put it back. TOS writes 0x96: RIE on, /64, 8N1. */
static _Atomic uint8_t last_ctrl_written = 0x96;

/* NOTE: must match gpio/ps_protocol.h exactly - the data argument is
 * uint16_t there, not uint8_t. */
extern void ps_write_8(uint32_t address, uint16_t value);

static int quarantined(void);

/* Hiding the garbage from the guest is not enough on its own: the real
 * ACIA still asserts its IRQ output on every noise byte, so the real MFP
 * keeps raising level 6 and TOS runs its ACIA handler thousands of times a
 * second for nothing. That storm is pure emulated-CPU overhead and shows up
 * as jerky mouse movement. Clearing RIE (control bit 7) stops the real ACIA
 * driving GPIP4 at all, while leaving it perfectly able to receive - so we
 * can still read RDRF to notice a keyboard being plugged back in, and our
 * synthesised level-6 is unaffected because it never goes near the MFP.
 * Only the keyboard ACIA is touched; the MIDI ACIA keeps its own RIE. */
static void real_acia_set_rie(int on)
{
    /* Never re-arm the real receiver's interrupt while quarantined. In
     * forced standalone the state machine can still wander to PRESENT off
     * cleanly-framed noise, and without this guard that would switch the
     * IRQ storm back on behind the user's back - silently, since the data
     * itself stays hidden, so there is no bell to warn you. */
    if (on && quarantined())
        return;

    uint8_t c = atomic_load_explicit(&last_ctrl_written, memory_order_relaxed);
    c = on ? (uint8_t)(c | 0x80) : (uint8_t)(c & ~0x80);
    ps_write_8(KBD_ACIA_CTRL_ADDR, c);
}

uint8_t kbd_usb_ctrl_filter(uint8_t v)
{
    atomic_store_explicit(&last_ctrl_written, v, memory_order_relaxed);
    if (quarantined())
        v &= (uint8_t)~0x80;          /* keep the real receiver silent   */
    return v;
}

static void real_state_set(int s)
{
    int old = atomic_exchange(&real_state, s);
    if (old == s)
        return;
    printf("[KBD] real IKBD %s - %s\n",
           s == IKBD_PRESENT ? "detected"
         : s == IKBD_ABSENT  ? "absent/noisy" : "unknown",
           s == IKBD_PRESENT ? "merging real + USB input"
                             : "quarantining real ACIA, USB input only");
    fflush(stdout);

    /* Runs on the CPU thread (all detection does), so touching the bus
     * here is safe. */
    real_acia_set_rie(s != IKBD_ABSENT);
}

static int quarantined(void)
{
    if (kbd_usb_force_mode == 1) return 0;
    if (kbd_usb_force_mode == 2) return 1;
    return atomic_load_explicit(&real_state, memory_order_relaxed) == IKBD_ABSENT;
}

static int ring_used(void)
{
    return (int)((atomic_load_explicit(&ring_tail, memory_order_acquire) -
                  atomic_load_explicit(&ring_head, memory_order_acquire)) & RING_MASK);
}

static int ring_free(void)
{
    return RING_SIZE - 1 - ring_used();
}

static void ring_push_packet(const uint8_t *bytes, int n)
{
    pthread_mutex_lock(&ring_push_lock);
    if (ring_free() < n)
    {
        kbd_usb_stat_dropped_bytes += (uint32_t)n;  /* whole packet or none */
        pthread_mutex_unlock(&ring_push_lock);
        return;
    }
    uint32_t t = atomic_load_explicit(&ring_tail, memory_order_relaxed);
    for (int i = 0; i < n; i++)
        ring[(t + (uint32_t)i) & RING_MASK] = (uint16_t)bytes[i] | (i == 0 ? PKT_START : 0);
    atomic_store_explicit(&ring_tail, t + (uint32_t)n, memory_order_release);
    pthread_mutex_unlock(&ring_push_lock);
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

/* ------------------------------------------------------------------ */
/* Native mouse: fewer IKBD reports, same pointer speed                */
/*                                                                     */
/* The level-6 interrupts a moving mouse costs are raised by the REAL  */
/* MFP, because the real ACIA latched a byte. By the time this file    */
/* sees that byte the interrupt has already been taken, so dropping or */
/* rewriting it here saves nothing. The only way to spend fewer        */
/* interrupts is to make the IKBD send fewer bytes - and IKBD command  */
/* 0x0B (set relative mouse threshold) does exactly that: it withholds */
/* a report until accumulated motion exceeds the threshold, so N costs */
/* roughly 1/N the packets for the same hand movement. At 7812.5 baud, */
/* 8N1, three bytes a packet, the stream tops out near 260 packets/s = */
/* ~780 level-6/s, on top of Timer C's 200/s.                          */
/*                                                                     */
/* The threshold BATCHES motion, it does not shrink it: the report      */
/* carries the full accumulated delta, so total distance is unchanged   */
/* and only the granularity gets coarser. Scaling therefore defaults to */
/* 1 - an earlier version defaulted it to N "to compensate", which made */
/* the pointer travel N times too far in N*N-count steps and was very   */
/* hard to position. SCALE is a speed preference, not a correction.     */
/*                                                                     */
/*   PISTORM_MOUSE_THRESH=N   1..15. 0 or unset = feature off entirely */
/*   PISTORM_MOUSE_SCALE=M    delta multiplier, default 1 (unscaled)   */
/*                                                                     */
/* Real IKBD bytes only. Injected USB packets are generated host-side  */
/* and cost no real interrupt, so they are left exactly as they are.   */
/*                                                                     */
/* This works with or without "kbd usb". Wanting a quieter native      */
/* mouse is not a reason to take on the USB injection layer and its    */
/* presence detection - that machinery can quarantine the real ACIA    */
/* and drop you to USB-only input, which is the opposite of what       */
/* someone asking for a better NATIVE mouse wants. So there are two    */
/* thin drivers over the same threshold/scale state:                   */
/*                                                                     */
/*   kbd usb ON  - hooks inside kbd_usb_acia_data_shim/tx_snoop, and   */
/*                 the extra presence guards apply because that state  */
/*                 exists and is meaningful;                           */
/*   kbd usb OFF - kbd_native_* below, called directly from the ACIA   */
/*                 paths in emulator.c. No presence detection, no      */
/*                 quarantine, no RIE handling - nothing to upset,     */
/*                 because none of it is running.                      */
/* ------------------------------------------------------------------ */

static int      mouse_thresh = -1;      /* -1 = env not read yet        */
static int      mouse_scale  = 1;
static uint8_t  thresh_tx[3];
static int      thresh_tx_left;         /* bytes of 0x0B x y still due  */
static uint64_t thresh_due_us;
static int      mouse_pkt_left;         /* dx/dy bytes still expected   */
/* native-path command tracker (see kbd_native_* below) */
static int      nat_cmd;
static int      nat_params;
static int      nat_memload;
static int      nat_mouse_rel = 1;      /* IKBD powers up relative      */

static void mouse_cfg_init(void)
{
    const char *e = getenv("PISTORM_MOUSE_THRESH");

    mouse_thresh = (e && *e) ? atoi(e) : 0;
    if (mouse_thresh < 0)  mouse_thresh = 0;
    if (mouse_thresh > 15) mouse_thresh = 15;

    e = getenv("PISTORM_MOUSE_SCALE");
    mouse_scale = (e && *e) ? atoi(e) : 1;
    if (mouse_scale < 1)  mouse_scale = 1;
    if (mouse_scale > 16) mouse_scale = 16;

    if (mouse_thresh)
        printf("[KBD] native mouse: IKBD threshold %d, delta scale %d\n",
               mouse_thresh, mouse_scale);
}

/* Queue "0x0B thresh thresh" to the real IKBD, not before now+delay -
 * after a reset the controller needs time before it will listen. */
static void mouse_thresh_arm(uint64_t delay_us)
{
    if (mouse_thresh <= 0)
        return;
    thresh_tx[0] = 0x0B;
    thresh_tx[1] = (uint8_t) mouse_thresh;
    thresh_tx[2] = (uint8_t) mouse_thresh;
    thresh_tx_left = 3;
    thresh_due_us  = now_us() + delay_us;
}

/* One byte per call at most. Called from the guest's ACIA read path, so
 * it is already serialised against the guest's own ACIA writes.
 *
 * The guards matter more than the write does. Injecting a byte at the
 * wrong moment makes the IKBD answer oddly, or not at all, and the
 * presence detector reads that as a missing or noisy keyboard - it
 * quarantines the real ACIA and drops to USB input only. So:
 *
 *   - only ever talk to a keyboard already confirmed PRESENT. UNKNOWN
 *     means detection has not finished; ABSENT means it is quarantined
 *     and a write would be pointless anyway;
 *   - never while the reset probe is armed. That window exists to hear
 *     the IKBD's 0xF1 answer, and a command arriving inside it is the
 *     most direct way to make a healthy keyboard look dead;
 *   - never part-way through a command the guest is sending, checked
 *     under ikbd.lock rather than racing the snoop that maintains it.
 */
/* Shared tail: emit one queued byte if the transmitter will take it. */
static void mouse_thresh_tx_one(void)
{
    uint8_t st = ps_read_8(KBD_ACIA_CTRL_ADDR);

    if (!(st & ACIA_TDRE))
        return;                         /* transmitter still busy       */
    ps_write_8(KBD_ACIA_DATA_ADDR, thresh_tx[3 - thresh_tx_left]);
    if (--thresh_tx_left == 0)
        printf("[KBD] native mouse: IKBD threshold %d applied\n", mouse_thresh);
}

/* USB path. */
static void mouse_thresh_pump(void)
{
    int busy;

    if (thresh_tx_left <= 0 || now_us() < thresh_due_us)
        return;
    if (atomic_load_explicit(&real_state, memory_order_relaxed) != IKBD_PRESENT)
        return;
    if (atomic_load_explicit(&reset_probe_armed, memory_order_relaxed))
        return;

    pthread_mutex_lock(&ikbd.lock);
    busy = (ikbd.pending_params > 0 || ikbd.memload_left > 0);
    pthread_mutex_unlock(&ikbd.lock);
    if (busy)
        return;

    mouse_thresh_tx_one();
}

/* Native path. None of the presence state above exists or is maintained
 * when USB injection is off, so the guards are just the post-reset delay
 * and not interleaving into a command the guest is sending. Nothing here
 * can quarantine anything, because there is no quarantine. */
static void mouse_native_pump(void)
{
    if (thresh_tx_left <= 0 || now_us() < thresh_due_us)
        return;
    if (nat_params > 0 || nat_memload > 0)
        return;
    mouse_thresh_tx_one();
}

/* Scale the dx/dy of a relative mouse packet on its way to the guest.
 * 0xF8..0xFB is the header (buttons in bits 0-1) followed by two signed
 * bytes. Only engaged in relative mode - in absolute or keycode mode
 * those byte values mean something else entirely. mouse_mode is written
 * under ikbd.lock by a single writer; a stale read here costs at worst
 * one mis-scaled packet across a mode change. */
static uint8_t mouse_scale_byte_mode(uint8_t v, int relative)
{
    if (mouse_thresh <= 0 || mouse_scale <= 1)
        return v;

    if (mouse_pkt_left > 0)
    {
        int d = (int)(int8_t) v * mouse_scale;
        if (d >  127) d =  127;
        if (d < -128) d = -128;
        mouse_pkt_left--;
        return (uint8_t)(int8_t) d;
    }
    if (v >= 0xF8 && v <= 0xFB && relative)
        mouse_pkt_left = 2;
    return v;
}

static uint8_t mouse_scale_byte(uint8_t v)
{
    return mouse_scale_byte_mode(v, ikbd.mouse_mode == MOUSE_REL);
}

/* ---- native path: used when "kbd usb" is OFF ---------------------- */
/* Its own tiny command tracker, because kbd_usb_tx_snoop() returns
 * immediately when USB is disabled and so ikbd.pending_params is not
 * maintained. Everything else - the threshold queue, the scaling - is
 * shared with the USB path above. */

int kbd_native_mouse_enabled(void)
{
    if (mouse_thresh < 0)
        mouse_cfg_init();
    return mouse_thresh > 0;
}

/* Guest -> IKBD. Track command boundaries so our own byte is never
 * interleaved into one, and re-arm whenever the guest resets the
 * controller or sets a threshold of its own. */
void kbd_native_tx_snoop(uint8_t v)
{
    if (mouse_thresh <= 0)
        return;

    if (nat_memload > 0)
    {
        nat_memload--;
        return;
    }
    if (nat_params > 0)
    {
        nat_params--;
        if (nat_cmd == 0x80 && v == 0x01)
        {
            /* full IKBD reset: back to threshold 1,1 and relative mode.
             * Leave it well alone until the controller has settled. */
            nat_mouse_rel = 1;
            mouse_pkt_left = 0;
            mouse_thresh_arm(RESET_ANSWER_US + 200000);
        }
        if (nat_cmd == 0x20 && nat_params == 0)
            nat_memload = v;
        if (nat_params == 0 && nat_cmd != 0x20)
        {
            if (nat_cmd == 0x0B)        /* guest replaced ours */
                mouse_thresh_arm(2000);
            nat_cmd = 0;
        }
        return;
    }

    nat_params = ikbd_param_len(v);
    if (nat_params > 0)
    {
        nat_cmd = v;
        return;
    }
    switch (v)
    {
        case 0x08: nat_mouse_rel = 1; break;   /* relative reporting  */
        case 0x09: case 0x0A: case 0x12:
                   nat_mouse_rel = 0; break;   /* absolute/keycode/off */
        default: break;
    }
}

/* IKBD -> guest. One byte, already read from the ACIA by the caller. */
uint8_t kbd_native_rx_filter(uint8_t v)
{
    if (mouse_thresh <= 0)
        return v;
    if (thresh_tx_left > 0)
        mouse_native_pump();
    return mouse_scale_byte_mode(v, nat_mouse_rel);
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
    /* a reset returns the IKBD to threshold 1,1 - put ours back once the
     * controller has finished resetting */
    mouse_pkt_left = 0;
    mouse_thresh_arm(RESET_ANSWER_US);
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
        {
            ikbd_reset_state();
            /* Arm the presence probe: a live IKBD answers a reset with
             * 0xF1 within a few hundred ms. Silence means no keyboard.
             * This is the only route back out of quarantine, so open the
             * real receiver's interrupt for the duration of the window -
             * otherwise a keyboard that HAS been reconnected can never be
             * heard, because quarantine keeps RIE clear. A few hundred ms
             * of noise is a fair price for being able to recover at all. */
            const int was_quarantined = quarantined();
            atomic_store(&probe_saw_clean, 0);
            atomic_store(&reset_probe_due, now_us() + RESET_ANSWER_US);
            atomic_store(&reset_probe_armed, 1);
            atomic_store(&real_good_run, 0);
            atomic_store(&rate_hot_wins, 0);
            atomic_store(&rate_win_bytes, 0);
            if (was_quarantined)
            {
                /* Only open the real receiver for the probe in AUTO mode.
                 * Under forced standalone we are never leaving quarantine,
                 * so a probe window would be 400ms of pointless IRQ storm
                 * on every guest reset. */
                if (kbd_usb_force_mode == 0)
                    real_acia_set_rie(1);
                /* Nothing real will answer if the line is dead, so answer
                 * for it - otherwise TOS waits out its own timeout. */
                static const uint8_t ack = 0xF1;
                ring_push_packet(&ack, 1);
            }
        }
        if (ikbd.pending_cmd == 0x20 && ikbd.pending_params == 0)
            ikbd.memload_left = v;          /* 3rd param = byte count    */
        if (ikbd.pending_params == 0 && ikbd.pending_cmd != 0x20)
        {
            /* The guest just set its own mouse threshold, which replaces
             * ours. Put ours back, or the saving silently disappears the
             * first time TOS or an app touches the mouse parameters. */
            if (ikbd.pending_cmd == 0x0B)
                mouse_thresh_arm(2000);
            ikbd.pending_cmd = 0;
        }
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
    else if (a == 0x00FFFA11u) {            /* ISRB: 0-bits written = EOI */
        if ((b & 0x40) == 0)                /* GPIP4's in-service cleared */
            atomic_store_explicit(&kbd_in_service, 0, memory_order_relaxed);
    }
    else if (a == 0x00FFFA17u) {            /* VR: S=0 -> auto-EOI mode  */
        atomic_store_explicit(&mfp_vr, b, memory_order_relaxed);
        if ((b & 0x08) == 0)
            atomic_store_explicit(&kbd_in_service, 0, memory_order_relaxed);
    }
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

/* ------------------------------------------------------------------ */
/* Real IKBD presence detection + quarantine                           */
/*                                                                      */
/* With the ST keyboard unplugged the 6850's RX pin floats: noise gets  */
/* framed as bytes, so the ACIA sits there with framing/overrun errors  */
/* set and garbage in RDRF. Passed to TOS that garbage becomes nonsense */
/* scancodes, an overflowing keyboard buffer and the constant bell -    */
/* and it also starves the injected stream, because the merge logic     */
/* defers to real bytes.                                                */
/*                                                                      */
/* So: watch the real receiver, and when it looks dead-or-noisy rather  */
/* than like an IKBD, QUARANTINE it - drain the ACIA (which also clears */
/* its IRQ, stopping the real GPIP4 storm) and hide it from the guest.  */
/* Clean real traffic reappearing flips straight back to merge mode, so */
/* hot-plugging the ST keyboard back in works without a restart.        */
/* ------------------------------------------------------------------ */

/* Called on every guest ACIA status read. Only the reset-probe timeout
 * lives here: RDRF and the error flags persist in the 6850 until RDR is
 * read, so a polling guest would see the SAME byte dozens of times.
 * Classification therefore happens per consumed byte, below. */
static void real_observe_status(uint8_t rs)
{
    (void)rs;
    if (kbd_usb_force_mode != 0)
        return;
    if (!atomic_load_explicit(&reset_probe_armed, memory_order_relaxed) ||
        now_us() <= atomic_load_explicit(&reset_probe_due, memory_order_relaxed))
        return;

    /* Probe window closed. A real IKBD answers a reset within a few
     * hundred ms; silence (or nothing but noise) means there is nobody
     * on the other end. This is the ONLY way back out of quarantine. */
    atomic_store_explicit(&reset_probe_armed, 0, memory_order_relaxed);
    const int clean = atomic_exchange(&probe_saw_clean, 0);
    real_state_set(clean ? IKBD_PRESENT : IKBD_ABSENT);
}

/* Classify one byte actually taken out of the real receiver.
 *
 * Error flags alone are NOT a sufficient test: a floating RX line produces
 * plenty of noise that happens to frame cleanly, and an earlier version of
 * this code counted those as proof of a live keyboard - so it stayed in
 * merge mode and kept feeding TOS garbage.
 *
 * The reliable discriminator is throughput. A real IKBD is silent unless
 * you touch it, and is hard-limited to ~780 bytes/s by the 7812.5 bps link;
 * even continuous mouse movement sits near 300 B/s. A floating line streams
 * at close to line rate without pause. So: sustained flood => no keyboard. */
static void real_byte_consumed(uint8_t rs)
{
    /* An explicit mode from the config is a decision, not a hint - don't
     * let detection second-guess it (and save the work while we're here). */
    if (kbd_usb_force_mode != 0)
        return;

    const uint64_t t = now_us();

    if (rs & ACIA_ERRS)
    {
        atomic_store_explicit(&real_good_run, 0, memory_order_relaxed);
        if (atomic_fetch_add(&real_err_run, 1) + 1 >= ERR_RUN_TO_ABSENT &&
            atomic_load_explicit(&real_state, memory_order_relaxed) != IKBD_ABSENT)
            real_state_set(IKBD_ABSENT);
    }
    else
    {
        atomic_store_explicit(&real_err_run, 0, memory_order_relaxed);
        /* Positive evidence of a live keyboard, but only from a cold start
         * or inside a reset probe - see the comment on recovery below. */
        if (atomic_load_explicit(&reset_probe_armed, memory_order_relaxed))
            atomic_store_explicit(&probe_saw_clean, 1, memory_order_relaxed);
        if (atomic_fetch_add(&real_good_run, 1) + 1 >= GOOD_RUN_TO_PRESENT &&
            atomic_load_explicit(&real_state, memory_order_relaxed) == IKBD_UNKNOWN)
            real_state_set(IKBD_PRESENT);
    }

    /* --- rate window --- */
    uint64_t ws = atomic_load_explicit(&rate_win_start, memory_order_relaxed);
    if (ws == 0)
    {
        atomic_store_explicit(&rate_win_start, t, memory_order_relaxed);
    }
    else if (t - ws >= 1000000)
    {
        const uint32_t n = atomic_exchange(&rate_win_bytes, 0);
        atomic_store_explicit(&rate_win_start, t, memory_order_relaxed);
        atomic_store_explicit(&rate_last_bps, n, memory_order_relaxed);

        if (n > RATE_FLOOD_BPS)
        {
            if (atomic_fetch_add(&rate_hot_wins, 1) + 1 >= RATE_HOT_WINS &&
                atomic_load_explicit(&real_state, memory_order_relaxed) != IKBD_ABSENT)
                real_state_set(IKBD_ABSENT);
        }
        else
        {
            atomic_store_explicit(&rate_hot_wins, 0, memory_order_relaxed);
        }

        /* NO rate-based recovery here. Quarantine works precisely BY making
         * the stream quiet (RIE is cleared, so the flood stops), so judging
         * "is the keyboard back?" from the observed rate is a feedback loop:
         * quarantine succeeds -> looks calm -> un-quarantine -> flood and
         * bell return -> quarantine again, oscillating once a second. The
         * only way out of quarantine is the reset probe below, which is
         * positive evidence rather than absence of evidence. */
    }

    /* Flood detection inside the window too, so a screaming line is caught
     * in a fraction of a second instead of taking two full windows - that
     * is the difference between a blip and a second of bell at boot.
     *
     * This also has to cancel any open reset probe: with RIE re-enabled for
     * the probe, a floating line delivers plenty of cleanly-framed noise,
     * and without this the probe would see a "clean reply" and conclude a
     * keyboard had appeared. A flood is decisive - it is not a keyboard. */
    if (atomic_load_explicit(&rate_win_bytes, memory_order_relaxed) > RATE_FLOOD_BPS &&
        atomic_load_explicit(&real_state, memory_order_relaxed) != IKBD_ABSENT)
    {
        atomic_store_explicit(&reset_probe_armed, 0, memory_order_relaxed);
        atomic_store_explicit(&probe_saw_clean, 0, memory_order_relaxed);
        real_state_set(IKBD_ABSENT);
    }
    atomic_fetch_add(&rate_win_bytes, 1);
    atomic_fetch_add(&real_rx_total, 1);
}

/* Swallow a pending real byte. Reading RDR also clears the ACIA's IRQ
 * output, which is what stops the real MFP GPIP4 interrupt storm. */
/* --- STBOX input routing -------------------------------------------------
 * While the sandbox's GEM window is focused, USB input goes to the sandbox
 * IKBD instead of the main machine - that is what gives games real key-up
 * events and a mouse (GEM's fallback path in STBOX.PRG has neither). F11
 * toggles the routing while focused, because with the mouse captured by the
 * box there is no other way to click outside its window. F12 keeps its
 * existing main-machine grab meaning. */
#include "stbox/stbox.h"
static int stbox_route_enabled = 1;

static int stbox_wants_input(void)
{
    return stbox_route_enabled && stbox_running() && stbox_get_focus();
}

static void real_drain(uint8_t rs)
{
    if (!(rs & (ACIA_RDRF | ACIA_ERRS)))
        return;
    (void)ps_read_8(KBD_ACIA_DATA_ADDR);
    atomic_fetch_add(&real_drained, 1);
    real_byte_consumed(rs);
}

/* ---- shared ACIA/GPIP shims (used by pistorm_natmem.cpp and the ---- */
/* ---- legacy emulator.c memory handlers)                          ---- */

uint8_t kbd_usb_acia_status_shim(uint8_t real)
{
    atomic_store_explicit(&real_last_status, real, memory_order_relaxed);
    real_observe_status(real);

    /* Sandbox focused: the real IKBD's output belongs to the box. Capture
     * the byte here (every real byte raises GPIP4, so the guest polls this
     * status at least once per byte - the capture keeps the 1-byte ACIA
     * from overrunning) and hide the real receiver from the main guest.
     * Raw bytes: the box's TOS does its own mouse handling, so the main
     * screen's threshold/scale tuning must not touch them. */
    if (stbox_wants_input() && (real & ACIA_RDRF))
    {
        uint8_t v = ps_read_8(KBD_ACIA_DATA_ADDR);
        real_byte_consumed(real);
        kbd_usb_note_real_rx();
        stbox_ikbd_byte(v);
        real &= (uint8_t)~(ACIA_RDRF | ACIA_ERRS | ACIA_IRQ);
    }

    if (quarantined())
    {
        /* hide the real receiver entirely: no RDRF, no error flags, no
         * IRQ claim - then present our own byte if one is due */
        real_drain(real);
        real &= (uint8_t)~(ACIA_RDRF | ACIA_ERRS | ACIA_IRQ);
        if (kbd_usb_rx_priority() || kbd_usb_rx_ready())
            real |= (ACIA_RDRF | ACIA_IRQ);
        return real;
    }

    if (kbd_usb_rx_priority() || (!(real & ACIA_RDRF) && kbd_usb_rx_ready()))
    {
        /* Presenting an injected byte: the error flags belong to the real
         * receiver, not to our byte. Leaving them set makes TOS discard
         * the character we just handed it. */
        real &= (uint8_t)~ACIA_ERRS;
        real |= (ACIA_RDRF | ACIA_IRQ);
    }
    return real;
}

uint8_t kbd_usb_acia_data_shim(void)
{
    if (mouse_thresh < 0)
        mouse_cfg_init();
    if (thresh_tx_left > 0)
        mouse_thresh_pump();

    if (kbd_usb_rx_priority())
        return kbd_usb_rx_read();

    uint8_t rs = ps_read_8(KBD_ACIA_CTRL_ADDR);
    real_observe_status(rs);

    if (quarantined())
    {
        real_drain(rs);
        if (kbd_usb_rx_ready())
            return kbd_usb_rx_read();
        return 0xFF;                          /* idle line, never a key    */
    }

    /* fresh real status decides whose byte the guest gets */
    if (rs & ACIA_RDRF)
    {
        uint8_t v = ps_read_8(KBD_ACIA_DATA_ADDR);
        atomic_fetch_add(&real_rx_passed, 1);
        real_byte_consumed(rs);
        kbd_usb_note_real_rx();
        if (stbox_wants_input())
        {
            stbox_ikbd_byte(v);          /* raw, for the sandbox */
            if (kbd_usb_rx_ready())
                return kbd_usb_rx_read();
            return 0xFF;                 /* idle line for the main guest */
        }
        return mouse_scale_byte(v);     /* native mouse only */
    }
    if (kbd_usb_rx_ready())
        return kbd_usb_rx_read();
    return ps_read_8(KBD_ACIA_DATA_ADDR);
}

uint8_t kbd_usb_gpip_shim(uint8_t real)
{
    if (kbd_usb_rx_ready() || kbd_usb_rx_priority())
        real &= (uint8_t)~0x10;              /* GPIP4 low (active low)    */
    else if (quarantined())
        real |= 0x10;                        /* hide the noisy real ACIA  */
    return real;
}

int kbd_usb_real_ikbd_present(void)
{
    return atomic_load_explicit(&real_state, memory_order_relaxed) == IKBD_PRESENT;
}

/* Once-per-second state dump. OFF by default - it was for bringing the
 * merge/quarantine logic up and is just noise on a working system. Build
 * with -DKBD_USB_DIAG=1 to get it back if the input path ever misbehaves.
 * Called from the input thread. */
#if KBD_USB_DIAG
void kbd_usb_diag_tick(void)
{
    static uint32_t last_real, last_inj, last_drain;
    const uint32_t r = atomic_load(&real_rx_total);
    const uint32_t i = kbd_usb_stat_injected_bytes;
    const uint32_t d = atomic_load(&real_drained);
    const int      st = atomic_load(&real_state);

    if (r == last_real && i == last_inj && d == last_drain)
        return;                                  /* idle - stay quiet      */

    printf("[KBD] %s real=%u/s (passed=%u drained=%u) inj=%u/s "
           "status=$%02X errs=%u vIACK=%u\n",
           kbd_usb_force_mode == 2 ? "STANDALONE(forced)"
         : kbd_usb_force_mode == 1 ? "MERGE(forced)"
         : st == IKBD_ABSENT       ? "QUARANTINE(auto)"
         : st == IKBD_PRESENT      ? "MERGE(auto)" : "PROBING",
           r - last_real,
           atomic_load(&real_rx_passed),
           d - last_drain,
           i - last_inj,
           atomic_load(&real_last_status),
           atomic_load(&real_err_run),
           kbd_usb_stat_virtual_iacks);
    fflush(stdout);
    last_real = r; last_inj = i; last_drain = d;
}
#else
void kbd_usb_diag_tick(void) { }
#endif

int kbd_usb_irq_wanted(void)
{
    if (!kbd_usb_rx_ready() && !kbd_usb_rx_priority())
        return 0;
    /* in-service: channel is inside its handler (IACKed, EOI not yet
     * written) - a real MFP cannot re-interrupt from this channel now,
     * and neither may we (see kbd_in_service above: the alternative is
     * nested re-entry until the supervisor stack dies). */
    if (atomic_load_explicit(&kbd_in_service, memory_order_relaxed))
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
    uint64_t next_diag_us;
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
    /* ESC toggles the capture. F11 was the first choice but ST keyboards
     * have no F11 - the USB scancode table maps it to nothing, so the
     * event died before reaching this function. ESC (ST scancode $01)
     * always arrives. The cost: while captured, ESC itself never reaches
     * the game - release and re-top the window if a game needs it. */
    if (scan == 0x01 /* ESC */ && stbox_running() && stbox_get_focus())
    {
        if (pressed)
        {
            stbox_route_enabled = !stbox_route_enabled;
            fprintf(stderr, "[STBOX] input routing %s\n",
                    stbox_route_enabled ? "ON (ESC releases)" : "OFF");
        }
        return;
    }
    if (stbox_wants_input())
    {
        stbox_key_event(scan, pressed);
        return;
    }
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
    if (stbox_wants_input())
    {
        /* forward accumulated motion and button changes, then clear the
         * accumulators so nothing leaks to the main machine when focus
         * returns. Idle flushes send nothing - each ring entry becomes an
         * IKBD F8 packet and the box's 7812.5 baud serial pace is easy to
         * flood. */
        static uint8_t stbox_last_buttons;
        if (in_state.dx || in_state.dy ||
            in_state.buttons != stbox_last_buttons)
        {
            stbox_mouse_rel(in_state.dx, in_state.dy, in_state.buttons);
            stbox_last_buttons = (uint8_t)in_state.buttons;
            in_state.dx = in_state.dy = 0;
        }
        return;
    }
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

    /* Scale host counts down to something ST-like. A real ST mouse is
     * ~200 CPI; modern optical mice are 800-1600 and will happily produce
     * more motion per second than a 7812.5 bps IKBD link can carry. The
     * remainder is carried, so slow movement stays pixel-accurate. */
    const int div = kbd_usb_mouse_div > 0 ? kbd_usb_mouse_div : 1;

    /* Cap carried motion. Without this a fast swipe banks thousands of
     * counts that then dribble out over seconds. */
    if (in_state.dx >  MOUSE_CARRY_CLAMP) in_state.dx =  MOUSE_CARRY_CLAMP;
    if (in_state.dx < -MOUSE_CARRY_CLAMP) in_state.dx = -MOUSE_CARRY_CLAMP;
    if (in_state.dy >  MOUSE_CARRY_CLAMP) in_state.dy =  MOUSE_CARRY_CLAMP;
    if (in_state.dy < -MOUSE_CARRY_CLAMP) in_state.dy = -MOUSE_CARRY_CLAMP;

    while (in_state.dx / div || in_state.dy / div ||
           in_state.buttons != in_state.last_sent_buttons)
    {
        /* Bounded queue. The old test let the ring fill to ~1016 bytes -
         * 1.3 SECONDS of backlogged motion at the IKBD byte rate, which
         * showed up as the pointer lagging then catching up in lurches.
         * Holding only ~2 packets in flight keeps latency near 8ms and
         * lets unsent motion coalesce into the next packet instead. */
        if (ring_used() >= MOUSE_MAX_QUEUE)
            break;

        int sx = in_state.dx / div, sy = in_state.dy / div;
        if (sx > 127)  sx = 127;
        if (sx < -128) sx = -128;
        if (sy > 127)  sy = 127;
        if (sy < -128) sy = -128;
        in_state.dx -= sx * div;      /* keep the sub-division remainder */
        in_state.dy -= sy * div;
        in_state.last_sent_buttons = in_state.buttons;

        if (!y0top)
            sy = -sy;

        uint8_t pkt[3];
        pkt[0] = (uint8_t)(0xF8 | (in_state.buttons & 0x03));
        pkt[1] = (uint8_t)(int8_t)sx;
        pkt[2] = (uint8_t)(int8_t)sy;
        ring_push_packet(pkt, 3);
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

    if (ev->code == KEY_F11)                     /* floppy A eject/insert */
    {                                            /* (no F11 on an ST, so  */
        if (pressed)                             /* the key is free)      */
            fdd_toggle_disk(0);
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
    in_state.next_diag_us  = now_us() + 1000000;

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

        if (t >= in_state.next_diag_us)
        {
            kbd_usb_diag_tick();
            in_state.next_diag_us = t + 1000000;
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
