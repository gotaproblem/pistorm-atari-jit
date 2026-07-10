// SPDX-License-Identifier: MIT

#include "pistorm_net.h"
#include "pistorm_net_backend.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pistorm_net_state {
  bool enabled;
  bool link_up;
  bool tx_busy;
  bool rx_ready;
  bool rx_dropped;
  uint32_t base;
  uint32_t irq_status;
  uint32_t tx_len;
  uint32_t rx_len;
  uint32_t rx_packets;
  uint32_t tx_packets;
  uint32_t rx_drops;
  uint32_t tx_errors;
  uint8_t mac[6];
  uint8_t tx_frame[PISTORM_NET_FRAME_MAX];
  uint8_t rx_frame[PISTORM_NET_FRAME_MAX];
  pthread_mutex_t lock;
  pistorm_net_backend_t backend;
} pistorm_net_state_t;

extern void atari_natfeat_raise_network_irq(void);

static pistorm_net_state_t g_net = {
  .base = PISTORM_NET_DEFAULT_BASE,
  .mac = { 0x52, 0x54, 0x50, 0x4E, 0x45, 0x54 },
  .lock = PTHREAD_MUTEX_INITIALIZER
};

static void pnet_trace_frame(const char *prefix, const uint8_t *frame, size_t len)
{
  uint16_t type = len >= 14 ? ((uint16_t)frame[12] << 8) | frame[13] : 0;
  const char *debug = getenv("PISTORM_NET_DEBUG");

  if (!debug || !debug[0] || strcmp(debug, "0") == 0)
    return;

  fprintf(stderr,
          "[NET] %s len=%zu type=0x%04X dst=%02X:%02X:%02X:%02X:%02X:%02X src=%02X:%02X:%02X:%02X:%02X:%02X\n",
          prefix, len, type,
          len > 0 ? frame[0] : 0, len > 1 ? frame[1] : 0,
          len > 2 ? frame[2] : 0, len > 3 ? frame[3] : 0,
          len > 4 ? frame[4] : 0, len > 5 ? frame[5] : 0,
          len > 6 ? frame[6] : 0, len > 7 ? frame[7] : 0,
          len > 8 ? frame[8] : 0, len > 9 ? frame[9] : 0,
          len > 10 ? frame[10] : 0, len > 11 ? frame[11] : 0);
}

static uint32_t pnet_load_be(const uint8_t *src, unsigned size)
{
  uint32_t value = 0;
  for (unsigned i = 0; i < size; i++)
    value = (value << 8) | src[i];
  return value;
}

static void pnet_store_be(uint8_t *dst, uint32_t value, unsigned size)
{
  for (unsigned i = 0; i < size; i++) {
    unsigned shift = (size - 1u - i) * 8u;
    dst[i] = (uint8_t)(value >> shift);
  }
}

static uint32_t pnet_status_locked(void)
{
  uint32_t status = 0;
  if (g_net.enabled)
    status |= PISTORM_NET_STATUS_ENABLED;
  if (g_net.link_up)
    status |= PISTORM_NET_STATUS_LINK_UP;
  if (g_net.rx_ready)
    status |= PISTORM_NET_STATUS_RX_READY;
  if (g_net.tx_busy)
    status |= PISTORM_NET_STATUS_TX_BUSY;
  if (g_net.rx_dropped)
    status |= PISTORM_NET_STATUS_RX_DROP;
  return status;
}

static void pnet_backend_rx(void *opaque, const uint8_t *frame, size_t len)
{
  (void)opaque;
  bool raise_irq = false;

  if (!frame || len == 0 || len > PISTORM_NET_FRAME_MAX)
    return;

  pthread_mutex_lock(&g_net.lock);
  if (!g_net.enabled) {
    pthread_mutex_unlock(&g_net.lock);
    return;
  }

  if (g_net.rx_ready) {
    g_net.rx_dropped = true;
    g_net.rx_drops++;
    pthread_mutex_unlock(&g_net.lock);
    return;
  }

  memcpy(g_net.rx_frame, frame, len);
  g_net.rx_len = (uint32_t)len;
  g_net.rx_ready = true;
  g_net.irq_status |= 1u;
  g_net.rx_packets++;
  raise_irq = true;
  pthread_mutex_unlock(&g_net.lock);

  if (raise_irq) {
    pnet_trace_frame("RX queued", frame, len);
    atari_natfeat_raise_network_irq();
  }
}

