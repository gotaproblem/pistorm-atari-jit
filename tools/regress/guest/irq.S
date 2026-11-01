| irq.S - IRQ.PRG: interrupt delivery and the MFP timers.
|
| Everything is counted against TOS's 200 Hz tick (MFP Timer C, the real
| chip's 2.4576 MHz crystal), so the expected counts are what a real ST
| gives, whatever speed the emulated CPU runs at.
|
|   irq_vbl        VBL rate: 100 per 2 s at 50 Hz (120 at 60 Hz)
|   irq_hbl        HBL per frame with the IPL at 1: 313 (50 Hz) / 263 (60)
|   tmr_a          Timer A delay mode 2457600/200/192 = 64 Hz
|   tmr_b          Timer B delay mode 2457600/64/96   = 400 Hz
|   tmr_d          Timer D delay mode 2457600/4/56    = 10971 Hz (sample rate)
|   tmr_a_fast     Timer A 51.2 kHz: metric only (fraction delivered)
|   tmr_b_event    Timer B event count, 1 per line: 200 per frame, every
|                  frame, with TOS's Timer C and ACIA handlers masked (they
|                  run at IPL 6 and can merge two lines' requests - a real
|                  ST under TOS 2.06 loses one a frame that way)
|                  + metrics: where in the line the handler read the video
|                  counter (0 = still in the border, as on a real ST)
|   irq_stop       stop #$2300 woken by VBL/Timer C, stop #$2100 by HBL
|   mfp_isr_block  software EOI: an in-service Timer D is not re-delivered
|                  until its ISR bit is cleared; automatic EOI keeps going

        .include "lib.S"

progname: .asciz "IRQ"
        .even

GPIP    =       0xFFFFFA01
IERA    =       0xFFFFFA07
IERB    =       0xFFFFFA09
IPRA    =       0xFFFFFA0B
IPRB    =       0xFFFFFA0D
ISRA    =       0xFFFFFA0F
ISRB    =       0xFFFFFA11
IMRA    =       0xFFFFFA13
IMRB    =       0xFFFFFA15
VR      =       0xFFFFFA17
TACR    =       0xFFFFFA19
TBCR    =       0xFFFFFA1B
TCDCR   =       0xFFFFFA1D
TADR    =       0xFFFFFA1F
TBDR    =       0xFFFFFA21
TDDR    =       0xFFFFFA25
SYNC    =       0xFFFF820A
VBASEH  =       0xFFFF8201
VBASEM  =       0xFFFF8203
VCNTM   =       0xFFFF8207
VCNTL   =       0xFFFF8209

V_TA    =       0x134                   | MFP vectors with VR base $40
V_TB    =       0x120
V_TD    =       0x110

main:
        bsr     save_mfp
        move.w  #0x2700,sr
        move.l  0x70,old_vbl
        move.l  #vbl_hook,0x70
        move.w  #0x2300,sr
        bsr     t_vbl
        bsr     t_hbl
        bsr     t_timers
        bsr     t_tb_event
        bsr     t_stop
        bsr     t_isr_block
        bsr     restore_mfp             | puts $70 back too
        rts

| ---------------------------------------------------------------- VBL hook
vbl_hook:
        addq.l  #1,vblcnt
        tst.l   vbl_extra
        beq.s   1f
        movem.l d0-d1/a0-a1,-(sp)
        move.l  vbl_extra,a0
        jsr     (a0)
        movem.l (sp)+,d0-d1/a0-a1
1:      move.l  old_vbl,-(sp)           | TOS's own VBL work carries on
        rts

| expected VBLs in 2 s: 100 at 50 Hz, 120 at 60 Hz -> d0
vbl_expect:
        moveq   #100,d0
        btst    #1,SYNC
        bne.s   1f
        moveq   #120,d0
1:      rts

