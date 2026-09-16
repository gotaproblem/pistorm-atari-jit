// SPDX-License-Identifier: MIT
//
// psweb_proto.h - the wire between the emulator and the psweb engine service.
//
// Two processes on the same arm64 Pi: the emulator (root, its NatFeat handler
// on the 68k thread) and psweb (user pistorm, WPE WebKit, core 1). They share
//
//   - a Unix socket: fixed-size command records emulator -> psweb, and
//     event records psweb -> emulator, each optionally followed by a string;
//   - one shared-memory object holding the view surface: the latest frame
//     in the guest's pixel format plus a serial/consumed pair that hands it
//     over. psweb only writes a new frame once the emulator has consumed the
//     last one, so there is never more than one frame in flight and the
//     engine's frame rate follows the guest.
//
// Everything is native little-endian: both ends are the same machine. The
// guest-facing byte order lives in the NatFeat handler, not here.
#ifndef PSWEB_PROTO_H
#define PSWEB_PROTO_H

#include <stdint.h>

#define PSWEB_PROTO_VERSION   1
#define PSWEB_SOCK_DEFAULT    "/tmp/psweb.sock"   /* psweb run by hand; env PSWEB_SOCK overrides */
#define PSWEB_SOCK_SYSTEMD    "/run/psweb/psweb.sock"   /* psweb.socket (install-full.sh web step) */
#define PSWEB_SHM_NAME_MAX    64
#define PSWEB_STR_MAX         2048                /* longest string on the wire */
#define PSWEB_DAMAGE_MAX      32
#define PSWEB_MAX_W           1920
#define PSWEB_MAX_H           1080
#define PSWEB_SHM_MAGIC       0x42575350u         /* "PSWB" */

/* ------------------------------------------------ emulator -> psweb -- */
enum psweb_cmd_type {
  PSWEB_CMD_HELLO = 1,   /* a = protocol version                             */
  PSWEB_CMD_VIEW_NEW,    /* a = w, b = h, c = bpp (16|32), d = flags          */
  PSWEB_CMD_VIEW_FREE,
  PSWEB_CMD_VIEW_SIZE,   /* a = w, b = h                                      */
  PSWEB_CMD_VIEW_STATE,  /* a = bits: 1 visible, 2 focused, 4 topped          */
  PSWEB_CMD_LOAD,        /* str = url, host name, or words to search          */
  PSWEB_CMD_NAV,         /* a = 0 back 1 fwd 2 reload 3 reload-nocache 4 stop */
  PSWEB_CMD_POINTER,     /* a = 0 move 1 press 2 release, b = x, c = y,
                          * d = button 1 left 2 right 3 middle, e = kstate    */
  PSWEB_CMD_SCROLL,      /* a = dx, b = dy (steps x 120, or pixels if
                          * e & 1), c = x, d = y, e = flags                   */
  PSWEB_CMD_KEY,         /* a = 1 down 0 up, b = AES key word
                          * (scancode << 8 | ascii), c = kstate               */
  PSWEB_CMD_TEXT,        /* str = text to insert as typed                     */
  PSWEB_CMD_ZOOM,        /* a = percent                                       */
  PSWEB_CMD_SETTING,     /* a = key, b = value, str = string value            */
  PSWEB_CMD_QUIT
};

/* AES kstate bits, as evnt_multi reports them */
#define PSWEB_KS_RSHIFT  0x01
#define PSWEB_KS_LSHIFT  0x02
#define PSWEB_KS_CTRL    0x04
#define PSWEB_KS_ALT     0x08

/* PSWEB_CMD_SETTING keys */
enum psweb_setting {
  PSWEB_SET_JAVASCRIPT = 0,   /* b = 0/1                                     */
  PSWEB_SET_IMAGES     = 1,   /* b = 0/1                                     */
  PSWEB_SET_UA         = 2,   /* b = 0 default, 1 desktop Safari             */
  PSWEB_SET_SEARCH     = 3,   /* str = search url with %s                    */
  PSWEB_SET_HOME       = 4,   /* str                                         */
  PSWEB_SET_ZOOM       = 5,   /* b = default zoom percent                    */
  PSWEB_SET_BLOCKER    = 6    /* b = 0/1 content blocker                     */
};

struct psweb_cmd {
  uint32_t type;
  uint32_t view;
  int32_t  a, b, c, d, e, f;
  uint32_t len;          /* bytes of string that follow, 0 if none        */
};

/* ------------------------------------------------ psweb -> emulator -- */
enum psweb_evt_type {
  PSWEB_EVT_HELLO = 1,   /* a = protocol version, b = shm bytes, str = shm name */
  PSWEB_EVT_VIEW,        /* a = result of VIEW_NEW: view id > 0 or -1       */
  PSWEB_EVT_TITLE,       /* str                                             */
  PSWEB_EVT_URI,         /* str                                             */
  PSWEB_EVT_PROGRESS,    /* a = 0..1000                                     */
  PSWEB_EVT_FLAGS,       /* a = PSWEB_FL_* bits                             */
  PSWEB_EVT_LINK,        /* str = hovered link, "" when none                */
  PSWEB_EVT_CRASHED,     /* the web process died; flags carry it too        */
  PSWEB_EVT_FIND         /* a = match count                                 */
};

/* PSWEB_EVT_FLAGS bits - the guest sees these in POLL */
#define PSWEB_FL_CAN_BACK   0x0001
#define PSWEB_FL_CAN_FWD    0x0002
#define PSWEB_FL_LOADING    0x0004
#define PSWEB_FL_SECURE     0x0008
#define PSWEB_FL_INSECURE   0x0010
#define PSWEB_FL_AUDIO      0x0020
#define PSWEB_FL_CRASHED    0x0040
#define PSWEB_FL_FIND_DONE  0x0080
#define PSWEB_FL_JS_OFF     0x0100
#define PSWEB_FL_BLOCKER    0x0200

struct psweb_evt {
  uint32_t type;
  uint32_t view;
  int32_t  a, b;
  uint32_t len;
};

/* ------------------------------------------------ shared memory ------ */
struct psweb_shm_hdr {
  uint32_t magic;
  uint32_t version;
  uint32_t size;             /* whole object                              */
  uint32_t surface_off;      /* struct psweb_surface                      */
  uint32_t pixels_off;       /* its pixels                                */
  uint32_t pixels_bytes;     /* PSWEB_MAX_W * PSWEB_MAX_H * 4             */
};

struct psweb_surface {
  uint32_t w, h;             /* current view size                         */
  uint32_t bpp;              /* 16 or 32, the guest's format              */
  uint32_t stride;           /* bytes per row in pixels[]                 */
  volatile uint32_t serial;    /* psweb: ++ after a frame is in place       */
  volatile uint32_t consumed;  /* emulator: = serial after copying it out   */
  uint32_t n_damage;
  int32_t  damage[PSWEB_DAMAGE_MAX][4];   /* x y w h, view pixels          */
  uint32_t frames_dropped;   /* engine frames collapsed while one waited  */
  uint32_t paint_us;         /* psweb's convert time for the last frame   */
};

#endif /* PSWEB_PROTO_H */
