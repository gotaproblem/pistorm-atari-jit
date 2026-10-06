| FASTRAM.PRG - register PiStorm TT-RAM (Fast RAM at $01000000) with
| Falcon TOS 4.04 through Maddalt, and install an _FRB cookie.
|
| Size, first found wins:
|   1. command line          FASTRAM.TTP 64M      (rename .PRG to .TTP)
|   2. \AUTO\FASTRAM.INF, then FASTRAM.INF in the current directory:
|      first line, e.g.  128M   64M   65536K   OFF
|      a plain number is KB, K/M/G scale it (as psctrl.cfg's ttram)
|   3. the emulator (NatFeat PSCTRL, GETINT 4 = TT-RAM mapped)
| A size from 1 or 2 is clamped to what the emulator maps, when it can be
| asked. OFF, 0, or no TT-RAM: nothing is added. If TOS already has Fast
| RAM (EmuTOS finds it itself) nothing is added either.
| Position independent: no relocation table.

	.equ	BP_TLEN,0x0c
	.equ	BP_DLEN,0x14
	.equ	BP_BBASE,0x18
	.equ	BP_BLEN,0x1c
	.equ	BP_CMD,0x80

	.equ	V_NF,0		| NF TT-RAM size, -1 = no NatFeats
	.equ	V_SIZE,4
	.equ	V_SRC,8		| -> source name
	.equ	V_SSP,12
	.equ	V_OLDILL,16
	.equ	V_CLAMP,20
	.equ	JAR,32		| 64 slots
	.equ	JARSLOTS,64
	.equ	INFBUF,32+512
	.equ	NUMBUF,INFBUF+64
	.equ	FRB,NUMBUF+32	| 64 KB _FRB buffer, ST-RAM (the program is)
	.equ	BSSLEN,FRB+65536

	.equ	TTBASE,0x01000000

	.text
start:	move.l	4(sp),a5		| basepage
	move.l	BP_BBASE(a5),a6		| BSS

	clr.l	-(sp)			| Super(0)
	move.w	#0x20,-(sp)
	trap	#1
	addq.l	#6,sp
	move.l	d0,V_SSP(a6)

	lea	m_hello(pc),a0
	bsr	print

| ---- 3. ask the emulator (needed for the clamp too) ----
	bsr	nf_ttram
	move.l	d0,V_NF(a6)
	clr.l	V_CLAMP(a6)

| ---- 1. command line ----
	lea	BP_CMD(a5),a0
	moveq	#0,d1
	move.b	(a0)+,d1
	beq.s	try_inf
	bsr	parse
	tst.w	d2
	beq.s	try_inf
	lea	s_arg(pc),a1
	bra.s	got_size

| ---- 2. FASTRAM.INF ----
try_inf:
	lea	f_inf1(pc),a0
	bsr	read_inf
	tst.w	d2
	bne.s	inf_ok
	lea	f_inf2(pc),a0
	bsr	read_inf
	tst.w	d2
	beq.s	try_nf
inf_ok:	lea	s_inf(pc),a1
	bra.s	got_size

try_nf:	move.l	V_NF(a6),d0
	bmi	no_size			| no NatFeats, no INF, no argument
	lea	s_nf(pc),a1

got_size:
	move.l	a1,V_SRC(a6)
	move.l	d0,V_SIZE(a6)
	move.l	V_NF(a6),d1
	bmi.s	1f			| cannot ask: trust the user
	tst.l	d1
	beq	no_ttram		| emulator has none mapped
	cmp.l	d1,d0
	bls.s	1f
	move.l	d1,V_SIZE(a6)		| more than is mapped: clamp
	st	V_CLAMP(a6)
1:	move.l	V_SIZE(a6),d0
	and.l	#0xffff0000,d0		| whole 64 KB
	move.l	d0,V_SIZE(a6)
	beq	is_off

| ---- already has Fast RAM? ----
	move.w	#1,-(sp)		| Mxalloc(-1, 1): largest alt-RAM block
	move.l	#-1,-(sp)
	move.w	#0x44,-(sp)
	trap	#1
	addq.l	#8,sp
	tst.l	d0
	bgt	already

| ---- Maddalt ----
	move.l	V_SIZE(a6),-(sp)
	move.l	#TTBASE,-(sp)
	move.w	#0x14,-(sp)
	trap	#1
	lea	10(sp),sp
	tst.l	d0
	bne	madd_fail

	lea	m_added(pc),a0
	bsr	print
	move.l	V_SIZE(a6),d0
	moveq	#20,d1
	lsr.l	d1,d0			| MB
	bsr	print_dec
	lea	m_mb(pc),a0
	bsr	print
	move.l	V_SRC(a6),a0
	bsr	print
	tst.b	V_CLAMP(a6)
	beq.s	2f
	lea	m_clamp(pc),a0
	bsr	print
2:	lea	m_crlf(pc),a0
	bsr	print

| ---- _FRB cookie ----
	bsr	frb_install		| d0: 0 nothing new, 1 installed

	move.l	d0,-(sp)
	bsr	user_mode
	move.l	(sp)+,d0
	tst.l	d0
	beq	quit0
	clr.w	-(sp)			| Ptermres(keep, 0)
	move.l	#0x100,d0
	add.l	BP_TLEN(a5),d0
	add.l	BP_DLEN(a5),d0
	add.l	BP_BLEN(a5),d0
	move.l	d0,-(sp)
	move.w	#0x31,-(sp)
	trap	#1

no_size:
	lea	m_nosize(pc),a0
	bra.s	msg_quit
no_ttram:
	lea	m_nottram(pc),a0
	bra.s	msg_quit
is_off:	lea	m_off(pc),a0
	bra.s	msg_quit
already:
	lea	m_already(pc),a0
	bra.s	msg_quit
madd_fail:
	lea	m_fail(pc),a0
msg_quit:
	bsr	print
	bsr	user_mode
quit0:	clr.w	-(sp)			| Pterm0
	trap	#1

user_mode:
	move.l	(sp)+,a3		| return address: the stack changes. a3,
	move.l	V_SSP(a6),-(sp)		| not a0-a2/d0-d2: GEMDOS may change those
	move.w	#0x20,-(sp)		| (TOS 4.04 left a1 pointing into its own
	trap	#1			| stack - jmp (a1) ran off into it: 4 bombs)
	addq.l	#6,sp
	jmp	(a3)

| ---- NatFeats: d0 = TT-RAM bytes, -1 when there are no NatFeats ----
nf_ttram:
	move.l	0x10,V_OLDILL(a6)
	lea	nf_trap(pc),a0
	move.l	a0,0x10
	lea	nf_name(pc),a0
	move.l	a0,-(sp)
	bsr	nf_id
	addq.l	#4,sp
	tst.l	d0
	beq.s	nf_none
	moveq	#4,d1			| GETINT index 4: TT-RAM size
	move.l	d1,-(sp)
	addq.l	#1,d0			| PSCTRL sub-op 1 = GETINT
	move.l	d0,-(sp)
	bsr	nf_call
	addq.l	#8,sp
	cmp.l	#-1,d0			| unknown index
	bne.s	nf_done
nf_none:
	moveq	#-1,d0
nf_done:
	move.l	V_OLDILL(a6),0x10
	rts
nf_id:	.word	0x7300
	rts
nf_call:
	.word	0x7301
	rts
nf_trap:				| no NatFeats: illegal instruction
	moveq	#0,d0
	addq.l	#2,2(sp)
	rte

| ---- read and parse an INF file: a0 = name; d0 size, d2 valid ----
read_inf:
	clr.w	-(sp)			| Fopen(name, 0)
	move.l	a0,-(sp)
	move.w	#0x3d,-(sp)
	trap	#1
	addq.l	#8,sp
	moveq	#0,d2
	tst.l	d0
	bmi.s	3f
	move.w	d0,d3
	pea	INFBUF(a6)		| Fread(h, 63, buf)
	move.l	#63,-(sp)
	move.w	d3,-(sp)
	move.w	#0x3f,-(sp)
	trap	#1
	lea	12(sp),sp
	move.l	d0,d4
	move.w	d3,-(sp)		| Fclose
	move.w	#0x3e,-(sp)
	trap	#1
	addq.l	#4,sp
	moveq	#0,d2
	tst.l	d4
	ble.s	3f
	lea	INFBUF(a6),a0
	move.l	d4,d1
	bsr	parse
3:	rts

| ---- parse: a0 text, d1 length -> d0 bytes, d2 1 = valid ----
| "OFF" (any case) -> 0 valid. A number: plain = KB, K/M/G suffix.
parse:	moveq	#0,d2
	moveq	#0,d0
4:	subq.l	#1,d1			| skip blanks
	bmi.s	9f
	move.b	(a0)+,d3
	cmp.b	#' ',d3
	beq.s	4b
	cmp.b	#9,d3
	beq.s	4b
	move.b	d3,d4
	or.b	#0x20,d4
	cmp.b	#'o',d4
	bne.s	5f
	moveq	#1,d2			| OFF
	bra.s	9f
5:	cmp.b	#'0',d3
	bcs.s	9f
	cmp.b	#'9',d3
	bhi.s	9f
6:	sub.b	#'0',d3			| digits
	ext.w	d3
	ext.l	d3
	move.l	d0,d4
	lsl.l	#2,d0
	add.l	d4,d0
	add.l	d0,d0
	add.l	d3,d0
	moveq	#1,d2
	subq.l	#1,d1
	bmi.s	7f
	move.b	(a0)+,d3
	cmp.b	#'0',d3
	bcs.s	8f
	cmp.b	#'9',d3
	bls.s	6b
8:	or.b	#0x20,d3		| suffix
	cmp.b	#'k',d3
	beq.s	7f
	cmp.b	#'m',d3
	bne.s	10f
	moveq	#20,d4
	lsl.l	d4,d0
	bra.s	9f
10:	cmp.b	#'g',d3
	bne.s	7f
	moveq	#30,d4
	lsl.l	d4,d0
	bra.s	9f
7:	moveq	#10,d4			| KB
	lsl.l	d4,d0
9:	rts

| ---- _FRB: d0 = 1 if the cookie is new (keep us resident) ----
frb_install:
	move.l	0x5a0,a0		| _p_cookies
	move.l	a0,d0
	beq.s	nojar
	moveq	#0,d1			| entries
11:	move.l	(a0),d0
	beq.s	12f
	cmp.l	#0x5f465242,d0		| '_FRB' already there
	beq.s	frb_none
	addq.l	#8,a0
	addq.l	#1,d1
	bra.s	11b
12:	move.l	4(a0),d2		| slots
	move.l	d1,d0
	addq.l	#1,d0
	cmp.l	d2,d0
	bcc.s	newjar			| full: move to a bigger jar
	move.l	#0x5f465242,(a0)+	| '_FRB'
	lea	FRB(a6),a1
	move.l	a1,(a0)+
	clr.l	(a0)+
	move.l	d2,(a0)
	moveq	#1,d0
	rts
frb_none:
	moveq	#0,d0
	rts
nojar:	moveq	#0,d1
newjar:	move.l	0x5a0,a0		| copy d1 entries to our jar
	lea	JAR(a6),a1
	move.l	d1,d0
	bra.s	14f
13:	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
14:	subq.l	#1,d0
	bpl.s	13b
	move.l	#0x5f465242,(a1)+
	lea	FRB(a6),a0
	move.l	a0,(a1)+
	clr.l	(a1)+
	move.l	#JARSLOTS,(a1)
	lea	JAR(a6),a0
	move.l	a0,0x5a0
	moveq	#1,d0
	rts

| ---- output ----
print:	move.l	a0,-(sp)		| Cconws
	move.w	#9,-(sp)
	trap	#1
	addq.l	#6,sp
	rts
print_dec:				| d0 < 655360
	lea	NUMBUF+16(a6),a0
	clr.b	-(a0)
15:	divu	#10,d0
	swap	d0
	add.b	#'0',d0
	move.b	d0,-(a0)
	clr.w	d0
	swap	d0
	tst.l	d0
	bne.s	15b
	bra.s	print

m_hello:	.asciz	"FASTRAM: "
m_added:	.asciz	"Fast RAM at $01000000, "
m_mb:		.asciz	" MB, from "
m_clamp:	.asciz	" (clamped to the emulator's TT-RAM)"
m_crlf:		.asciz	"\r\n"
m_nosize:	.asciz	"no size (no argument, no FASTRAM.INF, no NatFeats)\r\n"
m_nottram:	.asciz	"the emulator has no TT-RAM (cfg ttram)\r\n"
m_off:		.asciz	"off\r\n"
m_already:	.asciz	"TOS already has Fast RAM - nothing added\r\n"
m_fail:		.asciz	"Maddalt failed\r\n"
s_arg:		.asciz	"the command line"
s_inf:		.asciz	"FASTRAM.INF"
s_nf:		.asciz	"the emulator"
f_inf1:		.asciz	"\\AUTO\\FASTRAM.INF"
f_inf2:		.asciz	"FASTRAM.INF"
nf_name:	.asciz	"PSCTRL"
	.even
end:
