// SPDX-License-Identifier: GPL-2.0-or-later
//
// psdos.c - the DOS service for the PiSTorm Atari (psdos-design.md).
//
// A libretro front end with no display: it loads the DOSBox Pure core
// (dosbox_pure_libretro.so, built with its ARMV8 dynrec), runs it at the
// DOS machine's own frame rate, scales each frame into the guest's view in
// the guest's fVDI pixel format, and hands it over through shared memory -
// the psweb pattern. Sound goes into a ring in the same shared memory; the
// emulator drains it into its SDL3 device. Input arrives as commands.
//
// A separate process on purpose: a crash in DOS code costs the DOS window,
// not the Atari, and the core is GPL (this file is GPL-2.0-or-later; the
// emulator only talks to it over a socket).
//
// One thread. The libretro API is single-threaded, so the socket, the
// commands and retro_run() all happen here, between frames: poll() sleeps
// until the next frame's deadline or a command, whichever is first.
// (DOSBox Pure runs DOS on its own thread inside the core; retro_run is the
// frame handshake with it.)
//
//   psdos                         listens on $PSDOS_SOCK (default /tmp/psdos.sock),
//                                 or on the socket systemd hands it (LISTEN_FDS)
//   psdos --selftest [path] [s]   no socket: run the core headless for s seconds
//                                 (default 5), print the frame rate and write
//                                 the last frame to psdos-selftest.ppm
//   PSDOS_CORE=/path/x.so         the core (default: next to this binary, then
//                                 /usr/local/lib/psdos/dosbox_pure_libretro.so)
//   PSDOS_HOME=/var/lib/psdos     system/ and saves/ live here
//   PSDOS_CONF=/etc/psdos/psdos.conf   "dosbox_pure_x = value" lines
//   PSDOS_CPUS=2                  affinity mask (hex), default core 1
//   PSDOS_IDLE_S=600              exit after this long with no client (0 = never)
//   PSDOS_DEBUG=1                 chatter, and the core's own log
//
// Talks the protocol in platforms/atari/dos/psdos_proto.h.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "libretro.h"
#include "../platforms/atari/dos/psdos_proto.h"

/* --------------------------------------------------------------- state -- */

static int g_debug;
#define DBG(...) do { if (g_debug) { fprintf(stderr, "[psdos] " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define SAY(...) do { fprintf(stderr, "[psdos] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static volatile sig_atomic_t g_quit;

/* the core */
static void *g_dl;
static struct {
  void (*init)(void);
  void (*deinit)(void);
  void (*get_system_info)(struct retro_system_info *);
  void (*get_system_av_info)(struct retro_system_av_info *);
  void (*set_environment)(retro_environment_t);
  void (*set_video_refresh)(retro_video_refresh_t);
  void (*set_audio_sample)(retro_audio_sample_t);
  void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
  void (*set_input_poll)(retro_input_poll_t);
  void (*set_input_state)(retro_input_state_t);
  void (*set_controller_port_device)(unsigned, unsigned);
  void (*reset)(void);
  void (*run)(void);
  bool (*load_game)(const struct retro_game_info *);
  void (*unload_game)(void);
} C;
static int g_core_inited;          /* retro_init done                       */
static int g_loaded;               /* retro_load_game succeeded             */
static int g_core_exit;            /* the core asked to shut down           */
static double g_fps = 60.0;
static double g_sample_rate;       /* the core's audio rate, Hz              */
static int g_failed;               /* the last LOAD was refused             */
static int g_used;                 /* this process has run a game already   */
static char g_last_path[PATH_MAX]; /* what LOAD last asked for ("" = prompt) */
static char **g_argv;
static int g_resume_pending_input; /* commands carried across a re-exec */
static double g_aspect;            /* <= 0: use w/h                         */
static enum retro_pixel_format g_pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static retro_keyboard_event_t g_kbd_cb;
static char g_title[256];
static char g_status[256];
static char g_core_path[PATH_MAX];
static char g_home[PATH_MAX] = "/var/lib/psdos";
static char g_sysdir[PATH_MAX], g_savedir[PATH_MAX];

/* core options: defaults from the core, our defaults, then the .conf, then
 * OPTION commands; GET_VARIABLE answers the last one set */
#define MAX_OPT 128
static struct opt { char key[64]; char val[96]; int from_cmd; } g_opt[MAX_OPT];
static int g_nopt;
static int g_opt_dirty;

/* the latest frame from the core, as it was given (copied: the pointer is
 * only valid during the callback) and the one last published, for damage */
static uint8_t *g_src, *g_prev;
static size_t g_src_cap;
static unsigned g_src_w, g_src_h, g_src_pitch, g_src_bpp;   /* bpp 2 or 4 */
static int g_src_fresh;            /* a frame arrived since the last publish */
static unsigned g_prev_w, g_prev_h;
static uint32_t g_dropped;
static uint32_t g_frames_1s, g_fps_x100;
static double g_fps_t0;
static uint32_t g_core_us;

/* the view (one) */
static struct {
  int active;
  int visible;
  int w, h, bpp;
  int full;                        /* next publish repaints the whole view */
  /* scale map, recomputed when the source or the view changes */
  unsigned map_sw, map_sh, map_vw, map_vh;
  int dx, dy, dw, dh;
  int *xmap, *ymap;
} V;

/* input as the core reads it */
static uint8_t g_keys[RETROK_LAST];
static int g_mouse_dx, g_mouse_dy, g_mouse_btn;
static uint16_t g_joy[2];
static int16_t g_stick[2][4];           /* lx, ly, rx, ry: -32768..32767 */
static int16_t g_trig[2][2];            /* l2, r2: 0..32767              */

/* shared memory */
static int g_shm_fd = -1;
static uint8_t *g_shm;
static size_t g_shm_size;
static struct psdos_surface *g_surf;
static uint8_t *g_pixels;
static struct psdos_audio *g_audio;

/* the socket */
static int g_listen_fd = -1;
static int g_listen_inherited;
static int g_client_fd = -1;
static char g_sock_path[108];
static int g_idle_s = 600;
static double g_idle_since;
static uint8_t g_inbuf[sizeof(struct psdos_cmd) + PSDOS_STR_MAX + 16];
static size_t g_inlen;
static uint32_t g_flags_sent = 0xFFFFFFFFu;

static double now_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------ options --- */

static struct opt *opt_find(const char *key)
{
  for (int i = 0; i < g_nopt; i++)
    if (!strcmp(g_opt[i].key, key))
      return &g_opt[i];
  return NULL;
}

