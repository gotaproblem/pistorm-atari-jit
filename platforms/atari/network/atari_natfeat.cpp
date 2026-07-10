// SPDX-License-Identifier: MIT

#include "sysconfig.h"
#include "sysdeps.h"

#include "platforms/atari/network/atari_natfeat.h"
#include "platforms/atari/network/pistorm_net.h"

#include "options.h"
#include "memory.h"
#include "custom.h"
#include "events.h"
#include "newcpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define NF_ID_OPCODE   0x7300u
#define NF_CALL_OPCODE 0x7301u

#define NF_ID_SHIFT 20
#define NF_ID(index) (((index) + 1u) << NF_ID_SHIFT)
#define NF_INDEX(id) (((id) >> NF_ID_SHIFT) - 1u)
#define NF_SUBID(id) ((id) & ((1u << NF_ID_SHIFT) - 1u))

#define NF_VERSION_VALUE 0x00010000u
#define NFETH_NFAPI_VERSION 0x00000005u
#define NFETH_DEFAULT_INTERRUPT_LEVEL 4u
#define HOSTFS_NFAPI_VERSION 0x00000004u
#define FVDIDRV_NFAPI_VERSION 0x14000960u
#define HOSTFS_MINT_DEV_BASE 50u
#define HOSTFS_COOKIE_SIZE 12u
#define HOSTFS_MAX_NODES 1024u
#define HOSTFS_MAX_DIRS 64u
#define HOSTFS_MAX_FILES 64u
#define HOSTFS_HOST_PATH_MAX 512u
#define HOSTFS_PATHCONF_MAX 9
#define FVDI_MAX_ACCEL_PIXELS (4096 * 4096)
#define FVDI_MAX_ACCEL_SPAN 8192
#define NF_ST_RAM_SIZE 0x00400000u
#define NF_TT_RAM_BASE 0x01000000u
#define TOS_E_OK ((uae_u32)0)
#define TOS_EROFS ((uae_u32)-13)
#define TOS_EINVAL ((uae_u32)-25)
#define TOS_ENOENT ((uae_u32)-33)
#define TOS_ENHNDL ((uae_u32)-35)
#define TOS_EACCDN ((uae_u32)-36)
#define TOS_EIHNDL ((uae_u32)-37)
#define TOS_EDRIVE ((uae_u32)-46)
#define TOS_ENMFIL ((uae_u32)-49)
#define TOS_ERANGE ((uae_u32)-64)
#define TOS_EIO ((uae_u32)-90)
#define TOS_ENOSYS ((uae_u32)-32)

#define MINT_FIONREAD ((uae_u16)(('F' << 8) | 1))
#define MINT_FIONWRITE ((uae_u16)(('F' << 8) | 2))
#define MINT_FIOEXCEPT ((uae_u16)(('F' << 8) | 5))
#define MINT_FSTAT64 ((uae_u16)(('F' << 8) | 6))
#define MINT_MX_KER_XFSNAME ((uae_u16)(('m' << 8) | 5))

#define MINT_O_WRONLY 0x0001u
#define MINT_O_RDWR 0x0002u
#define MINT_O_EXEC 0x0003u
#define MINT_O_ACCMODE 0x0003u
#define MINT_O_APPEND 0x0008u
#define MINT_O_CREAT 0x0200u
#define MINT_O_TRUNC 0x0400u

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

enum nf_feature_index {
  NF_FEATURE_NAME = 0,
  NF_FEATURE_VERSION,
  NF_FEATURE_STDERR,
  NF_FEATURE_ETHERNET,
  NF_FEATURE_HOSTFS,
  NF_FEATURE_FVDI,
  NF_FEATURE_COUNT
};

enum nfeth_ops {
  NFETH_GET_VERSION = 0,
  NFETH_XIF_INTLEVEL,
  NFETH_XIF_IRQ,
  NFETH_XIF_START,
  NFETH_XIF_STOP,
  NFETH_XIF_READLENGTH,
  NFETH_XIF_READBLOCK,
  NFETH_XIF_WRITEBLOCK,
  NFETH_XIF_GET_MAC,
  NFETH_XIF_GET_IPHOST,
  NFETH_XIF_GET_IPATARI,
  NFETH_XIF_GET_NETMASK
};

enum hostfs_ops {
  HOSTFS_GET_VERSION = 0,
  HOSTFS_GET_DRIVE_BITS,
  HOSTFS_XFS_INIT,
  HOSTFS_XFS_ROOT,
  HOSTFS_XFS_LOOKUP,
  HOSTFS_XFS_CREATE,
  HOSTFS_XFS_GETDEV,
  HOSTFS_XFS_GETXATTR,
  HOSTFS_XFS_CHATTR,
  HOSTFS_XFS_CHOWN,
  HOSTFS_XFS_CHMOD,
  HOSTFS_XFS_MKDIR,
  HOSTFS_XFS_RMDIR,
  HOSTFS_XFS_REMOVE,
  HOSTFS_XFS_GETNAME,
  HOSTFS_XFS_RENAME,
  HOSTFS_XFS_OPENDIR,
  HOSTFS_XFS_READDIR,
  HOSTFS_XFS_REWINDDIR,
  HOSTFS_XFS_CLOSEDIR,
  HOSTFS_XFS_PATHCONF,
  HOSTFS_XFS_DFREE,
  HOSTFS_XFS_WRITELABEL,
  HOSTFS_XFS_READLABEL,
  HOSTFS_XFS_SYMLINK,
  HOSTFS_XFS_READLINK,
  HOSTFS_XFS_HARDLINK,
  HOSTFS_XFS_FSCNTL,
  HOSTFS_XFS_DSKCHNG,
  HOSTFS_XFS_RELEASE,
  HOSTFS_XFS_DUPCOOKIE,
  HOSTFS_XFS_SYNC,
  HOSTFS_XFS_MKNOD,
  HOSTFS_XFS_UNMOUNT,
  HOSTFS_DEV_OPEN,
  HOSTFS_DEV_WRITE,
  HOSTFS_DEV_READ,
  HOSTFS_DEV_LSEEK,
  HOSTFS_DEV_IOCTL,
  HOSTFS_DEV_DATIME,
  HOSTFS_DEV_CLOSE,
  HOSTFS_DEV_SELECT,
  HOSTFS_DEV_UNSELECT,
  HOSTFS_XFS_STAT64
};

enum fvdi_ops {
  FVDI_GET_VERSION = 0,
  FVDI_GET_PIXEL,
  FVDI_PUT_PIXEL,
  FVDI_MOUSE,
  FVDI_EXPAND_AREA,
  FVDI_FILL_AREA,
  FVDI_BLIT_AREA,
  FVDI_LINE,
  FVDI_FILL_POLYGON,
  FVDI_GET_HWCOLOR,
  FVDI_SET_COLOR,
  FVDI_GET_FBADDR,
  FVDI_SET_RESOLUTION,
  FVDI_GET_WIDTH,
  FVDI_GET_HEIGHT,
  FVDI_OPENWK,
  FVDI_CLOSEWK,
  FVDI_GETBPP,
  FVDI_EVENT,
  FVDI_TEXT_AREA,
  FVDI_GETCOMPONENT
};

static const char *nf_feature_names[NF_FEATURE_COUNT] = {
  "NF_NAME",
  "NF_VERSION",
  "NF_STDERR",
  "ETHERNET",
  "HOSTFS",
  "fVDI"
};

extern "C" uint32_t pistorm_fvdi_fb_base(void);
extern "C" uint8_t *pistorm_fvdi_fb_ptr(void);
extern "C" uint32_t pistorm_fvdi_width(void);
extern "C" uint32_t pistorm_fvdi_height(void);
extern "C" uint32_t pistorm_fvdi_bpp(void);
extern "C" int pistorm_fvdi_set_mode(uint32_t width, uint32_t height, uint32_t bpp);

extern "C" uint64_t pistorm_fvdi_write_count(void);
extern "C" void pistorm_fvdi_note_host_write(uint32_t o, uint32_t bytes);
extern "C" void pistorm_dma_to_stram(uaecptr addr, const uint8_t *src, uint32_t n);
extern uae_u8 *natmem_offset;
extern bool tt_ram_available;
extern uint32_t tt_ram_size;

static atari_natfeat_config_t g_nf_config;

typedef struct hostfs_mount {
  bool mounted;
  uint16_t dev;
  uint32_t fs_ptr;
  uint32_t fs_devdrv_ptr;
  uint32_t root_cookie;
  char mountpoint[8];
  const atari_natfeat_hostfs_drive_t *drive;
} hostfs_mount_t;

static hostfs_mount_t g_hostfs_mounts[ATARI_NATFEAT_HOSTFS_MAX_DRIVES];
static uint32_t g_hostfs_next_cookie = 1;

typedef struct hostfs_node {
  bool used;
  uint32_t cookie;
  uint16_t dev;
  int mount_index;
  char path[HOSTFS_HOST_PATH_MAX];
} hostfs_node_t;

static hostfs_node_t g_hostfs_nodes[HOSTFS_MAX_NODES];

typedef struct hostfs_dir {
  bool used;
  uint32_t id;
  DIR *dir;
  uint16_t dev;
  int mount_index;
  char path[HOSTFS_HOST_PATH_MAX];
} hostfs_dir_t;

static hostfs_dir_t g_hostfs_dirs[HOSTFS_MAX_DIRS];
static uint32_t g_hostfs_next_dir_id = 1;

typedef struct hostfs_file {
  bool used;
  uint32_t id;
  int fd;
  uint16_t dev;
  int mount_index;
  char path[HOSTFS_HOST_PATH_MAX];
} hostfs_file_t;

static hostfs_file_t g_hostfs_files[HOSTFS_MAX_FILES];
static uint32_t g_hostfs_next_file_id = 1;

static bool nf_host_ram_ptr(uaecptr addr, uint32_t size, uae_u8 **ptr)
{
  if (!natmem_offset || size == 0)
    return false;

  if (addr < NF_ST_RAM_SIZE && size <= NF_ST_RAM_SIZE - addr) {
    *ptr = natmem_offset + addr;
    return true;
  }

  if (tt_ram_available &&
      addr >= NF_TT_RAM_BASE &&
      addr - NF_TT_RAM_BASE <= tt_ram_size &&
      size <= tt_ram_size - (addr - NF_TT_RAM_BASE)) {
    *ptr = natmem_offset + addr;
    return true;
  }

  return false;
}

static uae_u32 nf_read_long(uaecptr addr)
{
  uae_u8 *p;
  if (nf_host_ram_ptr(addr, 4, &p))
    return ((uae_u32)p[0] << 24) |
           ((uae_u32)p[1] << 16) |
           ((uae_u32)p[2] << 8) |
           (uae_u32)p[3];
  return x_get_long(addr);
}

static uae_u8 nf_read_byte(uaecptr addr)
{
  uae_u8 *p;
  if (nf_host_ram_ptr(addr, 1, &p))
    return p[0];
  return (uae_u8)x_get_byte(addr);
}

static uae_u16 nf_read_word(uaecptr addr)
{
  uae_u8 *p;
  if (nf_host_ram_ptr(addr, 2, &p))
    return ((uae_u16)p[0] << 8) | (uae_u16)p[1];
  return (uae_u16)x_get_word(addr);
}

static void nf_write_byte(uaecptr addr, uae_u8 value)
{
  uae_u8 *p;
  if (addr < NF_ST_RAM_SIZE) {
    pistorm_dma_to_stram(addr, &value, 1);
    return;
  }
  if (nf_host_ram_ptr(addr, 1, &p)) {
    p[0] = value;
    return;
  }
  x_put_byte(addr, value);
}

static void nf_write_word(uaecptr addr, uae_u16 value)
{
  uae_u8 *p;
  uae_u8 bytes[2] = {
    (uae_u8)(value >> 8),
    (uae_u8)value
  };
  if (addr < NF_ST_RAM_SIZE && sizeof(bytes) <= NF_ST_RAM_SIZE - addr) {
    pistorm_dma_to_stram(addr, bytes, sizeof(bytes));
    return;
  }
  if (nf_host_ram_ptr(addr, sizeof(bytes), &p)) {
    p[0] = bytes[0];
    p[1] = bytes[1];
    return;
  }
  x_put_word(addr, value);
}

static void nf_write_long(uaecptr addr, uae_u32 value)
{
  uae_u8 *p;
  uae_u8 bytes[4] = {
    (uae_u8)(value >> 24),
    (uae_u8)(value >> 16),
    (uae_u8)(value >> 8),
    (uae_u8)value
  };
  if (addr < NF_ST_RAM_SIZE && sizeof(bytes) <= NF_ST_RAM_SIZE - addr) {
    pistorm_dma_to_stram(addr, bytes, sizeof(bytes));
    return;
  }
  if (nf_host_ram_ptr(addr, sizeof(bytes), &p)) {
    p[0] = bytes[0];
    p[1] = bytes[1];
    p[2] = bytes[2];
    p[3] = bytes[3];
    return;
  }
  x_put_long(addr, value);
}

static void nf_write_quad(uaecptr addr, uint64_t value)
{
  nf_write_long(addr, (uae_u32)(value >> 32));
  nf_write_long(addr + 4, (uae_u32)value);
}

static void nf_write_buffer(uaecptr addr, const uae_u8 *src, uae_u32 len)
{
  for (uae_u32 i = 0; i < len; i++)
    nf_write_byte(addr + i, src[i]);
}

static void nf_write_string(uaecptr addr, uae_u32 max_len, const char *text)
{
  if (!addr || max_len == 0)
    return;

  uae_u32 i = 0;
  for (; i + 1 < max_len && text[i]; i++)
    nf_write_byte(addr + i, (uae_u8)text[i]);
  nf_write_byte(addr + i, 0);
}

static void nf_read_string(uaecptr addr, char *dst, size_t dst_len)
{
  if (!dst || dst_len == 0)
    return;

  size_t i = 0;
  if (addr) {
    for (; i + 1 < dst_len; i++) {
      dst[i] = (char)nf_read_byte(addr + (uaecptr)i);
      if (dst[i] == '\0')
        return;
    }
  }
  dst[i] = '\0';
}

static uae_u32 nf_get_param(uaecptr params, unsigned index)
{
  return nf_read_long(params + (uaecptr)index * 4u);
}

static bool hostfs_is_enabled(void)
{
  for (unsigned i = 0; i < ATARI_NATFEAT_HOSTFS_MAX_DRIVES; i++) {
    if (g_nf_config.hostfs[i].enabled && g_nf_config.hostfs[i].path[0])
      return true;
  }
  return false;
}

static uae_u32 hostfs_drive_bits(void)
{
  uae_u32 bits = 0;
  for (unsigned i = 0; i < ATARI_NATFEAT_HOSTFS_MAX_DRIVES && i < 32; i++) {
    if (g_nf_config.hostfs[i].enabled && g_nf_config.hostfs[i].path[0])
      bits |= 1u << i;
  }
  return bits;
}

static int hostfs_drive_index_from_dev(uae_u32 dev)
{
  if (dev < HOSTFS_MINT_DEV_BASE)
    return -1;
  dev -= HOSTFS_MINT_DEV_BASE;
  return dev < ATARI_NATFEAT_HOSTFS_MAX_DRIVES ? (int)dev : -1;
}

static int hostfs_mounted_index_from_dev(uae_u32 dev)
{
  for (unsigned i = 0; i < ATARI_NATFEAT_HOSTFS_MAX_DRIVES; i++) {
    if (g_hostfs_mounts[i].mounted && g_hostfs_mounts[i].dev == dev)
      return (int)i;
  }
  return -1;
}

