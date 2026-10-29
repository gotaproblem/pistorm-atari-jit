/* disk_order.c - detect byte-swapped hard-disk images (see disk_order.h).
 *
 * Sector 0 is scored as it lies in the file and with every word swapped;
 * the clearly better reading wins:
 *   DOS MBR signature 55 AA at 510          +3
 *   DOS partition entries (with 55 AA)      +1 each (status 00/80, known type)
 *   AHDI root entries at $1C6/$1D2/$1DE/$1EA +2 each (flag bit 0 + GEM, BGM,
 *                                             XGM, RAW, LNX, MIX, BSD, UNX,
 *                                             F32, MAC, SWP, QWA)
 *   FAT BPB (bytes/sector 512..16384, sectors/cluster a power of 2) +1
 *   68000 BRA.S as the first word (60 xx, xx even, non-zero)        +1
 * A reading needs 3 or more and must beat the other; anything else is
 * "cannot tell" and the image is used as it is.
 *
 * Checked against the field images, 29 Oct 2026: the DOS-MBR images
 * (1GB, 1GBIDE, apple-boot-acsi, Satan256Mb*, freemint/mint-master,
 * mymint*, tuxies-Mint) native; freemint/EasyMiNT_*_Aranym swapped (AHDI,
 * readable only swapped); apple-boot-ide blank sector 0 - cannot tell. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "disk_order.h"

static int hdd_mode[8], acsi_mode[8];          /* DISK_ORDER_*, 0 = auto */

void disk_order_set_hdd  (int s, int m) { if (s >= 0 && s < 8) hdd_mode[s] = m; }
void disk_order_set_acsi (int s, int m) { if (s >= 0 && s < 8) acsi_mode[s] = m; }
int  disk_order_get_hdd  (int s) { return (s >= 0 && s < 8) ? hdd_mode[s] : 0; }
int  disk_order_get_acsi (int s) { return (s >= 0 && s < 8) ? acsi_mode[s] : 0; }

int disk_order_parse_mode (const char *s)
{
  while (*s == ' ' || *s == '\t') s++;
  if (!strncasecmp (s, "auto", 4))    return DISK_ORDER_AUTO;
  if (!strncasecmp (s, "native", 6))  return DISK_ORDER_NATIVE;
  if (!strncasecmp (s, "swapped", 7)) return DISK_ORDER_SWAPPED;
  if (!strncasecmp (s, "bootswap", 8)) return DISK_ORDER_BOOTSWAP;
  return -1;
}

const char *disk_order_mode_name (int m)
{
  return m == DISK_ORDER_NATIVE ? "native" : m == DISK_ORDER_SWAPPED ? "swapped" :
         m == DISK_ORDER_BOOTSWAP ? "bootswap" : "auto";
}

static int score (const uint8_t *s, char *why, size_t n)
{
  static const char *ids[] = { "GEM", "BGM", "XGM", "RAW", "LNX", "MIX", "BSD",
                               "UNX", "F32", "MAC", "SWP", "QWA" };
  int sc = 0, ahdi = 0, dos = 0;
  int sig = s[510] == 0x55 && s[511] == 0xAA;
  if (sig) sc += 3;

  for (int i = 0; i < 4; i++) {
    const uint8_t *e = s + 0x1C6 + 12 * i;
    if (e[0] & 1)
      for (unsigned k = 0; k < sizeof ids / sizeof ids[0]; k++)
        if (!memcmp (e + 1, ids[k], 3)) { ahdi++; break; }
  }
  sc += 2 * ahdi;

  if (sig)
    for (int i = 0; i < 4; i++) {
      const uint8_t *e = s + 0x1BE + 16 * i;
      static const uint8_t types[] = { 0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E, 0x82, 0x83, 0xA3 };
      if (e[0] == 0x00 || e[0] == 0x80)
        for (unsigned k = 0; k < sizeof types; k++)
          if (e[4] == types[k]) { dos++; break; }
    }
  sc += dos;

  unsigned bps = s[11] | (s[12] << 8), spc = s[13];
  int bpb = (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096 ||
             bps == 8192 || bps == 16384) && spc && !(spc & (spc - 1));
  if (bpb) sc += 1;

  int bra = s[0] == 0x60 && s[1] && !(s[1] & 1);
  if (bra) sc += 1;

  snprintf (why, n, "%s%s%s%s%s",
            sig ? "MBR 55AA " : "", ahdi ? "AHDI " : "", dos ? "DOS-parts " : "",
            bpb ? "BPB " : "", bra ? "BRA " : "");
  return sc;
}

int disk_order_detect (int fd, char *why, size_t why_len)
{
  uint8_t raw[512], sw[512];
  char wr[64], ws[64];

  if (pread (fd, raw, 512, 0) != 512) {
    snprintf (why, why_len, "sector 0 unreadable");
    return -1;
  }
  memcpy (sw, raw, 512);
  disk_swap512 (sw);

  int a = score (raw, wr, sizeof wr);
  int b = score (sw, ws, sizeof ws);

  if (b >= 3 && b > a) { snprintf (why, why_len, "%sreadable swapped", ws); return 1; }
  if (a >= 3 && a > b) {
    /* PP-prepared: table native, boot code word-swapped (1GBIDE.img:
     * 1E 60 / 79 48 = BRA.S / PEA swapped, "PT" and the table native) */
    int bra_raw = raw[0] == 0x60 && raw[1] && !(raw[1] & 1);
    int bra_sw  = sw[0]  == 0x60 && sw[1]  && !(sw[1]  & 1);
    if (bra_sw && !bra_raw) {
      snprintf (why, why_len, "%sboot code word-swapped (PP-prepared)", wr);
      return 2;
    }
    snprintf (why, why_len, "%sreadable as is", wr);
    return 0;
  }
  snprintf (why, why_len, "no MBR/AHDI/BPB either way round");
  return -1;
}

int disk_order_resolve (int fd, int mode, const char *tag, const char *path)
{
  char why[96];

  if (mode != DISK_ORDER_AUTO) {
    int r = mode == DISK_ORDER_SWAPPED ? 1 : mode == DISK_ORDER_BOOTSWAP ? 2 : 0;
    printf ("%s %s: %s (cfg)\n", tag, path,
            r == 1 ? "stored byte-swapped" : r == 2 ? "PP-prepared (boot code swapped)" : "stored native");
    return r;
  }
  int r = disk_order_detect (fd, why, sizeof why);
  printf ("%s %s: %s - %s\n", tag, path,
          r == 1 ? "stored byte-swapped, presented unswapped" :
          r == 2 ? "PP-prepared, presented in the twisted-cable order" :
          r == 0 ? "stored native" : "order unknown, used as is", why);
  return r < 0 ? 0 : r;
}
