| cpu.inc - the body of CPU.PRG / CPU16.PRG: how fast the 68000 runs,
| against the real MFP's Timer C (_hz_200). Included by cpu.S (TARGET
| 8000 kHz) and cpu16.S (TARGET 16000 kHz) - the cfg cpu_speed of the boot.
|
|   cpu_speed     a register-only loop (no bus access but the _hz_200 read)
|                 for 1.000 s at IPL 5: the governed speed, from the loop's
|                 68000 cycle count, within 2% of TARGET. Field: the governor
|                 has been broken by unrelated changes (catch-up bursts, a
|                 check interval that held interrupts back) more than once.
|   @M cpu_khz    that speed
|   @M scr_kbs    1 s of long writes into the screen (logbase): bytes/s in
|                 KB. Screen writes go out on the real bus, so this is the
|                 bus-bound speed a blitter/sprite demo sees (FastBobs, 5 Nov
|                 2026: the governor reached 4.8 MHz of 8 under it).
|   @M io_kreads  1 s of byte reads of the MFP's GPIP (a real-bus read):
|                 thousands per second
|
| The loop's cycles, 68000 (Motorola tables), every instruction a multiple
| of 4: a real ST stretches the others to the next 4-cycle bus slot (a
| dbra loop measured 6675 kHz in Hatari's cycle-exact ST), and the governor
| counts the tables - so only such a loop means the same on both.
|   nop x 100                   400
|   addq.l #1,d2                8
|   move.l HZ200.w,d0           16
|   sub.l d3,d0                 8
|   cmp.w #200,d0               8
|   bge.s (not taken)           8
|   jmp (a2)                    8
|                               456 per pass

LOOPCYC =       456

main:
        move.w  #0x2500,sr              | Timer C (level 6) only

        | ---- cpu_speed: register loop, 200 ticks
        lea     1f(pc),a2
        bsr     sync_tick
        move.l  HZ200.w,d3
        moveq   #0,d2
1:      .rept   100
        nop
        .endr
        addq.l  #1,d2
        move.l  HZ200.w,d0
        sub.l   d3,d0
        cmp.w   #200,d0
        bge.s   2f
        jmp     (a2)
2:
        move.l  d2,d0                   | passes in 1.000 s
        mulu    #LOOPCYC,d0             | cycles in 1.000 s
        divu    #1000,d0                | kHz (fits: < 65536)
        and.l   #0xFFFF,d0
        move.l  d0,d4
        METRIC  "cpu_khz"
        move.l  d4,d0
        sub.l   #TARGET,d0
        bpl.s   3f
        neg.l   d0
3:      cmp.l   #TARGET/50,d0           | within 2%
        CHECK   ls, "cpu_speed"
        move.l  d4,d0
        bsr     pr_dec
        PS      " kHz (target "
        move.l  #TARGET,d0
        bsr     pr_dec
        PS      " +-2%)"
        bsr     pr_flush

        | ---- scr_kbs: long writes into the screen, 200 ticks
        move.l  0x44E.w,a1              | _v_bas_ad / logbase
        bsr     sync_tick
        move.l  HZ200.w,d3
        moveq   #0,d2
        moveq   #0,d5
4:      move.l  a1,a0
        move.w  #(32000/32)-1,d1
5:      move.l  d5,(a0)+
        move.l  d5,(a0)+
        move.l  d5,(a0)+
        move.l  d5,(a0)+
        move.l  d5,(a0)+
        move.l  d5,(a0)+
        move.l  d5,(a0)+
        move.l  d5,(a0)+
        dbra    d1,5b
        add.l   #32000,d2
        move.l  HZ200.w,d0
        sub.l   d3,d0
        cmp.w   #200,d0
        blt.s   4b
        move.l  d2,d0
        moveq   #10,d1
        lsr.l   d1,d0                   | KB
        METRIC  "scr_kbs"

        | ---- io_kreads: GPIP byte reads, 200 ticks
        lea     MFP,a0
        bsr     sync_tick
        move.l  HZ200.w,d3
        moveq   #0,d2
6:      moveq   #99,d1
7:      move.b  (a0),d0
        dbra    d1,7b
        add.l   #100,d2
        move.l  HZ200.w,d0
        sub.l   d3,d0
        cmp.w   #200,d0
        blt.s   6b
        move.l  d2,d0
        divu    #1000,d0
        and.l   #0xFFFF,d0
        METRIC  "io_kreads"
        rts
