// SPDX-License-Identifier: MIT
//
// Host test for the PSCTRL settings surface. Builds psctrl_settings.cpp,
// psctrl_tunables.c and config_file_save.c against the stubs below and
// exercises them the way the accessory does - including parsing the
// PS_DESCRIBE record with the same byte offsets psctrl.c uses, which is
// the one place a silent disagreement between host and guest would show
// up as a dialog full of nonsense rather than as a compile error.
//
// It runs on the build machine, not the Pi, and needs nothing from SDL,
// libdrm or the JIT:
//
//   c++ -std=gnu++17 -Wall -D_GNU_SOURCE -I. -Iinclude -Igpio -Ithreaddep
//       -Isoftfloat -Ijit -o /tmp/pstest
//       platforms/atari/psctrl/test_psctrl_settings.cpp
//       platforms/atari/psctrl/psctrl_settings.cpp
//       platforms/atari/psctrl/psctrl_tunables.c
//       config_file/config_file_save.c
//   (all on one line; see tests/run-psctrl-test.sh)
//   /tmp/pstest
//
// Exit 0 = every check passed.

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"

#include "platforms/atari/psctrl/psctrl.h"
#include "platforms/atari/psctrl/psctrl_settings.h"
#include "platforms/atari/psctrl/psctrl_tunables.h"
#include "config_file/config_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ stubs --- */

struct uae_prefs currprefs, changed_prefs;
int pissoff_value = 0;
int config_changed = 0;

/* The real one sets SPCFLAG_MODE_CHANGE and the CPU loop re-derives
 * cpucycleunit. Here it just does what that eventually does to the
 * prefs, so the deferred multiplier can be checked end to end. */
void check_prefs_changed_cpu(void)
{
  if (!config_changed)
    return;
  currprefs.cpu_clock_multiplier = changed_prefs.cpu_clock_multiplier;
  config_changed = 0;
}

extern "C" void jit_request_cpu_exit(void) { }
extern "C" void psctrl_jit_flush_now(void) { }
extern "C" void pistorm_request_exit(int restart) { (void) restart; }
bool check_prefs_changed_comp(bool checkonly)
{
  if (!checkonly) {
    currprefs.cachesize = changed_prefs.cachesize;
    currprefs.compnf = changed_prefs.compnf;
    currprefs.comp_constjump = changed_prefs.comp_constjump;
    currprefs.compfpu = changed_prefs.compfpu;
    currprefs.cpu_clock_multiplier = changed_prefs.cpu_clock_multiplier;
  }
  return true;
}

extern "C" uint32_t psctrl_getint(uint32_t index)
{
  if (psctrl_settings_owns(index))
    return psctrl_settings_getint(index);
  return 0;
}

extern "C" int stbox_running(void) { return 0; }
extern "C" void ym2149_settings_changed(void) { }
extern "C" void kbd_usb_mouse_cfg_changed(void) { }

/* a fake drive, enough for the floppy rows */
static char g_img[2][256] = { "", "" };
static int  g_wp[2] = { 0, 0 };
static int  g_pulsed[2] = { 0, 0 };

extern "C" int fdd_insert_disk(int d, const char *p, bool wp)
{
  if (d < 0 || d > 1) return -1;
  snprintf(g_img[d], sizeof(g_img[0]), "%s", p);
  g_wp[d] = wp;
  return 0;
}
extern "C" void fdd_eject_disk(int d)        { if (d >= 0 && d < 2) g_img[d][0] = 0; }
extern "C" void fdd_set_write_protect(int d, bool wp) { if (d >= 0 && d < 2) g_wp[d] = wp; }
extern "C" void fdd_pulse_media(int d)       { if (d >= 0 && d < 2) g_pulsed[d]++; }
extern "C" int  fdd_query(int d, char *path, int n, int *wp, int *busy)
{
  if (d < 0 || d > 1) return -1;
  if (wp) *wp = g_wp[d];
  if (busy) *busy = 0;
  if (path && n > 0) { strncpy(path, g_img[d], n - 1); path[n - 1] = 0; }
  return g_img[d][0] ? 1 : 0;
}

/* the config the settings code reads its boot shadow from */
static struct emulator_config g_cfg;
static char g_cfgpath[512];

const struct emulator_config *emulator_config_current(void) { return &g_cfg; }
extern "C" const char *emulator_config_path(void) { return g_cfgpath; }
int emulator_config_fps(void) { return g_cfg.fps ? g_cfg.fps : 25; }

