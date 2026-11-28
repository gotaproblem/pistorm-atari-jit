/*
 * astos_fat.c - FAT12/16/32 volumes inside disk images (see astos_fat.h).
 *
 * Everything is read and written through io(), which undoes a word-swapped
 * file per volume. The FAT itself is read once into memory, changed there
 * and written back to every copy at the end of each operation (fat_sync);
 * file data and new directory clusters go to the image before the entry
 * that points at them, so an interrupted copy leaves at worst lost
 * clusters, never a broken file.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "astos_fat.h"

/* names and paths are bounded; a cut-short one fails its lookup */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define MAX_VOL 16

struct vol {
    uint64_t base;          /* byte offset of the partition in the file */
    uint64_t psize;         /* its size in bytes, 0 = to the end        */
    int      swapped;
    int      type;          /* 12 16 32                                 */
    uint32_t bps, spc, csize, rsv, nfats, spf, rootents, tot;
    uint32_t root_clus;     /* FAT32                                    */
    uint32_t fsinfo;        /* FAT32: the FSInfo sector, 0 = none       */
    uint64_t fat_off, root_off, data_off;   /* from base                */
    uint32_t nclus;         /* data clusters: 2 .. nclus+1              */
    uint8_t *fat;           /* the first copy, in memory                */
    int      fat_dirty;
    uint32_t hint;          /* where the next free cluster search starts */
    char     label[40];
};

struct fat_img {
    char     path[PATH_MAX];
    int      fd, ro, refs;
    int      nvol;
    struct vol v[MAX_VOL];
    struct fat_img *next;
};

static struct fat_img *open_list;
static char err[128];

const char *fat_error(void) { return err; }

static void ferr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, sizeof err, fmt, ap);
    va_end(ap);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static void swab_buf(uint8_t *b, size_t n)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint8_t t = b[i]; b[i] = b[i + 1]; b[i + 1] = t;
    }
}

/* raw file access, the word order undone; off and len are always even */
static int rd(int fd, int sw, uint64_t off, void *buf, size_t len)
{
    if (pread(fd, buf, len, (off_t)off) != (ssize_t)len) {
        ferr("read error at %llu", (unsigned long long)off);
        return -1;
    }
    if (sw)
        swab_buf(buf, len);
    return 0;
}

