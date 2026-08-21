// SPDX-License-Identifier: MIT
/*
 * ps_protocol_v2.c - Pi-side driver for the PSP2 firmware.  See
 * newfw/DESIGN.md for the contract this implements.  Same external API as
 * the old driver: the emulator and ataritest build against it unchanged.
 *
 * TO USE: back up gpio/ps_protocol.c and copy this file over it (same
 * name keeps the Makefile untouched).  Requires PSP2 firmware (CSR FWREV
 * 0x21, prints as "EPM240 1.1r" through the existing revision decoder).
 *
 * The three rules this driver lives by:
 *   1. A transaction begins only after BUSY (GPIO0) reads low.
 *   2. Strobes are shaped by COUNTED GPLEV READBACKS (each ~50-100ns of
 *      real wall time on the far side of the wire), never by NOPs whose
 *      duration depends on the compiler and the core clock.
 *   3. Read data is sampled ONCE, after BUSY falls - the firmware
 *      guarantees PI_D has then been driven stable for >=375ns.  No
 *      settle loops: validity is the firmware's contract, not our hope.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "ps_protocol.h"   /* keeps existing type/extern declarations */

/* ---- hardware (board facts) ---------------------------------------- */
#define P2_PIN_BUSY   (1u << 0)     /* PI_TXN_IN_PROGRESS               */
#define P2_PIN_WR     (1u << 1)     /* PI_CMD_WR                        */
#define P2_PIN_RD     (1u << 4)     /* PI_CMD_RD                        */
#define P2_PIN_BERR   (1u << 7)     /* PI_BERR                          */
#define P2_CMD_SHIFT  2             /* PI_CMD on GPIO2-3                */
#define P2_D_SHIFT    8             /* PI_D on GPIO8-23                 */

#define P2_SEL_DATA    0u
#define P2_SEL_ADDRLO  1u
#define P2_SEL_ADDRHI  2u
#define P2_SEL_CSR     3u

#define P2_HI_RD      (1u << 12)
#define P2_HI_BYTE    (1u << 11)
#define P2_FC_SHIFT   13

/* CSR write bits (firmware) */
#define P2_CSR_ENGRST (1u << 0)
#define P2_CSR_RESET  (1u << 1)
#define P2_CSR_HALT   (1u << 2)
#define P2_CSR_CLRSTK (1u << 3)

volatile uint32_t *gpio;
volatile uint32_t *gpclk;
volatile uint32_t *ioset;    /* GPSET0 */
volatile uint32_t *ioclr;    /* GPCLR0 */
volatile uint32_t *ioread;   /* GPLEV0 */

uint8_t fc = 6;
volatile uint32_t g_buserr = 0;
volatile uint32_t g_buserr_addr = 0;
volatile uint8_t  ps_bus_active = 0;

/* Diagnostic symbols dma_snoop.c links against.  In the v1 driver these
 * fed the bus census that found the read-path fault; under PSP2 the
 * contract makes that instrument redundant, but the symbols must exist.
 * The hook stays installable (dma_snoop sets it) and the census stays
 * zero - census log lines simply never print. */
int (*ps_xfer_active_hook)(void) = 0;
volatile uint32_t ps_xfer_census[8];

/* Strobe shaping - WIDTH IS THE WHOLE GAME.
 *
 * Bench-measured (p2diag, go-retry counter): at 6 readbacks the CPLD's
 * 2-clock/250ns sampler missed ~0.3% of strobes - 143 misses in 50k ops
 * even in the error-free experiment.  GO misses were healed by the
 * verified retry; ADDR-phase misses have no verification, so the write
 * silently went to the PREVIOUS op's latched address.  Every D/E/F
 * failure decoded as exactly that.
 *
 * Six reads can be as little as 240ns on a quiet AMBA bus - UNDER the
 * sampler floor.  The default is now 12 reads (~600-1200ns): even the
 * fastest plausible read pace stays >2x the sampler requirement.
 * PISTORM_P2_SHAPE tunes it; the go-retry counter is the meter - a
 * correctly sized strobe drives it to ZERO. */
