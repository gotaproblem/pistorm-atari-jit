// SPDX-License-Identifier: MIT
//
// PSIMG host-side implementation. See psimg.h.

#include "platforms/atari/psimg/psimg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb/stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb/stb_image_resize2.h"

int psimg_probe(const char *host_path, int *w, int *h)
{
  int comp;

  if (!stbi_info(host_path, w, h, &comp))
    return -1;

  return 0;
}

int psimg_load_scaled(const char *host_path, int dw, int dh, int bpp,
                      int mode, uint8_t **out, size_t *out_len)
{
  int sw, sh, comp;
  unsigned char *src;
  unsigned char *rgb;
  uint8_t *dst;
  size_t npix, len, i;
  int bw, bh, ox, oy;

  if (dw <= 0 || dh <= 0 || dw > 8192 || dh > 8192)
    return -1;
  if (bpp != 16 && bpp != 32)
    return -1;

  src = stbi_load(host_path, &sw, &sh, &comp, 3);
  if (!src) {
    printf("[PSIMG] cannot decode %s: %s\n", host_path, stbi_failure_reason());
    return -1;
  }

  /* target box inside dw x dh */

  if (mode == PSIMG_MODE_FIT) {
    /* preserve aspect ratio, letterbox with black */
    double sx = (double)dw / (double)sw;
    double sy = (double)dh / (double)sh;
    double s = (sx < sy) ? sx : sy;

    bw = (int)((double)sw * s + 0.5);
    bh = (int)((double)sh * s + 0.5);
    if (bw < 1) bw = 1;
    if (bw > dw) bw = dw;
    if (bh < 1) bh = 1;
    if (bh > dh) bh = dh;
    ox = (dw - bw) / 2;
    oy = (dh - bh) / 2;
  } else {
    bw = dw;
    bh = dh;
    ox = 0;
    oy = 0;
  }

  rgb = (unsigned char *)calloc((size_t)dw * (size_t)dh, 3);
  if (!rgb) {
    stbi_image_free(src);
    return -1;
  }

  if (!stbir_resize_uint8_srgb(src, sw, sh, 0,
                               rgb + ((size_t)oy * (size_t)dw + (size_t)ox) * 3,
                               bw, bh, dw * 3, STBIR_RGB)) {
    stbi_image_free(src);
    free(rgb);
    return -1;
  }

  stbi_image_free(src);

  /* convert to guest pixel bytes */

  npix = (size_t)dw * (size_t)dh;
  len = npix * (bpp / 8);
  dst = (uint8_t *)malloc(len);
  if (!dst) {
    free(rgb);
    return -1;
  }

  if (bpp == 32) {
    /* fVDI 32bpp: 00 RR GG BB, big-endian in guest memory */
    for (i = 0; i < npix; i++) {
      dst[i * 4 + 0] = 0;
      dst[i * 4 + 1] = rgb[i * 3 + 0];
      dst[i * 4 + 2] = rgb[i * 3 + 1];
      dst[i * 4 + 3] = rgb[i * 3 + 2];
    }
  } else {
    /* fVDI 16bpp: big-endian RGB565 */
    for (i = 0; i < npix; i++) {
      unsigned r = rgb[i * 3 + 0];
      unsigned g = rgb[i * 3 + 1];
      unsigned b = rgb[i * 3 + 2];
      unsigned p = ((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3);

      dst[i * 2 + 0] = (uint8_t)(p >> 8);
      dst[i * 2 + 1] = (uint8_t)(p & 0xFF);
    }
  }

  free(rgb);

  *out = dst;
  *out_len = len;
  return 0;
}

void psimg_free(uint8_t *buf)
{
  free(buf);
}