static int wr(struct fat_img *im, int sw, uint64_t off, const void *buf, size_t len)
{
    if (im->ro) {
        ferr("the image is read-only");
        return -1;
    }
    const void *src = buf;
    uint8_t *tmp = NULL;
    if (sw) {
        tmp = malloc(len);
        if (!tmp) { ferr("out of memory"); return -1; }
        memcpy(tmp, buf, len);
        swab_buf(tmp, len);
        src = tmp;
    }
    ssize_t n = pwrite(im->fd, src, len, (off_t)off);
    free(tmp);
    if (n != (ssize_t)len) {
        ferr("write error: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int vrd(struct fat_img *im, struct vol *v, uint64_t off, void *b, size_t n)
{
    return rd(im->fd, v->swapped, v->base + off, b, n);
}

static int vwr(struct fat_img *im, struct vol *v, uint64_t off, const void *b, size_t n)
{
    return wr(im, v->swapped, v->base + off, b, n);
}

/* ---- finding the volumes --------------------------------------------- */

static int pow2(uint32_t x) { return x && !(x & (x - 1)); }

/* the BPB at base, read with or without the swap; hint 12/16/32 or 0 */
static int try_bpb(struct fat_img *im, struct vol *v, uint64_t base, uint64_t psize,
                   int sw, int hint)
{
    uint8_t s[512];
    if (rd(im->fd, sw, base, s, 512) != 0)
        return -1;
    uint32_t bps = le16(s + 11), spc = s[13], rsv = le16(s + 14), nfats = s[16];
    uint32_t rootents = le16(s + 17), tot = le16(s + 19), spf = le16(s + 22);
    if (!tot) tot = le32(s + 32);
    int f32 = 0;
    if (!spf) { spf = le32(s + 36); f32 = 1; }
    if (bps < 512 || bps > 16384 || !pow2(bps) || !pow2(spc) || spc > 128 || !rsv ||
        !nfats || nfats > 4 || !spf || !tot || (!f32 && !rootents))
        return -1;
    uint64_t bytes = (uint64_t)tot * bps;
    if (psize && bytes > psize + 64 * 1024)          /* larger than its slot */
        return -1;
    uint32_t rootsec = (rootents * 32 + bps - 1) / bps;
    uint64_t meta = (uint64_t)rsv + (uint64_t)nfats * spf + rootsec;
    if (meta >= tot)
        return -1;
    memset(v, 0, sizeof *v);
    v->base = base; v->psize = psize; v->swapped = sw;
    v->bps = bps; v->spc = spc; v->csize = bps * spc; v->rsv = rsv;
    v->nfats = nfats; v->spf = spf; v->rootents = f32 ? 0 : rootents; v->tot = tot;
    v->fat_off = (uint64_t)rsv * bps;
    v->root_off = v->fat_off + (uint64_t)nfats * spf * bps;
    v->data_off = v->root_off + (uint64_t)rootsec * bps;
    v->nclus = (uint32_t)((tot - meta) / spc);
    if (f32) {
        v->type = 32;
        v->root_clus = le32(s + 44);
        v->fsinfo = le16(s + 48);
        if (v->root_clus < 2 || v->root_clus > v->nclus + 1)
            return -1;
    } else if (hint == 12 || hint == 16)
        v->type = hint;
    else
        v->type = v->nclus < 4085 ? 12 : 16;
    /* the FAT must hold every cluster */
    uint64_t need = v->type == 12 ? ((uint64_t)(v->nclus + 2) * 3 + 1) / 2
                                  : (uint64_t)(v->nclus + 2) * (v->type / 8);
    if ((uint64_t)spf * bps < need) {
        if (v->type == 32)
            return -1;
        /* fewer clusters than the size says: use what the FAT covers */
        v->nclus = (uint32_t)((uint64_t)spf * bps * 8 / (v->type == 12 ? 12 : 16)) - 2;
    }
    v->hint = 2;
    return 0;
}

static void size_txt(uint64_t b, char *o, size_t n)
{
    if (b >= 10ULL << 30)      snprintf(o, n, "%lluG", (unsigned long long)(b >> 30));
    else if (b >= 10ULL << 20) snprintf(o, n, "%lluM", (unsigned long long)(b >> 20));
    else                       snprintf(o, n, "%lluK", (unsigned long long)(b >> 10));
}

static void add_vol(struct fat_img *im, uint64_t sec, uint64_t nsec, int sw, int hint,
                    const char *kind)
{
    if (im->nvol >= MAX_VOL)
        return;
    struct vol *v = &im->v[im->nvol];
    uint64_t base = sec * 512, psize = nsec * 512;
    if (try_bpb(im, v, base, psize, sw, hint) != 0 &&
        try_bpb(im, v, base, psize, !sw, hint) != 0)
        return;
    char sz[16];
    size_txt((uint64_t)v->tot * v->bps, sz, sizeof sz);
    if (kind)
        snprintf(v->label, sizeof v->label, "%c: %s %s FAT%d%s", 'C' + im->nvol, kind, sz,
                 v->type, v->swapped ? " swapped" : "");
    else
        snprintf(v->label, sizeof v->label, "FAT%d %s%s", v->type, sz,
                 v->swapped ? " swapped" : "");
    im->nvol++;
}

static int ahdi_entry(const uint8_t *e, char id[4], uint32_t *st, uint32_t *sz)
{
    static const char *ids[] = { "GEM", "BGM", "XGM", "F32", "RAW", "LNX", "MIX",
                                 "BSD", "UNX", "MAC", "SWP", "QWA" };
    if (!(e[0] & 1))
        return 0;
    for (unsigned k = 0; k < sizeof ids / sizeof ids[0]; k++)
        if (!memcmp(e + 1, ids[k], 3)) {
            memcpy(id, e + 1, 3);
            id[3] = '\0';
            *st = be32(e + 4);
            *sz = be32(e + 8);
            return 1;
        }
    return 0;
}

static void ahdi_part(struct fat_img *im, const char *id, uint64_t st, uint64_t sz, int sw)
{
    if (!strcmp(id, "GEM") || !strcmp(id, "BGM"))
        add_vol(im, st, sz, sw, 16, id);
    else if (!strcmp(id, "F32"))
        add_vol(im, st, sz, sw, 32, id);
}

static int scan_ahdi(struct fat_img *im, const uint8_t *s, int sw)
{
    int found = 0;
    for (int pass = 0; pass < 2; pass++) {          /* AHDI 4, then ICD 8 */
        int n = pass ? 8 : 4, at = pass ? 0x156 : 0x1C6;
        int ok = 0;
        for (int i = 0; i < n; i++) {
            char id[4];
            uint32_t st, sz;
            const uint8_t *e = s + at + 12 * i;
            if (!ahdi_entry(e, id, &st, &sz))
                continue;
            ok++;
            if (!strcmp(id, "XGM")) {             /* the extended chain */
                uint64_t ext = st, cur = st;
                for (int hop = 0; hop < 64; hop++) {
                    uint8_t x[512];
                    char xid[4];
                    uint32_t xst, xsz;
                    if (rd(im->fd, sw, cur * 512, x, 512) != 0)
                        break;
                    if (ahdi_entry(x + 0x1C6, xid, &xst, &xsz))
                        ahdi_part(im, xid, cur + xst, xsz, sw);
                    if (!ahdi_entry(x + 0x1D2, xid, &xst, &xsz) || strcmp(xid, "XGM"))
                        break;
                    cur = ext + xst;
                }
            } else
                ahdi_part(im, id, st, sz, sw);
        }
        if (!pass && !ok)
            return 0;                             /* no AHDI: no ICD */
        found += ok;
    }
    return found;
}

static int dos_type(int t)
{
    switch (t) {
    case 0x01: return 12;
    case 0x04: case 0x06: case 0x0E: return 1;       /* by cluster count */
    case 0x0B: case 0x0C: return 32;
    }
    return 0;
}

static int scan_mbr(struct fat_img *im, const uint8_t *s, int sw)
{
    if (s[510] != 0x55 || s[511] != 0xAA)
        return 0;
    int found = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = s + 0x1BE + 16 * i;
        if (e[0] != 0x00 && e[0] != 0x80)
            return 0;                                  /* not a table */
        uint32_t st = le32(e + 8), sz = le32(e + 12);
        int t = e[4], h = dos_type(t);
        if (h) {
            found++;
            add_vol(im, st, sz, sw, h == 1 ? 0 : h, "DOS");
        } else if (t == 0x05 || t == 0x0F) {
            found++;
            uint64_t ext = st, cur = st;
            for (int hop = 0; hop < 64; hop++) {
                uint8_t x[512];
                if (rd(im->fd, sw, cur * 512, x, 512) != 0 || x[510] != 0x55 || x[511] != 0xAA)
                    break;
                const uint8_t *a = x + 0x1BE, *b = x + 0x1CE;
                int ah = dos_type(a[4]);
                if (ah)
                    add_vol(im, cur + le32(a + 8), le32(a + 12), sw, ah == 1 ? 0 : ah, "DOS");
                if (b[4] != 0x05 && b[4] != 0x0F)
                    break;
                cur = ext + le32(b + 8);
            }
        }
    }
    return found;
}

struct fat_img *fat_open(const char *path, char *why, int why_len)
{
    char real[PATH_MAX];
    if (!realpath(path, real)) {
        snprintf(why, (size_t)why_len, "%s", strerror(errno));
        return NULL;
    }
    for (struct fat_img *p = open_list; p; p = p->next)
        if (!strcmp(p->path, real)) {
            p->refs++;
            return p;
        }
    struct fat_img *im = calloc(1, sizeof *im);
    if (!im) {
        snprintf(why, (size_t)why_len, "out of memory");
        return NULL;
    }
    snprintf(im->path, sizeof im->path, "%s", real);
    im->fd = open(real, O_RDWR);
    if (im->fd < 0) {
        im->fd = open(real, O_RDONLY);
        im->ro = 1;
    }
    uint8_t s[512];
    if (im->fd < 0 || rd(im->fd, 0, 0, s, 512) != 0) {
        snprintf(why, (size_t)why_len, "cannot read it");
        if (im->fd >= 0) close(im->fd);
        free(im);
        return NULL;
    }
    uint8_t w[512];
    memcpy(w, s, 512);
    swab_buf(w, 512);
    /* a partition table either way round, else one volume at 0 */
    if (!scan_ahdi(im, s, 0) && !scan_ahdi(im, w, 1) &&
        !scan_mbr(im, s, 0) && !scan_mbr(im, w, 1)) {
        struct stat st;
        fstat(im->fd, &st);
        add_vol(im, 0, (uint64_t)st.st_size / 512, 0, 0, NULL);
    }
    if (!im->nvol) {
        snprintf(why, (size_t)why_len, "no FAT volume in it");
        close(im->fd);
        free(im);
        return NULL;
    }
    im->refs = 1;
    im->next = open_list;
    open_list = im;
    printf("[ASTOS] image %s: %d volume(s)%s\n", real, im->nvol, im->ro ? ", read-only" : "");
    for (int i = 0; i < im->nvol; i++)
        printf("[ASTOS]   %s at %llu, %u-byte sectors, %u clusters of %u\n", im->v[i].label,
               (unsigned long long)im->v[i].base, im->v[i].bps, im->v[i].nclus, im->v[i].csize);
    return im;
}

void fat_close(struct fat_img *im)
{
    if (!im || --im->refs > 0)
        return;
    for (struct fat_img **pp = &open_list; *pp; pp = &(*pp)->next)
        if (*pp == im) { *pp = im->next; break; }
    for (int i = 0; i < im->nvol; i++)
        free(im->v[i].fat);
    close(im->fd);
    free(im);
}

int fat_nvol(const struct fat_img *im) { return im->nvol; }
const char *fat_vol_label(const struct fat_img *im, int v) { return im->v[v].label; }
int fat_readonly(const struct fat_img *im) { return im->ro; }

/* ---- the FAT ----------------------------------------------------------- */

static int fat_load(struct fat_img *im, struct vol *v)
{
    if (v->fat)
        return 0;
    size_t n = (size_t)v->spf * v->bps;
    v->fat = malloc(n);
    if (!v->fat) { ferr("out of memory for the FAT"); return -1; }
    if (vrd(im, v, v->fat_off, v->fat, n) != 0) {
        free(v->fat);
        v->fat = NULL;
        return -1;
    }
    return 0;
}

static uint32_t fget(struct vol *v, uint32_t n)
{
    switch (v->type) {
    case 12: {
        uint32_t o = n + n / 2, x = le16(v->fat + o);
        return n & 1 ? x >> 4 : x & 0xFFF;
    }
    case 16: return le16(v->fat + 2 * n);
    default: return le32(v->fat + 4 * n) & 0x0FFFFFFF;
    }
}

static void fset(struct vol *v, uint32_t n, uint32_t x)
{
    switch (v->type) {
    case 12: {
        uint32_t o = n + n / 2;
        if (n & 1) {
            v->fat[o] = (uint8_t)((v->fat[o] & 0x0F) | (x << 4));
            v->fat[o + 1] = (uint8_t)(x >> 4);
        } else {
            v->fat[o] = (uint8_t)x;
            v->fat[o + 1] = (uint8_t)((v->fat[o + 1] & 0xF0) | ((x >> 8) & 0x0F));
        }
        break;
    }
    case 16: put16(v->fat + 2 * n, x); break;
    default: put32(v->fat + 4 * n, (le32(v->fat + 4 * n) & 0xF0000000) | (x & 0x0FFFFFFF));
    }
    v->fat_dirty = 1;
}

static uint32_t eoc(const struct vol *v) { return v->type == 12 ? 0xFFF : v->type == 16 ? 0xFFFF : 0x0FFFFFFF; }
static int is_eoc(const struct vol *v, uint32_t c) { return c >= (eoc(v) & ~7u); }
static int valid_clus(const struct vol *v, uint32_t c) { return c >= 2 && c <= v->nclus + 1; }

static int fat_sync(struct fat_img *im, struct vol *v)
{
    if (!v->fat_dirty)
        return 0;
    size_t n = (size_t)v->spf * v->bps;
    for (uint32_t i = 0; i < v->nfats; i++)
        if (vwr(im, v, v->fat_off + (uint64_t)i * n, v->fat, n) != 0)
            return -1;
    if (v->type == 32 && v->fsinfo && v->fsinfo < v->rsv) {
        /* FSInfo: the free count and where to look next, kept true */
        uint64_t at = (uint64_t)v->fsinfo * v->bps;
        uint8_t sig[4], sig2[4], ff[8];
        if (vrd(im, v, at, sig, 4) == 0 && vrd(im, v, at + 484, sig2, 4) == 0 &&
            le32(sig) == 0x41615252 && le32(sig2) == 0x61417272) {
            uint32_t nfree = 0;
            for (uint32_t c = 2; c <= v->nclus + 1; c++)
                if (!fget(v, c))
                    nfree++;
            put32(ff, nfree);
            put32(ff + 4, v->hint);
            vwr(im, v, at + 488, ff, 8);
        }
    }
    v->fat_dirty = 0;
    fsync(im->fd);
    return 0;
}

static uint64_t clus_off(const struct vol *v, uint32_t c)
{
    return v->data_off + (uint64_t)(c - 2) * v->csize;
}

/* a free cluster, marked end-of-chain and linked after prev (0: none) */
static uint32_t alloc_clus(struct vol *v, uint32_t prev)
{
    uint32_t n = v->nclus;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = 2 + (v->hint - 2 + i) % n;
        if (fget(v, c) == 0) {
            fset(v, c, eoc(v));
            if (prev)
                fset(v, prev, c);
            v->hint = c + 1 > n + 1 ? 2 : c + 1;
            return c;
        }
    }
    ferr("the volume is full");
    return 0;
}

static void free_chain(struct vol *v, uint32_t c)
{
    for (uint32_t k = 0; valid_clus(v, c) && k <= v->nclus; k++) {
        uint32_t nx = fget(v, c);
        fset(v, c, 0);
        if (is_eoc(v, nx) || !valid_clus(v, nx))
            break;
        c = nx;
    }
}

uint64_t fat_free(struct fat_img *im, int vi)
{
    struct vol *v = &im->v[vi];
    if (fat_load(im, v) != 0)
        return 0;
    uint64_t n = 0;
    for (uint32_t c = 2; c <= v->nclus + 1; c++)
        if (!fget(v, c))
            n++;
    return n * v->csize;
}

/* ---- directories -------------------------------------------------------- */

struct dirbuf {
    uint8_t  *b;
    uint64_t *off;          /* where each 32-byte slot is, from base */
    int       n;
    uint32_t  clus;         /* 0: the fixed root                     */
    uint32_t  last;         /* the chain's last cluster              */
};

static void db_free(struct dirbuf *d)
{
    free(d->b);
    free(d->off);
    memset(d, 0, sizeof *d);
}

static uint32_t root_of(const struct vol *v) { return v->type == 32 ? v->root_clus : 0; }

static int db_load(struct fat_img *im, struct vol *v, uint32_t clus, struct dirbuf *d)
{
    memset(d, 0, sizeof *d);
    d->clus = clus;
    if (!clus) {
        d->n = (int)v->rootents;
        d->b = malloc((size_t)d->n * 32);
        d->off = malloc((size_t)d->n * sizeof *d->off);
        if (!d->b || !d->off || vrd(im, v, v->root_off, d->b, (size_t)d->n * 32) != 0) {
            db_free(d);
            return -1;
        }
        for (int i = 0; i < d->n; i++)
            d->off[i] = v->root_off + (uint64_t)i * 32;
        return 0;
    }
    int per = (int)(v->csize / 32);
    for (uint32_t c = clus, k = 0; valid_clus(v, c) && k <= v->nclus; k++) {
        uint8_t *nb = realloc(d->b, (size_t)(d->n + per) * 32);
        uint64_t *no = realloc(d->off, (size_t)(d->n + per) * sizeof *no);
        if (!nb || !no) { free(nb ? nb : d->b); free(no ? no : d->off); d->b = NULL; d->off = NULL; return -1; }
        d->b = nb; d->off = no;
        if (vrd(im, v, clus_off(v, c), d->b + (size_t)d->n * 32, v->csize) != 0) {
            db_free(d);
            return -1;
        }
        for (int i = 0; i < per; i++)
            d->off[d->n + i] = clus_off(v, c) + (uint64_t)i * 32;
        d->n += per;
        d->last = c;
        uint32_t nx = fget(v, c);
        if (is_eoc(v, nx) || !valid_clus(v, nx))
            break;
        c = nx;
    }
    return 0;
}

static uint8_t lfn_sum(const uint8_t *n11)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1) << 7) + (s >> 1) + n11[i]);
    return s;
}

static void short_name(const uint8_t *e, char *out)
{
    int k = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++)
        out[k++] = (char)(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]);
    if (e[8] != ' ') {
        out[k++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++)
            out[k++] = (char)e[i];
    }
    out[k] = '\0';
}

/* one entry found while walking a directory */
struct hit {
    struct fat_ent e;
    int slot, first;        /* the short entry, the first long-name slot */
    uint32_t clus;
};

/* walk the entries; cb returns nonzero to stop */
static int db_walk(const struct vol *v, const struct dirbuf *d,
                   int (*cb)(const struct hit *, void *), void *ctx)
{
    char lfn[264];
    int lfn_n = 0, lfn_first = -1;
    uint8_t lfn_ck = 0;
    for (int i = 0; i < d->n; i++) {
        const uint8_t *e = d->b + (size_t)i * 32;
        if (e[0] == 0x00)
            break;
        if (e[0] == 0xE5) { lfn_n = 0; continue; }
        if (e[11] == 0x0F) {                        /* a long-name part */
            int ord = e[0] & 0x1F;
            if (e[0] & 0x40) {
                memset(lfn, 0, sizeof lfn);
                lfn_n = ord;
                lfn_first = i;
                lfn_ck = e[13];
            }
            if (!lfn_n || ord < 1 || ord > 20 || e[13] != lfn_ck) { lfn_n = 0; continue; }
            static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int j = 0; j < 13; j++) {
                int at = (ord - 1) * 13 + j;
                uint16_t ch = le16(e + pos[j]);
                if (at < 260)
                    lfn[at] = ch == 0 || ch == 0xFFFF ? '\0' : ch < 0x80 ? (char)ch : '_';
            }
            continue;
        }
        if (e[11] & 0x08) { lfn_n = 0; continue; }  /* the volume label */
        struct hit h;
        memset(&h, 0, sizeof h);
        short_name(e, h.e.sname);
        if (!strcmp(h.e.sname, ".") || !strcmp(h.e.sname, "..")) { lfn_n = 0; continue; }
        int has_lfn = lfn_n && lfn_sum(e) == lfn_ck && lfn[0];
        snprintf(h.e.name, sizeof h.e.name, "%s", has_lfn ? lfn : h.e.sname);
        h.e.dir = (e[11] & 0x10) != 0;
        h.e.attr = e[11];
        h.e.size = le32(e + 28);
        h.e.time = le16(e + 22);
        h.e.date = le16(e + 24);
        h.clus = le16(e + 26) | (v->type == 32 ? (uint32_t)le16(e + 20) << 16 : 0);
        h.slot = i;
        h.first = has_lfn ? lfn_first : i;
        lfn_n = 0;
        if (cb(&h, ctx))
            return 1;
    }
    return 0;
}

