// SPDX-License-Identifier: MIT
//
// PSCTRL settings — the descriptor table and the six sub-ops behind it.
// See psctrl_settings.h for the wire format and the sub-op numbering.
//
// Three rules hold everything here together.
//
// 1. A LIVE setting is a plain aligned int the emulator re-reads where it
//    uses it. Setting one is a single 32-bit store, which is atomic on
//    AArch64, so nothing here takes a lock or makes a syscall and nothing
//    is added to core 3's admission budget.
//
// 2. A DEFER setting is anything that reallocates or invalidates the
//    translation cache. It CANNOT be applied inside this handler: the
//    caller is executing inside a translated block, and alloc_cache()
//    would vm_release() the code it is about to return into. So the
//    handler parks the request, calls jit_request_cpu_exit(), and returns
//    PS_R_DEFER; m68k_run_jit() drains it at the next block boundary with
//    jit_in_compiled_code false. This is the same nesting level at which
//    the existing T0/T1/M trace path already calls flush_icache(3).
//
// 3. A BOOT setting is the machine's shape. Nothing pretends it changed
//    under the guest: the value goes into a shadow copy of the config,
//    the item reads back the shadow so the dialog shows what you typed,
//    PS_SAVE writes it to the .cfg, and the class badge says "restart".

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "events.h"

#include "platforms/atari/psctrl/psctrl.h"
#include "platforms/atari/psctrl/psctrl_settings.h"
#include "platforms/atari/psctrl/psctrl_tunables.h"
#include "config_file/config_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

extern "C" {
#include "platforms/atari/fdd/atari_fdd.h"
}

/* Provided by atari_natfeat.cpp: the one place that knows how to reach
 * guest memory. Keeping it there means this file never includes the
 * memory banks and can be compiled on the host for tests. */
extern "C" void psctrl_guest_write(uint32_t addr, const void *src, uint32_t n);
extern "C" void psctrl_guest_read(uint32_t addr, char *dst, uint32_t n);

/* jit_glue.cpp */
extern "C" void jit_request_cpu_exit(void);
/* emulator.c: orderly teardown then _exit(restart ? 42 : 0) */
extern "C" void pistorm_request_exit(int restart);
/* compemu_support_arm.cpp, added alongside the psctrl_jit_* readers */
extern "C" void psctrl_jit_flush_now(void);
/* newcpu.cpp */
extern bool check_prefs_changed_comp(bool checkonly);

/* ------------------------------------------------------------------ */
/* the boot shadow                                                     */
/* ------------------------------------------------------------------ */
//
// Boot-class values are edited here, never in the running config. The
// shadow is seeded once from the live config; from then on the dialog
// reads the shadow, so a value you set stays set on screen even though
// the machine is still running the old one.

static struct emulator_config g_boot;
static int g_boot_ready = 0;
static int g_boot_dirty = 0;

static void boot_seed(void)
{
  const struct emulator_config *cfg;

  if (g_boot_ready)
    return;
  cfg = emulator_config_current();
  if (cfg)
    memcpy(&g_boot, cfg, sizeof(g_boot));
  else
    memset(&g_boot, 0, sizeof(g_boot));
  g_boot_ready = 1;
}

/* ------------------------------------------------------------------ */
/* deferred queue                                                      */
/* ------------------------------------------------------------------ */

volatile int psctrl_pending_armed = 0;

#define PEND_MAX 24
static volatile int g_pend_idx[PEND_MAX];
static volatile int g_pend_val[PEND_MAX];
static volatile int g_pend_n = 0;

static int defer(int idx, int value)
{
  int n = g_pend_n;

  if (n >= PEND_MAX)
    return PS_R_REJECT;            /* the dialog cannot outrun this */
  g_pend_idx[n] = idx;
  g_pend_val[n] = value;
  g_pend_n = n + 1;
  psctrl_pending_armed = 1;
  jit_request_cpu_exit();
  return PS_R_DEFER;
}

/* ------------------------------------------------------------------ */
/* the item table                                                      */
/* ------------------------------------------------------------------ */

struct ps_item;
typedef int  (*ps_get_fn)(const struct ps_item *it);
typedef int  (*ps_set_fn)(const struct ps_item *it, int v);

struct ps_item {
  const char      *name;      /* the .cfg key, and the wire name       */
  const char      *title;     /* what the dialog prints                */
  uint8_t          tab;
  uint8_t          kind;
  uint8_t          klass;
  uint8_t          unit;
  int32_t          min, max, step;
  const char      *labels;    /* "a\0b\0c\0", NULL if not an enum      */
  uint8_t          nenum;
  uint16_t         flags;
  volatile int    *tgt;       /* the generic live target, or NULL      */
  ps_get_fn        get;
  ps_set_fn        set;
  void           (*apply)(void);
};

/* index of an item, for the deferred queue and the switch()es below */
static int item_index(const char *name);

/* --- generic --------------------------------------------------------- */

static int gen_get(const struct ps_item *it)
{
  return it->tgt ? *it->tgt : 0;
}

static int gen_set(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max)
    return PS_R_REJECT;
  if (it->tgt)
    *it->tgt = v;
  if (it->apply)
    it->apply();
  return PS_R_OK;
}

/* --- boot-class fields in the shadow ---------------------------------- */

#define BOOT_BOOL(field)                                                     \
  static int bg_##field(const struct ps_item *it)                            \
  { (void)it; boot_seed(); return g_boot.field ? 1 : 0; }                    \
  static int bs_##field(const struct ps_item *it, int v)                     \
  { (void)it; boot_seed(); g_boot.field = v ? true : false;                  \
    g_boot_dirty = 1; return PS_R_RESTART; }

#define BOOT_INT(field)                                                      \
  static int bg_##field(const struct ps_item *it)                            \
  { (void)it; boot_seed(); return (int)g_boot.field; }                       \
  static int bs_##field(const struct ps_item *it, int v)                     \
  { boot_seed();                                                             \
    if (v < it->min || v > it->max) return PS_R_REJECT;                      \
    g_boot.field = v; g_boot_dirty = 1; return PS_R_RESTART; }

BOOT_BOOL(fpu)
BOOT_BOOL(mmu)
BOOT_BOOL(cpu_compatible)
BOOT_BOOL(addr32)
BOOT_BOOL(stram_cache)
BOOT_BOOL(stram_direct)
BOOT_BOOL(native_hdmi)
BOOT_BOOL(ym2149)
BOOT_BOOL(dma_sound)
/* shifter: the writer only renders it when the cfg set it explicitly, so
 * a change from the dialog has to count as explicit */
static int bg_shifter_ste(const struct ps_item *it)
{ (void)it; boot_seed(); return g_boot.shifter_ste ? 1 : 0; }
static int bs_shifter_ste(const struct ps_item *it, int v)
{ (void)it; boot_seed(); g_boot.shifter_ste = v ? true : false;
  g_boot.shifter_set = true; g_boot_dirty = 1; return PS_R_RESTART; }
BOOT_BOOL(network_enabled)
BOOT_BOOL(ide)
BOOT_INT(monitor_force)
BOOT_INT(machine_kind)
BOOT_INT(kbd_mouse_div)
BOOT_INT(stbox_plane)

/*
 * cpu_type is ALREADY the M68K_CPU_TYPE_* enum minus one - the parser
 * stores `get_m68k_cpu_type(...) - 1`, so 68000 is 0 and 68040 is 4 -
 * which is exactly the 0..5 index the dialog wants over
 * {68000,68010,68020,68030,68040,68060}. Subtracting one again here made
 * the CPU row read one model low: a machine configured 68040 showed
 * 68030. (config_file_save.c's cpu_name() takes cpu_type + 1 because IT
 * indexes the enum-shaped cpu_types[] table, and that was right.)
 */
static int bg_cpu(const struct ps_item *it)
{
  (void)it;
  boot_seed();
  return (int)g_boot.cpu_type;
}

static int bs_cpu(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max)
    return PS_R_REJECT;
  boot_seed();
  g_boot.cpu_type = (uint8_t)v;
  g_boot_dirty = 1;
  return PS_R_RESTART;
}

