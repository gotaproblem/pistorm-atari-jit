/*
 * setup_hdmi.h - mirror the setup page onto HDMI.
 *
 * The page draws on the real shifter, which is invisible on a machine
 * wired to HDMI only, so the same 80x25 page goes out through DRM as
 * well. This is its own small KMS client rather than the ET4000 path:
 * the page runs before the .cfg is read, so nothing about the guest's
 * video is set up yet, and everything here is released before the
 * emulator opens DRM for real.
 */
#ifndef SETUP_HDMI_H
#define SETUP_HDMI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "shifter_setup.h"

/* 0 if a connected display was found and the mode is set. */
int  sh_open(void);
int  sh_active(void);

/* Put the page's shadow on the screen, scaled by a whole number and
 * centred on black. */
void sh_present(const struct ss_screen *ss);

/* The four pens as 0xRRGGBB (ASTOS; the page uses the defaults). */
void sh_set_palette(const uint32_t rgb[4]);

/* Colour beyond the Shifter's pens, on HDMI only (ASTOS: a green OK, a
 * red UV in the taskbar). Inside each rectangle - in the screen's own
 * pixels, x 0..639, y 0..199 or 0..399 - pixels of pen `pen` (0-3, as
 * sh_present indexes the palette; mono ink is 3) are drawn `rgb`
 * (0xRRGGBB). n = 0 clears them. At most SH_MAX_TINT. */
#define SH_MAX_TINT 8
struct sh_tint { int x, y, w, h, pen; uint32_t rgb; };
void sh_set_tints(const struct sh_tint *t, int n);

/* Restore whatever the console had and let go of DRM. */
void sh_close(void);

#ifdef __cplusplus
}
#endif

#endif
