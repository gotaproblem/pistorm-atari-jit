// SPDX-License-Identifier: MIT

#include "pistorm_net_backend.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif

#define PNET_TAP_FRAME_MAX 1518

typedef struct pistorm_net_tap {
  int fd;
  int running;
  pthread_t thread;
  pistorm_net_rx_cb rx_cb;
  void *rx_opaque;
  char ifname[64];
} pistorm_net_tap_t;

#if defined(__linux__)
static void *pnet_tap_thread(void *arg)
{
  pistorm_net_tap_t *tap = (pistorm_net_tap_t *)arg;
  uint8_t frame[PNET_TAP_FRAME_MAX];

  while (tap->running) {
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = tap->fd;
    pfd.events = POLLIN;

    int prc = poll(&pfd, 1, 50);
    if (prc < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (prc == 0)
      continue;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
      break;
    if (!(pfd.revents & POLLIN))
      continue;

    ssize_t len = read(tap->fd, frame, sizeof(frame));
    if (len > 0 && tap->rx_cb)
      tap->rx_cb(tap->rx_opaque, frame, (size_t)len);
  }

  return NULL;
}

static int pnet_tap_send(void *opaque, const uint8_t *frame, size_t len)
{
  pistorm_net_tap_t *tap = (pistorm_net_tap_t *)opaque;

  if (!tap || tap->fd < 0 || !frame || len == 0)
    return -1;

  ssize_t written = write(tap->fd, frame, len);
  if (written < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      fprintf(stderr, "[NET] TAP write failed on %s: %s\n", tap->ifname, strerror(errno));
    return -1;
  }

  return written == (ssize_t)len ? 0 : -1;
}

static void pnet_tap_close(void *opaque)
{
  pistorm_net_tap_t *tap = (pistorm_net_tap_t *)opaque;

  if (!tap)
    return;

  tap->running = 0;
  if (tap->fd >= 0)
    close(tap->fd);
  if (tap->thread)
    pthread_join(tap->thread, NULL);

  free(tap);
}

static void pnet_tap_poll(void *opaque)
{
  (void)opaque;
}

static int pnet_tap_set_up(const char *ifname)
{
  struct ifreq ifr;
  int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

  if (sock < 0) {
    fprintf(stderr, "[NET] cannot open control socket for %s: %s\n",
            ifname, strerror(errno));
    return -1;
  }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
  if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
    fprintf(stderr, "[NET] cannot read TAP flags for %s: %s\n",
            ifname, strerror(errno));
    close(sock);
    return -1;
  }

  ifr.ifr_flags |= IFF_UP;
  if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
    fprintf(stderr, "[NET] cannot bring TAP %s up: %s\n",
            ifname, strerror(errno));
    close(sock);
    return -1;
  }

  close(sock);
  return 0;
}
#endif

int pistorm_net_tap_open(const char *ifname,
                         pistorm_net_rx_cb rx_cb,
                         void *rx_opaque,
                         pistorm_net_backend_t *backend)
{
  if (!backend)
    return -1;

  memset(backend, 0, sizeof(*backend));

#if !defined(__linux__)
  (void)ifname;
  (void)rx_cb;
  (void)rx_opaque;
  fprintf(stderr, "[NET] TAP backend requires Linux\n");
  return -1;
#else
  pistorm_net_tap_t *tap = (pistorm_net_tap_t *)calloc(1, sizeof(*tap));
  if (!tap)
    return -1;

  tap->fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (tap->fd < 0) {
    fprintf(stderr, "[NET] cannot open /dev/net/tun: %s\n", strerror(errno));
    free(tap);
    return -1;
  }

  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
  if (ifname && ifname[0])
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
  else
    strncpy(ifr.ifr_name, "pistorm%d", IFNAMSIZ - 1);

  if (ioctl(tap->fd, TUNSETIFF, &ifr) < 0) {
    fprintf(stderr, "[NET] TUNSETIFF failed for %s: %s\n", ifr.ifr_name, strerror(errno));
    close(tap->fd);
    free(tap);
    return -1;
  }

  strncpy(tap->ifname, ifr.ifr_name, sizeof(tap->ifname) - 1);
  if (pnet_tap_set_up(tap->ifname) == 0)
    fprintf(stderr, "[NET] TAP interface up: %s\n", tap->ifname);
  tap->rx_cb = rx_cb;
  tap->rx_opaque = rx_opaque;
  tap->running = 1;

  if (pthread_create(&tap->thread, NULL, pnet_tap_thread, tap) != 0) {
    fprintf(stderr, "[NET] failed to start TAP thread for %s\n", tap->ifname);
    tap->running = 0;
    close(tap->fd);
    free(tap);
    return -1;
  }

  backend->opaque = tap;
  backend->close = pnet_tap_close;
  backend->send = pnet_tap_send;
  backend->poll = pnet_tap_poll;

  fprintf(stderr, "[NET] TAP opened: %s\n", tap->ifname);
  return 0;
#endif
}
