/* SPDX-License-Identifier: MIT
 * stbox_render.h - draw one STBOX scanline capture (stbox.h, SCANLINE
 * CAPTURE) as XRGB8888. Pure memory; the Pi's render thread and the
 * headless harness both use it. */
#ifndef PISTORM_STBOX_RENDER_H
#define PISTORM_STBOX_RENDER_H
#include <stdint.h>
#include "stbox.h"
#ifdef __cplusplus
extern "C" {
#endif
/* dst: XRGB8888, pitch in PIXELS. Colour frames: the display window
 * (cycles 56..376 of each line) at 320 wide, or 640 when any row is
 * medium resolution (low-res pixels doubled); one row per line with
 * vertical display enable. 71 Hz frames: 640 x rows mono. Returns 1 and
 * the size drawn, 0 if there is nothing usable (no rows). */
int stbox_render_cap(const stbox_cap_t *c, int ste, uint32_t *dst,
                     uint32_t pitch, int *w, int *h);
/* x0 = first cycle shown, cycles = how many (320..512). Default 56, 320.
 * Pi: PISTORM_STBOX_VIEW=monitor gives 24, 384 (borders/overscan shown). */
void stbox_render_set_view(int x0, int cycles);
#ifdef __cplusplus
}
#endif
#endif
