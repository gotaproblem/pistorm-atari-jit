| acsi.S - ACSI.PRG: an emulated ACSI target through the real DMA chip,
| driven the way EmuTOS's bios/acsi.c does it, on the mkimg.py hd pattern
| image as ACSI ID 0. The same DMA chip feeds the floppy, so the floppy
| programs that run after this one check the hand-back as well.
|   acsi_tur       TEST UNIT READY: status 0
|   acsi_inquiry   INQUIRY LUN 0: a direct-access device; LUN 1: $7F
|                  (no such LUN - 4856004)
|   acsi_read      READ(6): LBA 1 x1, 777 x16, last x1, checked
|   acsi_dma_addr  after a read the DMA address counter has advanced by
|                  exactly the bytes moved (22149ec)
|   acsi_write     WRITE(6) 4 at LBA 9000, read back
|   acsi_range     a read past the end ends in a non-zero status

        .include "lib.S"

progname: .asciz "ACSI"
        .even

DMADATA =       0xFFFF8604
DMACTL  =       0xFFFF8606
DMAHI   =       0xFFFF8609
DMAMID  =       0xFFFF860B
DMALO   =       0xFFFF860D
GPIP    =       0xFFFFFA01
IMRB    =       0xFFFFFA15
FLOCK   =       0x43E

C_WR    =       0x0100
C_DRQFL =       0x0080
C_CS    =       0x0008
C_SCREG =       0x0010
C_NNEW  =       0x0002

main:
        bsr     save_mfp
        move.w  #0x2300,sr
        bclr    #7,IMRB                 | GPIP5 polled, not interrupting
        bsr     t_tur
        bsr     t_inquiry
        bsr     t_read
        bsr     t_write
        bsr     t_range
        bsr     restore_mfp
        rts

delay:  move.l  d0,-(sp)
        moveq   #15,d0
1:      dbra    d0,1b
        move.l  (sp)+,d0
        rts

| wait_gpip: d0 = ticks -> d0 = 0 when GPIP5 went low, -1 on timeout
wait_gpip:
        move.l  d1,-(sp)
        add.l   HZ200,d0
        move.l  d0,d1
1:      btst    #5,GPIP
        beq.s   2f
        cmp.l   HZ200,d1
        bpl.s   1b
        moveq   #-1,d0
        bra.s   3f
2:      moveq   #0,d0
3:      move.l  (sp)+,d1
        rts

| set_dma: a0 = address
set_dma:
        move.l  a0,d0
        move.b  d0,DMALO
        lsr.l   #8,d0
        move.b  d0,DMAMID
        lsr.l   #8,d0
        move.b  d0,DMAHI
        rts

| get_dma -> d0 = the DMA address counter
get_dma:
        moveq   #0,d0
        move.b  DMAHI,d0
        lsl.l   #8,d0
        move.b  DMAMID,d0
        lsl.l   #8,d0
        move.b  DMALO,d0
        rts

| acsi: a0 = buffer, a1 = 6-byte CDB (ID 0), d1 = sectors for the DMA
| count, d2 = 0 read / 1 write -> d0 = status byte, or -1 on a timeout
acsi:
        movem.l d1-d5/a1,-(sp)
        st      FLOCK                   | TOS's floppy VBL keeps off the DMA
        bsr     set_dma
        move.w  #C_DRQFL,d3
        tst.w   d2
        beq.s   1f
        or.w    #C_WR,d3
1:      move.w  d3,d0                   | select sector count, toggle WR
        or.w    #C_SCREG,d0
        move.w  d0,d4
        eor.w   #C_WR,d4
        move.w  d4,DMACTL
        bsr     delay
        move.w  d0,DMACTL
        bsr     delay
        move.w  d1,DMADATA
        or.w    #C_CS,d3                | command
        move.w  d3,DMACTL
        or.w    #C_NNEW,d3
        moveq   #4,d5                   | bytes 0-4
