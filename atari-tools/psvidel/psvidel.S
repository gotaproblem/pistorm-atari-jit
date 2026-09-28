| PSVIDEL.PRG - Falcon Videl + SuperVidel XBIOS for the PiStorm HDMI.
|
| AUTO-folder TSR (put it BEFORE MINT.PRG). The host (cfg `psvidel`)
| emulates a Falcon Videl and a SuperVidel and shows them on the Pi's
| HDMI only; this is the XBIOS half, the part a Falcon's TOS 4 / the
| SuperVidel's SV_XBIOS would provide:
|
|   cookies   _VDO = $00030000 (Falcon video), SupV (only when the host
|             has the 0xA0000000 video RAM window - 32-bit bus),
|             _SND |= $1E (cfg falcon_dsp: DMA, CODEC, DSP, matrix)
|   XBIOS     Physbase (2), VsetScreen/Setscreen (5), VsetMode (88),
|             VgetMonitor (89), VsetSync (90), VgetSize (91), VsetRGB (93),
|             VgetRGB (94), Vfixmode (95), VsetMask (150),
|             ct60_vmalloc ($C60E)                       - cfg psvidel
|             Dsp_Lock (104), Dsp_Unlock (105), the sound calls
|             locksnd .. buffptr (128-141)               - cfg falcon_dsp
| Each group is hooked only when the host has it switched on.
|
| and it arms the host's register emulation ($FF8266, $FF8210, the
| $FF8282-$FF82C2 timing block, $FF9800 palette, $FF8006 monitor type),
| which stays invisible until now so the boot-time Videl probes still
| find an ST. Everything else in XBIOS passes straight through (XBRA id
| "PSVD").
|
| Position independent, no relocations, no library, 68000 code.

        .text
        .globl  _start

NF_ID   =       0x7300
NF_CALL =       0x7301

V_BAS   =       0x44E                   | _v_bas_ad (logical screen)
LONGFR  =       0x59E                   | _longframe
JARPTR  =       0x5A0                   | _p_cookies
TRAP14  =       0xB8

| host sub-ops (platforms/atari/network/atari_natfeat.cpp, nf_psvidel_ops)
P_ENABLE  =     1
P_SETMODE =     3
P_FIXMODE =     4
P_GETSIZE =     5
P_SETRGB  =     6
P_GETRGB  =     7
P_SETPHYS =     8
P_GETPHYS =     9
P_ACTIVE  =     10
P_VMALLOC =     11
P_SALLOC  =     12
P_SNDX    =     14

JAR_SLOTS =     64                      | size of our own jar, if needed

| NF op: call host sub-op with the arguments already pushed; d0 = result.
| Touches nothing but d0 and the stack (balanced).
        .macro  NF op
        move.l  nf_id(pc),-(sp)
        addi.l  #\op,(sp)
        subq.l  #4,sp
        .word   NF_CALL
        addq.l  #8,sp
        .endm

_start:
        move.l  4(sp),a5                | basepage

        pea     detect(pc)              | Supexec(detect): d0 = feature id
        move.w  #38,-(sp)
        trap    #14
        addq.l  #6,sp
        tst.l   d0
        beq     no_nf
        lea     nf_id(pc),a0
        move.l  d0,(a0)

        move.w  #4,-(sp)                | Getrez: the ST mode TOS is in
        trap    #14
        addq.l  #2,sp
        and.w   #3,d0
        add.w   d0,d0
        lea     rez2mode(pc),a0
        move.w  0(a0,d0.w),d0
        lea     init_mode(pc),a0
        move.w  d0,(a0)

        pea     install(pc)             | Supexec(install)
        move.w  #38,-(sp)
        trap    #14
        addq.l  #6,sp
        tst.l   d0
        beq     already
        bmi     refused

        move.l  host_flags(pc),d0
        btst    #3,d0
        beq.s   3f
        lea     msg_ok(pc),a0
        bsr     print
        move.l  host_flags(pc),d0
        btst    #1,d0
        beq.s   1f
        lea     msg_vram(pc),a0
        bra.s   2f
