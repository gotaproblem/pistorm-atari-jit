# FVDICON - TOS console on the fVDI screen

AUTO-folder TSR (588 bytes, 68000, position independent). With fVDI the
screen is a host framebuffer the ROM knows nothing about, so the BIOS VT52
console (every TOS program's text, EmuCON included) keeps drawing into the
native ST screen. FVDICON hooks the CON and RAWCON output vectors (XBRA
`PSCN`) and passes each character to the host `FVDICON` NatFeat, a VT52
terminal that draws into the fVDI framebuffer at 8/16/32 bit.

* Until fVDI has set its mode (boot, AUTO text), and for BEL, characters go
  on to the ROM console unchanged.
* The host is given the ROM's own 8x16 system font at install.
* Put FVDICON.PRG in AUTO (any position). Leave fVDI's own `bconout` option
  off. Use `xbiosfix` in FVDI.SYS so TOS programs such as EmuCON do not
  switch the native resolution (which resets the Line-A screen size fVDI's
  mouse clips to).
* Pointless on ET4000 (NOVA/NVDI/XVDI) setups - leave it out of AUTO there.

## Build

`FVDICON.PRG` is committed prebuilt. To rebuild with the crossmint
toolchain:

    make

(no libraries, no relocations - the code is PC-relative throughout).
