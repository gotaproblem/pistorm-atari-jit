/* et4000_drm.h - direct DRM/KMS scanout presenter (phase 2).
 *
 * Phase 2 vs phase 1:
 *   - HVS HARDWARE SCALING. The guest image is rendered into a small buffer at
 *     its NATIVE resolution and shown on a DRM plane whose destination rect is
 *     the whole display. The vc4 HVS upscales it to fullscreen during scanout,
 *     for free - so every guest mode (320x200 ST ... 1024x768 NOVA) fills the
 *     panel, not just an exact-mode fvdi.
 *   - DOUBLE BUFFERED. Two native source buffers. The emulator renders into its
 *     padded staging buffer as usual; the caller copies the visible frame into
 *     the back source buffer, we flip it onto the plane at vblank, then swap.
 *     The buffer being scanned is never written, so no shear.
 *
 * The present cost is one frame copy (cached staging -> write-combined source)
 * plus a single drmModeSetPlane latched at vblank - no SDL, no GL texture
 * upload, no GPU composite, and the HVS scale is free.
 *
 * Default display path; opt out with PISTORM_VGA_DRM=0 (or PISTORM_VGA_SDL=1).
 * et4000.c falls back to the SDL path if DRM init fails. Requires the console
 * (DRM master) - no X/Wayland/desktop.
 *
 *   drmpres_open()        once: card, connected connector's preferred mode,
 *                         CRTC, a black mode-size primary, and a scaling plane.
 *   drmpres_set_source()  (re)allocate the native double buffer; call whenever
 *                         the guest resolution changes (cheap no-op if same).
 *   drmpres_backbuffer()  the buffer to render THIS frame into (native size).
 *   drmpres_flip()        scale-present the back buffer (already filled by the
 *                         caller from the staging buffer), then swap.
 *   drmpres_close()       restore the saved CRTC and free everything.
 */
#ifndef ET4000_DRM_H
#define ET4000_DRM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the vc4 card, pick the connected connector's preferred mode, light the
 * CRTC with a black primary, and locate a scaling-capable plane. 0 on success,
 * -1 on any failure (caller should fall back to SDL). */
int drmpres_open(void);

/* (Re)allocate the double-buffered native source at w x h. Safe to call every
 * frame: only reallocates when the size actually changes. Returns 0 on success
 * (buffers ready), -1 on failure. After a (re)allocation the next flip performs
 * a full carry so both buffers converge. */
int drmpres_set_source(uint32_t w, uint32_t h);

uint8_t *drmpres_backbuffer(void);   /* current render target (native size) */
uint32_t drmpres_src_pitch(void);    /* bytes per scanline of the source     */
uint32_t drmpres_src_w(void);
uint32_t drmpres_src_h(void);

/* Present the current back buffer, scaled to fill the display, and swap. The
 * caller is expected to have written the complete frame into drmpres_backbuffer()
 * before calling (so the newly-back buffer is always current). */
void drmpres_flip(void);

void drmpres_close(void);

#ifdef __cplusplus
}
#endif

#endif /* ET4000_DRM_H */
