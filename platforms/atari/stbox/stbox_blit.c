/* SPDX-License-Identifier: MIT
 *
 * stbox_blit.c - the sandbox STE's BLiTTER, as a RESUMABLE engine.
 *
 * Semantics are the main machine's st_blitter.c (clean-room from the public
 * BLiTTER documentation: HOP/OP/endmask/skew/FXSR/NFSR/smudge, the source
 * FIFO that persists across blits, the bus-word latch that NFSR re-inserts),
 * ported word-for-word - but where st_blitter.c runs the whole blit inside
 * the register write, this one keeps the loop state in a struct and executes
 * a bounded number of bus accesses per call from stbox_slice() on core 3.
 * That is what keeps the ipl_task admission rule (bounded sub-microsecond
 * work per pass): a 32000-byte screen blit is ~3000 steps of 16 accesses,
 * which at 4 guest cycles per access is also exactly how long the real chip
 * takes (~24 ms), so BUSY-polling loops and blitter-paced code see hardware
 * timing for free.
 *
 * Memory goes through stbox.c's sandbox map (stbox_blit_mem_r16/w16): ST-RAM
 * and TOS ROM read, ST-RAM writes, nothing else - the isolation invariant in
 * stbox.h holds (this file never sees the real bus).
 *
 * Core 3 only: the register file is read/written by the guest (Musashi, on
 * core 3) and stepped by stbox_slice() (core 3). No locks needed.
 */

#include <stdint.h>
#include <string.h>

#include "stbox.h"

static uint8_t  R[0x40];        /* big-endian register file, $FF8A00-3F  */
static uint32_t g_fifo;         /* source FIFO, persists across blits    */
static uint16_t g_bus_word;     /* last word on the blitter's bus        */

/* in-flight blit state (valid while g_busy) */
static struct {
    uint32_t src, dst;
    int32_t  sxi, syi, dxi, dyi;
    uint16_t em1, em2, em3;
    uint32_t xc, yc, x;
    int hop, op, line, smudge, fxsr, nfsr, skew, desc;
    int have_fxsr, st_fxsr, st_nfsr;
} B;
static int g_busy;

/* ---- register-file helpers (st_blitter.c) ----------------------------- */
static inline uint16_t rd16(unsigned off)
{ return (uint16_t)((R[off] << 8) | R[off + 1]); }
static inline void wr16(unsigned off, uint16_t v)
{ R[off] = (uint8_t)(v >> 8); R[off + 1] = (uint8_t)v; }
static inline uint32_t rd32(unsigned off)
{ return ((uint32_t)rd16(off) << 16) | rd16(off + 2); }
static inline void wr32(unsigned off, uint32_t v)
{ wr16(off, (uint16_t)(v >> 16)); wr16(off + 2, (uint16_t)v); }

static void sanitize(void)
{
    R[0x20 + 1] &= 0xFE;              /* increments: even */
    R[0x22 + 1] &= 0xFE;
    R[0x2E + 1] &= 0xFE;
    R[0x30 + 1] &= 0xFE;
    R[0x24] = 0;                      /* addresses: 24-bit, even */
    R[0x24 + 3] &= 0xFE;
    R[0x32] = 0;
    R[0x32 + 3] &= 0xFE;
    R[0x3A] &= 0x03;                  /* HOP */
    R[0x3B] &= 0x0F;                  /* OP */
    R[0x3C] &= 0xEF;                  /* control: bit4 unused */
    R[0x3D] &= 0xCF;                  /* skew: bits 4-5 unused */
}

static inline uint16_t blit_op(int op, uint16_t s, uint16_t d)
{
    switch (op & 0xF) {
    case 0:  return 0;
    case 1:  return (uint16_t)(s & d);
    case 2:  return (uint16_t)(s & ~d);
    case 3:  return s;
    case 4:  return (uint16_t)(~s & d);
    case 5:  return d;
    case 6:  return (uint16_t)(s ^ d);
    case 7:  return (uint16_t)(s | d);
    case 8:  return (uint16_t)~(s | d);
    case 9:  return (uint16_t)~(s ^ d);
    case 10: return (uint16_t)~d;
    case 11: return (uint16_t)(s | ~d);
    case 12: return (uint16_t)~s;
    case 13: return (uint16_t)(~s | d);
    case 14: return (uint16_t)~(s & d);
    default: return 0xFFFF;
    }
}

