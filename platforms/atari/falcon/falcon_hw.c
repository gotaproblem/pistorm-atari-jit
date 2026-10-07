// SPDX-License-Identifier: MIT
/*
 * Falcon DSP host port, SSI, bootstrap, switching matrix and sound DMA,
 * and the engine thread that clocks them. See falcon.h / FALCON-DSP.md.
 *
 * Register layouts are the Falcon's (Falcon specification 1992 and the
 * released machine's sound registers); the DSP56001 host interface and SSI
 * follow the DSP56001 user's manual.
 *
 * Deliberate differences from the hardware, all documented:
 *  - host port FIFOs are 1024 words deep each way instead of the 56001's
 *    double buffer: code that blasts words without polling TXDE (DSPMOD
 *    does) cannot lose any to our thread scheduling. TXDE = room left,
 *    RXDF / HRDF = data waiting, TRDY = host FIFO fully drained.
 *  - the matrix runs at the DAC's rate; SSI slots are placed 1/8 frame
 *    apart (continuous-clock mode: 8 x 16-bit words per sample period).
 *  - sound DMA reads and writes host RAM (natmem) only.
 */
#define _GNU_SOURCE
#include "falcon.h"
#include "dsp56k.h"
#include "../mfp_hub.h"
#include "../psvidel/psvidel.h"

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#define DSP_HZ_REAL 32000000.0
/* The DSP runs faster than the Falcon's by this factor (PISTORM_DSP_TURBO,
 * 1-8, default 2). Programs pace themselves on the SSI and the host port,
 * not on cycle counts, so a faster 56001 only finishes its work sooner.
 * DSPMOD needs that here: BOR's VBL waits for the DSP's answer, and when
 * a late VBL is followed by an early one the DSP is still mixing the
 * last one - the wait makes the next VBL late too, the sample counts
 * grow, and a count far beyond a VBL's worth ends in a 65536-pass loop.
 * Costs nothing extra while the DSP idles (idle polls are skipped). */
static double g_dsp_hz = DSP_HZ_REAL * 2.0;
static uint32_t g_turbo_cut;           /* engine: batches run at 1x (behind) */
#define DSP_HZ g_dsp_hz

/* ------------------------------------------------------------------ */
/* Host port FIFOs (single producer, single consumer)                  */
/* ------------------------------------------------------------------ */
/* Deep, because the 68k does not handshake each word: DSPMOD waits for
 * TXDE once and then writes a voice's whole block (up to ~1000 words)
 * back to back, relying on a real 56001 emptying HTX between two 68030
 * bus cycles. Here the DSP drains in sample-clock steps, so a VBL's
 * worth of every voice (8 x 1000+ words at high pitches) must fit. TXDE
 * is only reported while a whole block still fits (HF_TXDE_ROOM), so a
 * 68k that does check waits instead of overflowing. */
#define HF_SIZE      32768u
#define HF_TXDE_ROOM  8192u
/* DSP->68k transmit depth. The 56001's host TX is one word (double
 * buffered): after the DSP writes HTX it must wait for HTDE - the 68k
 * has read the word - before the next. Software that hands the DSP a
 * request and reads a block back a word at a time (ACE Tracker) relies
 * on that pacing. The port here is a deep FIFO for the 68k->DSP burst a
 * MOD player makes, but on the DSP->68k side an unbounded HTDE let ACE's
 * DSP run whole blocks ahead of the 68k and the two desynced. HTDE now
 * clears once this many words are unread, so the DSP paces to the 68k.
 * Beats of Rage sends one word a VBL this way and is unaffected. */
static unsigned g_htx_depth = 4;
/* TXDE as the 68k sees it. Default: set while a whole block still fits in
 * the deep FIFO (HF_TXDE_ROOM), for DSPMOD's blind bursts.
 * PISTORM_DSP_TXDE=chip: the 56001's own rule - set while fewer than two
 * words are in flight (the host's TX register and the DSP's HRX), so a 68k
 * that checks TXDE before each word waits for the DSP to take them, as on
 * a Falcon. Words written without checking still queue (the FIFO stays
 * deep). Sonolumineszenz needs it: its music interrupt's host command
 * makes the DSP save every 68k word still in flight in a 10-word buffer
 * (X:$38AC) - with TXDE always set the main program had 24 queued, the
 * save ran over into the next buffer and the music state, the main
 * program's data came back with "DSP" in it, and the 68k ran into its own
 * command buffer (illegal instruction at $34A54). */
static int g_txde_chip;
static inline int tx_empty(uint32_t txn)
{
    return g_txde_chip ? txn < 2u : txn <= HF_SIZE - HF_TXDE_ROOM;
}
typedef struct {
    uint32_t w[HF_SIZE];
    _Atomic uint32_t head;          /* producer */
    _Atomic uint32_t tail;          /* consumer */
} hfifo_t;

static uint32_t g_hrx_ring[128];          /* last host words the DSP read */
static uint32_t g_hrx_n;
static inline uint32_t hf_count(hfifo_t *f)
{
    return atomic_load_explicit(&f->head, memory_order_acquire) -
           atomic_load_explicit(&f->tail, memory_order_acquire);
}
static inline int hf_push(hfifo_t *f, uint32_t v)
{
    uint32_t h = atomic_load_explicit(&f->head, memory_order_relaxed);
    if (h - atomic_load_explicit(&f->tail, memory_order_acquire) >= HF_SIZE)
        return 0;
    f->w[h % HF_SIZE] = v & 0xFFFFFF;
    atomic_store_explicit(&f->head, h + 1, memory_order_release);
    return 1;
}
static inline int hf_peek(hfifo_t *f, uint32_t *v)
{
    uint32_t t = atomic_load_explicit(&f->tail, memory_order_relaxed);
    if (t == atomic_load_explicit(&f->head, memory_order_acquire))
        return 0;
    *v = f->w[t % HF_SIZE];
    return 1;
}
static inline void hf_pop(hfifo_t *f)
{
    uint32_t t = atomic_load_explicit(&f->tail, memory_order_relaxed);
    if (t != atomic_load_explicit(&f->head, memory_order_acquire))
        atomic_store_explicit(&f->tail, t + 1, memory_order_release);
}

/* DSP side of the 68k->DSP FIFO: during a VBL-locked burst the words the
 * 68k writes after the burst began stay out of sight until it ends (see
 * the VBL lock in the engine). g_tx_vis_on / g_tx_vis_head: engine only. */
static int      g_tx_vis_on;
static uint32_t g_tx_vis_head;

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
enum { DSP_HELD = 0, DSP_BOOT, DSP_RUN };

/* A program from the DSP XBIOS: `boot` = the raw 512-word bootstrap
 * image (Dsp_ExecBoot), else the Dsp_LodToBinary stream (Dsp_ExecProg):
 * blocks of { space 0=P 1=X 2=Y, address, count, count words }. */
struct dsp_exec {
    int      boot;
    uint32_t n;
    uint32_t w[];
};

static struct {
    int          configured;
    _Atomic int  armed;
    uint8_t     *guest;
    uint32_t     guest_size;

    dsp56k_t    *dsp;
    _Atomic int  dsp_state;
    _Atomic uint32_t reset_epoch;       /* bumped on each reset release */
    uint32_t     reset_seen;            /* engine side                  */
    _Atomic uint32_t boot_head;         /* tx head at the release       */
    _Atomic int  reset_held;
    unsigned     boot_count;
    /* Dsp_ExecProg / Dsp_ExecBoot: a program handed over at a reset
     * release, loaded straight into DSP memory by the engine */
    _Atomic(struct dsp_exec *) exec;
    uint8_t      psg_latch, porta;

    /* host port: 68k side */
    hfifo_t      tx;                    /* 68k -> DSP */
    hfifo_t      rx;                    /* DSP -> 68k */
    _Atomic uint8_t icr, cvr;
    uint8_t      ivr;
    uint8_t      txb[3];
    uint32_t     rx_last;
    /* host port: DSP side */
    _Atomic uint8_t hcr;
    uint32_t     hrx_last;

    /* DSP peripherals */
    uint32_t     ipr, bcr, pbc, pcc, pbddr, pcddr, pbd, pcd;
    uint32_t     sci[8];
    uint32_t     cra, crb, ssisr, ssi_tx, ssi_rx;
    int          ssisr_read;

    /* sound registers $FF8900-$FF8943 (byte image) */
    uint8_t      reg[0x44];
    uint32_t     play_start, play_end, play_cnt;
    /* the end address of the frame now playing/recording: the registers
     * are latched when a frame starts (Falcon/STE sound DMA, as Hatari
     * models it); a new $FF890F-13 written during a frame is for the
     * next frame. ACE Tracker writes its next buffer's start/end at the
     * top of every Timer A handler; applied at once, the frame playing
     * ended early or late, firing Timer A again inside the handler -
     * ACE's "CPU Overload" - and playing the wrong buffer. */
    uint32_t     play_fstart, play_fend, rec_fend;
    /* frame-end pacing (dma_play_frame): wall time of the last play
     * frame end, and a frame end held until it is due */
    uint64_t     play_ev_ns, play_hold_until;
    int          play_hold;
    _Atomic uint32_t play_holds;
    uint32_t     rec_start, rec_end, rec_cnt;
    _Atomic int  play_on, rec_on;
    int          play_rep, rec_rep;

    /* locks */
    int          dsp_locked, snd_locked;

    /* engine */
    pthread_t    thread;
    _Atomic int  stop;
    double       frame_hz;
    double       cyc_per_frame, cyc_acc;
    uint64_t     frames;
    /* wall-clock sample clock (engine): see clock_due() */
    uint64_t     last_ns, emitted;
    double       clk, fill_avg, adj_i;
    unsigned     resyncs, hiccups;
    int          vbl_locked;           /* lockstep with the 68k's answers */
    int          dsp_idle;             /* last free run ended polling */
    /* lockstep: a program that answers the 68k about once a VBL (DSPMOD)
     * gets one VBL of samples per answer */
    uint32_t     answers;               /* answers to the 68k (engine)   */
    uint32_t     answers_at_win;
    uint64_t     talk_win_ns;
    uint64_t     answer_ns;             /* wall clock of the last answer  */
    double       answer_gap;            /* their average spacing, seconds */
    int          answer_armed;          /* read a host word since the last */
    uint32_t     answers_paid;          /* answers already turned into samples */
    unsigned     vbl_skipped;
    /* stall watchdog: the 68k's ISR polls vs host port movement */
    _Atomic uint32_t isr_polls;
    _Atomic uint32_t ta_events;         /* Timer A frame events raised  */
    uint64_t     dropped_tx;
} F;

static inline uint16_t rw16(int o) { return (uint16_t)((F.reg[o] << 8) | F.reg[o + 1]); }

/* engine: words the DSP may see */
static inline uint32_t tx_visible(void)
{
    uint32_t t = atomic_load_explicit(&F.tx.tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&F.tx.head, memory_order_acquire);
    if (g_tx_vis_on && (int32_t)(h - g_tx_vis_head) > 0)
        h = g_tx_vis_head;
    return (int32_t)(h - t) > 0 ? h - t : 0;
}

int falcon_configured(void) { return F.configured; }
int falcon_armed(void) { return atomic_load(&F.armed); }
/* The sound DMA is playing ($FF8901 bit 0 reads 1). On the STE and the
 * Falcon that state is XORed onto MFP GPIP7 by the hardware; the GPIP
 * shim needs it to present the same line. */
int falcon_dma_playing(void)
{
    return atomic_load(&F.armed) && atomic_load(&F.play_on);
}

/* The host port's HREQ line, as the CPU sees it. On a Falcon it is a
 * level-6 interrupt the DSP vectors itself: at IACK the host port puts
 * its IVR ($FFA203) on the bus (TOS and programs set it to $FF, vector
 * $3FC). HREQ = (ICR & ISR) bits 0-1: RREQ with RXDF (a word from the
 * DSP waiting), TREQ with TXDE (room to send). Read by ipl_task (core 3)
 * and the CPU thread's IACK: atomics only, nothing else touched, and
 * the common case (no request enabled) is two loads. */
static _Atomic int g_dsp_resetting;     /* defined with the bootstrap */
int falcon_hreq(void)
{
    if (!atomic_load_explicit(&F.armed, memory_order_relaxed))
        return 0;
    uint8_t icr = atomic_load_explicit(&F.icr, memory_order_relaxed);
    if (!(icr & 3))
        return 0;
    if (atomic_load_explicit(&g_dsp_resetting, memory_order_relaxed))
        return 0;
    if ((icr & 1) && hf_count(&F.rx))
        return 1;
    if ((icr & 2) && tx_empty(hf_count(&F.tx)))
        return 1;
    return 0;
}

uint8_t falcon_ivr(void) { return F.ivr; }

/* ------------------------------------------------------------------ */
/* Host-port trace and sync-point counters (PISTORM_DSP_TRACE)         */
/* ------------------------------------------------------------------ */
/* Every host-port event either side makes, in one time-ordered ring, with
 * the DSP pc and the sample frame. Off unless PISTORM_DSP_TRACE=<entries>
 * is set: each hook is then one load and a branch, and nothing else here
 * changes what the port does. Written to a file (dtr_dump) on a stall
 * (the 68k polling with nothing moving either way), on SIGUSR2 and at
 * exit. tools/dsptrace/ compares a dump with Hatari's.
 *   68k side: W word written   R word read   r read with nothing there
 *             (the old word again)   D word dropped (FIFO full)
 *             I ICR write   C CVR write   V IVR write   P ISR read (on change)
 *             w an RX-read wait ended   s a same-thread DSP run ended
 *   DSP side: G HRX read   g HRX read with nothing there   S HTX write
 *             H host command taken   F HCR write (on change)
 *             X reset released   B bootstrap done */
typedef struct {
    uint64_t ns;
    uint32_t frame;
    uint32_t v;
    uint16_t dpc;
    char     t;
    uint8_t  x;
} dtr_t;
static dtr_t   *g_tr;
static uint32_t g_tr_size;               /* entries, a power of two; 0 = off */
static int      g_tr_first;              /* keep the first g_tr_size events */
static _Atomic uint32_t g_tr_n;
static _Atomic int g_tr_pause;           /* recording held: a dump is being written */
static double g_tr_tick_ns = 1.0;       /* aarch64: ns per counter tick */
static inline uint64_t dtr_now(void)    /* ns; a counter read on the Pi */
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return (uint64_t)((double)v * g_tr_tick_ns);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
static void dtr_put(char t, uint32_t v, uint32_t x)
{
    if (atomic_load_explicit(&g_tr_pause, memory_order_relaxed))
        return;
    uint32_t i = atomic_fetch_add_explicit(&g_tr_n, 1, memory_order_relaxed);
    if (g_tr_first && i >= g_tr_size)
        return;                          /* full: the start is what is wanted */
    dtr_t *e = &g_tr[i & (g_tr_size - 1)];
    e->ns = dtr_now();
    e->frame = (uint32_t)F.frames;
    e->v = v;
    e->dpc = F.dsp ? F.dsp->pc : 0;
    e->x = x > 255 ? 255 : (uint8_t)x;
    e->t = t;
}
#define dtr(t, v, x) do { if (__builtin_expect(g_tr_size != 0, 0)) dtr_put((t), (v), (x)); } while (0)

/* per-sync-point counters for the 5 s [DSPPORT] line (trace on only) */
static struct {
    _Atomic uint32_t rxw_n, rxw_sum_us, rxw_max_us, rxw_got, rxw_parked, rxw_timeout, rxw_short;
    _Atomic uint32_t rx_stale;
    _Atomic uint32_t st_n, st_sum_us, st_max_us, st_parked, st_cut;
    _Atomic uint32_t hc_req, hc_taken, hc_sum_us, hc_max_us, hc_over, hc_wd;
    _Atomic uint32_t tx_w, tx_max, tx_drop, rx_s, rx_max;
} g_ps;
static _Atomic uint64_t g_hc_req_ns;
static void ps_max(_Atomic uint32_t *p, uint32_t v)
{
    uint32_t o = atomic_load_explicit(p, memory_order_relaxed);
    while (v > o && !atomic_compare_exchange_weak(p, &o, v))
        ;
}

/* ------------------------------------------------------------------ */
/* DSP side peripherals (engine thread)                                */
/* ------------------------------------------------------------------ */
#define HCR_HRIE 0x01
#define HCR_HTIE 0x02
#define HCR_HCIE 0x04

#define SSI_TDE 0x40
#define SSI_RDF 0x80
#define SSI_TUE 0x10
#define SSI_ROE 0x20
#define SSI_TFS 0x04
#define SSI_RFS 0x08

static int falcon_dbg(void);
static void host_irqs(void)
{
    dsp56k_t *d = F.dsp;
    int lvl = (int)((F.ipr >> 10) & 3);
    uint8_t hcr = atomic_load(&F.hcr);
    if (lvl && (hcr & HCR_HRIE) && tx_visible())
        dsp56k_irq_raise(d, DSP_VEC_HOST_RX, lvl);
    else
        dsp56k_irq_clear(d, DSP_VEC_HOST_RX);
    if (lvl && (hcr & HCR_HTIE) && hf_count(&F.rx) < g_htx_depth)
        dsp56k_irq_raise(d, DSP_VEC_HOST_TX, lvl);
    else
        dsp56k_irq_clear(d, DSP_VEC_HOST_TX);
    uint8_t cvr = atomic_load(&F.cvr);
    if ((cvr & 0x80) && lvl && (hcr & HCR_HCIE)) {
        dsp56k_irq_raise(d, (cvr & 0x1F) * 2, lvl);
        atomic_fetch_and(&F.cvr, (uint8_t)0x7F);      /* taken */
        if (g_tr_size) {
            dtr('H', (cvr & 0x1F) * 2u, cvr);
            atomic_fetch_add(&g_ps.hc_taken, 1);
            uint64_t t = atomic_exchange(&g_hc_req_ns, 0);
            if (t) {
                uint32_t us = (uint32_t)((dtr_now() - t) / 1000u);
                atomic_fetch_add(&g_ps.hc_sum_us, us);
                ps_max(&g_ps.hc_max_us, us);
            }
        }
        if (falcon_dbg()) {
            static unsigned said;
            if (said++ < 60)
                fprintf(stderr, "[FALCON] HC $%02X raised to the DSP: frame %llu, DSP pc $%04X r3 $%04X sr $%04X\n",
                        (cvr & 0x1F) * 2, (unsigned long long)F.frames, d->pc, d->r[3], d->sr);
        }
    }
}

static void ssi_irqs(void)
{
    dsp56k_t *d = F.dsp;
    int lvl = (int)((F.ipr >> 12) & 3);
    dsp56k_irq_clear(d, DSP_VEC_SSI_RX);
    dsp56k_irq_clear(d, DSP_VEC_SSI_RXE);
    dsp56k_irq_clear(d, DSP_VEC_SSI_TX);
    dsp56k_irq_clear(d, DSP_VEC_SSI_TXE);
    if (!lvl)
        return;
    if ((F.crb & 0x8000) && (F.ssisr & SSI_RDF))
        dsp56k_irq_raise(d, (F.ssisr & SSI_ROE) ? DSP_VEC_SSI_RXE : DSP_VEC_SSI_RX, lvl);
    if ((F.crb & 0x4000) && (F.ssisr & SSI_TDE))
        dsp56k_irq_raise(d, (F.ssisr & SSI_TUE) ? DSP_VEC_SSI_TXE : DSP_VEC_SSI_TX, lvl);
}

