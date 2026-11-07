| Mid-line palette writes (Spectrum-512 style): lock to the beam on the
| video counter, then 199 lines of exactly 512 ST cycles (every
| instruction a multiple of 4 on the ST bus): colour 0 blue, 160 cycles
| later red. Real ST: every row blue on the left, red from the
| same x on, no jitter between rows.
    .include "common.s"
    lea     vbl(%pc),%a0
    move.l  %a0,0x70:w
    move.w  #0x2300,%sr
    lea     VCLO,%a0
    lea     PAL,%a1
    move.w  #0x007,%d2
    move.w  #0x700,%d3
main:
    tst.w   FLAG:w
    beq.s   main
    clr.w   FLAG:w
    move.w  #0x2700,%sr
    moveq   #0,%d0
1:  move.b  (%a0),%d0
    beq.s   1b
    moveq   #24,%d1
    sub.w   %d0,%d1
    lsl.w   %d1,%d1                 | 6 + 2n cycles: absorbs the jitter
    move.w  #198,%d7
2:  move.w  %d2,(%a1)               | 8
    .rept 40
    nop                             | 160
    .endr
    move.w  %d3,(%a1)               | 8
    .rept 81
    nop                             | 324
    .endr
    dbra    %d7,2b                  | 10, 12 on the ST bus -> 512
    move.w  #0x2300,%sr
    bra     main
vbl: move.w #0x000,PAL
    move.w  #1,FLAG:w
    rte
