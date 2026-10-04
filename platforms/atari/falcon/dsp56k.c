// SPDX-License-Identifier: MIT
/*
 * dsp56k - DSP56001 interpreter. See dsp56k.h.
 *
 * Written from the DSP56000 family programming model: register set, the
 * instruction encodings (cross-checked against the a56 assembler's output
 * and the MIT-licensed jamesbehr/dsp56k decoder tables), the condition
 * code rules of the instruction summary table, and the Falcon's memory
 * wiring. No code from other DSP56k emulators.
 *
 * Accumulators are kept as sign-extended 56-bit values in int64_t:
 *   A2 = bits 55..48, A1 = bits 47..24, A0 = bits 23..0.
 * A parallel move reads its sources before the data ALU operation and
 * writes its register destinations after it, as the hardware does.
 */
#include "dsp56k.h"

#include <math.h>
#include <string.h>

#define M24  0xFFFFFFu
#define M48  0xFFFFFFFFFFFFull
#define M56  0xFFFFFFFFFFFFFFull

static inline int64_t sx56(uint64_t v) { return (int64_t)(v << 8) >> 8; }
static inline int64_t sx48(uint64_t v) { return (int64_t)(v << 16) >> 16; }
static inline int32_t sx24(uint32_t v) { return (int32_t)(v << 8) >> 8; }

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */
uint32_t dsp56k_mem_read(dsp56k_t *d, int space, uint16_t a)
{
    switch (space) {
    case DSP_SPACE_P:
        if (a < 0x200 && (d->omr & 3) != 3)
            return d->pint[a];
        return d->ext[a & 0x7FFF];
    case DSP_SPACE_X:
        if (a >= 0xFFC0)
            return d->periph_read ? d->periph_read(d->ctx, DSP_SPACE_X, a) & M24 : 0;
        if (a < 0x100)
            return d->xint[a];
        if (a < 0x200 && (d->omr & 4))
            return d->xrom[a & 0xFF];
        return d->ext[(a & 0x3FFF) | 0x4000];
    default:
        if (a >= 0xFFC0)
            return d->periph_read ? d->periph_read(d->ctx, DSP_SPACE_Y, a) & M24 : 0;
        if (a < 0x100)
            return d->yint[a];
        if (a < 0x200 && (d->omr & 4))
            return d->yrom[a & 0xFF];
        return d->ext[a & 0x3FFF];
    }
}

void dsp56k_mem_write(dsp56k_t *d, int space, uint16_t a, uint32_t v)
{
    v &= M24;
    switch (space) {
    case DSP_SPACE_P:
        if (a < 0x200 && (d->omr & 3) != 3) { d->pint[a] = v; return; }
        d->ext[a & 0x7FFF] = v;
        return;
    case DSP_SPACE_X:
        if (a >= 0xFFC0) {
            if (d->periph_write) d->periph_write(d->ctx, DSP_SPACE_X, a, v);
            return;
        }
        if (a < 0x100) { d->xint[a] = v; return; }
        if (a < 0x200 && (d->omr & 4)) return;          /* ROM */
        d->ext[(a & 0x3FFF) | 0x4000] = v;
        return;
    default:
        if (a >= 0xFFC0) {
            if (d->periph_write) d->periph_write(d->ctx, DSP_SPACE_Y, a, v);
            return;
        }
        if (a < 0x100) { d->yint[a] = v; return; }
        if (a < 0x200 && (d->omr & 4)) return;
        d->ext[a & 0x3FFF] = v;
        return;
    }
}

#define RD(sp, a)     dsp56k_mem_read(d, (sp), (uint16_t)(a))
#define WR(sp, a, v)  dsp56k_mem_write(d, (sp), (uint16_t)(a), (v))

static inline uint32_t fetch(dsp56k_t *d)
{
    uint32_t w = RD(DSP_SPACE_P, d->pc);
    d->pc++;
    d->cycles += 2;
    return w;
}

/* ------------------------------------------------------------------ */
/* Stack                                                               */
/* ------------------------------------------------------------------ */
static void push(dsp56k_t *d, uint16_t hi, uint16_t lo)
{
    uint8_t p = (uint8_t)((d->sp & 0x0F) + 1);
    if (p > 15) {                       /* overflow: stack error */
        d->sp |= 0x10;
        dsp56k_irq_raise(d, DSP_VEC_STACKERR, 3);
        p &= 0x0F;
    }
    d->sp = (uint8_t)((d->sp & 0x30) | p);
    d->ssh[p] = hi;
    d->ssl[p] = lo;
}

static void pop(dsp56k_t *d)
{
    uint8_t p = d->sp & 0x0F;
    if (p == 0) {                       /* underflow */
        d->sp |= 0x30;
        dsp56k_irq_raise(d, DSP_VEC_STACKERR, 3);
        return;
    }
    d->sp = (uint8_t)((d->sp & 0x30) | (p - 1));
}

#define SSH (d->ssh[d->sp & 0x0F])
#define SSL (d->ssl[d->sp & 0x0F])

/* ------------------------------------------------------------------ */
/* Condition codes                                                     */
/* ------------------------------------------------------------------ */
static inline int scaling(dsp56k_t *d) { return (d->sr >> 10) & 3; } /* 0 none 1 down 2 up */

/* E, U, N, Z of a 56-bit result */
static void flags_euzn(dsp56k_t *d, int64_t v)
{
    uint16_t sr = d->sr & (uint16_t)~(SR_E | SR_U | SR_N | SR_Z);
    uint64_t u = (uint64_t)v & M56;
    int s = scaling(d);
    /* E: the bits above the 48-bit part (plus its MSB) are not all equal */
    int top = s == 1 ? 48 : s == 2 ? 46 : 47;
    uint64_t hi = u >> top;                      /* bits 55..top */
    uint64_t allones = (1ull << (56 - top)) - 1;
    if (hi != 0 && hi != allones)
        sr |= SR_E;
    /* U: the two bits below the extension are equal */
    int b1 = top, b0 = top - 1;
    if (((u >> b1) & 1) == ((u >> b0) & 1))
        sr |= SR_U;
    if (u >> 55)
        sr |= SR_N;
    if (u == 0)
        sr |= SR_Z;
    d->sr = sr;
}

static inline void set_v(dsp56k_t *d, int v)
{
    if (v) d->sr |= SR_V | SR_L; else d->sr &= (uint16_t)~SR_V;
}

static inline void set_c(dsp56k_t *d, int c)
{
    if (c) d->sr |= SR_C; else d->sr &= (uint16_t)~SR_C;
}

/* 24-bit logic results: N from bit 23 of A1, Z of A1, V cleared */
static void flags_logic(dsp56k_t *d, uint32_t a1)
{
    d->sr &= (uint16_t)~(SR_N | SR_Z | SR_V);
    if (a1 & 0x800000) d->sr |= SR_N;
    if (!(a1 & M24)) d->sr |= SR_Z;
}

static int cond(dsp56k_t *d, int cc)
{
    uint16_t s = d->sr;
    int C = s & SR_C, V = !!(s & SR_V), Z = !!(s & SR_Z), N = !!(s & SR_N);
    int U = !!(s & SR_U), E = !!(s & SR_E), L = !!(s & SR_L);
    int r;
    switch (cc & 7) {
    case 0: r = !C; break;                          /* CC / HS */
    case 1: r = !(N ^ V); break;                    /* GE */
    case 2: r = !Z; break;                          /* NE */
    case 3: r = !N; break;                          /* PL */
    case 4: r = !(Z | (!U & !E)); break;            /* NN */
    case 5: r = !E; break;                          /* EC */
    case 6: r = !L; break;                          /* LC */
    default: r = !(Z | (N ^ V)); break;             /* GT */
    }
    return (cc & 8) ? !r : r;
}

/* ------------------------------------------------------------------ */
/* Accumulators on the data buses                                      */
/* ------------------------------------------------------------------ */
static inline int64_t *acc(dsp56k_t *d, int which) { return which ? &d->b : &d->a; }

