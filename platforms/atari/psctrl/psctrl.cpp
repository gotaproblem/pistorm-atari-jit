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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- counters bumped by the CPU/JIT core ------------------------------- */

volatile uint32_t psctrl_ctr_compiles = 0;
volatile uint32_t psctrl_ctr_flushes = 0;
volatile uint32_t psctrl_ctr_interp_calls = 0;
volatile uint32_t psctrl_ctr_stop_iters = 0;

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

  if (read_file_number("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", &v))
    g_snap.arm_freq_khz = (uint32_t)v;

  if (read_file_number("/proc/loadavg", &v))
    g_snap.loadavg_x100 = (uint32_t)(v * 100.0);

  if (read_file_number("/proc/uptime", &v))
    g_snap.uptime_s = (uint32_t)v;

  /* bump last so a reader that keys on the epoch sees finished values */
  g_snap.epoch = g_snap.epoch + 1;
}

static void *psctrl_sampler_thread(void *arg)
{
  (void)arg;

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

  pthread_t t;

  if (pthread_create(&t, NULL, psctrl_sampler_thread, NULL) == 0) {
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
  }

  return (uint32_t)-1;
}
