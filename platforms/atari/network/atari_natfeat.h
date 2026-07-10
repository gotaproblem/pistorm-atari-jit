// SPDX-License-Identifier: MIT

#ifndef PISTORM_ATARI_NATFEAT_H
#define PISTORM_ATARI_NATFEAT_H

#include <stdbool.h>
#include <stdint.h>

#define ATARI_NATFEAT_HOSTFS_MAX_DRIVES 32
#define ATARI_NATFEAT_HOSTFS_PATH_MAX 256

typedef struct atari_natfeat_hostfs_drive {
  bool enabled;
  char drive;
  char path[ATARI_NATFEAT_HOSTFS_PATH_MAX];
  bool readonly;
} atari_natfeat_hostfs_drive_t;

typedef struct atari_natfeat_config {
  char ip_host[32];
  char ip_atari[32];
  char netmask[32];
  uint8_t irq_level;
  bool debug;
  atari_natfeat_hostfs_drive_t hostfs[ATARI_NATFEAT_HOSTFS_MAX_DRIVES];
} atari_natfeat_config_t;

#ifdef __cplusplus
extern "C" {
#endif

void atari_natfeat_set_config(const atari_natfeat_config_t *config);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include "uae/types.h"
bool atari_natfeat_handle_opcode(uae_u32 opcode, uae_u32 *cycles);
#endif

#endif /* PISTORM_ATARI_NATFEAT_H */