/* blitter: 0 off, 1 real chip, 2 emulated - two bools in the config */
static int bg_blitter(const struct ps_item *it)
{
  (void)it;
  boot_seed();
  if (!g_boot.blitter)
    return 0;
  return g_boot.blitter_real ? 1 : 2;
}

static int bs_blitter(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max)
    return PS_R_REJECT;
  boot_seed();
  g_boot.blitter      = (v != 0);
  g_boot.blitter_real = (v == 1);
  g_boot.blitter_set  = true;
  g_boot_dirty = 1;
  return PS_R_RESTART;
}

/* kbd: 0 disabled, 1 usb auto, 2 usb merge, 3 usb standalone */
static int bg_kbd(const struct ps_item *it)
{
  (void)it;
  boot_seed();
  if (!g_boot.kbd_usb)
    return 0;
  return g_boot.kbd_mode + 1;
}

static int bs_kbd(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max)
    return PS_R_REJECT;
  boot_seed();
  g_boot.kbd_usb  = (v != 0);
  g_boot.kbd_mode = (v > 0) ? (v - 1) : 0;
  g_boot_dirty = 1;
  return PS_R_RESTART;
}

/* nograb is the inverse of kbd_grab, because that is how the cfg reads */
static int bg_nograb(const struct ps_item *it)
{
  (void)it;
  boot_seed();
  return g_boot.kbd_grab ? 0 : 1;
}

static int bs_nograb(const struct ps_item *it, int v)
{
  (void)it;
  boot_seed();
  g_boot.kbd_grab = v ? false : true;
  g_boot_dirty = 1;
  return PS_R_RESTART;
}

/* ST-RAM and TT-RAM as an index into a size ladder */
static const int stram_kb[] = { 0, 512, 1024, 2048, 4096, 8192, 14336 };
static const int ttram_mb[] = { 0, 16, 32, 64, 128, 256 };

static int bg_stram(const struct ps_item *it)
{
  unsigned i;

  (void)it;
  boot_seed();
  for (i = 0; i < sizeof(stram_kb) / sizeof(stram_kb[0]); i++)
    if ((uint32_t)stram_kb[i] * 1024u == g_boot.stram_size)
      return (int)i;
  return 0;
}

static int bs_stram(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max)
    return PS_R_REJECT;
  boot_seed();
  g_boot.stram_size = (uint32_t)stram_kb[v] * 1024u;
  g_boot_dirty = 1;
  return PS_R_RESTART;
}

static int bg_ttram_size(const struct ps_item *it)
{
  unsigned i;
  uint32_t mb;

  (void)it;
  boot_seed();
  mb = g_boot.ttram ? g_boot.ttram_size / (1024u * 1024u) : 0;
  for (i = 0; i < sizeof(ttram_mb) / sizeof(ttram_mb[0]); i++)
    if ((uint32_t)ttram_mb[i] == mb)
      return (int)i;
  return 0;
}

static int bs_ttram_size(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max)
    return PS_R_REJECT;
  boot_seed();
  g_boot.ttram      = (ttram_mb[v] != 0);
  g_boot.ttram_size = (uint32_t)ttram_mb[v] * 1024u * 1024u;
  if (g_boot.ttram)
    g_boot.addr32 = true;            /* the parser forces this too */
  g_boot_dirty = 1;
  return PS_R_RESTART;
}

/* --- JIT -------------------------------------------------------------- */

/*
 * jit_power. 1..6 is the compiled-chain budget on a log2 ladder
 * (256 << (n-1), so 3 == the 1024 default), and it is a plain store to
 * pissoff_value - the knob worth A/B-ing against a running game. 0 means
 * JIT off, which is cachesize = 0, which is alloc_cache(), which is
 * deferred. One control, two classes; the dialog is told LIVE and the
 * handler upgrades the one value that needs it.
 */
static int power_of_mult(int mult)
{
  int n = 1, m = 256;

  while (m < mult && n < 6) { m <<= 1; n++; }
  return n;
}

static int jg_power(const struct ps_item *it)
{
  (void)it;
  if (!currprefs.cachesize)
    return 0;
  return power_of_mult(pst_pissoff_mult);
}

static int js_power(const struct ps_item *it, int v)
{
  (void)it;
  if (v < 0 || v > 6)
    return PS_R_REJECT;
  if (v == 0)
    return defer(item_index("jit_power"), 0);
  if (!currprefs.cachesize)
    return defer(item_index("jit_power"), v);   /* turning it back on */
  pst_pissoff_mult = 256 << (v - 1);
  pissoff_value = pst_pissoff_mult * CYCLE_UNIT;
  return PS_R_OK;
}

static int jg_speed(const struct ps_item *it)  { (void)it; return currprefs.m68k_speed; }
static int js_speed(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max)
    return PS_R_REJECT;
  currprefs.m68k_speed = changed_prefs.m68k_speed = v;
  boot_seed();                     /* the .cfg writer renders m68k_speed from here */
  g_boot.m68k_speed = v;
  g_boot.m68k_speed_set = true;
  return PS_R_OK;
}

/*
 * The clock multiplier is a SLOWDOWN, and 0 is off.
 *
 * update_68k_cycles() reads it as: 0 leaves cpucycleunit at its default
 * and the CPU is not clock-limited at all; 1..255 make cpucycleunit
 * CYCLE_UNIT * n, which is n times SLOWER; 256 and up are a fixed-point
 * divider (n >> 8), so 512 would be twice as fast - a range this dialog
 * does not offer and the emulator does not use. jit_cpu_init() also
 * clamps a negative value to 0, so -1 is not a faster setting, it is 0
 * spelt differently; and it turns m68k_speed=max into timed scheduling
 * the moment it is non-zero. So 0 is the fastest setting and it must be
 * reachable - the row shipped with min 1, which put the default out of
 * range of its own slider.
 *
 * Writing currprefs directly did nothing useful either: cpucycleunit is
 * derived in update_68k_cycles(), which is not called for a poke. Set
 * changed_prefs and let the CPU loop apply it at a mode change, which is
 * what the deferred queue is for.
 */
static int jg_mult(const struct ps_item *it)   { (void)it; return changed_prefs.cpu_clock_multiplier; }
static int js_mult(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max)
    return PS_R_REJECT;
  boot_seed();                     /* the .cfg writer renders it from here */
  g_boot.cpu_clock_multiplier = v;
  g_boot.cpu_clock_multiplier_set = true;
  return defer(item_index("cpu_clock_multiplier"), v);
}

static const int cache_kb[] = { 0, 2048, 4096, 8192, 16384 };

static int jg_cache(const struct ps_item *it)
{
  unsigned i;

  (void)it;
  for (i = 0; i < sizeof(cache_kb) / sizeof(cache_kb[0]); i++)
    if (cache_kb[i] == currprefs.cachesize)
      return (int)i;
  return 0;
}

static int js_cache(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max)
    return PS_R_REJECT;
  boot_seed();                     /* the .cfg writer renders jit_cache from here */
  g_boot.jit_cache = cache_kb[v];
  g_boot.jit_cache_set = true;
  return defer(item_index("jit_cache"), v);
}

static int jg_constjump(const struct ps_item *it) { (void)it; return currprefs.comp_constjump ? 1 : 0; }
static int jg_compnf(const struct ps_item *it)    { (void)it; return currprefs.compnf ? 1 : 0; }
static int jg_compfpu(const struct ps_item *it)   { (void)it; return currprefs.compfpu ? 1 : 0; }

/* The tunable is what the .cfg reads and writes; currprefs is what runs.
 * While the .cfg is being loaded there is no CPU to defer to - jit_glue
 * seeds currprefs from the tunable when it starts. */
static int g_cfg_loading = 0;

