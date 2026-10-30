/*
* UAE - The Un*x Amiga Emulator
*
* Event stuff
*
* Copyright 1995-2002 Bernd Schmidt
* Copyright 1995 Alessandro Bissacco
* Copyright 2000-2012 Toni Wilen
*/

#include <errno.h>
#include <time.h>
#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "events.h"

extern "C" int pistorm_gov_khz;   /* cfg cpu_speed (kHz), jit_glue.cpp */
#ifdef WITH_PPC
#include "uae/ppc.h"
#endif
#include "xwin.h"
#include "audio.h"
#include "custom.h"
#ifdef WITH_THREADED_CPU
#include "cpu_thread.h"
#endif

extern uae_u8 agnus_hpos;
int custom_fastmode;
extern int linear_hpos;
void custom_trigger_start_fast(void);

evt_t event_cycles, nextevent, currcycle;
uae_u32 currcycle_cck;
int is_syncline;
static int syncline_cnt;
frame_time_t is_syncline_end;
int cycles_to_next_event;
int max_cycles_to_next_event;
int cycles_to_hsync_event;
evt_t start_cycles;
bool event_wait;

frame_time_t vsyncmintime, vsyncmintimepre;
frame_time_t vsyncmaxtime, vsyncwaittime;
frame_time_t vsynctimebase, cputimebase;

static void events_fast(void)
{
	cycles_do_special();
}

void events_reset_syncline(void)
{
	is_syncline = 0;
	events_fast();
}

void events_schedule(void)
{
	evt_t mintime = EVT_MAX;
	for (int i = 0; i < ev_max; i++) {
		if (eventtab[i].active) {
			evt_t eventtime = eventtab[i].evtime - currcycle;
			if (eventtime < mintime)
				mintime = eventtime;
		}
	}
	if (mintime < EVT_MAX) {
		nextevent = currcycle + mintime;
	} else {
		nextevent = EVT_MAX;
	}
}

extern int vsync_activeheight;

