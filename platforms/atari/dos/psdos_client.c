// SPDX-License-Identifier: MIT
//
// psdos_client.c - the emulator's connection to the psdos DOS service.
//
// The PSWEB client's shape (psweb_client.c): one connector thread, created
// with PTHREAD_EXPLICIT_SCHED / SCHED_OTHER on core 1 and never inheriting
// from the 68k thread, owns the socket and the shared-memory mapping. The
// NatFeat handler and the input paths push command records into a ring and
// kick an eventfd; the handler reads state words and copies pixels straight
// out of the shared surface. The thread also drains psdos's sound ring into
// an SDL3 stream every 10 ms (dmasnd_ext_*). See psdos-design.md.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "psdos/libretro.h"
#include "psdos_proto.h"
#include "psdos_client.h"
#include "platforms/atari/joy_usb.h"
#include "platforms/atari/audio/dmasnd.h"

/* ------------------------------------------------------------ ring ------ */

/* Producers: the CPU thread (NatFeat, real IKBD bytes) and the USB input
 * thread (evdev keys, mouse, pads) - so a mutex around the head, held for
 * a few stores. The connector is the only consumer. */
#define RING_N 256
struct ring_entry {
  struct psdos_cmd cmd;
  char str[PSDOS_STR_MAX];
};
static struct ring_entry g_ring[RING_N];
static volatile unsigned g_ring_head;
static volatile unsigned g_ring_tail;
static pthread_mutex_t g_prod = PTHREAD_MUTEX_INITIALIZER;

/* ----------------------------------------------------------- state ------ */

static pthread_t g_thread;
static volatile int g_thread_started;
static pthread_mutex_t g_start_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_efd = -1;
static int g_fd = -1;
static volatile int g_connected;
static volatile int g_view_ready;
static int g_hello_fd = -1;
static char g_last_err[160];

static uint8_t *g_shm;
static size_t g_shm_size;
static struct psdos_surface *volatile g_surf;
static uint8_t *volatile g_pixels;
static struct psdos_audio *volatile g_audio;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_title[PSDOS_STR_MAX];
static char g_status_txt[PSDOS_STR_MAX];
static volatile uint32_t g_title_serial;
static volatile uint32_t g_flags;
static volatile uint32_t g_frames_fetched;

/* input routing */
static volatile int g_capture;
static volatile int g_focus;
static uint32_t g_capture_serial;     /* __atomic ops */
static unsigned g_audio_rate;

