| TEST.PRG for the HOSTDOS bench (harness.c checks each slot).
| Results: long slots at RES + 4*n, Fsfirst names from RES+0x400.
        .text
        .globl  _start
RES     =       0x8000

.macro  r n
        move.l  d0,RES+4*\n
.endm
.macro  fopen str, mode=0
        move.w  #\mode,-(sp)
        pea     \str(pc)
        move.w  #0x3D,-(sp)
        trap    #1
        addq.l  #8,sp
.endm
.macro  fclose reg
        move.w  \reg,-(sp)
        move.w  #0x3E,-(sp)
        trap    #1
        addq.l  #4,sp
.endm
.macro  pathcall fn, str
        pea     \str(pc)
        move.w  #\fn,-(sp)
        trap    #1
        addq.l  #6,sp
.endm
.macro  dsetdrv d
        move.w  #\d,-(sp)
        move.w  #0x0E,-(sp)
        trap    #1
        addq.l  #4,sp
.endm
.macro  fattrib str, w, a
        move.w  #\a,-(sp)
        move.w  #\w,-(sp)
        pea     \str(pc)
        move.w  #0x43,-(sp)
        trap    #1
        lea     10(sp),sp
.endm
.macro  fseek off, reg, mode
        move.w  #\mode,-(sp)
        move.w  \reg,-(sp)
        move.l  #\off,-(sp)
        move.w  #0x42,-(sp)
        trap    #1
        lea     10(sp),sp
.endm
.macro  pexec mode, str
        clr.l   -(sp)
        pea     cmd(pc)
        pea     \str(pc)
        move.w  #\mode,-(sp)
        move.w  #0x4B,-(sp)
        trap    #1
        lea     16(sp),sp
.endm

_start:
        move.l  4(sp),a5
        pea     dta(pc)                 | Fsetdta
        move.w  #0x1A,-(sp)
        trap    #1
        addq.l  #6,sp

        move.w  #0x10,-(sp)             | 0: Fsfirst S:\*.*
        pea     s_all(pc)
        move.w  #0x4E,-(sp)
        trap    #1
        addq.l  #8,sp
        r       0
        lea     RES+0x400,a3
        tst.l   d0
        bne.s   2f
1:      lea     dta+30(pc),a0
3:      move.b  (a0)+,(a3)+
        bne.s   3b
        move.w  #0x4F,-(sp)
        trap    #1
        addq.l  #2,sp
        tst.l   d0
        beq.s   1b