/* the value an accumulator puts on a 24-bit bus: scaled, limited */
static uint32_t acc_bus24(dsp56k_t *d, int64_t v)
{
    int s = scaling(d);
    int64_t sv = s == 1 ? v >> 1 : s == 2 ? sx56((uint64_t)v << 1) : v;
    /* S (block floating point) bit: bits 46 and 45 of the scaled value differ */
    uint64_t u = (uint64_t)sv;
    if (((u >> 46) ^ (u >> 45)) & 1)
        d->sr |= SR_S;
    if (sx48((uint64_t)sv) != sv) {
        d->sr |= SR_L;
        return sv < 0 ? 0x800000u : 0x7FFFFFu;
    }
    return (uint32_t)((uint64_t)sv >> 24) & M24;
}

/* ... and on the 48-bit L bus */
static uint64_t acc_bus48(dsp56k_t *d, int64_t v)
{
    int s = scaling(d);
    int64_t sv = s == 1 ? v >> 1 : s == 2 ? sx56((uint64_t)v << 1) : v;
    uint64_t u = (uint64_t)sv;
    if (((u >> 46) ^ (u >> 45)) & 1)
        d->sr |= SR_S;
    if (sx48((uint64_t)sv) != sv) {
        d->sr |= SR_L;
        return sv < 0 ? 0x800000000000ull : 0x7FFFFFFFFFFFull;
    }
    return u & M48;
}

static inline int64_t acc_from24(uint32_t v) { return (int64_t)sx24(v) << 24; }
static inline int64_t acc_from48(uint64_t v) { return sx48(v); }

/* ------------------------------------------------------------------ */
/* Register file by encoding (6-bit DDDDDD; 5-bit codes are the same   */
/* numbers)                                                            */
/* ------------------------------------------------------------------ */
static uint32_t reg_read(dsp56k_t *d, int code)
{
    switch (code) {
    case 0x04: return d->x0;
    case 0x05: return d->x1;
    case 0x06: return d->y0;
    case 0x07: return d->y1;
    case 0x08: return (uint32_t)d->a & M24;
    case 0x09: return (uint32_t)d->b & M24;
    case 0x0A: return (uint32_t)((int32_t)(int8_t)((uint64_t)d->a >> 48)) & M24;
    case 0x0B: return (uint32_t)((int32_t)(int8_t)((uint64_t)d->b >> 48)) & M24;
    case 0x0C: return (uint32_t)((uint64_t)d->a >> 24) & M24;
    case 0x0D: return (uint32_t)((uint64_t)d->b >> 24) & M24;
    case 0x0E: return acc_bus24(d, d->a);
    case 0x0F: return acc_bus24(d, d->b);
    case 0x39: return d->sr;
    case 0x3A: return d->omr;
    case 0x3B: return d->sp;
    case 0x3C: { uint32_t v = SSH; pop(d); return v; }
    case 0x3D: return SSL;
    case 0x3E: return d->la;
    case 0x3F: return d->lc;
    }
    if (code >= 0x10 && code < 0x18) return d->r[code & 7];
    if (code >= 0x18 && code < 0x20) return d->n[code & 7];
    if (code >= 0x20 && code < 0x28) return d->m[code & 7];
    return 0;
}

static void reg_write(dsp56k_t *d, int code, uint32_t v)
{
    v &= M24;
    switch (code) {
    case 0x04: d->x0 = v; return;
    case 0x05: d->x1 = v; return;
    case 0x06: d->y0 = v; return;
    case 0x07: d->y1 = v; return;
    case 0x08: d->a = sx56(((uint64_t)d->a & ~(uint64_t)M24) | v); return;
    case 0x09: d->b = sx56(((uint64_t)d->b & ~(uint64_t)M24) | v); return;
    case 0x0A: d->a = sx56(((uint64_t)d->a & 0xFFFFFFFFFFFFull) | ((uint64_t)(v & 0xFF) << 48)); return;
    case 0x0B: d->b = sx56(((uint64_t)d->b & 0xFFFFFFFFFFFFull) | ((uint64_t)(v & 0xFF) << 48)); return;
    case 0x0C: d->a = sx56(((uint64_t)d->a & 0xFF000000FFFFFFull) | ((uint64_t)v << 24)); return;
    case 0x0D: d->b = sx56(((uint64_t)d->b & 0xFF000000FFFFFFull) | ((uint64_t)v << 24)); return;
    case 0x0E: d->a = acc_from24(v); return;
    case 0x0F: d->b = acc_from24(v); return;
    case 0x39: d->sr = (uint16_t)(v & 0xAFFF); return;
    case 0x3A: d->omr = (uint16_t)(v & 0x00C7); return;
    case 0x3B: d->sp = (uint8_t)(v & 0x3F); return;
    case 0x3C: {                                           /* push */
        uint8_t p = (uint8_t)((d->sp & 0x0F) + 1);
        if (p > 15) { d->sp |= 0x10; dsp56k_irq_raise(d, DSP_VEC_STACKERR, 3); p &= 0x0F; }
        d->sp = (uint8_t)((d->sp & 0x30) | p);
        d->ssh[p] = (uint16_t)v;
        return;
    }
    case 0x3D: SSL = (uint16_t)v; return;
    case 0x3E: d->la = (uint16_t)v; return;
    case 0x3F: d->lc = (uint16_t)v; return;
    }
    if (code >= 0x10 && code < 0x18) { d->r[code & 7] = (uint16_t)v; return; }
    if (code >= 0x18 && code < 0x20) { d->n[code & 7] = (uint16_t)v; return; }
    if (code >= 0x20 && code < 0x28) { d->m[code & 7] = (uint16_t)v; return; }
}

/* writing SSH pushes a fresh level; SSL of that level is whatever was
 * there - reg_write above passes the old SSL through push() */

/* 8-bit short immediate to a register: fractions for X/Y and A/B,
 * integers for the accumulator parts and the AGU */
static void reg_write_short(dsp56k_t *d, int code, uint32_t imm8)
{
    if ((code >= 0x04 && code <= 0x07) || code == 0x0E || code == 0x0F)
        reg_write(d, code, (imm8 & 0xFF) << 16);
    else
        reg_write(d, code, imm8 & 0xFF);
}

/* MOVEC 5-bit program-controller register code -> 6-bit */
static inline int pcr_code(int ddddd)
{
    return (ddddd & 0x18) == 0 ? 0x20 | (ddddd & 7) : 0x38 | (ddddd & 7);
}

/* ------------------------------------------------------------------ */
/* Address generation                                                  */
/* ------------------------------------------------------------------ */
static uint16_t agu_add(dsp56k_t *d, int rn, int32_t delta)
{
    uint16_t r = d->r[rn], m = d->m[rn];
    if (m == 0xFFFF)
        return (uint16_t)(r + delta);
    if (m == 0) {
        /* reverse-carry: add with the bit order reversed */
        uint32_t rr = 0, dd = 0, x = r, y = (uint16_t)delta;
        for (int i = 0; i < 16; i++) {
            rr |= ((x >> i) & 1u) << (15 - i);
            dd |= ((y >> i) & 1u) << (15 - i);
        }
        uint32_t s = (rr + dd) & 0xFFFF, out = 0;
        for (int i = 0; i < 16; i++)
            out |= ((s >> i) & 1u) << (15 - i);
        return (uint16_t)out;
    }
    if (m <= 0x7FFF) {
        uint32_t size = (uint32_t)m + 1;
        uint32_t p2 = 1;
        while (p2 < size) p2 <<= 1;
        int32_t ad = delta < 0 ? -delta : delta;
        if ((uint32_t)ad >= size && (ad & (int32_t)(p2 - 1)) == 0)
            return (uint16_t)(r + delta);     /* multiple of the block: linear */
        uint16_t base = (uint16_t)(r & ~(p2 - 1));
        int32_t off = (int32_t)(r - base) + delta;
        off %= (int32_t)size;
        if (off < 0) off += (int32_t)size;
        return (uint16_t)(base + off);
    }
    return (uint16_t)(r + delta);             /* reserved: linear */
}

/* 6-bit MMMRRR effective address; may fetch an extension word.
 * *imm is set when the mode is immediate (the address is the data). */