1:      lea     msg_novram(pc),a0
2:      bsr     print
3:      move.l  host_flags(pc),d0
        btst    #2,d0
        beq.s   4f
        lea     msg_dsp(pc),a0
        bsr     print
4:

        | Ptermres(basepage + text + data + bss + a 64K guard). The guard is
        | for whoever allocates next: Beats of Rage draws sprites a little
        | way above the start of its Mxalloc'd screen block, and a block
        | that followed this TSR directly had its XBIOS hook painted over
        | with pixels - the next XBIOS call took a Line F. On a Falcon that
        | overdraw lands in something that does not matter; here it does.
        move.l  #0x100+0x10000,d0
        add.l   12(a5),d0
        add.l   20(a5),d0
        add.l   28(a5),d0
        clr.w   -(sp)
        move.l  d0,-(sp)
        move.w  #49,-(sp)
        trap    #1

no_nf:  lea     msg_nonf(pc),a0
        bra.s   quit
refused:
        lea     msg_refused(pc),a0
        bra.s   quit
already:
        lea     msg_twice(pc),a0
quit:   bsr     print
        clr.w   -(sp)                   | Pterm0
        trap    #1

print:  move.l  a0,-(sp)                | Cconws
        move.w  #9,-(sp)
        trap    #1
        addq.l  #6,sp
        rts

| --- supervisor: NatFeat present? d0 = id of "PSVIDEL", or 0 ------------
| Without NatFeats 0x7300 is an illegal instruction: catch it.
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

| --- supervisor: arm the host, cookies, hook trap #14 ---------------------
| d0 = 1 installed, 0 already there, -1 the host said no
install:
        move.l  TRAP14.w,a0
        cmp.l   #0x50535644,-8(a0)      | 'PSVD'
        bne.s   1f
        cmp.l   #0x58425241,-12(a0)     | 'XBRA'
        bne.s   1f
        moveq   #0,d0
        rts
1:      moveq   #0,d0
        move.w  init_mode(pc),d0
        move.l  d0,-(sp)
        NF      P_ENABLE
        addq.l  #4,sp
        lea     host_flags(pc),a0
        move.l  d0,(a0)
        btst    #0,d0
        bne.s   2f
        moveq   #-1,d0
        rts
2:      bsr     cookies
        lea     old14(pc),a0
        move.l  TRAP14.w,(a0)
        lea     xbios(pc),a0
        move.l  a0,TRAP14.w
        moveq   #1,d0
        rts

| --- supervisor: _VDO = Falcon, SupV when there is video RAM --------------
cookies:
        movem.l d3-d4/a3,-(sp)
        moveq   #0,d3                   | bit0 _VDO, bit1 SupV, bit2 _SND done
        move.l  host_flags(pc),d0
        btst    #1,d0
        bne.s   1f
        bset    #1,d3                   | no VRAM window: no SupV either
1:      btst    #3,d0
        bne.s   12f
        bset    #0,d3                   | no Videl: leave _VDO alone
12:     btst    #2,d0
        bne.s   13f
        bset    #2,d3                   | no Falcon sound: leave _SND alone
13:     move.l  JARPTR.w,d0
        bne.s   2f
        | no jar at all (TOS 1.0x): start ours, empty
        lea     own_jar(pc),a0
        clr.l   (a0)
        move.l  #JAR_SLOTS,4(a0)
        move.l  a0,JARPTR.w
        move.l  a0,d0
2:      move.l  d0,a0
        moveq   #0,d4                   | entries used
3:      move.l  (a0),d0
        beq.s   6f
        cmp.l   #0x5F56444F,d0          | '_VDO'
        bne.s   4f
        move.l  #0x00030000,4(a0)
        bset    #0,d3
4:      cmp.l   #0x53757056,d0          | 'SupV'
        bne.s   14f
        clr.l   4(a0)
        bset    #1,d3
14:     cmp.l   #0x5F534E44,d0          | '_SND'
        bne.s   5f
        btst    #2,d3
        bne.s   5f
        ori.l   #0x1E,4(a0)             | + 8-bit DMA, CODEC, DSP, matrix
        bset    #2,d3
