// SPDX-License-Identifier: MIT
//
// psweb_client.c - the emulator's connection to the psweb engine service.
//
// One connector thread, created the VIDPLAY way (PTHREAD_EXPLICIT_SCHED,
// SCHED_OTHER, core 1 - never inherited from the 68k thread), owns the
// socket and the shared-memory mapping. The NatFeat handler on the CPU
// thread talks to it through a single-producer/single-consumer ring and an
// eventfd, reads state words that the thread keeps current, and copies
// pixels straight out of the shared surface. See psweb-design.md.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
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
#include <unistd.h>

#include "psweb_proto.h"
#include "psweb_client.h"
#include "platforms/atari/pdf/pspdf.h"     /* pspdf_utf8_to_atari / atari_to_utf8 */

/* ------------------------------------------------------------ ring ------ */

#define RING_N 128
struct ring_entry {
  struct psweb_cmd cmd;
  char str[PSWEB_STR_MAX];
};
static struct ring_entry g_ring[RING_N];
static volatile unsigned g_ring_head;   /* producer: CPU thread */
static volatile unsigned g_ring_tail;   /* consumer: connector  */

/* ----------------------------------------------------------- state ------ */

static pthread_t g_thread;
static int g_thread_started;
static int g_efd = -1;
static int g_fd = -1;
static volatile int g_connected;
/* the last thing that went wrong on the link, for the guest (PSWEB_STR_
 * STATUS while not connected): a silent "Connecting..." cost an evening */
static char g_last_err[160];
static int g_hello_fd = -1;       /* the surface fd psweb sent with HELLO */

static void link_err(const char *what, const char *detail)
{
  snprintf(g_last_err, sizeof g_last_err, "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
  fprintf(stderr, "[PSWEB] %s\n", g_last_err);
}
static volatile int g_view_ready;
static volatile int g_debug;

static uint8_t *g_shm;
static size_t g_shm_size;
static struct psweb_surface *volatile g_surf;
static uint8_t *volatile g_pixels;

/* state the handler reads; strings under a mutex, numbers as plain words */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_title[PSWEB_STR_MAX];
static char g_uri[PSWEB_STR_MAX];
static char g_link[PSWEB_STR_MAX];
static volatile uint32_t g_title_serial, g_uri_serial;
static volatile uint32_t g_progress, g_flags;
static volatile int g_link_set;
static volatile uint32_t g_frames_fetched, g_bytes_fetched;   /* PSMON's row */

/* PISTORM_WEB_SOCK names one socket; otherwise the systemd one is tried
 * first (it exists whenever the service is installed, and connecting to it
 * starts psweb), then the by-hand one in /tmp. */
static const char *sock_path(void)
{
  const char *e = getenv("PISTORM_WEB_SOCK");
  if (e && *e)
    return e;
  return access(PSWEB_SOCK_SYSTEMD, F_OK) == 0 ? PSWEB_SOCK_SYSTEMD : PSWEB_SOCK_DEFAULT;
}

/* ------------------------------------------------------- connector ------ */

static void shm_detach(void)
{
  if (g_shm) {
    munmap(g_shm, g_shm_size);
    g_shm = NULL;
  }
  g_surf = NULL;
  g_pixels = NULL;
}

/* The surface: by the descriptor psweb passed with HELLO when it did
 * (then it does not matter who either side runs as), else by name. */
static int shm_attach(const char *name, size_t size)
{
  int fd = g_hello_fd;
  g_hello_fd = -1;
  if (fd < 0) {
    fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
      char d[96];
      snprintf(d, sizeof d, "%s (%s)", name, strerror(errno));
      link_err("cannot open psweb's surface", d);
      return -1;
    }
  }
  uint8_t *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) {
    link_err("cannot map psweb's surface", strerror(errno));
    return -1;
  }
  struct psweb_shm_hdr *h = (struct psweb_shm_hdr *)m;
  if (h->magic != PSWEB_SHM_MAGIC || h->version != PSWEB_PROTO_VERSION ||
      h->size != size) {
    link_err("psweb's surface header does not match this emulator", NULL);
    munmap(m, size);
    return -1;
  }
  g_last_err[0] = 0;
  g_shm = m;
  g_shm_size = size;
  g_pixels = m + h->pixels_off;
  g_surf = (struct psweb_surface *)(m + h->surface_off);
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

