// SPDX-License-Identifier: MIT

#include "pistorm_net_backend.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#if defined(HAVE_LIBSLIRP)
#if defined(__has_include)
#if __has_include(<slirp/libslirp.h>)
#include <slirp/libslirp.h>
#else
#include <libslirp.h>
#endif
#else
#include <slirp/libslirp.h>
#endif
#include <arpa/inet.h>
#endif

#define PNET_SLIRP_MAX_POLL 64

#if defined(HAVE_LIBSLIRP)
static int pnet_slirp_debug_enabled(void)
{
  const char *debug = getenv("PISTORM_NET_DEBUG");
  return debug && debug[0] && strcmp(debug, "0") != 0;
}

typedef struct pnet_slirp_timer {
  void *owner;
  SlirpTimerCb cb;
  void *cb_opaque;
  int64_t expire_ms;
  struct pnet_slirp_timer *next;
} pnet_slirp_timer_t;

typedef struct pnet_slirp {
  Slirp *slirp;
  int running;
  pthread_t thread;
  pthread_mutex_t lock;
  SlirpCb cb;
  SlirpConfig cfg;
  pistorm_net_rx_cb rx_cb;
  void *rx_opaque;
  struct pollfd pollfds[PNET_SLIRP_MAX_POLL];
  size_t poll_count;
  pnet_slirp_timer_t *timers;
} pnet_slirp_t;

static int64_t pnet_slirp_now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int64_t pnet_slirp_now_ms(void)
{
  return pnet_slirp_now_ns() / 1000000LL;
}

static short pnet_slirp_to_poll_events(int events)
{
  short out = 0;
  if (events & SLIRP_POLL_IN)
    out |= POLLIN;
  if (events & SLIRP_POLL_OUT)
    out |= POLLOUT;
  if (events & SLIRP_POLL_PRI)
    out |= POLLPRI;
  if (events & SLIRP_POLL_ERR)
    out |= POLLERR;
  if (events & SLIRP_POLL_HUP)
    out |= POLLHUP;
  return out;
}

static int pnet_slirp_from_poll_revents(short revents)
{
  int out = 0;
  if (revents & POLLIN)
    out |= SLIRP_POLL_IN;
  if (revents & POLLOUT)
    out |= SLIRP_POLL_OUT;
  if (revents & POLLPRI)
    out |= SLIRP_POLL_PRI;
  if (revents & POLLERR)
    out |= SLIRP_POLL_ERR;
  if (revents & POLLHUP)
    out |= SLIRP_POLL_HUP;
  return out;
}

static slirp_ssize_t pnet_slirp_send_packet(const void *buf, size_t len, void *opaque)
{
  pnet_slirp_t *ctx = (pnet_slirp_t *)opaque;

  if (!ctx || !buf || len == 0)
    return -1;

  if (ctx->rx_cb)
    ctx->rx_cb(ctx->rx_opaque, (const uint8_t *)buf, len);
  if (pnet_slirp_debug_enabled())
    fprintf(stderr, "[NET] SLIRP RX len=%zu\n", len);
  return (slirp_ssize_t)len;
}

static void pnet_slirp_guest_error(const char *msg, void *opaque)
{
  (void)opaque;
  fprintf(stderr, "[NET] SLIRP guest error: %s\n", msg ? msg : "(unknown)");
}

static int64_t pnet_slirp_clock_get_ns(void *opaque)
{
  (void)opaque;
  return pnet_slirp_now_ns();
}

static void *pnet_slirp_timer_new(SlirpTimerCb cb, void *cb_opaque, void *opaque)
{
  pnet_slirp_t *ctx = (pnet_slirp_t *)opaque;
  pnet_slirp_timer_t *timer = (pnet_slirp_timer_t *)calloc(1, sizeof(*timer));

  if (!ctx || !timer)
    return NULL;

  timer->owner = ctx;
  timer->cb = cb;
  timer->cb_opaque = cb_opaque;
  timer->expire_ms = -1;
  timer->next = ctx->timers;
  ctx->timers = timer;
  return timer;
}

