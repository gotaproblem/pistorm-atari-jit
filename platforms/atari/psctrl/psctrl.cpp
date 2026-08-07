// SPDX-License-Identifier: MIT
//
// PSCTRL host-side sampler. See psctrl.h for the design summary.
//
// The sampler thread wakes every 500 ms, differences the free-running
// counters against its previous snapshot, reads the Raspberry Pi sysfs
// sensors, and publishes everything into g_snap. Readers (the CPU thread,
// via the PSCTRL NatFeat) see at worst a value from the previous window;
// individual aligned 32-bit fields cannot tear on AArch64.

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"

#include "platforms/atari/psctrl/psctrl.h"
#include "config_file/config_file.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

/* --- counters bumped by the CPU/JIT core ------------------------------- */

volatile uint32_t psctrl_ctr_compiles = 0;
volatile uint32_t psctrl_ctr_flushes = 0;
volatile uint32_t psctrl_ctr_interp_calls = 0;
volatile uint32_t psctrl_ctr_stop_iters = 0;
volatile uint32_t psctrl_ctr_interp_cycles = 0;

/* The JIT's cycle clock (cpu/events.cpp) and the current 68k cycle length
 * in clock units. Global data symbols, no C++ mangling to worry about. */
extern signed long long currcycle;
extern int cpucycleunit;

/* --- published snapshot ------------------------------------------------ */

struct psctrl_snapshot {
  volatile uint32_t epoch;
  volatile uint32_t cache_used;
  volatile uint32_t cache_total;
  volatile uint32_t compiles;
  volatile uint32_t flushes;
  volatile uint32_t interp_calls;
  volatile uint32_t stop_iters;
  volatile uint32_t soc_temp_mc;
  volatile uint32_t arm_freq_khz;
  volatile uint32_t loadavg_x100;
  volatile uint32_t uptime_s;
  volatile uint32_t throttled;
  volatile uint32_t jit_eff_khz;
  volatile uint32_t jit_hit_x10;
  volatile uint32_t jit_idle_x10;
};

static struct psctrl_snapshot g_snap;

/* previous counter values, sampler-thread private */
static uint32_t s_prev_compiles;
static uint32_t s_prev_flushes;
static uint32_t s_prev_interp_calls;
static uint32_t s_prev_stop_iters;

/* --- helpers ----------------------------------------------------------- */

/* Read the first numeric token of a small sysfs/proc file.
 * Returns 0 and leaves *out untouched on any failure. */
/* Read a small sysfs file holding a hex value (e.g. get_throttled).
 * Returns 0 and leaves *out untouched on any failure. */
static int read_file_hex(const char *path, unsigned long *out)
{
  FILE *f = fopen(path, "r");
  char buf[64];

  if (!f)
    return 0;

  if (!fgets(buf, sizeof(buf), f)) {
    fclose(f);
    return 0;
  }
  fclose(f);

  char *end = NULL;
  unsigned long v = strtoul(buf, &end, 16);

  if (end == buf)
    return 0;

  *out = v;
  return 1;
}

/* Read the firmware throttle state directly over the VideoCore mailbox
 * (property tag GET_THROTTLED), exactly as vcgencmd does. This is kernel-
 * version-proof - the sysfs get_throttled node moves/vanishes between
 * kernel releases (field finding on 6.18: the sampler silently read
 * nothing and the guest saw a permanently clean history while vcgencmd
 * showed 0xe0000). The request value is a clear-mask for the sticky
 * bits: we pass 0, so reading NEVER erases the since-boot history. */
static int vcio_fd = -2;            /* -2 = not tried, -1 = unavailable */

static int vcio_open(void)
{
  if (vcio_fd == -2) {
    vcio_fd = open("/dev/vcio", O_RDONLY | O_CLOEXEC);
    if (vcio_fd < 0)
      fprintf(stderr, "[PSCTRL] /dev/vcio unavailable - throttle state and "
                      "measured ARM clock fall back to sysfs (if present)\n");
  }
  return vcio_fd;
}