static void opt_set(const char *key, const char *val, int dirty)
{
  struct opt *o = opt_find(key);
  if (!o) {
    if (g_nopt >= MAX_OPT)
      return;
    o = &g_opt[g_nopt++];
    snprintf(o->key, sizeof o->key, "%s", key);
  }
  if (strcmp(o->val, val)) {
    snprintf(o->val, sizeof o->val, "%s", val);
    if (dirty)
      g_opt_dirty = 1;
  }
  if (dirty)
    o->from_cmd = 1;                    /* survives a re-exec */
}

/* "key = value" or "key=value"; '#' starts a comment */
static void opt_parse_line(char *line, int dirty)
{
  char *h = strchr(line, '#');
  if (h) *h = 0;
  char *eq = strchr(line, '=');
  if (!eq)
    return;
  *eq = 0;
  char *k = line, *v = eq + 1;
  while (*k == ' ' || *k == '\t') k++;
  char *e = k + strlen(k);
  while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
  while (*v == ' ' || *v == '\t') v++;
  e = v + strlen(v);
  while (e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
  if (*k && *v)
    opt_set(k, v, dirty);
}

/* What psdos wants, over the core's own defaults (psdos-design.md 3.1) */
static void opt_defaults(void)
{
  static const char *d[][2] = {
    { "dosbox_pure_cpu_core",    "dynamic" },
    { "dosbox_pure_cycles",      "auto" },
    { "dosbox_pure_machine",     "svga" },
    { "dosbox_pure_svga",        "svga_s3" },
    { "dosbox_pure_memory_size", "16" },
    { "dosbox_pure_sblaster_type", "sb16" },
    { "dosbox_pure_audiorate",   "48000" },
    { "dosbox_pure_mouse_input", "true" },
    { "dosbox_pure_menu_time",   "99" },
    { "dosbox_pure_perfstats",   "none" },
    { "dosbox_pure_savestate",   "disabled" },
    /* report 4:3 for the 320x200 / 640x400 modes, as a monitor showed them */
    { "dosbox_pure_aspect_correction", "true" },
  };
  for (size_t i = 0; i < sizeof d / sizeof d[0]; i++)
    opt_set(d[i][0], d[i][1], 0);

  const char *path = getenv("PSDOS_CONF");
  if (!path || !*path)
    path = "/etc/psdos/psdos.conf";
  FILE *f = fopen(path, "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof line, f))
      opt_parse_line(line, 0);
    fclose(f);
    DBG("options from %s", path);
  }
}

/* ------------------------------------------------------------- VFS v3 --- */
/* DOSBox Pure only uses the directory functions (case-insensitive path
 * resolution and its system-folder scan) and remove(). */
struct retro_vfs_dir_handle { DIR *d; struct dirent *e; char path[PATH_MAX]; };

static struct retro_vfs_dir_handle *vfs_opendir(const char *dir, bool hidden)
{
  (void)hidden;
  DIR *d = opendir(dir);
  if (!d)
    return NULL;
  struct retro_vfs_dir_handle *h = calloc(1, sizeof *h);
  if (!h) { closedir(d); return NULL; }
  h->d = d;
  snprintf(h->path, sizeof h->path, "%s", dir);
  return h;
}
static bool vfs_readdir(struct retro_vfs_dir_handle *h) { return (h->e = readdir(h->d)) != NULL; }
static const char *vfs_dirent_get_name(struct retro_vfs_dir_handle *h) { return h->e ? h->e->d_name : NULL; }
static bool vfs_dirent_is_dir(struct retro_vfs_dir_handle *h)
{
  if (!h->e)
    return false;
  if (h->e->d_type == DT_DIR)
    return true;
  if (h->e->d_type != DT_UNKNOWN && h->e->d_type != DT_LNK)
    return false;
  char p[PATH_MAX * 2];
  struct stat st;
  snprintf(p, sizeof p, "%s/%s", h->path, h->e->d_name);
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static int vfs_closedir(struct retro_vfs_dir_handle *h) { closedir(h->d); free(h); return 0; }
static int vfs_remove(const char *path) { return remove(path); }

static struct retro_vfs_interface g_vfs = {
  .remove = vfs_remove,
  .opendir = vfs_opendir,
  .readdir = vfs_readdir,
  .dirent_get_name = vfs_dirent_get_name,
  .dirent_is_dir = vfs_dirent_is_dir,
  .closedir = vfs_closedir,
};

/* --------------------------------------------------------- callbacks ---- */

static void RETRO_CALLCONV core_log(enum retro_log_level level, const char *fmt, ...)
{
  if (!g_debug && level < RETRO_LOG_WARN)
    return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  size_t n = strlen(buf);
  while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
  fprintf(stderr, "[psdos] core: %s\n", buf);
  if (level >= RETRO_LOG_ERROR)
    snprintf(g_status, sizeof g_status, "%s", buf);
}

static void apply_av_info(const struct retro_system_av_info *av)
{
  if (av->timing.fps > 1.0 && av->timing.fps < 500.0)
    g_fps = av->timing.fps;
  g_aspect = av->geometry.aspect_ratio;
  if (av->timing.sample_rate > 1000.0)
    g_sample_rate = av->timing.sample_rate;
}

static bool RETRO_CALLCONV environment(unsigned cmd, void *data)
{
  switch (cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      ((struct retro_log_callback *)data)->log = core_log;
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
      enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
      if (f != RETRO_PIXEL_FORMAT_XRGB8888 && f != RETRO_PIXEL_FORMAT_RGB565)
        return false;
      g_pixfmt = f;
      return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      *(const char **)data = g_sysdir;
      return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *(const char **)data = g_savedir;
      return true;
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
      *(const char **)data = g_sysdir;
      return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
      *(unsigned *)data = 0;            /* answer the v0 SET_VARIABLES list */
      return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
      /* "Description; default|other|..." - the first value is the default */
      for (const struct retro_variable *v = data; v && v->key; v++) {
        if (opt_find(v->key) || !v->value)
          continue;
        const char *s = strchr(v->value, ';');
        if (!s)
          continue;
        s++;
        while (*s == ' ') s++;
        char def[96];
        size_t n = strcspn(s, "|");
        if (n >= sizeof def) n = sizeof def - 1;
        memcpy(def, s, n);
        def[n] = 0;
        opt_set(v->key, def, 0);
      }
      return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      struct retro_variable *v = data;
      struct opt *o = v->key ? opt_find(v->key) : NULL;
      v->value = o ? o->val : NULL;
      return o != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *(bool *)data = g_opt_dirty != 0;
      g_opt_dirty = 0;
      return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      return true;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
      g_kbd_cb = ((const struct retro_keyboard_callback *)data)->callback;
      return true;
    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE: {
      struct retro_vfs_interface_info *vi = data;
      if (vi->required_interface_version > 3)
        return false;
      vi->required_interface_version = 3;
      vi->iface = &g_vfs;
      return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      apply_av_info(data);
      V.full = 1;
      return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
      g_aspect = ((const struct retro_game_geometry *)data)->aspect_ratio;
      V.full = 1;
      return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
      g_core_exit = 1;
      return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
      *(int *)data = 3;
      return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
      *(bool *)data = false;
      return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      *(bool *)data = true;
      return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
      *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
      const struct retro_message *m = data;
      if (m && m->msg) {
        DBG("message: %s", m->msg);
        snprintf(g_status, sizeof g_status, "%s", m->msg);
      }
      return true;
    }
    default:
      return false;                    /* everything else: not offered */
  }
}

static void RETRO_CALLCONV video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
  g_frames_1s++;
  if (!data || data == RETRO_HW_FRAME_BUFFER_VALID || !w || !h)
    return;                            /* a dupe: nothing changed */
  unsigned bpp = g_pixfmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
  size_t row = (size_t)w * bpp, need = row * h;
  if (need > g_src_cap) {
    free(g_src);
    free(g_prev);
    g_src = malloc(need);
    g_prev = malloc(need);
    g_src_cap = (g_src && g_prev) ? need : 0;
    if (!g_src_cap)
      return;
    V.full = 1;
  }
  for (unsigned y = 0; y < h; y++)
    memcpy(g_src + y * row, (const uint8_t *)data + y * pitch, row);
  if (g_src_fresh)
    g_dropped++;
  if (w != g_src_w || h != g_src_h || bpp != g_src_bpp)
    V.full = 1;
  g_src_w = w; g_src_h = h; g_src_pitch = (unsigned)row; g_src_bpp = bpp;
  g_src_fresh = 1;
}

static void audio_push(const int16_t *data, size_t frames)
{
  if (!g_audio)
    return;
  uint32_t w = g_audio->wpos;
  for (size_t i = 0; i < frames; i++) {
    uint32_t at = (w + (uint32_t)i) & (PSDOS_AUDIO_FRAMES - 1);
    g_audio->data[at * 2]     = data[i * 2];
    g_audio->data[at * 2 + 1] = data[i * 2 + 1];
  }
  __sync_synchronize();
  g_audio->wpos = w + (uint32_t)frames;
  if (g_audio->wpos - g_audio->rpos > PSDOS_AUDIO_FRAMES)
    g_audio->overruns++;
}

static void RETRO_CALLCONV audio_sample(int16_t l, int16_t r)
{
  int16_t f[2] = { l, r };
  if (V.visible)
    audio_push(f, 1);
}

static size_t RETRO_CALLCONV audio_batch(const int16_t *data, size_t frames)
{
  if (V.visible)
    audio_push(data, frames);
  return frames;
}

static void RETRO_CALLCONV input_poll(void) { }

static int16_t RETRO_CALLCONV input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
  switch (device & RETRO_DEVICE_MASK) {
    case RETRO_DEVICE_KEYBOARD:
      return (port == 0 && id < RETROK_LAST) ? g_keys[id] : 0;
    case RETRO_DEVICE_MOUSE:
      if (port != 0)
        return 0;
      switch (id) {
        case RETRO_DEVICE_ID_MOUSE_X: { int v = g_mouse_dx; g_mouse_dx = 0; return (int16_t)v; }
        case RETRO_DEVICE_ID_MOUSE_Y: { int v = g_mouse_dy; g_mouse_dy = 0; return (int16_t)v; }
        case RETRO_DEVICE_ID_MOUSE_LEFT:   return (g_mouse_btn & 1) != 0;
        case RETRO_DEVICE_ID_MOUSE_RIGHT:  return (g_mouse_btn & 2) != 0;
        case RETRO_DEVICE_ID_MOUSE_MIDDLE: return (g_mouse_btn & 4) != 0;
        default: return 0;
      }
    case RETRO_DEVICE_JOYPAD:
      if (port > 1)
        return 0;
      if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (int16_t)g_joy[port];
      return id < 16 ? (g_joy[port] >> id) & 1 : 0;
    case RETRO_DEVICE_ANALOG:
      if (port > 1)
        return 0;
      if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT || index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
        if (id > RETRO_DEVICE_ID_ANALOG_Y)
          return 0;
        return g_stick[port][(index == RETRO_DEVICE_INDEX_ANALOG_RIGHT ? 2 : 0) + id];
      }
      if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) {
        if (id == RETRO_DEVICE_ID_JOYPAD_L2 && g_trig[port][0]) return g_trig[port][0];
        if (id == RETRO_DEVICE_ID_JOYPAD_R2 && g_trig[port][1]) return g_trig[port][1];
        return (id < 16 && ((g_joy[port] >> id) & 1)) ? 32767 : 0;
      }
      return 0;
    default:
      return 0;
  }
}