static void handle_evt(const struct psweb_evt *e, const char *str)
{
  switch (e->type) {
    case PSWEB_EVT_HELLO:
      if (e->a != PSWEB_PROTO_VERSION) {
        char d[64];
        snprintf(d, sizeof d, "psweb %d, emulator %d", e->a, PSWEB_PROTO_VERSION);
        link_err("protocol version mismatch", d);
        if (g_hello_fd >= 0) { close(g_hello_fd); g_hello_fd = -1; }
        return;
      }
      if (shm_attach(str ? str : "", (size_t)e->b) == 0) {
        g_connected = 1;
        fprintf(stderr, "[PSWEB] connected to psweb (%s)\n", str ? str : "?");
      }
      break;
    case PSWEB_EVT_VIEW:
      g_view_ready = e->a > 0;
      break;
    case PSWEB_EVT_TITLE:
      pthread_mutex_lock(&g_lock);
      snprintf(g_title, sizeof g_title, "%s", str ? str : "");
      g_title_serial++;
      pthread_mutex_unlock(&g_lock);
      break;
    case PSWEB_EVT_URI:
      pthread_mutex_lock(&g_lock);
      snprintf(g_uri, sizeof g_uri, "%s", str ? str : "");
      g_uri_serial++;
      pthread_mutex_unlock(&g_lock);
      break;
    case PSWEB_EVT_LINK:
      pthread_mutex_lock(&g_lock);
      snprintf(g_link, sizeof g_link, "%s", str ? str : "");
      g_link_set = g_link[0] != 0;
      pthread_mutex_unlock(&g_lock);
      break;
    case PSWEB_EVT_PROGRESS:
      g_progress = (uint32_t)e->a;
      break;
    case PSWEB_EVT_FLAGS:
      g_flags = (uint32_t)e->a;
      break;
    case PSWEB_EVT_CRASHED:
      fprintf(stderr, "[PSWEB] the web process died (%d)\n", e->a);
      break;
    default:
      break;
  }
}

static void session(void)
{
  uint8_t buf[sizeof(struct psweb_evt) + PSWEB_STR_MAX + 16];
  size_t len = 0;

  struct psweb_cmd hello;
  memset(&hello, 0, sizeof hello);
  hello.type = PSWEB_CMD_HELLO;
  hello.a = PSWEB_PROTO_VERSION;
  if (send_all(g_fd, &hello, sizeof hello) < 0)
    return;

  for (;;) {
    struct pollfd pf[2] = {
      { g_fd, POLLIN, 0 },
      { g_efd, POLLIN, 0 },
    };
    if (poll(pf, 2, 1000) < 0) {
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
      if (n == 0) { link_err("psweb closed the connection", NULL); return; }
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        link_err("psweb link read failed", strerror(errno));
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
        if (len < sizeof(struct psweb_evt))
          break;
        struct psweb_evt e;
        memcpy(&e, buf, sizeof e);
        if (e.len > PSWEB_STR_MAX)
          return;
        size_t need = sizeof e + e.len;
        if (len < need)
          break;
        char str[PSWEB_STR_MAX + 1];
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
        shm_detach();
        close(fd);
        g_fd = -1;
        if (g_hello_fd >= 0) { close(g_hello_fd); g_hello_fd = -1; }
        fprintf(stderr, "[PSWEB] disconnected from psweb\n");
      } else {
        if (errno != ENOENT) {
          char d[160];
          snprintf(d, sizeof d, "%s (%s)", sa.sun_path, strerror(errno));
          link_err("cannot connect to psweb's socket", d);
        }
        close(fd);
      }
    }
    sleep(1);
  }
  return NULL;
}

/* Same rule as pspdf.cpp's worker: core 1, and nothing inherited. */
static int start_thread(void)
{
  if (g_thread_started)
    return 0;
  g_debug = getenv("PISTORM_WEB_DEBUG") != NULL;
  g_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (g_efd < 0)
    return -1;

  cpu_set_t set;
  CPU_ZERO(&set);
  const char *e = getenv("PISTORM_WEB_CPUS");
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
    fprintf(stderr, "[PSWEB] cannot start the connector thread: %s\n", strerror(rc));
    return -1;
  }
  pthread_detach(g_thread);
  g_thread_started = 1;
  return 0;
}

/* ------------------------------------------------ handler-side API ------ */

int psweb_status(void)
{
  start_thread();
  int s = 0;
  if (access(sock_path(), F_OK) == 0) s |= 1;
  if (g_connected) s |= 2;
  if (g_connected && g_view_ready) s |= 4;
  return s;
}

int psweb_cmd(uint32_t type, int32_t a, int32_t b, int32_t c, int32_t d,
              int32_t e, int32_t f, const char *str_atari)
{
  start_thread();
  if (!g_connected)
    return PSWEB_NOTCONN;
  unsigned head = g_ring_head;
  if (head - g_ring_tail >= RING_N)
    return PSWEB_BUSY;                 /* the connector is behind; drop it */
  struct ring_entry *en = &g_ring[head % RING_N];
  en->cmd.type = type;
  en->cmd.view = 1;
  en->cmd.a = a; en->cmd.b = b; en->cmd.c = c;
  en->cmd.d = d; en->cmd.e = e; en->cmd.f = f;
  en->cmd.len = 0;
  if (str_atari && *str_atari) {
    pspdf_atari_to_utf8(str_atari, en->str, sizeof en->str);
    en->cmd.len = (uint32_t)strnlen(en->str, sizeof en->str);
  }
  __sync_synchronize();
  g_ring_head = head + 1;
  uint64_t one = 1;
  if (write(g_efd, &one, sizeof one) < 0) { /* the thread polls anyway */ }
  return PSWEB_OK;
}