static void pnet_slirp_timer_free(void *timer_opaque, void *opaque)
{
  pnet_slirp_t *ctx = (pnet_slirp_t *)opaque;
  pnet_slirp_timer_t *timer = (pnet_slirp_timer_t *)timer_opaque;
  pnet_slirp_timer_t **link;

  if (!ctx || !timer)
    return;

  for (link = &ctx->timers; *link; link = &(*link)->next) {
    if (*link == timer) {
      *link = timer->next;
      free(timer);
      return;
    }
  }
}

static void pnet_slirp_timer_mod(void *timer_opaque, int64_t expire_time, void *opaque)
{
  (void)opaque;
  pnet_slirp_timer_t *timer = (pnet_slirp_timer_t *)timer_opaque;
  if (timer)
    timer->expire_ms = expire_time;
}

static void pnet_slirp_notify(void *opaque)
{
  (void)opaque;
}

static int pnet_slirp_add_poll(int fd, int events, void *opaque)
{
  pnet_slirp_t *ctx = (pnet_slirp_t *)opaque;
  size_t idx;

  if (!ctx || ctx->poll_count >= PNET_SLIRP_MAX_POLL)
    return -1;

  idx = ctx->poll_count++;
  ctx->pollfds[idx].fd = (int)fd;
  ctx->pollfds[idx].events = pnet_slirp_to_poll_events(events);
  ctx->pollfds[idx].revents = 0;
  return (int)idx;
}

static int pnet_slirp_get_revents(int idx, void *opaque)
{
  pnet_slirp_t *ctx = (pnet_slirp_t *)opaque;

  if (!ctx || idx < 0 || (size_t)idx >= ctx->poll_count)
    return 0;
  return pnet_slirp_from_poll_revents(ctx->pollfds[idx].revents);
}

static void pnet_slirp_run_expired_timers(pnet_slirp_t *ctx)
{
  int64_t now_ms = pnet_slirp_now_ms();
  pnet_slirp_timer_t *timer;

  for (timer = ctx->timers; timer; timer = timer->next) {
    if (timer->expire_ms >= 0 && timer->expire_ms <= now_ms) {
      timer->expire_ms = -1;
      if (timer->cb)
        timer->cb(timer->cb_opaque);
    }
  }
}

static void *pnet_slirp_thread(void *arg)
{
  pnet_slirp_t *ctx = (pnet_slirp_t *)arg;

  while (ctx->running) {
    uint32_t timeout_ms = 50;
    int timeout;
    int rc;

    pthread_mutex_lock(&ctx->lock);
    ctx->poll_count = 0;
    slirp_pollfds_fill(ctx->slirp, &timeout_ms, pnet_slirp_add_poll, ctx);
    pthread_mutex_unlock(&ctx->lock);

    if (timeout_ms > 50)
      timeout_ms = 50;
    timeout = (int)timeout_ms;

    rc = poll(ctx->pollfds, ctx->poll_count, timeout);
    if (rc < 0 && errno == EINTR)
      continue;

    pthread_mutex_lock(&ctx->lock);
    pnet_slirp_run_expired_timers(ctx);
    slirp_pollfds_poll(ctx->slirp, rc < 0, pnet_slirp_get_revents, ctx);
    pthread_mutex_unlock(&ctx->lock);
  }

  return NULL;
}

static int pnet_slirp_send(void *opaque, const uint8_t *frame, size_t len)
{
  pnet_slirp_t *ctx = (pnet_slirp_t *)opaque;

  if (!ctx || !ctx->slirp || !frame || len == 0 || len > INT32_MAX)
    return -1;

  pthread_mutex_lock(&ctx->lock);
  slirp_input(ctx->slirp, frame, (int)len);
  pthread_mutex_unlock(&ctx->lock);
  if (pnet_slirp_debug_enabled())
    fprintf(stderr, "[NET] SLIRP TX len=%zu\n", len);
  return 0;
}