static void link_err(const char *what, const char *detail)
{
  snprintf(g_last_err, sizeof g_last_err, "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
  fprintf(stderr, "[PSDOS] %s\n", g_last_err);
}

static const char *sock_path(void)
{
  const char *e = getenv("PISTORM_DOS_SOCK");
  if (e && *e)
    return e;
  return access(PSDOS_SOCK_SYSTEMD, F_OK) == 0 ? PSDOS_SOCK_SYSTEMD : PSDOS_SOCK_DEFAULT;
}

/* ------------------------------------------------------- connector ------ */

/* The CPU thread may be in the middle of a FETCH when the link drops, so
 * the mapping is never taken away from under it: it is replaced in place
 * by zeroed anonymous memory (serial == consumed == 0, so the handler sees
 * "no new frame"), and the next session maps its surface over the same
 * address. */
static uint8_t *g_dead;            /* the replaced range, reused next time */
static size_t g_dead_size;

static void shm_detach(void)
{
  g_surf = NULL;
  g_pixels = NULL;
  g_audio = NULL;
  __sync_synchronize();
  if (g_shm) {
    if (mmap(g_shm, g_shm_size, PROT_READ | PROT_WRITE,
             MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED) {
      g_dead = g_shm;
      g_dead_size = g_shm_size;
    }
    g_shm = NULL;
  }
}

static int shm_attach(size_t size)
{
  int fd = g_hello_fd;
  g_hello_fd = -1;
  if (fd < 0) {
    link_err("psdos sent no surface", NULL);
    return -1;
  }
  uint8_t *m;
  if (g_dead && g_dead_size == size)
    m = mmap(g_dead, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
  else
    m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m != MAP_FAILED && m == g_dead)
    g_dead = NULL;                          /* reused */
  if (m == MAP_FAILED) {
    link_err("cannot map psdos's surface", strerror(errno));
    return -1;
  }
  struct psdos_shm_hdr *h = (struct psdos_shm_hdr *)m;
  if (h->magic != PSDOS_SHM_MAGIC || h->version != PSDOS_PROTO_VERSION || h->size != size ||
      (size_t)h->surface_off + sizeof(struct psdos_surface) > size ||
      (size_t)h->pixels_off + h->pixels_bytes > size ||
      (size_t)h->audio_off + sizeof(struct psdos_audio) > size) {
    link_err("psdos's surface header does not match this emulator", NULL);
    munmap(m, size);
    return -1;
  }
  g_last_err[0] = 0;
  g_shm = m;
  g_shm_size = size;
  g_pixels = m + h->pixels_off;
  g_audio = (struct psdos_audio *)(m + h->audio_off);
  g_audio->rpos = g_audio->wpos;          /* nothing stale */
  g_surf = (struct psdos_surface *)(m + h->surface_off);
  return 0;
}

static int send_all(int fd, const void *buf, size_t len)
{
  const uint8_t *p = buf;
  while (len) {
    ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) {
        struct pollfd pf = { fd, POLLOUT, 0 };
        poll(&pf, 1, 100);
        continue;
      }
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static int drain_ring(void)
{
  while (g_ring_tail != g_ring_head) {
    struct ring_entry *e = &g_ring[g_ring_tail % RING_N];
    __sync_synchronize();
    if (send_all(g_fd, &e->cmd, sizeof e->cmd) < 0)
      return -1;
    if (e->cmd.len && send_all(g_fd, e->str, e->cmd.len) < 0)
      return -1;
    __sync_synchronize();
    g_ring_tail++;
  }
  return 0;
}

/* psdos's sound ring -> the SDL3 device. The ring is free-running; a
 * reader that fell more than a ring behind (the emulator stalled) skips to
 * the newest half rather than playing stale sound. */
static void drain_audio(void)
{
  struct psdos_audio *a = g_audio;
  if (!a || !a->rate)
    return;
  if (a->rate != g_audio_rate) {
    static unsigned failed_rate;
    if (a->rate == failed_rate) {           /* tried, no device: drop it */
      a->rpos = a->wpos;
      return;
    }
    if (dmasnd_ext_open(a->rate) != 0) {
      failed_rate = a->rate;
      fprintf(stderr, "[PSDOS] no sound device for DOS audio at %u Hz\n", a->rate);
      return;
    }
    failed_rate = 0;
    g_audio_rate = a->rate;
  }
  uint32_t w = a->wpos, r = a->rpos;
  __sync_synchronize();
  if (w - r > PSDOS_AUDIO_FRAMES)
    r = w - PSDOS_AUDIO_FRAMES / 2;
  while (r != w) {
    uint32_t at = r & (PSDOS_AUDIO_FRAMES - 1);
    uint32_t n = w - r;
    if (n > PSDOS_AUDIO_FRAMES - at)
      n = PSDOS_AUDIO_FRAMES - at;        /* up to the wrap */
    dmasnd_ext_write(&a->data[at * 2], n);
    r += n;
  }
  a->rpos = r;
}

static void handle_evt(const struct psdos_evt *e, const char *str)
{
  switch (e->type) {
    case PSDOS_EVT_HELLO:
      if (e->a != PSDOS_PROTO_VERSION) {
        char d[64];
        snprintf(d, sizeof d, "psdos %d, emulator %d", e->a, PSDOS_PROTO_VERSION);
        link_err("protocol version mismatch", d);
        if (g_hello_fd >= 0) { close(g_hello_fd); g_hello_fd = -1; }
        return;
      }
      if (shm_attach((size_t)e->b) == 0) {
        g_connected = 1;
        fprintf(stderr, "[PSDOS] connected to psdos\n");
      }
      break;
    case PSDOS_EVT_VIEW:
      g_view_ready = e->a > 0;
      break;
    case PSDOS_EVT_FLAGS:
      g_flags = (uint32_t)e->a;
      break;
    case PSDOS_EVT_TITLE:
      pthread_mutex_lock(&g_lock);
      snprintf(g_title, sizeof g_title, "%s", str ? str : "");
      g_title_serial++;
      pthread_mutex_unlock(&g_lock);
      break;
    case PSDOS_EVT_STATUS:
      pthread_mutex_lock(&g_lock);
      snprintf(g_status_txt, sizeof g_status_txt, "%s", str ? str : "");
      pthread_mutex_unlock(&g_lock);
      if (str && *str)
        fprintf(stderr, "[PSDOS] psdos: %s\n", str);
      break;
    default:
      break;
  }
}

static void session(void)
{
  uint8_t buf[sizeof(struct psdos_evt) + PSDOS_STR_MAX + 16];
  size_t len = 0;

  struct psdos_cmd hello;
  memset(&hello, 0, sizeof hello);
  hello.type = PSDOS_CMD_HELLO;
  hello.a = PSDOS_PROTO_VERSION;
  if (send_all(g_fd, &hello, sizeof hello) < 0)
    return;

  for (;;) {
    struct pollfd pf[2] = {
      { g_fd, POLLIN, 0 },
      { g_efd, POLLIN, 0 },
    };
    /* 10 ms while sound may be flowing, else 1 s */
    int to = (g_connected && g_view_ready) ? 10 : 1000;
    if (poll(pf, 2, to) < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (pf[1].revents & POLLIN) {
      uint64_t v;
      if (read(g_efd, &v, sizeof v) < 0 && errno != EAGAIN)
        return;
    }
    if (g_connected && drain_ring() < 0)
      return;
    if (g_connected && g_view_ready)
      drain_audio();
    if (pf[0].revents & (POLLHUP | POLLERR))
      return;
    if (pf[0].revents & POLLIN) {
      struct iovec iov = { buf + len, sizeof buf - len };
      union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } cm;
      struct msghdr mh;
      memset(&mh, 0, sizeof mh);
      mh.msg_iov = &iov;
      mh.msg_iovlen = 1;
      mh.msg_control = cm.b;
      mh.msg_controllen = sizeof cm.b;
      ssize_t n = recvmsg(g_fd, &mh, MSG_CMSG_CLOEXEC);
      if (n == 0) { link_err("psdos closed the connection", NULL); return; }
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        link_err("psdos link read failed", strerror(errno));
        return;
      }
      for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
          int fd;
          memcpy(&fd, CMSG_DATA(c), sizeof fd);
          if (g_hello_fd >= 0) close(g_hello_fd);
          g_hello_fd = fd;
        }
      }
      len += (size_t)n;
      for (;;) {
        if (len < sizeof(struct psdos_evt))
          break;
        struct psdos_evt e;
        memcpy(&e, buf, sizeof e);
        if (e.len > PSDOS_STR_MAX)
          return;
        size_t need = sizeof e + e.len;
        if (len < need)
          break;
        char str[PSDOS_STR_MAX + 1];
        memcpy(str, buf + sizeof e, e.len);
        str[e.len] = 0;
        handle_evt(&e, e.len ? str : NULL);
        memmove(buf, buf + need, len - need);
        len -= need;
      }
    }
  }
}

