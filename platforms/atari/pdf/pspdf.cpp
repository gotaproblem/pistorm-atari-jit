// SPDX-License-Identifier: MIT
//
// PSPDF host-side implementation. See pspdf.h and pdf-viewer-design.md.
//
// Measured on the Pi 4 (tools/pdfbench, 747 maintenance manual, 3272 pages):
//   open                        ~96 ms
//   render at 1920 px wide      36-63 ms per page   (19 MB per page)
//   render at 3840 px wide      51-1114 ms          (73 MB per page)
//   convert whole 19 MP page    ~110 ms   <- which is why converting happens
//                                            in fetch, on the viewport only
//   viewport copy 1920x1080     ~11 ms
//   find, whole document        ~7 ms per page
//
// So: rendering runs on a worker thread, conversion happens during the copy
// into guest memory, and search is one page per call with the guest driving
// the loop.

#include "platforms/atari/pdf/pspdf.h"

#include <poppler.h>
#include <cairo.h>
#include <glib.h>

#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

/* ------------------------------------------------------------------ */
/* tunables                                                            */

#define PSPDF_MAX_DOCS      8
#define PSPDF_DEF_CACHE_MB  192
#define PSPDF_DEF_MAX_MPIX  24      /* above this a page is rendered in parts */

static long env_long(const char *name, long def)
{
  const char *e = getenv(name);
  if (!e || !*e) return def;
  char *end = NULL;
  long v = strtol(e, &end, 0);
  return (end && end != e && v > 0) ? v : def;
}

/* ------------------------------------------------------------------ */
/* state                                                               */

struct pspdf_entry {              /* one rendered page (or part of one) */
  int page, zoom, rot;
  int ox, oy;                     /* top-left of this bitmap in page pixels */
  int w, h;                       /* bitmap size                            */
  int pw, ph;                     /* full page size at this zoom            */
  cairo_surface_t *surf;
  size_t bytes;
};

struct pspdf_outline_item {
  int depth;
  int page;                       /* 1-based, 0 = unknown */
  std::string title;              /* Atari charset */
};

struct pspdf_doc {
  bool used = false;
  PopplerDocument *doc = NULL;
  GBytes *bytes = NULL;           /* kept alive for OPENMEM documents */
  int pages = 0;
  bool have_outline = false;
  std::vector<pspdf_outline_item> outline;

  /* the page the guest is looking at */
  int cur_page = 0, cur_zoom = 0, cur_rot = 0;
  int cur_vx = 0, cur_vy = 0, cur_vw = 0, cur_vh = 0;
  int state = PSPDF_OK;           /* OK ready, 1 busy, PSPDF_ERR failed */

  /* pending job */
  bool job = false;
  bool job_prefetch = false;      /* fills the cache, changes nothing else */
  int job_page = 0, job_zoom = 0, job_rot = 0;
  int job_vx = 0, job_vy = 0, job_vw = 0, job_vh = 0;

  /* continuous mode: FETCH stitches the neighbouring pages in */
  bool continuous = false;
  int  gap = 16;

  /* page sizes in points, filled by whoever first asked or drew a page,
   * so PAGESIZE can answer without the render lock once a page is known
   * (a plain struct, not std::pair: gcc on the Pi notes the C++17 ABI
   * change for pair<double,double> at every use) */
  struct ptsz { double w, h; };
  std::map<int, ptsz> ptsize;

  /* highlight rectangles FETCH blends in: page pixels at the zoom they
   * were given for */
  int hl_page = 0, hl_zoom = 0, hl_n = 0;
  int32_t hl[PSPDF_HILITE_MAX * 4];
  uint32_t hl_rgb = 0x4cc2ff;

  std::deque<pspdf_entry> cache;
  size_t cache_bytes = 0;

  /* link URIs of the page LINKS was last called for */
  int uri_page = -1;
  std::vector<std::string> uris;
};

static pspdf_doc        g_docs[PSPDF_MAX_DOCS];
static pthread_mutex_t  g_lock   = PTHREAD_MUTEX_INITIALIZER;  /* table    */
static pthread_mutex_t  g_render = PTHREAD_MUTEX_INITIALIZER;  /* poppler  */
static pthread_cond_t   g_wake   = PTHREAD_COND_INITIALIZER;
static pthread_t        g_worker;
static bool             g_worker_started = false;
static bool             g_quit = false;

static pspdf_doc *doc_of(int handle)
{
  if (handle < 1 || handle > PSPDF_MAX_DOCS) return NULL;
  pspdf_doc *d = &g_docs[handle - 1];
  return d->used ? d : NULL;
}

/* ------------------------------------------------------------------ */
/* charset                                                             */

/* Atari ST 0x80..0xBF as Unicode. Above 0xBF the ST font is Hebrew and
 * symbols that no PDF text needs, so those become '?'. */
static const unsigned short atari_hi[0x40] = {
  0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,
  0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
  0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,
  0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x00DF,0x0192,
  0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,
  0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
  0x00E3,0x00F5,0x00D8,0x00F8,0x0153,0x0152,0x00C0,0x00C3,
  0x00D5,0x00A8,0x00B4,0x2020,0x00B6,0x00A9,0x00AE,0x2122
};

static int utf8_next(const char **p)
{
  const unsigned char *s = (const unsigned char *)*p;
  int c = *s++;

  if (c < 0x80) { }
  else if ((c & 0xE0) == 0xC0 && (s[0] & 0xC0) == 0x80) {
    c = ((c & 0x1F) << 6) | (s[0] & 0x3F); s += 1;
  } else if ((c & 0xF0) == 0xE0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) {
    c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F); s += 2;
  } else if ((c & 0xF8) == 0xF0) {
    c = '?';
    while ((*s & 0xC0) == 0x80) s++;
  } else {
    c = '?';
  }
  *p = (const char *)s;
  return c;
}

void pspdf_utf8_to_atari(const char *utf8, char *out, size_t out_len)
{
  size_t o = 0;
  if (!out || !out_len) return;
  if (!utf8) { out[0] = 0; return; }

  while (*utf8 && o + 1 < out_len) {
    int u = utf8_next(&utf8);
    int c = '?';

    if (u < 0x80) {
      c = u;
    } else if (u == 0x2019 || u == 0x2018) {
      c = '\'';
    } else if (u == 0x201C || u == 0x201D) {
      c = '"';
    } else if (u == 0x2013 || u == 0x2014) {
      c = '-';
    } else {
      for (int i = 0; i < 0x40; i++)
        if (atari_hi[i] == u) { c = 0x80 + i; break; }
    }
    out[o++] = (char)c;
  }
  out[o] = 0;
}

void pspdf_atari_to_utf8(const char *atari, char *out, size_t out_len)
{
  size_t o = 0;
  if (!out || !out_len) return;
  if (!atari) { out[0] = 0; return; }

  for (const unsigned char *s = (const unsigned char *)atari; *s; s++) {
    unsigned u = *s;
    if (u >= 0x80)
      u = (u < 0xC0) ? atari_hi[u - 0x80] : '?';

    if (u < 0x80) {
      if (o + 2 > out_len) break;
      out[o++] = (char)u;
    } else if (u < 0x800) {
      if (o + 3 > out_len) break;
      out[o++] = (char)(0xC0 | (u >> 6));
      out[o++] = (char)(0x80 | (u & 0x3F));
    } else {
      if (o + 4 > out_len) break;
      out[o++] = (char)(0xE0 | (u >> 12));
      out[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (u & 0x3F));
    }
  }
  out[o] = 0;
}

static void put_atari(char *buf, int len, const char *utf8)
{
  if (!buf || len <= 0) return;
  pspdf_utf8_to_atari(utf8 ? utf8 : "", buf, (size_t)len);
}

/* ------------------------------------------------------------------ */
/* geometry                                                            */

static double zoom_scale(int zoom)
{
  if (zoom <= 0) zoom = PSPDF_ZOOM_100;
  return (zoom / 1000.0) * (PSPDF_BASE_DPI / 72.0);
}

/* Page size in device pixels at this zoom, with rotation applied. */
static bool page_pixels(PopplerPage *pg, int zoom, int rot, int *w, int *h)
{
  double pw, ph;
  if (!pg) return false;
  poppler_page_get_size(pg, &pw, &ph);

  double s = zoom_scale(zoom);
  int iw = (int)(pw * s + 0.5);
  int ih = (int)(ph * s + 0.5);
  if (iw < 1) iw = 1;
  if (ih < 1) ih = 1;

  if (rot == 90 || rot == 270) { *w = ih; *h = iw; }
  else                         { *w = iw; *h = ih; }
  return true;
}

/* ------------------------------------------------------------------ */
/* cache                                                               */

static void cache_trim(pspdf_doc *d)
{
  size_t cap = (size_t)env_long("PISTORM_PDF_CACHE_MB", PSPDF_DEF_CACHE_MB)
               * 1024u * 1024u;

  while (d->cache.size() > 1 && d->cache_bytes > cap) {
    pspdf_entry &e = d->cache.front();
    d->cache_bytes -= e.bytes;
    cairo_surface_destroy(e.surf);
    d->cache.pop_front();
  }
}

static void cache_clear(pspdf_doc *d)
{
  for (size_t i = 0; i < d->cache.size(); i++)
    cairo_surface_destroy(d->cache[i].surf);
  d->cache.clear();
  d->cache_bytes = 0;
}

/* The cached bitmap covering (vx,vy,vw,vh) of this page, if there is one.
 * Called with g_lock held. */
