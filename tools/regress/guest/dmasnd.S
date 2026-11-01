| dmasnd.S - DMASND.PRG: STE DMA sound (machine ste). Plays a silent
| 12518-byte mono frame at 12517 Hz on repeat - one frame a second.
|   dma_present   the sound registers exist (no bus error at $FF8900)
|   dma_frames    a frame-end event per frame: Timer A in event-count mode
|                 (the STE wires the end-of-frame signal to its input)
|                 counts 4 frames in 800 ticks (4.000 s) +- 2%
|   dma_counter   the frame address counter stays inside the frame and
|                 moves ~6259 bytes in half a second (+- 3%)
|   dma_stop      clearing the play bit stops the counter

        .include "lib.S"

progname: .asciz "DMASND"
        .even

SCTL    =       0xFFFF8901
SBASEH  =       0xFFFF8903
SBASEM  =       0xFFFF8905
SBASEL  =       0xFFFF8907
SCNTH   =       0xFFFF8909
SCNTM   =       0xFFFF890B
SCNTL   =       0xFFFF890D
SENDH   =       0xFFFF890F
SENDM   =       0xFFFF8911
SENDL   =       0xFFFF8913
SMODE   =       0xFFFF8921
IERA    =       0xFFFFFA07
ISRA    =       0xFFFFFA0F
IMRA    =       0xFFFFFA13
TACR    =       0xFFFFFA19
TADR    =       0xFFFFFA1F
V_TA    =       0x134
FLEN    =       12518

main:
        bsr     save_mfp
        move.w  #0x2300,sr
        lea     0xFFFF8900,a0
        bsr     probe_w
        tst.l   d0
        CHECK   eq, dma_present
        PS      "sound registers at $FF8900"
        bsr     pr_flush
        tst.l   d0
        bne     9f

        lea     frame,a0                | silence
        move.w  #FLEN/4,d0
1:      clr.l   (a0)+
        dbra    d0,1b
        clr.b   SCTL
        move.l  #frame,d0
        bsr     setstart
        move.l  #frame+FLEN,d0
        bsr     setend
        move.b  #0x81,SMODE             | mono, 12517 Hz

        move.w  #0x2700,sr              | Timer A: one interrupt per frame end
        clr.l   events
        clr.l   ev_first
        clr.l   ev_fifth
        clr.b   TACR
        move.b  #1,TADR
        move.l  #ta_h,V_TA
        bset    #5,IERA
        bset    #5,IMRA
        move.b  #8,TACR
        move.w  #0x2300,sr
        move.b  #3,SCTL                 | play, repeat

        moveq   #100,d0                 | half a second in: the counter
        bsr     wait_ticks
        bsr     getcnt
        move.l  d0,d6
        moveq   #100,d0
        bsr     wait_ticks
        bsr     getcnt
        move.l  d0,d7

        move.l  HZ200,d5                | up to 8 s for five frame ends
        add.l   #1600,d5
2:      cmp.l   #5,events
        bhs.s   3f
        cmp.l   HZ200,d5
        bpl.s   2b
3:
        clr.b   SCTL                    | stop
        moveq   #4,d0
        bsr     wait_ticks
        bsr     getcnt
        move.l  d0,d4
        moveq   #20,d0
        bsr     wait_ticks
        bsr     getcnt
        move.l  d0,d3
        move.w  #0x2700,sr
        clr.b   TACR
        bclr    #5,IERA
        move.w  #0x2300,sr

        move.l  events,d0
        METRIC  dma_frame_events
        move.l  ev_fifth,d0
        sub.l   ev_first,d0
        move.l  d0,d5
        METRIC  dma_4frames_ticks
        moveq   #0,d2
        cmp.l   #5,events
        blo.s   4f
        cmp.l   #784,d5
        blo.s   4f
        cmp.l   #816,d5
        bhi.s   4f
        moveq   #1,d2
4:      tst.l   d2
        CHECK   ne, dma_frames
        PS      "events="
        move.l  events,d0
        bsr     pr_dec
        PS      " 4 frames in ticks="
        move.l  d5,d0
        bsr     pr_dec
        PS      " expect=800"
        bsr     pr_flush

        move.l  d7,d0                   | half-second advance, mod the frame
        sub.l   d6,d0
        bpl.s   5f
        add.l   #FLEN,d0
5:      move.l  d0,d5
        METRIC  dma_counter_halfsec
        moveq   #0,d2
        move.l  #frame,d0
        cmp.l   d0,d6
        blo.s   6f
        cmp.l   d0,d7
        blo.s   6f
        add.l   #FLEN,d0
        cmp.l   d0,d6
        bhi.s   6f
        cmp.l   d0,d7
        bhi.s   6f
        cmp.l   #6071,d5
        blo.s   6f
        cmp.l   #6447,d5
        bhi.s   6f
        moveq   #1,d2
6:      tst.l   d2
        CHECK   ne, dma_counter
        PS      "moved="
        move.l  d5,d0
        bsr     pr_dec
        PS      " expect=6259 at "
        move.l  d6,d0
        bsr     pr_hex
        PS      ","
        move.l  d7,d0
        bsr     pr_hex
        bsr     pr_flush

        cmp.l   d4,d3
        CHECK   eq, dma_stop
        PS      "counter "
        move.l  d4,d0
        bsr     pr_hex
        PS      " then "
        move.l  d3,d0
        bsr     pr_hex
        bsr     pr_flush
9:      bsr     restore_mfp
        rts

ta_h:   addq.l  #1,events
        cmp.l   #1,events
        bne.s   1f
        move.l  HZ200,ev_first
1:      cmp.l   #5,events
        bne.s   2f
        move.l  HZ200,ev_fifth
2:      move.b  #0xDF,ISRA
        rte

setstart:
        move.b  d0,SBASEL
        lsr.l   #8,d0
        move.b  d0,SBASEM
        lsr.l   #8,d0
        move.b  d0,SBASEH
        rts
setend:
        move.b  d0,SENDL
        lsr.l   #8,d0
        move.b  d0,SENDM
        lsr.l   #8,d0
        move.b  d0,SENDH
        rts
getcnt:
        moveq   #0,d0
        move.b  SCNTH,d0
        lsl.l   #8,d0
        move.b  SCNTM,d0
        lsl.l   #8,d0
        move.b  SCNTL,d0
        rts

        .bss
        .even
events: .space  4
ev_first: .space 4
ev_fifth: .space 4
frame:  .space  FLEN+16
        .text
