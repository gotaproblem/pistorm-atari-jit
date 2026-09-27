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

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DSP_HZ 32000000.0

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
typedef struct {
    uint32_t w[HF_SIZE];
    _Atomic uint32_t head;          /* producer */
    _Atomic uint32_t tail;          /* consumer */
} hfifo_t;

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

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
enum { DSP_HELD = 0, DSP_BOOT, DSP_RUN };

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
    /* wall-clock sample clock (engine): frames due since t0_ns */
    uint64_t     t0_ns, emitted, trim_hold_ns;
    unsigned     resyncs;
    uint64_t     dropped_tx;
} F;

static inline uint16_t rw16(int o) { return (uint16_t)((F.reg[o] << 8) | F.reg[o + 1]); }

int falcon_configured(void) { return F.configured; }
int falcon_armed(void) { return atomic_load(&F.armed); }

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

static void host_irqs(void)
{
    dsp56k_t *d = F.dsp;
    int lvl = (int)((F.ipr >> 10) & 3);
    uint8_t hcr = atomic_load(&F.hcr);
    if (lvl && (hcr & HCR_HRIE) && hf_count(&F.tx))
        dsp56k_irq_raise(d, DSP_VEC_HOST_RX, lvl);
    else
        dsp56k_irq_clear(d, DSP_VEC_HOST_RX);
    if (lvl && (hcr & HCR_HTIE) && hf_count(&F.rx) < HF_SIZE)
        dsp56k_irq_raise(d, DSP_VEC_HOST_TX, lvl);
    else
        dsp56k_irq_clear(d, DSP_VEC_HOST_TX);
    uint8_t cvr = atomic_load(&F.cvr);
    if ((cvr & 0x80) && lvl && (hcr & HCR_HCIE)) {
        dsp56k_irq_raise(d, (cvr & 0x1F) * 2, lvl);
        atomic_fetch_and(&F.cvr, (uint8_t)0x7F);      /* taken */
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
        if (hf_count(&F.tx)) v |= 0x01;                          /* HRDF */
        if (hf_count(&F.rx) < HF_SIZE) v |= 0x02;                 /* HTDE */
        if (atomic_load(&F.cvr) & 0x80) v |= 0x04;               /* HCP  */
        v |= (uint32_t)((atomic_load(&F.icr) >> 3) & 3) << 3;   /* HF0/HF1 */
        return v;
    }
    case 0xFFEB: {                                               /* HRX */
        uint32_t v;
        if (hf_peek(&F.tx, &v)) { hf_pop(&F.tx); F.hrx_last = v; }
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
    case 0xFFE8: atomic_store(&F.hcr, (uint8_t)(v & 0x1F)); host_irqs(); return;
    case 0xFFEB:                                                 /* HTX */
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
static uint32_t ssi_slot(int slot, uint32_t in, int *valid)
{
    uint32_t out = 0;
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
/* ------------------------------------------------------------------ */
static void dsp_power_state(void)
{
    uint32_t ep = atomic_load(&F.reset_epoch);
    if (ep != F.reset_seen) {
        F.reset_seen = ep;
        /* reset released: the bootstrap loader starts listening */
        dsp56k_reset(F.dsp);
        memset(F.dsp->pint, 0, sizeof F.dsp->pint);
        atomic_store(&F.tx.tail, atomic_load(&F.boot_head));
        F.hrx_last = 0;
        F.ipr = F.bcr = F.pcc = F.pbc = 0;
        F.cra = F.crb = 0;
        F.ssisr = SSI_TDE;
        atomic_store(&F.hcr, 0);
        F.boot_count = 0;
        atomic_store(&F.dsp_state, DSP_BOOT);
    }
    if (atomic_load(&F.reset_held))
        atomic_store(&F.dsp_state, DSP_HELD);
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
        F.dsp->pc = 0;
        F.dsp->omr = 0x0002;
        F.dsp->sr = 0x0300;
        atomic_store(&F.dsp_state, DSP_RUN);
        fprintf(stderr, "[FALCON] DSP booted (%u words)\n", F.boot_count);
    }
}

static inline void dsp_run(uint32_t cycles)
{
    host_irqs();
    dsp56k_run(F.dsp, cycles);
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

static void dma_frame_event(int rec)
{
    uint8_t c = F.reg[0x00];
    if (c & (rec ? 0x02 : 0x01))
        mfp_hub_timer_a_event();
    if (c & (rec ? 0x08 : 0x04))
        mfp_hub_raise(15);
}

/* 8 16-bit words of the playback DMA for this frame; 0 if idle */
static int dma_play_frame(int16_t w[8])
{
    if (!atomic_load(&F.play_on))
        return 0;
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
    if (c >= F.play_end) {
        dma_frame_event(0);
        if (F.play_rep) {
            c = F.play_start;
        } else {
            atomic_store(&F.play_on, 0);
            F.reg[0x01] &= (uint8_t)~0x03;
        }
    }
    F.play_cnt = c;
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
        if (c >= F.rec_end)
            break;
    }
    if (c >= F.rec_end) {
        dma_frame_event(1);
        if (F.rec_rep) {
            c = F.rec_start;
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

    /* DSP: 8 slots across the frame */
    double slot = F.cyc_per_frame / 8.0;
    if (running) {
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
#define ENGINE_LEAD_MS  30u     /* audio queued ahead of the device */
#define ENGINE_CHUNK    48u     /* frames per step (~1 ms at 49 kHz) */

static uint64_t eng_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* t0 = now; the first clock_due() then asks for the whole lead at once,
 * priming the ring so the device never starts dry */
static void clock_restart(void)
{
    F.t0_ns = eng_now_ns();
    F.emitted = 0;
    F.trim_hold_ns = F.t0_ns + 500000000ull;
}

/* frames the wall clock says are due now (may be <= 0) */
static int64_t clock_due(double hz)
{
    uint64_t now = eng_now_ns();
    int64_t lead = (int64_t)(hz * ENGINE_LEAD_MS / 1000.0);
    int64_t fill = (int64_t)falcon_audio_fill();
    /* drift trim: a ring far off its target means the device clock and
     * CLOCK_MONOTONIC disagree (or the device stalled) - slide t0 */
    if (now >= F.trim_hold_ns && (fill > 3 * lead || fill < lead / 4)) {
        int64_t err = fill - lead;                      /* frames */
        int64_t ns = (int64_t)((double)err * 1e9 / hz);
        F.t0_ns = (uint64_t)((int64_t)F.t0_ns + ns);
        F.trim_hold_ns = now + 250000000ull;            /* let it settle */
        F.resyncs++;
    }
    double el = (double)(int64_t)(now - F.t0_ns);
    int64_t want = (int64_t)(el * hz / 1e9) + lead;
    return want - (int64_t)F.emitted;
}

static int host_pending(void)
{
    return hf_count(&F.tx) || (atomic_load(&F.cvr) & 0x80);
}

static void *engine(void *arg)
{
    (void)arg;
    double last_hz = 0.0;
    uint64_t stat_ns = 0;
    unsigned stat_under = 0, stat_resync = 0;
    while (!atomic_load(&F.stop)) {
        if (!atomic_load(&F.armed)) {
            last_hz = 0.0;
            usleep(5000);
            continue;
        }
        dsp_power_state();
        int st = atomic_load(&F.dsp_state);
        if (st == DSP_BOOT) {
            dsp_boot_drain();
            if (atomic_load(&F.dsp_state) == DSP_BOOT)
                falcon_audio_wait(200);
            continue;
        }
        if (!matrix_active()) {
            out_flush();
            last_hz = 0.0;
            if (st == DSP_RUN) {
                /* nothing clocks the DSP: run it at its own speed, 1 ms
                 * at a time, sooner when the host has sent something */
                dsp_run(32000);
                if (!host_pending())
                    falcon_audio_wait(1000);
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
            clock_restart();
            stat_ns = 0;               /* new baseline for the stats */
        }
        int64_t due = clock_due(hz);
        if (due <= 0) {
            /* nothing due: the DSP waits with the sample clock. It is
             * not run ahead to answer the host - cycles taken early are
             * missing between later SSI slots, and a program that misses
             * its transmit slot takes the underrun vector instead */
            out_flush();
            falcon_audio_wait(500);
            continue;
        }
        unsigned n = due > (int64_t)(4 * ENGINE_CHUNK) ? 4 * ENGINE_CHUNK
                                                       : (unsigned)due;
        for (unsigned i = 0; i < n && !atomic_load(&F.stop); i++)
            frame();
        F.emitted += n;
        out_flush();

        uint64_t now = eng_now_ns();
        if (now - stat_ns > 10000000000ull) {
            unsigned u = falcon_audio_underruns(), r = F.resyncs;
            if (stat_ns && (u != stat_under || r != stat_resync))
                fprintf(stderr, "[FALCON] %.0f Hz: %u device underruns, %u clock trims "
                        "in 10 s (ring %u frames)\n", hz, u - stat_under,
                        r - stat_resync, falcon_audio_fill());
            stat_ns = now;
            stat_under = u;
            stat_resync = r;
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
            dsp_run(frames * 651u);
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

static uint8_t host_read8(uint32_t o)
{
    uint32_t v;
    int have = hf_peek(&F.rx, &v);
    if (!have)
        v = F.rx_last;
    switch (o) {
    case 0: return atomic_load(&F.icr);
    case 1: return atomic_load(&F.cvr);
    case 2: {
        uint8_t isr = 0;
        uint32_t txn = hf_count(&F.tx);
        if (have) isr |= 0x01;                               /* RXDF */
        if (txn <= HF_SIZE - HF_TXDE_ROOM) isr |= 0x02;      /* TXDE */
        if (txn == 0) isr |= 0x04;                           /* TRDY */
        isr |= (uint8_t)(((atomic_load(&F.hcr) >> 3) & 3) << 3); /* HF2 HF3 */
        uint8_t icr = atomic_load(&F.icr);
        if (((icr & 1) && have) || ((icr & 2) && txn <= HF_SIZE - HF_TXDE_ROOM))
            isr |= 0x80;                                     /* HREQ */
        return isr;
    }
    case 3: return F.ivr;
    case 5: return (uint8_t)(v >> 16);
    case 6: return (uint8_t)(v >> 8);
    case 7:
        if (have) { hf_pop(&F.rx); F.rx_last = v; }
        return (uint8_t)v;
    }
    return 0;
}

static void host_write8(uint32_t o, uint8_t v)
{
    switch (o) {
    case 0:
        if (v & 0x80) {                                      /* INIT */
            if (v & 0x02) atomic_store(&F.tx.head, atomic_load(&F.tx.tail));
            if (v & 0x01) atomic_store(&F.rx.tail, atomic_load(&F.rx.head));
            v &= 0x7F;
        }
        atomic_store(&F.icr, v);
        break;
    case 1: atomic_store(&F.cvr, (uint8_t)(v & 0x9F)); break;
    case 3: F.ivr = v; break;
    case 5: F.txb[0] = v; break;
    case 6: F.txb[1] = v; break;
    case 7:
        F.txb[2] = v;
        if (!hf_push(&F.tx, ((uint32_t)F.txb[0] << 16) | ((uint32_t)F.txb[1] << 8) | v)) {
            if (F.dropped_tx++ < 4)
                fprintf(stderr, "[FALCON] host port overflow: 68k word dropped "
                        "(DSP %u words behind)\n", hf_count(&F.tx));
        }
        break;
    default: return;
    }
    falcon_audio_kick();
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
    *a = (*a & ~(0xFFu << shift)) | ((uint32_t)v << shift);
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
        if (pon && !atomic_load(&F.play_on)) F.play_cnt = F.play_start;
        if (ron && !atomic_load(&F.rec_on)) F.rec_cnt = F.rec_start;
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

uint32_t falcon_hw_read(uint32_t a, int size)
{
    uint32_t v = 0;
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

void falcon_hw_write(uint32_t a, uint32_t v, int size)
{
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
        uint32_t beg = (uint32_t)a[1] & 0xFFFFFE, end = (uint32_t)a[2] & 0xFFFFFE;
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
        int shift = a[0] ? 2 : 0;
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
    F.dsp = (dsp56k_t *)calloc(1, sizeof(dsp56k_t));
    if (!F.dsp)
        return -1;
    F.guest = guest;
    F.guest_size = guest_size;
    F.dsp->periph_read = periph_read;
    F.dsp->periph_write = periph_write;
    F.dsp->ctx = NULL;
    dsp56k_init(F.dsp);
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
}
