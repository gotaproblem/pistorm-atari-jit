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

## Real IKBD absent, or present-but-noisy

If the ST keyboard is unplugged, the 6850's receive pin has nothing driving
it. Noise gets framed as bytes, so the ACIA sits with framing/overrun errors
set and garbage in RDRF. Handed to TOS that becomes nonsense scancodes, an
overflowing keyboard buffer and **a constant bell** — and it starves the
injected stream too, because the merge path defers to real bytes.

So the real receiver is watched and classified:

- **merge** (real IKBD trusted) — the normal path described above.
- **quarantine** (real IKBD absent or noisy) — the real ACIA is *drained*
  on every status read (which also clears its IRQ output, stopping the
  real GPIP4 interrupt storm) and hidden from the guest: no RDRF, no error
  flags, no IRQ claim. Only injected bytes reach TOS. IKBD reset commands
  are answered with `0xF1` host-side, since nothing real will answer.

Detection is automatic and runs both ways, so unplugging or reconnecting
the ST keyboard is handled live without a restart. Classification happens
per *consumed* byte, not per status read — RDRF and the error flags persist
in the 6850 until RDR is read, so a polling guest sees the same byte many
times and counting status reads wildly overcounts.

- **Throughput** is the primary test. A real IKBD is silent unless touched
  and is hard-limited to ~780 B/s by the 7812.5 bps link; even continuous
  mouse movement sits near 300 B/s. Over 400 B/s for 2 consecutive seconds
  ⇒ absent. This is the rule that catches a floating line, because much of
  that noise *frames cleanly* — an earlier version tested only the error
  flags, counted clean noise as proof of a live keyboard, stayed in merge
  mode and kept feeding TOS garbage.
- 12 consecutively error-flagged bytes ⇒ absent.
- A guest IKBD reset (`0x80 0x01`) with no clean answer inside 400 ms ⇒
  absent. Catches a *silent* disconnected line, which yields nothing to count.
**Recovery is deliberately not rate-based.** Quarantine works *by* making
the stream quiet — RIE is cleared, so the flood stops — which means judging
"is the keyboard back?" from the observed rate is a feedback loop: quarantine
succeeds, the line looks calm, it un-quarantines, the flood and the bell come
straight back, and it oscillates once a second. The only way out of
quarantine is the reset probe, which is *positive* evidence rather than
absence of evidence: when the guest resets the IKBD, RIE is re-enabled for
the 400 ms window so a genuinely reconnected keyboard can be heard, and a
flood during that window cancels the probe outright.

In practice that means: plug the ST keyboard back in, then reset or reboot
the machine, and it returns to merge mode.

Mode changes are logged: `[KBD] real IKBD absent/noisy - quarantining real
ACIA, USB input only`.

## Diagnostics

Once per second, when anything is moving, the input thread prints:

```
[KBD] QUARANTINE(auto) real=812/s (passed=0 drained=812) inj=6/s status=$B1 errs=0 vIACK=143
```

- `real=N/s` — bytes coming out of the *real* ACIA. Anything sustained above
  a few hundred with no keyboard attached is the floating-line noise.
- `passed` — real bytes handed to TOS. **Must be 0 in quarantine**; if it
  is climbing while beeping, the real receiver is still reaching the guest.
- `drained` — garbage swallowed (and the ACIA IRQ cleared with it).
- `inj=N/s` — injected USB/Bluetooth bytes actually consumed by TOS.
- `status` — last real ACIA status byte; bit 0 RDRF, bit 4 framing,
  bit 5 overrun, bit 7 IRQ.
- `vIACK` — synthesised level-6 acknowledges.

If the bell persists while `passed=0` and `real=` is near zero, the keyboard
ACIA is not the source — the next suspect is the **MIDI ACIA** at
`$FFFC04/06`, which shares the same MFP GPIP4 line and is not shimmed.

Note that the beeping is caused by the disconnected keyboard, not by this
patch — a stock build with `kbd usb` commented out will beep the same way on
that machine. Quarantine mode incidentally suppresses it.

## Config

```
kbd usb              # enable, grab devices, auto-detect the real IKBD (default)
kbd usb nograb       # enable, leave devices shared with the Pi console
kbd usb standalone   # force quarantine: ignore the real IKBD entirely
kbd usb merge        # force merge: always trust the real IKBD (old behaviour)
```

`standalone` and `merge` are escape hatches — `auto` is the right default,
and the only reason to force `merge` is if auto-detect misjudges a working
keyboard as noisy (which would show up as that log line appearing while the
real keyboard still works).

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
