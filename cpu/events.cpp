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
 *   Guest time   = cycles since the anchor / 8 MHz-units -> ns at gov_hz
 *                  (an 8 MHz ST: the board's measured 68000 clock).
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
 *   PISTORM_GOV_BEAM=0      no beam-timed Timer B handlers (see BEAM-TIMED
 *                           HANDLERS); PISTORM_GOV_BEAM_CYC=n moves their
 *                           start (cycles after the display end, default 40).
 *   A report steady near the target with audio/timing still wrong means
 *   the problem is not CPU speed (look at interrupt delivery instead).
 * ======================================================================== */
enum { GOV_STEP = 32 };                          /* default guest 8 MHz cycles per check */
static evt_t gov_step = GOV_STEP;                /* PISTORM_GOV_STEP may override */
static const int64_t GOV_FORGIVE_NS = 50000000LL; /* debt beyond this = host stall */
static const int64_t GOV_SPIN_NS    = 50000LL;    /* spin this much before a deadline */
static const int64_t GOV_WAIT_MAX   = 1000000LL;  /* cap on any single wait */

/* guest time: currcycle units -> ns at hz. 128-bit: cycles since the
 * anchor x 1e9 passes int64 after ~19 minutes at 8 MHz */
static inline int64_t gov_cyc_ns(evt_t cyc, evt_t unit, int64_t hz)
{
	return (int64_t)((__int128)(cyc / unit) * 1000000000 / hz);
}

static inline int64_t gov_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* newcpu.cpp: an interrupt is waiting that the CPU would take now; an
 * interrupt handler is running (no pauses inside one - see newcpu.cpp) */
extern int pistorm_irq_deliverable(void);
extern "C" uint32_t pistorm_cur_pc(void);   /* newcpu.cpp, for raster trace H */
extern int pistorm_handler_running(void);
#include "platforms/atari/raster_trace.h"   /* PISTORM_RASTER_TRACE */
#include "platforms/atari/stram_wb.h"       /* PISTORM_WRBEHIND */
#include "platforms/atari/video/shifter_mirror.h"   /* exact HDMI mirror */
#include "platforms/atari/video/beam_clock.h"       /* BEAM-TIMED HANDLERS */

/* Wait until CLOCK_MONOTONIC reaches `until` (ns): sleep most of it, spin
 * the last GOV_SPIN_NS. Returns the time on leaving.
 *
 * An interrupt ends the wait at once. The guest is AHEAD of real time
 * whenever this waits, and a real 68000 is never ahead: it takes an MFP
 * interrupt within a few microseconds of the chip raising it. Waiting out
 * the step first delayed it by up to a step (~31 us) - measured on
 * Shadow of the Beast II's sky (31 Oct 2026, raster trace): the Timer B
 * handler that must write the next frame's count within one 64 us line
 * was taken ~29 us late in 2-4% of frames, missed the line, and the whole
 * sky chain started 19 lines early the next frame. Taking the interrupt
 * and paying the time back at the next step keeps the average speed and
 * gives the handler its line back. The sleep is done in slices so a long
 * wait notices too. */
extern "C" volatile uint8_t pistorm_cpu_phase;   /* emulator.c STALL PROBE */
static int64_t gov_wait_until_(int64_t now, int64_t until);
static int64_t gov_wait_until(int64_t now, int64_t until)
{
	const uint8_t ph = pistorm_cpu_phase;
	pistorm_cpu_phase = 1;
	now = gov_wait_until_(now, until);
	pistorm_cpu_phase = ph;
	return now;
}
/* SPIN, DO NOT SLEEP (PISTORM_GOV_SLEEP=1: the old sleeping wait).
 * The interrupt test above only ran between sleeps, and a sleep was up
 * to 100 us (GOV_SLICE_NS) of a wait's first part, everything but its
 * last GOV_SPIN_NS - plus the kernel's wake-up latency. An interrupt
 * raised during one was not seen until it ended.
 * Turrican II title, 12 Nov 2026 (t2e, stall probe): every Timer B
 * interrupt taken more than 30 us after ipl_task latched it had the CPU
 * thread inside this wait, outside any handler, mask 3, g_irq 6, the
 * guest's instruction count not moving - for 30-97 us. The guest is
 * ahead after the Timer C (music) handler; the governor then sleeps the
 * lead off, and a Timer B raised in that sleep was taken one line late:
 * the lower border failed and the whole picture jumped for two frames
 * (17 in 73 s in t2d). The CPU thread has its own core; spinning costs
 * nothing the emulator needs, and the test is one load of g_irq. */
static int gov_sleep_on = -1;
static int64_t gov_wait_until_(int64_t now, int64_t until)
{
	static const int64_t GOV_SLICE_NS = 100000LL;  /* longest sleep between checks */

	if (gov_sleep_on < 0) {
		const char *e = getenv("PISTORM_GOV_SLEEP");
		gov_sleep_on = (e && *e == '1');
	}
	if (!gov_sleep_on) {
		while (now < until) {
			if (pistorm_irq_deliverable())
				break;
			asm volatile ("yield" ::: "memory");
			now = gov_now_ns();
		}
		return now;
	}
	while (until - now > GOV_SPIN_NS) {
		struct timespec dl;
		int64_t wake = until - GOV_SPIN_NS;

		if (pistorm_irq_deliverable())
			return now;
		if (wake - now > GOV_SLICE_NS)
			wake = now + GOV_SLICE_NS;
		dl.tv_sec = (time_t)(wake / 1000000000LL);
		dl.tv_nsec = (long)(wake % 1000000000LL);
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &dl, NULL) == EINTR)
			;
		now = gov_now_ns();
	}
	while (now < until) {
		if (pistorm_irq_deliverable())
			break;
		now = gov_now_ns();
	}
	return now;
}

/* THE LEAD AT AN INTERRUPT (PISTORM_GOV_PARK=0 turns this off).
 * An interrupt ends a governor wait at once, and no pause is taken inside
 * a handler - so a handler can start with the guest AHEAD of real time by
 * whatever the wait had left, and that is most when the interrupt comes
 * right behind another handler (which ran unpaced; its lead was about to
 * be waited out). The handler's first beam-timed access (pistorm_gov_sync)
 * then waited the whole lead out before touching the bus.
 *
 * Turrican II title, 12 Nov 2026 (write-behind on, HDMI mirror off, raster
 * trace): the line-199 Timer B handler polls $FF8209 for the $D0-$FF
 * window of its line to open the lower border, and has ~34 us from its
 * first read to the window's end. In the frames where the border did not
 * open - the handler then spins into the next frame, the VBL is 4 ms late,
 * the water is gone for two frames - the guest was 24-56 us ahead at the
 * handler's start in 15 of 17; in the 1416 good frames it was under 24 us
 * in all but 9. 10 of the 17 came within 20 us of a Timer C (music)
 * interrupt: the flicker follows the tune.
 *
 * A real interrupt happens NOW: at the outermost handler's entry the lead
 * is taken off the clock and parked, so the handler runs from real time,
 * and the parked lead is put back at the first check outside a handler,
 * where it is waited out as any lead is. Average speed is unchanged. */
int pistorm_gov_running;                    /* the governor paces this CPU (not JIT) */
static int     gov_park_on = -1;
static int64_t gov_parked_ns;
unsigned long  pistorm_gov_parks;           /* [GOV] report */
unsigned long  pistorm_gov_lead_off;        /* [GOV] report: leads over GOV_LEAD_MAX written off */
int64_t        pistorm_gov_lead_off_ns;
extern "C" void pistorm_irqtl_lead(void);   /* newcpu.cpp, PISTORM_CPU_DIAG timeline */
extern "C" uint16_t pistorm_cur_sr(void);