static uint16_t ea_calc(dsp56k_t *d, int mmmrrr, int *imm, uint32_t *immval)
{
    int mode = (mmmrrr >> 3) & 7, rn = mmmrrr & 7;
    uint16_t ea;
    if (imm) *imm = 0;
    switch (mode) {
    case 0: ea = d->r[rn]; d->r[rn] = agu_add(d, rn, -(int32_t)(int16_t)d->n[rn]); return ea;
    case 1: ea = d->r[rn]; d->r[rn] = agu_add(d, rn,  (int32_t)(int16_t)d->n[rn]); return ea;
    case 2: ea = d->r[rn]; d->r[rn] = agu_add(d, rn, -1); return ea;
    case 3: ea = d->r[rn]; d->r[rn] = agu_add(d, rn, 1); return ea;
    case 4: return d->r[rn];
    case 5: return agu_add(d, rn, (int32_t)(int16_t)d->n[rn]);
    case 6: {
        uint32_t w = fetch(d);
        if (rn & 4) {                         /* #xxxx */
            if (imm) *imm = 1;
            if (immval) *immval = w;
            return 0;
        }
        return (uint16_t)w;                   /* absolute */
    }
    default: d->r[rn] = agu_add(d, rn, -1); return d->r[rn];
    }
}

/* 2-bit MM effective address used by XY moves and LUA-style updates:
 * 00 (Rn)-Nn? no - the XY table: 00 (Rn), 01 (Rn)+Nn, 10 (Rn)-, 11 (Rn)+ */
static uint16_t ea_xy(dsp56k_t *d, int mm, int rn)
{
    uint16_t ea = d->r[rn];
    switch (mm) {
    case 1: d->r[rn] = agu_add(d, rn, (int32_t)(int16_t)d->n[rn]); break;
    case 2: d->r[rn] = agu_add(d, rn, -1); break;
    case 3: d->r[rn] = agu_add(d, rn, 1); break;
    default: break;
    }
    return ea;
}

/* ------------------------------------------------------------------ */
/* Data ALU                                                            */
/* ------------------------------------------------------------------ */
static int64_t alu_add(dsp56k_t *d, int64_t dst, int64_t src, int carry_in, int set_carry)
{
    uint64_t ud = (uint64_t)dst & M56, us = (uint64_t)src & M56;
    uint64_t r = ud + us + (uint64_t)(carry_in ? 1 : 0);
    int c = (int)((r >> 56) & 1);
    int64_t res = sx56(r);
    int sd = (int)(ud >> 55), ss = (int)(us >> 55), sr_ = (int)((r >> 55) & 1);
    set_v(d, sd == ss && sr_ != sd);
    if (set_carry) set_c(d, c);
    flags_euzn(d, res);
    return res;
}

static int64_t alu_sub(dsp56k_t *d, int64_t dst, int64_t src, int borrow_in, int set_carry)
{
    uint64_t ud = (uint64_t)dst & M56, us = (uint64_t)src & M56;
    uint64_t r = ud - us - (uint64_t)(borrow_in ? 1 : 0);
    int c = (ud < us + (uint64_t)(borrow_in ? 1 : 0));
    int64_t res = sx56(r & M56);
    int sd = (int)(ud >> 55), ss = (int)(us >> 55), sr_ = (int)((r >> 55) & 1);
    set_v(d, sd != ss && sr_ != sd);
    if (set_carry) set_c(d, c);
    flags_euzn(d, res);
    return res;
}

/* convergent rounding at the scaling-dependent position */
static int64_t alu_round(dsp56k_t *d, int64_t v)
{
    int s = scaling(d);
    int bit = s == 1 ? 24 : s == 2 ? 22 : 23;
    uint64_t half = 1ull << bit, mask = (half << 1) - 1;
    uint64_t u = (uint64_t)v & M56;
    uint64_t r = u + half;
    if ((u & mask) == half)
        r &= ~(half << 1);                    /* tie: round to even */
    r &= ~mask;
    int64_t res = sx56(r & M56);
    int sd = (int)(u >> 55), sr_ = (int)((r >> 55) & 1);
    set_v(d, sd == 0 && sr_ != 0);            /* +max rounding over the top */
    flags_euzn(d, res);
    return res;
}

static int64_t mul_product(uint32_t s1, uint32_t s2, int neg)
{
    int64_t p = ((int64_t)sx24(s1) * (int64_t)sx24(s2)) * 2;
    return neg ? -p : p;
}

/* Data ALU source for ADD/SUB/CMP/TFR: 000/001 = the other accumulator
 * (op dependent), 010 X, 011 Y, 1xx x0 y0 x1 y1 */
static int64_t jjj_src(dsp56k_t *d, int jjj, int dbit)
{
    switch (jjj) {
    case 0: case 1: return dbit ? d->a : d->b;
    case 2: return sx48(((uint64_t)d->x1 << 24) | d->x0);
    case 3: return sx48(((uint64_t)d->y1 << 24) | d->y0);
    case 4: return acc_from24(d->x0);
    case 5: return acc_from24(d->y0);
    case 6: return acc_from24(d->x1);
    default: return acc_from24(d->y1);
    }
}

static inline uint32_t jj_reg(dsp56k_t *d, int jj)
{
    switch (jj & 3) {
    case 0: return d->x0;
    case 1: return d->y0;
    case 2: return d->x1;
    default: return d->y1;
    }
}

static void qqq_pair(dsp56k_t *d, int qqq, uint32_t *s1, uint32_t *s2)
{
    switch (qqq & 7) {
    case 0: *s1 = d->x0; *s2 = d->x0; break;
    case 1: *s1 = d->y0; *s2 = d->y0; break;
    case 2: *s1 = d->x1; *s2 = d->x0; break;
    case 3: *s1 = d->y1; *s2 = d->y0; break;
    case 4: *s1 = d->x0; *s2 = d->y1; break;
    case 5: *s1 = d->y0; *s2 = d->x0; break;
    case 6: *s1 = d->x1; *s2 = d->y0; break;
    default: *s1 = d->y1; *s2 = d->x1; break;
    }
}

static inline uint32_t a1_of(int64_t v) { return (uint32_t)((uint64_t)v >> 24) & M24; }
static inline int64_t with_a1(int64_t v, uint32_t a1)
{
    return sx56(((uint64_t)v & 0xFF000000FFFFFFull) | ((uint64_t)(a1 & M24) << 24));
}

/* Execute the data ALU half of a parallel instruction. Returns 0 for an
 * encoding that does not exist. */