/* the last words the DSP sent the 68k, for the stall report */
#define RXLOG 8
static struct { uint32_t v; uint64_t frames, ns; } g_rxlog[RXLOG];
static unsigned g_rxlog_n;
/* every DSP->68k word, for the reload dump (engine writes, CPU reads) */
#define RXALL 512u
static uint32_t g_rxall[RXALL];
static _Atomic uint32_t g_rxall_n;
/* Time-ordered host-port trace: 'S' a word the DSP sent (push, engine),
 * 'R' a word the 68k read (pop, CPU), 'W' a word the 68k wrote to the DSP
 * (CPU). One ring, so the dump shows exactly where the 68k's reads
 * diverge from what the DSP sent. */
#define HLOG 256u
static struct { char dir; uint32_t v; uint16_t pc; } g_hlog[HLOG];
static _Atomic uint32_t g_hlog_n;
/* Debug ring and stats counters, bumped once per host word. A plain
 * relaxed load + store, not an atomic add: on the Pi 4 the add was a call
 * into a load/store-exclusive loop (~5% of the 68k thread in DSPBench's
 * transfer tests). Each has one writer at a time; the worst a race can
 * do is lose a debug-ring slot. */
static inline uint32_t bump(_Atomic uint32_t *p)
{
    uint32_t v = atomic_load_explicit(p, memory_order_relaxed);
    atomic_store_explicit(p, v + 1, memory_order_relaxed);
    return v;
}

static void hlog(char dir, uint32_t v, uint16_t pc)
{
    uint32_t i = bump(&g_hlog_n);
    g_hlog[i % HLOG].dir = dir;
    g_hlog[i % HLOG].v = v & 0xFFFFFF;
    g_hlog[i % HLOG].pc = pc;
}
static uint64_t rxlog_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* host -> DSP -> host latency, split in two (status line):
 *  pickup: the 68k writes a request into an empty port .. the DSP reads it
 *          (long when the DSP is still busy with the last VBL's work)
 *  answer: that read .. the DSP's reply */
static _Atomic uint64_t g_tx_idle_ns;          /* CPU sets, engine clears */
static uint64_t         g_pick_ns, g_pick_lat; /* engine only */
static _Atomic uint32_t g_pick_max_us, g_pick_sum_us, g_pick_n;
static _Atomic uint32_t g_ans_max_us, g_ans_sum_us, g_ans_n;

static void lat_add(_Atomic uint32_t *mx, _Atomic uint32_t *sum, _Atomic uint32_t *n,
                    uint64_t ns)
{
    uint32_t us = (uint32_t)(ns / 1000u);
    uint32_t o = atomic_load_explicit(mx, memory_order_relaxed);
    while (us > o && !atomic_compare_exchange_weak(mx, &o, us))
        ;
    atomic_fetch_add_explicit(sum, us, memory_order_relaxed);
    atomic_fetch_add_explicit(n, 1, memory_order_relaxed);
}

static void rxlog_add(uint32_t v)
{
    g_rxall[bump(&g_rxall_n) % RXALL] = v & 0xFFFFFF;
    unsigned i = g_rxlog_n++ % RXLOG;
    g_rxlog[i].v = v & 0xFFFFFF;
    g_rxlog[i].frames = F.frames;
    /* a clock read per DSP word, only for the stall dump's +ms column */
    g_rxlog[i].ns = falcon_dbg() ? rxlog_now() : 0;
}


static uint32_t periph_read(void *ctx, int space, uint16_t a)
{
    (void)ctx;
    if (space == DSP_SPACE_Y)
        return 0;
    switch (a) {
    case 0xFFE0: return F.pbc;
    case 0xFFE1: return F.pcc;
    case 0xFFE2: return F.pbddr;
    case 0xFFE3: return F.pcddr;
    case 0xFFE4: return F.pbd;
    case 0xFFE5: return F.pcd;
    case 0xFFE8: return atomic_load(&F.hcr);
    case 0xFFE9: {
        uint32_t v = 0;
        if (tx_visible()) v |= 0x01;                             /* HRDF */
        if (hf_count(&F.rx) < g_htx_depth) v |= 0x02;            /* HTDE */
        if (atomic_load(&F.cvr) & 0x80) v |= 0x04;               /* HCP  */
        v |= (uint32_t)((atomic_load(&F.icr) >> 3) & 3) << 3;   /* HF0/HF1 */
        return v;
    }
    case 0xFFEB: {                                               /* HRX */
        uint32_t v;
        if (tx_visible() && hf_peek(&F.tx, &v)) {
            dtr('G', v, hf_count(&F.tx));
            hf_pop(&F.tx);
            g_hrx_ring[g_hrx_n++ & 127u] = v;
            F.hrx_last = v;
            F.answer_armed = 1;
            uint64_t t = atomic_load_explicit(&g_tx_idle_ns, memory_order_relaxed)
                         ? atomic_exchange(&g_tx_idle_ns, 0) : 0;
            if (t) {
                uint64_t now = rxlog_now();
                g_pick_lat = now - t;
                g_pick_ns = now;
            }
        } else {
            dtr('g', F.hrx_last, 0);
        }
        host_irqs();
        return F.hrx_last;
    }
    case 0xFFEC: return F.cra;
    case 0xFFED: return F.crb;
    case 0xFFEE: F.ssisr_read = 1; return F.ssisr;
    case 0xFFEF: {                                               /* RX */
        F.ssisr &= ~SSI_RDF;
        if (F.ssisr_read) F.ssisr &= ~SSI_ROE;
        F.ssisr_read = 0;
        ssi_irqs();
        return F.ssi_rx;
    }
    case 0xFFF1: return 0x03 | F.sci[1];                         /* SCI: TDRE TRNE */
    case 0xFFFE: return F.bcr;
    case 0xFFFF: return F.ipr;
    }
    if (a >= 0xFFF0 && a <= 0xFFF7) return F.sci[a & 7];
    return 0;
}

static void periph_write(void *ctx, int space, uint16_t a, uint32_t v)
{
    (void)ctx;
    if (space == DSP_SPACE_Y)
        return;
    switch (a) {
    case 0xFFE0: F.pbc = v; return;
    case 0xFFE1: F.pcc = v; return;
    case 0xFFE2: F.pbddr = v; return;
    case 0xFFE3: F.pcddr = v; return;
    case 0xFFE4: F.pbd = v; return;
    case 0xFFE5: F.pcd = v; return;
    case 0xFFE8:
        if (g_tr_size && atomic_load(&F.hcr) != (uint8_t)(v & 0x1F))
            dtr('F', v & 0x1F, atomic_load(&F.hcr));
        atomic_store(&F.hcr, (uint8_t)(v & 0x1F));
        host_irqs();
        return;
    case 0xFFEB:                                                 /* HTX */
        if (F.answer_armed) {                /* the reply to what it read */
            F.answer_armed = 0;
            F.answers++;
            uint64_t now = rxlog_now();
            if (F.answer_ns) {
                double g = (double)(now - F.answer_ns) * 1e-9;
                if (g > 0.040) g = 0.040;
                if (g < 0.010) g = 0.010;
                if (F.answer_gap == 0.0)
                    F.answer_gap = g;
                else
                    F.answer_gap += (g - F.answer_gap) / 16.0;
            }
            F.answer_ns = now;
            /* lockstep: nothing the 68k sends from here on is seen until
             * the VBL of samples this answer pays for has played (it plays
             * at once, as a burst), so even a question asked straight after
             * is answered with a VBL's samples, never "0" - which DSPMOD
             * would pass on as a 65536-pass loop */
            if (F.vbl_locked && !g_tx_vis_on) {
                g_tx_vis_head = atomic_load(&F.tx.head);
                g_tx_vis_on = 1;
            }
        }
        rxlog_add(v);
        if (g_pick_ns) {
            /* the word the DSP answers is the last one it picked up from an
             * idle port - the request; data blocks follow the answer */
            lat_add(&g_pick_max_us, &g_pick_sum_us, &g_pick_n, g_pick_lat);
            lat_add(&g_ans_max_us, &g_ans_sum_us, &g_ans_n, rxlog_now() - g_pick_ns);
            g_pick_ns = 0;
        }
        hlog('S', v, F.dsp->pc);
        if (g_tr_size) {
            uint32_t q = hf_count(&F.rx);
            dtr('S', v, q);
            atomic_fetch_add(&g_ps.rx_s, 1);
            ps_max(&g_ps.rx_max, q + 1);
        }
        hf_push(&F.rx, v);
        host_irqs();
        return;
    case 0xFFEC: F.cra = v; return;
    case 0xFFED:
        if ((v & 0x1000) && !(F.crb & 0x1000))
            F.ssisr |= SSI_TDE;                                  /* TE on: empty */
        F.crb = v;
        ssi_irqs();
        return;
    case 0xFFEE: return;                                         /* TSR */
    case 0xFFEF:                                                 /* TX */
        F.ssi_tx = v;
        F.ssisr &= ~SSI_TDE;
        if (F.ssisr_read) F.ssisr &= ~SSI_TUE;
        F.ssisr_read = 0;
        ssi_irqs();
        return;
    case 0xFFFE: F.bcr = v; return;
    case 0xFFFF: F.ipr = v; host_irqs(); ssi_irqs(); return;
    }
    if (a >= 0xFFF0 && a <= 0xFFF7) F.sci[a & 7] = v;
}

/* one SSI time slot: the receiver takes `in`, the transmitter shifts out
 * what the DSP last wrote. Returns the transmitted 24-bit word. */
static uint32_t g_ssi_n;
static uint32_t ssi_slot(int slot, uint32_t in, int *valid)
{
    uint32_t out = 0;
    g_ssi_n++;
    *valid = 0;
    if (F.crb & 0x2000) {                                        /* RE */
        if (F.ssisr & SSI_RDF) F.ssisr |= SSI_ROE;
        F.ssi_rx = in & 0xFFFFFF;
        F.ssisr |= SSI_RDF;
        if (slot == 0) F.ssisr |= SSI_RFS; else F.ssisr &= ~SSI_RFS;
    }
    if (F.crb & 0x1000) {                                        /* TE */
        if (F.ssisr & SSI_TDE) F.ssisr |= SSI_TUE;               /* underrun */
        out = F.ssi_tx;
        *valid = 1;
        F.ssisr |= SSI_TDE;
        if (slot == 0) F.ssisr |= SSI_TFS; else F.ssisr &= ~SSI_TFS;
    }
    ssi_irqs();
    return out;
}

static int ssi_slots(void)
{
    if (!(F.crb & 0x0800))
        return 1;                                                /* normal mode */
    int n = (int)((F.cra >> 8) & 0x1F) + 1;
    return n > 8 ? 8 : n;
}

/* ------------------------------------------------------------------ */
/* DSP run control (engine thread)                                     */
/* Set while the DSP is being reset/reloaded: the 68k's host port reads an
 * empty receive port (no RXDF, no data) so it never sees a stale word the
 * old program left behind and waits for the new program's first word
 * (BDF). Cleared by the engine once the new program is loaded and about to
 * run. Without this the flat-out old DSP pushed words past the reset and
 * ACE's init read a stale word instead of BDF. */
static _Atomic int g_dsp_resetting;
static uint32_t g_resetting_cleared;      /* engine: stale flags cleared */

/* PISTORM_FALCON_DEBUG=1: the per-reload dump, the host-port trace, the
 * sound-DMA on/off line and the per-reload Dsp_ExecProg line. Off by
 * default so a program that reloads its DSP (ACE Tracker under load) does
 * not flood the console; the 10 s stats line and one-time init messages
 * stay on. */
static int falcon_dbg(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("PISTORM_FALCON_DEBUG"); v = (e && *e == '1'); }
    return v;
}

/* ------------------------------------------------------------------ */
/* The state the 56001 bootstrap leaves a program in (dsp_boot_drain) */
static void dsp_start_at_0(void)
{
    F.dsp->pc = 0;
    F.dsp->omr = 0x0002;
    F.dsp->sr = 0x0300;
    atomic_store(&F.dsp_state, DSP_RUN);
}

/* Engine, just after a reset: what TOS 4's loader would put in DSP
 * memory from the host port, written directly, then run from P:0. */
static void dsp_exec_load(const struct dsp_exec *x)
{
    F.dsp->omr = 0x0002;            /* P:$000-$1FF internal while loading */
    if (x->boot) {
        uint32_t n = x->n < 512 ? x->n : 512;
        for (uint32_t i = 0; i < n; i++)
            F.dsp->pint[i] = x->w[i] & 0xFFFFFFu;
        F.boot_count = n;
        dsp_start_at_0();
        if (falcon_dbg())
            fprintf(stderr, "[FALCON] Dsp_ExecBoot: %u words\n", n);
        return;
    }
    static const int space[3] = { DSP_SPACE_P, DSP_SPACE_X, DSP_SPACE_Y };
    uint32_t i = 0, blocks = 0, words = 0;
    while (i + 3 <= x->n) {
        uint32_t t = x->w[i], a = x->w[i + 1], c = x->w[i + 2];
        i += 3;
        if (t > 2 || c > x->n - i) {
            fprintf(stderr, "[FALCON] Dsp_ExecProg: bad block %u (type %u addr $%04X count %u)\n",
                    blocks, t, a, c);
            break;
        }
        for (uint32_t k = 0; k < c; k++)
            dsp56k_mem_write(F.dsp, space[t], (uint16_t)(a + k), x->w[i + k]);
        i += c;
        words += c;
        blocks++;
    }
    F.boot_count = 512;
    dsp_start_at_0();
    if (falcon_dbg())
        fprintf(stderr, "[FALCON] Dsp_ExecProg: %u words in %u blocks, run from P:0\n",
                words, blocks);
}

static void dsp_power_state(void)
{
    uint32_t ep = atomic_load(&F.reset_epoch);
    if (ep != F.reset_seen) {
        F.reset_seen = ep;
        dtr('X', ep, 0);
        /* reset released: the bootstrap loader starts listening */
        dsp56k_reset(F.dsp);
        memset(F.dsp->pint, 0, sizeof F.dsp->pint);
        atomic_store(&F.tx.tail, atomic_load(&F.boot_head));
        /* discard anything the old program pushed up to here, engine-side,
         * so the new program's BDF is the first word the 68k can read */
        atomic_store(&F.rx.tail, atomic_load(&F.rx.head));
        F.rx_last = 0;
        F.hrx_last = 0;
        F.ipr = F.bcr = F.pcc = F.pbc = 0;
        F.cra = F.crb = 0;
        F.ssisr = SSI_TDE;
        atomic_store(&F.hcr, 0);
        F.boot_count = 0;
        g_tx_vis_on = 0;
        F.answer_armed = 0;
        atomic_store(&F.dsp_state, DSP_BOOT);
        struct dsp_exec *x = atomic_exchange(&F.exec, NULL);
        if (x) {
            dsp_exec_load(x);
            free(x);
        }
        atomic_store(&g_dsp_resetting, 0);   /* new program is ready to run */
    }
    if (atomic_load(&F.reset_held))
        atomic_store(&F.dsp_state, DSP_HELD);
    /* "Resetting" means a reset is on its way to this thread: the line is
     * held (reset_held) or a release/Dsp_ExecProg is queued (a new epoch).
     * With neither, and the program running, the flag is stale - and a
     * stale flag hides every word the DSP sends (host_read8 treats the
     * port as empty), so each read crossed to this thread and the 68k
     * lived inside ACE's Timer A handler. Dsp_ExecProg sets the flag a
     * moment before it queues the epoch, but it is a trap on the 68k's
     * thread, so nothing reads the port in that moment. */
    if (atomic_load(&g_dsp_resetting) && !atomic_load(&F.reset_held) &&
        atomic_load(&F.reset_epoch) == F.reset_seen &&
        atomic_load(&F.dsp_state) == DSP_RUN) {
        atomic_store(&g_dsp_resetting, 0);
        g_resetting_cleared++;
    }
}

static void dsp_boot_drain(void)
{
    uint32_t v;
    while (F.boot_count < 512 && hf_peek(&F.tx, &v)) {
        hf_pop(&F.tx);
        F.dsp->pint[F.boot_count++] = v;
    }
    /* the 56001 bootstrap also starts early when the host sets HF0 */
    if (F.boot_count >= 512 || (F.boot_count && (atomic_load(&F.icr) & 0x08))) {
        dsp_start_at_0();
        dtr('B', F.boot_count, 0);
        fprintf(stderr, "[FALCON] DSP booted (%u words)\n", F.boot_count);
    }
}

/* PISTORM_DSP_INLINE (default 1): run the DSP on the 68k's own thread
 * while it drives the host port as a coprocessor (ACE Tracker's Timer A
 * exchange), so each host round-trip costs DSP cycles inline instead of a
 * cross-core hand-off to the engine - the latency that made ACE's IPL-3
 * Timer A loop overrun under interrupt load. In that mode the engine
 * leaves the DSP alone and only plays the sound-DMA buffer; one mutex
 * serialises the hand-off at the edges (SSI/DMA start-stop, reset).
 * PISTORM_DSP_INLINE=0 keeps the DSP on the engine thread as before. */
static pthread_mutex_t g_dsp_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_dsp_inline = 1;

/* Every dsp56k run goes through here: the mutex makes "engine runs it" and
 * "68k runs it inline" mutually exclusive. In steady state only one thread
 * ever takes it (the other side is gated off by cpu_coproc()), so it is
 * uncontended except for the instant ownership changes hands. */
/* PISTORM_FALCON_DEBUG=1, once: the first illegal DSP instruction, with
 * the code around it, the vectors and TOS's resident entry, and the last
 * 128 words the DSP took from the host port - to see whether a program
 * arrived intact or the DSP ran into memory nothing loaded. */
