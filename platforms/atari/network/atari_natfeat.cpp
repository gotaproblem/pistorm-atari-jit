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
/* Vertex / move-index / crossing cap for FILL_POLYGON. Was an inline 4096
 * with malloc'd scratch; now also sizes the static scratch arrays. */
#define FVDI_POLY_MAX 4096
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
  NF_FEATURE_MP3,
  NF_FEATURE_VIDEO,
  NF_FEATURE_COUNT
};

/* MP3PLAY subids (NF_SUBID of the id) */
enum nf_mp3_ops {
  NF_MP3_PLAY = 0,   /* param0 = ptr to GEMDOS path string (e.g. "U:\\dir\\x.mp3") */
  NF_MP3_STOP,       /* stop playback / unload */
  NF_MP3_STATUS,     /* -> 1 if playing/buffered, else 0 */
  NF_MP3_PAUSE,      /* param0: 1 = pause, 0 = resume */
  NF_MP3_SEEK,       /* param0: signed seconds relative to current position */
  NF_MP3_POS,        /* -> current position in seconds (-1 if n/a) */
  NF_MP3_LEN,        /* -> track length in seconds (0 if unknown) */
  NF_MP3_META        /* param0: 0=title 1=artist 2=album; param1: buf; param2: len */
};

/* VIDPLAY subids. 0..7 are deliberately identical to MP3PLAY so a front-end
 * can drive either feature through the same code path. */