static int hostfs_drive_index_from_mountpoint(uaecptr mountpoint)
{
  char text[16];
  nf_read_string(mountpoint, text, sizeof(text));

  if (!text[0])
    return -1;

  char drive = 0;
  if ((text[0] == 'u' || text[0] == 'U') && text[1] == ':' &&
      (text[2] == '\\' || text[2] == '/') && text[3])
    drive = text[3];
  else if (text[1] == ':' || text[1] == 0)
    drive = text[0];

  if (drive >= 'a' && drive <= 'z')
    drive = (char)(drive - 'a' + 'A');
  if (drive >= 'A' && drive <= 'Z')
    return drive - 'A';
  if (drive >= '1' && drive <= '6')
    return 26 + drive - '1';
  return -1;
}

static void hostfs_write_cookie(uaecptr cookie_addr, uae_u32 fs_ptr,
                                uae_u16 dev, uae_u16 aux, uae_u32 index)
{
  nf_write_long(cookie_addr + 0, fs_ptr);
  nf_write_word(cookie_addr + 4, dev);
  nf_write_word(cookie_addr + 6, aux);
  nf_write_long(cookie_addr + 8, index);
}

static void hostfs_copy_cookie(uaecptr dst, uaecptr src)
{
  hostfs_write_cookie(dst,
                      nf_read_long(src + 0),
                      nf_read_word(src + 4),
                      nf_read_word(src + 6),
                      nf_read_long(src + 8));
}

static uae_u16 hostfs_dir_index(uaecptr dirh)
{
  return nf_read_word(dirh + 12);
}

static void hostfs_write_dir_index(uaecptr dirh, uae_u16 index)
{
  nf_write_word(dirh + 12, index);
}

static void hostfs_write_dir_flags(uaecptr dirh, uae_u16 flags)
{
  nf_write_word(dirh + 14, flags);
}

static uae_u16 hostfs_dir_flags(uaecptr dirh)
{
  return nf_read_word(dirh + 14);
}

static uae_u32 hostfs_dir_id(uaecptr dirh)
{
  return nf_read_long(dirh + 16);
}

static void hostfs_write_dir_id(uaecptr dirh, uae_u32 id)
{
  nf_write_long(dirh + 16, id);
}

static uae_u32 hostfs_file_id(uaecptr filep)
{
  return nf_read_long(filep + 8);
}

static void hostfs_write_file_id(uaecptr filep, uae_u32 id)
{
  nf_write_long(filep + 8, id);
}

static uae_u16 hostfs_file_flags(uaecptr filep)
{
  return nf_read_word(filep + 2);
}

static bool hostfs_open_flags_are_readonly(uae_u16 flags)
{
  uae_u16 accmode = flags & MINT_O_ACCMODE;

  if (accmode == MINT_O_WRONLY || accmode == MINT_O_RDWR)
    return false;
  if (flags & (MINT_O_APPEND | MINT_O_CREAT | MINT_O_TRUNC))
    return false;
  return accmode == 0 || accmode == MINT_O_EXEC;
}

static int16_t hostfs_file_links(uaecptr filep)
{
  return (int16_t)nf_read_word(filep + 0);
}

static int hostfs_mounted_index_from_cookie(uaecptr cookie)
{
  if (!cookie)
    return -1;

  uae_u32 index = nf_read_long(cookie + 8);
  for (unsigned i = 0; i < ATARI_NATFEAT_HOSTFS_MAX_DRIVES; i++) {
    if (g_hostfs_mounts[i].mounted && g_hostfs_mounts[i].root_cookie == index)
      return (int)i;
  }
  for (unsigned i = 0; i < HOSTFS_MAX_NODES; i++) {
    if (g_hostfs_nodes[i].used && g_hostfs_nodes[i].cookie == index)
      return g_hostfs_nodes[i].mount_index;
  }
  return -1;
}

static hostfs_node_t *hostfs_node_from_cookie_index(uae_u32 index)
{
  for (unsigned i = 0; i < HOSTFS_MAX_NODES; i++) {
    if (g_hostfs_nodes[i].used && g_hostfs_nodes[i].cookie == index)
      return &g_hostfs_nodes[i];
  }
  return NULL;
}

static hostfs_dir_t *hostfs_dir_from_id(uae_u32 id)
{
  if (!id)
    return NULL;
  for (unsigned i = 0; i < HOSTFS_MAX_DIRS; i++) {
    if (g_hostfs_dirs[i].used && g_hostfs_dirs[i].id == id)
      return &g_hostfs_dirs[i];
  }
  return NULL;
}

static hostfs_dir_t *hostfs_alloc_dir(void)
{
  for (unsigned i = 0; i < HOSTFS_MAX_DIRS; i++) {
    if (!g_hostfs_dirs[i].used) {
      memset(&g_hostfs_dirs[i], 0, sizeof(g_hostfs_dirs[i]));
      g_hostfs_dirs[i].used = true;
      g_hostfs_dirs[i].id = g_hostfs_next_dir_id++;
      return &g_hostfs_dirs[i];
    }
  }
  return NULL;
}

static void hostfs_close_dir(hostfs_dir_t *dir)
{
  if (!dir || !dir->used)
    return;
  if (dir->dir)
    closedir(dir->dir);
  memset(dir, 0, sizeof(*dir));
}

static void hostfs_close_all_dirs(void)
{
  for (unsigned i = 0; i < HOSTFS_MAX_DIRS; i++)
    hostfs_close_dir(&g_hostfs_dirs[i]);
}

static void hostfs_close_dirs_for_mount(int mount_index)
{
  for (unsigned i = 0; i < HOSTFS_MAX_DIRS; i++) {
    if (g_hostfs_dirs[i].used && g_hostfs_dirs[i].mount_index == mount_index)
      hostfs_close_dir(&g_hostfs_dirs[i]);
  }
}

static hostfs_file_t *hostfs_file_from_id(uae_u32 id)
{
  if (!id)
    return NULL;
  for (unsigned i = 0; i < HOSTFS_MAX_FILES; i++) {
    if (g_hostfs_files[i].used && g_hostfs_files[i].id == id)
      return &g_hostfs_files[i];
  }
  return NULL;
}

static hostfs_file_t *hostfs_alloc_file(void)
{
  for (unsigned i = 0; i < HOSTFS_MAX_FILES; i++) {
    if (!g_hostfs_files[i].used) {
      memset(&g_hostfs_files[i], 0, sizeof(g_hostfs_files[i]));
      g_hostfs_files[i].used = true;
      g_hostfs_files[i].id = g_hostfs_next_file_id++;
      g_hostfs_files[i].fd = -1;
      return &g_hostfs_files[i];
    }
  }
  return NULL;
}

static void hostfs_close_file(hostfs_file_t *file)
{
  if (!file || !file->used)
    return;
  if (file->fd >= 0)
    close(file->fd);
  memset(file, 0, sizeof(*file));
  file->fd = -1;
}

static void hostfs_close_all_files(void)
{
  for (unsigned i = 0; i < HOSTFS_MAX_FILES; i++)
    hostfs_close_file(&g_hostfs_files[i]);
}

static void hostfs_close_files_for_mount(int mount_index)
{
  for (unsigned i = 0; i < HOSTFS_MAX_FILES; i++) {
    if (g_hostfs_files[i].used && g_hostfs_files[i].mount_index == mount_index)
      hostfs_close_file(&g_hostfs_files[i]);
  }
}

static const char *hostfs_path_from_cookie(uaecptr cookie)
{
  if (!cookie)
    return NULL;

  uae_u32 index = nf_read_long(cookie + 8);
  for (unsigned i = 0; i < ATARI_NATFEAT_HOSTFS_MAX_DRIVES; i++) {
    if (g_hostfs_mounts[i].mounted &&
        g_hostfs_mounts[i].root_cookie == index &&
        g_hostfs_mounts[i].drive)
      return g_hostfs_mounts[i].drive->path;
  }

  hostfs_node_t *node = hostfs_node_from_cookie_index(index);
  return node ? node->path : NULL;
}

static hostfs_node_t *hostfs_alloc_node(void)
{
  for (unsigned i = 0; i < HOSTFS_MAX_NODES; i++) {
    if (!g_hostfs_nodes[i].used) {
      memset(&g_hostfs_nodes[i], 0, sizeof(g_hostfs_nodes[i]));
      g_hostfs_nodes[i].used = true;
      g_hostfs_nodes[i].cookie = g_hostfs_next_cookie++;
      return &g_hostfs_nodes[i];
    }
  }
  return NULL;
}

static bool hostfs_same_path(const char *a, const char *b)
{
  size_t alen;
  size_t blen;

  if (!a || !b)
    return false;
  alen = strlen(a);
  blen = strlen(b);
  while (alen > 1 && a[alen - 1] == '/')
    alen--;
  while (blen > 1 && b[blen - 1] == '/')
    blen--;
  return alen == blen && strncmp(a, b, alen) == 0;
}

static bool hostfs_path_under(const char *base, const char *path, const char **relative)
{
  size_t base_len;

  if (!base || !path)
    return false;
  base_len = strlen(base);
  while (base_len > 1 && base[base_len - 1] == '/')
    base_len--;
  if (strncmp(base, path, base_len) != 0)
    return false;
  if (path[base_len] != '\0' && path[base_len] != '/')
    return false;
  if (relative)
    *relative = path + base_len;
  return true;
}

static bool hostfs_parent_path(char *dst, size_t dst_len, const char *path)
{
  size_t len;

  if (!path || !path[0])
    return false;
  if (snprintf(dst, dst_len, "%s", path) >= (int)dst_len)
    return false;
  len = strlen(dst);
  while (len > 1 && dst[len - 1] == '/')
    dst[--len] = '\0';
  char *slash = strrchr(dst, '/');
  if (!slash || slash == dst)
    return false;
  *slash = '\0';
  return true;
}

static hostfs_node_t *hostfs_alloc_path_node(int mount_index, uae_u16 dev, const char *path)
{
  hostfs_node_t *node = hostfs_alloc_node();
  if (!node)
    return NULL;
  node->dev = dev;
  node->mount_index = mount_index;
  strncpy(node->path, path, sizeof(node->path) - 1);
  node->path[sizeof(node->path) - 1] = '\0';
  return node;
}

static bool hostfs_name_is_safe(const char *name)
{
  if (!name)
    return false;
  if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return true;
  for (const char *p = name; *p; p++) {
    if (*p == '/' || *p == '\\' || *p == ':')
      return false;
  }
  return strstr(name, "..") == NULL;
}

static const char *hostfs_basename(const char *path)
{
  const char *end;
  const char *base;

  if (!path || !path[0])
    return "HOSTFS";

  end = path + strlen(path);
  while (end > path && end[-1] == '/')
    end--;
  base = end;
  while (base > path && base[-1] != '/')
    base--;
  return base < end ? base : path;
}

static bool hostfs_resolve_child(char *dst, size_t dst_len,
                                 const char *base, const char *name)
{
  if (snprintf(dst, dst_len, "%s/%s", base, name) >= (int)dst_len)
    return false;
  if (access(dst, F_OK) == 0)
    return true;

  DIR *dir = opendir(base);
  if (!dir)
    return false;

  bool found = false;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcasecmp(entry->d_name, name) != 0)
      continue;
    if (snprintf(dst, dst_len, "%s/%s", base, entry->d_name) < (int)dst_len)
      found = true;
    break;
  }

  closedir(dir);
  return found;
}

static void hostfs_make_tos_name(char *dst, size_t dst_len, const char *src)
{
  char name[9];
  char ext[4];
  size_t ni = 0;
  size_t ei = 0;
  const char *dot = strrchr(src, '.');

  memset(name, 0, sizeof(name));
  memset(ext, 0, sizeof(ext));

  for (const char *p = src; *p && (!dot || p < dot); p++) {
    if (ni >= sizeof(name) - 1)
      break;
    name[ni++] = (char)toupper((unsigned char)*p);
  }

  if (dot && dot[1]) {
    for (const char *p = dot + 1; *p && ei < sizeof(ext) - 1; p++)
      ext[ei++] = (char)toupper((unsigned char)*p);
  }

  if (ext[0])
    snprintf(dst, dst_len, "%s.%s", name[0] ? name : "_", ext);
  else
    snprintf(dst, dst_len, "%s", name[0] ? name : "_");
}

static uae_u16 hostfs_mode_to_mint(mode_t mode)
{
  uae_u16 result = 0;

  if (mode & S_IXOTH) result |= 00001;
  if (mode & S_IWOTH) result |= 00002;
  if (mode & S_IROTH) result |= 00004;
  if (mode & S_IXGRP) result |= 00010;
  if (mode & S_IWGRP) result |= 00020;
  if (mode & S_IRGRP) result |= 00040;
  if (mode & S_IXUSR) result |= 00100;
  if (mode & S_IWUSR) result |= 00200;
  if (mode & S_IRUSR) result |= 00400;
  if (mode & S_ISVTX) result |= 01000;
  if (mode & S_ISGID) result |= 02000;
  if (mode & S_ISUID) result |= 04000;

  if (S_ISCHR(mode))  result |= 0020000;
  if (S_ISDIR(mode))  result |= 0040000;
  if (S_ISBLK(mode))  result |= 0060000;
  if (S_ISREG(mode))  result |= 0100000;
  if (S_ISFIFO(mode)) result |= 0120000;
  if (S_ISLNK(mode))  result |= 0160000;

  return result;
}

static uae_u16 hostfs_mode_to_tos(mode_t mode)
{
  return S_ISDIR(mode) ? 0x10 : 0;
}

static uae_u16 hostfs_time_to_dos(time_t value)
{
  struct tm tmv;
  localtime_r(&value, &tmv);
  return (uae_u16)(((tmv.tm_hour & 0x1f) << 11) |
                   ((tmv.tm_min & 0x3f) << 5) |
                   ((tmv.tm_sec / 2) & 0x1f));
}

static uae_u16 hostfs_date_to_dos(time_t value)
{
  struct tm tmv;
  localtime_r(&value, &tmv);
  int year = tmv.tm_year - 80;
  if (year < 0)
    year = 0;
  if (year > 127)
    year = 127;
  return (uae_u16)(((year & 0x7f) << 9) |
                   (((tmv.tm_mon + 1) & 0x0f) << 5) |
                   (tmv.tm_mday & 0x1f));
}

static void hostfs_write_xattr(uaecptr xattr, const struct stat *st)
{
  uint64_t blksize = st->st_blksize ? (uint64_t)st->st_blksize : 512u;
  uint64_t blocks = ((uint64_t)st->st_blocks * 512u + blksize - 1u) / blksize;
  uint64_t size = (uint64_t)st->st_size;

  nf_write_word(xattr + 0, hostfs_mode_to_mint(st->st_mode));
  nf_write_long(xattr + 2, (uae_u32)st->st_ino);
  nf_write_word(xattr + 6, (uae_u16)st->st_dev);
  nf_write_word(xattr + 8, 0);
  nf_write_word(xattr + 10, (uae_u16)st->st_nlink);
  nf_write_word(xattr + 12, (uae_u16)st->st_uid);
  nf_write_word(xattr + 14, (uae_u16)st->st_gid);
  nf_write_long(xattr + 16, size > 0xffffffffULL ? 0xffffffffu : (uae_u32)size);
  nf_write_long(xattr + 20, blksize > 0xffffffffULL ? 0xffffffffu : (uae_u32)blksize);
  nf_write_long(xattr + 24, blocks > 0xffffffffULL ? 0xffffffffu : (uae_u32)blocks);
  nf_write_word(xattr + 28, hostfs_time_to_dos(st->st_mtime));
  nf_write_word(xattr + 30, hostfs_date_to_dos(st->st_mtime));
  nf_write_word(xattr + 32, hostfs_time_to_dos(st->st_atime));
  nf_write_word(xattr + 34, hostfs_date_to_dos(st->st_atime));
  nf_write_word(xattr + 36, hostfs_time_to_dos(st->st_ctime));
  nf_write_word(xattr + 38, hostfs_date_to_dos(st->st_ctime));
  nf_write_word(xattr + 40, hostfs_mode_to_tos(st->st_mode));
  nf_write_word(xattr + 42, 0);
  nf_write_long(xattr + 44, 0);
  nf_write_long(xattr + 48, 0);
}

