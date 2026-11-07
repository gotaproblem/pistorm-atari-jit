| Top and bottom border removal by HBL count (60 Hz across cycle 502 of
| lines 33 and 262). Screen: memory row k in colour k & 15.
| Real ST: display lines 34..309 = 276 rows, row y shows memory row y.
    .include "common.s"
    move.w  #290,%d0
    bsr     fill_rows
    bsr     set_pal
    lea     vbl(%pc),%a0
    move.l  %a0,0x70:w
    lea     hbl(%pc),%a0
    move.l  %a0,0x68:w
    move.w  #0x2100,%sr
main: bra.s main
vbl: moveq #0,%d6
    rte
hbl: addq.w  #1,%d6
    cmp.w   #33,%d6
    beq.s   flip
    cmp.w   #262,%d6
    beq.s   flip
    rte
flip: move.w  #30,%d5             | to ~cycle 470 (ST bus timing): after DE end
1:  dbra    %d5,1b
    move.b  #0,SYNC                 | 60 Hz across cycle 502 ...
    .rept 12
    nop
    .endr
    move.b  #2,SYNC                 | ... 50 Hz again before 52 of the next
    rte
