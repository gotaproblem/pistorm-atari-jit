| Medium-resolution status area: Timer B (count 150) switches to medium,
| the VBL back to low. Memory row k in colour k & 15.
| Real ST: rows 0-149 low res, rows 150-199 medium res.
    .include "common.s"
    move.w  #200,%d0
    bsr     fill_rows
    bsr     set_pal
    lea     vbl(%pc),%a0
    move.l  %a0,0x70:w
    lea     tb(%pc),%a0
    move.l  %a0,0x120:w
    bset    #0,IERA
    bset    #0,IMRA
    move.w  #0x2300,%sr
main: bra.s main
vbl: clr.b  REZ
    clr.b   TBCR
    move.b  #150,TBDR
    move.b  #8,TBCR
    rte
tb: move.b  #1,REZ
    rte