static void hostfs_write_stat64(uaecptr statp, const struct stat *st)
{
  uint64_t blksize = st->st_blksize ? (uint64_t)st->st_blksize : 512u;
  uint64_t blocks = (uint64_t)st->st_blocks;

  if (blksize < 512)
    blksize = 512;

  nf_write_quad(statp + 0, (uint64_t)st->st_dev);
  nf_write_long(statp + 8, (uae_u32)st->st_ino);
  nf_write_long(statp + 12, hostfs_mode_to_mint(st->st_mode));
  nf_write_long(statp + 16, (uae_u32)st->st_nlink);
  nf_write_long(statp + 20, (uae_u32)st->st_uid);
  nf_write_long(statp + 24, (uae_u32)st->st_gid);
  nf_write_quad(statp + 28, (uint64_t)st->st_rdev);
  nf_write_quad(statp + 36, (uint64_t)st->st_atime);
  nf_write_long(statp + 44, 0);
  nf_write_quad(statp + 48, (uint64_t)st->st_mtime);
  nf_write_long(statp + 56, 0);
  nf_write_quad(statp + 60, (uint64_t)st->st_ctime);
  nf_write_long(statp + 68, 0);
  nf_write_quad(statp + 72, (uint64_t)st->st_size);
  nf_write_quad(statp + 80, blocks);
  nf_write_long(statp + 88, (uae_u32)blksize);
  nf_write_long(statp + 92, 0);
  nf_write_long(statp + 96, 0);
  for (unsigned i = 0; i < 7; i++)
    nf_write_long(statp + 100 + i * 4, 0);
}

static bool hostfs_debug_enabled(void)
{
  const char *debug = getenv("PISTORM_HOSTFS_DEBUG");
  if (debug && debug[0])
    return strcmp(debug, "0") != 0;
  return g_nf_config.debug;
}

#define HOSTFS_LOG(...) \
  do { \
    if (hostfs_debug_enabled()) \
      fprintf(stderr, __VA_ARGS__); \
  } while (0)

static uae_u32 nf_get_id(uaecptr stack)
{
  char name[80];
  uaecptr name_ptr = nf_read_long(stack + 4);

  nf_read_string(name_ptr, name, sizeof(name));

  for (uae_u32 i = 0; i < NF_FEATURE_COUNT; i++) {
    if (strcasecmp(name, nf_feature_names[i]) == 0) {
      if (i == NF_FEATURE_ETHERNET && !pistorm_net_is_enabled()) {
        fprintf(stderr, "[NF] GET_ID(\"%s\") -> 0 (network disabled)\n", name);
        return 0;
      }
      if (i == NF_FEATURE_HOSTFS && !hostfs_is_enabled()) {
        HOSTFS_LOG("[NF] GET_ID(\"%s\") -> 0 (hostfs disabled)\n", name);
        return 0;
      }
      if (i == NF_FEATURE_ETHERNET)
        fprintf(stderr, "[NF] GET_ID(\"%s\") -> 0x%08X\n", name, NF_ID(i));
      if (i == NF_FEATURE_HOSTFS)
        HOSTFS_LOG("[NF] GET_ID(\"%s\") -> 0x%08X drive_bits=0x%08X\n",
                   name, NF_ID(i), hostfs_drive_bits());
      return NF_ID(i);
    }
  }

  if (strcasecmp(name, "ETHERNET") == 0)
    fprintf(stderr, "[NF] GET_ID(\"%s\") -> 0 (not found)\n", name);
  if (strcasecmp(name, "HOSTFS") == 0)
    HOSTFS_LOG("[NF] GET_ID(\"%s\") -> 0 (not found)\n", name);
  return 0;
}

static uae_u32 nf_call_name(uaecptr params)
{
  static const char name[] = "PiStorm Atari JIT";
  uaecptr buffer = nf_get_param(params, 0);
  uae_u32 len = nf_get_param(params, 1);

  nf_write_string(buffer, len, name);
  return (uae_u32)strlen(name);
}

static uae_u32 nf_call_stderr(uaecptr params)
{
  char text[256];
  uaecptr text_ptr = nf_get_param(params, 0);
  static bool at_line_start = true;

  nf_read_string(text_ptr, text, sizeof(text));

  for (const char *p = text; *p; p++) {
    if (at_line_start) {
      fputs("[NF_STDERR] ", stderr);
      at_line_start = false;
    }

    fputc(*p, stderr);
    if (*p == '\n' || *p == '\r')
      at_line_start = true;
  }

  fflush(stderr);
  return 0;
}

static uae_u32 nfeth_get_text_param(uaecptr params, const char *text)
{
  uaecptr buffer = nf_get_param(params, 1);
  uae_u32 len = nf_get_param(params, 2);

  nf_write_string(buffer, len, text);
  return (uae_u32)strlen(text);
}

extern "C" void atari_natfeat_set_config(const atari_natfeat_config_t *config)
{
  hostfs_close_all_files();
  hostfs_close_all_dirs();
  memset(&g_nf_config, 0, sizeof(g_nf_config));
  memset(g_hostfs_mounts, 0, sizeof(g_hostfs_mounts));
  memset(g_hostfs_nodes, 0, sizeof(g_hostfs_nodes));
  memset(g_hostfs_dirs, 0, sizeof(g_hostfs_dirs));
  memset(g_hostfs_files, 0, sizeof(g_hostfs_files));
  g_hostfs_next_cookie = 1;
  g_hostfs_next_dir_id = 1;
  g_hostfs_next_file_id = 1;
  if (config)
    g_nf_config = *config;
}

static const char *nfeth_config_text(const char *name, const char *configured, const char *fallback)
{
  const char *value = getenv(name);
  if (value && value[0])
    return value;
  return configured && configured[0] ? configured : fallback;
}

static bool nfeth_debug_enabled(void)
{
  const char *debug = getenv("PISTORM_NET_DEBUG");
  if (debug && debug[0])
    return strcmp(debug, "0") != 0;
  return g_nf_config.debug;
}

static uae_u32 nfeth_interrupt_level(void)
{
  const char *value = getenv("PISTORM_NET_IRQ_LEVEL");
  char *end = NULL;
  unsigned long level;

  if (!value || !value[0]) {
    if (g_nf_config.irq_level == 2 || g_nf_config.irq_level == 4 || g_nf_config.irq_level == 6)
      return g_nf_config.irq_level;
    return NFETH_DEFAULT_INTERRUPT_LEVEL;
  }

  level = strtoul(value, &end, 0);
  if (*end || (level != 2 && level != 4 && level != 6)) {
    fprintf(stderr, "[NF] ignoring invalid PISTORM_NET_IRQ_LEVEL '%s'\n", value);
    return NFETH_DEFAULT_INTERRUPT_LEVEL;
  }

  return (uae_u32)level;
}

static uae_u32 nf_call_ethernet(uae_u32 subid, uaecptr params)
{
  switch (subid) {
    case NFETH_GET_VERSION:
      fprintf(stderr, "[NF] ETHERNET.GET_VERSION -> %u\n", NFETH_NFAPI_VERSION);
      return NFETH_NFAPI_VERSION;

    case NFETH_XIF_INTLEVEL:
    {
      uae_u32 level = nfeth_interrupt_level();
      fprintf(stderr, "[NF] ETHERNET.XIF_INTLEVEL -> %u\n", level);
      return level;
    }

    case NFETH_XIF_IRQ:
    {
      uae_u32 mask = nf_get_param(params, 0);
      uae_u32 pending;
      if (mask)
        pistorm_net_ack_rx(mask);
      pending = pistorm_net_rx_pending_mask();
      if (nfeth_debug_enabled() && (mask || pending))
        fprintf(stderr, "[NF] ETHERNET.XIF_IRQ ack=0x%02X -> pending=0x%02X\n",
                mask, pending);
      return pending;
    }

    case NFETH_XIF_START:
      fprintf(stderr, "[NF] ETHERNET.XIF_START eth%u -> %d\n",
              nf_get_param(params, 0), pistorm_net_link_up() ? 0 : -15);
      return pistorm_net_link_up() ? 0 : (uae_u32)-15; /* TOS_EUNDEV */

    case NFETH_XIF_STOP:
      fprintf(stderr, "[NF] ETHERNET.XIF_STOP eth%u -> 0\n", nf_get_param(params, 0));
      return 0;

    case NFETH_XIF_READLENGTH:
    {
      uae_u32 len = pistorm_net_rx_length();
      if (nfeth_debug_enabled() && len)
        fprintf(stderr, "[NF] ETHERNET.XIF_READLENGTH eth%u -> %u\n",
                nf_get_param(params, 0), len);
      return len;
    }

    case NFETH_XIF_READBLOCK:
    {
      uae_u32 len = nf_get_param(params, 2);
      uae_u8 frame[PISTORM_NET_FRAME_MAX];
      uae_u32 copied = pistorm_net_read_frame(frame, len);
      nf_write_buffer(nf_get_param(params, 1), frame, copied);
      if (nfeth_debug_enabled())
        fprintf(stderr, "[NF] ETHERNET.XIF_READBLOCK eth%u len=%u -> %u\n",
                nf_get_param(params, 0), len, copied);
      return copied;
    }

    case NFETH_XIF_WRITEBLOCK:
    {
      uae_u32 len = nf_get_param(params, 2);
      uae_u8 frame[PISTORM_NET_FRAME_MAX];
      if (len > PISTORM_NET_FRAME_MAX)
        return (uae_u32)-1;
      for (uae_u32 i = 0; i < len; i++)
        frame[i] = nf_read_byte(nf_get_param(params, 1) + i);
      int rc = pistorm_net_write_frame(frame, len);
      if (nfeth_debug_enabled() || rc != 0)
        fprintf(stderr, "[NF] ETHERNET.XIF_WRITEBLOCK eth%u len=%u -> %d\n",
                nf_get_param(params, 0), len, rc);
      return rc == 0 ? 0 : (uae_u32)-1;
    }

    case NFETH_XIF_GET_MAC:
    {
      uae_u8 mac[6];
      uae_u32 len = nf_get_param(params, 2);
      if (!pistorm_net_is_enabled() || nf_get_param(params, 0) != 0 || len == 0) {
        fprintf(stderr, "[NF] ETHERNET.GET_MAC eth%u len=%u -> 0\n",
                nf_get_param(params, 0), len);
        return 0;
      }
      pistorm_net_get_mac(mac);
      nf_write_buffer(nf_get_param(params, 1), mac, len < sizeof(mac) ? len : sizeof(mac));
      fprintf(stderr, "[NF] ETHERNET.GET_MAC eth%u len=%u -> %02X:%02X:%02X:%02X:%02X:%02X\n",
              nf_get_param(params, 0), len, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      return 1;
    }

    case NFETH_XIF_GET_IPHOST:
      return nfeth_get_text_param(params,
                                  nfeth_config_text("PISTORM_NET_IP_HOST",
                                                    g_nf_config.ip_host,
                                                    "10.0.2.2"));

    case NFETH_XIF_GET_IPATARI:
      return nfeth_get_text_param(params,
                                  nfeth_config_text("PISTORM_NET_IP_ATARI",
                                                    g_nf_config.ip_atari,
                                                    "10.0.2.15"));

    case NFETH_XIF_GET_NETMASK:
      return nfeth_get_text_param(params,
                                  nfeth_config_text("PISTORM_NET_NETMASK",
                                                    g_nf_config.netmask,
                                                    "255.255.255.0"));
  }

  return (uae_u32)-32; /* TOS_ENOSYS */
}

static bool fvdi_offscreen_pixels_enabled(void)
{
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("PISTORM_FVDI_OFFSCREEN_PIXELS");
    enabled = value && value[0] && strcmp(value, "0") != 0;
  }
  return enabled != 0;
}

static uae_u32 fvdi_rgb_to_pixel(uae_u32 red, uae_u32 green, uae_u32 blue)
{
  red = (red * 255u + 500u) / 1000u;
  green = (green * 255u + 500u) / 1000u;
  blue = (blue * 255u + 500u) / 1000u;
  if (red > 255u)
    red = 255u;
  if (green > 255u)
    green = 255u;
  if (blue > 255u)
    blue = 255u;

  if (pistorm_fvdi_bpp() == 16)
    return ((red & 0xf8u) << 8) | ((green & 0xfcu) << 3) | (blue >> 3);

  return (red << 16) | (green << 8) | blue;
}

static uint32_t fvdi_bytes_per_pixel(void)
{
  uint32_t bpp = pistorm_fvdi_bpp();
  if (bpp == 16)
    return 2;
  if (bpp == 24)
    return 3;
  if (bpp == 32)
    return 4;
  return 0;
}

static bool fvdi_mfdb_is_screen(uaecptr mfdb)
{
  if (!mfdb)
    return true;

  uae_u32 addr = nf_read_long(mfdb + 0);
  return addr == 0 || addr == pistorm_fvdi_fb_base();
}

static uint32_t fvdi_mfdb_bpp(uaecptr mfdb)
{
  if (!mfdb)
    return pistorm_fvdi_bpp();
  uint32_t bpp = nf_read_word(mfdb + 12);
  if (bpp == 15)
    bpp = 16;
  return bpp ? bpp : pistorm_fvdi_bpp();
}

static uint32_t fvdi_mfdb_pixel_bytes(uaecptr mfdb)
{
  uint32_t bpp = fvdi_mfdb_bpp(mfdb);
  if (bpp == 16)
    return 2;
  if (bpp == 24)
    return 3;
  if (bpp == 32)
    return 4;
  return 0;
}

static bool fvdi_mfdb_addressable_direct(uaecptr mfdb)
{
  if (!mfdb || fvdi_mfdb_is_screen(mfdb))
    return true;

  uaecptr base = nf_read_long(mfdb + 0);
  uint32_t width = nf_read_word(mfdb + 4);
  uint32_t height = nf_read_word(mfdb + 6);
  uint32_t wdwidth = nf_read_word(mfdb + 8);
  uint32_t bpp = fvdi_mfdb_bpp(mfdb);
  uint32_t pixel_bytes = fvdi_mfdb_pixel_bytes(mfdb);
  uint32_t row_bytes = wdwidth * 2u * bpp;

  if (!base || !width || !height || !wdwidth || !pixel_bytes)
    return false;
  if (bpp != 16 && bpp != 24 && bpp != 32)
    return false;
  return row_bytes >= width * pixel_bytes;
}

static bool fvdi_mfdb_supported_direct(uaecptr mfdb)
{
  if (!fvdi_mfdb_addressable_direct(mfdb))
    return false;
  if (!mfdb || fvdi_mfdb_is_screen(mfdb))
    return true;
  return nf_read_word(mfdb + 10) == 0;
}

static uaecptr fvdi_mfdb_pixel_addr(uaecptr mfdb, int32_t x, int32_t y)
{
  if (!mfdb || x < 0 || y < 0)
    return 0;

  uaecptr base = nf_read_long(mfdb + 0);
  uint32_t width = nf_read_word(mfdb + 4);
  uint32_t height = nf_read_word(mfdb + 6);
  uint32_t wdwidth = nf_read_word(mfdb + 8);
  uint32_t bpp = fvdi_mfdb_bpp(mfdb);
  uint32_t pixel_bytes = fvdi_mfdb_pixel_bytes(mfdb);

  if (!fvdi_mfdb_addressable_direct(mfdb) ||
      !base || !width || !height || !wdwidth || !pixel_bytes ||
      (uint32_t)x >= width || (uint32_t)y >= height)
    return 0;

  return base + (uaecptr)y * wdwidth * 2u * bpp + (uaecptr)x * pixel_bytes;
}

