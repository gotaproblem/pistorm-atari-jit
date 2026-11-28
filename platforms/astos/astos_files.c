/*
 * astos_files.c - ASTOS's file manager.
 *
 * Two panes side by side (39 columns each fits medium's 80 and mono
 * alike). Each pane shows one place - a hostfs share, the disk image /
 * floppy image / ROM folders, a USB stick on the Pi, a folder from
 * `places` - chosen from a drop-down list, and cannot go above it. The
 * active pane is the one with the accent title; Copy and Move go from it
 * to the other one.
 *
 * Everything runs on the Pi, at SD/USB speed. ASTOS runs as root, so
 * what it creates is given to the owner of the folder it lands in (a
 * share stays writable over Samba). Delete moves into a .trash folder at
 * the top of the place; names starting with '.' are not listed.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "astos.h"
#include "astos_files.h"
#include "astos_fat.h"
#include "astos_view.h"

/* names are at most 255 and paths PATH_MAX: the recursive walks check
 * for a path that would not fit and stop there rather than act on a
 * cut-short one */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "setup_cfg.h"

/* ---- layout (logical 640x200) -------------------------------------- */
#define WX      4
#define WY      1
#define WW      632
#define WH      186
#define PW      308                   /* a pane                          */
#define PX(p)   ((p) ? 324 : 12)
#define PLACE_Y 14
#define PATH_Y  27
#define LIST_Y  38
#define RH      10
#define NVIS    11                    /* rows in a pane                  */
#define MSG_Y   (LIST_Y + NVIS * RH + 3)
#define FOOT_Y  (WY + WH - 18)
#define BTN_W   70
#define POP_MAX 12

/* ---- places --------------------------------------------------------- */
#define MAX_PLACES 24
static struct place { char label[40]; char root[PATH_MAX]; } place[MAX_PLACES];
static int nplace;

static void add_place(const char *label, const char *root)
{
    if (nplace >= MAX_PLACES || !root || !*root)
        return;
    char real[PATH_MAX];
    if (!realpath(root, real))
        return;                               /* not there: not listed */
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode))
        return;
    for (int i = 0; i < nplace; i++)
        if (!strcmp(place[i].root, real))
            return;                           /* listed already        */
    snprintf(place[nplace].label, sizeof place[nplace].label, "%s", label);
    snprintf(place[nplace].root, sizeof place[nplace].root, "%s", real);
    nplace++;
}

/* a [psctrl] path key, ~ = the invoking user's home, relative to the
 * emulator's directory (where ASTOS runs) as the emulator reads it */
static void path_key(struct sc_cfg *c, const char *key, char *out, size_t n)
{
    const char *v = sc_get(c, "psctrl", key);
    out[0] = '\0';
    if (!v || !*v)
        return;
    if (v[0] == '~') {
        const struct passwd *pw = sc_invoking_user();
        snprintf(out, n, "%s%s", pw && pw->pw_dir ? pw->pw_dir : "", v + 1);
    } else
        snprintf(out, n, "%s", v);
}

static void load_places(const char *cfg_path)
{
    static struct sc_cfg c;
    nplace = 0;
    if (sc_load(&c, cfg_path) != 0)
        return;
    char p[PATH_MAX];

    /* every hostfs share of every environment, by its drive letter */
    char secs[64][SC_SEC_LEN];
    int ns = sc_sections(&c, secs, 64);
    for (int s = 0; s < ns; s++) {
        int n = sc_count(&c, secs[s], "hostfs");
        for (int i = 0; i < n; i++) {
            const char *v = sc_get_n(&c, secs[s], "hostfs", i);
            char drive[8] = "", dir[PATH_MAX] = "";
            if (!v || sscanf(v, "%7s %4095s", drive, dir) != 2)
                continue;
            const char *base = strrchr(dir, '/');
            char lab[40];
            snprintf(lab, sizeof lab, "%c: %.30s", toupper((unsigned char)drive[0]),
                     base && base[1] ? base + 1 : dir);
            add_place(lab, dir);
        }
    }
    path_key(&c, "disk_path", p, sizeof p); add_place("Disk images", p);
    path_key(&c, "fdd_path", p, sizeof p);  add_place("Floppy images", p);
    path_key(&c, "rom_path", p, sizeof p);  add_place("ROMs", p);

    /* USB sticks and drives the Pi has mounted under /media */
    FILE *m = fopen("/proc/mounts", "r");
    if (m) {
        char dev[256], mnt[PATH_MAX], fs[64];
        while (fscanf(m, "%255s %4095s %63s %*[^\n]", dev, mnt, fs) == 3)
            if (!strncmp(mnt, "/media/", 7)) {
                const char *base = strrchr(mnt, '/');
                char lab[40];
                snprintf(lab, sizeof lab, "USB: %.30s", base ? base + 1 : mnt);
                add_place(lab, mnt);
            }
        fclose(m);
    }

    /* `places` in [psctrl]: extra folders, comma separated */
    const char *extra = sc_get(&c, "psctrl", "places");
    if (extra) {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", extra);
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            char *e = t + strlen(t);
            while (e > t && e[-1] == ' ') *--e = '\0';
            const char *base = strrchr(t, '/');
            add_place(base && base[1] ? base + 1 : t, t);
        }
    }
    const struct passwd *pw = sc_invoking_user();
    if (pw && pw->pw_dir)
        add_place("Pi home", pw->pw_dir);
}

static char msg[96];

/* ---- a pane --------------------------------------------------------- */
#define MAX_ENT 1024
struct ent { char name[256]; int dir; long long size; int mark; int vix; int is_vol; };
static struct pane {
    int place;                  /* index into place[]                   */
    char rel[PATH_MAX];         /* below the place's root, "" or "a/b"  */
    struct ent *e;
    int n, sel, top;
    /* inside a disk image: the image file in rel, a volume, a folder */
    struct fat_img *img;
    char imgname[256];
    int vol;                    /* -1: the list of its volumes          */
    char irel[PATH_MAX];        /* in the volume, "" or "A/B"           */
    long long freeb;            /* free bytes where it is, -1 unknown   */
} pane[2];
static int act;                 /* the active pane                       */

static void full_path(const struct pane *p, const char *name, char *out, size_t n)
{
    const char *root = nplace ? place[p->place].root : "/";
    if (p->rel[0] && name && *name)
        snprintf(out, n, "%s/%s/%s", root, p->rel, name);
    else if (p->rel[0])
        snprintf(out, n, "%s/%s", root, p->rel);
    else if (name && *name)
        snprintf(out, n, "%s/%s", root, name);
    else
        snprintf(out, n, "%s", root);
}

static int ent_cmp(const void *a, const void *b)
{
    const struct ent *x = a, *y = b;
    if (x->dir != y->dir)
        return y->dir - x->dir;              /* folders first */
    /* natural order: FILE2 before FILE10 */
    const char *a2 = x->name, *b2 = y->name;
    while (*a2 && *b2) {
        if (isdigit((unsigned char)*a2) && isdigit((unsigned char)*b2)) {
            while (*a2 == '0') a2++;
            while (*b2 == '0') b2++;
            size_t la = 0, lb = 0;
            while (isdigit((unsigned char)a2[la])) la++;
            while (isdigit((unsigned char)b2[lb])) lb++;
            if (la != lb) return la < lb ? -1 : 1;
            int c = strncmp(a2, b2, la);
            if (c) return c;
            a2 += la; b2 += lb;
            continue;
        }
        int ca = tolower((unsigned char)*a2), cb = tolower((unsigned char)*b2);
        if (ca != cb) return ca - cb;
        a2++; b2++;
    }
    if (*a2 || *b2) return *a2 ? 1 : -1;
    return strcasecmp(x->name, y->name);
}

