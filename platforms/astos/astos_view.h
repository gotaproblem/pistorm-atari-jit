/*
 * astos_view.h - looking at a file from ASTOS's file manager.
 *
 * Text (wrapped at the window's width, tabs every 8), hex (16 bytes a line,
 * read from the file as it scrolls, so any size), and ST pictures: Degas
 * .PI1/.PI2/.PI3, Degas Elite compressed .PC1/.PC2/.PC3 and NEOchrome .NEO.
 *
 * A picture takes the whole screen. A medium-resolution picture on a
 * colour monitor and a high-resolution one on a mono monitor are shown
 * exactly, palette and all. The rest are made to fit what ASTOS's screen
 * is: low resolution is doubled across and its colours brought down to the
 * four the medium screen has; high resolution on colour folds each pair of
 * rows into black, grey and white; colour on mono is dithered.
 */
#ifndef ASTOS_VIEW_H
#define ASTOS_VIEW_H

#include <stdint.h>
#include "setup_input.h"

/* 0, or -1 and a reason in why */
int  view_open(const char *path, const char *title, char *why, int why_len);
void view_close(void);
int  view_is_open(void);
int  view_is_picture(void);

/* text or hex, inside the rectangle (logical pixels) */
void view_draw(int x, int y, int w, int h);
/* 1 = closed */
int  view_key(struct si_event k);
int  view_wheel(int n);

/* the picture as the screen holds it: planar medium (2 planes, 32000
 * bytes) or mono (640x400, 32000 bytes), and the four pens ($0RGB) for
 * colour. NULL when there is no picture. */
const uint8_t *view_screen(int mono, uint16_t pal[4]);

#endif