static pspdf_entry *cache_find(pspdf_doc *d, int page, int zoom, int rot,
                               int vx, int vy, int vw, int vh)
{
  for (size_t i = 0; i < d->cache.size(); i++) {
    pspdf_entry &e = d->cache[i];
    if (e.page != page || e.zoom != zoom || e.rot != rot) continue;
    if (vw <= 0 || vh <= 0)
      return &e;
    if (vx >= e.ox && vy >= e.oy &&
        vx + vw <= e.ox + e.w && vy + vh <= e.oy + e.h)
      return &e;
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* rendering (worker thread)                                           */

static bool render_entry(pspdf_doc *d, int page, int zoom, int rot,
                         int vx, int vy, int vw, int vh, pspdf_entry *out)
{
  PopplerPage *pg = poppler_document_get_page(d->doc, page - 1);
  if (!pg) return false;

  int pw, ph;
  if (!page_pixels(pg, zoom, rot, &pw, &ph)) { g_object_unref(pg); return false; }
  {
    double ptw, pth;
    poppler_page_get_size(pg, &ptw, &pth);
    pthread_mutex_lock(&g_lock);
    d->ptsize[page].w = ptw;
    d->ptsize[page].h = pth;
    pthread_mutex_unlock(&g_lock);
  }

  long max_pix = env_long("PISTORM_PDF_MAX_MPIX", PSPDF_DEF_MAX_MPIX) * 1000000L;
  int ox = 0, oy = 0, w = pw, h = ph;

  if ((long)pw * ph > max_pix) {
    /* too big to hold whole: render a band around the viewport, with a
     * margin so small scrolls stay inside it */
    if (vw <= 0 || vh <= 0) { vx = 0; vy = 0; vw = pw < 1920 ? pw : 1920;
                              vh = ph < 1080 ? ph : 1080; }
    int mx = vw / 2, my = vh;
    ox = vx - mx; if (ox < 0) ox = 0;
    oy = vy - my; if (oy < 0) oy = 0;
    w  = vw + 2 * mx; if (ox + w > pw) w = pw - ox;
    h  = vh + 2 * my; if (oy + h > ph) h = ph - oy;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
  }

  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surf);
    g_object_unref(pg);
    return false;
  }

  cairo_t *cr = cairo_create(surf);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);          /* paper */
  cairo_paint(cr);

  cairo_translate(cr, -ox, -oy);
  switch (rot) {
    case 90:  cairo_translate(cr, pw, 0);  cairo_rotate(cr,  G_PI / 2); break;
    case 180: cairo_translate(cr, pw, ph); cairo_rotate(cr,  G_PI);     break;
    case 270: cairo_translate(cr, 0, ph);  cairo_rotate(cr, -G_PI / 2); break;
    default: break;
  }
  double s = zoom_scale(zoom);
  cairo_scale(cr, s, s);

  poppler_page_render(pg, cr);
  cairo_surface_flush(surf);
  cairo_destroy(cr);
  g_object_unref(pg);

  out->page = page;
  out->zoom = zoom;
  out->rot  = rot;
  out->ox   = ox;
  out->oy   = oy;
  out->w    = w;
  out->h    = h;
  out->pw   = pw;
  out->ph   = ph;
  out->surf = surf;
  out->bytes = (size_t)cairo_image_surface_get_stride(surf) * (size_t)h;
  return true;
}

static void *pspdf_worker(void *arg)
{
  (void)arg;

  for (;;) {
    pthread_mutex_lock(&g_lock);
    int handle = 0;
    while (!g_quit) {
      for (int i = 0; i < PSPDF_MAX_DOCS; i++)
        if (g_docs[i].used && g_docs[i].job) { handle = i + 1; break; }
      if (handle) break;
      pthread_cond_wait(&g_wake, &g_lock);
    }
    if (g_quit) { pthread_mutex_unlock(&g_lock); break; }

    pspdf_doc *d = &g_docs[handle - 1];
    int page = d->job_page, zoom = d->job_zoom, rot = d->job_rot;
    int vx = d->job_vx, vy = d->job_vy, vw = d->job_vw, vh = d->job_vh;
    bool prefetch = d->job_prefetch;
    d->job = false;
    d->job_prefetch = false;
    pthread_mutex_unlock(&g_lock);

    pspdf_entry e;
    memset(&e, 0, sizeof e);

    pthread_mutex_lock(&g_render);
    bool ok = doc_of(handle) && render_entry(d, page, zoom, rot,
                                             vx, vy, vw, vh, &e);
    pthread_mutex_unlock(&g_render);

    pthread_mutex_lock(&g_lock);
    if (!doc_of(handle)) {                 /* closed while rendering */
      if (ok) cairo_surface_destroy(e.surf);
    } else if (!ok) {
      if (!prefetch)
        d->state = PSPDF_ERR;
    } else {
      d->cache.push_back(e);
      d->cache_bytes += e.bytes;
      cache_trim(d);
      if (!d->job && d->cur_page == page && d->cur_zoom == zoom &&
          d->cur_rot == rot)
        d->state = PSPDF_OK;
    }
    pthread_mutex_unlock(&g_lock);
  }

  return NULL;
}

/* Where the worker runs. Same rule as the MP3 FILELEN worker in
 * dmasnd_hdmi.c: core 2 is the 68k thread (SCHED_FIFO 99), core 3 the IPL
 * poller, core 0 takes the hardware interrupts - so on a Pi 4 that leaves
 * core 1. PISTORM_PDF_CPUS=<hex> overrides. */
static void worker_cpuset(cpu_set_t *set)
{
  const char *e = getenv("PISTORM_PDF_CPUS");
  unsigned long mask = (e && *e) ? strtoul(e, NULL, 16) : 0;
  long n = sysconf(_SC_NPROCESSORS_CONF);

  CPU_ZERO(set);
  if (n < 1) n = 4;
  if (mask) {
    for (int i = 0; i < 64 && i < CPU_SETSIZE; i++)
      if (mask & (1UL << i)) CPU_SET(i, set);
  } else {
    for (long i = 0; i < n && i < CPU_SETSIZE; i++)
      if (i != 2 && !(n >= 4 && (i == 0 || i == 3)))
        CPU_SET((int)i, set);
  }
  if (CPU_COUNT(set) == 0) CPU_SET(0, set);
}

