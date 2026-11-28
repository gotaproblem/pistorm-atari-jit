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
- **Details panel:** the selected environment -
  *Machine* (Atari ST / STE / Mega ST / Falcon, and the TOS in its ROM file:
  `TOS 2.06`, `EmuTOS`, or the file name if it cannot be read),
  *Memory* (ST-RAM from `stram_size` / `falcon_stram`, 4MB when unset, plus
  TT-RAM; a metal build uses the board's), *CPU* (a metal build says
  *68000 8 MHz, real chips*), *Blitter* (real chip / emulated / none, or the
  machine's default), *IDE* and *ACSI* (off, or on with the first image and
  how many more), *Screen*.
- **This machine:** the real hardware, asked over the bus when ASTOS
  starts (68k halted, nothing emulated): the machine (ST / STE / Mega ST /
  Mega STE) and its TOS, ST-RAM per bank, the blitter (real chip or none),
  the monitor and frame rate, IDE (the IDE status register answers or not),
  ACSI (a TEST UNIT READY to IDs 0-7 through the DMA chip; the IDs that
  answer are listed). The machine, banks, blitter and TOS come from the same
  probe that writes the `[native]` build.
- **Taskbar:** the *ASTOS* start button, the PiStorm's CPLD firmware
  (`FW 1.1b`), Pi temperature,
  date and clock. Click the temperature to switch Celsius / Fahrenheit, the
  date to switch day/month/year and month/day/year, the clock to switch
  24 / 12 hour; saved as `[psctrl] temp c|f`, `date dmy|mdy` and
  `clock 24|12` (the last two are the keys the setup page used).
- **Pi state**, left of the temperature, from the firmware's
  `get_throttled` word (what `vcgencmd get_throttled` prints), read once a
  second through the VideoCore mailbox: `PI OK` when there has been no
  under-voltage and no temperature limit since boot; otherwise `UV`
  (under-voltage) and/or `TEMP` (soft temperature limit) - in a filled box
  while it is happening, in plain text once it has passed. On HDMI `PI OK`
  is green and `UV` / `TEMP` red; the ST monitor keeps its four pens. A
  click shows the raw value and every condition, frequency capping and
  throttling included. Every change is printed to the console / journal.
  ASTOS only reads it: the since-boot bits are never cleared.
  `PISTORM_ASTOS_THROTTLED=0x50005` fakes a value, to see how it looks.
- **Start menu:** *New environment…*, *Files…*, *Pi status…*, *HDMI display* (on / off / only),
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
| F3, or *Files…* | the file manager |
| Help (PgUp on a PC keyboard) | screenshot → `~/astos-shot-N.ppm` (640×400) |
| Help during the start-up splash | safe mode |

Inside a launched build, everything is as it was: Ctrl+Alt+Undo (Ctrl+Alt+End
on a PC keyboard) ends it, and with ASTOS that means **back to ASTOS**, not to
the setup page. So does the ST's reset button, APJ-OS's *Restart*, and a
build that fails to start. Ctrl+Alt+Shift+Undo still shuts the Pi down.

## Pi status and events

*Pi status…* (start menu, or a click on `PI OK` / `UV` / `TEMP` in the
taskbar) opens a window of live figures, refreshed once a second:

| | |
|---|---|
| CPU | load per core: 0-1 Linux, 2 the JIT, 3 the bus |
| Clocks | ARM now (and its maximum), core (VPU), V3D - measured, through the VideoCore mailbox |
| Volts | core, SDRAM controller, I/O and PHY (what `vcgencmd measure_volts` reads) |
| Temp | the SoC, against the 80 C soft and 85 C hard limits |
| Power | under-voltage, ARM frequency capped, throttled, soft temperature limit: *NOW*, *since boot, not now* or *never since boot* |
| Memory | Pi RAM used of total, GPU memory |
| SD card | free space, SD errors seen this session |
| Pi / Kernel | model, kernel, uptime |
| Network | IP address and interface, Wi-Fi link quality |

