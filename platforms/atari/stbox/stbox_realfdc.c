/* SPDX-License-Identifier: MIT
 *
 * stbox_realfdc.c - the sandbox's drive A on the REAL floppy hardware:
 * WD1772 + DMA chip + whatever hangs off the cable (a Gotek, here).
 *
 * WHERE THIS RUNS. stbox_errand_pump() is called from m68k_run_jit's
 * post-block spcflags window on the CPU thread - the bus owner - and
 * nowhere else. Core 3 posts an errand and keeps kicking the CPU thread
 * (SPCFLAG_BRK, the sampler's own wake pattern) until the pump reports
 * done. Each pump pass is bounded: a handful of register accesses, or
 * one sector's drain (~256 word reads, ~0.3 ms) - the main guest stalls
 * no worse than it would for its own ACSI traffic.
 *
 * MUTUAL EXCLUSION, borrowed from the OS itself:
 *   - flock ($43E) is set for the whole operation, so the main guest's
 *     TOS/MiNT keeps its hands off the FDC, the DMA chip and the drive
 *     select bits (flopvbl checks it; so does every sane ACSI driver).
 *   - PSG port A (drive/side select) is written with the select latch
 *     saved and restored via ym2149_selected_reg() - both sides run on
 *     this thread, so a guest select/data pair can never interleave.
 *   - the DMA lands in ST-RAM the guest DONATED: STBOX.PRG Mxalloc's a
 *     buffer and hands its physical address over SB_DMABUF, so MiNT
 *     guarantees nobody else touches it.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "stbox.h"
#include "../../../gpio/ps_protocol.h"

/* ym2149.c tracks the guest's PSG select latch (see stbox.h comment) */
extern uint8_t ym2149_selected_reg(void);
/* guest-coherent ST-RAM access (pistorm_natmem.cpp): the guest reads
 * ST-RAM Pi-side, so flock must be written through the banked path or
 * the guest never sees our claim */
extern uint16_t pistorm_guest_get_word(uint32_t a);
extern void     pistorm_guest_put_word(uint32_t a, uint16_t v);
/* ps_protocol's sticky bus-error latch: cleared after our bus work so a
 * fault in a pump access is never delivered to the GUEST as a phantom
 * vector-2 (natmem raises on the next guest access otherwise) */
extern volatile uint8_t g_buserr;

#define REG_DMA_DATA   0xFF8604u    /* FDC reg / sector count (word)  */
#define REG_DMA_MODE   0xFF8606u    /* w: mode, r: DMA status (word)  */
#define REG_DMA_HI     0xFF8609u
#define REG_DMA_MID    0xFF860Bu
#define REG_DMA_LO     0xFF860Du
#define REG_PSG_SEL    0xFF8800u
#define REG_PSG_DATA   0xFF8802u
#define ADDR_FLOCK     0x43Eu

/* DMA mode bits (matching atari_fdd.c's decode of the same register) */
#define DMAM_FDC_STATUS 0x080u      /* FDC command/status reg         */
#define DMAM_FDC_TRACK  0x082u
#define DMAM_FDC_SECTOR 0x084u
#define DMAM_FDC_DATA   0x086u
#define DMAM_COUNT      0x090u      /* sector count reg               */
#define DMAM_WRITE      0x100u      /* direction: to disk             */

enum { ST_IDLE, ST_SETUP, ST_POLL, ST_DRAIN, ST_FINISH };

static int      g_state = ST_IDLE;
volatile int    stbox_errand_active;    /* see stbox.h: inline gate       */
static uint8_t  g_saved_psg14;
static uint32_t g_polls;
static uint32_t g_drain_pos;        /* bytes drained so far           */
static int      g_writing;
static uint32_t g_flock_waits;
static uint32_t g_failures;         /* consecutive: 3 disables mode   */

#define MAX_POLLS 4000              /* * ~1ms kick cadence = ~4 s     */
#define MAX_FLOCK_WAITS 4000        /* main guest hogging the DMA     */

