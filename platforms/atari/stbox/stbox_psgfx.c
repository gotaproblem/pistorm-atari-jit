/* SPDX-License-Identifier: MIT
 *
 * stbox_psgfx.c - render-thread half of PSGFX in STBOX (see stbox_psgfx.h).
 *
 * Runs on the STBOX render thread only (cores 0/1). Pipeline per game frame:
 *
 *   records (core 3 ring) -> game frame: camera-space faces with vertex
 *       identities, objects, the frame's draw buffer and projection
 *   wait until the box DISPLAYS that buffer (stbox_cap_meta[].base)
 *   lift against the displayed picture:
 *       face colours  - the ST's colour where each face is frontmost
 *                       (dither pairs become their mix)
 *       background    - the picture with the faces removed, sky/ground
 *                       band edges re-drawn as straight segments at 4x
 *       overlay       - viewport pixels nothing explains (crosshair, sparks)
 *       cockpit       - everything outside the 3D viewport, 4x
 *   match objects and vertices with the previous game frame
 *   every output tick: interpolate matched vertices in camera space, warp the
 *       background along the horizon, draw on the GPU with the game's own
 *       projection (depth buffer, 4x MSAA, lighting, fog)
 *
 * This is the C port of psgfx-scan's lift3d.py / interp3d.py / bands.py,
 * kept close to them so the offline tools remain the reference. The record
 * parser is per engine ("#lift sg2" in the profile); everything after the
 * game frame is engine-independent.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "stbox.h"
#include "stbox_psgfx.h"
#include "stbox_gl.h"

#define SCALE 4
#define OW (320 * SCALE)
#define OH (200 * SCALE)
#define MAXF 768                 /* faces per game frame                 */
#define MAXV 12                  /* vertices per face                    */
#define MAXOBJ 256
#define MAXSEG 16

/* ======================================================================== */
/* GL loading                                                               */
/* ======================================================================== */
#define X(ret, name, args) static ret (*p_##name) args;
STBOX_GL_FUNCS(X)
STBOX_EGL_FUNCS(X)
#undef X
static void *g_egl_lib;
static EGLDisplay g_dpy;
static EGLContext g_ctx;
static int g_gl_state;           /* 0 untried, 1 up, -1 failed            */
static int g_gl_current;         /* context current on the calling thread */

static int gl_up(void)
{
    if (g_gl_state > 0 && !g_gl_current)          /* new render thread after a restart */
        g_gl_current = p_eglMakeCurrent(g_dpy, NULL, NULL, g_ctx) ? 1 : 0;
    if (g_gl_state) return g_gl_state > 0 && g_gl_current;
    g_gl_state = -1;
    g_egl_lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_egl_lib) {
        fprintf(stderr, "[PSGFX] libEGL.so.1 not found (apt install libegl1 libgles2 "
                        "libgl1-mesa-dri) - GPU path off\n");
        return 0;
    }
#define X(ret, name, args) p_##name = (ret (*) args)dlsym(g_egl_lib, #name); \
    if (!p_##name) { fprintf(stderr, "[PSGFX] libEGL lacks %s\n", #name); return 0; }
    STBOX_EGL_FUNCS(X)
#undef X
    EGLDisplay (*gpd)(EGLenum, void *, const EGLint *) =
        (EGLDisplay (*)(EGLenum, void *, const EGLint *))p_eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!gpd) { fprintf(stderr, "[PSGFX] no eglGetPlatformDisplayEXT\n"); return 0; }
    g_dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    if (!g_dpy || !p_eglInitialize(g_dpy, NULL, NULL)) {
        fprintf(stderr, "[PSGFX] EGL surfaceless display failed (0x%x)\n", p_eglGetError());
        return 0;
    }
    p_eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cx[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
    g_ctx = p_eglCreateContext(g_dpy, NULL, NULL, cx);
    if (!g_ctx || !p_eglMakeCurrent(g_dpy, NULL, NULL, g_ctx)) {
        fprintf(stderr, "[PSGFX] GLES 3.1 context failed (0x%x)\n", p_eglGetError());
        return 0;
    }
#define X(ret, name, args) p_##name = (ret (*) args)p_eglGetProcAddress(#name); \
    if (!p_##name) p_##name = (ret (*) args)dlsym(RTLD_DEFAULT, #name); \
    if (!p_##name) { fprintf(stderr, "[PSGFX] GLES lacks %s\n", #name); return 0; }
    STBOX_GL_FUNCS(X)
#undef X
    fprintf(stderr, "[PSGFX] GPU: %s\n", (const char *)p_glGetString(GL_RENDERER));
    g_gl_state = 1; g_gl_current = 1;
    return 1;
}

/* ======================================================================== */
/* profiles                                                                 */
/* ======================================================================== */
#define MAXPROF 8
static ph_table *g_prof[MAXPROF];
static int g_nprof;
static ph_table *g_armed_prof;   /* the one whose expect bytes match      */
static int g_hz = 50;
static float g_focal = 128.0f;

static int load_profile(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f || g_nprof >= MAXPROF) { if (f) fclose(f); return -1; }
    ph_table *t = calloc(1, sizeof *t);
    const char *b = strrchr(path, '/');
    snprintf(t->name, sizeof t->name, "%s", b ? b + 1 : path);
    char ln[1024], err[160];
    int lineno = 0, bad = 0;
    while (fgets(ln, sizeof ln, f)) {
        lineno++;
        if (!strncmp(ln, "#lift ", 6)) { sscanf(ln + 6, "%31s", t->lift); continue; }
        if (ph_add(t, ln, err, sizeof err) < 0) {
            fprintf(stderr, "[PSGFX] %s:%d: %s\n", t->name, lineno, err);
            bad = 1;
        }
    }
    fclose(f);
    if (bad || !t->n || strcmp(t->lift, "sg2")) {
        if (!bad && strcmp(t->lift, "sg2"))
            fprintf(stderr, "[PSGFX] %s: no '#lift sg2' line - skipped (only the sg2 "
                            "engine is ported so far)\n", t->name);
        free(t);
        return -1;
    }
    g_prof[g_nprof++] = t;
    fprintf(stderr, "[PSGFX] profile %s: %d hooks, lift %s\n", t->name, t->n, t->lift);
    return 0;
}

int stbox_psgfx_hz(void) { return g_hz; }

int stbox_psgfx_init(void)
{
    const char *e = getenv("PISTORM_STBOX_PSGFX");
    const char *path = e && *e ? e : "configs/psgfx";
    if (!strcmp(path, "0")) return 0;
    const char *hz = getenv("PISTORM_STBOX_PSGFX_HZ");
    if (hz && atoi(hz) >= 25 && atoi(hz) <= 240) g_hz = atoi(hz);
    if (g_nprof) return 1;                       /* already loaded        */
    struct stat st;
    if (stat(path, &st)) {
        if (e && *e) fprintf(stderr, "[PSGFX] %s: not found\n", path);
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path); struct dirent *de;
        while (d && (de = readdir(d))) {
            size_t n = strlen(de->d_name);
            if (n > 6 && !strcmp(de->d_name + n - 6, ".hooks")) {
                char p[1024]; snprintf(p, sizeof p, "%s/%s", path, de->d_name);
                load_profile(p);
            }
        }
        if (d) closedir(d);
    } else load_profile(path);
    return g_nprof > 0;
}

/* ======================================================================== */
/* game frames from the record stream (engine: sg2 = Starglider 2)          */
/* ======================================================================== */
typedef struct { float x, y, z; } v3;
typedef struct {
    int nv;  float sx[MAXV], sy[MAXV]; v3 rc[MAXV];   /* what the ST rasterised */
    int nk;  int32_t key[MAXV]; v3 cam[MAXV];          /* identities + camera space */
    int obj; uint16_t word;
} face_t;
typedef struct { int kind; int32_t model; } obj_t;
typedef struct {
    int nf; face_t f[MAXF];
    int nobj; obj_t o[MAXOBJ];
    int have_info; uint32_t a6, buf; int cx, cy, clip[4];
    uint32_t vbl_end;
    int bad;
} gframe_t;

#define NPEND 4
static gframe_t *g_cur;                    /* being assembled              */
static gframe_t *g_pend[NPEND]; static int g_npend;   /* waiting to be shown */
static gframe_t *g_free[NPEND + 2]; static int g_nfree;