static void load(struct pane *p)
{
    p->freeb = -1;
    if (p->img && p->vol >= 0)
        p->freeb = (long long)fat_free(p->img, p->vol);
    else if (!p->img) {
        char where[PATH_MAX];
        struct statvfs sv;
        full_path(p, NULL, where, sizeof where);
        if (statvfs(where, &sv) == 0)
            p->freeb = (long long)sv.f_bavail * (long long)sv.f_frsize;
    }
    char dir[PATH_MAX];
    full_path(p, NULL, dir, sizeof dir);
    if (!p->e)
        p->e = calloc(MAX_ENT, sizeof *p->e);
    p->n = 0;
    if (!p->e)
        return;
    if (p->img) {                             /* inside an image */
        struct ent *e = &p->e[0];
        snprintf(e->name, sizeof e->name, "..");
        e->dir = 1; e->size = 0; e->mark = 0; e->is_vol = 0;
        p->n = 1;
        if (p->vol < 0) {                     /* its volumes, in order */
            for (int i = 0; i < fat_nvol(p->img) && p->n < MAX_ENT; i++) {
                e = &p->e[p->n++];
                snprintf(e->name, sizeof e->name, "%s", fat_vol_label(p->img, i));
                e->dir = 1; e->mark = 0; e->vix = i;
                e->is_vol = 1;
                e->size = (long long)fat_free(p->img, i);      /* shown as free */
            }
        } else {
            static struct fat_ent fe[MAX_ENT];
            int n = fat_list(p->img, p->vol, p->irel, fe, MAX_ENT - 1);
            if (n < 0)
                snprintf(msg, sizeof msg, "%.90s", fat_error());
            for (int i = 0; i < n; i++) {
                e = &p->e[p->n++];
                snprintf(e->name, sizeof e->name, "%s", fe[i].name);
                e->dir = fe[i].dir; e->size = fe[i].size; e->mark = 0; e->is_vol = 0;
            }
            qsort(p->e + 1, (size_t)(p->n - 1), sizeof *p->e, ent_cmp);
        }
        if (p->sel >= p->n) p->sel = p->n - 1;
        if (p->top > p->sel) p->top = p->sel;
        return;
    }
    if (p->rel[0]) {                          /* the way back up */
        snprintf(p->e[0].name, sizeof p->e[0].name, "..");
        p->e[0].dir = 1;
        p->e[0].size = 0;
        p->e[0].mark = 0; p->e[0].is_vol = 0;
        p->n = 1;
    }
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && p->n < MAX_ENT) {
            if (de->d_name[0] == '.')
                continue;
            char fp[PATH_MAX];
            struct stat st;
            snprintf(fp, sizeof fp, "%s/%s", dir, de->d_name);
            if (stat(fp, &st) != 0)
                continue;
            struct ent *e = &p->e[p->n++];
            snprintf(e->name, sizeof e->name, "%s", de->d_name);
            e->dir = S_ISDIR(st.st_mode); e->is_vol = 0;
            e->size = (long long)st.st_size;
            e->mark = 0;
        }
        closedir(d);
    }
    int first = p->rel[0] ? 1 : 0;
    qsort(p->e + first, (size_t)(p->n - first), sizeof *p->e, ent_cmp);
    if (p->sel >= p->n) p->sel = p->n ? p->n - 1 : 0;
    if (p->top > p->sel) p->top = p->sel;
}

static void clamp(struct pane *p)
{
    if (p->sel < 0) p->sel = 0;
    if (p->sel >= p->n) p->sel = p->n ? p->n - 1 : 0;
    if (p->sel < p->top) p->top = p->sel;
    if (p->sel >= p->top + NVIS) p->top = p->sel - NVIS + 1;
    if (p->top < 0) p->top = 0;
}

/* ---- state ----------------------------------------------------------- */
static int open_;
static char cfg_path_[600];
enum { POP_NONE, POP_PLACE, POP_ASK, POP_CONFIRM, POP_CHOICE };
static int pop, pop_pane, pop_sel, pop_top;
enum { ASK_RENAME, ASK_MKDIR, ASK_IMAGE };
static int ask_kind;
static char ask_buf[200], ask_title[48];
enum { CF_DELETE, CF_COPY, CF_MOVE, CF_DROP };
static int cf_kind;
static char cf_text[96], cf_text2[64];

int files_is_open(void) { return open_; }

int files_open(const char *cfg_path)
{
    snprintf(cfg_path_, sizeof cfg_path_, "%s", cfg_path);
    load_places(cfg_path);
    if (!nplace) {
        snprintf(msg, sizeof msg, "no places: no shares or image folders found");
    } else
        msg[0] = '\0';
    for (int i = 0; i < 2; i++) {
        if (pane[i].place >= nplace)
            pane[i].place = 0;
        if (!pane[i].e) {                     /* first time: two places */
            pane[i].place = nplace > 1 ? i : 0;
            pane[i].rel[0] = '\0';
        }
        load(&pane[i]);
        clamp(&pane[i]);
    }
    open_ = 1;
    pop = POP_NONE;
    printf("[ASTOS] files: %d place(s)\n", nplace);
    return 0;
}

/* ---- the operations -------------------------------------------------- */

/* what ASTOS (root) makes belongs to whoever owns where it is made */
static void own_like_parent(const char *path)
{
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", path);
    char *s = strrchr(parent, '/');
    if (!s)
        return;
    *s = '\0';
    struct stat st;
    if (stat(parent[0] ? parent : "/", &st) == 0)
        (void)!lchown(path, st.st_uid, st.st_gid);
}

static int copy_file(const char *from, const char *to)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return -1;
    struct stat st;
    fstat(in, &st);
    int out = open(to, O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 0777);
    if (out < 0) {
        close(in);
        return -1;
    }
    static char buf[1 << 16];
    ssize_t n;
    int bad = 0;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) { bad = 1; break; }
    if (n < 0) bad = 1;
    close(in);
    if (close(out) != 0) bad = 1;
    if (bad) {
        unlink(to);
        return -1;
    }
    own_like_parent(to);
    return 0;
}

static int remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d)
            return -1;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            char a[PATH_MAX];
            if (snprintf(a, sizeof a, "%s/%s", path, de->d_name) >= (int)sizeof a)
                continue;
            remove_tree(a);
        }
        closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

/* ---- where an operation reads or writes: a host folder, or a folder in
 * a volume of an image ------------------------------------------------- */
struct loc {
    struct fat_img *img;        /* NULL: path is a host folder           */
    int vol;
    char path[PATH_MAX];        /* host: absolute; image: "" or "A/B"     */
};

static struct loc drop_loc;     /* where a drag was dropped              */
enum { DR_NONE, DR_PENDING, DR_ON };
static int drag, drag_pane, drag_k, drag_x, drag_y;
static double drag_t;
static int drop_pane = -1, drop_k = -1;         /* what to show as the target */

/* the pane's folder; -1 in an image's volume list */
static int pane_loc(const struct pane *p, struct loc *l)
{
    l->img = p->img;
    l->vol = p->vol;
    if (p->img) {
        if (p->vol < 0)
            return -1;
        snprintf(l->path, sizeof l->path, "%s", p->irel);
    } else
        full_path(p, NULL, l->path, sizeof l->path);
    return 0;
}

static int loc_child(const struct loc *l, const char *name, struct loc *c)
{
    *c = *l;
    int n = l->img && !l->path[0] ? snprintf(c->path, sizeof c->path, "%s", name)
                                  : snprintf(c->path, sizeof c->path, "%s/%s", l->path, name);
    return n < (int)sizeof c->path ? 0 : -1;
}

static int same_loc(const struct loc *a, const struct loc *b)
{
    return a->img == b->img && (!a->img || a->vol == b->vol) && !strcmp(a->path, b->path);
}

static char op_err[96];

static int loc_exists(const struct loc *l, const char *name)
{
    struct loc c;
    if (loc_child(l, name, &c) != 0)
        return 1;
    if (l->img)
        return fat_stat(l->img, l->vol, c.path, NULL) == 0;
    struct stat st;
    return lstat(c.path, &st) == 0;
}

static int loc_mkdir(const struct loc *l, const char *name)
{
    if (l->img) {
        if (fat_mkdir(l->img, l->vol, l->path, name) == 0)
            return 0;
        snprintf(op_err, sizeof op_err, "%s", fat_error());
        return -1;
    }
    struct loc c;
    if (loc_child(l, name, &c) != 0 || mkdir(c.path, 0775) != 0) {
        snprintf(op_err, sizeof op_err, "%s", strerror(errno));
        return -1;
    }
    own_like_parent(c.path);
    return 0;
}

static int loc_remove(const struct loc *l, const char *name)
{
    struct loc c;
    if (loc_child(l, name, &c) != 0)
        return -1;
    if (l->img) {
        if (fat_delete(l->img, l->vol, c.path) == 0)
            return 0;
        snprintf(op_err, sizeof op_err, "%s", fat_error());
        return -1;
    }
    return remove_tree(c.path);
}