static bool event_check_vsync(void)
{
	/* Keep only CPU emulation running while waiting for sync point. */
	if (is_syncline == -1) {

		if (!isvsync_chipset()) {
			events_reset_syncline();
			return false;
		}
		// wait for vblank
		audio_finish_pull();
		int done = vsync_isdone(NULL);
		if (done == -2) {
			// if no vsync thread
			int vp = target_get_display_scanline(-1);
			if (vp < is_syncline_end)
				done = 1;
			else if (vp > is_syncline_end)
				is_syncline_end = vp;
		}
		if (!done) {
#ifdef WITH_PPC
			if (ppc_state) {
				uae_ppc_execute_quick();
			}
#endif
			if (currprefs.cachesize)
				pissoff = pissoff_value;
			else
				pissoff = pissoff_nojit_value;
			return true;
		}
		vsync_clear();
		vsync_event_done();

	} else if (is_syncline == -2) {

		if (!isvsync_chipset()) {
			events_reset_syncline();
			return false;
		}
		// wait for vblank or early vblank
		audio_finish_pull();
		int done = vsync_isdone(NULL);
		if (done == -2)
			done = 0;
		int vp = target_get_display_scanline(-1);
		if (vp < 0 || vp >= is_syncline_end)
			done = 1;
		if (!done) {
#ifdef WITH_PPC
			if (ppc_state) {
				uae_ppc_execute_quick();
			}
#endif
			if (currprefs.cachesize)
				pissoff = pissoff_value;
			else
				pissoff = pissoff_nojit_value;
			return true;
		}
		vsync_clear();
		vsync_event_done();

	} else if (is_syncline == -3) {
		if (!isvsync_chipset()) {
			events_reset_syncline();
			return false;
		}
		// not vblank
		audio_finish_pull();
		int vp = target_get_display_scanline(-1);
		if (vp <= 0) {
#ifdef WITH_PPC
			if (ppc_state) {
				uae_ppc_execute_quick();
			}
#endif
			if (currprefs.cachesize)
				pissoff = pissoff_value;
			else
				pissoff = pissoff_nojit_value;
			return true;
		}
		vsync_clear();
		vsync_event_done();

	} else if (is_syncline > 0) {

		if (!isvsync_chipset()) {
			events_reset_syncline();
			return false;
		}
		audio_finish_pull();
		// wait for specific scanline
		int vp = target_get_display_scanline(-1);
		if (vp < 0 || is_syncline > vp) {
#ifdef WITH_PPC
			if (ppc_state) {
				uae_ppc_execute_check();
			}
#endif
			if (currprefs.cachesize)
				pissoff = pissoff_value;
			else
				pissoff = pissoff_nojit_value;
			return true;
		}
		vsync_event_done();

	}
	else if (is_syncline <= -100) {

		if (!isvsync_chipset()) {
			events_reset_syncline();
			return false;
		}
		audio_finish_pull();
		// wait for specific scanline
		int vp = target_get_display_scanline(-1);
		if (vp < 0 || vp >= (-(is_syncline + 100))) {
#ifdef WITH_PPC
			if (ppc_state) {
				uae_ppc_execute_check();
			}
#endif
			if (currprefs.cachesize)
				pissoff = pissoff_value;
			else
				pissoff = pissoff_nojit_value;
			return true;
		}
		vsync_event_done();

	} else if (is_syncline == -10) {

		// wait is_syncline_end
		if (event_wait) {
			frame_time_t rpt = read_processor_time();
			frame_time_t v = rpt - is_syncline_end;
			if (v < 0) {
#ifdef WITH_PPC
				if (ppc_state) {
					uae_ppc_execute_check();
				}
#endif
				if (currprefs.cachesize)
					pissoff = pissoff_value;
				else
					pissoff = pissoff_nojit_value;
				return true;
			}
		}
		events_reset_syncline();

	} else if (is_syncline < -10) {

		// wait is_syncline_end/vsyncmintime
		if (event_wait) {
			frame_time_t rpt = read_processor_time();
			frame_time_t v = rpt - vsyncmintime;
			frame_time_t v2 = rpt - is_syncline_end;
			if (v > vsynctimebase || v < -vsynctimebase) {
				v = 0;
			}
			if (v < 0 && v2 < 0) {
#ifdef WITH_PPC
				if (ppc_state) {
					if (is_syncline == -11) {
						uae_ppc_execute_check();
					} else {
						uae_ppc_execute_quick();
					}
				}
#endif
				if (currprefs.cachesize)
					pissoff = pissoff_value;
				else
					pissoff = pissoff_nojit_value;

				return true;
			}
		}
		events_reset_syncline();
	}
	return false;
}

#ifdef AMIGA_
void do_cycles_normal(int cycles_to_add)
{
	while ((nextevent - currcycle) <= cycles_to_add) {

		cycles_to_add -= (int)(nextevent - currcycle);
		currcycle = nextevent;

		for (int i = 0; i < ev_max; i++) {
			if (eventtab[i].active && eventtab[i].evtime == currcycle) {
				if (eventtab[i].handler == NULL) {
					gui_message(_T("eventtab[%d].handler is null!\n"), i);
					eventtab[i].active = 0;
				} else {
					(*eventtab[i].handler)();
				}
			}
		}
		events_schedule();

	}
	currcycle += cycles_to_add;
}
#else
void do_cycles_normal(int cycles_to_add) { currcycle += cycles_to_add; } 
#endif

static int cycles_to_add_remain;

