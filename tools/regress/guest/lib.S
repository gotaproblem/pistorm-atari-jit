| lib.S - shared code for the regression guest programs (68000, TOS).
|
| Output goes to the host through the NF_STDERR native feature, so it lands
| in the emulator's log as "[NF_STDERR] ..." lines that tools/regress/run.sh
| reads:
|   @S <program>                      a program started
|   @T <test> PASS|FAIL <detail>      one check
|   @M <metric> <value>               a measurement (compared run to run)
|   @I <test> <detail>                detail for the @T line that follows
|   @END                              the last program finished
|
| Each program is a TOS .PRG run from A:\AUTO at boot. It does its work in
| supervisor mode (Supexec) and restores what it changed - vectors, MFP
| registers, SR - before it returns, so the next program starts clean.
|
| Time base: _hz_200 ($4BA), TOS's 200 Hz counter driven by MFP Timer C on
| the real chip's crystal. Everything is measured against it, never
| against the emulated CPU's speed.

        .text
        .globl  _start

HZ200   =       0x4BA
MFP     =       0xFFFFFA01             | GPIP; the other registers at +2n

| ---------------------------------------------------------------- macros
| PS "text" - append a literal to the line
.macro  PS      str
        bsr     pr_inline
        .asciz  "\str"
        .even
.endm
| PASS / FAIL "name" - start an @T line (append detail, then bsr pr_flush)
.macro  TPASS   name
        bsr     pr_inline
        .asciz  "@T \name PASS "
        .even
.endm
.macro  TFAIL   name
        bsr     pr_inline
        .asciz  "@T \name FAIL "
        .even
.endm
| METRIC "name", value in d0 - a whole @M line
.macro  METRIC  name
        bsr     pr_inline
        .asciz  "@M \name "
        .even
        bsr     pr_dec
        bsr     pr_flush
.endm
| CHECK cond, "name" - @T PASS if the condition codes say \cond, else FAIL;
| the caller appends detail and flushes in both cases
.macro  CHECK   cond, name
        b\cond  1001f
        TFAIL   \name
        bra.s   1002f
1001:   TPASS   \name
1002:
.endm

| ---------------------------------------------------------------- start-up
| The program's own code starts at `main` (supervisor). PROGNAME is the
| string the @S line carries.
_start:
        move.l  4(sp),a5                | basepage
        lea     ustack_top,sp
        move.l  #0x100,d0               | Mshrink to text+data+bss+stack
        add.l   12(a5),d0
        add.l   20(a5),d0
        add.l   28(a5),d0
        move.l  d0,-(sp)
        move.l  a5,-(sp)
        clr.w   -(sp)
        move.w  #0x4A,-(sp)
        trap    #1
        lea     12(sp),sp
        move.l  #line,lp
        pea     super_main(pc)
        move.w  #38,-(sp)               | Supexec
        trap    #14
        addq.l  #6,sp
        move.l  post_user,d0            | a program's user-mode tail (LAUNCH)
        beq.s   1f
        move.l  d0,a0
        jsr     (a0)
1:      clr.w   -(sp)                   | Pterm0
        trap    #1

super_main:
        movem.l d0-d7/a0-a6,-(sp)
        move.w  sr,saved_sr
        bsr     nf_init                 | touches the $10 vector: supervisor
        PS      "@S "
        lea     progname(pc),a0
        bsr     pr_str
        bsr     pr_flush
        bsr     main
        move.w  saved_sr,sr
        movem.l (sp)+,d0-d7/a0-a6
        rts

| ---------------------------------------------------------------- NatFeats
| NF_ID / NF_CALL: the argument block is at 4(sp) as for a C call. If the
| emulator has no native features the opcode is an illegal instruction:
| caught, and the output then goes nowhere (nf_id stays 0).
nf_init:
        move.l  0x10.w,-(sp)            | illegal-instruction vector
        move.l  sp,nf_sp
        lea     nf_noillg(pc),a0
        move.l  a0,0x10.w
        pea     nf_name(pc)
        bsr.s   nf_getid
        addq.l  #4,sp
        move.l  d0,nf_id
nf_back:
        move.l  (sp)+,0x10.w
        rts
nf_noillg:
        move.l  nf_sp,sp
        clr.l   nf_id
        bra.s   nf_back
nf_getid:
        .word   0x7300
        rts
nf_call:
        .word   0x7301
        rts
nf_name:
        .asciz  "NF_STDERR"
        .even

