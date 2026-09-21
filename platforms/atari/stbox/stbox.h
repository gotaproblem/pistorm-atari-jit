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
    uint8_t  machine_ste;     /* 0 = plain ST, 1 = STE (blitter, DMA
                                 sound, STE shifter, joypad ports)      */
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
/* Hot-path gate. stbox_errand_pump() sits in m68k_run_jit's post-block
 * window AND its STOPped spin loop; as an out-of-line call it measured 3.3%
 * of all samples on an idle desktop (perf, no box running). This flag is
 * 1 whenever an errand is posted or in flight, 0 otherwise, so the caller
 * does one inline load and skips the call. Set by the poster AFTER req
 * (release fence in between); cleared by the pump only when it retires
 * an errand to ST_IDLE, so a spurious 1 costs one harmless call and a
 * posted errand can never be lost. */
extern volatile int stbox_errand_active;
static inline void stbox_errand_pump_if_active(void)
{
    if (stbox_errand_active)
        stbox_errand_pump();
}
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
/* joy: 0 = mouse port, 1 = game port; state = ST joystick byte (bits 0-3
 * directions, bit 7 fire); pad_buttons = STE joypad extras (bit 0 A,
 * 1 B, 2 C, 3 OPTION, 4 PAUSE) for the STE tier's $FF9200/$FF9202. */
void stbox_joypad_event(int joy, uint8_t state, uint8_t pad_buttons);
/* kbd_usb.c tells the host when its ESC toggle changes routing, so the
 * health line can show it (the host never sees the flag otherwise). */
void stbox_note_route(int on);
/* pushes by type (key, mouse, joy, raw), ring-full drops, ACIA bytes
 * delivered to the guest - for the health line */
void stbox_input_stats(uint32_t out[6]);

/* ------------------------------------------------------------------ */
/* core-3 slice entry (ipl_task housekeeping slot ONLY)               */
/* ------------------------------------------------------------------ */
/* now = CNTVCT_EL0. Executes a bounded burst of guest cycles when the
 * 8 MHz pace owes any; returns immediately when idle or stopped. */
void stbox_slice(uint64_t now);
int  stbox_core_armed(void);              /* plain load, safe every pass    */
/* Same idea for the ipl_task spin loop: test the armed flag inline. */
extern volatile int stbox_core_armed_flag;

/* ------------------------------------------------------------------ */
/* core <-> host glue (stbox_host.c only)                             */
/* ------------------------------------------------------------------ */
int  stbox_core_setup(uint8_t *ram, uint32_t ram_size,
                      const uint8_t *rom, uint32_t rom_size);
void stbox_core_set_machine(int ste);     /* before setup/reset             */

/* ---- STE tier ---------------------------------------------------- */
/* BLiTTER (stbox_blit.c): a resumable copy of st_blitter.c's engine,
 * stepped from stbox_slice() a bounded number of bus accesses at a time.
 * All core 3. Memory hooks below are implemented in stbox.c. */
uint32_t stbox_blit_reg_read(uint32_t addr, int size);
void     stbox_blit_reg_write(uint32_t addr, uint32_t val, int size);
int      stbox_blit_busy(void);
int      stbox_blit_hog(void);
int      stbox_blit_step(int max_accesses);    /* -> accesses made       */
void     stbox_blit_reset(void);
uint16_t stbox_blit_mem_r16(uint32_t a);
void     stbox_blit_mem_w16(uint32_t a, uint16_t v);

/* DMA sound: core 3 pushes S16 stereo frames at 50066 Hz into the ring;
 * stbox_dmasnd.c (normal core, SDL) drains it. LMC1992 volume indexes
 * are published raw; the host turns them into gains. */
#define STBOX_DMA_RING 32768                    /* stereo frames (~650 ms) */
extern volatile int16_t  stbox_dma_ring[STBOX_DMA_RING * 2];
extern volatile unsigned stbox_dma_head, stbox_dma_tail;
extern volatile unsigned stbox_dma_playing;
extern volatile int stbox_dma_lmc_master, stbox_dma_lmc_left, stbox_dma_lmc_right;
int  stbox_dmasnd_start(void);
void stbox_dmasnd_stop(void);

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
/* last group-0/illegal/privilege exception taken (2/3 bombs etc.) */
typedef struct {
    uint32_t vector, ppc, pc, sr, sp, a0, a1, a6, d0;
    uint64_t cycles;
    uint32_t stack[8];               /* longs from sp at the fault    */
} stbox_exc_info_t;
#define STBOX_EXC_RING 8
extern stbox_exc_info_t stbox_exc_ring[STBOX_EXC_RING];
extern volatile unsigned stbox_exc_count;      /* total taken; ring index */
extern volatile unsigned stbox_trace_count;
extern volatile uint32_t stbox_trace_first_ppc, stbox_trace_first_pc, stbox_trace_vec;
extern volatile uint32_t stbox_trace_first_sp, stbox_trace_first_a0, stbox_trace_first_sr;
extern volatile uint32_t stbox_trace_first_stack[6];
#define STBOX_PC_RING 65536
extern uint32_t stbox_pc_ring[STBOX_PC_RING];
typedef struct { uint32_t addr, val, pc; } stbox_watch_ev;
#ifndef STBOX_WATCH_RING
#define STBOX_WATCH_RING 256          /* harness builds override */
#endif
extern uint32_t stbox_watch_lo, stbox_watch_hi;   /* hi=0: off */
extern stbox_watch_ev stbox_watch_ring[STBOX_WATCH_RING];
extern volatile unsigned stbox_watch_idx;
extern volatile unsigned stbox_pc_ring_idx;
uint32_t stbox_core_overruns(void);
void stbox_core_sched_stats(uint32_t out[5]); /* calls/s slices/s capstops debtstops mean_ns */
/* debug: guest PC telemetry (written by core 3, read by the host) */
extern volatile uint32_t stbox_dbg_pc;
extern volatile uint32_t stbox_dbg_pc_lo, stbox_dbg_pc_hi;
extern volatile uint32_t stbox_dbg_last_hwr, stbox_dbg_last_hwr_pc;
unsigned int stbox_dasm_r16(unsigned int a);   /* for the host disassembler */

/* ------------------------------------------------------------------ */
/* shared state the renderer reads (racy by design, frame tier)       */
/* ------------------------------------------------------------------ */
typedef struct {
    volatile uint32_t frame;         /* VBL counter                     */
    volatile uint32_t video_base;    /* guest phys addr of screen       */
    volatile uint8_t  shift_res;     /* 0 low, 1 med, 2 high            */
    volatile uint16_t palette[16];   /* raw palette words (ST 3-bit or
                                        STE 4-bit per gun, see ste)      */
    volatile uint8_t  ste;           /* 1: STE palette/linewidth/hscroll */
    volatile uint8_t  linewidth;     /* $FF820F words added per line    */
    volatile uint8_t  hscroll;       /* $FF8265 0-15                    */
    volatile uint16_t vis_lines;     /* visible low/med lines this frame:
                                        200, or more when a game opens the
                                        bottom border (Defender: 232)     */
    uint8_t          *ram;           /* sandbox ST-RAM (stable pointer) */
    uint32_t          ram_size;
} stbox_shared_t;
extern stbox_shared_t stbox_shared;

#ifdef __cplusplus
}
#endif
#endif /* PISTORM_STBOX_H */