static int p2_shape = -1;

static inline void shape(void)
{
    volatile uint32_t d;
    if (__builtin_expect(p2_shape < 0, 0)) {
        const char *e = getenv("PISTORM_P2_SHAPE");
        p2_shape = e ? atoi(e) : 8;   /* width only; spacing no longer matters.
                                         * PSP2 (fw 0x2x): needs >=6 (8MHz sync).
                                         * PSP3 (fw 0x30+): async flags catch ANY
                                         * width - set PISTORM_P2_SHAPE=1. */
        if (p2_shape < 1)  p2_shape = 1;
        if (p2_shape > 64) p2_shape = 64;
    }
    for (int i = 0; i < p2_shape; i++) d = *ioread;
    (void)d;
}

/* Direction control, corrected after first bench contact:
 *
 * The CONTROL lines (WR, CMD, RD) are Pi outputs FOREVER - the CPLD never
 * drives them, so they are configured once at setup and never touched.
 * Only the 16 DATA lanes change direction.  The first version flipped
 * everything together, which drove PI_D against the '374s during every
 * read acknowledge - a 16-line bus fight on each read.  Never again:
 * data_in() before any window where the CPLD may drive. */
#define P2_FSEL0_CTL  (GPFSEL0_OUTPUT & 0x00FFFFFFu)   /* gpio8,9 -> input */

static inline void data_out(void)  { gpio[0] = GPFSEL0_OUTPUT;
                                     gpio[1] = GPFSEL1_OUTPUT;
                                     gpio[2] = GPFSEL2_OUTPUT; }
static inline void data_in(void)   { gpio[0] = P2_FSEL0_CTL;
                                     gpio[1] = GPFSEL1_INPUT;
                                     gpio[2] = GPFSEL2_INPUT; }

static inline void wr_phase(uint32_t sel, uint32_t data)
{
    *ioclr = (0x3u << P2_CMD_SHIFT) | (0xFFFFu << P2_D_SHIFT);
    *ioset = (sel << P2_CMD_SHIFT) | ((data & 0xFFFFu) << P2_D_SHIFT);
    *ioset = P2_PIN_WR;  shape();
    *ioclr = P2_PIN_WR;  shape();
}

static inline void rd_pulse(uint32_t sel)
{
    *ioclr = (0x3u << P2_CMD_SHIFT);
    *ioset = (sel << P2_CMD_SHIFT);
    *ioset = P2_PIN_RD;  shape();
    *ioclr = P2_PIN_RD;  shape();
}

/* COMPLETION IS A TOGGLE (firmware 0x23+).  The full story, because it
 * cost three bench runs to learn:
 *
 * Level-BUSY was unwinnable.  At 6-read strobes the Pi missed ~0.3% of
 * BUSY rises (the rise fit between polls); the verified-retry healed GO
 * but ADDR-phase misses silently wrote to the previous address.  At
 * 12-read strobes the ENTIRE bus cycle completed inside the Pi's own
 * strobe tail and 100% of rises were missed - the retry storm then
 * double-executed everything into chaos.  Any protocol that requires
 * the observer to catch a transient level is racing the observer's own
 * overhead, and both directions of that race have now been lost on the
 * bench.
 *
 * PI_TXN_IN_PROGRESS now TOGGLES once per completed transaction, flipped
 * by the firmware only after the read-data settle (so the data-valid
 * contract rides on the same edge).  The driver snapshots the line
 * before GO and waits for it to DIFFER.  Correct at any latency, any
 * preemption, any strobe width.  No retries exist any more; a timeout
 * here means the transaction genuinely never ran and is reported, not
 * papered over. */
uint32_t p2_go_misses;   /* now counts TIMEOUTS - should be ZERO forever */

static inline uint32_t toggle_snap(void)
{
    return *ioread & P2_PIN_BUSY;
}