struct find_ctx { const char *name; struct hit h; int found; };

static int find_cb(const struct hit *h, void *p)
{
    struct find_ctx *f = p;
    if (!strcasecmp(h->e.name, f->name) || !strcasecmp(h->e.sname, f->name)) {
        f->h = *h;
        f->found = 1;
        return 1;
    }
    return 0;
}

static int dir_find(const struct vol *v, const struct dirbuf *d, const char *name, struct hit *out)
{
    struct find_ctx f = { name, { { "", "", 0, 0, 0, 0, 0 }, 0, 0, 0 }, 0 };
    db_walk(v, d, find_cb, &f);
    if (f.found && out)
        *out = f.h;
    return f.found ? 0 : -1;
}

/* the cluster of the folder at path ("" = root); -1 if not a folder */
static int resolve_dir(struct fat_img *im, struct vol *v, const char *path, uint32_t *clus)
{
    uint32_t c = root_of(v);
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path ? path : "");
    for (char *t = strtok(buf, "/"); t; t = strtok(NULL, "/")) {
        struct dirbuf d;
        if (db_load(im, v, c, &d) != 0)
            return -1;
        struct hit h;
        int r = dir_find(v, &d, t, &h);
        db_free(&d);
        if (r != 0 || !h.e.dir) {
            ferr("no folder %.60s", t);
            return -1;
        }
        c = h.clus ? h.clus : root_of(v);
    }
    *clus = c;
    return 0;
}

/* the folder and the last name of a path */
static int split(struct fat_img *im, struct vol *v, const char *path, uint32_t *dclus,
                 char *name, size_t n)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    char *s = strrchr(buf, '/');
    if (s) {
        *s = '\0';
        snprintf(name, n, "%s", s + 1);
        return resolve_dir(im, v, buf, dclus);
    }
    snprintf(name, n, "%s", buf);
    return resolve_dir(im, v, "", dclus);
}

struct list_ctx { struct fat_ent *out; int max, n; };

static int list_cb(const struct hit *h, void *p)
{
    struct list_ctx *l = p;
    if (l->n < l->max)
        l->out[l->n++] = h->e;
    return 0;
}

int fat_list(struct fat_img *im, int vi, const char *dir, struct fat_ent *out, int max)
{
    struct vol *v = &im->v[vi];
    uint32_t c;
    struct dirbuf d;
    if (fat_load(im, v) != 0 || resolve_dir(im, v, dir, &c) != 0 || db_load(im, v, c, &d) != 0)
        return -1;
    struct list_ctx l = { out, max, 0 };
    db_walk(v, &d, list_cb, &l);
    db_free(&d);
    return l.n;
}

