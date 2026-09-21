/* SPDX-License-Identifier: MIT
 *
 * stbox.c - the sandboxed ST machine: memory map, hardware page, MFP, ACIA/
 * IKBD, FDC stub, interrupt plumbing, and the core-3 micro-slice engine.
 *
 * EVERY function in this file that runs after stbox_start() is core-3 code
 * under the ipl_task admission rule: memory-only, no syscalls, no locks,
 * bounded work. The exceptions are the *_load helpers, which stbox_host.c
 * calls from a normal core before the slice engine is armed.
 *
 * Timing model (plain ST, PAL):
 *   CPU        8021248 Hz  (32.084992 MHz master / 4)
 *   scanline    512 cycles, 313 lines -> VBL 50.05 Hz
 *   MFP clock  2457600 Hz  (tracked with a fixed-point accumulator)
 *   IKBD serial 7812.5 baud -> ~1.28 ms per byte (rx pacing)
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "platforms/atari/psctrl/psctrl_tunables.h"
#include "stbox.h"
#include "../../../third_party/musashi/m68k.h"

/* ------------------------------------------------------------------ */
/* constants                                                          */
/* ------------------------------------------------------------------ */
#define ST_CPU_HZ      8021248u
#define CYC_PER_LINE   512
#define LINES_PER_VBL  313
/* Display-enable window (PAL, 50 Hz, borders closed): DE is active on
 * lines 63..262, and within each of those for 320 of the 512 cycles,
 * starting 56 cycles into the line. The shifter fetches 160 bytes in
 * that window (one byte per two cycles); Timer B's event input is DE's
 * trailing edge, and the video address counter only advances while DE
 * is active. Games' raster code counts on all three. */
#define DE_FIRST_LINE  63
#define DE_LINES       200
#define DE_START_CYC   56
#define DE_CYC         320
#define DE_BYTES       160
#define CYC_PER_VBL    (CYC_PER_LINE * LINES_PER_VBL)
#define MFP_HZ         2457600u
#define IKBD_BYTE_CYC  10267        /* 8021248 / 781.25 bytes-per-sec   */

/* Slice budget. 64 guest cycles is ~10-20 instructions, roughly 200-400 ns
 * of host work on a Pi 4 core - inside the housekeeping admission rule. A
 * single long instruction (DIVS, MOVEM) may overshoot; Musashi stops at the
 * first boundary past the budget and the debt carries. */
/* Live, from the ST Box tab. Was a compile-time 64. The dialog shows the
 * approximate host cost beside it; the loop counts GUEST cycles, and
 * converting a nanosecond cap per pass would put a divide in the
 * admission path, so cycles is what the control sets. */
#define SLICE_CYC      ((int)pst_stbox_slice_cyc)
/* Host time the box may spend per ipl_task pass. ONE 64-cycle slice per
 * pass caps the box at 64 cycles / pass-time: ~15 us/pass on a Pi 4 =
 * ~4.3 M cyc/s, ~54% of an 8 MHz ST - fine while the box idles (the main
 * machine's passes are short then), too slow once a game runs. So while
 * the 8 MHz pace still owes cycles, keep running slices until this much
 * host time has elapsed since entry. The cost is paid by the main
 * machine: worst case ~one slice (~0.4 us) plus this cap is added to its
 * IPL sample latency. 1500 ns reaches full speed with margin; the IPL
 * confirm window itself is 2000 ns. 0 = one slice per pass (old
 * behaviour). Override live with PISTORM_STBOX_SLICE_NS. */
#define SLICE_HOST_CAP_NS 1500
/* If the box falls behind (host stall, heavy IPL traffic), never try to
 * catch up more than one frame - drop the debt instead of marathoning. */
#define MAX_DEBT_CYC   CYC_PER_VBL

/* ------------------------------------------------------------------ */
/* machine state                                                      */
/* ------------------------------------------------------------------ */
stbox_shared_t stbox_shared;

static uint8_t *g_ram;             /* ST-RAM                            */
static uint32_t g_ram_mask;        /* size-1 (power of two sizes only)  */
static uint32_t g_ram_size;
static uint8_t  g_rom[512 * 1024]; /* TOS image                         */
static uint32_t g_rom_base;        /* 0xE00000 or 0xFC0000, from header */
static uint32_t g_rom_size;

volatile int stbox_core_armed_flag; /* slice engine may run (exported for the inline gate) */
#define g_armed stbox_core_armed_flag
static volatile int g_reset_req;   /* cold reset on next slice          */

/* pacing */
static uint64_t g_last_ticks;
static uint64_t g_cntfrq;
static int64_t  g_cyc_debt_fp;     /* owed guest cycles, 32.32, SIGNED:
                                    * an instruction can overshoot the slice
                                    * budget, so the debt legitimately goes
                                    * negative - unsigned here wraps and the
                                    * clamp then gifts the box a whole frame
                                    * (measured: 2.56x real speed).          */
static uint64_t g_fp_step;         /* (ST_CPU_HZ << 32) / cntfrq        */
static uint64_t g_slice_cap_ticks; /* SLICE_HOST_CAP_NS in arch ticks; 0=off */
static int      g_slice_cap_ns = -1;
static int slice_cap_ns(void)
{
    if (g_slice_cap_ns < 0) {
        const char *e = getenv("PISTORM_STBOX_SLICE_NS");
        g_slice_cap_ns = e ? atoi(e) : SLICE_HOST_CAP_NS;
        if (g_slice_cap_ns < 0 || g_slice_cap_ns > 10000)
            g_slice_cap_ns = SLICE_HOST_CAP_NS;
    }
    return g_slice_cap_ns;
}

/* frame position */
static uint32_t g_frame_cyc;       /* cycle within current frame        */
static uint32_t g_next_line_cyc;   /* next scanline boundary            */
static uint32_t g_line;            /* current scanline                  */

/* stats */
static uint32_t g_stat_cyc;        /* guest cycles this second           */
static uint32_t g_stat_overrun;    /* slices that ran past 2x budget     */
static uint32_t g_stat_cps;        /* published cycles/sec               */
static uint32_t g_stat_calls, g_stat_slices, g_stat_capstop, g_stat_debtstop;
static uint32_t g_pub_calls, g_pub_slices, g_pub_capstop, g_pub_debtstop;
static uint64_t g_stat_slice_ticks;  /* host ticks spent inside the burst   */
static uint32_t g_pub_slice_ns;      /* mean host ns per slice, last second */
static uint64_t g_stat_next;       /* arch-tick of next 1 s rollover     */

/* ------------------------------------------------------------------ */
/* GLUE/Shifter registers                                             */
/* ------------------------------------------------------------------ */
static uint8_t  g_memcfg;

/* ST MMU banks. TOS sizes memory by configuring $FF8001 and probing for
 * ALIASING - within a bank smaller than configured, addresses wrap. A
 * flat no-alias model only boots when the actual size matches the probe
 * pattern (4 MB did; 1 MB double-faulted instantly). Model it: two
 * banks, configured size from memcfg (00=128K 01=512K 10=2MB), actual
 * size from ram_kb, offsets wrap within the actual bank. Absent bank =
 * open bus (reads 0xFF, writes sunk) - the whole configured range is
 * DTACKed by the MMU, never a bus error. Once the configured layout
 * matches the installed one, the flat fast path takes over. */
/* write watchpoint (diagnostics; 0 = off). Records writer PC per hit;
 * FDC DMA writes are tagged 0xFDC000 + command byte. */
uint32_t stbox_watch_lo, stbox_watch_hi;
stbox_watch_ev stbox_watch_ring[STBOX_WATCH_RING];
volatile unsigned stbox_watch_idx;
static inline void watch_w(uint32_t a, uint32_t v, uint32_t pc)
{
    if (a >= stbox_watch_lo && a < stbox_watch_hi) {
        stbox_watch_ev *e =
            &stbox_watch_ring[stbox_watch_idx++ & (STBOX_WATCH_RING - 1)];
        e->addr = a; e->val = v; e->pc = pc;
    }
}

static uint32_t g_bank_cfg[2], g_bank_act[2];
static int g_ram_flat;

static const uint32_t bank_sz[4] = { 128u<<10, 512u<<10, 2048u<<10, 2048u<<10 };

static void ram_recfg(void)
{
    g_bank_cfg[0] = bank_sz[(g_memcfg >> 2) & 3];
    g_bank_cfg[1] = bank_sz[g_memcfg & 3];
    g_ram_flat = (g_bank_cfg[0] == g_bank_act[0] &&
                  (g_bank_cfg[1] == g_bank_act[1] || g_bank_act[1] == 0));
}

/* returns storage offset; ok: 1 = backed RAM, 0 = open bus in the
 * configured range, -1 = beyond configured RAM entirely */
static inline uint32_t ram_map(uint32_t a, int *ok)
{
    if (g_ram_flat) {
        if (a < g_ram_size) { *ok = 1; return a; }
        *ok = (a < g_bank_cfg[0] + g_bank_cfg[1]) ? 0 : -1;
        return 0;
    }
    if (a < g_bank_cfg[0]) {
        if (!g_bank_act[0]) { *ok = 0; return 0; }
        *ok = 1; return a & (g_bank_act[0] - 1);
    }
    a -= g_bank_cfg[0];
    if (a < g_bank_cfg[1]) {
        if (!g_bank_act[1]) { *ok = 0; return 0; }
        *ok = 1; return g_bank_act[0] + (a & (g_bank_act[1] - 1));
    }
    *ok = -1; return 0;
}
static uint32_t g_vid_base;        /* latched video base                */
static uint8_t  g_sync;            /* $FF820A                           */
static uint8_t  g_res;             /* $FF8260                           */
static uint8_t  g_bottom_open;     /* border opened during this frame   */
static uint16_t g_pal[16];

/* ------------------------------------------------------------------ */
/* STE tier (g_ste): decoded only in STE mode, so an ST box is exactly */
/* the plain machine above - TOS 2.06's "am I an STE" probe of $FF820D */
/* still reads 0 there.                                               */
/* ------------------------------------------------------------------ */
static int      g_ste;
static int      g_ste_noblit = -1; /* PISTORM_STBOX_NOBLIT: blitter absent */
static int      g_ste_nodma  = -1; /* PISTORM_STBOX_NODMA: DMA sound absent */
static int ste_noblit(void){ if (g_ste_noblit<0){const char*e=getenv("PISTORM_STBOX_NOBLIT"); g_ste_noblit=(e&&*e=='1');} return g_ste_noblit; }
static int ste_nodma (void){ if (g_ste_nodma <0){const char*e=getenv("PISTORM_STBOX_NODMA"); g_ste_nodma =(e&&*e=='1');} return g_ste_nodma; }
static uint8_t  g_linewidth;       /* $FF820F: words added per line     */
static uint8_t  g_hscroll;         /* $FF8265: 0-15 pixels              */

/* STE DMA sound. Core 3 fetches samples in step with guest cycles and
 * pushes S16 stereo frames at the 50066 Hz master rate into the ring
 * (lower rates repeat each input sample 2/4/8x); stbox_dmasnd.c feeds an
 * SDL stream from it on a normal core. */
static struct {
    uint8_t  ctrl;                 /* $FF8901: bit0 play, bit1 repeat  */
    uint8_t  mode;                 /* $FF8921: bit7 mono, bits0-1 rate */
    uint32_t start, end;           /* frame registers as written       */
    uint32_t cur, cur_end;         /* latched frame in flight          */
    uint16_t mw_data, mw_mask;     /* $FF8922 / $FF8924 as written      */
    uint64_t mw_start;             /* g_total_cyc of the data write;
                                      0 = no transfer in flight         */
    uint64_t fp;                   /* output-sample accumulator, 32.32 */
    unsigned sub;                  /* repeats left of the current input */
    int16_t  l, r;                 /* current input sample             */
} dma;
#define DMA_MASTER_HZ 50066u
#define DMA_FP_STEP   ((((uint64_t)DMA_MASTER_HZ << 32) / ST_CPU_HZ))

volatile int16_t  stbox_dma_ring[STBOX_DMA_RING * 2];
volatile unsigned stbox_dma_head, stbox_dma_tail;
volatile unsigned stbox_dma_playing;
volatile int stbox_dma_lmc_master = 40, stbox_dma_lmc_left = 20,
             stbox_dma_lmc_right = 20;

void stbox_core_set_machine(int ste) { g_ste = ste ? 1 : 0; }

/* ------------------------------------------------------------------ */
/* PSG                                                                */
/* ------------------------------------------------------------------ */
static uint8_t g_psg_sel;
static uint8_t g_psg_reg[16];

/* register-write ring for stbox_psg.c (SPSC: core 3 -> audio thread).
 * Timestamps are the arch-timer value the slice was entered with. */
