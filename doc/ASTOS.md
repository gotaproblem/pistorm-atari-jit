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
  CPLD firmware as it is named (e.g. `1.1b (EPM240)`). The Pi's temperature
  is in the taskbar.
- **Taskbar:** the *ASTOS* start button, the selected build, Pi temperature,
  date and clock. Click the temperature to switch Celsius / Fahrenheit, the
  date to switch day/month/year and month/day/year, the clock to switch
  24 / 12 hour; saved as `[psctrl] temp c|f`, `date dmy|mdy` and
  `clock 24|12` (the last two are the keys the setup page used).
- **Throttle state**, left of the temperature: the firmware's
  `get_throttled` word (what `vcgencmd get_throttled` prints), read once a
  second through the VideoCore mailbox. `OK` when nothing has happened
  since boot; otherwise a code per condition - `UV` under-voltage, `CAP`
  ARM frequency capped, `THR` throttled, `TMP` soft temperature limit - in
  a filled box while it is happening now, in plain text once it has passed.
  A click shows the raw value and what it means. On HDMI, `OK` is green
  and `UV` / `TMP` are red; the ST monitor keeps its four pens (medium has
  no red or green to spare), so there the codes are boxes and plain text. Every change is also
  printed to the console / journal (`[ASTOS] get_throttled 0x50005 -
  under-voltage now ...`). ASTOS only reads it: the since-boot bits are
  never cleared. `PISTORM_ASTOS_THROTTLED=0x50005` fakes a value, to see
  how it looks.
- **Start menu:** *New environment…*, *HDMI display* (on / off / only),
  *Restart ASTOS*, *Exit to console*, *Shut down…*. The environments themselves are on the desktop.
- **Right-click on an icon:** *Launch*, *Settings…*, *Duplicate…*,
  *Delete…*.

**HDMI.** ASTOS shows the same desktop on HDMI, scaled, when any of these
holds:
- `hdmi_only` in `[psctrl]`, or the environment launched last has
  `hdmi_only` - whoever watches only HDMI finds ASTOS there when the build
  ends;
- the start menu's *HDMI display* is on (`[psctrl] hdmi on`; switches live);
- no ST memory answers at all.
Safe mode drops the plain mirror but never an HDMI-only setup. Only the
rows that changed are redrawn on the panel, so the pointer stays light.

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
| F2, or right-click → *Settings…* | the selected environment's settings |
| Help (PgUp on a PC keyboard) | screenshot → `~/astos-shot-N.ppm` (640×400) |
| Help during the start-up splash | safe mode |

Inside a launched build, everything is as it was: Ctrl+Alt+Undo (Ctrl+Alt+End
on a PC keyboard) ends it, and with ASTOS that means **back to ASTOS**, not to
the setup page. So does the ST's reset button, APJ-OS's *Restart*, and a
build that fails to start. Ctrl+Alt+Shift+Undo still shuts the Pi down.

## Settings

*Settings…* (F2, or right-click an icon) opens the environment's settings
window, drawn by ASTOS:

- **Category** is a drop-down list: Machine, Video, Sound, Input, Drives,
  Network, Tuning (Tab opens it, Left/Right steps through it).
- Every setting's **value is a drop-down list**: a list key shows its
  choices, a switch *On* / *Off*, a ROM or disk the files in `rom_path` /
  `disk_path` / `fdd_path` (plus *type a name…*), and anything set can be
  removed from the environment from the same list. A typed value (a number,
  a path) is an edit field in place: Enter accepts, Esc cancels.
- Rows that cannot apply to this environment are greyed with the reason
  (`needs 68020+`, `Falcon only`, `needs ide`); the cursor skips them.
- **Save** writes psctrl.cfg; **Close** (Esc, or the right button) goes back
  to the desktop - with unsaved changes the first Esc only warns.
- Keys: Up/Down a setting, Enter its list, Space on/off (or remove), F10
  save.

The rules behind it are the setup page's own (its editor used as a model:
`spm_*` in `setup_page.c`), so the file ends up exactly as the page would
have written it - the JIT / cpu / machine dependencies, pinned disk slots,
the defaults filled in on save.

**engine metal** fixes two settings: choosing it (or opening an older metal
environment) sets `cpu 68000` and `jit_power 0` - stboxmetal is an 8 MHz
68000, interpreted - and drops what needed more (fpu, mmu, ttram,
jit_cache). The cpu and jit_power rows then show *engine metal* and cannot
be changed until the engine is changed back.

**Not an STE (or Falcon):** DMA sound and the LMC1992 are STE hardware, so
on any other machine choosing the machine - or opening such an environment
- writes `dma_sound disabled` and `lmc 0` (lmc is on when its line is
absent, so it is written out). Both rows stay greyed *STE/Falcon only*.

*New environment…* asks for a name and opens the new, empty environment's
settings; *Duplicate…* copies one under a new name; *Delete…* asks first.
Names follow the page's rule: lower case letters, digits, `-` and `_`.

## psctrl.cfg

ASTOS reads the builds from `psctrl.cfg` and keeps its own keys in the
`[psctrl]` block, so the file still works with the setup page on other
branches. The page's `countdown` and `boot` are not used by ASTOS.

| key | |
|---|---|
| `astos_last` | the build launched last; ASTOS starts with it selected. Written by ASTOS |
| `astos_autolaunch` + `astos_countdown` | launch this build after N seconds, unless a key is pressed or the mouse clicked. No keys: no auto-launch |
| `hdmi` | `on`: show the ASTOS desktop on HDMI as well. Default off; the start menu's *HDMI display* sets it |
| `hdmi_only` | ASTOS on HDMI, whatever `hdmi` says (also when the last environment has it) |
| `clock`, `date` | as for the page; clicking the taskbar's clock / date switches them |
| `temp` | `c` (default) or `f`: the taskbar temperature; a click on it switches |

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
| `platforms/astos/astos_settings.c` | the settings window: category and value drop-down lists over the page's editor model |
| `platforms/astos/astos_sys.c` | Pi temperature, clock |
| `platforms/astos/astos_host_stub.c` | the no-board stubs for `astos-host` |

From the setup page it reuses `shifter_setup.c` (RAM probe, Shifter bring-up,
changed-bytes-only flush), `setup_cfg.c`, the key maps in `setup_input.c`,
`setup_hdmi.c` (with a palette setter added) and `native_probe.c`.
