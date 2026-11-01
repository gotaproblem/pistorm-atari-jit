| fdd.S - FDD.PRG: the floppy controller and DMA, through TOS.
| The boot disk carries PATTERN.DAT (the image builder's pattern: byte i
| is (i ^ i>>8 ^ i>>16) & 255), written first so it starts at sector 14
| (track 0, side 1, sector 6).
|   fdd_read      GEMDOS reads all of PATTERN.DAT in 16 KB pieces, checked
|   fdd_floprd    XBIOS Floprd: 4 sectors from mid-track (T0 S1 6-9) and a
|                 whole track (T1 S0 1-9) - multi-sector reads straight
|                 through the FDC, checked against the pattern
|   fdd_write     GEMDOS writes a 64 KB file, reads it back, deletes it
| + metric: read speed in bytes per second

        .include "lib.S"

progname: .asciz "FDD"
        .even

PSIZE   =       300000                  | runner and image builder agree
CHUNK   =       16384

main:
        move.w  #0x2300,sr
        bsr     t_read
        bsr     t_floprd
        bsr     t_write
        rts

| verify: a0 = buffer, d0 = file offset of its first byte, d1 = length,
| d2 = added to the offset before the formula (0, or 5 for the write test)
| -> d0 = 0 if all match, else the bad offset + 1
verify:
        movem.l d1-d6/a0,-(sp)
        move.l  d0,d3
        add.l   d2,d3                   | i
        subq.l  #1,d1
        bmi.s   8f
1:      move.l  d3,d4
        lsr.l   #8,d4
        move.l  d3,d5
        swap    d5
        eor.l   d3,d4
        eor.l   d5,d4                   | low byte = i ^ i>>8 ^ i>>16
        cmp.b   (a0)+,d4
        bne.s   7f
        addq.l  #1,d3
        subq.l  #1,d1
        bpl.s   1b
8:      moveq   #0,d0
        bra.s   9f
7:      move.l  d3,d0
        sub.l   d2,d0
        addq.l  #1,d0
9:      movem.l (sp)+,d1-d6/a0
        rts

| fopen a0 = name, d0 = mode -> d0 handle (or negative)
fopen:
        move.w  d0,-(sp)
        move.l  a0,-(sp)
        move.w  #0x3D,-(sp)
        trap    #1
        addq.l  #8,sp
        rts
fclose:
        move.w  d0,-(sp)
        move.w  #0x3E,-(sp)
        trap    #1
        addq.l  #4,sp
        rts
| fread d0 = handle, d1 = count, a0 = buffer -> d0
fread:
        move.l  a0,-(sp)
        move.l  d1,-(sp)
        move.w  d0,-(sp)
        move.w  #0x3F,-(sp)
        trap    #1
        lea     12(sp),sp
        rts
fwrite:
        move.l  a0,-(sp)
        move.l  d1,-(sp)
        move.w  d0,-(sp)
        move.w  #0x40,-(sp)
        trap    #1
        lea     12(sp),sp
        rts

t_read:
        bsr     now
        move.l  d0,t0
        lea     n_pat(pc),a0
        moveq   #0,d0
        bsr     fopen
        move.l  d0,d7                   | handle
        bmi     9f
        moveq   #0,d6                   | offset
        moveq   #0,d5                   | first bad + 1
1:      move.l  d7,d0
        move.l  #CHUNK,d1
        lea     buf,a0
        bsr     fread
        tst.l   d0
        ble.s   2f
        move.l  d0,d1
        lea     buf,a0
        move.l  d6,d0
        moveq   #0,d2
        bsr     verify
        tst.l   d5
        bne.s   3f
        move.l  d0,d5
3:      add.l   d1,d6
        bra.s   1b
2:      move.l  d7,d0
        bsr     fclose
        bsr     now
        sub.l   t0,d0
        move.l  d0,d1                   | bytes/s = bytes * 200 / ticks
        beq.s   4f
        move.l  d6,d0
        lsr.l   #3,d0                   | (bytes/8) * 200 / ticks * 8
        mulu    #200,d0
        divu    d1,d0
        and.l   #0xFFFF,d0
        lsl.l   #3,d0
4:      METRIC  fdd_read_bytes_per_s
        tst.l   d5
        bne.s   5f
        cmp.l   #PSIZE,d6
5:      CHECK   eq, fdd_read
        PS      "bytes="
        move.l  d6,d0
        bsr     pr_dec
        PS      " expect=300000 first-bad+1="
        move.l  d5,d0
        bsr     pr_dec
        bra     pr_flush
