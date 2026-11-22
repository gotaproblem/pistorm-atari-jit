/* everything else kbd_usb.c links against, as nothing */
#include <stdint.h>
volatile int pst_dbg_ikbd, pst_mouse_scale = 1, pst_mouse_thresh;
#define Z(n) int n(void) { return 0; }
Z(fdd_toggle_disk) Z(joy_usb_count) Z(joy_usb_dev_close) Z(joy_usb_dev_open)
Z(joy_usb_handle_event) Z(joy_usb_is_gamepad) Z(joy_usb_monitor_set)
Z(joy_usb_resend) Z(joy_usb_set_hooks) Z(joy_usb_state) Z(joy_usb_tick)
Z(mfp_hub_register_edge) Z(ps_write_8) Z(psdos_ikbd_byte) Z(psdos_key_linux)
Z(psdos_key_st) Z(psdos_mouse) Z(psdos_pad) Z(psdos_pads_wanted)
Z(psdos_set_capture) Z(psdos_view_open) Z(psdos_wants_input) Z(stbox_get_focus)
Z(stbox_ikbd_byte) Z(stbox_joypad_event) Z(stbox_key_event) Z(stbox_log_verbose)
Z(stbox_mouse_rel) Z(stbox_note_route) Z(stbox_running)
uint8_t joy_usb_real_rx_filter(uint8_t v) { return v; }
uint8_t ps_acia_kbd_read8(uint32_t a) { (void)a; return 0; }
