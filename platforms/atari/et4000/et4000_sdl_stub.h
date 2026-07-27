/* SPDX-License-Identifier: MIT
 *
 * et4000_sdl_stub.h — no-op SDL2 shim for the DRM-only build.
 *
 * et4000.c has an SDL2 render backend interleaved with the DRM/fbdev paths in
 * shared functions (sdl_open/sdl_set_logical/sdl_present/sdl_pump/sdl_close).
 * When PISTORM_ENABLE_SDL_DISPLAY is NOT defined (the default), et4000.c pulls
 * in this shim instead of <SDL2/SDL.h>: every SDL call becomes a compile-time
 * no-op, so no SDL2 symbols are referenced and the binary links no libSDL2 -
 * which frees the process to link SDL3 (audio + the CPU core's SDL_str* string
 * helpers) with no symbol clash. The SDL calls are never reached at runtime
 * because the backend selection defaults to DRM (or fbdev); they exist only so
 * the file still compiles.
 *
 * To use the real SDL2 display backend again, build with
 * -DPISTORM_ENABLE_SDL_DISPLAY and re-add SDL2 to the Makefile.
 */

#ifndef ET4000_SDL_STUB_H
#define ET4000_SDL_STUB_H

#include <stdint.h>

typedef uint32_t Uint32;
typedef uint16_t Uint16;
typedef uint8_t  Uint8;

/* Opaque handle types (used only as pointers). */
typedef struct SDL_Window   SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture  SDL_Texture;

typedef struct { int x, y, w, h; } SDL_Rect;
typedef struct { const char *name; Uint32 flags; } SDL_RendererInfo;

typedef struct {
    Uint32 type;
    struct {
        struct { int sym; Uint16 mod; } keysym;
    } key;
} SDL_Event;

/* Constants (values are irrelevant - the code paths using them never run). */
#define SDL_INIT_VIDEO                0u
#define SDL_NO_SIGNAL_HANDLERS        0u
#define SDL_WINDOWPOS_UNDEFINED       0
#define SDL_WINDOW_SHOWN              0u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0u
#define SDL_RENDERER_ACCELERATED      0u
#define SDL_RENDERER_SOFTWARE         0u
#define SDL_PIXELFORMAT_ARGB8888      0u
#define SDL_PIXELFORMAT_XRGB8888      0u
#define SDL_TEXTUREACCESS_STREAMING   0
#define SDL_ScaleModeNearest          0
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"
#define SDL_DISABLE                   0
#define SDL_QUIT                      0x100u
#define SDL_KEYDOWN                   0x300u
#define SDLK_ESCAPE                   27
#define KMOD_CTRL                     0x00C0

/* Functions -> no-op macros. Variadic so arguments are simply ignored; each
 * yields a benign "failure/empty" value so that if the SDL path were ever taken
 * it fails gracefully and the caller falls back to DRM. */
#define SDL_Init(...)                   (-1)
#define SDL_Quit(...)                   ((void)0)
#define SDL_SetHint(...)                (0)
#define SDL_GetError(...)               ("SDL display disabled")
#define SDL_GetCurrentVideoDriver(...)  ("none")
#define SDL_CreateWindow(...)           ((SDL_Window *)0)
#define SDL_DestroyWindow(...)          ((void)0)
#define SDL_CreateRenderer(...)         ((SDL_Renderer *)0)
#define SDL_DestroyRenderer(...)        ((void)0)
#define SDL_CreateTexture(...)          ((SDL_Texture *)0)
#define SDL_DestroyTexture(...)         ((void)0)
#define SDL_GetRendererInfo(...)        (-1)
#define SDL_GetRendererOutputSize(...)  (-1)
#define SDL_LockTexture(...)            (-1)
#define SDL_UnlockTexture(...)          ((void)0)
#define SDL_UpdateTexture(...)          (-1)
#define SDL_RenderClear(...)            (-1)
#define SDL_RenderCopy(...)             (-1)
#define SDL_RenderPresent(...)          ((void)0)
#define SDL_RenderReadPixels(...)       (-1)
#define SDL_RenderSetLogicalSize(...)   (-1)
#define SDL_SetRenderDrawColor(...)     (-1)
#define SDL_SetTextureScaleMode(...)    ((void)0)
#define SDL_ShowCursor(...)             (0)
#define SDL_PollEvent(...)              (0)

#endif /* ET4000_SDL_STUB_H */
