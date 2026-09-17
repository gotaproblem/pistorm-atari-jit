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

/* Restore whatever the console had and let go of DRM. */
void sh_close(void);

#ifdef __cplusplus
}
#endif

#endif