static void *connector(void *arg)
{
  (void)arg;
  for (;;) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
      struct sockaddr_un sa;
      memset(&sa, 0, sizeof sa);
      sa.sun_family = AF_UNIX;
      snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock_path());
      if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        g_fd = fd;
        session();
        g_connected = 0;
        g_view_ready = 0;
        pthread_mutex_lock(&g_prod);
        g_capture = 0;
        __atomic_add_fetch(&g_capture_serial, 1, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&g_prod);
        shm_detach();
        close(fd);
        g_fd = -1;
        if (g_hello_fd >= 0) { close(g_hello_fd); g_hello_fd = -1; }
        pthread_mutex_lock(&g_prod);
        g_ring_tail = g_ring_head;            /* what was queued is stale */
        pthread_mutex_unlock(&g_prod);
        fprintf(stderr, "[PSDOS] disconnected from psdos\n");
      } else {
        if (errno != ENOENT) {
          char d[160];
          snprintf(d, sizeof d, "%s (%s)", sa.sun_path, strerror(errno));
          link_err("cannot connect to psdos's socket", d);
        }
        close(fd);
      }
    }
    sleep(1);
  }
  return NULL;
}

/* core 1, nothing inherited - pspdf.cpp's rule; PISTORM_DOS_CPUS overrides */
static int start_thread(void)
{
  if (g_thread_started)
    return 0;
  pthread_mutex_lock(&g_start_lock);
  if (g_thread_started) {
    pthread_mutex_unlock(&g_start_lock);
    return 0;
  }
  g_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (g_efd < 0) {
    pthread_mutex_unlock(&g_start_lock);
    return -1;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  const char *e = getenv("PISTORM_DOS_CPUS");
  unsigned long mask = (e && *e) ? strtoul(e, NULL, 16) : 0;
  if (mask) {
    for (int i = 0; i < 64 && i < CPU_SETSIZE; i++)
      if (mask & (1UL << i)) CPU_SET(i, &set);
  } else {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    for (long i = 0; i < n && i < CPU_SETSIZE; i++)
      if (i != 2 && !(n >= 4 && (i == 0 || i == 3)))
        CPU_SET((int)i, &set);
  }
  if (CPU_COUNT(&set) == 0)
    CPU_SET(0, &set);

  pthread_attr_t attr;
  struct sched_param sp;
  memset(&sp, 0, sizeof sp);
  pthread_attr_init(&attr);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
  pthread_attr_setschedparam(&attr, &sp);
  pthread_attr_setaffinity_np(&attr, sizeof set, &set);
  int rc = pthread_create(&g_thread, &attr, connector, NULL);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    fprintf(stderr, "[PSDOS] cannot start the connector thread: %s\n", strerror(rc));
    pthread_mutex_unlock(&g_start_lock);
    return -1;
  }
  pthread_detach(g_thread);
  g_thread_started = 1;
  pthread_mutex_unlock(&g_start_lock);
  return 0;
}

