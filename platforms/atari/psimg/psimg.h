// SPDX-License-Identifier: MIT
//
// PSIMG — host-side image decoding for the guest (PNG/JPG wallpapers etc.).
//
// The guest asks for an image file decoded, scaled to a target size and
// converted to the fVDI framebuffer pixel format; the Pi does all the work
// (stb_image + stb_image_resize) and the result is copied into a
// guest-supplied buffer. Decoding a JPEG on a 1.5 GHz ARM takes tens of
// milliseconds; on a 68k it would take tens of seconds.

#ifndef PSIMG_H
#define PSIMG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSIMG_API_VERSION 1

/* PSIMG sub-operations (low 20 bits of the NatFeat id) */
enum psimg_subop {
  PSIMG_VERSION = 0,            /* -> API version                            */
  PSIMG_LOAD    = 1,            /* p0 path, p1 dest buf, p2 w, p3 h,
                                 * p4 bpp (16|32), p5 mode -> 0 ok, -1 error */
  PSIMG_INFO    = 2             /* p0 path, p1 which (0=w 1=h) -> dim, -1    */
};

/* PSIMG_LOAD scaling modes */
enum psimg_mode {
  PSIMG_MODE_STRETCH = 0,       /* fill w x h exactly, ignore aspect  */
  PSIMG_MODE_FIT     = 1        /* preserve aspect, letterbox black   */
};

/* Decode path, scale to dw x dh (per mode), convert to the guest pixel
 * format (bpp 32: 00 RR GG BB byte order; bpp 16: big-endian RGB565).
 * On success returns 0 and sets *out (malloc'd, free with psimg_free)
 * and *out_len (= dw * dh * bpp/8). Returns -1 on any failure. */
int psimg_load_scaled(const char *host_path, int dw, int dh, int bpp,
                      int mode, uint8_t **out, size_t *out_len);

void psimg_free(uint8_t *buf);

/* Probe image dimensions without decoding. Returns 0 on success. */
int psimg_probe(const char *host_path, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* PSIMG_H */