| ---------------------------------------------------------------- irq_vbl
t_vbl:
        bsr     sync_tick
        move.l  vblcnt,d4
        move.l  #400,d0
        bsr     wait_ticks
        move.l  vblcnt,d0
        sub.l   d4,d0
        move.l  d0,d5
        METRIC  vbl_per_2s
        bsr     vbl_expect
        move.l  d0,d6
        move.l  d5,d0
        sub.l   d6,d0
        bpl.s   1f
        neg.l   d0
1:      cmp.l   #2,d0
        CHECK   ls, irq_vbl
        PS      "vbl="
        move.l  d5,d0
        bsr     pr_dec
        PS      " expect="
        move.l  d6,d0
        bsr     pr_dec
        bra     pr_flush

| ---------------------------------------------------------------- irq_hbl
hbl_h:  addq.l  #1,hblcnt
        rte

t_hbl:
        move.w  #0x2700,sr
        move.l  0x68,d7
        move.l  #hbl_h,0x68
        move.w  #0x2300,sr
        bsr     sync_tick
        clr.l   hblcnt
        move.l  vblcnt,d4
        move.w  #0x2100,sr              | HBL (level 2) let in
        move.l  #200,d0
        bsr     wait_ticks
        move.w  #0x2300,sr
        move.l  hblcnt,d5
        move.l  vblcnt,d6
        sub.l   d4,d6                   | frames
        move.w  #0x2700,sr
        move.l  d7,0x68
        move.w  #0x2300,sr
        move.l  d5,d0                   | HBL per frame x100
        mulu    #100,d0                 | (d5 < 65536 for 1 s)
        divu    d6,d0
        and.l   #0xFFFF,d0
        move.l  d0,d5
        METRIC  hbl_per_frame_x100
        move.l  #31300,d6
        btst    #1,SYNC
        bne.s   1f
        move.l  #26300,d6
1:      move.l  d5,d0
        sub.l   d6,d0
        bpl.s   2f
        neg.l   d0
2:      move.l  d6,d1
        divu    #33,d1                  | 3%: an HBL arriving during TOS's VBL
                                        | handler (IPL 4) merges with the next
                                        | on a real ST too (Hatari: 310.4)
        and.l   #0xFFFF,d1
        cmp.l   d1,d0
        CHECK   ls, irq_hbl
        PS      "hbl/frame x100="
        move.l  d5,d0
        bsr     pr_dec
        PS      " expect="
        move.l  d6,d0
        bsr     pr_dec
        bra     pr_flush

| ---------------------------------------------------------------- timers
| Software EOI handlers (TOS leaves VR at $48). Each counts and clears its
| own in-service bit.
ta_h:   addq.l  #1,tcnt
        move.b  #0xDF,ISRA
        rte
tb_h:   addq.l  #1,tcnt
        move.b  #0xFE,ISRA
        rte
td_h:   addq.l  #1,tcnt
        move.b  #0xEF,ISRB
        rte

| run_timer: a0 = handler, d1 = which (0 A, 1 B, 2 D), d2 = control,
| d3 = data; counts for 2 s -> d0
run_timer:
        move.w  #0x2700,sr
        clr.l   tcnt
        cmp.w   #1,d1
        beq.s   2f
        bhi.s   3f
        clr.b   TACR                    | A
        move.b  d3,TADR
        move.l  a0,V_TA
        bset    #5,IERA
        bset    #5,IMRA
        move.b  d2,TACR
        bra.s   4f
2:      clr.b   TBCR                    | B
        move.b  d3,TBDR
        move.l  a0,V_TB
        bset    #0,IERA
        bset    #0,IMRA
        move.b  d2,TBCR
        bra.s   4f
3:      move.b  TCDCR,d0                | D: keep Timer C's bits
        and.b   #0xF0,d0
        move.b  d0,TCDCR
        move.b  d3,TDDR
        move.l  a0,V_TD
        bset    #4,IERB
        bset    #4,IMRB
        or.b    d2,d0
        move.b  d0,TCDCR
4:      move.w  #0x2500,sr              | MFP (level 6) in
        bsr     sync_tick
        clr.l   tcnt
        move.l  #400,d0
        bsr     wait_ticks
        move.w  #0x2700,sr
        move.l  tcnt,d4
        bsr     stop_timers
        move.w  #0x2300,sr
        move.l  d4,d0
        rts