static int read_throttled_mailbox(uint32_t *out)
{
  uint32_t msg[8];

  if (vcio_open() < 0)
    return 0;

  msg[0] = 8 * sizeof(uint32_t);    /* total buffer size */
  msg[1] = 0;                       /* this is a request */
  msg[2] = 0x00030046;              /* GET_THROTTLED */
  msg[3] = 4;                       /* value buffer size */
  msg[4] = 4;                       /* request: 4 bytes follow */
  msg[5] = 0;                       /* sticky clear-mask: clear NOTHING */
  msg[6] = 0;                       /* end tag */
  msg[7] = 0;

  if (ioctl(vcio_fd, _IOWR(100, 0, char *), msg) < 0)
    return 0;
  if (msg[1] != 0x80000000u)        /* firmware says: request failed */
    return 0;

  *out = msg[5];
  return 1;
}

/* The ACTUAL ARM clock, measured by the firmware (what vcgencmd
 * measure_clock arm shows). The kernel's cpufreq sysfs reports the
 * REQUESTED clock, which with force_turbo=1 is pinned at 1500 MHz no
 * matter what the firmware's thermal scaling is really doing - the
 * guest's tooltip showed a rock-steady 1500 while the real clock sat
 * at 1238. Only the firmware knows the truth. */
static int read_arm_clock_mailbox(uint32_t *out_khz)
{
  uint32_t msg[9];

  if (vcio_open() < 0)
    return 0;

  msg[0] = 9 * sizeof(uint32_t);    /* total buffer size */
  msg[1] = 0;                       /* this is a request */
  msg[2] = 0x00030047;              /* GET_CLOCK_RATE_MEASURED */
  msg[3] = 8;                       /* value buffer size */
  msg[4] = 8;                       /* request: 8 bytes follow */
  msg[5] = 3;                       /* clock id: ARM */
  msg[6] = 0;                       /* response: rate lands here (Hz) */
  msg[7] = 0;                       /* end tag */
  msg[8] = 0;

  if (ioctl(vcio_fd, _IOWR(100, 0, char *), msg) < 0)
    return 0;
  if (msg[1] != 0x80000000u || msg[6] == 0)
    return 0;

  *out_khz = msg[6] / 1000u;
  return 1;
}

static int read_file_number(const char *path, double *out)
{
  FILE *f = fopen(path, "r");
  char buf[64];

  if (!f)
    return 0;

  if (!fgets(buf, sizeof(buf), f)) {
    fclose(f);
    return 0;
  }
  fclose(f);

  char *end = NULL;
  double v = strtod(buf, &end);

  if (end == buf)
    return 0;

  *out = v;
  return 1;
}

