# Integrating the PCem ET4000AX engine

This removes your hand-rolled ET4000 emulation and replaces it with PCem's
register core + renderers (the same engine WinUAE/Amiberry use). Your SDL
output, frame pump, PNG dump and native-ST path stay exactly as they are.

## What's in `pcem/`

Vendored PCem engine (unmodified):
- `vid_svga.c` / `vid_svga.h`            — register/timing core, chain4/planar VRAM
- `vid_svga_render.c` / `.h` / `_remap.h`— the 4/8/15/16/24/32bpp scanline renderers
- `vid_et4000.c` / `vid_et4000.h`        — ET4000 extended registers + recalctimings
- `vid_unk_ramdac.c` / `vid_unk_ramdac.h`— Sierra SC1502x RAMDAC (the depth signal)

Compat + glue (mine):
- `pcem_shim.h` / `pcem_shim.c`          — stubs the PCem framework primitives; owns `buffer32`
- `ibm/io/mem/rom/device/video/timer/viewer/plat.h` — one-line stubs so the engine compiles unmodified
- `et4000_engine.c`                      — the bridge you call from your front-end

## Build

1. Add to your Makefile: `pcem/vid_svga.c pcem/vid_svga_render.c pcem/vid_et4000.c
   pcem/vid_unk_ramdac.c pcem/pcem_shim.c pcem/et4000_engine.c`, and `-Ipcem` on CFLAGS.
2. Nothing else. `vid_et4000.c` compiles **unmodified** — the Korean `et4000k_*` /
   `et4000_kasan_*` functions and the three `device_t …_device = {…}` tables build
   fine against the shim (inert font tables + permissive `device_t` + `rom_present`
   stub), even though the glue never invokes them. The whole `pcem/` set has been
   verified to compile with zero unresolved symbols beyond libc and your front-end.

## Rewire your `et4000.c` (5 points)

Delete from your `et4000.c`: `et4000_decode_mode` (both the live one and the
`#if 0` legacy), every `blit_*` function, `planar_write`, and the register/DAC
bookkeeping inside `et4000_io_read8/write8`. The engine owns all of that now.
Keep `sdl_open/sdl_set_logical/sdl_present`, `render_frame`, the PNG code and
`blit_st_native`.

Then delegate (prototypes are in `et4000_engine.c`):

| your function            | new body                                                            |
|--------------------------|---------------------------------------------------------------------|
| `et4000_init()`          | keep your SDL setup; after `sdl_open()` call `et4000_engine_init();` |
| `et4000_io_write8`       | `return et4000_engine_io_write(port, val), 1;`                      |
| `et4000_io_read8`        | `return et4000_engine_io_read(port);`                               |
| `et4000_vram_write8/16/32`| `et4000_engine_vram_write8/16/32(offset, val);`                    |
| `et4000_vram_read8/16/32` | `return et4000_engine_vram_read8/16/32(offset);`                   |
| `et4000_update_display`  | `int w,h; et4000_engine_render(s->fb_mem, ENGINE_PITCH, &w,&h); sdl_set_logical(s,w,h,w,h);` then `sdl_present` as today |

`ENGINE_PITCH` = the pixel pitch of `s->fb_mem` (your staging buffer width;
e.g. `ET4K_MAX_LW`). The engine writes a tightly-packed `w`-wide image, so
pass the staging buffer's row stride in pixels.

Your `video_subsystem & 0x01` gate in `render_frame` still works unchanged.

## Expect a compile-to-green pass — this is the linear part

The symbol scan caught the framework surface (`pclog`, `io_*`, `mem_mapping_*`,
`timer_*`, `rom_init`, `loadfont`, `video_*`, `updatewindowsize`, `viewer_*`),
all stubbed. A first build may surface a few more undefined symbols the scan
missed, or the `device_t` table issue above. That list is short and finite —
paste it and each one is a one-line stub. This is the opposite of the decoder
guessing: it converges.

Two things most likely to need a nudge once it *displays*:
- the `+32` overscan x-offset in `et4000_engine_render` (PCem renders into a
  bordered bitmap),
- the per-line `svga->ma` stepping in that same loop.

Both are isolated to `et4000_engine_render()` — flagged inline — so tuning them
won't touch the engine or your front-end.