/* parser state */
static struct { uint32_t gen; v3 c; } *g_cache;   /* 65536 vertex-cache slots */
static uint32_t g_cache_gen = 1;
static face_t *g_face;                    /* face in progress (inside g_cur) */
static int g_cur_obj = -1;
static uint16_t g_fword;
static int g_pend34; static int32_t g_p34[3];
static uint32_t g_drops_seen;

static gframe_t *frame_get(void)
{
    gframe_t *f = g_nfree ? g_free[--g_nfree] : malloc(sizeof *f);
    f->nf = 0; f->nobj = 0; f->have_info = 0; f->bad = 0;
    return f;
}
static void frame_put(gframe_t *f) { if (f) { if (g_nfree < NPEND + 2) g_free[g_nfree++] = f; else free(f); } }

static void face_begin(uint16_t word)
{
    if (!g_cur || g_cur->nf >= MAXF) { g_face = NULL; return; }
    g_face = &g_cur->f[g_cur->nf];
    g_face->nv = g_face->nk = 0; g_face->obj = -1; g_face->word = word;
}
static void face_commit(int need_cams)
{
    face_t *fc = g_face; g_face = NULL;
    if (!fc || fc->nv < 3 || (need_cams && fc->nk < 3)) return;
    if (!need_cams && fc->nk < 3) return;
    fc->obj = g_cur_obj;
    g_cur->nf++;
}
static void new_obj(int kind, int32_t model)
{
    if (!g_cur || g_cur->nobj >= MAXOBJ) { g_cur_obj = -1; return; }
    g_cur->o[g_cur->nobj].kind = kind; g_cur->o[g_cur->nobj].model = model;
    g_cur_obj = g_cur->nobj++;
}

static void frame_end(uint32_t vbl)
{
    gframe_t *f = g_cur;
    g_cur = frame_get(); g_face = NULL; g_cur_obj = -1;
    uint32_t d = stbox_psgfx_drops;
    if (d != g_drops_seen) { f->bad = 1; g_drops_seen = d; }
    if (f->bad || !f->have_info || !f->nf) { frame_put(f); return; }
    f->vbl_end = vbl;
    if (g_npend == NPEND) { frame_put(g_pend[0]); memmove(g_pend, g_pend + 1, sizeof g_pend[0] * (NPEND - 1)); g_npend--; }
    g_pend[g_npend++] = f;
}

static void sg2_record(const stbox_psgfx_rec *r)
{
    const int32_t *a = r->a;
    if (!g_cur) { g_cur = frame_get(); g_cur->bad = 1; }   /* first frame after arming is partial */
    switch (r->op) {
    case 24: g_cache_gen++; new_obj(1, -1); break;
    case 39: new_obj(2, a[1]); break;
    case 20:
        face_begin((uint16_t)a[1]);
        if (g_cur_obj >= 0 && g_cur->o[g_cur_obj].kind == 1 && g_cur->o[g_cur_obj].model == -1)
            g_cur->o[g_cur_obj].model = a[2];               /* first face names the model */
        break;
    case 21: { unsigned i = (unsigned)a[0] & 0xffff;
        g_cache[i].gen = g_cache_gen; g_cache[i].c = (v3){ (float)a[1], (float)a[2], (float)a[3] }; break; }
    case 22: if (g_face) {
        unsigned i = (unsigned)a[0] & 0xffff;
        if (g_cache[i].gen != g_cache_gen) { g_face = NULL; break; }
        if (g_face->nv < MAXV) {
            v3 c = g_cache[i].c;
            g_face->sx[g_face->nv] = (float)a[1]; g_face->sy[g_face->nv] = (float)a[2]; g_face->rc[g_face->nv] = c; g_face->nv++;
            g_face->key[g_face->nk] = (int32_t)(0x10000 | i); g_face->cam[g_face->nk] = c; g_face->nk++;
        }
    } break;
    case 23: face_commit(0); break;
    case 30: face_begin((uint16_t)a[0]); g_fword = (uint16_t)a[0]; break;
    case 31: if (g_face && g_face->nv < MAXV) {
        v3 c = { (float)a[1], (float)a[2], (float)a[3] };
        g_face->sx[g_face->nv] = (float)a[4]; g_face->sy[g_face->nv] = (float)a[5]; g_face->rc[g_face->nv] = c; g_face->nv++;
        g_face->key[g_face->nk] = (int32_t)(0x20000 | ((unsigned)a[0] & 0xffff)); g_face->cam[g_face->nk] = c; g_face->nk++;
    } break;
    case 36: face_begin(g_fword); break;                  /* path B: the game clips this face */
    case 38: if (g_face && g_face->nk < MAXV) {
        g_face->key[g_face->nk] = (int32_t)(0x20000 | ((unsigned)a[0] & 0xffff));
        g_face->cam[g_face->nk] = (v3){ (float)a[1], (float)a[2], (float)a[3] }; g_face->nk++;
    } break;
    case 34: if (g_face) { g_pend34 = 1; g_p34[0] = a[0]; g_p34[1] = a[1]; g_p34[2] = a[2]; } break;
    case 37: if (g_face && g_pend34 && g_face->nv < MAXV) {
        g_face->sx[g_face->nv] = (float)a[0]; g_face->sy[g_face->nv] = (float)a[1];
        g_face->rc[g_face->nv] = (v3){ g_p34[0] * 144.0f / 128.0f, g_p34[1] * 49.0f / 128.0f, (float)g_p34[2] };
        g_face->nv++; g_pend34 = 0;
    } break;
    case 32: case 35: face_commit(1); break;
    case 40:
        g_cur->have_info = 1; g_cur->a6 = (uint32_t)a[0]; g_cur->buf = (uint32_t)a[1] & 0xffffff;
        g_cur->cx = a[2]; g_cur->cy = a[3];
        g_cur->clip[0] = a[4]; g_cur->clip[1] = a[5]; g_cur->clip[2] = a[6]; g_cur->clip[3] = a[7];
        break;
    case 41: frame_end(r->vbl); break;
    }
}

/* ======================================================================== */
/* the displayed picture                                                    */
/* ======================================================================== */
static const uint8_t lvl3[8] = { 0, 36, 73, 109, 146, 182, 219, 255 };
static void pal_rgb(uint16_t p, int ste, uint8_t out[3])
{
    if (ste) {
        #define STE_LVL(n) ((uint8_t)(((((n) & 7) << 1) | (((n) >> 3) & 1)) * 17))
        out[0] = STE_LVL(p >> 8); out[1] = STE_LVL(p >> 4); out[2] = STE_LVL(p);
        #undef STE_LVL
    } else { out[0] = lvl3[(p >> 8) & 7]; out[1] = lvl3[(p >> 4) & 7]; out[2] = lvl3[p & 7]; }
}
/* low-res capture -> 320x200 colour indices + per-row RGB palettes */
static void cap_decode(const stbox_cap_t *c, uint8_t idx[200][320], uint8_t rowpal[200][16][3])
{
    int ste = stbox_shared.ste;
    for (int y = 0; y < 200; y++) {
        const uint8_t *s = c->pix + (size_t)y * STBOX_CAP_PITCH;
        for (int g = 0; g < 20; g++, s += 8) {
            uint16_t p0 = (s[0] << 8) | s[1], p1 = (s[2] << 8) | s[3], p2 = (s[4] << 8) | s[5], p3 = (s[6] << 8) | s[7];
            for (int b = 15; b >= 0; b--)
                idx[y][g * 16 + 15 - b] = (uint8_t)(((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) |
                                                   (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3));
        }
        for (int i = 0; i < 16; i++) pal_rgb(c->pal[y][i], ste, rowpal[y][i]);
    }
}

/* ======================================================================== */
/* lifted frame                                                             */
/* ======================================================================== */
typedef struct { float a, b, xa, xb; int c1, c2; int n; } seg_t;
typedef struct {
    int nseg; seg_t s[MAXSEG];
    float cx; int w, h;
    int hz_ok; float ha, hb;                 /* horizon: topmost long segment */
} bands_t;
typedef struct {
    int nk; int32_t key[MAXF]; v3 cam[MAXF]; v3 ctr;   /* object vertex table */
} otab_t;
typedef struct {
    gframe_t *g;
    float rgb[MAXF][3]; uint8_t has_col[MAXF];
    uint8_t *st, *ov;                        /* 320x200 RGBA: displayed picture, overlay */
    uint8_t *vpbg; int vpw, vph;             /* viewport background at 4x, RGBA */
    bands_t bands;
    float sky[3];
    uint32_t shown;                          /* VBL it was first displayed     */
    int vp[4];                               /* viewport in ST pixels x0 y0 x1 y1 */
    otab_t *ot; int not;                     /* per object                     */
    /* interpolation towards the NEXT frame (filled when it arrives) */
    uint8_t ok[MAXF]; v3 tgt[MAXF][MAXV];
    int gen;                                 /* GL texture upload stamp       */
} lifted_t;

static lifted_t g_lf[3];
static lifted_t *g_A, *g_B, *g_spare;
static uint32_t g_lift_count, g_lift_skip, g_interp_faces, g_total_faces;
static double g_lift_ms, g_render_ms, g_sec[8]; static uint64_t g_recs; static uint32_t g_render_count;
static double mono_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6; }

static uint8_t g_idx[200][320], g_rowpal[200][16][3];
static int16_t g_ids[200][320];

/* ---- polygon fill (PIL-like: interior + outline), owner id per pixel ---- */
static void put_id(int x, int y, int id, const int vp[4])
{
    if (x < vp[0] || x > vp[2] || y < vp[1] || y > vp[3]) return;
    g_ids[y][x] = (int16_t)id;
}
static void line_id(float fx0, float fy0, float fx1, float fy1, int id, const int vp[4])
{
    int x0 = (int)lrintf(fx0), y0 = (int)lrintf(fy0), x1 = (int)lrintf(fx1), y1 = (int)lrintf(fy1);
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (int guard = 0; guard < 2000; guard++) {
        put_id(x0, y0, id, vp);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}
static void poly_id(const face_t *fc, int id, const int vp[4])
{
    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < fc->nv; i++) { if (fc->sy[i] < ymin) ymin = fc->sy[i]; if (fc->sy[i] > ymax) ymax = fc->sy[i]; }
    int y0 = (int)ceilf(ymin), y1 = (int)floorf(ymax);
    if (y0 < vp[1]) y0 = vp[1];
    if (y1 > vp[3]) y1 = vp[3];
    for (int y = y0; y <= y1; y++) {
        float xs[MAXV]; int n = 0;
        for (int i = 0; i < fc->nv; i++) {
            int j = (i + 1) % fc->nv;
            float ya = fc->sy[i], yb = fc->sy[j];
            if ((ya <= y && yb > y) || (yb <= y && ya > y)) {
                float t = (y - ya) / (yb - ya);
                xs[n++] = fc->sx[i] + t * (fc->sx[j] - fc->sx[i]);
            }
        }
        for (int i = 1; i < n; i++) { float v = xs[i]; int k = i - 1; while (k >= 0 && xs[k] > v) { xs[k + 1] = xs[k]; k--; } xs[k + 1] = v; }
        for (int i = 0; i + 1 < n; i += 2) {
            int xa = (int)ceilf(xs[i]), xb = (int)floorf(xs[i + 1]);
            for (int x = xa; x <= xb; x++) put_id(x, y, id, vp);
        }
    }
    for (int i = 0; i < fc->nv; i++) {
        int j = (i + 1) % fc->nv;
        line_id(fc->sx[i], fc->sy[i], fc->sx[j], fc->sy[j], id, vp);
    }
}

/* ---- band edges (bands.py fit + render_hd) ------------------------------ */
static uint32_t g_rng = 1;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static void bands_fit(bands_t *B, const uint8_t (*bgp)[320], const int vp[4])
{
    int x0 = vp[0], y0 = vp[1], w = vp[2] - vp[0] + 1, h = vp[3] - vp[1] + 1;
    float cx = w / 2.0f;
    B->nseg = 0; B->cx = cx; B->w = w; B->h = h; B->hz_ok = 0;
    static int pc[256]; static uint8_t pair_seen[256]; int order[256], norder = 0;
    memset(pc, 0, sizeof pc); memset(pair_seen, 0, sizeof pair_seen);
    for (int y = 0; y + 1 < h; y++)
        for (int x = 0; x < w; x++) {
            int u = bgp[y0 + y][x0 + x], d = bgp[y0 + y + 1][x0 + x];
            if (u != d) { int k = u * 16 + d; if (!pair_seen[k]) { pair_seen[k] = 1; order[norder++] = k; } pc[k]++; }
        }
    static float xs[320 * 200], ys[320 * 200]; static uint8_t live[320 * 200], inl[320 * 200], best_inl[320 * 200];
    seg_t cand[64]; int ncand = 0;
    g_rng = 1;
    for (int oi = 0; oi < norder; oi++) {
        int k = order[oi], c1 = k >> 4, c2 = k & 15;
        if (pc[k] < 0.12f * w) continue;
        int n = 0;
        for (int y = 0; y + 1 < h; y++)
            for (int x = 0; x < w; x++)
                if (bgp[y0 + y][x0 + x] == c1 && bgp[y0 + y + 1][x0 + x] == c2) { ys[n] = y + 1.0f; xs[n] = x - cx + 0.5f; n++; }
        memset(live, 1, (size_t)n);
        for (int ln = 0; ln < 3 && ncand < 64; ln++) {
            int nlive = 0; for (int i = 0; i < n; i++) nlive += live[i];
            if (nlive < 0.12f * w) break;
            int best = -1;
            int *li = malloc(sizeof(int) * (size_t)nlive); int m = 0;
            for (int i = 0; i < n; i++) if (live[i]) li[m++] = i;
            for (int it = 0; it < 300; it++) {
                int i = li[rnd() % (unsigned)m], j = li[rnd() % (unsigned)m];
                if (i == j || fabsf(xs[i] - xs[j]) < 8) continue;
                float aa = (ys[j] - ys[i]) / (xs[j] - xs[i]), bb = ys[i] - aa * xs[i];
                int cnt = 0;
                for (int q = 0; q < m; q++) { int p = li[q]; inl[p] = fabsf(aa * xs[p] + bb - ys[p]) < 1.2f; cnt += inl[p]; }
                if (cnt > best) { best = cnt; for (int q = 0; q < m; q++) best_inl[li[q]] = inl[li[q]]; }
            }
            free(li);
            if (best < 0.12f * w) break;
            /* least squares on the inliers, retire them */
            double sx = 0, sy = 0, sxx = 0, sxy = 0; int cn = 0;
            static uint8_t colhit[320]; memset(colhit, 0, sizeof colhit);
            for (int i = 0; i < n; i++) if (live[i] && best_inl[i]) {
                sx += xs[i]; sy += ys[i]; sxx += (double)xs[i] * xs[i]; sxy += (double)xs[i] * ys[i]; cn++;
                int col = (int)floorf(xs[i] + cx); if (col >= 0 && col < 320) colhit[col] = 1;
                live[i] = 0;
            }
            double den = cn * sxx - sx * sx;
            if (cn < 2 || fabs(den) < 1e-9) continue;
            float aa = (float)((cn * sxy - sx * sy) / den), bb = (float)((sy - aa * sx) / cn);
            /* span: longest run of inlier columns, gaps up to 24 px allowed */
            int bxa = 0, bxb = 0, ra = -1, last = -100;
            for (int c = 0; c < w; c++) if (colhit[c]) {
                if (ra < 0) ra = c;
                else if (c - last > 24) { if (last + 1 - ra > bxb - bxa) { bxa = ra; bxb = last + 1; } ra = c; }
                last = c;
            }
            if (ra >= 0 && last + 1 - ra > bxb - bxa) { bxa = ra; bxb = last + 1; }
            if (bxb - bxa < 0.12f * w) continue;
            cand[ncand++] = (seg_t){ aa, bb, (float)bxa, (float)bxb, c1, c2, cn };
        }
    }
    /* dedupe, strongest first */
    for (int i = 1; i < ncand; i++) { seg_t v = cand[i]; int k = i - 1; while (k >= 0 && cand[k].n < v.n) { cand[k + 1] = cand[k]; k--; } cand[k + 1] = v; }
    for (int i = 0; i < ncand && B->nseg < MAXSEG; i++) {
        seg_t *s = &cand[i]; int dup = 0;
        for (int j = 0; j < B->nseg && !dup; j++) {
            seg_t *t = &B->s[j];
            float lo = fmaxf(s->xa, t->xa), hi = fminf(s->xb, t->xb);
            if (hi - lo < 0.5f * (s->xb - s->xa)) continue;
            float e0 = fabsf((s->a - t->a) * (lo - cx) + s->b - t->b), e1 = fabsf((s->a - t->a) * (hi - cx) + s->b - t->b);
            if (fmaxf(e0, e1) < 2.0f) dup = 1;
        }
        if (!dup) B->s[B->nseg++] = *s;
    }
    for (int i = 0; i < B->nseg; i++) {
        if (B->s[i].xa < 10) B->s[i].xa = 0;
        if (B->s[i].xb > w - 10) B->s[i].xb = (float)w;
    }
    for (int i = 0; i < B->nseg; i++)
        for (int j = 0; j < B->nseg; j++) {
            seg_t *s = &B->s[i], *t = &B->s[j];
            if (i == j || s->a == t->a) continue;
            float xi = (t->b - s->b) / (s->a - t->a) + cx;
            if (fabsf(xi - s->xb) < 24 && fabsf(xi - t->xa) < 24) s->xb = t->xa = xi;
        }
    float bestb = 1e9f;
    for (int i = 0; i < B->nseg; i++)
        if (B->s[i].xb - B->s[i].xa > 0.6f * w && B->s[i].b < bestb) {
            bestb = B->s[i].b; B->hz_ok = 1; B->ha = B->s[i].a; B->hb = B->s[i].b;
        }
}