/* ---- start a blit from the register file ------------------------------ */
static void blit_begin(void)
{
    B.src  = rd32(0x24) & 0x00FFFFFEu;
    B.dst  = rd32(0x32) & 0x00FFFFFEu;
    B.sxi  = (int16_t)rd16(0x20);
    B.syi  = (int16_t)rd16(0x22);
    B.dxi  = (int16_t)rd16(0x2E);
    B.dyi  = (int16_t)rd16(0x30);
    B.em1  = rd16(0x28);
    B.em2  = rd16(0x2A);
    B.em3  = rd16(0x2C);
    B.xc   = rd16(0x36);
    B.yc   = rd16(0x38);
    B.hop    = R[0x3A] & 3;
    B.op     = R[0x3B] & 0xF;
    B.line   = R[0x3C] & 0xF;
    B.smudge = R[0x3C] & 0x20;
    B.fxsr   = R[0x3D] & 0x80;
    B.nfsr   = R[0x3D] & 0x40;
    B.skew   = R[0x3D] & 0xF;
    B.desc   = (B.sxi < 0);          /* descending: new word enters high half */
    B.have_fxsr = B.st_fxsr = B.st_nfsr = 0;

    if (B.yc == 0) {                 /* restart with exhausted Y: no start */
        R[0x3C] &= 0x7F;
        g_busy = 0;
        return;
    }
    if (B.xc == 0) B.xc = 65536;
    B.x = B.xc;
    g_busy = 1;
}

/* architectural end state: addresses at the next line, X count reloads,
 * Y exhausted, halftone line updated, BUSY clear, HOG/SMUDGE as written */
static void blit_end(void)
{
    wr32(0x24, B.src & 0x00FFFFFEu);
    wr32(0x32, B.dst & 0x00FFFFFEu);
    wr16(0x38, 0);
    R[0x3C] = (uint8_t)((R[0x3C] & 0x60) | B.line);
    g_busy = 0;
}