int psweb_poll(struct psweb_pollstate *out)
{
  memset(out, 0, sizeof *out);
  if (!g_connected)
    return PSWEB_NOTCONN;
  struct psweb_surface *s = g_surf;
  if (s) {
    out->frame_serial = s->serial;
    out->n_damage = s->serial != s->consumed ? s->n_damage : 0;
  }
  out->progress = g_progress;
  out->flags = g_flags;
  out->cursor = g_link_set ? 1 : 0;
  out->title_serial = g_title_serial;
  out->uri_serial = g_uri_serial;
  out->dialog = 0;
  return PSWEB_OK;
}

int psweb_fetch(uint8_t *dst, uint32_t dst_stride, uint32_t dst_rows,
                int32_t *rects, int max_rects)
{
  struct psweb_surface *s = g_surf;
  if (!g_connected || !s)
    return PSWEB_NOTCONN;
  uint32_t serial = s->serial;
  if (serial == s->consumed)
    return 0;
  __sync_synchronize();

  uint32_t bpp = s->bpp, bytes = bpp / 8;
  uint32_t sw = s->w, sh = s->h, sstride = s->stride;
  if (sw * bytes > dst_stride) sw = dst_stride / bytes;
  if (sh > dst_rows) sh = dst_rows;

  int n = (int)s->n_damage;
  if (n > PSWEB_DAMAGE_MAX) n = PSWEB_DAMAGE_MAX;
  int out = 0;
  for (int i = 0; i < n; i++) {
    int32_t x = s->damage[i][0], y = s->damage[i][1];
    int32_t w = s->damage[i][2], h = s->damage[i][3];
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)sw) w = (int32_t)sw - x;
    if (y + h > (int32_t)sh) h = (int32_t)sh - y;
    if (w <= 0 || h <= 0)
      continue;
    const uint8_t *src = g_pixels + (size_t)y * sstride + (size_t)x * bytes;
    uint8_t *d = dst + (size_t)y * dst_stride + (size_t)x * bytes;
    size_t row = (size_t)w * bytes;
    for (int32_t r = 0; r < h; r++) {
      memcpy(d, src, row);
      src += sstride;
      d += dst_stride;
    }
    if (rects && out < max_rects) {
      rects[out * 4 + 0] = x;
      rects[out * 4 + 1] = y;
      rects[out * 4 + 2] = w;
      rects[out * 4 + 3] = h;
    }
    out++;
  }
  __sync_synchronize();
  s->consumed = serial;
  g_frames_fetched++;
  g_bytes_fetched += (uint32_t)(sw * sh * bytes);
  return out;
}

/* For the PSCTRL sampler (PSMON's browser row): no thread start, no
 * blocking - plain reads of what the handler keeps. state: 0 no psweb,
 * 1 socket present, 2 connected, 3 a view is live. */
void psweb_stats(uint32_t *state, uint32_t *frames, uint32_t *bytes)
{
  int st = 0;
  if (g_thread_started) {
    if (g_connected && g_view_ready) st = 3;
    else if (g_connected) st = 2;
    else if (access(sock_path(), F_OK) == 0) st = 1;
  }
  *state = (uint32_t)st;
  *frames = g_frames_fetched;
  *bytes = g_bytes_fetched;
}

int psweb_getstr(int which, char *out_atari, int len)
{
  if (!out_atari || len <= 0)
    return PSWEB_ERR;
  out_atari[0] = 0;
  if (which == PSWEB_STR_ENGINE) {
    snprintf(out_atari, (size_t)len, "psweb proto %d, %s", PSWEB_PROTO_VERSION,
             g_connected ? "connected" : "not connected");
    return (int)strlen(out_atari);
  }
  if (which == PSWEB_STR_STATUS) {
    if (!g_connected && g_last_err[0])
      snprintf(out_atari, (size_t)len, "%s", g_last_err);
    else if (!g_connected)
      snprintf(out_atari, (size_t)len, "%s", access(sock_path(), F_OK) == 0 ?
               "connecting to psweb..." : "psweb is not running on the Pi");
    else
      snprintf(out_atari, (size_t)len, "%s", g_view_ready ? "ready" : "no view");
    return (int)strlen(out_atari);
  }
  if (pthread_mutex_trylock(&g_lock) != 0)
    return PSWEB_BUSY;
  const char *src = which == PSWEB_STR_TITLE ? g_title :
                    which == PSWEB_STR_URI ? g_uri :
                    which == PSWEB_STR_LINK ? g_link : "";
  char tmp[PSWEB_STR_MAX];
  snprintf(tmp, sizeof tmp, "%s", src);
  pthread_mutex_unlock(&g_lock);
  pspdf_utf8_to_atari(tmp, out_atari, (size_t)len);
  return (int)strlen(out_atari);
}

void psweb_shutdown(void)
{
  if (g_connected)
    psweb_cmd(PSWEB_CMD_VIEW_FREE, 0, 0, 0, 0, 0, 0, NULL);
}