/* BEAM-TIMED HANDLERS (PISTORM_GOV_BEAM=0 turns this off).
 * Parking the lead makes a handler start from the moment the CPU thread
 * took the interrupt. For a Timer B event-count interrupt that moment is
 * late: ipl_task looks at the IPL lines every ~16 us, and the IACK, the
 * frame push and the handler's first I/O accesses each cost a bus
 * transaction. A border routine that counts its writes from the interrupt
 * then misses.
 * Defender (defender.st, 15 Nov 2026, raster trace + PISTORM_SHM_DEBUG=4):
 * the Timer B at the end of line 261's display enters a handler that reads
 * $FF8209 twice - once in the border (the counter frozen at $60), once
 * early in line 262's display, where `not/lsl d0,d0` turns the counter's
 * phase into a delay that ends at the same cycle wherever the read fell -
 * and then writes $FF820A = 0 / 2 around cycle 502 of line 262 to open the
 * bottom border. Its second read has to fall in a 62-cycle stretch of line
 * 262. On the Pi the first read came 17.6 us after the interrupt (7.5 on
 * an ST), the second fell past that stretch, and the 60 Hz write landed in
 * line 263-265: the border opened in 2 of 2775 frames.
 * A Timer B in event-count mode (TBCR = 8) fires a fixed time after a
 * line's display enable ends, and the beam clock (beam_clock.c: VBL
 * sightings plus one video counter read a frame, native Shifter only - no
 * HDMI needed) says when that was. So the handler's
 * clock is set to where a real 68000 would be: the interrupt's start
 * (do_interrupt) at GOV_BEAM_IRQ_CYC cycles after the display enable
 * ended. The guest is then BEHIND by the delivery delay; the handler runs
 * unpaced and catches up, and its beam-timed accesses (pistorm_gov_sync)
 * wait for their own moment. A read the CPU thread reaches late does not
 * write the debt off - that would move the clock back onto the late real
 * time - but is answered with the counter as it stood at the guest's time
 * (pistorm_natmem.cpp, BACK-DATED READS). A write reached late still
 * writes it off: it cannot land earlier. Only with the beam clock
 * calibrated (governed 68000, a counter read accepted in the last 0.5 s,
 * a 50 or 60 Hz screen with the normal line layout), only for Timer B in
 * event-count mode, and only when the delay found is 0-40 us; otherwise
 * the lead is parked as before. 50 and 60 Hz alike (the model's line
 * length and display end follow the frame rate).
 * GOV_BEAM_IRQ_CYC: where the interrupt starts, in cycles after the
 * display end - Hatari's figures: Timer B's event comes 24 cycles after
 * the display end (TIMERB_VIDEO_CYCLE_OFFSET) and the MFP's IRQ reaches
 * the CPU 4 cycles later (MFP_IRQ_DELAY_TO_CPU): 28. It was 40, tuned
 * against Defender (its second $FF8209 read at cycle ~87 of line 262, the
 * middle of its 56..118 stretch) while this core charged an interrupt the
 * generic 44 cycles; an ST's MFP interrupt is 56 (newcpu.cpp,
 * add_approximate_exception_cycles), and those 12 cycles were what the
 * extra 12 stood in for: the read lands where it did. PISTORM_GOV_BEAM_CYC
 * overrides. */
static int     gov_beam_on = -1;
static int64_t gov_beam_irq_cyc = 28;
static int     gov_beam_handler;            /* this handler's clock is the beam's */
static int64_t gov_beam_late;               /* ... set this far behind (part of the park) */
static uint32_t gov_beam_sp;                /* ... its stack at the start */
static uint32_t gov_beam_read_pc;           /* ... the instruction of its last kept-debt read */
unsigned long  pistorm_gov_beam_polls;      /* [GOV] report: reads answered as read (a poll) */
/* the last 16 handlers put on the beam's clock, for the CPU_DIAG crash
 * capture (cpu/newcpu.cpp pistorm_crash_capture) */
struct gov_beam_e { uint32_t pc, sp; int32_t late_ns, shift_ns; };
static gov_beam_e gov_beam_ring[16];
static unsigned gov_beam_ring_i;
extern "C" uint32_t pistorm_cur_ssp(void);  /* cpu/newcpu.cpp */
extern "C" int      pistorm_sp_live(uint32_t sp);
/* Is the beam-timed handler still running? Not the handler window
 * (pistorm_handler_running): that spans an outer handler, ends at its
 * 2000-cycle cap, and once ended let the park be put back and the governor
 * pause in the middle of the border routine - Defender, 15 Nov 2026: a
 * 164 us pause between the routine's reads and its $FF820A writes, the
 * writes 3-4 lines late or in the next frame's display (the picture
 * jumping). The handler is over when its RTE has taken the stack above
 * where it started; until then it is on the beam's clock whatever the
 * window says. */
