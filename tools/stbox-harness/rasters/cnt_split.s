| Palette split by polling the video counter: VBL sets colour 0 blue; the
| main loop waits for the counter to reach base + 100 lines and sets red.
| Real ST: rows 0-99 blue, 100-199 red.
    .include "common.s"
    lea     vbl(%pc),%a0
    move.l  %a0,0x70:w
    move.w  #0x2300,%sr
main:
    tst.w   FLAG:w
    beq.s   main
    clr.w   FLAG:w
1:  move.b  VCMI,%d0
    lsl.w   #8,%d0
    move.b  VCLO,%d0
    cmp.w   #0x3e80,%d0
    blo.s   1b
    move.w  #0x700,PAL
    bra.s   main
vbl: move.w #0x007,PAL
    move.w  #1,FLAG:w
    rte
