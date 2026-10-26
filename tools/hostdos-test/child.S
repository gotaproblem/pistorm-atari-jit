| CHILD.PRG for the HOSTDOS bench: needs relocation, BSS and a basepage.
        .text
        .globl  _start
RES     =       0x8000
_start:
        move.l  4(sp),a5
| Launched with command line "X" (by TEST.PRG): first start ourselves again
| through HOSTDOS with "Y" - a host-drive program starting another, as
| !RUNME.TOS -> COREMARK.TOS - and check our stack and registers survive.
| Results at RES+0x1E0 (grandchild's exit code), +0x1E4/+0x1E8 (sp before/
| after), +0x1EC (d7) and +0x1F0 (a6). The "Y" run does not recurse.
        cmp.b   #'X',0x81(a5)
        bne.s   9f
        move.l  sp,RES+0x1E4
        move.l  #0x7777D7D7,d7
        move.l  #0xA6A6A6A6,a6
        clr.l   -(sp)                   | Pexec(0, S:\CHILD.PRG, "Y", 0)
        pea     tail_y(pc)
        pea     s_self(pc)
        clr.w   -(sp)
        move.w  #0x4B,-(sp)
        trap    #1
        lea     16(sp),sp
        move.l  d0,RES+0x1E0
        move.l  sp,RES+0x1E8
        move.l  d7,RES+0x1EC
        move.l  a6,RES+0x1F0
9:
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
s_self:  .asciz "S:\\CHILD.PRG"
tail_y:  .byte 1,'Y',0
        .even
        .bss
bssv:   .space  4
