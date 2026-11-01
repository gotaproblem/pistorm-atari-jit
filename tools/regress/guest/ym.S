| ym.S - YM.PRG: the YM2149 (the real chip; the emulator also snoops these
| writes for its own YM sound).
|   ym_regs   registers 0-13 read back what was written, two patterns,
|             masked to each register's width; port A/B direction bits
|             in R7 are kept, R14/R15 (floppy select, printer) untouched
| + metric: ticks for 2000 select/write/select/read rounds
| The volumes are zeroed first, so nothing is heard, and everything is
| restored at the end.

        .include "lib.S"

progname: .asciz "YM"
        .even

YMSEL   =       0xFFFF8800
YMWR    =       0xFFFF8802

masks:  .byte   0xFF,0x0F,0xFF,0x0F,0xFF,0x0F,0x1F,0x3F
        .byte   0x1F,0x1F,0x1F,0xFF,0xFF,0x0F
        .even

| ymw: d0 = register, d1 = value     ymr: d0 = register -> d0 = value
ymw:    move.b  d0,YMSEL
        move.b  d1,YMWR
        rts
ymr:    move.b  d0,YMSEL
        move.b  YMSEL,d0
        and.l   #0xFF,d0
        rts

main:
        move.w  sr,-(sp)
        or.w    #0x0700,sr              | TOS's sound timer stays out
        lea     saved,a2                | save R0-R13
        moveq   #0,d2
1:      move.l  d2,d0
        bsr     ymr
        move.b  d0,(a2)+
        addq.l  #1,d2
        cmp.l   #14,d2
        bne.s   1b
        moveq   #8,d0                   | silence
        moveq   #0,d1
        bsr     ymw
        moveq   #9,d0
        bsr     ymw
        moveq   #10,d0
        bsr     ymw

        moveq   #0,d6                   | failures
        move.l  #0x55,d3
        bsr     pass
        move.l  #0xAA,d3
        bsr     pass
        move.l  #0x33,d3
        bsr     pass

        move.w  #0x2300,sr              | speed: the 200 Hz tick must run
        bsr     now
        move.l  d0,d4
        move.w  #1999,d5
2:      moveq   #11,d0
        move.b  d5,d1
        bsr     ymw
        moveq   #11,d0
        bsr     ymr
        dbra    d5,2b
        bsr     now
        sub.l   d4,d0
        move.l  d0,d4
        or.w    #0x0700,sr

        lea     saved,a2                | restore: volumes last
        moveq   #0,d2
3:      cmp.l   #8,d2
        blo.s   4f
        cmp.l   #10,d2
        bls.s   5f
4:      move.l  d2,d0
        move.b  0(a2,d2.w),d1
        bsr     ymw
5:      addq.l  #1,d2
        cmp.l   #14,d2
        bne.s   3b
        moveq   #8,d2
6:      move.l  d2,d0
        move.b  0(a2,d2.w),d1
        bsr     ymw
        addq.l  #1,d2
        cmp.l   #11,d2
        bne.s   6b
        move.w  (sp)+,sr

        move.l  d4,d0
        METRIC  ym_2000_rounds_ticks
        tst.l   d6
        CHECK   eq, ym_regs
        PS      "mismatches="
        move.l  d6,d0
        bsr     pr_dec
        PS      " first=R"
        moveq   #0,d0
        move.b  firstbad,d0
        bsr     pr_dec
        PS      " wrote="
        move.b  firstw,d0
        bsr     pr_hex
        PS      " read="
        move.b  firstr,d0
        bsr     pr_hex
        bra     pr_flush

| pass: d3 = pattern; every register but the volumes (kept 0) and R7's
| top two bits (kept as they were) gets pattern & mask, read back
pass:
        lea     masks(pc),a1
        lea     saved,a2
        moveq   #0,d2
1:      cmp.l   #8,d2
        blo.s   2f
        cmp.l   #10,d2
        bls.s   9f                      | volumes: leave at 0
2:      move.b  d3,d1
        and.b   0(a1,d2.w),d1
        cmp.l   #7,d2
        bne.s   3f
        move.b  7(a2),d0                | R7: keep the port directions
        and.b   #0xC0,d0
        or.b    #0x3F,d1                | and all channels off
        and.b   #0x3F,d1
        or.b    d0,d1
3:      move.l  d2,d0
        bsr     ymw
        move.l  d2,d0
        bsr     ymr
        moveq   #-1,d7
        move.b  0(a1,d2.w),d7
        cmp.l   #7,d2
        bne.s   4f
        move.b  #0xFF,d7
4:      and.b   d7,d0
        and.b   d7,d1
        cmp.b   d1,d0
        beq.s   9f
        tst.l   d6
        bne.s   5f
        move.b  d2,firstbad
        move.b  d1,firstw
        move.b  d0,firstr
5:      addq.l  #1,d6
9:      addq.l  #1,d2
        cmp.l   #14,d2
        bne.s   1b
        rts

        .bss
saved:  .space  14
firstbad: .space 1
firstw: .space  1
firstr: .space  1
        .even
        .text