/* viewport background at 4x: pixels upscaled (x8, then 2x2 box), every
 * segment re-drawn as a straight edge touching only its two colours.
 * dst = (w*4) x (h*4) RGBA */
static void bands_render(const bands_t *B, const uint8_t (*bgp)[320], const int vp[4],
                         const uint8_t (*pal)[16][3], uint8_t *dst)
{
    const int S = SCALE * 2, w = B->w, h = B->h, x0 = vp[0], y0 = vp[1], OWv = w * S;
    static uint8_t out[200 * 8][320 * 8];
    for (int y = 0; y < h; y++) {
        uint8_t *r = out[y * S];
        for (int x = 0; x < w; x++) memset(r + x * S, bgp[y0 + y][x0 + x], S);
        for (int k = 1; k < S; k++) memcpy(out[y * S + k], r, (size_t)OWv);
    }
    for (int i = 0; i < B->nseg; i++) {
        const seg_t *s = &B->s[i];
        int xa = (int)(s->xa * S), xb = (int)(s->xb * S);
        if (xa < 0) xa = 0;
        if (xb > OWv) xb = OWv;
        for (int col = xa; col < xb; col++) {
            float X = (col + 0.5f) / S - B->cx, yl = s->a * X + s->b;
            int r0 = (int)fmaxf(0, (yl - 2.0f) * S), r1 = (int)fminf((float)(h * S), (yl + 2.0f) * S);
            for (int r = r0; r < r1; r++) {
                uint8_t v = out[r][col];
                if (v != s->c1 && v != s->c2) continue;
                out[r][col] = ((r + 0.5f) / S < yl) ? (uint8_t)s->c1 : (uint8_t)s->c2;
            }
        }
    }
    const int dw = w * SCALE;
    for (int y = 0; y < h * SCALE; y++) {
        const uint8_t (*pr)[3] = pal[y0 + y / SCALE];
        const uint8_t *u = out[y * 2], *v = out[y * 2 + 1];
        uint8_t *d = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; x++, d += 4, u += 2, v += 2) {
            if (u[0] == u[1] && u[0] == v[0] && u[0] == v[1]) {
                const uint8_t *c = pr[u[0]]; d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = 255; continue;
            }
            const uint8_t *a = pr[u[0]], *b = pr[u[1]], *c = pr[v[0]], *e = pr[v[1]];
            d[0] = (uint8_t)((a[0] + b[0] + c[0] + e[0]) >> 2);
            d[1] = (uint8_t)((a[1] + b[1] + c[1] + e[1]) >> 2);
            d[2] = (uint8_t)((a[2] + b[2] + c[2] + e[2]) >> 2); d[3] = 255;
        }
    }
}

/* ---- object tables ------------------------------------------------------ */
static void build_otabs(lifted_t *L)
{
    gframe_t *g = L->g;
    if (!L->ot) L->ot = calloc(MAXOBJ + 1, sizeof(otab_t));
    L->not = g->nobj + 1;                    /* slot nobj = faces with no object */
    for (int o = 0; o < L->not; o++) L->ot[o].nk = 0;
    for (int i = 0; i < g->nf; i++) {
        face_t *fc = &g->f[i];
        otab_t *t = &L->ot[fc->obj >= 0 && fc->obj < g->nobj ? fc->obj : g->nobj];
        for (int k = 0; k < fc->nk; k++) {
            int q = 0;
            while (q < t->nk && t->key[q] != fc->key[k]) q++;
            if (q == t->nk) { if (t->nk >= MAXF) continue; t->key[t->nk++] = fc->key[k]; }
            t->cam[q] = fc->cam[k];
        }
    }
    for (int o = 0; o < L->not; o++) {
        otab_t *t = &L->ot[o]; v3 c = { 0, 0, 0 };
        for (int k = 0; k < t->nk; k++) { c.x += t->cam[k].x; c.y += t->cam[k].y; c.z += t->cam[k].z; }
        if (t->nk) { c.x /= t->nk; c.y /= t->nk; c.z /= t->nk; }
        t->ctr = c;
    }
}
static int otab_find(const otab_t *t, int32_t key)
{
    for (int q = 0; q < t->nk; q++) if (t->key[q] == key) return q;
    return -1;
}
static int keyset_eq(const otab_t *a, const otab_t *b)
{
    if (a->nk != b->nk) return 0;
    for (int q = 0; q < a->nk; q++) if (otab_find(b, a->key[q]) < 0) return 0;
    return 1;
}
static float v3len(v3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }

/* match A's objects to B's (interp3d.match_objects) and mark A's faces
 * whose every vertex has a partner in B */
static void match_frames(lifted_t *A, lifted_t *B)
{
    gframe_t *ga = A->g, *gb = B->g;
    static int omap[MAXOBJ + 1]; static uint8_t used[MAXOBJ + 1];
    typedef struct { float d; int a, b; } pr_t;
    static pr_t prs[(MAXOBJ + 1) * 8]; int np = 0;
    for (int o = 0; o < A->not; o++) omap[o] = -1;
    memset(used, 0, sizeof used);
    for (int oa = 0; oa < ga->nobj; oa++) {
        if (!A->ot[oa].nk) continue;
        for (int ob = 0; ob < gb->nobj; ob++) {
            if (!B->ot[ob].nk) continue;
            int same = ga->o[oa].kind == gb->o[ob].kind && ga->o[oa].model == gb->o[ob].model;
            if (!same && !keyset_eq(&A->ot[oa], &B->ot[ob])) continue;
            v3 ca = A->ot[oa].ctr, cb = B->ot[ob].ctr;
            float d = v3len((v3){ ca.x - cb.x, ca.y - cb.y, ca.z - cb.z }), r = v3len(ca) * 0.25f + 400.0f;
            if (d < r && np < (int)(sizeof prs / sizeof prs[0])) prs[np++] = (pr_t){ d * (same ? 1.0f : 1.5f), oa, ob };
        }
    }
    for (int i = 1; i < np; i++) { pr_t v = prs[i]; int k = i - 1; while (k >= 0 && prs[k].d > v.d) { prs[k + 1] = prs[k]; k--; } prs[k + 1] = v; }
    for (int i = 0; i < np; i++) {
        if (omap[prs[i].a] >= 0 || used[prs[i].b]) continue;
        omap[prs[i].a] = prs[i].b; used[prs[i].b] = 1;
    }
    int nok = 0;
    for (int i = 0; i < ga->nf; i++) {
        face_t *fc = &ga->f[i];
        A->ok[i] = 0;
        int oa = fc->obj;
        if (oa < 0 || oa >= ga->nobj || omap[oa] < 0) continue;
        const otab_t *tb = &B->ot[omap[oa]];
        int good = 1;
        for (int k = 0; k < fc->nk && good; k++) {
            int q = otab_find(tb, fc->key[k]);
            if (q < 0) { good = 0; break; }
            v3 c = fc->cam[k], t = tb->cam[q];
            if (v3len((v3){ t.x - c.x, t.y - c.y, t.z - c.z }) >= 0.35f * v3len(c) + 300.0f) good = 0;
            else A->tgt[i][k] = t;
        }
        A->ok[i] = (uint8_t)good; nok += good;
    }
    g_interp_faces += (uint32_t)nok; g_total_faces += (uint32_t)ga->nf;
}