/* ------------------------------------------------------------- core ----- */

static int core_open(void)
{
  if (g_dl)
    return 0;
  g_dl = dlopen(g_core_path, RTLD_NOW | RTLD_LOCAL);
  if (!g_dl) {
    snprintf(g_status, sizeof g_status, "core not loadable: %s", dlerror());
    SAY("%s", g_status);
    return -1;
  }
#define SYM(f, n) do { *(void **)&C.f = dlsym(g_dl, n); if (!C.f) { \
    snprintf(g_status, sizeof g_status, "core lacks %s", n); SAY("%s", g_status); \
    dlclose(g_dl); g_dl = NULL; return -1; } } while (0)
  SYM(init, "retro_init");
  SYM(deinit, "retro_deinit");
  SYM(get_system_info, "retro_get_system_info");
  SYM(get_system_av_info, "retro_get_system_av_info");
  SYM(set_environment, "retro_set_environment");
  SYM(set_video_refresh, "retro_set_video_refresh");
  SYM(set_audio_sample, "retro_set_audio_sample");
  SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
  SYM(set_input_poll, "retro_set_input_poll");
  SYM(set_input_state, "retro_set_input_state");
  SYM(set_controller_port_device, "retro_set_controller_port_device");
  SYM(reset, "retro_reset");
  SYM(run, "retro_run");
  SYM(load_game, "retro_load_game");
  SYM(unload_game, "retro_unload_game");
#undef SYM
  struct retro_system_info si;
  memset(&si, 0, sizeof si);
  C.get_system_info(&si);
  SAY("core %s %s (%s)", si.library_name ? si.library_name : "?",
      si.library_version ? si.library_version : "?", g_core_path);
  return 0;
}

/* deinit too: only at exit. DOSBox Pure does not survive retro_deinit()
 * followed by a second retro_init() in the same process (segfault in the
 * second load), and unload_game + load_game leaves the old DOS machine's
 * state behind (a program spinning with the PC speaker on kept sounding
 * into the next game) - both found with tools/dosbench. So each game after
 * the first gets a fresh process: see reexec_for_load(). */