//#ifdef AMIGA_
#ifndef AMIGA_
/* ===========================================================================
 * PiStorm Atari: the 68000 speed governor (cfg `cpu_speed`)
 * ===========================================================================
 *
 * WHAT IT DOES
 *   Paces the interpreter so the guest gets exactly `cpu_speed` worth of
 *   68000 cycles per second of real time (8 MHz = a real ST/STE, 16 MHz =
 *   a Mega STE). The Amiga vsync machinery that once converted UAE cycles
 *   into real time went with the chipset layer, so without this the CPU
 *   runs at whatever speed the Pi manages (~20 MHz interpreted on a Pi 4).
 *
 * WHEN IT RUNS
 *   Only with m68k_speed == 0 and the JIT off (currprefs.cachesize == 0).
 *   cfg `cpu_speed 8mhz|16mhz` arranges that (jit_glue.cpp: m68k_speed 0,
 *   no cpu_clock_multiplier, pistorm_gov_khz = target). JIT configs and the
 *   default max-speed configs never call into the pacing below.
 *
 * HOW IT KEEPS TIME
 *   Guest time   = cycles since the anchor / 8 MHz-units -> ns at gov_khz.
 *   Real time    = CLOCK_MONOTONIC since the anchor (gov_ns0).
 *   ahead_ns     = guest time - real time.
 *   Every gov_step guest cycles (GOV_STEP, 32 = 4 us at 8 MHz):
 *     ahead  > 0 : wait until real time catches up (gov_wait_until).
 *     behind     : run on to repay the debt, but no faster than
 *                  GOV_CATCHUP_X times the target (see CATCH-UP below).
 *     behind more than GOV_FORGIVE_NS: a real host stall - write off the
 *                  excess so it is not repaid at all.
 *
 * WAITING (gov_wait_until)
 *   This thread is SCHED_FIFO 99 pinned to core 2 (emulator.c cpu_task). A
 *   pure spin never gives the core back, so waits sleep with
 *   clock_nanosleep(TIMER_ABSTIME) to GOV_SPIN_NS before the deadline and
 *   spin only the rest, which absorbs the wake-up latency. A single wait is
 *   capped at 1 ms; with checks every 4 us the CPU is never more than
 *   4 us ahead of real time. A wait does not look at the interrupt lines:
 *   an MFP / ACIA / VBL interrupt that arrives during one is taken when it
 *   ends, so the step length IS the extra interrupt latency (see HISTORY,
 *   30 Oct 2026).
 *
 * CATCH-UP (repaying debt) - why it is capped
 *   Bus-heavy code (YM writes, ST-RAM writes for a native display) costs
 *   1-2 us of real time per access against 125-250 ns on a real ST, so the
 *   guest falls behind during such stretches and makes it up in lighter
 *   ones. Repaying flat out (~20 MHz) is a burst during which code that
 *   times itself by counting instructions - delay loops between IKBD
 *   commands, FDC/ACSI handshakes, raster tricks - runs too fast in real
 *   time. GOV_CATCHUP_X bounds that: while behind, a step may take no less
 *   real time than it would at GOV_CATCHUP_X x target. Default 2: a delay
 *   loop is at worst 2x short, and only while debt remains.
 *
 * HISTORY / WHY THE NUMBERS
 *   - Before 26 Oct 2026: checks every 1000 cycles, usleep() once more
 *     than 150 us ahead, debt over 2 ms forgiven. With a YM MOD player on
 *     a native ST at 8 MHz the playback was audibly slow, but correct
 *     unthrottled. Modelled: bus-heavy stretches longer than ~5 ms ran up
 *     more than 2 ms of debt, which was written off, so the average fell to
 *     6.2-6.5 MHz while the Pi could average 8-10 MHz.
 *   - 26 Oct 2026: GOV_STEP 250, exact waits, GOV_FORGIVE_NS 50 ms -> the
 *     MOD plays correctly, [GOV] reports 7999-8000 kHz once booted (the
 *     first windows after boot read lower: image loading is a real stall).
 *     Paula 2.6 on an STE at 50 kHz still pins 100% at 8 MHz - matching
 *     contemporary reviews of the real machine - and is clean at 16 MHz.
 *   - 26 Oct 2026: GOV_CATCHUP_X added, before merge, to bound the
 *     catch-up bursts the 50 ms window allows (see CATCH-UP).
 *   - A first version waited by spinning only; HOSTDOS (hostfs on plain
 *     TOS) seemed to stop starting. That turned out to be the build not
 *     containing HOSTDOS (wrong branch), but the sleep-then-spin wait was
 *     kept: a FIFO 99 thread should not monopolise its core.
 *   - 30 Oct 2026: GOV_STEP 250 -> 32. Turrican II (PISTORM_IPL_TIMELINE):
 *     from a fresh level-6 edge to the CPU taking it, only 25% of 270k
 *     interrupts took the 2-4 us the delivery path needs; the rest spread
 *     evenly out to a hard edge at 34 us - the 31 us step plus that path,
 *     i.e. the interrupt sat out the governor's wait. The game's lower-
 *     border Timer B handler must reach its $FF8209 poll inside the last
 *     12 us of line 199; in 138 of 978 frames it arrived late, spun until
 *     the next frame's display (VBL held off ~4 ms) and the screen jumped.
 *     At 32 cycles a check costs one clock_gettime (vDSO) per ~4 us of
 *     guest time, ~1-2% of the CPU thread. PISTORM_GOV_STEP overrides.
 *
 * TUNING / DIAGNOSIS (environment)
 *   PISTORM_GOV_KHZ=n       target in kHz, overriding cfg cpu_speed.
 *   PISTORM_GOV_REPORT=1    print the achieved kHz every 5 s. Without it,
 *                           only the first 5 s window under 90% is printed.
 *   PISTORM_GOV_STEP=n      guest cycles between checks (default 32,
 *                           8..2000; 250 = the pre-30 Oct 2026 behaviour).
 *   PISTORM_GOV_CATCHUP=x   catch-up cap as a multiple of the target
 *                           (default 2, 0 = unlimited, i.e. flat out).
 *   A report steady near the target with audio/timing still wrong means
 *   the problem is not CPU speed (look at interrupt delivery instead).
 * ======================================================================== */
