/*
 * astos_fat.h - disk images as folders for ASTOS's file manager.
 *
 * Opens an Atari or PC disk image - a floppy (.ST, raw sectors with a BPB
 * in sector 0) or a hard disk with an AHDI / ICD / XGM root sector or a
 * DOS MBR - and gives each FAT12 / FAT16 / FAT32 partition in it as a
 * volume: list, read out, write in, make folders, rename, delete.
 *
 * Byte order is judged per volume (a word-swapped ARAnyM disk, a twisted-
 * cable dump); the file is always written back in the order it is stored
 * (doc/DISK-BYTEORDER.md). Names written are 8.3, upper case, as TOS
 * wants them; a name that does not fit gets a short name (LONGNA~1.TXT)
 * plus long-name entries, which MiNT and EmuTOS read and TOS ignores.
 * Long names are read.
 *
 * One image file is opened once: both panes of the file manager share it,
 * so their FAT copies can never disagree. Paths inside a volume are
 * "" (the root) or "A/B/C".
 */
#ifndef ASTOS_FAT_H
#define ASTOS_FAT_H

#include <stdint.h>

struct fat_img;

struct fat_ent {
    char     name[256];     /* the long name if there is one         */
    char     sname[13];     /* the 8.3 name, "README.TXT"            */
    int      dir;
    uint32_t size;
    uint8_t  attr;
    uint16_t date, time;    /* DOS format                            */
};

/* NULL and a reason in why if no FAT volume is found */
struct fat_img *fat_open(const char *path, char *why, int why_len);
void fat_close(struct fat_img *im);              /* by reference count */

int  fat_nvol(const struct fat_img *im);
/* "C: GEM 31M FAT16", "FAT12 720K", ... */
const char *fat_vol_label(const struct fat_img *im, int v);
int  fat_readonly(const struct fat_img *im);

/* the entries of a folder, '.' '..' and volume labels left out;
 * returns the count (up to max) or -1 */
int  fat_list(struct fat_img *im, int v, const char *dir, struct fat_ent *out, int max);
/* 0 and *e when it is there, -1 when not */
int  fat_stat(struct fat_img *im, int v, const char *path, struct fat_ent *e);

/* a file out of the image into host_path (made new, never overwritten) */
int  fat_get(struct fat_img *im, int v, const char *path, const char *host_path);
/* host_path into the folder dir as name; -1 if name is there already */
int  fat_put(struct fat_img *im, int v, const char *dir, const char *name,
             const char *host_path);
int  fat_mkdir(struct fat_img *im, int v, const char *dir, const char *name);
/* a file, or a folder with everything in it */
int  fat_delete(struct fat_img *im, int v, const char *path);
/* in the same folder */
int  fat_rename(struct fat_img *im, int v, const char *path, const char *new_name);
/* free bytes on the volume */
uint64_t fat_free(struct fat_img *im, int v);

/* the last error, for the message line */
const char *fat_error(void);

#endif
