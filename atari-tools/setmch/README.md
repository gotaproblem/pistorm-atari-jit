# SETMCH.PRG — force the machine (`_MCH`) cookie

STE-specific software checks the `_MCH` cookie (STE = `0x00010000`, plain ST =
`0x00000000`) and refuses to run — or hides its STE features — when it reads
"ST". On a plain-ST PiStorm the cookie is "ST" even though the emulator
provides an **STE blitter** and **STE DMA sound** at the real register
addresses. `SETMCH.PRG` overwrites `_MCH` so that cookie-gated STE software
runs and uses those emulated features.

It lives in the **AUTO folder** deliberately: it must behave identically in a
plain-GEM/TOS boot (games) and a FreeMiNT boot (MOD players, GEM apps),
independent of the desktop. It patches the cookie jar every environment
shares, so anything launched afterwards sees the machine you chose.

## Install

1. Build it (needs the m68k-atari-mint cross toolchain — crossmint):

   ```
   make
   ```

2. Copy `SETMCH.PRG` into your Atari `AUTO` folder.

3. **Under FreeMiNT**, make sure it runs *after* `MINT.PRG` — the name `S…`
   already sorts after `M…`, so it does by default. This lets MiNT finish
   detecting the real machine before the cookie changes; only later-launched
   programs see the faked value.

4. Reboot.

## Choosing the machine

Defaults to **STE**. To pick another, put a one-line file `SETMCH.INF` in the
**root of the boot drive** containing one of:

```
ST   STE   MEGASTE   TT   FALCON        (case-insensitive)
```

or a raw hex value, e.g. `0x00030000` (Falcon).

## What it does and does not fix

**Fixes:** software that refuses to start, or disables its STE code paths,
purely because the cookie says "ST" — when the STE features it then uses are
the blitter and/or DMA sound (both emulated).

**Does not fix (hardware / timing, not the cookie):**

- **STE video** — hardware fine scroll, the 4096-colour palette, split-screen
  — is the real video shifter, which a plain ST does not have. Forcing the
  cookie makes such software *try* those and get nothing.
- **Cycle-exact software** (beam-racing demos, some games) still breaks under
  the JIT, which does not run in the ST's fixed 8 MHz lockstep with the beam.

So this unlocks well-behaved, cookie-gated STE software — not beam-racing
demos, and not STE-video effects on a plain-ST board.

## Requirements

A cookie jar must exist. EmuTOS, all modern TOS (1.06+/STE/TT/Falcon) and
FreeMiNT provide one; only TOS 1.00–1.04 lack it, in which case the tool
reports "no cookie jar" and changes nothing.
