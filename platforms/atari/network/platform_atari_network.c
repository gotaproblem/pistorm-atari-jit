// SPDX-License-Identifier: MIT

#include "platform_atari_network.h"
#include "atari_natfeat.h"
#include "../../../config_file/config_file.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pnet_env_bool(const char *value)
{
  if (!value || !value[0])
    return 0;
  return strcmp(value, "0") != 0 &&
         strcasecmp(value, "off") != 0 &&
         strcasecmp(value, "no") != 0 &&
         strcasecmp(value, "false") != 0;
}

static int pnet_hex_digit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  c = (char)tolower((unsigned char)c);
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

static int pnet_parse_mac(const char *text, uint8_t mac[6])
{
  if (!text)
    return -1;

  for (unsigned i = 0; i < 6; i++) {
    int hi = pnet_hex_digit(text[0]);
    int lo = pnet_hex_digit(text[1]);
    if (hi < 0 || lo < 0)
      return -1;
    mac[i] = (uint8_t)((hi << 4) | lo);
    text += 2;
    if (i < 5) {
      if (*text != ':' && *text != '.')
        return -1;
      text++;
    }
  }

  return *text == '\0' ? 0 : -1;
}

int platform_network_init(const pistorm_net_config_t *config)
{
  return pistorm_net_init(config);
}

static void pnet_copy_string(char *dst, size_t dst_len, const char *src)
{
  if (!dst || dst_len == 0 || !src)
    return;
  strncpy(dst, src, dst_len - 1);
  dst[dst_len - 1] = '\0';
}

static void pnet_set_backend(pistorm_net_config_t *config, const char *backend)
{
  if (!backend || !backend[0])
    return;

  if (strcasecmp(backend, "tap") == 0)
    config->backend = PISTORM_NET_BACKEND_TAP;
  else if (strcasecmp(backend, "slirp") == 0)
    config->backend = PISTORM_NET_BACKEND_SLIRP;
  else if (strcasecmp(backend, "none") == 0 || strcasecmp(backend, "off") == 0)
    config->backend = PISTORM_NET_BACKEND_NONE;
  else
    fprintf(stderr, "[NET] ignoring invalid backend '%s'\n", backend);
}

static void pnet_apply_env_overrides(pistorm_net_config_t *config)
{
  const char *enable = getenv("PISTORM_NET");
  const char *backend = getenv("PISTORM_NET_BACKEND");
  const char *tap = getenv("PISTORM_NET_TAP");
  const char *base = getenv("PISTORM_NET_BASE");
  const char *mac = getenv("PISTORM_NET_MAC");

  if (enable)
    config->enabled = pnet_env_bool(enable);
  if (base && base[0])
    config->base = (uint32_t)strtoul(base, NULL, 0);
  pnet_set_backend(config, backend);
  if (tap && tap[0]) {
    config->backend = PISTORM_NET_BACKEND_TAP;
    pnet_copy_string(config->ifname, sizeof(config->ifname), tap);
  }
  if (mac && pnet_parse_mac(mac, config->mac) != 0)
    fprintf(stderr, "[NET] ignoring invalid PISTORM_NET_MAC '%s'\n", mac);
}

int platform_network_init_from_config(const struct emulator_config *emulator)
{
  pistorm_net_config_t config;
  atari_natfeat_config_t nf_config;

  memset(&config, 0, sizeof(config));
  memset(&nf_config, 0, sizeof(nf_config));

  config.base = PISTORM_NET_DEFAULT_BASE;
  config.backend = PISTORM_NET_BACKEND_NONE;

  if (emulator) {
    config.enabled = emulator->network_enabled;
    config.base = emulator->network_base ? emulator->network_base : PISTORM_NET_DEFAULT_BASE;
    config.backend = config.enabled ? PISTORM_NET_BACKEND_TAP : PISTORM_NET_BACKEND_NONE;
    pnet_set_backend(&config, emulator->network_backend);
    pnet_copy_string(config.ifname, sizeof(config.ifname), emulator->network_tap);

    if (emulator->network_mac[0] && pnet_parse_mac(emulator->network_mac, config.mac) != 0)
      fprintf(stderr, "[NET] ignoring invalid network_mac '%s'\n", emulator->network_mac);

    pnet_copy_string(nf_config.ip_host, sizeof(nf_config.ip_host), emulator->network_host_ip);
    pnet_copy_string(nf_config.ip_atari, sizeof(nf_config.ip_atari), emulator->network_atari_ip);
    pnet_copy_string(nf_config.netmask, sizeof(nf_config.netmask), emulator->network_netmask);
    nf_config.irq_level = emulator->network_irq_level;
    nf_config.debug = emulator->network_debug;

    for (unsigned i = 0; i < ATARI_NATFEAT_HOSTFS_MAX_DRIVES && i < HOSTFS_MAX_DRIVES; i++) {
      nf_config.hostfs[i].enabled = emulator->hostfs[i].enabled;
      nf_config.hostfs[i].drive = emulator->hostfs[i].drive;
      pnet_copy_string(nf_config.hostfs[i].path,
                       sizeof(nf_config.hostfs[i].path),
                       emulator->hostfs[i].path);
      nf_config.hostfs[i].readonly = emulator->hostfs[i].readonly;
    }
  }

  pnet_apply_env_overrides(&config);
  atari_natfeat_set_config(&nf_config);
  return pistorm_net_init(&config);
}

int platform_network_init_from_env(void)
{
  return platform_network_init_from_config(NULL);
}

void platform_network_shutdown(void)
{
  pistorm_net_shutdown();
}