stbox_psg_ev stbox_psg_ring[STBOX_PSG_RING];
volatile unsigned stbox_psg_head;
volatile unsigned stbox_psg_tail;
static uint64_t g_slice_now;             /* CNTVCT at slice entry       */

/* Debug: core 3 publishes the guest PC after each executed slice, plus a
 * tiny recent-PC set, so the host health line can show where TOS/game is
 * spinning without touching Musashi from another thread. Plain volatile
 * stores; a torn read just prints a slightly stale PC, which is fine. */
volatile uint32_t stbox_dbg_pc;
volatile uint32_t stbox_dbg_pc_lo = 0xFFFFFFFFu, stbox_dbg_pc_hi;   /* window */
volatile uint32_t stbox_dbg_last_hwr, stbox_dbg_last_hwr_pc;        /* last HW read addr + PC */
static int g_dbg = -1;             /* PISTORM_STBOX_DBG: publish PC telemetry */
static int dbg_on(void){ if (g_dbg<0){const char*e=getenv("PISTORM_STBOX_DBG"); g_dbg=(e&&*e=='1');} return g_dbg; }

static void psg_snoop(uint8_t reg, uint8_t val)
{
    unsigned h = stbox_psg_head;
    if (h - stbox_psg_tail >= STBOX_PSG_RING) return;      /* full: drop */
    stbox_psg_ring[h & (STBOX_PSG_RING - 1)].ticks = g_slice_now;
    stbox_psg_ring[h & (STBOX_PSG_RING - 1)].reg = reg;
    stbox_psg_ring[h & (STBOX_PSG_RING - 1)].val = val;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    stbox_psg_head = h + 1;
}

/* ------------------------------------------------------------------ */
/* MFP 68901                                                          */
/* ------------------------------------------------------------------ */
static struct {
    uint8_t gpip, aer, ddr;
    uint8_t iera, ierb, ipra, iprb, isra, isrb, imra, imrb;
    uint8_t vr;
    uint8_t tacr, tbcr, tcdcr;
    uint8_t tadr, tbdr, tcdr, tddr;   /* data (reload) registers        */
    uint8_t tamain, tbmain, tcmain, tdmain; /* down-counters            */
    uint8_t scr, ucr, rsr, tsr, udr;
    /* prescaler accumulators, in MFP clocks */
    uint32_t ta_acc, tb_acc, tc_acc, td_acc;
} mfp;

/* 8021248 cycles -> 2457600 MFP ticks: accumulate in 32.32 fixed point */
static uint64_t g_mfp_fp;
#define MFP_FP_STEP ((((uint64_t)MFP_HZ << 32) / ST_CPU_HZ))

static const uint16_t mfp_presc[8] = { 0, 4, 10, 16, 50, 64, 100, 200 };

/* MFP channel numbers (15..0, priority order) */
#define MFP_CH_TIMER_A  13
#define MFP_CH_RXFULL   12
#define MFP_CH_TIMER_B   8
#define MFP_CH_GPIP5     7   /* FDC/HDC        */
#define MFP_CH_ACIA      6   /* GPIP4          */
#define MFP_CH_TIMER_C   5
#define MFP_CH_TIMER_D   4
#define MFP_CH_GPIP7    15   /* mono detect    */

/* ------------------------------------------------------------------ */
/* ACIA + IKBD                                                        */
/* ------------------------------------------------------------------ */
static struct {
    /* 6850 guest side */
    uint8_t rx;               /* data register                          */
    uint8_t sr;               /* bit0 RDRF, bit1 TDRE, bit7 IRQ         */
    uint8_t cr;
    /* IKBD -> guest fifo, drained at serial pace */
    uint8_t fifo[256];
    uint8_t fh, ft;
    uint32_t next_rx_cyc;     /* frame-relative pacing uses total cycles */
    /* guest -> IKBD command assembly */
    uint8_t cmd[8];
    uint8_t cmdlen, cmdneed;
    /* mouse state */
    uint8_t mouse_buttons;
    uint8_t mouse_mode;       /* 0 rel (power-on), 1 abs, 2 keycode, 3 off.
                                 A disabled/keycode mouse sends NO F8
                                 packets on a real IKBD; injecting them
                                 anyway rams TOS's dispatch through
                                 whatever stale vector the game left
                                 behind (field case: click -> jump to
                                 $D200D8 -> two bombs).                 */
    uint8_t joy_event;        /* $14 joystick event reporting on        */
    /* Real-IKBD fidelity (Hatari ikbd.c): joystick events are ON at
     * power-up and after $80 $01; $14 turns the mouse OFF unless it
     * lands inside the ~63 ms reset window after a $08/$12 (Barbarian,
     * Hammerfist keep both); while the mouse is on, joystick 1's fire is
     * the RIGHT mouse button and joystick 0 (the mouse port) is silent. */
    uint64_t reset_cyc;       /* g_total_cyc at the last $80 $01        */
    uint8_t mouse_touched;    /* $08 or $12 inside the window           */
    uint8_t both;             /* mouse + joystick reporting together    */
    uint8_t mouse_off_cmd, joy_off_cmd;
    /* host pads, as last delivered by input_drain: [0] = joystick 0
     * (mouse port), [1] = joystick 1 (game port). joy = STJOY bits,
     * pad = STE joypad extras (STPAD bits: A B C OPTION PAUSE). */
    uint8_t joy[2], pad[2];
    uint8_t joy_sent[2];      /* last state put on the wire per port    */
    uint8_t joy_rbutton;      /* joystick 1 fire riding as RMB          */
    uint16_t pad_select;      /* STE $FF9202 column select shadow       */
} acia;
#define IKBD_RESET_WINDOW_CYC 502000u   /* 8 MHz cycles (Hatari)     */
#define STJOY_FIRE 0x80
#define STPAD_A 0x01
#define STPAD_B 0x02
#define STPAD_C 0x04
#define STPAD_OPTION 0x08
#define STPAD_PAUSE 0x10
static uint64_t g_total_cyc;       /* free-running guest cycle counter  */

/* host input rings (SPSC: producer = input thread, consumer = core 3) */
#define INRING 256
static volatile uint32_t g_in_ring[INRING];
static volatile uint32_t g_in_head, g_in_tail;
/* encoding: type<<24 | a<<16 | b<<8 | c
 * type 0 = key (a=scancode|0x80 if break), 1 = mouse (a=dx s8, b=dy s8,
 * c=buttons), 2 = joystick (a=joy, b=state, c=STE pad extras),
 * 3 = raw IKBD byte (a) */
/* Diagnostic counters, reported in the host's periodic health line:
 * pushes per type (key/mouse/joy/raw), ring-full drops, and bytes the
 * ACIA actually presented to the guest. Each counter has one writer
 * (types come from one thread each; acia_rx is core 3), so plain
 * increments are fine. */
static volatile uint32_t g_in_cnt[4], g_in_drop, g_acia_rx_cnt;

/* ------------------------------------------------------------------ */
/* WD1772 FDC + ST DMA, drive A only.                                 */
/*                                                                    */
/* Commands complete after a guest-cycle delay (checked in the slice  */
/* loop, memory-only) so loaders that watch the busy bit or race the  */
/* index pulse see a floppy, not a syscall. The image lives in a      */
/* host-owned buffer handed over through a lock-free pending pointer; */
/* writes modify the buffer in memory (no write-back yet).            */
/* ------------------------------------------------------------------ */
static struct {
    uint8_t  cmd, track, sector, data, status;
    uint16_t dma_mode;
    uint16_t sector_count;
    uint32_t dma_addr;
    uint64_t event_cyc;       /* completion time, g_total_cyc units    */
    uint8_t  pending;         /* a command is in flight                */
    int8_t   step_dir;        /* +1 in, -1 out                         */
} fdc;

static struct {
    uint8_t *data;            /* raw .ST layout, host-owned            */
    uint32_t size;
    uint16_t spt, sides, tracks;
    int      present;
} diska;

/* host -> core media handoff (slice adopts, host spins on adopted) */
static uint8_t * volatile g_disk_new;
static volatile uint32_t  g_disk_new_size;
static volatile int       g_disk_adopted;

/* real-FDC bridge state (see stbox.h and stbox_realfdc.c) */
stbox_rfdc_t stbox_rfdc;
void (*stbox_cpu_kick)(void);
static uint64_t g_rfdc_next_kick;         /* pump poll cadence, cycles */
#define RFDC_KICK_CYC 8000                /* ~1 ms                     */

#define REV_CYC   1604250u    /* one revolution at 300 rpm             */
#define SEEK_CYC  12000u      /* per track step (~1.5 ms)              */
#define SECT_CYC  22000u      /* rotational+transfer per sector        */

static void mfp_raise(int ch);

static void fdc_intrq(void)
{
    mfp.gpip &= (uint8_t)~0x20;               /* GPIP5 low = INTRQ     */
    mfp_raise(MFP_CH_GPIP5);
}

static void fdc_geometry(void)
{
    const uint8_t *b = diska.data;
    uint16_t spt   = (uint16_t)(b[24] | (b[25] << 8));   /* BPB, LE    */
    uint16_t sides = (uint16_t)(b[26] | (b[27] << 8));
    /* Crack bootsectors have CODE where the BPB lives; bytes that merely
     * look plausible produce shifted sector maps and a loader that jumps
     * into garbage (field case: 820 KB 10-sector image, pc ended at an
     * odd mid-RAM address). Only trust a BPB the file size agrees with. */
    if (spt < 8 || spt > 12 || sides < 1 || sides > 2 ||
        diska.size % (512u * spt * sides) != 0) {
        /* bootsector lies (raw game dump): infer from size */
        static const uint16_t try_spt[] = { 9, 10, 11 };
        spt = 0;
        for (int si = 2; si >= 1 && !spt; si--)
            for (unsigned i = 0; i < 3 && !spt; i++)
                if (diska.size % (512u * try_spt[i] * si) == 0) {
                    spt = try_spt[i]; sides = (uint16_t)si;
                }
        if (!spt) { spt = 9; sides = 2; }
    }
    diska.spt = spt;
    diska.sides = sides;
    diska.tracks = (uint16_t)(diska.size / (512u * spt * sides));
}

static void fdc_adopt_media(void)
{
    if (!g_disk_new) return;
    diska.data = g_disk_new;
    diska.size = g_disk_new_size;
    g_disk_new = NULL;
    fdc_geometry();
    diska.present = 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_disk_adopted = 1;
}

/* CRC-16/CCITT over the address and data fields of synthesized track
 * reads, seeded the way the WD does: the three A1 sync marks included. */
static uint16_t mfm_crc(const uint8_t *p, int n)
{
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++)
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021)
                             : (uint16_t)(c << 1);
    }
    return c;
}

static void fdc_ram_put(uint32_t *dst, uint8_t v)
{
    uint32_t a = (*dst)++ & 0xFFFFFF;
    if (a >= 8 && a < g_ram_size) g_ram[a] = v;
}

/* side select: PSG port A bit 0, inverted (PA0 high = side 0) */
static int fdc_side(void) { return (~g_psg_reg[14]) & 1; }

#ifdef STBOX_FDC_TRACE
/* harness-only: every command with the register state it saw */
typedef struct { uint64_t cyc; uint8_t cmd, track, sector, data, side, was_pending, mode_hi; uint32_t dma, cnt; uint8_t result; } stbox_fdc_ev_t;
#define STBOX_FDC_TRACE_N 65536
stbox_fdc_ev_t stbox_fdc_trace[STBOX_FDC_TRACE_N];
volatile unsigned stbox_fdc_trace_idx;
static stbox_fdc_ev_t *g_fdc_last_ev;
#endif