| ---------------------------------------------------------------- the line
| pr_inline: append the string that follows the bsr, return past it
pr_inline:
        movem.l d0/a0,-(sp)
        move.l  8(sp),a0                | string = return address
        bsr   pr_str
        move.l  a0,d0                   | a0 = after the NUL
        addq.l  #1,d0
        and.w   #0xFFFE,d0              | even
        move.l  d0,8(sp)
        movem.l (sp)+,d0/a0
        rts

| pr_str: append the NUL-terminated string at a0; a0 ends past the NUL
pr_str:
        move.l  a1,-(sp)
        move.l  lp,a1
1:      move.b  (a0)+,(a1)+
        bne.s   1b
        subq.l  #1,a1
        move.l  a1,lp
        move.l  (sp)+,a1
        rts

| pr_chr: append the character in d0
pr_chr:
        move.l  a1,-(sp)
        move.l  lp,a1
        move.b  d0,(a1)+
        clr.b   (a1)
        move.l  a1,lp
        move.l  (sp)+,a1
        rts

| pr_dec: append d0 as a signed decimal
pr_dec:
        movem.l d0-d3/a0,-(sp)
        lea     numbuf+12(pc),a0
        clr.b   (a0)
        move.l  d0,d3
        bpl.s   1f
        neg.l   d0
1:      move.l  d0,d1                   | d1 / 10 by 16-bit pieces
2:      moveq   #0,d2
        swap    d1
        move.w  d1,d2
        divu    #10,d2                  | high word
        move.w  d2,d1
        swap    d1
        move.w  d1,d2                   | remainder:low word
        divu    #10,d2
        move.w  d2,d1
        swap    d2
        add.b   #'0',d2
        move.b  d2,-(a0)
        tst.l   d1
        bne.s   2b
        tst.l   d3
        bpl.s   3f
        move.b  #'-',-(a0)
3:      bsr.s   pr_str
        movem.l (sp)+,d0-d3/a0
        rts

| pr_hex: append d0 as 8 hex digits ($ prefixed)
pr_hex:
        movem.l d0-d2,-(sp)
        move.l  d0,d1
        moveq   #'$',d0
        bsr   pr_chr
        moveq   #7,d2
1:      rol.l   #4,d1
        move.b  d1,d0
        and.b   #15,d0
        add.b   #'0',d0
        cmp.b   #'9',d0
        bls.s   2f
        addq.b  #7,d0
2:      bsr.s   pr_chr
        dbra    d2,1b
        movem.l (sp)+,d0-d2
        rts

| pr_flush: end the line, send it, start a new one
pr_flush:
        movem.l d0-d2/a0-a1,-(sp)
        moveq   #10,d0
        bsr   pr_chr
        move.l  nf_id,d0
        beq.s   1f
        pea     line
        move.l  d0,-(sp)
        bsr     nf_call
        addq.l  #8,sp
1:      move.l  #line,lp
        clr.b   line
        movem.l (sp)+,d0-d2/a0-a1
        rts

| ---------------------------------------------------------------- time
| now: d0 = _hz_200
now:
        move.l  HZ200.w,d0
        rts

| wait_ticks: wait d0 ticks of 5 ms (Timer C must be running, IPL <= 5)
wait_ticks:
        movem.l d0-d1,-(sp)
        add.l   HZ200.w,d0
1:      move.l  HZ200.w,d1
        cmp.l   d0,d1
        bmi.s   1b
        movem.l (sp)+,d0-d1
        rts

| sync_tick: wait for the next tick edge (start a measurement on it)
sync_tick:
        move.l  d0,-(sp)
        move.l  HZ200.w,d0
1:      cmp.l   HZ200.w,d0
        beq.s   1b
        move.l  (sp)+,d0
        rts

| ---------------------------------------------------------------- probing
| probe_w: d0 = 0 if a word read of (a0) completes, -1 on a bus error
probe_w:
        move.l  0x8.w,-(sp)
        move.l  sp,berr_sp
        move.l  a1,-(sp)
        lea     1f(pc),a1
        move.l  a1,0x8.w
        move.l  (sp)+,a1
        tst.w   (a0)
        moveq   #0,d0
        bra.s   2f
1:      move.l  berr_sp,sp
        moveq   #-1,d0
2:      move.l  (sp)+,0x8.w
        rts

| ---------------------------------------------------------------- MFP state
| save_mfp / restore_mfp: the registers and vectors the tests reprogram.
| mfp_save[n] = the register at MFP + 2n: 0 GPIP 1 AER 2 DDR 3 IERA 4 IERB
| 5 IPRA 6 IPRB 7 ISRA 8 ISRB 9 IMRA 10 IMRB 11 VR 12 TACR 13 TBCR
| 14 TCDCR 15 TADR 16 TBDR 17 TCDR 18 TDDR. Timer C is TOS's and is never
| touched; Timers A, B and D are the tests'.
save_mfp:
        movem.l d0/a0-a1,-(sp)
        lea     MFP,a0
        lea     mfp_save,a1
        moveq   #18,d0