/* ------------------------------------------------ handler-side API ------ */

/* lock held by the caller */
static int push_locked(uint32_t type, int32_t a, int32_t b, int32_t c, const char *str);

static int push(uint32_t type, int32_t a, int32_t b, int32_t c, const char *str)
{
  if (!g_connected)
    return PSDOS_NOTCONN;
  pthread_mutex_lock(&g_prod);
  int r = push_locked(type, a, b, c, str);
  pthread_mutex_unlock(&g_prod);
  return r;
}

/* real input: only while captured, checked under the same lock that
 * drops the capture, so no key-down can land after the KEYS_UP */
static int push_input(uint32_t type, int32_t a, int32_t b, int32_t c)
{
  if (!g_connected)
    return PSDOS_NOTCONN;
  pthread_mutex_lock(&g_prod);
  int r = g_capture ? push_locked(type, a, b, c, NULL) : PSDOS_ERR;
  pthread_mutex_unlock(&g_prod);
  return r;
}

static int push_entry(uint32_t type, int32_t a, int32_t b, int32_t c,
                      int32_t d, int32_t e, const char *str)
{
  unsigned head = g_ring_head;
  if (head - g_ring_tail >= RING_N)
    return PSDOS_BUSY;
  struct ring_entry *en = &g_ring[head % RING_N];
  memset(&en->cmd, 0, sizeof en->cmd);
  en->cmd.type = type;
  en->cmd.a = a; en->cmd.b = b; en->cmd.c = c;
  en->cmd.d = d; en->cmd.e = e;
  en->cmd.len = 0;
  if (str && *str) {
    snprintf(en->str, sizeof en->str, "%s", str);
    en->cmd.len = (uint32_t)strnlen(en->str, sizeof en->str);
  }
  __sync_synchronize();
  g_ring_head = head + 1;
  uint64_t one = 1;
  if (write(g_efd, &one, sizeof one) < 0) { /* the thread polls anyway */ }
  return PSDOS_OK;
}

static int push_locked(uint32_t type, int32_t a, int32_t b, int32_t c, const char *str)
{
  return push_entry(type, a, b, c, 0, 0, str);
}

static int push_locked_de(uint32_t type, int32_t a, int32_t b, int32_t c,
                          int32_t d, int32_t e)
{
  return push_entry(type, a, b, c, d, e, NULL);
}

/* pads: while the DOS window is on top, captured or not (a pad cannot
 * steer the desktop, so it needs no click first) */
static int pads_wanted(void)
{
  return g_connected && g_view_ready && g_focus;
}

static int push_pad(uint32_t type, int32_t a, int32_t b, int32_t c,
                    int32_t d, int32_t e)
{
  if (!g_connected)
    return PSDOS_NOTCONN;
  pthread_mutex_lock(&g_prod);
  int r = PSDOS_ERR;
  if (pads_wanted()) {
    r = push_locked_de(type, a, b, c, d, e);
  }
  pthread_mutex_unlock(&g_prod);
  return r;
}

int psdos_status(void)
{
  start_thread();
  int s = 0;
  /* the socket check is a syscall: only while there is no link */
  if (g_connected || access(sock_path(), F_OK) == 0) s |= PSDOS_ST_SOCKET;
  if (g_connected) s |= PSDOS_ST_CONN;
  if (g_connected && g_view_ready) s |= PSDOS_ST_VIEW;
  if (g_connected && (g_flags & PSDOS_FL_LOADED)) s |= PSDOS_ST_LOADED;
  if (g_capture) s |= PSDOS_ST_CAPTURE;
  if (g_flags & PSDOS_FL_NOCORE) s |= PSDOS_ST_NOCORE;
  if (g_flags & PSDOS_FL_FAILED) s |= PSDOS_ST_FAILED;
  if (g_flags & PSDOS_FL_EXITED) s |= PSDOS_ST_EXITED;
  return s;
}

int psdos_cmd(uint32_t type, int32_t a, int32_t b, int32_t c, const char *str)
{
  start_thread();
  if (type == PSDOS_CMD_VIEW_FREE) {
    g_view_ready = 0;
    psdos_set_capture(0);
  }
  return push(type, a, b, c, str);
}

int psdos_poll(struct psdos_pollstate *out)
{
  memset(out, 0, sizeof *out);
  out->capture = g_capture;
  out->capture_serial = __atomic_load_n(&g_capture_serial, __ATOMIC_SEQ_CST);
  if (!g_connected)
    return PSDOS_NOTCONN;
  struct psdos_surface *s = g_surf;
  if (s) {
    out->frame_serial = s->serial;
    out->fps_x100 = s->fps_x100;
    out->src_w = s->src_w;
    out->src_h = s->src_h;
  }
  out->flags = g_flags;
  out->title_serial = g_title_serial;
  return PSDOS_OK;
}

