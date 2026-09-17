/*
 * setup_input.h - keys for the pre-boot setup page, from all three
 * sources: the real ST keyboard (ACIA/IKBD over the bus), a USB keyboard
 * and a USB gamepad (evdev on the Pi).
 *
 * The 68k is halted while this runs, so the ST keyboard is read by the
 * Pi directly. TOS resets the ACIA and the IKBD itself at boot, so the
 * state left behind here does not matter to the guest.
 */
#ifndef SETUP_INPUT_H
#define SETUP_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

enum si_key {
    SI_NONE = 0,
    SI_UP, SI_DOWN, SI_LEFT, SI_RIGHT,
    SI_ENTER, SI_ESC, SI_TAB, SI_BACKSPACE, SI_SPACE,
    SI_HOME, SI_INSERT, SI_DELETE,
    SI_F1, SI_F2, SI_F3, SI_F4, SI_F5,
    SI_F6, SI_F7, SI_F8, SI_F9, SI_F10,
    SI_CHAR                       /* printable: see si_event.ch */
};

enum si_source { SI_SRC_ST = 0, SI_SRC_USB, SI_SRC_PAD };

struct si_event {
    enum si_key    key;
    enum si_source src;
    char           ch;            /* SI_CHAR only, already shifted */
};

/* want_st: read the ST keyboard too (resets the ACIA and the IKBD).
 * grab: take the USB devices off the Linux console while the page is up.
 * Returns the number of sources opened, or -1. */
int  si_open(int want_st, int grab);

/* Next key, or SI_NONE after timeout_ms with nothing pressed. */
struct si_event si_poll(int timeout_ms);

/* Release the USB devices. The ACIA is left as TOS expects to find it. */
void si_close(void);

/* Print every device found and every raw event, to find out why a pad
 * is not answering. */
void si_set_debug(int on);

/* The devices si_open() kept, for the page and for diagnostics. */
int         si_device_count(void);
const char *si_device_name(int i, int *is_pad);

/* What si_open() found, for the page's status line. */
int  si_have_st(void);
int  si_usb_keyboards(void);
int  si_gamepads(void);

const char *si_key_name(const struct si_event *e, char *buf, unsigned long n);

/* Exposed for the host harness: the two scancode maps and the stick
 * hysteresis (-1 / 0 / 1 from a raw axis value). */
int si_stick_state(int cur, int value, int centre, int on, int off);
struct si_event si_map_st(unsigned char scancode, int shift);
struct si_event si_map_evdev(int code, int shift);

#ifdef __cplusplus
}
#endif

#endif