enum { GOV_STEP = 32 };                          /* default guest 8 MHz cycles per check */
static evt_t gov_step = GOV_STEP;                /* PISTORM_GOV_STEP may override */
static const int64_t GOV_FORGIVE_NS = 50000000LL; /* debt beyond this = host stall */
static const int64_t GOV_SPIN_NS    = 50000LL;    /* spin this much before a deadline */
static const int64_t GOV_WAIT_MAX   = 1000000LL;  /* cap on any single wait */

static inline int64_t gov_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Wait until CLOCK_MONOTONIC reaches `until` (ns): sleep most of it, spin
 * the last GOV_SPIN_NS. Returns the time on leaving. */
static int64_t gov_wait_until(int64_t now, int64_t until)
{
	if (until - now > GOV_SPIN_NS) {
		struct timespec dl;
		int64_t wake = until - GOV_SPIN_NS;
		dl.tv_sec = (time_t)(wake / 1000000000LL);
		dl.tv_nsec = (long)(wake % 1000000000LL);
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &dl, NULL) == EINTR)
			;
	}
	do {
		now = gov_now_ns();
	} while (now < until);
	return now;
}

/* sync != 0: see pistorm_gov_sync() below */
static void pistorm_governor(int sync)
{
	if (currprefs.m68k_speed != 0 || currprefs.cachesize)
		return;                                 /* JIT / max speed: not ours */

	static int     gov_on;
	static int     gov_report;
	static int64_t gov_khz;                     /* target, kHz               */
	static int64_t gov_catchup_x;               /* catch-up cap, x target    */
	static evt_t   gov_cyc0;                    /* anchor: guest cycles ...  */
	static int64_t gov_ns0;                     /* ... and real time         */
	static evt_t   gov_next;                    /* next check, guest cycles  */
	static evt_t   step_cyc;                    /* last check: guest cycles  */
	static int64_t step_ns;                     /* ... and real time         */
	const evt_t    unit = CYCLE_UNIT / 2;       /* currcycle units per 8 MHz cycle */

	/* first call, or currcycle went backwards (reset): re-anchor */
	if (!gov_on || currcycle < gov_cyc0) {
		gov_ns0 = step_ns = gov_now_ns();
		gov_cyc0 = step_cyc = currcycle;
		if (!gov_on) {
			const char *stp = getenv("PISTORM_GOV_STEP");
			if (stp) {
				long long v = atoll(stp);
				if (v >= 8 && v <= 2000)
					gov_step = (evt_t)v;
			}
			const char *env = getenv("PISTORM_GOV_KHZ");
			gov_khz = env ? atoll(env) : pistorm_gov_khz ? pistorm_gov_khz : 8000;
			if (gov_khz < 1000 || gov_khz > 64000)
				gov_khz = 8000;
			const char *rep = getenv("PISTORM_GOV_REPORT");
			gov_report = rep && *rep == '1';
			const char *cu = getenv("PISTORM_GOV_CATCHUP");
			gov_catchup_x = cu ? atoll(cu) : 2;
			if (gov_catchup_x < 0 || gov_catchup_x > 100)
				gov_catchup_x = 2;
			fprintf(stderr, "[GOV] 68k governor active: %lld kHz, catch-up cap %s%lldx, "
			        "check every %lld cycles\n",
			        (long long)gov_khz, gov_catchup_x ? "" : "off ",
			        (long long)gov_catchup_x, (long long)gov_step);
		}
		gov_next = currcycle + gov_step * unit;
		gov_on = 1;
	}
	if (!sync && currcycle < gov_next)
		return;

	int64_t now = gov_now_ns();

	/* Achieved speed over 5 s windows of real time. Printed once if a
	 * window falls below 90% of the target, or every window with
	 * PISTORM_GOV_REPORT=1. A Pi 4's interpreter tops out near 20 MHz. */
	{
		static int64_t win_ns0;
		static evt_t   win_cyc0;
		static int     said;
		if (!win_ns0 || currcycle < win_cyc0) {
			win_ns0 = now;
			win_cyc0 = currcycle;
		} else if (now - win_ns0 >= 5000000000LL) {
			int64_t khz = (int64_t)((currcycle - win_cyc0) / unit) * 1000000LL
			              / (now - win_ns0);
			if (gov_report || (!said && khz * 10 < gov_khz * 9)) {
				said = 1;
				fprintf(stderr, "[GOV] reaching %lld kHz of the %lld kHz target "
				        "(5 s average)\n", (long long)khz, (long long)gov_khz);
			}
			win_ns0 = now;
			win_cyc0 = currcycle;
		}
	}

	int64_t virt_ns  = (int64_t)((currcycle - gov_cyc0) / unit) * 1000000LL / gov_khz;
	int64_t ahead_ns = virt_ns - (now - gov_ns0);

	if (sync) {
		/* a beam-timed register access: the guest and the real beam must
		 * agree NOW. Ahead: wait out all of it (still capped). Behind:
		 * write the debt off here instead of repaying it at up to
		 * GOV_CATCHUP_X x speed - repayment compresses exactly the delay
		 * loops a border routine times its next write with. */
		if (ahead_ns > 0)
			now = gov_wait_until(now, now + (ahead_ns < GOV_WAIT_MAX ? ahead_ns : GOV_WAIT_MAX));
		else
			gov_ns0 = now - virt_ns;
	} else if (ahead_ns > 0) {
		/* ahead of real time: wait for it (at most GOV_WAIT_MAX per check) */
		now = gov_wait_until(now, now + (ahead_ns < GOV_WAIT_MAX ? ahead_ns : GOV_WAIT_MAX));
	} else {
		if (ahead_ns < -GOV_FORGIVE_NS) {
			/* a host stall, not bus time: write off all but GOV_FORGIVE_NS */
			gov_ns0 -= ahead_ns + GOV_FORGIVE_NS;
		}
		/* repaying debt: this step may not have run faster than
		 * gov_catchup_x times the target (see CATCH-UP above) */
		if (gov_catchup_x) {
			int64_t min_ns = (int64_t)((currcycle - step_cyc) / unit) * 1000000LL
			                 / (gov_khz * gov_catchup_x);
			if (now - step_ns < min_ns)
				now = gov_wait_until(now, step_ns + min_ns);
		}
	}
	step_ns  = now;
	step_cyc = currcycle;
	gov_next = currcycle + gov_step * unit;
}

