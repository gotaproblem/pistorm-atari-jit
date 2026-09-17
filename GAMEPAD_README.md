# USB/Bluetooth game controllers → Atari joysticks

`usb gamepad` in the cfg. Xbox pads (and any other evdev gamepad) plugged
into or Bluetooth-paired with the Pi 4 become the ST's joysticks, on the
main machine (TOS/GEM, APJ-OS) and inside STBOX, on an ST or an STE host.
Joystick only: the pad never drives the GEM pointer.

## Mapping

| Pad                  | ST joystick (IKBD)         | STE joypad ($FF9200/2) |
|----------------------|----------------------------|------------------------|
| pad 0 (first found)  | joystick 1, game port, $FF | joypad A               |
| pad 1                | joystick 0, mouse port, $FE| joypad B               |
| D-pad / left stick   | up/down/left/right         | directions             |
| A (or BTN_TRIGGER)   | fire                       | A                      |
| B                    | –                          | B                      |
| X                    | –                          | C                      |
| Y                    | –                          | OPTION                 |
| Start                | Space bar                  | PAUSE                  |

The left stick engages at 50 % of its travel and releases at 35 %
(hysteresis), so a stick resting near the threshold cannot chatter packets
down the 7812.5 bps IKBD link. Axis ranges come from `EVIOCGABS`: wired
`xpad` reports ±32767, the Bluetooth HID path 0..65535, and both work.

## How it rides on `kbd usb`

`platforms/atari/joy_usb.c` owns the pad state and mapping; the evdev
reading, hotplug, the IKBD command state machine and the ACIA injection ring
are `kbd_usb.c`'s, exactly as for USB keyboards and mice. Either cfg line
alone brings the injection layer up (`kbd usb` opens keyboards and mice,
`usb gamepad` opens gamepads); the real-IKBD detection and the
`merge`/`standalone` modifiers apply to both.

Joystick packets follow the IKBD's rules, checked against Hatari's
`ikbd.c`:

- After a reset both mouse and joystick reporting are on.
- `$14` (joystick event reporting) turns the mouse **off** – except inside
  the ~63 ms reset window after a `$08` or `$12`, when both stay on
  (Barbarian, Hammerfist); `$12` + `$1A` inside the window also leaves both
  on. `$14` forgets the previous joystick state, so held directions are
  re-sent.
- While the mouse is on, joystick 1's fire button is the **right mouse
  button** (it goes in the `$F8` header) and joystick 0 – the mouse port –
  is not reported at all.
- `$16` (interrogate): with the real IKBD present its `$FD j0 j1` reply is
  parsed and the pads OR'd in; with it quarantined the reply is synthesised.
- `$17` joystick monitoring and `$19` keycode joystick are not implemented
  (Hatari doesn't either).

Only changes go on the wire, so an idle pad costs nothing.

## STE enhanced joypad ports

On an STE/Mega STE host the `$FF9200`/`$FF9202` chips are real; the emulator
reads them and clears the bits the pads hold (active low), so a real pad on
the real socket still works. Column select is the low byte written to
`$FF9202` (Jaguar-pad layout: bit 0/4 stick + A + PAUSE, 1/5 B, 2/6 C, 3/7
OPTION; keypad rows read as nothing). The Mega STE DIP switches in the high
byte of `$FF9200` pass through untouched. On a plain ST the read bus-errors
before anything is merged, so the guest sees exactly what the hardware does
and no machine-type switch is needed.

## STBOX

While the sandbox window is focused the pads go to the box's IKBD model
(`stbox_joypad_event`), which now follows the same rules as above and
answers `$16` with the pad state; the STE tier's `$FF9200`/`$FF9202` are
served from the same state. Fixed on the way: the box had joystick events
OFF at power-on and after reset (a real IKBD has them on), never silenced
the mouse on `$14`, and answered `$16` with zeros.

## Pi side

Wired: the stock Pi kernel has `xpad` – just plug in (360, One, Series X|S).

Bluetooth, the short way – `sudo tools/pair-gamepad.sh` scans, pairs
(retrying while you hold the pair button), trusts, connects and checks the
kernel made an input device. `--fresh` forgets a stale bond first; a MAC or
a name substring picks the pad. Pairing is a one-off: a trusted pad
reconnects by itself when you press its power button, and the emulator
picks it up by hotplug.

Bluetooth by hand (One S model 1708, Series X|S model 1914; update the
pad's firmware first with the Xbox Accessories app):

    bluetoothctl
      power on
      agent on
      default-agent
      scan on            # hold the pair button until the X flashes fast
      pair    XX:XX:XX:XX:XX:XX
      trust   XX:XX:XX:XX:XX:XX
      connect XX:XX:XX:XX:XX:XX

If it connects and then drops (X keeps flashing) and `journalctl -u
bluetooth` shows `hog-lib.c ... unlikely error`, the pad is refusing GATT
reads on the new bond. Put in `/etc/bluetooth/main.conf` under
`[General]`:

    ControllerMode = dual
    JustWorksRepairing = always
    Privacy = device

restart bluetooth, `remove` the pad and pair again; if it still loops,
update the pad firmware (5.09 is known to do this; 5.1x fixed it). The
`[ConnectionParameters]` info-file hack seen online is for a different
(timeout) loop – a malformed block makes BlueZ drop the device entirely.

Not recommended: xpadneo (DKMS against the 64-bit kernel from the armhf
userland) and the Xbox Wireless Adapter dongle (needs out-of-tree `xone`).

Check the pad before blaming the emulator: `sudo apt install evtest`,
`sudo evtest`, pick the pad, press everything. The emulator logs
`[JOY] pad 0: Xbox Wireless Controller -> ST joystick 1 (game port), STE pad A`
when it takes one; pads are grabbed with the keyboards (F12 releases).
PSCTRL's host-input word gets bit 4 = a gamepad is attached.
