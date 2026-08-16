/*
 * kbd_usb.h - USB/Bluetooth keyboard & mouse injection into the ST ACIA/IKBD
 *
 * Merges host input (Linux evdev - USB and Bluetooth HID both surface there)
 * into the keyboard ACIA RX stream seen by the guest, while keeping the real
 * IKBD fully functional. The real 6850/MFP stay in charge of all real
 * traffic; injected bytes are presented by shadowing ACIA/GPIP reads and
 * synthesising the MFP GPIP4 (vector 0x46) interrupt in the 68k core.
 *
 * Threading contract:
 *   producer  : kbd_usb input thread (evdev -> IKBD byte packets)
 *   consumer  : CPU thread (ACIA read shadows pop bytes)
 *   observer  : ipl_task thread (kbd_usb_irq_wanted) - read-only
 */

#ifndef KBD_USB_H
#define KBD_USB_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Master enable, set from config ("kbd usb").  Checked by every hook so the
 * whole feature is dead code when disabled. */
extern bool KBD_USB_enabled;

/* Real-IKBD handling: 0 = auto-detect (default), 1 = always merge with the
 * real IKBD, 2 = always quarantine it (USB/Bluetooth only).
 *
 * Auto-detect exists because an ST with the keyboard unplugged leaves the
 * 6850's RX pin floating: noise frames as garbage bytes with framing/overrun
 * errors, which TOS turns into nonsense scancodes and a constant bell, and
 * which also starve the injected stream. When the real receiver looks noisy
 * or dead it is quarantined - drained (clearing its IRQ, so the real GPIP4
 * interrupt storm stops) and hidden from the guest. Clean real traffic
 * reappearing switches back to merging, so plugging the ST keyboard back in
 * works without a restart. */
extern int kbd_usb_force_mode;

/* True while the real IKBD is trusted and being merged. */
int kbd_usb_real_ikbd_present(void);

/* ---- lifecycle ------------------------------------------------------- */
/* grab != 0: EVIOCGRAB input devices so keystrokes stop reaching the Pi
 * console. F12 toggles the grab at runtime (F11/F12 don't exist on an ST). */
int  kbd_usb_init(int grab);
void kbd_usb_shutdown(void);

/* ---- CPU-thread ACIA shadow ------------------------------------------ */
/* True when an injected byte is presented (pacing + real-traffic gaps
 * respected) and the guest should see RDRF|IRQ on the keyboard ACIA. */
int kbd_usb_rx_ready(void);

/* True while we are mid-way through delivering a multi-byte injected packet;
 * injected bytes then take priority over real RX so packets never interleave. */
int kbd_usb_rx_priority(void);

/* Pop the presented byte (only after kbd_usb_rx_ready()/_rx_priority()). */
uint8_t kbd_usb_rx_read(void);

/* Guest wrote to the keyboard ACIA data register ($FFFC02): snoop the IKBD
 * command stream so injected traffic follows the current IKBD mode. The
 * write itself must still go to the real bus. */
void kbd_usb_tx_snoop(uint8_t v);

/* Guest wrote to the keyboard ACIA control register ($FFFC00). */
void kbd_usb_ctrl_snoop(uint8_t v);

/* Filter a control-register write on its way to the real ACIA. While the
 * real receiver is quarantined this clears RIE (bit 7) so the chip stops
 * driving its IRQ output - otherwise every noise byte still raises a real
 * level 6 and TOS burns the emulated CPU servicing dead interrupts, which
 * shows up as stuttering. Returns the value to actually put on the bus. */
uint8_t kbd_usb_ctrl_filter(uint8_t v);

/* Host mouse-count divisor, "kbd usb mousediv N" (default 1). Modern mice
 * are 800-1600 CPI against the ST's ~200, and can generate more motion per
 * second than the 7812.5 bps IKBD link can carry. */
extern int kbd_usb_mouse_div;

/* A real IKBD byte was passed through to the guest: hold off starting new
 * injected packets for ~2 byte-times so we never split a real packet. */
void kbd_usb_note_real_rx(void);

/* Snoop guest MFP register writes (IERB $FFFA09 / IMRB $FFFA15) so injected
 * interrupts respect the guest's keyboard interrupt mask. Call with the raw
 * write; handles byte and word (register byte is on the odd address). */
void kbd_usb_mfp_snoop(uint32_t addr, uint32_t value, int is_word);

/* ---- shared register shims ------------------------------------------- */
/* Merge injected state into a just-read real register value. status/data
 * are for the keyboard ACIA ($FFFC00/$FFFC02); gpip for MFP $FFFA01. */
uint8_t kbd_usb_acia_status_shim(uint8_t real);
uint8_t kbd_usb_acia_data_shim(void);
uint8_t kbd_usb_gpip_shim(uint8_t real);

/* ---- ipl_task / IACK ------------------------------------------------- */
/* True when a level-6 interrupt should be raised for injected input
 * (byte presented AND guest MFP mask allows keyboard interrupts). */
int kbd_usb_irq_wanted(void);

/* Diagnostic counters (read-only) */
extern volatile uint32_t kbd_usb_stat_injected_bytes;
extern volatile uint32_t kbd_usb_stat_virtual_iacks;
extern volatile uint32_t kbd_usb_stat_dropped_bytes;

/* ------------------------------------------------------------------ */
/* Native mouse threshold (PISTORM_MOUSE_THRESH), usable WITHOUT USB   */
/* injection - see the long comment in kbd_usb.c. When "kbd usb" is    */
/* enabled the equivalent hooks live inside the USB shims instead, so  */
/* callers should reach for these only on the non-USB branch.          */
/* These MUST stay inside the extern "C" block: kbd_usb.c is built as  */
/* C, while emulator.c and pistorm_natmem.cpp - which call them - are  */
/* built as C++, so without it the callers emit mangled symbols and    */
/* the link fails.                                                     */
/* ------------------------------------------------------------------ */
int     kbd_native_mouse_enabled(void);   /* threshold configured?     */
void    kbd_native_tx_snoop(uint8_t v);   /* guest -> IKBD byte        */
uint8_t kbd_native_rx_filter(uint8_t v);  /* IKBD -> guest byte        */

#ifdef __cplusplus
}
#endif

#endif /* KBD_USB_H */