5:      addq.l  #8,a0
        addq.l  #1,d4
        bra.s   3b
6:      | a0 = terminator, 4(a0) = slots, d4 = used. Room for 3 more + end?
        move.l  d4,d0
        addq.l  #4,d0
        cmp.l   4(a0),d0
        bls.s   8f
        | full: copy into our own, bigger jar (if even that has room)
        cmp.l   #JAR_SLOTS-4,d4
        bhi.s   11f
        move.l  JARPTR.w,a1
        lea     own_jar(pc),a0
        move.l  a0,JARPTR.w
        move.w  d4,d0
        bra.s   71f
7:      move.l  (a1)+,(a0)+
        move.l  (a1)+,(a0)+
71:     dbra    d0,7b
        clr.l   (a0)
        move.l  #JAR_SLOTS,4(a0)
8:      move.l  4(a0),d0                | keep the slot count for the new end
        btst    #0,d3
        bne.s   9f
        move.l  #0x5F56444F,(a0)+
        move.l  #0x00030000,(a0)+
9:      btst    #1,d3
        bne.s   15f
        move.l  #0x53757056,(a0)+
        clr.l   (a0)+
15:     btst    #2,d3
        bne.s   10f
        move.l  #0x5F534E44,(a0)+       | _SND: PSG + the Falcon's
        move.l  #0x1F,(a0)+
10:     clr.l   (a0)
        move.l  d0,4(a0)
11:     movem.l (sp)+,d3-d4/a3
        rts

| --- the XBIOS hook ---------------------------------------------------------
| TOS convention: d0-d2/a0-a2 are scratch. a0 -> the caller's arguments.
        .ascii  "XBRA"
        .ascii  "PSVD"
old14:  .long   0
xbios:
        move.l  usp,a0
        btst    #5,(sp)                 | called from supervisor mode?
        beq.s   1f
        lea     6(sp),a0
        tst.w   LONGFR.w
        beq.s   1f
        addq.l  #2,a0                   | 68010+ frame has a format word
1:      move.w  (a0)+,d0
        move.l  host_flags(pc),d1
        btst    #2,d1
        beq.s   2f
        cmp.w   #104,d0                 | Dsp_Lock / Dsp_Unlock
        beq     x_sndx
        cmp.w   #105,d0
        beq     x_sndx
        cmp.w   #128,d0                 | the sound calls 128-141
        bcs.s   2f
        cmp.w   #141,d0
        bls     x_sndx
2:      btst    #3,d1                   | the video calls: only with a Videl
        beq     pass
        cmp.w   #88,d0
        beq     x_vsetmode
        cmp.w   #5,d0
        beq     x_setscreen
        cmp.w   #2,d0
        beq     x_physbase
        cmp.w   #89,d0
        beq     x_monitor
        cmp.w   #90,d0
        beq     x_zero
        cmp.w   #91,d0
        beq     x_getsize
        cmp.w   #93,d0
        beq     x_setrgb
        cmp.w   #94,d0
        beq     x_getrgb
        cmp.w   #95,d0
        beq     x_fixmode
        cmp.w   #150,d0
        beq     x_zero
        cmp.w   #0xC60E,d0
        beq     x_vmalloc
pass:   move.l  old14(pc),-(sp)
        rts

x_sndx:                                 | Falcon DSP lock / sound XBIOS
        move.l  a0,-(sp)                | -> the arguments
        and.l   #0xFFFF,d0
        move.l  d0,-(sp)
        NF      P_SNDX
        addq.l  #8,sp
        rte

x_monitor:                              | VgetMonitor: always VGA
        moveq   #2,d0
        rte
x_zero:                                 | VsetSync, VsetMask
        moveq   #0,d0
        rte

x_physbase:
        NF      P_ACTIVE
        tst.l   d0
        beq.s   pass                    | ST world: TOS knows
        NF      P_GETPHYS
        rte

x_getsize:                              | VgetSize(mode)
        move.w  (a0),d1
        ext.l   d1
        move.l  d1,-(sp)
        NF      P_GETSIZE
        addq.l  #4,sp
        rte

