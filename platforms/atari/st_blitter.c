/*
 * st_blitter.c — emulated Atari ST BLiTTER (see st_blitter.h for rationale).
 *
 * Clean-room implementation from the public Atari BLiTTER documentation.
 *
 * Register map (offsets from $FF8A00, all big-endian):
 *   0x00-0x1F  halftone RAM, 16 words
 *   0x20       SRC X increment (signed, even)
 *   0x22       SRC Y increment (signed, even)
 *   0x24       SRC address (long, 24-bit, even)
 *   0x28/2A/2C endmask 1 (first word) / 2 (middle) / 3 (last word)
 *   0x2E       DST X increment (signed, even)
 *   0x30       DST Y increment (signed, even)
 *   0x32       DST address (long, 24-bit, even)
 *   0x36       X count (words per line; 0 = 65536)
 *   0x38       Y count (lines; blit runs only if > 0)
 *   0x3A       HOP: 0=all ones, 1=halftone, 2=source, 3=source AND halftone
 *   0x3B       OP:  16 combine ops on (source, destination)
 *   0x3C       control: bit7 BUSY, bit6 HOG, bit5 SMUDGE, bits0-3 halftone line
 *   0x3D       skew: bit7 FXSR, bit6 NFSR, bits0-3 skew
 *
 * The engine is a literal transcription of the hardware's per-line walk:
 * source words are funnelled through a 32-bit barrel (buf), shifted by SKEW;
 * FXSR forces one extra source fetch before the first word of a line, NFSR
 * suppresses the final fetch of a line (the address register simply reflects
 * the fetches actually performed, as on silicon — programs account for this
 * in their Y increments). Endmask 1 applies to the first word of a line,
 * endmask 3 to the last (both to a single-word line), endmask 2 in between.
 * SMUDGE selects the halftone line from the low nibble of the shifted source
 * word instead of the LINE field. The halftone line index steps +1/-1 per
 * line by the sign of DST Y increment.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st_blitter.h"

static uint8_t R[0x40];   /* big-endian register file */
static uint32_t g_fifo;      /* source FIFO, persists across blits */
static uint16_t g_bus_word;  /* last word on the blitter's bus (hardware latch) */

/* Timed BUSY: the blit's DATA completes instantly, but the BUSY bit stays
 * visible for as long as the real chip would have needed - many programs
 * (fast-bob engines especially) pace themselves against real blitter
 * throughput and overrun their own buffers against an infinitely fast one.
 * Real timing: 4 bus cycles (500 ns) per bus access; in shared (non-HOG)
 * mode the blitter only owns every other 64-access slot, so wall time
 * doubles. PISTORM_BLIT_INSTANT=1 disables the simulated delay.
 * PISTORM_BLIT_NS_PER_ACCESS overrides the per-access cost (default 500). */
#include <time.h>
static uint64_t g_busy_until;   /* CLOCK_MONOTONIC ns; 0 = idle */
static uint32_t g_accesses;     /* bus accesses of the current blit */

static uint64_t blit_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int blit_timing_cfg(void)   /* 0 = instant, else ns per bus access */
{
    static int ns = -1;
    if (ns < 0)
    {
        /* default: instant (blitter data + busy both immediate). Timed BUSY
         * is opt-in via PISTORM_BLIT_TIMED_NS=<ns per bus access> (500 =
         * realistic STE) for software that paces itself on blitter speed. */
        const char *n = getenv("PISTORM_BLIT_TIMED_NS");
        ns = n ? atoi(n) : 0;
        if (ns < 0 || ns > 100000) ns = 0;
    }
    return ns;
}

/* ---- register-file helpers -------------------------------------------- */

static inline uint16_t rd16(unsigned off)
{
    return (uint16_t)((R[off] << 8) | R[off + 1]);
}

static inline void wr16(unsigned off, uint16_t v)
{
    R[off] = (uint8_t)(v >> 8);
    R[off + 1] = (uint8_t)v;
}

static inline uint32_t rd32(unsigned off)
{
    return ((uint32_t)rd16(off) << 16) | rd16(off + 2);
}

static inline void wr32(unsigned off, uint32_t v)
{
    wr16(off, (uint16_t)(v >> 16));
    wr16(off + 2, (uint16_t)v);
}