/* Overlay pixels identical in two consecutive game frames are screen-fixed
 * (crosshair, HUD marks) and stay in the overlay. The rest (distant ridge
 * lines, lasers, sparks) belong to the scene: they move into each frame's
 * viewport background and follow the horizon warp with it. */
static void move_px(lifted_t *L, int x, int y, const uint8_t *c)
{
    int lx = (x - L->vp[0]) * SCALE, ly = (y - L->vp[1]) * SCALE;
    for (int yy = 0; yy < SCALE; yy++) {
        uint8_t *d = L->vpbg + ((size_t)(ly + yy) * L->vpw + lx) * 4;
        for (int xx = 0; xx < SCALE; xx++, d += 4) { d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = 255; }
    }
}
static void overlay_split(lifted_t *A, lifted_t *B)
{
    for (int y = A->vp[1]; y <= A->vp[3]; y++)
        for (int x = A->vp[0]; x <= A->vp[2]; x++) {
            uint8_t *a = A->ov + ((size_t)y * 320 + x) * 4, *b = B->ov + ((size_t)y * 320 + x) * 4;
            int same = a[3] && b[3] && a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
            if (same) continue;
            if (a[3]) { move_px(A, x, y, a); a[3] = 0; }
            if (b[3] && y >= B->vp[1] && y <= B->vp[3] && x >= B->vp[0] && x <= B->vp[2]) { move_px(B, x, y, b); b[3] = 0; }
        }
}

/* ---- lift one game frame against the picture the box displayed ---------- */
static void lift_frame(lifted_t *L, gframe_t *g, const stbox_cap_t *cap, uint32_t shown)
{
    L->g = g; L->shown = shown;
    double tq = mono_ms(), tn;
#define SEC(i) (tn = mono_ms(), g_sec[i] += tn - tq, tq = tn)
    cap_decode(cap, g_idx, g_rowpal);
    int vp[4] = { g->clip[0], g->clip[1], g->clip[2], g->clip[3] };
    if (vp[0] < 0) vp[0] = 0;
    if (vp[1] < 0) vp[1] = 0;
    if (vp[2] > 319) vp[2] = 319;
    if (vp[3] > 199) vp[3] = 199;
    memcpy(L->vp, vp, sizeof vp);
    const int my = (vp[1] + vp[3]) / 2;
    const uint8_t (*pal)[3] = g_rowpal[my];

    SEC(0);
    /* owners, painter's order (far first) with the ST's own screen coordinates */
    for (int y = 0; y < 200; y++) for (int x = 0; x < 320; x++) g_ids[y][x] = -1;
    static int ord[MAXF]; static float zm[MAXF];
    for (int i = 0; i < g->nf; i++) {
        float z = 0; for (int k = 0; k < g->f[i].nv; k++) z += g->f[i].rc[k].z;
        zm[i] = z / g->f[i].nv; ord[i] = i;
    }
    for (int i = 1; i < g->nf; i++) { int v = ord[i], k = i - 1; while (k >= 0 && zm[ord[k]] < zm[v]) { ord[k + 1] = ord[k]; k--; } ord[k + 1] = v; }
    for (int i = 0; i < g->nf; i++) poly_id(&g->f[ord[i]], ord[i], vp);

    SEC(1);
    /* face colours: most common ST colour where the face is frontmost; a
     * second colour with >= 30% of the first is a dither -> mixed colour */
    static int cnt[MAXF][16];
    memset(cnt, 0, sizeof(cnt[0]) * (size_t)g->nf);
    for (int y = vp[1]; y <= vp[3]; y++) for (int x = vp[0]; x <= vp[2]; x++)
        if (g_ids[y][x] >= 0) cnt[g_ids[y][x]][g_idx[y][x]]++;
    static int col1[MAXF]; uint16_t facecols = 0;
    for (int i = 0; i < g->nf; i++) {
        int c1 = -1, c2 = -1;
        for (int c = 0; c < 16; c++) {
            if (!cnt[i][c]) continue;
            if (c1 < 0 || cnt[i][c] > cnt[i][c1]) { c2 = c1; c1 = c; }
            else if (c2 < 0 || cnt[i][c] > cnt[i][c2]) c2 = c;
        }
        col1[i] = c1; L->has_col[i] = c1 >= 0;
        if (c1 < 0) continue;
        facecols |= (uint16_t)(1u << c1);
        if (c2 >= 0 && cnt[i][c2] >= 0.3f * cnt[i][c1]) {
            float w0 = (float)cnt[i][c1], w1 = (float)cnt[i][c2];
            facecols |= (uint16_t)(1u << c2);
            for (int k = 0; k < 3; k++) L->rgb[i][k] = (pal[c1][k] * w0 + pal[c2][k] * w1) / (w0 + w1) / 255.0f;
        } else for (int k = 0; k < 3; k++) L->rgb[i][k] = pal[c1][k] / 255.0f;
    }

    SEC(2);
    /* background plane: faces removed, each covered pixel takes the nearest
     * uncovered viewport pixel of its row */
    static uint8_t bgp[200][320];
    memcpy(bgp, g_idx, sizeof bgp);
    for (int y = vp[1]; y <= vp[3]; y++) {
        int any = 0; for (int x = vp[0]; x <= vp[2]; x++) if (g_ids[y][x] >= 0) { any = 1; break; }
        if (!any) continue;
        int freex[320], nfree = 0;
        for (int x = vp[0]; x <= vp[2]; x++) if (g_ids[y][x] < 0) freex[nfree++] = x;
        if (!nfree) continue;
        int j = 0;
        for (int x = vp[0]; x <= vp[2]; x++) {
            if (g_ids[y][x] < 0) continue;
            while (j < nfree && freex[j] < x) j++;
            int jr = j < nfree ? j : nfree - 1, jl = j > 0 ? j - 1 : 0;
            int pick = abs(freex[jr] - x) < abs(freex[jl] - x) ? freex[jr] : freex[jl];
            bgp[y][x] = g_idx[y][pick];
        }
    }

    SEC(3);
    /* overlay: viewport pixels that neither the background nor a face
     * explains, in colours the faces/background don't use (crosshair, lasers) */
    int bgcnt[16] = { 0 };
    for (int y = vp[1]; y <= vp[3]; y++) for (int x = vp[0]; x <= vp[2]; x++)
        if (g_ids[y][x] < 0) bgcnt[bgp[y][x]]++;
    uint16_t notodd = facecols;
    for (int c = 0; c < 16; c++) if (bgcnt[c] > 200) notodd |= (uint16_t)(1u << c);
    memset(L->ov, 0, 320 * 200 * 4);
    for (int y = vp[1]; y <= vp[3]; y++) for (int x = vp[0]; x <= vp[2]; x++) {
        int id = g_ids[y][x];
        uint8_t fin = g_idx[y][x], reb = id >= 0 && col1[id] >= 0 ? (uint8_t)col1[id] : bgp[y][x];
        if (reb == fin || (notodd >> fin) & 1) continue;
        if (id < 0) {                          /* edge pixels of faces belong to the faces */
            int near = (y > 0 && g_ids[y - 1][x] >= 0) || (y < 199 && g_ids[y + 1][x] >= 0) ||
                       (x > 0 && g_ids[y][x - 1] >= 0) || (x < 319 && g_ids[y][x + 1] >= 0);
            if (near) continue;
        }
        const uint8_t *c = g_rowpal[y][fin]; uint8_t *d = L->ov + ((size_t)y * 320 + x) * 4;
        d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = 255;
    }

    SEC(4);
    /* the displayed picture at ST resolution (cockpit, panel: the GPU scales
     * it), and the viewport's background from the band model at 4x */
    for (int y = 0; y < 200; y++) for (int x = 0; x < 320; x++) {
        const uint8_t *c = g_rowpal[y][g_idx[y][x]]; uint8_t *d = L->st + ((size_t)y * 320 + x) * 4;
        d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = 255;
    }
    SEC(5);
    bands_fit(&L->bands, (const uint8_t (*)[320])bgp, vp);
    SEC(6);
    L->vpw = (vp[2] - vp[0] + 1) * SCALE; L->vph = (vp[3] - vp[1] + 1) * SCALE;
    bands_render(&L->bands, (const uint8_t (*)[320])bgp, vp, (const uint8_t (*)[16][3])g_rowpal, L->vpbg);
    SEC(7);
    const uint8_t *sk = g_rowpal[vp[1] + 2][bgp[vp[1] + 2][(vp[0] + vp[2]) / 2]];
    for (int k = 0; k < 3; k++) L->sky[k] = sk[k] / 255.0f;
    build_otabs(L);
    L->gen++;
#undef SEC
    if (getenv("PSGFX_PROF") && g_lift_count % 100 == 99)
        fprintf(stderr, "[PSGFX] lift sections ms: dec %.2f own %.2f col %.2f bgp %.2f ov %.2f full %.2f fit %.2f rend %.2f\n",
                g_sec[0] / 100, g_sec[1] / 100, g_sec[2] / 100, g_sec[3] / 100, g_sec[4] / 100, g_sec[5] / 100, g_sec[6] / 100, g_sec[7] / 100),
        memset(g_sec, 0, sizeof g_sec);
    g_lift_count++;
}

/* ======================================================================== */
/* GPU                                                                      */
/* ======================================================================== */
static const char *VS_Q =
    "#version 310 es\nlayout(location=0) in vec2 p; uniform float uFlip;\n"
    "void main(){ gl_Position = vec4(p.x, p.y * uFlip, 0.0, 1.0); }\n";
/* background: cockpit straight from frame A; the 3D viewport is frame A
 * warped forward along the horizon by t and frame B warped back by 1-t,
 * cross-faded (bands.tween) */
static const char *FS_BG =
    "#version 310 es\nprecision highp float;\n"
    "uniform sampler2D tSt, tA, tB; uniform vec4 uVP; uniform vec3 uMa0, uMa1, uMb0, uMb1;\n"
    "uniform float uT; uniform int uBGRA; uniform vec2 uWH; out vec4 o;\n"
    "vec4 samp(sampler2D t, vec2 s){ vec2 sz = uVP.zw - uVP.xy; s = clamp(s, vec2(0.0), sz - 1.0);\n"
    "  return texture(t, (s + 0.5) / sz); }\n"
    "void main(){ vec2 p = gl_FragCoord.xy - 0.5; vec4 c;\n"
    "  if (p.x >= uVP.x && p.x < uVP.z && p.y >= uVP.y && p.y < uVP.w) {\n"
    "    vec3 q = vec3(p - uVP.xy, 1.0);\n"
    "    c = mix(samp(tA, vec2(dot(uMa0, q), dot(uMa1, q))), samp(tB, vec2(dot(uMb0, q), dot(uMb1, q))), uT);\n"
    "  } else c = texture(tSt, (p + 0.5) / uWH);\n"
    "  c.a = 1.0; o = uBGRA == 1 ? c.bgra : c; }\n";
static const char *FS_OV =
    "#version 310 es\nprecision mediump float; uniform sampler2D t; uniform int uBGRA; uniform vec2 uWH; out vec4 o;\n"
    "void main(){ vec4 c = texture(t, gl_FragCoord.xy / uWH); o = uBGRA == 1 ? c.bgra : c; }\n";  /* t: 320x200, nearest */
/* camera space: x right, y down, z forward; screen = f*x/z + c (the game's own projection) */
static const char *VS_M =
    "#version 310 es\nlayout(location=0) in vec3 p; layout(location=1) in vec3 n; layout(location=2) in vec3 c;\n"
    "uniform vec2 uC; uniform float uF; uniform vec2 uWH; uniform vec2 uZ; uniform float uFlip;\n"
    "out vec3 vc; out vec3 vn; out float vz; out vec3 vp;\n"
    "void main(){ vec2 s = uF * p.xy / p.z + uC; vec2 ndc = vec2(s.x / uWH.x * 2.0 - 1.0, 1.0 - s.y / uWH.y * 2.0);\n"
    "  float zn = (p.z - uZ.x) / (uZ.y - uZ.x) * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc.x * p.z, ndc.y * p.z * uFlip, zn * p.z, p.z); vc = c; vn = n; vz = p.z; vp = p; }\n";
static const char *FS_M =
    "#version 310 es\nprecision highp float; in vec3 vc; in vec3 vn; in float vz; in vec3 vp;\n"
    "uniform vec3 uSky; uniform float uFog; uniform int uBGRA; out vec4 o;\n"
    "void main(){ vec3 n = normalize(vn); if (dot(n, vp) > 0.0) n = -n;\n"
    "  vec3 L = normalize(vec3(-0.45, -0.8, -0.35));\n"
    "  float d = max(dot(n, -L), 0.0);\n"
    "  vec3 V = normalize(-vp); vec3 H = normalize(V - L); float sp = pow(max(dot(n, H), 0.0), 24.0) * 0.25;\n"
    "  vec3 col = vc * (0.78 + 0.42 * d) + vec3(sp);\n"
    "  float f = clamp(vz / uFog, 0.0, 1.0); f = f * f * 0.55;\n"
    "  vec4 c = vec4(mix(col, uSky, f), 1.0); o = uBGRA == 1 ? c.bgra : c; }\n";

static GLuint g_pbg, g_pov, g_pm, g_qvbo, g_vbo, g_fms, g_frs, g_rbc, g_rbd, g_rbr;
static GLuint g_tst[3], g_tvp[3], g_tov[3];
static int g_gpu_ok;

static GLuint sh(GLenum t, const char *src)
{
    GLuint s = p_glCreateShader(t); p_glShaderSource(s, 1, &src, NULL); p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[1024]; p_glGetShaderInfoLog(s, sizeof b, NULL, b); fprintf(stderr, "[PSGFX] shader: %s\n", b); return 0; }
    return s;
}
static GLuint prog(const char *vs, const char *fs)
{
    GLuint a = sh(GL_VERTEX_SHADER, vs), b = sh(GL_FRAGMENT_SHADER, fs);
    if (!a || !b) return 0;
    GLuint p = p_glCreateProgram(); p_glAttachShader(p, a); p_glAttachShader(p, b); p_glLinkProgram(p);
    GLint ok = 0; p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char m[1024]; p_glGetProgramInfoLog(p, sizeof m, NULL, m); fprintf(stderr, "[PSGFX] link: %s\n", m); return 0; }
    return p;
}
static void tex_init(GLuint t, GLint filter)
{
    p_glBindTexture(GL_TEXTURE_2D, t);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
static int gpu_init(void)
{
    if (g_gpu_ok) return g_gpu_ok > 0 && gl_up();
    g_gpu_ok = -1;
    if (!gl_up()) return 0;
    g_pbg = prog(VS_Q, FS_BG); g_pov = prog(VS_Q, FS_OV); g_pm = prog(VS_M, FS_M);
    if (!g_pbg || !g_pov || !g_pm) return 0;
    static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    p_glGenBuffers(1, &g_qvbo); p_glBindBuffer(GL_ARRAY_BUFFER, g_qvbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    p_glGenBuffers(1, &g_vbo);
    p_glGenTextures(3, g_tst); p_glGenTextures(3, g_tvp); p_glGenTextures(3, g_tov);
    for (int i = 0; i < 3; i++) { tex_init(g_tst[i], GL_NEAREST); tex_init(g_tvp[i], GL_LINEAR); tex_init(g_tov[i], GL_NEAREST); }
    p_glGenRenderbuffers(1, &g_rbc); p_glBindRenderbuffer(GL_RENDERBUFFER, g_rbc);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, OW, OH);
    p_glGenRenderbuffers(1, &g_rbd); p_glBindRenderbuffer(GL_RENDERBUFFER, g_rbd);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT24, OW, OH);
    p_glGenFramebuffers(1, &g_fms); p_glBindFramebuffer(GL_FRAMEBUFFER, g_fms);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_rbc);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_rbd);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[PSGFX] 4x MSAA framebuffer incomplete\n"); return 0;
    }
    p_glGenRenderbuffers(1, &g_rbr); p_glBindRenderbuffer(GL_RENDERBUFFER, g_rbr);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, OW, OH);
    p_glGenFramebuffers(1, &g_frs); p_glBindFramebuffer(GL_FRAMEBUFFER, g_frs);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_rbr);
    g_gpu_ok = 1;
    return 1;
}
static int lf_slot(const lifted_t *L) { return (int)(L - g_lf); }
static void upload(lifted_t *L)
{
    int s = lf_slot(L);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    p_glBindTexture(GL_TEXTURE_2D, g_tst[s]);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 320, 200, 0, GL_RGBA, GL_UNSIGNED_BYTE, L->st);
    p_glBindTexture(GL_TEXTURE_2D, g_tov[s]);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 320, 200, 0, GL_RGBA, GL_UNSIGNED_BYTE, L->ov);
    p_glBindTexture(GL_TEXTURE_2D, g_tvp[s]);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, L->vpw, L->vph, 0, GL_RGBA, GL_UNSIGNED_BYTE, L->vpbg);
}