x_fixmode:                              | Vfixmode(mode)
        move.w  (a0),d1
        ext.l   d1
        move.l  d1,-(sp)
        NF      P_FIXMODE
        addq.l  #4,sp
        and.l   #0xFFFF,d0
        rte

x_vsetmode:                             | VsetMode(mode) -> old mode
        move.w  (a0),d1
        ext.l   d1
        move.l  d1,-(sp)
        NF      P_SETMODE
        addq.l  #4,sp
        btst    #16,d0
        beq.s   1f
        bsr     st_shifter              | ST compatible: the real shifter
1:      and.l   #0xFFFF,d0
        rte

| ST compatible mode chosen (d0 = SETMODE result): put the real ST shifter
| on the ST rez the mode means, if it is not there already. Keeps d0.
st_shifter:
        move.l  d0,d1
        swap    d1
        lsr.w   #1,d1
        and.w   #3,d1                   | strez = bits 17-18
        move.b  0xFFFF8260,d2
        and.w   #3,d2
        cmp.w   d1,d2
        beq.s   1f
        move.b  d1,0xFFFF8260
1:      rts

| VsetScreen / Setscreen(log.l, phys.l, rez.w, mode.w)
x_setscreen:
        move.w  8(a0),d1
        cmp.w   #3,d1
        bne     ss_st
        move.w  10(a0),d1
        cmp.w   #-1,d1
        beq     ss_host                 | rez 3, mode -1: addresses only
        ext.l   d1
        move.l  d1,-(sp)
        NF      P_SETMODE
        addq.l  #4,sp
        btst    #20,d0
        bne     ss_done                 | invalid mode: nothing changed
        btst    #16,d0
        bne     ss_compat
        | Falcon / SV mode. (0, 0) = "give me new screen memory"
        tst.l   (a0)
        bne     ss_host
        tst.l   4(a0)
        bne     ss_host
        move.w  10(a0),d1
        ext.l   d1
        move.l  d1,-(sp)
        NF      P_GETSIZE
        addq.l  #4,sp
        move.l  d0,d2                   | bytes
        move.l  d0,-(sp)
        NF      P_SALLOC                | cleared video RAM, if there is any
        addq.l  #4,sp
        tst.l   d0
        bne.s   1f
        | no video RAM window: ST-RAM from GEMDOS, 256-byte aligned
        move.l  a0,-(sp)
        add.l   #256,d2
        clr.w   -(sp)                   | ST-RAM only
        move.l  d2,-(sp)
        move.w  #0x44,-(sp)             | Mxalloc
        trap    #1
        addq.l  #8,sp
        move.l  (sp)+,a0
        tst.l   d0
        beq     ss_done                 | no memory: the old screen stays
        add.l   #255,d0
        and.l   #0xFFFFFF00,d0
1:      move.l  d0,d1                   | logical
        move.l  d0,d2                   | physical
        bra.s   ss_apply

ss_host:                                | a Falcon/SV mode owns the screen
        move.l  (a0),d1
        move.l  4(a0),d2
ss_apply:
        cmp.l   #-1,d2
        beq.s   1f
        tst.l   d2
        beq.s   1f
        move.l  d2,-(sp)
        NF      P_SETPHYS
        addq.l  #4,sp
1:      cmp.l   #-1,d1
        beq.s   ss_done
        tst.l   d1
        beq.s   ss_done
        move.l  d1,V_BAS.w
ss_done:
        moveq   #0,d0
        rte

ss_compat:
        | back to an ST mode: the real shifter takes over, so the rest is
        | an ordinary ST Setscreen for TOS - with the rez only if it changes
        move.l  d0,d1
        swap    d1
        lsr.w   #1,d1
        and.w   #3,d1
        move.b  0xFFFF8260,d2
        and.w   #3,d2
        cmp.w   d1,d2
        bne.s   1f
        moveq   #-1,d1
1:      move.w  d1,8(a0)
        | 0 is "allocate" to TOS 4 but address 0 to TOS 2: leave it alone
        tst.l   (a0)
        bne.s   2f
        moveq   #-1,d0
        move.l  d0,(a0)
