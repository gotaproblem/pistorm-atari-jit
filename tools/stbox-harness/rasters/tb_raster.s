| Timer B raster bars: event count 1, the handler sets colour 0 = line
| number. Real ST: row r shows colour r & $777 (row 0: 0 from the VBL).
    .include "common.s"
    lea     vbl(%pc),%a0
    move.l  %a0,0x70:w
    lea     tb(%pc),%a0
    move.l  %a0,0x120:w
    move.b  #1,TBDR
    move.b  #8,TBCR
    bset    #0,IERA
    bset    #0,IMRA
    move.w  #0x2300,%sr
main: bra.s main
vbl: moveq #0,%d7
    move.w  %d7,PAL
    rte
tb: addq.w  #1,%d7
    move.w  %d7,PAL
    rte
