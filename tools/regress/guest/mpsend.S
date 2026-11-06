| mpsend.S - MPSEND.PRG, put on the boot floppy as MPSEND.ACC: make Paula
|
| Paula 2.6 takes a module on its command line or from its own MP.PRG -
| both free() a string literal (MP.C: pcmd="\0" ... free(pcmd)) and die
| with an address error on a 68000 (Hatari, 6 Nov 2026). This accessory
| sends the MP_START message MP.PRG would: it reads A:\MPSEND.INF (one
| line, the module's full path), waits for an application named
| "PAULA   " (started by the desktop's autorun) and sends MP_START with
| that path. Then it sleeps in evnt_mesag like any accessory.
|
| Standalone (not lib.S): an accessory starts with its basepage in a0 and
| no stack of its own, and must never Pterm.

        .text
_start:
        lea     stack_top,sp
        lea     aespb,a0                | AES parameter block, filled here
        move.l  #aes_ctl,(a0)+
        move.l  #aes_glob,(a0)+
        move.l  #aes_ii,(a0)+
        move.l  #aes_io,(a0)+
        move.l  #aes_ai,(a0)+
        move.l  #aes_ao,(a0)+

        | ---- the module's path from A:\MPSEND.INF
        clr.w   -(sp)
        pea     inf_name(pc)
        move.w  #0x3D,-(sp)             | Fopen
        trap    #1
        addq.l  #8,sp
        tst.l   d0
        bmi     sleep
        move.w  d0,d3
        pea     modpath
        move.l  #200,-(sp)
        move.w  d3,-(sp)
        move.w  #0x3F,-(sp)             | Fread
        trap    #1
        lea     12(sp),sp
        move.l  d0,d4
        move.w  d3,-(sp)
        move.w  #0x3E,-(sp)             | Fclose
        trap    #1
        addq.l  #4,sp
        tst.l   d4
        ble     sleep
        lea     modpath,a0              | cut at CR/LF/end
1:      move.b  (a0),d0
        beq.s   2f
        cmp.b   #13,d0
        beq.s   2f
        cmp.b   #10,d0
        beq.s   2f
        addq.l  #1,a0
        bra.s   1b
2:      clr.b   (a0)

        | ---- appl_init
        move.w  #10,d0
        moveq   #0,d1
        moveq   #1,d2
        moveq   #0,d3
        bsr     aes
        move.w  aes_io,apid

        | ---- every 2 s: is Paula there yet? (up to 2 minutes)
        moveq   #59,d6
3:      move.w  #2000,aes_ii             | evnt_timer 2000 ms
        clr.w   aes_ii+2
        move.w  #24,d0
        moveq   #2,d1
        moveq   #1,d2
        moveq   #0,d3
        bsr     aes
        move.l  #paula_name,aes_ai      | appl_find
        move.w  #13,d0
        moveq   #0,d1
        moveq   #1,d2
        moveq   #1,d3
        bsr     aes
        move.w  aes_io,d5
        bpl.s   4f
        dbra    d6,3b
        bra.s   sleep

4:      | ---- MP_START with the path
        lea     msg,a0
        move.w  #0x4802,(a0)+           | MP_START
        move.w  apid,(a0)+
        clr.w   (a0)+
        move.l  #modpath,(a0)+          | msg[3..4]
        clr.l   (a0)+
        clr.w   (a0)+
        move.w  d5,aes_ii                | appl_write(paula, 16, msg)
        move.w  #16,aes_ii+2
        move.l  #msg,aes_ai
        move.w  #12,d0
        moveq   #2,d1
        moveq   #1,d2
        moveq   #1,d3
        bsr     aes

sleep:  | an accessory never ends: wait for messages forever
        move.l  #msg,aes_ai
        move.w  #23,d0                  | evnt_mesag
        moveq   #0,d1
        moveq   #1,d2
        moveq   #1,d3
        bsr     aes
        bra.s   sleep

| aes: d0 opcode, d1 aes_ii count, d2 aes_io count, d3 aes_ai count
aes:
        lea     aes_ctl,a0
        move.w  d0,(a0)+
        move.w  d1,(a0)+
        move.w  d2,(a0)+
        move.w  d3,(a0)+
        clr.w   (a0)
        move.l  #aespb,d1
        move.w  #200,d0
        trap    #2
        rts

inf_name:   .asciz  "A:\\MPSEND.INF"
paula_name: .asciz  "PAULA   "
        .even


        .bss
aespb:   .space 24
aes_ctl: .space 10
aes_glob:  .space 30
aes_ii:   .space 32
aes_io:  .space 16
aes_ai:  .space 16
aes_ao: .space 8
apid:    .space 2
msg:     .space 16
modpath: .space 208
        .space  1024
stack_top:
