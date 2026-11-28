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
#include <time.h>
#include <unistd.h>

#include "astos.h"
#include "astos_files.h"
#include "astos_fat.h"

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
#define BTN_W   72
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
struct ent { char name[256]; int dir; long long size; int mark; int vix; };
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
        e->dir = 1; e->size = 0; e->mark = 0;
        p->n = 1;
        if (p->vol < 0) {                     /* its volumes, in order */
            for (int i = 0; i < fat_nvol(p->img) && p->n < MAX_ENT; i++) {
                e = &p->e[p->n++];
                snprintf(e->name, sizeof e->name, "%s", fat_vol_label(p->img, i));
                e->dir = 1; e->size = 0; e->mark = 0; e->vix = i;
            }
        } else {
            static struct fat_ent fe[MAX_ENT];
            int n = fat_list(p->img, p->vol, p->irel, fe, MAX_ENT - 1);
            if (n < 0)
                snprintf(msg, sizeof msg, "%.90s", fat_error());
            for (int i = 0; i < n; i++) {
                e = &p->e[p->n++];
                snprintf(e->name, sizeof e->name, "%s", fe[i].name);
                e->dir = fe[i].dir; e->size = fe[i].size; e->mark = 0;
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
        p->e[0].mark = 0;
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
            e->dir = S_ISDIR(st.st_mode);
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
enum { POP_NONE, POP_PLACE, POP_ASK, POP_CONFIRM };
static int pop, pop_pane, pop_sel, pop_top;
enum { ASK_RENAME, ASK_MKDIR };
static int ask_kind;
static char ask_buf[200], ask_title[48];
enum { CF_DELETE, CF_COPY, CF_MOVE };
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

static void do_copy_move(int move)
{
    struct pane *s = &pane[act], *d = &pane[!act];
    struct loc sl, dl;
    if (need_volume(s) || need_volume(d))
        return;
    pane_loc(s, &sl);
    pane_loc(d, &dl);
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

static void do_ask(void)
{
    struct pane *p = &pane[act];
    if (name_bad(ask_buf)) {
        snprintf(msg, sizeof msg, "not a usable name");
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

/* into an image file, or say what it is */
static void enter_image(struct pane *p, const struct ent *e)
{
    char path[PATH_MAX], why[96];
    full_path(p, e->name, path, sizeof path);
    struct fat_img *im = e->size >= 512 ? fat_open(path, why, sizeof why) : NULL;
    if (!im) {
        snprintf(msg, sizeof msg, "%.40s - %lld bytes%s%.40s", e->name, e->size,
                 e->size >= 512 ? ", " : "", e->size >= 512 ? why : "");
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
            snprintf(msg, sizeof msg, "%.40s - %lld bytes", e->name, e->size);
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

static const char *const btn[] = { "Copy", "Move", "Rename", "Folder", "Delete", "Close" };
#define NBTN 6
static int btn_x(int i) { return 12 + i * (BTN_W + 6); }

static void size_text(const struct ent *e, char *b, size_t n)
{
    if (e->dir)                snprintf(b, n, "<DIR>");
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
    int len = (int)strlen(path), maxc = PW / 8;
    g_textn(x, PATH_Y, len > maxc ? path + len - maxc : path, maxc, C_DIM);
    /* the list */
    g_frame(x, LIST_Y - 2, PW, NVIS * RH + 3, i == act ? C_ACCENT : C_DIM);
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
        g_text(x + 6, y + 2, cf_kind == CF_DELETE ? "Delete" : cf_kind == CF_COPY ? "Copy" : "Move",
               C_WHITE);
        g_textn(x + 10, y + 16, cf_text, (w - 20) / 8, C_TEXT);
        if (cf_text2[0])
            g_textn(x + 10, y + 27, cf_text2, (w - 20) / 8, C_ACCENT);
        dlg_button(x + w - 200, y + h - 16, 96, cf_kind != CF_DELETE ? "OK" :
                   pane[act].img ? "Delete" : "To trash");
        dlg_button(x + w - 96, y + h - 16, 88, "Cancel");
    }
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
    g_textn(12, MSG_Y + 1, msg, 76, C_ACCENT);
    for (int i = 0; i < NBTN; i++)
        button(btn_x(i), btn[i]);
    g_text(btn_x(NBTN) + 2, FOOT_Y + 2, "Tab pane", C_DIM);
    if (pop)
        draw_pop();
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
    }
}

int files_key(struct si_event k)
{
    struct pane *p = &pane[act];
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
        if (inside(x, y, dx + w - 200, dy + h - 16, 96, 12)) k.key = SI_ENTER;
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

int files_hover(int x, int y)
{
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