static void dsp_illegal_dump(void)
{
    static uint32_t seen;
    static int dumped;
    dsp56k_t *d = F.dsp;
    if (dumped || d->illegal_count == seen)
        return;
    seen = d->illegal_count;
    if (!falcon_dbg())
        return;
    dumped = 1;
    fprintf(stderr, "[FALCON] DSP illegal at p:$%04X - dump (pc now $%04X, %u boots)\n",
            d->illegal_pc, d->pc, F.boot_count);
    uint16_t c = d->illegal_pc;
    for (int r = -2; r < 4; r++) {
        uint16_t b = (uint16_t)((c & ~7u) + r * 8);
        fprintf(stderr, "[FALCON]   p:%04X:", b);
        for (int k = 0; k < 8; k++)
            fprintf(stderr, " %06X", dsp56k_mem_read(d, DSP_SPACE_P, (uint16_t)(b + k)));
        fprintf(stderr, "\n");
    }
    for (int b = 0; b < 0x48; b += 8) {
        fprintf(stderr, "[FALCON]   p:%04X:", b);
        for (int k = 0; k < 8; k++)
            fprintf(stderr, " %06X", dsp56k_mem_read(d, DSP_SPACE_P, (uint16_t)(b + k)));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[FALCON]   p:7EA9: %06X %06X %06X %06X\n",
            dsp56k_mem_read(d, DSP_SPACE_P, 0x7EA9), dsp56k_mem_read(d, DSP_SPACE_P, 0x7EAA),
            dsp56k_mem_read(d, DSP_SPACE_P, 0x7EAB), dsp56k_mem_read(d, DSP_SPACE_P, 0x7EAC));
    {
        uint32_t hn = d->hist_n, k0h = hn > 64u ? hn - 64u : 0u;
        fprintf(stderr, "[FALCON]   last %u DSP steps (I = interrupt: vector, return pc, long):\n", hn - k0h);
        for (uint32_t k = k0h; k < hn; k++) {
            const __typeof__(d->hist[0]) *h = &d->hist[k & 63u];
            if (h->kind)
                fprintf(stderr, "[FALCON]    I vec $%02X ret $%04X %s sr $%04X sp %u\n",
                        h->pc, h->pc2, h->op ? "long" : "fast", h->sr, h->sp & 15);
            else
                fprintf(stderr, "[FALCON]    p:$%04X %06X -> $%04X sr $%04X sp %u\n",
                        h->pc, h->op, h->pc2, h->sr, h->sp & 15);
        }
    }
    uint32_t n = g_hrx_n, k0 = n > 128u ? n - 128u : 0u;
    fprintf(stderr, "[FALCON]   last %u host words the DSP took (of %u):", n - k0, n);
    for (uint32_t k = k0; k < n; k++)
        fprintf(stderr, "%s%06X", (k - k0) % 12 ? " " : "\n[FALCON]    ", g_hrx_ring[k & 127u]);
    fprintf(stderr, "\n");
}

/* Parked: the last engine run ended in a wait loop (idle skip) and since
 * then nothing the DSP can see has changed - no interrupt pending, no SSI
 * slot, no 68k access to the host port or the sound registers, and nobody
 * else ran it (its cycle count is where that run left it). Running it
 * again would only go round the same loop to find out, so the time is
 * passed at once. FalcAMP waits like this for most of every frame; each
 * of the 8 runs a frame cost a lock, a pass through its loop and the
 * snapshot compare - an eighth of the Pi's engine time with nothing to do. */
static volatile uint32_t g_host_acc;      /* 68k accesses (defined below) */
static int      g_park_ok;
static uint32_t g_park_acc, g_park_ssi;
static uint64_t g_park_cyc;
static inline void dsp_run(uint32_t cycles)
{
    pthread_mutex_lock(&g_dsp_mtx);
    host_irqs();
    dsp56k_t *d = F.dsp;
    if (g_park_ok && !d->irq_pending && !d->halted && d->cycles == g_park_cyc &&
        g_park_acc == g_host_acc && g_park_ssi == g_ssi_n) {
        d->cycles += cycles;
        d->idle_skips++;
        g_park_cyc = d->cycles;
    } else {
        uint64_t sk = d->idle_skips;
        g_park_acc = g_host_acc;
        g_park_ssi = g_ssi_n;
        dsp56k_run(d, cycles);
        dsp_illegal_dump();
        g_park_ok = d->idle_skips != sk;
        g_park_cyc = d->cycles;
    }
    pthread_mutex_unlock(&g_dsp_mtx);
}

/* Coprocessor mode as either thread sees it: a program is running, it is
 * off the SSI (its time is nobody's sample count) and not VBL-locked, and
 * no reset is in flight. True => the 68k thread owns the DSP and runs it
 * inline through the host port; the engine must not touch it. The acquire
 * on g_dsp_resetting pairs with the engine clearing it last after a load,
 * so a true result also means the freshly loaded program is fully in DSP
 * memory. */
static inline int cpu_coproc(void)
{
#ifdef FALCON_HARNESS
    return 0;                 /* harness runs everything synchronously */
#endif
    /* Not while the sound DMA plays: ACE's DSP mixes the next buffer
     * between Timer A interrupts, in parallel with the 68k, as on the
     * Falcon. Run on the 68k's thread that mix comes out of the 68k's
     * time every buffer (tried 1 Oct: the mouse froze, files would not
     * load). During playback the DSP stays on the engine's core. */
    return g_dsp_inline &&
           atomic_load_explicit(&F.dsp_state, memory_order_acquire) == DSP_RUN &&
           !atomic_load_explicit(&g_dsp_resetting, memory_order_acquire) &&
           !atomic_load_explicit(&F.reset_held, memory_order_acquire) &&
           !atomic_load_explicit(&F.play_on, memory_order_relaxed) &&
           !F.vbl_locked && !(F.crb & 0x3000);
}

/* ------------------------------------------------------------------ */
/* Sound DMA and matrix (engine thread)                                */
/* ------------------------------------------------------------------ */
static inline int guest_ok(uint32_t a, uint32_t n)
{
    return F.guest && a < F.guest_size && n <= F.guest_size - a;
}
static inline uint16_t g16(uint32_t a)
{
    if (!guest_ok(a, 2)) return 0;
    return (uint16_t)((F.guest[a] << 8) | F.guest[a + 1]);
}
static inline uint8_t g8(uint32_t a)
{
    return guest_ok(a, 1) ? F.guest[a] : 0;
}
static inline void p16(uint32_t a, uint16_t v)
{
    if (!guest_ok(a, 2)) return;
    F.guest[a] = (uint8_t)(v >> 8);
    F.guest[a + 1] = (uint8_t)v;
}

/* $FF8900: bit 0 MFP-15 at play end, bit 1 MFP-15 at record end, bit 2
 * Timer A at play end, bit 3 Timer A at record end (Falcon hardware
 * reference; Hatari's crossbar, measured on the machine). These were the
 * other way round: ACE Tracker's $04 - Timer A after every buffer, the
 * interrupt its whole DSP round trip hangs off - raised MFP-15, which it
 * had not enabled, and its Timer A handler never ran. */
static void dma_frame_event(int rec)
{
    uint8_t c = F.reg[0x00];
    if (c & (rec ? 0x08 : 0x04)) {
        atomic_fetch_add_explicit(&F.ta_events, 1, memory_order_relaxed);
        mfp_hub_timer_a_event();
    }
    if (c & (rec ? 0x02 : 0x01))
        mfp_hub_raise(15);
}

/* 8 16-bit words of the playback DMA for this frame; 0 if idle */
static uint64_t dma_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* bytes per sample frame of the play DMA */
static uint32_t dma_play_bpf(void)
{
    int mode = (F.reg[0x21] >> 6) & 3;
    if (mode & 1)
        return 4u * (uint32_t)((F.reg[0x20] & 3) + 1);
    return mode == 0 ? 2u : 1u;
}

/* A play frame has ended: the event, then the next frame (registers
 * latched) or the stop. */
static void dma_play_end(uint64_t now)
{
    F.play_ev_ns = now;
    dma_frame_event(0);
    if (F.play_rep) {
        F.play_cnt = F.play_start;          /* next frame: latch the registers */
        F.play_fstart = F.play_start;
        F.play_fend = F.play_end;
    } else {
        atomic_store(&F.play_on, 0);
        F.reg[0x01] &= (uint8_t)~0x03;
        if (falcon_dbg())
            fprintf(stderr, "[FALCON] sound DMA once-play ended (frame %llu, DSP r3 $%04X)\n",
                    (unsigned long long)F.frames, F.dsp->r[3]);
    }
}

/* Frame-end pacing. On a Falcon the DMA and the 68k share one clock, so
 * a frame end is never closer to the previous one than a frame - the
 * handler that answers it (ACE Tracker's Timer A handler: it lowers
 * itself to IPL 3, takes the DSP's block and treats a second Timer A
 * arriving before it is done as "CPU Overload") always has a frame's
 * time. Here the engine plays on the wall clock and catches up in bursts
 * after anything holds it up (a device underrun, a busy core); every
 * frame end in a burst was a Timer A microseconds after the last. So a
 * frame end that would come less than half a frame after the previous
 * one waits, playing silence, until that much real time has passed. A
 * hiccup is then a short gap in the sound, never an interrupt storm. */
static int dma_play_frame(int16_t w[8])
{
    if (!atomic_load(&F.play_on))
        return 0;
    if (F.play_hold) {
        uint64_t now = dma_now_ns();
        if (now < F.play_hold_until) {
            memset(w, 0, 8 * sizeof w[0]);
            return 1;
        }
        F.play_hold = 0;
        dma_play_end(now);
        if (!atomic_load(&F.play_on))
            return 0;
    }
    int mode = (F.reg[0x21] >> 6) & 3;
    uint32_t c = F.play_cnt;
    if (mode & 1) {                                   /* 16-bit stereo */
        int tracks = (F.reg[0x20] & 3) + 1;
        for (int t = 0; t < tracks; t++) {
            w[2 * t]     = (int16_t)g16(c);
            w[2 * t + 1] = (int16_t)g16(c + 2);
            c += 4;
        }
    } else if (mode == 0) {                           /* 8-bit stereo */
        uint16_t v = g16(c);
        w[0] = (int16_t)((int8_t)(v >> 8) * 256);
        w[1] = (int16_t)((int8_t)v * 256);
        c += 2;
    } else {                                          /* 8-bit mono */
        w[0] = w[1] = (int16_t)((int8_t)g8(c) * 256);
        c += 1;
    }
    F.play_cnt = c;
    if (c >= F.play_fend) {
        uint64_t now = dma_now_ns();
        if (F.play_rep && (F.reg[0x00] & 0x05) && F.play_ev_ns && F.frame_hz > 0.0) {
            uint32_t len = F.play_fend > F.play_fstart ? F.play_fend - F.play_fstart : 0;
            uint64_t half = (uint64_t)((double)(len / dma_play_bpf()) / F.frame_hz * 0.5e9);
            if (now - F.play_ev_ns < half) {
                F.play_hold = 1;
                F.play_hold_until = F.play_ev_ns + half;
                atomic_fetch_add_explicit(&F.play_holds, 1, memory_order_relaxed);
                return 1;
            }
        }
        dma_play_end(now);
    }
    return 1;
}

static void dma_rec_frame(const int16_t *w, int valid)
{
    if (!atomic_load(&F.rec_on) || !valid)
        return;
    int tracks = (F.reg[0x36] & 3) + 1;              /* $8936 */
    uint32_t c = F.rec_cnt;
    for (int t = 0; t < tracks; t++) {
        p16(c, (uint16_t)w[2 * t]);
        p16(c + 2, (uint16_t)w[2 * t + 1]);
        c += 4;
        if (c >= F.rec_fend)
            break;
    }
    if (c >= F.rec_fend) {
        dma_frame_event(1);
        if (F.rec_rep) {
            c = F.rec_start;
            F.rec_fend = F.rec_end;
        } else {
            atomic_store(&F.rec_on, 0);
            F.reg[0x01] &= (uint8_t)~0x30;
        }
    }
    F.rec_cnt = c;
}

/* the sample rate a source device runs at */
static double src_rate(int src)
{
    uint16_t s = rw16(0x30);
    int clk;
    switch (src) {
    case 0: clk = (s >> 1) & 3; break;               /* DMA play */
    case 1: clk = (s >> 5) & 3; break;               /* DSP xmit */
    case 2: clk = (s >> 9) & 3; break;               /* ext in   */
    default: clk = (s >> 13) & 3; break;             /* ADC      */
    }
    int pre = F.reg[0x35] & 0x0F;
    if (pre == 0) {
        /* STE-compatible: the old prescaler in $8921 bits 0-1 */
        static const double ste[4] = { 6258.0, 12517.0, 25033.0, 50066.0 };
        return ste[F.reg[0x21] & 3];
    }
    double mhz = clk == 2 ? 32000000.0 : 25175000.0;
    if (clk == 1)
        pre = F.reg[0x34] & 0x0F ? F.reg[0x34] & 0x0F : pre;   /* ext: guess */
    return mhz / 256.0 / (pre + 1);
}

/* which source feeds each destination ($8932) */
static inline int dst_src(int dst)
{
    uint16_t d = rw16(0x32);
    switch (dst) {
    case 0: return (d >> 1) & 3;                     /* DMA record */
    case 1: return (d >> 5) & 3;                     /* DSP receive */
    case 2: return (d >> 9) & 3;                     /* ext out */
    default: return (d >> 13) & 3;                   /* DAC */
    }
}

/* nonzero when some path needs the sample clock */
static int matrix_active(void)
{
    int running = atomic_load(&F.dsp_state) == DSP_RUN;
    int dsp_ssi = running && (F.crb & 0x3000);
    return atomic_load(&F.play_on) || atomic_load(&F.rec_on) || dsp_ssi;
}

static double master_rate(void)
{
    int running = atomic_load(&F.dsp_state) == DSP_RUN;
    int dac = dst_src(3);
    if (dac == 0 && atomic_load(&F.play_on)) return src_rate(0);
    if (dac == 1 && running) return src_rate(1);
    if (atomic_load(&F.rec_on)) return src_rate(dst_src(0));
    if (running) return src_rate(1);
    return src_rate(0);
}

/* 10^(-1.5 dB * i / 20) */
static const float k_att[16] = {
    1.000000f, 0.841395f, 0.707946f, 0.595662f, 0.501187f, 0.421697f,
    0.354813f, 0.298538f, 0.251189f, 0.211349f, 0.177828f, 0.149624f,
    0.125893f, 0.105925f, 0.089125f, 0.074989f
};

static int16_t g_out[512 * 2];
static unsigned g_out_n;

static void frame(void)
{
    int16_t dma[8] = { 0 }, dsp[8] = { 0 }, zero[8] = { 0 };
    int have_dma = dma_play_frame(dma);
    int running = atomic_load(&F.dsp_state) == DSP_RUN;
    int dsp_valid = 0;
    /* In coprocessor mode the 68k thread owns the DSP (it is off the SSI,
     * so its slots feed nothing here); the engine only plays the DMA
     * buffer. Do not clock the DSP from the engine then. */
    int run_dsp = running && !cpu_coproc();

    /* DSP: 8 slots across the frame */
    double slot = F.cyc_per_frame / 8.0;
    if (run_dsp) {
        int n = ssi_slots();
        const int16_t *rxsrc = dst_src(1) == 0 ? dma : zero;
        for (int s = 0; s < 8; s++) {
            F.cyc_acc += slot;
            uint32_t run = 0;
            if (F.cyc_acc >= 1.0) {
                run = (uint32_t)F.cyc_acc;
                F.cyc_acc -= run;
            }
            if (s < n) {
                int v;
                uint32_t w = ssi_slot(s, (uint32_t)(uint16_t)rxsrc[s] << 8, &v);
                dsp[s] = (int16_t)(w >> 8);
                dsp_valid |= v;
            }
            if (run)
                dsp_run(run);
        }
    }

    /* DAC */
    int16_t l = 0, r = 0;
    if (F.reg[0x37] & 0x02) {                       /* matrix -> DAC adder */
        switch (dst_src(3)) {
        case 0:
            if (have_dma) {
                int mon = (F.reg[0x20] >> 4) & 3;
                l = dma[2 * mon]; r = dma[2 * mon + 1];
            }
            break;
        case 1:
            if (dsp_valid) { l = dsp[0]; r = dsp[1]; }
            break;
        default: break;
        }
    }
    /* attenuation: $893A bits 11-8 left, 7-4 right, 1.5 dB steps */
    uint16_t att = rw16(0x3A);
    int al = (att >> 8) & 0xF, ar = (att >> 4) & 0xF;
    if (al) l = (int16_t)((float)l * k_att[al]);
    if (ar) r = (int16_t)((float)r * k_att[ar]);
    g_out[g_out_n * 2] = l;
    g_out[g_out_n * 2 + 1] = r;
    g_out_n++;

    /* record */
    switch (dst_src(0)) {
    case 0: dma_rec_frame(dma, have_dma); break;
    case 1: dma_rec_frame(dsp, dsp_valid); break;
    default: dma_rec_frame(zero, 1); break;
    }

    F.frames++;
    if (g_out_n == 512) {
        falcon_audio_push(g_out, g_out_n);
        g_out_n = 0;
    }
}

static void out_flush(void)
{
    if (g_out_n) {
        falcon_audio_push(g_out, g_out_n);
        g_out_n = 0;
    }
}

/* ------------------------------------------------------------------ */
/* The engine thread                                                   */
/* ------------------------------------------------------------------ */
/* The sample clock follows CLOCK_MONOTONIC, a few dozen frames at a
 * time, so DSP time advances smoothly with the 68k's. Filling the audio
 * ring in big lumps instead (as the SDL device takes its periods) made
 * DSP time jump 20 ms at once: DSPMOD, which refills the DSP's buffer
 * once a VBL, then found it drained and played in bursts, and its VBL
 * sat polling the host port for a reply that waited on the next lump.
 * The ring only absorbs the device's lumpy reads; its fill level trims
 * the clock when the device's crystal and ours drift apart. */
#define ENGINE_LEAD_MS  50u     /* audio queued ahead of the device */
#define ENGINE_CHUNK    48u     /* frames per step (~1 ms at 49 kHz) */
#define ENGINE_HICCUP_S 0.010   /* longer gaps are counted as hiccups */

/* Lockstep with the 68k (default; PISTORM_FALCON_VBLSYNC=0 turns it off).
 *
 * DSPMOD asks the DSP once a VBL how many samples it played since the
 * last time and mixes that many. On the wall clock the answer follows
 * when in its VBL the 68k happened to ask - 3 ms after the last time, or
 * 33 - and BOR's VBL work on the PiStorm varies enough that the answers
 * swung from a handful to twice a VBL's worth; DSPMOD's buffers do not
 * hold that, the DSP ran off into garbage, and an answer of 0 is a
 * 65536-pass mix loop.
 *
 * So while the DSP answers the 68k about once a VBL (70% of VBLs to
 * engage, 40% to let go), sample periods are paid per answer: exactly one
 * VBL's worth (VBL period measured by ipl_task) for each answer, played at
 * once as a burst, and nothing the 68k sends after an answer reaches the
 * DSP until that burst is done. Every answer is then one VBL of samples,
 * as on a Falcon whose VBL is never late. Between bursts the DSP gets
 * instruction cycles while the 68k waits on it, so it reads, mixes and
 * answers at once. The ring's fill trims the samples per VBL (PI, +-3%),
 * which is all the audio device needs; two answers are the most one
 * burst will pay for. */
static _Atomic uint32_t g_vbl_edges;
static _Atomic uint32_t g_vbl_period_us = 20000;   /* ipl_task writes */
static _Atomic uint64_t g_vbl_seen_ns;
static int              g_vbl_sync = 1;

static uint64_t eng_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* the first clock_due() asks for the whole lead at once, priming the
 * ring so the device never starts dry */
static void clock_restart(double hz)
{
    F.last_ns = eng_now_ns();
    F.clk = 0.0;
    F.emitted = 0;
    F.fill_avg = hz * ENGINE_LEAD_MS / 1000.0 + 1024.0;
    F.adj_i = 0.0;
    F.talk_win_ns = F.last_ns;
    F.answers_at_win = F.answers;
    F.answers_paid = F.answers;
    F.vbl_locked = 0;
    g_tx_vis_on = 0;
}