2:      moveq   #0,d0
        move.b  (a1)+,d0
        swap    d0
        move.w  d3,d0
        move.l  d0,DMADATA
        moveq   #20,d0                  | 100 ms
        bsr     wait_gpip
        tst.l   d0
        bmi.s   8f
        dbra    d5,2b
        moveq   #0,d0                   | last byte starts the DMA
        move.b  (a1)+,d0
        swap    d0
        move.w  d3,d0
        and.w   #0xFF00,d0
        move.l  d0,DMADATA
        move.l  #400,d0                 | 2 s
        bsr     wait_gpip
        tst.l   d0
        bmi.s   8f
        and.w   #~C_WR,d3               | status
        move.w  d3,DMACTL
        move.w  DMADATA,d0
        and.l   #0xFF,d0
        bra.s   9f
8:      moveq   #-1,d0
9:      move.w  #C_DRQFL,DMACTL         | back to the floppy
        sf      FLOCK
        movem.l (sp)+,d1-d5/a1
        rts

| cdb6: build a CDB in `cdb`: d0 = opcode, d1 = LBA (21 bits), d2 = byte 4
| -> a1 = cdb
cdb6:
        lea     cdb,a1
        move.b  d0,(a1)
        move.l  d1,d0
        swap    d0
        and.b   #0x1F,d0
        move.b  d0,1(a1)
        move.l  d1,d0
        lsr.w   #8,d0
        move.b  d0,2(a1)
        move.b  d1,3(a1)
        move.b  d2,4(a1)
        clr.b   5(a1)
        rts

t_tur:
        moveq   #0,d0
        moveq   #0,d1
        moveq   #0,d2
        bsr     cdb6
        lea     buf,a0
        moveq   #0,d1
        moveq   #0,d2
        bsr     acsi
        move.l  d0,d7
        tst.l   d7
        CHECK   eq, acsi_tur
        PS      "status="
        move.l  d7,d0
        bsr     pr_dec
        bra     pr_flush

t_inquiry:
        moveq   #0x12,d0                | LUN 0
        moveq   #0,d1
        moveq   #36,d2
        bsr     cdb6
        lea     buf,a0
        move.b  #0xEE,(a0)
        moveq   #1,d1
        moveq   #0,d2
        bsr     acsi
        move.l  d0,d6
        moveq   #0,d7
        move.b  buf,d7                  | peripheral type
        moveq   #0x12,d0                | LUN 1
        moveq   #0,d1
        moveq   #36,d2
        bsr     cdb6
        or.b    #0x20,1(a1)
        lea     buf,a0
        move.b  #0xEE,(a0)
        moveq   #1,d1
        moveq   #0,d2
        bsr     acsi
        move.l  d0,d5
        moveq   #0,d4
        move.b  buf,d4
        moveq   #0,d3
        tst.l   d6
        bne.s   1f
        tst.b   d7
        bne.s   1f
        cmp.b   #0x7F,d4
        bne.s   1f
        moveq   #1,d3
1:      tst.l   d3
        CHECK   ne, acsi_inquiry
        PS      "lun0 status="
        move.l  d6,d0
        bsr     pr_dec
        PS      " type="
        move.l  d7,d0
        bsr     pr_hex
        PS      " lun1 status="
        move.l  d5,d0
        bsr     pr_dec
        PS      " type="
        move.l  d4,d0
        bsr     pr_hex
        PS      " expect 0,$00,*,$7F"
        bra     pr_flush

| rd: d1 = LBA, d2 = count, a0 = buffer -> d0 = status
rd:
        movem.l d1-d2,-(sp)
        moveq   #0x08,d0
        bsr     cdb6
        move.l  d2,d1
        moveq   #0,d2
        bsr     acsi
        movem.l (sp)+,d1-d2
        rts