enum nf_vid_ops {
  NF_VID_PLAY = 0,   /* param0 = ptr to GEMDOS path string ("S:\\FILM\\X.MP4") */
  NF_VID_STOP,       /* stop playback, hide the overlay, free everything       */
  NF_VID_STATUS,     /* -> 1 while playing/paused, 0 when stopped or finished  */
  NF_VID_PAUSE,      /* param0: 1 = pause, 0 = resume                          */
  NF_VID_SEEK,       /* param0: signed seconds relative to current position    */
  NF_VID_POS,        /* -> current position in seconds (-1 if n/a)             */
  NF_VID_LEN,        /* -> duration in seconds (0 if unknown)                  */
  NF_VID_META,       /* param0: 0=title 1=author 2=codecs; param1: buf; p2: len*/
  NF_VID_RECT,       /* param0..3 = x,y,w,h in display pixels; all 0 = auto    */
  NF_VID_VOLUME,     /* param0: 0..200 percent                                 */
  NF_VID_INFO,       /* param0: 0=w 1=h 2=fps*100 3=audio 4=hwdec 5=volume     */
  NF_VID_CLIP        /* param0..3 = visible part of the rect, display pixels   */
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
  "fVDI",
  "MP3PLAY",
  "VIDPLAY"
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
extern "C" int  dmasnd_mp3_play(const char *host_path);
extern "C" void dmasnd_mp3_stop(void);
extern "C" int  dmasnd_mp3_active(void);
extern "C" void dmasnd_mp3_pause(int on);
extern "C" long dmasnd_mp3_pos_s(void);
extern "C" long dmasnd_mp3_len_s(void);
extern "C" void dmasnd_mp3_seek_rel(long delta_s);
extern "C" const char *dmasnd_mp3_meta(int which);

/* Host video player - platforms/atari/video/vidplay.c */
extern "C" int  vidplay_play(const char *host_path);
extern "C" void vidplay_stop(void);
extern "C" int  vidplay_active(void);
extern "C" void vidplay_pause(int on);
extern "C" long vidplay_pos_s(void);
extern "C" long vidplay_len_s(void);
extern "C" void vidplay_seek_rel(long delta_s);
extern "C" const char *vidplay_meta(int which);
extern "C" long vidplay_info(int what);
extern "C" void vidplay_set_rect(int x, int y, int w, int h);
extern "C" void vidplay_set_volume(int percent);
extern "C" void vidplay_set_clip(int x, int y, int w, int h);
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

/* Return the existing node for (mount_index, dev, path) if we already have one,
 * otherwise allocate a fresh one. Deduping here is what keeps the node table
 * bounded: the MiNT kernel re-looks-up the same paths constantly (see the
 * HOSTFS_DEBUG trace - 'ico24' and each icon get looked up over and over), and
 * without dedup every lookup burned a new slot until the 1024-entry table
 * filled and returned EINVFN (TOS ERROR #32). We deliberately never free nodes
 * (a live fcookie the kernel still holds could otherwise be reused underneath
 * it - that corruption is what broke exec and showed ---w- permissions); dedup
 * bounds the table by the number of DISTINCT paths touched instead of by the
 * number of operations, which is the Hatari-style "bounded, recycled state"
 * principle adapted to the XFS cookie model. */
/* Path-dedup keeps the node table bounded by distinct paths instead of by op
 * count (see hostfs_alloc_path_node). Default ON - confirmed stable on hardware
 * with programs running off the share. PISTORM_HOSTFS_DEDUP=0 restores the old
 * fresh-node-per-lookup behaviour as an escape hatch. */
static bool hostfs_dedup_enabled(void)
{
  static int cached = -1;
  if (cached < 0) {
    const char *e = getenv("PISTORM_HOSTFS_DEDUP");
    cached = (e && atoi(e) == 0) ? 0 : 1;
  }
  return cached != 0;
}

static hostfs_node_t *hostfs_find_path_node(int mount_index, uae_u16 dev, const char *path)
{
  if (!hostfs_dedup_enabled())
    return NULL;
  for (unsigned i = 0; i < HOSTFS_MAX_NODES; i++) {
    hostfs_node_t *n = &g_hostfs_nodes[i];
    if (n->used && n->mount_index == mount_index && n->dev == dev &&
        strcmp(n->path, path) == 0)
      return n;
  }
  return NULL;
}

static hostfs_node_t *hostfs_alloc_path_node(int mount_index, uae_u16 dev, const char *path)
{
  hostfs_node_t *node = hostfs_find_path_node(mount_index, dev, path);
  if (node)
    return node;
  node = hostfs_alloc_node();
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

/* --- MFDB header cache ---------------------------------------------------
 * Every leaf accessor below used to re-read the guest MFDB header: bpp is a
 * word, pixel_bytes calls bpp again, addressable_direct is a long + three
 * words + two bpp reads (~7 guest reads), supported_direct ~9, pixel_addr
 * ~13 - so resolving a single pixel address cost over twenty guest memory
 * accesses, each one a bank-indexed indirect call. In the row fast paths
 * that is ~26 header reads per row, and the two-pass ROP blitter resolves
 * every row twice, so a 1080-row blit re-read the same six fields ~100k
 * times for values that cannot change while it runs.
 *
 * A NatFeat call is a single guest instruction executed on the CPU thread,
 * so nothing can mutate an MFDB header underneath us for its duration. The
 * cache is therefore keyed only on the MFDB address and reset at the top of
 * every fVDI call. Three distinct MFDBs per call (source, destination, the
 * workstation's screen MFDB) is the practical maximum; a fourth falls back
 * to an uncached parse rather than evicting.
 *
 * Values are byte-identical to what the old accessors computed. */
typedef struct fvdi_mfdb_info {
  uaecptr mfdb;
  uae_u32 base;
  uint32_t width;
  uint32_t height;
  uint32_t wdwidth;
  uint32_t standard;
  uint32_t bpp;
  uint32_t pixel_bytes;
  uint32_t row_bytes;
  bool screen;
  bool addressable;
} fvdi_mfdb_info_t;

#define FVDI_MFDB_CACHE_SLOTS 4
static fvdi_mfdb_info_t g_fvdi_mfdb_cache[FVDI_MFDB_CACHE_SLOTS];
static unsigned g_fvdi_mfdb_cache_used;

static void fvdi_mfdb_cache_reset(void)
{
  g_fvdi_mfdb_cache_used = 0;
}

static void fvdi_mfdb_parse(uaecptr mfdb, fvdi_mfdb_info_t *out)
{
  out->mfdb = mfdb;
  out->base = nf_read_long(mfdb + 0);
  out->width = nf_read_word(mfdb + 4);
  out->height = nf_read_word(mfdb + 6);
  out->wdwidth = nf_read_word(mfdb + 8);
  out->standard = nf_read_word(mfdb + 10);

  uint32_t bpp = nf_read_word(mfdb + 12);
  if (bpp == 15)
    bpp = 16;
  if (!bpp)
    bpp = pistorm_fvdi_bpp();
  out->bpp = bpp;

  out->pixel_bytes = (bpp == 16) ? 2u : (bpp == 24) ? 3u : (bpp == 32) ? 4u : 0u;
  out->row_bytes = out->wdwidth * 2u * bpp;
  out->screen = (out->base == 0 || out->base == pistorm_fvdi_fb_base());

  out->addressable =
      out->screen ||
      (out->base && out->width && out->height && out->wdwidth &&
       out->pixel_bytes && (bpp == 16 || bpp == 24 || bpp == 32) &&
       out->row_bytes >= out->width * out->pixel_bytes);
}

/* NULL for the screen (mfdb == 0); never NULL otherwise. */
static const fvdi_mfdb_info_t *fvdi_mfdb_info(uaecptr mfdb)
{
  static fvdi_mfdb_info_t scratch;

  if (!mfdb)
    return NULL;

  for (unsigned i = 0; i < g_fvdi_mfdb_cache_used; i++) {
    if (g_fvdi_mfdb_cache[i].mfdb == mfdb)
      return &g_fvdi_mfdb_cache[i];
  }

  if (g_fvdi_mfdb_cache_used < FVDI_MFDB_CACHE_SLOTS) {
    fvdi_mfdb_info_t *slot = &g_fvdi_mfdb_cache[g_fvdi_mfdb_cache_used++];
    fvdi_mfdb_parse(mfdb, slot);
    return slot;
  }

  fvdi_mfdb_parse(mfdb, &scratch);
  return &scratch;
}

static bool fvdi_mfdb_is_screen(uaecptr mfdb)
{
  const fvdi_mfdb_info_t *info = fvdi_mfdb_info(mfdb);
  return !info || info->screen;
}

/* --- Destination redirection for fill / line / polygon -------------------
 * fVDI passes the destination for these operations inside the workstation
 * (vwk->real_address->screen.mfdb, offsets from ARAnyM src/natfeat/nfvdi.h)
 * rather than as an argument. The renderers below were written against the
 * visible framebuffer only, so drawing aimed at an off-screen bitmap
 * (v_opnbm - e.g. the GEM port of Elite renders its 3D view that way) was
 * painted onto the screen at 0,0, detached from the window, and the window
 * itself was blitted from a buffer nothing had drawn into.
 *
 * g_fvdi_dest_mfdb is 0 for the screen, in which case every path below is
 * bit-identical to before. When set, the leaf pixel accessors redirect into
 * the MFDB and dirty-rect marking is suppressed (an off-screen bitmap has
 * nothing to present). */
#define FVDI_VWK_REAL_ADDRESS 0
#define FVDI_WK_SCREEN_MFDB   24

static uaecptr g_fvdi_dest_mfdb = 0;
static int32_t g_fvdi_dest_w = 0;
static int32_t g_fvdi_dest_h = 0;

static int32_t fvdi_dest_width(void)
{
  return g_fvdi_dest_mfdb ? g_fvdi_dest_w : (int32_t)pistorm_fvdi_width();
}

static int32_t fvdi_dest_height(void)
{
  return g_fvdi_dest_mfdb ? g_fvdi_dest_h : (int32_t)pistorm_fvdi_height();
}


/* (fvdi_mfdb_bpp() / fvdi_mfdb_addressable_direct() are gone: every caller
 * now reads ->bpp / ->addressable straight off the cached header.) */

static uint32_t fvdi_mfdb_pixel_bytes(uaecptr mfdb)
{
  const fvdi_mfdb_info_t *info = fvdi_mfdb_info(mfdb);
  if (info)
    return info->pixel_bytes;
  uint32_t bpp = pistorm_fvdi_bpp();
  return (bpp == 16) ? 2u : (bpp == 24) ? 3u : (bpp == 32) ? 4u : 0u;
}

static bool fvdi_mfdb_supported_direct(uaecptr mfdb)
{
  const fvdi_mfdb_info_t *info = fvdi_mfdb_info(mfdb);
  if (!info)
    return true;
  if (!info->addressable)
    return false;
  if (info->screen)
    return true;
  return info->standard == 0;
}

/* Row base address for a directly addressable MFDB, or 0. Callers that walk
 * a row want this once instead of per pixel. */
static uaecptr fvdi_mfdb_row_addr(const fvdi_mfdb_info_t *info, int32_t y)
{
  if (!info || y < 0 || !info->addressable ||
      !info->base || !info->width || !info->height || !info->wdwidth ||
      !info->pixel_bytes || (uint32_t)y >= info->height)
    return 0;
  return info->base + (uaecptr)y * info->row_bytes;
}

static uaecptr fvdi_mfdb_pixel_addr(uaecptr mfdb, int32_t x, int32_t y)
{
  const fvdi_mfdb_info_t *info = fvdi_mfdb_info(mfdb);
  if (!info || x < 0 || (uint32_t)x >= info->width)
    return 0;

  uaecptr row = fvdi_mfdb_row_addr(info, y);
  if (!row)
    return 0;

  return row + (uaecptr)x * info->pixel_bytes;
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

/* Host pointer to a horizontal run of the *current fill/line/polygon
 * destination* - the framebuffer normally, the redirected off-screen MFDB
 * when g_fvdi_dest_mfdb is armed. NULL when the run is not directly
 * addressable (ST-RAM MFDB, 24bpp, out of bounds), in which case callers
 * must use the per-pixel path.
 *
 * This exists because the fill fast paths used fvdi_screen_span_ptr(), which
 * has no g_fvdi_dest_mfdb check. With the redirection armed (v_opnbm - the
 * GEM Elite case the redirection was added for) a solid fill wrote row 0
 * into the bitmap via fvdi_fill_solid_span() and then memcpy'd *screen*
 * content over rows 1..h-1 of the visible framebuffer; the patterned row
 * path painted the whole rect onto the screen instead of the bitmap. Dirty
 * marking is suppressed while redirected, so the damage was not even
 * presented until something else touched those rows. */
static uint8_t *fvdi_dest_span_ptr(int32_t x, int32_t y, int32_t w,
                                   uint32_t *bytes_out)
{
  if (w <= 0)
    return NULL;

  if (!g_fvdi_dest_mfdb) {
    uint32_t bytes = fvdi_bytes_per_pixel();
    uint8_t *p = fvdi_screen_span_ptr(x, y, w);
    if (!p || (bytes != 2 && bytes != 4))
      return NULL;
    if (bytes_out)
      *bytes_out = bytes;
    return p;
  }

  const uint32_t mb = fvdi_mfdb_pixel_bytes(g_fvdi_dest_mfdb);
  if ((mb != 2 && mb != 4) || !fvdi_mfdb_supported_direct(g_fvdi_dest_mfdb))
    return NULL;

  const uaecptr a0 = fvdi_mfdb_pixel_addr(g_fvdi_dest_mfdb, x, y);
  const uaecptr a1 = fvdi_mfdb_pixel_addr(g_fvdi_dest_mfdb, x + w - 1, y);
  uae_u8 *rp;
  if (!a0 || !a1 || a1 != a0 + (uaecptr)(w - 1) * mb || a0 < NF_ST_RAM_SIZE ||
      !nf_host_ram_ptr(a0, (uint32_t)((size_t)w * mb), &rp))
    return NULL;

  if (bytes_out)
    *bytes_out = mb;
  return rp;
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
  if (g_fvdi_dest_mfdb)
    return x >= 0 && y >= 0 && x < g_fvdi_dest_w && y < g_fvdi_dest_h;
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
  if (g_fvdi_dest_mfdb) {
    fvdi_mfdb_put_pixel(g_fvdi_dest_mfdb, x, y, colour);
    return;
  }

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
  if (g_fvdi_dest_mfdb)
    return;
  uint32_t bytes = fvdi_bytes_per_pixel();
  if (bytes == 0 || w <= 0 || x < 0 || y < 0)
    return;
  pistorm_fvdi_note_host_write(((uint32_t)y * pistorm_fvdi_width() + (uint32_t)x) * bytes,
                               (uint32_t)w * bytes);
}

/* Guest pixels are stored big-endian in host memory, so a whole pixel can be
 * written with one store of the byte-swapped value. Alignment is not
 * guaranteed (an MFDB base only has to be word aligned, and 24bpp rows make
 * odd strides possible), hence the aligned(1) typedefs - AArch64 handles
 * unaligned stores to normal memory natively and the compiler still
 * auto-vectorises these loops. */
typedef uint16_t fvdi_u16_una __attribute__((aligned(1), may_alias));
typedef uint32_t fvdi_u32_una __attribute__((aligned(1), may_alias));

static inline void fvdi_store_px(uint8_t *p, uint32_t bytes, uint32_t colour)
{
  if (bytes == 2)
    *(fvdi_u16_una *)p = (uint16_t)__builtin_bswap16((uint16_t)colour);
  else
    *(fvdi_u32_una *)p = __builtin_bswap32(colour);
}

static inline uint32_t fvdi_load_px(const uint8_t *p, uint32_t bytes)
{
  if (bytes == 2)
    return __builtin_bswap16(*(const fvdi_u16_una *)p);
  return __builtin_bswap32(*(const fvdi_u32_una *)p);
}

/* Fill a run of one row with a constant colour, through a host pointer when
 * the run is directly addressable. The stores are whole-pixel and uniform,
 * so -O3 turns both loops into NEON stores; the old byte-at-a-time version
 * defeated vectorisation entirely. */
static void fvdi_fill_solid_span_ptr(uint8_t *p, uint32_t bytes, int32_t w,
                                     uint32_t colour)
{
  if (bytes == 2) {
    fvdi_u16_una *q = (fvdi_u16_una *)p;
    const uint16_t v = (uint16_t)__builtin_bswap16((uint16_t)colour);
    for (int32_t i = 0; i < w; i++)
      q[i] = v;
  } else {
    fvdi_u32_una *q = (fvdi_u32_una *)p;
    const uint32_t v = __builtin_bswap32(colour);
    for (int32_t i = 0; i < w; i++)
      q[i] = v;
  }
}

static void fvdi_fill_solid_span(int32_t x, int32_t y, int32_t w, uint32_t colour)
{
  if (w <= 0)
    return;

  uint32_t bytes = 0;
  uint8_t *p = fvdi_dest_span_ptr(x, y, w, &bytes);
  if (p) {
    fvdi_fill_solid_span_ptr(p, bytes, w, colour);
    fvdi_note_screen_span(x, y, w);
    return;
  }

  if (g_fvdi_dest_mfdb) {
    for (int32_t i = 0; i < w; i++)
      fvdi_mfdb_put_pixel(g_fvdi_dest_mfdb, x + i, y, colour);
    return;
  }

  /* Screen, but not a directly addressable run (24bpp / clipped away). */
  for (int32_t i = 0; i < w; i++)
    fvdi_put_raw_pixel_marked(x + i, y, colour, false);
  fvdi_note_screen_span(x, y, w);
}

static uint32_t fvdi_get_raw_pixel(int32_t x, int32_t y)
{
  if (g_fvdi_dest_mfdb)
    return fvdi_mfdb_get_pixel(g_fvdi_dest_mfdb, x, y);

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
  if (x + w > fvdi_dest_width())
    w = fvdi_dest_width() - x;
  if (y + h > fvdi_dest_height())
    h = fvdi_dest_height() - y;
  if (w <= 0 || h <= 0)
    return 1;

  if (!pattern && (mode == 1 || mode == 2)) {
    /* Solid fill via row replication: build the first row once, memcpy the
     * rest (a full-screen 1080p32 fill through the span loop was 7-11ms).
     * fvdi_dest_span_ptr() rather than fvdi_screen_span_ptr() so an armed
     * off-screen redirection stays inside the bitmap - see the comment on
     * fvdi_dest_span_ptr(). */
    uint32_t fbytes = 0;
    fvdi_fill_solid_span(x, y, w, fg);
    const uint8_t *first = fvdi_dest_span_ptr(x, y, w, &fbytes);
    if (first && fbytes) {
      const size_t frow = (size_t)w * fbytes;
      for (int32_t yy = 1; yy < h; yy++) {
        uint8_t *dst = fvdi_dest_span_ptr(x, y + yy, w, NULL);
        if (dst) {
          memcpy(dst, first, frow);
          fvdi_note_screen_span(x, y + yy, w);
        } else {
          fvdi_fill_solid_span(x, y + yy, w, fg);
        }
      }
    } else {
      for (int32_t yy = 1; yy < h; yy++)
        fvdi_fill_solid_span(x, y + yy, w, fg);
    }
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
    /* Row fast path (latency work, phase 2): patterned fills used to run
     * per-pixel through fvdi_get/put_raw_pixel, each call re-deriving
     * fb_ptr/width/bounds -> ~80ns/px, so a ~300x300 dithered fill was a
     * 4-14ms uninterruptible natfeat op (the Boing saturation signature).
     * Same logic on a hoisted row pointer is ~20x faster; per-pixel loop
     * kept below as the fallback. */
    uint32_t rbytes = 0;
    uint8_t *rp = fvdi_dest_span_ptr(x, y + yy, w, &rbytes);
    if (rp) {
      if (mode == 1) {
        /* MD_REPLACE: the destination is never read, and fvdi_apply_logic()
         * indexes the pattern with (x & 0x0f) - so the row is exactly a
         * 16-pixel tile repeated. Build one tile, then replicate it with
         * memcpy (doubling, so the copies grow geometrically). The old loop
         * recomputed the pattern bit and issued a separate store for every
         * pixel of a 1920-pixel row. */
        const int32_t tile = w < 16 ? w : 16;
        for (int32_t xx = 0; xx < tile; xx++) {
          const bool bit = (pat & (1u << ((x + xx) & 0x0f))) != 0;
          fvdi_store_px(rp + (size_t)xx * rbytes, rbytes, bit ? fg : bg);
        }
        int32_t done = tile;
        while (done < w) {
          int32_t n = done;
          if (n > w - done)
            n = w - done;
          memcpy(rp + (size_t)done * rbytes, rp, (size_t)n * rbytes);
          done += n;
        }
      } else {
        for (int32_t xx = 0; xx < w; xx++) {
          uint8_t *pp = rp + (size_t)xx * rbytes;
          uint32_t dst = fvdi_load_px(pp, rbytes);
          fvdi_store_px(pp, rbytes,
                        fvdi_apply_logic(dst, fg, bg, pat, x + xx, mode));
        }
      }
    } else {
      for (int32_t xx = 0; xx < w; xx++) {
        int32_t px = x + xx;
        int32_t py = y + yy;
        uint32_t dst = fvdi_get_raw_pixel(px, py);
        fvdi_put_raw_pixel_marked(px, py,
                                  fvdi_apply_logic(dst, fg, bg, pat, px, mode),
                                  false);
      }
    }
    fvdi_note_screen_span(x, y + yy, w);
  }

  return 1;
}

uint64_t get_time_us(void);
static void fvdi_dump_mfdb_miss(const char *tag, unsigned op,
                                uaecptr src_mfdb, uaecptr dst_mfdb,
                                int32_t w, int32_t h);

/* --- 8-bit chunky (antialiased) expand -----------------------------------
 * fVDI's FreeType module renders every antialiased glyph into an MFDB with
 * standard = 0x0100 (chunky) and bitplanes = 8, where each source byte is a
 * coverage/alpha value, and pushes it through vrt_cpyfm -> EXPAND_AREA
 * (modules/ft2/ft2.c: "This MFDB is only supported by the aranym driver").
 *
 * This backend used to reject that shape by returning 1 - i.e. claiming the
 * operation was done - so with `antialias` enabled in fvdi.sys every
 * antialiased glyph was silently dropped and the text simply did not appear.
 * Returning 0 instead would be no better: fVDI's fallback is _default_expand,
 * a *mono* expander, which would read the coverage bytes as bitmap bits and
 * draw noise.
 *
 * Semantics follow ARAnyM's SoftVdiDriver::expandArea, which builds an
 * alpha surface and lets SDL blend it (out = src*a + dst*(255-a)):
 *
 *   MD_REPLACE  blend(bg,  fg,     a)      - destination not read
 *   MD_TRANS    blend(dst, fg,     a)
 *   MD_XOR      blend(dst, ~dst,   a)
 *   MD_ERASE    blend(dst, bg,     255-a)
 *
 * ARAnyM restricts this to 32bpp screen surfaces; 16bpp is handled here too,
 * blended in the native RGB565 field widths. */
static inline uint32_t fvdi_blend_ch(uint32_t d, uint32_t s, uint32_t a)
{
  const uint32_t t = s * a + d * (255u - a) + 128u;
  return (t + (t >> 8)) >> 8;   /* rounded /255 */
}

static inline uint32_t fvdi_blend_px(uint32_t dst, uint32_t src, uint32_t a,
                                     uint32_t bytes)
{
  if (a == 0)
    return dst;
  if (a >= 255)
    return src;

  if (bytes == 2) {
    return (fvdi_blend_ch((dst >> 11) & 0x1fu, (src >> 11) & 0x1fu, a) << 11) |
           (fvdi_blend_ch((dst >> 5) & 0x3fu, (src >> 5) & 0x3fu, a) << 5) |
            fvdi_blend_ch(dst & 0x1fu, src & 0x1fu, a);
  }
  return (fvdi_blend_ch((dst >> 16) & 0xffu, (src >> 16) & 0xffu, a) << 16) |
         (fvdi_blend_ch((dst >> 8) & 0xffu, (src >> 8) & 0xffu, a) << 8) |
          fvdi_blend_ch(dst & 0xffu, src & 0xffu, a);
}

/* Host pointer to one destination row of an expand, or NULL for the
 * per-pixel fallback. Screen rows come straight out of the shadow
 * framebuffer; off-screen MFDBs are host-direct in TT-RAM, while ST-RAM
 * destinations decline because writes there have to go through DMA. */
static uint8_t *fvdi_expand_dst_row(uaecptr dst_mfdb, bool dst_screen,
                                    int32_t dst_x, int32_t row_y, int32_t w,
                                    uint32_t *bytes_io)
{
  if (dst_screen)
    return fvdi_screen_span_ptr(dst_x, row_y, w);

  const uint32_t mb = fvdi_mfdb_pixel_bytes(dst_mfdb);
  if ((mb != 2 && mb != 4) || !fvdi_mfdb_supported_direct(dst_mfdb))
    return NULL;

  const uaecptr da = fvdi_mfdb_pixel_addr(dst_mfdb, dst_x, row_y);
  const uaecptr de = fvdi_mfdb_pixel_addr(dst_mfdb, dst_x + w - 1, row_y);
  uae_u8 *p;
  if (!da || !de || de != da + (uaecptr)(w - 1) * mb || da < NF_ST_RAM_SIZE ||
      !nf_host_ram_ptr(da, (uint32_t)((size_t)w * mb), &p))
    return NULL;

  *bytes_io = mb;
  return p;
}

static uae_u32 fvdi_expand_chunky_rows(uaecptr dst_mfdb, bool dst_screen,
                                       uint32_t dst_bytes, uaecptr data,
                                       uint32_t pitch, int32_t src_x,
                                       int32_t dst_x, int32_t dst_y,
                                       int32_t w, int32_t h, unsigned mode,
                                       uint32_t fg, uint32_t bg)
{
  for (int32_t yy = 0; yy < h; yy++) {
    uint32_t rb = dst_bytes;
    uint8_t *rp = fvdi_expand_dst_row(dst_mfdb, dst_screen, dst_x, dst_y + yy,
                                      w, &rb);
    const uaecptr srow = data + (uaecptr)yy * pitch + (uaecptr)src_x;

    /* Coverage bytes come from the FreeType glyph buffer in guest RAM; take
     * a host pointer for the row when we can, so the inner loop is not one
     * bank-dispatched guest read per pixel. Reads from ST-RAM through the
     * mirror are fine - only writes have to go via DMA. */
    const uae_u8 *sp = NULL;
    uae_u8 *sq;
    if (nf_host_ram_ptr(srow, (uint32_t)w, &sq))
      sp = sq;

    if (rp && (rb == 2 || rb == 4)) {
      for (int32_t xx = 0; xx < w; xx++) {
        const uint32_t a = sp ? sp[xx] : nf_read_byte(srow + (uaecptr)xx);
        uint8_t *pp = rp + (size_t)xx * rb;
        uint32_t out;

        switch (mode) {
          case 1: /* MD_REPLACE */
            out = fvdi_blend_px(bg, fg, a, rb);
            break;
          case 3: /* MD_XOR */
            if (!a)
              continue;
            {
              const uint32_t d = fvdi_load_px(pp, rb);
              const uint32_t inv = (rb == 2) ? (~d & 0xffffu) : (~d & 0xffffffu);
              out = fvdi_blend_px(d, inv, a, rb);
            }
            break;
          case 4: /* MD_ERASE */
            if (a >= 255)
              continue;
            out = fvdi_blend_px(fvdi_load_px(pp, rb), bg, 255u - a, rb);
            break;
          case 2: /* MD_TRANS */
          default:
            if (!a)
              continue;
            out = (a >= 255) ? fg : fvdi_blend_px(fvdi_load_px(pp, rb), fg, a, rb);
            break;
        }
        fvdi_store_px(pp, rb, out);
      }
    } else {
      for (int32_t xx = 0; xx < w; xx++) {
        const uint32_t a = sp ? sp[xx] : nf_read_byte(srow + (uaecptr)xx);
        const int32_t px = dst_x + xx;
        const int32_t py = dst_y + yy;
        uint32_t out;

        switch (mode) {
          case 1:
            out = fvdi_blend_px(bg, fg, a, dst_bytes);
            break;
          case 3:
            if (!a)
              continue;
            {
              const uint32_t d = fvdi_target_get_pixel(dst_mfdb, px, py);
              const uint32_t inv =
                  (dst_bytes == 2) ? (~d & 0xffffu) : (~d & 0xffffffu);
              out = fvdi_blend_px(d, inv, a, dst_bytes);
            }
            break;
          case 4:
            if (a >= 255)
              continue;
            out = fvdi_blend_px(fvdi_target_get_pixel(dst_mfdb, px, py), bg,
                                255u - a, dst_bytes);
            break;
          case 2:
          default:
            if (!a)
              continue;
            out = (a >= 255)
                      ? fg
                      : fvdi_blend_px(fvdi_target_get_pixel(dst_mfdb, px, py),
                                      fg, a, dst_bytes);
            break;
        }
        fvdi_target_put_pixel(dst_mfdb, px, py, out, false);
      }
    }

    if (dst_screen)
      fvdi_note_screen_span(dst_x, dst_y + yy, w);
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
  uint32_t pitch = (uint32_t)nf_read_word(src + 8) * 2u;
  uint32_t src_planes = (uint32_t)nf_read_word(src + 12);
  uint32_t src_standard = (uint32_t)nf_read_word(src + 10);
  if (!data_base || pitch == 0)
    return 1;

  /* MFDB_STAND bit 0x1000 = "does not wrap on words" (ARAnyM applies the
   * same halving in expandArea). Without it a source that sets the bit is
   * read a row out of step. */
  if ((src_standard & 0x1000u) != 0)
    pitch >>= 1;
  if (pitch == 0)
    return 1;

  /* 0x100 = chunky. Combined with 8 planes that is fVDI's antialiased-glyph
   * shape and is handled below; anything else chunky, or a plane count that
   * is not mono, we still decline. */
  const bool chunky = (src_standard & 0x100u) != 0;
  if (chunky) {
    if (src_planes != 8)
      return 1;
  } else if (src_planes != 0 && src_planes != 1) {
    return 1;
  }

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

  /* Row fast path (latency work, phase 3): mono expand is the ball/text op
   * (vrt_cpyfm). Per-pixel get/put through the target dispatchers cost
   * 4-14ms per ~256x256 expand -- the Boing saturation residue after the
   * fill fix. Screen-destination rows run on a hoisted pointer instead;
   * per-pixel loop kept as fallback for offscreen destinations. */
  const bool exp_dst_screen = fvdi_mfdb_is_screen(dst_mfdb);
  const uint32_t exp_bytes = fvdi_bytes_per_pixel();

  if (chunky)
    return fvdi_expand_chunky_rows(dst_mfdb, exp_dst_screen, exp_bytes,
                                   data, pitch, src_x, dst_x, dst_y, w, h,
                                   mode, fg, bg);

  /* (diagnostic moved into the per-row fallback below so it only fires
   * when the row fast path genuinely declined) */

  for (int32_t yy = 0; yy < h; yy++) {
    uint16_t word = 0;
    int32_t word_base = -1;
    uint32_t rb = exp_bytes;
    uint8_t *rp = fvdi_expand_dst_row(dst_mfdb, exp_dst_screen, dst_x,
                                      dst_y + yy, w, &rb);
    if (rp && (rb == 2 || rb == 4)) {
      /* Walk the row a source word at a time instead of a pixel at a time.
       * Two things fall out of that:
       *
       *  - MD_REPLACE never reads the destination, so the read-modify-write
       *    collapses to a pure store. The old loop loaded the destination
       *    pixel even in mode 1, where the value is discarded.
       *  - A uniform source word (0xffff / 0x0000) makes the whole 16-pixel
       *    run one action: a solid span, or nothing at all. Glyph and icon
       *    bitmaps - the traffic this op actually carries, since fVDI turns
       *    every string into vrt_cpyfm - are mostly uniform words, and in
       *    MD_TRANS an all-zero word means the run can be skipped outright.
       */
      const uaecptr srow = data + (uaecptr)yy * pitch;
      int32_t xx = 0;
      while (xx < w) {
        const int32_t sx = src_x + xx;
        const int32_t bit = sx & 0x0f;
        if ((sx & ~15) != word_base) {
          word_base = sx & ~15;
          word = nf_read_word(srow + (uaecptr)((sx >> 3) & ~1));
        }

        int32_t run = 16 - bit;
        if (run > w - xx)
          run = w - xx;
        uint8_t *rowp = rp + (size_t)xx * rb;

        if (word == 0xffffu || word == 0x0000u) {
          const bool set = (word != 0);
          switch (mode) {
            case 1: /* MD_REPLACE */
              fvdi_fill_solid_span_ptr(rowp, rb, run, set ? fg : bg);
              xx += run;
              continue;
            case 2: /* MD_TRANS: clear bits leave the destination alone */
              if (set)
                fvdi_fill_solid_span_ptr(rowp, rb, run, fg);
              xx += run;
              continue;
            case 3: /* MD_XOR: clear bits leave the destination alone */
              if (!set) {
                xx += run;
                continue;
              }
              break;
            case 4: /* MD_ERASE: set bits leave the destination alone */
              if (set) {
                xx += run;
                continue;
              }
              fvdi_fill_solid_span_ptr(rowp, rb, run, bg);
              xx += run;
              continue;
            default:
              break;
          }
        }

        if (mode == 1) {
          for (int32_t i = 0; i < run; i++) {
            const bool set = ((word >> (15 - (bit + i))) & 1u) != 0;
            fvdi_store_px(rowp + (size_t)i * rb, rb, set ? fg : bg);
          }
        } else {
          for (int32_t i = 0; i < run; i++) {
            const uint16_t bit_pattern =
                ((word >> (15 - (bit + i))) & 1u) ? 0xffffu : 0x0000u;
            uint8_t *pp = rowp + (size_t)i * rb;
            fvdi_store_px(pp, rb,
                          fvdi_apply_mono_logic(fvdi_load_px(pp, rb), fg, bg,
                                                bit_pattern, mode));
          }
        }
        xx += run;
      }
    } else {
      if (yy == 0 && (int64_t)w * (int64_t)h >= 4096)
        fvdi_dump_mfdb_miss("expand", mode, src, dst_mfdb, w, h);
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
    }
    if (exp_dst_screen)
      fvdi_note_screen_span(dst_x, dst_y + yy, w);
  }

  return 1;
}

/* Crossing lists are a handful of entries per scanline, so an insertion sort
 * beats qsort's function-pointer comparator by a wide margin - and qsort was
 * being called once per scanline of every polygon. The engine synthesises
 * circles, ellipses, arcs, thick lines and bezier fills as polygons, so this
 * runs far more often than "fill polygon" suggests. */
static void fvdi_sort_int16(int16_t *v, int32_t n)
{
  for (int32_t i = 1; i < n; i++) {
    const int16_t key = v[i];
    int32_t j = i - 1;
    while (j >= 0 && v[j] > key) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = key;
  }
}

static uae_u32 fvdi_fill_polygon(uaecptr points, int32_t count,
                                 uaecptr index_addr, int32_t moves,
                                 uaecptr pattern, uint32_t fg, uint32_t bg,
                                 unsigned mode, uaecptr clip)
{
  if (!points || count <= 0)
    return 1;
  if (count > FVDI_POLY_MAX || moves > FVDI_POLY_MAX)
    return (uae_u32)-1;

  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t max_x = fvdi_dest_width() - 1;
  int32_t max_y = fvdi_dest_height() - 1;

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
  if (max_x >= fvdi_dest_width())
    max_x = fvdi_dest_width() - 1;
  if (max_y >= fvdi_dest_height())
    max_y = fvdi_dest_height() - 1;
  if (min_x > max_x || min_y > max_y)
    return 1;

  /* Scratch is static rather than malloc'd: count and moves are already
   * capped at FVDI_POLY_MAX above, this runs only on the CPU thread inside a
   * single guest instruction, and the old code did three or four
   * malloc/free pairs per polygon call. Same precedent as the ROP blitter's
   * static row buffers. */
  static int16_t px[FVDI_POLY_MAX];
  static int16_t py[FVDI_POLY_MAX];
  static int16_t indices_buf[FVDI_POLY_MAX];
  static int16_t crossings[FVDI_POLY_MAX];
  int16_t *indices = NULL;

  for (int32_t i = 0; i < count; i++) {
    px[i] = (int16_t)nf_read_word(points + (uaecptr)i * 4u);
    py[i] = (int16_t)nf_read_word(points + (uaecptr)i * 4u + 2u);
  }

  bool use_indices = moves > 0 && index_addr != 0;
  if (use_indices) {
    indices = indices_buf;
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
  if (count <= 0)
    return 1;

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

    fvdi_sort_int16(crossings, ints);
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

  return 1;
}

static uae_u32 fvdi_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                              uint32_t fg, uint32_t bg, uint16_t pattern,
                              unsigned mode, uaecptr clip)
{
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t max_x = fvdi_dest_width() - 1;
  int32_t max_y = fvdi_dest_height() - 1;
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

  /* Horizontal solid run. fVDI emits a lot of these - window frames, widget
   * separators, and everything the engine decomposes into single-row spans -
   * and they are exactly a solid span fill, which is a vectorised store loop
   * instead of a Bresenham walk. */
  if (y1 == y2 && pattern == 0xffffu && (mode == 1 || mode == 2) &&
      y1 >= min_y && y1 <= max_y) {
    int32_t lx = x1 < x2 ? x1 : x2;
    int32_t rx = x1 < x2 ? x2 : x1;
    if (lx < min_x)
      lx = min_x;
    if (rx > max_x)
      rx = max_x;
    if (lx <= rx)
      fvdi_fill_solid_span(lx, y1, rx - lx + 1, fg);
    return 1;
  }

  /* General case. The old loop went through fvdi_get_raw_pixel /
   * fvdi_put_raw_pixel per pixel: each call re-derived the framebuffer base,
   * width and depth, and put_raw_pixel marked the dirty rect once *per
   * pixel*. Hoisting the destination row pointer and accumulating one dirty
   * span per row removes both. */
  const int32_t dest_w = fvdi_dest_width();
  uint8_t *rowp = NULL;
  uint32_t rbytes = 0;
  int32_t row_y = -1;
  int32_t run_x0 = 0;
  int32_t run_x1 = -1;

  for (;;) {
    if (x1 >= min_x && x1 <= max_x && y1 >= min_y && y1 <= max_y &&
        fvdi_xy_in_bounds(x1, y1)) {
      if (y1 != row_y) {
        if (run_x1 >= run_x0)
          fvdi_note_screen_span(run_x0, row_y, run_x1 - run_x0 + 1);
        row_y = y1;
        run_x0 = x1;
        run_x1 = x1 - 1;
        rowp = fvdi_dest_span_ptr(0, y1, dest_w, &rbytes);
      }

      const uint16_t pat = (pattern & (1u << (15 - (step & 0x0f)))) ? 0xffffu : 0u;
      if (rowp) {
        uint8_t *pp = rowp + (size_t)x1 * rbytes;
        fvdi_store_px(pp, rbytes,
                      fvdi_apply_logic(fvdi_load_px(pp, rbytes), fg, bg, pat, 0,
                                       mode));
      } else {
        uint32_t dst = fvdi_get_raw_pixel(x1, y1);
        fvdi_put_raw_pixel_marked(x1, y1,
                                  fvdi_apply_logic(dst, fg, bg, pat, 0, mode),
                                  false);
      }

      if (x1 < run_x0)
        run_x0 = x1;
      if (x1 > run_x1)
        run_x1 = x1;
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

  if (run_x1 >= run_x0)
    fvdi_note_screen_span(run_x0, row_y, run_x1 - run_x0 + 1);

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
  if (max_x >= fvdi_dest_width())
    max_x = fvdi_dest_width() - 1;
  if (max_y >= fvdi_dest_height())
    max_y = fvdi_dest_height() - 1;

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

/* ------------------------------------------------------------------
 * Generic row-copy fast path for op==3 (S_ONLY copy), any combination
 * of screen and directly-addressable offscreen MFDBs.
 *
 * WHY (root cause of the level-6 latency stalls): the per-pixel loop
 * below costs ~40ns/pixel; a large offscreen->screen blit (the standard
 * AES redraw) is ~1M pixels = 30-160ms executed synchronously on the
 * CPU thread as ONE guest instruction, during which no interrupt can
 * be delivered. The ACIA tolerates ~2.56ms -> IKBD bytes lost ->
 * keyboard beeping / erratic mouse under load. Row memcpy brings a
 * full-screen blit under ~1ms, meeting the <1ms level-6 requirement
 * without touching any interrupt machinery.
 *
 * Correctness notes:
 *  - op 3 is a raw copy; both surfaces store big-endian byte streams,
 *    so a byte copy is exact for 16/24/32bpp when depths match.
 *  - offscreen dst in ST-RAM goes through pistorm_dma_to_stram per row
 *    (same primitive the per-pixel path used per pixel).
 *  - a row buffer makes same-surface horizontal overlap safe; vertical
 *    overlap is handled by row order (bottom-up when dst_y > src_y).
 *  - any precondition failure returns false and the caller falls back
 *    to the existing per-pixel loop (copy is idempotent, so a partial
 *    fast-path pass followed by the slow path is still correct).
 */
/* Diagnostic (measurement only): when a big op falls back to the per-pixel
 * path, print the parameters once per second so we can see exactly which
 * fast-path precondition declined. */
static void fvdi_dump_mfdb_miss(const char *tag, unsigned op,
                                uaecptr src_mfdb, uaecptr dst_mfdb,
                                int32_t w, int32_t h)
{
#ifndef ATARI_LAT_DIAG
  (void)tag; (void)op; (void)src_mfdb; (void)dst_mfdb; (void)w; (void)h;
  return;
#else
  static uint64_t last;
  const uint64_t now = get_time_us();
  if (now - last < 1000000)
    return;
  last = now;
  char buf[320];
  int len = snprintf(buf, sizeof buf, "[FVDI-MISS] %s op=%u w=%d h=%d", tag, op, w, h);
  const uaecptr m[2] = { src_mfdb, dst_mfdb };
  const char *nm[2] = { "src", "dst" };
  for (int i = 0; i < 2; i++) {
    if (fvdi_mfdb_is_screen(m[i])) {
      len += snprintf(buf + len, sizeof buf - (size_t)len, " %s=SCREEN", nm[i]);
    } else {
      len += snprintf(buf + len, sizeof buf - (size_t)len,
                      " %s=%08X(base=%08X w=%u h=%u wdw=%u stand=%u bpp=%u)",
                      nm[i], (unsigned)m[i],
                      (unsigned)nf_read_long(m[i] + 0),
                      (unsigned)nf_read_word(m[i] + 4),
                      (unsigned)nf_read_word(m[i] + 6),
                      (unsigned)nf_read_word(m[i] + 8),
                      (unsigned)nf_read_word(m[i] + 10),
                      (unsigned)nf_read_word(m[i] + 12));
    }
    if (len > (int)sizeof buf - 8) break;
  }
  fprintf(stderr, "%s\n", buf);
#endif /* ATARI_LAT_DIAG */
}

static bool fvdi_copy_rows_generic(uaecptr src_mfdb, uaecptr dst_mfdb,
                                   int32_t src_x, int32_t src_y,
                                   int32_t dst_x, int32_t dst_y,
                                   int32_t w, int32_t h)
{
  const bool src_scr = fvdi_mfdb_is_screen(src_mfdb);
  const bool dst_scr = fvdi_mfdb_is_screen(dst_mfdb);

  if (src_scr && dst_scr)
    return fvdi_screen_copy_rows(src_x, src_y, dst_x, dst_y, w, h);

  const uint32_t sbytes = src_scr ? fvdi_bytes_per_pixel()
                                  : fvdi_mfdb_pixel_bytes(src_mfdb);
  const uint32_t dbytes = dst_scr ? fvdi_bytes_per_pixel()
                                  : fvdi_mfdb_pixel_bytes(dst_mfdb);
  if (!sbytes || sbytes != dbytes)
    return false;
  if (!src_scr && !fvdi_mfdb_supported_direct(src_mfdb))
    return false;
  if (!dst_scr && !fvdi_mfdb_supported_direct(dst_mfdb))
    return false;

  static uae_u8 rowbuf[FVDI_MAX_ACCEL_SPAN * 4];
  const size_t row_bytes = (size_t)w * sbytes;
  if (row_bytes == 0 || row_bytes > sizeof rowbuf)
    return false;

  const bool cp_no_overlap = (src_scr != dst_scr) ||
                             (!src_scr && !dst_scr && src_mfdb != dst_mfdb);

  int32_t yy = 0, yy_end = h, step = 1;
  if (dst_y > src_y) { yy = h - 1; yy_end = -1; step = -1; }

  for (; yy != yy_end; yy += step) {
    if (cp_no_overlap) {
      /* different surfaces: one direct pass, no staging (lever 1) */
      const uae_u8 *sp;
      if (src_scr) {
        sp = fvdi_screen_span_ptr(src_x, src_y + yy, w);
        if (!sp) return false;
      } else {
        const uaecptr sa = fvdi_mfdb_pixel_addr(src_mfdb, src_x, src_y + yy);
        const uaecptr se = fvdi_mfdb_pixel_addr(src_mfdb, src_x + w - 1, src_y + yy);
        uae_u8 *q;
        if (!sa || !se || se != sa + (uaecptr)(w - 1) * sbytes ||
            !nf_host_ram_ptr(sa, (uint32_t)row_bytes, &q))
          return false;
        sp = q;
      }
      if (dst_scr) {
        uae_u8 *dp = fvdi_screen_span_ptr(dst_x, dst_y + yy, w);
        if (!dp) return false;
        memcpy(dp, sp, row_bytes);
        fvdi_note_screen_span(dst_x, dst_y + yy, w);
      } else {
        const uaecptr da = fvdi_mfdb_pixel_addr(dst_mfdb, dst_x, dst_y + yy);
        const uaecptr de = fvdi_mfdb_pixel_addr(dst_mfdb, dst_x + w - 1, dst_y + yy);
        uae_u8 *dp;
        if (!da || !de || de != da + (uaecptr)(w - 1) * sbytes)
          return false;
        if (da < NF_ST_RAM_SIZE) {
          if (row_bytes > NF_ST_RAM_SIZE - da)
            return false;
          pistorm_dma_to_stram(da, sp, (uint32_t)row_bytes);
        } else if (nf_host_ram_ptr(da, (uint32_t)row_bytes, &dp)) {
          memcpy(dp, sp, row_bytes);
        } else {
          return false;
        }
      }
      continue;
    }
    /* source row -> rowbuf */
    if (src_scr) {
      const uint8_t *sp = fvdi_screen_span_ptr(src_x, src_y + yy, w);
      if (!sp)
        return false;
      memcpy(rowbuf, sp, row_bytes);
    } else {
      const uaecptr sa = fvdi_mfdb_pixel_addr(src_mfdb, src_x, src_y + yy);
      const uaecptr se = fvdi_mfdb_pixel_addr(src_mfdb, src_x + w - 1, src_y + yy);
      uae_u8 *sp;
      if (!sa || !se || se != sa + (uaecptr)(w - 1) * sbytes)
        return false;                      /* non-contiguous or out of bounds */
      if (!nf_host_ram_ptr(sa, (uint32_t)row_bytes, &sp))
        return false;
      memcpy(rowbuf, sp, row_bytes);
    }

    /* rowbuf -> destination row */
    if (dst_scr) {
      uint8_t *dp = fvdi_screen_span_ptr(dst_x, dst_y + yy, w);
      if (!dp)
        return false;
      memcpy(dp, rowbuf, row_bytes);
      fvdi_note_screen_span(dst_x, dst_y + yy, w);
    } else {
      const uaecptr da = fvdi_mfdb_pixel_addr(dst_mfdb, dst_x, dst_y + yy);
      const uaecptr de = fvdi_mfdb_pixel_addr(dst_mfdb, dst_x + w - 1, dst_y + yy);
      uae_u8 *dp;
      if (!da || !de || de != da + (uaecptr)(w - 1) * dbytes)
        return false;
      if (da < NF_ST_RAM_SIZE && row_bytes <= NF_ST_RAM_SIZE - da) {
        pistorm_dma_to_stram(da, rowbuf, (uint32_t)row_bytes);
      } else if (nf_host_ram_ptr(da, (uint32_t)row_bytes, &dp)) {
        memcpy(dp, rowbuf, row_bytes);
      } else {
        return false;
      }
    }
  }

  return true;
}

/* Row fast path for ALL raster ops (latency work, phase 4).
 * [FVDI-MISS] showed Boing compositing with op 7 (S OR D) into an offscreen
 * 32bpp TT-RAM buffer; the per-pixel fallback made each blit a 5-15ms
 * uninterruptible natfeat op. Every VDI rop is a pure bitwise function of
 * S and D, and bitwise ops are byte-order agnostic, so they apply bytewise
 * on host row pointers for any depth as long as src/dst depths match.
 * Two passes: rows are validated (addresses resolvable, contiguous) BEFORE
 * any write, so a decline can never leave a partially-combined surface. */
static bool fvdi_blit_rows_rop(uaecptr src_mfdb, uaecptr dst_mfdb,
                               int32_t src_x, int32_t src_y,
                               int32_t dst_x, int32_t dst_y,
                               int32_t w, int32_t h, unsigned op)
{
  op &= 15u;
  if (op == 5)                    /* D only: destination unchanged */
    return true;

  const bool src_scr = fvdi_mfdb_is_screen(src_mfdb);
  const bool dst_scr = fvdi_mfdb_is_screen(dst_mfdb);
  const bool src_needed = !(op == 0 || op == 10 || op == 15);
  const uint32_t sbytes = src_scr ? fvdi_bytes_per_pixel()
                                  : fvdi_mfdb_pixel_bytes(src_mfdb);
  const uint32_t dbytes = dst_scr ? fvdi_bytes_per_pixel()
                                  : fvdi_mfdb_pixel_bytes(dst_mfdb);
  if (!dbytes)
    return false;
  if (src_needed && (!sbytes || sbytes != dbytes))
    return false;
  if (src_needed && !src_scr && !fvdi_mfdb_supported_direct(src_mfdb))
    return false;
  if (!dst_scr && !fvdi_mfdb_supported_direct(dst_mfdb))
    return false;

  static uae_u8 ropsrc[FVDI_MAX_ACCEL_SPAN * 4];
  static uae_u8 ropdst[FVDI_MAX_ACCEL_SPAN * 4];
  const size_t row_bytes = (size_t)w * dbytes;
  if (row_bytes == 0 || row_bytes > sizeof ropdst)
    return false;

  /* pass 1: validate every row before touching anything */
  for (int32_t yy = 0; yy < h; yy++) {
    if (src_needed) {
      if (src_scr) {
        if (!fvdi_screen_span_ptr(src_x, src_y + yy, w))
          return false;
      } else {
        const uaecptr sa = fvdi_mfdb_pixel_addr(src_mfdb, src_x, src_y + yy);
        const uaecptr se = fvdi_mfdb_pixel_addr(src_mfdb, src_x + w - 1, src_y + yy);
        uae_u8 *sp;
        if (!sa || !se || se != sa + (uaecptr)(w - 1) * sbytes ||
            !nf_host_ram_ptr(sa, (uint32_t)row_bytes, &sp))
          return false;
      }
    }
    if (dst_scr) {
      if (!fvdi_screen_span_ptr(dst_x, dst_y + yy, w))
        return false;
    } else {
      const uaecptr da = fvdi_mfdb_pixel_addr(dst_mfdb, dst_x, dst_y + yy);
      const uaecptr de = fvdi_mfdb_pixel_addr(dst_mfdb, dst_x + w - 1, dst_y + yy);
      uae_u8 *dp;
      if (!da || !de || de != da + (uaecptr)(w - 1) * dbytes ||
          !nf_host_ram_ptr(da, (uint32_t)row_bytes, &dp))
        return false;
      if (da < NF_ST_RAM_SIZE && row_bytes > NF_ST_RAM_SIZE - da)
        return false;
    }
  }

  /* Different surfaces cannot overlap, so skip the staging copies and do a
   * single read + read-modify-write pass on host pointers (lever 1: halves
   * memory traffic; matters at 1920x1080x32 where a full frame is 8.3MB). */
  const bool no_overlap = (src_scr != dst_scr) ||
                          (!src_scr && !dst_scr && src_mfdb != dst_mfdb);

  /* pass 2: execute */
  int32_t yy = 0, yy_end = h, step = 1;
  if (dst_y > src_y) { yy = h - 1; yy_end = -1; step = -1; }

  for (; yy != yy_end; yy += step) {
    if (no_overlap) {
      /* resolve host pointers directly */
      const uae_u8 *sp = NULL;
      if (src_needed) {
        if (src_scr) {
          sp = fvdi_screen_span_ptr(src_x, src_y + yy, w);
        } else {
          uae_u8 *q;
          nf_host_ram_ptr(fvdi_mfdb_pixel_addr(src_mfdb, src_x, src_y + yy),
                          (uint32_t)row_bytes, &q);
          sp = q;
        }
      }
      uae_u8 *dp2 = NULL;
      uaecptr da2 = 0;
      if (dst_scr) {
        dp2 = fvdi_screen_span_ptr(dst_x, dst_y + yy, w);
      } else {
        da2 = fvdi_mfdb_pixel_addr(dst_mfdb, dst_x, dst_y + yy);
        uae_u8 *q;
        nf_host_ram_ptr(da2, (uint32_t)row_bytes, &q);
        dp2 = (da2 < NF_ST_RAM_SIZE) ? NULL : q;
      }
      if (dp2) {
        switch (op) {
          case 0:  memset(dp2, 0x00, row_bytes); break;
          case 1:  for (size_t i = 0; i < row_bytes; i++) dp2[i] &= sp[i]; break;
          case 2:  for (size_t i = 0; i < row_bytes; i++) dp2[i] = sp[i] & (uae_u8)~dp2[i]; break;
          case 3:  memcpy(dp2, sp, row_bytes); break;
          case 4:  for (size_t i = 0; i < row_bytes; i++) dp2[i] = (uae_u8)~sp[i] & dp2[i]; break;
          case 6:  for (size_t i = 0; i < row_bytes; i++) dp2[i] ^= sp[i]; break;
          case 7:  for (size_t i = 0; i < row_bytes; i++) dp2[i] |= sp[i]; break;
          case 8:  for (size_t i = 0; i < row_bytes; i++) dp2[i] = (uae_u8)~(sp[i] | dp2[i]); break;
          case 9:  for (size_t i = 0; i < row_bytes; i++) dp2[i] = (uae_u8)~(sp[i] ^ dp2[i]); break;
          case 10: for (size_t i = 0; i < row_bytes; i++) dp2[i] = (uae_u8)~dp2[i]; break;
          case 11: for (size_t i = 0; i < row_bytes; i++) dp2[i] = sp[i] | (uae_u8)~dp2[i]; break;
          case 12: for (size_t i = 0; i < row_bytes; i++) dp2[i] = (uae_u8)~sp[i]; break;
          case 13: for (size_t i = 0; i < row_bytes; i++) dp2[i] = (uae_u8)~sp[i] | dp2[i]; break;
          case 14: for (size_t i = 0; i < row_bytes; i++) dp2[i] = (uae_u8)~(sp[i] & dp2[i]); break;
          case 15: memset(dp2, 0xFF, row_bytes); break;
        }
        if (dst_scr)
          fvdi_note_screen_span(dst_x, dst_y + yy, w);
        continue;
      }
      /* ST-RAM destination: fall through to the staged path below */
    }
    if (src_needed) {
      if (src_scr) {
        memcpy(ropsrc, fvdi_screen_span_ptr(src_x, src_y + yy, w), row_bytes);
      } else {
        uae_u8 *sp;
        const uaecptr sa = fvdi_mfdb_pixel_addr(src_mfdb, src_x, src_y + yy);
        nf_host_ram_ptr(sa, (uint32_t)row_bytes, &sp);
        memcpy(ropsrc, sp, row_bytes);
      }
    }

    uae_u8 *dp = NULL;
    uaecptr da = 0;
    if (dst_scr) {
      dp = fvdi_screen_span_ptr(dst_x, dst_y + yy, w);
    } else {
      da = fvdi_mfdb_pixel_addr(dst_mfdb, dst_x, dst_y + yy);
      uae_u8 *p;
      nf_host_ram_ptr(da, (uint32_t)row_bytes, &p);
      dp = (da < NF_ST_RAM_SIZE) ? NULL : p;   /* ST-RAM writes go via DMA */
      if (!dp)
        memcpy(ropdst, p, row_bytes);          /* mirror read is valid */
    }
    if (dp)
      memcpy(ropdst, dp, row_bytes);

    switch (op) {
      case 0:  memset(ropdst, 0x00, row_bytes); break;
      case 1:  for (size_t i = 0; i < row_bytes; i++) ropdst[i] = ropsrc[i] & ropdst[i]; break;
      case 2:  for (size_t i = 0; i < row_bytes; i++) ropdst[i] = ropsrc[i] & (uae_u8)~ropdst[i]; break;
      case 3:  memcpy(ropdst, ropsrc, row_bytes); break;
      case 4:  for (size_t i = 0; i < row_bytes; i++) ropdst[i] = (uae_u8)~ropsrc[i] & ropdst[i]; break;
      case 6:  for (size_t i = 0; i < row_bytes; i++) ropdst[i] ^= ropsrc[i]; break;
      case 7:  for (size_t i = 0; i < row_bytes; i++) ropdst[i] |= ropsrc[i]; break;
      case 8:  for (size_t i = 0; i < row_bytes; i++) ropdst[i] = (uae_u8)~(ropsrc[i] | ropdst[i]); break;
      case 9:  for (size_t i = 0; i < row_bytes; i++) ropdst[i] = (uae_u8)~(ropsrc[i] ^ ropdst[i]); break;
      case 10: for (size_t i = 0; i < row_bytes; i++) ropdst[i] = (uae_u8)~ropdst[i]; break;
      case 11: for (size_t i = 0; i < row_bytes; i++) ropdst[i] = ropsrc[i] | (uae_u8)~ropdst[i]; break;
      case 12: for (size_t i = 0; i < row_bytes; i++) ropdst[i] = (uae_u8)~ropsrc[i]; break;
      case 13: for (size_t i = 0; i < row_bytes; i++) ropdst[i] = (uae_u8)~ropsrc[i] | ropdst[i]; break;
      case 14: for (size_t i = 0; i < row_bytes; i++) ropdst[i] = (uae_u8)~(ropsrc[i] & ropdst[i]); break;
      case 15: memset(ropdst, 0xFF, row_bytes); break;
    }

    if (dp) {
      memcpy(dp, ropdst, row_bytes);
      if (dst_scr)
        fvdi_note_screen_span(dst_x, dst_y + yy, w);
    } else {
      pistorm_dma_to_stram(da, ropdst, (uint32_t)row_bytes);
    }
  }

  return true;
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

  /* Fast path for plain copies between any direct surfaces (see above). */
  if (op == 3 &&
      fvdi_copy_rows_generic(src_mfdb, dst_mfdb, src_x, src_y,
                             dst_x, dst_y, w, h))
    return 1;

  if (fvdi_blit_rows_rop(src_mfdb, dst_mfdb, src_x, src_y,
                         dst_x, dst_y, w, h, op))
    return 1;

  if ((int64_t)w * (int64_t)h >= 4096)
    fvdi_dump_mfdb_miss("blit", op, src_mfdb, dst_mfdb, w, h);

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
  int32_t last_x;             /* last show() position (pre-hotspot) */
  int32_t last_y;
  uint32_t backup[16 * 16];
} fvdi_mouse_state_t;

static fvdi_mouse_state_t g_fvdi_mouse;

static void fvdi_mouse_hide(void)
{
  if (!g_fvdi_mouse.visible)
    return;

  /* Row-hoisted: hide/show run on every cursor move *and* around every
   * drawing op whose rectangle intersects the cursor, so 256 dispatcher
   * round-trips with a dirty-rect mark per pixel was a fixed tax on the
   * whole fVDI path. One span mark per row now. */
  const int32_t bx = g_fvdi_mouse.backup_x;
  const int32_t by = g_fvdi_mouse.backup_y;
  const int32_t bw = g_fvdi_mouse.backup_w;
  const uint32_t bytes = fvdi_bytes_per_pixel();

  for (int32_t yy = 0; yy < g_fvdi_mouse.backup_h; yy++) {
    uint8_t *rp = g_fvdi_dest_mfdb ? NULL : fvdi_screen_span_ptr(bx, by + yy, bw);
    if (rp && (bytes == 2 || bytes == 4)) {
      for (int32_t xx = 0; xx < bw; xx++)
        fvdi_store_px(rp + (size_t)xx * bytes, bytes,
                      g_fvdi_mouse.backup[yy * 16 + xx]);
      fvdi_note_screen_span(bx, by + yy, bw);
    } else {
      for (int32_t xx = 0; xx < bw; xx++)
        fvdi_put_raw_pixel(bx + xx, by + yy, g_fvdi_mouse.backup[yy * 16 + xx]);
    }
  }

  g_fvdi_mouse.visible = false;
}

static void fvdi_mouse_show(int32_t x, int32_t y)
{
  if (!g_fvdi_mouse.shape_set)
    return;

  g_fvdi_mouse.last_x = x;
  g_fvdi_mouse.last_y = y;

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

  const uint32_t bytes = fvdi_bytes_per_pixel();

  for (int32_t yy = 0; yy < h; yy++) {
    uint16_t mask = g_fvdi_mouse.mask[sy + yy] << sx;
    uint16_t data = g_fvdi_mouse.data[sy + yy] << sx;
    uint8_t *rp = g_fvdi_dest_mfdb ? NULL : fvdi_screen_span_ptr(dx, dy + yy, w);

    if (rp && (bytes == 2 || bytes == 4)) {
      bool touched = false;
      for (int32_t xx = 0; xx < w; xx++) {
        uint8_t *pp = rp + (size_t)xx * bytes;
        g_fvdi_mouse.backup[yy * 16 + xx] = fvdi_load_px(pp, bytes);
        if (data & 0x8000u) {
          fvdi_store_px(pp, bytes, g_fvdi_mouse.fg);
          touched = true;
        } else if (mask & 0x8000u) {
          fvdi_store_px(pp, bytes, g_fvdi_mouse.bg);
          touched = true;
        }
        mask <<= 1;
        data <<= 1;
      }
      if (touched)
        fvdi_note_screen_span(dx, dy + yy, w);
    } else {
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
  }

  g_fvdi_mouse.visible = true;
}

/* -----------------------------------------------------------------------
 * Cursor guard. The host-drawn cursor keeps a 16x16 backup of the screen
 * under it and stamps that backup back when hidden or moved. Drawing ops
 * used to ignore it, so an op painting under the cursor was overwritten by
 * the STALE backup on the next cursor move (mouse droppings), and blits
 * could read the drawn cursor image into a copy. Every host-side op that
 * touches the screen now hides the cursor first when its rectangle
 * intersects it, and re-shows it afterwards. All serialized on the CPU
 * thread via natfeat. */
static bool fvdi_mouse_obscure_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
  if (!g_fvdi_mouse.visible || w <= 0 || h <= 0)
    return false;
  if (x + w <= g_fvdi_mouse.backup_x ||
      g_fvdi_mouse.backup_x + g_fvdi_mouse.backup_w <= x ||
      y + h <= g_fvdi_mouse.backup_y ||
      g_fvdi_mouse.backup_y + g_fvdi_mouse.backup_h <= y)
    return false;
  fvdi_mouse_hide();
  return true;
}

static bool fvdi_mouse_obscure_all(void)
{
  if (!g_fvdi_mouse.visible)
    return false;
  fvdi_mouse_hide();
  return true;
}

static void fvdi_mouse_unobscure(bool was_visible)
{
  if (was_visible)
    fvdi_mouse_show(g_fvdi_mouse.last_x, g_fvdi_mouse.last_y);
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

/* Per-op timing (measurement only): names the op that stalls level-6
 * delivery, instead of inferring it from stripped symbol offsets. Printed
 * once per second as an [FVDI] line for ops that consumed time. */
static struct { uint32_t n; uint32_t us; uint32_t maxus; } g_fvdi_prof[32];

static void fvdi_prof_flush(uint64_t now)
{
  static uint64_t win;
  if (now - win < 1000000)
    return;
  win = now;
  char line[256];
  int len = 0;
  for (unsigned i = 0; i < 32; i++) {
    if (!g_fvdi_prof[i].n)
      continue;
    len += snprintf(line + len, sizeof line - (size_t)len,
                    " op%u:n=%u,us=%u,max=%u", i,
                    g_fvdi_prof[i].n, g_fvdi_prof[i].us, g_fvdi_prof[i].maxus);
    g_fvdi_prof[i].n = g_fvdi_prof[i].us = g_fvdi_prof[i].maxus = 0;
    if (len > (int)sizeof line - 40)
      break;
  }
  if (len)
    fprintf(stderr, "[FVDI]%s\n", line);
}

static uae_u32 nf_call_fvdi_inner(uae_u32 subid, uaecptr params);

static uae_u32 nf_call_fvdi(uae_u32 subid, uaecptr params)
{
  /* MFDB headers cannot change while a NatFeat call runs (one guest
   * instruction, CPU thread), so the header cache is valid for exactly the
   * span of this call and is dropped on entry to the next one. */
  fvdi_mfdb_cache_reset();

#ifndef ATARI_LAT_DIAG
  return nf_call_fvdi_inner(subid, params);
#else
  const uint64_t t0 = get_time_us();
  uae_u32 r = nf_call_fvdi_inner(subid, params);
  const uint64_t t1 = get_time_us();
  if (subid < 32) {
    const uint32_t d = (uint32_t)(t1 - t0);
    g_fvdi_prof[subid].n++;
    g_fvdi_prof[subid].us += d;
    if (d > g_fvdi_prof[subid].maxus)
      g_fvdi_prof[subid].maxus = d;
  }
  fvdi_prof_flush(t1);
  return r;
#endif /* ATARI_LAT_DIAG */
}

/* Resolve the fill/line/polygon destination out of the workstation and arm
 * the redirection. Stays disarmed (screen) whenever the destination is the
 * screen or cannot be resolved, so screen drawing is untouched. */
static void fvdi_dest_begin(uae_u32 vwk)
{
  g_fvdi_dest_mfdb = 0;
  g_fvdi_dest_w = 0;
  g_fvdi_dest_h = 0;

  uaecptr real = (uaecptr)(vwk & ~1u);
  if (!real)
    return;
  uaecptr wk = nf_read_long(real + FVDI_VWK_REAL_ADDRESS);
  if (!wk)
    return;

  uaecptr mfdb = wk + FVDI_WK_SCREEN_MFDB;
  uae_u32 addr = nf_read_long(mfdb + 0);
  if (addr == 0 || addr == pistorm_fvdi_fb_base())
    return;                       /* the screen - behave exactly as before */

  int32_t w = (int32_t)nf_read_word(mfdb + 4);
  int32_t h = (int32_t)nf_read_word(mfdb + 6);
  if (w <= 0 || h <= 0)
    return;

  g_fvdi_dest_mfdb = mfdb;
  g_fvdi_dest_w = w;
  g_fvdi_dest_h = h;
}

static void fvdi_dest_end(void)
{
  g_fvdi_dest_mfdb = 0;
  g_fvdi_dest_w = 0;
  g_fvdi_dest_h = 0;
}

static uae_u32 nf_call_fvdi_inner(uae_u32 subid, uaecptr params)
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
      bool gp_m = fvdi_mouse_obscure_rect(x, y, 1, 1);
      uae_u32 gp_v = fvdi_get_raw_pixel(x, y);
      fvdi_mouse_unobscure(gp_m);
      return gp_v;
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
      bool pp_m = fvdi_mouse_obscure_rect(x, y, 1, 1);
      fvdi_put_raw_pixel(x, y, colour);
      fvdi_mouse_unobscure(pp_m);
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
      bool exp_m = fvdi_mfdb_is_screen(dst) &&
                   fvdi_mouse_obscure_rect(dst_x, dst_y, w, h);
      uae_u32 exp_r = fvdi_expand_mono(src, dst, src_x, src_y, dst_x, dst_y,
                                       w, h, mode, fg, bg);
      fvdi_mouse_unobscure(exp_m);
      return exp_r;
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
      fvdi_dest_begin(vwk);
      bool fill_m = false;
      uae_u32 fill_r;
      if (vwk & 1u) {
        if (((uint32_t)y & 0xffffu) != 0)
          fill_r = (uae_u32)-1;
        else {
          if (!g_fvdi_dest_mfdb)
            fill_m = fvdi_mouse_obscure_all();    /* span table: extent unknown */
          fill_r = fvdi_fill_table((uaecptr)(uint32_t)x,
                                   (int32_t)((uint32_t)y >> 16),
                                   pattern, fg, bg, mode);
        }
      } else {
        if (!g_fvdi_dest_mfdb)
          fill_m = fvdi_mouse_obscure_rect(x, y, w, h);
        fill_r = fvdi_fill_rect(x, y, w, h, pattern, fg, bg, mode);
      }
      fvdi_mouse_unobscure(fill_m);
      fvdi_dest_end();
      return fill_r;
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
      bool blit_m = false;
      if (fvdi_mfdb_is_screen(src))
        blit_m |= fvdi_mouse_obscure_rect(src_x, src_y, w, h);
      if (fvdi_mfdb_is_screen(dst))
        blit_m |= fvdi_mouse_obscure_rect(dst_x, dst_y, w, h);
      uae_u32 blit_r = fvdi_blit_area(src, dst, src_x, src_y, dst_x, dst_y,
                                      w, h, op);
      fvdi_mouse_unobscure(blit_m);
      return blit_r;
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

      fvdi_dest_begin((uae_u32)vwk);
      bool line_m = false;
      uae_u32 line_r;
      if (vwk & 1u) {
        if (!g_fvdi_dest_mfdb)
          line_m = fvdi_mouse_obscure_all();      /* line table: extent unknown */
        line_r = fvdi_draw_line_table((uaecptr)(uint32_t)x1, (uint32_t)y1,
                                      (uaecptr)(uint32_t)y2, (uint32_t)x2 & 0xffffu,
                                      fg, bg, pattern, mode, clip);
      } else if (!fvdi_line_coords_plausible(x1, y1, x2, y2))
        line_r = 1;
      else {
        if (!g_fvdi_dest_mfdb) {
          int32_t bx0 = x1 < x2 ? x1 : x2, bx1 = x1 < x2 ? x2 : x1;
          int32_t by0 = y1 < y2 ? y1 : y2, by1 = y1 < y2 ? y2 : y1;
          line_m = fvdi_mouse_obscure_rect(bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
        }
        line_r = fvdi_draw_line(x1, y1, x2, y2, fg, bg, pattern, mode, clip);
      }
      fvdi_mouse_unobscure(line_m);
      fvdi_dest_end();
      return line_r;
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
      fvdi_dest_begin(vwk);
      bool poly_m = false;
      if (!g_fvdi_dest_mfdb)
        poly_m = fvdi_mouse_obscure_all();
      uae_u32 poly_r = fvdi_fill_polygon(points, count, index, moves,
                                         pattern, fg, bg, mode, clip);
      fvdi_mouse_unobscure(poly_m);
      fvdi_dest_end();
      return poly_r;
    }

    case FVDI_TEXT_AREA:
    {
      /* Deliberately declined (fvdi_text_area() below is kept but unused).
       *
       * Returning 0 makes the fVDI engine render the whole string into a
       * temporary mono buffer and issue ONE vrt_cpyfm, i.e. one
       * FVDI_EXPAND_AREA per string - which after the run-oriented expand
       * loop above is faster than the per-pixel text_area() renderer would
       * be. aranym.sys also pre-filters this call hard (drivers/aranym/
       * dispatch.c: no text effects, no offset table, unpacked bitmap font,
       * cell width exactly 8), so implementing it host-side would only ever
       * capture the GEM system font and never the antialiased FreeType text
       * that XaAES actually draws. ARAnyM declines it for the same reason.
       *
       * Two caveats worth knowing if this is ever revisited:
       *  - the engine falls back to one vrt_cpyfm *per character* when
       *    asm_allocate_block() fails, so `blocks` / `blocksize` in fvdi.sys
       *    matter for text throughput;
       *  - antialiased FT2 text is always one EXPAND_AREA per glyph with
       *    standard=0x0100 / nplanes=8 (chunky alpha), which fvdi_expand_mono
       *    rejects - collapsing that would need a guest-driver change, not a
       *    host one. */
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

      hostfs_node_t *node = hostfs_alloc_path_node(idx,
                                                   (uae_u16)nf_read_word(dir_cookie + 4),
                                                   resolved);
      if (!node)
        return TOS_ENOSYS;

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

      hostfs_node_t *node = hostfs_alloc_path_node(host_dir->mount_index,
                                                   host_dir->dev, child_path);
      if (!node)
        return TOS_ENOSYS;

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
      HOSTFS_LOG("[NF] HOSTFS.XFS_RELEASE cookie=0x%08X -> 0\n", cookie);
      return TOS_E_OK;
    }

    case HOSTFS_XFS_DUPCOOKIE:
    {
      uaecptr dst = nf_get_param(params, 0);
      uaecptr src = nf_get_param(params, 1);
      if (!dst || !src)
        return TOS_EDRIVE;
      hostfs_copy_cookie(dst, src);
      HOSTFS_LOG("[NF] HOSTFS.XFS_DUPCOOKIE src=0x%08X dst=0x%08X -> 0\n",
              src, dst);
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

/* Translate a GEMDOS path ("U:\dir\file.mp3") to a host path via the mounted
 * HOSTFS drive whose letter matches. Requires an explicit drive letter. */
static bool mp3_gemdos_to_host(const char *gem, char *out, size_t outsz)
{
  if (!gem || !gem[0])
    return false;

  char letter = 0;
  const char *rest = NULL;
  if (gem[1] == ':') {
    /* GEMDOS drive form:  X:\dir\file  or  X:/dir/file */
    letter = gem[0];
    rest = gem + 2;
  } else if (gem[0] == '/' && gem[1] && gem[2] == '/') {
    /* MiNT unix form:  /x/dir/file */
    letter = gem[1];
    rest = gem + 2;                 /* keep the '/' after the letter */
  } else {
    return false;
  }
  if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');

  const char *root = NULL;
  for (unsigned i = 0; i < ATARI_NATFEAT_HOSTFS_MAX_DRIVES; i++) {
    if (g_hostfs_mounts[i].mounted && g_hostfs_mounts[i].drive) {
      char dl = g_hostfs_mounts[i].drive->drive;
      if (dl >= 'a' && dl <= 'z') dl = (char)(dl - 'a' + 'A');
      if (dl == letter) { root = g_hostfs_mounts[i].drive->path; break; }
    }
  }
  if (!root)
    return false;

  while (*rest == '\\' || *rest == '/') rest++;
  int n = snprintf(out, outsz, "%s/%s", root, rest);
  if (n < 0 || (size_t)n >= outsz)
    return false;
  for (char *p = out; *p; p++)
    if (*p == '\\') *p = '/';
  return true;
}

static uae_u32 nf_call_mp3(uae_u32 subid, uaecptr params)
{
  switch (subid) {
    case NF_MP3_PLAY: {
      uaecptr pathp = nf_get_param(params, 0);
      char gem[512];
      char host[HOSTFS_HOST_PATH_MAX + 16];
      if (!pathp)
        return (uae_u32)-1;
      nf_read_string(pathp, gem, sizeof(gem));
      if (!mp3_gemdos_to_host(gem, host, sizeof(host))) {
        fprintf(stderr, "[NF] MP3.PLAY '%s' -> could not map to a host path "
                "(need a mounted HOSTFS drive letter; accepts X:\\.. or /x/..)\n", gem);
        return (uae_u32)-1;
      }
      int rc = dmasnd_mp3_play(host);
      HOSTFS_LOG("[NF] MP3.PLAY '%s' -> host '%s' rc=%d\n", gem, host, rc);
      return rc == 0 ? 0u : (uae_u32)-1;
    }
    case NF_MP3_STOP:
      dmasnd_mp3_stop();
      HOSTFS_LOG("[NF] MP3.STOP\n");
      return 0;
    case NF_MP3_STATUS:
      return (uae_u32)dmasnd_mp3_active();
    case NF_MP3_PAUSE:
      dmasnd_mp3_pause((int)nf_get_param(params, 0));
      return 0;
    case NF_MP3_SEEK:
      dmasnd_mp3_seek_rel((long)(int32_t)nf_get_param(params, 0));
      return 0;
    case NF_MP3_POS:
      return (uae_u32)dmasnd_mp3_pos_s();
    case NF_MP3_LEN:
      return (uae_u32)dmasnd_mp3_len_s();
    case NF_MP3_META: {
      uae_u32 which = nf_get_param(params, 0);
      uaecptr buf = nf_get_param(params, 1);
      uae_u32 len = nf_get_param(params, 2);
      if (!buf || len == 0)
        return (uae_u32)-1;
      nf_write_string(buf, len, dmasnd_mp3_meta((int)which));
      return 0;
    }
  }
  return (uae_u32)-1;
}

static uae_u32 nf_call_video(uae_u32 subid, uaecptr params)
{
  switch (subid) {
    case NF_VID_PLAY: {
      uaecptr pathp = nf_get_param(params, 0);
      char gem[512];
      char host[HOSTFS_HOST_PATH_MAX + 16];
      if (!pathp)
        return (uae_u32)-1;
      nf_read_string(pathp, gem, sizeof(gem));
      if (!mp3_gemdos_to_host(gem, host, sizeof(host))) {
        fprintf(stderr, "[NF] VID.PLAY '%s' -> could not map to a host path "
                "(need a mounted HOSTFS drive letter; accepts X:\\.. or /x/..)\n", gem);
        return (uae_u32)-1;
      }
      int rc = vidplay_play(host);
      HOSTFS_LOG("[NF] VID.PLAY '%s' -> host '%s' rc=%d\n", gem, host, rc);
      return rc == 0 ? 0u : (uae_u32)-1;
    }
    case NF_VID_STOP:
      vidplay_stop();
      HOSTFS_LOG("[NF] VID.STOP\n");
      return 0;
    case NF_VID_STATUS:
      return (uae_u32)vidplay_active();
    case NF_VID_PAUSE:
      vidplay_pause((int)nf_get_param(params, 0));
      return 0;
    case NF_VID_SEEK:
      vidplay_seek_rel((long)(int32_t)nf_get_param(params, 0));
      return 0;
    case NF_VID_POS:
      return (uae_u32)vidplay_pos_s();
    case NF_VID_LEN:
      return (uae_u32)vidplay_len_s();
    case NF_VID_META: {
      uae_u32 which = nf_get_param(params, 0);
      uaecptr buf = nf_get_param(params, 1);
      uae_u32 len = nf_get_param(params, 2);
      if (!buf || len == 0)
        return (uae_u32)-1;
      nf_write_string(buf, len, vidplay_meta((int)which));
      return 0;
    }
    case NF_VID_RECT:
      vidplay_set_rect((int)(int32_t)nf_get_param(params, 0),
                       (int)(int32_t)nf_get_param(params, 1),
                       (int)(int32_t)nf_get_param(params, 2),
                       (int)(int32_t)nf_get_param(params, 3));
      return 0;
    case NF_VID_VOLUME:
      vidplay_set_volume((int)(int32_t)nf_get_param(params, 0));
      return 0;
    case NF_VID_INFO:
      return (uae_u32)vidplay_info((int)nf_get_param(params, 0));
    case NF_VID_CLIP:
      vidplay_set_clip((int)(int32_t)nf_get_param(params, 0),
                       (int)(int32_t)nf_get_param(params, 1),
                       (int)(int32_t)nf_get_param(params, 2),
                       (int)(int32_t)nf_get_param(params, 3));
      return 0;
  }
  return (uae_u32)-1;
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
    case NF_FEATURE_MP3:
      return nf_call_mp3(subid, params);
    case NF_FEATURE_VIDEO:
      return nf_call_video(subid, params);
  }

  return 0;
}

/* INVARIANT (relied on by the JIT): this must behave as an ordinary
 * straight-line instruction. It may not modify the PC other than the +2
 * below, may not branch, and may not raise a 68k exception. build_comp()
 * declares 0x7300/0x7301 as cflow = fl_normal on the strength of that, which
 * lets a NatFeat call sit in the middle of a translated block instead of
 * terminating it. A future sub-handler that needs to change control flow
 * must revert that declaration to fl_trap. */
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
