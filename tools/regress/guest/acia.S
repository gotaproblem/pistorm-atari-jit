| acia.S - ACIA.PRG: the keyboard ACIA and the real IKBD.
|
| Takes over the ACIA interrupt (GPIP4, vector $118), stops mouse and
| joystick reports, then asks the IKBD questions with fixed-format answers
| and checks every byte comes back, in order, once, with no ACIA errors:
|   ikbd_tod      20 x $1C interrogate time-of-day -> $FC + 6 BCD bytes
|   ikbd_mouse    20 x $87 interrogate mouse button action -> $F6 + 7 bytes
|   acia_clean    no framing/overrun/parity errors, no interrupt that
|                 found nothing to read, nothing left pending afterwards
| The IKBD is reset at the end, so TOS gets its usual mouse back.

        .include "lib.S"

progname: .asciz "ACIA"
        .even

ACIAC   =       0xFFFFFC00
ACIAD   =       0xFFFFFC02
GPIP    =       0xFFFFFA01
ISRB    =       0xFFFFFA11
V_ACIA  =       0x118
RING    =       512

main:
        bsr     save_mfp
        move.w  #0x2700,sr
        move.l  #acia_h,V_ACIA
        clr.l   nbytes
        clr.l   nentries
        clr.l   nempty
        clr.l   nerrors
        move.w  #0x2300,sr

        moveq   #0x12,d0                | mouse off
        bsr     send
        moveq   #0x1A,d0                | joysticks off
        bsr     send
        moveq   #20,d0                  | 100 ms: anything in flight lands
        bsr     wait_ticks
        clr.l   nbytes                  | count from here
        clr.l   nentries
        clr.l   nempty
        clr.l   nerrors
        clr.w   rd
        clr.w   wr

        bsr     now
        move.l  d0,t0
        moveq   #0x1C,d1                | time-of-day: $FC + 6
        moveq   #7,d2
        move.l  #0xFC,d3
        bsr     ask20
        move.l  d0,d6                   | good packets
        moveq   #0x87-256,d1            | mouse button action: $F6 + 7
        and.l   #0xFF,d1
        moveq   #8,d2
        move.l  #0xF6,d3
        bsr     ask20
        move.l  d0,d7
        bsr     now
        sub.l   t0,d0
        METRIC  ikbd_40_questions_ticks

        moveq   #10,d0
        bsr     wait_ticks
        moveq   #0,d5                   | GPIP4 still low = a byte stuck
        btst    #4,GPIP
        bne.s   1f
        moveq   #1,d5
1:
        move.l  d6,d0
        cmp.l   #20,d0
        CHECK   eq, ikbd_tod
        PS      "packets="
        move.l  d6,d0
        bsr     pr_dec
        PS      " expect=20"
        bsr     pr_flush
        move.l  d7,d0
        cmp.l   #20,d0
        CHECK   eq, ikbd_mouse
        PS      "packets="
        move.l  d7,d0
        bsr     pr_dec
        PS      " expect=20"
        bsr     pr_flush

        move.l  nbytes,d0
        METRIC  acia_bytes
        move.l  nentries,d0
        METRIC  acia_irq_entries
        move.l  nempty,d0
        METRIC  acia_irq_empty
        move.l  nerrors,d0
        add.l   nempty,d0
        add.l   d5,d0
        tst.l   d0
        CHECK   eq, acia_clean
        PS      "bytes="
        move.l  nbytes,d0
        bsr     pr_dec
        PS      " expect=300 errors="
        move.l  nerrors,d0
        bsr     pr_dec
        PS      " empty-irqs="
        move.l  nempty,d0
        bsr     pr_dec
        PS      " stuck="
        move.l  d5,d0
        bsr     pr_dec
        bsr     pr_flush
        cmp.l   #300,nbytes
        CHECK   eq, acia_count
        PS      "bytes="
        move.l  nbytes,d0
        bsr     pr_dec
        PS      " expect=300"
        bsr     pr_flush

        moveq   #0x80-256,d0            | reset: mouse back on for TOS
        bsr     send
        moveq   #1,d0
        bsr     send
        moveq   #40,d0                  | 200 ms for the reset's reply
        bsr     wait_ticks
        bsr     restore_mfp
        rts