/* guest memory: a plain buffer, which is what the NatFeat would write into */
static unsigned char g_guest[4096];

extern "C" void psctrl_guest_write(uint32_t addr, const void *src, uint32_t n)
{
  if (addr + n <= sizeof(g_guest))
    memcpy(g_guest + addr, src, n);
}

extern "C" void psctrl_guest_read(uint32_t addr, char *dst, uint32_t n)
{
  uint32_t i = 0;

  for (; i + 1 < n && addr + i < sizeof(g_guest); i++) {
    dst[i] = (char)g_guest[addr + i];
    if (!dst[i]) return;
  }
  dst[i] = 0;
}

/* ------------------------------------------------------------- test --- */

static int fails;

static void fail(const char *fmt, ...)
{
  va_list ap;

  fputs("FAIL ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fails++;
}

/*
 * Exactly the offsets psctrl.c's parse_desc() uses - and the same
 * signedness. On the 68k a long is 32 bits, so the shift-and-or there
 * yields -1 for 0xFFFFFFFF all by itself; on a 64-bit host it yields
 * 4294967295 and every negative minimum (m68k_speed's -1 = "max") looks
 * like a colossal positive one. The cast is what makes this test the
 * same test the accessory runs.
 */
static long be32(const unsigned char *p)
{
  return (long)(int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                         ((uint32_t)p[2] << 8) | p[3]);
}
static int be16(const unsigned char *p) { return (p[0] << 8) | p[1]; }

struct row {
  char name[24], title[64], label[12][24];
  int tab, kind, klass, nenum, unit, flags;
  long min, max, step, value;
};

static int parse(int idx, struct row *r)
{
  long got = (long)psctrl_settings_call(PSCTRL_DESCRIBE, idx, 64,
                                        sizeof(g_guest) - 64, 0);
  const unsigned char *b = g_guest + 64;
  long off;
  int i;

  if (got <= 0)
    return 0;
  memset(r, 0, sizeof(*r));
  memcpy(r->name, b, 23);
  r->tab = b[24]; r->kind = b[25]; r->klass = b[26]; r->nenum = b[27];
  r->min = be32(b + 28); r->max = be32(b + 32); r->step = be32(b + 36);
  r->unit = be16(b + 40); r->flags = be16(b + 42); r->value = be32(b + 44);
  off = PS_DESC_FIXED;
  for (i = 0; i < r->nenum && off < got; i++) {
    snprintf(r->label[i], sizeof(r->label[0]), "%s", (const char *)b + off);
    off += (long)strlen((const char *)b + off) + 1;
  }
  if (off < got)
    snprintf(r->title, sizeof(r->title), "%s", (const char *)b + off);
  return 1;
}

int main(void)
{
  int n, i, ntab[PS_TAB_N];
  struct row r;

  /* a plausible machine */
  memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.cpu_type = 4;            /* 68040: the parser stores enum - 1 */
  g_cfg.fpu = true;
  g_cfg.machine_set = true;
  g_cfg.machine_kind = 1;        /* ste */
  g_cfg.blitter = true;
  g_cfg.blitter_real = false;
  g_cfg.ttram = true;
  g_cfg.ttram_size = 128u * 1024u * 1024u;
  g_cfg.addr32 = true;
  g_cfg.stram_size = 4096u * 1024u;
  g_cfg.fps = 60;
  g_cfg.kbd_usb = true;
  g_cfg.kbd_grab = true;
  g_cfg.kbd_mode = 1;
  g_cfg.kbd_mouse_div = 2;
  g_cfg.jit_cache_set = true;
  g_cfg.jit_cache = 8192;
  snprintf(g_cfg.rom.rom_path, sizeof(g_cfg.rom.rom_path),
           "/home/pistorm/roms/tos206uk.img");
  currprefs.cachesize = changed_prefs.cachesize = 8192;
  currprefs.m68k_speed = -1;
  currprefs.cpu_clock_multiplier = 2;
  currprefs.compnf = currprefs.comp_constjump = true;
  psctrl_tunables_init();

  n = (int)psctrl_settings_call(PSCTRL_COUNT, 0, 0, 0, 0);
  printf("%d settings, settings API %u\n", n,
         psctrl_settings_call(PSCTRL_SETAPI, 0, 0, 0, 0));
  if (n < 40)
    fail("only %d settings - the table looks truncated", n);

  memset(ntab, 0, sizeof(ntab));

  /* --- every descriptor must survive the round trip -------------- */
  for (i = 0; i < n; i++) {
    if (!parse(i, &r)) {
      fail("DESCRIBE %d returned nothing", i);
      continue;
    }
    if (!r.name[0])       fail("setting %d has no name", i);
    if (!r.title[0])      fail("%s has no title", r.name);
    if (r.tab >= PS_TAB_N) fail("%s has tab %d", r.name, r.tab);
    if (r.kind > PS_K_INFO) fail("%s has kind %d", r.name, r.kind);
    if (r.klass > PS_C_RO)  fail("%s has class %d", r.name, r.klass);
    if (r.nenum > 12)     fail("%s has %d labels", r.name, r.nenum);
    ntab[r.tab]++;

    /* an enum's labels must all be there and non-empty - a missing one
     * is a radio with no caption and is invisible on screen */
    if (r.kind == PS_K_ENUM || r.kind == PS_K_BOOL) {
      int k;

      if (r.nenum < 2)
        fail("%s is an enum with %d labels", r.name, r.nenum);
      for (k = 0; k < r.nenum; k++)
        if (!r.label[k][0])
          fail("%s label %d is empty", r.name, k);
      if (r.max != r.nenum - 1)
        fail("%s: max %ld but %d labels", r.name, r.max, r.nenum);
    }
    if (r.kind == PS_K_INT && r.max <= r.min)
      fail("%s: empty range %ld..%ld", r.name, r.min, r.max);

    /* PS_GETINT on the settings index must agree with DESCRIBE */
    if (r.kind != PS_K_STR && r.kind != PS_K_ACTION) {
      long v = (long)(int32_t)psctrl_settings_getint(PS_SET_BASE + i);

      if (v != r.value)
        fail("%s: DESCRIBE says %ld, GETINT says %ld", r.name, r.value, v);
    }
  }
  for (i = 0; i < PS_TAB_N; i++) {
    printf("  tab %d: %d rows\n", i, ntab[i]);
    if (!ntab[i])
      fail("tab %d is empty", i);
  }

  /*
   * --- a boot row must show what the CONFIG SAYS ------------------
   *
   * Not the label one either side of it. This is worth its own check
   * because the enums here are indices into two different tables -
   * cpu_type is the M68K_CPU_TYPE_* enum minus one, the dialog wants a
   * plain 0..5, and the cfg writer wants the enum back - and an
   * off-by-one in any of the three is invisible except as a machine
   * quietly reporting the wrong CPU. Which is exactly what happened:
   * a 68040 read as 68030 on hardware.
   */
  {
    static const struct { const char *key, *want; } expect[] = {
      { "cpu",     "68040" },     /* g_cfg.cpu_type = 4 */
      { "machine", "ste"   },     /* machine_kind = 1   */
      { "blitter", "emulated" },  /* blitter && !real   */
      { "ttram",   "128M"  },     /* 128 MB             */
    };
    unsigned k;

    for (k = 0; k < sizeof(expect) / sizeof(expect[0]); k++) {
      int found = 0;

      for (i = 0; i < n; i++) {
        parse(i, &r);
        if (strcmp(r.name, expect[k].key))
          continue;
        found = 1;
        if (r.value < 0 || r.value >= r.nenum)
          fail("%s: value %ld is outside its %d labels",
               r.name, r.value, r.nenum);
        else if (strcmp(r.label[r.value], expect[k].want))
          fail("%s reads '%s', the config says '%s'",
               r.name, r.label[r.value], expect[k].want);
        break;
      }
      if (!found)
        fail("no %s row in the table", expect[k].key);
    }
  }

  /*
   * --- the three classes answer differently -----------------------
   *
   * From here on the table is MUTATED, so anything that checks a value
   * against the config has to have run above this point. The first
   * version of the check above sat below it and failed because this
   * block had already set `machine` to st.
   */
  {
    int live = -1, defer = -1, boot = -1;

    for (i = 0; i < n; i++) {
      parse(i, &r);
      if (live < 0 && r.klass == PS_C_LIVE && r.kind == PS_K_INT) live = i;
      if (defer < 0 && r.klass == PS_C_DEFER && r.kind == PS_K_ENUM) defer = i;
      if (boot < 0 && r.klass == PS_C_BOOT && r.kind == PS_K_ENUM) boot = i;
    }
    if (live >= 0) {
      long mid;

      parse(live, &r);
      mid = r.min + (r.max - r.min) / 2;
      if ((int)psctrl_settings_call(PSCTRL_SETINT, live, mid, 0, 0) != PS_R_OK)
        fail("%s: a live setting did not answer OK", r.name);
      if ((long)(int32_t)psctrl_settings_getint(PS_SET_BASE + live) != mid)
        fail("%s: a live setting did not take", r.name);
      /* and out of range is refused, not clamped silently */
      if ((int)psctrl_settings_call(PSCTRL_SETINT, live, r.max + 1000, 0, 0)
          != PS_R_REJECT)
        fail("%s: an out-of-range value was not refused", r.name);
    }
    if (defer >= 0) {
      if ((int)psctrl_settings_call(PSCTRL_SETINT, defer, 1, 0, 0) != PS_R_DEFER)
        fail("a deferred setting did not answer DEFER");
      if (!psctrl_pending_armed)
        fail("a deferred setting did not arm the boundary hook");
      psctrl_apply_pending();
      if (psctrl_pending_armed)
        fail("apply_pending did not disarm");
    }
    if (boot >= 0) {
      if ((int)psctrl_settings_call(PSCTRL_SETINT, boot, 0, 0, 0) != PS_R_RESTART)
        fail("a boot setting did not answer RESTART");
    }

    /*
     * jit_power is one control with two classes, and that is the whole
     * reason the dialog cannot be trusted to know: 1..6 is a store to
     * pissoff_value, but 0 means cachesize = 0, which means
     * alloc_cache(), which cannot happen inside a translated block. The
     * handler has to upgrade it to deferred on its own.
     */
    for (i = 0; i < n; i++) {
      parse(i, &r);
      if (strcmp(r.name, "jit_power"))
        continue;
      if ((int)psctrl_settings_call(PSCTRL_SETINT, i, 4, 0, 0) != PS_R_OK)
        fail("jit_power 4 was not applied live");
      if ((int)psctrl_settings_call(PSCTRL_SETINT, i, 0, 0, 0) != PS_R_DEFER)
        fail("jit_power 0 (JIT off) was not deferred");
      psctrl_apply_pending();
      if (currprefs.cachesize != 0)
        fail("jit_power 0 did not turn the cache off at the boundary");
      /* and back on again */
      if ((int)psctrl_settings_call(PSCTRL_SETINT, i, 3, 0, 0) != PS_R_DEFER)
        fail("jit_power back on was not deferred");
      psctrl_apply_pending();
      if (currprefs.cachesize == 0)
        fail("jit_power could not be turned back on");
      break;
    }

    /*
     * The clock multiplier is a SLOWDOWN and 0 is off - the fastest
     * setting, and the one the emulator boots with. The row shipped
     * with min 1, so its own default was outside its slider. It must
     * reach 0, it must not pretend to apply live (cpucycleunit is
     * derived in update_68k_cycles(), not poked), and a negative value
     * is not a faster setting - jit_cpu_init() clamps it to 0.
     */
    for (i = 0; i < n; i++) {
      parse(i, &r);
      if (strcmp(r.name, "cpu_clock_multiplier"))
        continue;
      if (r.min != 0)
        fail("the clock multiplier cannot reach 0, its own default");
      if (r.klass != PS_C_DEFER)
        fail("the clock multiplier claims to apply live");
      if ((int)psctrl_settings_call(PSCTRL_SETINT, i, -1, 0, 0) != PS_R_REJECT)
        fail("a negative clock multiplier was accepted");
      if ((int)psctrl_settings_call(PSCTRL_SETINT, i, 4, 0, 0) != PS_R_DEFER)
        fail("the clock multiplier was not deferred");
      if (currprefs.cpu_clock_multiplier == 4)
        fail("the clock multiplier reached currprefs before the boundary");
      psctrl_apply_pending();
      if (currprefs.cpu_clock_multiplier != 4)
        fail("the clock multiplier did not apply at the boundary");
      if ((int)psctrl_settings_call(PSCTRL_SETINT, i, 0, 0, 0) != PS_R_DEFER)
        fail("the clock multiplier could not be set back to 0");
      psctrl_apply_pending();
      if (currprefs.cpu_clock_multiplier != 0)
        fail("the clock multiplier did not go back to 0 (off)");
      break;
    }
  }

  /* --- strings ---------------------------------------------------- */
  for (i = 0; i < n; i++) {
    parse(i, &r);
    if (r.kind != PS_K_STR)
      continue;
    psctrl_settings_call(PSCTRL_GETSTR, i, 64, 256, 0);
    if (!strcmp(r.name, "rom") &&
        !strstr((const char *)g_guest + 64, "tos206uk"))
      fail("rom GETSTR gave '%s'", (const char *)g_guest + 64);
    if (!strcmp(r.name, "floppy_a")) {
      snprintf((char *)g_guest + 512, 64, "%s", "/tmp/disk1.st");
      if ((int)psctrl_settings_call(PSCTRL_SETSTR, i, 512, 0, 0) != PS_R_OK)
        fail("inserting a floppy was refused");
      if (!g_pulsed[0])
        fail("inserting a floppy did not pulse the media change");
    }
  }

  /* --- the config writer keeps the file --------------------------- */
  {
    FILE *f;
    char line[512];
    int saw_comment = 0, saw_unknown = 0, saw_cpu = 0, saw_debug = 0, nmach = 0;

    snprintf(g_cfgpath, sizeof(g_cfgpath), "/tmp/psctrl-test-%d.cfg",
             (int)getpid());
    f = fopen(g_cfgpath, "w");
    fprintf(f, "# a comment that must survive\n");
    fprintf(f, "machine ste\n");
    fprintf(f, "cpu 68000\n");
    fprintf(f, "something_we_do_not_manage 42\n");
    fprintf(f, "\n# and a trailing one\n");
    fclose(f);

    if ((int)psctrl_settings_call(PSCTRL_SAVE, 0, 0, 0, 0) != PS_R_OK)
      fail("SAVE failed");

    f = fopen(g_cfgpath, "r");
    if (!f)
      fail("the config vanished");
    else {
      while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "a comment that must survive")) saw_comment++;
        if (strstr(line, "something_we_do_not_manage 42")) saw_unknown++;
        if (!strncmp(line, "cpu ", 4)) saw_cpu++;
        if (!strncmp(line, "machine ", 8)) nmach++;
        if (!strncmp(line, "debug", 5)) saw_debug++;
      }
      fclose(f);
    }
    if (!saw_comment) fail("the writer dropped a comment");
    if (!saw_unknown) fail("the writer dropped a key it does not manage");
    if (saw_cpu != 1)  fail("cpu appears %d times", saw_cpu);
    if (nmach != 1)    fail("machine appears %d times", nmach);
    if (!saw_debug)    fail("no debug line was written");

    /* the original is kept */
    {
      char bak[600];

      snprintf(bak, sizeof(bak), "%s.bak", g_cfgpath);
      f = fopen(bak, "r");
      if (!f)
        fail("no .bak was left behind");
      else
        fclose(f);
      remove(bak);
    }

    /* --- a LIVE value that is also a boot key is saved as set -------
     *
     * `fps` is both: the render loop reads pst_fps, but the writer
     * renders boot keys from the boot shadow. Field report: change the
     * frame rate, Save, and the file still said the old fps - so the
     * change was lost on the next boot, and a second save left the file
     * byte for byte the same as its .bak. */
    {
      int fps_idx = -1, fps_lines = 0, fps_new = 0;
      char bak[600];

      for (i = 0; i < n; i++)
        if (parse(i, &r) && !strcmp(r.name, "fps"))
          fps_idx = i;
      if (fps_idx < 0)
        fail("no fps item");
      else {
        f = fopen(g_cfgpath, "w");
        fprintf(f, "cpu 68000\nfps 60\n");
        fclose(f);
        if ((int)psctrl_settings_call(PSCTRL_SETINT, fps_idx, 30, 0, 0) != PS_R_OK)
          fail("fps 30 was refused");
        if ((int)psctrl_settings_call(PSCTRL_SAVE, 0, 0, 0, 0) != PS_R_OK)
          fail("SAVE failed");
        f = fopen(g_cfgpath, "r");
        while (f && fgets(line, sizeof(line), f)) {
          if (!strncmp(line, "fps ", 4)) fps_lines++;
          if (!strcmp(line, "fps 30\n")) fps_new++;
        }
        if (f) fclose(f);
        if (fps_lines != 1) fail("fps appears %d times after a save", fps_lines);
        if (!fps_new)       fail("fps was changed to 30 and saved, but the file does not say so");
        snprintf(bak, sizeof(bak), "%s.bak", g_cfgpath);
        remove(bak);
      }
    }

    /* --- one line per managed key, whatever the file had -----------
     *
     * A doubled key is not a setting, it is a question of which copy
     * the parser saw last. The writer keeps the first and drops the
     * rest; keys it does not manage (hdd, hostfs) may repeat by design
     * and are left alone. And a boot item that lives in a tunable
     * (audio_frames) must be written at all - it was not. */
    {
      int nstram = 0, nfps = 0, nhdd = 0, nframes = 0, frames_idx = -1;
      char bak[600];

      for (i = 0; i < n; i++)
        if (parse(i, &r) && !strcmp(r.name, "audio_frames"))
          frames_idx = i;
      f = fopen(g_cfgpath, "w");
      fprintf(f, "stram_size 4M\nfps 60\nhdd a.img\nhdd b.img\nstram_size 1M\nfps 50\n");
      fclose(f);
      if (frames_idx < 0)
        fail("no audio_frames item");
      else if ((int)psctrl_settings_call(PSCTRL_SETINT, frames_idx, 3, 0, 0) != PS_R_RESTART)
        fail("audio_frames 3 was refused");
      if ((int)psctrl_settings_call(PSCTRL_SAVE, 0, 0, 0, 0) != PS_R_OK)
        fail("SAVE failed");
      f = fopen(g_cfgpath, "r");
      while (f && fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "stram_size ", 11)) nstram++;
        if (!strncmp(line, "fps ", 4))         nfps++;
        if (!strncmp(line, "hdd ", 4))         nhdd++;
        if (!strcmp(line, "audio_frames 4096\n")) nframes++;
      }
      if (f) fclose(f);
      if (nstram != 1) fail("stram_size appears %d times after a save", nstram);
      if (nfps != 1)   fail("fps appears %d times after a save", nfps);
      if (nhdd != 2)   fail("hdd lines: %d, expected both kept", nhdd);
      if (!nframes)    fail("audio_frames 4096 was set and saved, but the file does not say so");
      snprintf(bak, sizeof(bak), "%s.bak", g_cfgpath);
      remove(bak);
    }

    /* --- EVERY accepted change is saved --------------------------------
     *
     * The rule the field asked for: change a thing in the dialog, Save,
     * and the file says so - whatever class the thing is, wherever its
     * value happens to live (boot shadow, currprefs, a tunable). Each
     * settable item is moved to a different valid value and the file
     * must change; jit_cache, fps and the three JIT switches were all
     * silently not saved before this check existed.
     *
     * Not config, by design: the floppy write-protect toggles (media).
     * hostfs_debug 'off' and 'follow cfg' both render as absent. */
    for (i = 0; i < n; i++) {
      long v, rc, sz0, sz1;
      unsigned long h0 = 0, h1 = 0;
      int c;

      if (!parse(i, &r))
        continue;
      if (r.kind == PS_K_STR || r.kind == PS_K_INFO || r.kind == PS_K_ACTION)
        continue;
      if (!strcmp(r.name, "floppy_a_wp") || !strcmp(r.name, "floppy_b_wp") ||
          !strcmp(r.name, "hostfs_debug"))
        continue;
      if (r.kind == PS_K_BOOL)      v = !r.value;
      else if (r.kind == PS_K_ENUM) v = (r.value + 1) % r.nenum;
      else                          v = (r.value == r.max) ? r.min : r.max;

      psctrl_settings_call(PSCTRL_SAVE, 0, 0, 0, 0);
      f = fopen(g_cfgpath, "r"); sz0 = 0; h0 = 5381;
      while (f && (c = fgetc(f)) != EOF) { h0 = (h0 * 33) ^ (unsigned)c; sz0++; }
      if (f) fclose(f);

      rc = (long)psctrl_settings_call(PSCTRL_SETINT, i, (uint32_t)v, 0, 0);
      if (rc != PS_R_OK && rc != PS_R_RESTART && rc != PS_R_DEFER) {
        fail("%s: %ld -> %ld refused (%ld)", r.name, r.value, v, rc);
        continue;
      }
      if (rc == PS_R_DEFER)
        psctrl_apply_pending();

      psctrl_settings_call(PSCTRL_SAVE, 0, 0, 0, 0);
      f = fopen(g_cfgpath, "r"); sz1 = 0; h1 = 5381;
      while (f && (c = fgetc(f)) != EOF) { h1 = (h1 * 33) ^ (unsigned)c; sz1++; }
      if (f) fclose(f);
      if (sz0 == sz1 && h0 == h1)
        fail("%s: changed %ld -> %ld and saved, but the .cfg did not change", r.name, r.value, v);
    }
    {
      char bak[600];
      snprintf(bak, sizeof(bak), "%s.bak", g_cfgpath);
      remove(bak);
    }
    remove(g_cfgpath);
  }

  printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
  return fails ? 1 : 0;
}
