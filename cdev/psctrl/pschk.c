/* SPDX-License-Identifier: MIT
 *
 * PSCHK.TTP - dump one PSCTRL snapshot to the console.
 *
 * Phase-1 test tool for the PSCTRL NatFeat (see PSCTRL-DESIGN.md and
 * platforms/atari/psctrl/psctrl.h in the emulator tree). Runs under plain
 * TOS or FreeMiNT; needs nothing installed. On an emulator without PSCTRL
 * (or on real hardware, where the probe is safely trapped) it reports the
 * feature as absent and exits.
 *
 * Build (m68k-atari-mint-gcc):
 *   make -C cdev/psctrl
 *
 * The NatFeat calling stubs follow the pattern of FreeMiNT's nf_ops
 * (as also used by TeraDesk, library/utility/nf_ops.c): cdecl arguments
 * on the stack, opcode 0x7300 = NF_GET_ID, 0x7301 = NF_CALL.
 */

#include <stdio.h>
#include <osbind.h>

#pragma GCC optimize "-fomit-frame-pointer"

#define ASM_NATFEAT(opcode) "\t.word " opcode "\n"

static long __attribute__((noinline)) _nf_get_id(const char *feature_name)
{
	register long ret __asm__ ("d0");

	(void) feature_name;
	__asm__ volatile (
		ASM_NATFEAT("0x7300")
	: "=g"(ret)
	:
	: "d1", "cc", "memory"
	);
	return ret;
}

static long __attribute__((noinline)) _nf_call(long id, ...)
{
	register long ret __asm__ ("d0");

	(void) id;
	__asm__ volatile (
		ASM_NATFEAT("0x7301")
	: "=g"(ret)
	:
	: "d1", "cc", "memory"
	);
	return ret;
}

/* Probe for NatFeats with the illegal-instruction vector hooked, so this
 * binary is harmless on a real 68k. Must run in supervisor mode: call
 * through Supexec(). */
static long _nf_detect(void)
{
	register long ret __asm__ ("d0");
	register const char *nf_vers __asm__ ("a1") = "NF_VERSION";

	__asm__ volatile (
	"\tmove.l	%1,-(%%sp)\n"
	"\tmoveq	#0,%%d0\n"
	"\tmove.l	%%d0,-(%%sp)\n"
	"\tlea		(1f:w,%%pc),%%a1\n"
	"\tmove.l	(0x0010).w,%%a0\n"
	"\tmove.l	%%a1,(0x0010).w\n"
	"\tmove.l	%%sp,%%a1\n"
	"\tnop\n"
	ASM_NATFEAT("0x7300")
	"\ttst.l	%%d0\n"
	"\tbeq.s	1f\n"
	"\tmoveq	#1,%%d0\n"
	"\tmove.l	%%d0,(%%sp)\n"
"1:\n"
	"\tmove.l	%%a1,%%sp\n"
	"\tmove.l	%%a0,(0x0010).w\n"
	"\tmove.l	(%%sp)+,%%d0\n"
	"\taddq.l	#4,%%sp\n"
	"\tnop\n"
	: "=g"(ret)
	: "g"(nf_vers)
	: "a0", "d1", "cc", "memory"
	);
	return ret;
}

/* PSCTRL sub-ops and PS_GETINT indices - keep in sync with psctrl.h */
#define PSCTRL_VERSION       0L
#define PSCTRL_GETINT        1L

#define PS_CFG_JIT_ENABLED   0L
#define PS_CFG_CACHE_SIZE_KB 1L
#define PS_CFG_CPU_MODEL     2L
#define PS_CFG_FPU_MODEL     3L
#define PS_CFG_TTRAM_SIZE    4L

#define PS_STAT_EPOCH        32L
#define PS_STAT_CACHE_USED   39L
#define PS_STAT_CACHE_TOTAL  40L
#define PS_STAT_COMPILES     41L
#define PS_STAT_FLUSHES      42L
#define PS_STAT_INTERP_CALLS 44L
#define PS_STAT_STOP_ITERS   46L

#define PS_HOST_SOC_TEMP_MC  64L
#define PS_HOST_ARM_FREQ_KHZ 65L
#define PS_HOST_LOADAVG_X100 66L
#define PS_HOST_UPTIME_S     67L

static long psctrl_id;

static long ps_getint(long index)
{
	return _nf_call(psctrl_id | PSCTRL_GETINT, index);
}

int main(void)
{
	long temp_mc;
	long used, total;

	if (Supexec(_nf_detect) == 0)
	{
		printf("NatFeats not present (real hardware or old emulator).\n");
		return 1;
	}

	psctrl_id = _nf_get_id("PSCTRL");

	if (psctrl_id == 0)
	{
		printf("PSCTRL feature not present (emulator too old).\n");
		return 1;
	}

	printf("PSCTRL API version %ld\n\n", _nf_call(psctrl_id | PSCTRL_VERSION));

	printf("JIT enabled     : %ld\n", ps_getint(PS_CFG_JIT_ENABLED));
	printf("JIT cache (cfg) : %ld KB\n", ps_getint(PS_CFG_CACHE_SIZE_KB));
	printf("CPU / FPU model : %ld / %ld\n",
		   ps_getint(PS_CFG_CPU_MODEL), ps_getint(PS_CFG_FPU_MODEL));
	printf("TT-RAM (cfg)    : %ld KB\n\n", ps_getint(PS_CFG_TTRAM_SIZE) / 1024L);

	printf("epoch           : %ld\n", ps_getint(PS_STAT_EPOCH));
	used = ps_getint(PS_STAT_CACHE_USED);
	total = ps_getint(PS_STAT_CACHE_TOTAL);
	printf("JIT cache used  : %ldK / %ldK\n", used / 1024L, total / 1024L);
	printf("compiles/window : %ld\n", ps_getint(PS_STAT_COMPILES));
	printf("flushes/window  : %ld\n", ps_getint(PS_STAT_FLUSHES));
	printf("interp/window   : %ld\n", ps_getint(PS_STAT_INTERP_CALLS));
	printf("stop iters/win  : %ld\n\n", ps_getint(PS_STAT_STOP_ITERS));

	temp_mc = ps_getint(PS_HOST_SOC_TEMP_MC);
	printf("Pi SoC temp     : %ld.%ld C\n", temp_mc / 1000L, (temp_mc % 1000L) / 100L);
	printf("Pi ARM clock    : %ld MHz\n", ps_getint(PS_HOST_ARM_FREQ_KHZ) / 1000L);
	printf("Pi load avg     : %ld.%02ld\n",
		   ps_getint(PS_HOST_LOADAVG_X100) / 100L,
		   ps_getint(PS_HOST_LOADAVG_X100) % 100L);
	printf("Pi uptime       : %ld s\n", ps_getint(PS_HOST_UPTIME_S));

	return 0;
}