#ifdef STBOX_FDC_TRACE
#define FDC_RESULT(s) do { if (g_fdc_last_ev) g_fdc_last_ev->result = (uint8_t)(s); } while (0)
#else
#define FDC_RESULT(s) ((void)0)
#endif
static void fdc_complete(void)
{
    uint8_t cmd = fdc.cmd;
    fdc.pending = 0;

    if (cmd < 0x80) {                          /* type I               */
        fdc.status = 0xA0 |                    /* motor + spin-up      */
                     (fdc.track == 0 ? 0x04 : 0x00);
        if (!diska.present) fdc.status |= 0x10; /* verify fails: RNF   */
        FDC_RESULT(fdc.status);
        fdc_intrq();
        return;
    }

    if (cmd < 0xC0) {                          /* type II: rd/wr sector */
        int multi = cmd & 0x10;
        int side = fdc_side();
        if (!diska.present || fdc.track >= diska.tracks ||
            fdc.sector < 1 || fdc.sector > diska.spt ||
            side >= diska.sides) {
            fdc.status = 0x90;                 /* motor + RNF          */
            FDC_RESULT(fdc.status);
            fdc_intrq();
            return;
        }
        do {
            uint32_t off = (((uint32_t)fdc.track * diska.sides + side) *
                            diska.spt + (fdc.sector - 1)) * 512u;
            uint32_t dst = fdc.dma_addr;
            if (off + 512 > diska.size) break;
            if ((cmd & 0x20) == 0) {           /* read                 */
                if (stbox_watch_hi && dst < stbox_watch_hi &&
                    dst + 512 > stbox_watch_lo) {
                    /* report the first watched byte the sector covers (the
                     * old test only fired when the sector STARTED inside
                     * the window, so a hit mid-sector went unseen) */
                    uint32_t hit = dst > stbox_watch_lo ? dst : stbox_watch_lo;
                    watch_w(hit, off, 0xFDC000 | cmd);
                }
                for (int i = 0; i < 512; i++) {
                    uint32_t a = (dst + i) & 0xFFFFFF;
                    if (a >= 8 && a < g_ram_size)
                        g_ram[a] = diska.data[off + i];
                }
            } else {                           /* write (image only)   */
                for (int i = 0; i < 512; i++) {
                    uint32_t a = (dst + i) & 0xFFFFFF;
                    diska.data[off + i] =
                        (a < g_ram_size) ? g_ram[a] : 0;
                }
            }
            fdc.dma_addr += 512;
            if (fdc.sector_count) fdc.sector_count--;
            fdc.sector++;
        } while (multi && fdc.sector <= diska.spt && fdc.sector_count);
        fdc.status = 0x80;                     /* motor, clean         */
        FDC_RESULT(fdc.status);
        fdc_intrq();
        return;
    }

    if ((cmd & 0xF0) == 0xC0) {                /* type III: read address */
        if (!diska.present) { fdc.status = 0x90; fdc_intrq(); return; }
        int side = fdc_side();
        /* next ID field: rotate through sectors by time */
        uint8_t sec = (uint8_t)(1 + (g_total_cyc / SECT_CYC) % diska.spt);
        uint8_t hdr[6] = { (uint8_t)fdc.track, (uint8_t)side, sec, 2,
                           0xDE, 0xAD };
        for (int i = 0; i < 6; i++) {
            uint32_t a = (fdc.dma_addr + i) & 0xFFFFFF;
            if (a >= 8 && a < g_ram_size) g_ram[a] = hdr[i];
        }
        fdc.dma_addr += 6;
        fdc.sector = fdc.track;                /* WD1772 quirk         */
        fdc.status = 0x80;
        fdc_intrq();
        return;
    }

    if ((cmd & 0xF0) == 0xE0) {                /* READ TRACK */
        /* An .ST image has no raw MFM, so synthesize the standard layout
         * the way Hatari does: gaps, A1 sync runs, ID and data address
         * marks, true CRCs, sector data from the image. Loaders that
         * slurp raw tracks (820 KB one-disk cracks live on this) parse
         * it and find their sectors. */
        if (!diska.present) { fdc.status = 0x90; fdc_intrq(); return; }
        int side = fdc_side();
        uint32_t want = (uint32_t)(fdc.sector_count ? fdc.sector_count : 13)
                        * 512u;
        uint32_t dst = fdc.dma_addr, end = fdc.dma_addr + want;
        uint8_t hdr[8], dam[4] = { 0xA1, 0xA1, 0xA1, 0xFB };
        for (int i = 0; i < 60 && dst < end; i++) fdc_ram_put(&dst, 0x4E);
        for (uint8_t sec = 1; sec <= diska.spt && dst < end; sec++) {
            uint32_t off = (((uint32_t)fdc.track * diska.sides + side) *
                            diska.spt + (sec - 1)) * 512u;
            if (off + 512 > diska.size) break;
            hdr[0] = hdr[1] = hdr[2] = 0xA1; hdr[3] = 0xFE;
            hdr[4] = fdc.track; hdr[5] = (uint8_t)side;
            hdr[6] = sec; hdr[7] = 2;
            uint16_t hc = mfm_crc(hdr, 8);
            for (int i = 0; i < 12; i++) fdc_ram_put(&dst, 0x00);
            for (int i = 0; i < 8; i++)  fdc_ram_put(&dst, hdr[i]);
            fdc_ram_put(&dst, (uint8_t)(hc >> 8));
            fdc_ram_put(&dst, (uint8_t)hc);
            for (int i = 0; i < 22; i++) fdc_ram_put(&dst, 0x4E);
            for (int i = 0; i < 12; i++) fdc_ram_put(&dst, 0x00);
            uint16_t dc = 0xFFFF;
            for (int i = 0; i < 4; i++) {
                fdc_ram_put(&dst, dam[i]);
                dc ^= (uint16_t)dam[i] << 8;
                for (int b = 0; b < 8; b++)
                    dc = (dc & 0x8000) ? (uint16_t)((dc << 1) ^ 0x1021)
                                       : (uint16_t)(dc << 1);
            }
            for (int i = 0; i < 512; i++) {
                uint8_t v = diska.data[off + i];
                fdc_ram_put(&dst, v);
                dc ^= (uint16_t)v << 8;
                for (int b = 0; b < 8; b++)
                    dc = (dc & 0x8000) ? (uint16_t)((dc << 1) ^ 0x1021)
                                       : (uint16_t)(dc << 1);
            }
            fdc_ram_put(&dst, (uint8_t)(dc >> 8));
            fdc_ram_put(&dst, (uint8_t)dc);
            for (int i = 0; i < 24; i++) fdc_ram_put(&dst, 0x4E);
        }
        while (dst < end) fdc_ram_put(&dst, 0x4E);
        fdc.dma_addr = end;
        fdc.sector_count = 0;
        fdc.status = 0x80;
        fdc_intrq();
        return;
    }

    /* write track (format): accept and discard, clean status - a loader
     * mid-flow must not see RNF for it */
    fdc.status = 0x80;
    fdc_intrq();
}


static void fdc_command(uint8_t v)
{
#ifdef STBOX_FDC_TRACE
    {
        stbox_fdc_ev_t *e = &stbox_fdc_trace[stbox_fdc_trace_idx++ & (STBOX_FDC_TRACE_N - 1)];
        e->cyc = g_total_cyc; e->cmd = v; e->track = fdc.track; e->sector = fdc.sector;
        e->data = fdc.data; e->side = (uint8_t)fdc_side(); e->was_pending = fdc.pending;
        e->dma = fdc.dma_addr; e->cnt = fdc.sector_count; e->result = 0xFF; g_fdc_last_ev = e;
    }
#endif
    if ((v & 0xF0) == 0xD0) {                  /* force interrupt      */
        fdc.pending = 0;
        fdc.status &= (uint8_t)~0x01;
        fdc.status |= 0x80;
        if (v & 0x0F) fdc_intrq();
        return;
    }
    if (fdc.pending) return;                   /* busy: ignored        */

    /* Loading a command deasserts INTRQ, like the chip (the status read
     * does too, below). Loaders that poll GPIP5 for completion and never
     * read the status between commands otherwise see the PREVIOUS
     * command's interrupt still pending, treat the new one as done
     * immediately, and issue the next - which is then ignored as busy.
     * Field case: Xenon 2's loader lost sector 1 of every track after
     * a seek, so the decrypted game was full of holes (4 bombs at $400). */
#ifndef STBOX_OLD_FDC_INTRQ                    /* harness A/B switch */
    mfp.gpip |= 0x20;
#endif

    fdc.cmd = v;
    fdc.status = 0x81;                         /* motor + busy         */

    /* Real-drive mode: forward the command to the physical WD1772 and
     * let the errand pump run it. Sandbox-side registers keep their
     * roles; completion arrives via stbox_rfdc.done in the slice. */
    if (stbox_rfdc.enabled && stbox_cpu_kick) {
        stbox_rfdc.cmd = v;
        stbox_rfdc.arg_track = fdc.data;
        stbox_rfdc.sector = fdc.sector;
        stbox_rfdc.side = (uint8_t)fdc_side();
        uint16_t cnt = fdc.sector_count ? fdc.sector_count : 1;
        if (!(v & 0x10) && v >= 0x80 && v < 0xC0)
            cnt = 1;                           /* single-sector cmd    */
        if (cnt > STBOX_RFDC_MAXSEC) cnt = STBOX_RFDC_MAXSEC;
        stbox_rfdc.count = cnt;
        if (v >= 0xA0 && v < 0xC0) {           /* write: preload data  */
            uint32_t bytes = (uint32_t)cnt * 512u;
            for (uint32_t i = 0; i < bytes; i++) {
                uint32_t a = (fdc.dma_addr + i) & 0xFFFFFF;
                stbox_rfdc.staging[i] = (a < g_ram_size) ? g_ram[a] : 0;
            }
        }
        fdc.pending = 2;                       /* 2 = real op in flight */
        stbox_rfdc.done = 0;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        stbox_rfdc.req = 1;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        stbox_errand_active = 1;
        stbox_rfdc.kick = 1;
        g_rfdc_next_kick = g_total_cyc + RFDC_KICK_CYC;
        stbox_cpu_kick();
        return;
    }

    uint32_t delay = SECT_CYC;

    if (v < 0x80) {                            /* type I               */
        uint8_t old = fdc.track;
        switch (v & 0xF0) {
            case 0x00: fdc.track = 0; break;                 /* restore */
            case 0x10: fdc.track = fdc.data; break;          /* seek    */
            case 0x40: case 0x50: fdc.step_dir = 1; goto step;
            case 0x60: case 0x70: fdc.step_dir = -1; goto step;
            case 0x20: case 0x30:                            /* step    */
            step:
                if (v & 0x10 || (v & 0xE0) != 0x20) {        /* update  */
                    int t = fdc.track + fdc.step_dir;
                    fdc.track = (uint8_t)(t < 0 ? 0 : t);
                }
                break;
        }
        int delta = fdc.track - old;
        if (delta < 0) delta = -delta;
        if (!delta) delta = 1;
        delay = (uint32_t)delta * SEEK_CYC;
        if (v & 0x04) delay += SECT_CYC;       /* verify adds a rev bit */
    }

    fdc.pending = 1;
    fdc.event_cyc = g_total_cyc + delay;
}

/* ================================================================== */
/* interrupt plumbing                                                 */
/* ================================================================== */
static uint8_t g_hbl_pending, g_vbl_pending;

static void update_irq(void)
{
    int level = 0;
    uint16_t pend = ((mfp.ipra & mfp.imra) << 8) | (mfp.iprb & mfp.imrb);
    if (pend)            level = 6;
    else if (g_vbl_pending) level = 4;
    else if (g_hbl_pending) level = 2;
    m68k_set_irq(level);
}

static void mfp_raise(int ch)
{
    uint8_t bit;
    if (ch >= 8) { bit = 1u << (ch - 8); if (mfp.iera & bit) mfp.ipra |= bit; }
    else         { bit = 1u << ch;       if (mfp.ierb & bit) mfp.iprb |= bit; }
    update_irq();
}

static int stbox_int_ack(int level)
{
    if (level == 6) {
        /* highest pending, enabled MFP channel */
        uint16_t pend = ((uint16_t)(mfp.ipra & mfp.imra) << 8) |
                        (mfp.iprb & mfp.imrb);
        for (int ch = 15; ch >= 0; ch--) {
            if (pend & (1u << ch)) {
                if (ch >= 8) {
                    uint8_t bit = 1u << (ch - 8);
                    mfp.ipra &= ~bit;
                    if (mfp.vr & 0x08) mfp.isra |= bit;   /* software EOI */
                } else {
                    uint8_t bit = 1u << ch;
                    mfp.iprb &= ~bit;
                    if (mfp.vr & 0x08) mfp.isrb |= bit;
                }
                update_irq();
                return (mfp.vr & 0xF0) + ch;
            }
        }
        update_irq();
        return M68K_INT_ACK_SPURIOUS;
    }
    if (level == 4) { g_vbl_pending = 0; update_irq(); return M68K_INT_ACK_AUTOVECTOR; }
    if (level == 2) { g_hbl_pending = 0; update_irq(); return M68K_INT_ACK_AUTOVECTOR; }
    return M68K_INT_ACK_SPURIOUS;
}

/* ================================================================== */
/* IKBD                                                               */
/* ================================================================== */
static void ikbd_tx(uint8_t b)      /* IKBD -> guest */
{
    uint8_t nt = (uint8_t)(acia.ft + 1);
    if (nt != acia.fh) { acia.fifo[acia.ft] = b; acia.ft = nt; }
}

/* bytes-of-parameters for guest->IKBD commands we must consume */
static uint8_t ikbd_cmd_len(uint8_t c)
{
    switch (c) {
        case 0x07: return 1;  case 0x09: return 4;  case 0x0A: return 2;
        case 0x0B: return 2;  case 0x0C: return 2;  case 0x0E: return 5;
        case 0x17: return 1;  case 0x19: return 6;  case 0x1B: return 6;
        case 0x20: return 3;  case 0x21: return 2;  case 0x22: return 3;
        case 0x80: return 1;
        default:   return 0;
    }
}

static void joy_resend(void);

static void ikbd_command(void)      /* complete command in acia.cmd */
{
#ifdef STBOX_IKBD_DEBUG
    fprintf(stderr, "[ikbd] cmd %02x len %u mode=%u\n",
            acia.cmd[0], acia.cmdneed, acia.mouse_mode);
#endif
    const int in_win = (g_total_cyc - acia.reset_cyc) < IKBD_RESET_WINDOW_CYC;
    switch (acia.cmd[0]) {
        case 0x08:                       /* relative mouse on */
            acia.mouse_mode = 0;
            if (in_win) acia.mouse_touched = 1;
            joy_resend();
            break;
        case 0x09: acia.mouse_mode = 1; break;   /* absolute       */
        case 0x0A: acia.mouse_mode = 2; break;   /* keycode        */
        case 0x12:                       /* mouse off */
            acia.mouse_mode = 3;
            acia.mouse_off_cmd = 1;
            if (in_win) {
                acia.mouse_touched = 1;
                if (acia.joy_off_cmd) {  /* $1A then $12: both back on */
                    acia.mouse_mode = 0; acia.joy_event = 1; acia.both = 1;
                }
            }
            joy_resend();
            break;
        case 0x14:                       /* joystick event reporting */
            acia.joy_event = 1;
            if (in_win && acia.mouse_touched) {
                acia.mouse_mode = 0; acia.both = 1;
            } else
                acia.mouse_mode = 3;     /* a real IKBD silences the mouse */
            /* the IKBD forgets previous joystick state and reports now */
            acia.joy_sent[0] = acia.joy_sent[1] = 0;
            joy_resend();
            break;
        case 0x15: acia.joy_event = 0; break;
        case 0x1A:
            acia.joy_event = 0;
            acia.joy_off_cmd = 1;
            if (in_win && acia.mouse_off_cmd) {  /* $12 then $1A */
                acia.mouse_mode = 0; acia.joy_event = 1; acia.both = 1;
            }
            break;
        case 0x80:                       /* RESET ($80 $01) */
            if (acia.cmd[1] == 0x01) {
                acia.fh = acia.ft = 0;
                acia.mouse_buttons = 0;
                acia.mouse_mode = 0;
                acia.joy_event = 1;      /* real IKBD: both on after reset */
                acia.reset_cyc = g_total_cyc;
                acia.mouse_touched = acia.both = 0;
                acia.mouse_off_cmd = acia.joy_off_cmd = 0;
                acia.joy_sent[0] = acia.joy_sent[1] = 0;
                ikbd_tx(0xF1);           /* version/self-test OK. TODO:
                                            verify $F0 vs $F1 against a
                                            real IKBD before games rely
                                            on it. */
            }
            break;
        case 0x0D: {                     /* interrogate mouse position */
            ikbd_tx(0xF7); ikbd_tx(0); ikbd_tx(0); ikbd_tx(0); ikbd_tx(0);
            break;
        }
        case 0x16:                       /* joystick interrogate */
            ikbd_tx(0xFD); ikbd_tx(acia.joy[0]); ikbd_tx(acia.joy[1]);
            break;
        case 0x1C:                       /* read clock: BCD zeros */
            ikbd_tx(0xFC);
            for (int i = 0; i < 6; i++) ikbd_tx(0);
            break;
        default: break;                  /* modes/config: accept silently */
    }
}

static void ikbd_rx(uint8_t b)      /* guest -> IKBD, one byte */
{
    if (acia.cmdlen == 0) {
        acia.cmd[0] = b; acia.cmdlen = 1;
        acia.cmdneed = 1 + ikbd_cmd_len(b);
    } else if (acia.cmdlen < sizeof(acia.cmd)) {
        acia.cmd[acia.cmdlen++] = b;
    }
    if (acia.cmdlen >= acia.cmdneed) { ikbd_command(); acia.cmdlen = 0; }
}

/* Put a port's joystick state on the wire if the IKBD mode allows and
 * it changed. Joystick 1's fire becomes the right mouse button while
 * the mouse is on (a real IKBD wires them together): it is stripped
 * from the $FF packet and an F8 packet with the button carries it. */
static void joy_emit(int port)
{
    uint8_t st = acia.joy[port];
    const int mouse_on = acia.mouse_mode != 3;
    if (port == 1) {
        const uint8_t rb = mouse_on && (st & STJOY_FIRE) ? 1 : 0;
        if (rb != acia.joy_rbutton) {
            acia.joy_rbutton = rb;
            if (acia.mouse_mode == 0) {
                ikbd_tx(0xF8 | (acia.mouse_buttons & 3) | rb);
                ikbd_tx(0); ikbd_tx(0);
            }
        }
        if (mouse_on) st &= (uint8_t)~STJOY_FIRE;
    }
    if (!acia.joy_event) return;
    if (port == 0 && mouse_on && !acia.both) return;  /* mouse owns port 0 */
    if (st == acia.joy_sent[port]) return;      /* only changes go out */
    acia.joy_sent[port] = st;
    ikbd_tx(0xFE + port); ikbd_tx(st);
}

static void joy_resend(void)
{
    joy_emit(1);
    joy_emit(0);
}

/* drain host input ring into the IKBD fifo (core 3 only) */
static void input_drain(void)
{
    while (g_in_head != g_in_tail) {
        uint32_t v = g_in_ring[g_in_tail & (INRING - 1)];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        g_in_tail++;
        uint8_t type = v >> 24, a = v >> 16, b = v >> 8, c = v;
        if (type == 0) {                          /* key */
            ikbd_tx(a);
        } else if (type == 1) {                   /* mouse relative */
            acia.mouse_buttons = c & 3;
            if (acia.mouse_mode == 0) {           /* only when the game
                                                     wants packets      */
                ikbd_tx(0xF8 | (c & 3) | acia.joy_rbutton);
                ikbd_tx(a); ikbd_tx(b);
            }
        } else if (type == 2) {                   /* joystick state */
            acia.joy[a & 1] = b;
            acia.pad[a & 1] = c;
            joy_emit(a & 1);
        } else if (type == 3) {                   /* raw real-IKBD byte  */
            ikbd_tx(a);
        }
    }
}

/* serial-pace one fifo byte into the ACIA when due */
static void acia_pump(void)
{
    if ((acia.sr & 0x01) || acia.fh == acia.ft) return;
    if (g_total_cyc < acia.next_rx_cyc) return;
    acia.rx = acia.fifo[acia.fh++];
    g_acia_rx_cnt++;
    acia.sr |= 0x81;                              /* RDRF + IRQ */
    acia.next_rx_cyc = (uint32_t)g_total_cyc + IKBD_BYTE_CYC;
    mfp.gpip &= (uint8_t)~0x10;                   /* GPIP4 low = ACIA irq */
    mfp_raise(MFP_CH_ACIA);
}

/* ================================================================== */
/* MFP timers                                                         */
/* ================================================================== */
static void mfp_timer_tick(uint32_t *acc, uint8_t cr, uint8_t *cnt,
                           uint8_t reload, int ch, uint32_t mfp_ticks)
{
    uint16_t pre = mfp_presc[cr & 7];
    if (!pre) return;                             /* stopped or event mode */
    *acc += mfp_ticks;
    while (*acc >= pre) {
        *acc -= pre;
        if (--(*cnt) == 0) {
            *cnt = reload ? reload : 0;           /* 0 == 256 on real MFP */
            if (!*cnt) *cnt = reload;
            mfp_raise(ch);
        }
    }
}

static void mfp_advance(uint32_t guest_cycles)
{
    g_mfp_fp += (uint64_t)guest_cycles * MFP_FP_STEP;
    uint32_t ticks = (uint32_t)(g_mfp_fp >> 32);
    if (!ticks) return;
    g_mfp_fp &= 0xFFFFFFFFu;

    mfp_timer_tick(&mfp.ta_acc, mfp.tacr, &mfp.tamain, mfp.tadr,
                   MFP_CH_TIMER_A, ticks);
    if (!(mfp.tbcr & 0x08))                       /* delay mode only here */
        mfp_timer_tick(&mfp.tb_acc, mfp.tbcr, &mfp.tbmain, mfp.tbdr,
                       MFP_CH_TIMER_B, ticks);
    mfp_timer_tick(&mfp.tc_acc, (mfp.tcdcr >> 4) & 7, &mfp.tcmain, mfp.tcdr,
                   MFP_CH_TIMER_C, ticks);
    mfp_timer_tick(&mfp.td_acc, mfp.tcdcr & 7, &mfp.tdmain, mfp.tddr,
                   MFP_CH_TIMER_D, ticks);
}

/* Display-enable window: Timer B's event input is DE, which pulses only on
 * VISIBLE lines - roughly 200 of the 313 (PAL, 50 Hz, no borders opened).
 * TOS's boot-time VBL synchronization counts on the pulses STOPPING during
 * vertical blank (it polls for a ~616-iteration gap), so modelling DE on
 * every line hangs it. Games' raster effects count on the window position,
 * so keep it at the standard PAL frame layout. */

/* Timer A in event mode counts STE DMA-sound frame ends (its event input
 * is the sound DMA's frame-end signal on an STE; nothing drives it on an
 * ST, which is why plain-ST code never runs it in that mode) */
static void mfp_timera_event(void)
{
    if ((mfp.tacr & 0x0F) == 0x08) {
        if (--mfp.tamain == 0) {
            mfp.tamain = mfp.tadr;
            mfp_raise(MFP_CH_TIMER_A);
        }
    }
}

/* Timer B in event mode counts end-of-line (DE trailing edge) pulses */
static void mfp_timerb_event(void)
{
    if (g_line < DE_FIRST_LINE || g_line >= DE_FIRST_LINE + DE_LINES)
        return;
    if ((mfp.tbcr & 0x0F) == 0x08) {
        if (--mfp.tbmain == 0) {
            mfp.tbmain = mfp.tbdr;
            mfp_raise(MFP_CH_TIMER_B);
        }
    }
}

/* ================================================================== */
/* Musashi bus interface (renamed via m68kconf.h)                     */
/*                                                                    */
/* DOUBLE BUS FAULT = HALT, like the silicon. A guest faulting with a */
/* garbage supervisor stack makes the exception-frame pushes fault    */
/* too; recursing through m68k_pulse_bus_error() overflows the HOST   */
/* stack (field backtrace: 50+ frames of hw_read -> pulse -> hw_read  */
/* on the ipl thread). The real 68000 halts on a fault-within-fault;  */
/* so do we: freeze the box, frozen frame on screen, until reset.     */
/* ================================================================== */
/* last-N program counters, filled by the Musashi instruction hook
 * (see m68kconf.h) - dumped with the halt report so a crash names the
 * road it took, not just the wall it hit */
uint32_t stbox_pc_ring[STBOX_PC_RING];
volatile unsigned stbox_pc_ring_idx;
void stbox_pc_hook(unsigned int pc)
{
    stbox_pc_ring[stbox_pc_ring_idx++ & (STBOX_PC_RING - 1)] = pc;
}

static int g_berr_nest;              /* inside bus-error processing    */
static volatile int g_halted;        /* 0 run, 1 halted, 2 unreported  */
stbox_halt_info_t stbox_halt_info;   /* forensics for the halt report  */

static void stbox_buserr(uint32_t addr)
{
    if (g_halted) return;
    if (g_berr_nest) {               /* fault during fault: halt      */
        stbox_halt_info.fault2 = addr;
        stbox_halt_info.pc  = m68k_get_reg(NULL, M68K_REG_PC);
        stbox_halt_info.ppc = m68k_get_reg(NULL, M68K_REG_PPC);
        stbox_halt_info.sr  = m68k_get_reg(NULL, M68K_REG_SR);
        stbox_halt_info.sp  = m68k_get_reg(NULL, M68K_REG_SP);
        g_halted = 2;                /* render thread reports it once */
        return;
    }
    stbox_halt_info.fault1 = addr;   /* the fault that started it all */
    g_berr_nest = 1;
    m68k_pulse_bus_error();
    g_berr_nest = 0;                 /* only reached if no longjmp    */
}
/* every site below has the (masked) address in a local named 'a' */
#define m68k_pulse_bus_error() stbox_buserr(a)

/* ================================================================== */
/* hardware page dispatch                                             */
/* ================================================================== */
/* Decode granularity: on a real ST the GLUE DTACKs whole register BLOCKS;
 * unimplemented addresses inside a decoded block are silent no-ops that
 * read back 0 - TOS 2.06's STE detection depends on it (it pokes $FF820D
 * with the vector table deliberately trashed, then reads it back). Bus
 * errors are only for entirely absent blocks: blitter on a pre-blitter
 * ST, STE DMA sound, and everything undecoded. */