/* one item, folders with all they hold; never over something there */
static int loc_copy(const struct loc *s, const char *name, const struct loc *d, int depth)
{
    struct loc sc, dc;
    if (depth > 32 || loc_child(s, name, &sc) != 0 || loc_child(d, name, &dc) != 0) {
        snprintf(op_err, sizeof op_err, "folders too deep");
        return -1;
    }
    if (loc_exists(d, name)) {
        snprintf(op_err, sizeof op_err, "%.60s is there already", name);
        return -1;
    }
    int isdir;
    if (s->img) {
        struct fat_ent fe;
        if (fat_stat(s->img, s->vol, sc.path, &fe) != 0) {
            snprintf(op_err, sizeof op_err, "%s", fat_error());
            return -1;
        }
        isdir = fe.dir;
    } else {
        struct stat st;
        if (lstat(sc.path, &st) != 0) {
            snprintf(op_err, sizeof op_err, "%s", strerror(errno));
            return -1;
        }
        isdir = S_ISDIR(st.st_mode);
    }

    if (isdir) {
        if (loc_mkdir(d, name) != 0)
            return -1;
        /* what is in it, then each */
        int max = 4096, n = 0;
        char (*kids)[256] = malloc((size_t)max * 256);
        if (!kids) { snprintf(op_err, sizeof op_err, "out of memory"); return -1; }
        if (s->img) {
            struct fat_ent *fe = malloc((size_t)max * sizeof *fe);
            n = fe ? fat_list(s->img, s->vol, sc.path, fe, max) : -1;
            for (int i = 0; i < n; i++)
                snprintf(kids[i], 256, "%s", fe[i].name);
            free(fe);
        } else {
            DIR *dd = opendir(sc.path);
            struct dirent *de;
            while (dd && (de = readdir(dd)) && n < max)
                if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
                    snprintf(kids[n++], 256, "%s", de->d_name);
            if (dd) closedir(dd);
            else n = -1;
        }
        int r = n < 0 ? -1 : 0;
        for (int i = 0; i < n && !r; i++)
            r = loc_copy(&sc, kids[i], &dc, depth + 1);
        free(kids);
        return r;
    }

    if (!s->img && !d->img)
        return copy_file(sc.path, dc.path) == 0 ? 0 :
               (snprintf(op_err, sizeof op_err, "%s", strerror(errno)), -1);
    if (!s->img) {                                  /* host into an image */
        if (fat_put(d->img, d->vol, d->path, name, sc.path) == 0)
            return 0;
        snprintf(op_err, sizeof op_err, "%s", fat_error());
        return -1;
    }
    if (!d->img) {                                  /* image out to host */
        if (fat_get(s->img, s->vol, sc.path, dc.path) == 0) {
            own_like_parent(dc.path);
            return 0;
        }
        snprintf(op_err, sizeof op_err, "%s", fat_error());
        return -1;
    }
    /* image to image: by way of a file on the Pi */
    char tmp[] = "/tmp/astos-copy.XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) { snprintf(op_err, sizeof op_err, "%s", strerror(errno)); return -1; }
    close(fd);
    unlink(tmp);
    int r = fat_get(s->img, s->vol, sc.path, tmp) == 0 &&
            fat_put(d->img, d->vol, d->path, name, tmp) == 0 ? 0 : -1;
    if (r)
        snprintf(op_err, sizeof op_err, "%s", fat_error());
    unlink(tmp);
    return r;
}

/* the entries an operation works on: the marked ones, else the cursor */
static int targets(struct pane *p, int *idx, int max)
{
    int n = 0;
    for (int i = 0; i < p->n && n < max; i++)
        if (p->e[i].mark)
            idx[n++] = i;
    if (!n && p->n && strcmp(p->e[p->sel].name, ".."))
        idx[n++] = p->sel;
    return n;
}

static void reload_both(void)
{
    for (int i = 0; i < 2; i++) {
        load(&pane[i]);
        clamp(&pane[i]);
    }
}

static int need_volume(const struct pane *p)
{
    if (p->img && p->vol < 0) {
        snprintf(msg, sizeof msg, "open a volume first");
        return 1;
    }
    return 0;
}

static void copy_move_to(int move, struct loc dl);

static void do_copy_move(int move)
{
    struct pane *s = &pane[act], *d = &pane[!act];
    struct loc dl;
    if (need_volume(s) || need_volume(d))
        return;
    pane_loc(d, &dl);
    copy_move_to(move, dl);
}

/* the active pane's items into dl (the other pane, or a folder dropped on) */
static void copy_move_to(int move, struct loc dl)
{
    struct pane *s = &pane[act];
    struct loc sl;
    if (need_volume(s))
        return;
    pane_loc(s, &sl);
    if (same_loc(&sl, &dl)) {
        snprintf(msg, sizeof msg, "both panes show the same folder");
        return;
    }
    static int idx[MAX_ENT];
    int n = targets(s, idx, MAX_ENT), done = 0, failed = 0;
    op_err[0] = '\0';
    for (int k = 0; k < n; k++) {
        const char *name = s->e[idx[k]].name;
        if (loc_exists(&dl, name)) {                /* never overwrite */
            snprintf(op_err, sizeof op_err, "%.60s is there already", name);
            failed++;
            continue;
        }
        if (move && !sl.img && !dl.img) {           /* same filesystem */
            struct loc a, b;
            if (loc_child(&sl, name, &a) == 0 && loc_child(&dl, name, &b) == 0 &&
                rename(a.path, b.path) == 0) {
                done++;
                continue;
            }
        }
        if (loc_copy(&sl, name, &dl, 0) != 0) {
            char keep[96];
            snprintf(keep, sizeof keep, "%s", op_err);
            loc_remove(&dl, name);                  /* no half copies */
            snprintf(op_err, sizeof op_err, "%s", keep);
            failed++;
            continue;
        }
        if (move && loc_remove(&sl, name) != 0) {
            failed++;
            continue;
        }
        done++;
    }
    if (failed)
        snprintf(msg, sizeof msg, "%s %d, %d failed: %.50s", move ? "moved" : "copied", done,
                 failed, op_err);
    else
        snprintf(msg, sizeof msg, "%s %d item%s", move ? "moved" : "copied", done,
                 done == 1 ? "" : "s");
    printf("[ASTOS] files: %s\n", msg);
    reload_both();
}

static void do_delete(void)
{
    struct pane *p = &pane[act];
    static int idx[MAX_ENT];
    int n = targets(p, idx, MAX_ENT), done = 0;
    if (need_volume(p))
        return;
    if (p->img) {                                   /* no trash in an image */
        struct loc l;
        pane_loc(p, &l);
        op_err[0] = '\0';
        for (int k = 0; k < n; k++)
            if (loc_remove(&l, p->e[idx[k]].name) == 0)
                done++;
        snprintf(msg, sizeof msg, "%d item%s deleted%s%.50s", done, done == 1 ? "" : "s",
                 done < n ? ": " : "", done < n ? op_err : "");
        printf("[ASTOS] files: %s\n", msg);
        reload_both();
        return;
    }
    char trash[PATH_MAX];
    snprintf(trash, sizeof trash, "%s/.trash", place[p->place].root);
    if (mkdir(trash, 0775) == 0)
        own_like_parent(trash);
    for (int k = 0; k < n; k++) {
        char a[PATH_MAX], b[PATH_MAX];
        full_path(p, p->e[idx[k]].name, a, sizeof a);
        snprintf(b, sizeof b, "%s/%s", trash, p->e[idx[k]].name);
        struct stat st;
        if (lstat(b, &st) == 0) {             /* one there already: stamp */
            char t[24];
            time_t now = time(NULL);
            strftime(t, sizeof t, "%Y%m%d-%H%M%S", localtime(&now));
            snprintf(b, sizeof b, "%s/%s.%s", trash, p->e[idx[k]].name, t);
        }
        if (rename(a, b) == 0)
            done++;
    }
    snprintf(msg, sizeof msg, "%d item%s moved to the trash", done, done == 1 ? "" : "s");
    printf("[ASTOS] files: %s\n", msg);
    reload_both();
}

static int name_bad(const char *s)
{
    return !*s || strchr(s, '/') || !strcmp(s, ".") || !strcmp(s, "..") || s[0] == '.';
}

static void make_image(void);

static void do_ask(void)
{
    struct pane *p = &pane[act];
    if (name_bad(ask_buf)) {
        snprintf(msg, sizeof msg, "not a usable name");
        return;
    }
    if (ask_kind == ASK_IMAGE) {
        struct loc l;
        pane_loc(p, &l);
        if (loc_exists(&l, ask_buf)) {
            snprintf(msg, sizeof msg, "%.40s is there already", ask_buf);
            return;
        }
        pop = POP_NONE;
        make_image();
        load(p);
        for (int i = 0; i < p->n; i++)
            if (!strcmp(p->e[i].name, ask_buf))
                p->sel = i;
        clamp(p);
        return;
    }
    struct loc l;
    if (need_volume(p) || pane_loc(p, &l) != 0)
        return;
    const char *old = p->e[p->sel].name;
    int case_only = ask_kind == ASK_RENAME && p->img && !strcasecmp(old, ask_buf);
    if (loc_exists(&l, ask_buf) && !case_only) {
        snprintf(msg, sizeof msg, "%.40s is there already", ask_buf);
        return;
    }
    if (ask_kind == ASK_MKDIR) {
        if (loc_mkdir(&l, ask_buf) == 0)
            snprintf(msg, sizeof msg, "made folder %.40s", ask_buf);
        else
            snprintf(msg, sizeof msg, "could not make it: %.60s", op_err);
    } else {
        struct loc a, b;
        int ok;
        if (p->img) {
            ok = loc_child(&l, old, &a) == 0 && fat_rename(p->img, p->vol, a.path, ask_buf) == 0;
            if (!ok) snprintf(op_err, sizeof op_err, "%s", fat_error());
        } else {
            ok = loc_child(&l, old, &a) == 0 && loc_child(&l, ask_buf, &b) == 0 &&
                 rename(a.path, b.path) == 0;
            if (!ok) snprintf(op_err, sizeof op_err, "%s", strerror(errno));
        }
        if (ok)
            snprintf(msg, sizeof msg, "renamed to %.40s", ask_buf);
        else
            snprintf(msg, sizeof msg, "could not rename: %.60s", op_err);
    }
    pop = POP_NONE;
    load(p);
    for (int i = 0; i < p->n; i++)              /* the cursor on it */
        if (!strcasecmp(p->e[i].name, ask_buf))
            p->sel = i;
    clamp(p);
}