stop_timers:
        clr.b   TACR
        clr.b   TBCR
        move.b  TCDCR,d0
        and.b   #0xF0,d0
        move.b  d0,TCDCR
        bclr    #5,IERA
        bclr    #0,IERA
        bclr    #4,IERB
        move.b  #0xDE,IPRA
        move.b  #0xEF,IPRB
        move.b  #0xDE,ISRA
        move.b  #0xEF,ISRB
        rts

| check_rate: d0 = counted, d6 = expected, tolerance 0.5% (min 2);
| a1 = name for the line. Leaves the CHECK line flushed.
check_rate:
        move.l  d0,d5
        move.l  d6,d1
        divu    #200,d1
        and.l   #0xFFFF,d1
        cmp.l   #2,d1
        bhs.s   1f
        moveq   #2,d1
1:      sub.l   d6,d0
        bpl.s   2f
        neg.l   d0
2:      cmp.l   d1,d0
        bls.s   3f
        PS      "@T "
        move.l  a1,a0
        bsr     pr_str
        PS      " FAIL "
        bra.s   4f
3:      PS      "@T "
        move.l  a1,a0
        bsr     pr_str
        PS      " PASS "
4:      PS      "count="
        move.l  d5,d0
        bsr     pr_dec
        PS      " expect="
        move.l  d6,d0
        bsr     pr_dec
        bra     pr_flush

t_timers:
        lea     ta_h,a0                 | Timer A /200, 192 -> 64 Hz
        moveq   #0,d1
        moveq   #7,d2
        move.l  #192,d3
        bsr     run_timer
        move.l  #128,d6
        lea     n_ta(pc),a1
        bsr     check_rate

        lea     tb_h,a0                 | Timer B /64, 96 -> 400 Hz
        moveq   #1,d1
        moveq   #5,d2
        moveq   #96,d3
        bsr     run_timer
        move.l  #800,d6
        lea     n_tb(pc),a1
        bsr     check_rate

        lea     td_h,a0                 | Timer D /4, 56 -> 10971.43 Hz
        moveq   #2,d1
        moveq   #1,d2
        moveq   #56,d3
        bsr     run_timer
        move.l  #21943,d6
        lea     n_td(pc),a1
        bsr     check_rate

        lea     ta_h,a0                 | Timer A /4, 12 -> 51200 Hz
        moveq   #0,d1
        moveq   #1,d2
        moveq   #12,d3
        bsr     run_timer
        move.l  d0,d1                   | per mille of 102400:
        lsl.l   #2,d0                   | count*1000/102400 = count*5/512
        add.l   d1,d0
        lsr.l   #8,d0
        lsr.l   #1,d0
        METRIC  tmr_a_fast_permille
        rts

n_ta:   .asciz  "tmr_a"
n_tb:   .asciz  "tmr_b"
n_td:   .asciz  "tmr_d"
        .even

| ---------------------------------------------------------------- Timer B event
| One interrupt per line (TBCR=8 event count, TBDR=1). The handler reads
| the Shifter's video counter: at the end of line k of the picture it
| holds base + k*160 until the next line's display starts, so anything
| above that is how far into the NEXT line the handler got (bytes; one
| byte = 2 low-res pixels).
tbe_h:
        movem.l d0-d1,-(sp)
        move.b  VCNTM,d0
        lsl.w   #8,d0
        move.b  VCNTL,d0
        move.w  d0,tbe_lcnt             | the frame's last one so far
        move.w  tbe_line,d1
        addq.w  #1,d1
        move.w  d1,tbe_line
        cmp.w   #1,d1
        bne.s   2f
        move.w  d0,tbe_fcnt             | the frame's first