static int hw_decoded(uint32_t a)
{
    if (a >= 0xFF8000 && a <= 0xFF800F) return 1;   /* MMU config      */
    if (a >= 0xFF8200 && a <= 0xFF827F) return 1;   /* GLUE/Shifter    */
    if (a >= 0xFF8600 && a <= 0xFF860F) return 1;   /* DMA/FDC         */
    if (a >= 0xFF8800 && a <= 0xFF88FF) return 1;   /* PSG (mirrors)   */
    if (a >= 0xFFFA00 && a <= 0xFFFA3F) return 1;   /* MFP             */
    if (a >= 0xFFFC00 && a <= 0xFFFC07) return 1;   /* ACIAs           */
    if (g_ste) {
        if (a >= 0xFF8900 && a <= 0xFF893F) return !ste_nodma();  /* DMA snd */
        if (a >= 0xFF8A00 && a <= 0xFF8A3F) return !ste_noblit(); /* BLiTTER */
        if (a >= 0xFF9200 && a <= 0xFF923F) return 1;             /* joypads */
    }
    return 0;
}

static uint32_t hw_read(uint32_t a, int size);
static void     hw_write(uint32_t a, uint32_t v, int size);
static uint16_t mw_data_read(void);           /* STE microwire, below */
static uint16_t mw_mask_read(void);

static uint8_t mfp_read(uint32_t a)
{
    switch (a & 0x3F) {
        case 0x01: return (uint8_t)((mfp.gpip & ~mfp.ddr) | 0x80); /* GPIP7=1: color */
        case 0x03: return mfp.aer;   case 0x05: return mfp.ddr;
        case 0x07: return mfp.iera;  case 0x09: return mfp.ierb;
        case 0x0B: return mfp.ipra;  case 0x0D: return mfp.iprb;
        case 0x0F: return mfp.isra;  case 0x11: return mfp.isrb;
        case 0x13: return mfp.imra;  case 0x15: return mfp.imrb;
        case 0x17: return mfp.vr;
        case 0x19: return mfp.tacr;  case 0x1B: return mfp.tbcr;
        case 0x1D: return mfp.tcdcr;
        case 0x1F: return mfp.tamain; case 0x21: return mfp.tbmain;
        case 0x23: return mfp.tcmain; case 0x25: return mfp.tdmain;
        case 0x27: return mfp.scr;   case 0x29: return mfp.ucr;
        case 0x2B: return mfp.rsr;   case 0x2D: return mfp.tsr | 0x80;
        case 0x2F: return mfp.udr;
        default:   return 0;
    }
}

static void mfp_write(uint32_t a, uint8_t v)
{
    switch (a & 0x3F) {
        case 0x01: mfp.gpip = v; break;
        case 0x03: mfp.aer  = v; break;
        case 0x05: mfp.ddr  = v; break;
        case 0x07: mfp.iera = v; mfp.ipra &= v; break;
        case 0x09: mfp.ierb = v; mfp.iprb &= v; break;
        case 0x0B: mfp.ipra &= v; break;          /* write 0 to clear */
        case 0x0D: mfp.iprb &= v; break;
        case 0x0F: mfp.isra &= v; break;
        case 0x11: mfp.isrb &= v; break;
        case 0x13: mfp.imra = v; break;
        case 0x15: mfp.imrb = v; break;
        case 0x17: mfp.vr   = v; break;
        case 0x19: mfp.tacr = v & 0x0F;
                   if (!(v & 7)) mfp.tamain = mfp.tadr;
                   break;
        case 0x1B: mfp.tbcr = v & 0x0F;
                   if (!(v & 7)) mfp.tbmain = mfp.tbdr;
                   break;
        case 0x1D: mfp.tcdcr = v & 0x77; break;
        case 0x1F: mfp.tadr = v; if (!(mfp.tacr & 7)) mfp.tamain = v; break;
        case 0x21: mfp.tbdr = v; if (!(mfp.tbcr & 7)) mfp.tbmain = v; break;
        case 0x23: mfp.tcdr = v; if (!((mfp.tcdcr >> 4) & 7)) mfp.tcmain = v; break;
        case 0x25: mfp.tddr = v; if (!(mfp.tcdcr & 7)) mfp.tdmain = v; break;
        case 0x27: mfp.scr = v; break;
        case 0x29: mfp.ucr = v; break;
        case 0x2B: mfp.rsr = v; break;
        case 0x2D: mfp.tsr = v; break;
        case 0x2F: mfp.udr = v; break;
        default: break;
    }
    update_irq();
}

static uint32_t hw_read(uint32_t a, int size)
{
    if (dbg_on()) {
        stbox_dbg_last_hwr = a;
        stbox_dbg_last_hwr_pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PPC);
    }
    /* memory controller */
    if (a == 0xFF8001) return g_memcfg;

    /* Shifter/GLUE video */
    if (a == 0xFF8201) return (g_vid_base >> 16) & 0xFF;
    if (a == 0xFF8203) return (g_vid_base >> 8) & 0xFF;
    if (a >= 0xFF8205 && a <= 0xFF8209) {          /* video counter */
        /* base through the top border; +stride per visible line, byte per
         * 2 cycles inside the DE window; base+200 lines through the bottom
         * border until VBL reloads. Was one byte per 2 cycles for the
         * WHOLE frame (80 KB by the bottom), which fed raster-sync code
         * garbage line numbers (field case: Xenon 2, address error with
         * $FF8207 in A0 in both ST and STE mode). STE: linewidth and the
         * extra hscroll word group are part of the per-line stride. */
        uint32_t stride = DE_BYTES;
        if (g_ste) stride += (uint32_t)g_linewidth * 2 + (g_hscroll ? 8u : 0u);
        uint32_t line = g_line, cyc = g_frame_cyc - (g_next_line_cyc - CYC_PER_LINE);
        uint32_t off;
#ifdef STBOX_OLD_VIDCOUNTER                    /* harness A/B: pre-fix model */
        off = (g_frame_cyc / 2) & ~1u; (void)line; (void)cyc; (void)stride;
#else
        if (line < DE_FIRST_LINE)                  off = 0;
        else if (line >= DE_FIRST_LINE + DE_LINES) off = DE_LINES * stride;
        else {
            uint32_t in = 0;
            if (cyc > DE_START_CYC) {
                in = (cyc - DE_START_CYC) / 2;
                if (in > DE_BYTES) in = DE_BYTES;
            }
            off = (line - DE_FIRST_LINE) * stride + (in & ~1u);
        }
#endif
        uint32_t vc = (g_vid_base + off) & 0xFFFFFF;
        if (a == 0xFF8205) return (vc >> 16) & 0xFF;
        if (a == 0xFF8207) return (vc >> 8) & 0xFF;
        return vc & 0xFF;
    }
    if (a == 0xFF820A) return g_sync;
    if (a >= 0xFF8240 && a < 0xFF8260) {
        uint16_t w = g_pal[(a - 0xFF8240) >> 1];
        if (size == 2) return w;
        return (a & 1) ? (w & 0xFF) : (w >> 8);
    }
    if (a == 0xFF8260) return g_res;

    if (g_ste) {
        /* STE shifter */
        if (a == 0xFF820D) return g_vid_base & 0xFE;
        if (a == 0xFF820F) return g_linewidth;
        if (a == 0xFF8264 || a == 0xFF8265) return g_hscroll;

        /* DMA sound (byte registers on odd addresses; word reads give the
         * register in the low byte, 0 in the high, like the chip) */
        if (!ste_nodma() && a >= 0xFF8900 && a <= 0xFF893F) {
            if (size == 2 && (a & 1) == 0 && a != 0xFF8922 && a != 0xFF8924)
                return hw_read(a + 1, 1) & 0xFF;
            switch (a) {
                case 0xFF8901: return dma.ctrl & 0x03;
                case 0xFF8903: return (dma.start >> 16) & 0x3F;
                case 0xFF8905: return (dma.start >> 8) & 0xFF;
                case 0xFF8907: return dma.start & 0xFE;
                case 0xFF8909: return (dma.cur >> 16) & 0x3F;
                case 0xFF890B: return (dma.cur >> 8) & 0xFF;
                case 0xFF890D: return dma.cur & 0xFE;
                case 0xFF890F: return (dma.end >> 16) & 0x3F;
                case 0xFF8911: return (dma.end >> 8) & 0xFF;
                case 0xFF8913: return dma.end & 0xFE;
                case 0xFF8921: return dma.mode & 0x83;
                case 0xFF8922: { uint16_t d = mw_data_read(); return size == 2 ? d : (d >> 8); }
                case 0xFF8923: return mw_data_read() & 0xFF;
                case 0xFF8924: { uint16_t m = mw_mask_read(); return size == 2 ? m : (m >> 8); }
                case 0xFF8925: return mw_mask_read() & 0xFF;
                default: return 0;
            }
        }

        /* BLiTTER */
        if (!ste_noblit() && a >= 0xFF8A00 && a <= 0xFF8A3F)
            return stbox_blit_reg_read(a, size);

        /* enhanced joystick ports, host pads merged in (active low).
         * $FF9200: low byte = fire buttons of the selected column (bit 1
         * pad A, bit 0 pad A PAUSE; bits 3/2 pad B), high byte = DIP
         * switches (none: $FF). $FF9202: high byte = directions of the
         * selected column (low nibble pad A, high nibble pad B), low
         * byte $FF. Column select = low byte last written to $FF9202,
         * bits 0-3 pad A, 4-7 pad B, bit 0/4 = stick + A + PAUSE,
         * 1/5 = B, 2/6 = C, 3/7 = OPTION (Jaguar pad, as Hatari joy.c). */
        if (a >= 0xFF9200 && a <= 0xFF9203) {
            uint32_t w = 0xFFFFFFFFu;             /* 9200.w : 9202.w */
            for (int p = 0; p < 2; p++) {         /* p: 0 = pad A (joy 1) */
                const int sel = (acia.pad_select >> (p * 4)) & 0x0F;
                const uint8_t jb = acia.pad[p ? 0 : 1], js = acia.joy[p ? 0 : 1];
                uint8_t btn = 0x03, dirs = 0x0F;
                if (sel != 0x0F) {
                    if (!(sel & 1)) {
                        dirs = (uint8_t)(~js & 0x0F);
                        if (jb & STPAD_A)     btn &= (uint8_t)~0x02;
                        if (jb & STPAD_PAUSE) btn &= (uint8_t)~0x01;
                    } else if (!(sel & 2)) { if (jb & STPAD_B)      btn &= (uint8_t)~0x02; }
                    else if (!(sel & 4))   { if (jb & STPAD_C)      btn &= (uint8_t)~0x02; }
                    else if (!(sel & 8))   { if (jb & STPAD_OPTION) btn &= (uint8_t)~0x02; }
                }
                w &= ~(uint32_t)(((0x03 ^ btn) << (16 + p * 2)) |
                                 ((0x0F ^ dirs) << (8 + p * 4)));
            }
            const int off = a & 3;
            if (size == 4) return w;
            if (size == 2) return (w >> (16 - off * 8)) & 0xFFFF;
            return (w >> (24 - off * 8)) & 0xFF;
        }
        if (a >= 0xFF9204 && a <= 0xFF923F) return 0;
    }

    /* DMA/FDC */
    if (a == 0xFF8604) {                           /* disk controller / count */
        if (fdc.dma_mode & 0x10) return fdc.sector_count;
        switch ((fdc.dma_mode >> 1) & 3) {
            case 0: {
                /* status read releases INTRQ, like the real WD1772 */
                mfp.gpip |= 0x20;
                uint8_t st = fdc.status;
                if (fdc.pending) st |= 0x01;       /* busy while in flight */
                /* index pulse, 300 rpm, for loaders that count on it */
                if (fdc.cmd < 0x80 && diska.present &&
                    (g_total_cyc % REV_CYC) < 6000)
                    st |= 0x02;
                return st;
            }
            case 1: return fdc.track;
            case 2: return fdc.sector;
            default: return fdc.data;
        }
    }
    if (a == 0xFF8606)                             /* DMA status */
        return 0x01 | (fdc.sector_count ? 0x02 : 0x00);
    if (a == 0xFF8609) return (fdc.dma_addr >> 16) & 0xFF;
    if (a == 0xFF860B) return (fdc.dma_addr >> 8) & 0xFF;
    if (a == 0xFF860D) return fdc.dma_addr & 0xFF;

    /* PSG */
    if (a == 0xFF8800) return g_psg_reg[g_psg_sel & 15];
    if (a == 0xFF8802) return 0xFF;

    /* MFP */
    if (a >= 0xFFFA01 && a <= 0xFFFA3F) return mfp_read(a);

    /* ACIAs */
    if (a == 0xFFFC00) return acia.sr;
    if (a == 0xFFFC02) {
        acia.sr &= (uint8_t)~0x81;                 /* clear RDRF + IRQ */
        mfp.gpip |= 0x10;                          /* GPIP4 back high  */
        return acia.rx;
    }
    if (a == 0xFFFC04) return 0x02;                /* MIDI: TDRE, no rx */
    if (a == 0xFFFC06) return 0;

    /* gaps inside decoded blocks read 0; absent blocks bus error */
    if (hw_decoded(a)) return 0;
    m68k_pulse_bus_error();
    return 0;
}

