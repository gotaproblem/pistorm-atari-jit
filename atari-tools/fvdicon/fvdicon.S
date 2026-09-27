| FVDICON.PRG - put the TOS text console on the PiStorm fVDI screen.
|
| AUTO-folder TSR. With fVDI the screen is a host framebuffer the ROM
| knows nothing about, so the BIOS VT52 console (Bconout CON / RAWCON -
| the text of every TOS program, EmuCON included) keeps drawing into the
| native ST screen. This hooks the two console output vectors (XBRA id
| "PSCN") and hands each character to the host's FVDICON NatFeat, which
| draws it into the fVDI framebuffer. Whenever the host answers 0 - fVDI
| has not set its mode yet (the boot and AUTO text), or the character is a
| BEL - the character goes on to the ROM console exactly as before.
|
| At install it gives the host the ROM's own 8x16 system font (Line-A font
| ring), so the console looks like the ROM's.
|
| Position independent (every reference PC-relative), no relocations, no
| library: 68000 code for any TOS/EmuTOS. Needs the PiStorm emulator with
| the FVDICON NatFeat; without it, it prints a line and exits.

        .text
        .globl  _start

NF_ID   =       0x7300
NF_CALL =       0x7301

XCON    =       0x586                   | xconout[2]: CON
XRAW    =       0x592                   | xconout[5]: RAWCON

_start:
        move.l  4(sp),a5                | basepage

        pea     detect(pc)              | Supexec(detect): d0 = feature id
        move.w  #38,-(sp)
        trap    #14
        addq.l  #6,sp
        tst.l   d0
        beq     no_nf

        lea     id_putc(pc),a0          | sub-op ids: base + 1 / 2 / 3
        move.l  d0,d1
        addq.l  #1,d1
        move.l  d1,(a0)+                | id_putc
        addq.l  #1,d1
        move.l  d1,(a0)+                | id_rawc
        addq.l  #1,d1
        move.l  d1,(a0)                 | id_font

        .word   0xA000                  | Line-A init: a1 = font ring
        move.l  8(a1),d1                | [2] = 8x16 system font
        move.l  d1,-(sp)
        move.l  id_font(pc),-(sp)
        subq.l  #4,sp                   | slot where a jsr puts its return
        .word   NF_CALL
        lea     12(sp),sp
        tst.l   d0
        beq     no_font

        pea     install(pc)             | Supexec(install): d0 = 1 new
        move.w  #38,-(sp)
        trap    #14
        addq.l  #6,sp
        tst.l   d0
        beq     already

        pea     msg_ok(pc)
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

no_nf:  lea     msg_nonf(pc),a0
        bra.s   quit
no_font:
        lea     msg_nofont(pc),a0
        bra.s   quit
already:
        lea     msg_twice(pc),a0
quit:   move.l  a0,-(sp)
        move.w  #9,-(sp)
        trap    #1
        addq.l  #6,sp
        clr.w   -(sp)                   | Pterm0
        trap    #1

| --- supervisor: NatFeat present? d0 = id of "FVDICON", or 0 ------------
| On a machine without NatFeats 0x7300 is an illegal instruction: catch
| it through the illegal-instruction vector and answer 0.
detect:
        move.l  0x10.w,a1               | old illegal-instruction vector
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
        move.l  a2,sp                   | drops the args or the exception frame
        move.l  a1,0x10.w
        rts

| --- supervisor: hook both vectors; d0 = 0 when already hooked ----------
install:
        move.l  XCON.w,a0               | ours already there?
        cmp.l   #0x5053434E,-8(a0)      | 'PSCN'
        bne.s   1f
        cmp.l   #0x58425241,-12(a0)     | 'XBRA'
        bne.s   1f
        moveq   #0,d0
        rts
1:      lea     old_con(pc),a0
        move.l  XCON.w,(a0)
        lea     con_hook(pc),a0
        move.l  a0,XCON.w
        lea     old_raw(pc),a0
        move.l  XRAW.w,(a0)
        lea     raw_hook(pc),a0
        move.l  a0,XRAW.w
        moveq   #1,d0
        rts

| --- the hooks ----------------------------------------------------------
| Called as LONG f(WORD dev, WORD ch): 4(sp) = device, 6(sp) = character.
| d0-d2/a0-a2 are scratch for BIOS device routines; all are kept anyway.
        .ascii  "XBRA"
        .ascii  "PSCN"
old_con:
        .long   0
con_hook:
        movem.l d1-d2/a0-a2,-(sp)       | 20 bytes
        moveq   #0,d0
        move.b  20+7(sp),d0             | low byte of the character word
        move.l  d0,-(sp)
        move.l  id_putc(pc),-(sp)
        subq.l  #4,sp
        .word   NF_CALL
        lea     12(sp),sp
        movem.l (sp)+,d1-d2/a0-a2
        tst.l   d0
        beq.s   1f
        rts                             | drawn on the fVDI screen
1:      move.l  old_con(pc),-(sp)       | not ours: the ROM console
        rts

        .ascii  "XBRA"
        .ascii  "PSCN"
old_raw:
        .long   0
raw_hook:
        movem.l d1-d2/a0-a2,-(sp)
        moveq   #0,d0
        move.b  20+7(sp),d0
        move.l  d0,-(sp)
        move.l  id_rawc(pc),-(sp)
        subq.l  #4,sp
        .word   NF_CALL
        lea     12(sp),sp
        movem.l (sp)+,d1-d2/a0-a2
        tst.l   d0
        beq.s   1f
        rts
1:      move.l  old_raw(pc),-(sp)
        rts

id_putc:        .long   0
id_rawc:        .long   0
id_font:        .long   0

nf_name:        .asciz  "FVDICON"
msg_ok:         .asciz  "FVDICON: TOS console drawn on the fVDI screen\r\n"
msg_nonf:       .asciz  "FVDICON: no FVDICON NatFeat - not installed\r\n"
msg_nofont:     .asciz  "FVDICON: host refused the 8x16 font - not installed\r\n"
msg_twice:      .asciz  "FVDICON: already installed\r\n"
        .even