| ask20: d1 = command, d2 = reply length, d3 = header. Sends it 20 times,
| each time waiting (<= 250 ms) for the whole reply; d0 = good replies.
ask20:
        movem.l d1-d7,-(sp)
        moveq   #0,d7
        moveq   #19,d6
1:      move.l  d1,d0
        bsr     send
        bsr     now
        move.l  d0,d4
        add.l   #50,d4
2:      bsr     avail
        cmp.l   d2,d0
        bhs.s   3f
        bsr     now
        cmp.l   d4,d0
        bmi.s   2b
        bra.s   5f                      | timed out: drop what came
3:      bsr     get                     | header
        cmp.b   d3,d0
        seq     d5
        move.l  d2,d4
        subq.l  #2,d4
4:      bsr     get                     | the rest; time-of-day must be BCD
        cmp.b   #0xFC,d3
        bne.s   6f
        move.b  d0,d1
        and.b   #0x0F,d1
        cmp.b   #9,d1
        bls.s   7f
        sf      d5
7:      lsr.b   #4,d0
        cmp.b   #9,d0
        bls.s   6f
        sf      d5
6:      dbra    d4,4b
        tst.b   d5
        beq.s   5f
        addq.l  #1,d7
5:      move.l  (sp),d1                 | d1 again (avail/get keep it, but
        moveq   #3,d0                   |  be safe) - 15 ms between asks
        bsr     wait_ticks
        dbra    d6,1b
        move.l  d7,d0
        movem.l (sp)+,d1-d7
        rts

| send: d0 = byte to the IKBD
send:
        btst    #1,ACIAC
        beq.s   send
        move.b  d0,ACIAD
        rts

| avail: d0 = bytes in the ring
avail:
        move.w  wr,d0
        sub.w   rd,d0
        and.l   #RING-1,d0
        rts

| get: d0 = next byte from the ring (call only when avail > 0)
get:
        move.l  a0,-(sp)
        lea     ring,a0
        move.w  rd,d0
        move.b  0(a0,d0.w),d0
        and.l   #0xFF,d0
        move.l  d0,-(sp)
        move.w  rd,d0
        addq.w  #1,d0
        and.w   #RING-1,d0
        move.w  d0,rd
        move.l  (sp)+,d0
        move.l  (sp)+,a0
        rts

| the ACIA interrupt: drain the receiver while GPIP4 says it is asking
acia_h:
        movem.l d0-d1/a0,-(sp)
        addq.l  #1,nentries
        moveq   #0,d1                   | bytes this entry
1:      move.b  ACIAC,d0
        btst    #0,d0
        beq.s   2f
        and.b   #0x70,d0                | FE, OVRN, PE
        beq.s   3f
        addq.l  #1,nerrors
3:      move.b  ACIAD,d0
        lea     ring,a0
        move.w  wr,d1
        move.b  d0,0(a0,d1.w)
        addq.w  #1,d1
        and.w   #RING-1,d1
        move.w  d1,wr
        addq.l  #1,nbytes
        moveq   #1,d1
        bra.s   1b
2:      tst.w   d1
        bne.s   4f
        btst    #4,GPIP                 | nothing read: was there anything?
        beq.s   4f                      | GPIP4 still low - the MIDI ACIA?
        addq.l  #1,nempty
4:      btst    #4,GPIP
        beq.s   1b                      | keep going while it asks
        move.b  #0xBF,ISRB
        movem.l (sp)+,d0-d1/a0
        rte

        .bss
        .even
ring:   .space  RING
rd:     .space  2
wr:     .space  2
t0:     .space  4
nbytes: .space  4
nentries: .space 4
nempty: .space  4
nerrors: .space 4
        .text