int psdos_fetch(uint8_t *dst, uint32_t dst_stride, uint32_t dst_rows, int32_t rect[4])
{
  struct psdos_surface *s = g_surf;
  if (!g_connected || !s)
    return PSDOS_NOTCONN;
  uint32_t serial = s->serial;
  if (serial == s->consumed)
    return 0;
  __sync_synchronize();
  uint32_t bytes = s->bpp / 8, sstride = s->stride;
  int32_t x = s->damage[0], y = s->damage[1], w = s->damage[2], h = s->damage[3];
  int32_t vw = (int32_t)s->w, vh = (int32_t)s->h;
  if (bytes != 2 && bytes != 4) {
    s->consumed = serial;
    return 0;
  }
  if ((int32_t)(dst_stride / bytes) < vw) vw = (int32_t)(dst_stride / bytes);
  if ((int32_t)dst_rows < vh) vh = (int32_t)dst_rows;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > vw) w = vw - x;
  if (y + h > vh) h = vh - y;
  if (w > 0 && h > 0) {
    const uint8_t *src = g_pixels + (size_t)y * sstride + (size_t)x * bytes;
    uint8_t *d = dst + (size_t)y * dst_stride + (size_t)x * bytes;
    for (int32_t r = 0; r < h; r++) {
      memcpy(d, src, (size_t)w * bytes);
      src += sstride;
      d += dst_stride;
    }
  } else
    w = h = 0;
  __sync_synchronize();
  s->consumed = serial;
  g_frames_fetched++;
  if (rect) { rect[0] = x; rect[1] = y; rect[2] = w; rect[3] = h; }
  return (w > 0 && h > 0) ? 1 : 0;
}

int psdos_getstr(int which, char *out, int len)
{
  if (!out || len <= 0)
    return PSDOS_ERR;
  out[0] = 0;
  if (which == PSDOS_STR_STATUS && !g_connected) {
    if (g_last_err[0])
      snprintf(out, (size_t)len, "%s", g_last_err);
    else
      snprintf(out, (size_t)len, "%s", access(sock_path(), F_OK) == 0 ?
               "connecting to psdos..." : "psdos is not running on the Pi");
    return (int)strlen(out);
  }
  if (pthread_mutex_trylock(&g_lock) != 0)
    return PSDOS_BUSY;
  snprintf(out, (size_t)len, "%s", which == PSDOS_STR_TITLE ? g_title :
           which == PSDOS_STR_STATUS ? g_status_txt : "");
  pthread_mutex_unlock(&g_lock);
  return (int)strlen(out);
}

void psdos_set_capture(int on)
{
  on = on ? 1 : 0;
  if (on && !(g_connected && g_view_ready && g_focus))
    on = 0;                               /* nothing to give it to */
  pthread_mutex_lock(&g_prod);
  if (on == g_capture) {
    pthread_mutex_unlock(&g_prod);
    return;
  }
  g_capture = on;
  __atomic_add_fetch(&g_capture_serial, 1, __ATOMIC_SEQ_CST);
  if (!on && g_connected)
    push_locked(PSDOS_CMD_KEYS_UP, 0, 0, 0, NULL);   /* nothing stays held */
  pthread_mutex_unlock(&g_prod);
  fprintf(stderr, "[PSDOS] input %s\n", on ? "captured (middle mouse button, Scroll Lock, Ctrl+Alt+F12 or ST Undo releases)" : "released");
}

int psdos_view_open(void)
{
  return g_connected && g_view_ready;
}

void psdos_set_focus(int focused)
{
  focused = focused ? 1 : 0;
  pthread_mutex_lock(&g_prod);
  int lost = g_focus && !focused;
  g_focus = focused;
  if (lost && g_connected && !g_capture)   /* pads held at the switch */
    push_locked(PSDOS_CMD_KEYS_UP, 0, 0, 0, NULL);
  pthread_mutex_unlock(&g_prod);
  if (!focused)
    psdos_set_capture(0);                 /* KEYS_UP too, if it was on */
}

int psdos_pads_wanted(void)
{
  return pads_wanted();
}

int psdos_wants_input(void)
{
  return g_capture && g_focus && g_connected && g_view_ready;
}

void psdos_shutdown(void)
{
  if (g_connected)
    push(PSDOS_CMD_VIEW_FREE, 0, 0, 0, NULL);
}

void psdos_stats(uint32_t *state, uint32_t *frames, uint32_t *fps_x100)
{
  uint32_t st = 0;
  if (g_thread_started) {
    if (g_connected && g_view_ready && (g_flags & PSDOS_FL_LOADED)) st = 4;
    else if (g_connected && g_view_ready) st = 3;
    else if (g_connected) st = 2;
    else if (access(sock_path(), F_OK) == 0) st = 1;
  }
  struct psdos_surface *s = g_surf;
  *state = st;
  *frames = g_frames_fetched;
  *fps_x100 = s ? s->fps_x100 : 0;
}