2:
        mulu    #160,d1
        add.w   tbe_base,d1
        sub.w   d1,d0                   | bytes past the line's end
        beq.s   1f
        cmp.w   #160,d0                 | a carry between the two reads or
        bhs.s   1f                      | a counter not yet moved: ignore
        addq.l  #1,tbe_inpic
        cmp.w   tbe_maxoff,d0
        bls.s   1f
        move.w  d0,tbe_maxoff
1:      addq.l  #1,tbe_total
        movem.l (sp)+,d0-d1
        move.b  #0xFE,ISRA
        rte

| at each VBL: record the line count of the frame that ended
tbe_vbl:
        move.w  tbe_line,d0
        clr.w   tbe_line
        tst.w   tbe_frames_skip
        beq.s   1f
        subq.w  #1,tbe_frames_skip
        rts
1:      addq.w  #1,tbe_frames
        cmp.w   tbe_min,d0
        bhs.s   2f
        move.w  d0,tbe_min
2:      cmp.w   tbe_max,d0
        bls.s   3f
        move.w  d0,tbe_max
3:
| Which screen lines the frame's first and last interrupt came on: the
| counter / 160 from the screen base (line k = the end of display line k).
| Kept per kind of frame - 199 lines, 200 lines, anything else - so a
| frame that gains or loses one shows WHERE: at the top or the bottom.
        lea     tbe_cat,a0
        cmp.w   #199,d0
        beq.s   4f
        lea     10(a0),a0
        cmp.w   #200,d0
        beq.s   4f
        lea     10(a0),a0
4:      addq.w  #1,(a0)
        tst.w   d0
        beq.s   7f                      | no interrupt at all this frame
        move.w  tbe_fcnt,d0
        bsr.s   cnt2line
        cmp.w   2(a0),d0
        bhs.s   5f
        move.w  d0,2(a0)
5:      cmp.w   4(a0),d0
        bls.s   6f
        move.w  d0,4(a0)
6:      move.w  tbe_lcnt,d0
        bsr.s   cnt2line
        cmp.w   6(a0),d0
        bhs.s   8f
        move.w  d0,6(a0)
8:      cmp.w   8(a0),d0
        bls.s   7f
        move.w  d0,8(a0)
7:      rts

| cnt2line: d0 = video counter low 16 bits -> d0 = line number
cnt2line:
        sub.w   tbe_base,d0
        and.l   #0xFFFF,d0
        divu    #160,d0
        and.l   #0xFFFF,d0
        rts

| tbe_report: an @I line per kind of frame that occurred
tbe_report:
        lea     tbe_cat,a2
        lea     tbe_catname(pc),a3
        moveq   #2,d3
1:      tst.w   (a2)
        beq     2f
        PS      "@I tmr_b_event "
        move.l  a3,a0
        bsr     pr_str
        PS      " frames="
        moveq   #0,d0
        move.w  (a2),d0
        bsr     pr_dec
        PS      " first-line="
        moveq   #0,d0
        move.w  2(a2),d0
        bsr     pr_dec
        PS      ".."
        moveq   #0,d0
        move.w  4(a2),d0
        bsr     pr_dec
        PS      " last-line="
        moveq   #0,d0
        move.w  6(a2),d0
        bsr     pr_dec
        PS      ".."
        moveq   #0,d0
        move.w  8(a2),d0
        bsr     pr_dec
        bsr     pr_flush
2:      lea     10(a2),a2
3:      tst.b   (a3)+                   | next name
        bne.s   3b
        dbra    d3,1b
        rts
tbe_catname:
        .asciz  "199-line"
        .asciz  "200-line"
        .asciz  "other"
        .even

t_tb_event:
        moveq   #0,d0
        move.b  0xFFFFFA03,d0           | AER: bit 3 picks Timer B's DE edge
        METRIC  mfp_aer
        move.w  #0x2700,sr
        moveq   #0,d0                   | screen base, low 16 bits
        move.b  VBASEM,d0
        lsl.w   #8,d0
        move.w  d0,tbe_base
        clr.w   tbe_line
        clr.w   tbe_frames
        move.w  #3,tbe_frames_skip
        move.w  #0xFFFF,tbe_min
        clr.w   tbe_max
        clr.w   tbe_maxoff
        clr.l   tbe_inpic
        clr.l   tbe_total
        lea     tbe_cat,a0              | per kind: n, first min/max, last min/max
        moveq   #2,d0
