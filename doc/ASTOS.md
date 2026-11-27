# ASTOS — the Atari ST Operating System front end

ASTOS is what the machine boots into. It replaces the pre-boot setup page:
the desktop comes up on the ST's own monitor, every `psctrl.cfg` build is an
icon on it, and launching one starts that build (the JIT emulator, or
stboxmetal for an `engine metal` build) on the whole screen. When the build
ends you are back at the ASTOS desktop.

ASTOS runs on the Pi with the 68k halted, like the setup page did. It runs no
GEM or TOS programs itself: those only ever run inside a launched build.

Branch `astos` (off `hybrid-stbox`, so `engine metal` builds work). This is
**phase 0** of the design (`astos-design.md`, rev 5.2): boot into ASTOS,
launch full screen, come back. Phase 1 is the Pi status monitor, phase 2 the
file manager, then HDMI and windows.

## Building and starting

`make` builds `astos` with the emulator and stboxmetal (`make astos` alone).
`run-pistorm.sh` starts it whenever `./astos` exists, so with the
`pistorm.service` autostart the machine boots straight into it:

    sudo systemctl restart pistorm          # or power the Atari on

By hand (stop the service first):

    sudo systemctl stop pistorm
    cd ~/pistorm-atari-jit
    sudo sh run-pistorm.sh                  # ASTOS, then whatever it launches
    sudo ./astos --config ../configs/psctrl.cfg   # ASTOS alone, one pass

`PISTORM_ASTOS=0 sh run-pistorm.sh` brings the old setup page back.

## The screen

Medium resolution (640×200, 4 colours) on a colour monitor, high (640×400)
on a mono one, from the monitor sense line. Both use the same layout:

- **Desktop:** one icon per build (GEM machine, APJ-OS, Native ST, HDMI build,
  metal joystick), up to 16.
- **Details panel:** the selected build's engine, CPU, machine, ROM, disks
  and screen.
- **This machine:** ST-RAM found, monitor and frame rate, input devices,
  CPLD firmware as it is named (e.g. `1.1b (EPM240)`), Pi temperature.
- **Taskbar:** the *ASTOS* start button, the selected build, Pi temperature,
  date and clock (`[psctrl] clock 24|12`, `date dmy|mdy`).
- **Start menu:** *Environments…* (the setup page's builds screen: new,
  copy, delete, edit any build), *Restart ASTOS*, *Exit to console*,
  *Shut down…*. The builds themselves are on the desktop, not in the menu.
- **Right-click on an icon:** *Launch*, *Settings…* (that build's editor).

If no ST-RAM answers (no ST monitor wired), ASTOS draws on HDMI instead.
`hdmi on` in `[psctrl]` mirrors the desktop to HDMI as well.

## Keys and mouse

The ST's own keyboard and mouse, USB/Bluetooth keyboards and mice, and
gamepads all work.

| | |
|---|---|
| arrows | move between builds |
| click | select a build (on the desktop or in the start menu) - never launches |
| double-click, Enter, pad A or Start | launch the selected build |
| a letter | jump to the next build starting with it |
| Tab, Esc, F1, the start button, right-click off an icon | start menu |
| F2, or right-click → *Settings…* | edit the selected build's settings |
| Help (PgUp on a PC keyboard) | screenshot → `~/astos-shot-N.ppm` (640×400) |
| Help during the start-up splash | safe mode |

Inside a launched build, everything is as it was: Ctrl+Alt+Undo (Ctrl+Alt+End
on a PC keyboard) ends it, and with ASTOS that means **back to ASTOS**, not to
the setup page. So does the ST's reset button, APJ-OS's *Restart*, and a
build that fails to start. Ctrl+Alt+Shift+Undo still shuts the Pi down.

## Settings

*Settings…* opens the setup page's editor for that build inside ASTOS: the
same seven tabs (Machine, Video, Sound, Input, Drives, Network, Tuning), the
same rules (a key ticked when the build has the line, JIT and machine
dependencies, the image pickers for ROMs and disks), and the same *Save*.
Esc (or the right mouse button) comes back to the desktop - with unsaved
changes it asks once first - and *Boot now* launches the build. It still
looks like the page (its own palette, 80×25 text); an ASTOS-styled settings
window is a later phase. *Environments…* opens the page's builds screen
(N new, copy, D delete, E edit, S save) the same way. ASTOS re-reads
psctrl.cfg when you come back.

## psctrl.cfg

ASTOS reads the builds from `psctrl.cfg` and keeps its own keys in the
`[psctrl]` block, so the file still works with the setup page on other
branches. The page's `countdown` and `boot` are not used by ASTOS.

| key | |
|---|---|
| `astos_last` | the build launched last; ASTOS starts with it selected. Written by ASTOS |
| `astos_autolaunch` + `astos_countdown` | launch this build after N seconds, unless a key is pressed or the mouse clicked. No keys: no auto-launch |
| `hdmi` | `on`: mirror the ASTOS desktop to HDMI. Default off |
| `clock`, `date` | as for the page |

The emulator gained one switch for ASTOS: `--build <section>` boots that
build with no setup page (`run-pistorm.sh` passes it).

## Exit codes (run-pistorm.sh)

| ASTOS exits | run-pistorm.sh |
|---|---|
| 44 | runs `./emulator --no-setup --build <the build in /run/astos-launch>`, then ASTOS again (43 from the build: shut down) |
| 42 | ASTOS again (Restart, the ST's reset button, power lost - ASTOS waits for the power on the way up) |
| 43 | shuts Linux down |
| 0 | leaves you at the console |
| anything else | ASTOS again; after 2 in a row with `--safe`; after 5 stops |

**Safe mode** (`--safe`, Help at start-up, or two failures): no auto-launch,
no HDMI mirror. Everything else works.

## Testing without the board

    make astos-host
    ./astos-host --config configs/psctrl.cfg.default --shot out.ppm [--mono] [--keys KEYS]

draws the desktop into `out.ppm` (as the ST monitor shows it, 640×400) after
the keys: `r l u d` arrows, `e` Enter, `m` start menu, `x` Esc, `s` the shut
down dialog. Needs nothing installed beyond gcc.

## Source

| file | |
|---|---|
| `platforms/astos/astos.c` | start-up, the desktop, the event loop, launching |
| `platforms/astos/astos_gfx.c` | the 640×200 logical canvas, icons, packing onto the Shifter (medium planes / mono with rows doubled), the pointer, screenshots |
| `platforms/astos/astos_input.c` | ST keyboard + IKBD relative mouse over the ACIA; USB keyboards, mice, pads |
| `platforms/astos/astos_env.c` | builds from psctrl.cfg, details, the launch file |
| `platforms/astos/astos_sys.c` | Pi temperature, clock |
| `platforms/astos/astos_host_stub.c` | the no-board stubs for `astos-host` |

From the setup page it reuses `shifter_setup.c` (RAM probe, Shifter bring-up,
changed-bytes-only flush), `setup_cfg.c`, the key maps in `setup_input.c`,
`setup_hdmi.c` (with a palette setter added) and `native_probe.c`.
