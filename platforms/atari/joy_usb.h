/*
 * joy_usb.h - USB/Bluetooth game controllers as Atari joysticks
 *
 * "usb gamepad" in the cfg. Xbox (and any other evdev gamepad) pads on the
 * Pi become the ST's joysticks: pad 0 is joystick 1 (the game port, IKBD
 * $FF packets) and STE enhanced joypad A; pad 1 is joystick 0 (the mouse
 * port, $FE) and joypad B. Joystick only - the pad never drives the GEM
 * pointer.
 *
 * The evdev reading lives in kbd_usb.c's input thread (it already owns
 * /dev/input, hotplug and the ACIA injection ring); this module owns the
 * pad state, the mapping and the STE port register model.
 *
 * Threading:
 *   input thread : joy_usb_dev_*, joy_usb_handle_event, joy_usb_tick
 *   CPU thread   : joy_usb_state, joy_usb_ste_*, joy_usb_real_rx_filter
 */
#ifndef JOY_USB_H
#define JOY_USB_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct input_event;

/* Master enable ("usb gamepad"). Pads are only opened when set. */
extern bool JOY_USB_enabled;

#define JOY_USB_MAX_PADS 2

/* ST joystick byte: bits 0-3 up/down/left/right, bit 7 fire */
#define STJOY_UP    0x01
#define STJOY_DOWN  0x02
#define STJOY_LEFT  0x04
#define STJOY_RIGHT 0x08
#define STJOY_FIRE  0x80

/* STE joypad extra buttons (Jaguar-pad layout, see Hatari joy.c) */
#define STPAD_A      0x01
#define STPAD_B      0x02
#define STPAD_C      0x04
#define STPAD_OPTION 0x08
#define STPAD_PAUSE  0x10

/* ---- input thread --------------------------------------------------- */
/* Is this evdev node a gamepad? keybits/absbits are the EVIOCGBIT masks
 * the caller already fetched. */
int  joy_usb_is_gamepad(const unsigned long *evbits,
                        const unsigned long *keybits,
                        const unsigned long *absbits);
/* Claim a pad slot for an open fd (reads the axis ranges). Returns the
 * pad index, or -1 when both slots are taken. */
int  joy_usb_dev_open(int fd, const char *name);
void joy_usb_dev_close(int pad);
void joy_usb_handle_event(int pad, const struct input_event *ev);
/* Periodic (every input-thread tick): emit IKBD packets for changed
 * state. Hook provided by kbd_usb.c; see joy_usb_emit_hooks. */
void joy_usb_tick(void);

/* What kbd_usb.c provides for emission. joy_usb.c never touches the ring
 * directly, so all IKBD-mode gating stays in one place (kbd_usb.c). */
typedef struct {
    /* Send a joystick event for ST port (0 = mouse port, 1 = game port)
     * with the given state byte and the STE pad extras (STPAD_*). Return
     * 1 if it was accepted (so the pad's "last sent" state advances), 0
     * to retry next tick. */
    int (*send_joy)(int st_port, uint8_t state, uint8_t pad_buttons);
    /* Pad 0's fire while the IKBD mouse is on = right mouse button. */
    void (*set_joy_rbutton)(int down);
    /* Start button -> Space bar tap (ST scancode $39). */
    void (*send_key)(uint8_t st_scan, int pressed);
} joy_usb_emit_hooks;
void joy_usb_set_hooks(const joy_usb_emit_hooks *h);

/* The IKBD (re)entered joystick event mode ($14): a real IKBD forgets its
 * previous joystick state, so held directions are sent again. Any thread. */
void joy_usb_resend(void);

/* ---- CPU thread ----------------------------------------------------- */
/* Current ST joystick byte for a port (0 = mouse port, 1 = game port). */
uint8_t joy_usb_state(int st_port);
/* Number of pads attached (for status displays). */
int     joy_usb_count(void);

/* Real-IKBD byte on its way to the main guest: merge pad state into a
 * joystick-interrogation reply ($FD j0 j1). Keeps its own packet framing;
 * call it with EVERY real byte the main guest receives, in order. */
uint8_t joy_usb_real_rx_filter(uint8_t v);

/* STE enhanced joypad ports, $FF9200 (buttons) / $FF9202 (directions and
 * column select). The real chips answer on an STE - merge our pads into
 * what came back (active low). `a` is the folded 24-bit address, `size`
 * 1/2/4, `real` the bus value. On a plain ST the bus errors before this
 * is reached, so no machine-type switch is needed. */
uint32_t joy_usb_ste_read(uint32_t a, int size, uint32_t real);
void     joy_usb_ste_write(uint32_t a, int size, uint32_t v);
static inline int joy_usb_ste_addr(uint32_t a)
{
    return (a & 0x00FFFFFCu) == 0x00FF9200u;
}

#ifdef __cplusplus
}
#endif

#endif /* JOY_USB_H */