1:      move.b  (a0),(a1)+
        addq.l  #2,a0
        dbra    d0,1b
        lea     0x100.w,a0              | MFP vectors $100-$13F
        lea     vec_save,a1
        moveq   #15,d0
2:      move.l  (a0)+,(a1)+
        dbra    d0,2b
        move.l  0x68.w,(a1)+            | HBL
        move.l  0x70.w,(a1)+            | VBL
        move.l  0x118.w,(a1)+           | (also in the block above; harmless)
        movem.l (sp)+,d0/a0-a1
        rts

restore_mfp:
        movem.l d0/a0-a1,-(sp)
        move.w  sr,-(sp)
        or.w    #0x0700,sr
        lea     MFP,a0
        lea     mfp_save,a1
        clr.b   0x18(a0)                | stop A and B while reloading
        clr.b   0x1A(a0)
        move.b  1(a1),0x02(a0)          | AER
        move.b  2(a1),0x04(a0)          | DDR
        move.b  11(a1),0x16(a0)         | VR
        move.b  15(a1),0x1E(a0)         | TADR
        move.b  16(a1),0x20(a0)         | TBDR
        move.b  18(a1),0x24(a0)         | TDDR
        move.b  12(a1),0x18(a0)         | TACR
        move.b  13(a1),0x1A(a0)         | TBCR
        move.b  14(a1),0x1C(a0)         | TCDCR (C as it was, D restored)
        move.b  3(a1),0x06(a0)          | IERA
        move.b  4(a1),0x08(a0)          | IERB
        move.b  9(a1),0x12(a0)          | IMRA
        move.b  10(a1),0x14(a0)         | IMRB
        move.b  #0xDE,0x0A(a0)          | drop the tests' pending/in-service:
        move.b  #0xEF,0x0C(a0)          | Timer A (A5) B (A0) D (B4) only
        move.b  #0xDE,0x0E(a0)
        move.b  #0xEF,0x10(a0)
        lea     0x100.w,a0
        lea     vec_save,a1
        moveq   #15,d0
2:      move.l  (a1)+,(a0)+
        dbra    d0,2b
        move.l  (a1)+,0x68.w
        move.l  (a1)+,0x70.w
        move.w  (sp)+,sr
        movem.l (sp)+,d0/a0-a1
        rts

| ---------------------------------------------------------------- disks
| The IDE/ACSI pattern image (mkimg.py hd): sector n holds n as a big-endian
| long in bytes 0-3, then (n + i + k) & $FF at byte i (4..511), k = 0 as
| built; the write tests use k = 77 for what they write.
HDSECT  =       16384                   | sectors in the image (mkimg.py hd)

| hd_fill: a0 = 512-byte buffer, d0 = sector n, d1 = k
hd_fill:
        movem.l d0-d3/a0,-(sp)
        move.l  d0,(a0)+
        move.l  d0,d2
        add.l   d1,d2
        addq.l  #4,d2
        move.w  #507,d3
1:      move.b  d2,(a0)+
        addq.l  #1,d2
        dbra    d3,1b
        movem.l (sp)+,d0-d3/a0
        rts

| hd_check: a0 = 512-byte buffer, d0 = sector n, d1 = k
| -> d0 = 0 if it is that sector, else 1 + the first bad byte offset
hd_check:
        movem.l d1-d4/a0,-(sp)
        cmp.l   (a0)+,d0
        bne.s   2f
        move.l  d0,d2
        add.l   d1,d2
        addq.l  #4,d2
        moveq   #4,d4
1:      cmp.b   (a0)+,d2
        bne.s   3f
        addq.l  #1,d2
        addq.l  #1,d4
        cmp.w   #512,d4
        bne.s   1b
        moveq   #0,d0
        bra.s   9f
2:      moveq   #1,d0
        bra.s   9f
3:      move.l  d4,d0
        addq.l  #1,d0
9:      movem.l (sp)+,d1-d4/a0
        rts

| ---------------------------------------------------------------- data
post_user: .space 4                     | 0, or code to run in user mode before Pterm
numbuf: .space  14
        .even

        .bss
        .even
lp:     .space  4
line:   .space  256
nf_id:  .space  4
nf_sp:  .space  4
berr_sp: .space 4
saved_sr: .space 2
mfp_save: .space 20
vec_save: .space 76
        .space  4096
ustack_top:

        .text
        .even