static int js_constjump(const struct ps_item *it, int v)
{
  (void)it;
  pst_comp_constjump = !!v;
  return g_cfg_loading ? PS_R_OK : defer(item_index("comp_constjump"), !!v);
}
static int js_compnf(const struct ps_item *it, int v)
{
  (void)it;
  pst_compnf = !!v;
  return g_cfg_loading ? PS_R_OK : defer(item_index("compnf"), !!v);
}
static int js_compfpu(const struct ps_item *it, int v)
{
  (void)it;
  pst_compfpu = !!v;
  return g_cfg_loading ? PS_R_OK : defer(item_index("compfpu"), !!v);
}

static int jg_zero(const struct ps_item *it) { (void)it; return 0; }
static int js_flush(const struct ps_item *it, int v)
{
  (void)it; (void)v;
  return defer(item_index("jit_flush"), 1);
}

/* read-only readouts, so the JIT tab is a diagnostic and not just a form */
static int ig_temp(const struct ps_item *it)  { (void)it; return (int)(psctrl_getint(PS_HOST_SOC_TEMP_MC) / 1000u); }
/* what the emulator is ACTUALLY running, as against what the cfg asks
 * for - they differ whenever a cfg change has not been restarted into */
static int ig_cpu_run(const struct ps_item *it) { (void)it; return (int)psctrl_getint(PS_CFG_CPU_MODEL); }
static int ig_fpu_run(const struct ps_item *it) { (void)it; return (int)psctrl_getint(PS_CFG_FPU_MODEL); }
static int ig_thr(const struct ps_item *it)   { (void)it; return (int)psctrl_getint(PS_HOST_THROTTLED); }

/* --- live tunables that need a poke after the store -------------------- */

extern "C" void ym2149_settings_changed(void);   /* patched in, re-applies gain/lag */
extern "C" void kbd_usb_mouse_cfg_changed(void); /* patched in, re-arms IKBD 0x0B   */

static void apply_ym(void)    { ym2149_settings_changed(); }
static void apply_mouse(void) { kbd_usb_mouse_cfg_changed(); }
static void apply_ipl(void)   { psctrl_tunables_ipl_recalc(); }

/* --- floppy ------------------------------------------------------------ */
//
// The plumbing (fdd_insert_disk / _eject_disk / _set_write_protect) has
// always been there; what was missing is a way to see the drive from
// outside atari_fdd.c and a media-change the guest notices. Both arrive
// with this work: fdd_query() reports the drive, and fdd_pulse_media()
// arms the write-protect flicker that makes GEMDOS Mediach() answer 2.

extern "C" int  fdd_query(int drive, char *path, int pathlen, int *wp, int *busy);
extern "C" void fdd_pulse_media(int drive);

static char g_fddir[PATH_MAX] = "";

static void fddir_default(void)
{
  const struct emulator_config *cfg;

  if (g_fddir[0])
    return;
  cfg = emulator_config_current();
  if (cfg && cfg->fdd.img_path[0]) {
    const char *slash;

    snprintf(g_fddir, sizeof(g_fddir), "%s", cfg->fdd.img_path);
    slash = strrchr(g_fddir, '/');
    if (slash)
      g_fddir[slash - g_fddir] = '\0';
    else
      g_fddir[0] = '\0';
  }
  if (!g_fddir[0])
    snprintf(g_fddir, sizeof(g_fddir), "%s", "floppies");
}

static int fg_wp_a(const struct ps_item *it)
{
  int wp = 0, busy = 0;

  (void)it;
  fdd_query(0, NULL, 0, &wp, &busy);
  return wp;
}

static int fg_wp_b(const struct ps_item *it)
{
  int wp = 0, busy = 0;

  (void)it;
  fdd_query(1, NULL, 0, &wp, &busy);
  return wp;
}

static int fs_wp_a(const struct ps_item *it, int v) { (void)it; fdd_set_write_protect(0, v != 0); return PS_R_OK; }
static int fs_wp_b(const struct ps_item *it, int v) { (void)it; fdd_set_write_protect(1, v != 0); return PS_R_OK; }

/* --- ST Box ------------------------------------------------------------ */

extern "C" int stbox_running(void);

static int sg_running(const struct ps_item *it) { (void)it; return stbox_running() ? 1 : 0; }

/* ------------------------------------------------------------------ */
/* the table                                                           */
/* ------------------------------------------------------------------ */

#define L_OFFON      "off\0on\0"
#define L_NOYES      "no\0yes\0"

static const char L_machine[]  = "st\0ste\0megast\0";
static const char L_cpu[]      = "68000\0" "68010\0" "68020\0" "68030\0" "68040\0" "68060\0";
static const char L_blitter[]  = "off\0real chip\0emulated\0";
static const char L_shifter[]  = "st\0ste\0";
static const char L_monitor[]  = "auto\0mono\0colour\0";
static const char L_stram[]    = "flat 4M\0" "512K\0" "1M\0" "2M\0" "4M\0" "8M\0" "14M\0";
static const char L_ttram[]    = "off\0" "16M\0" "32M\0" "64M\0" "128M\0" "256M\0";
static const char L_cache[]    = "off\0" "2048K\0" "4096K\0" "8192K\0" "16384K\0";
static const char L_kbd[]      = "disabled\0usb auto\0usb merge\0usb standalone\0";
static const char L_frames[]   = "512\0" "1024\0" "2048\0" "4096\0" "8192\0";
static const char L_dmasnd[]   = "off\0verbose\0summary\0";
static const char L_hostfsd[]  = "follow cfg\0off\0on\0";
static const char L_card[]     = "none\0ET4000AX\0ATI\0Matrox\0";
static const char L_driver[]   = "none\0NOVA\0XVDI\0NVDI\0fVDI\0";

/* audio_frames is a ladder like the RAM sizes */
static const int frames_v[] = { 512, 1024, 2048, 4096, 8192 };

static int ag_frames(const struct ps_item *it)
{
  unsigned i;

  (void)it;
  for (i = 0; i < sizeof(frames_v) / sizeof(frames_v[0]); i++)
    if (frames_v[i] == pst_audio_frames)
      return (int)i;
  return 2;
}

static int as_frames(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max)
    return PS_R_REJECT;
  pst_audio_frames = frames_v[v];
  return PS_R_RESTART;             /* SDL reads it at device open */
}

static int vg_card(const struct ps_item *it)   { (void)it; boot_seed(); return (int)g_boot.graphics.card; }
static int vs_card(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max) return PS_R_REJECT;
  boot_seed(); g_boot.graphics.card = (graphics_card)v; g_boot_dirty = 1;
  return PS_R_RESTART;
}
static int vg_drv(const struct ps_item *it)    { (void)it; boot_seed(); return (int)g_boot.graphics.driver; }
static int vs_drv(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max) return PS_R_REJECT;
  boot_seed(); g_boot.graphics.driver = (graphics_driver)v; g_boot_dirty = 1;
  return PS_R_RESTART;
}

static int fg_fps(const struct ps_item *it)
{
  (void)it;
  return pst_fps ? pst_fps : emulator_config_fps();
}

static int fs_fps(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max)
    return PS_R_REJECT;
  pst_fps = v;                     /* the render loop re-reads it */
  /* `fps` is also a boot key, and the .cfg writer renders boot keys from
   * the boot shadow - so the live value has to land there too, or a save
   * writes the fps the machine STARTED with and the change is lost on
   * the next boot. */
  boot_seed();
  g_boot.fps = v;
  g_boot_dirty = 1;
  return PS_R_OK;
}

static int ng_irq(const struct ps_item *it)  { (void)it; boot_seed(); return g_boot.network_irq_level; }
static int ns_irq(const struct ps_item *it, int v)
{
  if (v < it->min || v > it->max) return PS_R_REJECT;
  boot_seed(); g_boot.network_irq_level = (uint8_t)v; g_boot_dirty = 1;
  return PS_R_RESTART;
}

#define ITEM(nm, ti, tab, kind, kls, unit, lo, hi, st, lab, ne, fl, tg, g, s, ap) \
  { nm, ti, tab, kind, kls, unit, lo, hi, st, lab, ne, fl, tg, g, s, ap }