static bool worker_start(void)
{
  if (g_worker_started) return true;

  /* Born free, not escaped later (the lesson from the FILELEN worker): a
   * thread created with default attributes inherits its creator's core and
   * class - and the creator is the JIT CPU thread, core 2, SCHED_FIFO 99 -
   * so it could not run its first instruction until the 68k blocked.
   * PTHREAD_EXPLICIT_SCHED is what makes the policy below take effect at
   * all; without it setschedpolicy() is ignored. */
  pthread_attr_t attr;
  struct sched_param sp;
  cpu_set_t set;

  worker_cpuset(&set);
  pthread_attr_init(&attr);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
  memset(&sp, 0, sizeof sp);
  pthread_attr_setschedparam(&attr, &sp);
  pthread_attr_setaffinity_np(&attr, sizeof set, &set);

  if (pthread_create(&g_worker, &attr, pspdf_worker, NULL) != 0) {
    pthread_attr_destroy(&attr);
    fprintf(stderr, "[PSPDF] cannot start the render thread\n");
    return false;
  }
  pthread_attr_destroy(&attr);
  pthread_setname_np(g_worker, "pspdf");
  g_worker_started = true;
  return true;
}

/* ------------------------------------------------------------------ */
/* outline                                                             */

static int dest_page(PopplerDocument *doc, PopplerDest *dest)
{
  int page = 0;

  if (!dest) return 0;
  if (dest->type == POPPLER_DEST_NAMED) {
    PopplerDest *real = poppler_document_find_dest(doc, dest->named_dest);
    if (real) {
      page = real->page_num;
      poppler_dest_free(real);
    }
  } else {
    page = dest->page_num;
  }
  return page;
}

static void outline_walk(pspdf_doc *d, PopplerIndexIter *iter, int depth)
{
  if (!iter || depth > 8) return;

  do {
    PopplerAction *act = poppler_index_iter_get_action(iter);
    if (!act) continue;

    pspdf_outline_item item;
    item.depth = depth;
    item.page  = 0;

    char title[PSPDF_TITLE_MAX];
    put_atari(title, sizeof title, act->any.title);
    item.title = title;

    if (act->type == POPPLER_ACTION_GOTO_DEST)
      item.page = dest_page(d->doc, act->goto_dest.dest);

    d->outline.push_back(item);
    poppler_action_free(act);

    PopplerIndexIter *child = poppler_index_iter_get_child(iter);
    if (child) {
      outline_walk(d, child, depth + 1);
      poppler_index_iter_free(child);
    }
  } while (poppler_index_iter_next(iter));
}

static void outline_build(pspdf_doc *d)
{
  PopplerIndexIter *iter = poppler_index_iter_new(d->doc);
  if (!iter) return;
  d->have_outline = true;
  outline_walk(d, iter, 0);
  poppler_index_iter_free(iter);
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */

static int doc_finish_open(PopplerDocument *doc, GBytes *bytes)
{
  int handle = 0;

  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < PSPDF_MAX_DOCS; i++)
    if (!g_docs[i].used) { handle = i + 1; break; }
  pthread_mutex_unlock(&g_lock);

  if (!handle) {
    g_object_unref(doc);
    if (bytes) g_bytes_unref(bytes);
    fprintf(stderr, "[PSPDF] too many open documents\n");
    return PSPDF_ERR;
  }

  pspdf_doc *d = &g_docs[handle - 1];
  d->doc     = doc;
  d->bytes   = bytes;
  d->pages   = poppler_document_get_n_pages(doc);
  d->state   = PSPDF_OK;
  d->job     = false;
  d->uri_page = -1;
  d->outline.clear();
  d->uris.clear();
  outline_build(d);

  pthread_mutex_lock(&g_lock);
  d->used = true;
  pthread_mutex_unlock(&g_lock);

  if (!worker_start()) {
    pspdf_close(handle);
    return PSPDF_ERR;
  }

  printf("[PSPDF] opened %d pages, %d outline entries (handle %d)\n",
         d->pages, (int)d->outline.size(), handle);
  return handle;
}

int pspdf_open(const char *host_path)
{
  GError *err = NULL;

  if (!host_path || !*host_path) return PSPDF_ERR;

  char *uri = g_filename_to_uri(host_path, NULL, &err);
  if (!uri) {
    fprintf(stderr, "[PSPDF] %s: %s\n", host_path,
            err ? err->message : "bad path");
    g_clear_error(&err);
    return PSPDF_ERR;
  }

  PopplerDocument *doc = poppler_document_new_from_file(uri, NULL, &err);
  g_free(uri);

  if (!doc) {
    int rc = (err && err->code == POPPLER_ERROR_ENCRYPTED) ? PSPDF_LOCKED
                                                           : PSPDF_ERR;
    fprintf(stderr, "[PSPDF] %s: %s\n", host_path,
            err ? err->message : "cannot open");
    g_clear_error(&err);
    return rc;
  }

  return doc_finish_open(doc, NULL);
}

int pspdf_open_mem(const uint8_t *data, size_t len)
{
  GError *err = NULL;

  if (!data || !len) return PSPDF_ERR;

  /* copy: the guest may reuse its buffer the moment this returns */
  GBytes *bytes = g_bytes_new(data, len);
  PopplerDocument *doc = poppler_document_new_from_bytes(bytes, NULL, &err);
  if (!doc) {
    int rc = (err && err->code == POPPLER_ERROR_ENCRYPTED) ? PSPDF_LOCKED
                                                           : PSPDF_ERR;
    fprintf(stderr, "[PSPDF] memory document: %s\n",
            err ? err->message : "cannot open");
    g_clear_error(&err);
    g_bytes_unref(bytes);
    return rc;
  }

  return doc_finish_open(doc, bytes);
}