static void ask(int kind)
{
    struct pane *p = &pane[act];
    if (need_volume(p))
        return;
    if (kind == ASK_RENAME && (!p->n || !strcmp(p->e[p->sel].name, "..")))
        return;
    ask_kind = kind;
    snprintf(ask_title, sizeof ask_title, kind == ASK_RENAME ? "Rename" : "New folder");
    if (kind == ASK_RENAME)
        snprintf(ask_buf, sizeof ask_buf, "%s", p->e[p->sel].name);
    else
        ask_buf[0] = '\0';
    pop = POP_ASK;
}

static void confirm(int kind)
{
    struct pane *p = &pane[act];
    if (need_volume(p) || (kind != CF_DELETE && need_volume(&pane[!act])))
        return;
    static int idx[MAX_ENT];
    int n = targets(p, idx, MAX_ENT);
    if (!n)
        return;
    cf_kind = kind;
    const char *verb = kind == CF_DELETE ? "Delete" : kind == CF_COPY ? "Copy" : "Move";
    if (n == 1)
        snprintf(cf_text, sizeof cf_text, "%s %.34s%s", verb, p->e[idx[0]].name,
                 kind == CF_DELETE ? "?" : " across?");
    else
        snprintf(cf_text, sizeof cf_text, "%s %d items%s", verb, n,
                 kind == CF_DELETE ? "?" : " to the other pane?");
    cf_text2[0] = '\0';
    if (kind == CF_DELETE && p->img)
        snprintf(cf_text2, sizeof cf_text2, "No trash in an image: gone for good.");
    else if (kind != CF_DELETE && pane[!act].img)
        snprintf(cf_text2, sizeof cf_text2, "Into the image: names become 8.3.");
    pop = POP_CONFIRM;
}

/* look at a file: text, hex or a picture (one in an image is read out
 * to /tmp first, and the copy dropped once it is open) */
static void view_file(struct pane *p, const struct ent *e)
{
    char path[PATH_MAX], why[96] = "";
    int tmp = 0;
    if (p->img) {
        if (p->vol < 0)
            return;
        struct loc l, c;
        pane_loc(p, &l);
        if (loc_child(&l, e->name, &c) != 0)
            return;
        snprintf(path, sizeof path, "/tmp/astos-view.%d", (int)getpid());
        unlink(path);
        if (fat_get(p->img, p->vol, c.path, path) != 0) {
            snprintf(msg, sizeof msg, "%.90s", fat_error());
            return;
        }
        tmp = 1;
    } else
        full_path(p, e->name, path, sizeof path);
    int r = view_open(path, e->name, why, sizeof why);
    if (tmp)
        unlink(path);
    if (r != 0)
        snprintf(msg, sizeof msg, "%.40s: %.50s", e->name, why);
}

/* into an image file, else look at it */
static void enter_image(struct pane *p, const struct ent *e)
{
    char path[PATH_MAX], why[96];
    full_path(p, e->name, path, sizeof path);
    struct fat_img *im = e->size >= 512 ? fat_open(path, why, sizeof why) : NULL;
    if (!im) {
        view_file(p, e);
        return;
    }
    p->img = im;
    snprintf(p->imgname, sizeof p->imgname, "%s", e->name);
    p->vol = fat_nvol(im) == 1 ? 0 : -1;
    p->irel[0] = '\0';
    p->sel = p->top = 0;
    snprintf(msg, sizeof msg, "%s%s", fat_nvol(im) == 1 ? fat_vol_label(im, 0) : "",
             fat_readonly(im) ? " - read-only" : "");
    load(p);
    clamp(p);
}

static void leave_image(struct pane *p)
{
    char was[256];
    snprintf(was, sizeof was, "%s", p->imgname);
    fat_close(p->img);
    p->img = NULL;
    p->sel = p->top = 0;
    load(p);
    for (int i = 0; i < p->n; i++)
        if (!strcmp(p->e[i].name, was))
            p->sel = i;
    clamp(p);
}

static void open_entry(struct pane *p)
{
    if (!p->n)
        return;
    struct ent *e = &p->e[p->sel];
    if (!strcmp(e->name, "..")) {
        char was[256] = "";
        if (p->img && !p->irel[0]) {
            if (p->vol >= 0 && fat_nvol(p->img) > 1) {   /* the volume list */
                int v = p->vol;
                p->vol = -1;
                p->sel = v + 1;
                p->top = 0;
                load(p);
                clamp(p);
            } else
                leave_image(p);
            return;
        }
        char *rel = p->img ? p->irel : p->rel;
        char *s = strrchr(rel, '/');
        snprintf(was, sizeof was, "%s", s ? s + 1 : rel);
        if (s) *s = '\0'; else rel[0] = '\0';
        p->sel = p->top = 0;
        load(p);
        for (int i = 0; i < p->n; i++)          /* back onto where we were */
            if (!strcmp(p->e[i].name, was))
                p->sel = i;
        clamp(p);
        return;
    }
    if (p->img && p->vol < 0) {                 /* a volume */
        p->vol = e->vix;
        p->irel[0] = '\0';
        p->sel = p->top = 0;
        snprintf(msg, sizeof msg, "%s", fat_vol_label(p->img, p->vol));
        load(p);
        clamp(p);
        return;
    }
    if (!e->dir) {
        if (p->img)
            view_file(p, e);
        else
            enter_image(p, e);
        return;
    }
    char *rel = p->img ? p->irel : p->rel;
    char nrel[PATH_MAX];
    if (rel[0])
        snprintf(nrel, sizeof nrel, "%s/%s", rel, e->name);
    else
        snprintf(nrel, sizeof nrel, "%s", e->name);
    snprintf(rel, PATH_MAX, "%s", nrel);
    p->sel = p->top = 0;
    load(p);
    clamp(p);
}

static void parent(struct pane *p)
{
    if (!p->rel[0] && !p->img)
        return;
    p->sel = 0;
    open_entry(p);                              /* ".." is entry 0 */
}

static void set_place(int pn, int pl)
{
    struct pane *p = &pane[pn];
    if (p->img) {
        fat_close(p->img);
        p->img = NULL;
    }
    p->place = pl;
    p->rel[0] = '\0';
    p->sel = p->top = 0;
    load(p);
    clamp(p);
    msg[0] = '\0';
}

/* ---- drawing ---------------------------------------------------------- */

static void button(int x, const char *label)
{
    g_fill(x, FOOT_Y, BTN_W, 12, C_FACE);
    g_frame(x, FOOT_Y, BTN_W, 12, C_TEXT);
    g_text(x + (BTN_W - g_text_w(label)) / 2, FOOT_Y + 2, label, C_TEXT);
}

static const char *const btn[] = { "Copy", "Move", "Rename", "Folder", "Delete", "New img",
                                   "Use in", "Close" };
#define NBTN 8
static int btn_x(int i) { return 12 + i * (BTN_W + 6); }

static void human(long long v, char *b, size_t n)
{
    if (v < 10000)                     snprintf(b, n, "%lld", v);
    else if (v < 10000LL * 1024)       snprintf(b, n, "%lldK", v >> 10);
    else if (v < 10000LL * 1024 * 1024) snprintf(b, n, "%lldM", v >> 20);
    else                               snprintf(b, n, "%lldG", v >> 30);
}

static void size_text(const struct ent *e, char *b, size_t n)
{
    if (e->is_vol) {                                  /* a partition: its free */
        char h[16];
        human(e->size, h, sizeof h);
        snprintf(b, n, "%s free", h);
    } else if (e->dir)         snprintf(b, n, "<DIR>");
    else if (e->size < 10000)  snprintf(b, n, "%lld", e->size);
    else if (e->size < 10000LL * 1024) snprintf(b, n, "%lldK", e->size >> 10);
    else if (e->size < 10000LL * 1024 * 1024) snprintf(b, n, "%lldM", e->size >> 20);
    else                       snprintf(b, n, "%lldG", e->size >> 30);
}