static void psctrl_sample_once(void)
{
  double v;

  /* JIT cache state (plain loads of core-side values) */
  g_snap.cache_used = psctrl_jit_cache_used();
  g_snap.cache_total = psctrl_jit_cache_total();

  /* window deltas of the free-running counters */
  uint32_t c;

  c = psctrl_ctr_compiles;
  g_snap.compiles = c - s_prev_compiles;
  s_prev_compiles = c;

  c = psctrl_ctr_flushes;
  g_snap.flushes = c - s_prev_flushes;
  s_prev_flushes = c;

  c = psctrl_ctr_interp_calls;
  g_snap.interp_calls = c - s_prev_interp_calls;
  s_prev_interp_calls = c;

  c = psctrl_ctr_stop_iters;
  g_snap.stop_iters = c - s_prev_stop_iters;
  s_prev_stop_iters = c;

  /* host sensors; on failure the previous value is retained */
  if (read_file_number("/sys/class/thermal/thermal_zone0/temp", &v))
    g_snap.soc_temp_mc = (uint32_t)v;

  {
    /* Prefer the firmware's MEASURED clock (thermal scaling shows up
     * there); cpufreq sysfs is the fallback, but with force_turbo=1 it
     * reports a pinned 1500 MHz regardless of reality. */
    uint32_t khz;

    if (read_arm_clock_mailbox(&khz))
      g_snap.arm_freq_khz = khz;
    else if (read_file_number(
               "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", &v))
      g_snap.arm_freq_khz = (uint32_t)v;
  }

  if (read_file_number("/proc/loadavg", &v))
    g_snap.loadavg_x100 = (uint32_t)(v * 100.0);

  if (read_file_number("/proc/uptime", &v))
    g_snap.uptime_s = (uint32_t)v;

  {
    /* Primary source: the VideoCore mailbox with a zero clear-mask -
     * kernel-proof and history-preserving. Fallback: the sysfs node
     * (path varies by kernel; on some it does not exist at all).
     * Either way, keep our own latch of every event ever observed, so
     * the guest's since-boot history survives anything that clears the
     * firmware's sticky bits behind our back. */
    static uint32_t thr_seen = 0;
    uint32_t t;
    unsigned long thr;
    int got = read_throttled_mailbox(&t);

    if (!got && read_file_hex(
            "/sys/devices/platform/soc/soc:firmware/get_throttled", &thr)) {
      t = (uint32_t)thr;
      got = 1;
    }

    if (got) {
      thr_seen |= (t & 0xFu) << 16;    /* active now -> seen since boot */
      thr_seen |= t & 0xF0000u;        /* firmware-reported sticky bits */
      g_snap.throttled = (t & 0xFFFFu) | thr_seen;
    }
  }

  /* Phase 4: JIT engine figures, entirely from state the emulator already
   * maintains (design: the project's phase4-impact-report - nothing new on
   * the hot path, the sampler just differences existing globals).
   *   speed   : Delta(currcycle) at the 8 MHz reference (one 68k cycle =
   *             CYCLE_UNIT/2 clock units = 125 ns) over measured wall time
   *   hit rate: 1 - interpreted cycles / all cycles (cycle-weighted, so a
   *             million cheap interpreted instructions don't hide)
   *   idle    : STOP iterations x 4 x cpucycleunit / all cycles - true
   *             MiNT idle, since do_cycles_stop feeds the same clock
   * PISTORM_NOSTATS=1 skips the arithmetic entirely (A/B benchmarking). */
  {
    static int nostats = -1;
    static signed long long prev_cc;
    static uint32_t prev_ic, prev_si;
    static struct timespec prev_ts;
    signed long long cc = currcycle;
    uint32_t ic = psctrl_ctr_interp_cycles;
    uint32_t si = psctrl_ctr_stop_iters;
    struct timespec ts;

    if (nostats < 0) {
      const char *e = getenv("PISTORM_NOSTATS");
      nostats = (e && *e == '1') ? 1 : 0;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);

    if (!nostats && prev_ts.tv_sec != 0) {
      double dt = (double)(ts.tv_sec - prev_ts.tv_sec) +
                  (double)(ts.tv_nsec - prev_ts.tv_nsec) / 1e9;
      signed long long dcc = cc - prev_cc;

      if (dt > 0.05 && dcc > 0) {
        double cyc68k = (double)dcc / (double)(CYCLE_UNIT / 2);
        double share;
        long v;

        g_snap.jit_eff_khz = (uint32_t)(cyc68k / dt / 1000.0 + 0.5);

        share = (double)(uint32_t)(ic - prev_ic) / (double)dcc;
        if (share > 1.0)
          share = 1.0;
        v = (long)((1.0 - share) * 1000.0 + 0.5);
        g_snap.jit_hit_x10 = (uint32_t)(v < 0 ? 0 : v);

        share = (double)(uint32_t)(si - prev_si) * 4.0 *
                (double)cpucycleunit / (double)dcc;
        if (share > 1.0)
          share = 1.0;
        g_snap.jit_idle_x10 = (uint32_t)(share * 1000.0 + 0.5);
      }
    }

    prev_cc = cc;
    prev_ic = ic;
    prev_si = si;
    prev_ts = ts;
  }

  /* bump last so a reader that keys on the epoch sees finished values */
  g_snap.epoch = g_snap.epoch + 1;
}

static void *psctrl_sampler_thread(void *arg)
{
  (void)arg;

  /* Belt and braces (see spawn below): this thread is created from the
   * CPU thread, which runs SCHED_FIFO pinned to an isolated core. If it
   * inherited that, it would stand behind a 100%-busy JIT loop and never
   * run again - the bug signature is host statistics frozen at their
   * boot-time values. Demote to SCHED_OTHER on the non-isolated cores,
   * same rules as the VIDPLAY media threads. */
  {
    struct sched_param sp;
    cpu_set_t set;
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    memset(&sp, 0, sizeof(sp));
    sched_setscheduler(0, SCHED_OTHER, &sp);

    CPU_ZERO(&set);
    for (long i = 0; i < n && i < CPU_SETSIZE; i++)
      if (i != 2 && i != 3)            /* 2 = JIT CPU, 3 = IPL poller (both
                                        * isolated); helpers stay on 0-1 */
        CPU_SET((int)i, &set);
    sched_setaffinity(0, sizeof(set), &set);
  }

  struct timespec ts = { 0, 500000000L };  /* 500 ms */

  for (;;) {
    nanosleep(&ts, NULL);
    psctrl_sample_once();
  }

  return NULL;
}