void pspdf_close(int handle)
{
  pthread_mutex_lock(&g_lock);
  pspdf_doc *d = doc_of(handle);
  if (!d) { pthread_mutex_unlock(&g_lock); return; }
  d->used = false;
  d->job  = false;
  pthread_mutex_unlock(&g_lock);

  /* wait for a render in flight before tearing the document down */
  pthread_mutex_lock(&g_render);
  pthread_mutex_lock(&g_lock);
  cache_clear(d);
  d->outline.clear();
  d->uris.clear();
  if (d->doc)   { g_object_unref(d->doc); d->doc = NULL; }
  if (d->bytes) { g_bytes_unref(d->bytes); d->bytes = NULL; }
  pthread_mutex_unlock(&g_lock);
  pthread_mutex_unlock(&g_render);
}

void pspdf_shutdown(void)
{
  for (int i = 1; i <= PSPDF_MAX_DOCS; i++)
    pspdf_close(i);

  pthread_mutex_lock(&g_lock);
  g_quit = true;
  pthread_cond_broadcast(&g_wake);
  pthread_mutex_unlock(&g_lock);

  if (g_worker_started) {
    pthread_join(g_worker, NULL);
    g_worker_started = false;
    g_quit = false;
  }
}

/* ------------------------------------------------------------------ */
/* queries                                                             */