The Pi 4 cannot measure its 5 V input: under-voltage is the firmware's flag.

**Events** (E, Tab or the *Events…* button) lists what has happened, newest
first, kept across reboots in `astos-events.log` beside psctrl.cfg (trimmed to
the last 200 lines past 1000):
- each throttle condition starting and clearing while ASTOS runs; one that
  happened while a build ran (its since-boot bit is new) is logged when
  ASTOS comes back;
- kernel log lines that matter, read from `/dev/kmsg` - SD card errors and
  timeouts, filesystem errors and read-only remounts, the out-of-memory
  killer (naming the program), the kernel's own under-voltage message.
  ASTOS remembers the last record it read (in /run), so what happened
  during a build is caught when it comes back;
- how each environment ended (run-pistorm.sh records the exit code: normal,
  failed to start, crashed with a signal), launches, the ST's reset button,
  ST power lost.

A warning or error also shows as a toast above the taskbar for 10 seconds.
Esc or the right button closes the window.

## Files

*Files…* (start menu, or F3) is a two-pane file manager for the Pi's side
of things. Each pane shows one **place**, picked from its drop-down list
(click the bar, or F1):

- every `hostfs` share of every environment, by drive letter (`Q: gem-share`);
- *Disk images*, *Floppy images*, *ROMs*: `disk_path`, `fdd_path`, `rom_path`;
- USB sticks and drives the Pi has mounted under `/media` (`USB: …`);
- folders listed in `[psctrl] places` (comma separated);
- *Pi home*: the pistorm user's home.

A pane never goes above its place. Folders come first, in natural order
(FILE2 before FILE10); names starting with `.` are not listed. The pane
with the blue bar is the active one; Copy and Move go from it to the other.

| | |
|---|---|
| arrows Up/Down, Home, a letter | move |
| Enter, double-click | open a folder (`..` goes up) |
| Backspace | up a folder |
| Tab, Left/Right, a click in a pane | the other pane |
| Space | mark / unmark, then down (operations take the marked items, else the one under the cursor) |
| F5 *Copy*, F6 *Move* | to the other pane, after a confirm. Nothing is ever overwritten |
| F2 *Rename*, F7 *Folder* | type the name, Enter |
| F8 / Delete *Delete* | moves into `.trash` at the top of the place (a time stamp is added if the name is there already) |
| Esc, right-click, *Close* | back to the desktop |

ASTOS runs as root: what it creates is handed to the owner of the folder it
lands in, so a share stays writable over Samba.

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
| `places` | extra folders for *Files…*, comma separated |

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
down dialog, `f` Files, `t` Tab, space, `b` Backspace, `P` place list,
`C` Copy, `N` New folder, `R` Rename, `D` Delete, `'text'` typed. Needs
nothing installed beyond gcc.

## Source

| file | |
|---|---|
| `platforms/astos/astos.c` | start-up, the desktop, the event loop, launching |
| `platforms/astos/astos_gfx.c` | the 640×200 logical canvas, icons, packing onto the Shifter (medium planes / mono with rows doubled), the pointer, screenshots |
| `platforms/astos/astos_input.c` | ST keyboard + IKBD relative mouse over the ACIA; USB keyboards, mice, pads |
| `platforms/astos/astos_env.c` | builds from psctrl.cfg, details, the launch file |
| `platforms/astos/astos_settings.c` | the settings window: category and value drop-down lists over the page's editor model |
| `platforms/astos/astos_files.c` | the file manager: places, two panes, copy / move / rename / new folder / delete to `.trash` |
| `platforms/astos/astos_sys.c` | Pi temperature, clock |
| `platforms/astos/astos_host_stub.c` | the no-board stubs for `astos-host` |

From the setup page it reuses `shifter_setup.c` (RAM probe, Shifter bring-up,
changed-bytes-only flush), `setup_cfg.c`, the key maps in `setup_input.c`,
`setup_hdmi.c` (with a palette setter added) and `native_probe.c`.