int pistorm_net_init(const pistorm_net_config_t *config)
{
  pistorm_net_config_t local;

  if (!config) {
    memset(&local, 0, sizeof(local));
    local.base = PISTORM_NET_DEFAULT_BASE;
    local.backend = PISTORM_NET_BACKEND_NONE;
    config = &local;
  }

  pistorm_net_shutdown();

  pthread_mutex_lock(&g_net.lock);
  g_net.enabled = config->enabled;
  g_net.base = config->base ? config->base : PISTORM_NET_DEFAULT_BASE;
  if (config->mac[0] || config->mac[1] || config->mac[2] ||
      config->mac[3] || config->mac[4] || config->mac[5]) {
    memcpy(g_net.mac, config->mac, sizeof(g_net.mac));
  }
  g_net.link_up = false;
  g_net.rx_ready = false;
  g_net.rx_dropped = false;
  g_net.tx_busy = false;
  g_net.tx_len = 0;
  g_net.rx_len = 0;
  g_net.irq_status = 0;
  pthread_mutex_unlock(&g_net.lock);

  if (!config->enabled)
    return 0;

  switch (config->backend) {
    case PISTORM_NET_BACKEND_TAP:
      if (pistorm_net_tap_open(config->ifname, pnet_backend_rx, NULL, &g_net.backend) != 0) {
        fprintf(stderr, "[NET] TAP backend failed for '%s'\n",
                config->ifname[0] ? config->ifname : "(default)");
        return -1;
      }
      pthread_mutex_lock(&g_net.lock);
      g_net.link_up = true;
      pthread_mutex_unlock(&g_net.lock);
      fprintf(stderr, "[NET] TAP backend online at 0x%06X\n", g_net.base);
      return 0;

    case PISTORM_NET_BACKEND_NONE:
      fprintf(stderr, "[NET] enabled without backend at 0x%06X\n", g_net.base);
      return 0;

    case PISTORM_NET_BACKEND_SLIRP:
      if (pistorm_net_slirp_open(pnet_backend_rx, NULL, &g_net.backend) != 0) {
        fprintf(stderr, "[NET] SLIRP backend failed\n");
        return -1;
      }
      pthread_mutex_lock(&g_net.lock);
      g_net.link_up = true;
      pthread_mutex_unlock(&g_net.lock);
      fprintf(stderr, "[NET] SLIRP backend online at 0x%06X\n", g_net.base);
      return 0;
  }

  return -1;
}

void pistorm_net_shutdown(void)
{
  pistorm_net_backend_t backend;

  pthread_mutex_lock(&g_net.lock);
  backend = g_net.backend;
  memset(&g_net.backend, 0, sizeof(g_net.backend));
  g_net.enabled = false;
  g_net.link_up = false;
  g_net.rx_ready = false;
  g_net.tx_busy = false;
  pthread_mutex_unlock(&g_net.lock);

  if (backend.close)
    backend.close(backend.opaque);
}

void pistorm_net_reset(void)
{
  pthread_mutex_lock(&g_net.lock);
  g_net.rx_ready = false;
  g_net.rx_dropped = false;
  g_net.tx_busy = false;
  g_net.tx_len = 0;
  g_net.rx_len = 0;
  g_net.irq_status = 0;
  pthread_mutex_unlock(&g_net.lock);
}

void pistorm_net_poll(void)
{
  pistorm_net_backend_t backend;

  pthread_mutex_lock(&g_net.lock);
  backend = g_net.backend;
  pthread_mutex_unlock(&g_net.lock);

  if (backend.poll)
    backend.poll(backend.opaque);
}

bool pistorm_net_is_enabled(void)
{
  bool enabled;

  pthread_mutex_lock(&g_net.lock);
  enabled = g_net.enabled;
  pthread_mutex_unlock(&g_net.lock);
  return enabled;
}

bool pistorm_net_link_up(void)
{
  bool link_up;

  pthread_mutex_lock(&g_net.lock);
  link_up = g_net.enabled && g_net.link_up;
  pthread_mutex_unlock(&g_net.lock);
  return link_up;
}