static uint32_t fvdi_mfdb_get_pixel(uaecptr mfdb, int32_t x, int32_t y)
{
  if (!fvdi_mfdb_supported_direct(mfdb))
    return 0;

  uaecptr addr = fvdi_mfdb_pixel_addr(mfdb, x, y);
  uint32_t bytes = fvdi_mfdb_pixel_bytes(mfdb);

  if (!addr)
    return 0;
  if (bytes == 2)
    return nf_read_word(addr);
  if (bytes == 3)
    return ((uint32_t)nf_read_byte(addr + 2) << 16) |
           ((uint32_t)nf_read_byte(addr + 1) << 8) |
           (uint32_t)nf_read_byte(addr);
  if (bytes == 4)
    return nf_read_long(addr);
  return 0;
}

static uint32_t fvdi_mfdb_get_pixel_nf(uaecptr mfdb, int32_t x, int32_t y)
{
  if (!fvdi_offscreen_pixels_enabled())
    return 0;
  return fvdi_mfdb_get_pixel(mfdb, x, y);
}

static bool fvdi_mfdb_put_pixel(uaecptr mfdb, int32_t x, int32_t y, uint32_t colour)
{
  if (!fvdi_mfdb_supported_direct(mfdb))
    return false;
  uaecptr addr = fvdi_mfdb_pixel_addr(mfdb, x, y);
  uint32_t bytes = fvdi_mfdb_pixel_bytes(mfdb);

  if (!addr)
    return false;
  if (bytes == 2) {
    nf_write_word(addr, (uae_u16)colour);
    return true;
  }
  if (bytes == 3) {
    nf_write_byte(addr, (uae_u8)colour);
    nf_write_byte(addr + 1, (uae_u8)(colour >> 8));
    nf_write_byte(addr + 2, (uae_u8)(colour >> 16));
    return true;
  }
  if (bytes == 4) {
    nf_write_long(addr, colour);
    return true;
  }
  return false;
}

static bool fvdi_mfdb_put_pixel_nf(uaecptr mfdb, int32_t x, int32_t y, uint32_t colour)
{
  if (!fvdi_offscreen_pixels_enabled())
    return false;
  return fvdi_mfdb_put_pixel(mfdb, x, y, colour);
}

static bool fvdi_xy_in_bounds(int32_t x, int32_t y);
static uint32_t fvdi_get_raw_pixel(int32_t x, int32_t y);
static void fvdi_put_raw_pixel_marked(int32_t x, int32_t y, uint32_t colour, bool mark);
static void fvdi_note_screen_span(int32_t x, int32_t y, int32_t w);

static uint32_t fvdi_target_get_pixel(uaecptr mfdb, int32_t x, int32_t y)
{
  return fvdi_mfdb_is_screen(mfdb) ? fvdi_get_raw_pixel(x, y)
                                  : fvdi_mfdb_get_pixel(mfdb, x, y);
}

static bool fvdi_target_put_pixel(uaecptr mfdb, int32_t x, int32_t y,
                                  uint32_t colour, bool mark)
{
  if (!fvdi_mfdb_is_screen(mfdb))
    return fvdi_mfdb_put_pixel(mfdb, x, y, colour);

  if (!fvdi_xy_in_bounds(x, y))
    return false;
  fvdi_put_raw_pixel_marked(x, y, colour, mark);
  return true;
}

static bool fvdi_target_bounds(uaecptr mfdb, int32_t *w, int32_t *h)
{
  if (fvdi_mfdb_is_screen(mfdb)) {
    *w = (int32_t)pistorm_fvdi_width();
    *h = (int32_t)pistorm_fvdi_height();
    return *w > 0 && *h > 0;
  }

  if (!fvdi_mfdb_supported_direct(mfdb))
    return false;

  *w = (int32_t)nf_read_word(mfdb + 4);
  *h = (int32_t)nf_read_word(mfdb + 6);
  return *w > 0 && *h > 0;
}

static uint8_t *fvdi_screen_span_ptr(int32_t x, int32_t y, int32_t w)
{
  uint32_t bytes = fvdi_bytes_per_pixel();
  if (bytes == 0 || w <= 0 || x < 0 || y < 0 ||
      (uint32_t)(x + w) > pistorm_fvdi_width() ||
      (uint32_t)y >= pistorm_fvdi_height())
    return NULL;
  return pistorm_fvdi_fb_ptr() +
         ((size_t)(uint32_t)y * pistorm_fvdi_width() + (uint32_t)x) * bytes;
}

static bool fvdi_screen_copy_rows(int32_t src_x, int32_t src_y,
                                  int32_t dst_x, int32_t dst_y,
                                  int32_t w, int32_t h)
{
  uint32_t bytes = fvdi_bytes_per_pixel();
  if (bytes == 0 || w <= 0 || h <= 0)
    return false;

  if (dst_y > src_y) {
    for (int32_t yy = h - 1; yy >= 0; yy--) {
      uint8_t *src = fvdi_screen_span_ptr(src_x, src_y + yy, w);
      uint8_t *dst = fvdi_screen_span_ptr(dst_x, dst_y + yy, w);
      if (!src || !dst)
        return false;
      memmove(dst, src, (size_t)w * bytes);
      fvdi_note_screen_span(dst_x, dst_y + yy, w);
    }
  } else {
    for (int32_t yy = 0; yy < h; yy++) {
      uint8_t *src = fvdi_screen_span_ptr(src_x, src_y + yy, w);
      uint8_t *dst = fvdi_screen_span_ptr(dst_x, dst_y + yy, w);
      if (!src || !dst)
        return false;
      memmove(dst, src, (size_t)w * bytes);
      fvdi_note_screen_span(dst_x, dst_y + yy, w);
    }
  }

  return true;
}

static bool fvdi_xy_in_bounds(int32_t x, int32_t y)
{
  return x >= 0 && y >= 0 &&
         (uint32_t)x < pistorm_fvdi_width() &&
         (uint32_t)y < pistorm_fvdi_height();
}

static uint8_t *fvdi_pixel_ptr(int32_t x, int32_t y)
{
  uint32_t w = pistorm_fvdi_width();
  uint32_t h = pistorm_fvdi_height();
  uint32_t bytes = fvdi_bytes_per_pixel();
  uint8_t *fb = pistorm_fvdi_fb_ptr();

  if (!fb || bytes == 0 || x < 0 || y < 0 || (uint32_t)x >= w || (uint32_t)y >= h)
    return NULL;

  return fb + ((size_t)(uint32_t)y * w + (uint32_t)x) * bytes;
}

static void fvdi_put_raw_pixel_marked(int32_t x, int32_t y, uint32_t colour, bool mark)
{
  uint8_t *p = fvdi_pixel_ptr(x, y);
  uint32_t bytes = fvdi_bytes_per_pixel();
  if (!p)
    return;

  if (bytes == 2) {
    p[0] = (uint8_t)(colour >> 8);
    p[1] = (uint8_t)colour;
  } else {
    p[0] = (uint8_t)(colour >> 24);
    p[1] = (uint8_t)(colour >> 16);
    p[2] = (uint8_t)(colour >> 8);
    p[3] = (uint8_t)colour;
  }
  if (mark)
    pistorm_fvdi_note_host_write(((uint32_t)y * pistorm_fvdi_width() + (uint32_t)x) * bytes,
                                 bytes);
}

static void fvdi_put_raw_pixel(int32_t x, int32_t y, uint32_t colour)
{
  fvdi_put_raw_pixel_marked(x, y, colour, true);
}

static void fvdi_note_screen_span(int32_t x, int32_t y, int32_t w)
{
  uint32_t bytes = fvdi_bytes_per_pixel();
  if (bytes == 0 || w <= 0 || x < 0 || y < 0)
    return;
  pistorm_fvdi_note_host_write(((uint32_t)y * pistorm_fvdi_width() + (uint32_t)x) * bytes,
                               (uint32_t)w * bytes);
}

static void fvdi_fill_solid_span(int32_t x, int32_t y, int32_t w, uint32_t colour)
{
  uint8_t *p = fvdi_pixel_ptr(x, y);
  uint32_t bytes = fvdi_bytes_per_pixel();

  if (!p || w <= 0)
    return;

  if (bytes == 2) {
    uint8_t hi = (uint8_t)(colour >> 8);
    uint8_t lo = (uint8_t)colour;
    for (int32_t i = 0; i < w; i++) {
      p[i * 2] = hi;
      p[i * 2 + 1] = lo;
    }
  } else {
    uint8_t a = (uint8_t)(colour >> 24);
    uint8_t r = (uint8_t)(colour >> 16);
    uint8_t g = (uint8_t)(colour >> 8);
    uint8_t b = (uint8_t)colour;
    for (int32_t i = 0; i < w; i++) {
      p[i * 4] = a;
      p[i * 4 + 1] = r;
      p[i * 4 + 2] = g;
      p[i * 4 + 3] = b;
    }
  }

  fvdi_note_screen_span(x, y, w);
}

static uint32_t fvdi_get_raw_pixel(int32_t x, int32_t y)
{
  uint8_t *p = fvdi_pixel_ptr(x, y);
  if (!p)
    return 0;

  if (pistorm_fvdi_bpp() == 16)
    return ((uint32_t)p[0] << 8) | p[1];

  return ((uint32_t)p[0] << 24) |
         ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) |
         p[3];
}

static uint32_t fvdi_apply_logic(uint32_t dst, uint32_t fg, uint32_t bg,
                                 uint16_t pattern, int x, unsigned mode)
{
  bool bit = (pattern & (1u << (x & 0x0f))) != 0;

  switch (mode) {
    case 1: /* MD_REPLACE */
      return bit ? fg : bg;
    case 2: /* MD_TRANS */
      return bit ? fg : dst;
    case 3: /* MD_XOR */
      return bit ? ~dst : dst;
    case 4: /* MD_ERASE */
      return bit ? dst : fg;
  }

  return bit ? fg : bg;
}

static uint32_t fvdi_apply_mono_logic(uint32_t dst, uint32_t fg, uint32_t bg,
                                      uint16_t pattern, unsigned mode)
{
  bool bit = pattern != 0;

  switch (mode) {
    case 1: /* MD_REPLACE */
      return bit ? fg : bg;
    case 2: /* MD_TRANS */
      return bit ? fg : dst;
    case 3: /* MD_XOR */
      return bit ? ~dst : dst;
    case 4: /* MD_ERASE */
      return bit ? dst : bg;
  }

  return bit ? fg : bg;
}

static uint16_t fvdi_pattern_word(uaecptr pattern, unsigned y)
{
  return pattern ? nf_read_word(pattern + (uaecptr)((y & 0x0f) * 2u)) : 0xffffu;
}

static uae_u32 fvdi_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                              uaecptr pattern, uint32_t fg, uint32_t bg,
                              unsigned mode)
{
  if (w <= 0 || h <= 0)
    return 1;

  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > (int32_t)pistorm_fvdi_width())
    w = (int32_t)pistorm_fvdi_width() - x;
  if (y + h > (int32_t)pistorm_fvdi_height())
    h = (int32_t)pistorm_fvdi_height() - y;
  if (w <= 0 || h <= 0)
    return 1;

  if (!pattern && (mode == 1 || mode == 2)) {
    for (int32_t yy = 0; yy < h; yy++)
      fvdi_fill_solid_span(x, y + yy, w, fg);
    return 1;
  }

  for (int32_t yy = 0; yy < h; yy++) {
    uint16_t pat = fvdi_pattern_word(pattern, (unsigned)(y + yy));
    if (pat == 0xffffu && (mode == 1 || mode == 2)) {
      fvdi_fill_solid_span(x, y + yy, w, fg);
      continue;
    }
    if (pat == 0x0000u && mode == 1) {
      fvdi_fill_solid_span(x, y + yy, w, bg);
      continue;
    }
    for (int32_t xx = 0; xx < w; xx++) {
      int32_t px = x + xx;
      int32_t py = y + yy;
      uint32_t dst = fvdi_get_raw_pixel(px, py);
      fvdi_put_raw_pixel_marked(px, py,
                                fvdi_apply_logic(dst, fg, bg, pat, px, mode),
                                false);
    }
    fvdi_note_screen_span(x, y + yy, w);
  }

  return 1;
}

static uae_u32 fvdi_expand_mono(uaecptr src, uaecptr dst_mfdb,
                                int32_t src_x, int32_t src_y,
                                int32_t dst_x, int32_t dst_y, int32_t w, int32_t h,
                                unsigned mode, uint32_t fg, uint32_t bg)
{
  if (!src || w <= 0 || h <= 0)
    return 1;

  uaecptr data_base = nf_read_long(src + 0);
  int32_t src_w = (int32_t)nf_read_word(src + 4);
  int32_t src_h = (int32_t)nf_read_word(src + 6);
  uint32_t pitch = (uint32_t)nf_read_word(src + 8) * 2u; /* monochrome MFDB */
  uint32_t src_planes = (uint32_t)nf_read_word(src + 12);
  uint32_t src_standard = (uint32_t)nf_read_word(src + 10);
  if (!data_base || pitch == 0)
    return 1;
  if ((src_standard & 0x100u) != 0 || (src_planes != 0 && src_planes != 1))
    return 1;

  int32_t dst_w;
  int32_t dst_h;
  if (!fvdi_target_bounds(dst_mfdb, &dst_w, &dst_h))
    return 1;

  if ((int64_t)w * (int64_t)h > FVDI_MAX_ACCEL_PIXELS)
    return 1;
  if (w > FVDI_MAX_ACCEL_SPAN || h > FVDI_MAX_ACCEL_SPAN)
    return 1;

  if (src_x < 0) {
    dst_x -= src_x;
    w += src_x;
    src_x = 0;
  }
  if (src_y < 0) {
    dst_y -= src_y;
    h += src_y;
    src_y = 0;
  }
  if (src_w > 0 && src_x + w > src_w)
    w = src_w - src_x;
  if (src_h > 0 && src_y + h > src_h)
    h = src_h - src_y;

  if (dst_x < 0) {
    src_x -= dst_x;
    w += dst_x;
    dst_x = 0;
  }
  if (dst_y < 0) {
    src_y -= dst_y;
    h += dst_y;
    dst_y = 0;
  }
  if (dst_x + w > dst_w)
    w = dst_w - dst_x;
  if (dst_y + h > dst_h)
    h = dst_h - dst_y;
  if (src_w > 0 && src_x + w > src_w)
    w = src_w - src_x;
  if (src_h > 0 && src_y + h > src_h)
    h = src_h - src_y;
  if (w <= 0 || h <= 0)
    return 1;

  uaecptr data = data_base + (uaecptr)src_y * pitch;

  for (int32_t yy = 0; yy < h; yy++) {
    uint16_t word = 0;
    int32_t word_base = -1;
    for (int32_t xx = 0; xx < w; xx++) {
      int32_t sx = src_x + xx;
      int32_t base = sx & ~15;
      if (base != word_base) {
        word_base = base;
        word = nf_read_word(data + (uaecptr)yy * pitch + (uaecptr)((sx >> 3) & ~1));
      }
      uint16_t bit_pattern = ((word >> (15 - (sx & 0x0f))) & 1u) ? 0xffffu : 0x0000u;
      int32_t px = dst_x + xx;
      int32_t py = dst_y + yy;
      uint32_t dst = fvdi_target_get_pixel(dst_mfdb, px, py);
      fvdi_target_put_pixel(dst_mfdb, px, py,
                            fvdi_apply_mono_logic(dst, fg, bg, bit_pattern, mode),
                            false);
    }
    if (fvdi_mfdb_is_screen(dst_mfdb))
      fvdi_note_screen_span(dst_x, dst_y + yy, w);
  }

  return 1;
}

static int fvdi_compare_int16(const void *a, const void *b)
{
  int av = *(const int16_t *)a;
  int bv = *(const int16_t *)b;
  return (av > bv) - (av < bv);
}

static uae_u32 fvdi_fill_polygon(uaecptr points, int32_t count,
                                 uaecptr index_addr, int32_t moves,
                                 uaecptr pattern, uint32_t fg, uint32_t bg,
                                 unsigned mode, uaecptr clip)
{
  if (!points || count <= 0)
    return 1;
  if (count > 4096 || moves > 4096)
    return (uae_u32)-1;

  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t max_x = (int32_t)pistorm_fvdi_width() - 1;
  int32_t max_y = (int32_t)pistorm_fvdi_height() - 1;

  if (clip) {
    min_x = (int32_t)nf_read_long(clip + 0);
    min_y = (int32_t)nf_read_long(clip + 4);
    max_x = (int32_t)nf_read_long(clip + 8);
    max_y = (int32_t)nf_read_long(clip + 12);
  }

  if (min_x < 0)
    min_x = 0;
  if (min_y < 0)
    min_y = 0;
  if (max_x >= (int32_t)pistorm_fvdi_width())
    max_x = (int32_t)pistorm_fvdi_width() - 1;
  if (max_y >= (int32_t)pistorm_fvdi_height())
    max_y = (int32_t)pistorm_fvdi_height() - 1;
  if (min_x > max_x || min_y > max_y)
    return 1;

  int16_t *px = (int16_t *)malloc((size_t)count * sizeof(*px));
  int16_t *py = (int16_t *)malloc((size_t)count * sizeof(*py));
  int16_t *indices = NULL;
  int16_t *crossings = (int16_t *)malloc((size_t)count * sizeof(*crossings));
  if (!px || !py || !crossings) {
    free(px);
    free(py);
    free(crossings);
    return (uae_u32)-1;
  }

  for (int32_t i = 0; i < count; i++) {
    px[i] = (int16_t)nf_read_word(points + (uaecptr)i * 4u);
    py[i] = (int16_t)nf_read_word(points + (uaecptr)i * 4u + 2u);
  }

  bool use_indices = moves > 0 && index_addr != 0;
  if (use_indices) {
    indices = (int16_t *)malloc((size_t)moves * sizeof(*indices));
    if (!indices) {
      free(px);
      free(py);
      free(crossings);
      return (uae_u32)-1;
    }
    for (int32_t i = 0; i < moves; i++)
      indices[i] = (int16_t)nf_read_word(index_addr + (uaecptr)i * 2u);
    while (moves > 0 && indices[moves - 1] == -4)
      moves--;
    while (moves > 0 && indices[moves - 1] == -2)
      moves--;
    use_indices = moves > 0;
  }

  if (!use_indices && count > 1 && px[0] == px[count - 1] && py[0] == py[count - 1])
    count--;
  if (count <= 0) {
    free(indices);
    free(crossings);
    free(px);
    free(py);
    return 1;
  }

  int32_t poly_min_y = py[0];
  int32_t poly_max_y = py[0];
  for (int32_t i = 1; i < count; i++) {
    if (py[i] < poly_min_y)
      poly_min_y = py[i];
    if (py[i] > poly_max_y)
      poly_max_y = py[i];
  }
  if (poly_min_y < min_y)
    poly_min_y = min_y;
  if (poly_max_y > max_y)
    poly_max_y = max_y;

  for (int32_t y = poly_min_y; y <= poly_max_y; y++) {
    int32_t ints = 0;
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;
    int32_t move_n = 0;
    int32_t movepnt = -1;

    if (use_indices) {
      move_n = moves - 1;
      movepnt = (indices[move_n] + 4) / 2;
      x2 = px[0];
      y2 = py[0];
    } else {
      x1 = px[count - 1];
      y1 = py[count - 1];
    }

    for (int32_t i = use_indices ? 1 : 0; i < count && ints < count; i++) {
      if (use_indices) {
        x1 = x2;
        y1 = y2;
      }
      x2 = px[i];
      y2 = py[i];
      if (use_indices && i == movepnt) {
        if (--move_n >= 0)
          movepnt = (indices[move_n] + 4) / 2;
        else
          movepnt = -1;
        continue;
      }

      if (y1 < y2) {
        if (y >= y1 && y < y2)
          crossings[ints++] = (int16_t)(x1 + ((int64_t)(y - y1) * (x2 - x1)) / (y2 - y1));
      } else if (y1 > y2) {
        if (y >= y2 && y < y1)
          crossings[ints++] = (int16_t)(x2 + ((int64_t)(y - y2) * (x1 - x2)) / (y1 - y2));
      }

      if (!use_indices) {
        x1 = x2;
        y1 = y2;
      }
    }

    qsort(crossings, (size_t)ints, sizeof(*crossings), fvdi_compare_int16);
    for (int32_t i = 0; i < ints - 1; i += 2) {
      int32_t span_x1 = crossings[i];
      int32_t span_x2 = crossings[i + 1];
      if (span_x1 < min_x)
        span_x1 = min_x;
      if (span_x2 > max_x)
        span_x2 = max_x;
      if (span_x1 <= span_x2)
        fvdi_fill_rect(span_x1, y, span_x2 - span_x1 + 1, 1, pattern, fg, bg, mode);
    }
  }

  free(indices);
  free(crossings);
  free(px);
  free(py);
  return 1;
}

static uae_u32 fvdi_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                              uint32_t fg, uint32_t bg, uint16_t pattern,
                              unsigned mode, uaecptr clip)
{
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t max_x = (int32_t)pistorm_fvdi_width() - 1;
  int32_t max_y = (int32_t)pistorm_fvdi_height() - 1;
  int32_t dx = abs(x2 - x1);
  int32_t sx = x1 < x2 ? 1 : -1;
  int32_t dy = -abs(y2 - y1);
  int32_t sy = y1 < y2 ? 1 : -1;
  int32_t err = dx + dy;
  unsigned step = 0;

  if (dx > FVDI_MAX_ACCEL_SPAN || -dy > FVDI_MAX_ACCEL_SPAN)
    return 1;

  if (clip) {
    min_x = (int32_t)nf_read_long(clip + 0);
    min_y = (int32_t)nf_read_long(clip + 4);
    max_x = (int32_t)nf_read_long(clip + 8);
    max_y = (int32_t)nf_read_long(clip + 12);
  }

  for (;;) {
    if (x1 >= min_x && x1 <= max_x && y1 >= min_y && y1 <= max_y) {
      uint32_t dst = fvdi_get_raw_pixel(x1, y1);
      uint16_t pat = (pattern & (1u << (15 - (step & 0x0f)))) ? 0xffffu : 0u;
      fvdi_put_raw_pixel(x1, y1, fvdi_apply_logic(dst, fg, bg, pat, 0, mode));
    }
    if (x1 == x2 && y1 == y2)
      break;
    int32_t e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x1 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y1 += sy;
    }
    step++;
  }

  return 1;
}

static bool fvdi_line_coord_plausible(int32_t value)
{
  return value >= -FVDI_MAX_ACCEL_SPAN && value <= FVDI_MAX_ACCEL_SPAN;
}

static bool fvdi_line_coords_plausible(int32_t x1, int32_t y1,
                                       int32_t x2, int32_t y2)
{
  return fvdi_line_coord_plausible(x1) && fvdi_line_coord_plausible(y1) &&
         fvdi_line_coord_plausible(x2) && fvdi_line_coord_plausible(y2);
}

static uae_u32 fvdi_draw_line_table(uaecptr table, uint32_t table_spec,
                                    uaecptr index, uint32_t moves,
                                    uint32_t fg, uint32_t bg,
                                    uint16_t pattern, unsigned mode,
                                    uaecptr clip)
{
  uint32_t type = table_spec & 0xffffu;
  uint32_t length = table_spec >> 16;
  int32_t movepnt = -1;

  if (!table || length > 4096 || type > 1)
    return (uae_u32)-1;
  if (length == 0)
    return 1;

  if (type == 1) {
    if (!index)
      return (uae_u32)-1;
    if (moves > 4096)
      return (uae_u32)-1;
    while (moves > 0 && (int16_t)nf_read_word(index + (uaecptr)(moves - 1) * 2u) == -4)
      moves--;
    while (moves > 0 && (int16_t)nf_read_word(index + (uaecptr)(moves - 1) * 2u) == -2)
      moves--;
    if (moves > 0)
      movepnt = ((int16_t)nf_read_word(index + (uaecptr)(moves - 1) * 2u) + 4) / 2;
  }

  int32_t init_x = (int16_t)nf_read_word(table + 0);
  int32_t init_y = (int16_t)nf_read_word(table + 2);
  int32_t x1 = init_x;
  int32_t y1 = init_y;

  if (length == 1) {
    int32_t x2 = (int16_t)nf_read_word(table + 4);
    int32_t y2 = (int16_t)nf_read_word(table + 6);
    if (fvdi_line_coords_plausible(x1, y1, x2, y2))
      fvdi_draw_line(x1, y1, x2, y2, fg, bg, pattern, mode, clip);
    return 1;
  }

  for (uint32_t n = 1; n < length; n++) {
    int32_t x2 = (int16_t)nf_read_word(table + (uaecptr)n * 4u);
    int32_t y2 = (int16_t)nf_read_word(table + (uaecptr)n * 4u + 2u);

    if ((int32_t)n == movepnt) {
      if (moves > 0)
        moves--;
      if (moves > 0)
        movepnt = ((int16_t)nf_read_word(index + (uaecptr)(moves - 1) * 2u) + 4) / 2;
      else
        movepnt = -1;
      init_x = x1 = x2;
      init_y = y1 = y2;
      continue;
    }

    (void)init_x;
    (void)init_y;
    if (fvdi_line_coords_plausible(x1, y1, x2, y2))
      fvdi_draw_line(x1, y1, x2, y2, fg, bg, pattern, mode, clip);

    x1 = x2;
    y1 = y2;
  }

  return 1;
}

static uae_u32 fvdi_text_area(uaecptr text, int32_t length,
                              int32_t dst_x, int32_t dst_y,
                              uaecptr font, int32_t cell_w, int32_t h,
                              uint32_t fg, uint32_t bg, unsigned mode,
                              uaecptr clip)
{
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t max_x = (int32_t)pistorm_fvdi_width() - 1;
  int32_t max_y = (int32_t)pistorm_fvdi_height() - 1;

  if (!text || !font || length <= 0 || cell_w != 8 || h <= 0 || h > 16)
    return 0;

  if (clip) {
    min_x = (int32_t)nf_read_long(clip + 0);
    min_y = (int32_t)nf_read_long(clip + 4);
    max_x = (int32_t)nf_read_long(clip + 8);
    max_y = (int32_t)nf_read_long(clip + 12);
  }

  if (max_x < 0 || max_y < 0 ||
      min_x >= (int32_t)pistorm_fvdi_width() ||
      min_y >= (int32_t)pistorm_fvdi_height())
    return 1;

  if (min_x < 0)
    min_x = 0;
  if (min_y < 0)
    min_y = 0;
  if (max_x >= (int32_t)pistorm_fvdi_width())
    max_x = (int32_t)pistorm_fvdi_width() - 1;
  if (max_y >= (int32_t)pistorm_fvdi_height())
    max_y = (int32_t)pistorm_fvdi_height() - 1;

  for (int32_t row = 0; row < h; row++) {
    int32_t y = dst_y + row;
    if (y < min_y || y > max_y)
      continue;

    int32_t span_start = 0;
    int32_t span_end = 0;
    bool have_span = false;

    for (int32_t i = 0; i < length; i++) {
      uint16_t ch = nf_read_word(text + (uaecptr)i * 2u);
      uint8_t bits = nf_read_byte(font + (uaecptr)(ch & 0xffu) * 16u + (uaecptr)row);
      int32_t x = dst_x + i * 8;

      for (int32_t col = 0; col < 8; col++, x++) {
        if (x < min_x || x > max_x)
          continue;

        uint16_t pat = (bits & (0x80u >> col)) ? 0xffffu : 0x0000u;
        uint32_t dst = fvdi_get_raw_pixel(x, y);
        fvdi_put_raw_pixel_marked(x, y,
                                  fvdi_apply_logic(dst, fg, bg, pat, 0, mode),
                                  false);
        if (!have_span) {
          span_start = span_end = x;
          have_span = true;
        } else {
          if (x < span_start)
            span_start = x;
          if (x > span_end)
            span_end = x;
        }
      }
    }

    if (have_span)
      fvdi_note_screen_span(span_start, y, span_end - span_start + 1);
  }

  return 1;
}

static uint32_t fvdi_apply_raster_op(uint32_t src, uint32_t dst, unsigned op)
{
  switch (op & 15u) {
    case 0:  return 0;
    case 1:  return src & dst;
    case 2:  return src & ~dst;
    case 3:  return src;
    case 4:  return ~src & dst;
    case 5:  return dst;
    case 6:  return src ^ dst;
    case 7:  return src | dst;
    case 8:  return ~(src | dst);
    case 9:  return ~(src ^ dst);
    case 10: return ~dst;
    case 11: return src | ~dst;
    case 12: return ~src;
    case 13: return ~src | dst;
    case 14: return ~(src & dst);
    case 15: return 0xffffffffu;
  }
  return src;
}

static uae_u32 fvdi_blit_area(uaecptr src_mfdb, uaecptr dst_mfdb,
                              int32_t src_x, int32_t src_y,
                              int32_t dst_x, int32_t dst_y,
                              int32_t w, int32_t h, unsigned op)
{
  if (w <= 0 || h <= 0)
    return 1;

  if ((int64_t)w * (int64_t)h > FVDI_MAX_ACCEL_PIXELS)
    return 1;
  if (w > FVDI_MAX_ACCEL_SPAN || h > FVDI_MAX_ACCEL_SPAN)
    return 1;

  int32_t src_w;
  int32_t src_h;
  int32_t dst_w;
  int32_t dst_h;
  if (!fvdi_target_bounds(src_mfdb, &src_w, &src_h) ||
      !fvdi_target_bounds(dst_mfdb, &dst_w, &dst_h))
    return 1;

  if (src_x < 0) {
    dst_x -= src_x;
    w += src_x;
    src_x = 0;
  }
  if (src_y < 0) {
    dst_y -= src_y;
    h += src_y;
    src_y = 0;
  }
  if (dst_x < 0) {
    src_x -= dst_x;
    w += dst_x;
    dst_x = 0;
  }
  if (dst_y < 0) {
    src_y -= dst_y;
    h += dst_y;
    dst_y = 0;
  }
  if (src_x + w > src_w)
    w = src_w - src_x;
  if (src_y + h > src_h)
    h = src_h - src_y;
  if (dst_x + w > dst_w)
    w = dst_w - dst_x;
  if (dst_y + h > dst_h)
    h = dst_h - dst_y;
  if (src_x + w > src_w)
    w = src_w - src_x;
  if (src_y + h > src_h)
    h = src_h - src_y;
  if (w <= 0 || h <= 0)
    return 1;

  if (op == 3 && fvdi_mfdb_is_screen(src_mfdb) && fvdi_mfdb_is_screen(dst_mfdb) &&
      fvdi_screen_copy_rows(src_x, src_y, dst_x, dst_y, w, h))
    return 1;

  int y_start = 0;
  int y_end = h;
  int y_step = 1;
  if (fvdi_mfdb_is_screen(src_mfdb) == fvdi_mfdb_is_screen(dst_mfdb) &&
      dst_y > src_y) {
    y_start = h - 1;
    y_end = -1;
    y_step = -1;
  }

  for (int yy = y_start; yy != y_end; yy += y_step) {
    if (src_mfdb == dst_mfdb && dst_y + yy == src_y + yy && dst_x > src_x) {
      for (int32_t xx = w - 1; xx >= 0; xx--) {
        uint32_t src = fvdi_target_get_pixel(src_mfdb, src_x + xx, src_y + yy);
        uint32_t dst = fvdi_target_get_pixel(dst_mfdb, dst_x + xx, dst_y + yy);
        fvdi_target_put_pixel(dst_mfdb, dst_x + xx, dst_y + yy,
                              fvdi_apply_raster_op(src, dst, op), false);
      }
    } else {
      for (int32_t xx = 0; xx < w; xx++) {
        uint32_t src = fvdi_target_get_pixel(src_mfdb, src_x + xx, src_y + yy);
        uint32_t dst = fvdi_target_get_pixel(dst_mfdb, dst_x + xx, dst_y + yy);
        fvdi_target_put_pixel(dst_mfdb, dst_x + xx, dst_y + yy,
                              fvdi_apply_raster_op(src, dst, op), false);
      }
    }
    if (fvdi_mfdb_is_screen(dst_mfdb))
      fvdi_note_screen_span(dst_x, dst_y + yy, w);
  }

  return 1;
}

