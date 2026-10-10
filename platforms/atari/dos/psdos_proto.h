// SPDX-License-Identifier: MIT
//
// psdos_proto.h - the wire between the emulator and the psdos DOS service.
//
// Two processes on the same arm64 Pi: the emulator (its NatFeat handler on
// the 68k thread, a connector thread on core 1) and psdos (DOSBox Pure as a
// libretro core, core 1). They share
//
//   - a Unix socket: fixed-size command records emulator -> psdos, and event
//     records psdos -> emulator, each optionally followed by a string;
//   - one shared-memory object (a memfd passed with HELLO) holding the view
//     surface - the latest frame scaled to the view, in the guest's pixel
//     format - and an audio ring of int16 stereo frames.
//
// The surface follows psweb's rule: psdos writes a frame only once the
// emulator has consumed the last (serial == consumed), so there is never
// more than one in flight and the DOS machine never waits for the guest.
// The audio ring is single-producer (psdos) / single-consumer (emulator);
// psdos overwrites when it is full and the reader skips forward.
//
// Native little-endian throughout: both ends are the same machine. The
// guest-facing byte order lives in the NatFeat handler and in the pixels,
// which are already in the guest's format.
//
// See psdos-design.md.
#ifndef PSDOS_PROTO_H
#define PSDOS_PROTO_H

#include <stdint.h>

#define PSDOS_PROTO_VERSION   1
#define PSDOS_SOCK_DEFAULT    "/tmp/psdos.sock"         /* psdos run by hand; env PSDOS_SOCK */
#define PSDOS_SOCK_SYSTEMD    "/run/psdos/psdos.sock"   /* psdos.socket (install-full.sh DOS step) */
#define PSDOS_STR_MAX         1024
#define PSDOS_MAX_W           1920
#define PSDOS_MAX_H           1080
#define PSDOS_SHM_MAGIC       0x534F4450u               /* "PDOS" */
#define PSDOS_AUDIO_FRAMES    16384u                    /* stereo frames, power of two */

/* ------------------------------------------------ emulator -> psdos -- */
enum psdos_cmd_type {
  PSDOS_CMD_HELLO = 1,   /* a = protocol version                              */
  PSDOS_CMD_VIEW_NEW,    /* a = w, b = h, c = bpp (16|32)                     */
  PSDOS_CMD_VIEW_FREE,   /* stop and unload the game, drop the view           */
  PSDOS_CMD_VIEW_SIZE,   /* a = w, b = h                                      */
  PSDOS_CMD_STATE,       /* a = bit0 visible, bit1 focused; !visible = paused */
  PSDOS_CMD_LOAD,        /* str = host path, or none for the DOS prompt       */
  PSDOS_CMD_KEY,         /* a = 1 down 0 up, b = RETROK_* code                */
  PSDOS_CMD_MOUSE,       /* a = dx, b = dy, c = buttons (1 L, 2 R, 4 M)        */
  PSDOS_CMD_JOY,         /* a = port 0/1, b = RETRO_DEVICE_ID_JOYPAD bit mask  */
  PSDOS_CMD_RESET,
  PSDOS_CMD_OPTION,      /* str = "dosbox_pure_x=value"                       */
  PSDOS_CMD_KEYS_UP,     /* release every held key/button (capture dropped)   */
  PSDOS_CMD_QUIT,
  PSDOS_CMD_PAD          /* a whole pad: a = port 0/1, b = JOYPAD bit mask,
                          * c = lx<<16 | (ly & 0xffff), d = rx<<16 | (ry & 0xffff),
                          * e = l2<<16 | r2 (0..32767); sticks -32768..32767 */
};

struct psdos_cmd {
  uint32_t type;
  int32_t  a, b, c, d, e, f;
  uint32_t len;          /* bytes of string that follow, 0 if none         */
};

/* ------------------------------------------------ psdos -> emulator -- */
enum psdos_evt_type {
  PSDOS_EVT_HELLO = 1,   /* a = protocol version, b = shm bytes; fd in SCM_RIGHTS */
  PSDOS_EVT_VIEW,        /* a = 1 view ready, 0 refused                     */
  PSDOS_EVT_FLAGS,       /* a = PSDOS_FL_* bits                             */
  PSDOS_EVT_TITLE,       /* str = what is loaded                            */
  PSDOS_EVT_STATUS       /* str = the last thing worth saying (errors)      */
};

#define PSDOS_FL_LOADED    0x0001   /* a game (or the bare prompt) is running */
#define PSDOS_FL_PAUSED    0x0002   /* view not visible: core held            */
#define PSDOS_FL_NOCORE    0x0004   /* dosbox_pure_libretro.so not loadable   */
#define PSDOS_FL_FAILED    0x0008   /* retro_load_game refused the content    */
#define PSDOS_FL_EXITED    0x0010   /* the core asked to shut down            */

struct psdos_evt {
  uint32_t type;
  int32_t  a, b;
  uint32_t len;
};

/* ------------------------------------------------ shared memory ------ */
struct psdos_shm_hdr {
  uint32_t magic;
  uint32_t version;
  uint32_t size;             /* whole object                               */
  uint32_t surface_off;      /* struct psdos_surface                       */
  uint32_t pixels_off;       /* its pixels                                 */
  uint32_t pixels_bytes;     /* PSDOS_MAX_W * PSDOS_MAX_H * 4              */
  uint32_t audio_off;        /* struct psdos_audio                         */
  uint32_t audio_bytes;
};

struct psdos_surface {
  uint32_t w, h;             /* current view size                          */
  uint32_t bpp;              /* 16 or 32, the guest's format               */
  uint32_t stride;           /* bytes per row in pixels[]                  */
  volatile uint32_t serial;    /* psdos: ++ after a frame is in place        */
  volatile uint32_t consumed;  /* emulator: = serial after copying it out    */
  int32_t  damage[4];        /* x y w h, view pixels                       */
  uint32_t fps_x100;         /* frames the core produced per second x100   */
  uint32_t src_w, src_h;     /* the DOS screen before scaling              */
  uint32_t frames_dropped;   /* core frames collapsed while one waited     */
  uint32_t core_us;          /* last retro_run, microseconds               */
};

struct psdos_audio {
  volatile uint32_t rate;    /* Hz, 0 = nothing yet                        */
  volatile uint32_t wpos;    /* frames written (free-running)              */
  volatile uint32_t rpos;    /* frames read (free-running, emulator)       */
  uint32_t overruns;         /* times the writer lapped the reader         */
  int16_t  data[PSDOS_AUDIO_FRAMES * 2];
};

#endif /* PSDOS_PROTO_H */