static void hw_write(uint32_t a, uint32_t v, int size)
{
    if (a == 0xFF8001) { g_memcfg = (uint8_t)v; ram_recfg(); return; }

    /* On an STE, writing the high or mid video-base byte clears the low
     * byte (the shifter's 3-byte latch), so ST-style two-byte setters
     * still land on a 256-byte boundary. */
    if (a == 0xFF8201) { g_vid_base = (g_vid_base & (g_ste ? 0x00FFFF : 0x00FF00)) |
                                      ((v & 0xFF) << 16);
                         stbox_shared.video_base = g_vid_base; return; }
    if (a == 0xFF8203) { g_vid_base = (g_vid_base & (g_ste ? 0xFF00FF : 0xFF0000)) |
                                      ((v & 0xFF) << 8);
                         stbox_shared.video_base = g_vid_base; return; }
    if (a == 0xFF820A) {
        g_sync = (uint8_t)v;
        /* Bottom-border removal: the GLUE's vertical-border compare is at
         * the start of line 263 (PAL). A game defeats it by flipping the
         * sync rate for one line right there, and the display then runs on
         * to the physical bottom - about 32 more low-res lines. A write to
         * SYNC (or REZ) in that window is the whole signature; anywhere
         * else it is an ordinary rate set and means nothing here. */
        if (g_line >= 260 && g_line <= 266)
            g_bottom_open = 1;
        return;
    }
    if (a >= 0xFF8240 && a < 0xFF8260) {
        int idx = (a - 0xFF8240) >> 1;
        uint16_t w = g_pal[idx];
        if (size == 2)      w = (uint16_t)v;
        else if (a & 1)     w = (uint16_t)((w & 0xFF00) | (v & 0xFF));
        else                w = (uint16_t)((w & 0x00FF) | ((v & 0xFF) << 8));
        g_pal[idx] = w & (g_ste ? 0x0FFF : 0x0777);   /* STE: 4 bits/gun */
        stbox_shared.palette[idx] = g_pal[idx];
        return;
    }
    if (a == 0xFF8260) {
        g_res = v & 3; stbox_shared.shift_res = g_res;
        if (g_line >= 260 && g_line <= 266)
            g_bottom_open = 1;
        return;
    }

    if (g_ste) {
        /* STE shifter */
        if (a == 0xFF820D) { g_vid_base = (g_vid_base & 0xFFFF00) | (v & 0xFE);
                             stbox_shared.video_base = g_vid_base; return; }
        if (a == 0xFF820F) { g_linewidth = (uint8_t)v;
                             stbox_shared.linewidth = g_linewidth; return; }
        if (a == 0xFF8264 || a == 0xFF8265) {
                             g_hscroll = (uint8_t)(v & 0x0F);
                             stbox_shared.hscroll = g_hscroll; return; }
        if (a >= 0xFF8205 && a <= 0xFF8209) return;   /* counter writes: sink */

        /* DMA sound */
        if (!ste_nodma() && a >= 0xFF8900 && a <= 0xFF893F) {
            if (size == 2 && (a & 1) == 0 && a != 0xFF8922 && a != 0xFF8924) {
                hw_write(a + 1, v & 0xFF, 1);         /* register is the low byte */
                return;
            }
            switch (a) {
                case 0xFF8901: {
                    uint8_t was = dma.ctrl;
                    dma.ctrl = (uint8_t)(v & 0x03);
                    if ((dma.ctrl & 1) && !(was & 1)) {   /* play: latch frame */
                        dma.cur = dma.start; dma.cur_end = dma.end;
                        dma.fp = 0; dma.sub = 0; dma.l = dma.r = 0;
                    }
                    stbox_dma_playing = dma.ctrl & 1;
                    return;
                }
                case 0xFF8903: dma.start = (dma.start & 0x00FFFF) | ((v & 0x3F) << 16); return;
                case 0xFF8905: dma.start = (dma.start & 0x3F00FF) | ((v & 0xFF) << 8);  return;
                case 0xFF8907: dma.start = (dma.start & 0x3FFF00) | (v & 0xFE);         return;
                case 0xFF890F: dma.end   = (dma.end   & 0x00FFFF) | ((v & 0x3F) << 16); return;
                case 0xFF8911: dma.end   = (dma.end   & 0x3F00FF) | ((v & 0xFF) << 8);  return;
                case 0xFF8913: dma.end   = (dma.end   & 0x3FFF00) | (v & 0xFE);         return;
                case 0xFF8921: dma.mode = (uint8_t)(v & 0x83); return;
                case 0xFF8922: case 0xFF8923: {
                    /* LMC1992 microwire: 11-bit frame, device address 10 =
                     * the mixer; functions 011 master, 101 left, 100 right,
                     * 000 mixing. Volumes reach the host as the raw indexes
                     * (0-40 / 0-20), converted to dB gains there. */
                    if (size == 2)      dma.mw_data = (uint16_t)v;
                    else if (a & 1)     dma.mw_data = (uint16_t)((dma.mw_data & 0xFF00) | (v & 0xFF));
                    else                dma.mw_data = (uint16_t)((dma.mw_data & 0x00FF) | ((v & 0xFF) << 8));
                    if (size == 2 || (a & 1))
                        dma.mw_start = g_total_cyc ? g_total_cyc : 1;   /* start shifting */
                    uint16_t d = dma.mw_data;
                    if (((d >> 9) & 3) == 2) {
                        switch ((d >> 6) & 7) {
                            case 3: stbox_dma_lmc_master = d & 0x3F; break;
                            case 4: stbox_dma_lmc_right  = d & 0x1F; break;
                            case 5: stbox_dma_lmc_left   = d & 0x1F; break;
                            default: break;      /* mixing/bass/treble: not modelled */
                        }
                    }
                    return;
                }
                case 0xFF8924: case 0xFF8925:
                    if (size == 2)      dma.mw_mask = (uint16_t)v;
                    else if (a & 1)     dma.mw_mask = (uint16_t)((dma.mw_mask & 0xFF00) | (v & 0xFF));
                    else                dma.mw_mask = (uint16_t)((dma.mw_mask & 0x00FF) | ((v & 0xFF) << 8));
                    return;
                default: return;
            }
        }

        /* BLiTTER */
        if (!ste_noblit() && a >= 0xFF8A00 && a <= 0xFF8A3F) { stbox_blit_reg_write(a, v, size); return; }

        /* joypad column select ($FF9202, low byte of the word) */
        if (a >= 0xFF9200 && a <= 0xFF9203) {
            const int off = a & 3;
            if (size == 4)      acia.pad_select = (uint16_t)v;
            else if (size == 2) { if (off == 2) acia.pad_select = (uint16_t)v; }
            else if (off == 2)  acia.pad_select = (uint16_t)((acia.pad_select & 0xFF) | (v << 8));
            else if (off == 3)  acia.pad_select = (uint16_t)((acia.pad_select & 0xFF00) | (v & 0xFF));
            return;
        }
        /* paddles / lightpen: sink */
        if (a >= 0xFF9204 && a <= 0xFF923F) return;
    }

    /* DMA/FDC */
    if (a == 0xFF8604) {
        if (fdc.dma_mode & 0x10) { fdc.sector_count = (uint16_t)v; return; }
        switch ((fdc.dma_mode >> 1) & 3) {
            case 0: fdc_command((uint8_t)v); break;
            case 1: fdc.track  = (uint8_t)v; break;
            case 2: fdc.sector = (uint8_t)v; break;
            default: fdc.data  = (uint8_t)v; break;
        }
        return;
    }
    if (a == 0xFF8606) { fdc.dma_mode = (uint16_t)v; return; }
    if (a == 0xFF8609) { fdc.dma_addr = (fdc.dma_addr & 0x00FFFF) | ((v & 0xFF) << 16); return; }
    if (a == 0xFF860B) { fdc.dma_addr = (fdc.dma_addr & 0xFF00FF) | ((v & 0xFF) << 8); return; }
    if (a == 0xFF860D) { fdc.dma_addr = (fdc.dma_addr & 0xFFFF00) | (v & 0xFF); return; }

    /* PSG */
    if (a == 0xFF8800) { g_psg_sel = (uint8_t)v; return; }
    if (a == 0xFF8802) { g_psg_reg[g_psg_sel & 15] = (uint8_t)v;
                         psg_snoop(g_psg_sel & 15, (uint8_t)v); return; }

    /* MFP */
    if (a >= 0xFFFA01 && a <= 0xFFFA3F) { mfp_write(a, (uint8_t)v); return; }

    /* ACIAs */
    if (a == 0xFFFC00) { acia.cr = (uint8_t)v; return; }
    if (a == 0xFFFC02) { ikbd_rx((uint8_t)v); return; }
    if (a == 0xFFFC04 || a == 0xFFFC06) return;    /* MIDI: sink */

    if (hw_decoded(a)) return;                     /* block gap: sink */
    m68k_pulse_bus_error();
}

/* ================================================================== */
/* Musashi bus interface (renamed via m68kconf.h)                     */
/* ================================================================== */
static inline int rom_hit(uint32_t a)
{
    return a >= g_rom_base && a < g_rom_base + g_rom_size;
}

unsigned int stbox_bus_r8(unsigned int address)
{
    uint32_t a = address & 0xFFFFFF;
    if (a < 8) return g_rom[a];                    /* reset-vector shadow */
    if (a < 0x400000) {
        int ok; uint32_t m = ram_map(a, &ok);
        if (ok == 1) return g_ram[m];
        if (ok == 0) return 0xFF;                  /* open bus            */
        /* beyond configured RAM: fall through (bus error below)          */
    }
    if (rom_hit(a)) return g_rom[a - g_rom_base];
    if (a >= 0xFA0000 && a < 0xFC0000) return 0xFF; /* empty cartridge   */
    if (a >= 0xFF8000) return hw_read(a, 1) & 0xFF;
    m68k_pulse_bus_error();
    return 0;
}

unsigned int stbox_bus_r16(unsigned int address)
{
    uint32_t a = address & 0xFFFFFF;
    if (a < 8) return (g_rom[a] << 8) | g_rom[a + 1];
    if (a + 1 < 0x400000) {
        int ok; uint32_t m = ram_map(a, &ok);
        if (ok == 1) return ((uint32_t)g_ram[m] << 8) | g_ram[m + 1];
        if (ok == 0) return 0xFFFF;
    }
    if (rom_hit(a)) { uint32_t o = a - g_rom_base;
                      return (g_rom[o] << 8) | g_rom[o + 1]; }
    if (a >= 0xFA0000 && a < 0xFC0000) return 0xFFFF;
    if (a >= 0xFF8000) return hw_read(a, 2) & 0xFFFF;
    m68k_pulse_bus_error();
    return 0;
}

/* Exception forensics: Musashi fetches the vector as a long read from
 * the vector table, so a read of $08 (bus error), $0C (address error),
 * $10 (illegal) or $20 (privilege) IS the exception being taken. Record
 * the faulting instruction (PPC), PC, SR, SP and the word at PPC here -
 * memory-only - and let the render thread print it (2/3-bomb reports
 * would otherwise vanish into TOS's handler). */
stbox_exc_info_t stbox_exc_ring[STBOX_EXC_RING];
volatile unsigned stbox_exc_count;
/* trace (vector 9) is taken per instruction by trace-based protections,
 * so it gets a counter, not a ring entry; the first one is recorded so
 * the host can say where trace was switched on and where its handler is */
volatile unsigned stbox_trace_count;
volatile uint32_t stbox_trace_first_ppc, stbox_trace_first_pc, stbox_trace_vec;
volatile uint32_t stbox_trace_first_sp, stbox_trace_first_a0, stbox_trace_first_sr;
volatile uint32_t stbox_trace_first_stack[6];

