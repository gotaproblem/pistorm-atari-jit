| ide.S - IDE.PRG: the emulated IDE controller at $F00000, driven directly
| (PIO, LBA28, no driver), on the mkimg.py hd pattern image as drive 0.
|   ide_present    the registers answer (no bus error)
|   ide_identify   IDENTIFY: words 60-61 (LBA sectors) = the image size
|   ide_read       READ SECTORS: LBA 1 x1, 777 x16, last x1 - every sector
|                  checked against the pattern
|   ide_intrq      INTRQ (GPIP5 low) comes for every sector, and is gone
|                  after the last one - a model that leaves it asserted
|                  makes TOS's next ACSI probe read garbage (8326bd3)
|   ide_write      WRITE SECTORS 4 at LBA 9000, read back
|   ide_range      a read past the end sets ERR, returns no data

        .include "lib.S"

progname: .asciz "IDE"
        .even

IDE     =       0xFFF00000
I_DATA  =       IDE+0x00
I_ERR   =       IDE+0x05
I_CNT   =       IDE+0x09
I_LBA0  =       IDE+0x0D
I_LBA1  =       IDE+0x11
I_LBA2  =       IDE+0x15
I_DEV   =       IDE+0x19
I_STAT  =       IDE+0x1D
I_CMD   =       IDE+0x1D
I_CTRL  =       IDE+0x39
GPIP    =       0xFFFFFA01
IMRB    =       0xFFFFFA15

main:
        bsr     save_mfp
        move.w  #0x2300,sr
        bclr    #7,IMRB                 | GPIP5's interrupt stays off: polled
        lea     I_STAT-1,a0
        bsr     probe_w
        tst.l   d0
        CHECK   eq, ide_present
        PS      "status reg at $F0001D"
        bsr     pr_flush
        tst.l   d0
        bne     9f
        move.b  #0x00,I_CTRL            | nIEN = 0: INTRQ enabled
        clr.l   nointrq
        clr.l   stuck

        bsr     t_identify
        bsr     t_read
        bsr     t_write
        bsr     t_range

        move.l  nointrq,d0
        METRIC  ide_sectors_without_intrq
        move.l  stuck,d0
        METRIC  ide_intrq_left_asserted
        move.l  nointrq,d0
        add.l   stuck,d0
        tst.l   d0
        CHECK   eq, ide_intrq
        PS      "missing="
        move.l  nointrq,d0
        bsr     pr_dec
        PS      " left-asserted="
        move.l  stuck,d0
        bsr     pr_dec
        bsr     pr_flush
9:      bsr     restore_mfp
        rts

| wait_busy: up to 2 s for BSY to clear -> d0 = status, or -1
wait_busy:
        move.l  d1,-(sp)
        move.l  HZ200,d1
        add.l   #400,d1
1:      move.b  I_STAT,d0
        and.l   #0xFF,d0
        btst    #7,d0
        beq.s   2f
        cmp.l   HZ200,d1
        bpl.s   1b
        moveq   #-1,d0
2:      move.l  (sp)+,d1
        rts

| wait_intrq: up to 1 s for GPIP5 low -> d0 = 0, or -1 (counted)
wait_intrq:
        move.l  d1,-(sp)
        move.l  HZ200,d1
        add.l   #200,d1
1:      btst    #5,GPIP
        beq.s   2f
        cmp.l   HZ200,d1
        bpl.s   1b
        addq.l  #1,nointrq
        moveq   #-1,d0
        bra.s   3f
2:      moveq   #0,d0
3:      move.l  (sp)+,d1
        rts

| setup: d0 = LBA, d1 = count, d2 = command
setup:
        move.l  d0,-(sp)
        bsr     wait_busy
        move.l  (sp)+,d0
        move.b  d1,I_CNT
        move.b  d0,I_LBA0
        ror.l   #8,d0
        move.b  d0,I_LBA1
        ror.l   #8,d0
        move.b  d0,I_LBA2
        ror.l   #8,d0
        and.b   #0x0F,d0
        or.b    #0xE0,d0                | LBA, drive 0
        move.b  d0,I_DEV
        move.b  d2,I_CMD
        rts

| read_secs: d0 = LBA, d1 = count, a0 = buffer -> d0 = 0 or status|$100
read_secs:
        movem.l d1-d3/a0,-(sp)
        moveq   #0x20,d2
        bsr     setup
        move.l  d1,d3
        subq.l  #1,d3
1:      bsr     wait_intrq
        bsr     wait_busy               | reading status clears INTRQ
        btst    #0,d0
        bne.s   8f
        btst    #3,d0
        beq.s   8f
        move.w  #255,d2
2:      move.w  I_DATA,(a0)+
        dbra    d2,2b
        dbra    d3,1b
        bsr     after_last
        moveq   #0,d0
        bra.s   9f
8:      or.w    #0x100,d0
9:      movem.l (sp)+,d1-d3/a0
        rts

| after_last: once the last sector's data is out, INTRQ must not come back
after_last:
        move.l  d0,-(sp)
        moveq   #2,d0                   | 10 ms
        bsr     wait_ticks
        btst    #5,GPIP
        bne.s   1f
        addq.l  #1,stuck
        move.b  I_STAT,d0               | clear it so the next test is clean
1:      move.l  (sp)+,d0
        rts

