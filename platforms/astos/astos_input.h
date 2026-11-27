/*
 * astos_input.h - keys and the pointer for ASTOS, from every source:
 * the real ST keyboard and mouse (IKBD through the ACIA, read over the
 * bus with the 68k halted), USB / Bluetooth keyboards and mice, and
 * gamepads (evdev). Key events reuse the setup page's maps
 * (si_map_st / si_map_evdev / si_map_pad in setup_input.c).
 */
#ifndef ASTOS_INPUT_H
#define ASTOS_INPUT_H

#include "setup_input.h"

enum ai_type { AI_NONE = 0, AI_KEY, AI_MOUSE };

struct ai_event {
    enum ai_type    type;
    struct si_event key;        /* AI_KEY                                   */
    int             dx, dy;     /* AI_MOUSE: movement in the source's units */
    int             buttons;    /* AI_MOUSE: bit 0 left, bit 1 right (state)*/
    int             usb;        /* AI_MOUSE: 1 = a USB/BT mouse             */
};

int  ai_open(int want_st, int grab);
struct ai_event ai_poll(int timeout_ms);
void ai_close(void);

int  ai_have_st(void);
int  ai_keyboards(void);
int  ai_mice(void);
int  ai_pads(void);

#endif