static inline int beam_live(void)
{
	if (gov_beam_handler && !pistorm_sp_live(gov_beam_sp))
		gov_beam_handler = 0;
	return gov_beam_handler;
}
extern "C" int pistorm_gov_beam_live(void) { return beam_live(); }   /* newcpu.cpp timeline */
unsigned long  pistorm_gov_beam_n, pistorm_gov_beam_rej, pistorm_gov_beam_back;   /* [GOV] report */
unsigned long  pistorm_gov_beam_nomodel;
/* cpu/newcpu.cpp: guest clock at the start of do_interrupt */
extern evt_t   pistorm_irq_cyc;
/* pistorm_natmem.cpp reads this right after pistorm_gov_sync_read(): how
 * far behind real time the guest was at a beam-timed read whose debt was
 * kept (0 = not behind, or written off) */
extern "C" { int64_t pistorm_gov_read_behind_ns, pistorm_gov_read_behind_at; }
extern uae_u16 pistorm_iack_vector;         /* jit_glue.cpp */
extern "C" uint8_t mfp_hub_timer_b_vector(void);
extern "C" uint8_t mfp_hub_tbcr(void);

/* mode 0: a normal check; 1: see pistorm_gov_sync(); 2: an interrupt
 * handler is starting (pistorm_gov_irq_entry); 3: pistorm_gov_sync_read() */
/* newcpu.cpp's interrupt timeline (PISTORM_CPU_DIAG): where the governor
 * has the guest against real time right now, and the lead parked */
static evt_t   *gov_cyc0_p;
static int64_t *gov_ns0_p;
static int64_t  gov_hz_now = 8021247;
extern "C" long long pistorm_gov_ahead_now(long long *parked)
{
	if (parked)
		*parked = gov_parked_ns;
	if (!gov_cyc0_p || !gov_ns0_p)
		return 0;
	const evt_t unit = CYCLE_UNIT / 2;
	const int64_t now = gov_now_ns();
	return gov_cyc_ns(get_cycles() - *gov_cyc0_p, unit, gov_hz_now) - (now - *gov_ns0_p);
}