unsigned int stbox_bus_r32(unsigned int address)
{
    uint32_t a = address & 0xFFFFFF;
    if (a == 0x24) {                                   /* trace */
        if (stbox_trace_count++ == 0) {
            stbox_trace_first_ppc = m68k_get_reg(NULL, M68K_REG_PPC);
            stbox_trace_first_pc  = m68k_get_reg(NULL, M68K_REG_PC);
            stbox_trace_first_sp  = m68k_get_reg(NULL, M68K_REG_SP);
            stbox_trace_first_a0  = m68k_get_reg(NULL, M68K_REG_A0);
            stbox_trace_first_sr  = m68k_get_reg(NULL, M68K_REG_SR);
            for (int i = 0; i < 6; i++) {
                uint32_t sa = (stbox_trace_first_sp + (uint32_t)i * 4) & 0xFFFFFF;
                int ok2; uint32_t m2 = ram_map(sa, &ok2);
                stbox_trace_first_stack[i] = (ok2 == 1 && sa + 3 < 0x400000)
                    ? ((uint32_t)g_ram[m2] << 24) | ((uint32_t)g_ram[m2+1] << 16) |
                      ((uint32_t)g_ram[m2+2] << 8) | g_ram[m2+3] : 0xDEADBEEFu;
            }
            int ok; uint32_t m = ram_map(0x24, &ok);
            stbox_trace_vec = ok == 1 ? ((uint32_t)g_ram[m] << 24) | ((uint32_t)g_ram[m+1] << 16) |
                                        ((uint32_t)g_ram[m+2] << 8) | g_ram[m+3] : 0;
        }
    }
    if (a == 0x08 || a == 0x0C || a == 0x10 || a == 0x20) {
        stbox_exc_info_t *e = &stbox_exc_ring[stbox_exc_count & (STBOX_EXC_RING - 1)];
        e->vector = a >> 2;
        e->ppc = m68k_get_reg(NULL, M68K_REG_PPC);
        e->pc  = m68k_get_reg(NULL, M68K_REG_PC);
        e->sr  = m68k_get_reg(NULL, M68K_REG_SR);
        e->sp  = m68k_get_reg(NULL, M68K_REG_SP);
        e->a0  = m68k_get_reg(NULL, M68K_REG_A0);
        e->a1  = m68k_get_reg(NULL, M68K_REG_A1);
        e->a6  = m68k_get_reg(NULL, M68K_REG_A6);
        e->d0  = m68k_get_reg(NULL, M68K_REG_D0);
        e->cycles = g_total_cyc;
        for (int i = 0; i < 8; i++) {            /* stack at the fault */
            uint32_t sa = (e->sp + (uint32_t)i * 4) & 0xFFFFFF;
            int ok; uint32_t m = ram_map(sa, &ok);
            e->stack[i] = (ok == 1 && sa + 3 < 0x400000)
                ? ((uint32_t)g_ram[m] << 24) | ((uint32_t)g_ram[m + 1] << 16) |
                  ((uint32_t)g_ram[m + 2] << 8) | g_ram[m + 3]
                : 0xDEADBEEFu;
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);
        stbox_exc_count++;
    }
    return (stbox_bus_r16(address) << 16) | stbox_bus_r16(address + 2);
}

void stbox_bus_w8(unsigned int address, unsigned int value)
{
    uint32_t a = address & 0xFFFFFF;
    if (a < 0x400000) {
        int ok; uint32_t m = ram_map(a, &ok);
        if (ok == 1) {
            if (stbox_watch_hi) watch_w(a, value, m68k_get_reg(NULL, M68K_REG_PPC));
            if (a >= 8) g_ram[m] = (uint8_t)value;
            return;
        }
        if (ok == 0) return;                       /* open bus: sink      */
    }
    if (a >= 0xFF8000) { hw_write(a, value, 1); return; }
    if (rom_hit(a) || (a >= 0xFA0000 && a < 0xFC0000)) return; /* ROM: sink */
    m68k_pulse_bus_error();
}

void stbox_bus_w16(unsigned int address, unsigned int value)
{
    uint32_t a = address & 0xFFFFFF;
    if (a + 1 < 0x400000) {
        int ok; uint32_t m = ram_map(a, &ok);
        if (ok == 1) {
            if (stbox_watch_hi) watch_w(a, value, m68k_get_reg(NULL, M68K_REG_PPC));
            if (a >= 8) { g_ram[m] = (uint8_t)(value >> 8);
                          g_ram[m + 1] = (uint8_t)value; }
            return;
        }
        if (ok == 0) return;
    }
    if (a >= 0xFF8000) { hw_write(a, value, 2); return; }
    if (rom_hit(a) || (a >= 0xFA0000 && a < 0xFC0000)) return;
    m68k_pulse_bus_error();
}

void stbox_bus_w32(unsigned int address, unsigned int value)
{
    stbox_bus_w16(address, value >> 16);
    stbox_bus_w16(address + 2, value & 0xFFFF);
}

/* disassembler: side-effect-free */
unsigned int stbox_dasm_r8(unsigned int address)
{
    uint32_t a = address & 0xFFFFFF;
    if (a < g_ram_size) return g_ram[a];
    if (rom_hit(a)) return g_rom[a - g_rom_base];
    return 0;
}
unsigned int stbox_dasm_r16(unsigned int a)
{ return (stbox_dasm_r8(a) << 8) | stbox_dasm_r8(a + 1); }
unsigned int stbox_dasm_r32(unsigned int a)
{ return (stbox_dasm_r16(a) << 16) | stbox_dasm_r16(a + 2); }

/* ================================================================== */
/* input producers (any thread)                                       */
/* ================================================================== */
static void in_push(uint32_t v)
{
    uint32_t h = g_in_head;
    if ((uint32_t)(h - g_in_tail) >= INRING) { g_in_drop++; return; } /* full */
    g_in_ring[h & (INRING - 1)] = v;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_in_head = h + 1;
    g_in_cnt[(v >> 24) & 3]++;
}

void stbox_input_stats(uint32_t out[6])
{
    out[0] = g_in_cnt[0]; out[1] = g_in_cnt[1];
    out[2] = g_in_cnt[2]; out[3] = g_in_cnt[3];
    out[4] = g_in_drop;   out[5] = g_acia_rx_cnt;
}

void stbox_key_event(uint8_t sc, int down)
{ in_push((0u << 24) | ((uint32_t)(sc | (down ? 0 : 0x80)) << 16)); }

void stbox_mouse_rel(int dx, int dy, int buttons)
{
    if (dx > 127)  dx = 127;
    if (dx < -127) dx = -127;
    if (dy > 127)  dy = 127;
    if (dy < -127) dy = -127;
    in_push((1u << 24) | ((uint32_t)(uint8_t)dx << 16) |
            ((uint32_t)(uint8_t)dy << 8) | (uint32_t)(buttons & 3));
}

void stbox_joy_event(int joy, uint8_t state)
{ in_push((2u << 24) | ((uint32_t)(joy & 1) << 16) | ((uint32_t)state << 8)); }

void stbox_joypad_event(int joy, uint8_t state, uint8_t pad_buttons)
{ in_push((2u << 24) | ((uint32_t)(joy & 1) << 16) | ((uint32_t)state << 8) |
          pad_buttons); }

void stbox_ikbd_byte(uint8_t b)
{ in_push((3u << 24) | ((uint32_t)b << 16)); }

/* ================================================================== */
/* machine lifecycle (host side calls via stbox_host.c)               */
/* ================================================================== */
int stbox_core_setup(uint8_t *ram, uint32_t ram_size,
                     const uint8_t *rom, uint32_t rom_size)
{
    if (rom_size < 8 || rom_size > sizeof(g_rom)) return -1;
    g_ram = ram; g_ram_size = ram_size; g_ram_mask = ram_size - 1;
    if (ram_size >= 4096u<<10)      { g_bank_act[0] = 2048u<<10; g_bank_act[1] = 2048u<<10; }
    else if (ram_size >= 2048u<<10) { g_bank_act[0] = 2048u<<10; g_bank_act[1] = 0; }
    else if (ram_size >= 1024u<<10) { g_bank_act[0] = 512u<<10;  g_bank_act[1] = 512u<<10; }
    else                            { g_bank_act[0] = 512u<<10;  g_bank_act[1] = 0; }
    memcpy(g_rom, rom, rom_size);
    g_rom_size = rom_size;
    /* TOS header: base address at offset 8 */
    g_rom_base = ((uint32_t)rom[8] << 24) | ((uint32_t)rom[9] << 16) |
                 ((uint32_t)rom[10] << 8) | rom[11];
    if (g_rom_base != 0xE00000 && g_rom_base != 0xFC0000 &&
        g_rom_base != 0xE00000 + 0) {
        /* unexpected but not fatal - trust the header */
    }
    stbox_shared.ram = ram;
    stbox_shared.ram_size = ram_size;

    memset(&diska, 0, sizeof diska);   /* host frees/reinserts media */
    g_disk_new = NULL;

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_int_ack_callback(stbox_int_ack);
    g_reset_req = 1;
    return 0;
}

void stbox_core_arm(uint64_t now, uint64_t cntfrq)
{
    g_cntfrq  = cntfrq;
    g_fp_step = ((uint64_t)ST_CPU_HZ << 32) / cntfrq;
    g_slice_cap_ticks = ((uint64_t)slice_cap_ns() * cntfrq) / 1000000000ull;
    fprintf(stderr, "[STBOX] slice cap %d ns = %llu ticks (cntfrq %llu)\n",
            slice_cap_ns(), (unsigned long long)g_slice_cap_ticks,
            (unsigned long long)cntfrq);
    g_last_ticks = now;
    g_cyc_debt_fp = 0;
    g_stat_next = now + cntfrq;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_armed = 1;
}

void stbox_core_disarm(void) { g_armed = 0; }

/* Hand a disk image to the core (host thread). The buffer becomes core-
 * owned until the NEXT insert or stop; caller frees the PREVIOUS buffer
 * this call returns. Returns the old buffer, NULL on first insert. */
uint8_t *stbox_core_disk_insert(uint8_t *buf, uint32_t size)
{
    uint8_t *old = diska.data == buf ? NULL : diska.data;
    g_disk_adopted = 0;
    g_disk_new_size = size;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_disk_new = buf;
    if (g_armed) {
        for (int i = 0; i < 2000 && !g_disk_adopted; i++)
            for (volatile int j = 0; j < 20000; j++) ;
    } else {
        fdc_adopt_media();      /* engine parked: adopt synchronously */
    }
    return g_disk_adopted ? old : NULL;
}
void stbox_request_reset(void){ g_reset_req = 1; }

void stbox_rfdc_set_buffer(uint32_t guest_addr, uint32_t len)
{
    stbox_rfdc.dmabuf = guest_addr;
    stbox_rfdc.buflen = len;
}

void stbox_rfdc_enable(int on)
{
    stbox_rfdc.enabled = (on && stbox_rfdc.dmabuf) ? 1 : 0;
}
int  stbox_core_armed(void)  { return g_armed; }
/* returns 1 exactly once per halt event (render thread reports it) */
int stbox_core_take_halt_report(void)
{
    if (g_halted == 2) { g_halted = 1; return 1; }
    return 0;
}
uint32_t stbox_core_cps(void){ return g_stat_cps; }
void stbox_core_sched_stats(uint32_t out[5])
{
    out[0] = g_pub_calls; out[1] = g_pub_slices;
    out[2] = g_pub_capstop; out[3] = g_pub_debtstop; out[4] = g_pub_slice_ns;
}
uint32_t stbox_core_overruns(void){ return g_stat_overrun; }

static void machine_cold_reset(void)
{
    memset(&mfp, 0, sizeof(mfp));
    mfp.gpip = 0xBF;                  /* GPIP4/5 high (no irq), GPIP7 color…
                                         bit6 low; see mfp_read for GPIP7  */
    memset(&acia, 0, sizeof(acia));
    acia.sr = 0x02;                   /* TDRE */
    acia.joy_event = 1;               /* real IKBD: on at power-up */
    memset(&fdc, 0, sizeof(fdc));
    fdc.status = diska.present ? 0x80 : 0x00;   /* media survives reset */
    g_psg_sel = 0; memset(g_psg_reg, 0, sizeof(g_psg_reg));
    g_memcfg = 0x0A;      /* power-on default: MMU maximal; TOS resizes */
    ram_recfg();
    g_vid_base = 0; g_res = 0; g_sync = 2;        /* 50 Hz */
    memset(g_pal, 0, sizeof(g_pal));
    g_linewidth = 0; g_hscroll = 0;
    memset(&dma, 0, sizeof(dma));
    stbox_dma_playing = 0;
    stbox_dma_tail = stbox_dma_head;               /* drop queued audio */
    stbox_dma_lmc_master = 40; stbox_dma_lmc_left = stbox_dma_lmc_right = 20;
    stbox_blit_reset();
    stbox_shared.ste = (uint8_t)g_ste;
    stbox_shared.linewidth = 0;
    stbox_shared.hscroll = 0;
    stbox_shared.vis_lines = 200;
    g_halted = 0;
    g_berr_nest = 0;
    g_frame_cyc = 0; g_next_line_cyc = CYC_PER_LINE; g_line = 0;
    g_hbl_pending = g_vbl_pending = 0;
    g_total_cyc = 0; g_mfp_fp = 0;
    stbox_shared.frame = 0;
    stbox_shared.video_base = 0;
    stbox_shared.shift_res = 0;
    m68k_pulse_reset();
}

