| STE split screen: the playfield (rows 0-99) fine-scrolled by 4 pixels
| with line width 4 words, then Timer B (count 100) points the video
| counter at a status panel at $18000 (solid colour 15), scroll and line
| width off. Run with --ste. Real STE: rows 0-99 the row pattern shifted
| 4 px left (stride 168+8 bytes), rows 100-199 solid colour 15.
    .include "common.s"
    move.w  #220,%d0
    bsr     fill_rows
    bsr     set_pal
    lea     0x18000,%a0             | panel: all four planes set
    move.w  #8000-1,%d0
1:  move.l  #-1,(%a0)+
    dbra    %d0,1b
    lea     vbl(%pc),%a0
    move.l  %a0,0x70:w
    lea     tb(%pc),%a0
    move.l  %a0,0x120:w
    bset    #0,IERA
    bset    #0,IMRA
    move.w  #0x2300,%sr
main: bra.s main
vbl: move.b #4,0xff8265
    move.b  #4,0xff820f
    clr.b   TBCR
    move.b  #100,TBDR
    move.b  #8,TBCR
    rte
tb: clr.b   0xff8265
    clr.b   0xff820f
    move.b  #0x01,0xff8205          | counter -> $018000 for the next line
    move.b  #0x80,0xff8207
    move.b  #0x00,0xff8209
    rte