static int alu_op(dsp56k_t *d, int op)
{
    if (op == 0)
        return 1;                              /* move only */
    int dbit = (op >> 3) & 1;
    int64_t *D = acc(d, dbit);

    if (op & 0x80) {                           /* MPY/MPYR/MAC/MACR */
        uint32_t s1, s2;
        qqq_pair(d, (op >> 4) & 7, &s1, &s2);
        int64_t p = mul_product(s1, s2, (op >> 2) & 1);
        int64_t r;
        if (op & 2) {                          /* MAC */
            uint16_t c = d->sr & SR_C;
            r = alu_add(d, *D, p, 0, 0);
            d->sr = (uint16_t)((d->sr & ~SR_C) | c);
        } else {
            r = sx56((uint64_t)p);
            d->sr &= (uint16_t)~SR_V;
            flags_euzn(d, r);
        }
        if (op & 1)
            r = alu_round(d, r);
        *D = r;
        return 1;
    }

    int jjj = (op >> 4) & 7, low = op & 7;
    int64_t other = dbit ? d->a : d->b;
    switch (low) {
    case 0:                                    /* ADD */
        if (jjj == 0) return 0;
        *D = alu_add(d, *D, jjj_src(d, jjj, dbit), 0, 1);
        return 1;
    case 1:
        if (jjj == 0) { *D = other; return 1; }               /* TFR acc */
        if (jjj == 1) { *D = alu_round(d, *D); return 1; }     /* RND */
        if (jjj == 2 || jjj == 3) {                           /* ADC */
            *D = alu_add(d, *D, jjj_src(d, jjj, dbit), d->sr & SR_C, 1);
            return 1;
        }
        *D = jjj_src(d, jjj, dbit);                           /* TFR reg */
        return 1;
    case 2:
        if (jjj == 0) {                                       /* ADDR */
            *D = alu_add(d, *D >> 1, other, 0, 1);
            return 1;
        }
        if (jjj == 1) {                                       /* ADDL */
            uint64_t u = (uint64_t)*D & M56;
            int msb_changed = (int)(((u >> 55) ^ (u >> 54)) & 1);
            *D = alu_add(d, sx56(u << 1), other, 0, 1);
            if (msb_changed) d->sr |= SR_V | SR_L;
            return 1;
        }
        if (jjj == 2) {                                       /* ASR */
            uint64_t u = (uint64_t)*D & M56;
            set_c(d, (int)(u & 1));
            *D = *D >> 1;
            d->sr &= (uint16_t)~SR_V;
            flags_euzn(d, *D);
            return 1;
        }
        if (jjj == 3) {                                       /* ASL */
            uint64_t u = (uint64_t)*D & M56;
            set_c(d, (int)((u >> 55) & 1));
            set_v(d, (int)(((u >> 55) ^ (u >> 54)) & 1));
            *D = sx56((u << 1) & M56);
            flags_euzn(d, *D);
            return 1;
        }
        {                                                     /* OR */
            uint32_t r = a1_of(*D) | jj_reg(d, jjj);
            *D = with_a1(*D, r);
            flags_logic(d, r);
            return 1;
        }
    case 3:
        if (jjj == 0) {                                       /* TST */
            d->sr &= (uint16_t)~SR_V;
            flags_euzn(d, *D);
            return 1;
        }
        if (jjj == 1) {                                       /* CLR */
            *D = 0;
            d->sr = (uint16_t)((d->sr & ~(SR_E | SR_N | SR_V)) | SR_U | SR_Z);
            return 1;
        }
        if (jjj == 2) {                                       /* LSR */
            uint32_t a1 = a1_of(*D);
            set_c(d, (int)(a1 & 1));
            a1 >>= 1;
            *D = with_a1(*D, a1);
            flags_logic(d, a1);
            return 1;
        }
        if (jjj == 3) {                                       /* LSL */
            uint32_t a1 = a1_of(*D);
            set_c(d, (int)((a1 >> 23) & 1));
            a1 = (a1 << 1) & M24;
            *D = with_a1(*D, a1);
            flags_logic(d, a1);
            return 1;
        }
        {                                                     /* EOR */
            uint32_t r = a1_of(*D) ^ jj_reg(d, jjj);
            *D = with_a1(*D, r);
            flags_logic(d, r);
            return 1;
        }
    case 4:                                    /* SUB */
        if (jjj == 0) return 0;
        *D = alu_sub(d, *D, jjj_src(d, jjj, dbit), 0, 1);
        return 1;
    case 5:
        if (jjj == 0) { alu_sub(d, *D, other, 0, 1); return 1; }        /* CMP acc */
        if (jjj == 1) return 0;
        if (jjj == 2 || jjj == 3) {                                     /* SBC */
            *D = alu_sub(d, *D, jjj_src(d, jjj, dbit), d->sr & SR_C, 1);
            return 1;
        }
        alu_sub(d, *D, jjj_src(d, jjj, dbit), 0, 1);                    /* CMP */
        return 1;
    case 6:
        if (jjj == 0) {                                       /* SUBR */
            *D = alu_sub(d, *D >> 1, other, 0, 1);
            return 1;
        }
        if (jjj == 1) {                                       /* SUBL */
            uint64_t u = (uint64_t)*D & M56;
            int msb_changed = (int)(((u >> 55) ^ (u >> 54)) & 1);
            *D = alu_sub(d, sx56(u << 1), other, 0, 1);
            if (msb_changed) d->sr |= SR_V | SR_L;
            return 1;
        }
        if (jjj == 2) {                                       /* ABS */
            uint16_t c = d->sr & SR_C;
            if (*D < 0)
                *D = alu_sub(d, 0, *D, 0, 0);
            else {
                d->sr &= (uint16_t)~SR_V;
                flags_euzn(d, *D);
            }
            d->sr = (uint16_t)((d->sr & ~SR_C) | c);
            return 1;
        }
        if (jjj == 3) {                                       /* NEG */
            uint16_t c = d->sr & SR_C;
            *D = alu_sub(d, 0, *D, 0, 0);
            d->sr = (uint16_t)((d->sr & ~SR_C) | c);
            return 1;
        }
        {                                                     /* AND */
            uint32_t r = a1_of(*D) & jj_reg(d, jjj);
            *D = with_a1(*D, r);
            flags_logic(d, r);
            return 1;
        }
    default: /* 7 */
        if (jjj == 1) {                                       /* NOT */
            uint32_t r = (~a1_of(*D)) & M24;
            *D = with_a1(*D, r);
            flags_logic(d, r);
            return 1;
        }
        if (jjj == 2) {                                       /* ROR */
            uint32_t a1 = a1_of(*D);
            int cin = d->sr & SR_C;
            set_c(d, (int)(a1 & 1));
            a1 = (a1 >> 1) | (cin ? 0x800000u : 0);
            *D = with_a1(*D, a1);
            flags_logic(d, a1);
            return 1;
        }
        if (jjj == 3) {                                       /* ROL */
            uint32_t a1 = a1_of(*D);
            int cin = d->sr & SR_C;
            set_c(d, (int)((a1 >> 23) & 1));
            a1 = ((a1 << 1) | (cin ? 1u : 0)) & M24;
            *D = with_a1(*D, a1);
            flags_logic(d, a1);
            return 1;
        }
        {                                                     /* CMPM */
            int64_t s = jjj == 0 ? other : jjj_src(d, jjj, dbit);
            int64_t x = *D < 0 ? -*D : *D;
            int64_t y = s < 0 ? -s : s;
            alu_sub(d, sx56((uint64_t)x), sx56((uint64_t)y), 0, 1);
            return 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Parallel moves                                                      */
/* ------------------------------------------------------------------ */
/* Deferred register writes of one instruction (after the ALU op). */
typedef struct {
    int n;
    struct { int kind, code; uint64_t v; } w[3];
} pend_t;

enum { PW_REG24, PW_REG_SHORT, PW_ACC48, PW_A10 };

static inline void pend_reg(pend_t *p, int code, uint32_t v)
{
    p->w[p->n].kind = PW_REG24; p->w[p->n].code = code; p->w[p->n].v = v; p->n++;
}

static void pend_commit(dsp56k_t *d, pend_t *p)
{
    for (int i = 0; i < p->n; i++) {
        int c = p->w[i].code;
        switch (p->w[i].kind) {
        case PW_REG24:     reg_write(d, c, (uint32_t)p->w[i].v); break;
        case PW_REG_SHORT: reg_write_short(d, c, (uint32_t)p->w[i].v); break;
        case PW_ACC48:     *acc(d, c) = acc_from48(p->w[i].v); break;
        case PW_A10:       {
            int64_t *A = acc(d, c);
            *A = sx56(((uint64_t)*A & 0xFF000000000000ull) | (p->w[i].v & M48));
            break;
        }
        }
    }
}

static const int xy_x_reg[4] = { 0x04, 0x05, 0x0E, 0x0F };  /* x0 x1 a b */
static const int xy_y_reg[4] = { 0x06, 0x07, 0x0E, 0x0F };  /* y0 y1 a b */

/* Decode and perform the move half (reads + memory writes now, register
 * writes deferred into *p). mv is the 16-bit move field (bits 23..8).
 * Returns 0 for an encoding that is not a parallel move. */
static int par_move(dsp56k_t *d, uint32_t mv, pend_t *p)
{
    if (mv & 0x8000) {                                  /* X: and Y: */
        int xrn = mv & 7, xmm = (mv >> 3) & 3, xw = (mv >> 7) & 1;
        int yrn = ((mv >> 5) & 3) | (xrn < 4 ? 4 : 0);
        int ymm = (mv >> 12) & 3, yw = (mv >> 14) & 1;
        int xreg = xy_x_reg[(mv >> 10) & 3], yreg = xy_y_reg[(mv >> 8) & 3];
        uint32_t xv = xw ? 0 : reg_read(d, xreg);
        uint32_t yv = yw ? 0 : reg_read(d, yreg);
        uint16_t xa = ea_xy(d, xmm, xrn);
        uint16_t ya = ea_xy(d, ymm, yrn);
        if (xw) pend_reg(p, xreg, RD(DSP_SPACE_X, xa)); else WR(DSP_SPACE_X, xa, xv);
        if (yw) pend_reg(p, yreg, RD(DSP_SPACE_Y, ya)); else WR(DSP_SPACE_Y, ya, yv);
        return 1;
    }
    if ((mv & 0xC000) == 0x4000) {                      /* X:, Y:, L: */
        int w = (mv >> 7) & 1;
        int ea_mode = (mv >> 6) & 1;                    /* 1 = ea, 0 = aa */
        int dd = (mv >> 12) & 3, ddd = (mv >> 8) & 7;
        if (dd == 0 && !(ddd & 4)) {                    /* L: */
            int lll = (((mv >> 11) & 1) << 2) | (ddd & 3);
            uint16_t ea = ea_mode ? ea_calc(d, mv & 0x3F, NULL, NULL) : (uint16_t)(mv & 0x3F);
            if (w) {
                uint32_t xv = RD(DSP_SPACE_X, ea), yv = RD(DSP_SPACE_Y, ea);
                uint64_t lv = ((uint64_t)xv << 24) | yv;
                switch (lll) {
                case 0: p->w[p->n].kind = PW_A10;  p->w[p->n].code = 0; p->w[p->n].v = lv; p->n++; break;
                case 1: p->w[p->n].kind = PW_A10;  p->w[p->n].code = 1; p->w[p->n].v = lv; p->n++; break;
                case 2: pend_reg(p, 0x05, xv); pend_reg(p, 0x04, yv); break;
                case 3: pend_reg(p, 0x07, xv); pend_reg(p, 0x06, yv); break;
                case 4: p->w[p->n].kind = PW_ACC48; p->w[p->n].code = 0; p->w[p->n].v = lv; p->n++; break;
                case 5: p->w[p->n].kind = PW_ACC48; p->w[p->n].code = 1; p->w[p->n].v = lv; p->n++; break;
                case 6: pend_reg(p, 0x0E, xv); pend_reg(p, 0x0F, yv); break;
                default: pend_reg(p, 0x0F, xv); pend_reg(p, 0x0E, yv); break;
                }
            } else {
                uint32_t xv, yv;
                switch (lll) {
                case 0: xv = a1_of(d->a); yv = (uint32_t)d->a & M24; break;
                case 1: xv = a1_of(d->b); yv = (uint32_t)d->b & M24; break;
                case 2: xv = d->x1; yv = d->x0; break;
                case 3: xv = d->y1; yv = d->y0; break;
                case 4: { uint64_t v = acc_bus48(d, d->a); xv = (uint32_t)(v >> 24); yv = (uint32_t)v & M24; break; }
                case 5: { uint64_t v = acc_bus48(d, d->b); xv = (uint32_t)(v >> 24); yv = (uint32_t)v & M24; break; }
                case 6: xv = acc_bus24(d, d->a); yv = acc_bus24(d, d->b); break;
                default: xv = acc_bus24(d, d->b); yv = acc_bus24(d, d->a); break;
                }
                WR(DSP_SPACE_X, ea, xv);
                WR(DSP_SPACE_Y, ea, yv);
            }
            return 1;
        }
        int reg = (dd << 3) | ddd;
        int space = (mv >> 11) & 1 ? DSP_SPACE_Y : DSP_SPACE_X;
        if (ea_mode) {
            int imm = 0; uint32_t immv = 0;
            uint32_t sv = w ? 0 : reg_read(d, reg);
            uint16_t ea = ea_calc(d, mv & 0x3F, &imm, &immv);
            if (imm) {
                if (!w) return 1;                       /* "move x0,#" - nonsense */
                pend_reg(p, reg, immv);
                return 1;
            }
            if (w) pend_reg(p, reg, RD(space, ea)); else WR(space, ea, sv);
        } else {
            uint16_t ea = (uint16_t)(mv & 0x3F);
            if (w) pend_reg(p, reg, RD(space, ea)); else WR(space, ea, reg_read(d, reg));
        }
        return 1;
    }
    if ((mv & 0xE000) == 0x2000) {                      /* #xx, R, U, none */
        if ((mv & 0xFC00) == 0x2000) {
            if (mv == 0x2000)
                return 1;
            if ((mv & 0xFFE0) == 0x2040) {              /* U: ea update */
                int mm = (mv >> 3) & 3, rn = mv & 7;
                static const int map[4] = { 0, 1, 2, 3 };  /* -N +N - + */
                (void)ea_calc(d, (map[mm] << 3) | rn, NULL, NULL);
                return 1;
            }
            int src = (mv >> 5) & 0x1F, dst = mv & 0x1F;
            if (src < 4 || dst < 4)
                return 0;
            pend_reg(p, dst, reg_read(d, src));
            return 1;
        }
        int dst = (mv >> 8) & 0x1F;
        if (dst < 4)
            return 0;
        p->w[p->n].kind = PW_REG_SHORT; p->w[p->n].code = dst; p->w[p->n].v = mv & 0xFF; p->n++;
        return 1;
    }
    if ((mv & 0xF000) == 0x1000) {                      /* X:R and R:Y */
        int w = (mv >> 7) & 1;
        int imm = 0; uint32_t immv = 0;
        if (!(mv & 0x40)) {                             /* X:ea,D1  S2,D2 */
            int r1 = xy_x_reg[(mv >> 10) & 3];
            int s2 = (mv >> 9) & 1;                     /* A/B */
            int d2 = (mv >> 8) & 1 ? 0x07 : 0x06;       /* y1/y0 */
            uint32_t v1 = w ? 0 : reg_read(d, r1);
            uint32_t v2 = acc_bus24(d, *acc(d, s2));
            uint16_t ea = ea_calc(d, mv & 0x3F, &imm, &immv);
            if (w) pend_reg(p, r1, imm ? immv : RD(DSP_SPACE_X, ea));
            else WR(DSP_SPACE_X, ea, v1);
            pend_reg(p, d2, v2);
        } else {                                        /* S1,D1  Y:ea,D2 */
            /* 0001 deff W1MMMRRR: d (bit 11) is S1 = A/B, e (bit 10)
             * is D1 = X0/X1. They were swapped, so "A,X1 Y:ea,Y1" wrote
             * X0 - in ACE Tracker's mix that overwrote a coefficient and
             * put two clicks in every buffer (the buzz). */
            int d1 = (mv >> 10) & 1 ? 0x05 : 0x04;      /* x1/x0 */
            int s1 = (mv >> 11) & 1;                    /* A/B */
            int r2 = xy_y_reg[(mv >> 8) & 3];
            uint32_t v1 = acc_bus24(d, *acc(d, s1));
            uint32_t v2 = w ? 0 : reg_read(d, r2);
            uint16_t ea = ea_calc(d, mv & 0x3F, &imm, &immv);
            pend_reg(p, d1, v1);
            if (w) pend_reg(p, r2, imm ? immv : RD(DSP_SPACE_Y, ea));
            else WR(DSP_SPACE_Y, ea, v2);
        }
        return 1;
    }
    if ((mv & 0xFE00) == 0x0800 && !(mv & 0x40)) {     /* A->X:ea X0->A etc */
        int a = (mv >> 8) & 1;
        int ysp = (mv >> 7) & 1;
        uint32_t v = acc_bus24(d, *acc(d, a));
        uint16_t ea = ea_calc(d, mv & 0x3F, NULL, NULL);
        WR(ysp ? DSP_SPACE_Y : DSP_SPACE_X, ea, v);
        pend_reg(p, a ? 0x0F : 0x0E, ysp ? d->y0 : d->x0);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */
void dsp56k_irq_raise(dsp56k_t *d, int vector, int level)
{
    int i = (vector >> 1) & 31;
    d->irq_level[i] = (uint8_t)level;
    /* Only the thread running the DSP raises and clears (under the DSP
     * lock), and the host sources are re-asserted on every HSR/HRX/HTX
     * access: skip the read-modify-write when the bit is already set -
     * on the Pi 4 (no LSE) each was a call into a load/store-exclusive
     * loop, ~4% of the 68k thread in DSPBench's transfer tests. */
    if (!(__atomic_load_n(&d->irq_pending, __ATOMIC_RELAXED) & (1ull << i)))
        __atomic_or_fetch(&d->irq_pending, 1ull << i, __ATOMIC_RELEASE);
}

void dsp56k_irq_clear(dsp56k_t *d, int vector)
{
    int i = (vector >> 1) & 31;
    if (__atomic_load_n(&d->irq_pending, __ATOMIC_RELAXED) & (1ull << i))
        __atomic_and_fetch(&d->irq_pending, ~(1ull << i), __ATOMIC_RELEASE);
}

static int is_jsr(uint32_t op)
{
    int hi = (op >> 16) & 0xFF;
    if (hi == 0x0D || hi == 0x0F) return 1;              /* JSR / JScc xxx */
    if (hi != 0x0B) return 0;
    if (((op >> 14) & 3) != 3)
        return (op >> 7) & 1;                             /* JSCLR/JSSET aa ea pp */
    if (op & 0x80) return 1;                              /* JSR / JScc ea */
    return ((op >> 6) & 3) == 0;                          /* JSCLR/JSSET reg */
}

static void execute(dsp56k_t *d, uint32_t op);

/* Accept the highest-priority pending interrupt the mask allows. */
static void service_irq(dsp56k_t *d)
{
    uint64_t pend = __atomic_load_n(&d->irq_pending, __ATOMIC_ACQUIRE);
    if (!pend)
        return;
    int mask = (d->sr >> 8) & 3;
    int best = -1, best_lvl = -1;
    for (int i = 0; i < 32; i++) {
        if (!(pend & (1ull << i)))
            continue;
        int lvl = d->irq_level[i];
        if (lvl <= 0)
            continue;
        if (!(lvl > mask || lvl == 3))
            continue;
        if (lvl > best_lvl) { best = i; best_lvl = lvl; }
    }
    if (best < 0)
        return;
    dsp56k_irq_clear(d, best << 1);
    d->halted = 0;
    d->accept_level = best_lvl;

    uint16_t vec = (uint16_t)(best << 1);
    uint32_t op0 = RD(DSP_SPACE_P, vec);
    if (is_jsr(op0)) {
        /* long interrupt: the JSR pushes the interrupted PC and SR */
        d->irq_ret_pc = d->pc;
        d->in_fast_irq = 1;
        d->irq_end_pc = (uint16_t)(vec + 2);
        uint16_t save = d->pc;
        d->pc = vec;
        uint32_t op = fetch(d);
        execute(d, op);
        d->in_fast_irq = 0;
        (void)save;
        /* the JSR set I1:I0 to the level, see do_jsr() */
        return;
    }
    /* fast interrupt: two words at the vector, then back */
    uint16_t ret = d->pc;
    d->in_fast_irq = 1;
    d->irq_ret_pc = ret;
    d->irq_end_pc = (uint16_t)(vec + 2);
    d->pc = vec;
    while (d->in_fast_irq && d->pc < d->irq_end_pc && d->pc >= vec) {
        uint32_t op = fetch(d);
        execute(d, op);
    }
    if (d->in_fast_irq) {
        d->in_fast_irq = 0;
        d->pc = ret;
    }
    (void)best_lvl;
}

/* ------------------------------------------------------------------ */
/* Program control helpers                                             */
/* ------------------------------------------------------------------ */
static void do_jsr(dsp56k_t *d, uint16_t target)
{
    uint16_t ret = d->pc;
    if (d->in_fast_irq) {
        /* a JSR in an interrupt vector turns it into a long interrupt */
        ret = d->irq_ret_pc;
        d->in_fast_irq = 0;
        push(d, ret, d->sr);
        int lvl = d->accept_level > 0 ? d->accept_level : 3;
        d->sr = (uint16_t)((d->sr & ~(SR_I0 | SR_I1 | SR_S0 | SR_S1 | SR_LF | SR_T)) | (lvl << 8));
        d->pc = target;
        return;
    }
    push(d, ret, d->sr);
    d->pc = target;
}

static void loop_end_check(dsp56k_t *d, uint16_t last_word_addr)
{
    if (!(d->sr & SR_LF) || last_word_addr != d->la)
        return;
    if (d->lc == 1) {
        /* done: restore LF from the stacked SR, pop both levels */
        d->sr = (uint16_t)((d->sr & ~SR_LF) | (SSL & SR_LF));
        pop(d);
        d->la = SSH;
        d->lc = SSL;
        pop(d);
        d->pc = (uint16_t)(last_word_addr + 1);
    } else {
        d->lc--;
        d->pc = SSH;
    }
}

static void start_do(dsp56k_t *d, uint32_t count, uint16_t la)
{
    push(d, d->la, d->lc);
    d->la = la;
    d->lc = (uint16_t)count;
    push(d, d->pc, d->sr);
    d->sr |= SR_LF;
}

/* bit test on a 24-bit value; returns the bit */
static inline int bit_of(uint32_t v, int b) { return (int)((v >> (b & 31)) & 1); }

/* read/modify/write helper for BCHG/BCLR/BSET/BTST: kind 0 clr 1 set 2 chg 3 tst */
static uint32_t bitop(dsp56k_t *d, uint32_t v, int b, int kind)
{
    int old = bit_of(v, b);
    set_c(d, old);
    switch (kind) {
    case 0: v &= ~(1u << b); break;
    case 1: v |= (1u << b); break;
    case 2: v ^= (1u << b); break;
    default: break;
    }
    return v & M24;
}

/* ------------------------------------------------------------------ */
/* The instruction decoder                                             */
/* ------------------------------------------------------------------ */
static void illegal(dsp56k_t *d)
{
    d->illegal_count++;
    d->illegal_pc = d->pc;
    dsp56k_irq_raise(d, DSP_VEC_ILLEGAL, 3);
}

/* Instructions with no parallel move (op < $100000, except the
 * $08xxxx/$09xxxx parallel class with bit 14 clear). */
static void exec_nonpar(dsp56k_t *d, uint32_t op, uint16_t start_pc)
{
    int hi = (op >> 16) & 0xFF;
    switch (hi) {
    case 0x00:
        switch (op) {
        case 0x000000: return;                                   /* NOP */
        case 0x000004:                                           /* RTI */
            d->pc = SSH; d->sr = SSL & 0xAFFF; pop(d); return;
        case 0x000005: illegal(d); return;                       /* ILLEGAL */
        case 0x000006: dsp56k_irq_raise(d, DSP_VEC_SWI, 3); return; /* SWI */
        case 0x00000C: d->pc = SSH; pop(d); return;              /* RTS */
        case 0x000084: return;                                   /* RESET (peripherals) */
        case 0x000086: d->halted = 1; return;                    /* WAIT */
        case 0x000087: d->halted = 1; return;                    /* STOP */
        case 0x00008C:                                           /* ENDDO */
            d->sr = (uint16_t)((d->sr & ~SR_LF) | (SSL & SR_LF));
            pop(d);
            d->la = SSH; d->lc = SSL;
            pop(d);
            return;
        }
        if ((op & 0xFF00FC) == 0x0000B8) {                        /* ANDI */
            uint32_t imm = (op >> 8) & 0xFF;
            switch (op & 3) {
            case 0: d->sr &= (uint16_t)((imm << 8) | 0xFF); break;
            case 1: d->sr &= (uint16_t)(imm | 0xFF00); break;
            case 2: d->omr &= (uint16_t)imm; break;
            }
            return;
        }
        if ((op & 0xFF00FC) == 0x0000F8) {                        /* ORI */
            uint32_t imm = (op >> 8) & 0xFF;
            switch (op & 3) {
            case 0: d->sr |= (uint16_t)(imm << 8); break;
            case 1: d->sr |= (uint16_t)imm; break;
            case 2: d->omr |= (uint16_t)imm; break;
            }
            return;
        }
        illegal(d);
        return;

    case 0x01:
        if ((op & 0xFFFFC7) == 0x018040) {                        /* DIV */
            uint32_t s = jj_reg(d, (op >> 4) & 3);
            int64_t *D = acc(d, (op >> 3) & 1);
            uint64_t u = (uint64_t)*D & M56;
            int cin = d->sr & SR_C;
            int v = (int)(((u >> 55) ^ (u >> 54)) & 1);
            int64_t sh = sx56(((u << 1) | (cin ? 1 : 0)) & M56);
            int64_t sv = (int64_t)sx24(s) << 24;
            int64_t r = ((u >> 55) ^ ((s >> 23) & 1)) ? sh + sv : sh - sv;
            r = sx56((uint64_t)r & M56);
            *D = r;
            set_c(d, !((((uint64_t)r >> 55) ^ ((s >> 23) & 1)) & 1));
            if (v) d->sr |= SR_V | SR_L; else d->sr &= (uint16_t)~SR_V;
            return;
        }
        if ((op & 0xFFF8F7) == 0x01D815) {                        /* NORM Rn,D */
            int rn = (op >> 8) & 7;
            int64_t *D = acc(d, (op >> 3) & 1);
            uint16_t s = d->sr;
            if (!(s & SR_E) && (s & SR_U) && !(s & SR_Z)) {
                uint64_t u = (uint64_t)*D & M56;
                set_v(d, (int)(((u >> 55) ^ (u >> 54)) & 1));
                *D = sx56((u << 1) & M56);
                d->r[rn]--;
                flags_euzn(d, *D);
            } else if (s & SR_E) {
                *D = *D >> 1;
                d->r[rn]++;
                d->sr &= (uint16_t)~SR_V;
                flags_euzn(d, *D);
            }
            return;
        }
        illegal(d);
        return;

    case 0x02: case 0x03: {                                       /* Tcc */
        int cc = (op >> 12) & 0xF;
        int jjj = (op >> 4) & 7, dbit = (op >> 3) & 1;
        if (!cond(d, cc))
            return;
        int64_t src;
        if (jjj == 0) src = dbit ? d->a : d->b;
        else if (jjj >= 4) src = jjj_src(d, jjj, dbit);
        else { illegal(d); return; }
        if (hi == 0x03) {
            int t = (op >> 8) & 7, T = op & 7;
            d->r[T] = d->r[t];
        }
        *acc(d, dbit) = src;
        return;
    }

    case 0x04:
        if ((op & 0xFFE0F0) == 0x044010) {                        /* LUA ea,D */
            int mm = (op >> 11) & 3, rn = (op >> 8) & 7;
            uint16_t save = d->r[rn];
            (void)ea_calc(d, (mm << 3) | rn, NULL, NULL);
            uint16_t v = d->r[rn];
            d->r[rn] = save;
            int dd = op & 0xF;
            if (dd & 8) d->n[dd & 7] = v; else d->r[dd & 7] = v;
            return;
        }
        if ((op & 0xFF40E0) == 0x0440A0) {                        /* MOVEC S1,D2 / S2,D1 */
            int w = (op >> 15) & 1;
            int e = (op >> 8) & 0x3F;
            int pc5 = pcr_code(op & 0x1F);
            if (w) reg_write(d, pc5, reg_read(d, e));
            else reg_write(d, e, reg_read(d, pc5));
            return;
        }
        illegal(d);
        return;

    case 0x05:
        if ((op & 0xFF00E0) == 0x0500A0) {                        /* MOVEC #xx,D1 */
            reg_write(d, pcr_code(op & 0x1F), (op >> 8) & 0xFF);
            return;
        }
        if ((op & 0xFF0020) == 0x050020 && !(op & 0x80)) {        /* MOVEC ea/aa */
            int w = (op >> 15) & 1, isea = (op >> 14) & 1;
            int space = (op >> 6) & 1 ? DSP_SPACE_Y : DSP_SPACE_X;
            int pc5 = pcr_code(op & 0x1F);
            if (isea) {
                int imm = 0; uint32_t immv = 0;
                uint32_t v = w ? 0 : reg_read(d, pc5);
                uint16_t ea = ea_calc(d, (op >> 8) & 0x3F, &imm, &immv);
                if (w) reg_write(d, pc5, imm ? immv : RD(space, ea));
                else if (!imm) WR(space, ea, v);
            } else {
                uint16_t ea = (uint16_t)((op >> 8) & 0x3F);
                if (w) reg_write(d, pc5, RD(space, ea));
                else WR(space, ea, reg_read(d, pc5));
            }
            return;
        }
        illegal(d);
        return;

    case 0x06: {                                                  /* DO / REP */
        int is_rep = (op >> 5) & 1;
        uint32_t count;
        if ((op & 0xFF0080) == 0x060080) {                       /* #xxx */
            count = ((op >> 8) & 0xFF) | ((op & 0xF) << 8);
        } else if ((op & 0xFFC000) == 0x06C000) {                /* S */
            count = reg_read(d, (op >> 8) & 0x3F);
        } else {
            int space = (op >> 6) & 1 ? DSP_SPACE_Y : DSP_SPACE_X;
            uint16_t ea;
            if (op & 0x4000) ea = ea_calc(d, (op >> 8) & 0x3F, NULL, NULL);
            else ea = (uint16_t)((op >> 8) & 0x3F);
            count = RD(space, ea);
        }
        count &= 0xFFFF;
        if (is_rep) {
            /* repeat the next instruction; not interruptible */
            uint16_t lc_save = d->lc;
            uint32_t n = count ? count : 0x10000;
            uint16_t ipc = d->pc;
            for (uint32_t i = 0; i < n; i++) {
                d->pc = ipc;
                uint32_t iop = fetch(d);
                execute(d, iop);
            }
            d->lc = lc_save;
            return;
        }
        uint16_t la = (uint16_t)fetch(d);
        if (count == 0 && 0) {
            /* (56000: LC=0 means 65536 passes - handled by the counter) */
        }
        start_do(d, count, la);
        return;
    }

    case 0x07: {                                                  /* MOVEM */
        int w = (op >> 15) & 1;
        int reg = op & 0x3F;
        if ((op & 0xFF4080) == 0x074080) {                       /* P:ea */
            int imm = 0; uint32_t immv = 0;
            uint32_t v = w ? 0 : reg_read(d, reg);
            uint16_t ea = ea_calc(d, (op >> 8) & 0x3F, &imm, &immv);
            if (w) reg_write(d, reg, imm ? immv : RD(DSP_SPACE_P, ea));
            else if (!imm) WR(DSP_SPACE_P, ea, v);
            return;
        }
        if ((op & 0xFF40C0) == 0x070000) {                       /* P:aa */
            uint16_t ea = (uint16_t)((op >> 8) & 0x3F);
            if (w) reg_write(d, reg, RD(DSP_SPACE_P, ea));
            else WR(DSP_SPACE_P, ea, reg_read(d, reg));
            return;
        }
        illegal(d);
        return;
    }

    case 0x08: case 0x09: {                                       /* MOVEP */
        int ppsp = (op >> 16) & 1 ? DSP_SPACE_Y : DSP_SPACE_X;
        int w = (op >> 15) & 1;                  /* 1: into the peripheral */
        uint16_t pp = (uint16_t)(0xFFC0 | (op & 0x3F));
        int kind = (op >> 6) & 3;
        if (kind & 2) {                                          /* X/Y:ea */
            int easp = (op >> 6) & 1 ? DSP_SPACE_Y : DSP_SPACE_X;
            int imm = 0; uint32_t immv = 0;
            uint16_t ea = ea_calc(d, (op >> 8) & 0x3F, &imm, &immv);
            if (w) WR(ppsp, pp, imm ? immv : RD(easp, ea));
            else if (!imm) WR(easp, ea, RD(ppsp, pp));
        } else if (kind == 1) {                                  /* P:ea */
            int imm = 0; uint32_t immv = 0;
            uint16_t ea = ea_calc(d, (op >> 8) & 0x3F, &imm, &immv);
            if (w) WR(ppsp, pp, imm ? immv : RD(DSP_SPACE_P, ea));
            else if (!imm) WR(DSP_SPACE_P, ea, RD(ppsp, pp));
        } else {                                                 /* register */
            int reg = (op >> 8) & 0x3F;
            if (w) WR(ppsp, pp, reg_read(d, reg));
            else reg_write(d, reg, RD(ppsp, pp));
        }
        return;
    }

    case 0x0A: case 0x0B: {
        int group = (op >> 14) & 3;          /* 0 aa, 1 ea, 2 pp, 3 register */
        int bit5 = (op >> 5) & 1, bit7 = (op >> 7) & 1;
        int b = op & 0x1F;
        int is0B = hi == 0x0B;
        if (group == 3) {
            int reg = (op >> 8) & 0x3F;
            if (bit7) {
                /* JMP/Jcc/JSR/JScc ea */
                uint32_t sub = op & 0xFF;
                int doit;
                if (sub == 0x80) doit = 1;
                else if ((sub & 0xF0) == 0xA0) doit = cond(d, sub & 0xF);
                else { illegal(d); return; }
                uint16_t ea = ea_calc(d, reg, NULL, NULL);
                if (!doit) return;
                if (is0B) do_jsr(d, ea); else d->pc = ea;
                /* "jmp *": waiting for an interrupt, which only arrives
                 * between runs - the caller may skip ahead */
                if (!is0B && sub == 0x80 && ea == start_pc)
                    d->idle_hint = 1;
                return;
            }
            int kind = (op >> 5) & 7;         /* 000 JCLR 001 JSET 010 BCLR 011 BSET */
            uint32_t v = reg_read(d, reg);
            if (kind <= 1) {
                /* JCLR/JSET/JSCLR/JSSET: the condition codes are not
                 * affected (unlike BTST). Setting C here corrupted the
                 * carry a program keeps across a polling wait. */
                uint16_t tgt = (uint16_t)fetch(d);
                int bitv = bit_of(v, b);
                if (bitv == kind) {
                    if (is0B) do_jsr(d, tgt); else d->pc = tgt;
                }
                return;
            }
            /* 0A: 010 BCLR 011 BSET; 0B: 010 BCHG 011 BTST */
            int k = is0B ? (kind == 2 ? 2 : 3) : (kind == 2 ? 0 : 1);
            uint32_t nv = bitop(d, v, b, k);
            if (k != 3) reg_write(d, reg, nv);
            return;
        }
        int space = (op >> 6) & 1 ? DSP_SPACE_Y : DSP_SPACE_X;
        uint16_t ea;
        if (group == 1) ea = ea_calc(d, (op >> 8) & 0x3F, NULL, NULL);
        else if (group == 0) ea = (uint16_t)((op >> 8) & 0x3F);
        else ea = (uint16_t)(0xFFC0 | ((op >> 8) & 0x3F));
        uint32_t v = RD(space, ea);
        if (bit7) {
            /* JCLR/JSET (0A) and JSCLR/JSSET (0B): CCR not affected */
            uint16_t tgt = (uint16_t)fetch(d);
            int bitv = bit_of(v, b);
            if (bitv == bit5) {
                if (is0B) do_jsr(d, tgt); else d->pc = tgt;
                /* "jclr #n,x:<<periph,*": waiting on a peripheral - the
                 * caller may skip ahead to its next event */
                if (!is0B && tgt == start_pc && (group == 2 || ea >= 0xFFC0))
                    d->idle_hint = 1;
            }
            return;
        }
        /* 0A: bit5 0 BCLR 1 BSET; 0B: bit5 0 BCHG 1 BTST */
        int k = is0B ? (bit5 ? 3 : 2) : (bit5 ? 1 : 0);
        uint32_t nv = bitop(d, v, b, k);
        if (k != 3) WR(space, ea, nv);
        return;
    }

    case 0x0C:
        if ((op & 0xFFF000) == 0x0C0000) {
            d->pc = (uint16_t)(op & 0xFFF);
            if (d->pc == start_pc)
                d->idle_hint = 1;             /* "jmp *" (short form) */
            return;
        }
        illegal(d);
        return;
    case 0x0D:
        if ((op & 0xFFF000) == 0x0D0000) { do_jsr(d, (uint16_t)(op & 0xFFF)); return; }
        illegal(d);
        return;
    case 0x0E:
        if (cond(d, (op >> 12) & 0xF)) d->pc = (uint16_t)(op & 0xFFF);
        return;
    case 0x0F:
        if (cond(d, (op >> 12) & 0xF)) do_jsr(d, (uint16_t)(op & 0xFFF));
        return;
    }
    (void)start_pc;
    illegal(d);
}

static void execute(dsp56k_t *d, uint32_t op)
{
    uint16_t start_pc = (uint16_t)(d->pc - 1);

    int parallel = op >= 0x100000 ||
                   (((op >> 17) == 0x04) && !(op & 0x4000));  /* $08/$09 class */
    if (parallel) {
        pend_t p;
        p.n = 0;
        if (!par_move(d, (op >> 8) & 0xFFFF, &p)) { illegal(d); return; }
        if (!alu_op(d, op & 0xFF)) { illegal(d); return; }
        pend_commit(d, &p);
    } else {
        exec_nonpar(d, op, start_pc);
    }
}

/* ------------------------------------------------------------------ */
/* Run loop                                                            */
/* ------------------------------------------------------------------ */
void dsp56k_run(dsp56k_t *d, uint32_t cycles)
{
    uint64_t end = d->cycles + cycles;
    while (d->cycles < end) {
        if (d->irq_pending)
            service_irq(d);
        if (d->halted) {
            d->cycles = end;
            return;
        }
        uint16_t spc = d->pc;
        uint32_t op = fetch(d);
        execute(d, op);
        if (d->idle_hint) {
            d->idle_hint = 0;
            d->idle_skips++;
            d->cycles = end;
            return;
        }
        /* the loop-end test uses the address of the instruction's last
         * word; a jump or interrupt has already moved the PC on */
        uint16_t last = (uint16_t)(spc + ((d->pc == (uint16_t)(spc + 2)) ? 1 : 0));
        if ((d->sr & SR_LF) && (d->pc == (uint16_t)(spc + 1) || d->pc == (uint16_t)(spc + 2)))
            loop_end_check(d, last);
    }
}

/* ------------------------------------------------------------------ */
/* Init / reset                                                        */
/* ------------------------------------------------------------------ */
static uint32_t frac24(double v)
{
    double s = v * 8388608.0;
    if (s > 8388607.0) s = 8388607.0;
    if (s < -8388608.0) s = -8388608.0;
    long iv = lround(s);
    return (uint32_t)iv & M24;
}

void dsp56k_init(dsp56k_t *d)
{
    void (*pw)(void *, int, uint16_t, uint32_t) = d->periph_write;
    uint32_t (*pr)(void *, int, uint16_t) = d->periph_read;
    void *ctx = d->ctx;
    memset(d, 0, sizeof *d);
    d->periph_write = pw;
    d->periph_read = pr;
    d->ctx = ctx;

    /* Y ROM: one full sine period */
    for (int i = 0; i < 256; i++)
        d->yrom[i] = frac24(sin(2.0 * M_PI * i / 256.0));
    /* X ROM: mu-law ($100-$17F) and A-law ($180-$1FF) expansion, positive
     * magnitudes, 13/14-bit linear left-justified into 24 bits */
    for (int i = 0; i < 128; i++) {
        int u = (~i) & 0x7F;
        int t = ((u & 0x0F) << 3) + 0x84;
        t <<= (u & 0x70) >> 4;
        int mu = t - 0x84;                                 /* 0..8031 */
        d->xrom[i] = ((uint32_t)mu << 10) & M24;
        int a = i ^ 0x55;
        int seg = (a & 0x70) >> 4;
        int val = (a & 0x0F) << 4;
        val = seg == 0 ? val + 8 : seg == 1 ? val + 0x108 : (val + 0x108) << (seg - 1);
        d->xrom[128 + i] = ((uint32_t)val << 11) & M24;
    }
    dsp56k_reset(d);
}

void dsp56k_reset(dsp56k_t *d)
{
    d->pc = 0;
    d->sr = 0x0300;                    /* interrupts masked */
    d->omr = 0x0002;                   /* mode 2: normal expanded */
    d->sp = 0;
    d->la = 0xFFFF;
    d->lc = 0;
    for (int i = 0; i < 8; i++)
        d->m[i] = 0xFFFF;
    d->irq_pending = 0;
    d->halted = 0;
    d->in_fast_irq = 0;
    d->rep_active = 0;
}
