/* SPDX-License-Identifier: MIT
 *
 * SDL3/SDL.h  —  shim with two modes, selected per translation unit:
 *
 *  - Default (CPU core, JIT, uae/string.h): a minimal stub. Those files include
 *    <SDL3/SDL.h> but only ever reference SDL_Quit() (under JIT_DEBUG, off) and
 *    the SDL_str* helpers in uae/string.h are dead code, so nothing real is
 *    needed. Keeping the stub avoids dragging the whole SDL3 header into the
 *    Amiberry-derived core.
 *
 *  - Real SDL3 (audio only): dmasnd_hdmi.c is compiled with -DPISTORM_REAL_SDL3
 *    (plus the sdl3 pkg-config cflags), so this header forwards to the actual
 *    system <SDL3/SDL.h> via #include_next. That gives the audio backend the
 *    real SDL3 API without exposing it to any other file.
 */

#ifdef PISTORM_REAL_SDL3
#  include_next <SDL3/SDL.h>
#else
#  ifndef PISTORM_SDL3_SHIM_H
#  define PISTORM_SDL3_SHIM_H
     static inline void SDL_Quit(void) { }
#  endif
#endif
