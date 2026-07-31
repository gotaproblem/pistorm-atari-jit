# USB/Bluetooth keyboard & mouse → ACIA/IKBD injection

Adds a `kbd usb` config option to pistorm-atari-jit: keyboards and mice
plugged into (or Bluetooth-paired with) the Raspberry Pi 4 are injected into
the Atari's keyboard ACIA stream, while the real IKBD keyboard and mouse
keep working alongside.

## How it works

The ACIA (6850) and MFP are real chips on the ST bus, so you cannot push a
received byte into them from the CPU side. Instead the emulator shadows the
guest-visible registers and synthesises the interrupt:

1. **Input** — a host thread reads Linux evdev devices (`/dev/input/event*`).
   USB and Bluetooth HID both surface there, so Bluetooth needs nothing
   beyond normal `bluetoothctl` pairing on the Pi. Hotplug is handled via
   inotify. Keys map to ST scancodes; mouse motion becomes IKBD relative
   packets (`0xF8|buttons, dx, dy`); the wheel maps to Up/Down arrow taps.

2. **Presentation** — injected bytes are paced at the real IKBD serial rate
   (7812.5 bps ≈ 1.28 ms/byte). Reads of the keyboard ACIA status/data
   (`$FFFC00/$FFFC02`) and MFP GPIP (`$FFFA01`) are shadowed: when an
   injected byte is presented, status shows RDRF|IRQ and GPIP4 reads low,
   exactly as with a real received byte. Real IKBD traffic passes through
   untouched and always has priority.

3. **Merging without corruption** — two guards keep the byte streams from
   interleaving mid-packet: a new injected packet only starts after the real
   RX stream has been quiet for ≥ 2 byte-times, and once an injected packet
   has started its remaining bytes take priority until it completes.

4. **Interrupts** — the real MFP can't latch GPIP4 for us, so `ipl_task`
   raises level 6 in the 68k core when a byte is presented (respecting the
   guest's IERB/IMRB keyboard mask, which is snooped from MFP writes), and
   `intlev_ack()` supplies vector `0x46` directly instead of running a bus
   IACK cycle (the real MFP has nothing to acknowledge — an IACK would BERR).
   If a real MFP interrupt races us at level 6, the real one is acknowledged
   on the bus first and the injected byte simply re-raises after the RTE.

5. **IKBD modes** — writes to `$FFFC02` are snooped through an IKBD command
   state machine, so injection follows the guest's configuration: mouse
   pause/disable, y-origin, keycode/absolute modes (injection of mouse
   packets is suppressed in absolute/keycode modes — the real IKBD answers
   position queries), and 6850 master resets / `0x80 0x01` IKBD resets flush
   the injection queue. Command *responses* (interrogate, clock reads, reset
   version byte) come from the real IKBD as before — we never fake them.

## Config

```
kbd usb            # enable, grab devices away from the Pi console (default)
kbd usb nograb     # enable, leave devices shared with the console
```

F12 toggles the grab at runtime (F11/F12 don't exist on an ST, so they're
never forwarded). The emulator needs to run as root for GPIO anyway, which
also covers `/dev/input` access.

Key mapping extras: PageUp → HELP, End/PageDown → UNDO, the ISO `<>` key →
ST scancode `0x60`, NumLock/ScrollLock → keypad `(` `)`. Right Ctrl/Alt fold
onto the single ST Control/Alternate. Edit the `st_scan[]` table in
`platforms/atari/kbd_usb.c` to taste.

## Build (on the Pi 4)

```
cd pistorm-atari-jit
git apply 0001-Add-USB-Bluetooth-keyboard-mouse-injection-into-ACIA.patch   # or git am
make
```

No new library dependencies (evdev + inotify are plain kernel interfaces).

## Test checklist

1. `kbd usb` in the cfg, boot to the desktop. Console should print
   `[KBD] using /dev/input/eventN (...)` per device.
2. Type on the USB keyboard — characters appear in the GEM desktop; the
   real ST keyboard still types; both mice move the pointer.
3. Hold a key: autorepeat is TOS's own (host autorepeat events are
   filtered), so repeat rate should match the real keyboard's.
4. Move real and USB mice simultaneously — pointer may fight but must not
   "explode" (packet interleave corruption shows as wild jumps/phantom
   clicks that persist after you stop moving).
5. Reset the machine (guest reset) — IKBD reset flushes the queue; no
   stale input after boot.
6. A game with a custom IKBD handler (e.g. anything Union/auto-booting
   that reprograms the MFP): keyboard should still work; if the game sets
   joystick-only modes, key injection continues, mouse injection stops.
7. Bluetooth: pair once with `bluetoothctl` (`scan on`, `pair`, `trust`,
   `connect`) — device shows up as a new `[KBD] using ...` line via hotplug.

Diagnostics: `kbd_usb_stat_injected_bytes`, `kbd_usb_stat_virtual_iacks`,
`kbd_usb_stat_dropped_bytes` counters, plus the existing `[MFPDUMP]`
`IACK46` counter which now includes virtual acknowledges.

## Known limitations

- Absolute-mouse-mode programs (rare; some CAD) only see the real mouse.
- Joysticks/gamepads are not mapped yet (the IKBD state machine already
  tracks joystick modes, so wiring evdev gamepads to `0xFF` joystick
  packets is a natural follow-up).
- If both the real IKBD and the injector stream heavily at the same time,
  a real byte can sit in the real ACIA for a few ms while an injected
  packet completes; a sustained collision could in principle overrun the
  real ACIA (one lost real mouse packet — self-correcting for relative
  packets, as TOS resyncs on the next header byte).
- The legacy (`PISTORM_LEGACY_MEM_HOOKS`) read/write paths got the same
  shims for parity, but only the active natmem I/O bank path is tested.
