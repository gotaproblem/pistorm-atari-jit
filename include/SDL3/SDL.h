/* SPDX-License-Identifier: MIT
 *
 * SDL3/SDL.h  —  minimal shim for the headless pistorm JIT build.
 *
 * jit/arm/compemu_support_arm.cpp does an unconditional `#include <SDL3/SDL.h>`,
 * but the only thing it ever uses from SDL is SDL_Quit(), and that sits inside
 * `#ifdef JIT_DEBUG` (which we do not define). So nothing from real SDL is
 * actually needed — this stub just satisfies the include. SDL_Quit() is provided
 * anyway, in case JIT_DEBUG is ever switched on.
 *
 * Place at  <tree>/include/SDL3/SDL.h  so `-Iinclude` resolves the angle-bracket
 * include. Remove this shim (and use the real SDL dev headers) only if you later
 * pull in SDL-dependent Amiberry code.
 */

#ifndef PISTORM_SDL3_SHIM_H
#define PISTORM_SDL3_SHIM_H

static inline void SDL_Quit(void) { }

#endif /* PISTORM_SDL3_SHIM_H */