static void draw_pane(int i)
{
    struct pane *p = &pane[i];
    int x = PX(i);
    /* the place: a drop-down list */
    g_fill(x, PLACE_Y, PW, 11, i == act ? C_ACCENT : C_FACE);
    g_frame(x, PLACE_Y, PW, 11, C_TEXT);
    g_textn(x + 4, PLACE_Y + 2, nplace ? place[p->place].label : "-", (PW - 20) / 8,
            i == act ? C_WHITE : C_TEXT);
    int ax = x + PW - 12, c = i == act ? C_WHITE : C_TEXT;
    g_fill(ax, PLACE_Y + 3, 7, 1, c); g_fill(ax + 1, PLACE_Y + 4, 5, 1, c);
    g_fill(ax + 2, PLACE_Y + 5, 3, 1, c); g_fill(ax + 3, PLACE_Y + 6, 1, 1, c);
    /* where in it */
    char path[PATH_MAX + 2];
    if (p->img && p->vol >= 0)
        snprintf(path, sizeof path, "/%s%s%s > %c:/%s", p->rel, p->rel[0] ? "/" : "",
                 p->imgname, fat_vol_label(p->img, p->vol)[1] == ':' ?
                 fat_vol_label(p->img, p->vol)[0] : 'A', p->irel);
    else if (p->img)
        snprintf(path, sizeof path, "/%s%s%s >", p->rel, p->rel[0] ? "/" : "", p->imgname);
    else
        snprintf(path, sizeof path, "/%s", p->rel);
    /* and the free space there, on the right */
    char fr[24] = "";
    if (p->freeb >= 0) {
        char h[16];
        human(p->freeb, h, sizeof h);
        snprintf(fr, sizeof fr, "%s free", h);
    }
    int fw = fr[0] ? g_text_w(fr) + 22 : 0;
    int len = (int)strlen(path), maxc = (PW - fw) / 8;
    g_textn(x, PATH_Y, len > maxc ? path + len - maxc : path, maxc, C_DIM);
    if (fr[0])
        g_text(x + PW - 14 - g_text_w(fr), PATH_Y, fr, C_ACCENT);
    /* the list */
    g_frame(x, LIST_Y - 2, PW, NVIS * RH + 3, i == act ? C_ACCENT : C_DIM);
    if (drop_pane == i && drop_k < 0)                   /* a drop target */
        g_frame(x + 1, LIST_Y - 1, PW - 2, NVIS * RH + 1, C_ACCENT);
    for (int r = 0; r < NVIS; r++) {
        int k = p->top + r;
        if (k >= p->n)
            break;
        const struct ent *e = &p->e[k];
        int y = LIST_Y + r * RH;
        int on = k == p->sel && i == act;
        if (on)
            g_fill(x + 2, y, PW - 4, RH, C_ACCENT);
        else if (k == p->sel)
            g_fill(x + 2, y, PW - 4, RH, C_FACE);
        char sz[16];
        size_text(e, sz, sizeof sz);
        int fg = on ? C_WHITE : e->dir ? C_TEXT : C_TEXT;
        g_textn(x + 6, y + 1, e->mark ? "*" : " ", 1, on ? C_WHITE : C_ACCENT);
        g_textn(x + 14, y + 1, e->name, (PW - 14 - 64) / 8, fg);
        g_text(x + PW - 8 - g_text_w(sz), y + 1, sz, on ? C_WHITE : e->dir ? C_ACCENT : C_DIM);
        if (drop_pane == i && drop_k == k)               /* the folder to drop in */
            g_frame(x + 2, y, PW - 4, RH, C_ACCENT);
    }
    if (!p->n)
        g_text(x + 14, LIST_Y + 1, "(empty)", C_DIM);
    if (p->top > 0)
        g_text(x + PW - 10, LIST_Y - 10, "^", C_ACCENT);
    if (p->top + NVIS < p->n)
        g_text(x + PW - 10, MSG_Y - 1, "v", C_ACCENT);
}

static void dlg_rect(int *w, int *h, int *x, int *y)
{
    *w = 360;
    *h = pop == POP_ASK ? 50 : cf_text2[0] ? 56 : 46;
    *x = (AW - *w) / 2;
    *y = 60;
}

static void dlg_button(int x, int y, int w, const char *label)
{
    g_fill(x, y, w, 12, C_FACE);
    g_frame(x, y, w, 12, C_TEXT);
    g_text(x + (w - g_text_w(label)) / 2, y + 2, label, C_TEXT);
}

static void draw_pop(void)
{
    if (pop == POP_PLACE) {
        int x = PX(pop_pane), y = PLACE_Y + 12;
        int vis = nplace < POP_MAX ? nplace : POP_MAX, h = vis * RH + 4;
        g_fill(x + 2, y + 1, PW, h, C_TEXT);
        g_fill(x, y, PW, h, C_WHITE);
        g_frame(x, y, PW, h, C_TEXT);
        for (int k = pop_top; k < nplace && k < pop_top + POP_MAX; k++) {
            int iy = y + 2 + (k - pop_top) * RH, on = k == pop_sel;
            if (on)
                g_fill(x + 2, iy, PW - 4, RH, C_ACCENT);
            g_textn(x + 6, iy + 1, place[k].label, (PW - 12) / 8, on ? C_WHITE : C_TEXT);
        }
        return;
    }
    int w, h, x, y;
    dlg_rect(&w, &h, &x, &y);
    g_fill(x + 2, y + 1, w, h, C_TEXT);
    g_fill(x, y, w, h, C_WHITE);
    g_fill(x, y, w, 11, C_ACCENT);
    g_frame(x, y, w, h, C_TEXT);
    if (pop == POP_ASK) {
        g_text(x + 6, y + 2, ask_title, C_WHITE);
        char f[220];
        snprintf(f, sizeof f, "%s_", ask_buf);
        int len = (int)strlen(f), maxc = (w - 28) / 8;
        g_fill(x + 10, y + 16, w - 20, 10, C_WHITE);
        g_frame(x + 10, y + 16, w - 20, 10, C_ACCENT);
        g_textn(x + 14, y + 17, len > maxc ? f + len - maxc : f, maxc, C_TEXT);
        dlg_button(x + w - 200, y + h - 16, 96, "OK");
        dlg_button(x + w - 96, y + h - 16, 88, "Cancel");
    } else {
        g_text(x + 6, y + 2, cf_kind == CF_DELETE ? "Delete" : cf_kind == CF_COPY ? "Copy" :
               cf_kind == CF_MOVE ? "Move" : "Drop", C_WHITE);
        g_textn(x + 10, y + 16, cf_text, (w - 20) / 8, C_TEXT);
        if (cf_text2[0])
            g_textn(x + 10, y + 27, cf_text2, (w - 20) / 8, C_ACCENT);
        if (cf_kind == CF_DROP)
            dlg_button(x + w - 300, y + h - 16, 96, "Copy");
        dlg_button(x + w - 200, y + h - 16, 96, cf_kind == CF_DROP ? "Move" :
                   cf_kind != CF_DELETE ? "OK" : pane[act].img ? "Delete" : "To trash");
        dlg_button(x + w - 96, y + h - 16, 88, "Cancel");
    }
}

static int inside(int x, int y, int rx, int ry, int rw, int rh);

/* ---- a list to choose from: the kind of a new image, an environment,
 * a drive slot ----------------------------------------------------------- */
#define CH_MAX 16
enum { CH_IMG_KIND, CH_USE_ENV, CH_USE_SLOT };
static int ch_step, ch_n, ch_sel;
static char ch_title[48], ch_item[CH_MAX][48];
static int new_kind;                         /* the image kind chosen      */
static int cfg_changed;                      /* psctrl.cfg written          */

int files_cfg_changed(void)
{
    int c = cfg_changed;
    cfg_changed = 0;
    return c;
}

static void choose(int step, const char *title)
{
    ch_step = step;
    ch_sel = 0;
    snprintf(ch_title, sizeof ch_title, "%s", title);
    pop = POP_CHOICE;
}

static void ch_rect(int *w, int *h, int *x, int *y)
{
    *w = 400;
    *h = 14 + ch_n * RH + 20;
    *x = (AW - *w) / 2;
    *y = (FOOT_Y - *h) / 2 + 6;
}

/* ---- New image ---------------------------------------------------------- */

static void new_image(void)
{
    struct pane *p = &pane[act];
    if (p->img) {
        snprintf(msg, sizeof msg, "a new image goes in a Pi folder, not in an image");
        return;
    }
    ch_n = fat_image_kinds() < CH_MAX ? fat_image_kinds() : CH_MAX;
    for (int i = 0; i < ch_n; i++)
        snprintf(ch_item[i], sizeof ch_item[i], "%s", fat_image_kind(i));
    choose(CH_IMG_KIND, "New image");
}

static void make_image(void)
{
    struct pane *p = &pane[act];
    char path[PATH_MAX];
    full_path(p, ask_buf, path, sizeof path);
    uint64_t need = fat_image_bytes(new_kind);
    if (p->freeb >= 0 && (uint64_t)p->freeb < need) {
        snprintf(msg, sizeof msg, "not enough room here for it");
        return;
    }
    if (fat_make_image(path, new_kind) != 0) {
        snprintf(msg, sizeof msg, "%.90s", fat_error());
        return;
    }
    own_like_parent(path);
    snprintf(msg, sizeof msg, "made %.40s - Enter opens it", ask_buf);
    printf("[ASTOS] files: new image %s (%s)\n", path, fat_image_kind(new_kind));
}