static void psctrl_sampler_start_once(void)
{
  /* one synchronous sample so the very first guest read is not all zeros */
  psctrl_sample_once();

  /* Create the thread explicitly SCHED_OTHER: without EXPLICIT_SCHED the
   * creator's SCHED_FIFO policy and core pinning are inherited and the
   * sampler starves behind the JIT loop (values frozen at boot). */
  pthread_t t;
  pthread_attr_t attr;
  struct sched_param sp;
  int rc;

  memset(&sp, 0, sizeof(sp));

  pthread_attr_init(&attr);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
  pthread_attr_setschedparam(&attr, &sp);

  rc = pthread_create(&t, &attr, psctrl_sampler_thread, NULL);
  pthread_attr_destroy(&attr);

  if (rc != 0)                          /* some setups refuse explicit sched */
    rc = pthread_create(&t, NULL, psctrl_sampler_thread, NULL);

  if (rc == 0) {
    pthread_setname_np(t, "psctrl-sampler");
    pthread_detach(t);
    printf("[PSCTRL] sampler started (500 ms tick)\n");
  } else {
    printf("[PSCTRL] WARNING: sampler thread failed to start; "
           "values will be stale\n");
  }
}

void psctrl_sampler_start(void)
{
  static pthread_once_t once = PTHREAD_ONCE_INIT;

  pthread_once(&once, psctrl_sampler_start_once);
}

/* --- the indexed read -------------------------------------------------- */

uint32_t psctrl_getint(uint32_t index)
{
  switch (index) {
    /* configuration */
    case PS_CFG_JIT_ENABLED:
      return psctrl_jit_enabled() ? 1 : 0;
    case PS_CFG_CACHE_SIZE_KB:
      return (uint32_t)currprefs.cachesize;
    case PS_CFG_CPU_MODEL:
      return (uint32_t)currprefs.cpu_model;
    case PS_CFG_FPU_MODEL:
      return (uint32_t)currprefs.fpu_model;
    case PS_CFG_TTRAM_SIZE: {
      const struct emulator_config *cfg = emulator_config_current();
      return (cfg && cfg->ttram) ? cfg->ttram_size : 0;
    }

    /* sampled guest/JIT statistics */
    case PS_STAT_EPOCH:
      return g_snap.epoch;
    case PS_STAT_CACHE_USED:
      return g_snap.cache_used;
    case PS_STAT_CACHE_TOTAL:
      return g_snap.cache_total;
    case PS_STAT_COMPILES:
      return g_snap.compiles;
    case PS_STAT_FLUSHES:
      return g_snap.flushes;
    case PS_STAT_INTERP_CALLS:
      return g_snap.interp_calls;
    case PS_STAT_STOP_ITERS:
      return g_snap.stop_iters;

    /* host statistics */
    case PS_HOST_SOC_TEMP_MC:
      return g_snap.soc_temp_mc;
    case PS_HOST_ARM_FREQ_KHZ:
      return g_snap.arm_freq_khz;
    case PS_HOST_LOADAVG_X100:
      return g_snap.loadavg_x100;
    case PS_HOST_UPTIME_S:
      return g_snap.uptime_s;
    case PS_HOST_THROTTLED:
      return g_snap.throttled;
    case PS_JIT_EFF_KHZ:
      return g_snap.jit_eff_khz;
    case PS_JIT_HITRATE_X10:
      return g_snap.jit_hit_x10;
    case PS_JIT_IDLE_X10:
      return g_snap.jit_idle_x10;

    /* Pi wall clock, computed on demand (the Pi is NTP-synced; the
     * Atari has no battery RTC, so the guest can set its GEMDOS clock
     * from these) */
    case PS_HOST_TIME_DOS:
    case PS_HOST_DATE_DOS: {
      time_t now = time(NULL);
      struct tm lt;

      if (localtime_r(&now, &lt) == NULL)
        return (uint32_t)-1;

      if (index == PS_HOST_TIME_DOS)
        return (uint32_t)((lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2));

      return (uint32_t)(((lt.tm_year - 80) << 9) | ((lt.tm_mon + 1) << 5) | lt.tm_mday);
    }
  }

  return (uint32_t)-1;
}