void pistorm_net_get_mac(uint8_t mac[6])
{
  if (!mac)
    return;

  pthread_mutex_lock(&g_net.lock);
  memcpy(mac, g_net.mac, sizeof(g_net.mac));
  pthread_mutex_unlock(&g_net.lock);
}

uint32_t pistorm_net_rx_pending_mask(void)
{
  uint32_t mask;

  pthread_mutex_lock(&g_net.lock);
  mask = (g_net.enabled && g_net.rx_ready) ? 1u : 0u;
  pthread_mutex_unlock(&g_net.lock);
  return mask;
}

uint32_t pistorm_net_rx_length(void)
{
  uint32_t len;

  pthread_mutex_lock(&g_net.lock);
  len = (g_net.enabled && g_net.rx_ready) ? g_net.rx_len : 0u;
  pthread_mutex_unlock(&g_net.lock);
  return len;
}

uint32_t pistorm_net_read_frame(uint8_t *buffer, uint32_t max_len)
{
  uint32_t len;

  if (!buffer || max_len == 0)
    return 0;

  pthread_mutex_lock(&g_net.lock);
  if (!g_net.enabled || !g_net.rx_ready) {
    pthread_mutex_unlock(&g_net.lock);
    return 0;
  }

  len = g_net.rx_len;
  if (len > max_len)
    len = max_len;
  memcpy(buffer, g_net.rx_frame, len);
  pthread_mutex_unlock(&g_net.lock);
  return len;
}

void pistorm_net_ack_rx(uint32_t mask)
{
  pthread_mutex_lock(&g_net.lock);
  if (mask & 1u) {
    g_net.rx_ready = false;
    g_net.rx_len = 0;
    g_net.irq_status &= ~1u;
  }
  pthread_mutex_unlock(&g_net.lock);
}

int pistorm_net_write_frame(const uint8_t *frame, uint32_t len)
{
  pistorm_net_backend_t backend;

  if (!frame || len == 0 || len > PISTORM_NET_FRAME_MAX)
    return -1;

  pthread_mutex_lock(&g_net.lock);
  if (!g_net.enabled) {
    pthread_mutex_unlock(&g_net.lock);
    return -1;
  }
  backend = g_net.backend;
  g_net.tx_busy = true;
  pthread_mutex_unlock(&g_net.lock);

  int result = backend.send ? backend.send(backend.opaque, frame, len) : -1;
  pnet_trace_frame(result == 0 ? "TX sent" : "TX failed", frame, len);

  pthread_mutex_lock(&g_net.lock);
  g_net.tx_busy = false;
  if (result == 0)
    g_net.tx_packets++;
  else
    g_net.tx_errors++;
  pthread_mutex_unlock(&g_net.lock);

  return result;
}

bool pistorm_net_owns_address(uint32_t address)
{
  uint32_t folded = address & 0x00FFFFFFu;
  uint32_t base = g_net.base & 0x00FFFFFFu;
  return g_net.enabled &&
         folded >= base &&
         folded < base + PISTORM_NET_WINDOW_SIZE;
}

static uint32_t pnet_read_reg_locked(uint32_t offset)
{
  switch (offset) {
    case PISTORM_NET_REG_ID:
      return PISTORM_NET_ID;
    case PISTORM_NET_REG_VERSION:
      return PISTORM_NET_VERSION;
    case PISTORM_NET_REG_STATUS:
      return pnet_status_locked();
    case PISTORM_NET_REG_IRQ:
      return g_net.irq_status;
    case PISTORM_NET_REG_MAC_HI:
      return ((uint32_t)g_net.mac[0] << 24) |
             ((uint32_t)g_net.mac[1] << 16) |
             ((uint32_t)g_net.mac[2] << 8) |
             g_net.mac[3];
    case PISTORM_NET_REG_MAC_LO:
      return ((uint32_t)g_net.mac[4] << 24) |
             ((uint32_t)g_net.mac[5] << 16);
    case PISTORM_NET_REG_MTU:
      return 1500;
    case PISTORM_NET_REG_TX_LEN:
      return g_net.tx_len;
    case PISTORM_NET_REG_RX_LEN:
      return g_net.rx_ready ? g_net.rx_len : 0;
    default:
      return 0;
  }
}