/* Frames due now (may be <= 0). The clock runs at the nominal rate
 * warped by a few % at most to hold the ring's AVERAGE fill on target: the
 * device reads in periods (1-4K frames on the Pi's HDMI), so the fill
 * swings by a period every read and only its average says anything
 * about drift. A hard jump is kept for gross upsets only (device
 * stalled or restarted) - an earlier version jumped on the momentary
 * fill, and each jump stopped the DSP for ~50 ms. */
static int64_t clock_due(double hz)
{
    uint64_t now = eng_now_ns();
    double dt = (double)(int64_t)(now - F.last_ns) * 1e-9;
    F.last_ns = now;
    if (dt < 0.0) dt = 0.0;
    /* The engine was held off (descheduled, or starved of CPU): the time
     * is caught up, so DSP time never falls behind the 68k's for long -
     * DSPMOD asks the DSP once a VBL how many samples it played since
     * the last time, and a DSP that stood still answers 0, which DSPMOD
     * passes on as a mix-loop count: 65536 passes, a stall and a trashed
     * buffer. Beyond a quarter second it is a real stop (the guest
     * paused), not a hiccup. */
    if (dt > ENGINE_HICCUP_S)
        F.hiccups++;
    if (dt > 0.25)
        dt = 0.25;

    double lead = hz * ENGINE_LEAD_MS / 1000.0;
    double target = lead + 1024.0;
    double fill = (double)falcon_audio_fill();
    double k = dt / 0.5;                        /* 0.5 s average */
    F.fill_avg += (fill - F.fill_avg) * (k > 1.0 ? 1.0 : k);

    /* PI: the integral learns the crystal difference, so the average
     * fill settles on target instead of beside it */
    double e = (F.fill_avg - target) / lead;
    /* lockstep pays per answer, and a 68k that misses the odd VBL answers
     * less often than the VBL rate: allow more trim there (+-15%, which
     * only moves each answer by as much) than on the wall clock (+-3%) */
    double lim = F.vbl_locked ? 0.15 : 0.03;
    F.adj_i -= (F.vbl_locked ? 0.03 : 0.01) * e * dt;
    if (F.adj_i > lim * 0.8) F.adj_i = lim * 0.8;
    if (F.adj_i < -lim * 0.8) F.adj_i = -lim * 0.8;
    double adj = F.adj_i - 0.02 * e;
    if (adj > lim) adj = lim;
    if (adj < -lim) adj = -lim;

    /* Lockstep with the 68k (see the note above g_vbl_edges): while the
     * DSP answers the 68k about once a VBL, each answer buys exactly one
     * VBL of sample periods; otherwise the wall clock drives. */
    if (now - F.talk_win_ns >= 250000000ull) {
        double vbls = (double)(now - F.talk_win_ns) * 1e-3 /
                      (double)atomic_load(&g_vbl_period_us);
        uint32_t a = F.answers - F.answers_at_win;
        F.talk_win_ns = now;
        F.answers_at_win = F.answers;
        /* on at 70% of a VBL's answers, off below 40% - and only while
         * the DSP is what the DAC plays (DSPMOD: mixed into the SSI).
         * A DSP that answers the 68k through the host port and leaves
         * the playing to the sound DMA (ACE Tracker: samples go both
         * ways through the port, several answers a buffer) is a
         * coprocessor; holding its host words back a VBL per answer
         * stretched ACE's Timer A handler across seconds, starving its
         * main loop. Nor is a DSP answering many times a VBL "once a
         * VBL". */
        double r = vbls > 0.0 ? (double)a / vbls : 0.0;
        int want = F.vbl_locked ? r >= 0.4 : r >= 0.7;
        if (r > 3.0 || dst_src(3) != 1)
            want = 0;
        if (!g_vbl_sync)
            want = 0;
        if (want != F.vbl_locked) {
            F.vbl_locked = want;
            /* carry on from here, topping the ring up to its target so the
             * switch itself is not heard */
            F.clk = (double)F.emitted - lead + (fill < target ? target - fill : 0.0);
            F.answers_paid = F.answers;
            F.answer_gap = 0.0;
            F.answer_ns = 0;
            if (!want)
                g_tx_vis_on = 0;
        }
    }
    if (F.vbl_locked) {
        uint32_t n = F.answers - F.answers_paid;
        F.answers_paid = F.answers;
        if (n > 2) {
            n = 2;
            F.vbl_skipped++;
        }
        /* one answer's worth: the answers' own average spacing, not the
         * VBL period - Beats of Rage answers 50 times a second on a 60 Hz
         * VGA VBL (its handler overruns the frame), and paying a VBL per
         * answer left the ring 17% short */
        double gap = F.answer_gap > 0.0 ? F.answer_gap
                   : (double)atomic_load(&g_vbl_period_us) * 1e-6;
        double fpv = hz * gap * (1.0 + adj);
        F.clk += (double)n * fpv;
        /* the 68k went quiet mid-lockstep: do not let owed time pile up */
        double floor_clk = (double)F.emitted - lead - fpv;
        if (F.clk < floor_clk)
            F.clk = floor_clk;
    } else {
        F.clk += dt * hz * (1.0 + adj);
    }

    if (fill > 8.0 * lead + 8192.0 ||
        (F.emitted > (uint64_t)(8.0 * lead) && fill < 16.0 && F.fill_avg < lead / 2.0)) {
        F.clk += target - fill;
        F.fill_avg = target;
        F.resyncs++;
    }
    return (int64_t)F.clk + (int64_t)lead - (int64_t)F.emitted;
}

/* the last words the 68k sent the DSP (CPU thread writes, the stall
 * report reads them racily - fine for a log) */
#define TXLOG 128u
static uint32_t g_txlog[TXLOG];
static _Atomic uint32_t g_txlog_n;

/* Timing seen from the 68k side, for the 10 s status line:
 *  - the real VBL (ipl_task, falcon_vbl): longest gap, gaps > 25 ms
 *  - how long the 68k waits for the DSP: from its first poll of ISR that
 *    finds nothing to read (after its last write) to the word it reads */
static _Atomic uint64_t g_vbl_last_ns;
static _Atomic uint32_t g_vbl_maxgap_us, g_vbl_late;
static uint64_t         g_req_ns;            /* CPU thread only */
static _Atomic uint32_t g_wait_max_us, g_wait_sum_us, g_wait_n;

/* The 68k is reading the host port's RX with nothing in it (rx_wait) */
static _Atomic int      g_rx_starved;
static _Atomic uint32_t g_rx_wait_short;    /* rx_wait cut to 2 ms (no question out) */
static _Atomic uint32_t g_rx_wait_parked;   /* rx_wait gave up: DSP parked */
/* Wall-clock of the last host-port access by the 68k. ACE Tracker runs
 * its whole DSP exchange inside the Timer A ISR and must finish within a
 * tiny (~2.6 ms) buffer; each host round-trip that waits for this engine
 * thread to be scheduled adds up and the ISR overruns, so ACE sees an
 * overload and resets the DSP. While the 68k is actively working the
 * port the engine runs the DSP flat out instead of sleeping between
 * round-trips, so an exchange costs DSP cycles, not thread wake-ups. */
static _Atomic uint64_t g_host_act_ns;
/* "actively exchanging" window: how long after a host-port access the
 * engine keeps spinning the DSP instead of sleeping, so back-to-back
 * round-trips inside one of ACE Tracker's per-frame exchanges cost DSP
 * cycles, not a thread wake-up each. Default 150 us - long enough to
 * bridge the gaps between round-trips within an exchange burst, short
 * enough to sleep in the idle gap between sound frames (~5 ms) so the
 * engine core is not pegged (the old 8 ms pegged it and made the machine
 * fragile under mouse load). PISTORM_DSP_HOT_US tunes it; 0 = never spin
 * on the window (rely on the coproc spin + kick only). */
static uint64_t g_host_hot_ns = 150000ull;
/* PISTORM_DSP_HOT_US: the exchange-spin window in microseconds (g_host_hot_ns).
 * Read once. 0 disables the window entirely. */
static void host_hot_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    const char *e = getenv("PISTORM_DSP_HOT_US");
    if (e) {
        long us = atol(e);
        if (us < 0) us = 0;
        if (us > 5000) us = 5000;
        g_host_hot_ns = (uint64_t)us * 1000ull;
    }
}
static inline int host_hot(void)
{
    uint64_t w = g_host_hot_ns;
    return w &&
           eng_now_ns() - atomic_load_explicit(&g_host_act_ns, memory_order_relaxed) < w;
}
static _Atomic uint32_t g_rx_waits, g_rx_wait_max_us, g_rx_wait_timeouts;
/* dsp_settle: the coprocessor DSP run on the 68k thread (see there) */
static int g_settled;                /* CPU thread only                   */
/* 68k host-port accesses, counted on the CPU thread (no clock read): the
 * engine compares it across its 2 ms sleeps to see the 68k has gone quiet
 * on the port while the DSP still has its words to read (coproc_idle). */
static volatile uint32_t g_host_acc;
static _Atomic uint32_t g_settles, g_settle_max_us, g_settle_cutoffs;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* PISTORM_FALCON_PROF=1: where a host-port access spends its time. Every
 * 5 s: reads/writes per second as the 68k made them, the average time
 * inside falcon_hw per access, and how much of that was the DSP running
 * (dsp_settle). 1e9 / rate minus the time inside is what the access cost
 * outside this file (JIT, bus dispatch). One counter read per access. */
static int g_prof = -1;
static _Atomic uint64_t g_prof_rd_n, g_prof_rd_t, g_prof_wr_n, g_prof_wr_t, g_prof_dsp_t;
static inline uint64_t prof_ticks(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return mono_ns();
#endif
}
static inline double prof_ns_per_tick(void)
{
#if defined(__aarch64__)
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? 1e9 / (double)f : 1.0;
#else
    return 1.0;
#endif
}
static inline int prof_on(void)
{
    if (g_prof < 0) { const char *e = getenv("PISTORM_FALCON_PROF"); g_prof = (e && *e == '1'); }
    return g_prof;
}
static void prof_report(void)
{
    static uint64_t t0;
    uint64_t now = mono_ns();
    if (!t0) { t0 = now; return; }
    if (now - t0 < 5000000000ull)
        return;
    double secs = (double)(now - t0) / 1e9, k = prof_ns_per_tick();
    t0 = now;
    uint64_t rn = atomic_exchange(&g_prof_rd_n, 0), rt = atomic_exchange(&g_prof_rd_t, 0);
    uint64_t wn = atomic_exchange(&g_prof_wr_n, 0), wt = atomic_exchange(&g_prof_wr_t, 0);
    uint64_t dt = atomic_exchange(&g_prof_dsp_t, 0);
    if (!rn && !wn)
        return;
    fprintf(stderr, "[FALCON] prof 5 s: reads %.0f/s (%.0f ns inside each), writes %.0f/s "
            "(%.0f ns inside each), DSP runs %.0f ms of the %.0f ms inside\n",
            rn / secs, rn ? rt * k / rn : 0.0, wn / secs, wn ? wt * k / wn : 0.0,
            dt * k / 1e6, (rt + wt) * k / 1e6);
}


static void atomic_max_u32(_Atomic uint32_t *p, uint32_t v)
{
    uint32_t o = atomic_load_explicit(p, memory_order_relaxed);
    while (v > o && !atomic_compare_exchange_weak(p, &o, v))
        ;
}

void falcon_vbl(void)
{
    if (!atomic_load_explicit(&F.armed, memory_order_relaxed))
        return;
    uint64_t now = mono_ns();
    uint64_t last = atomic_exchange(&g_vbl_last_ns, now);
    if (!last)
        return;
    uint32_t us = (uint32_t)((now - last) / 1000u);
    atomic_max_u32(&g_vbl_maxgap_us, us);
    if (us > 25000u)
        atomic_fetch_add_explicit(&g_vbl_late, 1, memory_order_relaxed);
    /* the VBL period (50 / 60 / 71 Hz), from ordinary intervals only */
    if (us >= 10000u && us <= 22000u) {
        uint32_t p = atomic_load_explicit(&g_vbl_period_us, memory_order_relaxed);
        atomic_store_explicit(&g_vbl_period_us, p + (uint32_t)(((int32_t)us - (int32_t)p) / 16),
                              memory_order_relaxed);
    }
    atomic_store_explicit(&g_vbl_seen_ns, now, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_vbl_edges, 1, memory_order_release);
    /* no kick from here: ipl_task must not touch a mutex. The engine
     * looks at least every 0.5 ms. */
}

/* Why a Falcon program is stuck, for the log: the 68k has been polling
 * the host port's status for a while and nothing has moved either way. */
static void watchdog(void)
{
    static uint64_t since_ns;
    static uint32_t polls0, txt0, rxh0, ill0;
    static int reported;
    uint64_t now = eng_now_ns();
    dsp56k_t *d = F.dsp;

    if (d->illegal_count != ill0) {
        static int shown;
        if (shown < 4) {
            shown++;
            fprintf(stderr, "[FALCON] DSP illegal instruction near p:$%04X (%u so far)\n",
                    d->illegal_pc, d->illegal_count);
        }
        ill0 = d->illegal_count;
    }
    if (now - since_ns < 300000000ull)
        return;
    uint32_t polls = atomic_load(&F.isr_polls);
    uint32_t txt = atomic_load(&F.tx.tail), rxh = atomic_load(&F.rx.head);
    int moved = txt != txt0 || rxh != rxh0;
    int polling = polls - polls0 > 1000;
    if (moved || !polling) {
        reported = 0;
    } else if (!reported) {
        reported = 1;
        uint16_t pcs[12];
        for (int i = 0; i < 12; i++) {
            dsp_run(3);
            pcs[i] = d->pc;
        }
        fprintf(stderr, "[FALCON] stall: 68k polling the host port, nothing moving for 0.3 s - "
                "host tx %u rx %u, DSP pc $%04X sr $%04X sp %u la $%04X lc %u, "
                "SSI cra $%04X crb $%04X sr $%02X, hcr $%02X ipr $%06X, irq %llx\n",
                hf_count(&F.tx), hf_count(&F.rx), d->pc, d->sr, d->sp & 15, d->la, d->lc,
                F.cra, F.crb, F.ssisr, atomic_load(&F.hcr), F.ipr,
                (unsigned long long)d->irq_pending);
        fprintf(stderr, "[FALCON] stall: DSP pcs");
        for (int i = 0; i < 12; i++)
            fprintf(stderr, " %04X", pcs[i]);
        fprintf(stderr, "\n[FALCON] stall: last DSP->68k words (value, +frames, +ms):");
        unsigned n = g_rxlog_n < RXLOG ? g_rxlog_n : RXLOG;
        for (unsigned k = 0; k < n; k++) {
            unsigned i = (g_rxlog_n - n + k) % RXLOG;
            unsigned p = (i + RXLOG - 1) % RXLOG;
            int first = k == 0;
            fprintf(stderr, " $%06X(+%llu,+%.1f)", g_rxlog[i].v,
                    first ? 0ull : (unsigned long long)(g_rxlog[i].frames - g_rxlog[p].frames),
                    first ? 0.0 : (double)(g_rxlog[i].ns - g_rxlog[p].ns) / 1e6);
        }
        fprintf(stderr, "\n[FALCON] stall: DSP r0-r7");
        for (int i = 0; i < 8; i++)
            fprintf(stderr, " %04X", d->r[i]);
        fprintf(stderr, " n0-n7");
        for (int i = 0; i < 8; i++)
            fprintf(stderr, " %04X", d->n[i]);
        fprintf(stderr, "\n[FALCON] stall: DSP x:$0000-$001F");
        for (int i = 0; i < 32; i++)
            fprintf(stderr, " %06X", dsp56k_mem_read(d, DSP_SPACE_X, (uint16_t)i));
        uint32_t tn = atomic_load(&g_txlog_n);
        uint32_t cnt = tn < TXLOG ? tn : TXLOG;
        fprintf(stderr, "\n[FALCON] stall: last %u 68k->DSP words:", cnt);
        for (uint32_t k = 0; k < cnt; k++) {
            if (k % 16 == 0)
                fprintf(stderr, "\n[FALCON]   ");
            fprintf(stderr, " %06X", g_txlog[(tn - cnt + k) % TXLOG]);
        }
        fprintf(stderr, "\n");
    }
    since_ns = now;
    polls0 = polls;
    txt0 = txt;
    rxh0 = rxh;
}

static int host_pending(void)
{
    return hf_count(&F.tx) || (atomic_load(&F.cvr) & 0x80);
}

/* The 68k is waiting on the DSP: it has sent something the DSP has not
 * taken, or keeps polling ISR with nothing to read. */
static int host_waiting(void)
{
    static uint32_t polls0;
    uint32_t p = atomic_load_explicit(&F.isr_polls, memory_order_relaxed);
    int polling = p != polls0;
    polls0 = p;
    return host_pending() || atomic_load_explicit(&g_rx_starved, memory_order_relaxed) ||
           (polling && !hf_count(&F.rx));
}

/* ------------------------------------------------------------------ */
/* Trace dumps, the stall check and the [DSPPORT] line (engine thread) */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t g_tr_sig;
static void dtr_on_sigusr2(int sig)
{
    (void)sig;
    g_tr_sig = 1;                        /* the engine dumps within ~2 ms */
}

static const char *dtr_what(char t)
{
    switch (t) {
    case 'W': return "68k wrote word (x = words queued before)";
    case 'R': return "68k read word (x = words left before)";
    case 'r': return "68k read RX, nothing there: old word";
    case 'D': return "68k word DROPPED, FIFO full";
    case 'I': return "68k wrote ICR (x = old)";
    case 'C': return "68k wrote CVR (x = old)";
    case 'V': return "68k wrote IVR (x = old)";
    case 'P': return "68k read ISR, changed (x = words queued to DSP)";
    case 'w': return "RX-read wait ended: v = us, x 1 word 2 parked 3 timeout +16 short";
    case 's': return "same-thread DSP run: v = cycles, x 1 parked 2 cut off 3 stopped";
    case 'G': return "DSP read HRX (x = words left before)";
    case 'g': return "DSP read HRX, nothing there";
    case 'S': return "DSP wrote HTX (x = words unread before)";
    case 'H': return "DSP took host command, v = vector (x = CVR)";
    case 'F': return "DSP wrote HCR (x = old)";
    case 'X': return "DSP reset released";
    case 'B': return "DSP bootstrap done (v = words)";
    }
    return "?";
}

/* the last `want` entries (0 = the whole ring) and the port state */
/* number formatting for dtr_dump: printf per line took long enough on the
 * Pi (4M lines) that a dump was cut short by quitting the emulator */
static char *put_dec(char *p, uint64_t v, int width)
{
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = n; i < width; i++) *p++ = ' ';
    while (n) *p++ = t[--n];
    return p;
}
static char *put_hex(char *p, uint32_t v, int digits)
{
    static const char hx[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) *p++ = hx[(v >> (4 * i)) & 15];
    return p;
}