2:      tst.l   4(a0)
        bne.s   ss_st_phys
        moveq   #-1,d0
        move.l  d0,4(a0)
        bra     pass

ss_st:  | rez 0..2 or -1
        NF      P_ACTIVE
        tst.l   d0
        beq.s   ss_st_phys
        | a Falcon/SV mode is on
        move.w  8(a0),d1
        cmp.w   #2,d1
        bhi     ss_host                 | -1 (or nonsense): addresses only
        | an ST rez: go ST compatible first, then as above
        add.w   d1,d1
        lea     rez2mode(pc),a1
        move.w  0(a1,d1.w),d1
        ext.l   d1
        move.l  d1,-(sp)
        NF      P_SETMODE
        addq.l  #4,sp
        bra.s   ss_compat
ss_st_phys:
        | the ST world: TOS does it. A physical address the ST shifter
        | cannot reach is recorded for PSVIDEL but not passed on.
        move.l  4(a0),d2
        cmp.l   #-1,d2
        beq     pass
        tst.l   d2
        beq     pass
        move.l  d2,-(sp)
        NF      P_SETPHYS
        addq.l  #4,sp
        cmp.l   #0x01000000,d2
        bcs     pass
        moveq   #-1,d0
        move.l  d0,4(a0)
        bra     pass

x_setrgb:                               | VsetRGB(index.w, count.w, array.l)
        pea     ste_buf(pc)
        move.l  4(a0),-(sp)
        move.w  2(a0),d1
        ext.l   d1
        move.l  d1,-(sp)
        move.w  (a0),d1
        ext.l   d1
        move.l  d1,-(sp)
        NF      P_SETRGB
        lea     16(sp),sp
        tst.l   d0                      | STE-palette mode: d0 words to set
        beq.s   2f
        move.w  (a0),d1
        add.w   d1,d1
        lea     0xFFFF8240,a1
        add.w   d1,a1
        lea     ste_buf(pc),a2
        subq.w  #1,d0
1:      move.w  (a2)+,(a1)+
        dbra    d0,1b
2:      moveq   #0,d0
        rte

x_getrgb:                               | VgetRGB(index.w, count.w, array.l)
        move.l  4(a0),-(sp)
        move.w  2(a0),d1
        ext.l   d1
        move.l  d1,-(sp)
        move.w  (a0),d1
        ext.l   d1
        move.l  d1,-(sp)
        NF      P_GETRGB
        lea     12(sp),sp
        moveq   #0,d0
        rte

x_vmalloc:                              | ct60_vmalloc(mode.w, value.l)
        move.l  2(a0),-(sp)
        move.w  (a0),d1
        ext.l   d1
        move.l  d1,-(sp)
        NF      P_VMALLOC
        addq.l  #8,sp
        rte

| --- data (kept in text: position independent, resident) -----------------
nf_id:          .long   0
host_flags:     .long   0
init_mode:      .word   0
| Getrez 0/1/2 -> the Falcon ST-compatible VGA mode (ST low, medium, high)
rez2mode:       .word   0x0192, 0x0199, 0x0098, 0x0192
ste_buf:        .space  32
own_jar:        .space  JAR_SLOTS*8

nf_name:        .asciz  "PSVIDEL"
msg_ok:         .asciz  "PSVIDEL: Falcon Videl + SuperVidel XBIOS on the HDMI output\r\n"
msg_vram:       .asciz  "PSVIDEL: video RAM at $A1000000, SupV cookie set\r\n"
msg_novram:     .asciz  "PSVIDEL: 24-bit bus - no video RAM, no SupV cookie\r\n"
msg_dsp:        .asciz  "PSVIDEL: Falcon DSP56001 + sound matrix, XBIOS 104-105 128-141\r\n"
msg_nonf:       .asciz  "PSVIDEL: no PSVIDEL NatFeat (cfg psvidel / falcon_dsp) - not installed\r\n"
msg_refused:    .asciz  "PSVIDEL: the host refused - not installed\r\n"
msg_twice:      .asciz  "PSVIDEL: already installed\r\n"
        .even
