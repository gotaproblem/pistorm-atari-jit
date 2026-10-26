| CHILD.PRG for the HOSTDOS bench: needs relocation, BSS and a basepage.
        .text
        .globl  _start
RES     =       0x8000
_start:
        move.l  4(sp),a5
        move.l  #dval,a0                 | relocated
        move.l  (a0),RES+160
        move.l  #bssv,a0
        move.l  (a0),RES+164
        lea     _start(pc),a0
        moveq   #0,d0
        cmp.l   8(a5),a0
        bne.s   1f
        moveq   #1,d0
1:      move.l  d0,RES+168
        clr.w   -(sp)
        pea     s_inner(pc)
        move.w  #0x3D,-(sp)
        trap    #1
        addq.l  #8,sp
        move.l  d0,RES+172
        move.w  d0,-(sp)
        move.w  #0x3E,-(sp)
        trap    #1
        addq.l  #4,sp
        moveq   #0,d0
        move.b  0x81(a5),d0
        move.l  d0,RES+176
        move.w  #42,-(sp)
        move.w  #0x4C,-(sp)
        trap    #1
dval:   .long   0x12345678
s_inner: .asciz "INNER.DAT"
        .bss
bssv:   .space  4