static void pistorm_governor(int sync)
{
	const int irq_entry = (sync == 2);
	const int sync_read = (sync == 3);
	if (irq_entry || sync_read)
		sync = 1;                               /* evaluate now, whatever gov_next says */
	if (sync_read)
		pistorm_gov_read_behind_ns = 0;
	if (currprefs.m68k_speed != 0 || currprefs.cachesize)
		return;                                 /* JIT / max speed: not ours */
	pistorm_gov_running = 1;

	static int     gov_on;
	static int     gov_report;
	static int64_t gov_khz;                     /* target, kHz (cfg/env)     */
	static int64_t gov_hz;                      /* target, Hz: the pace      */
	static int     gov_hz_auto;                 /* follow the board's clock  */
	static int64_t gov_catchup_x;               /* catch-up cap, x target    */
	static evt_t   gov_cyc0;                    /* anchor: guest cycles ...  */
	static int64_t gov_ns0;                     /* ... and real time         */
	gov_cyc0_p = &gov_cyc0; gov_ns0_p = &gov_ns0;
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
			/* An 8 MHz ST is not 8 MHz: a PAL board's 68000 runs at
			 * 8.021247 MHz (32.084988 MHz / 4; Hatari clocks_timings.c),
			 * and the Shifter, GLUE and MFP here are real, so the frame is
			 * 160256 of THOSE cycles (19979 us - what [BEAM] measures).
			 * Paced at 8.000 MHz the guest lost 0.27% to the beam: ~0.17 us
			 * a line from any sync point, half a line by line 200, and
			 * every park/write-off re-synced it at a different point - a
			 * wandering error. So an 8 MHz target follows the board's own
			 * clock, measured by the beam model (cycles per frame over the
			 * period; NTSC boards come out at theirs), starting at PAL.
			 * PISTORM_GOV_KHZ set by hand is taken as given. */
			gov_hz_auto = !env && gov_khz == 8000;
			gov_hz = gov_hz_auto ? 8021247 : gov_khz * 1000;
			const char *rep = getenv("PISTORM_GOV_REPORT");
			gov_report = rep && *rep == '1';
			const char *cu = getenv("PISTORM_GOV_CATCHUP");
			gov_catchup_x = cu ? atoll(cu) : 2;
			if (gov_catchup_x < 0 || gov_catchup_x > 100)
				gov_catchup_x = 2;
			fprintf(stderr, "[GOV] 68k governor active: %lld kHz, catch-up cap %s%lldx, "
			        "check every %lld cycles%s\n",
			        (long long)gov_khz, gov_catchup_x ? "" : "off ",
			        (long long)gov_catchup_x, (long long)gov_step,
			        gov_hz_auto ? ", paced at the board's 68000 clock "
			                      "(8021247 Hz until the beam model measures it)" : "");
		}
		gov_next = currcycle + gov_step * unit;
		gov_on = 1;
	}
	if (!sync && currcycle < gov_next)
		return;

	/* Exact HDMI mirror: once a frame, mid-display, read the real video
	 * counter so the mirror knows the beam to the cycle (shifter_mirror.c).
	 * Never inside an interrupt handler - a raster handler gets no extra
	 * bus time from us. One test of a flag when the mirror is off. */
	const int in_handler = pistorm_handler_running() || beam_live();
	if (__builtin_expect(shm_on, 0) && !in_handler)
		shm_cal_poll();
	/* the beam clock (beam_clock.c): VBL sightings in, and once a frame
	 * its own video counter read - never inside a handler */
	if (!in_handler)
		bc_poll();

	int64_t now = gov_now_ns();
	/* PISTORM_RASTER_TRACE 'H': real time since the last check ended,
	 * i.e. spent running 32 guest cycles (and their bus accesses) */
	{
		static int64_t gov_last_exit;
		struct gov_exit_t {
			~gov_exit_t() { if (__builtin_expect(rt_on, 0)) gov_last_exit = gov_now_ns(); }
		} gov_exit_;
		if (__builtin_expect(rt_on, 0) && gov_last_exit && !irq_entry &&
		    now - gov_last_exit > 20000)
			rt_gap(0, now - gov_last_exit, pistorm_cur_pc());
	}

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
			if (gov_report || (!said && khz * 10000 < gov_hz * 9)) {
				said = 1;
				fprintf(stderr, "[GOV] reaching %lld kHz of the %lld kHz target "
				        "(5 s average)", (long long)khz, (long long)(gov_hz / 1000));
				if (pistorm_gov_beam_n || pistorm_gov_beam_rej || pistorm_gov_beam_nomodel)
					fprintf(stderr, " | beam-timed Timer B handlers %lu (%lu out of range, "
					        "%lu with no calibrated beam model), late reads back-dated %lu, "
					        "polls on real time %lu",
					        pistorm_gov_beam_n, pistorm_gov_beam_rej,
					        pistorm_gov_beam_nomodel, pistorm_gov_beam_back,
					        pistorm_gov_beam_polls);
				if (pistorm_gov_lead_off)
					fprintf(stderr, " | leads written off %lu (%lld us in all)",
					        pistorm_gov_lead_off, (long long)(pistorm_gov_lead_off_ns / 1000));
				fputc('\n', stderr);
			}
			win_ns0 = now;
			win_cyc0 = currcycle;
		}
	}

	/* PISTORM_WRBEHIND: RAM writes an interrupt handler left queued go
	 * to the bus now that it has ended - until an interrupt is due
	 * (platforms/atari/stram_wb.c). The time is real: it shows in `now`. */
	if (__builtin_expect(stram_wb_n != 0, 0) && !sync && !in_handler) {
		stram_wb_drain();
		now = gov_now_ns();
	}

	/* follow the board's clock (above): once a second, outside handlers.
	 * Re-anchored so the lead or debt carries over unchanged. Steps under
	 * 20 Hz (2.5 ppm) are the period estimate's own noise: ignored. */
	if (gov_hz_auto && !in_handler && !irq_entry) {
		static int64_t hz_chk_ns;
		if (now - hz_chk_ns > 1000000000LL) {
			hz_chk_ns = now;
			const double m = bc_cpu_hz();
			if (m > 7900000.0 && m < 8100000.0) {
				const int64_t hz = (int64_t)(m + 0.5);
				if (hz - gov_hz > 20 || gov_hz - hz > 20) {
					const int64_t old_ahead = gov_cyc_ns(currcycle - gov_cyc0, unit, gov_hz)
					                          - (now - gov_ns0);
					static int said_hz;
					if (gov_report || !said_hz)
						fprintf(stderr, "[GOV] board 68000 clock measured: %lld Hz "
						        "(was pacing at %lld)\n", (long long)hz, (long long)gov_hz);
					said_hz = 1;
					gov_hz   = hz;
					gov_cyc0 = currcycle;
					gov_ns0  = now + old_ahead;
				}
			}
		}
	}

	gov_hz_now = gov_hz;
	int64_t virt_ns  = gov_cyc_ns(currcycle - gov_cyc0, unit, gov_hz);
	int64_t ahead_ns = virt_ns - (now - gov_ns0);

	/* A LEAD NO 68000 CAN HAVE (PISTORM_GOV_LEAD_MAX_US, default 1000;
	 * 0 = off). A real CPU is never ahead of its clock. Here a lead is
	 * normal only in small amounts: a handler runs unpaced for at most its
	 * 2000-cycle window (~250 us of guest time), any other lead is waited
	 * off at the next 4 us check. Turrican II, 18 Nov 2026 (interrupt
	 * timeline at the crash): 10.7 ms of lead, parked and put back at every
	 * Timer C tick, waited off at ~53 us per 77 us tick - the guest crawled
	 * at a third of its speed for ~30 ms, so the music handler's sample
	 * mixer (one sample per tick) was still running when its block ran
	 * out: the handler re-entered itself, its saved registers were lost,
	 * bombs. Over the limit is an accounting fault, not time the guest is
	 * owed: it is written off, counted, and the first ones are printed
	 * with what the governor thought was running. */
	{
		static int64_t lead_max = -1;
		if (lead_max < 0) {
			const char *e = getenv("PISTORM_GOV_LEAD_MAX_US");
			lead_max = e ? atoll(e) * 1000LL : 1000000LL;
			if (lead_max < 0)
				lead_max = 0;
		}
		const int64_t lead = ahead_ns + gov_parked_ns;
		if (lead_max && sync != 2 && lead > lead_max) {
			static unsigned shown;
			pistorm_gov_lead_off++;
			pistorm_gov_lead_off_ns += lead;
			if (shown < 20) {
				shown++;
				fprintf(stderr, "[GOV] lead of %lld us written off (ahead %lld, parked %lld) at pc "
				        "%06X sp %06X sr %04X: window %d, beam handler %d (its sp %06X)%s\n",
				        (long long)(lead / 1000), (long long)(ahead_ns / 1000),
				        (long long)(gov_parked_ns / 1000), (unsigned)(pistorm_cur_pc() & 0xFFFFFFu),
				        (unsigned)(pistorm_cur_ssp() & 0xFFFFFFu), (unsigned)pistorm_cur_sr(),
				        pistorm_handler_running(), beam_live(), (unsigned)(gov_beam_sp & 0xFFFFFFu),
				        shown == 20 ? " - no more of these printed" : "");
			}
			pistorm_irqtl_lead();
			gov_ns0 += ahead_ns;            /* the guest back on real time */
			gov_parked_ns = 0;
			ahead_ns = 0;
			virt_ns = now - gov_ns0;
		}
	}

	if (irq_entry) {
		/* BEAM-TIMED HANDLERS: a Timer B event-count interrupt starts on
		 * the beam's clock (see above) */
		if (gov_beam_on < 0) {
			const char *e = getenv("PISTORM_GOV_BEAM");
			gov_beam_on = !(e && *e == '0');
			const char *c = getenv("PISTORM_GOV_BEAM_CYC");
			if (c) {
				long v = atol(c);
				if (v >= 0 && v <= 200)
					gov_beam_irq_cyc = v;
			}
		}
		if (gov_beam_on &&
		    (pistorm_iack_vector & 0xFFu) == mfp_hub_timer_b_vector() &&
		    (mfp_hub_tbcr() & 0x0Fu) == 8u) {
			const uint64_t tk = bc_ticks();
			double de, tc;
			if (bc_de_end(tk, &de, &tc)) {
				/* real ns since the interrupt should have started, less
				 * the guest's ns since do_interrupt began */
				const double t_irq = de + (double)gov_beam_irq_cyc * tc;
				const int64_t since_irq_ns = (int64_t)(((double)tk - t_irq) * 1e9 / bc_tick_hz());
				const int64_t guest_ns = currcycle > pistorm_irq_cyc
				    ? gov_cyc_ns(currcycle - pistorm_irq_cyc, unit, gov_hz) : 0;
				const int64_t late_ns = since_irq_ns - guest_ns;
				if (late_ns > -15000 && late_ns < 40000) {
					/* the guest's clock to (real - late): it is behind by late */
					const int64_t shift = ahead_ns + late_ns;
					gov_ns0 -= shift;
					gov_parked_ns += shift;     /* put back when the handler is over */
					gov_beam_handler = 1;
					gov_beam_late = late_ns;
					gov_beam_read_pc = 0;
					gov_beam_sp = pistorm_cur_ssp();
					pistorm_gov_beam_n++;
					{
						gov_beam_e *r = &gov_beam_ring[gov_beam_ring_i++ & 15];
						r->pc = pistorm_cur_pc(); r->sp = gov_beam_sp;
						r->late_ns = (int32_t)late_ns; r->shift_ns = (int32_t)shift;
					}
					rt_gov(late_ns, 2);
					return;
				}
				pistorm_gov_beam_rej++;
			} else
				pistorm_gov_beam_nomodel++;
		}
		if (beam_live())
			return;                             /* nested in a beam-timed handler: its clock stays */
		if (ahead_ns > 0) {                     /* park the lead */
			gov_ns0 -= ahead_ns;
			gov_parked_ns += ahead_ns;
			pistorm_gov_parks++;
			rt_gov(ahead_ns, 1);
		}
		return;
	}
	if (gov_parked_ns && !sync && !in_handler) {
		gov_ns0 += gov_parked_ns;               /* ... and put it back */
		ahead_ns += gov_parked_ns;
		gov_parked_ns = 0;
	}

	if (sync) {
		/* a beam-timed register access: the guest and the real beam must
		 * agree NOW. Ahead: wait out all of it (still capped). Behind:
		 * write the debt off here instead of repaying it at up to
		 * GOV_CATCHUP_X x speed - repayment compresses exactly the delay
		 * loops a border routine times its next write with.
		 * This comes BEFORE the handler test below: Turrican II's
		 * lower-border sync is done from inside its Timer B handler, so a
		 * handler must still line up with the beam when it asks to. */
		if (ahead_ns > 0) {
			const int64_t w0 = now;
			now = gov_wait_until(now, now + (ahead_ns < GOV_WAIT_MAX ? ahead_ns : GOV_WAIT_MAX));
			if (now - w0 > 2000)
				rt_gap(1, now - w0, pistorm_cur_pc());
		} else if (sync_read && beam_live() && pistorm_cur_pc() != gov_beam_read_pc) {
			/* BEAM-TIMED HANDLER, a read reached late: keep the beam's
			 * clock, the read is answered as at the guest's time */
			gov_beam_read_pc = pistorm_cur_pc();
			pistorm_gov_read_behind_ns = -ahead_ns;
			pistorm_gov_read_behind_at = now;   /* CLOCK_MONOTONIC: only for the read that follows */
			pistorm_gov_beam_back++;
		} else {
			/* ... but the SAME instruction reading again is a poll: the
			 * loop's own exit times what follows, as on a real ST, so
			 * the guest is put on real time and the read answered as
			 * read. Back-dating every pass kept the guest off real time
			 * through the whole poll - Turrican II's lower-border poll in
			 * its Timer B handler ($114E4, move.b $8209 / cmp.b #$D0 /
			 * bcs): reads back-dated by up to 62 counter bytes, the guest
			 * up to 16 us behind. Now and then its main loop then met the
			 * end of a sample block in the Timer C music handler while A7
			 * pointed at a table just above the real stack ($28F5E): the
			 * handler's ~60 bytes of pushes overwrote the return address,
			 * rts popped garbage, address error, bombs (17 Nov 2026:
			 * crashes in seconds to minutes with the beam path, an hour
			 * clean with PISTORM_GOV_BEAM=0). Defender's two reads are two
			 * instructions: both are still answered at the guest's time. */
			if (sync_read && beam_live())
				pistorm_gov_beam_polls++;
			gov_ns0 = now - virt_ns;
			if (beam_live() && gov_beam_late) {
				/* the debt written off includes the delay the beam's clock
				 * put on: it is not owed again when the park is put back */
				gov_parked_ns -= gov_beam_late;
				gov_beam_late = 0;
			}
		}
	} else if (in_handler) {
		/* inside an interrupt handler: no pause - it is paid back at the
		 * next check outside. step_ns/step_cyc stay where they are, so
		 * the catch-up cap there spans the handler too. */
		if (ahead_ns > 0)
			rt_gov(ahead_ns, 1);
		gov_next = currcycle + gov_step * unit;
		return;
	} else if (ahead_ns > 0) {
		/* ahead of real time: wait for it (at most GOV_WAIT_MAX per check) */
		rt_gov(ahead_ns < GOV_WAIT_MAX ? ahead_ns : GOV_WAIT_MAX, 0);
		now = gov_wait_until(now, now + (ahead_ns < GOV_WAIT_MAX ? ahead_ns : GOV_WAIT_MAX));
	} else {
		if (ahead_ns < -GOV_FORGIVE_NS) {
			/* a host stall, not bus time: write off all but GOV_FORGIVE_NS */
			gov_ns0 -= ahead_ns + GOV_FORGIVE_NS;
		}
		/* repaying debt: this step may not have run faster than
		 * gov_catchup_x times the target (see CATCH-UP above) */
		if (gov_catchup_x) {
			int64_t min_ns = gov_cyc_ns(currcycle - step_cyc, unit, gov_hz * gov_catchup_x);
			if (now - step_ns < min_ns) {
				rt_gov(step_ns + min_ns - now, 0);
				now = gov_wait_until(now, step_ns + min_ns);
			}
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

/* the same, before a READ of one of those registers (see BEAM-TIMED
 * HANDLERS: a late read does not write the debt off) */
extern "C" void pistorm_gov_sync_read(void)
{
	pistorm_governor(3);
}

/* newcpu.cpp: the outermost interrupt handler is starting - see THE LEAD
 * AT AN INTERRUPT */
void pistorm_gov_irq_entry(void)
{
	if (gov_park_on < 0) {
		const char *e = getenv("PISTORM_GOV_PARK");
		gov_park_on = !(e && *e == '0');
	}
	if (gov_park_on)
		pistorm_governor(2);
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

/* raster_trace.c (PISTORM_RASTER_TRACE): guest 68000 clocks so far, to
 * set the work done in a Timer B window against the time it took */
extern "C" uint64_t pistorm_guest_clocks(void)
{
	return (uint64_t)get_cycles() / (CYCLE_UNIT / 2);
}

/* CPU_DIAG crash capture (cpu/newcpu.cpp): the beam-timed handler state */
extern "C" void pistorm_gov_beam_dump(void)
{
	fprintf(stderr, "[CRASH] governor: beam-timed handlers %lu (%lu out of range, %lu no "
	        "model), late reads back-dated %lu; now: beam handler %d (live %d, sp %08X), "
	        "late %lld ns, parked %lld ns, read behind %lld ns\n",
	        pistorm_gov_beam_n, pistorm_gov_beam_rej, pistorm_gov_beam_nomodel,
	        pistorm_gov_beam_back, gov_beam_handler, beam_live(), (unsigned)gov_beam_sp,
	        (long long)gov_beam_late, (long long)gov_parked_ns,
	        (long long)pistorm_gov_read_behind_ns);
	unsigned n = gov_beam_ring_i < 16 ? gov_beam_ring_i : 16;
	fprintf(stderr, "[CRASH] last %u handlers put on the beam's clock (pc sp late_ns shift_ns):\n", n);
	for (unsigned k = 0; k < n; k++) {
		const gov_beam_e *r = &gov_beam_ring[(gov_beam_ring_i - n + k) & 15];
		fprintf(stderr, "[CRASH]   %06X %08X %d %d\n", (unsigned)r->pc, (unsigned)r->sp,
		        (int)r->late_ns, (int)r->shift_ns);
	}
}