/* returns the GPLEV word sampled at the completion flip (data-valid) */
static inline uint32_t wait_toggle(uint32_t snap)
{
    uint32_t lev;
    for (uint32_t i = 0; i < 4000000u; i++) {
        lev = *ioread;
        if ((lev & P2_PIN_BUSY) != snap)
            return lev;
    }
    p2_go_misses++;                       /* genuine loss - visible, loud */
    return *ioread;
}

/* ---- core transactions --------------------------------------------- */

/* POSTED WRITES (the old system's big performance win, now RACE-FREE).
 * A write returns right after its GO strobe; its completion toggle is
 * collected at the START of the next transaction, before any new phase
 * strobe - so phases still never overlap a running cycle (the '373 gate
 * contract), but the Pi gets the whole bus-cycle time (~2us) back to
 * compute in.  BERR for a posted write surfaces one transaction late -
 * identical to the old stack's posted-write semantics. */
static uint32_t pend_snap;
static int      pend_wr;
static uint32_t pend_addr;

static inline void flush_write(void)
{
    if (pend_wr) {
        uint32_t lev = wait_toggle(pend_snap);
        if (lev & P2_PIN_BERR) { g_buserr = 1; g_buserr_addr = pend_addr; }
        pend_wr = 0;
    }
}

static uint32_t p2_read(uint32_t addr, int byte_sz, uint8_t fcode,
                        uint8_t *berr_out)
{
    uint32_t lev, snap;

    flush_write();                        /* collect any posted write    */
    data_out();
    wr_phase(P2_SEL_ADDRLO, addr & 0xFFFFu);
    snap = toggle_snap();                 /* BEFORE the GO strobe        */
    wr_phase(P2_SEL_ADDRHI, ((uint32_t)fcode << P2_FC_SHIFT) | P2_HI_RD |
                            (byte_sz ? P2_HI_BYTE : 0) |
                            ((addr >> 16) & 0xFFu));      /* GO          */
    data_in();                            /* wr_phase's trailing shape() 
                                             already held the lanes well
                                             past the latch gate         */
    lev = wait_toggle(snap);
    /* CONTRACT: the flip happens after the RD-OE settle - this very
     * sample is the data, and BERR for this cycle rides in it.          */
    if (berr_out) *berr_out = (lev & P2_PIN_BERR) ? 1 : 0;

    rd_pulse(P2_SEL_DATA);                /* ack: releases '374 OE        */
    return (lev >> P2_D_SHIFT) & 0xFFFFu;
}

static void p2_write(uint32_t addr, uint32_t data, int byte_sz, uint8_t fcode)
{
    uint32_t snap;

    flush_write();                        /* collect any posted write    */
    data_out();
    wr_phase(P2_SEL_ADDRLO, addr & 0xFFFFu);
    wr_phase(P2_SEL_ADDRHI, ((uint32_t)fcode << P2_FC_SHIFT) |
                            (byte_sz ? P2_HI_BYTE : 0) |
                            ((addr >> 16) & 0xFFu));
    snap = toggle_snap();
    wr_phase(P2_SEL_DATA, data & 0xFFFFu);            /* GO              */
    data_in();
    pend_snap = snap; pend_addr = addr; pend_wr = 1;  /* posted          */
    /* (an $FF860x un-post+2us pacing experiment lived here: it WORSENED
     * the window loss, as did a mode-write double - both timing knobs
     * move the fault, neither direction helps => wrong variable) */
}

/* ---- public API (unchanged shape) ----------------------------------- */

/* BYTE READS: pass the FULL address - A0 travels in ADDR_LO and the
 * firmware derives UDS/LDS from it.  The first version masked A0 off
 * (habit from word-register thinking), so every byte read asserted UDS
 * and odd bytes returned their even neighbour's data.  Bench signature:
 * "bytes terrible, words fine" - lane selection, first bench run. */
