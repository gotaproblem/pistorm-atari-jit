// SPDX-License-Identifier: MIT

#ifndef PISTORM_ATARI_NETWORK_BACKEND_H
#define PISTORM_ATARI_NETWORK_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pistorm_net_rx_cb)(void *opaque, const uint8_t *frame, size_t len);

typedef struct pistorm_net_backend {
  void *opaque;
  void (*close)(void *opaque);
  int (*send)(void *opaque, const uint8_t *frame, size_t len);
  void (*poll)(void *opaque);
} pistorm_net_backend_t;

int pistorm_net_tap_open(const char *ifname,
                         pistorm_net_rx_cb rx_cb,
                         void *rx_opaque,
                         pistorm_net_backend_t *backend);

int pistorm_net_slirp_open(pistorm_net_rx_cb rx_cb,
                           void *rx_opaque,
                           pistorm_net_backend_t *backend);

#ifdef __cplusplus
}
#endif

#endif /* PISTORM_ATARI_NETWORK_BACKEND_H */