9:      TFAIL   fdd_read
        PS      "PATTERN.DAT will not open: "
        move.l  d7,d0
        bsr     pr_dec
        bra     pr_flush

| floprd: d1 = sector, d2 = track, d3 = side, d4 = count -> d0 (buf)
floprd:
        move.w  d4,-(sp)
        move.w  d3,-(sp)
        move.w  d2,-(sp)
        move.w  d1,-(sp)
        clr.w   -(sp)                   | drive A
        clr.l   -(sp)
        pea     buf
        move.w  #8,-(sp)
        trap    #14
        lea     20(sp),sp
        rts

t_floprd:
        moveq   #6,d1                   | T0 side 1 sectors 6-9 = pattern 0..2047
        moveq   #0,d2
        moveq   #1,d3
        moveq   #4,d4
        bsr     floprd
        move.l  d0,d6                   | XBIOS result
        lea     buf,a0
        moveq   #0,d0
        move.l  #2048,d1
        moveq   #0,d2
        bsr     verify
        move.l  d0,d5
        moveq   #1,d1                   | T1 side 0 sectors 1-9 = pattern 2048..
        moveq   #1,d2
        moveq   #0,d3
        moveq   #9,d4
        bsr     floprd
        move.l  d0,d7
        lea     buf,a0
        move.l  #2048,d0
        move.l  #4608,d1
        moveq   #0,d2
        bsr     verify
        move.l  d0,d4
        move.l  d6,d0
        or.l    d7,d0
        or.l    d5,d0
        or.l    d4,d0
        tst.l   d0
        CHECK   eq, fdd_floprd
        PS      "rc="
        move.l  d6,d0
        bsr     pr_dec
        PS      ","
        move.l  d7,d0
        bsr     pr_dec
        PS      " first-bad+1="
        move.l  d5,d0
        bsr     pr_dec
        PS      ","
        move.l  d4,d0
        bsr     pr_dec
        bra     pr_flush

t_write:
        lea     buf,a0                  | 64 KB of pattern(i + 5)
        move.l  #65535,d1
        moveq   #5,d3
1:      move.l  d3,d4
        lsr.l   #8,d4
        move.l  d3,d5
        swap    d5
        eor.l   d3,d4
        eor.l   d5,d4
        move.b  d4,(a0)+
        addq.l  #1,d3
        dbra    d1,1b

        clr.w   -(sp)                   | Fcreate
        pea     n_w(pc)
        move.w  #0x3C,-(sp)
        trap    #1
        addq.l  #8,sp
        move.l  d0,d7
        bmi     8f
        move.l  d7,d0
        move.l  #65536,d1
        lea     buf,a0
        bsr     fwrite
        move.l  d0,d6                   | bytes written
        move.l  d7,d0
        bsr     fclose
        lea     buf,a0                  | wipe, then read back
        move.l  #16383,d1
2:      clr.l   (a0)+
        dbra    d1,2b
        lea     n_w(pc),a0
        moveq   #0,d0
        bsr     fopen
        move.l  d0,d7
        bmi     8f
        move.l  d7,d0
        move.l  #65536,d1
        lea     buf,a0
        bsr     fread
        move.l  d0,d5                   | bytes read
        move.l  d7,d0
        bsr     fclose
        lea     buf,a0
        moveq   #0,d0
        move.l  d5,d1
        moveq   #5,d2
        bsr     verify
        move.l  d0,d4
        pea     n_w(pc)                 | Fdelete
        move.w  #0x41,-(sp)
        trap    #1
        addq.l  #6,sp
        moveq   #0,d3
        cmp.l   #65536,d6
        bne.s   3f
        cmp.l   #65536,d5
        bne.s   3f
        tst.l   d4
        bne.s   3f
        moveq   #1,d3
3:      tst.l   d3
        CHECK   ne, fdd_write
        PS      "written="
        move.l  d6,d0
        bsr     pr_dec
        PS      " read="
        move.l  d5,d0
        bsr     pr_dec
        PS      " first-bad+1="
        move.l  d4,d0
        bsr     pr_dec
        bra     pr_flush
8:      TFAIL   fdd_write
        PS      "create/open failed: "
        move.l  d7,d0
        bsr     pr_dec
        bra     pr_flush

n_pat:  .asciz  "A:\\PATTERN.DAT"
n_w:    .asciz  "A:\\WTEST.DAT"
        .even

        .bss
        .even
t0:     .space  4
buf:    .space  65536+16
        .text