static void pnet_slirp_close(void *opaque)
{
  pnet_slirp_t *ctx = (pnet_slirp_t *)opaque;
  pnet_slirp_timer_t *timer;

  if (!ctx)
    return;

  ctx->running = 0;
  if (ctx->thread)
    pthread_join(ctx->thread, NULL);

  pthread_mutex_lock(&ctx->lock);
  if (ctx->slirp)
    slirp_cleanup(ctx->slirp);
  ctx->slirp = NULL;
  while (ctx->timers) {
    timer = ctx->timers;
    ctx->timers = timer->next;
    free(timer);
  }
  pthread_mutex_unlock(&ctx->lock);

  pthread_mutex_destroy(&ctx->lock);
  free(ctx);
}

static void pnet_slirp_poll(void *opaque)
{
  (void)opaque;
}
#endif

int pistorm_net_slirp_open(pistorm_net_rx_cb rx_cb,
                           void *rx_opaque,
                           pistorm_net_backend_t *backend)
{
  if (!backend)
    return -1;

  memset(backend, 0, sizeof(*backend));

#if !defined(HAVE_LIBSLIRP)
  (void)rx_cb;
  (void)rx_opaque;
  fprintf(stderr, "[NET] SLIRP backend unavailable: build with libslirp development headers\n");
  return -1;
#else
  pnet_slirp_t *ctx = (pnet_slirp_t *)calloc(1, sizeof(*ctx));

  if (!ctx)
    return -1;

  pthread_mutex_init(&ctx->lock, NULL);
  ctx->rx_cb = rx_cb;
  ctx->rx_opaque = rx_opaque;

  memset(&ctx->cfg, 0, sizeof(ctx->cfg));
  /*
   * Only use the original config/callback ABI fields. Some Pi distributions
   * mix newer headers with older libslirp runtimes; advertising the newest
   * config version can make libslirp read callback slots we did not intend to
   * provide.
   */
  ctx->cfg.version = 1;
  ctx->cfg.restricted = 0;
  ctx->cfg.in_enabled = true;
  inet_aton("10.0.2.0", &ctx->cfg.vnetwork);
  inet_aton("255.255.255.0", &ctx->cfg.vnetmask);
  inet_aton("10.0.2.2", &ctx->cfg.vhost);
  inet_aton("10.0.2.15", &ctx->cfg.vdhcp_start);
  inet_aton("10.0.2.3", &ctx->cfg.vnameserver);
  ctx->cfg.vhostname = "pistorm-atari";
  ctx->cfg.if_mtu = 1500;
  ctx->cfg.if_mru = 1500;

  memset(&ctx->cb, 0, sizeof(ctx->cb));
  ctx->cb.send_packet = pnet_slirp_send_packet;
  ctx->cb.guest_error = pnet_slirp_guest_error;
  ctx->cb.clock_get_ns = pnet_slirp_clock_get_ns;
  ctx->cb.timer_new = pnet_slirp_timer_new;
  ctx->cb.timer_free = pnet_slirp_timer_free;
  ctx->cb.timer_mod = pnet_slirp_timer_mod;
  ctx->cb.notify = pnet_slirp_notify;

  pthread_mutex_lock(&ctx->lock);
  ctx->slirp = slirp_new(&ctx->cfg, &ctx->cb, ctx);
  pthread_mutex_unlock(&ctx->lock);
  if (!ctx->slirp) {
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
    fprintf(stderr, "[NET] SLIRP initialization failed\n");
    return -1;
  }

  ctx->running = 1;
  if (pthread_create(&ctx->thread, NULL, pnet_slirp_thread, ctx) != 0) {
    fprintf(stderr, "[NET] failed to start SLIRP thread\n");
    pnet_slirp_close(ctx);
    return -1;
  }

  backend->opaque = ctx;
  backend->close = pnet_slirp_close;
  backend->send = pnet_slirp_send;
  backend->poll = pnet_slirp_poll;

  fprintf(stderr, "[NET] SLIRP backend opened: guest=10.0.2.15 gateway=10.0.2.2 dns=10.0.2.3\n");
  return 0;
#endif
}