static void dtr_dump(const char *why, uint32_t want)
{
    static unsigned seq;
    char path[256];
    const char *pre = getenv("PISTORM_DSP_TRACE_FILE");
    snprintf(path, sizeof path, "%s-%03u.txt", pre && *pre ? pre : "/tmp/dsptrace", seq++);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[DSPPORT] trace dump: cannot write %s\n", path);
        return;
    }
    uint32_t n = atomic_load(&g_tr_n);
    if (g_tr_first && n > g_tr_size)
        n = g_tr_size;
    uint32_t have = n < g_tr_size ? n : g_tr_size;
    if (want && want < have)
        have = want;
    dsp56k_t *d = F.dsp;
    fprintf(f, "# PiStorm Falcon host-port trace: %s\n", why);
    fprintf(f, "# 68k: ICR $%02X CVR $%02X IVR $%02X, 68k->DSP %u queued, DSP->68k %u unread, "
            "last word read $%06X\n", atomic_load(&F.icr), atomic_load(&F.cvr), F.ivr,
            hf_count(&F.tx), hf_count(&F.rx), F.rx_last);
    fprintf(f, "# DSP: pc $%04X sr $%04X HCR $%02X IPR $%06X, last HRX $%06X, idle hint %d, "
            "irq %llx, state %d, %s, %s\n", d->pc, d->sr, atomic_load(&F.hcr), F.ipr,
            F.hrx_last, d->idle_hint, (unsigned long long)d->irq_pending,
            atomic_load(&F.dsp_state), cpu_coproc() ? "same-thread (coprocessor)" :
            F.vbl_locked ? "engine, VBL lockstep" : "engine",
            (F.crb & 0x3000) ? "SSI on" : "SSI off");
    fprintf(f, "# frame %llu, ISR polls %u, %u events in all, last %u below\n",
            (unsigned long long)F.frames, atomic_load(&F.isr_polls), n, have);
    fprintf(f, "#     seq        ms    frame t  value   x  dsp-pc\n");
    if (have > 100000)
        fprintf(stderr, "[DSPPORT] writing trace dump (%s): %u events to %s - "
                "wait for \"done\" before quitting\n", why, have, path);
    static char buf[1 << 20];
    char *p = buf;
    uint64_t t0 = 0;
    for (uint32_t k = n - have; k != n; k++) {
        const dtr_t *e = &g_tr[k & (g_tr_size - 1)];
        if (!t0)
            t0 = e->ns;
        /* "%9u %9.3f %8u %c %06X %3u  %04X\n" */
        int64_t us = (int64_t)(e->ns - t0) / 1000;
        int neg = us < 0;
        if (neg) us = -us;
        p = put_dec(p, k, 9);
        *p++ = ' ';
        {
            char t[24], *q = put_dec(t, (uint64_t)us / 1000, 0);
            int len = (int)(q - t) + 4 + neg;
            for (int i = len; i < 9; i++) *p++ = ' ';
            if (neg) *p++ = '-';
            for (char *r = t; r < q; r++) *p++ = *r;
            *p++ = '.';
            uint32_t frac = (uint32_t)((uint64_t)us % 1000);
            *p++ = (char)('0' + frac / 100);
            *p++ = (char)('0' + frac / 10 % 10);
            *p++ = (char)('0' + frac % 10);
        }
        *p++ = ' ';
        p = put_dec(p, e->frame, 8);
        *p++ = ' ';
        *p++ = e->t;
        *p++ = ' ';
        p = put_hex(p, e->v & 0xFFFFFF, 6);
        *p++ = ' ';
        p = put_dec(p, e->x, 3);
        *p++ = ' ';
        *p++ = ' ';
        p = put_hex(p, e->dpc, 4);
        *p++ = '\n';
        if (p - buf > (long)sizeof buf - 128) {
            fwrite(buf, 1, (size_t)(p - buf), f);
            p = buf;
        }
    }
    fwrite(buf, 1, (size_t)(p - buf), f);
    fprintf(f, "# event types:\n");
    static const char types[] = "WRrDICVPwsGgSHFXB";
    for (const char *t = types; *t; t++)
        fprintf(f, "#   %c  %s\n", *t, dtr_what(*t));
    fclose(f);
    fprintf(stderr, "[DSPPORT] trace dump (%s): %u events -> %s%s\n", why, have, path,
            have > 100000 ? " - done" : "");
}

/* A whole-ring dump is millions of lines: written on its own thread so the
 * engine keeps clocking the DSP and the sound plays on (on the engine it
 * stopped both for the length of the write - "frozen, no sound, no more
 * logging"). Recording is held while it writes, so the dump is the ring
 * exactly as it was when asked for; events in that time are not kept. */
static _Atomic int g_tr_dumping;
static _Atomic(const char *) g_tr_mark_why;
static int g_tr_mark_done;               /* its dump has been started */
struct dtr_job { const char *why; int resume; };
static void *dtr_dump_thread(void *arg)
{
    struct dtr_job *j = (struct dtr_job *)arg;
    dtr_dump(j->why, 0);
    if (j->resume && !atomic_load(&g_tr_mark_why))
        atomic_store(&g_tr_pause, 0);
    atomic_store(&g_tr_dumping, 0);
    free(j);
    return NULL;
}
static void dtr_dump_async(const char *why, int resume)
{
    if (atomic_exchange(&g_tr_dumping, 1))
        return;                              /* one at a time */
    atomic_store(&g_tr_pause, 1);
    struct dtr_job *j = (struct dtr_job *)malloc(sizeof *j);
    pthread_t th;
    if (j) {
        j->why = why;
        j->resume = resume;
        if (pthread_create(&th, NULL, dtr_dump_thread, j) == 0) {
            pthread_detach(th);
            return;
        }
        free(j);
    }
    dtr_dump(why, 0);                        /* no thread: write it here */
    if (resume && !atomic_load(&g_tr_mark_why))
        atomic_store(&g_tr_pause, 0);
    atomic_store(&g_tr_dumping, 0);
}

/* Something outside the DSP went wrong (the 68k ran into an illegal
 * instruction): hold the ring as it is and write it. Called from the CPU
 * thread; the engine starts the write on its next pass. */
void falcon_trace_mark(const char *why)
{
    if (!g_tr_size)
        return;
    atomic_store(&g_tr_pause, 1);
    const char *none = NULL;
    atomic_compare_exchange_strong(&g_tr_mark_why, &none, why);
}

static void dtr_tick(void)
{
    if (!g_tr_size)
        return;
    if (g_tr_sig) {
        g_tr_sig = 0;
        dtr_dump_async("SIGUSR2", 1);
    }
    {
        static int marked;
        const char *why = atomic_load(&g_tr_mark_why);
        if (why && !marked && !atomic_load(&g_tr_dumping)) {
            marked = 1;                      /* recording stays held */
            g_tr_mark_done = 1;
            dtr_dump_async(why, 0);
        }
    }
    if (g_tr_first) {
        static int dumped;
        if (!dumped && atomic_load(&g_tr_n) >= g_tr_size) {
            dumped = 1;
            dtr_dump_async("first events: the ring is full, recording stopped", 0);
        }
    }
    uint64_t now = rxlog_now();
    /* Stall: the 68k keeps polling the port and nothing has moved either
     * way - no word, no host command - for 0.3 s. Dumped once per stall. */
    {
        static uint64_t at;
        static uint32_t txh, txt, rxh, rxt, hc, polls;
        static int reported;
        if (now - at >= 300000000ull) {
            uint32_t a = atomic_load(&F.tx.head), b = atomic_load(&F.tx.tail);
            uint32_t c = atomic_load(&F.rx.head), e = atomic_load(&F.rx.tail);
            uint32_t h = atomic_load(&g_ps.hc_req) + atomic_load(&g_ps.hc_taken);
            uint32_t p = atomic_load(&F.isr_polls);
            int moved = a != txh || b != txt || c != rxh || e != rxt || h != hc;
            if (moved || p - polls < 1000u) {
                reported = 0;
            } else if (!reported && atomic_load(&F.dsp_state) == DSP_RUN &&
                       !atomic_load(&g_tr_dumping)) {
                reported = 1;
                fprintf(stderr, "[DSPPORT] stall: the 68k polls the host port, nothing has moved "
                        "for 0.3 s - DSP pc $%04X, %u queued to the DSP, %u unread by the 68k, "
                        "CVR $%02X\n", F.dsp->pc, hf_count(&F.tx), hf_count(&F.rx),
                        atomic_load(&F.cvr));
                dtr_dump("stall: 68k polling, nothing moving either way for 0.3 s", 512);
            }
            at = now;
            txh = a; txt = b; rxh = c; rxt = e; hc = h; polls = p;
        }
    }
    /* every 5 s: what each sync point cost */
    {
        static uint64_t at;
        static uint32_t polls0;
        if (!at)
            at = now;
        if (now - at >= 5000000000ull) {
            at = now;
            uint32_t p = atomic_load(&F.isr_polls);
            uint32_t rn = atomic_exchange(&g_ps.rxw_n, 0), rs = atomic_exchange(&g_ps.rxw_sum_us, 0);
            uint32_t sn = atomic_exchange(&g_ps.st_n, 0), ss = atomic_exchange(&g_ps.st_sum_us, 0);
            uint32_t ht = atomic_exchange(&g_ps.hc_taken, 0), hs = atomic_exchange(&g_ps.hc_sum_us, 0);
            fprintf(stderr, "[DSPPORT] 5 s: RX-read waits %u (avg %.2f max %.2f ms; word %u, "
                    "parked %u, timed out %u, short %u), empty RX reads %u; "
                    "same-thread runs %u (avg %u max %u us; parked %u, cut off %u); "
                    "HC requested %u, taken %u (avg %u max %u us; rewritten while pending %u, "
                    "withdrawn %u, pending now %s); 68k->DSP %u words (max queued %u, dropped %u), "
                    "DSP->68k %u words (max unread %u); ISR polls %u\n",
                    rn, rn ? rs / 1000.0 / rn : 0.0, atomic_exchange(&g_ps.rxw_max_us, 0) / 1000.0,
                    atomic_exchange(&g_ps.rxw_got, 0), atomic_exchange(&g_ps.rxw_parked, 0),
                    atomic_exchange(&g_ps.rxw_timeout, 0), atomic_exchange(&g_ps.rxw_short, 0),
                    atomic_exchange(&g_ps.rx_stale, 0),
                    sn, sn ? ss / sn : 0, atomic_exchange(&g_ps.st_max_us, 0),
                    atomic_exchange(&g_ps.st_parked, 0), atomic_exchange(&g_ps.st_cut, 0),
                    atomic_exchange(&g_ps.hc_req, 0), ht, ht ? hs / ht : 0,
                    atomic_exchange(&g_ps.hc_max_us, 0), atomic_exchange(&g_ps.hc_over, 0),
                    atomic_exchange(&g_ps.hc_wd, 0),
                    (atomic_load(&F.cvr) & 0x80) ? "yes" : "no",
                    atomic_exchange(&g_ps.tx_w, 0), atomic_exchange(&g_ps.tx_max, 0),
                    atomic_exchange(&g_ps.tx_drop, 0), atomic_exchange(&g_ps.rx_s, 0),
                    atomic_exchange(&g_ps.rx_max, 0), p - polls0);
            polls0 = p;
        }
    }
}

static void *engine(void *arg)
{
    (void)arg;
#ifdef __linux__
    /* Core 1 is where every normal-class helper lands (mp3 length probes,
     * web, audio feeders ...). The DSP must not wait behind them: when it
     * stands still while the 68k waits on it, BOR's VBL overruns and
     * DSPMOD's next question gets a 0 answer (see clock_due). Still
     * SCHED_OTHER - only a larger share of the core. */
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -10) != 0)
        fprintf(stderr, "[FALCON] engine: could not raise priority (nice -10)\n");