static int lookup(struct fat_img *im, struct vol *v, const char *path, uint32_t *dclus,
                  struct dirbuf *d, struct hit *h)
{
    char name[256];
    if (fat_load(im, v) != 0 || split(im, v, path, dclus, name, sizeof name) != 0 ||
        db_load(im, v, *dclus, d) != 0)
        return -1;
    if (dir_find(v, d, name, h) != 0) {
        ferr("%.60s is not there", name);
        db_free(d);
        return -1;
    }
    return 0;
}

int fat_stat(struct fat_img *im, int vi, const char *path, struct fat_ent *e)
{
    struct vol *v = &im->v[vi];
    uint32_t dc;
    struct dirbuf d;
    struct hit h;
    if (lookup(im, v, path, &dc, &d, &h) != 0)
        return -1;
    db_free(&d);
    if (e)
        *e = h.e;
    return 0;
}

/* ---- reading a file out -------------------------------------------------- */

int fat_get(struct fat_img *im, int vi, const char *path, const char *host_path)
{
    struct vol *v = &im->v[vi];
    uint32_t dc;
    struct dirbuf d;
    struct hit h;
    if (lookup(im, v, path, &dc, &d, &h) != 0)
        return -1;
    db_free(&d);
    if (h.e.dir) { ferr("%.60s is a folder", h.e.name); return -1; }
    int out = open(host_path, O_WRONLY | O_CREAT | O_EXCL, 0664);
    if (out < 0) { ferr("cannot make %.60s: %s", host_path, strerror(errno)); return -1; }
    uint8_t *buf = malloc(v->csize);
    uint32_t left = h.e.size, c = h.clus;
    int bad = !buf;
    for (uint32_t k = 0; left && !bad && k <= v->nclus; k++) {
        if (!valid_clus(v, c)) { ferr("broken cluster chain in %.60s", h.e.name); bad = 1; break; }
        uint32_t n = left < v->csize ? left : v->csize;
        /* read whole words: a swapped image needs even lengths */
        if (vrd(im, v, clus_off(v, c), buf, (n + 1) & ~1u) != 0 || write(out, buf, n) != (ssize_t)n) {
            bad = 1;
            break;
        }
        left -= n;
        c = fget(v, c);
    }
    free(buf);
    if (close(out) != 0) bad = 1;
    if (bad || left) {
        if (!bad) ferr("%.60s is shorter than its size", h.e.name);
        unlink(host_path);
        return -1;
    }
    /* the file's own date */
    struct tm tm = { 0 };
    tm.tm_year = ((h.e.date >> 9) & 0x7F) + 80;
    tm.tm_mon = ((h.e.date >> 5) & 0x0F) - 1;
    tm.tm_mday = h.e.date & 0x1F;
    tm.tm_hour = h.e.time >> 11;
    tm.tm_min = (h.e.time >> 5) & 0x3F;
    tm.tm_sec = (h.e.time & 0x1F) * 2;
    tm.tm_isdst = -1;
    if (tm.tm_mday) {
        struct timespec ts[2];
        ts[0].tv_sec = ts[1].tv_sec = mktime(&tm);
        ts[0].tv_nsec = ts[1].tv_nsec = 0;
        utimensat(AT_FDCWD, host_path, ts, 0);
    }
    return 0;
}

/* ---- names ------------------------------------------------------------- */

static int ok83(int c)
{
    return isalnum(c) || (c && strchr("!#$%&'()-@^_`{}~", c)) || c >= 0x80;
}

/* name as an exact 8.3 (upper-cased): 1, else 0 */
static int as83(const char *name, uint8_t n11[11])
{
    const char *dot = strrchr(name, '.');
    size_t bl = dot ? (size_t)(dot - name) : strlen(name);
    size_t el = dot ? strlen(dot + 1) : 0;
    if (!bl || bl > 8 || el > 3 || (dot && !el))
        return 0;
    memset(n11, ' ', 11);
    for (size_t i = 0; i < bl; i++) {
        if (!ok83((unsigned char)name[i])) return 0;
        n11[i] = (uint8_t)toupper((unsigned char)name[i]);
    }
    for (size_t i = 0; i < el; i++) {
        if (!ok83((unsigned char)dot[1 + i])) return 0;
        n11[8 + i] = (uint8_t)toupper((unsigned char)dot[1 + i]);
    }
    if (n11[0] == 0xE5) n11[0] = 0x05;
    return 1;
}

struct short_ctx { const uint8_t *n11; int used; };

static int short_cb(const struct hit *h, void *p)
{
    struct short_ctx *s = p;
    char mine[13];
    short_name(s->n11, mine);
    if (!strcasecmp(mine, h->e.sname)) { s->used = 1; return 1; }
    return 0;
}

/* LONGNA~1.TXT, the first number not in the folder */
static int mangle(const struct vol *v, const struct dirbuf *d, const char *name, uint8_t n11[11])
{
    char base[9] = "", ext[4] = "";
    const char *dot = strrchr(name, '.');
    int k = 0;
    for (const char *p = name; *p && p != dot && k < 8; p++)
        if (ok83((unsigned char)*p))
            base[k++] = (char)toupper((unsigned char)*p);
    base[k] = '\0';
    if (!k) { strcpy(base, "FILE"); k = 4; }
    k = 0;
    for (const char *p = dot ? dot + 1 : ""; *p && k < 3; p++)
        if (ok83((unsigned char)*p))
            ext[k++] = (char)toupper((unsigned char)*p);
    ext[k] = '\0';
    for (int n = 1; n < 1000000; n++) {
        char tail[9];
        int tl = snprintf(tail, sizeof tail, "~%d", n);
        int bl = (int)strlen(base);
        if (bl > 8 - tl) bl = 8 - tl;
        memset(n11, ' ', 11);
        memcpy(n11, base, (size_t)bl);
        memcpy(n11 + bl, tail, (size_t)tl);
        memcpy(n11 + 8, ext, strlen(ext));
        struct short_ctx s = { n11, 0 };
        db_walk(v, d, short_cb, &s);
        if (!s.used)
            return 0;
    }
    ferr("no short name left for %.40s", name);
    return -1;
}