/* ================================================================== */
/* STE tier: blitter memory hooks, DMA sound sample engine            */
/* ================================================================== */
/* The blitter sees what the CPU sees: ST-RAM, TOS ROM, the cartridge
 * hole (0xFFFF) - and nothing else (hardware registers as blit source or
 * destination are undefined on the real chip too). */
uint16_t stbox_blit_mem_r16(uint32_t a)
{
    a &= 0xFFFFFE;
    if (a + 1 < 0x400000) {
        int ok; uint32_t m = ram_map(a, &ok);
        if (ok == 1) return (uint16_t)(((uint32_t)g_ram[m] << 8) | g_ram[m + 1]);
        return 0xFFFF;
    }
    if (rom_hit(a)) { uint32_t o = a - g_rom_base;
                      return (uint16_t)((g_rom[o] << 8) | g_rom[o + 1]); }
    return 0xFFFF;
}

void stbox_blit_mem_w16(uint32_t a, uint16_t v)
{
    a &= 0xFFFFFE;
    if (a + 1 < 0x400000) {
        int ok; uint32_t m = ram_map(a, &ok);
        if (ok == 1 && a >= 8) {
            if (stbox_watch_hi) watch_w(a, v, m68k_get_reg(NULL, M68K_REG_PPC));
            g_ram[m] = (uint8_t)(v >> 8);
            g_ram[m + 1] = (uint8_t)v;
        }
    }
}

/* Microwire: writing $FF8922 starts shifting the 16-bit frame out to the
 * LMC1992 one bit per microwire clock (~8 guest cycles). While it runs,
 * reading the data register returns the bits still to go (shifted left)
 * and the mask register is rotated by the same amount; when the last bit
 * has gone the data register reads 0 and the mask is back as written.
 * TOS 2.06's volume init spins on 'tst.w $FF8922 / bne' (ROM $E00240)
 * waiting for that 0 - a data register that just echoes what was
 * written hangs the boot on a white screen (field case). */
#define MW_CYC_PER_BIT 8
static unsigned mw_bits_done(void)
{
    if (!dma.mw_start) return 16;
    uint64_t d = g_total_cyc - dma.mw_start;
    unsigned n = (unsigned)(d / MW_CYC_PER_BIT);
    if (n >= 16) { dma.mw_start = 0; return 16; }
    return n;
}
static uint16_t mw_data_read(void)
{
    unsigned n = mw_bits_done();
    return n >= 16 ? 0 : (uint16_t)(dma.mw_data << n);
}
static uint16_t mw_mask_read(void)
{
    unsigned n = mw_bits_done();
    if (n >= 16) return dma.mw_mask;
    return (uint16_t)((dma.mw_mask << n) | (dma.mw_mask >> (16 - n)));
}

/* Push the output samples owed for `cycles` guest cycles. One input byte
 * (pair) per 8 >> rate output samples; the ring carries S16 stereo at the
 * master rate. Frame end: Timer A event + GPIP7, then repeat or stop. */
static void dma_advance(uint32_t cycles)
{
    if (!(dma.ctrl & 1)) return;
    dma.fp += (uint64_t)cycles * DMA_FP_STEP;
    unsigned n = (unsigned)(dma.fp >> 32);
    if (!n) return;
    dma.fp &= 0xFFFFFFFFu;

    const unsigned factor = 8u >> (dma.mode & 3);
    const int stereo = !(dma.mode & 0x80);
    while (n--) {
        if (dma.sub == 0) {
            if (dma.cur >= dma.cur_end) {
                /* frame end */
                mfp_timera_event();
                mfp_raise(MFP_CH_GPIP7);
                if (dma.ctrl & 2) {                    /* repeat: next frame */
                    dma.cur = dma.start; dma.cur_end = dma.end;
                } else {
                    dma.ctrl &= (uint8_t)~1;           /* one-shot: stop  */
                    stbox_dma_playing = 0;
                    return;
                }
                if (dma.cur >= dma.cur_end) {          /* empty frame     */
                    dma.ctrl &= (uint8_t)~1;
                    stbox_dma_playing = 0;
                    return;
                }
            }
            int ok; uint32_t m = ram_map(dma.cur & 0x3FFFFF, &ok);
            if (ok == 1) {
                dma.l = (int16_t)((int8_t)g_ram[m] << 8);
                if (stereo) {
                    int ok2; uint32_t m2 = ram_map((dma.cur + 1) & 0x3FFFFF, &ok2);
                    dma.r = (ok2 == 1) ? (int16_t)((int8_t)g_ram[m2] << 8) : dma.l;
                } else
                    dma.r = dma.l;
            } else
                dma.l = dma.r = 0;
            dma.cur += stereo ? 2 : 1;
            dma.sub = factor;
        }
        dma.sub--;

        unsigned h = stbox_dma_head;
        if (h - stbox_dma_tail < STBOX_DMA_RING) {
            stbox_dma_ring[(h & (STBOX_DMA_RING - 1)) * 2]     = dma.l;
            stbox_dma_ring[(h & (STBOX_DMA_RING - 1)) * 2 + 1] = dma.r;
            __atomic_thread_fence(__ATOMIC_RELEASE);
            stbox_dma_head = h + 1;
        }                                              /* full: drop     */
    }
}

/* ================================================================== */
/* the core-3 micro-slice                                             */
/* ================================================================== */
void stbox_slice(uint64_t now)
{
    if (!g_armed) return;

    g_slice_now = now;
    if (g_reset_req) { g_reset_req = 0; machine_cold_reset(); }
    fdc_adopt_media();               /* media swap works even halted  */
    if (g_halted) return;            /* dead machine: frozen frame -
                                        SB_RESET revives it           */
    g_berr_nest = 0;                 /* a longjmp escape from Musashi
                                        skips the guard's own clear   */

    /* accrue debt at 8 MHz pace */
    uint64_t dt = now - g_last_ticks;
    g_last_ticks = now;
    g_cyc_debt_fp += (int64_t)(dt * g_fp_step);
    if ((g_cyc_debt_fp >> 32) > MAX_DEBT_CYC)
        g_cyc_debt_fp = (int64_t)MAX_DEBT_CYC << 32;

    g_stat_calls++;
    if ((g_cyc_debt_fp >> 32) < SLICE_CYC) return;

    /* Burst: run slices until the pace is caught up or the host-time cap
     * (SLICE_HOST_CAP_NS) is reached. One slice per pass starves the box
     * to ~54% under load; the cap bounds how long the main machine's
     * ipl_task pass is lengthened. g_slice_cap_ticks==0 => one slice. */
    for (;;) {
        fdc_adopt_media();
        input_drain();
        acia_pump();
        if (fdc.pending == 1 && g_total_cyc >= fdc.event_cyc)
            fdc_complete();
        if (fdc.pending == 2) {                    /* real op in flight    */
            if (stbox_rfdc.done) {
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                fdc.pending = 0;
                fdc.status = stbox_rfdc.status | 0x80;   /* motor on      */
                uint8_t cmd = stbox_rfdc.cmd;
                if (cmd < 0x80) {                  /* type I: track sync   */
                    if ((cmd & 0xF0) == 0x00) fdc.track = 0;
                    else if ((cmd & 0xF0) == 0x10) fdc.track = fdc.data;
                } else if (!(cmd >= 0xA0 && cmd < 0xC0) && !(fdc.status & 0x10)) {
                    uint32_t bytes = (uint32_t)stbox_rfdc.xferred * 512u;
                    if ((cmd & 0xF0) == 0xC0) bytes = 6;
                    for (uint32_t i = 0; i < bytes; i++) {
                        uint32_t a = (fdc.dma_addr + i) & 0xFFFFFF;
                        if (a >= 8 && a < g_ram_size)
                            g_ram[a] = stbox_rfdc.staging[i];
                    }
                    fdc.dma_addr += bytes;
                    if (fdc.sector_count >= stbox_rfdc.xferred)
                        fdc.sector_count -= stbox_rfdc.xferred;
                    else
                        fdc.sector_count = 0;
                }
                fdc_intrq();
            } else if (g_total_cyc >= g_rfdc_next_kick && stbox_cpu_kick) {
                g_rfdc_next_kick = g_total_cyc + RFDC_KICK_CYC;
                stbox_rfdc.kick = 1;
                stbox_cpu_kick();
            }
        }

        /* STE blitter: 16 bus accesses per slice = 64 guest cycles at the
         * chip's 4 cycles/access. HOG mode holds the CPU off the bus for the
         * duration; shared mode interleaves it with a CPU slice. Either way
         * the host work per pass stays one bounded unit. */
        int ran = 0;
        if (g_ste && !ste_noblit() && stbox_blit_busy()) {
            int acc = stbox_blit_step(SLICE_CYC / 4);
            ran = acc * 4;
            if (stbox_blit_hog() || acc == 0) {
                if (ran <= 0) ran = SLICE_CYC;
                goto blitted;                     /* CPU halted this slice */
            }
        }
        {
            int cpu = m68k_execute(SLICE_CYC);
            if (cpu <= 0) cpu = SLICE_CYC;
            if (cpu > SLICE_CYC * 2) g_stat_overrun++;
            ran += cpu;
        }
    blitted:
        g_cyc_debt_fp -= (int64_t)ran << 32;
        g_total_cyc += (uint32_t)ran;
        g_stat_cyc += (uint32_t)ran;

        if (dbg_on()) {
            uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
            stbox_dbg_pc = pc;
            if (pc < stbox_dbg_pc_lo) stbox_dbg_pc_lo = pc;
            if (pc > stbox_dbg_pc_hi) stbox_dbg_pc_hi = pc;
        }

        mfp_advance((uint32_t)ran);
        if (g_ste && !ste_nodma()) dma_advance((uint32_t)ran);

        /* scanline / frame bookkeeping */
        g_frame_cyc += (uint32_t)ran;
        while (g_frame_cyc >= g_next_line_cyc) {
            g_next_line_cyc += CYC_PER_LINE;
            g_line++;
            mfp_timerb_event();
            if (g_line >= LINES_PER_VBL) {
                g_line = 0;
                g_frame_cyc -= CYC_PER_VBL;
                g_next_line_cyc = CYC_PER_LINE;
                g_vbl_pending = 1;
                /* Publish the height this frame actually reached, then
                 * disarm for the next one. Sticky-free: a game that stops
                 * opening the border is back to 200 the very next frame,
                 * which is correct - the border is closed again. */
                stbox_shared.vis_lines = g_bottom_open ? 232 : 200;
                g_bottom_open = 0;
                stbox_shared.frame++;
                update_irq();
            } else {
                g_hbl_pending = 1;
                update_irq();
            }
        }

        g_stat_slices++;
        if (g_halted) break;
        if ((g_cyc_debt_fp >> 32) < SLICE_CYC) { g_stat_debtstop++; break; }
        if (!g_slice_cap_ticks) break;              /* cap 0: one slice/pass */
#if defined(__aarch64__)
        uint64_t tnow;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(tnow));
        g_stat_slice_ticks += tnow - now;           /* cumulative, see below */
        if (tnow - now >= g_slice_cap_ticks) { g_stat_capstop++; break; }
        g_slice_now = tnow;                         /* PSG timestamps       */
#else
        break;                                      /* no arch timer: 1/pass */
#endif
    }

    /* once-a-second stats rollover (arch timer, no syscalls) */
    if (now >= g_stat_next) {
        g_stat_cps = g_stat_cyc;
        g_stat_cyc = 0;
        g_pub_calls = g_stat_calls; g_pub_slices = g_stat_slices;
        g_pub_capstop = g_stat_capstop; g_pub_debtstop = g_stat_debtstop;
        /* g_stat_slice_ticks sums (t - entry) at each loop check, so it is
         * an upper bound on burst time; divide by checks for a mean */
        uint32_t chk = g_stat_capstop + (g_stat_slices - g_stat_calls > 0 ? g_stat_slices - g_stat_calls : 0);
        g_pub_slice_ns = chk ? (uint32_t)((g_stat_slice_ticks * 1000000000ull / g_cntfrq) / chk) : 0;
        g_stat_calls = g_stat_slices = g_stat_capstop = g_stat_debtstop = 0;
        g_stat_slice_ticks = 0;
        g_stat_next = now + g_cntfrq;
    }
}