long pspdf_info(int handle, int what)
{
  pthread_mutex_lock(&g_lock);
  pspdf_doc *d = doc_of(handle);
  long v = PSPDF_ERR;

  if (d) {
    switch (what) {
      case PSPDF_INFO_PAGES:   v = d->pages; break;
      case PSPDF_INFO_OUTLINE: v = (long)d->outline.size(); break;
      case PSPDF_INFO_FLAGS:   v = (d->have_outline ? 1 : 0); break;
      default: break;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return v;
}

static long size_from_pts(double ptw, double pth, int zoom)
{
  double sc = zoom_scale(zoom);
  long w = (long)(ptw * sc + 0.5), h = (long)(pth * sc + 0.5);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (w > 0xffff) w = 0xffff;
  if (h > 0xffff) h = 0xffff;
  return (w << 16) | h;
}

long pspdf_page_size(int handle, int page, int zoom)
{
  pspdf_doc *d = doc_of(handle);
  if (!d || page < 1 || page > d->pages) return PSPDF_ERR;

  /* known already? then no lock, and no BUSY while the worker draws */
  pthread_mutex_lock(&g_lock);
  std::map<int, pspdf_doc::ptsz>::iterator it = d->ptsize.find(page);
  if (it != d->ptsize.end()) {
    long v = size_from_pts(it->second.w, it->second.h, zoom);
    pthread_mutex_unlock(&g_lock);
    return v;
  }
  pthread_mutex_unlock(&g_lock);

  if (pthread_mutex_trylock(&g_render) != 0) return PSPDF_BUSY;

  if (!d->used || !d->doc) {           /* closed while we were waiting */
    pthread_mutex_unlock(&g_render);
    return PSPDF_ERR;
  }

  long v = PSPDF_ERR;
  PopplerPage *pg = poppler_document_get_page(d->doc, page - 1);
  if (pg) {
    double ptw, pth;
    poppler_page_get_size(pg, &ptw, &pth);
    pthread_mutex_lock(&g_lock);
    d->ptsize[page].w = ptw;
    d->ptsize[page].h = pth;
    pthread_mutex_unlock(&g_lock);
    v = size_from_pts(ptw, pth, zoom);
    g_object_unref(pg);
  }

  pthread_mutex_unlock(&g_render);
  return v;
}

int pspdf_continuous(int handle, int on, int gap)
{
  pthread_mutex_lock(&g_lock);
  pspdf_doc *d = doc_of(handle);
  if (!d) { pthread_mutex_unlock(&g_lock); return PSPDF_ERR; }
  d->continuous = on != 0;
  d->gap = (gap < 0) ? 0 : (gap > 256 ? 256 : gap);
  pthread_mutex_unlock(&g_lock);
  return PSPDF_OK;
}

static int queue_render(int handle, int page, int zoom, int rot, bool prefetch)
{
  pthread_mutex_lock(&g_lock);
  pspdf_doc *d = doc_of(handle);
  if (!d || page < 1 || page > d->pages) {
    pthread_mutex_unlock(&g_lock);
    return PSPDF_ERR;
  }

  if (rot != 90 && rot != 180 && rot != 270) rot = 0;
  if (zoom <= 0) zoom = PSPDF_ZOOM_100;

  if (!prefetch) {
    d->cur_page = page;
    d->cur_zoom = zoom;
    d->cur_rot  = rot;
  }

  if (cache_find(d, page, zoom, rot, prefetch ? 0 : d->cur_vx,
                 prefetch ? 0 : d->cur_vy, prefetch ? 0 : d->cur_vw,
                 prefetch ? 0 : d->cur_vh)) {
    if (!prefetch)
      d->state = PSPDF_OK;                /* already drawn */
    pthread_mutex_unlock(&g_lock);
    return PSPDF_OK;
  }

  /* a prefetch never displaces a real job; a real job displaces anything */
  if (prefetch && d->job && !d->job_prefetch) {
    pthread_mutex_unlock(&g_lock);
    return PSPDF_OK;
  }

  d->job      = true;
  d->job_prefetch = prefetch;
  d->job_page = page;
  d->job_zoom = zoom;
  d->job_rot  = rot;
  d->job_vx   = prefetch ? 0 : d->cur_vx;
  d->job_vy   = prefetch ? 0 : d->cur_vy;
  d->job_vw   = prefetch ? 0 : d->cur_vw;
  d->job_vh   = prefetch ? 0 : d->cur_vh;
  if (!prefetch)
    d->state  = 1;                        /* busy */
  pthread_cond_broadcast(&g_wake);
  pthread_mutex_unlock(&g_lock);
  return PSPDF_OK;
}

int pspdf_render(int handle, int page, int zoom, int rot)
{
  return queue_render(handle, page, zoom, rot, false);
}

int pspdf_prefetch(int handle, int page, int zoom, int rot)
{
  return queue_render(handle, page, zoom, rot, true);
}

int pspdf_hilite(int handle, int page, const int32_t *rects, int n,
                 uint32_t rgb)
{
  pthread_mutex_lock(&g_lock);
  pspdf_doc *d = doc_of(handle);
  if (!d) { pthread_mutex_unlock(&g_lock); return PSPDF_ERR; }

  if (n < 0) n = 0;
  if (n > PSPDF_HILITE_MAX) n = PSPDF_HILITE_MAX;
  d->hl_page = page;
  d->hl_zoom = d->cur_zoom;
  d->hl_n = (rects && n > 0) ? n : 0;
  if (d->hl_n)
    memcpy(d->hl, rects, (size_t)d->hl_n * 4 * sizeof(int32_t));
  d->hl_rgb = rgb & 0xffffff;
  pthread_mutex_unlock(&g_lock);
  return PSPDF_OK;
}

int pspdf_status(int handle)
{
  pthread_mutex_lock(&g_lock);
  pspdf_doc *d = doc_of(handle);
  int v = d ? d->state : PSPDF_ERR;
  pthread_mutex_unlock(&g_lock);
  return v;
}

/* ------------------------------------------------------------------ */
/* fetch                                                               */

static inline uint32_t to_xrgb(uint32_t argb)
{
  return __builtin_bswap32(argb & 0x00ffffffu);
}

static inline uint16_t to_rgb565_be(uint32_t argb)
{
  unsigned r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
  uint16_t v = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
  return (uint16_t)((v >> 8) | (v << 8));
}

int pspdf_fetch(int handle, int x, int y, int w, int h, int bpp,
                uint8_t *dst, size_t dst_stride, uint32_t bg)
{
  if (!dst || w <= 0 || h <= 0 || (bpp != 16 && bpp != 32))
    return PSPDF_ERR;

  pthread_mutex_lock(&g_lock);
  pspdf_doc *d = doc_of(handle);
  if (!d) { pthread_mutex_unlock(&g_lock); return PSPDF_ERR; }

  d->cur_vx = x; d->cur_vy = y; d->cur_vw = w; d->cur_vh = h;

  pspdf_entry *e = cache_find(d, d->cur_page, d->cur_zoom, d->cur_rot,
                              x, y, w, h);
  if (!e)                                  /* fall back to any band we have */
    e = cache_find(d, d->cur_page, d->cur_zoom, d->cur_rot, 0, 0, 0, 0);
  if (!e) {
    int rc = (d->state == PSPDF_ERR) ? PSPDF_ERR : PSPDF_BUSY;
    pthread_mutex_unlock(&g_lock);
    return rc;
  }

  const uint8_t *src = cairo_image_surface_get_data(e->surf);
  size_t src_stride = (size_t)cairo_image_surface_get_stride(e->surf);

  /* one row of page pixels (ARGB, 0xff alpha), highlights blended in,
   * then converted - so the conversion loop is the same with or without
   * them and the guest never sees a hit rectangle as pixels to draw */
  /* continuous mode: the neighbours, if the cache holds them whole */
  pspdf_entry *prev = NULL, *next = NULL;
  if (d->continuous) {
    if (d->cur_page > 1)
      prev = cache_find(d, d->cur_page - 1, d->cur_zoom, d->cur_rot, 0, 0, 0, 0);
    if (d->cur_page < d->pages)
      next = cache_find(d, d->cur_page + 1, d->cur_zoom, d->cur_rot, 0, 0, 0, 0);
  }

  std::vector<uint32_t> row((size_t)w);
  const bool hl_on = d->hl_n > 0 && d->hl_page == d->cur_page &&
                     d->hl_zoom == d->cur_zoom;
  const uint32_t hr = (d->hl_rgb >> 16) & 0xff, hg = (d->hl_rgb >> 8) & 0xff,
                 hb = d->hl_rgb & 0xff;

  for (int r = 0; r < h; r++) {
    int py = y + r;                              /* page pixel row */
    int sy = py - e->oy;
    uint8_t *drow = dst + (size_t)r * dst_stride;

    const uint8_t *rsrc = src;          /* which bitmap this row comes from */
    size_t rstride = src_stride;
    int rx = x, rox = e->ox, rw = e->w;   /* and its horizontal frame */

    if (sy < 0 && prev && py < -d->gap) {
      /* above this page: the bottom of the previous one, centred on it */
      sy = py + d->gap + prev->ph - prev->oy;
      rsrc = cairo_image_surface_get_data(prev->surf);
      rstride = (size_t)cairo_image_surface_get_stride(prev->surf);
      rx = x + (prev->pw - e->pw) / 2;
      rox = prev->ox; rw = prev->w;
      if (sy < 0 || sy >= prev->h) sy = -1;
    } else if (py >= e->ph + d->gap && next) {
      /* below it: the top of the next one */
      sy = py - e->ph - d->gap - next->oy;
      rsrc = cairo_image_surface_get_data(next->surf);
      rstride = (size_t)cairo_image_surface_get_stride(next->surf);
      rx = x + (next->pw - e->pw) / 2;
      rox = next->ox; rw = next->w;
      if (sy < 0 || sy >= next->h) sy = -1;
    } else if (sy < 0 || sy >= e->h) {
      sy = -1;
    }

    if (sy < 0) {
      for (int i = 0; i < w; i++) row[i] = 0xff000000u | (bg & 0xffffffu);
    } else {
      const uint32_t *srow = (const uint32_t *)(rsrc + (size_t)sy * rstride);
      for (int i = 0; i < w; i++) {
        int sx = rx + i - rox;
        row[i] = (sx < 0 || sx >= rw) ? (0xff000000u | (bg & 0xffffffu))
                                      : (srow[sx] | 0xff000000u);
      }
    }

    if (hl_on) {
      for (int k = 0; k < d->hl_n; k++) {
        const int32_t *q = d->hl + k * 4;
        int hx0 = q[0], hy0 = q[1], hx1 = q[0] + q[2], hy1 = q[1] + q[3];
        if (py < hy0 || py >= hy1) continue;
        int a0 = hx0 - x, a1 = hx1 - x;            /* into this row */
        if (a0 < 0) a0 = 0;
        if (a1 > w) a1 = w;
        bool edge_row = (py == hy0 || py == hy1 - 1);
        for (int i = a0; i < a1; i++) {
          bool edge = edge_row || (x + i == hx0) || (x + i == hx1 - 1);
          unsigned a = edge ? 200 : 80;            /* out of 255 */
          uint32_t p = row[i];
          unsigned pr = (p >> 16) & 0xff, pg = (p >> 8) & 0xff, pb = p & 0xff;
          pr = (pr * (255 - a) + hr * a) / 255;
          pg = (pg * (255 - a) + hg * a) / 255;
          pb = (pb * (255 - a) + hb * a) / 255;
          row[i] = 0xff000000u | (pr << 16) | (pg << 8) | pb;
        }
      }
    }

    if (bpp == 32) {
      uint32_t *p = (uint32_t *)drow;
      for (int i = 0; i < w; i++) p[i] = to_xrgb(row[i]);
    } else {
      uint16_t *p = (uint16_t *)drow;
      for (int i = 0; i < w; i++) p[i] = to_rgb565_be(row[i]);
    }
  }

  /* Scrolled off the part of a huge page we have? Ask for the next band. */
  bool need_more = (x < e->ox || y < e->oy ||
                    x + w > e->ox + e->w || y + h > e->oy + e->h) &&
                   (e->w < e->pw || e->h < e->ph);
  if (need_more && !d->job) {
    d->job = true;
    d->job_page = d->cur_page;
    d->job_zoom = d->cur_zoom;
    d->job_rot  = d->cur_rot;
    d->job_vx = x; d->job_vy = y; d->job_vw = w; d->job_vh = h;
    d->state = 1;
    pthread_cond_broadcast(&g_wake);
  }

  pthread_mutex_unlock(&g_lock);
  return PSPDF_OK;
}

/* ------------------------------------------------------------------ */
/* text, search, links, outline, metadata                              */

/* poppler gives search and link rectangles in PDF points measured from the
 * bottom of the page; the guest wants pixels from the top. */
static void rect_to_pixels(const PopplerRectangle *r, double page_h_pts,
                           int zoom, int32_t *out)
{
  double s = zoom_scale(zoom);
  double x1 = r->x1 < r->x2 ? r->x1 : r->x2;
  double x2 = r->x1 < r->x2 ? r->x2 : r->x1;
  double y1 = r->y1 < r->y2 ? r->y1 : r->y2;
  double y2 = r->y1 < r->y2 ? r->y2 : r->y1;

  double top = page_h_pts - y2;                 /* flip */
  out[0] = (int32_t)(x1 * s + 0.5);
  out[1] = (int32_t)(top * s + 0.5);
  out[2] = (int32_t)((x2 - x1) * s + 0.5);
  out[3] = (int32_t)((y2 - y1) * s + 0.5);
}

int pspdf_find(int handle, const char *needle_utf8, int page, int flags,
               int zoom, int32_t *rects, int max)
{
  pspdf_doc *d = doc_of(handle);
  if (!d || !needle_utf8 || !*needle_utf8 || page < 1 || page > d->pages)
    return PSPDF_ERR;

  if (pthread_mutex_trylock(&g_render) != 0) return PSPDF_BUSY;

  if (!d->used || !d->doc) {           /* closed while we were waiting */
    pthread_mutex_unlock(&g_render);
    return PSPDF_ERR;
  }

  int n = 0;
  PopplerPage *pg = poppler_document_get_page(d->doc, page - 1);
  if (pg) {
    PopplerFindFlags ff = POPPLER_FIND_DEFAULT;
    if (flags & PSPDF_FIND_CASE)  ff = (PopplerFindFlags)(ff | POPPLER_FIND_CASE_SENSITIVE);
    if (flags & PSPDF_FIND_WORDS) ff = (PopplerFindFlags)(ff | POPPLER_FIND_WHOLE_WORDS_ONLY);

    double pw, ph;
    poppler_page_get_size(pg, &pw, &ph);

    GList *hits = poppler_page_find_text_with_options(pg, needle_utf8, ff);
    for (GList *l = hits; l && n < max; l = l->next, n++)
      if (rects)
        rect_to_pixels((PopplerRectangle *)l->data, ph, zoom, rects + n * 4);
    g_list_free_full(hits, (GDestroyNotify)poppler_rectangle_free);
    g_object_unref(pg);
  }

  pthread_mutex_unlock(&g_render);
  return n;
}

int pspdf_text(int handle, int page, int zoom, const int32_t *rect,
               char *buf, int len)
{
  pspdf_doc *d = doc_of(handle);
  if (!d || page < 1 || page > d->pages || !buf || len <= 0) return PSPDF_ERR;

  if (pthread_mutex_trylock(&g_render) != 0) return PSPDF_BUSY;

  if (!d->used || !d->doc) {           /* closed while we were waiting */
    pthread_mutex_unlock(&g_render);
    return PSPDF_ERR;
  }

  int out = PSPDF_ERR;
  PopplerPage *pg = poppler_document_get_page(d->doc, page - 1);
  if (pg) {
    char *text;
    /* poppler_page_get_text_for_area() takes points from the top-left,
     * unlike find_text and link mappings which count from the bottom. */
    if (rect) {
      double s = zoom_scale(zoom);
      PopplerRectangle r;
      r.x1 = rect[0] / s;
      r.y1 = rect[1] / s;
      r.x2 = (rect[0] + rect[2]) / s;
      r.y2 = (rect[1] + rect[3]) / s;
      text = poppler_page_get_text_for_area(pg, &r);
    } else {
      text = poppler_page_get_text(pg);
    }
    put_atari(buf, len, text);
    out = (int)strlen(buf);
    g_free(text);
    g_object_unref(pg);
  }

  pthread_mutex_unlock(&g_render);
  return out;
}

int pspdf_links(int handle, int page, int zoom, int32_t *out, int max)
{
  pspdf_doc *d = doc_of(handle);
  if (!d || page < 1 || page > d->pages) return PSPDF_ERR;

  if (pthread_mutex_trylock(&g_render) != 0) return PSPDF_BUSY;

  if (!d->used || !d->doc) {           /* closed while we were waiting */
    pthread_mutex_unlock(&g_render);
    return PSPDF_ERR;
  }

  int n = 0;
  PopplerPage *pg = poppler_document_get_page(d->doc, page - 1);
  if (pg) {
    double pw, ph;
    poppler_page_get_size(pg, &pw, &ph);

    d->uris.clear();
    d->uri_page = page;

    GList *maps = poppler_page_get_link_mapping(pg);
    for (GList *l = maps; l && n < max; l = l->next) {
      PopplerLinkMapping *m = (PopplerLinkMapping *)l->data;
      if (!m || !m->action) continue;

      int kind = -1, target = 0;
      if (m->action->type == POPPLER_ACTION_URI) {
        kind = PSPDF_LINK_URI;
        target = (int)d->uris.size();
        d->uris.push_back(m->action->uri.uri ? m->action->uri.uri : "");
      } else if (m->action->type == POPPLER_ACTION_GOTO_DEST) {
        kind = PSPDF_LINK_PAGE;
        target = dest_page(d->doc, m->action->goto_dest.dest);
        if (target <= 0) continue;
      } else {
        continue;
      }

      if (out) {
        rect_to_pixels(&m->area, ph, zoom, out + n * 6);
        out[n * 6 + 4] = kind;
        out[n * 6 + 5] = target;
      }
      n++;
    }
    poppler_page_free_link_mapping(maps);
    g_object_unref(pg);
  }

  pthread_mutex_unlock(&g_render);
  return n;
}

int pspdf_link_uri(int handle, int page, int index, char *buf, int len)
{
  pspdf_doc *d = doc_of(handle);
  if (!d || !buf || len <= 0) return PSPDF_ERR;
  if (d->uri_page != page || index < 0 || index >= (int)d->uris.size())
    return PSPDF_ERR;

  put_atari(buf, len, d->uris[index].c_str());
  return (int)strlen(buf);
}

int pspdf_outline(int handle, int index, int32_t *depth, int32_t *page,
                  char *title, int title_len)
{
  pthread_mutex_lock(&g_lock);
  pspdf_doc *d = doc_of(handle);
  int rc = PSPDF_ERR;

  if (d && index >= 0 && index < (int)d->outline.size()) {
    const pspdf_outline_item &it = d->outline[index];
    if (depth) *depth = it.depth;
    if (page)  *page  = it.page;
    if (title && title_len > 0)
      snprintf(title, (size_t)title_len, "%s", it.title.c_str());
    rc = PSPDF_OK;
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

int pspdf_meta(int handle, int which, char *buf, int len)
{
  pspdf_doc *d = doc_of(handle);
  if (!d || !buf || len <= 0) return PSPDF_ERR;

  if (pthread_mutex_trylock(&g_render) != 0) return PSPDF_BUSY;

  if (!d->used || !d->doc) {           /* closed while we were waiting */
    pthread_mutex_unlock(&g_render);
    return PSPDF_ERR;
  }

  char *v = NULL;
  switch (which) {
    case 0: v = poppler_document_get_title(d->doc);    break;
    case 1: v = poppler_document_get_author(d->doc);   break;
    case 2: v = poppler_document_get_subject(d->doc);  break;
    case 3: v = poppler_document_get_producer(d->doc); break;
    case 4: v = poppler_document_get_creator(d->doc);  break;
    default: break;
  }

  put_atari(buf, len, v);
  g_free(v);
  pthread_mutex_unlock(&g_render);
  return (int)strlen(buf);
}