#endif
    double last_hz = 0.0;
    uint64_t stat_ns = 0, busy_ns = 0, stat_busy = 0;
    unsigned stat_under = 0, stat_resync = 0, stat_hiccup = 0, stat_skip = 0;
    uint32_t stat_cut = 0;
    while (!atomic_load(&F.stop)) {
        if (!atomic_load(&F.armed)) {
            last_hz = 0.0;
            usleep(5000);
            continue;
        }
        dsp_power_state();
        dtr_tick();
        if (prof_on())
            prof_report();
        int st = atomic_load(&F.dsp_state);
        /* PISTORM_FALCON_DEBUG=1, while a DSP program runs: what moved in the last 5 s.
         * A 68k stuck waiting on the DSP shows as polls with no rx. */
        {
            static uint64_t hb_ns;
            static uint32_t txh0, rxh0, ans0, polls0, ta0, flips0;
            static uint64_t idle0;
            uint64_t now = eng_now_ns();
            if (!hb_ns)
                hb_ns = now;
            if (falcon_dbg() && st == DSP_RUN && now - hb_ns >= 5000000000ull) {
                uint32_t txh = atomic_load(&F.tx.head), rxh = atomic_load(&F.rx.head);
                uint32_t ans = F.answers, polls = atomic_load(&F.isr_polls);
                uint32_t ta = atomic_load(&F.ta_events);
                uint64_t idle = F.dsp->idle_skips;
                uint32_t flips = psvidel_info(7);
                fprintf(stderr, "[FALCON] 5 s: 68k->DSP %u words (%u unread), DSP->68k %u words "
                        "(%u answers, %u unread), ISR polls %u, Timer A events %u (%u held back), "
                        "play %s, DSP pc $%04X idle-skips %llu, %s, screen flips %u, "
                        "empty RX reads %u (max wait %u us, %u timed out, %u DSP parked, %u cut short), "
                        "DSP settles %u (max %u us, %u cut off)\n",
                        txh - txh0, hf_count(&F.tx), rxh - rxh0, ans - ans0,
                        hf_count(&F.rx), polls - polls0, ta - ta0,
                        atomic_exchange(&F.play_holds, 0),
                        atomic_load(&F.play_on) ? "on" : "off", F.dsp->pc,
                        (unsigned long long)(idle - idle0),
                        F.vbl_locked ? "lockstep" : "wall clock", flips - flips0,
                        atomic_exchange(&g_rx_waits, 0), atomic_exchange(&g_rx_wait_max_us, 0),
                        atomic_exchange(&g_rx_wait_timeouts, 0),
                        atomic_exchange(&g_rx_wait_parked, 0),
                        atomic_exchange(&g_rx_wait_short, 0),
                        atomic_exchange(&g_settles, 0), atomic_exchange(&g_settle_max_us, 0),
                        atomic_exchange(&g_settle_cutoffs, 0));
                {
                    /* where a Timer A goes: raised (above) -> pending in the
                     * MFP shadow -> latched for the CPU (g_irq) -> IACKed */
                    extern volatile uint8_t g_irq, g_irq_mask;
                    static uint32_t iack0;
                    uint16_t ier, imr, pend, isr;
                    uint8_t vr;
                    mfp_hub_snapshot(&ier, &imr, &pend, &isr, &vr);
                    uint32_t iack = mfp_hub_iacks(13);
                    fprintf(stderr, "[FALCON] 5 s: Timer A IACKed %u; MFP shadow IER %04X IMR %04X "
                            "pending %04X in-service %04X VR %02X; CPU latch %u mask %u; "
                            "DMA $%06X in $%06X-$%06X%s, %.0f Hz; same-thread DSP %s "
                            "(inline %d resetting %d held %d CRB %04X locked %d, stale resets cleared %u)\n",
                            iack - iack0, ier, imr, pend, isr, vr, g_irq, g_irq_mask,
                            F.play_cnt, F.play_fstart, F.play_fend,
                            F.play_hold ? " (frame end held)" : "", F.frame_hz,
                            cpu_coproc() ? "ON" : "off", g_dsp_inline,
                            atomic_load(&g_dsp_resetting), atomic_load(&F.reset_held),
                            (unsigned)F.crb, F.vbl_locked, g_resetting_cleared);
                    iack0 = iack;
                }
                txh0 = txh; rxh0 = rxh; ans0 = ans; polls0 = polls; ta0 = ta; idle0 = idle;
                flips0 = flips;
                hb_ns = now;
            }
        }
        if (st == DSP_BOOT) {
            dsp_boot_drain();
            if (atomic_load(&F.dsp_state) == DSP_BOOT)
                falcon_audio_wait(200);
            continue;
        }
        if (!matrix_active()) {
            out_flush();
            last_hz = 0.0;
            /* lockstep ends with the sound: a hold on the 68k's words that
             * outlived the DMA left the DSP deaf and the 68k waiting on
             * it for good (ACE Tracker after a DSP_Overload stop) */
            F.vbl_locked = 0;
            g_tx_vis_on = 0;
            if (st == DSP_RUN && cpu_coproc()) {
                /* The 68k thread owns the DSP and clocks it through the
                 * host port - but only while it keeps touching the port.
                 * A 68k that writes its words and then goes quiet (TOS 4's
                 * Dsp_DoBlock at boot: 152 words of its resident DSP code,
                 * one TXDE check, no reads after) left them in the FIFO and
                 * the DSP parked at P:$004E for good: TOS's handlers at
                 * P:$7EA9 never arrived, and the next Dsp_ExecProg jumped
                 * into empty memory (DSPBench hung, DSP pc $EABD). So when
                 * the port has been quiet for a whole sleep and the DSP has
                 * input it has not taken (a word or a host command), the
                 * engine gives it its 1 ms of cycles - under the same mutex
                 * as an inline run, so the 68k's next access cannot race
                 * it - and the 68k's next read settles afresh. */
                /* Likewise a program the 68k started and then left alone
                 * must run to its first wait, as a free-running 56001
                 * would: DSPBench boots its own loader by hand (PSG reset,
                 * 512 words, no reads after) whose first act is copying
                 * its handlers to P:$7EA9 - over the X:$3EA9 its memory
                 * march wrote. Not run, the next Dsp_ExecProg jumped into
                 * the march pattern (illegal instructions, RX/TX tests
                 * dead). So also run while the DSP is neither settled by
                 * the 68k nor seen parked by the last engine run. */
                static uint32_t seen_acc;
                static int eng_parked;
                uint32_t acc = g_host_acc;
                if (acc != seen_acc)
                    eng_parked = 0;
                if (acc == seen_acc &&
                    (hf_count(&F.tx) || (atomic_load(&F.cvr) & 0x80) ||
                     (!g_settled && !eng_parked))) {
                    pthread_mutex_lock(&g_dsp_mtx);
                    uint64_t sk = F.dsp->idle_skips;
                    host_irqs();
                    dsp56k_run(F.dsp, (uint32_t)(DSP_HZ / 1000.0));
                    dsp_illegal_dump();
                    eng_parked = F.dsp->idle_skips != sk ||
                                 (F.dsp->halted && !F.dsp->irq_pending);
                    pthread_mutex_unlock(&g_dsp_mtx);
                    g_settled = 0;
                }
                seen_acc = acc;
                falcon_audio_wait(2000);
            } else if (st == DSP_RUN) {
                /* nothing clocks the DSP: run it at its own speed, 1 ms
                 * at a time, sooner when the host has sent something; while
                 * the 68k is actively exchanging, run flat out (no sleep)
                 * so the Timer A ISR's round-trips cost DSP cycles, not
                 * thread wake-ups */
                watchdog();
                if (host_hot()) {
                    dsp_run(1024);
                } else {
                    dsp_run((uint32_t)(DSP_HZ / 1000.0));
                    if (!host_pending())
                        falcon_audio_wait(1000);
                }
            } else {
                falcon_audio_wait(2000);
            }
            continue;
        }
        double hz = master_rate();
        if (hz != last_hz) {
            out_flush();
            last_hz = hz;
            F.frame_hz = hz;
            F.cyc_per_frame = DSP_HZ / hz;
            F.cyc_acc = 0.0;
            falcon_audio_rate((unsigned)(hz + 0.5));
            clock_restart(hz);
            stat_ns = 0;               /* new baseline for the stats */
        }
        if (st == DSP_RUN)
            watchdog();
        /* A DSP off the SSI is a coprocessor, not the sample source: its
         * time is nobody's sample count, so while the 68k waits on it
         * (ACE Tracker's Timer A handler: send, poll, receive) it runs at
         * full speed on top of its per-frame share, and this thread does
         * not sleep - a real 56001 answers in microseconds. */
        int coproc = st == DSP_RUN && !F.vbl_locked && !(F.crb & 0x3000);
        /* With inline execution the 68k thread runs the coprocessor DSP
         * itself; the engine only plays the DMA buffer here and must not
         * clock the DSP. Without it (PISTORM_DSP_INLINE=0) the engine spins
         * for the whole hot window - between two host round-trips
         * host_waiting() goes false for a moment, and sleeping there is the
         * latency that made ACE's Timer A ISR overrun. */
        int coproc_hot = coproc && !cpu_coproc() && (host_waiting() || host_hot());
        if (coproc_hot)
            dsp_run(1024);
        int64_t due = clock_due(hz);
        if (due <= 0 && coproc_hot)
            continue;
        if (due <= 0 && F.vbl_locked && st == DSP_RUN &&
            (!F.dsp_idle || host_waiting())) {
            /* Lockstep, this VBL's sample periods all played: the DSP gets
             * instruction cycles without sample periods until it is idle
             * (polling a peripheral in place) - so it reads the 68k's data
             * and mixes as soon as the data is there, and is waiting at
             * its request loop when the next question comes, as a real
             * 56001 would be. Stopping as soon as the 68k stopped writing
             * left the mix half done: the next question then waited for
             * it (~9 ms a VBL, and one VBL in five overran). Nothing is
             * borrowed - later SSI slots keep their full share. */
            uint64_t sk = F.dsp->idle_skips;
            dsp_run(4096);
            F.dsp_idle = F.dsp->idle_skips != sk;
            continue;
        }
        if (due <= 0) {
            /* Nothing due: the DSP waits with the sample clock. It is
             * not run ahead to answer the host sooner: DSP time banked
             * ahead of the wall clock is missing from the next VBL's
             * interval, and DSPMOD's answer for that VBL shrinks towards
             * the fatal 0 (see clock_due). The 68k's polling wakes this
             * thread instead (falcon_audio_kick in host_read8), so an
             * answer waits a sample period or two, not a sleep. */
            out_flush();
            falcon_audio_wait(500);
            continue;
        }
        unsigned n = due > (int64_t)(4 * ENGINE_CHUNK) ? 4 * ENGINE_CHUNK
                                                       : (unsigned)due;
        uint64_t t_run = eng_now_ns();
        if (F.vbl_locked) {
            /* A VBL's worth, all of it, before the DSP sees anything the
             * 68k sends from now on: a request made during this VBL is
             * answered from the end of it, so each answer is one VBL's
             * samples however early or late in its VBL the 68k asked. */
            if (!g_tx_vis_on) {               /* else: held since the answer */
                g_tx_vis_head = atomic_load(&F.tx.head);
                g_tx_vis_on = 1;
            }
            n = (unsigned)due;
            for (unsigned i = 0; i < n && !atomic_load(&F.stop); i++) {
                frame();
                if ((i & 511) == 511)
                    out_flush();
            }
            g_tx_vis_on = 0;
            host_irqs();
            F.dsp_idle = 0;                   /* the held words are in */
        } else {
            /* Behind the wall clock (more than ~2 ms of frames due): the
             * DSP gets the Falcon's own 32 MHz per frame, not the turbo
             * share. A program that keeps the DSP busy (FalcAMP decoding
             * MP3 at nearly a real 56001's full load) otherwise asks the Pi
             * for twice a real DSP's cycles per sample while it decodes;
             * the Pi could not keep up, the frames came out late and the
             * device ran dry in bursts (the "Dalek" warble). Turbo is a
             * bonus for programs with time to spare, never more than the
             * clock allows. */
            double cpf = F.cyc_per_frame;
            double real = DSP_HZ_REAL / F.frame_hz;
            if (due > (int64_t)(2 * ENGINE_CHUNK) && cpf > real) {
                F.cyc_per_frame = real;
                g_turbo_cut++;
            }
            for (unsigned i = 0; i < n && !atomic_load(&F.stop); i++)
                frame();
            F.cyc_per_frame = cpf;
        }
        F.emitted += n;
        out_flush();

        uint64_t now = eng_now_ns();
        busy_ns += now - t_run;
        if (now - stat_ns > 10000000000ull) {
            unsigned u = falcon_audio_underruns(), r = F.resyncs, h = F.hiccups;
            unsigned sk = F.vbl_skipped;
            {
                uint32_t gap = atomic_exchange(&g_vbl_maxgap_us, 0);
                uint32_t late = atomic_exchange(&g_vbl_late, 0);
                uint32_t wmax = atomic_exchange(&g_wait_max_us, 0);
                uint32_t wsum = atomic_exchange(&g_wait_sum_us, 0);
                uint32_t wn = atomic_exchange(&g_wait_n, 0);
                uint32_t pm = atomic_exchange(&g_pick_max_us, 0);
                uint32_t ps = atomic_exchange(&g_pick_sum_us, 0);
                uint32_t pn = atomic_exchange(&g_pick_n, 0);
                uint32_t am = atomic_exchange(&g_ans_max_us, 0);
                uint32_t as = atomic_exchange(&g_ans_sum_us, 0);
                uint32_t an = atomic_exchange(&g_ans_n, 0);
                /* PISTORM_FALCON_STATS=1: the 68k timing line every 10 s
                 * (the counters above are reset either way) */
                static int stats_env = -1;
                if (stats_env < 0) {
                    const char *e = getenv("PISTORM_FALCON_STATS");
                    stats_env = (e && *e == '1');
                }
                if (stats_env && stat_ns && wn)
                    fprintf(stderr, "[FALCON] 68k: VBL gap max %.1f ms, %u VBLs later than "
                            "25 ms; waits on the DSP avg %.2f ms max %.2f ms (%u); "
                            "request picked up by the DSP after avg %.2f max %.2f ms, answered after avg %.2f max %.2f ms\n",
                            gap / 1000.0, late, wsum / 1000.0 / wn, wmax / 1000.0, wn,
                            pn ? ps / 1000.0 / pn : 0.0, pm / 1000.0,
                            an ? as / 1000.0 / an : 0.0, am / 1000.0);
            }
            if (stat_ns && (u != stat_under || r != stat_resync || h != stat_hiccup ||
                            sk != stat_skip))
                fprintf(stderr, "[FALCON] %.0f Hz (%s): %u device underruns, %u clock resyncs, "
                        "%u engine hiccups, %u answers merged in 10 s (ring %u frames, "
                        "average %.0f, DSP load %.0f%%, %u batches at real DSP speed)\n", hz,
                        F.vbl_locked ? "lockstep with the 68k" : "wall clock",
                        u - stat_under, r - stat_resync, h - stat_hiccup, sk - stat_skip,
                        falcon_audio_fill(), F.fill_avg,
                        100.0 * (double)(busy_ns - stat_busy) / (double)(now - stat_ns),
                        g_turbo_cut - stat_cut);
            stat_cut = g_turbo_cut;
            stat_busy = busy_ns;
            stat_ns = now;
            stat_under = u;
            stat_resync = r;
            stat_hiccup = h;
            stat_skip = sk;
        }
    }
    return NULL;
}

/* Synchronous step for test harnesses: advance the Falcon side by
 * `frames` sample periods (or the equivalent DSP time when no path is
 * clocked), without the engine thread or any pacing. */
void falcon_step(unsigned frames)
{
    dsp_power_state();
    int st = atomic_load(&F.dsp_state);
    if (st == DSP_BOOT) {
        dsp_boot_drain();
        st = atomic_load(&F.dsp_state);
        if (st != DSP_RUN)
            return;
    }
    if (!matrix_active()) {
        if (st == DSP_RUN)
            dsp_run((uint32_t)(frames * (DSP_HZ / 49170.0)));
        return;
    }
    double hz = master_rate();
    F.frame_hz = hz;
    F.cyc_per_frame = DSP_HZ / hz;
    for (unsigned i = 0; i < frames; i++)
        frame();
    out_flush();
}

/* ------------------------------------------------------------------ */
/* 68k side (CPU thread)                                               */
/* ------------------------------------------------------------------ */
int falcon_hw_owns(uint32_t a)
{
    if (!atomic_load_explicit(&F.armed, memory_order_relaxed))
        return 0;
    return (a >= 0xFFA200u && a <= 0xFFA207u) ||
           (a >= 0xFF8900u && a <  0xFF8944u);
}

/* RX read with nothing there. A 56001 refills HTX within a few of its
 * cycles, so software reads the port back to back without looking at
 * RXDF - ACE Tracker takes each mixed block with a run of move.w
 * $FFFFA206,(a0)+ - and the 68k, slower than the DSP, never sees it
 * empty. Here the DSP runs on another core and its words arrive when
 * that thread runs, so a read that outran it got the previous word again
 * and ACE's block came back garbled (error, DSP reloaded every buffer).
 * Wait for the word, as the hardware's speed did: the engine is kicked
 * and runs the DSP flat out while g_rx_starved is up. A DSP that is not
 * producing (idle, waiting for the 68k) makes this cost a millisecond,
 * once per such read - software that checks RXDF never comes here. */
/* 50 ms: a read must not give up and hand the 68k the previous word -
 * that put a stale word where ACE expected "OK!"/"SDS" and broke its
 * protocol for good (DSP Overload, codes $10/$13). The DSP always
 * answers in time on the hardware; here it may be a core away. */
#define RX_WAIT_NS 50000000ull
/* A read with no question out (the 68k has not written a word since its
 * last read) is waited for in full only
 * while such waits get answered. ACE's DSP sends its blocks unasked and
 * every wait ends with a word (75000 waits in the harness, none missed).
 * FalcAMP's flush read never gets one - its DSP answers host commands
 * only - and 37 x 50 ms of 68k stalls per 5 s made the MP3 stutter. After
 * 4 such misses in a row the wait is cut to 2 ms; the first word that
 * arrives in one restores the full wait. */
#define RX_WAIT_SHORT_NS 2000000ull
static int g_wrote_since_rx;          /* CPU thread: question out to the DSP */
static uint64_t g_last_rx_frame;
static unsigned g_noreq_misses;       /* CPU thread: unanswered such waits in a row */
static int rx_wait(uint32_t *v)
{
    if (atomic_load(&F.dsp_state) != DSP_RUN || atomic_load(&F.reset_held))
        return 0;
    atomic_store(&g_rx_starved, 1);
    falcon_audio_kick();
    uint64_t t0 = mono_ns(), t;
    uint64_t idle0 = F.dsp->idle_skips;
    int noreq = !g_wrote_since_rx;
    uint64_t budget = RX_WAIT_NS;
    if (noreq && g_noreq_misses >= 4) {
        budget = RX_WAIT_SHORT_NS;
        atomic_fetch_add_explicit(&g_rx_wait_short, 1, memory_order_relaxed);
    }
    int have;
    while (!(have = hf_peek(&F.rx, v))) {
        /* The DSP has parked twice (a polling or "wait for an interrupt"
         * loop) without sending a word: no word is coming, and the
         * hardware would have handed back the old one at once. FalcAMP
         * reads the port as a flush from a Timer B interrupt while its DSP
         * spins on a flag; each read cost the full 50 ms here, its next
         * host command came 150 ms late and the DSP's buffer overran its
         * code. ACE's waits end with a word before the DSP parks. */
        if (F.dsp->idle_skips - idle0 >= 2) {
            /* it may have parked on a full HTX after sending (the word
             * landed between the peek above and here): look once more */
            if ((have = hf_peek(&F.rx, v)))
                break;
            atomic_fetch_add_explicit(&g_rx_wait_parked, 1, memory_order_relaxed);
            break;
        }
        t = mono_ns();
        if (t - t0 > budget) {
            atomic_fetch_add_explicit(&g_rx_wait_timeouts, 1, memory_order_relaxed);
            break;
        }
        if (((t - t0) & 0xFFF) == 0)
            falcon_audio_kick();
    }
    atomic_store(&g_rx_starved, 0);
    if (noreq)
        g_noreq_misses = have ? 0 : g_noreq_misses + (g_noreq_misses < 255);
    uint32_t us = (uint32_t)((mono_ns() - t0) / 1000u);
    atomic_fetch_add_explicit(&g_rx_waits, 1, memory_order_relaxed);
    atomic_max_u32(&g_rx_wait_max_us, us);
    if (g_tr_size) {
        /* x: 1 a word came, 2 the DSP parked, 3 timed out; +16 = short wait */
        int how = have ? 1 : (F.dsp->idle_skips - idle0 >= 2 ? 2 : 3);
        dtr('w', us, (unsigned)how | (budget == RX_WAIT_SHORT_NS ? 16u : 0u));
        atomic_fetch_add(&g_ps.rxw_n, 1);
        atomic_fetch_add(&g_ps.rxw_sum_us, us);
        ps_max(&g_ps.rxw_max_us, us);
        atomic_fetch_add(how == 1 ? &g_ps.rxw_got : how == 2 ? &g_ps.rxw_parked : &g_ps.rxw_timeout, 1);
        if (budget == RX_WAIT_SHORT_NS)
            atomic_fetch_add(&g_ps.rxw_short, 1);
    }
    return have;
}

/* Coprocessor mode, Hatari's way. Hatari runs the DSP on the CPU's own
 * thread, interleaved with it, so whenever the 68k looks at the host port
 * the DSP has already done everything it could do up to that moment. ACE
 * Tracker depends on that (it breaks on a real Falcon with a CPU
 * accelerator, i.e. when the 68k outruns the DSP): it writes words without
 * waiting for TXDE and reads blocks back with no RXDF check.
 *
 * The PiStorm 68k has no cycle count to interleave against, so the DSP is
 * made "infinitely fast" instead: on every host-port access by the 68k (on
 * the 68k's thread) the DSP runs until it can make no further progress
 * without the 68k - parked at a polling loop (jclr #n,x:<<$ffe9,*), a
 * "jmp *" or a WAIT for an interrupt, or blocked on a full HTX (HTDE).
 * The port then shows exactly what a 56001 that never lags would show. Once
 * settled it stays settled until the 68k changes something the DSP can
 * see (writes a word/command/flag, reads a word), so a 68k spinning on the
 * ISR costs nothing. The DSP's state is invisible to the 68k between
 * accesses (off the SSI, not on the DAC), so running it lazily here is the
 * same as running it all the time.
 *
 * A DSP that computes without parking anywhere is cut off after
 * SETTLE_QUIET cycles with nothing moving on the port (not settled: the
 * next access gives it another slice), and a settle never runs longer than
 * SETTLE_MAX cycles, so a runaway DSP program cannot hang the 68k. */
#define SETTLE_QUIET  (1u << 20)     /* ~16 ms of a real 64 MHz 56001     */
#define SETTLE_MAX    (1u << 22)

static void dsp_settle(void)
{
    if (!cpu_coproc()) {
        g_settled = 0;               /* the engine (or a reset) has it    */
        return;
    }
    if (g_settled)
        return;
    const int dbg = falcon_dbg();            /* settle stats are debug-only: */
    const int tr = g_tr_size != 0;           /* (or with the trace on):       */
    uint64_t t0 = dbg || tr ? mono_ns() : 0; /* two clock reads per word read */
    int how = 3;                             /* trace: 1 parked 2 cut off 3 stopped */
    dsp56k_t *d = F.dsp;
    uint32_t quiet = 0, total = 0;
    const int prof = prof_on();
    uint64_t pt0 = prof ? prof_ticks() : 0;
    pthread_mutex_lock(&g_dsp_mtx);
    for (;;) {
        uint32_t rxh = atomic_load_explicit(&F.rx.head, memory_order_relaxed);
        uint32_t txt = atomic_load_explicit(&F.tx.tail, memory_order_relaxed);
        uint8_t  cvr = atomic_load_explicit(&F.cvr, memory_order_relaxed);
        uint64_t sk = d->idle_skips;
        host_irqs();
        dsp56k_run(d, 64);
        dsp_illegal_dump();
        total += 64;
        int moved = rxh != atomic_load_explicit(&F.rx.head, memory_order_relaxed) ||
                    txt != atomic_load_explicit(&F.tx.tail, memory_order_relaxed) ||
                    cvr != atomic_load_explicit(&F.cvr, memory_order_relaxed);
        /* Parked (a run ends at the first idle poll): waiting on the 68k.
         * Moving first and parking after, in the same run, is settled
         * too - nothing the DSP waits on can change while the 68k is in
         * here - so a word read costs one run, not a second one to see
         * it park. */
        if (d->idle_skips != sk || (d->halted && !d->irq_pending)) {
            g_settled = 1;
            how = 1;
            break;
        }
        (void)moved;
        if (atomic_load_explicit(&F.dsp_state, memory_order_relaxed) != DSP_RUN)
            break;                   /* illegal instruction etc.          */
        quiet = moved ? 0 : quiet + 64;
        if (quiet >= SETTLE_QUIET || total >= SETTLE_MAX) {
            atomic_fetch_add_explicit(&g_settle_cutoffs, 1, memory_order_relaxed);
            how = 2;
            break;
        }
    }
    pthread_mutex_unlock(&g_dsp_mtx);
    if (prof)
        atomic_fetch_add_explicit(&g_prof_dsp_t, prof_ticks() - pt0, memory_order_relaxed);
    if (dbg) {
        atomic_fetch_add_explicit(&g_settles, 1, memory_order_relaxed);
        atomic_max_u32(&g_settle_max_us, (uint32_t)((mono_ns() - t0) / 1000u));
    }
    if (tr) {
        uint32_t us = (uint32_t)((mono_ns() - t0) / 1000u);
        dtr('s', total, (unsigned)how);
        atomic_fetch_add(&g_ps.st_n, 1);
        atomic_fetch_add(&g_ps.st_sum_us, us);
        ps_max(&g_ps.st_max_us, us);
        if (how == 1) atomic_fetch_add(&g_ps.st_parked, 1);
        if (how == 2) atomic_fetch_add(&g_ps.st_cut, 1);
    }
}

/* the 68k changed something the DSP can see: it must run again */
static int g_rx_lazy;
static inline void dsp_unsettle(void) { g_settled = 0; g_rx_lazy = 0; }

/* One 68k access to the host port (falcon_hw_read): whether the DSP is the
 * 68k's to run, decided once per access, not once per byte - a long read
 * at $FFA204 was four cpu_coproc() checks and four settles. */