#define LIVE_INT(nm, ti, tab, unit, lo, hi, st, tg, ap) \
  ITEM(nm, ti, tab, PS_K_INT, PS_C_LIVE, unit, lo, hi, st, NULL, 0, 0, tg, gen_get, gen_set, ap)

#define LIVE_BOOL(nm, ti, tab, tg, ap) \
  ITEM(nm, ti, tab, PS_K_BOOL, PS_C_LIVE, PS_U_NONE, 0, 1, 1, L_OFFON, 2, 0, tg, gen_get, gen_set, ap)

#define BOOT_B(nm, ti, tab, field) \
  ITEM(nm, ti, tab, PS_K_BOOL, PS_C_BOOT, PS_U_NONE, 0, 1, 1, L_OFFON, 2, 0, NULL, bg_##field, bs_##field, NULL)

#define RO_INT(nm, ti, tab, unit, g) \
  ITEM(nm, ti, tab, PS_K_INFO, PS_C_RO, unit, 0, 0, 0, NULL, 0, 0, NULL, g, NULL, NULL)

static const struct ps_item g_items[] = {

/* ------------------------------------------------------------- JIT --- */
ITEM("jit_power", "JIT power", PS_TAB_JIT, PS_K_INT, PS_C_LIVE, PS_U_NONE,
     0, 6, 1, NULL, 0, 0, NULL, jg_power, js_power, NULL),
ITEM("m68k_speed", "68k speed", PS_TAB_JIT, PS_K_INT, PS_C_LIVE, PS_U_NONE,
     -1, 20, 1, NULL, 0, 0, NULL, jg_speed, js_speed, NULL),
ITEM("cpu_clock_multiplier", "CPU slowdown", PS_TAB_JIT, PS_K_INT,
     PS_C_DEFER, PS_U_NONE, 0, 8, 1, NULL, 0, 0, NULL, jg_mult, js_mult, NULL),
ITEM("jit_cache", "Translation cache", PS_TAB_JIT, PS_K_ENUM, PS_C_DEFER,
     PS_U_NONE, 0, 4, 1, L_cache, 5, PS_F_NEWLINE, NULL, jg_cache, js_cache, NULL),
/* tgt makes these .cfg keys (`compnf 0`); the value shown is currprefs */
ITEM("comp_constjump", "Follow constant jumps", PS_TAB_JIT, PS_K_BOOL,
     PS_C_DEFER, PS_U_NONE, 0, 1, 1, L_OFFON, 2, 0, &pst_comp_constjump, jg_constjump, js_constjump, NULL),
ITEM("compnf", "Skip dead flags", PS_TAB_JIT, PS_K_BOOL, PS_C_DEFER,
     PS_U_NONE, 0, 1, 1, L_OFFON, 2, 0, &pst_compnf, jg_compnf, js_compnf, NULL),
ITEM("compfpu", "Translate FPU", PS_TAB_JIT, PS_K_BOOL, PS_C_DEFER,
     PS_U_NONE, 0, 1, 1, L_OFFON, 2, 0, &pst_compfpu, jg_compfpu, js_compfpu, NULL),
ITEM("jit_flush", "Flush cache now", PS_TAB_JIT, PS_K_ACTION, PS_C_DEFER,
     PS_U_NONE, 0, 0, 0, NULL, 0, PS_F_NEWLINE, NULL, jg_zero, js_flush, NULL),

/* --------------------------------------------------------- CPU/RAM --- */
ITEM("machine", "Machine", PS_TAB_CPU, PS_K_ENUM, PS_C_BOOT, PS_U_NONE,
     0, 2, 1, L_machine, 3, 0, NULL, bg_machine_kind, bs_machine_kind, NULL),
ITEM("cpu", "CPU", PS_TAB_CPU, PS_K_ENUM, PS_C_BOOT, PS_U_NONE,
     0, 5, 1, L_cpu, 6, 0, NULL, bg_cpu, bs_cpu, NULL),
BOOT_B("fpu", "FPU", PS_TAB_CPU, fpu),
BOOT_B("mmu", "MMU", PS_TAB_CPU, mmu),
BOOT_B("cpu_compatible", "Prefetch-accurate 68000", PS_TAB_CPU, cpu_compatible),
ITEM("shifter", "Shifter", PS_TAB_CPU, PS_K_ENUM, PS_C_BOOT, PS_U_NONE,
     0, 1, 1, L_shifter, 2, 0, NULL, bg_shifter_ste, bs_shifter_ste, NULL),
ITEM("blitter", "Blitter", PS_TAB_CPU, PS_K_ENUM, PS_C_BOOT, PS_U_NONE,
     0, 2, 1, L_blitter, 3, 0, NULL, bg_blitter, bs_blitter, NULL),
ITEM("stram_size", "ST-RAM", PS_TAB_CPU, PS_K_ENUM, PS_C_BOOT, PS_U_NONE,
     0, 6, 1, L_stram, 7, PS_F_NEWLINE, NULL, bg_stram, bs_stram, NULL),
ITEM("ttram", "TT-RAM", PS_TAB_CPU, PS_K_ENUM, PS_C_BOOT, PS_U_NONE,
     0, 5, 1, L_ttram, 6, 0, NULL, bg_ttram_size, bs_ttram_size, NULL),
BOOT_B("addr32", "32-bit addressing", PS_TAB_CPU, addr32),
BOOT_B("stram_cache", "ST-RAM cache", PS_TAB_CPU, stram_cache),
BOOT_B("stram_direct", "ST-RAM direct", PS_TAB_CPU, stram_direct),
ITEM("monitor", "Monitor detect", PS_TAB_CPU, PS_K_ENUM, PS_C_BOOT, PS_U_NONE,
     0, 2, 1, L_monitor, 3, PS_F_NEWLINE, NULL, bg_monitor_force, bs_monitor_force, NULL),
ITEM("rom", "TOS image", PS_TAB_CPU, PS_K_STR, PS_C_BOOT, PS_U_NONE,
     0, 0, 0, NULL, 0, 0, NULL, jg_zero, NULL, NULL),
/* live, and it lives here because it is a bus-timing knob, not a video one */
LIVE_INT("blit_timed_ns", "Blitter bus cost (0 = instant)", PS_TAB_CPU,
         PS_U_NS, 0, 2000, 50, &pst_blit_timed_ns, NULL),
RO_INT("cpu_running", "CPU now running", PS_TAB_CPU, PS_U_NONE, ig_cpu_run),
RO_INT("fpu_running", "FPU now running", PS_TAB_CPU, PS_U_NONE, ig_fpu_run),
RO_INT("soc_temp", "SoC temperature", PS_TAB_CPU, PS_U_NONE, ig_temp),
RO_INT("throttled", "Firmware throttle bits", PS_TAB_CPU, PS_U_NONE, ig_thr),

/* ----------------------------------------------------------- Video --- */
ITEM("fps", "Host frame rate", PS_TAB_VIDEO, PS_K_INT, PS_C_LIVE, PS_U_HZ,
     10, 60, 1, NULL, 0, 0, NULL, fg_fps, fs_fps, NULL),
/* Range 0..20 ms in 0.1 ms steps. The old max of 200000 ns (0.2 ms) was
 * below the tunable's own 5 ms default, so the slider could not represent
 * the live value: it showed 5.0 ms on entry and then snapped into the
 * 0..0.2 ms band the moment it was touched. 20 ms is one PAL frame, which
 * is as long as a VBL refractory could sensibly be; the env clamp already
 * allows up to 200 ms for anyone who wants more. */
LIVE_INT("vbl_refract_ns", "VBL refractory", PS_TAB_VIDEO, PS_U_NS,
         0, 20000000, 100000, &pst_vbl_refract_ns, apply_ipl),
LIVE_BOOL("drm_dirtyband", "DRM dirty band", PS_TAB_VIDEO, &pst_drm_dirtyband, NULL),
ITEM("drm_async", "DRM async page flip", PS_TAB_VIDEO, PS_K_BOOL, PS_C_BOOT,
     PS_U_NONE, 0, 1, 1, L_OFFON, 2, 0, &pst_drm_async, gen_get, gen_set, NULL),
BOOT_B("native_hdmi", "Native HDMI", PS_TAB_VIDEO, native_hdmi),
ITEM("vga_card", "Graphics card", PS_TAB_VIDEO, PS_K_ENUM, PS_C_BOOT,
     PS_U_NONE, 0, 3, 1, L_card, 4, PS_F_NEWLINE, NULL, vg_card, vs_card, NULL),
ITEM("vga_driver", "Graphics driver", PS_TAB_VIDEO, PS_K_ENUM, PS_C_BOOT,
     PS_U_NONE, 0, 4, 1, L_driver, 5, 0, NULL, vg_drv, vs_drv, NULL),

/* ----------------------------------------------------------- Audio --- */
BOOT_B("ym2149", "YM2149", PS_TAB_AUDIO, ym2149),
BOOT_B("dma_sound", "STE DMA sound", PS_TAB_AUDIO, dma_sound),
LIVE_INT("ym_gain", "YM gain", PS_TAB_AUDIO, PS_U_X100, 0, 400, 5,
         &pst_ym_gain_x100, apply_ym),
LIVE_INT("ym_lag_ms", "YM lag", PS_TAB_AUDIO, PS_U_MS, 5, 200, 5,
         &pst_ym_lag_ms, apply_ym),
LIVE_BOOL("lmc", "LMC1992 shadow", PS_TAB_AUDIO, &pst_lmc, NULL),
/* tgt is what makes the writer treat this as a .cfg key (`audio_frames
 * 2048`, through the labels); the dialog value still goes via ag/as */
ITEM("audio_frames", "SDL buffer", PS_TAB_AUDIO, PS_K_ENUM, PS_C_BOOT,
     PS_U_NONE, 0, 4, 1, L_frames, 5, 0, &pst_audio_frames, ag_frames, as_frames, NULL),

/* ----------------------------------------------------------- Input --- */
ITEM("kbd", "Keyboard source", PS_TAB_INPUT, PS_K_ENUM, PS_C_BOOT, PS_U_NONE,
     0, 3, 1, L_kbd, 4, 0, NULL, bg_kbd, bs_kbd, NULL),
ITEM("kbd_nograb", "Leave devices to the Pi console", PS_TAB_INPUT, PS_K_BOOL,
     PS_C_BOOT, PS_U_NONE, 0, 1, 1, L_NOYES, 2, 0, NULL, bg_nograb, bs_nograb, NULL),
ITEM("mousediv", "Host mouse divisor", PS_TAB_INPUT, PS_K_INT, PS_C_BOOT,
     PS_U_NONE, 1, 16, 1, NULL, 0, 0, NULL, bg_kbd_mouse_div, bs_kbd_mouse_div, NULL),
LIVE_INT("mouse_thresh", "IKBD threshold (0 = off)", PS_TAB_INPUT, PS_U_NONE,
         0, 15, 1, &pst_mouse_thresh, apply_mouse),
LIVE_INT("mouse_scale", "Delta scale", PS_TAB_INPUT, PS_U_NONE, 1, 16, 1,
         &pst_mouse_scale, apply_mouse),

/* ---------------------------------------------------------- ST Box --- */
ITEM("stbox_tos", "Box TOS image", PS_TAB_STBOX, PS_K_STR, PS_C_BOXBOOT,
     PS_U_NONE, 0, 0, 0, NULL, 0, 0, NULL, jg_zero, NULL, NULL),
ITEM("stbox_plane", "DRM plane (0 = auto)", PS_TAB_STBOX, PS_K_INT,
     PS_C_BOXBOOT, PS_U_NONE, 0, 128, 1, NULL, 0, 0, NULL,
     bg_stbox_plane, bs_stbox_plane, NULL),
LIVE_INT("stbox_slice_cyc", "Admission slice", PS_TAB_STBOX, PS_U_CYC,
         8, 4096, 8, &pst_stbox_slice_cyc, NULL),
LIVE_BOOL("stbox_telemetry", "Telemetry", PS_TAB_STBOX, &pst_stbox_telemetry, NULL),
RO_INT("stbox_running", "Box running", PS_TAB_STBOX, PS_U_NONE, sg_running),

/* ---------------------------------------------------------- Floppy --- */
ITEM("floppy_a", "Drive A:", PS_TAB_FLOPPY, PS_K_STR, PS_C_LIVE, PS_U_NONE,
     0, 0, 0, NULL, 0, 0, NULL, jg_zero, NULL, NULL),
ITEM("floppy_b", "Drive B:", PS_TAB_FLOPPY, PS_K_STR, PS_C_LIVE, PS_U_NONE,
     0, 0, 0, NULL, 0, 0, NULL, jg_zero, NULL, NULL),
ITEM("floppy_a_wp", "A: write protect", PS_TAB_FLOPPY, PS_K_BOOL, PS_C_LIVE,
     PS_U_NONE, 0, 1, 1, L_OFFON, 2, 0, NULL, fg_wp_a, fs_wp_a, NULL),
ITEM("floppy_b_wp", "B: write protect", PS_TAB_FLOPPY, PS_K_BOOL, PS_C_LIVE,
     PS_U_NONE, 0, 1, 1, L_OFFON, 2, 0, NULL, fg_wp_b, fs_wp_b, NULL),
ITEM("floppy_eject_a", "Eject A:", PS_TAB_FLOPPY, PS_K_ACTION, PS_C_LIVE,
     PS_U_NONE, 0, 0, 0, NULL, 0, PS_F_NEWLINE, NULL, jg_zero, NULL, NULL),
ITEM("floppy_eject_b", "Eject B:", PS_TAB_FLOPPY, PS_K_ACTION, PS_C_LIVE,
     PS_U_NONE, 0, 0, 0, NULL, 0, 0, NULL, jg_zero, NULL, NULL),
ITEM("floppy_swap", "Swap A: <-> B:", PS_TAB_FLOPPY, PS_K_ACTION, PS_C_LIVE,
     PS_U_NONE, 0, 0, 0, NULL, 0, 0, NULL, jg_zero, NULL, NULL),

/* --------------------------------------------------------- Network --- */
BOOT_B("network", "Network", PS_TAB_NET, network_enabled),
ITEM("network_irq", "IRQ level", PS_TAB_NET, PS_K_INT, PS_C_BOOT, PS_U_NONE,
     1, 7, 1, NULL, 0, 0, NULL, ng_irq, ns_irq, NULL),
BOOT_B("ide", "IDE", PS_TAB_NET, ide),
LIVE_BOOL("network_debug", "Network trace", PS_TAB_NET, &pst_dbg_net, NULL),

/* ----------------------------------------------------------- Debug --- */
LIVE_INT("ipl_confirm_ns", "IPL confirm window", PS_TAB_DEBUG, PS_U_NS,
         0, 10000, 100, &pst_ipl_confirm_ns, apply_ipl),
LIVE_BOOL("verbose", "Verbose console", PS_TAB_DEBUG, &pst_verbose, NULL),
LIVE_BOOL("ipl_stats", "IPL statistics", PS_TAB_DEBUG, &pst_dbg_ipl_stats, NULL),
LIVE_BOOL("irq_stats", "IRQ statistics", PS_TAB_DEBUG, &pst_dbg_irq_stats, NULL),
LIVE_BOOL("mfp_debug", "MFP trace", PS_TAB_DEBUG, &pst_dbg_mfp, NULL),
LIVE_BOOL("mfp_hub_debug", "MFP hub trace", PS_TAB_DEBUG, &pst_dbg_mfp_hub, NULL),
LIVE_BOOL("fdd_mfp_irq", "FDC MFP interrupt", PS_TAB_DEBUG, &pst_fdd_mfp_irq, NULL),
LIVE_INT("blit_trace", "Blits to trace", PS_TAB_DEBUG, PS_U_NONE,
         0, 10000, 10, &pst_dbg_blit_trace, NULL),
ITEM("dmasnd_debug", "DMA sound trace", PS_TAB_DEBUG, PS_K_ENUM, PS_C_LIVE,
     PS_U_NONE, 0, 2, 1, L_dmasnd, 3, 0, &pst_dbg_dmasnd, gen_get, gen_set, NULL),
LIVE_BOOL("acsi_debug", "ACSI trace", PS_TAB_DEBUG, &pst_dbg_acsi, NULL),
LIVE_BOOL("ide_debug", "IDE trace", PS_TAB_DEBUG, &pst_dbg_ide, NULL),
ITEM("hostfs_debug", "HOSTFS trace", PS_TAB_DEBUG, PS_K_ENUM, PS_C_LIVE,
     PS_U_NONE, 0, 2, 1, L_hostfsd, 3, 0, NULL, NULL, NULL, NULL),
LIVE_BOOL("gemdos_debug", "GEMDOS trace", PS_TAB_DEBUG, &pst_dbg_gemdos, NULL),
LIVE_BOOL("stram_debug", "ST-RAM trace", PS_TAB_DEBUG, &pst_dbg_stram, NULL),

/* -------------------------------------------------------- Advanced --- */
ITEM("save_cfg", "Save to .cfg", PS_TAB_ADV, PS_K_ACTION, PS_C_LIVE,
     PS_U_NONE, 0, 0, 0, NULL, 0, 0, NULL, jg_zero, NULL, NULL),
RO_INT("cfg_dirty", "Unsaved boot changes", PS_TAB_ADV, PS_U_NONE, NULL)
};

#define NITEMS ((int)(sizeof(g_items) / sizeof(g_items[0])))

/* hostfs_debug is three-valued and lives on -1/0/1, so it gets its own
 * pair rather than a target; wired up here to keep the table readable */
static int hg_hostfs(const struct ps_item *it)
{
  (void)it;
  return pst_dbg_hostfs < 0 ? 0 : (pst_dbg_hostfs ? 2 : 1);
}

static int hs_hostfs(const struct ps_item *it, int v)
{
  (void)it;
  if (v < 0 || v > 2)
    return PS_R_REJECT;
  pst_dbg_hostfs = (v == 0) ? -1 : (v == 2);
  return PS_R_OK;
}

static int cg_dirty(const struct ps_item *it) { (void)it; return g_boot_dirty; }

/* The two entries above are declared with NULL accessors in the table so
 * the table stays one screen per tab; they are filled in on first use. */
static struct ps_item g_live[NITEMS];
static int g_live_ready = 0;

static void table_init(void)
{
  int i;

  if (g_live_ready)
    return;
  memcpy(g_live, g_items, sizeof(g_live));
  for (i = 0; i < NITEMS; i++) {
    if (!strcmp(g_live[i].name, "hostfs_debug")) {
      g_live[i].get = hg_hostfs;
      g_live[i].set = hs_hostfs;
    } else if (!strcmp(g_live[i].name, "cfg_dirty")) {
      g_live[i].get = cg_dirty;
    }
  }
  g_live_ready = 1;
}

static int item_index(const char *name)
{
  int i;

  for (i = 0; i < NITEMS; i++)
    if (!strcmp(g_items[i].name, name))
      return i;
  return -1;
}

/* ------------------------------------------------------------------ */
/* deferred apply — runs at a JIT block boundary, outside compiled code */
/* ------------------------------------------------------------------ */

void psctrl_apply_pending(void)
{
  int n, i;

  if (!psctrl_pending_armed)
    return;

  n = g_pend_n;
  g_pend_n = 0;
  psctrl_pending_armed = 0;

  for (i = 0; i < n; i++) {
    int idx = g_pend_idx[i];
    int v   = g_pend_val[i];
    const char *nm = (idx >= 0 && idx < NITEMS) ? g_items[idx].name : "";

    if (!strcmp(nm, "jit_power")) {
      if (v == 0) {
        changed_prefs.cachesize = 0;
        check_prefs_changed_comp(false);
      } else {
        /* back on at the size the cfg asked for, then the budget */
        const struct emulator_config *cfg = emulator_config_current();
        int kb = (cfg && cfg->jit_cache_set && cfg->jit_cache > 0)
               ? cfg->jit_cache : 8192;

        changed_prefs.cachesize = kb;
        check_prefs_changed_comp(false);
        if (currprefs.cachesize) {
          pst_pissoff_mult = 256 << (v - 1);
          pissoff_value = pst_pissoff_mult * CYCLE_UNIT;
        }
      }
    } else if (!strcmp(nm, "jit_cache")) {
      changed_prefs.cachesize = cache_kb[v];
      check_prefs_changed_comp(false);
      /* pissoff_value is forced to 0 while the cache is off; restore it */
      pissoff_value = currprefs.cachesize
                    ? pst_pissoff_mult * CYCLE_UNIT : 0;
    } else if (!strcmp(nm, "comp_constjump")) {
      changed_prefs.comp_constjump = v ? true : false;
      check_prefs_changed_comp(false);
    } else if (!strcmp(nm, "compnf")) {
      changed_prefs.compnf = v ? true : false;
      check_prefs_changed_comp(false);
    } else if (!strcmp(nm, "compfpu")) {
      changed_prefs.compfpu = v ? true : false;
      check_prefs_changed_comp(false);
    } else if (!strcmp(nm, "cpu_clock_multiplier")) {
      /* changed_prefs already holds it; config_changed is what makes
       * check_prefs_changed_cpu() look, and the mode change is where
       * update_68k_cycles() re-derives cpucycleunit */
      changed_prefs.cpu_clock_multiplier = v;
      config_changed = 1;
      check_prefs_changed_cpu();
    } else if (!strcmp(nm, "jit_flush")) {
      psctrl_jit_flush_now();
    }
  }
}

/* ------------------------------------------------------------------ */
/* string values                                                       */
/* ------------------------------------------------------------------ */

static const char *item_str(const struct ps_item *it)
{
  static char buf[PATH_MAX];
  int wp = 0, busy = 0;

  if (!strcmp(it->name, "rom")) {
    boot_seed();
    return g_boot.rom.rom_path;
  }
  if (!strcmp(it->name, "stbox_tos")) {
    boot_seed();
    return g_boot.stbox_tos;
  }
  if (!strcmp(it->name, "floppy_a") || !strcmp(it->name, "floppy_b")) {
    int drive = it->name[7] == 'b';

    buf[0] = '\0';
    if (fdd_query(drive, buf, (int)sizeof(buf), &wp, &busy) <= 0)
      return "(empty)";
    return buf;
  }
  return "";
}

extern "C" int emulator_gemdos_to_host(const char *gem, char *out, size_t n);

/* A ROM/TOS field is now filled by the native file selector, which hands
 * back a GEMDOS path (S:\dir\file). Turn it into the real host path the
 * emulator will fopen, exactly as STBOX.PRG's TOS argument is mapped. A
 * bare filename or an absolute host path is left as-is - the loader still
 * resolves a bare name against rom_path. */
static const char *setstr_map(const char *s, char *buf, size_t n)
{
  if (s && emulator_gemdos_to_host(s, buf, n))
    return buf;
  return s;
}

static int item_setstr(const struct ps_item *it, const char *s)
{
  char mapped[PATH_MAX];

  if (!strcmp(it->name, "rom")) {
    s = setstr_map(s, mapped, sizeof mapped);
    boot_seed();
    snprintf(g_boot.rom.rom_path, sizeof(g_boot.rom.rom_path), "%s", s);
    g_boot_dirty = 1;
    return PS_R_RESTART;
  }
  if (!strcmp(it->name, "stbox_tos")) {
    s = setstr_map(s, mapped, sizeof mapped);
    boot_seed();
    snprintf(g_boot.stbox_tos, sizeof(g_boot.stbox_tos), "%s", s);
    g_boot_dirty = 1;
    return PS_R_BOXBOOT;
  }
  if (!strcmp(it->name, "floppy_a") || !strcmp(it->name, "floppy_b")) {
    int drive = it->name[7] == 'b';
    char path[PATH_MAX];
    int wp = 0, busy = 0;

    fdd_query(drive, NULL, 0, &wp, &busy);
    if (busy)
      return PS_R_BUSY;
    if (emulator_gemdos_to_host(s, path, sizeof(path))) {
      /* the file selector's Z:\game.st -> real host path */
    }
    else if (strchr(s, '/'))
      snprintf(path, sizeof(path), "%s", s);
    else {
      size_t dl, sl;

      fddir_default();
      dl = strlen(g_fddir);
      sl = strlen(s);
      if (dl + sl + 2 > sizeof(path))
        return PS_R_REJECT;
      memcpy(path, g_fddir, dl);
      path[dl] = '/';
      memcpy(path + dl + 1, s, sl + 1);
    }
    if (fdd_insert_disk(drive, path, false) != 0)
      return PS_R_REJECT;
    fdd_pulse_media(drive);
    return PS_R_OK;
  }
  return PS_R_REJECT;
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

extern "C" int config_file_save(const char *path, const struct emulator_config *cfg);
extern "C" const char *emulator_config_path(void);

static int do_action(const struct ps_item *it, int arg)
{
  if (!strcmp(it->name, "jit_flush"))
    return defer(item_index("jit_flush"), 1);

  if (!strcmp(it->name, "floppy_eject_a") || !strcmp(it->name, "floppy_eject_b")) {
    int drive = it->name[13] == 'b';
    int wp = 0, busy = 0;

    fdd_query(drive, NULL, 0, &wp, &busy);
    if (busy)
      return PS_R_BUSY;
    fdd_eject_disk(drive);
    fdd_pulse_media(drive);
    return PS_R_OK;
  }

  if (!strcmp(it->name, "floppy_swap")) {
    char a[PATH_MAX], b[PATH_MAX];
    int wpa = 0, wpb = 0, busy = 0, ha, hb;

    fdd_query(0, NULL, 0, &wpa, &busy);
    if (busy)
      return PS_R_BUSY;
    fdd_query(1, NULL, 0, &wpb, &busy);
    if (busy)
      return PS_R_BUSY;
    a[0] = b[0] = '\0';
    ha = fdd_query(0, a, (int)sizeof(a), &wpa, &busy) > 0;
    hb = fdd_query(1, b, (int)sizeof(b), &wpb, &busy) > 0;
    fdd_eject_disk(0);
    fdd_eject_disk(1);
    if (hb) fdd_insert_disk(0, b, wpb != 0);
    if (ha) fdd_insert_disk(1, a, wpa != 0);
    fdd_pulse_media(0);
    fdd_pulse_media(1);
    return PS_R_OK;
  }

  if (!strcmp(it->name, "save_cfg")) {
    const char *path = emulator_config_path();

    boot_seed();
    if (!path || !*path)
      return PS_R_REJECT;
    if (config_file_save(path, &g_boot) != 0)
      return PS_R_REJECT;
    g_boot_dirty = 0;
    return PS_R_OK;
  }

  (void)arg;
  return PS_R_REJECT;
}

/* ------------------------------------------------------------------ */
/* the sub-op entry points                                             */
/* ------------------------------------------------------------------ */

int psctrl_settings_owns(uint32_t index)
{
  return index >= PS_SET_BASE && index < PS_SET_BASE + (uint32_t)NITEMS;
}

uint32_t psctrl_settings_getint(uint32_t index)
{
  const struct ps_item *it;

  table_init();
  if (!psctrl_settings_owns(index))
    return (uint32_t)-1;
  it = &g_live[index - PS_SET_BASE];
  if (!it->get)
    return 0;
  return (uint32_t)(int32_t)it->get(it);
}

static void put_be32(uint8_t *p, int32_t v)
{
  p[0] = (uint8_t)((uint32_t)v >> 24);
  p[1] = (uint8_t)((uint32_t)v >> 16);
  p[2] = (uint8_t)((uint32_t)v >> 8);
  p[3] = (uint8_t)((uint32_t)v);
}

static void put_be16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static uint32_t describe(uint32_t idx, uint32_t buf, uint32_t len)
{
  uint8_t rec[512];
  const struct ps_item *it;
  uint32_t n = PS_DESC_FIXED;
  const char *p;
  int i;

  table_init();
  if (idx >= (uint32_t)NITEMS || !buf || len < PS_DESC_FIXED + 2)
    return (uint32_t)-1;
  it = &g_live[idx];

  memset(rec, 0, sizeof(rec));
  snprintf((char *)rec, 24, "%s", it->name);
  rec[24] = it->tab;
  rec[25] = it->kind;
  rec[26] = it->klass;
  rec[27] = it->nenum;
  put_be32(rec + 28, it->min);
  put_be32(rec + 32, it->max);
  put_be32(rec + 36, it->step);
  put_be16(rec + 40, it->unit);
  put_be16(rec + 42, it->flags);
  put_be32(rec + 44, it->get ? (int32_t)it->get(it) : 0);

  /* nenum NUL-terminated labels, then the title */
  p = it->labels;
  for (i = 0; i < it->nenum && p; i++) {
    size_t l = strlen(p) + 1;

    if (n + l >= sizeof(rec) - 1)
      break;
    memcpy(rec + n, p, l);
    n += (uint32_t)l;
    p += l;
  }
  {
    size_t l = strlen(it->title) + 1;

    if (n + l < sizeof(rec)) {
      memcpy(rec + n, it->title, l);
      n += (uint32_t)l;
    } else
      rec[n++] = '\0';
  }

  if (n > len)
    n = len;
  psctrl_guest_write(buf, rec, n);
  return n;
}

uint32_t psctrl_settings_call(uint32_t subop, uint32_t p0, uint32_t p1,
                              uint32_t p2, uint32_t p3)
{
  const struct ps_item *it;

  table_init();

  switch (subop) {
    case PSCTRL_SETAPI:
      return PSCTRL_SET_API_VERSION;

    /*
     * Restart and shutdown, from the taskbar. Both leave through the
     * emulator's ordinary orderly-exit path (tty restored, JIT stats
     * dumped): restart exits 42 so the launcher relaunches it, which
     * re-reads the .cfg the user just saved and cold-boots; shutdown
     * exits 0 so the launcher stops and drops back to the console.
     * The guest asks only after its own confirmation dialog. */
    case PSCTRL_RESTART:
      pistorm_request_exit(1);
      return PS_R_OK;		/* not reached */
    case PSCTRL_SHUTDOWN:
      pistorm_request_exit(0);
      return PS_R_OK;		/* not reached */

    case PSCTRL_COUNT:
      return (uint32_t)NITEMS;

    case PSCTRL_DESCRIBE:
      return describe(p0, p1, p2);

    case PSCTRL_GETSTR: {
      const char *s;
      uint32_t n;

      if (p0 >= (uint32_t)NITEMS || !p1 || !p2)
        return (uint32_t)-1;
      s = item_str(&g_live[p0]);
      n = (uint32_t)strlen(s) + 1;
      if (n > p2)
        n = p2;
      psctrl_guest_write(p1, s, n);
      return n - 1;
    }

    case PSCTRL_SETSTR: {
      char s[PATH_MAX];

      if (p0 >= (uint32_t)NITEMS || !p1)
        return (uint32_t)PS_R_REJECT;
      psctrl_guest_read(p1, s, (uint32_t)sizeof(s));
      return (uint32_t)item_setstr(&g_live[p0], s);
    }

    case PSCTRL_SETINT:
      if (p0 >= (uint32_t)NITEMS)
        return (uint32_t)PS_R_REJECT;
      it = &g_live[p0];
      if (!it->set)
        return (uint32_t)PS_R_REJECT;
      return (uint32_t)it->set(it, (int)(int32_t)p1);

    case PSCTRL_ACTION:
      if (p0 >= (uint32_t)NITEMS)
        return (uint32_t)PS_R_REJECT;
      return (uint32_t)do_action(&g_live[p0], (int)(int32_t)p1);

    case PSCTRL_SAVE: {
      const char *path = emulator_config_path();

      (void)p0;
      boot_seed();
      if (!path || !*path)
        return (uint32_t)PS_R_REJECT;
      if (config_file_save(path, &g_boot) != 0)
        return (uint32_t)PS_R_REJECT;
      g_boot_dirty = 0;
      return PS_R_OK;
    }

    case PSCTRL_LIST:
      /* No host-served list for any field. An empty list sends the
       * accessory to the native GEM file selector - ROM/box-TOS open on
       * the share at S:\apj-os\stbox\roms, floppies at Z:\ - instead of
       * the old three-at-a-time form_alert. The picked GEMDOS path is
       * mapped back to a host path in item_setstr(). */
      (void)p1; (void)p2; (void)p3;
      return 0;
  }

  return (uint32_t)-1;
}

/* ------------------------------------------------------------------ */
/* the .cfg side: the descriptor table IS the key list                 */
/* ------------------------------------------------------------------ */
//
// "The descriptor table becomes the single list of tunables and its name
// is the .cfg key" - psctrl-live-settings-design.md. That is literal
// here: config_file_save() asks this file which keys exist and how to
// render each one, and load_config_file() hands every key it does not
// recognise back here to be matched against the same table. Adding a
// tunable is still one line in g_items.
//
// The boolean trace flags are the one exception. Twelve `foo_debug true`
// lines would bury the rest of the file, so they collapse onto a single
// `debug mfp,ipl,acsi` line, which is also the answer to the design
// doc's open question.

static const char *const g_bootkeys[] = {
  "machine", "cpu", "fpu", "mmu", "cpu_compatible", "shifter", "blitter",
  "stram_size", "ttram", "addr32", "stram_cache", "stram_direct",
  "native_hdmi", "vga", "monitor", "ym2149", "dma_sound",
  "ide", "kbd", "network", "network_irq", "fps", "rom", "stbox_tos",
  "stbox_plane", "jit_cache", "m68k_speed", "cpu_clock_multiplier"
};
#define NBOOTKEYS ((int)(sizeof(g_bootkeys) / sizeof(g_bootkeys[0])))

/* the debug flags, in the order they appear on a `debug` line */
static const struct { const char *tag; volatile int *tgt; } g_dbg[] = {
  { "verbose", &pst_verbose },
  { "ipl",    &pst_dbg_ipl_stats },
  { "irq",    &pst_dbg_irq_stats },
  { "mfp",    &pst_dbg_mfp },
  { "mfphub", &pst_dbg_mfp_hub },
  { "dmasnd", &pst_dbg_dmasnd },
  { "acsi",   &pst_dbg_acsi },
  { "ide",    &pst_dbg_ide },
  { "gemdos", &pst_dbg_gemdos },
  { "stram",  &pst_dbg_stram },
  { "net",    &pst_dbg_net },
  { "ikbd",   &pst_dbg_ikbd },
  { "hostfs", &pst_dbg_hostfs }
};
#define NDBG ((int)(sizeof(g_dbg) / sizeof(g_dbg[0])))

/* an item is written to the .cfg as its own key when it is a plain live
 * target that is not one of the trace flags */
static int item_is_cfg_tunable(const struct ps_item *it)
{
  int i;

  if (!strcmp(it->name, "jit_power"))
    return 1;
  if (!it->tgt || it->kind == PS_K_INFO || it->kind == PS_K_ACTION)
    return 0;
  for (i = 0; i < NDBG; i++)
    if (g_dbg[i].tgt == it->tgt)
      return 0;
  for (i = 0; i < NBOOTKEYS; i++)
    if (!strcmp(g_bootkeys[i], it->name))
      return 0;                       /* config_file_save renders it */
  return 1;
}

extern "C" int psctrl_settings_key_count(void)
{
  int i, n = NBOOTKEYS + 1;           /* + the single `debug` line */

  table_init();
  for (i = 0; i < NITEMS; i++)
    if (item_is_cfg_tunable(&g_live[i]))
      n++;
  return n;
}

extern "C" const char *psctrl_settings_key_at(int idx)
{
  int i;

  table_init();
  if (idx < NBOOTKEYS)
    return g_bootkeys[idx];
  idx -= NBOOTKEYS;
  if (idx == 0)
    return "debug";
  idx--;
  for (i = 0; i < NITEMS; i++)
    if (item_is_cfg_tunable(&g_live[i]) && idx-- == 0)
      return g_live[i].name;
  return "";
}

extern "C" int psctrl_settings_render_key(const char *key, char *out,
                                          unsigned long n)
{
  int i;

  table_init();

  if (!strcmp(key, "debug")) {
    unsigned long p = (unsigned long)snprintf(out, n, "debug");
    int any = 0;

    for (i = 0; i < NDBG; i++) {
      int v = *g_dbg[i].tgt;

      if (v <= 0)
        continue;                     /* -1 (follow cfg) and 0 are absent */
      p += (unsigned long)snprintf(out + p, (p < n) ? n - p : 0,
                                   "%s%s", any ? "," : " ", g_dbg[i].tag);
      if (v > 1)                      /* dmasnd is 0/1/2 */
        p += (unsigned long)snprintf(out + p, (p < n) ? n - p : 0, "=%d", v);
      any = 1;
    }
    if (!any)
      snprintf(out, n, "debug none");
    return 1;
  }

  for (i = 0; i < NITEMS; i++) {
    const struct ps_item *it = &g_live[i];

    if (strcmp(it->name, key) || !item_is_cfg_tunable(it))
      continue;
    if (it->kind == PS_K_ENUM && it->labels) {
      const char *p = it->labels;
      int v = it->get ? it->get(it) : 0, k;

      for (k = 0; k < v && *p; k++)
        p += strlen(p) + 1;
      snprintf(out, n, "%s %s", key, p);
      return 1;
    }
    snprintf(out, n, "%s %d", key, it->get ? it->get(it) : 0);
    return 1;
  }
  return 0;
}

/*
 * Called by load_config_file() for a key it does not know. Returns 1 if
 * this was one of ours, 0 to let the parser warn as it always has.
 * Precedence is cfg first, environment second: psctrl_tunables_init()
 * runs after the config load and only overrides what is actually set in
 * the environment, so a PISTORM_* in a launch script still wins.
 */
extern "C" int psctrl_settings_config_key(const char *key, const char *value)
{
  int i;

  table_init();
  if (!key)
    return 0;

  if (!strcmp(key, "debug")) {
    char buf[256], *p, *tok;

    for (i = 0; i < NDBG; i++)
      *g_dbg[i].tgt = (g_dbg[i].tgt == &pst_dbg_hostfs) ? -1 : 0;
    if (!value || !*value || !strcasecmp(value, "none"))
      return 1;
    snprintf(buf, sizeof(buf), "%s", value);
    for (p = buf; (tok = strtok(p, ", \t")) != NULL; p = NULL) {
      char *eq = strchr(tok, '=');
      int v = 1;

      if (eq) { *eq = '\0'; v = atoi(eq + 1); }
      for (i = 0; i < NDBG; i++)
        if (!strcasecmp(tok, g_dbg[i].tag))
          *g_dbg[i].tgt = v;
    }
    return 1;
  }

  for (i = 0; i < NITEMS; i++) {
    const struct ps_item *it = &g_live[i];

    if (strcmp(it->name, key) || !item_is_cfg_tunable(it))
      continue;
    if (it->kind == PS_K_ENUM && it->labels && value) {
      const char *p = it->labels;
      int k;

      for (k = 0; k < it->nenum && *p; k++, p += strlen(p) + 1)
        if (!strcasecmp(p, value)) {
          if (it->set) {
            g_cfg_loading = 1;
            it->set(it, k);
            g_cfg_loading = 0;
          }
          return 1;
        }
    }
    if (value && it->set) {
      g_cfg_loading = 1;
      it->set(it, (int)strtol(value, NULL, 0));
      g_cfg_loading = 0;
    }
    return 1;
  }
  return 0;
}