t_identify:
        moveq   #0,d0
        moveq   #0,d1
        moveq   #0,d2
        bsr     wait_busy
        move.b  #0xA0,I_DEV
        move.b  #0xEC,I_CMD
        bsr     wait_intrq
        bsr     wait_busy
        move.l  d0,d7                   | status
        btst    #3,d0
        beq.s   1f
        lea     buf,a0
        move.w  #255,d2
2:      move.w  I_DATA,(a0)+
        dbra    d2,2b
        bsr     after_last
1:      lea     buf,a0
        move.w  120(a0),d3              | word 60 (low)
        move.w  122(a0),d4              | word 61 (high)
        move.w  d4,d5                   | as read
        swap    d5
        move.w  d3,d5
        ror.w   #8,d3                   | and byte-swapped
        ror.w   #8,d4
        move.w  d4,d6
        swap    d6
        move.w  d3,d6
        moveq   #0,d0
        cmp.l   #HDSECT,d5
        beq.s   3f
        moveq   #1,d0
        cmp.l   #HDSECT,d6
        beq.s   3f
        moveq   #-1,d0
3:      move.l  d0,d3
        METRIC  ide_identify_word_swap
        tst.l   d3
        CHECK   pl, ide_identify
        PS      "lba-sectors="
        move.l  d5,d0
        bsr     pr_dec
        PS      "/swapped "
        move.l  d6,d0
        bsr     pr_dec
        PS      " expect=16384 status="
        move.l  d7,d0
        bsr     pr_hex
        bra     pr_flush

| check_buf: a0 = buffer, d0 = first LBA, d1 = count, d2 = k
| -> d0 = 0, or 1 + (sector index << 16 | byte offset)
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

| one read check -> d0 = 0 ok, else nonzero; appends to the line
rd1:
        movem.l d1-d3/a0,-(sp)
        move.l  d0,d3                   | LBA
        lea     buf,a0
        bsr     read_secs
        tst.l   d0
        bne.s   1f
        move.l  d3,d0
        moveq   #0,d2
        bsr     check_buf
1:      move.l  d0,-(sp)
        PS      " lba"
        move.l  d3,d0
        bsr     pr_dec
        PS      "x"
        move.l  d1,d0
        bsr     pr_dec
        PS      "="
        move.l  (sp),d0
        bsr     pr_hex
        move.l  (sp)+,d0
        movem.l (sp)+,d1-d3/a0
        rts

t_read:
        PS      "@I ide_read"           | detail; the verdict is the @T below
        moveq   #0,d6
        moveq   #1,d0
        moveq   #1,d1
        bsr     rd1
        or.l    d0,d6
        move.l  #777,d0
        moveq   #16,d1
        bsr     rd1
        or.l    d0,d6
        move.l  #HDSECT-1,d0
        moveq   #1,d1
        bsr     rd1
        or.l    d0,d6
        bsr     pr_flush
        tst.l   d6
        CHECK   eq, ide_read
        PS      "(see the line above)"
        bra     pr_flush

t_write:
        lea     buf,a0                  | 4 sectors of pattern k=77
        move.l  #9000,d0
        moveq   #77,d1
        moveq   #3,d2
1:      bsr     hd_fill
        lea     512(a0),a0
        addq.l  #1,d0
        dbra    d2,1b
        move.l  #9000,d0
        moveq   #4,d1
        moveq   #0x30,d2
        bsr     setup
        lea     buf,a0
        moveq   #3,d3
        moveq   #0,d7
2:      bsr     wait_busy               | DRQ for this sector
        btst    #3,d0
        beq.s   7f
        move.w  #255,d2
3:      move.w  (a0)+,I_DATA
        dbra    d2,3b
        bsr     wait_intrq              | written
        dbra    d3,2b
        bsr     wait_busy
        bsr     after_last
        lea     buf,a0                  | wipe, read back
        move.w  #511,d2
4:      clr.l   (a0)+
        dbra    d2,4b
        move.l  #9000,d0
        moveq   #4,d1
        lea     buf,a0
        bsr     read_secs
        move.l  d0,d7
        bne.s   7f
        move.l  #9000,d0
        moveq   #4,d1
        moveq   #77,d2
        lea     buf,a0
        bsr     check_buf
        move.l  d0,d7
        bra.s   8f
7:      or.l    #0x80000000,d7
8:      tst.l   d7
        CHECK   eq, ide_write
        PS      "result="
        move.l  d7,d0
        bsr     pr_hex
        bra     pr_flush

t_range:
        move.l  nointrq,-(sp)           | a refusal need not interrupt
        move.l  #HDSECT,d0              | one past the end
        moveq   #1,d1
        moveq   #0x20,d2
        bsr     setup
        bsr     wait_intrq
        move.l  (sp)+,nointrq
        bsr     wait_busy
        move.l  d0,d7
        moveq   #0,d6
        move.b  I_ERR,d6
        moveq   #0,d3                   | pass = ERR set and no DRQ
        btst    #0,d7
        beq.s   1f
        btst    #3,d7
        bne.s   1f
        moveq   #1,d3
1:      tst.l   d3
        CHECK   ne, ide_range
        PS      "status="
        move.l  d7,d0
        bsr     pr_hex
        PS      " error="
        move.l  d6,d0
        bsr     pr_hex
        bra     pr_flush

        .bss
        .even
nointrq: .space 4
stuck:  .space  4
buf:    .space  16*512
        .text
