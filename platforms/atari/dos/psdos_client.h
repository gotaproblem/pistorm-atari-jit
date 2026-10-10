// SPDX-License-Identifier: MIT
//
// PSDOS - the emulator's side of DOS games (psdos-design.md).
//
// psdos, a separate process, runs DOSBox Pure and puts each frame, scaled
// to the guest's view and in the guest's pixel format, in a shared-memory
// surface, and its sound in a ring beside it. This file is the client: a
// connector thread on core 1 owns the socket and drains the sound into the
// SDL3 device; the NatFeat handler on the 68k thread only pushes command
// records, reads state words, or copies pixels; and kbd_usb.c hands it the
// real keyboard, mouse and joysticks while the DOS window has captured them.
// Nothing here blocks the CPU thread.
#ifndef PSDOS_CLIENT_H
#define PSDOS_CLIENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSDOS_API_VERSION 1

/* results */
#define PSDOS_OK        0
#define PSDOS_ERR      (-1)
#define PSDOS_BUSY     (-3)   /* command ring full, ask again          */
#define PSDOS_NOTCONN  (-4)   /* psdos is not running or not connected */

/* NatFeat sub-operations (low 20 bits of the id), psdos-design.md 3.3 */
enum psdos_subop {
  PSDOS_VERSION   = 0,   /* -> API version                                  */
  PSDOS_STATUS    = 1,   /* -> PSDOS_ST_* bits                              */
  PSDOS_VIEW_NEW  = 2,   /* p0 w, p1 h, p2 bpp -> 1 (poll STATUS view bit)  */
  PSDOS_VIEW_FREE = 3,
  PSDOS_VIEW_SIZE = 4,   /* p0 w, p1 h                                       */
  PSDOS_STATE     = 5,   /* p0 bits: 1 visible, 2 focused                    */
  PSDOS_LOAD      = 6,   /* p0 GEMDOS path (HOSTFS) or 0/"" for the prompt   */
  PSDOS_POLL      = 7,   /* p0 ptr to 32-byte BE state block                 */
  PSDOS_FETCH     = 8,   /* p0 TT-RAM dest, p1 bytes per row, p2 rows,
                          * p3 ptr to x,y,w,h (4 longs) -> 1 copied, 0 none  */
  PSDOS_KEY       = 9,   /* p0 down, p1 ST scancode (GEM fallback)           */
  PSDOS_CAPTURE   = 10,  /* p0 1/0: route the real input to DOS              */
  PSDOS_GETSTR    = 11,  /* p0 which, p1 buf, p2 len -> bytes                */
  PSDOS_RESET     = 12,
  PSDOS_OPTION    = 13   /* p0 "dosbox_pure_x=value"                         */
};

/* STATUS bits */
#define PSDOS_ST_SOCKET   0x01
#define PSDOS_ST_CONN     0x02
#define PSDOS_ST_VIEW     0x04
#define PSDOS_ST_LOADED   0x08
#define PSDOS_ST_CAPTURE  0x10
#define PSDOS_ST_NOCORE   0x20
#define PSDOS_ST_FAILED   0x40
#define PSDOS_ST_EXITED   0x80

/* GETSTR selectors */
#define PSDOS_STR_TITLE   0
#define PSDOS_STR_STATUS  1

/* the 32-byte block POLL writes, eight big-endian longs */
struct psdos_pollstate {
  uint32_t frame_serial;
  uint32_t flags;          /* PSDOS_FL_* from psdos_proto.h                 */
  uint32_t fps_x100;
  uint32_t src_w, src_h;
  uint32_t capture;        /* 1 while the real input goes to DOS            */
  uint32_t title_serial;
  uint32_t capture_serial; /* bumps on every capture change, either way     */
};

/* ---- NatFeat handler (CPU thread). All return promptly. ---- */
int  psdos_status(void);
int  psdos_cmd(uint32_t type, int32_t a, int32_t b, int32_t c, const char *str);
int  psdos_poll(struct psdos_pollstate *out);
int  psdos_fetch(uint8_t *dst, uint32_t dst_stride, uint32_t dst_rows, int32_t rect[4]);
int  psdos_getstr(int which, char *out, int len);
void psdos_set_capture(int on);
void psdos_set_focus(int focused);
void psdos_key_st(uint8_t scan, int down);        /* also the GEM fallback */

/* ---- input routing (kbd_usb.c, CPU and input threads) ---- */
int  psdos_wants_input(void);       /* connected, view, focused, captured */
void psdos_key_linux(unsigned code, int down);    /* evdev KEY_* */
void psdos_mouse(int dx, int dy, int st_buttons); /* ST bits: 2 left, 1 right */
void psdos_joy(int st_port, uint8_t st_state, uint8_t stpad);
void psdos_ikbd_byte(uint8_t v);    /* one raw real-IKBD byte, packets whole */

void psdos_shutdown(void);
/* for the PSCTRL sampler: 0 none / 1 socket / 2 connected / 3 view / 4 running */
void psdos_stats(uint32_t *state, uint32_t *frames, uint32_t *fps_x100);

#ifdef __cplusplus
}
#endif
#endif /* PSDOS_CLIENT_H */
