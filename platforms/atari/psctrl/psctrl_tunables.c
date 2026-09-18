// SPDX-License-Identifier: MIT
//
// PSCTRL tunables — see psctrl_tunables.h.
//
// One place where the environment is read, once, at startup. Everything
// after that goes through the settings descriptor table.

#include "platforms/atari/psctrl/psctrl_tunables.h"

#include <stdlib.h>
#include <stdio.h>

/* --- storage -------------------------------------------------------- */

volatile int pst_pissoff_mult    = 1024;

volatile int pst_fps             = 0;      /* 0 = follow the .cfg          */
volatile int pst_drm_dirtyband   = 1;
volatile int pst_drm_async       = 0;

volatile int pst_blit_timed_ns   = 0;

volatile int pst_ym_gain_x100    = 100;
volatile int pst_ym_lag_ms       = 100;
volatile int pst_lmc             = 1;
volatile int pst_audio_frames    = 2048;

volatile int pst_mouse_thresh    = 0;
volatile int pst_mouse_scale     = 1;

volatile int pst_stbox_slice_cyc = 64;
volatile int pst_stbox_telemetry = 0;

volatile int pst_ipl_confirm_ns  = 2000;
volatile int pst_vbl_refract_ns  = 5000000;
volatile uint64_t pst_ipl_confirm_ticks = 0;
volatile uint64_t pst_vbl_refract_ticks = 0;
volatile uint64_t psctrl_cntfrq         = 0;

volatile int pst_verbose         = 0;
volatile int pst_comp_constjump  = -1;
volatile int pst_compnf          = -1;
volatile int pst_compfpu         = -1;
volatile int pst_dbg_ipl_stats   = 0;
volatile int pst_dbg_irq_stats   = 0;
volatile int pst_dbg_mfp         = 0;
volatile int pst_dbg_mfp_hub     = 0;
volatile int pst_dbg_blit_trace  = 0;
volatile int pst_dbg_dmasnd      = 0;
volatile int pst_dbg_acsi        = 0;
volatile int pst_dbg_ide         = 0;
volatile int pst_dbg_hostfs      = -1;
volatile int pst_dbg_gemdos      = 0;
volatile int pst_dbg_stram       = 0;
volatile int pst_dbg_net         = 0;
volatile int pst_dbg_ikbd        = 0;

/* --- reading the environment ---------------------------------------- */

/* The tree had three different truth rules for its boolean env vars:
 * `== '1'`, `non-empty && != "0"`, and bare presence. PISTORM_ACSI_DEBUG=2
 * enabled the debug print in acsi.c but not the one in atari_fdd.c, and
 * PISTORM_MFP_DEBUG=0 silenced mfp_trace() while still enabling the REZ
 * trace in pistorm_natmem.cpp. Collapsing them onto one rule here is a
 * deliberate behaviour change, and the rule is the forgiving one: set is
 * on, "0" is off, a number is that number. */
static int env_int(const char *name, int dflt, int lo, int hi)
{
  const char *e = getenv(name);
  long v;
  char *end = NULL;

  if (!e || !*e)
    return dflt;
  v = strtol(e, &end, 0);
  if (end == e)
    return dflt;                    /* not a number: leave the default */
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return (int)v;
}

static int env_bool(const char *name, int dflt)
{
  const char *e = getenv(name);

  if (!e)
    return dflt;
  if (!*e)
    return 1;                       /* bare presence = on */
  return (e[0] == '0' && e[1] == '\0') ? 0 : 1;
}

static int env_gain_x100(const char *name, int dflt)
{
  const char *e = getenv(name);
  double d;
  char *end = NULL;

  if (!e || !*e)
    return dflt;
  d = strtod(e, &end);
  if (end == e)
    return dflt;
  if (d < 0.0) d = 0.0;
  if (d > 4.0) d = 4.0;
  return (int)(d * 100.0 + 0.5);
}

void psctrl_tunables_ipl_recalc(void)
{
  uint64_t f = psctrl_cntfrq;

  if (!f)
    f = 54000000ULL;                /* Pi 4 arch-timer fallback */

  pst_ipl_confirm_ticks = (uint64_t)pst_ipl_confirm_ns * f / 1000000000ULL;
  pst_vbl_refract_ticks = (uint64_t)pst_vbl_refract_ns * f / 1000000000ULL;
}

