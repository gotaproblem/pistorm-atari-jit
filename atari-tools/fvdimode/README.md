# FVDIMODE - fVDI screen mode picker

A GEM accessory (and program - same binary) that sets the fVDI mode used by
the `aranym.sys` driver on the PiStorm fVDI framebuffer (`vga ... FVDI`).

fVDI reads its mode from `FVDI.SYS` once, at boot, and the AES keeps the
screen size it started with, so the mode cannot be switched under a running
desktop. FVDIMODE edits the `mode WxHxD@F` option on the aranym.sys driver
line and offers a warm reboot.

## Use

* `FVDIMODE.ACC` in the boot drive root -> **Desk > fVDI Mode**, or run
  `FVDIMODE.PRG`.
* Shows the mode running now and the one in `FVDI.SYS`.
* Resolutions: 640x480, 800x600, 1024x768, 1280x720, 1280x1024, 1920x1080.
  Colours: 256 (8 bit), 65K (16 bit), 16M (32 bit). All fit the 16 MB host
  framebuffer and the 1920x1200 renderer limit.
* **Save** writes `FVDI.SYS`; **Save+Reboot** writes it and warm-boots.

## What it changes

* `FVDI.SYS` is found where fVDI finds it: boot drive root, then `C:\`,
  then `A:\`.
* The driver line is the first `##[r|p] <driver>` line naming `aranym`,
  else the first driver line. Commented (`#`) lines are ignored.
* Only the value after `mode` is replaced (the `@freq` is kept; `@60` when
  there was none). If the line has no `mode`, ` mode WxHxD@60` is appended
  to it. Every other byte, line endings included, is written back unchanged.
* The previous file is saved as `FVDI.BAK` first; nothing is written if the
  backup fails.

## 8 bit

Programs that check `vq_scrninfo` for 8-bit packed pixels (e.g. BALL.PRG)
need the packed-pixel driver made by `aranym8packed.py`. The stock
`aranym.sys` reports its 8-bit mode as interleaved bitplanes; with it, start the
emulator with `PISTORM_FVDI8_PLANAR=1`.
