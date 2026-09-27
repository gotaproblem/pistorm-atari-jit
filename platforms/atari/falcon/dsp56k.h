// SPDX-License-Identifier: MIT
/*
 * dsp56k - a DSP56001 interpreter (clean-room, written from the Motorola
 * programming model; no code from other emulators).
 *
 * The core knows the Falcon's memory wiring (below) and nothing else about
 * the machine: peripherals (host interface, SSI, ...) live in falcon_dsp.c
 * behind the periph_read/periph_write hooks, and raise interrupts with
 * dsp56k_irq_raise().
 *
 * Falcon wiring (Falcon specification 1992, table 6.1): 32K x 24 external
 * SRAM. P:$0000-$7FFF is all of it; Y:$0000-$3FFF is P:$0000-$3FFF and
 * X:$0000-$3FFF is P:$4000-$7FFF; X/Y:$4000-$7FFF shadow their lower half.
 * Internal P RAM $000-$1FF, internal X/Y RAM $000-$0FF, data ROMs at
 * $100-$1FF when OMR.DE is set. X/Y:$FFC0-$FFFF are the peripherals.
 */
#ifndef DSP56K_H
#define DSP56K_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_SPACE_X 0
#define DSP_SPACE_Y 1
#define DSP_SPACE_P 2

/* interrupt vectors (P-memory addresses) */
#define DSP_VEC_RESET     0x00
#define DSP_VEC_STACKERR  0x02
#define DSP_VEC_TRACE     0x04
#define DSP_VEC_SWI       0x06
#define DSP_VEC_IRQA      0x08
#define DSP_VEC_IRQB      0x0A
#define DSP_VEC_SSI_RX    0x0C
#define DSP_VEC_SSI_RXE   0x0E
#define DSP_VEC_SSI_TX    0x10
#define DSP_VEC_SSI_TXE   0x12
#define DSP_VEC_HOST_RX   0x20
#define DSP_VEC_HOST_TX   0x22
#define DSP_VEC_HOST_CMD  0x24
#define DSP_VEC_ILLEGAL   0x3E

/* status register */
#define SR_C   0x0001
#define SR_V   0x0002
#define SR_Z   0x0004
#define SR_N   0x0008
#define SR_U   0x0010
#define SR_E   0x0020
#define SR_L   0x0040
#define SR_S   0x0080
#define SR_I0  0x0100
#define SR_I1  0x0200
#define SR_S0  0x0400
#define SR_S1  0x0800
#define SR_T   0x2000
#define SR_LF  0x8000

typedef struct dsp56k dsp56k_t;

struct dsp56k {
    /* data ALU */
    uint32_t x0, x1, y0, y1;     /* 24 bit                                */
    int64_t  a, b;               /* 56 bit, kept sign-extended to 64      */
    /* AGU */
    uint16_t r[8], n[8], m[8];
    /* program control */
    uint16_t pc;
    uint16_t sr;
    uint16_t omr;
    uint16_t la, lc;
    uint8_t  sp;                 /* P3..P0 stack pointer, bit4 SE, bit5 UF */
    uint16_t ssh[16], ssl[16];

    /* memory */
    uint32_t ext[0x8000];        /* the Falcon's 32K words of SRAM        */
    uint32_t pint[0x200];
    uint32_t xint[0x100], yint[0x100];
    uint32_t xrom[0x100], yrom[0x100];

    /* peripherals: X/Y $FFC0-$FFFF */
    uint32_t (*periph_read)(void *ctx, int space, uint16_t addr);
    void     (*periph_write)(void *ctx, int space, uint16_t addr, uint32_t v);
    void      *ctx;

    /* interrupts: one pending bit per vector/2, with its priority 1..3 */
    uint64_t  irq_pending;       /* bit (vector >> 1)                     */
    uint8_t   irq_level[32];     /* priority for each pending source      */

    /* run state */
    uint64_t  cycles;
    int       halted;            /* STOP / WAIT: until an interrupt       */
    int       rep_active;        /* executing a REP                       */
    uint16_t  rep_lc_save;
    uint32_t  rep_op, rep_ext;   /* the repeated instruction             */
    uint16_t  rep_pc_next;
    int       in_fast_irq;       /* executing a fast interrupt's 2 words  */
    int       accept_level;      /* level of the interrupt being taken    */
    uint16_t  irq_ret_pc;
    uint16_t  irq_end_pc;
    uint32_t  illegal_count;
    int       idle_hint;         /* polling a peripheral in place         */
    uint64_t  idle_skips;
};

void     dsp56k_init(dsp56k_t *d);       /* ROM tables, power-on state   */
void     dsp56k_reset(dsp56k_t *d);      /* hardware reset (not memory)  */
/* run at least `cycles` DSP clock cycles (2 per instruction word) */
void     dsp56k_run(dsp56k_t *d, uint32_t cycles);
void     dsp56k_irq_raise(dsp56k_t *d, int vector, int level);
void     dsp56k_irq_clear(dsp56k_t *d, int vector);

uint32_t dsp56k_mem_read(dsp56k_t *d, int space, uint16_t addr);
void     dsp56k_mem_write(dsp56k_t *d, int space, uint16_t addr, uint32_t v);

#ifdef __cplusplus
}
#endif

#endif /* DSP56K_H */