2:      clr.b   (a3)
        r       1

        fopen   s_hello                 | 2
        r       2
        move.w  d0,d6
        pea     RES+0x300               | 3: Fread
        move.l  #100,-(sp)
        move.w  d6,-(sp)
        move.w  #0x3F,-(sp)
        trap    #1
        lea     12(sp),sp
        r       3
        fclose  d6                      | 4
        r       4
        fopen   s_long                  | 5
        r       5
        fclose  d0

        dsetdrv 18
        pathcall 0x3B, s_sub            | 6: Dsetpath \SUB
        r       6
        clr.w   -(sp)                   | Dgetpath(RES+0x380, 0)
        pea     RES+0x380
        move.w  #0x47,-(sp)
        trap    #1
        addq.l  #8,sp
        fopen   s_inner                 | 7
        r       7
        fclose  d0

        clr.w   -(sp)                   | 8: Fcreate NEW.TXT
        pea     s_new(pc)
        move.w  #0x3C,-(sp)
        trap    #1
        addq.l  #8,sp
        r       8
        move.w  d0,d6
        pea     s_abcde(pc)             | 9: Fwrite 5
        move.l  #5,-(sp)
        move.w  d6,-(sp)
        move.w  #0x40,-(sp)
        trap    #1
        lea     12(sp),sp
        r       9
        fseek   10, d6, 0               | 10
        r       10
        fseek   2, d6, 0                | 11
        r       11
        fclose  d6

        pea     s_ren(pc)               | 12: Frename
        pea     s_new(pc)
        clr.w   -(sp)
        move.w  #0x56,-(sp)
        trap    #1
        lea     12(sp),sp
        r       12
        fattrib s_ren, 1, 1             | 13
        r       13
        pathcall 0x41, s_ren            | 14
        r       14
        fattrib s_ren, 1, 0             | 15
        r       15
        pathcall 0x41, s_ren            | 16
        r       16

        pathcall 0x39, s_newdir         | 17 18 19
        r       17
        pathcall 0x39, s_newdir
        r       18
        pathcall 0x3A, s_newdir
        r       19

        move.w  #19,-(sp)               | 20: Dfree S:
        pea     RES+0x3C0
        move.w  #0x36,-(sp)
        trap    #1
        addq.l  #8,sp
        r       20

        fopen   s_cfoo                  | 21
        r       21
        dsetdrv 2
        fopen   s_foo                   | 22
        r       22
        clr.w   -(sp)                   | 23: Dfree current (C:)
        pea     RES+0x3E0
        move.w  #0x36,-(sp)
        trap    #1
        addq.l  #8,sp
        r       23
        dsetdrv 18

        pea     RES+0x3F0               | 24: Fread handle 3
        move.l  #1,-(sp)
        move.w  #3,-(sp)
        move.w  #0x3F,-(sp)
        trap    #1
        lea     12(sp),sp
        r       24

        move.w  #0x7F,-(sp)             | 45: live blocks before
        trap    #1
        addq.l  #2,sp
        r       45
        pexec   0, s_child              | 25
        r       25
        move.w  #0x7F,-(sp)             | 46: and after
        trap    #1
        addq.l  #2,sp
        r       46
        pexec   3, s_child              | 26
        r       26
        move.l  d0,a4
        clr.l   -(sp)                   | 27: Pexec 4
        move.l  a4,-(sp)
        clr.l   -(sp)
        move.w  #4,-(sp)
        move.w  #0x4B,-(sp)
        trap    #1
        lea     16(sp),sp
        r       27
        move.l  0x2C(a4),-(sp)          | 28 29: Mfree env, basepage
        move.w  #0x49,-(sp)
        trap    #1
        addq.l  #6,sp
        r       28
        move.l  a4,-(sp)
        move.w  #0x49,-(sp)
        trap    #1
        addq.l  #6,sp
        r       29

        pea     sup(pc)                 | 30 31: from supervisor mode
        move.w  #38,-(sp)
        trap    #14
        addq.l  #6,sp

        pea     dta2(pc)                | 32: Fsnext, TOS's DTA
        move.w  #0x1A,-(sp)
        trap    #1
        addq.l  #6,sp
        move.w  #0x4F,-(sp)
        trap    #1
        addq.l  #2,sp
        r       32
        pea     dta(pc)
        move.w  #0x1A,-(sp)
        trap    #1
        addq.l  #6,sp

        clr.w   -(sp)                   | 33: Fsfirst no match
        pea     s_nope(pc)
        move.w  #0x4E,-(sp)
        trap    #1
        addq.l  #8,sp
        r       33
        fopen   s_subabs                | 34: a folder
        r       34
        clr.w   -(sp)                   | 35: Fcreate on R: (ro)
        pea     s_rx(pc)
        move.w  #0x3C,-(sp)
        trap    #1
        addq.l  #8,sp
        r       35
        fopen   s_dx                    | 36: TOS's D:
        r       36
        pexec   0, s_missing            | 37
        r       37
        fopen   s_hello                 | 38: left open
        r       38
        pea     RES+0x3F0               | 39: Fread handle 100
        move.l  #1,-(sp)
        move.w  #100,-(sp)
        move.w  #0x3F,-(sp)
        trap    #1
        lea     12(sp),sp
        r       39

        move.w  #7,-(sp)
        move.w  #0x4C,-(sp)
        trap    #1

sup:    fopen   s_hello
        r       30
        fclose  d0
        r       31
        rts

s_all:     .asciz "S:\\*.*"
s_hello:   .asciz "S:\\HELLO.TXT"
s_long:    .asciz "s:\\alongn~1.tex"
s_sub:     .asciz "\\SUB"
s_subabs:  .asciz "S:\\SUB"
s_inner:   .asciz "INNER.DAT"
s_new:     .asciz "NEW.TXT"
s_ren:     .asciz "RENAMED.TXT"
s_abcde:   .asciz "abcde"
s_newdir:  .asciz "S:\\NEWDIR"
s_cfoo:    .asciz "C:\\FOO"
s_foo:     .asciz "FOO"
s_child:   .asciz "S:\\CHILD.PRG"
s_missing: .asciz "S:\\MISSING.PRG"
s_nope:    .asciz "S:\\NOPE.*"
s_rx:      .asciz "R:\\X"
s_dx:      .asciz "D:\\X"
cmd:       .byte 1,'X',0
        .even
dta:    .space  44
dta2:   .space  44