/* BEAM-TIMED ACCESSES. The governor checked every 250 guest cycles (~31
 * us at 8 MHz; now 32, see HISTORY), and between checks the interpreter runs at the
 * Pi's speed (~20 MHz); while behind it repays at up to GOV_CATCHUP_X x.
 * Right for averages, wrong for code that times one hardware access off
 * another by counting instructions. Turrican II's lower-border routine
 * ($0114FE, 30 Oct 2026): poll the video counter low byte ($FF8209) until
 * it reaches $D0, align with lsl.l, write 0 to $FF820A (60 Hz), dbra x9
 * (~11 us on an ST), write 2 (50 Hz). The PISTORM_IPL_TIMELINE capture had
 * that 60 -> 50 Hz gap at 4 us in 450 of 500 frames and 21-25 us in most
 * of the rest - the border opened in some frames and not others. Called
 * (pistorm_natmem.cpp) before the guest's accesses to the video counter
 * $FF8205/07/09, the sync register $FF820A and the resolution register
 * $FF8260 - rare outside such routines - so each lands at its guest time
 * on the real clock. JIT / max speed: not ours, nothing happens. */
extern "C" void pistorm_gov_sync(void)
{
	pistorm_governor(1);
}
#endif

void do_cycles_slow(int cycles_to_add)
{
#ifdef WITH_THREADED_CPU
	if (currprefs.cpu_thread && !is_mainthread()) {
		/* Track stalls when CPU thread can't access chipset */
		if (cpu_thread_diag.enable_diagnostics) {
			cpu_thread_diag_log_stall(cycles_to_add);
		}
		return;
	}

	/* Phase 2: Periodic sync point - flush register batch every 1000 cycles (proven optimal - 10.36% improvement) */
	static evt_t last_sync_cycle = 0;
	evt_t current_cycle = get_cycles();
	if (current_cycle - last_sync_cycle >= 1000) {
		cpu_thread_flush_register_batch();
		cpu_thread_sync_point(1);
		last_sync_cycle = current_cycle;
	}
#endif

#ifdef WITH_X86
#if 0
	if (x86_turbo_on) {
		execute_other_cpu_single();
	}
#endif
#endif
#ifdef AMIGA_
	if (is_syncline) {
		if (syncline_cnt > 0) {
			syncline_cnt--;
			return;
		}
		// runs CPU emulation with chipset stopped
		// when there is free time to do so.
		if (event_check_vsync()) {
			syncline_cnt = currprefs.cachesize ? 2 : 32;
			return;
		}
	}
#endif
	cycles_to_add += cycles_to_add_remain;
	cycles_to_add_remain = 0;

	if (!currprefs.cpu_thread) {
		if ((pissoff -= cycles_to_add) >= 0) {
			return;
		}
		if (currprefs.cachesize && pissoff_value > 0) {
			cycles_to_add = pissoff_value - pissoff;
			pissoff = pissoff_value;
		} else {
			cycles_to_add = -pissoff;
			pissoff = 0;
		}
	}
	currcycle += cycles_to_add;     /* PiStorm: advance the clock, no event dispatch */

#ifndef AMIGA_
	pistorm_governor(0);            /* cpu_speed pacing - see the design note above pistorm_governor() */
#endif
#ifdef AMIGA_
	while (cycles_to_add >= CYCLE_UNIT) {

		if (!eventtab[ev_sync].active) {
			int cyc = cycles_to_add;
			cyc = do_cycles_cck(cyc);
			cycles_to_add -= cyc;
		} else {
			do_cycles_normal(cycles_to_add);
			cycles_to_add = 0;
		}

	}

	int remain = cycles_to_add;
	cycles_to_add_remain += remain;
#endif
}
//#else
//void do_cycles_slow(int cycles_to_add)   { currcycle += cycles_to_add; }
//#endif

