/* SPDX-License-Identifier: MIT
 *
 * stbox.h - a sandboxed plain-ST machine (Musashi 68000) inside the emulator.
 *
 * WHAT THIS IS. The JIT'd 68040 + FreeMiNT is the main machine; games that
 * need a real 8 MHz 68000 with ST timing run in here instead: a second,
 * fully private Atari ST - its own ST-RAM, its own TOS ROM, its own Shifter/
 * MFP/PSG/ACIA/FDC models - executed by Musashi and displayed on a DRM
 * overlay plane positioned over a GEM window (the vidplay pattern).
 *
 * WHERE IT RUNS. stbox_slice() is called from the ipl_task loop on the
 * isolated core 3, under that loop's admission rule: NO syscalls, NO locks,
 * bounded sub-microsecond work per call. Everything in the slice path is
 * memory-only; file I/O, DRM and SDL live in stbox_host.c on the normal
 * cores, talking to the core through lock-free rings and plain flags.
 *
 * ISOLATION INVARIANT. Nothing in this module may ever call the real bus
 * accessors (m68k_read_memory_* in emulator.c) or touch GPIO. Musashi's bus
 * interface is renamed to stbox_bus_* in third_party/musashi/m68kconf.h
 * precisely so the link fails if the two worlds meet.
 */
#ifndef PISTORM_STBOX_H
#define PISTORM_STBOX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* configuration (set before start; read-only while running)          */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t ram_kb;          /* 512, 1024, 2048, 4096                  */
    char     tos_path[256];   /* host path to TOS ROM image             */
    char     floppy_a[256];   /* .ST/.MSA image, empty = no disk        */
    uint8_t  machine_ste;     /* 0 = plain ST (v1 supports 0 only)      */
    uint8_t  accuracy;        /* 0 = frame tier (v1), 1 = scanline      */
} stbox_cfg_t;

/* ------------------------------------------------------------------ */
/* host-side control (any thread EXCEPT core 3)                       */
/* ------------------------------------------------------------------ */
int  stbox_host_init(void);               /* once, at emulator startup      */
void stbox_host_shutdown(void);

/* Load ROM (and floppy image if any) on the calling thread, then arm the
 * core-3 slice engine. 0 on success. */
int  stbox_start(const stbox_cfg_t *cfg);
void stbox_stop(void);                    /* park the box, release plane    */
void stbox_reset(void);                   /* guest cold reset               */
int  stbox_running(void);

/* Load a .ST or .MSA image (host path) and insert it as drive A. Safe
 * while the box runs; a following stbox_reset() boots it. */
int  stbox_disk_insert_path(const char *path);

/* GEM front-end geometry in GUEST DESKTOP pixels (what a GEM app knows);
 * the render thread maps them through the presenter's integer-scaled,
 * centred geometry (drmpres_dst_x/y/w/h and drmpres_src_w/h) each frame.
 * NatFeat STBOX subops call these; they forward to the render side, never
 * core 3. */
void stbox_set_rect(int x, int y, int w, int h);
void stbox_set_clip(int x, int y, int w, int h);
void stbox_set_focus(int focused);
int  stbox_get_focus(void);

/* stats for PSMON/NatFeat: guest MIPS-ish, frames, slice overruns */
void stbox_get_stats(uint32_t out[4]);

/* ------------------------------------------------------------------ */
/* sandbox PSG audio (stbox_psg.c renders; stbox.c produces)          */
/* ------------------------------------------------------------------ */
typedef struct { uint64_t ticks; uint8_t reg, val; } stbox_psg_ev;
#define STBOX_PSG_RING 1024
int  stbox_psg_start(void);               /* bind to the SDL device         */
void stbox_psg_stop(void);

/* ------------------------------------------------------------------ */
/* real-FDC bridge: the sandbox's drive A on the REAL WD1772 (Gotek). */
/*                                                                    */
/* Core 3 posts one errand at a time and kicks the CPU thread; the    */
/* errand pump (stbox_realfdc.c) runs in m68k_run_jit's spcflags      */
/* window - the only place host code may drive the bus - stepping a   */
/* small state machine: flock, PSG drive/side select (latch restored  */
/* after, see ym2149_selected_reg), real DMA setup, real FDC command, */
/* status polls, buffer drain into staging. Core 3 finishes the       */
/* sandbox side: staging -> sandbox RAM, status, INTRQ.               */
/* ------------------------------------------------------------------ */
#define STBOX_RFDC_MAXSEC 16
typedef struct {
    /* core 3 -> pump */
    volatile uint8_t req;         /* errand posted                     */
    volatile uint8_t kick;        /* pump attention wanted (poll)      */
    uint8_t  cmd;                 /* WD1772 command byte to forward    */
    uint8_t  arg_track;           /* FDC data reg for type I           */
    uint8_t  sector;              /* start sector for type II          */
    uint8_t  side;                /* from the sandbox PSG port A       */
    uint16_t count;               /* sectors to transfer               */
    /* pump -> core 3 */
    volatile uint8_t done;
    volatile uint8_t status;      /* real FDC status at completion     */
    volatile uint16_t xferred;    /* sectors actually drained          */
    uint8_t  staging[STBOX_RFDC_MAXSEC * 512];
    /* config (host writes while core parked or between ops) */
    volatile uint32_t dmabuf;     /* guest ST-RAM phys addr, 0 = none  */
    volatile uint32_t buflen;
    volatile int enabled;
} stbox_rfdc_t;
extern stbox_rfdc_t stbox_rfdc;

void stbox_errand_pump(void);             /* CPU thread, spcflags window    */
void stbox_rfdc_abort(void);              /* refuse new errands (teardown)  */
void stbox_rfdc_set_buffer(uint32_t guest_addr, uint32_t len);
void stbox_rfdc_enable(int on);
/* core 3 -> CPU thread wake; host installs jit_request_cpu_exit here */
extern void (*stbox_cpu_kick)(void);

/* ------------------------------------------------------------------ */
/* input (called from the emulator's input paths; lock-free)          */
/* ------------------------------------------------------------------ */
void stbox_key_event(uint8_t st_scancode, int down);
/* One raw IKBD-protocol byte (device->host direction) straight into the
 * sandbox keyboard fifo - the real IKBD's output is forwarded through
 * here while the box has focus; no translation, no scaling. */
void stbox_ikbd_byte(uint8_t b);
void stbox_mouse_rel(int dx, int dy, int buttons);   /* buttons: bit1 L, bit0 R */
void stbox_joy_event(int joy, uint8_t state);        /* ST joystick bits       */

/* ------------------------------------------------------------------ */
/* core-3 slice entry (ipl_task housekeeping slot ONLY)               */
/* ------------------------------------------------------------------ */
/* now = CNTVCT_EL0. Executes a bounded burst of guest cycles when the
 * 8 MHz pace owes any; returns immediately when idle or stopped. */
void stbox_slice(uint64_t now);
int  stbox_core_armed(void);              /* plain load, safe every pass    */

/* ------------------------------------------------------------------ */
/* core <-> host glue (stbox_host.c only)                             */
/* ------------------------------------------------------------------ */
int  stbox_core_setup(uint8_t *ram, uint32_t ram_size,
                      const uint8_t *rom, uint32_t rom_size);
void stbox_core_arm(uint64_t now, uint64_t cntfrq);
void stbox_core_disarm(void);
void stbox_request_reset(void);           /* cold reset on next slice       */
uint8_t *stbox_core_disk_insert(uint8_t *buf, uint32_t size);
uint32_t stbox_core_cps(void);
int  stbox_core_take_halt_report(void);   /* 1 once per double-fault halt   */
typedef struct {
    uint32_t fault1;      /* address of the ORIGINAL bus error - if a real
                             ST would not fault here, the decode is the bug */
    uint32_t fault2;      /* the fault during exception processing (~SSP)   */
    uint32_t pc, ppc, sr, sp;
} stbox_halt_info_t;
extern stbox_halt_info_t stbox_halt_info;
#define STBOX_PC_RING 65536
extern uint32_t stbox_pc_ring[STBOX_PC_RING];
typedef struct { uint32_t addr, val, pc; } stbox_watch_ev;
#define STBOX_WATCH_RING 256
extern uint32_t stbox_watch_lo, stbox_watch_hi;   /* hi=0: off */
extern stbox_watch_ev stbox_watch_ring[STBOX_WATCH_RING];
extern volatile unsigned stbox_watch_idx;
extern volatile unsigned stbox_pc_ring_idx;
uint32_t stbox_core_overruns(void);

/* ------------------------------------------------------------------ */
/* shared state the renderer reads (racy by design, frame tier)       */
/* ------------------------------------------------------------------ */
typedef struct {
    volatile uint32_t frame;         /* VBL counter                     */
    volatile uint32_t video_base;    /* guest phys addr of screen       */
    volatile uint8_t  shift_res;     /* 0 low, 1 med, 2 high            */
    volatile uint16_t palette[16];   /* raw ST palette words            */
    uint8_t          *ram;           /* sandbox ST-RAM (stable pointer) */
    uint32_t          ram_size;
} stbox_shared_t;
extern stbox_shared_t stbox_shared;

#ifdef __cplusplus
}
#endif
#endif /* PISTORM_STBOX_H */