/* ---- one word of the blit: st_blitter.c's ProcessWord, hardware order -- */
static int blit_word(void)             /* returns bus accesses used      */
{
    static const uint8_t lop_src[16] = {0,1,1,1,1,0,1,1,1,1,0,1,1,1,1,0};
    static const uint8_t lop_dst[16] = {0,1,1,0,1,1,1,1,1,1,1,1,0,1,1,0};
    int acc = 0;
    uint32_t buf = g_fifo;

    const int FirstWord = (B.x == B.xc);
    uint16_t mask = (FirstWord || B.xc == 1) ? B.em1 : (B.x == 1) ? B.em3 : B.em2;
    if (FirstWord) {
        B.st_nfsr = 0;
        B.st_fxsr = B.fxsr;
    }
    const int need_src = lop_src[B.op] && ((B.hop & 2) || (B.hop == 1 && B.smudge));
    const int need_dst = lop_dst[B.op] || mask != 0xFFFF;
    int fetch_src = 0;
    uint16_t dst_word = 0;

#define FIFO_SHIFT()    (buf = B.desc ? (buf >> 16) : (buf << 16))
#define FIFO_INSERT(w_) (buf |= B.desc ? ((uint32_t)(w_) << 16) : (uint32_t)(w_))

    if (B.st_fxsr && !B.have_fxsr && need_src) {
        FIFO_SHIFT();
        g_bus_word = stbox_blit_mem_r16(B.src);
        FIFO_INSERT(g_bus_word);
        B.src = (B.src + (uint32_t)B.sxi) & 0x00FFFFFEu;
        B.have_fxsr = 1;
        acc++;
    }
    if (need_src && !B.st_nfsr) {
        FIFO_SHIFT();
        g_bus_word = stbox_blit_mem_r16(B.src);
        FIFO_INSERT(g_bus_word);
        fetch_src = 1;
        acc++;
    }
    if (need_dst) {
        dst_word = stbox_blit_mem_r16(B.dst);
        g_bus_word = dst_word;
        acc++;
    }
    if (B.nfsr && B.x == 1 && need_src) {
        /* suppressed final fetch: FIFO re-inserts the bus latch */
        FIFO_SHIFT();
        FIFO_INSERT(g_bus_word);
    }

    uint16_t sw = (uint16_t)(buf >> B.skew);
    uint16_t ht = rd16(((unsigned)(B.smudge ? (sw & 0xF) : B.line)) * 2u);
    uint16_t sdata;
    switch (B.hop) {
    case 0:  sdata = 0xFFFF;              break;
    case 1:  sdata = ht;                  break;
    case 2:  sdata = sw;                  break;
    default: sdata = (uint16_t)(sw & ht); break;
    }
    uint16_t out = mask != 0xFFFF
        ? (uint16_t)((blit_op(B.op, sdata, dst_word) & mask) | (dst_word & ~mask))
        : blit_op(B.op, sdata, dst_word);
    stbox_blit_mem_w16(B.dst, out);
    g_bus_word = out;
    acc++;
    if (B.nfsr && B.x == 1 && need_src) {
        /* second pseudo-fetch after the write: the FIFO carries the
         * just-written word into the next line (hardware quirk) */
        FIFO_SHIFT();
        FIFO_INSERT(g_bus_word);
    }
#undef FIFO_SHIFT
#undef FIFO_INSERT
    g_fifo = buf;

    /* post-word updates, hardware order */
    if (B.x == 2 && B.nfsr)
        B.st_nfsr = 1;
    if (fetch_src)
        B.src = (B.src + (uint32_t)((B.x == 1 || B.st_nfsr) ? B.syi : B.sxi)) & 0x00FFFFFEu;
    if (B.x == 1) {
        B.have_fxsr = 0;
        B.yc--;
        B.x = B.xc;
        B.dst = (B.dst + (uint32_t)B.dyi) & 0x00FFFFFEu;
        B.line = (B.line + (B.dyi >= 0 ? 1 : -1)) & 0xF;
        wr16(0x38, (uint16_t)B.yc);            /* Y count visible mid-blit */
        R[0x3C] = (uint8_t)((R[0x3C] & 0xE0) | B.line);
    } else {
        B.x--;
        B.dst = (B.dst + (uint32_t)B.dxi) & 0x00FFFFFEu;
    }
    return acc;
}

/* ---- public ------------------------------------------------------------ */

int stbox_blit_busy(void) { return g_busy; }
int stbox_blit_hog(void)  { return g_busy && (R[0x3C] & 0x40); }

/* Run up to max_accesses bus accesses of the current blit. Returns the
 * number actually made (0 when idle). Charged to the guest at 4 cycles
 * each by the caller. */
int stbox_blit_step(int max_accesses)
{
    int used = 0;
    while (g_busy && used < max_accesses) {
        used += blit_word();
        if (B.yc == 0) blit_end();
    }
    return used;
}

uint32_t stbox_blit_reg_read(uint32_t addr, int size)
{
    unsigned off = addr & 0x3F;
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        unsigned o = (off + (unsigned)i) & 0x3F;
        uint8_t b = R[o];
        if (o == 0x3C) b = (uint8_t)((b & 0x7F) | (g_busy ? 0x80 : 0));
        v = (v << 8) | b;
    }
    return v;
}

void stbox_blit_reg_write(uint32_t addr, uint32_t val, int size)
{
    unsigned off = addr & 0x3F;
    /* the register file is the engine's working state while a blit is in
     * flight (halftone RAM, endmasks...): hardware lets you poke it and
     * gets what it deserves; we do the same, but a control write that
     * clears BUSY mid-blit aborts (documented as "don't") */
    for (int i = 0; i < size; i++)
        R[(off + (unsigned)i) & 0x3F] = (uint8_t)(val >> (8 * (size - 1 - i)));
    sanitize();

    if (off <= 0x3C && off + (unsigned)size > 0x3C) {
        if (R[0x3C] & 0x80) {
            if (!g_busy) blit_begin();     /* start (or restart after Y=0) */
        } else if (g_busy) {
            blit_end();                    /* BUSY cleared by software */
        }
    }
}

void stbox_blit_reset(void)
{
    memset(R, 0, sizeof R);
    memset(&B, 0, sizeof B);
    g_fifo = 0;
    g_bus_word = 0;
    g_busy = 0;
}