1:      clr.w   (a0)+
        move.w  #0xFFFF,(a0)+
        clr.w   (a0)+
        move.w  #0xFFFF,(a0)+
        clr.w   (a0)+
        dbra    d0,1b
        clr.b   TBCR
        move.b  #1,TBDR
        move.l  #tbe_h,V_TB
        bset    #0,IERA
        bset    #0,IMRA
        move.l  #tbe_vbl,vbl_extra
        move.b  IMRB,d7                 | TOS's own level-6 handlers out:
        and.b   #0x9F,IMRB              | Timer C (B5) and the ACIA (B6). A
                                        | 200 Hz tick handler that runs longer
                                        | than a line at IPL 6 merges two Timer
                                        | B requests on a real ST too (Hatari,
                                        | TOS 2.06: line 1..200, 199 interrupts)
        move.b  #8,TBCR                 | event count mode
        move.w  #0x2300,sr              | VBL (4) and Timer B (6) in
        move.l  vblcnt,d0               | 100 frames, counted by VBL - the
        add.l   #100,d0                 | 200 Hz tick is masked
1:      cmp.l   vblcnt,d0
        bhi.s   1b
        move.w  #0x2700,sr
        move.b  d7,IMRB                 | the held Timer C/ACIA come in now
        clr.l   vbl_extra
        bsr     stop_timers
        move.w  #0x2300,sr

        moveq   #0,d0
        move.w  tbe_min,d0
        METRIC  tb_lines_min
        moveq   #0,d0
        move.w  tbe_max,d0
        METRIC  tb_lines_max
        move.l  tbe_inpic,d0            | in the picture, per mille
        mulu    #1000,d0
        move.l  tbe_total,d1
        beq.s   1f
        bsr     udiv32
1:      METRIC  tb_in_picture_permille
        moveq   #0,d0
        move.w  tbe_maxoff,d0
        METRIC  tb_max_bytes_into_line
        bsr     tbe_report

        cmp.w   #200,tbe_min            | 200 every frame: with nothing else
        bne.s   2f                      | at level 6, each display line's
        cmp.w   #200,tbe_max            | interrupt must reach the CPU
2:      CHECK   eq, tmr_b_event
        PS      "lines/frame min="
        moveq   #0,d0
        move.w  tbe_min,d0
        bsr     pr_dec
        PS      " max="
        moveq   #0,d0
        move.w  tbe_max,d0
        bsr     pr_dec
        PS      " frames="
        moveq   #0,d0
        move.w  tbe_frames,d0
        bsr     pr_dec
        PS      " expect=200"
        bra     pr_flush

| udiv32: d0 = d0 / d1 (unsigned 32/32, d1 nonzero), slow and simple
udiv32:
        movem.l d1-d3,-(sp)
        moveq   #0,d2                   | quotient
        moveq   #0,d3                   | remainder
        move.l  #31,-(sp)
1:      add.l   d0,d0
        addx.l  d3,d3
        add.l   d2,d2
        cmp.l   d1,d3
        blo.s   2f
        sub.l   d1,d3
        addq.l  #1,d2
2:      subq.l  #1,(sp)
        bpl.s   1b
        addq.l  #4,sp
        move.l  d2,d0
        movem.l (sp)+,d1-d3
        rts

| ---------------------------------------------------------------- stop
t_stop:
        bsr     sync_tick
        move.l  HZ200,d4
        move.w  #99,d3
1:      stop    #0x2300                 | VBL / Timer C wake it
        dbra    d3,1b
        move.l  HZ200,d0
        sub.l   d4,d0
        move.l  d0,d5
        METRIC  stop2300_100x_ticks
        cmp.l   #400,d5                 | 100 wake-ups take <= 100 VBLs (2 s)
        CHECK   ls, irq_stop_2300
        PS      "ticks="
        move.l  d5,d0
        bsr     pr_dec
        bsr     pr_flush

        move.w  #0x2700,sr
        move.l  0x68,d7
        move.l  #hbl_h,0x68
        move.w  #0x2300,sr
        bsr     sync_tick
        move.l  HZ200,d4
        move.w  #999,d3
