# HOSTDOS - HOSTFS drives under plain TOS / EmuTOS

AUTO-folder TSR (1.6 KB, 68000, position independent). Under FreeMiNT the
emulator's `hostfs` drives come through `hostfs.xfs`; plain TOS has no
file-system plug-ins, so without this a `hostfs S /home/pistorm/atari-share`
drive simply does not exist on TOS or EmuTOS. HOSTDOS hooks trap #1 (XBRA
`PSHD`) and hands every GEMDOS call that names a HOSTFS drive to the host's
`HOSTDOS` NatFeat (`platforms/atari/network/hostdos.cpp`), which runs it on
the Pi's folder. Everything else goes to the ROM untouched.

* Put `HOSTDOS.PRG` in `C:\AUTO` (any position). It prints the drives it
  serves, e.g. `HOSTDOS: S: on the Pi`, and adds them to `_drvbits`. On the
  TOS desktop use *Install Disk Drive* once to give the drive an icon and
  *Save Desktop*; EmuTOS's desktop shows it by itself.
* Read **and write**: open, read, write, seek, create, delete, rename, file
  dates and the read-only attribute, folders (create, delete, set/get path),
  free space, and running programs straight from the drive (Pexec 0 and 3,
  so the desktop, the file selector and shells all work). A cfg line ending
  in `ro` makes the drive write-protected (GEMDOS error -13).
* Names: the Pi's files appear under their 8.3 TOS names. Names that already
  are valid 8.3 just become upper case; long names, spaces, a second dot or
  non-ASCII become `BASE~N.EXT` (`A Long Name.text` -> `ALONGN~1.TEX`),
  numbered in host-name order so a folder lists the same way every time.
  Dot-files are hidden. New files and folders are created under the 8.3 name
  the program used (upper case, as TOS writes them).
* Handles 64..95 are HOSTDOS files (32 open at once); a program's files are
  closed when it terminates. Volume labels are not provided.
* It refuses to install under FreeMiNT (the `MiNT` cookie) - `hostfs.xfs`
  does the job there. If MiNT is started after it, the host stops answering
  for HOSTDOS as soon as `hostfs.xfs` mounts.
* Needs pistorm-atari-jit with the HOSTDOS NatFeat and at least one `hostfs`
  line; otherwise it prints a line and exits. A drive letter TOS already has
  (a hard-disk partition, an emulated IDE drive) is never taken over.
* `debug hostfs` (or `PISTORM_HOSTFS_DEBUG=1`) traces every HOSTDOS call on
  the Pi console as `[HOSTDOS] ...`.

## Build

`HOSTDOS.PRG` is committed prebuilt (`make binutils` with Debian/Ubuntu
`binutils-m68k-linux-gnu`; `make` with the crossmint toolchain gives the same
code). No libraries, no relocations - PC-relative throughout.

## Test

`tools/hostdos-test/` runs this exact binary under Musashi over a fake ROM
GEMDOS, with the real host engine behind the NatFeat - see its README.