/* ======================================================================== */
/* poll: arm, drain, lift                                                   */
/* ======================================================================== */
static void reset_scene(void)
{
    if (g_cur) { frame_put(g_cur); g_cur = NULL; }
    for (int i = 0; i < g_npend; i++) frame_put(g_pend[i]);
    g_npend = 0;
    for (int i = 0; i < 3; i++) { if (g_lf[i].g) frame_put(g_lf[i].g); g_lf[i].g = NULL; }
    g_A = g_B = NULL; g_spare = NULL; g_face = NULL; g_cur_obj = -1;
}

void stbox_psgfx_poll(void)
{
    if (!g_nprof) return;
    if (!g_cache) g_cache = calloc(65536, sizeof *g_cache);
    uint8_t *ram = stbox_shared.ram; uint32_t rs = stbox_shared.ram_size;
    if (!ram) return;

    /* arm/disarm by the profiles' expect bytes (a few dozen compares) */
    static int vcount;
    if (++vcount >= 8) {
        vcount = 0;
        ph_table *hit = NULL;
        for (int i = 0; i < g_nprof && !hit; i++) if (ph_verify(g_prof[i], ram, rs)) hit = g_prof[i];
        if (hit != g_armed_prof) {
            stbox_psgfx_armed = 0;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            reset_scene();
            stbox_psgfx_tail = stbox_psgfx_head;        /* discard old records */
            g_drops_seen = stbox_psgfx_drops;
            g_armed_prof = hit;
            if (hit) {
                if (!gpu_init()) { g_armed_prof = NULL; hit = NULL; }
                else {
                    stbox_psgfx_table = hit;
                    __atomic_thread_fence(__ATOMIC_RELEASE);
                    stbox_psgfx_armed = 1;
                    fprintf(stderr, "[PSGFX] %s armed - 3D goes to the GPU at %d Hz\n", hit->name, g_hz);
                }
            } else fprintf(stderr, "[PSGFX] profile disarmed (program changed)\n");
        }
    }
    if (!g_armed_prof) return;

    /* drain the record ring */
    uint32_t hd = __atomic_load_n(&stbox_psgfx_head, __ATOMIC_ACQUIRE);
    g_recs += hd - stbox_psgfx_tail;
    for (uint32_t t = stbox_psgfx_tail; t != hd; t++)
        sg2_record(&stbox_psgfx_ring[t & (STBOX_PSGFX_RING - 1)]);
    __atomic_store_n(&stbox_psgfx_tail, hd, __ATOMIC_RELEASE);

    /* lift the oldest pending frame once the box displays its buffer */
    uint32_t ci = stbox_shared.cap_idx;
    while (g_npend && ci < STBOX_CAP_BUFS) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        gframe_t *f = g_pend[0];
        stbox_cap_meta_t m = stbox_cap_meta[ci];
        if ((int32_t)(stbox_shared.frame - f->vbl_end) > 100) {        /* never shown: drop */
            frame_put(f); memmove(g_pend, g_pend + 1, sizeof g_pend[0] * (NPEND - 1)); g_npend--; g_lift_skip++;
            continue;
        }
        if (m.base != f->buf || (int32_t)(m.vbl - f->vbl_end) < 0 || stbox_cap[ci].res != 0) break;
        memmove(g_pend, g_pend + 1, sizeof g_pend[0] * (NPEND - 1)); g_npend--;
        /* a later pending frame shown already? then this one was skipped */
        lifted_t *L = NULL;
        for (int i = 0; i < 3 && !L; i++) if (&g_lf[i] != g_A && &g_lf[i] != g_B) L = &g_lf[i];
        if (L->g) { frame_put(L->g); L->g = NULL; }
        if (!L->st) { L->st = malloc(320 * 200 * 4); L->ov = malloc(320 * 200 * 4); L->vpbg = malloc((size_t)OW * OH * 4); }
        double t0 = mono_ms();
        lift_frame(L, f, &stbox_cap[ci], m.vbl);
        upload(L);
        g_lift_ms += mono_ms() - t0;
        if (g_B) { match_frames(g_B, L); overlay_split(g_B, L); upload(g_B); upload(L); }
        if (g_A) { frame_put(g_A->g); g_A->g = NULL; }
        g_A = g_B; g_B = L;
    }
}

int stbox_psgfx_active(void)
{
    return g_armed_prof && g_gpu_ok > 0 && g_B &&
           (int32_t)(stbox_shared.frame - g_B->shown) < 100;   /* lift alive in the last 2 s */
}

static void warp_inv(float ha, float hb, float ha2, float hb2, float t, float cx, float out0[3], float out1[3])
{
    /* forward: rotation about the horizon centre by t*(dtheta) plus t*(dpitch) */
    float th = (atanf(ha2) - atanf(ha)) * t, dy = (hb2 - hb) * t;
    float px = cx * SCALE, py = hb * SCALE, c = cosf(th), s = sinf(th);
    float tx = px - c * px + s * py, ty = py - s * px - c * py + dy * SCALE;
    out0[0] = c;  out0[1] = s; out0[2] = -(c * tx + s * ty);
    out1[0] = -s; out1[1] = c; out1[2] = -(-s * tx + c * ty);
}