static int event2idx;
static int event2cnt;
static int event2restart;

void MISC_handler(void)
{
	evt_t mintime;
	evt_t ct = get_cycles();

	if (event2cnt) {
		event2restart++;
	}
	eventtab[ev_misc].active = 0;
	mintime = EVT_MAX;
	int idx2 = event2idx;
	for (int i = 0; i < ev2_max; i++) {
		int idx = (idx2 + i) & (ev2_max - 1);
		ev2 *e = &eventtab2[idx];
		if (e->active) {
			if (e->evtime == ct) {
				e->active = false;
				event2cnt++;
				e->handler(e->data);
				event2cnt--;
				if (event2restart > 0) {
					event2restart--;
					mintime = EVT_MAX;
					i = 0;
				}
			} else {
				evt_t eventtime = e->evtime - ct;
				if (eventtime < mintime) {
					mintime = eventtime;
				}
			}
		}
	}
	if (mintime < EVT_MAX) {
		ev *e = &eventtab[ev_misc];
		e->active = true;
		e->oldcycles = ct;
		e->evtime = ct + mintime;
		events_schedule();
	}
}

void event2_newevent_xx_ce(evt_t t, uae_u32 data, evfunc2 func)
{
	if (!currprefs.cpu_memory_cycle_exact) {
		func(data);
		return;
	}
	event2_newevent_xx(-1, t, data, func);
}