/* the entries for name: long-name slots then the short entry, in ent[] */
static int build_entries(const struct vol *v, const struct dirbuf *d, const char *name,
                         const uint8_t *proto, uint8_t ent[][32])
{
    uint8_t n11[11];
    int nl = 0;
    if (!as83(name, n11)) {
        if (mangle(v, d, name, n11) != 0)
            return -1;
        size_t len = strlen(name);
        if (len > 255) { ferr("name too long"); return -1; }
        nl = (int)((len + 12) / 13);
        uint8_t ck = lfn_sum(n11);
        static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        for (int i = 0; i < nl; i++) {
            int ord = nl - i;                      /* stored last part first */
            uint8_t *e = ent[i];
            memset(e, 0, 32);
            e[0] = (uint8_t)(ord | (i == 0 ? 0x40 : 0));
            e[11] = 0x0F;
            e[13] = ck;
            for (int j = 0; j < 13; j++) {
                size_t at = (size_t)(ord - 1) * 13 + (size_t)j;
                uint16_t ch = at < len ? (unsigned char)name[at] : at == len ? 0 : 0xFFFF;
                put16(e + pos[j], ch);
            }
        }
    }
    uint8_t *e = ent[nl];
    if (proto)
        memcpy(e, proto, 32);
    else
        memset(e, 0, 32);
    memcpy(e, n11, 11);
    return nl + 1;
}

static void dos_time(time_t t, uint16_t *date, uint16_t *tim)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year < 80) { tm.tm_year = 80; tm.tm_mon = 0; tm.tm_mday = 1; }
    *date = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    *tim = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
}

/* n free slots in a row in the folder, growing it when it can */
static int find_slots(struct fat_img *im, struct vol *v, struct dirbuf *d, int n)
{
    for (int tries = 0; tries < 2; tries++) {
        int run = 0;
        for (int i = 0; i < d->n; i++) {
            uint8_t b0 = d->b[(size_t)i * 32];
            run = b0 == 0x00 || b0 == 0xE5 ? run + 1 : 0;
            if (run == n)
                return i - n + 1;
        }
        if (!d->clus) {
            ferr("the root folder is full");
            return -1;
        }
        /* one more cluster for the folder, zeroed before it is linked */
        uint32_t c = alloc_clus(v, 0);
        if (!c)
            return -1;
        uint8_t *z = calloc(1, v->csize);
        if (!z || vwr(im, v, clus_off(v, c), z, v->csize) != 0) {
            free(z);
            fset(v, c, 0);
            return -1;
        }
        free(z);
        fset(v, d->last, c);
        uint32_t clus = d->clus;
        db_free(d);
        if (db_load(im, v, clus, d) != 0)
            return -1;
    }
    ferr("no room in the folder");
    return -1;
}

static int write_entries(struct fat_img *im, struct vol *v, struct dirbuf *d, int at,
                         uint8_t ent[][32], int n)
{
    for (int i = 0; i < n; i++) {
        if (vwr(im, v, d->off[at + i], ent[i], 32) != 0)
            return -1;
        memcpy(d->b + (size_t)(at + i) * 32, ent[i], 32);
    }
    return 0;
}

static int erase_entries(struct fat_img *im, struct vol *v, const uint64_t *off, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t e[2];
        if (vrd(im, v, off[i], e, 2) != 0)
            return -1;
        e[0] = 0xE5;
        if (vwr(im, v, off[i], e, 2) != 0)
            return -1;
    }
    return 0;
}

static int name_ok(const char *name)
{
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/') ||
        strchr(name, '\\') || strlen(name) > 255) {
        ferr("not a usable name");
        return 0;
    }
    for (const char *p = name; *p; p++)
        if (strchr("\"*:<>?|", *p) || (unsigned char)*p < 0x20) {
            ferr("'%c' cannot be in a name", *p);
            return 0;
        }
    return 1;
}

/* the new entry in folder dclus; the folder is loaded in d */
static int add_entry(struct fat_img *im, struct vol *v, struct dirbuf *d, const char *name,
                     const uint8_t *proto)
{
    uint8_t ent[22][32];
    if (dir_find(v, d, name, NULL) == 0) {
        ferr("%.60s is there already", name);
        return -1;
    }
    int n = build_entries(v, d, name, proto, ent);
    if (n < 0)
        return -1;
    int at = find_slots(im, v, d, n);
    if (at < 0)
        return -1;
    return write_entries(im, v, d, at, ent, n);
}

static void short_entry(uint8_t *e, uint8_t attr, uint32_t clus, uint32_t size, time_t t,
                        int type)
{
    memset(e, 0, 32);
    memset(e, ' ', 11);
    e[11] = attr;
    uint16_t dd, tt;
    dos_time(t, &dd, &tt);
    put16(e + 14, tt); put16(e + 16, dd); put16(e + 18, dd);   /* created, accessed */
    put16(e + 22, tt); put16(e + 24, dd);
    put16(e + 26, clus & 0xFFFF);
    if (type == 32)
        put16(e + 20, clus >> 16);
    put32(e + 28, size);
}

/* ---- writing ------------------------------------------------------------ */

int fat_put(struct fat_img *im, int vi, const char *dir, const char *name, const char *host_path)
{
    struct vol *v = &im->v[vi];
    if (im->ro) { ferr("the image is read-only"); return -1; }
    if (!name_ok(name))
        return -1;
    uint32_t dc;
    struct dirbuf d;
    if (fat_load(im, v) != 0 || resolve_dir(im, v, dir, &dc) != 0 || db_load(im, v, dc, &d) != 0)
        return -1;
    if (dir_find(v, &d, name, NULL) == 0) {
        ferr("%.60s is there already", name);
        db_free(&d);
        return -1;
    }
    int in = open(host_path, O_RDONLY);
    struct stat st;
    if (in < 0 || fstat(in, &st) != 0) {
        ferr("cannot read %.60s", host_path);
        if (in >= 0) close(in);
        db_free(&d);
        return -1;
    }
    if (st.st_size > 0xFFFFFFFFLL) {
        ferr("over 4 GB: FAT cannot hold it");
        close(in);
        db_free(&d);
        return -1;
    }
    if ((uint64_t)st.st_size > fat_free(im, vi)) {
        ferr("not enough room: %lld bytes, %llu free", (long long)st.st_size,
             (unsigned long long)fat_free(im, vi));
        close(in);
        db_free(&d);
        return -1;
    }
    uint8_t *buf = malloc(v->csize);
    uint32_t first = 0, prev = 0;
    uint64_t done = 0;
    int bad = !buf;
    while (!bad && done < (uint64_t)st.st_size) {
        size_t want = v->csize;
        ssize_t got = read(in, buf, want);
        if (got <= 0) { ferr("read error on %.60s", host_path); bad = 1; break; }
        if ((size_t)got < want)
            memset(buf + got, 0, want - (size_t)got);
        uint32_t c = alloc_clus(v, prev);
        if (!c) { bad = 1; break; }
        if (!first) first = c;
        if (vwr(im, v, clus_off(v, c), buf, v->csize) != 0) { bad = 1; break; }
        prev = c;
        done += (uint64_t)got;
    }
    free(buf);
    close(in);
    uint8_t e[32];
    short_entry(e, 0x20, first, (uint32_t)st.st_size, st.st_mtime, v->type);
    if (bad || add_entry(im, v, &d, name, e) != 0) {
        if (first) free_chain(v, first);
        fat_sync(im, v);
        db_free(&d);
        return -1;
    }
    db_free(&d);
    return fat_sync(im, v);
}