| check_buf: a0, d0 = first LBA, d1 = count, d2 = k -> d0 (0 = good)
check_buf:
        movem.l d1-d5/a0,-(sp)
        move.l  d0,d4
        moveq   #0,d5
1:      move.l  d4,d0
        move.l  d1,-(sp)
        move.l  d2,d1
        bsr     hd_check
        move.l  (sp)+,d1
        tst.l   d0
        bne.s   2f
        lea     512(a0),a0
        addq.l  #1,d4
        addq.l  #1,d5
        cmp.l   d1,d5
        bne.s   1b
        moveq   #0,d0
        bra.s   9f
2:      swap    d5
        clr.w   d5
        or.l    d5,d0
9:      movem.l (sp)+,d1-d5/a0
        rts

| rd1: one checked read, appended to the @I line -> d0 = 0 if good
rd1:
        movem.l d1-d3/a0,-(sp)
        lea     buf,a0
        bsr     rd
        move.l  d0,d3
        bne.s   1f
        move.l  d1,d0
        move.l  d2,d1
        moveq   #0,d2
        bsr     check_buf
        move.l  d0,d3
1:      PS      " lba"
        move.l  (sp),d0
        bsr     pr_dec
        PS      "x"
        move.l  4(sp),d0
        bsr     pr_dec
        PS      "="
        move.l  d3,d0
        bsr     pr_hex
        move.l  d3,d0
        movem.l (sp)+,d1-d3/a0
        rts

t_read:
        PS      "@I acsi_read"
        moveq   #0,d6
        moveq   #1,d1
        moveq   #1,d2
        bsr     rd1
        or.l    d0,d6
        move.l  #777,d1
        moveq   #16,d2
        bsr     rd1
        or.l    d0,d6
        bsr     get_dma                 | the 16-sector read moved 8192 bytes
        move.l  d0,d5
        move.l  #HDSECT-1,d1
        moveq   #1,d2
        bsr     rd1
        or.l    d0,d6
        bsr     pr_flush
        tst.l   d6
        CHECK   eq, acsi_read
        PS      "(see the line above)"
        bsr     pr_flush
        move.l  #buf+8192,d4
        cmp.l   d4,d5
        CHECK   eq, acsi_dma_addr
        PS      "after 16 sectors="
        move.l  d5,d0
        bsr     pr_hex
        PS      " expect="
        move.l  d4,d0
        bsr     pr_hex
        bra     pr_flush

t_write:
        lea     buf,a0
        move.l  #9000,d0
        moveq   #77,d1
        moveq   #3,d2
1:      bsr     hd_fill
        lea     512(a0),a0
        addq.l  #1,d0
        dbra    d2,1b
        moveq   #0x0A,d0
        move.l  #9000,d1
        moveq   #4,d2
        bsr     cdb6
        lea     buf,a0
        moveq   #4,d1
        moveq   #1,d2
        bsr     acsi
        move.l  d0,d7
        bne.s   8f
        lea     buf,a0
        move.w  #511,d2
2:      clr.l   (a0)+
        dbra    d2,2b
        lea     buf,a0
        move.l  #9000,d1
        moveq   #4,d2
        bsr     rd
        move.l  d0,d7
        bne.s   8f
        lea     buf,a0
        move.l  #9000,d0
        moveq   #4,d1
        moveq   #77,d2
        bsr     check_buf
        move.l  d0,d7
8:      tst.l   d7
        CHECK   eq, acsi_write
        PS      "result="
        move.l  d7,d0
        bsr     pr_hex
        bra     pr_flush

t_range:
        lea     buf,a0
        move.l  #HDSECT,d1
        moveq   #1,d2
        bsr     rd
        move.l  d0,d7
        tst.l   d7
        CHECK   ne, acsi_range
        PS      "status="
        move.l  d7,d0
        bsr     pr_dec
        PS      " expect non-zero"
        bra     pr_flush

        .bss
        .even
cdb:    .space  6
        .even
buf:    .space  16*512
        .text