/* ------------------------------------------------------------- input ---- */

/* ST IKBD scancodes -> RETROK. The main block is PC set 1; the ST has no
 * PgUp/PgDn/End, so keypad ( and ) and Help stand in for them. Undo ($61)
 * is not here: it releases the capture. */
static const uint16_t st_to_retrok[128] = {
  [0x01] = RETROK_ESCAPE,
  [0x02] = RETROK_1, [0x03] = RETROK_2, [0x04] = RETROK_3, [0x05] = RETROK_4,
  [0x06] = RETROK_5, [0x07] = RETROK_6, [0x08] = RETROK_7, [0x09] = RETROK_8,
  [0x0A] = RETROK_9, [0x0B] = RETROK_0, [0x0C] = RETROK_MINUS, [0x0D] = RETROK_EQUALS,
  [0x0E] = RETROK_BACKSPACE, [0x0F] = RETROK_TAB,
  [0x10] = RETROK_q, [0x11] = RETROK_w, [0x12] = RETROK_e, [0x13] = RETROK_r,
  [0x14] = RETROK_t, [0x15] = RETROK_y, [0x16] = RETROK_u, [0x17] = RETROK_i,
  [0x18] = RETROK_o, [0x19] = RETROK_p, [0x1A] = RETROK_LEFTBRACKET,
  [0x1B] = RETROK_RIGHTBRACKET, [0x1C] = RETROK_RETURN, [0x1D] = RETROK_LCTRL,
  [0x1E] = RETROK_a, [0x1F] = RETROK_s, [0x20] = RETROK_d, [0x21] = RETROK_f,
  [0x22] = RETROK_g, [0x23] = RETROK_h, [0x24] = RETROK_j, [0x25] = RETROK_k,
  [0x26] = RETROK_l, [0x27] = RETROK_SEMICOLON, [0x28] = RETROK_QUOTE,
  [0x29] = RETROK_BACKQUOTE, [0x2A] = RETROK_LSHIFT, [0x2B] = RETROK_BACKSLASH,
  [0x2C] = RETROK_z, [0x2D] = RETROK_x, [0x2E] = RETROK_c, [0x2F] = RETROK_v,
  [0x30] = RETROK_b, [0x31] = RETROK_n, [0x32] = RETROK_m, [0x33] = RETROK_COMMA,
  [0x34] = RETROK_PERIOD, [0x35] = RETROK_SLASH, [0x36] = RETROK_RSHIFT,
  [0x38] = RETROK_LALT, [0x39] = RETROK_SPACE, [0x3A] = RETROK_CAPSLOCK,
  [0x3B] = RETROK_F1, [0x3C] = RETROK_F2, [0x3D] = RETROK_F3, [0x3E] = RETROK_F4,
  [0x3F] = RETROK_F5, [0x40] = RETROK_F6, [0x41] = RETROK_F7, [0x42] = RETROK_F8,
  [0x43] = RETROK_F9, [0x44] = RETROK_F10,
  [0x47] = RETROK_HOME, [0x48] = RETROK_UP, [0x4A] = RETROK_KP_MINUS,
  [0x4B] = RETROK_LEFT, [0x4D] = RETROK_RIGHT, [0x4E] = RETROK_KP_PLUS,
  [0x50] = RETROK_DOWN, [0x52] = RETROK_INSERT, [0x53] = RETROK_DELETE,
  [0x60] = RETROK_LESS, [0x62] = RETROK_END,
  [0x63] = RETROK_PAGEUP, [0x64] = RETROK_PAGEDOWN,
  [0x65] = RETROK_KP_DIVIDE, [0x66] = RETROK_KP_MULTIPLY,
  [0x67] = RETROK_KP7, [0x68] = RETROK_KP8, [0x69] = RETROK_KP9,
  [0x6A] = RETROK_KP4, [0x6B] = RETROK_KP5, [0x6C] = RETROK_KP6,
  [0x6D] = RETROK_KP1, [0x6E] = RETROK_KP2, [0x6F] = RETROK_KP3,
  [0x70] = RETROK_KP0, [0x71] = RETROK_KP_PERIOD, [0x72] = RETROK_KP_ENTER,
};

#define ST_UNDO 0x61