/* ---- Use in an environment ---------------------------------------------- */

static struct env use_env[MAX_ENV];
static int use_nenv, use_ei, use_floppy;
static char use_path[PATH_MAX], use_name[256];
struct slot { const char *key; int num; int line; char img[80]; };
static struct slot slots[CH_MAX];
static int nslots;

static void slot_name(const struct slot *s, char *b, size_t n)
{
    if (!strcmp(s->key, "fdd")) snprintf(b, n, "drive %c", 'A' + s->num);
    else snprintf(b, n, "%s %d", strcmp(s->key, "hdd") ? "ACSI" : "IDE", s->num);
}

static int is_switch(const char *v)
{
    static const char *const w[] = { "enabled", "disabled", "on", "off", "yes", "no",
                                     "true", "false", "1", "0" };
    for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++)
        if (!strcasecmp(v, w[i]))
            return 1;
    return 0;
}

static int is_off(const char *v)
{
    return v && (!strcasecmp(v, "disabled") || !strcasecmp(v, "off") ||
                 !strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcmp(v, "0"));
}

/* which slot each existing line of key fills, as the emulator reads them */
static void read_slots(struct sc_cfg *c, const char *sec, const char *key, int nslot,
                       int first_slot_idx)
{
    int n = sc_count(c, sec, key), next = 0;
    for (int i = 0; i < n; i++) {
        const char *v = sc_get_n(c, sec, key, i);
        if (!v || is_switch(v))
            continue;
        int num = -1;
        const char *img = v;
        if (!strcmp(key, "fdd") && v[0] && v[1] == ':') {
            int ch = toupper((unsigned char)v[0]);
            num = ch == 'A' || ch == '0' ? 0 : ch == 'B' || ch == '1' ? 1 : -1;
            img = v + 2;
        } else if (isdigit((unsigned char)v[0]) && v[1] == ':') {
            num = v[0] - '0';
            img = v + 2;
        }
        if (num < 0) {                           /* unpinned: the next free */
            while (next < nslot) {
                int taken = 0;
                for (int s = 0; s < nslot; s++)
                    if (slots[first_slot_idx + s].num == next && slots[first_slot_idx + s].line >= 0)
                        taken = 1;
                if (!taken) break;
                next++;
            }
            num = next++;
        }
        for (int s = 0; s < nslot; s++)
            if (slots[first_slot_idx + s].num == num) {
                slots[first_slot_idx + s].line = i;
                const char *b = strrchr(img, '/');
                snprintf(slots[first_slot_idx + s].img, sizeof slots[0].img, "%s", b ? b + 1 : img);
            }
    }
}

static void use_in(void)
{
    struct pane *p = &pane[act];
    if (p->img || !p->n || p->e[p->sel].dir) {
        snprintf(msg, sizeof msg, "put the cursor on an image file on the Pi first");
        return;
    }
    const struct ent *e = &p->e[p->sel];
    snprintf(use_name, sizeof use_name, "%s", e->name);
    full_path(p, e->name, use_path, sizeof use_path);
    const char *dot = strrchr(e->name, '.');
    use_floppy = e->size <= 2949120 ||
                 (dot && (!strcasecmp(dot, ".st") || !strcasecmp(dot, ".msa") ||
                          !strcasecmp(dot, ".stx")));
    static struct sc_cfg c;
    if (sc_load(&c, cfg_path_) != 0) {
        snprintf(msg, sizeof msg, "cannot read %.60s", cfg_path_);
        return;
    }
    use_nenv = env_load(&c, use_env, MAX_ENV);
    ch_n = 0;
    for (int i = 0; i < use_nenv && ch_n < CH_MAX; i++)
        snprintf(ch_item[ch_n++], sizeof ch_item[0], "%s", use_env[i].title);
    if (!ch_n) {
        snprintf(msg, sizeof msg, "no environments to use it in");
        return;
    }
    char t[48];
    snprintf(t, sizeof t, "Use %.20s in", e->name);
    choose(CH_USE_ENV, t);
}

static void use_pick_env(int ei)
{
    use_ei = ei;
    static struct sc_cfg c;
    if (sc_load(&c, cfg_path_) != 0)
        return;
    const char *sec = use_env[ei].sec;
    const char *mach = sc_get(&c, sec, "machine");
    int falcon = mach && !strcasecmp(mach, "falcon");
    nslots = 0;
    if (use_floppy) {
        for (int i = 0; i < 2; i++)
            slots[nslots++] = (struct slot){ "fdd", i, -1, "" };
        read_slots(&c, sec, "fdd", 2, 0);
    } else {
        for (int i = 0; i < 2; i++)
            slots[nslots++] = (struct slot){ "hdd", i, -1, "" };
        read_slots(&c, sec, "hdd", 2, 0);
        if (!falcon) {                             /* no ACSI on a Falcon */
            for (int i = 0; i < 4; i++)
                slots[nslots++] = (struct slot){ "acsi", i, -1, "" };
            read_slots(&c, sec, "acsi", 4, 2);
        }
    }
    ch_n = nslots;
    for (int i = 0; i < nslots; i++) {
        const struct slot *s = &slots[i];
        char name[12];
        slot_name(s, name, sizeof name);
        snprintf(ch_item[i], sizeof ch_item[i], "%-8s %.30s", name, s->img[0] ? s->img : "(empty)");
    }
    char t[48];
    snprintf(t, sizeof t, "%.20s: which drive?", use_env[ei].title);
    choose(CH_USE_SLOT, t);
}

static void use_pick_slot(int si)
{
    static struct sc_cfg c;
    if (sc_load(&c, cfg_path_) != 0)
        return;
    const struct slot *s = &slots[si];
    const char *sec = use_env[use_ei].sec;
    for (int i = 0; i < nslots; i++)           /* one image, one drive */
        if (i != si && !strcmp(slots[i].img, use_name)) {
            char sn[12];
            slot_name(&slots[i], sn, sizeof sn);
            snprintf(msg, sizeof msg, "%.30s is already %s of %.24s", use_name, sn,
                     use_env[use_ei].title);
            return;
        }
    /* in the image folder the emulator looks in: the bare name, else the
     * whole path */
    char dir[PATH_MAX], img[PATH_MAX], val[PATH_MAX + 4];
    snprintf(dir, sizeof dir, "%s", use_path);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = '\0';
    const char *base_key = use_floppy ? "fdd_path" : "disk_path";
    char base[PATH_MAX] = "", rb[PATH_MAX], rd[PATH_MAX];
    path_key(&c, base_key, base, sizeof base);
    if (base[0] && realpath(base, rb) && realpath(dir, rd) && !strcmp(rb, rd))
        snprintf(img, sizeof img, "%s", use_name);
    else
        snprintf(img, sizeof img, "%s", use_path);
    if (!strcmp(s->key, "fdd"))
        snprintf(val, sizeof val, "%c:%s", 'A' + s->num, img);
    else
        snprintf(val, sizeof val, "%d:%s", s->num, img);
    int n = s->line >= 0 ? s->line : sc_count(&c, sec, s->key);
    int bad = sc_set_n(&c, sec, s->key, n, val) != 0;
    /* the controller switched on if it was off */
    if (!bad && !strcmp(s->key, "hdd") && is_off(sc_get(&c, sec, "ide")))
        bad = sc_set(&c, sec, "ide", "enabled") != 0;
    if (!bad && !strcmp(s->key, "acsi"))
        for (int i = 0; i < sc_count(&c, sec, "acsi"); i++)
            if (is_off(sc_get_n(&c, sec, "acsi", i)))
                bad = sc_set_n(&c, sec, "acsi", i, "enabled") != 0;
    if (bad || sc_save(&c, cfg_path_) != 0) {
        snprintf(msg, sizeof msg, "could not save: %.60s", bad ? "file full" : sc_save_error(&c));
        return;
    }
    cfg_changed = 1;
    char sn[12];
    slot_name(s, sn, sizeof sn);
    snprintf(msg, sizeof msg, "%.30s is now %s of %.24s", use_name, sn, use_env[use_ei].title);
    printf("[ASTOS] files: [%s] %s %s\n", sec, s->key, val);
}

static void ch_pick(int i)
{
    if (i < 0 || i >= ch_n)
        return;
    pop = POP_NONE;
    switch (ch_step) {
    case CH_IMG_KIND: {
        new_kind = i;
        const char *stem = fat_image_floppy(i) ? "NEW" : "NEWHD";
        const char *ext = fat_image_floppy(i) ? "ST" : "IMG";
        struct loc l;
        pane_loc(&pane[act], &l);
        snprintf(ask_buf, sizeof ask_buf, "%s.%s", stem, ext);
        for (int n = 2; loc_exists(&l, ask_buf) && n < 100; n++)
            snprintf(ask_buf, sizeof ask_buf, "%s%d.%s", stem, n, ext);
        ask_kind = ASK_IMAGE;
        snprintf(ask_title, sizeof ask_title, "New image: its name");
        pop = POP_ASK;
        break;
    }
    case CH_USE_ENV:  use_pick_env(i); break;
    case CH_USE_SLOT: use_pick_slot(i); break;
    }
}