2:      stop    #0x2100                 | HBL (every 64 us) wakes it
        move.w  #0x2300,sr
        dbra    d3,2b
        move.l  HZ200,d0
        sub.l   d4,d0
        move.l  d0,d5
        move.w  #0x2700,sr
        move.l  d7,0x68
        move.w  #0x2300,sr
        METRIC  stop2100_1000x_ticks
        cmp.l   #100,d5                 | ~64 ms on a real ST; 0.5 s is a fail
        CHECK   ls, irq_stop_2100
        PS      "ticks="
        move.l  d5,d0
        bsr     pr_dec
        bra     pr_flush

| ---------------------------------------------------------------- in-service
| Timer D at 10.97 kHz with a handler that does NOT clear its ISR bit:
| with software EOI the MFP holds every later Timer D back - one interrupt.
| Clearing the bit lets them through again. With automatic EOI (VR=$40)
| nothing is held and they keep coming.
tdn_h:  addq.l  #1,tcnt
        rte

t_isr_block:
        move.w  #0x2700,sr
        move.b  VR,d7
        move.b  #0x48,VR                | software EOI
        clr.l   tcnt
        move.b  TCDCR,d0
        and.b   #0xF0,d0
        move.b  d0,TCDCR
        move.b  #56,TDDR
        move.l  #tdn_h,V_TD
        bset    #4,IERB
        bset    #4,IMRB
        or.b    #1,d0
        move.b  d0,TCDCR
        move.w  #0x2500,sr
        moveq   #20,d0                  | 100 ms: ~1097 periods
        bsr     wait_ticks
        move.l  tcnt,d4                 | must be 1
        clr.l   tcnt
        move.b  #0xEF,ISRB              | EOI: the held one comes, then more
        moveq   #20,d0
        bsr     wait_ticks
        move.l  tcnt,d5                 | handler still never EOIs: 1 again
        move.w  #0x2700,sr
        move.b  #0xEF,ISRB
        move.b  #0x40,VR                | automatic EOI
        clr.l   tcnt
        move.w  #0x2500,sr
        moveq   #20,d0
        bsr     wait_ticks
        move.w  #0x2700,sr
        move.l  tcnt,d6                 | ~1097
        bsr     stop_timers
        move.b  d7,VR
        move.w  #0x2300,sr

        move.l  d4,d0
        METRIC  isr_held_count
        move.l  d6,d0
        METRIC  auto_eoi_count_100ms
        moveq   #0,d3                   | d3 = 1 when all three hold
        cmp.l   #1,d4
        bne.s   1f
        cmp.l   #1,d5
        bne.s   1f
        cmp.l   #1000,d6                | >= 91% of 1097
        blo.s   1f
        moveq   #1,d3
1:      tst.l   d3
        CHECK   ne, mfp_isr_block
        PS      "held="
        move.l  d4,d0
        bsr     pr_dec
        PS      " after-eoi="
        move.l  d5,d0
        bsr     pr_dec
        PS      " auto-eoi="
        move.l  d6,d0
        bsr     pr_dec
        PS      " expect=1,1,>=1000"
        bra     pr_flush

        .bss
        .even
old_vbl:        .space 4
vbl_extra:      .space 4
vblcnt:         .space 4
hblcnt:         .space 4
tcnt:           .space 4
tbe_base:       .space 2
tbe_line:       .space 2
tbe_frames:     .space 2
tbe_frames_skip: .space 2
tbe_min:        .space 2
tbe_max:        .space 2
tbe_maxoff:     .space 2
tbe_fcnt:       .space 2
tbe_lcnt:       .space 2
tbe_cat:        .space 30
tbe_inpic:      .space 4
tbe_total:      .space 4
        .text