/* Enforce per-register don't-care bits after any write. */
static void sanitize(void)
{
    R[0x20 + 1] &= 0xFE;              /* increments: even */
    R[0x22 + 1] &= 0xFE;
    R[0x2E + 1] &= 0xFE;
    R[0x30 + 1] &= 0xFE;
    R[0x24] = 0;                      /* addresses: 24-bit, even (byte 0 unused) */
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
    switch (op & 0xF)
    {
    case 0:  return 0x0000;
    case 1:  return (uint16_t)(s & d);
    case 2:  return (uint16_t)(s & ~d);
    case 3:  return s;
    case 4:  return (uint16_t)(~s & d);
    case 5:  return d;
    case 6:  return (uint16_t)(s ^ d);
    case 7:  return (uint16_t)(s | d);
    case 8:  return (uint16_t)(~s & ~d);
    case 9:  return (uint16_t)~(s ^ d);
    case 10: return (uint16_t)~d;
    case 11: return (uint16_t)(s | ~d);
    case 12: return (uint16_t)~s;
    case 13: return (uint16_t)(~s | d);
    case 14: return (uint16_t)(~s | ~d);
    default: return 0xFFFF;
    }
}

/* ---- the blit engine --------------------------------------------------- */

static void blit_run(void)
{
    uint32_t src  = rd32(0x24) & 0x00FFFFFEu;
    uint32_t dst  = rd32(0x32) & 0x00FFFFFEu;
    int32_t  sxi  = (int16_t)rd16(0x20);
    int32_t  syi  = (int16_t)rd16(0x22);
    int32_t  dxi  = (int16_t)rd16(0x2E);
    int32_t  dyi  = (int16_t)rd16(0x30);
    uint16_t em1  = rd16(0x28);
    uint16_t em2  = rd16(0x2A);
    uint16_t em3  = rd16(0x2C);
    uint32_t xc   = rd16(0x36);
    uint32_t yc   = rd16(0x38);
    int hop    = R[0x3A] & 3;
    int op     = R[0x3B] & 0xF;
    int line   = R[0x3C] & 0xF;
    int smudge = R[0x3C] & 0x20;
    int fxsr   = R[0x3D] & 0x80;
    int nfsr   = R[0x3D] & 0x40;
    int skew   = R[0x3D] & 0xF;
    const int desc = (sxi < 0);      /* descending: new word enters high half */
    uint32_t buf = g_fifo;           /* FIFO persists across blits (hardware) */

    if (yc == 0)
    {
        /* restart with an exhausted Y count: hardware does not start */
        R[0x3C] &= 0x7F;
        return;
    }
    if (xc == 0)
        xc = 65536;
    g_accesses = 0;

    {   /* PISTORM_BLIT_TRACE=N traces the first N blits to stderr */
        static int trace_left = -1;
        if (trace_left < 0)
        {
            const char *e = getenv("PISTORM_BLIT_TRACE");
            trace_left = e ? atoi(e) : 0;
        }
        if (trace_left > 0)
        {
            trace_left--;
            fprintf(stderr,
                "[BLIT] src=%06X sxi=%d syi=%d dst=%06X dxi=%d dyi=%d "
                "xc=%u yc=%u hop=%d op=%d skew=%d fx=%d nf=%d sm=%d "
                "em=%04X/%04X/%04X ln=%d\n",
                src, (int)sxi, (int)syi, dst, (int)dxi, (int)dyi,
                xc, yc, hop, op, skew, !!fxsr, !!nfsr, !!smudge,
                em1, em2, em3, line);
        }
    }

    /* Which ops read source / destination. Matches hardware: the LOP decides
     * whether source is fetched at all (ops 0,5,10,15 never read source, ops
     * 3,12 never read dest unless an endmask forces read-modify-write), and
     * HOP gates it further (source modes, or halftone+smudge). */
    static const uint8_t lop_src[16] = {0,1,1,1,1,0,1,1,1,1,0,1,1,1,1,0};
    static const uint8_t lop_dst[16] = {0,1,1,0,1,1,1,1,1,1,1,1,0,1,1,0};

    int have_fxsr = 0, st_fxsr = 0, st_nfsr = 0;
    uint32_t x = xc;                 /* counts down, hardware style */

    while (yc)
    {
        const int FirstWord = (x == xc);
        uint16_t mask = (FirstWord || xc == 1) ? em1 : (x == 1) ? em3 : em2;
        if (FirstWord)
        {
            st_nfsr = 0;
            st_fxsr = fxsr;
        }
        const int need_src = lop_src[op] && ((hop & 2) || (hop == 1 && smudge));
        const int need_dst = lop_dst[op] || mask != 0xFFFF;
        int fetch_src = 0;
        uint16_t dst_word = 0;

#define FIFO_SHIFT()  (buf = desc ? (buf >> 16) : (buf << 16))
#define FIFO_INSERT(w_) (buf |= desc ? ((uint32_t)(w_) << 16) : (uint32_t)(w_))

        /* ProcessWord, in hardware order */
        if (st_fxsr && !have_fxsr && need_src)
        {
            FIFO_SHIFT();
            g_bus_word = pistorm_blit_read16(src);
            FIFO_INSERT(g_bus_word);
            src = (src + (uint32_t)sxi) & 0x00FFFFFEu;
            have_fxsr = 1;
            g_accesses++;
        }
        if (need_src && !st_nfsr)
        {
            FIFO_SHIFT();
            g_bus_word = pistorm_blit_read16(src);
            FIFO_INSERT(g_bus_word);
            fetch_src = 1;
            g_accesses++;
        }
        if (need_dst)
        {
            dst_word = pistorm_blit_read16(dst);
            g_bus_word = dst_word;
            g_accesses++;
        }
        if (nfsr && x == 1 && need_src)
        {
            /* suppressed final fetch: the FIFO shifts and re-inserts the last
             * word seen on the bus (hardware latch), not fresh memory */
            FIFO_SHIFT();
            FIFO_INSERT(g_bus_word);
        }

        uint16_t sw = (uint16_t)(buf >> skew);
        uint16_t ht = rd16(((unsigned)(smudge ? (sw & 0xF) : line)) * 2u);
        uint16_t sdata;
        switch (hop)
        {
        case 0:  sdata = 0xFFFF;              break;
        case 1:  sdata = ht;                  break;
        case 2:  sdata = sw;                  break;
        default: sdata = (uint16_t)(sw & ht); break;
        }
        uint16_t out = mask != 0xFFFF
            ? (uint16_t)((blit_op(op, sdata, dst_word) & mask) | (dst_word & ~mask))
            : blit_op(op, sdata, dst_word);
        pistorm_blit_write16(dst, out);
        g_bus_word = out;
        g_accesses++;
        if (nfsr && x == 1 && need_src)
        {
            /* second pseudo-fetch after the write (hardware quirk): the FIFO
             * carries the just-written word into the next line */
            FIFO_SHIFT();
            FIFO_INSERT(g_bus_word);
        }

#undef FIFO_SHIFT
#undef FIFO_INSERT

        /* post-word updates, hardware order */
        if (x == 2 && nfsr)
            st_nfsr = 1;
        if (fetch_src)
            src = (src + (uint32_t)((x == 1 || st_nfsr) ? syi : sxi)) & 0x00FFFFFEu;
        if (x == 1)
        {
            have_fxsr = 0;
            yc--;
            x = xc;
            dst = (dst + (uint32_t)dyi) & 0x00FFFFFEu;
            line = (line + (dyi >= 0 ? 1 : -1)) & 0xF;
        }
        else
        {
            x--;
            dst = (dst + (uint32_t)dxi) & 0x00FFFFFEu;
        }
    }

    /* write back the architectural end state: addresses point at the next
     * line, X count reloads (register unchanged), Y count is exhausted,
     * halftone line updated, BUSY clear, HOG/SMUDGE as written. */
    {   /* arm the BUSY window to mimic real blitter duration */
        int ns = blit_timing_cfg();
        if (ns)
        {
            uint64_t dur = (uint64_t)g_accesses * (uint64_t)ns;
            if (!(R[0x3C] & 0x40))     /* shared mode: alternate 64-access slots */
                dur *= 2;
            g_busy_until = blit_now_ns() + dur;
        }
    }
    g_fifo = buf;
    wr32(0x24, src & 0x00FFFFFEu);
    wr32(0x32, dst & 0x00FFFFFEu);
    wr16(0x38, 0);
    R[0x3C] = (uint8_t)((R[0x3C] & 0x60) | line);   /* BUSY off */
}

/* ---- guest-visible register access ------------------------------------ */

uint32_t st_blitter_reg_read(uint32_t addr, int size)
{
    unsigned off = addr & 0x3F;
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
    {
        unsigned o = (off + (unsigned)i) & 0x3F;
        uint8_t b = R[o];
        if (o == 0x3C && g_busy_until && blit_now_ns() < g_busy_until)
            b |= 0x80;                 /* still "running" for pacing purposes */
        v = (v << 8) | b;
    }
    return v;
}

void st_blitter_reg_write(uint32_t addr, uint32_t val, int size)
{
    unsigned off = addr & 0x3F;
    for (int i = 0; i < size; i++)
        R[(off + (unsigned)i) & 0x3F] =
            (uint8_t)(val >> (8 * (size - 1 - i)));
    sanitize();

    /* start on any write whose span covers the control byte and left BUSY set */
    if (off <= 0x3C && off + (unsigned)size > 0x3C && (R[0x3C] & 0x80))
        blit_run();
}

void st_blitter_reset(void)
{
    memset(R, 0, sizeof R);
    g_fifo = 0;
    g_bus_word = 0;
    g_busy_until = 0;
}