static void draw_choice(void)
{
    int w, h, x, y;
    ch_rect(&w, &h, &x, &y);
    g_fill(x + 2, y + 1, w, h, C_TEXT);
    g_fill(x, y, w, h, C_WHITE);
    g_fill(x, y, w, 11, C_ACCENT);
    g_frame(x, y, w, h, C_TEXT);
    g_text(x + 6, y + 2, ch_title, C_WHITE);
    for (int i = 0; i < ch_n; i++) {
        int iy = y + 13 + i * RH, on = i == ch_sel;
        if (on)
            g_fill(x + 2, iy, w - 4, RH, C_ACCENT);
        g_textn(x + 8, iy + 1, ch_item[i], (w - 16) / 8, on ? C_WHITE : C_TEXT);
    }
    g_text(x + 8, y + h - 14, "Enter or click to choose", C_DIM);
    dlg_button(x + w - 96, y + h - 16, 88, "Cancel");
}

static int ch_key(struct si_event k)
{
    switch (k.key) {
    case SI_UP:   if (ch_sel > 0) ch_sel--; break;
    case SI_DOWN: if (ch_sel < ch_n - 1) ch_sel++; break;
    case SI_ENTER: case SI_SPACE: ch_pick(ch_sel); break;
    case SI_ESC: pop = POP_NONE; break;
    default: break;
    }
    return 0;
}

static int ch_click(int x, int y)
{
    int w, h, dx, dy;
    ch_rect(&w, &h, &dx, &dy);
    if (inside(x, y, dx + w - 96, dy + h - 16, 88, 12) || !inside(x, y, dx, dy, w, h)) {
        pop = POP_NONE;
        return 0;
    }
    int i = (y - dy - 13) / RH;
    if (y >= dy + 13 && i < ch_n)
        ch_pick(i);
    return 0;
}

static int ch_hover(int x, int y)
{
    int w, h, dx, dy;
    ch_rect(&w, &h, &dx, &dy);
    if (!inside(x, y, dx, dy + 13, w, ch_n * RH))
        return 0;
    int i = (y - dy - 13) / RH;
    if (i != ch_sel) { ch_sel = i; return 1; }
    return 0;
}

void files_draw(void)
{
    g_fill(WX + 2, WY + 1, WW, WH, C_TEXT);
    g_fill(WX, WY, WW, WH, C_WHITE);
    g_fill(WX, WY, WW, 11, C_ACCENT);
    g_frame(WX, WY, WW, WH, C_TEXT);
    g_text(WX + 6, WY + 2, "Files", C_WHITE);
    draw_pane(0);
    draw_pane(1);
    g_textn(12, MSG_Y + 8, msg, 76, C_ACCENT);
    for (int i = 0; i < NBTN; i++)
        button(btn_x(i), btn[i]);
    if (pop == POP_CHOICE)
        draw_choice();
    else if (pop)
        draw_pop();
    if (view_is_open() && !view_is_picture())
        view_draw(WX, WY, WW, WH);
}

/* ---- input ------------------------------------------------------------ */

static void do_button(int i)
{
    switch (i) {
    case 0: confirm(CF_COPY); break;
    case 1: confirm(CF_MOVE); break;
    case 2: ask(ASK_RENAME); break;
    case 3: ask(ASK_MKDIR); break;
    case 4: confirm(CF_DELETE); break;
    case 5: new_image(); break;
    case 6: use_in(); break;
    }
}

int files_key(struct si_event k)
{
    struct pane *p = &pane[act];
    if (view_is_open()) {
        view_key(k);
        return 0;
    }
    if (pop == POP_CHOICE)
        return ch_key(k);
    if (pop == POP_ASK) {
        size_t n = strlen(ask_buf);
        switch (k.key) {
        case SI_CHAR: case SI_SPACE:
            if (n + 1 < sizeof ask_buf) { ask_buf[n] = k.ch ? k.ch : ' '; ask_buf[n + 1] = '\0'; }
            break;
        case SI_BACKSPACE: if (n) ask_buf[n - 1] = '\0'; break;
        case SI_ENTER: do_ask(); break;
        case SI_ESC: pop = POP_NONE; break;
        default: break;
        }
        return 0;
    }
    if (pop == POP_CONFIRM && cf_kind == CF_DROP) {    /* Enter copy, M move */
        if (k.key == SI_ENTER || (k.key == SI_CHAR && (k.ch | 0x20) == 'c')) {
            pop = POP_NONE;
            copy_move_to(0, drop_loc);
        } else if (k.key == SI_CHAR && (k.ch | 0x20) == 'm') {
            pop = POP_NONE;
            copy_move_to(1, drop_loc);
        } else if (k.key == SI_ESC)
            pop = POP_NONE;
        return 0;
    }
    if (pop == POP_CONFIRM) {
        if (k.key == SI_ENTER) {
            pop = POP_NONE;
            if (cf_kind == CF_DELETE) do_delete();
            else do_copy_move(cf_kind == CF_MOVE);
        } else if (k.key == SI_ESC)
            pop = POP_NONE;
        return 0;
    }
    if (pop == POP_PLACE) {
        switch (k.key) {
        case SI_UP:   if (pop_sel > 0) pop_sel--; break;
        case SI_DOWN: if (pop_sel < nplace - 1) pop_sel++; break;
        case SI_ENTER: case SI_SPACE: set_place(pop_pane, pop_sel); pop = POP_NONE; break;
        case SI_ESC: pop = POP_NONE; break;
        default: break;
        }
        if (pop_sel < pop_top) pop_top = pop_sel;
        if (pop_sel >= pop_top + POP_MAX) pop_top = pop_sel - POP_MAX + 1;
        return 0;
    }
    switch (k.key) {
    case SI_UP:    p->sel--; clamp(p); break;
    case SI_DOWN:  p->sel++; clamp(p); break;
    case SI_HOME:  p->sel = 0; clamp(p); break;
    case SI_TAB:   act = !act; msg[0] = '\0'; break;
    case SI_LEFT:  act = 0; break;
    case SI_RIGHT: act = 1; break;
    case SI_ENTER: open_entry(p); break;
    case SI_BACKSPACE: parent(p); break;
    case SI_SPACE:                                  /* mark, then down */
        if (p->n && strcmp(p->e[p->sel].name, ".."))
            p->e[p->sel].mark = !p->e[p->sel].mark;
        p->sel++;
        clamp(p);
        break;
    case SI_F1:                                     /* the place list */
        pop = POP_PLACE; pop_pane = act; pop_sel = p->place; pop_top = 0;
        break;
    case SI_F5: confirm(CF_COPY); break;
    case SI_F6: confirm(CF_MOVE); break;
    case SI_F7: ask(ASK_MKDIR); break;
    case SI_F8: case SI_DELETE: confirm(CF_DELETE); break;
    case SI_F2: ask(ASK_RENAME); break;
    case SI_F9: new_image(); break;
    case SI_F3:                                     /* look at it as it is */
        if (p->n && !p->e[p->sel].dir && !(p->img && p->vol < 0))
            view_file(p, &p->e[p->sel]);
        break;
    case SI_F4: use_in(); break;
    case SI_ESC: open_ = 0; return 1;
    case SI_CHAR:                                   /* jump by letter */
        for (int n = 1; n <= p->n; n++) {
            int i = (p->sel + n) % p->n;
            if ((p->e[i].name[0] | 0x20) == (k.ch | 0x20)) { p->sel = i; break; }
        }
        clamp(p);
        break;
    default: break;
    }
    return 0;
}