static void core_unload_ex(int deinit)
{
  if (g_loaded) {
    C.unload_game();
    g_loaded = 0;
  }
  if (deinit && g_core_inited) {
    C.deinit();
    g_core_inited = 0;
  }
  memset(g_keys, 0, sizeof g_keys);
  g_mouse_dx = g_mouse_dy = g_mouse_btn = 0;
  g_joy[0] = g_joy[1] = 0;
  memset(g_stick, 0, sizeof g_stick);
  memset(g_trig, 0, sizeof g_trig);
  g_src_fresh = 0;
  g_title[0] = 0;
}
static void core_unload(void) { core_unload_ex(0); }

/* path NULL or "": the bare DOS prompt */
static int core_load(const char *path)
{
  if (core_open() < 0)
    return -1;
  core_unload();
  g_core_exit = 0;
  snprintf(g_last_path, sizeof g_last_path, "%s", path ? path : "");
  if (!g_core_inited) {
    C.set_environment(environment);
    C.set_video_refresh(video_refresh);
    C.set_audio_sample(audio_sample);
    C.set_audio_sample_batch(audio_batch);
    C.set_input_poll(input_poll);
    C.set_input_state(input_state);
    C.init();
    g_core_inited = 1;
  }
  C.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
  C.set_controller_port_device(1, RETRO_DEVICE_JOYPAD);

  struct retro_game_info gi;
  memset(&gi, 0, sizeof gi);
  gi.path = (path && *path) ? path : NULL;
  if (!C.load_game(gi.path ? &gi : NULL)) {
    snprintf(g_status, sizeof g_status, "DOSBox Pure could not load %s", path && *path ? path : "(no content)");
    SAY("%s", g_status);
    core_unload();
    return -1;
  }
  g_loaded = 1;
  g_used = 1;
  /* the core may answer the keyboard only after a game is loaded */
  C.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
  C.set_controller_port_device(1, RETRO_DEVICE_JOYPAD);

  struct retro_system_av_info av;
  memset(&av, 0, sizeof av);
  C.get_system_av_info(&av);
  apply_av_info(&av);
  if (path && *path) {
    const char *b = strrchr(path, '/');
    snprintf(g_title, sizeof g_title, "%s", b && b[1] ? b + 1 : path);
  } else
    snprintf(g_title, sizeof g_title, "DOS");
  g_status[0] = 0;
  V.full = 1;
  SAY("loaded %s, %.3f fps", path && *path ? path : "(DOS prompt)", g_fps);
  return 0;
}

/* --------------------------------------------------------- frame path --- */

static void build_map(void)
{
  if (V.map_sw == g_src_w && V.map_sh == g_src_h && V.map_vw == (unsigned)V.w &&
      V.map_vh == (unsigned)V.h && V.xmap)
    return;
  free(V.xmap);
  free(V.ymap);
  V.xmap = V.ymap = NULL;
  int sw = (int)g_src_w, sh = (int)g_src_h;
  double a = g_aspect > 0.1 ? g_aspect : (double)sw / sh;
  int dw = V.w, dh = (int)(V.w / a + 0.5);
  if (dh > V.h) { dh = V.h; dw = (int)(V.h * a + 0.5); }
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;
  if (dw > V.w) dw = V.w;
  V.dw = dw; V.dh = dh;
  V.dx = (V.w - dw) / 2;
  V.dy = (V.h - dh) / 2;
  V.xmap = malloc(sizeof(int) * (size_t)dw);
  V.ymap = malloc(sizeof(int) * (size_t)dh);
  if (!V.xmap || !V.ymap)
    return;
  for (int x = 0; x < dw; x++) V.xmap[x] = (int)((int64_t)x * sw / dw);
  for (int y = 0; y < dh; y++) V.ymap[y] = (int)((int64_t)y * sh / dh);
  V.map_sw = g_src_w; V.map_sh = g_src_h;
  V.map_vw = (unsigned)V.w; V.map_vh = (unsigned)V.h;
  V.full = 1;
  DBG("scale %ux%u -> %dx%d at %d,%d in %dx%d", g_src_w, g_src_h, dw, dh, V.dx, V.dy, V.w, V.h);
}

static inline void put_px(uint8_t *d, uint32_t rgb, int bpp)
{
  if (bpp == 32) {
    d[0] = 0;
    d[1] = (uint8_t)(rgb >> 16);
    d[2] = (uint8_t)(rgb >> 8);
    d[3] = (uint8_t)rgb;
  } else {
    uint16_t v = (uint16_t)(((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f));
    d[0] = (uint8_t)(v >> 8);
    d[1] = (uint8_t)v;
  }
}

static inline uint32_t src_px(const uint8_t *row, int x)
{
  if (g_src_bpp == 4)
    return ((const uint32_t *)row)[x] & 0xFFFFFFu;
  uint16_t p = ((const uint16_t *)row)[x];
  if (g_pixfmt == RETRO_PIXEL_FORMAT_0RGB1555) {
    uint32_t r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
  }
  uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
  return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

static int surface_free(void)
{
  return g_surf && g_surf->consumed == g_surf->serial;
}

static void publish(void)
{
  if (!V.active || !V.visible || !g_src_fresh || !surface_free() || !g_src_w)
    return;
  build_map();
  if (!V.xmap || !V.ymap)
    return;
  int bpp = V.bpp, bytes = bpp / 8;
  size_t stride = (size_t)V.w * (size_t)bytes;
  int y0, y1;                           /* view rows to repaint, inclusive */

  if (V.full || g_prev_w != g_src_w || g_prev_h != g_src_h) {
    /* the whole view: borders black, picture converted */
    memset(g_pixels, 0, stride * (size_t)V.h);
    y0 = 0;
    y1 = V.h - 1;
  } else {
    /* the band of source rows that changed since the last publish */
    int s0 = -1, s1 = -1;
    for (unsigned y = 0; y < g_src_h; y++)
      if (memcmp(g_src + (size_t)y * g_src_pitch, g_prev + (size_t)y * g_src_pitch, g_src_pitch)) {
        if (s0 < 0) s0 = (int)y;
        s1 = (int)y;
      }
    g_src_fresh = 0;
    if (s0 < 0)
      return;                           /* identical: nothing to send */
    y0 = -1; y1 = -1;
    for (int y = 0; y < V.dh; y++)
      if (V.ymap[y] >= s0 && V.ymap[y] <= s1) {
        if (y0 < 0) y0 = y;
        y1 = y;
      }
    if (y0 < 0)
      return;
    y0 += V.dy;
    y1 += V.dy;
  }

  double t = now_s();
  for (int vy = y0; vy <= y1; vy++) {
    int py = vy - V.dy;
    if (py < 0 || py >= V.dh)
      continue;
    const uint8_t *srow = g_src + (size_t)V.ymap[py] * g_src_pitch;
    uint8_t *d = g_pixels + (size_t)vy * stride + (size_t)V.dx * bytes;
    for (int x = 0; x < V.dw; x++, d += bytes)
      put_px(d, src_px(srow, V.xmap[x]), bpp);
  }
  memcpy(g_prev, g_src, (size_t)g_src_pitch * g_src_h);
  g_prev_w = g_src_w;
  g_prev_h = g_src_h;
  g_src_fresh = 0;
  V.full = 0;

  g_surf->w = (uint32_t)V.w;
  g_surf->h = (uint32_t)V.h;
  g_surf->bpp = (uint32_t)bpp;
  g_surf->stride = (uint32_t)stride;
  g_surf->damage[0] = 0;
  g_surf->damage[1] = y0;
  g_surf->damage[2] = V.w;
  g_surf->damage[3] = y1 - y0 + 1;
  g_surf->src_w = g_src_w;
  g_surf->src_h = g_src_h;
  g_surf->frames_dropped = g_dropped;
  g_surf->core_us = g_core_us;
  (void)t;
  __sync_synchronize();
  g_surf->serial++;
}

/* ------------------------------------------------------- shared memory -- */

static int shm_create(void)
{
  size_t hdr = 4096;
  size_t surf = 4096;
  size_t pix = (size_t)PSDOS_MAX_W * PSDOS_MAX_H * 4;
  size_t aud = (sizeof(struct psdos_audio) + 4095) & ~(size_t)4095;
  g_shm_size = hdr + surf + pix + aud;
  g_shm_fd = memfd_create("psdos", MFD_CLOEXEC);
  if (g_shm_fd < 0) {
    perror("memfd_create");
    return -1;
  }
  if (ftruncate(g_shm_fd, (off_t)g_shm_size) < 0) {
    perror("ftruncate");
    return -1;
  }
  g_shm = mmap(NULL, g_shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
  if (g_shm == MAP_FAILED) {
    perror("mmap");
    g_shm = NULL;
    return -1;
  }
  struct psdos_shm_hdr *h = (struct psdos_shm_hdr *)g_shm;
  h->magic = PSDOS_SHM_MAGIC;
  h->version = PSDOS_PROTO_VERSION;
  h->size = (uint32_t)g_shm_size;
  h->surface_off = (uint32_t)hdr;
  h->pixels_off = (uint32_t)(hdr + surf);
  h->pixels_bytes = (uint32_t)pix;
  h->audio_off = (uint32_t)(hdr + surf + pix);
  h->audio_bytes = (uint32_t)aud;
  g_surf = (struct psdos_surface *)(g_shm + h->surface_off);
  g_pixels = g_shm + h->pixels_off;
  g_audio = (struct psdos_audio *)(g_shm + h->audio_off);
  return 0;
}

/* ------------------------------------------------------------ socket ---- */

static void send_evt_fd(uint32_t type, int32_t a, int32_t b, const char *str, int fd)
{
  if (g_client_fd < 0)
    return;
  struct psdos_evt e;
  e.type = type;
  e.a = a;
  e.b = b;
  e.len = str ? (uint32_t)strnlen(str, PSDOS_STR_MAX - 1) : 0;
  struct iovec iov[2] = { { &e, sizeof e }, { (void *)str, e.len } };
  struct msghdr mh;
  memset(&mh, 0, sizeof mh);
  mh.msg_iov = iov;
  mh.msg_iovlen = e.len ? 2 : 1;
  union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } cm;
  if (fd >= 0) {
    memset(&cm, 0, sizeof cm);
    mh.msg_control = cm.buf;
    mh.msg_controllen = sizeof cm.buf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
  }
  if (sendmsg(g_client_fd, &mh, MSG_NOSIGNAL) < 0)
    DBG("send failed: %s", strerror(errno));
}
static void send_evt(uint32_t type, int32_t a, int32_t b, const char *str)
{
  send_evt_fd(type, a, b, str, -1);
}

static uint32_t cur_flags(void)
{
  uint32_t f = 0;
  if (g_loaded) f |= PSDOS_FL_LOADED;
  if (g_loaded && !V.visible) f |= PSDOS_FL_PAUSED;
  if (!g_dl && g_status[0] && !strncmp(g_status, "core", 4)) f |= PSDOS_FL_NOCORE;
  if (g_core_exit) f |= PSDOS_FL_EXITED;
  if (g_failed) f |= PSDOS_FL_FAILED;
  return f;
}

static char g_title_sent[256], g_status_sent[256];

static void push_state(void)
{
  uint32_t f = cur_flags();
  if (f != g_flags_sent) {
    g_flags_sent = f;
    send_evt(PSDOS_EVT_FLAGS, (int32_t)f, 0, NULL);
  }
  if (strcmp(g_title, g_title_sent)) {
    snprintf(g_title_sent, sizeof g_title_sent, "%s", g_title);
    send_evt(PSDOS_EVT_TITLE, 0, 0, g_title);
  }
  if (strcmp(g_status, g_status_sent)) {
    snprintf(g_status_sent, sizeof g_status_sent, "%s", g_status);
    send_evt(PSDOS_EVT_STATUS, 0, 0, g_status);
  }
}

static uint16_t key_mods(void)
{
  uint16_t m = 0;
  if (g_keys[RETROK_LSHIFT] || g_keys[RETROK_RSHIFT]) m |= RETROKMOD_SHIFT;
  if (g_keys[RETROK_LCTRL] || g_keys[RETROK_RCTRL])   m |= RETROKMOD_CTRL;
  if (g_keys[RETROK_LALT] || g_keys[RETROK_RALT])     m |= RETROKMOD_ALT;
  return m;
}

static void key_event(int down, unsigned code)
{
  if (code == 0 || code >= RETROK_LAST)
    return;
  if (g_keys[code] == (down ? 1 : 0))
    return;                             /* no auto-repeat: DOS does its own */
  g_keys[code] = down ? 1 : 0;
  if (g_kbd_cb && g_loaded)
    g_kbd_cb(down != 0, code, 0, key_mods());
}

static void keys_up(void)
{
  for (unsigned k = 1; k < RETROK_LAST; k++)
    if (g_keys[k])
      key_event(0, k);
  g_mouse_btn = 0;
  g_joy[0] = g_joy[1] = 0;
  memset(g_stick, 0, sizeof g_stick);
  memset(g_trig, 0, sizeof g_trig);
}

static void view_free(void)
{
  core_unload();
  V.active = 0;
  V.visible = 0;
}

/* A second game in this process: replace the process image, keeping the
 * client connection, the listening socket and the shared memory (the
 * emulator's mapping of the memfd stays valid - it is the same object), and
 * hand the view and the path to load over in the environment. */
static void reexec_for_load(const char *path)
{
  char buf[64];
  int fds[3] = { g_client_fd, g_listen_fd, g_shm_fd };
  /* everything the core opened (a game's ZIP, an ISO) must NOT ride
   * along into the next game's process; only our three do */
#ifdef SYS_close_range
  syscall(SYS_close_range, 3u, ~0u, 4u /* CLOSE_RANGE_CLOEXEC */);
#endif
  for (int i = 0; i < 3; i++)
    if (fds[i] >= 0)
      fcntl(fds[i], F_SETFD, 0);          /* survive the exec */
  /* commands that arrived behind the LOAD, and options set at run time */
  {
    static char hex[sizeof g_inbuf * 2 + 1];
    for (size_t i = 0; i < g_inlen && i < sizeof g_inbuf; i++)
      snprintf(hex + i * 2, 3, "%02x", g_inbuf[i]);
    hex[g_inlen * 2] = 0;
    setenv("PSDOS_RESUME_IN", hex, 1);
    static char opts[MAX_OPT * 170];
    size_t n = 0;
    opts[0] = 0;
    for (int i = 0; i < g_nopt; i++)
      if (g_opt[i].from_cmd && n + 170 < sizeof opts)
        n += (size_t)snprintf(opts + n, sizeof opts - n, "%s=%s\n", g_opt[i].key, g_opt[i].val);
    setenv("PSDOS_RESUME_OPTS", opts, 1);
  }
  snprintf(buf, sizeof buf, "%d,%d,%d,%d,%d,%d,%d,%d", g_client_fd, g_listen_fd,
           g_shm_fd, V.active, V.w, V.h, V.bpp, V.visible);
  setenv("PSDOS_RESUME", buf, 1);
  setenv("PSDOS_RESUME_LOAD", path ? path : "", 1);
  setenv("PSDOS_RESUME_SOCK", g_listen_inherited ? "" : g_sock_path, 1);
  if (g_audio)
    g_audio->rate = 0;
  SAY("fresh process for %s", path && *path ? path : "(DOS prompt)");
  execv("/proc/self/exe", g_argv);
  SAY("re-exec failed (%s) - loading in this process", strerror(errno));
  for (int i = 0; i < 3; i++)
    if (fds[i] >= 0)
      fcntl(fds[i], F_SETFD, FD_CLOEXEC);
  unsetenv("PSDOS_RESUME");
}

/* the other half: called first thing in main() */
static int resume_from_exec(void)
{
  const char *r = getenv("PSDOS_RESUME");
  if (!r || !*r)
    return 0;
  int cfd, lfd, sfd, act, w, h, bpp, vis;
  if (sscanf(r, "%d,%d,%d,%d,%d,%d,%d,%d", &cfd, &lfd, &sfd, &act, &w, &h, &bpp, &vis) != 8)
    return 0;
  struct stat st;
  const char *spth = getenv("PSDOS_RESUME_SOCK");
  if (sfd < 0 || fstat(sfd, &st) < 0 ||
      (g_shm = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0)) == MAP_FAILED) {
    /* cannot resume: drop the old client (the emulator reconnects) and
     * the old listener unless systemd owns it, then start afresh */
    g_shm = NULL;
    if (cfd >= 0) close(cfd);
    if (sfd >= 0) close(sfd);
    if (lfd >= 0 && spth && *spth) close(lfd);
    unsetenv("PSDOS_RESUME");
    return 0;
  }
  g_shm_fd = sfd;
  g_shm_size = (size_t)st.st_size;
  struct psdos_shm_hdr *hd = (struct psdos_shm_hdr *)g_shm;
  g_surf = (struct psdos_surface *)(g_shm + hd->surface_off);
  g_pixels = g_shm + hd->pixels_off;
  g_audio = (struct psdos_audio *)(g_shm + hd->audio_off);
  g_client_fd = cfd;
  g_listen_fd = lfd;
  if (spth && *spth)
    snprintf(g_sock_path, sizeof g_sock_path, "%s", spth);
  else
    g_listen_inherited = 1;
  {
    const char *hx = getenv("PSDOS_RESUME_IN");
    g_inlen = 0;
    for (; hx && hx[0] && hx[1] && g_inlen < sizeof g_inbuf; hx += 2) {
      unsigned v;
      if (sscanf(hx, "%2x", &v) != 1)
        break;
      g_inbuf[g_inlen++] = (uint8_t)v;
    }
    const char *op = getenv("PSDOS_RESUME_OPTS");
    char line[256];
    while (op && *op) {
      size_t l = strcspn(op, "\n");
      if (l < sizeof line) {
        memcpy(line, op, l);
        line[l] = 0;
        opt_parse_line(line, 1);
      }
      op += l;
      if (*op) op++;
    }
    g_opt_dirty = 0;                    /* the core reads them fresh */
    unsetenv("PSDOS_RESUME_IN");
    unsetenv("PSDOS_RESUME_OPTS");
  }
  int fds[3] = { cfd, lfd, sfd };
  for (int i = 0; i < 3; i++)
    if (fds[i] >= 0)
      fcntl(fds[i], F_SETFD, FD_CLOEXEC);
  V.active = act; V.w = w; V.h = h; V.bpp = bpp; V.visible = vis;
  V.full = 1;
  g_surf->consumed = g_surf->serial;
  char path[PATH_MAX];
  const char *lp = getenv("PSDOS_RESUME_LOAD");
  snprintf(path, sizeof path, "%s", lp ? lp : "");
  unsetenv("PSDOS_RESUME");
  unsetenv("PSDOS_RESUME_LOAD");
  unsetenv("PSDOS_RESUME_SOCK");
  if (core_open() == 0)
    g_failed = core_load(path) < 0;
  g_flags_sent = 0xFFFFFFFFu;          /* say everything again */
  g_resume_pending_input = g_inlen > 0;
  return 1;
}

static void handle_cmd(const struct psdos_cmd *c, const char *str)
{
  switch (c->type) {
    case PSDOS_CMD_HELLO:
      if (c->a != PSDOS_PROTO_VERSION) {
        SAY("client speaks protocol %d, this psdos %d", c->a, PSDOS_PROTO_VERSION);
        send_evt(PSDOS_EVT_HELLO, PSDOS_PROTO_VERSION, 0, NULL);
        return;
      }
      send_evt_fd(PSDOS_EVT_HELLO, PSDOS_PROTO_VERSION, (int32_t)g_shm_size, NULL, g_shm_fd);
      g_flags_sent = 0xFFFFFFFFu;
      g_title_sent[0] = g_status_sent[0] = 0;
      push_state();
      break;
    case PSDOS_CMD_VIEW_NEW:
    case PSDOS_CMD_VIEW_SIZE: {
      int w = c->a, h = c->b, bpp = c->type == PSDOS_CMD_VIEW_NEW ? c->c : V.bpp;
      if (w > PSDOS_MAX_W) w = PSDOS_MAX_W;     /* a bigger window: DOSGEM centres */
      if (h > PSDOS_MAX_H) h = PSDOS_MAX_H;
      if (w < 16 || h < 16 || (bpp != 16 && bpp != 32)) {
        send_evt(PSDOS_EVT_VIEW, 0, 0, NULL);
        return;
      }
      V.w = w; V.h = h; V.bpp = bpp;
      V.active = 1;
      if (c->type == PSDOS_CMD_VIEW_NEW)
        V.visible = 1;
      V.full = 1;
      V.map_vw = 0;                     /* rebuild the scale map */
      g_src_fresh = g_src_w != 0;       /* republish the last frame at the new size */
      g_surf->consumed = g_surf->serial;
      send_evt(PSDOS_EVT_VIEW, 1, 0, NULL);
      break;
    }
    case PSDOS_CMD_VIEW_FREE:
      view_free();
      break;
    case PSDOS_CMD_STATE: {
      int vis = (c->a & 1) != 0;
      if (vis && !V.visible) {
        V.full = 1;
        g_src_fresh = g_src_w != 0;
      }
      if (!vis || !(c->a & 2))
        keys_up();                      /* nothing held off-screen or
                                         * behind another window */
      V.visible = vis;
      break;
    }
    case PSDOS_CMD_LOAD:
      if (g_used)
        reexec_for_load(str);           /* returns only if exec failed */
      g_failed = core_load(str) < 0;
      break;
    /* RESET of a machine that shut down (or never loaded): load it again */
    case PSDOS_CMD_KEY:
      key_event(c->a, (unsigned)c->b);
      break;
    case PSDOS_CMD_MOUSE:
      g_mouse_dx += c->a;
      g_mouse_dy += c->b;
      g_mouse_btn = c->c & 7;
      break;
    case PSDOS_CMD_JOY:
      if (c->a == 0 || c->a == 1)
        g_joy[c->a] = (uint16_t)c->b;
      break;
    case PSDOS_CMD_PAD:
      if (c->a == 0 || c->a == 1) {
        int p = c->a;
        g_joy[p] = (uint16_t)c->b;
        g_stick[p][0] = (int16_t)((uint32_t)c->c >> 16);
        g_stick[p][1] = (int16_t)(c->c & 0xffff);
        g_stick[p][2] = (int16_t)((uint32_t)c->d >> 16);
        g_stick[p][3] = (int16_t)(c->d & 0xffff);
        g_trig[p][0]  = (int16_t)((uint32_t)c->e >> 16);
        g_trig[p][1]  = (int16_t)(c->e & 0xffff);
      }
      break;
    case PSDOS_CMD_RESET:
      if (g_loaded && !g_core_exit)
        C.reset();
      else {
        char again[PATH_MAX];
        snprintf(again, sizeof again, "%s", g_last_path);
        if (g_used)
          reexec_for_load(again);
        g_failed = core_load(again) < 0;
      }
      break;
    case PSDOS_CMD_OPTION:
      if (str) {
        char line[256];
        snprintf(line, sizeof line, "%s", str);
        opt_parse_line(line, 1);
      }
      break;
    case PSDOS_CMD_KEYS_UP:
      keys_up();
      break;
    case PSDOS_CMD_QUIT:
      g_quit = 1;
      break;
    default:
      DBG("unknown command %u", c->type);
      break;
  }
}

static void client_drop(void)
{
  if (g_client_fd >= 0) {
    close(g_client_fd);
    g_client_fd = -1;
  }
  view_free();
  g_inlen = 0;
  g_idle_since = now_s();
  SAY("client gone");
}

static void client_parse(void);

static void client_read(void)
{
  ssize_t n = recv(g_client_fd, g_inbuf + g_inlen, sizeof g_inbuf - g_inlen, 0);
  if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
    client_drop();
    return;
  }
  if (n < 0)
    return;
  g_inlen += (size_t)n;
  client_parse();
}

static void client_parse(void)
{
  for (;;) {
    if (g_inlen < sizeof(struct psdos_cmd))
      break;
    struct psdos_cmd c;
    memcpy(&c, g_inbuf, sizeof c);
    if (c.len > PSDOS_STR_MAX) {
      SAY("bad command length %u", c.len);
      client_drop();
      return;
    }
    size_t need = sizeof c + c.len;
    if (g_inlen < need)
      break;
    char str[PSDOS_STR_MAX + 1];
    memcpy(str, g_inbuf + sizeof c, c.len);
    str[c.len] = 0;
    /* drop the record BEFORE acting on it: a LOAD or RESET may re-exec,
     * and what is left in g_inbuf then travels to the new process */
    memmove(g_inbuf, g_inbuf + need, g_inlen - need);
    g_inlen -= need;
    handle_cmd(&c, c.len ? str : NULL);
    if (g_client_fd < 0)
      return;
  }
}

static int listen_setup(void)
{
  const char *lf = getenv("LISTEN_FDS"), *lp = getenv("LISTEN_PID");
  if (lf && lp && atoi(lf) >= 1 && atoi(lp) == (int)getpid()) {
    g_listen_fd = 3;
    g_listen_inherited = 1;
    fcntl(g_listen_fd, F_SETFD, FD_CLOEXEC);
    SAY("listening on the socket systemd handed over");
    return 0;
  }
  const char *p = getenv("PSDOS_SOCK");
  snprintf(g_sock_path, sizeof g_sock_path, "%s", (p && *p) ? p : PSDOS_SOCK_DEFAULT);
  g_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (g_listen_fd < 0) {
    perror("socket");
    return -1;
  }
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof sa.sun_path, "%s", g_sock_path);
  unlink(g_sock_path);
  if (bind(g_listen_fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(g_listen_fd, 2) < 0) {
    fprintf(stderr, "[psdos] %s: %s\n", g_sock_path, strerror(errno));
    return -1;
  }
  chmod(g_sock_path, 0666);
  SAY("listening on %s", g_sock_path);
  return 0;
}

/* ---------------------------------------------------------------- main -- */

static void set_affinity(void)
{
  const char *e = getenv("PSDOS_CPUS");
  unsigned long mask = (e && *e) ? strtoul(e, NULL, 16) : 0x2;   /* core 1 */
  long n = sysconf(_SC_NPROCESSORS_CONF);
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int i = 0; i < 64 && i < n; i++)
    if (mask & (1UL << i)) CPU_SET(i, &set);
  if (CPU_COUNT(&set) == 0)
    return;
  if (sched_setaffinity(0, sizeof set, &set) < 0)
    DBG("affinity: %s", strerror(errno));
}

static void find_core(char *argv0)
{
  const char *e = getenv("PSDOS_CORE");
  if (e && *e) {
    snprintf(g_core_path, sizeof g_core_path, "%s", e);
    return;
  }
  char self[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  (void)argv0;
  if (n > 0) {
    self[n] = 0;
    char *s = strrchr(self, '/');
    if (s) {
      *s = 0;
      snprintf(g_core_path, sizeof g_core_path, "%s/dosbox_pure_libretro.so", self);
      if (access(g_core_path, R_OK) == 0)
        return;
    }
  }
  snprintf(g_core_path, sizeof g_core_path, "/usr/local/lib/psdos/dosbox_pure_libretro.so");
}

static void make_dirs(void)
{
  const char *h = getenv("PSDOS_HOME");
  if (h && *h)
    snprintf(g_home, sizeof g_home, "%s", h);
  snprintf(g_sysdir, sizeof g_sysdir, "%s/system", g_home);
  snprintf(g_savedir, sizeof g_savedir, "%s/saves", g_home);
  mkdir(g_home, 0755);
  mkdir(g_sysdir, 0755);
  mkdir(g_savedir, 0755);
}

static void on_signal(int s) { (void)s; g_quit = 1; }

/* one frame: run the core, publish what it drew, keep the fps figure */
static void frame(void)
{
  double t = now_s();
  C.run();
  g_core_us = (uint32_t)((now_s() - t) * 1e6);
  if (t - g_fps_t0 >= 1.0) {
    g_fps_x100 = (uint32_t)(g_frames_1s * 100.0 / (t - g_fps_t0));
    g_frames_1s = 0;
    g_fps_t0 = t;
    if (g_surf)
      g_surf->fps_x100 = g_fps_x100;
  }
  publish();
  if (g_audio && g_sample_rate > 0 && g_audio->rate != (uint32_t)(g_sample_rate + 0.5))
    g_audio->rate = (uint32_t)(g_sample_rate + 0.5);
}

static int write_ppm(const char *path)
{
  if (!g_src_w || !g_src)
    return -1;
  FILE *f = fopen(path, "wb");
  if (!f)
    return -1;
  fprintf(f, "P6\n%u %u\n255\n", g_src_w, g_src_h);
  for (unsigned y = 0; y < g_src_h; y++)
    for (unsigned x = 0; x < g_src_w; x++) {
      uint32_t p = src_px(g_src + (size_t)y * g_src_pitch, (int)x);
      fputc((int)(p >> 16) & 255, f);
      fputc((int)(p >> 8) & 255, f);
      fputc((int)p & 255, f);
    }
  fclose(f);
  return 0;
}

static int selftest(const char *path, double secs)
{
  if (core_load(path) < 0)
    return 1;
  double t0 = now_s(), next = t0, worst = 0;
  unsigned frames = 0;
  double busy = 0;
  while (now_s() - t0 < secs && !g_quit && !g_core_exit) {
    double t = now_s();
    C.run();
    double d = now_s() - t;
    busy += d;
    if (d > worst) worst = d;
    frames++;
    next += 1.0 / g_fps;
    double w = next - now_s();
    if (w > 0)
      usleep((useconds_t)(w * 1e6));
    else if (w < -0.25)
      next = now_s();
  }
  double el = now_s() - t0;
  printf("[psdos] selftest: %u frames in %.2f s = %.1f fps (core asks %.2f), "
         "retro_run avg %.2f ms worst %.2f ms, %.0f%% of a core, screen %ux%u\n",
         frames, el, frames / el, g_fps, busy / frames * 1e3, worst * 1e3,
         busy / el * 100.0, g_src_w, g_src_h);
  if (write_ppm("psdos-selftest.ppm") == 0)
    printf("[psdos] last frame -> psdos-selftest.ppm\n");
  core_unload();
  return 0;
}

int main(int argc, char **argv)
{
  g_debug = getenv("PSDOS_DEBUG") != NULL;
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  set_affinity();
  find_core(argv[0]);
  make_dirs();
  opt_defaults();
  const char *idle = getenv("PSDOS_IDLE_S");
  if (idle && *idle)
    g_idle_s = atoi(idle);

  if (argc > 1 && !strcmp(argv[1], "--selftest")) {
    double secs = argc > 3 ? atof(argv[3]) : 5.0;
    if (secs <= 0) secs = 5.0;
    return selftest(argc > 2 ? argv[2] : NULL, secs);
  }

  g_argv = argv;
  if (!resume_from_exec() && (shm_create() < 0 || listen_setup() < 0))
    return 1;
  if (g_resume_pending_input && g_client_fd >= 0)
    client_parse();                     /* what followed the LOAD */
  if (core_open() < 0)
    SAY("continuing without a core: the client is told so");
  g_idle_since = now_s();
  g_fps_t0 = now_s();

  double next = now_s();
  while (!g_quit) {
    int running = g_loaded && V.visible && !g_core_exit;
    double wait = running ? next - now_s() : 0.25;
    if (wait < 0) wait = 0;
    struct pollfd pf[2];
    int n = 0;
    pf[n].fd = g_client_fd >= 0 ? g_client_fd : g_listen_fd;
    pf[n].events = POLLIN;
    pf[n++].revents = 0;
    int pr = poll(pf, (nfds_t)n, (int)(wait * 1000.0));
    if (pr < 0 && errno != EINTR)
      break;
    if (pr > 0 && (pf[0].revents & (POLLIN | POLLHUP | POLLERR))) {
      if (g_client_fd < 0) {
        int fd = accept4(g_listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd >= 0) {
          g_client_fd = fd;
          SAY("client connected");
        }
      } else
        client_read();
    }
    push_state();

    running = g_loaded && V.visible && !g_core_exit;
    if (running && now_s() >= next) {
      frame();
      next += 1.0 / g_fps;
      if (now_s() - next > 0.25)        /* fell far behind: do not race */
        next = now_s();
    } else if (!running)
      next = now_s();

    if (g_client_fd < 0 && g_idle_s > 0 && now_s() - g_idle_since > g_idle_s) {
      SAY("idle for %d s, exiting", g_idle_s);
      break;
    }
  }
  view_free();
  core_unload_ex(1);
  if (g_client_fd >= 0)
    close(g_client_fd);
  if (!g_listen_inherited && g_sock_path[0])
    unlink(g_sock_path);
  return 0;
}
