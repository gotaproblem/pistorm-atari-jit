// SPDX-License-Identifier: MIT
//
// PSPDF - host-side PDF rendering for the guest (see pdf-viewer-design.md).
//
// The guest opens a PDF by name (or hands over the bytes), asks for a page at
// a zoom, and later copies the visible part of the rendered page into its own
// buffer in the fVDI pixel format. Poppler does the work on an ARM core; the
// 68k only asks.
//
// Rendering happens on a worker thread, so the NatFeat handler never blocks
// the CPU thread for longer than a mutex trylock - drawing a page takes tens
// of milliseconds, which is far too long to sit inside a 68k instruction.
//
// Everything that touches a document takes that document's lock. Calls made
// from the CPU thread use a trylock and return PSPDF_BUSY rather than wait,
// so the guest polls instead of stalling.

#ifndef PSPDF_H
#define PSPDF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSPDF_API_VERSION 3

/* Results shared by most calls */
#define PSPDF_OK        0
#define PSPDF_ERR      (-1)
#define PSPDF_LOCKED   (-2)   /* needs a password */
#define PSPDF_BUSY     (-3)   /* worker holds the document, try again */

/* PSPDF sub-operations (low 20 bits of the NatFeat id) */
enum pspdf_subop {
  PSPDF_VERSION  = 0,   /* -> API version                                    */
  PSPDF_OPEN     = 1,   /* p0 path            -> handle > 0                  */
  PSPDF_OPENMEM  = 2,   /* p0 guest buf, p1 len -> handle > 0                */
  PSPDF_CLOSE    = 3,   /* p0 handle                                         */
  PSPDF_INFO     = 4,   /* p0 handle, p1 what -> value                       */
  PSPDF_PAGESIZE = 5,   /* p0 handle, p1 page, p2 zoom -> (w<<16)|h          */
  PSPDF_RENDER   = 6,   /* p0 handle, p1 page, p2 zoom, p3 rot -> queued     */
  PSPDF_STATUS   = 7,   /* p0 handle -> 0 ready, 1 busy, -1 failed           */
  PSPDF_FETCH    = 8,   /* p0 handle, p1 dest, p2 x, p3 y, p4 w, p5 h,
                         * p6 bpp, p7 row bytes, p8 background RGB           */
  PSPDF_FIND     = 9,   /* p0 handle, p1 needle, p2 page, p3 flags, p4 zoom,
                         * p5 result buf, p6 max -> hits on that page        */
  PSPDF_TEXT     = 10,  /* p0 handle, p1 page, p2 zoom, p3 rect, p4 buf,
                         * p5 len -> bytes (Atari charset)                   */
  PSPDF_LINKS    = 11,  /* p0 handle, p1 page, p2 zoom, p3 buf, p4 max       */
  PSPDF_LINKURI  = 12,  /* p0 handle, p1 page, p2 index, p3 buf, p4 len      */
  PSPDF_OUTLINE  = 13,  /* p0 handle, p1 index, p2 buf -> 0 / -1             */
  PSPDF_META     = 14,  /* p0 handle, p1 which, p2 buf, p3 len               */
  PSPDF_HILITE   = 15,  /* p0 handle, p1 page, p2 rects (int32 x,y,w,h x n),
                         * p3 n (0 clears, max 64), p4 RGB: blended into the
                         * page by FETCH, host-side, so the guest never
                         * touches a pixel for a search hit or a selection */
  PSPDF_PREFETCH = 16,  /* like RENDER, but the page only goes into the
                         * cache: what the guest is looking at, and what
                         * FETCH returns, do not change. For the next page. */
  PSPDF_CONTINUOUS = 17 /* p0 handle, p1 on/off, p2 gap in pixels: with it
                         * on, FETCH rows above the page come from the
                         * previous page and rows below it from the next
                         * (when the cache has them), with a gap of
                         * background between - so the guest can scroll
                         * through a document as one strip.               */
};

#define PSPDF_HILITE_MAX 64

/* PSPDF_INFO selectors */
enum pspdf_info {
  PSPDF_INFO_PAGES   = 0,
  PSPDF_INFO_OUTLINE = 1,   /* flattened outline entries                     */
  PSPDF_INFO_FLAGS   = 2    /* bit0 has outline, bit1 encrypted              */
};

/* PSPDF_FIND flags */
#define PSPDF_FIND_CASE   1
#define PSPDF_FIND_WORDS  2

/* Link kinds returned by PSPDF_LINKS */
#define PSPDF_LINK_PAGE   0   /* target = page number, 1-based               */
#define PSPDF_LINK_URI    1   /* target = index for PSPDF_LINKURI            */

/* Zoom is percent x 10: 1000 = 100% = 96 DPI (PDF points x 96/72). */
#define PSPDF_ZOOM_100    1000
#define PSPDF_BASE_DPI    96.0

/* All of these are called from the NatFeat handler on the CPU thread and
 * return promptly. Geometry is in device pixels at the given zoom. */

int      pspdf_open(const char *host_path);
int      pspdf_open_mem(const uint8_t *data, size_t len);
void     pspdf_close(int handle);
void     pspdf_shutdown(void);            /* emulator exit                   */

long     pspdf_info(int handle, int what);
long     pspdf_page_size(int handle, int page, int zoom);

int      pspdf_render(int handle, int page, int zoom, int rot);
int      pspdf_prefetch(int handle, int page, int zoom, int rot);
int      pspdf_status(int handle);

/* rectangles (page pixels at the current zoom) FETCH blends over the page
 * in rgb, translucent with a stronger edge; n = 0 clears them */
int      pspdf_hilite(int handle, int page, const int32_t *rects, int n,
                      uint32_t rgb);
int      pspdf_continuous(int handle, int on, int gap);

/* Copy (x,y,w,h) of the rendered page into dst, converting to the fVDI
 * format (bpp 32: 00 RR GG BB; bpp 16: big-endian RGB565). Pixels outside
 * the page get bg. dst_stride is in bytes. */
int      pspdf_fetch(int handle, int x, int y, int w, int h, int bpp,
                     uint8_t *dst, size_t dst_stride, uint32_t bg);

/* One page at a time: the guest loops over pages so it stays responsive.
 * rects is filled with 4 int32 per hit (x, y, w, h). */
int      pspdf_find(int handle, const char *needle_utf8, int page, int flags,
                    int zoom, int32_t *rects, int max);

int      pspdf_text(int handle, int page, int zoom, const int32_t *rect,
                    char *buf, int len);

/* links: 6 int32 per entry (x, y, w, h, kind, target) */
int      pspdf_links(int handle, int page, int zoom, int32_t *out, int max);
int      pspdf_link_uri(int handle, int page, int index, char *buf, int len);

/* outline entry: depth, page, then a NUL-terminated title of up to
 * PSPDF_TITLE_MAX bytes */
#define PSPDF_TITLE_MAX 80
int      pspdf_outline(int handle, int index, int32_t *depth, int32_t *page,
                       char *title, int title_len);

int      pspdf_meta(int handle, int which, char *buf, int len);

/* UTF-8 <-> Atari ST charset, for text going either way across the bus. */
void     pspdf_utf8_to_atari(const char *utf8, char *out, size_t out_len);
void     pspdf_atari_to_utf8(const char *atari, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* PSPDF_H */