int stbox_psgfx_render(double T, uint8_t *dst, int w, int h, int pitch, int bgra)
{
    if (!stbox_psgfx_active() || w != OW || h != OH) return -1;
    double t0 = mono_ms();
    lifted_t *A = g_A ? g_A : g_B, *B = g_B;
    float t = 0.0f;
    if (A != B && B->shown > A->shown) {
        t = (float)((T - B->shown) / (double)(B->shown - A->shown));
        if (t < 0) t = 0;
        if (t > 1) t = 1;
    }
    gframe_t *g = A->g;
    int vx0 = A->vp[0] * SCALE, vy0 = A->vp[1] * SCALE, vx1 = (A->vp[2] + 1) * SCALE, vy1 = (A->vp[3] + 1) * SCALE;

    p_glBindFramebuffer(GL_FRAMEBUFFER, g_fms); p_glViewport(0, 0, OW, OH);
    p_glDisable(GL_DEPTH_TEST); p_glDisable(GL_BLEND); p_glDisable(GL_SCISSOR_TEST); p_glDisable(GL_CULL_FACE);
    p_glClearColor(0, 0, 0, 1); p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* background */
    float ma0[3] = { 1, 0, 0 }, ma1[3] = { 0, 1, 0 }, mb0[3] = { 1, 0, 0 }, mb1[3] = { 0, 1, 0 }, mix = t;
    if (A != B && A->bands.hz_ok && B->bands.hz_ok) {
        warp_inv(A->bands.ha, A->bands.hb, B->bands.ha, B->bands.hb, t, A->bands.cx, ma0, ma1);
        warp_inv(B->bands.ha, B->bands.hb, A->bands.ha, A->bands.hb, 1.0f - t, B->bands.cx, mb0, mb1);
    } else mix = t < 0.5f ? 0.0f : 1.0f;
    p_glUseProgram(g_pbg);
    p_glActiveTexture(GL_TEXTURE0); p_glBindTexture(GL_TEXTURE_2D, g_tvp[lf_slot(A)]);
    p_glActiveTexture(GL_TEXTURE0 + 1); p_glBindTexture(GL_TEXTURE_2D, g_tvp[lf_slot(B)]);
    p_glActiveTexture(GL_TEXTURE0 + 2); p_glBindTexture(GL_TEXTURE_2D, g_tst[lf_slot(A)]);
    p_glUniform1i(p_glGetUniformLocation(g_pbg, "tA"), 0); p_glUniform1i(p_glGetUniformLocation(g_pbg, "tB"), 1);
    p_glUniform1i(p_glGetUniformLocation(g_pbg, "tSt"), 2);
    p_glUniform4f(p_glGetUniformLocation(g_pbg, "uVP"), (float)vx0, (float)vy0, (float)vx1, (float)vy1);
    p_glUniform3f(p_glGetUniformLocation(g_pbg, "uMa0"), ma0[0], ma0[1], ma0[2]);
    p_glUniform3f(p_glGetUniformLocation(g_pbg, "uMa1"), ma1[0], ma1[1], ma1[2]);
    p_glUniform3f(p_glGetUniformLocation(g_pbg, "uMb0"), mb0[0], mb0[1], mb0[2]);
    p_glUniform3f(p_glGetUniformLocation(g_pbg, "uMb1"), mb1[0], mb1[1], mb1[2]);
    p_glUniform1f(p_glGetUniformLocation(g_pbg, "uT"), mix);
    p_glUniform1i(p_glGetUniformLocation(g_pbg, "uBGRA"), bgra);
    p_glUniform2f(p_glGetUniformLocation(g_pbg, "uWH"), (float)OW, (float)OH);
    p_glUniform1f(p_glGetUniformLocation(g_pbg, "uFlip"), -1.0f);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_qvbo); p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, 0);
    p_glDisableVertexAttribArray(1); p_glDisableVertexAttribArray(2);
    p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    p_glActiveTexture(GL_TEXTURE0);

    /* faces: matched vertices interpolated in camera space */
    static float vb[MAXF * (MAXV - 2) * 3 * 9];
    int nvtx = 0; float zmax = 1.0f;
    for (int i = 0; i < g->nf; i++) {
        if (!A->has_col[i]) continue;
        const face_t *fc = &g->f[i];
        v3 P[MAXV];
        for (int k = 0; k < fc->nk; k++) {
            P[k] = fc->cam[k];
            if (A != B && A->ok[i]) {
                P[k].x += t * (A->tgt[i][k].x - P[k].x);
                P[k].y += t * (A->tgt[i][k].y - P[k].y);
                P[k].z += t * (A->tgt[i][k].z - P[k].z);
            }
        }
        for (int k = 1; k + 1 < fc->nk; k++) {
            v3 a = P[0], b = P[k], c = P[k + 1];
            float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
            float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
            const v3 *tri[3] = { &a, &b, &c };
            for (int q = 0; q < 3; q++) {
                float *V = vb + (size_t)nvtx * 9;
                V[0] = tri[q]->x; V[1] = tri[q]->y; V[2] = tri[q]->z; V[3] = nx; V[4] = ny; V[5] = nz;
                V[6] = A->rgb[i][0]; V[7] = A->rgb[i][1]; V[8] = A->rgb[i][2];
                if (V[2] > zmax) zmax = V[2];
                nvtx++;
            }
        }
    }
    if (nvtx) {
        p_glEnable(GL_SCISSOR_TEST); p_glScissor(vx0, vy0, vx1 - vx0, vy1 - vy0);
        p_glEnable(GL_DEPTH_TEST); p_glDepthFunc(GL_LESS);
        p_glUseProgram(g_pm);
        p_glUniform2f(p_glGetUniformLocation(g_pm, "uC"), (g->cx + 0.5f) * SCALE, (g->cy + 0.5f) * SCALE);
        p_glUniform1f(p_glGetUniformLocation(g_pm, "uF"), g_focal * SCALE);
        p_glUniform2f(p_glGetUniformLocation(g_pm, "uWH"), (float)OW, (float)OH);
        p_glUniform2f(p_glGetUniformLocation(g_pm, "uZ"), 32.0f, zmax * 1.5f + 100.0f);
        p_glUniform3f(p_glGetUniformLocation(g_pm, "uSky"), A->sky[0], A->sky[1], A->sky[2]);
        p_glUniform1f(p_glGetUniformLocation(g_pm, "uFog"), 30000.0f);
        p_glUniform1i(p_glGetUniformLocation(g_pm, "uBGRA"), bgra);
        p_glUniform1f(p_glGetUniformLocation(g_pm, "uFlip"), -1.0f);
        p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nvtx * 9 * 4, vb, GL_STREAM_DRAW);
        p_glEnableVertexAttribArray(0); p_glVertexAttribPointer(0, 3, GL_FLOAT, 0, 36, (void *)0);
        p_glEnableVertexAttribArray(1); p_glVertexAttribPointer(1, 3, GL_FLOAT, 0, 36, (void *)12);
        p_glEnableVertexAttribArray(2); p_glVertexAttribPointer(2, 3, GL_FLOAT, 0, 36, (void *)24);
        p_glDrawArrays(GL_TRIANGLES, 0, nvtx);
        p_glDisable(GL_DEPTH_TEST); p_glDisable(GL_SCISSOR_TEST);
        p_glDisableVertexAttribArray(1); p_glDisableVertexAttribArray(2);
    }

    /* overlay (crosshair etc.) from frame A */
    p_glEnable(GL_BLEND); p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glUseProgram(g_pov);
    p_glBindTexture(GL_TEXTURE_2D, g_tov[lf_slot(A)]);
    p_glUniform1i(p_glGetUniformLocation(g_pov, "t"), 0);
    p_glUniform1i(p_glGetUniformLocation(g_pov, "uBGRA"), bgra);
    p_glUniform2f(p_glGetUniformLocation(g_pov, "uWH"), (float)OW, (float)OH);
    p_glUniform1f(p_glGetUniformLocation(g_pov, "uFlip"), -1.0f);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_qvbo); p_glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, 0);
    p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    p_glDisable(GL_BLEND);

    /* resolve and read back, top row first (everything was drawn y-flipped) */
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fms); p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_frs);
    p_glBlitFramebuffer(0, 0, OW, OH, 0, 0, OW, OH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_frs);
    p_glPixelStorei(GL_PACK_ALIGNMENT, 4);
    p_glPixelStorei(GL_PACK_ROW_LENGTH, pitch / 4);
    p_glReadPixels(0, 0, OW, OH, GL_RGBA, GL_UNSIGNED_BYTE, dst);
    p_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    g_render_ms += mono_ms() - t0; g_render_count++;
    return p_glGetError() == GL_NO_ERROR ? 0 : -1;
}

void stbox_psgfx_stats(char *buf, int n)
{
    snprintf(buf, (size_t)n, "psgfx: %s lifted=%u skipped=%u interp=%u/%u faces ring-drops=%u "
             "lift %.1f ms, draw %.1f ms, %.0f hook hits/frame",
             g_armed_prof ? g_armed_prof->name : "(not armed)", g_lift_count, g_lift_skip,
             g_interp_faces, g_total_faces, stbox_psgfx_drops,
             g_lift_count ? g_lift_ms / g_lift_count : 0.0, g_render_count ? g_render_ms / g_render_count : 0.0,
             g_lift_count ? (double)g_recs / g_lift_count : 0.0);
}

void stbox_psgfx_thread_exit(void)
{
    if (g_gl_state > 0 && g_gl_current) { p_eglMakeCurrent(g_dpy, NULL, NULL, NULL); g_gl_current = 0; }
}

void stbox_psgfx_shutdown(void)
{
    stbox_psgfx_armed = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    reset_scene();
    g_armed_prof = NULL;
}