uint32_t pistorm_net_read(uint32_t address, unsigned size)
{
  uint32_t offset = (address & 0x00FFFFFFu) - (g_net.base & 0x00FFFFFFu);
  uint32_t aligned = offset & ~3u;
  uint32_t value = 0;

  if (size != 1 && size != 2 && size != 4)
    return 0;

  pthread_mutex_lock(&g_net.lock);
  if (offset >= PISTORM_NET_RX_DATA &&
      offset + size <= PISTORM_NET_RX_DATA + PISTORM_NET_FRAME_MAX) {
    uint32_t frame_off = offset - PISTORM_NET_RX_DATA;
    if (g_net.rx_ready && frame_off + size <= g_net.rx_len)
      value = pnet_load_be(&g_net.rx_frame[frame_off], size);
  } else if (aligned < PISTORM_NET_TX_DATA) {
    uint32_t reg = pnet_read_reg_locked(aligned);
    unsigned byte_shift = (3u - (offset & 3u)) * 8u;
    if (size == 4)
      value = reg;
    else if (size == 2)
      value = (reg >> (byte_shift & 16u)) & 0xFFFFu;
    else
      value = (reg >> byte_shift) & 0xFFu;
  }
  pthread_mutex_unlock(&g_net.lock);

  return value;
}

static void pnet_send_tx_frame(void)
{
  pistorm_net_backend_t backend;
  uint8_t frame[PISTORM_NET_FRAME_MAX];
  uint32_t len;
  int rc = -1;

  pthread_mutex_lock(&g_net.lock);
  if (!g_net.enabled || g_net.tx_len == 0 || g_net.tx_len > PISTORM_NET_FRAME_MAX) {
    g_net.tx_errors++;
    pthread_mutex_unlock(&g_net.lock);
    return;
  }

  backend = g_net.backend;
  len = g_net.tx_len;
  memcpy(frame, g_net.tx_frame, len);
  g_net.tx_busy = true;
  pthread_mutex_unlock(&g_net.lock);

  if (backend.send)
    rc = backend.send(backend.opaque, frame, len);

  pthread_mutex_lock(&g_net.lock);
  g_net.tx_busy = false;
  if (rc == 0)
    g_net.tx_packets++;
  else
    g_net.tx_errors++;
  pthread_mutex_unlock(&g_net.lock);
}

static void pnet_write_reg_locked(uint32_t offset, uint32_t value)
{
  switch (offset) {
    case PISTORM_NET_REG_IRQ:
      g_net.irq_status &= ~value;
      break;

    case PISTORM_NET_REG_TX_LEN:
      if (value <= PISTORM_NET_FRAME_MAX)
        g_net.tx_len = value;
      else
        g_net.tx_errors++;
      break;

    case PISTORM_NET_REG_RX_POP:
      if (value) {
        g_net.rx_ready = false;
        g_net.rx_len = 0;
        g_net.rx_dropped = false;
        g_net.irq_status &= ~1u;
      }
      break;

    default:
      break;
  }
}

void pistorm_net_write(uint32_t address, uint32_t value, unsigned size)
{
  uint32_t offset = (address & 0x00FFFFFFu) - (g_net.base & 0x00FFFFFFu);
  uint32_t aligned = offset & ~3u;
  bool kick = false;

  if (size != 1 && size != 2 && size != 4)
    return;

  pthread_mutex_lock(&g_net.lock);
  if (offset >= PISTORM_NET_TX_DATA &&
      offset + size <= PISTORM_NET_TX_DATA + PISTORM_NET_FRAME_MAX) {
    uint32_t frame_off = offset - PISTORM_NET_TX_DATA;
    pnet_store_be(&g_net.tx_frame[frame_off], value, size);
  } else if (aligned < PISTORM_NET_TX_DATA) {
    if (aligned == PISTORM_NET_REG_TX_KICK && value)
      kick = true;
    else if (size == 4)
      pnet_write_reg_locked(aligned, value);
  }
  pthread_mutex_unlock(&g_net.lock);

  if (kick)
    pnet_send_tx_frame();
}