typedef struct fvdi_mouse_state {
  bool shape_set;
  bool visible;
  uint16_t mask[16];
  uint16_t data[16];
  uint32_t fg;
  uint32_t bg;
  int16_t hot_x;
  int16_t hot_y;
  int32_t backup_x;
  int32_t backup_y;
  int32_t backup_w;
  int32_t backup_h;
  uint32_t backup[16 * 16];
} fvdi_mouse_state_t;

static fvdi_mouse_state_t g_fvdi_mouse;

static void fvdi_mouse_hide(void)
{
  if (!g_fvdi_mouse.visible)
    return;

  for (int32_t yy = 0; yy < g_fvdi_mouse.backup_h; yy++) {
    for (int32_t xx = 0; xx < g_fvdi_mouse.backup_w; xx++) {
      fvdi_put_raw_pixel(g_fvdi_mouse.backup_x + xx,
                         g_fvdi_mouse.backup_y + yy,
                         g_fvdi_mouse.backup[yy * 16 + xx]);
    }
  }

  g_fvdi_mouse.visible = false;
}

static void fvdi_mouse_show(int32_t x, int32_t y)
{
  if (!g_fvdi_mouse.shape_set)
    return;

  if (g_fvdi_mouse.visible)
    fvdi_mouse_hide();

  x -= g_fvdi_mouse.hot_x;
  y -= g_fvdi_mouse.hot_y;

  int32_t sx = x < 0 ? -x : 0;
  int32_t sy = y < 0 ? -y : 0;
  int32_t dx = x + sx;
  int32_t dy = y + sy;
  int32_t w = 16 - sx;
  int32_t h = 16 - sy;

  if (dx + w > (int32_t)pistorm_fvdi_width())
    w = (int32_t)pistorm_fvdi_width() - dx;
  if (dy + h > (int32_t)pistorm_fvdi_height())
    h = (int32_t)pistorm_fvdi_height() - dy;
  if (w <= 0 || h <= 0)
    return;

  g_fvdi_mouse.backup_x = dx;
  g_fvdi_mouse.backup_y = dy;
  g_fvdi_mouse.backup_w = w;
  g_fvdi_mouse.backup_h = h;

  for (int32_t yy = 0; yy < h; yy++) {
    uint16_t mask = g_fvdi_mouse.mask[sy + yy] << sx;
    uint16_t data = g_fvdi_mouse.data[sy + yy] << sx;
    for (int32_t xx = 0; xx < w; xx++) {
      uint32_t dst = fvdi_get_raw_pixel(dx + xx, dy + yy);
      g_fvdi_mouse.backup[yy * 16 + xx] = dst;
      if (data & 0x8000u)
        fvdi_put_raw_pixel(dx + xx, dy + yy, g_fvdi_mouse.fg);
      else if (mask & 0x8000u)
        fvdi_put_raw_pixel(dx + xx, dy + yy, g_fvdi_mouse.bg);
      mask <<= 1;
      data <<= 1;
    }
  }

  g_fvdi_mouse.visible = true;
}

static uae_u32 fvdi_mouse_call(uaecptr params)
{
  int32_t x = (int32_t)nf_get_param(params, 1);
  int32_t y = (int32_t)nf_get_param(params, 2);
  uae_u32 mouse = nf_get_param(params, 3);

  if (mouse > 7u) {
    fvdi_mouse_hide();
    uaecptr mask = nf_get_param(params, 3);
    uaecptr data = nf_get_param(params, 4);
    for (unsigned i = 0; i < 16; i++) {
      g_fvdi_mouse.mask[i] = nf_read_word(mask + (uaecptr)i * 2u);
      g_fvdi_mouse.data[i] = nf_read_word(data + (uaecptr)i * 2u);
    }
    g_fvdi_mouse.hot_x = (int16_t)nf_get_param(params, 5);
    g_fvdi_mouse.hot_y = (int16_t)nf_get_param(params, 6);
    g_fvdi_mouse.fg = nf_get_param(params, 7);
    g_fvdi_mouse.bg = nf_get_param(params, 8);
    g_fvdi_mouse.shape_set = true;
    return 1;
  }

  switch (mouse) {
    case 0: /* Move visible */
    case 4: /* Move visible forced */
      fvdi_mouse_show(x, y);
      return 1;
    case 1: /* Move hidden */
    case 5: /* Move hidden forced */
      return 1;
    case 2: /* Hide */
      fvdi_mouse_hide();
      return 1;
    case 3: /* Show */
      fvdi_mouse_show(x, y);
      return 1;
  }

  return 0;
}

static uae_u32 fvdi_fill_table(uaecptr table, int32_t n, uaecptr pattern,
                               uint32_t fg, uint32_t bg, unsigned mode)
{
  if (!table || n <= 0)
    return 0;

  for (int32_t i = 0; i < n; i++) {
    int32_t y = (int16_t)nf_read_word(table + (uaecptr)i * 6u + 0);
    int32_t x1 = (int16_t)nf_read_word(table + (uaecptr)i * 6u + 2);
    int32_t x2 = (int16_t)nf_read_word(table + (uaecptr)i * 6u + 4);
    if (x2 < x1)
      continue;
    fvdi_fill_rect(x1, y, x2 - x1 + 1, 1, pattern, fg, bg, mode);
  }

  return 1;
}

static uae_u32 nf_call_fvdi(uae_u32 subid, uaecptr params)
{
  switch (subid) {
    case FVDI_GET_VERSION:
      return FVDIDRV_NFAPI_VERSION;

    case FVDI_GET_FBADDR:
      return pistorm_fvdi_fb_base();

    case FVDI_SET_RESOLUTION:
    {
      uae_u32 width = nf_get_param(params, 0);
      uae_u32 height = nf_get_param(params, 1);
      uae_u32 depth = nf_get_param(params, 2);
      int ok = pistorm_fvdi_set_mode(width, height, depth);
      return ok ? 0 : TOS_ENOSYS;
    }

    case FVDI_GET_WIDTH:
      return pistorm_fvdi_width();

    case FVDI_GET_HEIGHT:
      return pistorm_fvdi_height();

    case FVDI_GETBPP:
      return pistorm_fvdi_bpp();

    case FVDI_OPENWK:
      return 1;

    case FVDI_CLOSEWK:
      return 1;

    case FVDI_GET_HWCOLOR:
    {
      uae_u32 red = nf_get_param(params, 1);
      uae_u32 green = nf_get_param(params, 2);
      uae_u32 blue = nf_get_param(params, 3);
      uae_u32 pixel = fvdi_rgb_to_pixel(red, green, blue);
      uaecptr out = nf_get_param(params, 4);
      if (out)
        nf_write_long(out, pixel);
      return pixel;
    }

    case FVDI_SET_COLOR:
      return 1;

    case FVDI_EVENT:
    {
      uae_u32 mode = nf_get_param(params, 0);
      if (mode == 0)
        return 0;
      if (mode == 1)
        return 1;

      uaecptr events = nf_get_param(params, 1);
      if (events)
        nf_write_long(events, 0);
      return 1;
    }

    case FVDI_MOUSE:
      return fvdi_mouse_call(params);

    case FVDI_GET_PIXEL:
    {
      uaecptr mfdb = nf_get_param(params, 1);
      int32_t x = (int32_t)nf_get_param(params, 2);
      int32_t y = (int32_t)nf_get_param(params, 3);
      if (!fvdi_mfdb_is_screen(mfdb))
        return fvdi_mfdb_get_pixel_nf(mfdb, x, y);
      return fvdi_get_raw_pixel(x, y);
    }

    case FVDI_PUT_PIXEL:
    {
      uaecptr vwk = nf_get_param(params, 0);
      uaecptr mfdb = nf_get_param(params, 1);
      int32_t x = (int32_t)nf_get_param(params, 2);
      int32_t y = (int32_t)nf_get_param(params, 3);
      uint32_t colour = nf_get_param(params, 4);
      if (vwk & 1u)
        return 0;
      bool screen = fvdi_mfdb_is_screen(mfdb);
      bool in_bounds = fvdi_xy_in_bounds(x, y);
      if (!screen) {
        bool direct = fvdi_mfdb_supported_direct(mfdb);
        if (direct)
          fvdi_mfdb_put_pixel_nf(mfdb, x, y, colour);
        return 1;
      }
      if (!in_bounds)
        return 1;
      fvdi_put_raw_pixel(x, y, colour);
      return 1;
    }

    case FVDI_EXPAND_AREA:
    {
      uaecptr src = nf_get_param(params, 1);
      int32_t src_x = (int32_t)nf_get_param(params, 2);
      int32_t src_y = (int32_t)nf_get_param(params, 3);
      uaecptr dst = nf_get_param(params, 4);
      int32_t dst_x = (int32_t)nf_get_param(params, 5);
      int32_t dst_y = (int32_t)nf_get_param(params, 6);
      int32_t w = (int32_t)nf_get_param(params, 7);
      int32_t h = (int32_t)nf_get_param(params, 8);
      unsigned mode = nf_get_param(params, 9);
      uint32_t fg = nf_get_param(params, 10);
      uint32_t bg = nf_get_param(params, 11);
      return fvdi_expand_mono(src, dst, src_x, src_y, dst_x, dst_y, w, h, mode, fg, bg);
    }

    case FVDI_FILL_AREA:
    {
      uae_u32 vwk = nf_get_param(params, 0);
      int32_t x = (int32_t)nf_get_param(params, 1);
      int32_t y = (int32_t)nf_get_param(params, 2);
      int32_t w = (int32_t)nf_get_param(params, 3);
      int32_t h = (int32_t)nf_get_param(params, 4);
      uaecptr pattern = nf_get_param(params, 5);
      uint32_t fg = nf_get_param(params, 6);
      uint32_t bg = nf_get_param(params, 7);
      unsigned mode = nf_get_param(params, 8);
      if (vwk & 1u) {
        if (((uint32_t)y & 0xffffu) != 0)
          return (uae_u32)-1;
        return fvdi_fill_table((uaecptr)(uint32_t)x,
                               (int32_t)((uint32_t)y >> 16),
                               pattern, fg, bg, mode);
      }
      return fvdi_fill_rect(x, y, w, h, pattern, fg, bg, mode);
    }

    case FVDI_BLIT_AREA:
    {
      uaecptr src = nf_get_param(params, 1);
      int32_t src_x = (int32_t)nf_get_param(params, 2);
      int32_t src_y = (int32_t)nf_get_param(params, 3);
      uaecptr dst = nf_get_param(params, 4);
      int32_t dst_x = (int32_t)nf_get_param(params, 5);
      int32_t dst_y = (int32_t)nf_get_param(params, 6);
      int32_t w = (int32_t)nf_get_param(params, 7);
      int32_t h = (int32_t)nf_get_param(params, 8);
      unsigned op = nf_get_param(params, 9);
      return fvdi_blit_area(src, dst, src_x, src_y, dst_x, dst_y, w, h, op);
    }

    case FVDI_LINE:
    {
      uaecptr vwk = nf_get_param(params, 0);
      int32_t x1 = (int32_t)nf_get_param(params, 1);
      int32_t y1 = (int32_t)nf_get_param(params, 2);
      int32_t x2 = (int32_t)nf_get_param(params, 3);
      int32_t y2 = (int32_t)nf_get_param(params, 4);
      uint16_t pattern = (uint16_t)nf_get_param(params, 5);
      uint32_t fg = nf_get_param(params, 6);
      uint32_t bg = nf_get_param(params, 7);
      unsigned mode = nf_get_param(params, 8);
      uaecptr clip = nf_get_param(params, 9);

      if (vwk & 1u)
        return fvdi_draw_line_table((uaecptr)(uint32_t)x1, (uint32_t)y1,
                                    (uaecptr)(uint32_t)y2, (uint32_t)x2 & 0xffffu,
                                    fg, bg, pattern, mode, clip);
      if (!fvdi_line_coords_plausible(x1, y1, x2, y2))
        return 1;
      return fvdi_draw_line(x1, y1, x2, y2, fg, bg, pattern, mode, clip);
    }

    case FVDI_FILL_POLYGON:
    {
      uae_u32 vwk = nf_get_param(params, 0);
      uaecptr points = nf_get_param(params, 1);
      int32_t count = (int32_t)nf_get_param(params, 2);
      uaecptr index = nf_get_param(params, 3);
      int32_t moves = (int32_t)nf_get_param(params, 4);
      uaecptr pattern = nf_get_param(params, 5);
      uint32_t fg = nf_get_param(params, 6);
      uint32_t bg = nf_get_param(params, 7);
      unsigned mode = nf_get_param(params, 8);
      uaecptr clip = nf_get_param(params, 10);
      if (vwk & 1u)
        return (uae_u32)-1;
      return fvdi_fill_polygon(points, count, index, moves, pattern, fg, bg, mode, clip);
    }

    case FVDI_TEXT_AREA:
    {
      return 0;
    }

    case FVDI_GETCOMPONENT:
      return 0;
  }

  return TOS_ENOSYS;
}