int fat_mkdir(struct fat_img *im, int vi, const char *dir, const char *name)
{
    struct vol *v = &im->v[vi];
    if (im->ro) { ferr("the image is read-only"); return -1; }
    if (!name_ok(name))
        return -1;
    uint32_t dc;
    struct dirbuf d;
    if (fat_load(im, v) != 0 || resolve_dir(im, v, dir, &dc) != 0 || db_load(im, v, dc, &d) != 0)
        return -1;
    if (dir_find(v, &d, name, NULL) == 0) {
        ferr("%.60s is there already", name);
        db_free(&d);
        return -1;
    }
    uint32_t c = alloc_clus(v, 0);
    if (!c) { db_free(&d); return -1; }
    uint8_t *z = calloc(1, v->csize);
    time_t now = time(NULL);
    int bad = !z;
    if (!bad) {
        short_entry(z, 0x10, c, 0, now, v->type);
        memcpy(z, ".          ", 11);
        uint32_t parent = dc == root_of(v) ? 0 : dc;      /* the root is 0 */
        short_entry(z + 32, 0x10, parent, 0, now, v->type);
        memcpy(z + 32, "..         ", 11);
        bad = vwr(im, v, clus_off(v, c), z, v->csize) != 0;
    }
    free(z);
    uint8_t e[32];
    short_entry(e, 0x10, c, 0, now, v->type);
    if (bad || add_entry(im, v, &d, name, e) != 0) {
        fset(v, c, 0);
        fat_sync(im, v);
        db_free(&d);
        return -1;
    }
    db_free(&d);
    return fat_sync(im, v);
}

static int delete_in(struct fat_img *im, struct vol *v, const char *path, int depth)
{
    uint32_t dc;
    struct dirbuf d;
    struct hit h;
    if (depth > 32) { ferr("folders too deep"); return -1; }
    if (lookup(im, v, path, &dc, &d, &h) != 0)
        return -1;
    if (h.e.dir && h.clus) {                       /* everything in it first */
        db_free(&d);
        struct fat_ent *kids = malloc(64 * sizeof *kids);
        if (!kids) { ferr("out of memory"); return -1; }
        for (;;) {
            struct dirbuf sub;
            if (db_load(im, v, h.clus, &sub) != 0) { free(kids); return -1; }
            struct list_ctx l = { kids, 64, 0 };
            db_walk(v, &sub, list_cb, &l);
            db_free(&sub);
            if (!l.n)
                break;
            for (int i = 0; i < l.n; i++) {
                char p[1024];
                if (snprintf(p, sizeof p, "%s/%s", path, kids[i].name) >= (int)sizeof p ||
                    delete_in(im, v, p, depth + 1) != 0) {
                    free(kids);
                    return -1;
                }
            }
        }
        free(kids);
        if (lookup(im, v, path, &dc, &d, &h) != 0)   /* its entry again */
            return -1;
    }
    uint64_t offs[22];
    int n = h.slot - h.first + 1;
    if (n > 22) n = 22;
    for (int i = 0; i < n; i++)
        offs[i] = d.off[h.slot - n + 1 + i];
    db_free(&d);
    if (erase_entries(im, v, offs, n) != 0)
        return -1;
    if (h.clus)
        free_chain(v, h.clus);
    return 0;
}

int fat_delete(struct fat_img *im, int vi, const char *path)
{
    struct vol *v = &im->v[vi];
    if (im->ro) { ferr("the image is read-only"); return -1; }
    if (fat_load(im, v) != 0)
        return -1;
    int r = delete_in(im, v, path, 0);
    if (fat_sync(im, v) != 0)
        return -1;
    return r;
}

int fat_rename(struct fat_img *im, int vi, const char *path, const char *new_name)
{
    struct vol *v = &im->v[vi];
    if (im->ro) { ferr("the image is read-only"); return -1; }
    if (!name_ok(new_name))
        return -1;
    uint32_t dc;
    struct dirbuf d;
    struct hit h;
    if (lookup(im, v, path, &dc, &d, &h) != 0)
        return -1;
    struct hit other;
    if (dir_find(v, &d, new_name, &other) == 0 && other.slot != h.slot) {
        ferr("%.60s is there already", new_name);
        db_free(&d);
        return -1;
    }
    uint8_t proto[32];
    memcpy(proto, d.b + (size_t)h.slot * 32, 32);
    uint64_t offs[22];
    int n = h.slot - h.first + 1;
    if (n > 22) n = 22;
    for (int i = 0; i < n; i++)
        offs[i] = d.off[h.slot - n + 1 + i];
    /* the old entries out of the way of the name check, then the new */
    for (int i = 0; i < n; i++)
        d.b[(size_t)(h.slot - n + 1 + i) * 32] = 0xE5;
    uint8_t ent[22][32];
    int ne = build_entries(v, &d, new_name, proto, ent);
    int at = ne < 0 ? -1 : find_slots(im, v, &d, ne);
    /* new first, old out after - unless the new run reuses the old slots */
    int bad = at < 0;
    if (!bad) {
        int lo = h.slot - n + 1;
        int overlap = at <= h.slot && at + ne > lo && d.n > h.slot && d.off[lo] == offs[0];
        if (overlap)
            bad = erase_entries(im, v, offs, n) != 0 || write_entries(im, v, &d, at, ent, ne) != 0;
        else
            bad = write_entries(im, v, &d, at, ent, ne) != 0 || erase_entries(im, v, offs, n) != 0;
    }
    db_free(&d);
    if (fat_sync(im, v) != 0)
        return -1;
    return bad ? -1 : 0;
}

/* ---- new, empty images --------------------------------------------------- */

