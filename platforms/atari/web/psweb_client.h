// SPDX-License-Identifier: MIT
//
// PSWEB - the emulator's side of the web browser (psweb-design.md).
//
// psweb, a separate process, renders pages with WPE WebKit and puts each
// frame in a shared-memory surface in the guest's pixel format. This file
// is the client: a connector thread on core 1 talks to psweb over a Unix
// socket, and the NatFeat handler on the 68k thread only ever pushes a
// command record into a ring, reads a few words of shared state, or copies
// pixels. Nothing here blocks the CPU thread.
#ifndef PSWEB_CLIENT_H
#define PSWEB_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSWEB_API_VERSION 1

/* results */
#define PSWEB_OK        0
#define PSWEB_ERR      (-1)
#define PSWEB_BUSY     (-3)   /* connector holds the lock, ask again  */
#define PSWEB_NOTCONN  (-4)   /* psweb is not running or not connected */

/* NatFeat sub-operations (low 20 bits of the id). Numbering follows
 * psweb-design.md section 4.2 so later additions keep their slots. */
enum psweb_subop {
  PSWEB_VERSION    = 0,   /* -> API version                                */
  PSWEB_STATUS     = 1,   /* -> bit0 installed, bit1 connected, bit2 view  */
  PSWEB_VIEW_NEW   = 2,   /* p0 w, p1 h, p2 bpp, p3 flags -> 1 (poll STATUS bit2) */
  PSWEB_VIEW_FREE  = 3,
  PSWEB_VIEW_SIZE  = 4,   /* p0 view, p1 w, p2 h                            */
  PSWEB_VIEW_STATE = 5,   /* p0 view, p1 bits                               */
  PSWEB_LOAD       = 6,   /* p0 view, p1 text (Atari charset)               */
  PSWEB_NAV        = 7,   /* p0 view, p1 op                                 */
  PSWEB_POLL       = 8,   /* p0 view, p1 ptr to 32-byte BE state block      */
  PSWEB_FETCH      = 9,   /* p0 view, p1 TT-RAM dest, p2 bytes per row,
                           * p3 rows in dest, p4 rect array (x y w h int32),
                           * p5 max rects -> rects copied, 0 = no new frame */
  PSWEB_POINTER    = 10,  /* p0 view, p1 kind, p2 x, p3 y, p4 button, p5 kstate */
  PSWEB_SCROLL     = 11,  /* p0 view, p1 dx, p2 dy, p3 x, p4 y, p5 flags    */
  PSWEB_KEY        = 12,  /* p0 view, p1 down, p2 AES key word, p3 kstate   */
  PSWEB_TEXT       = 13,  /* p0 view, p1 string                             */
  PSWEB_GETSTR     = 14,  /* p0 view, p1 which, p2 buf, p3 len -> bytes     */
  PSWEB_ZOOM       = 19,  /* p0 view, p1 percent                            */
  PSWEB_SETTING    = 24   /* p0 view, p1 key, p2 value, p3 string or 0      */
};

/* PSWEB_GETSTR selectors */
enum psweb_str {
  PSWEB_STR_TITLE   = 0,
  PSWEB_STR_URI     = 1,
  PSWEB_STR_LINK    = 2,   /* hovered link                                  */
  PSWEB_STR_STATUS  = 3,   /* engine status text                            */
  PSWEB_STR_ENGINE  = 6    /* engine version                                */
};

/* the 32-byte block POLL writes, eight big-endian longs */
struct psweb_pollstate {
  uint32_t frame_serial;
  uint32_t n_damage;
  uint32_t progress;       /* 0..1000                                       */
  uint32_t flags;          /* PSWEB_FL_* from psweb_proto.h                 */
  uint32_t cursor;         /* 0 arrow, 1 hand (over a link)                 */
  uint32_t title_serial;
  uint32_t uri_serial;
  uint32_t dialog;         /* 0 in v1                                       */
};

/* Called from the NatFeat handler. All return promptly. */
int  psweb_status(void);
int  psweb_cmd(uint32_t type, int32_t a, int32_t b, int32_t c, int32_t d,
               int32_t e, int32_t f, const char *str_atari);
int  psweb_poll(struct psweb_pollstate *out);
/* copy the damaged rectangles of the current frame into dst (guest format,
 * dst_stride bytes per row, dst_rows rows); rects gets x,y,w,h per copied
 * rectangle; returns the count, 0 when there is no new frame */
int  psweb_fetch(uint8_t *dst, uint32_t dst_stride, uint32_t dst_rows,
                 int32_t *rects, int max_rects);
int  psweb_getstr(int which, char *out_atari, int len);
void psweb_shutdown(void);
/* for the PSCTRL sampler: state 0 none / 1 socket / 2 connected / 3 view,
 * and the free-running frame and byte counters of FETCH */
void psweb_stats(uint32_t *state, uint32_t *frames, uint32_t *bytes);

#ifdef __cplusplus
}
#endif
#endif /* PSWEB_CLIENT_H */