void event2_newevent_xx(int no, evt_t t, uae_u32 data, evfunc2 func)
{
	evt_t et;
	static int next = ev2_misc;

	et = t + get_cycles();
	if (no < 0) {
		no = next;
		for (;;) {
			if (!eventtab2[no].active) {
				break;
			}
			if (eventtab2[no].evtime == et && eventtab2[no].handler == func && eventtab2[no].data == data)
				break;
			no++;
			if (no == ev2_max) {
				no = ev2_misc;
			}
			if (no == next) {
				// we may have multiple interrupts queued, merge if possible
				for (int i = 0; i < ev2_max; i++) {
					auto ev2 = &eventtab2[i];
					if (ev2->active && ev2->handler == func && ev2->evtime == et && func == event_doint_delay_do_ext) {
						ev2->data |= data;
						return;
					}
				}
				write_log(_T("out of event2's!\n"));
				// execute most recent event immediately
				evt_t mintime = EVT_MAX;
				int minevent = -1;
				evt_t ct = get_cycles();
				for (int i = 0; i < ev2_max; i++) {
					if (eventtab2[i].active) {
						evt_t eventtime = eventtab2[i].evtime - ct;
						if (eventtime < mintime) {
							mintime = eventtime;
							minevent = i;
						}
					}
				}
				if (minevent >= 0) {
					eventtab2[minevent].active = false;
					eventtab2[minevent].handler(eventtab2[minevent].data);
				}
				continue;
			}
		}
		next = no;
	}
	ev2 *e = &eventtab2[no];
	e->active = true;
	e->evtime = et;
	e->handler = func;
	e->data = data;
	event2idx = addrdiff(e, eventtab2) + 1;
	MISC_handler();
}

void event2_newevent_x_replace_exists(evt_t t, uae_u32 data, evfunc2 func)
{
	for (int i = 0; i < ev2_max; i++) {
		if (eventtab2[i].active && eventtab2[i].handler == func) {
			eventtab2[i].active = false;
			if (t <= 0) {
				func(data);
				return;
			}
			event2_newevent_xx(-1, t * CYCLE_UNIT, data, func);
			return;
		}
	}
}

void event2_newevent_x_remove(evfunc2 func)
{
	for (int i = 0; i < ev2_max; i++) {
		if (eventtab2[i].active && eventtab2[i].handler == func) {
			eventtab2[i].active = false;
		}
	}
}

bool event2_newevent_x_exists(evfunc2 func)
{
	for (int i = 0; i < ev2_max; i++) {
		if (eventtab2[i].active && eventtab2[i].handler == func) {
			return true;
		}
	}
	return false;
}

void event2_newevent_x_replace(evt_t t, uae_u32 data, evfunc2 func)
{
	event2_newevent_x_remove(func);
	if (t <= 0) {
		func(data);
		return;
	}
	event2_newevent_xx(-1, t * CYCLE_UNIT, data, func);
}

void event2_newevent_x_add_not_exists(evt_t t, uae_u32 data, evfunc2 func)
{
	if (event2_newevent_x_exists(func)) {
		return;
	}
	if (t <= 0) {
		func(data);
		return;
	}
	event2_newevent_xx(-1, t * CYCLE_UNIT, data, func);
}

void event_init(void)
{
}

void clear_events(void)
{
	nextevent = EVT_MAX;
	for (int i = 0; i < ev_max; i++) {
		eventtab[i].active = 0;
		eventtab[i].oldcycles = get_cycles();
	}
	for (int i = 0; i < ev2_max; i++) {
		eventtab2[i].active = 0;
	}
}