static int g_acc_coproc;
static int g_acc_settled;     /* this access has brought the DSP up to now */
static int g_acc_waited;      /* this access already waited for a word     */
/* An empty-port read is worth waiting for (rx_wait) only when a word is
 * on its way: the 68k has sent the DSP a word it has not answered yet, or
 * words are flowing (one was read within the last ~2 ms of sound frames).
 * Otherwise the hardware hands back the previous word at once. FalcAMP
 * flushes the port with a long read from its Timer B interrupt while the
 * DSP waits for its next block; that read cost 3 x 50 ms here, its host
 * command came ~150 ms late and the DSP's receive buffer ran over its
 * code. Without the sound engine's frame clock it waits as before. */
static int matrix_active(void);
static inline int rx_worth_waiting(void)
{
    return g_wrote_since_rx || !matrix_active() ||
           F.frames - g_last_rx_frame < 100u;
}
/* g_rx_lazy (see dsp_unsettle): words the DSP sent are still unread. The
 * 68k's data reads only take them in order and cannot see the DSP's
 * progress, so a word read does not make the DSP run again until the
 * receive FIFO is empty or the 68k looks at the status/control side
 * (ICR/CVR/ISR/IVR), which it can see. */

static uint8_t host_read8(uint32_t o)
{
    uint32_t v;
    int have = hf_peek(&F.rx, &v);
    if (atomic_load_explicit(&g_dsp_resetting, memory_order_relaxed)) {
        have = 0;                            /* no RXDF, no data during reset */
        v = F.rx_last;
    }
    /* Coprocessor: bring the DSP up to "now" before the 68k sees the port
     * (dsp_settle). Otherwise (engine-clocked DSP) a receive read that
     * outran the engine waits for the word. */
    if (g_acc_coproc) {
        if (o < 4 && g_rx_lazy) {
            g_rx_lazy = 0;
            dsp_unsettle();
            g_acc_settled = 0;
        }
        if (!g_acc_settled) {
            dsp_settle();
            g_acc_settled = 1;
        }
        have = hf_peek(&F.rx, &v);
        if (atomic_load_explicit(&g_dsp_resetting, memory_order_relaxed))
            have = 0;
    } else if (!have && o >= 5 && o <= 7 && !g_acc_waited) {
        g_acc_waited = 1;                    /* once per access, not per byte */
        if (rx_worth_waiting())
            have = rx_wait(&v);
    }
    if (!have)
        v = F.rx_last;
    switch (o) {
    case 0: return atomic_load(&F.icr);
    case 1: return atomic_load(&F.cvr);
    case 2: {
        uint8_t isr = 0;
        uint32_t txn = hf_count(&F.tx);
        bump(&F.isr_polls);
        if (!have) {
            falcon_audio_kick();                 /* waiting on the DSP */
            if (!g_req_ns)
                g_req_ns = mono_ns();
        }
        if (have) isr |= 0x01;                               /* RXDF */
        if (tx_empty(txn)) isr |= 0x02;                      /* TXDE */
        if (txn == 0) isr |= 0x04;                           /* TRDY */
        isr |= (uint8_t)(((atomic_load(&F.hcr) >> 3) & 3) << 3); /* HF2 HF3 */
        uint8_t icr = atomic_load(&F.icr);
        if (((icr & 1) && have) || ((icr & 2) && tx_empty(txn)))
            isr |= 0x80;                                     /* HREQ */
        if (g_tr_size) {
            static int last = -1;                /* CPU thread only */
            if (isr != last)
                dtr('P', isr, txn);
            last = isr;
        }
        return isr;
    }
    case 3: return F.ivr;
    case 5: return (uint8_t)(v >> 16);
    case 6: return (uint8_t)(v >> 8);
    case 7:
        if (g_tr_size) {
            if (have) {
                dtr('R', v, hf_count(&F.rx));
            } else {
                dtr('r', v, 0);
                atomic_fetch_add(&g_ps.rx_stale, 1);
            }
        }
        if (have) {
            hf_pop(&F.rx);
            g_wrote_since_rx = 0;
            g_last_rx_frame = F.frames;
            /* HTDE may have opened. With words still queued the DSP
             * only needs to run when the 68k can see it (g_rx_lazy) */
            if (g_acc_coproc && hf_count(&F.rx))
                g_rx_lazy = 1;
            else
                dsp_unsettle();
            hlog('R', v, 0);
            F.rx_last = v;
            if (g_req_ns) {
                uint32_t us = (uint32_t)((mono_ns() - g_req_ns) / 1000u);
                atomic_max_u32(&g_wait_max_us, us);
                atomic_fetch_add_explicit(&g_wait_sum_us, us, memory_order_relaxed);
                atomic_fetch_add_explicit(&g_wait_n, 1, memory_order_relaxed);
                g_req_ns = 0;
            }
        }
        return (uint8_t)v;
    }
    return 0;
}

#define TX_SETTLE_AT 1024u   /* see host_write8: the DSP keeps up with a blast */
static void host_write8(uint32_t o, uint8_t v)
{
    switch (o) {
    case 0:
        dtr('I', v, atomic_load(&F.icr));
        if (v & 0x80) {                                      /* INIT */
            if (v & 0x02) atomic_store(&F.tx.head, atomic_load(&F.tx.tail));
            if (v & 0x01) atomic_store(&F.rx.tail, atomic_load(&F.rx.head));
            v &= 0x7F;
        }
        atomic_store(&F.icr, v);
        break;
    case 1:
        if (g_tr_size) {
            uint8_t old = atomic_load(&F.cvr);
            dtr('C', v, old);
            if (v & 0x80) {
                atomic_fetch_add(&g_ps.hc_req, 1);
                if (old & 0x80)
                    atomic_fetch_add(&g_ps.hc_over, 1);    /* rewritten while pending */
                atomic_store(&g_hc_req_ns, dtr_now());
            } else if (old & 0x80) {
                atomic_fetch_add(&g_ps.hc_wd, 1);          /* withdrawn (HC = 0) */
            }
        }
        atomic_store(&F.cvr, (uint8_t)(v & 0x9F));
        if ((v & 0x80) && falcon_dbg()) {               /* host command timeline */
            static unsigned said;
            if (said++ < 60)
                fprintf(stderr, "[FALCON] HC $%02X requested: frame %llu, DSP pc $%04X r3 $%04X sr $%04X\n",
                        (v & 0x1F) * 2, (unsigned long long)F.frames, F.dsp->pc, F.dsp->r[3], F.dsp->sr);
        }
        break;
    case 3: dtr('V', v, F.ivr); F.ivr = v; break;
    case 5: F.txb[0] = v; break;
    case 6: F.txb[1] = v; break;
    case 7:
        F.txb[2] = v;
        g_wrote_since_rx = 1;                /* a word the DSP may answer */
        g_req_ns = 0;                        /* still talking, not waiting */
        {
            uint32_t n = bump(&g_txlog_n);
            g_txlog[n % TXLOG] = ((uint32_t)F.txb[0] << 16) | ((uint32_t)F.txb[1] << 8) | v;
        }
        /* pick-up latency stats (debug): a clock read per word otherwise */
        if (falcon_dbg() && !hf_count(&F.tx) &&
            !atomic_load_explicit(&g_tx_idle_ns, memory_order_relaxed))
            atomic_store(&g_tx_idle_ns, mono_ns());
        hlog('W', ((uint32_t)F.txb[0] << 16) | ((uint32_t)F.txb[1] << 8) | v, 0);
        /* Coprocessor, 68k blasting words without a handshake (DSPBench's
         * raw write rate: 4M longs straight at $FFA204, no TXDE poll). A
         * real 56001 empties HTX faster than a 68030 can fill it, and
         * DSPBench counts the words. Here the DSP only runs when the 68k
         * reads, so the FIFO filled and words were dropped ("DSP lost
         * sync with host"). Let it catch up every TX_SETTLE_AT words: one
         * settle per burst chunk, so ordinary bursts stay cheap. */
        if (hf_count(&F.tx) >= TX_SETTLE_AT && cpu_coproc()) {
            dsp_unsettle();
            dsp_settle();
        }
        if (g_tr_size) {
            uint32_t q = hf_count(&F.tx);
            dtr('W', ((uint32_t)F.txb[0] << 16) | ((uint32_t)F.txb[1] << 8) | v, q);
            atomic_fetch_add(&g_ps.tx_w, 1);
            ps_max(&g_ps.tx_max, q + 1);
            if (q >= HF_SIZE) {
                dtr('D', ((uint32_t)F.txb[0] << 16) | ((uint32_t)F.txb[1] << 8) | v, 0);
                atomic_fetch_add(&g_ps.tx_drop, 1);
            }
        }
        if (!hf_push(&F.tx, ((uint32_t)F.txb[0] << 16) | ((uint32_t)F.txb[1] << 8) | v)) {
            if (F.dropped_tx++ < 4)
                fprintf(stderr, "[FALCON] host port overflow: 68k word dropped "
                        "(DSP %u words behind)\n", hf_count(&F.tx));
        }
        break;
    default: return;
    }
    /* the DSP has something new to act on. Coprocessor: it catches up at
     * the 68k's next read, so a burst of writes costs nothing extra -
     * unless the 68k has a host request enabled (ICR RREQ/TREQ). Then it
     * sees the DSP through HREQ, a level-6 interrupt, without reading the
     * port at all: DSPBench's RX test re-enables RREQ in its handler and
     * waits in a RAM loop for the next interrupt, which only comes once
     * the DSP has taken the word just sent and answered. So run the DSP
     * to its next wait now. Otherwise wake the engine. */
    dsp_unsettle();
    if (!cpu_coproc())
        falcon_audio_kick();
    else if (atomic_load_explicit(&F.icr, memory_order_relaxed) & 3)
        dsp_settle();
}

static uint8_t snd_read8(uint32_t o)
{
    int rec = F.reg[0x01] & 0x80;
    uint32_t start = rec ? F.rec_start : F.play_start;
    uint32_t end   = rec ? F.rec_end : F.play_end;
    uint32_t cnt   = rec ? F.rec_cnt : F.play_cnt;
    switch (o) {
    case 0x01:
        return (uint8_t)((F.reg[0x01] & 0x80) |
                         (atomic_load(&F.play_on) ? 1 : 0) | (F.play_rep ? 2 : 0) |
                         (atomic_load(&F.rec_on) ? 0x10 : 0) | (F.rec_rep ? 0x20 : 0));
    case 0x03: return (uint8_t)(start >> 16);
    case 0x05: return (uint8_t)(start >> 8);
    case 0x07: return (uint8_t)start;
    case 0x09: return (uint8_t)(cnt >> 16);
    case 0x0B: return (uint8_t)(cnt >> 8);
    case 0x0D: return (uint8_t)cnt;
    case 0x0F: return (uint8_t)(end >> 16);
    case 0x11: return (uint8_t)(end >> 8);
    case 0x13: return (uint8_t)end;
    case 0x22: case 0x23: return 0;                          /* microwire idle */
    }
    return o < sizeof F.reg ? F.reg[o] : 0;
}

static void set_addr_byte(uint32_t *a, int shift, uint8_t v)
{
    uint32_t keep = ~(0xFFu << shift);
    if (shift == 16)
        keep &= 0x00FFFFFFu;        /* a register write means a 24-bit address */
    *a = (*a & keep) | ((uint32_t)v << shift);
}

static void snd_write8(uint32_t o, uint8_t v)
{
    int rec = F.reg[0x01] & 0x80;
    switch (o) {
    case 0x01: {
        F.reg[0x01] = (uint8_t)((F.reg[0x01] & 0x7F) | (v & 0x80));
        int pon = v & 1, ron = (v >> 4) & 1;
        F.play_rep = (v >> 1) & 1;
        F.rec_rep = (v >> 5) & 1;
        if (pon != atomic_load(&F.play_on) && falcon_dbg())
            fprintf(stderr, "[FALCON] sound DMA play %s: $%06X-$%06X %s mode $%02X ctrl $%02X (frame %llu, DSP r3 $%04X)\n",
                    pon ? "ON" : "off", F.play_start, F.play_end,
                    pon && F.play_rep ? "repeat" : "once", F.reg[0x21], F.reg[0x00],
                    (unsigned long long)F.frames, F.dsp->r[3]);
        if (pon && !atomic_load(&F.play_on)) {
            F.play_cnt = F.play_fstart = F.play_start;
            F.play_fend = F.play_end;
            F.play_hold = 0;
            F.play_ev_ns = 0;
        }
        if (ron && !atomic_load(&F.rec_on)) { F.rec_cnt = F.rec_start; F.rec_fend = F.rec_end; }
        atomic_store(&F.play_on, pon);
        atomic_store(&F.rec_on, ron);
        falcon_audio_kick();
        return;
    }
    case 0x03: set_addr_byte(rec ? &F.rec_start : &F.play_start, 16, v); return;
    case 0x05: set_addr_byte(rec ? &F.rec_start : &F.play_start, 8, v); return;
    case 0x07: set_addr_byte(rec ? &F.rec_start : &F.play_start, 0, v & 0xFE); return;
    case 0x0F: set_addr_byte(rec ? &F.rec_end : &F.play_end, 16, v); return;
    case 0x11: set_addr_byte(rec ? &F.rec_end : &F.play_end, 8, v); return;
    case 0x13: set_addr_byte(rec ? &F.rec_end : &F.play_end, 0, v & 0xFE); return;
    case 0x09: case 0x0B: case 0x0D: return;                 /* counters: read-only */
    }
    if (o < sizeof F.reg)
        F.reg[o] = v;
}

/* Host-port activity time for the engine's hot window (host_hot). Once
 * per 68k access, not per byte - it was four clock reads per long - and
 * not at all in coprocessor mode, where the engine leaves the DSP alone
 * and never looks at it. */
static inline void host_stamp(uint32_t a)
{
    if (a <= 0xFFA207u && a + 3u >= 0xFFA200u && !cpu_coproc())
        atomic_store_explicit(&g_host_act_ns, mono_ns(), memory_order_relaxed);
}

static uint32_t falcon_hw_read_impl(uint32_t a, int size);
static void falcon_hw_write_impl(uint32_t a, uint32_t v, int size);
uint32_t falcon_hw_read(uint32_t a, int size)
{
    if (!prof_on())
        return falcon_hw_read_impl(a, size);
    uint64_t t = prof_ticks();
    uint32_t v = falcon_hw_read_impl(a, size);
    atomic_fetch_add_explicit(&g_prof_rd_t, prof_ticks() - t, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_prof_rd_n, 1, memory_order_relaxed);
    return v;
}
void falcon_hw_write(uint32_t a, uint32_t v, int size)
{
    if (!prof_on()) {
        falcon_hw_write_impl(a, v, size);
        return;
    }
    uint64_t t = prof_ticks();
    falcon_hw_write_impl(a, v, size);
    atomic_fetch_add_explicit(&g_prof_wr_t, prof_ticks() - t, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_prof_wr_n, 1, memory_order_relaxed);
}

static uint32_t falcon_hw_read_impl(uint32_t a, int size)
{
    g_host_acc++;
    host_stamp(a);
    uint32_t v = 0;
    if (a <= 0xFFA207u && a + (uint32_t)size > 0xFFA200u)
        g_acc_coproc = cpu_coproc(), g_acc_settled = 0, g_acc_waited = 0;
    for (int i = 0; i < size; i++) {
        uint32_t b = a + (uint32_t)i;
        uint8_t x;
        if (b >= 0xFFA200u && b <= 0xFFA207u) x = host_read8(b - 0xFFA200u);
        else if (b >= 0xFF8900u && b < 0xFF8944u) x = snd_read8(b - 0xFF8900u);
        else x = 0;
        v = (v << 8) | x;
    }
    return v;
}

static void falcon_hw_write_impl(uint32_t a, uint32_t v, int size)
{
    g_host_acc++;
    host_stamp(a);
    for (int i = 0; i < size; i++) {
        uint32_t b = a + (uint32_t)i;
        uint8_t x = (uint8_t)(v >> (8 * (size - 1 - i)));
        if (b >= 0xFFA200u && b <= 0xFFA207u) host_write8(b - 0xFFA200u, x);
        else if (b >= 0xFF8900u && b < 0xFF8944u) snd_write8(b - 0xFF8900u, x);
    }
}

static void porta_write(uint8_t v)
{
    uint8_t old = F.porta;
    F.porta = v;
    if ((v & 0x10) && !(old & 0x10)) {
        atomic_store(&F.reset_held, 1);                      /* DSP in reset */
        atomic_store(&g_dsp_resetting, 1);
        g_settled = 0;
    } else if (!(v & 0x10) && (old & 0x10)) {
        /* reset released: whatever the host writes from now on is the
         * bootstrap; older words and replies are gone */
        atomic_store(&F.boot_head, atomic_load(&F.tx.head));
        atomic_store(&F.rx.tail, atomic_load(&F.rx.head));
        atomic_store(&F.reset_held, 0);
        atomic_fetch_add(&F.reset_epoch, 1);
        falcon_audio_kick();
    }
}

void falcon_psg_snoop(uint32_t a, uint32_t v, int size)
{
    if (!atomic_load_explicit(&F.armed, memory_order_relaxed))
        return;
    a &= 0x00FFFFFFu;
    if ((a & 0xFFFF00u) != 0xFF8800u)
        return;
    /* even addresses carry the byte; bit 1 picks select (0) / data (2) */
    for (int i = 0; i < size; i += 2) {
        uint32_t b = a + (uint32_t)i;
        uint8_t x = (uint8_t)(v >> (8 * (size - 1 - i)));
        if (size == 1) x = (uint8_t)v;
        if (b & 1) continue;
        if (!(b & 2)) F.psg_latch = x & 0x0F;
        else if (F.psg_latch == 14) porta_write(x);
    }
}

/* ------------------------------------------------------------------ */
/* The Falcon sound XBIOS (TOS 4 opcodes 128-141), done host-side       */
/* ------------------------------------------------------------------ */
static void reg_or_and(uint32_t o, uint8_t and_mask, uint8_t or_bits)
{
    snd_write8(o, (uint8_t)((snd_read8(o) & and_mask) | or_bits));
}

static void sound_defaults(void)
{
    atomic_store(&F.play_on, 0);
    atomic_store(&F.rec_on, 0);
    F.reg[0x00] = 0;
    F.reg[0x01] = 0;
    F.reg[0x20] = 0; F.reg[0x21] = 0;
    F.reg[0x30] = 0x00; F.reg[0x31] = 0x01;
    F.reg[0x32] = 0x00; F.reg[0x33] = 0x00;
    F.reg[0x34] = 0; F.reg[0x35] = 0;
    F.reg[0x36] = 0; F.reg[0x37] = 0x02;
    F.reg[0x38] = 0; F.reg[0x39] = 0;
    F.reg[0x3A] = 0; F.reg[0x3B] = 0;
}

int32_t falcon_sound_xbios(int op, const int32_t *a, uint32_t *out4)
{
    switch (op) {
    case 104: return falcon_dsp_lock(1);                     /* Dsp_Lock   */
    case 105: return falcon_dsp_lock(0);                     /* Dsp_Unlock */
    case 128: return falcon_snd_lock(1);                     /* locksnd    */
    case 129: return falcon_snd_lock(0);                     /* unlocksnd  */
    case 130: {                                              /* soundcmd   */
        int mode = a[0], data = a[1];
        uint16_t att = rw16(0x3A);
        switch (mode) {
        case 0:                                              /* LTATTEN */
            if (data >= 0) { att = (uint16_t)((att & 0xF0FF) | ((data & 0xF0) << 4)); F.reg[0x3A] = (uint8_t)(att >> 8); F.reg[0x3B] = (uint8_t)att; }
            return (att >> 4) & 0xF0;
        case 1:                                              /* RTATTEN */
            if (data >= 0) { att = (uint16_t)((att & 0xFF0F) | (data & 0xF0)); F.reg[0x3B] = (uint8_t)att; }
            return att & 0xF0;
        case 2:                                              /* LTGAIN */
            if (data >= 0) F.reg[0x39] = (uint8_t)((F.reg[0x39] & 0x0F) | (data & 0xF0));
            return F.reg[0x39] & 0xF0;
        case 3:                                              /* RTGAIN */
            if (data >= 0) F.reg[0x39] = (uint8_t)((F.reg[0x39] & 0xF0) | ((data >> 4) & 0x0F));
            return (F.reg[0x39] << 4) & 0xF0;
        case 4:                                              /* ADDERIN */
            if (data >= 0) F.reg[0x37] = (uint8_t)(data & 3);
            return F.reg[0x37] & 3;
        case 5:                                              /* ADCINPUT */
            if (data >= 0) F.reg[0x38] = (uint8_t)(data & 3);
            return F.reg[0x38] & 3;
        case 6:                                              /* SETPRESCALE */
            if (data >= 0) F.reg[0x21] = (uint8_t)((F.reg[0x21] & 0xFC) | (data & 3));
            return F.reg[0x21] & 3;
        }
        return 0;
    }
    case 131: {                                              /* setbuffer  */
        /* The full 32-bit address, not the chip's 24 bits: a program
         * loaded into TT-RAM (Sonic Falcon, load-to-TT flag set, its
         * stream ring in BSS) hands the XBIOS a TT-RAM buffer. A real
         * Falcon would play garbage from the ST-RAM alias; the DMA here
         * reads TT-RAM fine, so let it. Direct register writes still
         * address 24 bits (set_addr_byte). */
        uint32_t beg = (uint32_t)a[1] & 0xFFFFFFFEu, end = (uint32_t)a[2] & 0xFFFFFFFEu;
        if (a[0]) { F.rec_start = beg; F.rec_end = end; }
        else      { F.play_start = beg; F.play_end = end; }
        return 0;
    }
    case 132:                                                /* setmode    */
        F.reg[0x21] = (uint8_t)((F.reg[0x21] & 0x3F) | ((a[0] & 3) << 6));
        return 0;
    case 133:                                                /* settracks  */
        F.reg[0x20] = (uint8_t)((F.reg[0x20] & 0xFC) | (a[0] & 3));
        F.reg[0x36] = (uint8_t)((F.reg[0x36] & 0xFC) | (a[1] & 3));
        return 0;
    case 134:                                                /* setmontracks */
        F.reg[0x20] = (uint8_t)((F.reg[0x20] & 0xCF) | ((a[0] & 3) << 4));
        return 0;
    case 135: {                                              /* setinterrupt */
        int shift = a[0] ? 0 : 2;              /* 0 = Timer A (bits 2-3), 1 = MFP-15 (bits 0-1) */
        F.reg[0x00] = (uint8_t)((F.reg[0x00] & ~(3 << shift)) | ((a[1] & 3) << shift));
        return 0;
    }
    case 136: {                                              /* buffoper   */
        uint8_t cur = snd_read8(0x01);
        int32_t now = (cur & 1) | (cur & 2) | ((cur >> 2) & 4) | ((cur >> 2) & 8);
        if (a[0] < 0)
            return now;
        int m = a[0];
        uint8_t v = (uint8_t)((cur & 0x80) | (m & 1) | (m & 2) | ((m & 4) << 2) | ((m & 8) << 2));
        snd_write8(0x01, v);
        return 0;
    }
    case 137:                                                /* dsptristate */
        reg_or_and(0x31, 0x7F, a[0] ? 0x80 : 0);
        reg_or_and(0x33, 0x7F, a[1] ? 0x80 : 0);
        return 0;
    case 138:                                                /* gpio */
        if (a[0] == 0) { F.reg[0x41] = (uint8_t)(a[1] & 7); return 0; }
        if (a[0] == 1) return F.reg[0x43] & 7;
        F.reg[0x43] = (uint8_t)(a[1] & 7);
        return 0;
    case 139: {                                              /* devconnect */
        int src = a[0] & 3, dst = a[1], clk = a[2] & 3, pre = a[3], proto = a[4] & 1;
        uint16_t sv = rw16(0x30), dv = rw16(0x32);
        int sh = src * 4;
        uint16_t nib = (uint16_t)(((sv >> sh) & 0x8) | (clk << 1) | proto);
        if (src == 1) nib |= 0x8;                            /* DSP xmit enabled */
        sv = (uint16_t)((sv & ~(0xF << sh)) | (nib << sh));
        for (int d = 0; d < 4; d++) {
            if (!(dst & (1 << d))) continue;
            int dsh = d * 4;
            uint16_t dn = (uint16_t)((src << 1) | proto | (d == 1 ? 0x8 : ((dv >> dsh) & 0x8)));
            dv = (uint16_t)((dv & ~(0xF << dsh)) | (dn << dsh));
        }
        F.reg[0x30] = (uint8_t)(sv >> 8); F.reg[0x31] = (uint8_t)sv;
        F.reg[0x32] = (uint8_t)(dv >> 8); F.reg[0x33] = (uint8_t)dv;
        if (clk == 1) F.reg[0x34] = (uint8_t)(pre & 0xF);
        else F.reg[0x35] = (uint8_t)(pre & 0xF);
        falcon_audio_kick();
        return 0;
    }
    case 140:                                                /* sndstatus  */
        if (a[0] == 1)
            sound_defaults();
        return 0;
    case 141:                                                /* buffptr    */
        if (out4) {
            out4[0] = F.play_cnt;
            out4[1] = F.rec_cnt;
            out4[2] = 0;
            out4[3] = 0;
        }
        return 0;
    }
    return op;                                               /* unknown: TOS style */
}

/* ------------------------------------------------------------------ */
/* Locks, arm, reset, init                                             */
/* ------------------------------------------------------------------ */
int32_t falcon_dsp_lock(int lock)
{
    if (lock) {
        if (F.dsp_locked) return -1;
        F.dsp_locked = 1;
        return 0;
    }
    F.dsp_locked = 0;
    return 0;
}

/* Dsp_ExecProg (boot 0) / Dsp_ExecBoot (boot 1), CPU thread: reset the
 * DSP as TOS 4 does and hand the engine the program to load. Anything
 * the host sent before is dropped, as at a PSG reset release. */
/* A program reloaded over a running one: what the two sides last said.
 * Software that reloads on a protocol error (ACE Tracker) shows here what
 * it saw versus what it expected. */
static void reload_dump(void)
{
    uint32_t unread = hf_count(&F.rx);
    uint32_t rn = atomic_load(&g_rxall_n);
    uint32_t cnt = rn < 160u ? rn : 160u;
    fprintf(stderr, "[FALCON] reload while running: DSP pc $%04X, %u words the 68k never read; "
            "last %u DSP->68k words (| marks the unread tail):", F.dsp->pc, unread, cnt);
    for (uint32_t k = 0; k < cnt; k++) {
        if (k % 16 == 0)
            fprintf(stderr, "\n[FALCON]   ");
        if (cnt - k == unread)
            fprintf(stderr, " |");
        fprintf(stderr, " %06X", g_rxall[(rn - cnt + k) % RXALL]);
    }
    uint32_t hn = atomic_load(&g_hlog_n);
    uint32_t hc = hn < HLOG ? hn : HLOG;
    fprintf(stderr, "\n[FALCON]   host-port trace (S=DSP sent@pc, R=68k read, W=68k wrote), oldest first:");
    for (uint32_t k = 0; k < hc; k++) {
        if (k % 8 == 0)
            fprintf(stderr, "\n[FALCON]   ");
        uint32_t i = (hn - hc + k) % HLOG;
        if (g_hlog[i].dir == 'S')
            fprintf(stderr, " S%06X@%04X", g_hlog[i].v, g_hlog[i].pc);
        else
            fprintf(stderr, " %c%06X", g_hlog[i].dir, g_hlog[i].v);
    }
    fprintf(stderr, "\n");
}

int32_t falcon_dsp_exec(const uint32_t *w, uint32_t n, int boot)
{
    if (!atomic_load(&F.armed))
        return 0;
    {
        /* dump a real reload - one where the DSP produced a block this
         * cycle - at most twice a second, forever (the storm starts at
         * launch, so a fixed budget is spent before anyone reads it) */
        static uint64_t last_dump_ns;
        static uint32_t last_rxall;
        uint32_t rn = atomic_load(&g_rxall_n);
        uint64_t t = mono_ns();
        if (falcon_dbg() && rn - last_rxall > 64u && t - last_dump_ns > 500000000ull) {
            last_dump_ns = t;
            reload_dump();
        }
        last_rxall = rn;
    }
    struct dsp_exec *x = (struct dsp_exec *)malloc(sizeof *x + (size_t)n * sizeof(uint32_t));
    if (!x)
        return 0;
    x->boot = boot;
    x->n = n;
    memcpy(x->w, w, (size_t)n * sizeof(uint32_t));
    atomic_store(&g_dsp_resetting, 1);       /* hide the port until BDF */
    free(atomic_exchange(&F.exec, x));       /* an unconsumed one is stale */
    F.porta &= (uint8_t)~0x10;               /* reset line released */
    atomic_store(&F.boot_head, atomic_load(&F.tx.head));
    atomic_store(&F.rx.tail, atomic_load(&F.rx.head));
    atomic_store(&F.reset_held, 0);
    atomic_fetch_add(&F.reset_epoch, 1);
    falcon_audio_kick();
#ifndef FALCON_HARNESS
    /* On real hardware Dsp_ExecProg resets, loads and runs the DSP all
     * within the trap, so when it returns the program is already
     * answering (ACE's play loader immediately polls for the program's
     * BDF reply). The engine does the reset on its own thread; returning
     * before it is done let ACE's BDF wait race it and the load looped
     * forever. Wait for the engine to finish the reset (g_dsp_resetting
     * clears after the load, release-ordered), then run the DSP on this
     * thread until its first host word is out - the coprocessor DSP is
     * ours to run once resetting is clear, and the engine defers to us
     * (cpu_coproc). Matches the synchronous harness, which plays fine.
     * Bounded so a program that never answers cannot hang the 68k. */
    if (g_dsp_inline) {
        uint64_t t0 = mono_ns();
        while (atomic_load_explicit(&g_dsp_resetting, memory_order_acquire)) {
            falcon_audio_kick();
            if (mono_ns() - t0 > 50000000ull) break;      /* 50 ms safety */
        }
        dsp_unsettle();
        dsp_settle();                /* run the new program to its first wait */
    }
#endif
    return 0;
}

int32_t falcon_snd_lock(int lock)
{
    if (lock) {
        if (F.snd_locked) return -129;                       /* SNDLOCKED */
        F.snd_locked = 1;
        return 1;
    }
    if (!F.snd_locked) return -128;                          /* SNDNOTLOCK */
    F.snd_locked = 0;
    return 0;
}

uint32_t falcon_info(uint32_t what)
{
    switch (what) {
    case 0: return (uint32_t)atomic_load(&F.dsp_state);
    case 1: return (uint32_t)F.frames;
    case 2: return F.dsp ? (uint32_t)F.dsp->idle_skips : 0;
    case 3: return F.dsp ? (uint32_t)(F.dsp->cycles >> 10) : 0;
    case 4: return F.dsp ? F.dsp->illegal_count : 0;
    case 5: return F.boot_count;
    case 6: return (uint32_t)F.frame_hz;
    case 7: return F.dsp ? F.dsp->pc : 0;
    case 8: return hf_count(&F.tx);
    case 9: return hf_count(&F.rx);
    }
    return 0;
}

static void regs_default(void)
{
    memset(F.reg, 0, sizeof F.reg);
    sound_defaults();              /* DMA play -> everything, matrix -> DAC */
    F.play_rep = F.rec_rep = 0;
}

void falcon_arm(void)
{
    if (!F.configured)
        return;
    regs_default();
    F.dsp_locked = F.snd_locked = 0;
    atomic_store(&F.armed, 1);
    fprintf(stderr, "[FALCON] DSP56001 + sound matrix armed\n");
}

void falcon_disarm(void)
{
    atomic_store(&F.armed, 0);
    atomic_store(&F.play_on, 0);
    atomic_store(&F.rec_on, 0);
}

void falcon_reset(void)
{
    if (!F.configured)
        return;
    falcon_disarm();
    atomic_store(&F.reset_held, 1);
    atomic_store(&F.dsp_state, DSP_HELD);
    F.dsp_locked = F.snd_locked = 0;
    F.porta = 0;
}

int falcon_init(uint8_t *guest, uint32_t guest_size)
{
    memset(&F, 0, sizeof F);
    {
        const char *e = getenv("PISTORM_DSP_TURBO");
        int t = e ? atoi(e) : 2;
        if (t < 1) t = 1;
        if (t > 8) t = 8;
        g_dsp_hz = DSP_HZ_REAL * t;
        host_hot_init();
        const char *tm = getenv("PISTORM_DSP_TXDE");
        g_txde_chip = tm && (!strcmp(tm, "chip") || !strcmp(tm, "1"));
        if (g_txde_chip)
            fprintf(stderr, "[FALCON] host port TXDE: chip rule, two words in flight (PISTORM_DSP_TXDE=chip)\n");
        const char *hd = getenv("PISTORM_DSP_HTX_DEPTH");
        if (hd) { int v = atoi(hd); if (v >= 1 && v <= 32768) g_htx_depth = (unsigned)v; }
        const char *vs = getenv("PISTORM_FALCON_VBLSYNC");
        g_vbl_sync = !(vs && *vs == '0');
        if (!g_vbl_sync)
            fprintf(stderr, "[FALCON] sample clock on the wall clock (PISTORM_FALCON_VBLSYNC=0)\n");
        const char *in = getenv("PISTORM_DSP_INLINE");
        g_dsp_inline = !(in && *in == '0');
        if (!g_dsp_inline)
            fprintf(stderr, "[FALCON] coprocessor DSP stays on the engine thread (PISTORM_DSP_INLINE=0)\n");
        if (t != 2)
            fprintf(stderr, "[FALCON] DSP at %d x 32 MHz (PISTORM_DSP_TURBO)\n", t);
        /* PISTORM_DSP_TRACE=<entries>: the host-port trace ring (see dtr) */
        const char *tr = getenv("PISTORM_DSP_TRACE");
        long te = tr ? atol(tr) : 0;
        if (te > 0) {
            uint32_t sz = 1024;
            if (te < 1024) te = 8192;        /* "1" etc.: the default size */
            while (sz < (uint32_t)te && sz < (1u << 24))
                sz <<= 1;
            g_tr = (dtr_t *)calloc(sz, sizeof *g_tr);
#if defined(__aarch64__)
            {
                uint64_t fq;
                __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(fq));
                if (fq)
                    g_tr_tick_ns = 1e9 / (double)fq;
            }
#endif
            if (g_tr) {
                const char *tf = getenv("PISTORM_DSP_TRACE_FIRST");
                g_tr_first = tf && *tf == '1';
                g_tr_size = sz;
                struct sigaction sa;
                memset(&sa, 0, sizeof sa);
                sa.sa_handler = dtr_on_sigusr2;
                sigemptyset(&sa.sa_mask);
                sa.sa_flags = SA_RESTART;
                sigaction(SIGUSR2, &sa, NULL);
                fprintf(stderr, "[DSPPORT] host-port trace on: %s %u events; dumped %son a stall, "
                        "on SIGUSR2 and at exit to %s-NNN.txt\n",
                        g_tr_first ? "the first" : "the last", sz,
                        g_tr_first ? "when full, " : "",
                        getenv("PISTORM_DSP_TRACE_FILE") ? getenv("PISTORM_DSP_TRACE_FILE")
                                                         : "/tmp/dsptrace");
            }
        }
    }
    F.dsp = (dsp56k_t *)calloc(1, sizeof(dsp56k_t));
    if (!F.dsp)
        return -1;
    F.guest = guest;
    F.guest_size = guest_size;
    F.dsp->periph_read = periph_read;
    F.dsp->periph_write = periph_write;
    F.dsp->ctx = NULL;
    dsp56k_init(F.dsp);
    {
        /* last 64 steps for the illegal dump: PISTORM_DSP_HIST=1 only - it
         * is a few stores per DSP instruction, and FalcAMP needs every
         * cycle the Pi has */
        const char *h = getenv("PISTORM_DSP_HIST");
        F.dsp->hist_on = h && *h == '1';
    }
    F.ssisr = SSI_TDE;
    regs_default();
    atomic_store(&F.dsp_state, DSP_HELD);
    F.configured = 1;

    int audio = falcon_audio_open();
#ifdef FALCON_HARNESS
    (void)audio;
    return 0;
#endif

    /* the engine must not inherit the caller's scheduling: explicit
     * SCHED_OTHER on core 1 (core 2 is the 68k, 3 ipl_task, 0 render) */
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_OTHER);
    struct sched_param sp = { 0 };
    pthread_attr_setschedparam(&at, &sp);
#ifdef __linux__
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(1, &cs);
    pthread_attr_setaffinity_np(&at, sizeof cs, &cs);
#endif
    if (pthread_create(&F.thread, &at, engine, NULL) != 0) {
        perror("[FALCON] engine thread");
        pthread_attr_destroy(&at);
        F.configured = 0;
        return -1;
    }
    pthread_attr_destroy(&at);
#ifdef __linux__
    pthread_setname_np(F.thread, "falcon-dsp");
#endif
    printf("[INIT] Falcon DSP56001 + sound matrix (HDMI%s)\n",
           audio == 0 ? "" : ", no audio device: silent");
    return 0;
}

void falcon_shutdown(void)
{
    if (!F.configured)
        return;
    atomic_store(&F.stop, 1);
    falcon_audio_kick();
    pthread_join(F.thread, NULL);
    falcon_audio_close();
    if (g_tr_size) {
        /* a dump being written on its own thread must finish before the
         * process goes (quitting cut one short at 78 MB) */
        for (int i = 0; i < 6000 && atomic_load(&g_tr_dumping); i++)
            usleep(10000);
        static int marked_written;
        const char *why = atomic_load(&g_tr_mark_why);
        if (why)
            marked_written = 1;              /* written by the engine, or: */
        if (why && !atomic_load(&g_tr_dumping) && atomic_load(&g_tr_pause) == 1 &&
            !g_tr_mark_done)
            dtr_dump(why, 0);
        else if (!marked_written)
            dtr_dump("exit", 0);
    }
}