static void psg_porta_select(int side)
{
    /* read-modify-write reg 14: drive A select is bit 1 (active LOW),
     * drive B bit 2 (keep deselected = high), side is bit 0 (low = 1) */
    ps_write_8(REG_PSG_SEL, 14);
    uint8_t v = (uint8_t)ps_read_8(REG_PSG_SEL);
    g_saved_psg14 = v;
    v &= (uint8_t)~0x02;                       /* select drive A     */
    v |= 0x04;                                  /* deselect drive B   */
    if (side) v &= (uint8_t)~0x01; else v |= 0x01;
    ps_write_8(REG_PSG_DATA, v);
    ps_write_8(REG_PSG_SEL, ym2149_selected_reg());
}

static void psg_porta_restore(void)
{
    ps_write_8(REG_PSG_SEL, 14);
    ps_write_8(REG_PSG_DATA, g_saved_psg14);
    ps_write_8(REG_PSG_SEL, ym2149_selected_reg());
}

static void dma_set_addr(uint32_t a)
{
    ps_write_8(REG_DMA_HI,  (a >> 16) & 0xFF);
    ps_write_8(REG_DMA_MID, (a >> 8) & 0xFF);
    ps_write_8(REG_DMA_LO,  a & 0xFF);
}

void stbox_errand_pump(void)
{
    stbox_rfdc_t *r = &stbox_rfdc;

    if (g_state == ST_IDLE) {
        if (!r->req)
            return;
        r->req = 0;
        r->done = 0;
        g_state = ST_SETUP;
    }

    switch (g_state) {
    case ST_SETUP: {
        /* flock is TEST-and-set: the main guest's ACSI/IDE drivers own
         * the same DMA chip, and claiming it mid-transfer corrupts their
         * I/O (field report: main desktop lost its drives). The guest
         * only runs when this pump is not - same thread - so read-then-
         * write is race-free. If the guest holds the lock, retry on the
         * next kick. */
        if (pistorm_guest_get_word(ADDR_FLOCK) != 0) {
            if (++g_flock_waits > MAX_FLOCK_WAITS) {
                g_flock_waits = 0;
                r->status = 0x10;              /* RNF: give up        */
                r->xferred = 0;
                g_state = ST_IDLE;
                stbox_errand_active = 0;
                r->done = 1;
                if (++g_failures >= 3) {
                    r->enabled = 0;
                    fprintf(stderr, "[STBOX] real-FDC: main guest never "
                            "released flock - real mode disabled\n");
                }
            }
            return;
        }
        g_flock_waits = 0;
        pistorm_guest_put_word(ADDR_FLOCK, 1); /* claim it            */
        psg_porta_select(r->side);
        g_writing = (r->cmd >= 0xA0 && r->cmd < 0xC0);

        if (r->cmd < 0x80) {                   /* type I: seek family */
            /* target track through the FDC data reg, then the command */
            ps_write_16(REG_DMA_MODE, DMAM_FDC_DATA);
            ps_write_16(REG_DMA_DATA, r->arg_track);
            ps_write_16(REG_DMA_MODE, DMAM_FDC_STATUS);
            ps_write_16(REG_DMA_DATA, r->cmd);
            g_state = ST_POLL;
            g_polls = 0;
            break;
        }

        /* type II/III: full DMA setup */
        uint32_t bytes = (uint32_t)r->count * 512u;
        if ((r->cmd & 0xF0) == 0xC0) bytes = 6; /* read address       */
        if (!r->dmabuf || bytes > r->buflen || bytes > sizeof r->staging) {
            r->status = 0x10;                  /* RNF                 */
            g_state = ST_FINISH;
            /* fall through to FINISH on the next pass */
            break;
        }
        if (g_writing) {                       /* preload guest buffer */
            for (uint32_t i = 0; i < bytes; i += 2)
                ps_write_16(r->dmabuf + i,
                            (uint16_t)((r->staging[i] << 8) | r->staging[i+1]));
        }
        /* direction toggle resets the DMA fifo/status */
        uint16_t dir = g_writing ? DMAM_WRITE : 0;
        ps_write_16(REG_DMA_MODE, (uint16_t)(DMAM_FDC_STATUS | DMAM_WRITE));
        ps_write_16(REG_DMA_MODE, (uint16_t)(DMAM_FDC_STATUS));
        dma_set_addr(r->dmabuf);
        ps_write_16(REG_DMA_MODE, (uint16_t)(DMAM_COUNT | dir));
        ps_write_16(REG_DMA_DATA, r->count ? r->count : 1);
        /* sector register, then the command */
        ps_write_16(REG_DMA_MODE, (uint16_t)(DMAM_FDC_SECTOR | dir));
        ps_write_16(REG_DMA_DATA, r->sector);
        ps_write_16(REG_DMA_MODE, (uint16_t)(DMAM_FDC_STATUS | dir));
        ps_write_16(REG_DMA_DATA, r->cmd);
        g_state = ST_POLL;
        g_polls = 0;
        break;
    }

    case ST_POLL: {
        if (!r->kick && g_polls)               /* wait for the cadence */
            return;
        r->kick = 0;
        ps_write_16(REG_DMA_MODE, DMAM_FDC_STATUS);
        uint8_t st = (uint8_t)ps_read_16(REG_DMA_DATA);
        if (st & 0x01) {                       /* still busy          */
            if (++g_polls > MAX_POLLS) {
                /* force interrupt, give up */
                ps_write_16(REG_DMA_DATA, 0xD0);
                r->status = 0x10;
                if (++g_failures >= 3) {
                    r->enabled = 0;
                    fprintf(stderr, "[STBOX] real-FDC: 3 timeouts - "
                            "real mode disabled (Gotek attached? media "
                            "selected?)\n");
                }
                g_state = ST_FINISH;
            }
            return;
        }
        r->status = st;
        if (r->cmd < 0x80 || g_writing) {      /* nothing to drain    */
            r->xferred = r->count;
            g_state = ST_FINISH;
        } else {
            g_drain_pos = 0;
            g_state = ST_DRAIN;
        }
        break;
    }

    case ST_DRAIN: {
        uint32_t bytes = (uint32_t)r->count * 512u;
        if ((r->cmd & 0xF0) == 0xC0) bytes = 6;
        /* one sector per pass keeps the pass bounded */
        uint32_t end = g_drain_pos + 512;
        if (end > bytes) end = bytes;
        for (uint32_t i = g_drain_pos; i < end; i += 2) {
            uint16_t w = (uint16_t)ps_read_16(r->dmabuf + i);
            r->staging[i]     = (uint8_t)(w >> 8);
            r->staging[i + 1] = (uint8_t)w;
        }
        g_drain_pos = end;
        if (g_drain_pos >= bytes) {
            r->xferred = r->count;
            g_state = ST_FINISH;
        }
        break;
    }

    case ST_FINISH:
        psg_porta_restore();
        pistorm_guest_put_word(ADDR_FLOCK, 0);
        if (!(r->status & 0x10))
            g_failures = 0;                    /* success resets fuse  */
        g_state = ST_IDLE;
        stbox_errand_active = 0;
        g_buserr = 0;                          /* never hand the guest
                                                  a phantom fault      */
        __atomic_thread_fence(__ATOMIC_RELEASE);
        r->done = 1;
        break;
    }
    g_buserr = 0;
}

/* Host-side abort: called with real mode being torn down. If an op is
 * mid-flight the pump finishes or times it out on its own; this just
 * refuses new ones and, if the pump is parked, clears stale state. */
void stbox_rfdc_abort(void)
{
    stbox_rfdc.enabled = 0;
}