static unsigned linux_to_retrok(unsigned c)
{
  /* KEY_ESC .. KEY_KPDOT follow PC set 1, the same order as the ST table */
  if (c <= 0x53 && c != KEY_KPASTERISK && c != KEY_NUMLOCK && c != KEY_SCROLLLOCK &&
      !(c >= KEY_KP7 && c <= KEY_KPDOT) && c != KEY_RIGHTALT && st_to_retrok[c])
    return st_to_retrok[c];
  switch (c) {
    case KEY_KPASTERISK: return RETROK_KP_MULTIPLY;
    case KEY_NUMLOCK:    return RETROK_NUMLOCK;
    case KEY_KP7: return RETROK_KP7; case KEY_KP8: return RETROK_KP8; case KEY_KP9: return RETROK_KP9;
    case KEY_KPMINUS: return RETROK_KP_MINUS;
    case KEY_KP4: return RETROK_KP4; case KEY_KP5: return RETROK_KP5; case KEY_KP6: return RETROK_KP6;
    case KEY_KPPLUS: return RETROK_KP_PLUS;
    case KEY_KP1: return RETROK_KP1; case KEY_KP2: return RETROK_KP2; case KEY_KP3: return RETROK_KP3;
    case KEY_KP0: return RETROK_KP0; case KEY_KPDOT: return RETROK_KP_PERIOD;
    case KEY_102ND:      return RETROK_LESS;
    case KEY_F11:        return RETROK_F11;
    case KEY_F12:        return RETROK_F12;
    case KEY_KPENTER:    return RETROK_KP_ENTER;
    case KEY_RIGHTCTRL:  return RETROK_RCTRL;
    case KEY_KPSLASH:    return RETROK_KP_DIVIDE;
    case KEY_SYSRQ:      return RETROK_PRINT;
    case KEY_RIGHTALT:   return RETROK_RALT;
    case KEY_HOME:       return RETROK_HOME;
    case KEY_UP:         return RETROK_UP;
    case KEY_PAGEUP:     return RETROK_PAGEUP;
    case KEY_LEFT:       return RETROK_LEFT;
    case KEY_RIGHT:      return RETROK_RIGHT;
    case KEY_END:        return RETROK_END;
    case KEY_DOWN:       return RETROK_DOWN;
    case KEY_PAGEDOWN:   return RETROK_PAGEDOWN;
    case KEY_INSERT:     return RETROK_INSERT;
    case KEY_DELETE:     return RETROK_DELETE;
    case KEY_PAUSE:      return RETROK_PAUSE;
    case KEY_LEFTMETA:   return RETROK_LSUPER;
    case KEY_RIGHTMETA:  return RETROK_RSUPER;
    case KEY_COMPOSE:    return RETROK_MENU;
    default:             return 0;
  }
}

void psdos_key_st(uint8_t scan, int down)
{
  scan &= 0x7F;
  if (scan == ST_UNDO) {
    if (down && g_capture)
      psdos_set_capture(0);
    return;
  }
  unsigned k = st_to_retrok[scan];
  if (k)
    push_input(PSDOS_CMD_KEY, down ? 1 : 0, (int32_t)k, 0);
}

/* DOSGEM's fallback while NOT captured: GEM key presses */
void psdos_key_gem(uint8_t scan, int down)
{
  unsigned k = st_to_retrok[scan & 0x7F];
  if (k)
    push(PSDOS_CMD_KEY, down ? 1 : 0, (int32_t)k, 0, NULL);
}

void psdos_key_linux(unsigned code, int down)
{
  /* Ctrl+Alt+F12: the release for keyboards with no Scroll Lock */
  static int ctrl_l, ctrl_r, alt_l, alt_r;
  switch (code) {
    case KEY_LEFTCTRL:  ctrl_l = down; break;
    case KEY_RIGHTCTRL: ctrl_r = down; break;
    case KEY_LEFTALT:   alt_l = down; break;
    case KEY_RIGHTALT:  alt_r = down; break;
  }
  if (code == KEY_SCROLLLOCK ||
      (code == KEY_F12 && (ctrl_l || ctrl_r) && (alt_l || alt_r))) {
    if (down)
      psdos_set_capture(0);
    return;
  }
  unsigned k = linux_to_retrok(code);
  if (k)
    push_input(PSDOS_CMD_KEY, down ? 1 : 0, (int32_t)k, 0);
}

void psdos_mouse(int dx, int dy, int st_buttons)
{
  int b = ((st_buttons & 2) ? 1 : 0) | ((st_buttons & 1) ? 2 : 0);
  push_input(PSDOS_CMD_MOUSE, dx, dy, b);
}

/* ST port 1 is the first joystick (pad 0), port 0 the second */
void psdos_joy(int st_port, uint8_t st, uint8_t stpad)
{
  int m = 0;
  if (st & 0x01) m |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
  if (st & 0x02) m |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
  if (st & 0x04) m |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
  if (st & 0x08) m |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
  if ((st & 0x80) || (stpad & 0x01)) m |= 1 << RETRO_DEVICE_ID_JOYPAD_B;
  if (stpad & 0x02) m |= 1 << RETRO_DEVICE_ID_JOYPAD_A;
  if (stpad & 0x04) m |= 1 << RETRO_DEVICE_ID_JOYPAD_Y;
  if (stpad & 0x08) m |= 1 << RETRO_DEVICE_ID_JOYPAD_X;
  if (stpad & 0x10) m |= 1 << RETRO_DEVICE_ID_JOYPAD_START;
  push_pad(PSDOS_CMD_JOY, st_port == 1 ? 0 : 1, m, 0, 0, 0);
}