static const struct { const char *label; int floppy; uint32_t kb; } kinds[] = {
    { "Floppy 360K  (single sided)", 1, 360 },
    { "Floppy 720K  (double sided)", 1, 720 },
    { "Floppy 1.44M (high density)", 1, 1440 },
    { "Hard disk 32M  (1 partition)", 0, 32 * 1024 },
    { "Hard disk 128M (1 partition)", 0, 128 * 1024 },
    { "Hard disk 256M (2 partitions)", 0, 256 * 1024 },
    { "Hard disk 512M (4 partitions)", 0, 512 * 1024 },
    { "Hard disk 1G   (4 partitions)", 0, 1024 * 1024 },
};

int fat_image_kinds(void) { return (int)(sizeof kinds / sizeof kinds[0]); }
const char *fat_image_kind(int k) { return kinds[k].label; }
int fat_image_floppy(int k) { return kinds[k].floppy; }
uint64_t fat_image_bytes(int k) { return (uint64_t)kinds[k].kb * 1024; }

/* a FAT12/16 boot sector + empty FATs + empty root at byte off */
static int format_vol(int fd, uint64_t off, uint32_t bps, uint32_t spc, uint32_t tot,
                      uint32_t rootents, uint32_t spf, uint8_t media, uint32_t spt,
                      uint32_t heads, int fat16)
{
    uint8_t *s = calloc(1, bps);
    if (!s) return -1;
    s[0] = 0xE9;                     /* not a 68000 BRA: never executed */
    memcpy(s + 2, "ASTOS ", 6);
    uint32_t serial = (uint32_t)time(NULL) ^ (uint32_t)getpid() ^ (uint32_t)off;
    s[8] = (uint8_t)serial; s[9] = (uint8_t)(serial >> 8); s[10] = (uint8_t)(serial >> 16);
    put16(s + 11, bps);
    s[13] = (uint8_t)spc;
    put16(s + 14, 1);                /* reserved: the boot sector */
    s[16] = 2;
    put16(s + 17, rootents);
    put16(s + 19, tot);
    s[21] = media;
    put16(s + 22, spf);
    put16(s + 24, spt);
    put16(s + 26, heads);
    /* the Atari boot checksum (big-endian words over 512 bytes) must not
     * be $1234, or TOS would run it */
    uint16_t sum = 0;
    for (int i = 0; i < 512; i += 2)
        sum = (uint16_t)(sum + (s[i] << 8 | s[i + 1]));
    if (sum == 0x1234)
        s[10] ^= 1;
    int bad = pwrite(fd, s, bps, (off_t)off) != (ssize_t)bps;
    /* both FATs: media byte, then end-of-chain for cluster 1 */
    memset(s, 0, bps);
    s[0] = media; s[1] = 0xFF; s[2] = 0xFF;
    if (fat16) s[3] = 0xFF;
    for (int f = 0; f < 2 && !bad; f++)
        bad = pwrite(fd, s, bps, (off_t)(off + (uint64_t)bps * (1 + (uint64_t)f * spf))) != (ssize_t)bps;
    free(s);
    return bad ? -1 : 0;
}

/* the smallest FAT that holds every cluster */
static uint32_t fat_size(uint32_t bps, uint32_t spc, uint32_t tot, uint32_t rootsec, int fat16)
{
    uint32_t spf = 1;
    for (;;) {
        uint32_t clus = (tot - 1 - 2 * spf - rootsec) / spc;
        uint64_t need = fat16 ? (uint64_t)(clus + 2) * 2 : ((uint64_t)(clus + 2) * 3 + 1) / 2;
        if ((uint64_t)spf * bps >= need)
            return spf;
        spf++;
    }
}

int fat_make_image(const char *path, int k)
{
    if (k < 0 || k >= fat_image_kinds()) { ferr("no such kind"); return -1; }
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0664);
    if (fd < 0) { ferr("cannot make it: %s", strerror(errno)); return -1; }
    uint64_t bytes = fat_image_bytes(k);
    int bad = ftruncate(fd, (off_t)bytes) != 0;          /* sparse: zeros */
    if (!bad && kinds[k].floppy) {
        uint32_t tot = kinds[k].kb * 2;
        if (kinds[k].kb == 1440)
            bad = format_vol(fd, 0, 512, 1, tot, 224, 9, 0xF0, 18, 2, 0);
        else {
            int ds = kinds[k].kb == 720;
            bad = format_vol(fd, 0, 512, 2, tot, 112, 5, ds ? 0xF9 : 0xF8, 9, ds ? 2 : 1, 0);
        }
    } else if (!bad) {
        /* AHDI: the root sector, then up to 4 partitions, the first at
         * sector 2 (sector 1 is the bad-sector list). Over 16M a BGM with
         * logical sectors big enough that TOS's limits hold: at most
         * 65535 sectors and 32766 clusters of 2 sectors. */
        uint32_t total = (uint32_t)(bytes / 512);
        int nparts = kinds[k].kb <= 128 * 1024 ? 1 : kinds[k].kb <= 256 * 1024 ? 2 : 4;
        uint32_t first = 2, each = (total - first) / (uint32_t)nparts;
        uint8_t root[512];
        memset(root, 0, sizeof root);
        root[0x1C2] = (uint8_t)(total >> 24); root[0x1C3] = (uint8_t)(total >> 16);
        root[0x1C4] = (uint8_t)(total >> 8);  root[0x1C5] = (uint8_t)total;
        for (int p = 0; p < nparts && !bad; p++) {
            uint32_t st = first + (uint32_t)p * each, sz = each;
            int bgm = sz > 32767;
            uint32_t bps = 512;
            while ((uint64_t)sz * 512 / bps > 65535 || (uint64_t)sz * 512 / bps / 2 > 32766)
                bps *= 2;
            uint32_t tot = (uint32_t)((uint64_t)sz * 512 / bps);
            uint32_t rootents = 256 > bps / 32 ? 256 : bps / 32;
            uint32_t rootsec = rootents * 32 / bps;
            uint32_t spf = fat_size(bps, 2, tot, rootsec, 1);
            uint8_t *e = root + 0x1C6 + 12 * p;
            e[0] = 1;
            memcpy(e + 1, bgm ? "BGM" : "GEM", 3);
            for (int b = 0; b < 4; b++) {
                e[4 + b] = (uint8_t)(st >> (24 - 8 * b));
                e[8 + b] = (uint8_t)(sz >> (24 - 8 * b));
            }
            bad = format_vol(fd, (uint64_t)st * 512, bps, 2, tot, rootents, spf, 0xF8, 32, 2, 1);
        }
        if (!bad)
            bad = pwrite(fd, root, 512, 0) != 512;
    }
    if (fsync(fd) != 0) bad = 1;
    close(fd);
    if (bad) {
        if (!err[0]) ferr("write error");
        unlink(path);
        return -1;
    }
    return 0;
}