static uae_u32 nf_call_hostfs(uae_u32 subid, uaecptr params)
{
  if (!hostfs_is_enabled())
    return 0;

  switch (subid) {
    case HOSTFS_GET_VERSION:
      HOSTFS_LOG("[NF] HOSTFS.GET_VERSION -> %u\n", HOSTFS_NFAPI_VERSION);
      return HOSTFS_NFAPI_VERSION;

    case HOSTFS_GET_DRIVE_BITS:
    {
      uae_u32 bits = hostfs_drive_bits();
      HOSTFS_LOG("[NF] HOSTFS.GET_DRIVE_BITS -> 0x%08X\n", bits);
      return bits;
    }

    case HOSTFS_XFS_INIT:
    {
      uae_u32 dev = nf_get_param(params, 0);
      uaecptr mountpoint = nf_get_param(params, 1);
      uae_u32 half_sensitive = nf_get_param(params, 3);
      uaecptr filesys = nf_get_param(params, 4);
      uaecptr filesys_devdrv = nf_get_param(params, 5);
      int idx = hostfs_drive_index_from_dev(dev);
      if (idx < 0)
        idx = hostfs_drive_index_from_mountpoint(mountpoint);
      if (idx < 0 ||
          !g_nf_config.hostfs[idx].enabled ||
          !g_nf_config.hostfs[idx].path[0]) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_INIT dev=%u -> EDRIVE\n", dev);
        return TOS_EDRIVE;
      }

      g_hostfs_mounts[idx].mounted = true;
      g_hostfs_mounts[idx].dev = dev ? (uint16_t)dev : (uint16_t)(HOSTFS_MINT_DEV_BASE + idx);
      g_hostfs_mounts[idx].fs_ptr = filesys;
      g_hostfs_mounts[idx].fs_devdrv_ptr = filesys_devdrv;
      g_hostfs_mounts[idx].root_cookie = g_hostfs_next_cookie++;
      g_hostfs_mounts[idx].drive = &g_nf_config.hostfs[idx];
      nf_read_string(mountpoint, g_hostfs_mounts[idx].mountpoint,
                     sizeof(g_hostfs_mounts[idx].mountpoint));

      HOSTFS_LOG("[NF] HOSTFS.XFS_INIT dev=%u idx=%d mount='%s' path='%s' half=%u fs=0x%08X devdrv=0x%08X -> 0\n",
                 dev, idx, g_hostfs_mounts[idx].mountpoint,
                 g_nf_config.hostfs[idx].path, half_sensitive,
                 filesys, filesys_devdrv);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_ROOT:
    {
      uae_u32 dev = nf_get_param(params, 0);
      uaecptr cookie = nf_get_param(params, 1);
      int idx = hostfs_mounted_index_from_dev(dev);
      if (idx < 0 && dev < ATARI_NATFEAT_HOSTFS_MAX_DRIVES &&
          g_hostfs_mounts[dev].mounted)
        idx = (int)dev;
      if (idx < 0 || !g_hostfs_mounts[idx].mounted || !cookie) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_ROOT dev=%u cookie=0x%08X -> EDRIVE\n",
                dev, cookie);
        return TOS_EDRIVE;
      }

      g_hostfs_mounts[idx].dev = (uint16_t)dev;
      hostfs_write_cookie(cookie,
                          g_hostfs_mounts[idx].fs_ptr,
                          g_hostfs_mounts[idx].dev,
                          0,
                          g_hostfs_mounts[idx].root_cookie);
      HOSTFS_LOG("[NF] HOSTFS.XFS_ROOT dev=%u cookie=0x%08X index=%u -> 0\n",
              dev, cookie, g_hostfs_mounts[idx].root_cookie);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_LOOKUP:
    {
      uaecptr dir_cookie = nf_get_param(params, 0);
      uaecptr name_ptr = nf_get_param(params, 1);
      uaecptr out_cookie = nf_get_param(params, 2);
      char name[256];
      char resolved[HOSTFS_HOST_PATH_MAX];
      struct stat st;
      int idx = hostfs_mounted_index_from_cookie(dir_cookie);
      const char *base = hostfs_path_from_cookie(dir_cookie);

      nf_read_string(name_ptr, name, sizeof(name));
      if (idx < 0 || !base || !out_cookie || !hostfs_name_is_safe(name)) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_LOOKUP dir=0x%08X name='%s' out=0x%08X -> ENOENT\n",
                dir_cookie, name, out_cookie);
        return TOS_ENOENT;
      }

      if (name[0] == '\0' || strcmp(name, ".") == 0) {
        hostfs_copy_cookie(out_cookie, dir_cookie);
        HOSTFS_LOG("[NF] HOSTFS.XFS_LOOKUP dir=0x%08X name='%s' -> same index=%u\n",
                dir_cookie, name, nf_read_long(out_cookie + 8));
        return TOS_E_OK;
      }

      if (strcmp(name, "..") == 0) {
        const char *root = g_hostfs_mounts[idx].drive ? g_hostfs_mounts[idx].drive->path : NULL;
        char parent[HOSTFS_HOST_PATH_MAX];
        if (!root || hostfs_same_path(base, root) ||
            !hostfs_parent_path(parent, sizeof(parent), base) ||
            hostfs_same_path(parent, root)) {
          hostfs_write_cookie(out_cookie,
                              g_hostfs_mounts[idx].fs_ptr,
                              (uae_u16)nf_read_word(dir_cookie + 4),
                              0,
                              g_hostfs_mounts[idx].root_cookie);
          HOSTFS_LOG("[NF] HOSTFS.XFS_LOOKUP name='..' path='%s' -> root index=%u\n",
                  base, g_hostfs_mounts[idx].root_cookie);
          return TOS_E_OK;
        }

        hostfs_node_t *parent_node = hostfs_alloc_path_node(idx,
                                                           (uae_u16)nf_read_word(dir_cookie + 4),
                                                           parent);
        if (!parent_node)
          return TOS_ENOSYS;
        hostfs_write_cookie(out_cookie,
                            g_hostfs_mounts[idx].fs_ptr,
                            parent_node->dev,
                            0,
                            parent_node->cookie);
        HOSTFS_LOG("[NF] HOSTFS.XFS_LOOKUP name='..' path='%s' out=0x%08X index=%u -> 0\n",
                parent_node->path, out_cookie, parent_node->cookie);
        return TOS_E_OK;
      }

      if (!hostfs_resolve_child(resolved, sizeof(resolved), base, name) ||
          stat(resolved, &st) != 0) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_LOOKUP base='%s' name='%s' path='%s' errno=%d -> ENOENT\n",
                base, name, resolved, errno);
        return TOS_ENOENT;
      }

      hostfs_node_t *node = hostfs_alloc_node();
      if (!node)
        return TOS_ENOSYS;

      node->dev = (uint16_t)nf_read_word(dir_cookie + 4);
      node->mount_index = idx;
      strncpy(node->path, resolved, sizeof(node->path) - 1);
      node->path[sizeof(node->path) - 1] = '\0';

      hostfs_write_cookie(out_cookie,
                          g_hostfs_mounts[idx].fs_ptr,
                          node->dev,
                          0,
                          node->cookie);
      HOSTFS_LOG("[NF] HOSTFS.XFS_LOOKUP name='%s' path='%s' out=0x%08X index=%u -> 0\n",
              name, node->path, out_cookie, node->cookie);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_GETNAME:
    {
      uaecptr rel_cookie = nf_get_param(params, 0);
      uaecptr dir_cookie = nf_get_param(params, 1);
      uaecptr path_ptr = nf_get_param(params, 2);
      uae_u32 size = nf_get_param(params, 3);
      const char *rel_path = hostfs_path_from_cookie(rel_cookie);
      const char *dir_path = hostfs_path_from_cookie(dir_cookie);
      const char *relative = NULL;
      char atari_path[HOSTFS_HOST_PATH_MAX];
      uae_u32 out = 0;

      if (!rel_path || !dir_path || !path_ptr || size == 0) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_GETNAME rel=0x%08X dir=0x%08X path=0x%08X size=%u -> ENOENT\n",
                rel_cookie, dir_cookie, path_ptr, size);
        return TOS_ENOENT;
      }

      if (!hostfs_path_under(rel_path, dir_path, &relative)) {
        int idx = hostfs_mounted_index_from_cookie(dir_cookie);
        if (idx >= 0 && g_hostfs_mounts[idx].drive)
          hostfs_path_under(g_hostfs_mounts[idx].drive->path, dir_path, &relative);
      }
      if (!relative)
        relative = dir_path;

      if (relative[0] == '\0')
        atari_path[out++] = '\\';
      else {
        if (relative[0] != '/')
          atari_path[out++] = '\\';
        for (const char *p = relative; *p && out + 1 < sizeof(atari_path); p++)
          atari_path[out++] = (*p == '/') ? '\\' : *p;
      }
      atari_path[out] = '\0';

      if (strlen(atari_path) + 1 > size) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_GETNAME rel='%s' dir='%s' -> ERANGE '%s'\n",
                rel_path, dir_path, atari_path);
        return TOS_ERANGE;
      }

      nf_write_string(path_ptr, size, atari_path);
      HOSTFS_LOG("[NF] HOSTFS.XFS_GETNAME rel='%s' dir='%s' -> '%s'\n",
              rel_path, dir_path, atari_path);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_GETXATTR:
    {
      uaecptr cookie = nf_get_param(params, 0);
      uaecptr xattr = nf_get_param(params, 1);
      int idx = hostfs_mounted_index_from_cookie(cookie);
      const char *path = hostfs_path_from_cookie(cookie);
      struct stat st;

      if (idx < 0 || !xattr || !path) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_GETXATTR cookie=0x%08X xattr=0x%08X -> ENOENT\n",
                cookie, xattr);
        return TOS_ENOENT;
      }

      if (stat(path, &st) != 0) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_GETXATTR path='%s' -> ENOENT\n",
                path);
        return TOS_ENOENT;
      }

      hostfs_write_xattr(xattr, &st);
      HOSTFS_LOG("[NF] HOSTFS.XFS_GETXATTR path='%s' xattr=0x%08X mode=0%o size=%llu -> 0\n",
              path, xattr,
              (unsigned)hostfs_mode_to_mint(st.st_mode),
              (unsigned long long)st.st_size);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_GETDEV:
    {
      uaecptr cookie = nf_get_param(params, 0);
      uaecptr devspecial = nf_get_param(params, 1);
      int idx = hostfs_mounted_index_from_cookie(cookie);

      if (idx < 0 || !g_hostfs_mounts[idx].fs_devdrv_ptr) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_GETDEV cookie=0x%08X devspecial=0x%08X -> EDRIVE\n",
                cookie, devspecial);
        return TOS_EDRIVE;
      }

      if (devspecial)
        nf_write_long(devspecial, 0);
      HOSTFS_LOG("[NF] HOSTFS.XFS_GETDEV cookie=0x%08X devspecial=0x%08X -> 0x%08X\n",
              cookie, devspecial, g_hostfs_mounts[idx].fs_devdrv_ptr);
      return g_hostfs_mounts[idx].fs_devdrv_ptr;
    }

    case HOSTFS_XFS_OPENDIR:
    {
      uaecptr dirh = nf_get_param(params, 0);
      uae_u16 flags = (uae_u16)nf_get_param(params, 1);
      int idx = hostfs_mounted_index_from_cookie(dirh);
      const char *path = hostfs_path_from_cookie(dirh);
      struct stat st;

      if (idx < 0 || !dirh || !path || stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_OPENDIR dir=0x%08X path='%s' -> ENOENT\n",
                dirh, path ? path : "(null)");
        return TOS_ENOENT;
      }

      hostfs_dir_t *host_dir = hostfs_alloc_dir();
      if (!host_dir)
        return TOS_ENHNDL;

      host_dir->dir = opendir(path);
      if (!host_dir->dir) {
        hostfs_close_dir(host_dir);
        HOSTFS_LOG("[NF] HOSTFS.XFS_OPENDIR path='%s' errno=%d -> ENOENT\n",
                path, errno);
        return TOS_ENOENT;
      }

      host_dir->dev = nf_read_word(dirh + 4);
      host_dir->mount_index = idx;
      strncpy(host_dir->path, path, sizeof(host_dir->path) - 1);
      host_dir->path[sizeof(host_dir->path) - 1] = '\0';
      hostfs_write_dir_index(dirh, 0);
      hostfs_write_dir_flags(dirh, flags);
      hostfs_write_dir_id(dirh, host_dir->id);

      HOSTFS_LOG("[NF] HOSTFS.XFS_OPENDIR path='%s' dir=0x%08X id=%u flags=0x%04X -> 0\n",
              host_dir->path, dirh, host_dir->id, flags);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_READDIR:
    {
      uaecptr dirh = nf_get_param(params, 0);
      uaecptr name_ptr = nf_get_param(params, 1);
      uae_u32 name_len = nf_get_param(params, 2);
      uaecptr out_cookie = nf_get_param(params, 3);
      hostfs_dir_t *host_dir = dirh ? hostfs_dir_from_id(hostfs_dir_id(dirh)) : NULL;
      struct dirent *entry;
      char child_path[HOSTFS_HOST_PATH_MAX];
      char tos_name[14];
      const char *guest_name = NULL;
      uae_u32 guest_name_offset = 0;

      if (!host_dir || !host_dir->dir || !name_ptr || name_len == 0) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_READDIR dir=0x%08X id=%u -> ENOENT\n",
                dirh, dirh ? hostfs_dir_id(dirh) : 0);
        return TOS_ENOENT;
      }

      do {
        entry = readdir(host_dir->dir);
        if (!entry) {
          HOSTFS_LOG("[NF] HOSTFS.XFS_READDIR dir=0x%08X id=%u -> ENMFIL\n",
                  dirh, host_dir->id);
          return TOS_ENMFIL;
        }
      } while (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0);

      if (snprintf(child_path, sizeof(child_path), "%s/%s",
                   host_dir->path, entry->d_name) >= (int)sizeof(child_path))
        return TOS_ERANGE;

      if (hostfs_dir_flags(dirh) == 0) {
        guest_name = entry->d_name;
        guest_name_offset = 4;
        if (name_len < strlen(guest_name) + 5)
          return TOS_ERANGE;
      } else {
        hostfs_make_tos_name(tos_name, sizeof(tos_name), entry->d_name);
        guest_name = tos_name;
        guest_name_offset = 0;
        if (name_len < strlen(guest_name) + 1)
          return TOS_ERANGE;
      }

      hostfs_node_t *node = hostfs_alloc_node();
      if (!node)
        return TOS_ENOSYS;

      node->dev = host_dir->dev;
      node->mount_index = host_dir->mount_index;
      strncpy(node->path, child_path, sizeof(node->path) - 1);
      node->path[sizeof(node->path) - 1] = '\0';

      if (out_cookie) {
        hostfs_write_cookie(out_cookie,
                            g_hostfs_mounts[host_dir->mount_index].fs_ptr,
                            node->dev,
                            0,
                            node->cookie);
      }

      if (hostfs_dir_flags(dirh) == 0) {
        nf_write_long(name_ptr, (uae_u32)entry->d_ino);
      }
      nf_write_string(name_ptr + guest_name_offset, name_len - guest_name_offset, guest_name);

      hostfs_write_dir_index(dirh, (uae_u16)(hostfs_dir_index(dirh) + 1));
      HOSTFS_LOG("[NF] HOSTFS.XFS_READDIR path='%s' name='%s' dir=0x%08X index=%u cookie=%u -> 0\n",
              host_dir->path, entry->d_name, dirh, hostfs_dir_index(dirh), node->cookie);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_REWINDDIR:
    {
      uaecptr dirh = nf_get_param(params, 0);
      hostfs_dir_t *host_dir = dirh ? hostfs_dir_from_id(hostfs_dir_id(dirh)) : NULL;
      if (!host_dir || !host_dir->dir)
        return TOS_ENOENT;
      rewinddir(host_dir->dir);
      hostfs_write_dir_index(dirh, 0);
      HOSTFS_LOG("[NF] HOSTFS.XFS_REWINDDIR dir=0x%08X id=%u -> 0\n",
              dirh, host_dir->id);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_CLOSEDIR:
    {
      uaecptr dirh = nf_get_param(params, 0);
      hostfs_dir_t *host_dir = dirh ? hostfs_dir_from_id(hostfs_dir_id(dirh)) : NULL;
      uae_u32 id = dirh ? hostfs_dir_id(dirh) : 0;
      if (!host_dir)
        return TOS_ENOENT;
      hostfs_close_dir(host_dir);
      if (dirh)
        hostfs_write_dir_id(dirh, 0);
      HOSTFS_LOG("[NF] HOSTFS.XFS_CLOSEDIR dir=0x%08X id=%u -> 0\n",
              dirh, id);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_PATHCONF:
    {
      uaecptr cookie = nf_get_param(params, 0);
      int which = (int)(int32_t)nf_get_param(params, 1);
      const char *path = hostfs_path_from_cookie(cookie);
      long value = -1;

      if (!path) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_PATHCONF cookie=0x%08X which=%d -> ENOENT\n",
                cookie, which);
        return TOS_ENOENT;
      }

      switch (which) {
        case -1: /* DP_INQUIRE */
          value = HOSTFS_PATHCONF_MAX;
          break;
        case 0: /* DP_IOPEN */
          value = 0x7fffffffL;
          break;
        case 1: /* DP_MAXLINKS */
          errno = 0;
          value = pathconf(path, _PC_LINK_MAX);
          if (value < 0 && errno)
            value = 1;
          break;
        case 2: /* DP_PATHMAX */
          value = HOSTFS_HOST_PATH_MAX - 1;
          break;
        case 3: /* DP_NAMEMAX */
          errno = 0;
          value = pathconf(path, _PC_NAME_MAX);
          if (value < 0 && errno)
            value = NAME_MAX;
          break;
        case 4: /* DP_ATOMIC */
#ifdef _PC_PIPE_BUF
          errno = 0;
          value = pathconf(path, _PC_PIPE_BUF);
          if (value < 0 && errno)
            value = 512;
#else
          value = 512;
#endif
          break;
        case 5: /* DP_TRUNC */
          value = 0; /* DP_NOTRUNC */
          break;
        case 6: /* DP_CASE */
          value = 2; /* DP_CASEINSENS: lookup preserves case but accepts case-insensitive names */
          break;
        case 7: /* DP_MODEATTR */
          value = 0x0fffffdfL;
          break;
        case 8: /* DP_XATTRFIELDS */
          value = 0x00000ffbL;
          break;
        case 9: /* DP_VOLNAMEMAX */
          value = 0;
          break;
        default:
          HOSTFS_LOG("[NF] HOSTFS.XFS_PATHCONF path='%s' which=%d -> ENOSYS\n",
                  path, which);
          return TOS_ENOSYS;
      }

      HOSTFS_LOG("[NF] HOSTFS.XFS_PATHCONF path='%s' which=%d -> %ld\n",
              path, which, value);
      return (uae_u32)value;
    }

    case HOSTFS_XFS_DFREE:
    {
      uaecptr cookie = nf_get_param(params, 0);
      uaecptr diskinfo = nf_get_param(params, 1);
      const char *path = hostfs_path_from_cookie(cookie);
      struct statvfs svfs;

      if (!path || !diskinfo || statvfs(path, &svfs) != 0) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_DFREE cookie=0x%08X diskinfo=0x%08X -> ENOENT\n",
                cookie, diskinfo);
        return TOS_ENOENT;
      }

      nf_write_long(diskinfo + 0, svfs.f_bavail > 0xffffffffULL ? 0xffffffffu : (uae_u32)svfs.f_bavail);
      nf_write_long(diskinfo + 4, svfs.f_blocks > 0xffffffffULL ? 0xffffffffu : (uae_u32)svfs.f_blocks);
      nf_write_long(diskinfo + 8, svfs.f_bsize ? (uae_u32)svfs.f_bsize : 512u);
      nf_write_long(diskinfo + 12, 1);
      HOSTFS_LOG("[NF] HOSTFS.XFS_DFREE path='%s' diskinfo=0x%08X -> 0\n",
              path, diskinfo);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_STAT64:
    {
      uaecptr cookie = nf_get_param(params, 0);
      uaecptr statp = nf_get_param(params, 1);
      const char *path = hostfs_path_from_cookie(cookie);
      struct stat st;

      if (!path || !statp || stat(path, &st) != 0) {
        HOSTFS_LOG("[NF] HOSTFS.XFS_STAT64 cookie=0x%08X stat=0x%08X -> ENOENT\n",
                cookie, statp);
        return TOS_ENOENT;
      }

      hostfs_write_stat64(statp, &st);
      HOSTFS_LOG("[NF] HOSTFS.XFS_STAT64 path='%s' stat=0x%08X size=%llu -> 0\n",
              path, statp, (unsigned long long)st.st_size);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_READLABEL:
    {
      uaecptr cookie = nf_get_param(params, 0);
      uaecptr buffer = nf_get_param(params, 1);
      uae_u32 len = nf_get_param(params, 2);
      const char *path = hostfs_path_from_cookie(cookie);
      if (!path || !buffer || len == 0)
        return TOS_ENOENT;
      nf_write_string(buffer, len, hostfs_basename(path));
      HOSTFS_LOG("[NF] HOSTFS.XFS_READLABEL path='%s' -> '%s'\n",
              path, hostfs_basename(path));
      return TOS_E_OK;
    }

    case HOSTFS_XFS_FSCNTL:
    {
      uaecptr dir_cookie = nf_get_param(params, 0);
      uaecptr name_ptr = nf_get_param(params, 1);
      uae_u16 cmd = (uae_u16)nf_get_param(params, 2);
      uaecptr arg = nf_get_param(params, 3);
      int idx = hostfs_mounted_index_from_cookie(dir_cookie);

      (void)name_ptr;
      if (idx < 0)
        return TOS_EDRIVE;
      if (cmd == MINT_MX_KER_XFSNAME && arg) {
        nf_write_string(arg, 16, "hostfs");
        HOSTFS_LOG("[NF] HOSTFS.XFS_FSCNTL XFSNAME -> hostfs\n");
        return TOS_E_OK;
      }
      HOSTFS_LOG("[NF] HOSTFS.XFS_FSCNTL cmd=0x%04X -> ENOSYS\n", cmd);
      return TOS_ENOSYS;
    }

    case HOSTFS_XFS_CREATE:
    case HOSTFS_XFS_CHATTR:
    case HOSTFS_XFS_CHOWN:
    case HOSTFS_XFS_CHMOD:
    case HOSTFS_XFS_MKDIR:
    case HOSTFS_XFS_RMDIR:
    case HOSTFS_XFS_REMOVE:
    case HOSTFS_XFS_RENAME:
    case HOSTFS_XFS_WRITELABEL:
    case HOSTFS_XFS_SYMLINK:
    case HOSTFS_XFS_HARDLINK:
    case HOSTFS_XFS_MKNOD:
      HOSTFS_LOG("[NF] HOSTFS op %u -> EROFS\n", subid);
      return TOS_EROFS;

    case HOSTFS_XFS_DSKCHNG:
    {
      uae_u32 dev = nf_get_param(params, 0);
      uae_u32 mode = nf_get_param(params, 1);
      HOSTFS_LOG("[NF] HOSTFS.XFS_DSKCHNG dev=%u mode=%u -> 0\n", dev, mode);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_RELEASE:
    {
      uaecptr cookie = nf_get_param(params, 0);
      HOSTFS_LOG("[NF] HOSTFS.XFS_RELEASE cookie=0x%08X index=%u -> 0\n",
              cookie, cookie ? nf_read_long(cookie + 8) : 0);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_DUPCOOKIE:
    {
      uaecptr dst = nf_get_param(params, 0);
      uaecptr src = nf_get_param(params, 1);
      if (!dst || !src)
        return TOS_EDRIVE;
      hostfs_copy_cookie(dst, src);
      HOSTFS_LOG("[NF] HOSTFS.XFS_DUPCOOKIE src=0x%08X dst=0x%08X index=%u -> 0\n",
              src, dst, nf_read_long(dst + 8));
      return TOS_E_OK;
    }

    case HOSTFS_XFS_SYNC:
      HOSTFS_LOG("[NF] HOSTFS.XFS_SYNC -> 0\n");
      return TOS_E_OK;

    case HOSTFS_DEV_OPEN:
    {
      uaecptr filep = nf_get_param(params, 0);
      uae_u16 flags;
      int idx;
      const char *path;
      struct stat st;

      if (!filep)
        return TOS_EIHNDL;
      flags = hostfs_file_flags(filep);
      idx = hostfs_mounted_index_from_cookie(filep + 12);
      path = hostfs_path_from_cookie(filep + 12);

      if (idx < 0 || !path || stat(path, &st) != 0) {
        HOSTFS_LOG("[NF] HOSTFS.DEV_OPEN file=0x%08X path='%s' -> ENOENT\n",
                filep, path ? path : "(null)");
        return TOS_ENOENT;
      }
      if (S_ISDIR(st.st_mode))
        return TOS_EACCDN;
      if (!hostfs_open_flags_are_readonly(flags)) {
        HOSTFS_LOG("[NF] HOSTFS.DEV_OPEN path='%s' flags=0x%04X -> EROFS\n",
                path, flags);
        return TOS_EROFS;
      }

      hostfs_file_t *file = hostfs_alloc_file();
      if (!file)
        return TOS_ENHNDL;

      file->fd = open(path, O_RDONLY);
      if (file->fd < 0) {
        hostfs_close_file(file);
        HOSTFS_LOG("[NF] HOSTFS.DEV_OPEN path='%s' errno=%d -> ENOENT\n",
                path, errno);
        return TOS_ENOENT;
      }

      file->dev = nf_read_word(filep + 16);
      file->mount_index = idx;
      strncpy(file->path, path, sizeof(file->path) - 1);
      file->path[sizeof(file->path) - 1] = '\0';
      hostfs_write_file_id(filep, file->id);
      HOSTFS_LOG("[NF] HOSTFS.DEV_OPEN path='%s' file=0x%08X id=%u flags=0x%04X -> 0\n",
              file->path, filep, file->id, flags);
      return TOS_E_OK;
    }

    case HOSTFS_DEV_READ:
    {
      uaecptr filep = nf_get_param(params, 0);
      uaecptr buffer = nf_get_param(params, 1);
      uae_u32 count = nf_get_param(params, 2);
      hostfs_file_t *file = filep ? hostfs_file_from_id(hostfs_file_id(filep)) : NULL;
      uae_u8 chunk[8192];
      uae_u32 total = 0;

      if (!file || file->fd < 0 || !buffer)
        return TOS_EIHNDL;

      while (total < count) {
        uae_u32 want = count - total;
        if (want > sizeof(chunk))
          want = sizeof(chunk);
        ssize_t got = read(file->fd, chunk, want);
        if (got < 0) {
          HOSTFS_LOG("[NF] HOSTFS.DEV_READ id=%u count=%u -> EIO\n",
                  file->id, count);
          return TOS_EIO;
        }
        if (got == 0)
          break;
        nf_write_buffer(buffer + total, chunk, (uae_u32)got);
        total += (uae_u32)got;
      }

      HOSTFS_LOG("[NF] HOSTFS.DEV_READ path='%s' id=%u count=%u -> %u\n",
              file->path, file->id, count, total);
      return total;
    }

    case HOSTFS_DEV_LSEEK:
    {
      uaecptr filep = nf_get_param(params, 0);
      int32_t offset = (int32_t)nf_get_param(params, 1);
      uae_u32 seekmode = nf_get_param(params, 2);
      hostfs_file_t *file = filep ? hostfs_file_from_id(hostfs_file_id(filep)) : NULL;
      int whence;

      if (!file || file->fd < 0)
        return TOS_EIHNDL;
      if (seekmode == 0)
        whence = SEEK_SET;
      else if (seekmode == 1)
        whence = SEEK_CUR;
      else if (seekmode == 2)
        whence = SEEK_END;
      else
        return TOS_EINVAL;

      off_t result = lseek(file->fd, offset, whence);
      if (result < 0)
        return TOS_EIO;
      HOSTFS_LOG("[NF] HOSTFS.DEV_LSEEK path='%s' id=%u offset=%d mode=%u -> %lld\n",
              file->path, file->id, offset, seekmode, (long long)result);
      return (uae_u32)result;
    }

    case HOSTFS_DEV_IOCTL:
    {
      uaecptr filep = nf_get_param(params, 0);
      uae_u16 mode = (uae_u16)nf_get_param(params, 1);
      uaecptr buffer = nf_get_param(params, 2);
      hostfs_file_t *file = filep ? hostfs_file_from_id(hostfs_file_id(filep)) : NULL;

      if (!file || file->fd < 0)
        return TOS_EIHNDL;

      switch (mode) {
        case MINT_FIONWRITE:
          if (buffer)
            nf_write_long(buffer, 1);
          return TOS_E_OK;
        case MINT_FIONREAD:
        {
          off_t pos = lseek(file->fd, 0, SEEK_CUR);
          off_t end = lseek(file->fd, 0, SEEK_END);
          if (pos >= 0)
            lseek(file->fd, pos, SEEK_SET);
          if (buffer)
            nf_write_long(buffer, (pos >= 0 && end >= pos) ? (uae_u32)(end - pos) : 0);
          return TOS_E_OK;
        }
        case MINT_FIOEXCEPT:
          if (buffer)
            nf_write_long(buffer, 0);
          return TOS_E_OK;
        case MINT_FSTAT64:
        {
          struct stat st;
          if (!buffer || fstat(file->fd, &st) != 0)
            return TOS_ENOENT;
          hostfs_write_stat64(buffer, &st);
          return TOS_E_OK;
        }
      }

      HOSTFS_LOG("[NF] HOSTFS.DEV_IOCTL path='%s' id=%u mode=0x%04X -> ENOSYS\n",
              file->path, file->id, mode);
      return TOS_ENOSYS;
    }

    case HOSTFS_DEV_DATIME:
    {
      uaecptr filep = nf_get_param(params, 0);
      uaecptr datetimep = nf_get_param(params, 1);
      uae_u32 wflag = nf_get_param(params, 2);
      hostfs_file_t *file = filep ? hostfs_file_from_id(hostfs_file_id(filep)) : NULL;
      struct stat st;

      if (!file || file->fd < 0 || !datetimep)
        return TOS_EIHNDL;
      if (wflag)
        return TOS_EROFS;
      if (fstat(file->fd, &st) != 0)
        return TOS_EIO;
      nf_write_long(datetimep,
                    ((uae_u32)hostfs_time_to_dos(st.st_mtime) << 16) |
                    hostfs_date_to_dos(st.st_mtime));
      HOSTFS_LOG("[NF] HOSTFS.DEV_DATIME path='%s' id=%u -> 0\n",
              file->path, file->id);
      return TOS_E_OK;
    }

    case HOSTFS_DEV_CLOSE:
    {
      uaecptr filep = nf_get_param(params, 0);
      hostfs_file_t *file = filep ? hostfs_file_from_id(hostfs_file_id(filep)) : NULL;
      uae_u32 id = filep ? hostfs_file_id(filep) : 0;
      int16_t links = filep ? hostfs_file_links(filep) : 0;

      if (!file)
        return TOS_EIHNDL;
      if (links <= 0) {
        hostfs_close_file(file);
        if (filep)
          hostfs_write_file_id(filep, 0);
      }
      HOSTFS_LOG("[NF] HOSTFS.DEV_CLOSE file=0x%08X id=%u links=%d -> 0\n",
              filep, id, links);
      return TOS_E_OK;
    }

    case HOSTFS_DEV_WRITE:
      HOSTFS_LOG("[NF] HOSTFS.DEV_WRITE -> EROFS\n");
      return TOS_EROFS;

    case HOSTFS_DEV_SELECT:
    case HOSTFS_DEV_UNSELECT:
      return TOS_E_OK;

    case HOSTFS_XFS_UNMOUNT:
    {
      uae_u32 dev = nf_get_param(params, 0);
      int idx = hostfs_mounted_index_from_dev(dev);
      if (idx >= 0) {
        hostfs_close_files_for_mount(idx);
        hostfs_close_dirs_for_mount(idx);
        g_hostfs_mounts[idx].mounted = false;
      }
      HOSTFS_LOG("[NF] HOSTFS.XFS_UNMOUNT dev=%u -> 0\n", dev);
      return TOS_E_OK;
    }
  }

  HOSTFS_LOG("[NF] HOSTFS op %u not implemented yet\n", subid);
  return TOS_ENOSYS;
}

extern "C" void atari_natfeat_raise_network_irq(void)
{
  atari_request_irq_level(nfeth_interrupt_level());
}

static uae_u32 nf_call(uaecptr stack)
{
  uae_u32 id = nf_read_long(stack + 4);
  uae_u32 index = NF_INDEX(id);
  uae_u32 subid = NF_SUBID(id);
  uaecptr params = stack + 8;

  if (index >= NF_FEATURE_COUNT)
    return 0;

  switch (index) {
    case NF_FEATURE_NAME:
      return nf_call_name(params);
    case NF_FEATURE_VERSION:
      return NF_VERSION_VALUE;
    case NF_FEATURE_STDERR:
      return nf_call_stderr(params);
    case NF_FEATURE_ETHERNET:
      return nf_call_ethernet(subid, params);
    case NF_FEATURE_HOSTFS:
      return nf_call_hostfs(subid, params);
    case NF_FEATURE_FVDI:
      return nf_call_fvdi(subid, params);
  }

  return 0;
}

bool atari_natfeat_handle_opcode(uae_u32 opcode, uae_u32 *cycles)
{
  if (opcode != NF_ID_OPCODE && opcode != NF_CALL_OPCODE)
    return false;

  uaecptr stack = m68k_areg(regs, 7);
  uae_u32 result = opcode == NF_ID_OPCODE ? nf_get_id(stack) : nf_call(stack);
  m68k_dreg(regs, 0) = result;

  m68k_incpc_normal(2);
  if (cycles)
    *cycles = (1 * 4 * CYCLE_UNIT / 2) * 4;
  return true;
}
