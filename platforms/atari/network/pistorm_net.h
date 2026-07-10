// SPDX-License-Identifier: MIT

#ifndef PISTORM_ATARI_NETWORK_H
#define PISTORM_ATARI_NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PISTORM_NET_DEFAULT_BASE 0x00F10000u
#define PISTORM_NET_WINDOW_SIZE  0x00001000u
#define PISTORM_NET_FRAME_MAX    1514u

#define PISTORM_NET_ID           0x504E4554u /* "PNET" */
#define PISTORM_NET_VERSION      1u

#define PISTORM_NET_REG_ID       0x000u
#define PISTORM_NET_REG_VERSION  0x004u
#define PISTORM_NET_REG_STATUS   0x008u
#define PISTORM_NET_REG_IRQ      0x00Cu
#define PISTORM_NET_REG_MAC_HI   0x010u
#define PISTORM_NET_REG_MAC_LO   0x014u
#define PISTORM_NET_REG_MTU      0x018u
#define PISTORM_NET_REG_TX_LEN   0x020u
#define PISTORM_NET_REG_TX_KICK  0x024u
#define PISTORM_NET_REG_RX_LEN   0x028u
#define PISTORM_NET_REG_RX_POP   0x02Cu

#define PISTORM_NET_TX_DATA      0x100u
#define PISTORM_NET_RX_DATA      0x800u

#define PISTORM_NET_STATUS_ENABLED   0x00000001u
#define PISTORM_NET_STATUS_LINK_UP   0x00000002u
#define PISTORM_NET_STATUS_RX_READY  0x00000004u
#define PISTORM_NET_STATUS_TX_BUSY   0x00000008u
#define PISTORM_NET_STATUS_RX_DROP   0x00000010u

typedef enum pistorm_net_backend_kind {
  PISTORM_NET_BACKEND_NONE = 0,
  PISTORM_NET_BACKEND_TAP,
  PISTORM_NET_BACKEND_SLIRP
} pistorm_net_backend_kind_t;

typedef struct pistorm_net_config {
  bool enabled;
  uint32_t base;
  pistorm_net_backend_kind_t backend;
  char ifname[64];
  uint8_t mac[6];
} pistorm_net_config_t;

int pistorm_net_init(const pistorm_net_config_t *config);
void pistorm_net_shutdown(void);
void pistorm_net_reset(void);
void pistorm_net_poll(void);

bool pistorm_net_is_enabled(void);
bool pistorm_net_link_up(void);
void pistorm_net_get_mac(uint8_t mac[6]);
uint32_t pistorm_net_rx_pending_mask(void);
uint32_t pistorm_net_rx_length(void);
uint32_t pistorm_net_read_frame(uint8_t *buffer, uint32_t max_len);
void pistorm_net_ack_rx(uint32_t mask);
int pistorm_net_write_frame(const uint8_t *frame, uint32_t len);

bool pistorm_net_owns_address(uint32_t address);
uint32_t pistorm_net_read(uint32_t address, unsigned size);
void pistorm_net_write(uint32_t address, uint32_t value, unsigned size);

#ifdef __cplusplus
}
#endif

#endif /* PISTORM_ATARI_NETWORK_H */
