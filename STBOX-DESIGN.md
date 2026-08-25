# STBOX — a sandboxed ST (Musashi) in a GEM window

Native ST games need a real 8 MHz 68000 with ST timing; the JIT'd 68040 +
FreeMiNT is the wrong machine for them. STBOX is a second, fully private
Atari ST inside the emulator process — its own ST-RAM, TOS ROM and
Shifter/MFP/PSG/ACIA/FDC models, executed by Musashi, displayed on a DRM
overlay plane positioned over a GEM window, controlled through NatFeats.

## Where everything lives

| Piece | File(s) |
|---|---|
| Musashi 4.10 core | `third_party/musashi/` (see VENDOR-NOTES; bus interface renamed to `stbox_bus_*` in `m68kconf.h` so it can never link against the REAL bus accessors in emulator.c) |
| Machine model + slice engine | `platforms/atari/stbox/stbox.c` |
| Host side: ROM load, DRM plane, render thread | `platforms/atari/stbox/stbox_host.c` |
| Core-3 hook | `emulator.c`, ipl_task housekeeping slot: `stbox_slice()` |
| Control plane | `NF_FEATURE_STBOX` in `platforms/atari/network/atari_natfeat.cpp` |
| Input routing | `platforms/atari/kbd_usb.c`: `send_key()` / `mouse_flush()` divert to the box while its window is focused; **F11 toggles routing** (the mouse is captured, so F11 is how you get the desktop pointer back) |
| GEM front-end | STBOX.PRG, prebuilt at `configs/gem-binaries/STBOX.PRG` (source kept out of the repo with the rest of cdev/; builds with m68k-atari-mint-gcc + gemlib) |

## Execution model

`stbox_slice()` runs inside the ipl_task loop on isolated core 3, under
that loop's admission rule (no syscalls, no locks, bounded sub-microsecond
work). Each call executes at most one 64-guest-cycle burst (~10–20
instructions, ~200–400 ns host) and only when the 8.021248 MHz pace owes
one. Pacing debt is a SIGNED 32.32 fixed-point accumulator against
CNTVCT_EL0 — see the comment at `g_cyc_debt_fp` for why signed matters.
Everything the slice path touches is process memory; file I/O, DRM and SDL
stay in `stbox_host.c` on cores 0/1.

Frame layout: 512 cycles/line × 313 lines (PAL) = 50.05 Hz VBL. MFP clock
tracked at 2 457 600 Hz via fixed point. IKBD bytes paced at 7812.5 baud.

## Hardware-model findings (paid for in debugging, do not rediscover)

1. **Timer B counts DE, not HBL.** The event input pulses only on the ~200
   visible lines (63–262 here). TOS's boot VBL-sync polls for the pulses
   to STOP (a ~616-iteration gap = vertical blank); firing every line
   hangs every TOS at `$FC0DF0` (1.04).
2. **Decode is block-granular.** Gaps inside a decoded register block
   (MMU, GLUE/Shifter, DMA/FDC, PSG, MFP, ACIA) are silent no-ops reading
   0. Bus errors are only for whole absent blocks (blitter, STE DMA
   sound, undecoded space). TOS 2.06's STE probe writes $5A to $FF820D
   with the vector table deliberately trashed — bus-erroring there crashes
   the boot; reading 0 back is precisely how it concludes "plain ST".
3. **MOVE16 must trap on a 68000.** Upstream Musashi installs the
   68040-only MOVE16 handler ($F620-F627) for every CPU type with no
   runtime guard, so it EXECUTES on a 68000, swallowing its extension
   word. TOS 1.04's AES is built on line-F trampolines; a single
   desktop click derails the stream into a junk dispatch (two bombs).
   Fixed locally in m68kops.c (guard + exception_1111); the vendored
   core is also compiled 68000-generation-only now. The sandbox IKBD
   additionally honours mouse mode ($08/$09/$0A/$12) - a disabled
   mouse sends no packets, like the real 6301.
4. **Signed pacing debt.** Musashi overshoots the slice budget by up to
   one instruction; unsigned debt wraps, the clamp gifts a frame, and the
   box runs 2.56× fast.

## Video out

Sandbox Shifter → planar-to-XRGB8888 conversion (all three ST modes) into
double-buffered dumb buffers → spare overlay plane on the guest CRTC
(never the guest's plane, never a plane with an FB attached — so vidplay
and STBOX coexist), non-blocking atomic commits per vidplane.c's rules.
The GEM front-end reports rect/clip/focus in guest desktop pixels; the
render thread maps them through `drmpres_dst_x/y/w/h`,`drmpres_src_w/h`
every frame. `PISTORM_STBOX_PLANE=<id>` forces a plane.

## NatFeat protocol ("STBOX")

START(path,ram_kb,flags) STOP RESET STATUS RECT(x,y,w,h) CLIP(x,y,w,h)
FOCUS(0/1) STATS(ptr to 4 longs: cps, frames, overruns, running)
KEY(scan,down) MOUSE(dx,dy,buttons) JOY(n,state) — see `enum nf_stbox_ops`.
TOS path accepts a HOSTFS drive form (`X:\...`) or a literal host path;
empty falls back to `PISTORM_STBOX_TOS`.

## Testing without the Pi

`stbox.c` + Musashi compile and run on any host. The harness boots a real
TOS image headless by driving `stbox_slice()` with a synthetic arch timer
and checks VBL rate, cycles/sec, video base, palette and screen content.
Verified: TOS 1.04 UK, TOS 2.06 UK (its cold-boot RAM test takes ~15
guest-seconds), EmuTOS 256K 1.3 — all at 50.0 VBL/s, 8 021 2xx cycles/s.
TOS 2.06 sizes flat RAM without bank aliasing correctly (4 MB, phystop
$400000).

## Status

Working: boot to desktop; games boot from .ST/.MSA images (WD1772 with
Type I/II/III commands, DMA, side select, index pulse, READ TRACK
synthesized from sector data with real CRCs); GEM file selector in
STBOX.PRG (run with no arguments) browsing HOSTFS for images - the
sandbox's answer to the Gotek's OLED menu; PSG audio into the SDL mixer;
USB keyboard+mouse via focus routing (ESC toggles capture); MMU bank
aliasing so 512K/1M/2M/4M all size correctly; double-bus-fault = halt
with a full crash report (vault decode, 64K PC trace, disassembly of the
scene, write watchpoints with FDC attribution).

Real-Gotek bridge (stbox_realfdc.c) is EXPERIMENTAL, off by default
(STBOX.PRG "gotek" argument): errand pump in the JIT spcflags window,
flock test-and-set against the main guest's ACSI, PSG select latch
save/restore. Field status: not yet booting; park until wanted.

Not yet: GEMDOS HD, STE tier, scanline palette (frame tier - mid-frame
rasters smear), disk write-back (writes stay in memory), joystick host
routing (NatFeat path exists), real-IKBD routing into the box (USB only;
the ACIA-shim divert is not the path the native-mouse natmem hook takes).
