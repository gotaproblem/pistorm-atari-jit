| launch.S - LAUNCH.PRG: start the real program a tests.txt boot is about.
|
| One hard-disk image holds the programs (FastBobs, Bad Apple, Paula...);
| each boot gets its own floppy (run.sh, mkimg.py) whose root has RUN.INF:
|   line 1   the program, full path        C:\PAULA26\PAULA.APP
|   line 2   its command line (may be empty)  MOD\AXEL_F.MOD
| LAUNCH sits in the floppy's AUTO folder. It makes the program's folder
| current (drive and path - Bad Apple opens BA.DAT, Paula its MOD, by
| relative names) and Pexecs it. Programs under test run until the boot's
| time is up; if one returns, LAUNCH says so (@T launch FAIL).

        .include "lib.S"

progname: .asciz "LAUNCH"
        .even

main:
        | ---- read A:\RUN.INF
        clr.w   -(sp)
        pea     inf_name(pc)
        move.w  #0x3D,-(sp)             | Fopen
        trap    #1
        addq.l  #8,sp
        tst.l   d0
        bmi     no_inf
        move.w  d0,d3
        pea     inf_buf
        move.l  #250,-(sp)
        move.w  d3,-(sp)
        move.w  #0x3F,-(sp)             | Fread
        trap    #1
        lea     12(sp),sp
        move.l  d0,d4
        move.w  d3,-(sp)
        move.w  #0x3E,-(sp)             | Fclose
        trap    #1
        addq.l  #4,sp
        tst.l   d4
        ble     no_inf
        lea     inf_buf,a0
        clr.b   0(a0,d4.l)

        | ---- line 1 -> path (cut at CR/LF), line 2 -> args
        lea     inf_buf,a0
        lea     path,a1
1:      move.b  (a0)+,d0
        beq.s   2f
        cmp.b   #13,d0
        beq.s   2f
        cmp.b   #10,d0
        beq.s   2f
        move.b  d0,(a1)+
        bra.s   1b
2:      clr.b   (a1)
        tst.b   d0
        beq.s   4f
        cmp.b   #13,d0                  | CR LF: skip the LF
        bne.s   3f
        cmp.b   #10,(a0)
        bne.s   3f
        addq.l  #1,a0
3:      lea     cmd+1,a1                | args into a Pascal command line
        moveq   #0,d1
5:      move.b  (a0)+,d0
        beq.s   4f
        cmp.b   #13,d0
        beq.s   4f
        cmp.b   #10,d0
        beq.s   4f
        move.b  d0,(a1)+
        addq.b  #1,d1
        cmp.b   #120,d1
        bcs.s   5b
4:      clr.b   (a1)
        move.b  d1,cmd

        | ---- dir = path up to its last backslash
        lea     path,a0
        lea     dir,a1
        move.l  a1,a2                   | after the last '\' seen
6:      move.b  (a0)+,d0
        move.b  d0,(a1)+
        beq.s   7f
        cmp.b   #'\\',d0
        bne.s   6b
        move.l  a1,a2
        bra.s   6b
7:      clr.b   -1(a2)                  | cut before the last '\'

        PS      "@I launch "
        lea     path,a0
        bsr     pr_str
        PS      " "
        lea     cmd+1,a0
        bsr     pr_str
        bsr     pr_flush
        move.l  #run_it,post_user       | Pexec in user mode, after Supexec
        rts

no_inf:
        TFAIL   "launch"
        PS      "no A:\\RUN.INF"
        bsr     pr_flush
        rts

| user mode: make the program's folder current and run it
run_it:
        lea     dir,a0
        moveq   #0,d0
        move.b  (a0),d0                 | drive letter
        cmp.b   #':',1(a0)
        bne.s   1f
        and.b   #0xDF,d0
        sub.b   #'A',d0
        move.w  d0,-(sp)
        move.w  #0x0E,-(sp)             | Dsetdrv
        trap    #1
        addq.l  #4,sp
        lea     dir+2,a0
1:      tst.b   (a0)
        bne.s   2f
        lea     root(pc),a0             | program in the drive's root
2:      move.l  a0,-(sp)
        move.w  #0x3B,-(sp)             | Dsetpath
        trap    #1
        addq.l  #6,sp
        pea     no_env(pc)              | env: empty - an inherited ARGV= would
                                        | be taken as THIS program's arguments
                                        | (Pure C startup: Paula, MP crashed)
        pea     cmd
        pea     path
        clr.w   -(sp)                   | load and go
        move.w  #0x4B,-(sp)             | Pexec
        trap    #1
        lea     16(sp),sp
        move.l  d0,d4
        TFAIL   "launch"
        PS      "the program returned "
        move.l  d4,d0
        bsr     pr_hex
        bsr     pr_flush
        rts

inf_name: .asciz "A:\\RUN.INF"
root:   .asciz  "\\"
no_env: .byte   0, 0
        .even

        .bss
inf_buf: .space 256
path:   .space  128
dir:    .space  128
cmd:    .space  128
        .text
