| common.s - ROM header + machine setup for the STBOX raster test ROMs.
| Each test is a tiny "TOS ROM" at $FC0000: reset SSP/PC at 0/4, the ROM
| base at 8 (where stbox_core_setup reads it). Screen at $010000, low res,
| 50 Hz, MFP all off with auto-EOI vectors from $40 ($100), every autovector
| on an RTE. GNU as (m68k-linux-gnu-as -m68000), linked at $FC0000.
    .equ SCR,   0x10000
    .equ PAL,   0xff8240
    .equ SYNC,  0xff820a
    .equ REZ,   0xff8260
    .equ VCLO,  0xff8209
    .equ VCMI,  0xff8207
    .equ IERA,  0xfffa07
    .equ IMRA,  0xfffa13
    .equ VR,    0xfffa17
    .equ TBCR,  0xfffa1b
    .equ TBDR,  0xfffa21
    .equ FLAG,  0x7000          | VBL flag (word)

    .text
    .long 0x00008000
    .long start
    .long 0x00fc0000
    .long 0

| fill_rows: d0.w = rows. Row k gets colour index (k & 15) across the line.
fill_rows:
    lea     SCR,%a0
    moveq   #0,%d1
1:  moveq   #3,%d3
    move.w  %d1,%d2
2:  lsr.w   #1,%d2                  | plane bits of (k & 15), plane 0 first
    scs     %d4
    ext.w   %d4
    move.w  %d4,-(%sp)
    dbra    %d3,2b
    move.w  (%sp)+,%d4              | plane3 .. plane0 popped in reverse
    move.w  (%sp)+,%d5
    move.w  (%sp)+,%d6
    move.w  (%sp)+,%d7
    moveq   #19,%d3
3:  move.w  %d7,(%a0)+              | plane 0
    move.w  %d6,(%a0)+
    move.w  %d5,(%a0)+
    move.w  %d4,(%a0)+
    dbra    %d3,3b
    addq.w  #1,%d1
    cmp.w   %d0,%d1
    blo.s   1b
    rts

| sixteen distinct colours
set_pal:
    lea     PAL,%a0
    lea     pal16(%pc),%a1
    moveq   #15,%d0
1:  move.w  (%a1)+,(%a0)+
    dbra    %d0,1b
    rts
pal16:
    .word 0x000,0x700,0x070,0x007,0x770,0x707,0x077,0x777
    .word 0x300,0x030,0x003,0x330,0x303,0x033,0x333,0x531

rte_h:
    rte

setup:
    move.b  #0x01,0xff8201
    move.b  #0x00,0xff8203
    clr.b   REZ
    move.b  #2,SYNC
    clr.b   IERA
    clr.b   0xfffa09
    clr.b   IMRA
    clr.b   0xfffa15
    clr.b   TBCR
    move.b  #0x40,VR                | vectors $100.., auto EOI
    lea     rte_h(%pc),%a0
    move.l  %a0,0x68:w
    move.l  %a0,0x70:w
    move.l  %a0,0x120:w
    clr.w   FLAG:w
    rts

start:
    move.w  #0x2700,%sr
    lea     0x8000,%sp
    bsr     setup
