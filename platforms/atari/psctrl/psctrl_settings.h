// SPDX-License-Identifier: MIT
//
// PSCTRL settings — the self-describing control surface behind PSCTRL.ACC.
//
// The emulator ships a descriptor table; the accessory builds its tabs
// from it. Adding a switch later is one line here and no .ACC rebuild,
// which is the whole point of doing it this way instead of hard-coding
// the dialog on the 68k side.
//
// Sub-op numbering, and where it departs from psctrl-live-settings-design.md:
// the design assumed sub-ops 0-6 were free, but 0, 1 and 2 are already
// PSCTRL_VERSION, PSCTRL_GETINT and PSCTRL_FX (the desk-slide transition)
// in the shipped emulator, and PSMON in the field calls 0 and 1. So the
// settings sub-ops start at 3 and the design doc's numbering is superseded
// by this header.
//
// Values live in one index namespace with the existing read-only status
// indices: 0..95 are the psctrl.h status/config reads, and settings start
// at PS_SET_BASE. A setting's index is simply its position in the table,
// so PS_GETINT(PS_SET_BASE + i) reads back exactly what PS_SETINT(PS_SET_BASE
// + i, v) wrote, and the accessory needs no compiled-in index constants.

#ifndef PSCTRL_SETTINGS_H
#define PSCTRL_SETTINGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSCTRL_SET_API_VERSION 1

/* Sub-operations, continuing psctrl_subop in psctrl.h (0 VERSION,
 * 1 GETINT, 2 FX). */
enum psctrl_set_subop {
  PSCTRL_GETSTR   = 3,   /* p0 idx, p1 buf, p2 len  -> length written    */
  PSCTRL_SETINT   = 4,   /* p0 idx, p1 value        -> PS_R_*            */
  PSCTRL_COUNT    = 5,   /*                         -> number of settings */
  PSCTRL_DESCRIBE = 6,   /* p0 idx, p1 buf, p2 len  -> bytes written      */
  PSCTRL_SAVE     = 7,   /* p0 which (0 = .cfg)     -> 0 ok, -1 failed    */
  PSCTRL_SETSTR   = 8,   /* p0 idx, p1 ptr          -> PS_R_*             */
  PSCTRL_ACTION   = 9,   /* p0 idx, p1 arg          -> PS_R_* or a value  */
  PSCTRL_LIST     = 10,  /* p0 which, p1 i, p2 buf, p3 len -> len; i<0 =
                          * rescan and return the count                   */
  PSCTRL_SETAPI   = 11   /*                         -> settings API version */
};

/* PS_SETINT / PS_SETSTR / PS_ACTION results */
enum psctrl_result {
  PS_R_OK       =  0,    /* applied now                                   */
  PS_R_DEFER    =  1,    /* accepted; lands at the next JIT block boundary */
  PS_R_RESTART  =  2,    /* accepted into the shadow config; needs a boot  */
  PS_R_BOXBOOT  =  3,    /* accepted; takes effect on the next box start   */
  PS_R_REJECT   = -1,    /* out of range, unknown index, or refused        */
  PS_R_BUSY     = -2     /* not now (a floppy swap during FDC activity)    */
};

/* Where the accessory puts the control */
enum psctrl_tab {
  PS_TAB_JIT = 0, PS_TAB_CPU, PS_TAB_VIDEO, PS_TAB_AUDIO, PS_TAB_INPUT,
  PS_TAB_STBOX, PS_TAB_FLOPPY, PS_TAB_NET, PS_TAB_DEBUG, PS_TAB_ADV,
  PS_TAB_N
};

/* What kind of control to draw */
enum psctrl_kind {
  PS_K_BOOL = 0,         /* two radios / a switch                         */
  PS_K_ENUM,             /* radios if nenum <= 4, else a popup            */
  PS_K_INT,              /* slider, min..max step                         */
  PS_K_STR,              /* a text field; read with GETSTR                */
  PS_K_ACTION,           /* a button; PS_ACTION does it                   */
  PS_K_INFO              /* read-only readout                             */
};

/* How it applies — the badge on every control, enforced by PS_SETINT and
 * not merely drawn */
enum psctrl_class {
  PS_C_LIVE = 0,         /* the host re-reads it; effective at once       */
  PS_C_DEFER,            /* parked, applied at the next block boundary    */
  PS_C_BOOT,             /* machine shape; needs a restart                */
  PS_C_BOXBOOT,          /* ST Box shape; next box start                  */
  PS_C_RO                /* readout only                                  */
};

/* Suffix the dialog prints after the number */
enum psctrl_unit {
  PS_U_NONE = 0, PS_U_MS, PS_U_NS, PS_U_US, PS_U_KB, PS_U_MB, PS_U_PCT,
  PS_U_HZ, PS_U_KBPS, PS_U_CYC, PS_U_X100, PS_U_SEC,
  PS_U_PCT10          /* tenths of a percent: 1000 reads as "100.0%" */
};

/* Descriptor flags */
#define PS_F_ADVANCED  0x0001   /* hide behind "show advanced"            */
#define PS_F_NEWLINE   0x0002   /* start a new group in the tab           */
#define PS_F_DANGER    0x0004   /* confirm before applying                */

/*
 * The DESCRIBE wire record. Big-endian, packed, written into the guest's
 * buffer by PS_DESCRIBE. Fixed part is 48 bytes, then nenum NUL-terminated
 * labels, then the title, then a final NUL. The accessory walks it with a
 * pointer; nothing here needs the 68k to know sizeof(struct).
 *
 *   0  char   name[24]    NUL-padded; this is also the .cfg key
 *  24  uint8  tab
 *  25  uint8  kind
 *  26  uint8  klass
 *  27  uint8  nenum
 *  28  int32  min
 *  32  int32  max
 *  36  int32  step
 *  40  uint16 unit
 *  42  uint16 flags
 *  44  int32  value        current value, so DESCRIBE alone paints a tab
 *  48  char   labels[]     nenum NUL-terminated strings
 *      char   title[]      one NUL-terminated string
 */
#define PS_DESC_FIXED 48

/* Settings occupy their own index range so they never collide with the
 * status reads in psctrl.h. */
#define PS_SET_BASE 256

/* PS_LIST selectors */
enum psctrl_list {
  PS_LIST_FLOPPY = 0,    /* raw .ST/.IMG images in the floppy directory   */
  PS_LIST_TOS    = 1     /* TOS images beside the configured one          */
};

/* Called from nf_call_psctrl() for sub-ops >= 3. */
uint32_t psctrl_settings_call(uint32_t subop, uint32_t p0, uint32_t p1,
                              uint32_t p2, uint32_t p3);

/* Read a settings index (PS_SET_BASE and above). psctrl_getint() forwards
 * here so one namespace serves both. */
uint32_t psctrl_settings_getint(uint32_t index);
int      psctrl_settings_owns(uint32_t index);

/* Drained from m68k_run_jit() at a block boundary, outside compiled code.
 * Cheap to call: one predictable load when nothing is armed. */
extern volatile int psctrl_pending_armed;
void psctrl_apply_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* PSCTRL_SETTINGS_H */
