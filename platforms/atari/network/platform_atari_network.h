// SPDX-License-Identifier: MIT

#ifndef PLATFORM_ATARI_NETWORK_H
#define PLATFORM_ATARI_NETWORK_H

#include "pistorm_net.h"

struct emulator_config;

#ifdef __cplusplus
extern "C" {
#endif

int platform_network_init(const pistorm_net_config_t *config);
int platform_network_init_from_config(const struct emulator_config *emulator);
int platform_network_init_from_env(void);
void platform_network_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_ATARI_NETWORK_H */