static int inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static double last_t;
static int last_pane = -1, last_k = -1;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int files_click(int x, int y)
{
    if (view_is_open()) {                       /* a click: back */
        if (view_is_picture() || !inside(x, y, WX, WY, WW, WH))
            view_close();
        return 0;
    }
    if (pop == POP_CHOICE)
        return ch_click(x, y);
    if (pop == POP_PLACE) {
        int px = PX(pop_pane), py = PLACE_Y + 12;
        int vis = nplace < POP_MAX ? nplace : POP_MAX;
        if (inside(x, y, px, py + 2, PW, vis * RH)) {
            int k = pop_top + (y - py - 2) / RH;
            if (k >= 0 && k < nplace)
                set_place(pop_pane, k);
        }
        pop = POP_NONE;
        return 0;
    }
    if (pop == POP_ASK || pop == POP_CONFIRM) {
        int w, h, dx, dy;
        dlg_rect(&w, &h, &dx, &dy);
        struct si_event k = { SI_NONE, SI_SRC_ST, 0 };
        int drop = pop == POP_CONFIRM && cf_kind == CF_DROP;
        if (drop && inside(x, y, dx + w - 300, dy + h - 16, 96, 12)) k.key = SI_ENTER;
        else if (drop && inside(x, y, dx + w - 200, dy + h - 16, 96, 12)) { k.key = SI_CHAR; k.ch = 'm'; }
        else if (inside(x, y, dx + w - 200, dy + h - 16, 96, 12)) k.key = SI_ENTER;
        else if (inside(x, y, dx + w - 96, dy + h - 16, 88, 12)) k.key = SI_ESC;
        if (k.key)
            files_key(k);
        return 0;
    }
    for (int i = 0; i < NBTN; i++)
        if (inside(x, y, btn_x(i), FOOT_Y, BTN_W, 12)) {
            if (i == NBTN - 1) { open_ = 0; return 1; }
            do_button(i);
            return 0;
        }
    for (int i = 0; i < 2; i++) {
        int px = PX(i);
        if (inside(x, y, px, PLACE_Y, PW, 11)) {
            act = i;
            pop = POP_PLACE; pop_pane = i; pop_sel = pane[i].place; pop_top = 0;
            if (pop_sel >= POP_MAX) pop_top = pop_sel - POP_MAX + 1;
            return 0;
        }
        if (inside(x, y, px, LIST_Y, PW, NVIS * RH)) {
            struct pane *p = &pane[i];
            act = i;
            int k = p->top + (y - LIST_Y) / RH;
            if (k >= p->n)
                return 0;
            double t = now_s();
            int dbl = last_pane == i && last_k == k && t - last_t < 0.45;
            /* a press on an item may become a drag */
            drag = DR_PENDING; drag_pane = i; drag_k = k;
            drag_x = x; drag_y = y; drag_t = t;
            p->sel = k;
            last_pane = i; last_k = k; last_t = t;
            if (dbl) {
                last_k = -1;
                open_entry(p);
            }
            return 0;
        }
        /* the scroll marks */
        if (inside(x, y, px + PW - 14, LIST_Y - 11, 14, 10) && pane[i].top > 0) {
            pane[i].top -= NVIS - 1;
            if (pane[i].top < 0) pane[i].top = 0;
            pane[i].sel = pane[i].top;
            act = i;
            return 0;
        }
        if (inside(x, y, px + PW - 14, MSG_Y - 2, 14, 10) && pane[i].top + NVIS < pane[i].n) {
            pane[i].top += NVIS - 1;
            pane[i].sel = pane[i].top;
            act = i;
            clamp(&pane[i]);
            return 0;
        }
    }
    return 0;
}

/* ---- drag and drop -------------------------------------------------------
 * Press on an item and move: what is dragged is the marked items when the
 * item pressed is marked, else that item. Dropped on the other pane it goes
 * into that pane's folder; dropped on a folder (either pane, not one being
 * dragged) into that folder. The drop asks Copy, Move or Cancel. */

/* where a drop at x,y would go: 1 and *l, *label; 0 nowhere */
static int drop_target(int x, int y, struct loc *l, char *label, size_t n, int *hl_pane, int *hl_k)
{
    *hl_pane = -1; *hl_k = -1;
    for (int i = 0; i < 2; i++) {
        int px = PX(i);
        if (!inside(x, y, px, LIST_Y - 2, PW, NVIS * RH + 3))
            continue;
        struct pane *p = &pane[i];
        if (p->img && p->vol < 0)
            return 0;                                    /* a volume list */
        struct loc base;
        pane_loc(p, &base);
        int k = y >= LIST_Y ? p->top + (y - LIST_Y) / RH : -1;
        if (k >= 0 && k < p->n && p->e[k].dir && strcmp(p->e[k].name, "..") &&
            !(i == drag_pane && (p->e[k].mark || k == p->sel))) {
            if (loc_child(&base, p->e[k].name, l) != 0)
                return 0;
            snprintf(label, n, "%s", p->e[k].name);
            *hl_pane = i; *hl_k = k;
            return 1;
        }
        if (i == drag_pane)
            return 0;                                    /* its own folder */
        *l = base;
        snprintf(label, n, "the %s pane", i ? "right" : "left");
        *hl_pane = i;
        return 1;
    }
    return 0;
}

int files_motion(int x, int y, int held)
{
    if (view_is_open())
        return 0;
    if (drag == DR_NONE || pop)
        return 0;
    if (!held && drag == DR_PENDING) {
        /* the switch chattering on the press: not a release yet */
        if (now_s() - drag_t > 0.06)
            drag = DR_NONE;
        return 0;
    }
    if (drag == DR_PENDING) {
        int ddx = x - drag_x, ddy = y - drag_y;
        if (ddx * ddx + ddy * ddy < 36)
            return 0;
        struct pane *p = &pane[drag_pane];
        if (drag_k >= p->n || !strcmp(p->e[drag_k].name, "..") || (p->img && p->vol < 0)) {
            drag = DR_NONE;
            return 0;
        }
        act = drag_pane;
        if (!p->e[drag_k].mark) {                        /* just this one */
            for (int i = 0; i < p->n; i++)
                p->e[i].mark = 0;
            p->sel = drag_k;
        }
        drag = DR_ON;
        last_k = -1;                                     /* not a double-click */
    }
    struct loc l;
    char label[64];
    int hp, hk, ok = drop_target(x, y, &l, label, sizeof label, &hp, &hk);
    static int idx[MAX_ENT];
    int n = targets(&pane[drag_pane], idx, MAX_ENT);
    char m[96];
    if (ok)
        snprintf(m, sizeof m, "drop %d item%s into %.40s", n, n == 1 ? "" : "s", label);
    else
        snprintf(m, sizeof m, "dragging %d item%s", n, n == 1 ? "" : "s");
    int changed = hp != drop_pane || hk != drop_k || strcmp(m, msg);
    drop_pane = hp; drop_k = hk;
    snprintf(msg, sizeof msg, "%s", m);
    return changed;
}

int files_release(int x, int y)
{
    if (drag == DR_PENDING) {
        if (now_s() - drag_t > 0.06)                     /* a plain click */
            drag = DR_NONE;
        return 0;
    }
    if (drag != DR_ON)
        return 0;
    drag = DR_NONE;
    drop_pane = drop_k = -1;
    char label[64];
    int hp, hk;
    if (!drop_target(x, y, &drop_loc, label, sizeof label, &hp, &hk)) {
        msg[0] = '\0';
        return 1;
    }
    static int idx[MAX_ENT];
    int n = targets(&pane[act], idx, MAX_ENT);
    if (!n)
        return 1;
    msg[0] = '\0';
    cf_kind = CF_DROP;
    if (n == 1)
        snprintf(cf_text, sizeof cf_text, "%.20s into %.20s?", pane[act].e[idx[0]].name, label);
    else
        snprintf(cf_text, sizeof cf_text, "%d items into %.24s?", n, label);
    cf_text2[0] = '\0';
    if (drop_loc.img)
        snprintf(cf_text2, sizeof cf_text2, "Into the image: names become 8.3.");
    pop = POP_CONFIRM;
    return 1;
}

int files_hover(int x, int y)
{
    if (view_is_open())
        return 0;
    if (pop == POP_CHOICE)
        return ch_hover(x, y);
    if (pop != POP_PLACE)
        return 0;
    int px = PX(pop_pane), py = PLACE_Y + 12;
    int vis = nplace < POP_MAX ? nplace : POP_MAX;
    if (!inside(x, y, px, py + 2, PW, vis * RH))
        return 0;
    int k = pop_top + (y - py - 2) / RH;
    if (k != pop_sel && k >= 0 && k < nplace) {
        pop_sel = k;
        return 1;
    }
    return 0;
}

/* three rows a notch: the view moves, the cursor stays on screen */
int files_wheel(int x, int y, int n)
{
    if (view_is_open())
        return view_wheel(n);
    if (pop == POP_PLACE) {
        int max = nplace - POP_MAX;
        pop_top -= n;
        if (pop_top > max) pop_top = max;
        if (pop_top < 0) pop_top = 0;
        files_hover(x, y);                         /* the row now under it */
        return 1;
    }
    if (pop)
        return 0;
    for (int i = 0; i < 2; i++) {
        int px = PX(i);
        if (!inside(x, y, px, PLACE_Y, PW, MSG_Y - PLACE_Y))
            continue;
        struct pane *p = &pane[i];
        int max = p->n - NVIS;
        p->top -= 3 * n;
        if (p->top > max) p->top = max;
        if (p->top < 0) p->top = 0;
        if (p->sel < p->top) p->sel = p->top;
        if (p->sel >= p->top + NVIS) p->sel = p->top + NVIS - 1;
        if (p->sel >= p->n) p->sel = p->n ? p->n - 1 : 0;
        return 1;
    }
    return 0;
}

/* the test harness: how a pane looks after some keys */
void files_test_place(int pn, int pl)
{
    if (pl < nplace)
        set_place(pn, pl);
}
