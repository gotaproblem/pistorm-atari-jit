| HOSTDOS.PRG - HOSTFS drives for plain TOS / EmuTOS GEM (see README.md).
|
| AUTO-folder TSR. FreeMiNT reaches the emulator's HOSTFS drives through
| hostfs.xfs; plain TOS has no file-system plug-ins, so this hooks trap #1
| (XBRA id "PSHD") and, for every GEMDOS call that names one of those
| drives, hands the caller's argument block to the host's HOSTDOS NatFeat
| (platforms/atari/network/hostdos.cpp), which runs the call on the Pi's
| folder and returns the GEMDOS result. The drive letters are added to
| _drvbits so the desktop and file selectors offer them.
|
| What is ours:
|   path calls   Fsfirst Fopen Fcreate Fdelete Fattrib Frename Dcreate
|                Ddelete Dsetpath Pexec 0/3 - when the path's drive (or,
|                with no "X:", the current drive) is a HOSTDOS drive
|   drive calls  Dfree Dgetpath - drive argument, 0 = current
|   handles      Fclose Fread Fwrite Fseek Fdatime - handles 64..95
|   Fsnext       when the DTA carries the "HDOS" mark Fsfirst left in it
| Pterm / Pterm0 / Ptermres tell the host to close the leaving process's
| handles, then go on to the ROM. Everything else goes straight to the
| ROM, untouched.
|
| Pexec 0/3 of a program on a host drive: TOS's loader cannot read our
| drive, so the program is loaded here: Pexec 7 (GEMDOS >= 0.19, with the
| file's prgflags) or 5 makes the basepage, the host copies and relocates
| the image into it, then Pexec 6 (GEMDOS >= 0.15) or 4 runs it - after a
| 4, or a failed load, the basepage and environment are freed here.
| Mode 3 hands the basepage back. All state lives on the stack: the child
| runs inside this handler and its own GEMDOS calls come back through it.
|
| Position independent (every reference PC-relative), no relocations, no
| library: 68000 code for any TOS/EmuTOS. Not for FreeMiNT (refuses to
| install when the MiNT cookie is there; if MiNT starts after it, the host
| stops answering for HOSTDOS once hostfs.xfs mounts).

        .text
        .globl  _start

NF_ID   =       0x7300
NF_CALL =       0x7301

DRVBITS =       0x4C2
LONGFRM =       0x59E                   | _longframe: format word in frames
SYSBASE =       0x4F2
COOKIES =       0x5A0
TRAP1   =       0x84

PASS    =       0x80000000              | host: not ours
PS_GEMDOS =     256                     | room left for what GEMDOS saves at a go
PEXEC   =       0x80000001              | host: load this program here

_start:
        move.l  4(sp),a5                | basepage

        move.w  #0x30,-(sp)             | Sversion: 0x1500 = GEMDOS 0.15
        trap    #1
        addq.l  #2,sp
        ror.w   #8,d0
        lea     gemver(pc),a0
        move.w  d0,(a0)

        pea     install(pc)             | Supexec(install)
        move.w  #38,-(sp)
        trap    #14
        addq.l  #6,sp
        lea     msg_nonf(pc),a0
        cmp.l   #-1,d0
        beq.s   quit
        lea     msg_mint(pc),a0
        cmp.l   #-2,d0
        beq.s   quit
        lea     msg_twice(pc),a0
        cmp.l   #-3,d0
        beq.s   quit
        lea     msg_nodrv(pc),a0
        cmp.l   #-4,d0
        beq.s   quit

        lea     msgbuf(pc),a1           | "HOSTDOS: S: T: on the Pi\r\n"
        lea     msg_head(pc),a0
1:      move.b  (a0)+,(a1)+
        bne.s   1b
        subq.l  #1,a1
        moveq   #0,d1
2:      btst    d1,d0
        beq.s   3f
        moveq   #'A',d2
        add.b   d1,d2
        move.b  d2,(a1)+
        move.b  #':',(a1)+
        move.b  #' ',(a1)+
3:      addq.w  #1,d1
        cmp.w   #26,d1
        blo.s   2b
        lea     msg_tail(pc),a0
4:      move.b  (a0)+,(a1)+
        bne.s   4b
        pea     msgbuf(pc)
        move.w  #9,-(sp)                | Cconws
        trap    #1
        addq.l  #6,sp

        move.l  #0x100,d0               | Ptermres(basepage + text + data + bss)
        add.l   12(a5),d0
        add.l   20(a5),d0
        add.l   28(a5),d0
        clr.w   -(sp)
        move.l  d0,-(sp)
        move.w  #49,-(sp)
        trap    #1

quit:   move.l  a0,-(sp)
        move.w  #9,-(sp)
        trap    #1
        addq.l  #6,sp
        clr.w   -(sp)                   | Pterm0
        trap    #1

| --- supervisor: d0 = drives served, or -1 no NatFeat, -2 MiNT, -3 twice,
| -4 no drive to serve -----------------------------------------------------
install:
        move.l  COOKIES.w,d0            | MiNT running: hostfs.xfs is the way
        beq.s   2f
        move.l  d0,a0
1:      move.l  (a0),d0
        beq.s   2f
        cmp.l   #0x4D694E54,d0          | 'MiNT'
        beq     in_mint
        addq.l  #8,a0
        bra.s   1b

2:      move.l  TRAP1.w,a0              | ours already in the XBRA chain?
        moveq   #31,d1
3:      cmp.l   #0x58425241,-12(a0)     | 'XBRA'
        bne.s   4f
        cmp.l   #0x50534844,-8(a0)      | 'PSHD'
        beq     in_twice
        move.l  -4(a0),a0
        dbra    d1,3b

4:      bsr     detect                  | d0 = HOSTDOS id, 0 = none
        tst.l   d0
        beq     in_nonf
        lea     id_dis(pc),a0           | sub-ops 2 3 4 5
        move.l  d0,d1
        addq.l  #2,d1
        move.l  d1,(a0)+                | id_dis
        addq.l  #1,d1
        move.l  d1,(a0)+                | id_pterm
        addq.l  #1,d1
        move.l  d1,(a0)+                | id_flags
        addq.l  #1,d1
        move.l  d1,(a0)                 | id_load

        addq.l  #1,d0                   | INSTALL: fresh state, drive bits
        move.l  d0,-(sp)
        subq.l  #4,sp
        .word   NF_CALL
        addq.l  #8,sp
        and.l   #0x03FFFFFF,d0          | A..Z
        move.l  DRVBITS.w,d1            | never take over a drive TOS has
        not.l   d1
        and.l   d1,d0
        beq.s   in_nodrv
        lea     drvmask(pc),a0
        move.l  d0,(a0)
        or.l    d0,DRVBITS.w

        move.l  SYSBASE.w,a0            | current basepage pointer (p_run)
        move.l  8(a0),a0                | os_beg: the ROM's own header
        cmp.w   #0x0102,2(a0)           | os_version
        blo.s   5f
        move.l  0x28(a0),a1
        bra.s   6f
5:      move.l  #0x602C,a1              | TOS 1.00
        move.w  0x1C(a0),d1             | os_conf
        lsr.w   #1,d1
        cmp.w   #4,d1                   | Spanish 1.00
        bne.s   6f
        move.l  #0x873C,a1
6:      lea     runp(pc),a0
        move.l  a1,(a0)

        lea     pstack_hi(pc),a1        | our launch stack: empty
        lea     pstack_cur(pc),a0
        move.l  a1,(a0)

        lea     old1(pc),a0             | hook trap #1
        move.l  TRAP1.w,(a0)
        lea     trap1(pc),a0
        move.l  a0,TRAP1.w
        move.l  drvmask(pc),d0
        rts

in_nonf:  moveq #-1,d0
          rts
in_mint:  moveq #-2,d0
          rts
in_twice: moveq #-3,d0
          rts
in_nodrv: moveq #-4,d0
          rts

| On a machine without NatFeats 0x7300 is an illegal instruction: catch
| it through the illegal-instruction vector and answer 0.
detect:
        move.l  0x10.w,a1
        lea     nf_absent(pc),a0
        move.l  a0,0x10.w
        move.l  sp,a2
        moveq   #0,d0
        pea     nf_name(pc)
        subq.l  #4,sp
        .word   NF_ID
        bra.s   nf_done
nf_absent:
        moveq   #0,d0
nf_done:
        move.l  a2,sp
        move.l  a1,0x10.w
        rts

| --- resident: the trap #1 hook -------------------------------------------
        .even
        .ascii  "XBRA"
        .ascii  "PSHD"
old1:   .long   0
trap1:
        movem.l d1-d2/a0-a2,-(sp)       | 20 bytes; restored on every exit
        btst    #5,20(sp)               | S bit of the caller's SR
        bne.s   1f
        move.l  usp,a0
        bra.s   2f
1:      lea     20+6(sp),a0
        tst.w   LONGFRM.w
        beq.s   2f
        addq.l  #2,a0
2:      move.w  (a0),d0                 | a0 = argument block, d0 = function
        cmp.w   #0x57,d0
        bhi     pass
        lea     class(pc),a1
        moveq   #0,d1
        move.b  0(a1,d0.w),d1
        beq     pass
        add.w   d1,d1
        lea     jt-2(pc),a1
        add.w   0(a1,d1.w),a1
        jmp     (a1)
jt:     .word   c_path-jt+2             | 1
        .word   c_hand2-jt+2            | 2
        .word   c_hand6-jt+2            | 3
        .word   c_drv6-jt+2             | 4
        .word   c_ren-jt+2              | 5
        .word   c_pexec-jt+2            | 6
        .word   c_fsf-jt+2              | 7
        .word   c_fsn-jt+2              | 8
        .word   c_term-jt+2             | 9

pass:   movem.l (sp)+,d1-d2/a0-a2
        move.l  old1(pc),-(sp)
        rts

c_path: move.l  2(a0),a1                | path at +2
        bsr     pathdrv
        bmi     pass
        moveq   #0,d1
        bra     call

c_ren:  move.l  4(a0),a1                | Frename(0, old, new): old at +4
        bsr     pathdrv
        bmi     pass
        moveq   #0,d1
        bra     call

c_fsf:  move.l  2(a0),a1                | Fsfirst(spec, attr)
        bsr     pathdrv
        bmi     pass
        bsr     getdta
        bra     call

c_hand2:
        move.w  2(a0),d1
        bra.s   1f
c_hand6:
        move.w  6(a0),d1
1:      sub.w   #64,d1                  | ours: 64..95
        cmp.w   #31,d1
        bhi     pass
        moveq   #0,d2
        moveq   #0,d1
        bra     call

c_drv6: moveq   #0,d2                   | Dfree / Dgetpath: drive at +6
        move.w  6(a0),d2
        bne.s   1f
        bsr     getcur
        bra.s   2f
1:      subq.l  #1,d2
2:      bsr     testdrv
        bmi     pass
        moveq   #0,d1
        bra     call

c_fsn:  bsr     getdta                  | Fsnext: our DTA?
        move.l  d1,a1
        tst.l   d1
        beq     pass
        cmp.b   #'H',(a1)
        bne     pass
        cmp.b   #'D',1(a1)
        bne     pass
        cmp.b   #'O',2(a1)
        bne     pass
        cmp.b   #'S',3(a1)
        bne     pass
        moveq   #0,d2
        bra     call

c_pexec:
        move.w  2(a0),d1                | modes 0 and 3 only
        beq.s   1f
        cmp.w   #3,d1
        bne     pass
1:      move.l  4(a0),a1
        bsr     pathdrv
        bmi     pass
        moveq   #0,d1
        bra     call

c_term: move.l  runp(pc),a1             | close the leaver's handles first
        move.l  (a1),-(sp)
        move.l  id_pterm(pc),-(sp)
        subq.l  #4,sp
        .word   NF_CALL
        lea     12(sp),sp
        bra     pass

| a0 = arguments, d2 = drive, d1 = DTA -> host
call:   move.l  runp(pc),a1
        move.l  d1,-(sp)                | p3 DTA
        move.l  (a1),-(sp)              | p2 current basepage
        move.l  d2,-(sp)                | p1 drive
        move.l  a0,-(sp)                | p0 argument block
        move.l  id_dis(pc),-(sp)
        subq.l  #4,sp
        .word   NF_CALL
        lea     24(sp),sp
        cmp.l   #PASS,d0
        beq     pass
        cmp.l   #PEXEC,d0
        beq.s   pexec
done:   movem.l (sp)+,d1-d2/a0-a2
        rte

| --- Pexec 0/3 of a program on a host drive --------------------------------
| The child runs inside this handler, and two things about that matter:
|
|  1. GEMDOS starts every process with the supervisor stack reset to the
|     top of its system stack (EmuTOS proc_go(): other_sp = &supstk[SUPSIZ],
|     after Atari's GEMDOS). A caller in user mode - a program, e.g.
|     !RUNME.TOS starting COREMARK.TOS - came in through a trap frame on
|     that very stack, with our saved d1-d2/a0-a2 above it, so the child's
|     traps and interrupts overwrite both. Returning through them then
|     landed back in the child after its Pterm (field case 26 Oct 2026:
|     COREMARK's Pterm "returned" to its own bra.s *).
|  2. Everything we keep while the child runs adds to the stack it runs
|     over: ~90 bytes more than TOS's own Pexec on the desktop's stack, and
|     PAULA.APP from a host drive crashed out of a Timer C interrupt with
|     the SSP at $175E (same day; GEMDRIVE hit and fixed the same thing
|     with a private stack).
|
| So the whole launch runs on our own stack. The caller's saved registers
| and trap frame are COPIED onto it (and copied back at pex_out, just
| before `done` returns through them), and pstack_cur marks how far down
| it is in use, so a launch from inside a launched program goes below the
| outer one instead of over it.
|
| Stack here: [our d1-d2/a0-a2 (20)][frame (6, or 8 with _longframe)]
pexec:  moveq   #20+6,d1                | bytes to carry over
        tst.w   LONGFRM.w
        beq.s   1f
        addq.l  #2,d1
1:      move.l  sp,a1                   | caller's area
        move.l  pstack_cur(pc),a2
        sub.l   d1,a2
        move.l  a2,d2                   | its copy, at the bottom of what is in use
        move.w  d1,d0
        lsr.w   #1,d0
        subq.w  #1,d0
2:      move.w  (a1)+,(a2)+
        dbra    d0,2b
        move.l  sp,a1
        move.l  d2,sp                   | onto our stack: [copy]
        move.l  pstack_cur(pc),-(sp)    | [outer pstack_cur][copy]
        move.l  a1,-(sp)                | [caller area][outer cur][copy]
        move.w  d1,-(sp)                | [size][caller area][outer cur][copy]
        move.w  2(a0),-(sp)             | [mode][size][caller area]...
        move.l  8(a0),a1                | command line
        move.l  12(a0),a2               | environment
        move.w  gemver(pc),d1           | GEMDOS 0.19: Pexec 7 with prgflags
        cmp.w   #0x19,d1
        blo.s   1f
        move.l  id_flags(pc),-(sp)
        subq.l  #4,sp
        .word   NF_CALL
        addq.l  #8,sp
        move.l  a2,-(sp)
        move.l  a1,-(sp)
        move.l  d0,-(sp)
        move.w  #7,-(sp)
        bra.s   2f
1:      move.l  a2,-(sp)
        move.l  a1,-(sp)
        clr.l   -(sp)
        move.w  #5,-(sp)
2:      move.w  #0x4B,-(sp)
        trap    #1
        lea     16(sp),sp
        tst.l   d0
        bmi     pex_out                 | no basepage: TOS's error
        move.l  d0,-(sp)                | [basepage][mode]
        move.l  d0,-(sp)                | host: load + relocate into it
        move.l  id_load(pc),-(sp)
        subq.l  #4,sp
        .word   NF_CALL
        lea     12(sp),sp
        tst.l   d0
        bmi.s   pex_free
        cmp.w   #3,4(sp)
        beq     pex_bp
        moveq   #6,d2                   | go: 6 frees it all at Pterm
        move.w  gemver(pc),d1
        cmp.w   #0x15,d1
        bhs.s   3f
        moveq   #4,d2
| The child runs from HERE, a supervisor-mode Pexec inside our trap #1
| handler. When it ends, real TOS hands back the supervisor stack and d0
| only: the user stack pointer is still the child's and d1-d7/a0-a6 hold
| what the child left. A caller in user mode - a program on a host drive
| starting another one - then resumed on the child's stack and crashed
| (field case 26 Oct 2026: !RUNME.TOS running COREMARK.TOS, illegal
| instruction at $0000000C on its next rts). So keep the caller's USP and
| d3-d7/a3-a6 here, across the Pexec. (d1-d2/a0-a2 are restored at `done`;
| d0 is the child's exit code.) The desktop calls Pexec in supervisor mode,
| which is why programs started from it never showed this.
3:      move.l  usp,a1
        move.l  a1,-(sp)                | [usp][basepage][mode]
        movem.l d3-d7/a3-a6,-(sp)       | [36 regs][usp][basepage][mode]
        lea     -PS_GEMDOS(sp),a1       | while the child runs, a launch from
        lea     pstack_cur(pc),a2       | inside it starts below this and
        move.l  a1,(a2)                 | below what GEMDOS saves here
        clr.l   -(sp)                   | Pexec(d2, 0, basepage, 0)
        move.l  4+40(sp),-(sp)          | basepage, past the regs and usp
        clr.l   -(sp)
        move.w  d2,-(sp)
        move.w  #0x4B,-(sp)
        trap    #1
        lea     16(sp),sp
        movem.l (sp)+,d3-d7/a3-a6       | (movem/move to An keep d0 and CCR)
        move.l  (sp)+,a1
        move.l  a1,usp                  | [basepage][mode] as before
        move.w  gemver(pc),d1
        cmp.w   #0x15,d1
        bhs.s   pex_pop
pex_free:                               | Mfree environment + basepage, keep d0
        move.l  d0,-(sp)
        move.l  4(sp),a1
        move.l  0x2C(a1),-(sp)          | p_env
        move.w  #0x49,-(sp)
        trap    #1
        addq.l  #6,sp
        move.l  4(sp),-(sp)
        move.w  #0x49,-(sp)
        trap    #1
        addq.l  #6,sp
        move.l  (sp)+,d0
pex_pop:
        addq.l  #4,sp
pex_out:
        addq.l  #2,sp                   | [size][caller area][outer cur][copy]
        move.w  (sp)+,d1
        move.l  (sp)+,a1
        lea     pstack_cur(pc),a2
        move.l  (sp)+,(a2)              | the outer launch's mark again
        move.l  a1,a2                   | copy the caller's regs + frame back
        move.w  d1,d2                   | (d0 = our result, kept)
        lsr.w   #1,d2
        subq.w  #1,d2
1:      move.w  (sp)+,(a2)+
        dbra    d2,1b
        move.l  a1,sp
        bra     done
pex_bp: move.l  (sp),d0                 | mode 3: the caller gets the basepage
        bra.s   pex_pop

| --- helpers (all keep a0) ----------------------------------------------------
| a1 = GEMDOS path -> d2 = its drive and N clear when HOSTDOS serves it,
| else d2 = -1 (N set)
pathdrv:
        move.l  a1,d2
        beq.s   no
        cmp.b   #':',1(a1)
        bne.s   1f
        moveq   #0,d2
        move.b  (a1),d2
        cmp.b   #'a',d2
        blo.s   2f
        sub.b   #0x20,d2
2:      sub.l   #'A',d2
        bra.s   testdrv
1:      bsr.s   getcur
testdrv:
        cmp.l   #25,d2
        bhi.s   no
        move.l  drvmask(pc),d1
        btst    d2,d1
        beq.s   no
        tst.l   d2
        rts
no:     moveq   #-1,d2
        rts

getcur: movem.l d1/a0-a1,-(sp)          | d2 = Dgetdrv()
        move.w  #0x19,-(sp)
        trap    #1
        addq.l  #2,sp
        movem.l (sp)+,d1/a0-a1
        moveq   #0,d2
        move.w  d0,d2
        rts

getdta: movem.l d2/a0-a1,-(sp)          | d1 = Fgetdta()
        move.w  #0x2F,-(sp)
        trap    #1
        addq.l  #2,sp
        movem.l (sp)+,d2/a0-a1
        move.l  d0,d1
        rts

class:
        .byte   9,0,0,0,0,0,0,0         | 00-07  Pterm0
        .byte   0,0,0,0,0,0,0,0         | 08-0F
        .byte   0,0,0,0,0,0,0,0         | 10-17
        .byte   0,0,0,0,0,0,0,0         | 18-1F
        .byte   0,0,0,0,0,0,0,0         | 20-27
        .byte   0,0,0,0,0,0,0,0         | 28-2F
        .byte   0,9,0,0,0,0,4,0         | 30-37  Ptermres Dfree
        .byte   0,1,1,1,1,1,2,2         | 38-3F  Dcreate..Fopen Fclose Fread
        .byte   2,1,3,1,0,0,0,4         | 40-47  Fwrite Fdelete Fseek Fattrib Dgetpath
        .byte   0,0,0,6,9,0,7,8         | 48-4F  Pexec Pterm Fsfirst Fsnext
        .byte   0,0,0,0,0,0,5,3         | 50-57  Frename Fdatime
        .even

drvmask:  .long 0                       | drives we serve (A = bit 0)
runp:     .long 0                       | -> current basepage pointer
id_dis:   .long 0
id_pterm: .long 0
id_flags: .long 0
id_load:  .long 0
gemver:   .word 0                       | 0x0015 = GEMDOS 0.15

nf_name:   .asciz "HOSTDOS"
msg_head:  .asciz "HOSTDOS: "
msg_tail:  .asciz "on the Pi\r\n"
msg_nonf:  .asciz "HOSTDOS: no HOSTDOS NatFeat (needs pistorm-atari-jit and a hostfs line) - not installed\r\n"
msg_mint:  .asciz "HOSTDOS: not needed under MiNT (hostfs.xfs serves the drives) - not installed\r\n"
msg_twice: .asciz "HOSTDOS: already installed\r\n"
msg_nodrv: .asciz "HOSTDOS: no hostfs drive left to serve - not installed\r\n"
        .even
msgbuf:    .space 96

| Private supervisor stack for a host-drive program (see pexec). It holds
| HOSTDOS's Pexec frame and everything the child does in supervisor mode -
| its GEMDOS/BIOS/XBIOS calls and every interrupt it takes - so it is
| generous. Kept resident by Ptermres with the rest of the text.
        .even
pstack_cur: .long 0                     | lowest byte in use (install: pstack_hi)
pstack_lo: .space 4096
pstack_hi:
