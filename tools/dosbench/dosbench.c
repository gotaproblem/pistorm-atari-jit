// SPDX-License-Identifier: MIT
//
// dosbench - drive psdos the way the emulator's PSDOS client does, with no
// emulator: connect, take the surface fd from HELLO, ask for a view, load
// content, play a script of keys, copy every published frame out (setting
// consumed, as FETCH does), drain the audio ring, and write what the guest
// would have on screen to a PPM. The test for psdos and the protocol.
//
//   dosbench [-s sock] [-v WxH] [-b 16|32] [-l path] [-t seconds] [-o out.ppm]
//            [script...]
//
// Script words, in order:  wait:S   key:NAME   type:TEXT   shot:FILE.ppm
//   NAME = enter esc space up down left right f1..f10 or a single character
//   type:TEXT presses each character (a-z 0-9 space . \ : - /)
//
// Prints frames received, damage rows, fps the core reported, audio frames
// drained, the rate and the peak level.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../../psdos/libretro.h"
#include "../../platforms/atari/dos/psdos_proto.h"

static int g_fd = -1, g_shm_fd = -1;
static uint8_t *g_shm;
static struct psdos_surface *g_surf;
static uint8_t *g_pix;
static struct psdos_audio *g_aud;
static uint8_t *g_view;
static int g_w = 640, g_h = 480, g_bpp = 32;
static unsigned g_frames, g_damage_rows, g_flags;
static unsigned long g_audio_frames;
static int g_peak;
static char g_title[256], g_status[256];

static double now_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void cmd(uint32_t type, int a, int b, int c, const char *str)
{
  struct psdos_cmd k;
  memset(&k, 0, sizeof k);
  k.type = type; k.a = a; k.b = b; k.c = c;
  k.len = str ? (uint32_t)strlen(str) : 0;
  if (write(g_fd, &k, sizeof k) != (ssize_t)sizeof k ||
      (k.len && write(g_fd, str, k.len) != (ssize_t)k.len)) {
    perror("write");
    exit(1);
  }
}

static uint8_t inbuf[sizeof(struct psdos_evt) + PSDOS_STR_MAX + 16];
static size_t inlen;

static void read_events(void)
{
  struct iovec iov = { inbuf + inlen, sizeof inbuf - inlen };
  union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } cm;
  struct msghdr mh;
  memset(&mh, 0, sizeof mh);
  mh.msg_iov = &iov;
  mh.msg_iovlen = 1;
  mh.msg_control = cm.b;
  mh.msg_controllen = sizeof cm.b;
  ssize_t n = recvmsg(g_fd, &mh, MSG_DONTWAIT);
  if (n == 0) { fprintf(stderr, "psdos closed the connection\n"); exit(1); }
  if (n < 0) return;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
      memcpy(&g_shm_fd, CMSG_DATA(c), sizeof(int));
  inlen += (size_t)n;
  while (inlen >= sizeof(struct psdos_evt)) {
    struct psdos_evt e;
    memcpy(&e, inbuf, sizeof e);
    size_t need = sizeof e + e.len;
    if (inlen < need) break;
    char s[PSDOS_STR_MAX + 1];
    memcpy(s, inbuf + sizeof e, e.len);
    s[e.len] = 0;
    switch (e.type) {
      case PSDOS_EVT_HELLO:
        if (g_shm_fd >= 0 && !g_shm) {
          g_shm = mmap(NULL, (size_t)e.b, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
          if (g_shm == MAP_FAILED) { perror("mmap"); exit(1); }
          struct psdos_shm_hdr *h = (struct psdos_shm_hdr *)g_shm;
          if (h->magic != PSDOS_SHM_MAGIC) { fprintf(stderr, "bad shm magic\n"); exit(1); }
          g_surf = (struct psdos_surface *)(g_shm + h->surface_off);
          g_pix = g_shm + h->pixels_off;
          g_aud = (struct psdos_audio *)(g_shm + h->audio_off);
          g_aud->rpos = g_aud->wpos;
          printf("HELLO proto %d, shm %d bytes\n", e.a, e.b);
        }
        break;
      case PSDOS_EVT_VIEW:   printf("VIEW %s\n", e.a ? "ready" : "refused"); break;
      case PSDOS_EVT_FLAGS:  g_flags = (unsigned)e.a; printf("FLAGS 0x%x\n", e.a); break;
      case PSDOS_EVT_TITLE:  snprintf(g_title, sizeof g_title, "%s", s); printf("TITLE %s\n", s); break;
      case PSDOS_EVT_STATUS: snprintf(g_status, sizeof g_status, "%s", s); printf("STATUS %s\n", s); break;
    }
    memmove(inbuf, inbuf + need, inlen - need);
    inlen -= need;
  }
}

/* what FETCH does */
static void fetch(void)
{
  if (!g_surf || g_surf->serial == g_surf->consumed)
    return;
  uint32_t serial = g_surf->serial;
  __sync_synchronize();
  int bytes = (int)g_surf->bpp / 8;
  int x = g_surf->damage[0], y = g_surf->damage[1], w = g_surf->damage[2], h = g_surf->damage[3];
  if (x < 0 || y < 0 || x + w > g_w || y + h > g_h || g_surf->w != (uint32_t)g_w) {
    fprintf(stderr, "damage %d,%d %dx%d outside the %dx%d view\n", x, y, w, h, g_w, g_h);
    exit(2);
  }
  for (int r = 0; r < h; r++)
    memcpy(g_view + ((size_t)(y + r) * g_w + x) * bytes,
           g_pix + (size_t)(y + r) * g_surf->stride + (size_t)x * bytes, (size_t)w * bytes);
  __sync_synchronize();
  g_surf->consumed = serial;
  g_frames++;
  g_damage_rows += (unsigned)h;
}

static void drain_audio(void)
{
  if (!g_aud)
    return;
  uint32_t w = g_aud->wpos, r = g_aud->rpos;
  if (w - r > PSDOS_AUDIO_FRAMES)
    r = w - PSDOS_AUDIO_FRAMES / 2;
  for (; r != w; r++) {
    uint32_t at = r & (PSDOS_AUDIO_FRAMES - 1);
    int l = g_aud->data[at * 2], rr = g_aud->data[at * 2 + 1];
    if (abs(l) > g_peak) g_peak = abs(l);
    if (abs(rr) > g_peak) g_peak = abs(rr);
    g_audio_frames++;
  }
  g_aud->rpos = r;
}

static void pump(double secs)
{
  double end = now_s() + secs;
  while (now_s() < end) {
    struct pollfd pf = { g_fd, POLLIN, 0 };
    poll(&pf, 1, 5);
    if (pf.revents & POLLIN)
      read_events();
    fetch();
    drain_audio();
  }
}

static void shot(const char *path)
{
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); return; }
  fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
  for (int i = 0; i < g_w * g_h; i++) {
    const uint8_t *p = g_view + (size_t)i * (g_bpp / 8);
    if (g_bpp == 32) { fputc(p[1], f); fputc(p[2], f); fputc(p[3], f); }
    else {
      unsigned v = (unsigned)p[0] << 8 | p[1];
      fputc((int)((v >> 8) & 0xf8), f); fputc((int)((v >> 3) & 0xfc), f); fputc((int)((v << 3) & 0xf8), f);
    }
  }
  fclose(f);
  printf("shot %s\n", path);
}

static unsigned keycode(const char *n)
{
  static const struct { const char *n; unsigned k; } t[] = {
    { "enter", RETROK_RETURN }, { "esc", RETROK_ESCAPE }, { "space", RETROK_SPACE },
    { "up", RETROK_UP }, { "down", RETROK_DOWN }, { "left", RETROK_LEFT }, { "right", RETROK_RIGHT },
    { "f1", RETROK_F1 }, { "f2", RETROK_F2 }, { "f3", RETROK_F3 }, { "f4", RETROK_F4 },
    { "f5", RETROK_F5 }, { "f10", RETROK_F10 }, { "tab", RETROK_TAB }, { "bs", RETROK_BACKSPACE },
  };
  for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
    if (!strcmp(n, t[i].n)) return t[i].k;
  if (strlen(n) == 1) {
    char c = n[0];
    if (c >= 'a' && c <= 'z') return RETROK_a + (unsigned)(c - 'a');
    if (c >= '0' && c <= '9') return RETROK_0 + (unsigned)(c - '0');
    switch (c) {
      case ' ': return RETROK_SPACE; case '.': return RETROK_PERIOD;
      case '\\': return RETROK_BACKSLASH; case '-': return RETROK_MINUS;
      case '/': return RETROK_SLASH;
    }
  }
  return 0;
}

static void press(unsigned k, int shift)
{
  if (!k) return;
  if (shift) { cmd(PSDOS_CMD_KEY, 1, RETROK_LSHIFT, 0, NULL); pump(0.05); }
  cmd(PSDOS_CMD_KEY, 1, (int)k, 0, NULL);
  pump(0.08);
  cmd(PSDOS_CMD_KEY, 0, (int)k, 0, NULL);
  pump(0.05);
  if (shift) { cmd(PSDOS_CMD_KEY, 0, RETROK_LSHIFT, 0, NULL); pump(0.05); }
}

int main(int argc, char **argv)
{
  const char *sock = getenv("PSDOS_SOCK") ? getenv("PSDOS_SOCK") : PSDOS_SOCK_DEFAULT;
  const char *load = NULL, *out = "dosbench.ppm";
  double total = 0;
  int i = 1;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (!strcmp(argv[i], "-s") && i + 1 < argc) sock = argv[++i];
    else if (!strcmp(argv[i], "-v") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &g_w, &g_h);
    else if (!strcmp(argv[i], "-b") && i + 1 < argc) g_bpp = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-l") && i + 1 < argc) load = argv[++i];
    else if (!strcmp(argv[i], "-t") && i + 1 < argc) total = atof(argv[++i]);
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
  }
  g_view = calloc((size_t)g_w * g_h, (size_t)g_bpp / 8);
  g_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
  if (connect(g_fd, (struct sockaddr *)&sa, sizeof sa) < 0) { perror(sock); return 1; }

  double t0 = now_s();
  cmd(PSDOS_CMD_HELLO, PSDOS_PROTO_VERSION, 0, 0, NULL);
  pump(0.3);
  if (!g_shm) { fprintf(stderr, "no surface from HELLO\n"); return 1; }
  cmd(PSDOS_CMD_VIEW_NEW, g_w, g_h, g_bpp, NULL);
  cmd(PSDOS_CMD_LOAD, 0, 0, 0, load);
  pump(1.0);

  for (; i < argc; i++) {
    char *a = argv[i];
    if (!strncmp(a, "wait:", 5)) pump(atof(a + 5));
    else if (!strncmp(a, "key:", 4)) press(keycode(a + 4), 0);
    else if (!strncmp(a, "type:", 5)) {
      for (char *p = a + 5; *p; p++) {
        char s[2] = { *p, 0 };
        if (*p == ':') { press(RETROK_SEMICOLON, 1); continue; }
        if (*p >= 'A' && *p <= 'Z') { s[0] = (char)(*p - 'A' + 'a'); press(keycode(s), 1); continue; }
        press(keycode(s), 0);
      }
    } else if (!strncmp(a, "shot:", 5)) { pump(0.2); shot(a + 5); }
    else if (!strncmp(a, "resize:", 7)) {
      int w, h;
      if (sscanf(a + 7, "%dx%d", &w, &h) == 2) {
        free(g_view);
        g_w = w; g_h = h;
        g_view = calloc((size_t)g_w * g_h, (size_t)g_bpp / 8);
        cmd(PSDOS_CMD_VIEW_SIZE, w, h, 0, NULL);
        pump(0.3);
      }
    } else if (!strncmp(a, "load:", 5)) { cmd(PSDOS_CMD_LOAD, 0, 0, 0, a[5] ? a + 5 : NULL); pump(1.0); }
    else if (!strncmp(a, "pause:", 6)) { cmd(PSDOS_CMD_STATE, atoi(a + 6) ? 0 : 3, 0, 0, NULL); pump(0.1); }
  }
  if (total > 0) {
    double left = total - (now_s() - t0);
    if (left > 0) pump(left);
  }
  pump(0.2);
  shot(out);
  double el = now_s() - t0;
  printf("frames %u in %.1f s (%.1f/s), damage rows %u (%.0f%% of full), core fps %.2f, src %ux%u, "
         "dropped %u, retro_run %u us\n",
         g_frames, el, g_frames / el, g_damage_rows,
         g_frames ? 100.0 * g_damage_rows / ((double)g_frames * g_h) : 0.0,
         g_surf->fps_x100 / 100.0, g_surf->src_w, g_surf->src_h, g_surf->frames_dropped, g_surf->core_us);
  printf("audio %lu frames at %u Hz (%.1f s), peak %d, overruns %u\n",
         g_audio_frames, g_aud->rate, g_aud->rate ? (double)g_audio_frames / g_aud->rate : 0.0,
         g_peak, g_aud->overruns);
  cmd(PSDOS_CMD_VIEW_FREE, 0, 0, 0, NULL);
  pump(0.1);
  return 0;
}