/* A whole USB pad (joy_usb.c, JOYB_* bits, Xbox names) as a libretro
 * RetroPad: positions, not names - Xbox A (bottom) is RetroPad B, B
 * (right) is A, X (left) is Y, Y (top) is X. 1 = queued. */
int psdos_pad(int pad, unsigned b, const int16_t ax[6])
{
  static const struct { unsigned joyb; int id; } map[] = {
    { JOYB_A,      RETRO_DEVICE_ID_JOYPAD_B },
    { JOYB_TRIG,   RETRO_DEVICE_ID_JOYPAD_B },
    { JOYB_B,      RETRO_DEVICE_ID_JOYPAD_A },
    { JOYB_X,      RETRO_DEVICE_ID_JOYPAD_Y },
    { JOYB_Y,      RETRO_DEVICE_ID_JOYPAD_X },
    { JOYB_LB,     RETRO_DEVICE_ID_JOYPAD_L },
    { JOYB_RB,     RETRO_DEVICE_ID_JOYPAD_R },
    { JOYB_LT,     RETRO_DEVICE_ID_JOYPAD_L2 },
    { JOYB_RT,     RETRO_DEVICE_ID_JOYPAD_R2 },
    { JOYB_L3,     RETRO_DEVICE_ID_JOYPAD_L3 },
    { JOYB_R3,     RETRO_DEVICE_ID_JOYPAD_R3 },
    { JOYB_START,  RETRO_DEVICE_ID_JOYPAD_START },
    { JOYB_SELECT, RETRO_DEVICE_ID_JOYPAD_SELECT },
    { JOYB_DUP,    RETRO_DEVICE_ID_JOYPAD_UP },
    { JOYB_DDOWN,  RETRO_DEVICE_ID_JOYPAD_DOWN },
    { JOYB_DLEFT,  RETRO_DEVICE_ID_JOYPAD_LEFT },
    { JOYB_DRIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT },
  };
  int32_t m = 0;
  for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
    if (b & map[i].joyb)
      m |= 1 << map[i].id;
  if (pad < 0 || pad > 1)
    return 1;                             /* psdos has two ports: dropped */
#define PK(hi, lo) ((int32_t)(((uint32_t)(uint16_t)(hi) << 16) | (uint16_t)(lo)))
  return push_pad(PSDOS_CMD_PAD, pad, m, PK(ax[0], ax[1]), PK(ax[2], ax[3]),
                  PK(ax[4], ax[5])) == PSDOS_OK;
#undef PK
}

/* The real IKBD stream while captured: kbd_usb.c frames the packets and
 * hands us whole ones (see stbox_divert_real_byte); parse them again here.
 * Called on the CPU thread only. */
static uint8_t g_pkt[8];
static int g_pkt_len, g_pkt_need;

static int ikbd_pkt_len(uint8_t h)
{
  switch (h) {
    case 0xF6: return 8;
    case 0xF7: return 6;
    case 0xF8: case 0xF9: case 0xFA: case 0xFB: return 3;
    case 0xFC: return 7;
    case 0xFD: return 3;
    case 0xFE: case 0xFF: return 2;
    default:   return 1;
  }
}

void psdos_ikbd_byte(uint8_t v)
{
  if (g_pkt_len == 0)
    g_pkt_need = ikbd_pkt_len(v);
  if (g_pkt_len < (int)sizeof g_pkt)
    g_pkt[g_pkt_len] = v;
  g_pkt_len++;
  if (g_pkt_len < g_pkt_need)
    return;
  g_pkt_len = 0;

  uint8_t h = g_pkt[0];
  if (h < 0xF6) {                         /* a key: make, or break with bit 7 */
    psdos_key_st(h & 0x7F, !(h & 0x80));
    return;
  }
  if (h >= 0xF8 && h <= 0xFB) {           /* relative mouse */
    int dx = (int8_t)g_pkt[1], dy = (int8_t)g_pkt[2];
    psdos_mouse(dx, dy, h & 3);
    return;
  }
  if (h == 0xFE || h == 0xFF) {           /* joystick 0 / 1 event */
    psdos_joy(h & 1, g_pkt[1], 0);
    return;
  }
  if (h == 0xFD) {                        /* both sticks */
    psdos_joy(0, g_pkt[1], 0);
    psdos_joy(1, g_pkt[2], 0);
  }
  /* F6 status, F7 absolute mouse, FC clock: not for DOS */
}