uint8_t ps_read_8(uint32_t addr)
{
    uint8_t berr;
    uint32_t w = p2_read(addr, 1, fc, &berr);
    if (berr) { g_buserr = 1; g_buserr_addr = addr; }
    return (addr & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
}

uint8_t ps_read_8_fc(uint32_t addr, uint8_t fcv, uint8_t *berr_out)
{
    uint8_t berr;
    uint32_t w = p2_read(addr, 1, fcv, &berr);
    if (berr_out) *berr_out = berr;
    if (berr) { g_buserr = 1; g_buserr_addr = addr; }
    return (addr & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
}

uint16_t ps_read_16(uint32_t addr)
{
    uint8_t berr;
    uint32_t w;
    if (addr & 1)                        /* misaligned: split, hi first */
        return (uint16_t)(((uint16_t)ps_read_8(addr) << 8) |
                           ps_read_8(addr + 1));
    w = p2_read(addr, 0, fc, &berr);
    if (berr) { g_buserr = 1; g_buserr_addr = addr; }
    return (uint16_t)w;
}

uint16_t ps_read_16_fc(uint32_t addr, uint8_t fcv, uint8_t *berr_out)
{
    uint8_t berr;
    uint32_t w = p2_read(addr, 0, fcv, &berr);
    if (berr_out) *berr_out = berr;
    if (berr) { g_buserr = 1; g_buserr_addr = addr; }
    return (uint16_t)w;
}

uint32_t ps_read_32(uint32_t addr)
{
    uint32_t hi = ps_read_16(addr);
    uint32_t lo = ps_read_16(addr + 2);
    return (hi << 16) | lo;
}

void ps_write_8(uint32_t addr, uint16_t data)
{
    /* 68K byte write: same byte on both halves, strobes select the lane */
    uint32_t w = ((uint32_t)(data & 0xFF) << 8) | (data & 0xFF);
    p2_write(addr, w, 1, fc);
}

void ps_write_16(uint32_t addr, uint16_t data)
{
    /* Misaligned words: the 68000 cannot do them; the old system's
     * observable behaviour (per ataritest's ODD expectations) is a split
     * into two byte accesses, high byte first.  Match it. */
    if (addr & 1) {
        ps_write_8(addr,     (uint16_t)(data >> 8));
        ps_write_8(addr + 1, (uint16_t)(data & 0xFF));
        return;
    }
    p2_write(addr, data, 0, fc);
}

void ps_write_32(uint32_t addr, uint32_t data)
{
    ps_write_16(addr,     (uint16_t)(data >> 16));
    ps_write_16(addr + 2, (uint16_t)data);
}

/* Barrier: returns only after any posted write has fully completed on
 * the 68k bus.  ACSI sequencing needs real landing points, not posting. */
void ps_flush_posted(void)
{
    flush_write();
}

/* ---- block read: the mirror-sync hot path --------------------------- */

/* Reads `words` sequential words with per-word overhead trimmed to the
 * contract minimum.  Purpose: the floppy mirror pull must fit inside a
 * ~1.4ms inter-sector gap or every sector costs a 200ms revolution.
 * Strobe shaping here is tighter than the general path: the CPLD's 2-FF
 * sync at 8MHz needs >=250ns high/low; 5 GPLEV readbacks (~300-500ns)
 * clears that with margin, and the completion TOGGLE makes any miss a
 * visible timeout (p2_go_misses), never silent corruption.
 * PISTORM_P2_BLOCK_SHAPE tunes it; 0 timeouts = safe, as always. */
void ps_read_block(uint32_t addr, uint8_t *dst, uint32_t words)
{
    static int bshape = -1;
    uint32_t k, snap, lev;
    if (bshape < 0) {
        const char *e = getenv("PISTORM_P2_BLOCK_SHAPE");
        bshape = e ? atoi(e) : 5;     /* PSP3: 1 is safe (async capture) */
        if (bshape < 1)  bshape = 1;
        if (bshape > 64) bshape = 64;
    }
    flush_write();
    for (k = 0; k < words; k++, addr += 2)
    {
        volatile uint32_t d; int i;
        data_out();
        /* ADDR_LO */
        *ioclr = (0x3u << P2_CMD_SHIFT) | (0xFFFFu << P2_D_SHIFT);
        *ioset = (P2_SEL_ADDRLO << P2_CMD_SHIFT) |
                 ((addr & 0xFFFFu) << P2_D_SHIFT);
        *ioset = P2_PIN_WR; for (i = 0; i < bshape; i++) d = *ioread;
        *ioclr = P2_PIN_WR; for (i = 0; i < bshape; i++) d = *ioread;
        snap = *ioread & P2_PIN_BUSY;
        /* ADDR_HI + RD = GO */
        *ioclr = (0x3u << P2_CMD_SHIFT) | (0xFFFFu << P2_D_SHIFT);
        *ioset = (P2_SEL_ADDRHI << P2_CMD_SHIFT) |
                 ((((uint32_t)fc << P2_FC_SHIFT) | P2_HI_RD |
                   ((addr >> 16) & 0xFFu)) << P2_D_SHIFT);
        *ioset = P2_PIN_WR; for (i = 0; i < bshape; i++) d = *ioread;
        *ioclr = P2_PIN_WR; for (i = 0; i < bshape; i++) d = *ioread;
        data_in();
        lev = wait_toggle(snap);
        dst[2*k]     = (uint8_t)(lev >> (P2_D_SHIFT + 8));
        dst[2*k + 1] = (uint8_t)(lev >>  P2_D_SHIFT);
        /* ack: releases the '374 OE; ctl lines are permanent outputs */
        *ioclr = (0x3u << P2_CMD_SHIFT);
        *ioset = P2_PIN_RD; for (i = 0; i < bshape; i++) d = *ioread;
        *ioclr = P2_PIN_RD; for (i = 0; i < bshape; i++) d = *ioread;
        (void)d;
    }
    data_out(); data_in();               /* leave lanes in idle state    */
}

/* ---- status / control ------------------------------------------------ */

uint32_t ps_read_status_reg(void)
{
    uint32_t csr, snap, lev;
    flush_write();
    data_in();
    snap = toggle_snap();
    rd_pulse(P2_SEL_CSR);                 /* GO                          */
    lev = wait_toggle(snap);
    csr = (lev >> P2_D_SHIFT) & 0xFFFFu;
    rd_pulse(P2_SEL_DATA);                /* ack releases the CSR drive  */

    /* Old callers treat this as raw GPLEV with PI_D at bits 8-23; give
     * them the CSR word in that position so every existing decoder -
     * including the firmware-revision print - keeps working. */
    return csr << 8;
}

/* FW 0x27 debug page: {st_fight, cnt_wr[6:0], cnt_rd[7:0]} - executed
 * bus-cycle counters.  p2diag reconciles them against issued counts to
 * split "write ran as a read cycle" from "write ran and the board lost
 * it".  Page select is CSR bit4; restored to 0 before returning. */
uint32_t p2_dbg_counters(void)
{
    uint32_t v, snap, lev;
    flush_write();
    data_out();
    wr_phase(P2_SEL_CSR, 1u << 4);        /* debug page on               */
    data_in();
    snap = toggle_snap();
    rd_pulse(P2_SEL_CSR);                 /* CSR read GO                 */
    lev = wait_toggle(snap);
    v = (lev >> P2_D_SHIFT) & 0xFFFFu;
    rd_pulse(P2_SEL_DATA);                /* ack releases the CSR drive  */
    data_out();
    wr_phase(P2_SEL_CSR, 0);              /* debug page off              */
    data_in();
    return v;
}

/* FW 0x2A: debug page 2 = {br_edges[7:0], bgack_edges[7:0]} free-running */
uint32_t p2_dbg2_counters(void)
{
    uint32_t v, snap, lev;
    flush_write();
    data_out();
    wr_phase(P2_SEL_CSR, 1u << 5);        /* page 2 on                   */
    data_in();
    snap = toggle_snap();
    rd_pulse(P2_SEL_CSR);
    lev = wait_toggle(snap);
    v = (lev >> P2_D_SHIFT) & 0xFFFFu;
    rd_pulse(P2_SEL_DATA);
    data_out();
    wr_phase(P2_SEL_CSR, 0);
    data_in();
    return v;
}

void ps_write_status_reg(uint16_t value)
{
    /* Compatibility shim: the ONE historical caller pattern is the
     * 0x0004/0x0000 sticky-clear pulse (dma_snoop).  In PSP2 the sticky
     * clear is CSR bit3; old bit2 meant the same thing.  Translate, and
     * pass the reset/halt bits through in their PSP2 positions. */
    uint32_t v = 0;
    if (value & 0x0004u) v |= P2_CSR_CLRSTK;
    if (value & STATUS_BIT_RESET) v |= P2_CSR_RESET;
    if (value & STATUS_BIT_HALT)  v |= P2_CSR_HALT;

    flush_write();                        /* order after posted writes   */
    data_out();
    wr_phase(P2_SEL_CSR, v);              /* CSR writes are edge-actioned,
                                             no completion toggle needed */
    data_in();
}

void ps_reset_state_machine(void)
{
    data_out();
    wr_phase(P2_SEL_CSR, P2_CSR_ENGRST);
    wr_phase(P2_SEL_CSR, 0);
    data_in();
}

void ps_pulse_reset(void)
{
    data_out();
    wr_phase(P2_SEL_CSR, P2_CSR_RESET);
    data_in();
    usleep(100000);                       /* 100ms: >16 E-clocks at 8MHz */
    data_out();
    wr_phase(P2_SEL_CSR, 0);
    data_in();
    usleep(1500);
}

void ps_pulse_halt(void)
{
    data_out();
    wr_phase(P2_SEL_CSR, P2_CSR_HALT);
    data_in();
    usleep(100000);
    data_out();
    wr_phase(P2_SEL_CSR, 0);
    data_in();
}

void ps_read_ipl(uint8_t *ipl)
{
    *ipl = (uint8_t)((*ioread & 0x60u) >> 4);   /* GPIO5/6, as before */
}

void ps_get_firmware_revision(void)
{
    /* CSR low byte = FWREV: 0x2A -> 2.10r (PSP2), 0x30 -> 3.0r (PSP3) */
    uint8_t fw = (uint8_t)((ps_read_status_reg() >> 8) & 0xFF);
    printf("[INIT] PiSTorm firmware EPM240 %d.%dr (%s)\n",
           fw >> 4, fw & 0x0F,
           (fw >= 0x30) ? "PSP3" : "PSP2");
    if (p2_go_misses)
        printf("[PSP2] %u GO strobes needed a retry - strobe margin is "
               "thin, raise P2_SHAPE\n", p2_go_misses);
}

void ps_write_latchtype(uint16_t latchtype) { (void)latchtype; }

/* ---- init ------------------------------------------------------------ */

static int create_dev_mem_mapping(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return -1; }
    void *m = mmap(NULL, BCM2708_PERI_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, BCM2708_PERI_BASE);
    if (m == MAP_FAILED) { perror("mmap"); return -1; }
    gpio  = (volatile uint32_t *)((uintptr_t)m + GPIO_ADDR);
    gpclk = (volatile uint32_t *)((uintptr_t)m + GPCLK_ADDR);
    ioset  = gpio + (0x1C / 4);
    ioclr  = gpio + (0x28 / 4);
    ioread = gpio + (0x34 / 4);
    return 0;
}

void ps_setup_protocol(void)
{
    if (create_dev_mem_mapping() < 0) exit(1);
    data_in();
    *ioclr = P2_PIN_WR | P2_PIN_RD | (0x3u << P2_CMD_SHIFT);
    /* engine to a known state, clear power-on sticky noise */
    ps_reset_state_machine();
    ps_write_status_reg(0x0004);
    ps_write_status_reg(0x0000);
}