void psctrl_tunables_init(void)
{
  static int done = 0;

  if (done)
    return;
  done = 1;

  /* Called AFTER the config file is parsed, and every default below is
   * the value already in the global. So the precedence is: built-in
   * default, then the .cfg key, then the environment - which keeps a
   * PISTORM_* in an existing launch script winning, the way it does
   * today, while making the .cfg the place these actually live. */

  pst_pissoff_mult   = env_int ("PISTORM_PISSOFF",         pst_pissoff_mult, 1, 65536);

  pst_drm_dirtyband  = env_bool("PISTORM_DRM_DIRTYBAND",   pst_drm_dirtyband);
  pst_drm_async      = env_bool("PISTORM_DRM_ASYNC",       pst_drm_async);

  pst_blit_timed_ns  = env_int ("PISTORM_BLIT_TIMED_NS",   pst_blit_timed_ns, 0, 100000);

  pst_ym_gain_x100   = env_gain_x100("PISTORM_YM_GAIN",    pst_ym_gain_x100);
  pst_ym_lag_ms      = env_int ("PISTORM_YM_LAG_MS",       pst_ym_lag_ms, 5, 200);
  pst_lmc            = env_bool("PISTORM_LMC",             pst_lmc);
  pst_audio_frames   = env_int ("PISTORM_AUDIO_FRAMES",    pst_audio_frames, 512, 8192);

  pst_mouse_thresh   = env_int ("PISTORM_MOUSE_THRESH",    pst_mouse_thresh, 0, 15);
  pst_mouse_scale    = env_int ("PISTORM_MOUSE_SCALE",     pst_mouse_scale, 1, 16);

  pst_stbox_slice_cyc = env_int("PISTORM_STBOX_SLICE_CYC", pst_stbox_slice_cyc, 8, 4096);
  pst_stbox_telemetry = env_bool("PISTORM_STBOX_DBG",      pst_stbox_telemetry);

  pst_ipl_confirm_ns = env_int ("PISTORM_IPL_CONFIRM_NS",  pst_ipl_confirm_ns, 0, 1000000);
  pst_vbl_refract_ns = env_int ("PISTORM_VBL_REFRACT_NS",  pst_vbl_refract_ns, 0, 200000000);
  psctrl_tunables_ipl_recalc();

  pst_verbose        = env_bool("PISTORM_VERBOSE",         pst_verbose);
  pst_dbg_ipl_stats  = env_bool("PISTORM_IPL_STATS",       pst_dbg_ipl_stats);
  pst_dbg_irq_stats  = env_bool("PISTORM_IRQ_STATS",       pst_dbg_irq_stats);
  pst_dbg_mfp        = env_bool("PISTORM_MFP_DEBUG",       pst_dbg_mfp);
  pst_dbg_mfp_hub    = env_bool("PISTORM_MFP_HUB_DEBUG",   pst_dbg_mfp_hub);
  pst_dbg_blit_trace = env_int ("PISTORM_BLIT_TRACE",      pst_dbg_blit_trace, 0, 1000000);
  pst_dbg_dmasnd     = env_int ("PISTORM_DMASND_DEBUG",    pst_dbg_dmasnd, 0, 2);
  pst_dbg_acsi       = env_bool("PISTORM_ACSI_DEBUG",      pst_dbg_acsi);
  pst_dbg_ide        = env_bool("PISTORM_IDE_DEBUG",       pst_dbg_ide);
  pst_dbg_gemdos     = env_bool("PISTORM_GEMDOS_DEBUG",    pst_dbg_gemdos);
  pst_dbg_stram      = env_bool("PISTORM_STRAM_DEBUG",     pst_dbg_stram);
  pst_dbg_net        = env_bool("PISTORM_NET_DEBUG",       pst_dbg_net);
  pst_dbg_ikbd       = env_bool("PISTORM_IKBD_DEBUG",      pst_dbg_ikbd);

  /* HOSTFS debug is three-valued: -1 means "whatever the cfg's own hostfs
   * debug flag says", which is how hostfs_debug_enabled() has always
   * behaved when the variable was unset. */
  if (getenv("PISTORM_HOSTFS_DEBUG"))
    pst_dbg_hostfs = env_bool("PISTORM_HOSTFS_DEBUG", 0);
}
