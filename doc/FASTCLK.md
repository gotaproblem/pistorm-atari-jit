# Fast-clock line (branch `fastclk`, CPLD firmware 2.2b)

ST-RAM at real 68000 bus speed: GEMBench (TOS 2.06, STE) RAM/ROM access
100 %, ST-side memory test ~3.7 MB/s read, 3.2-3.5 MB/s write (was ~1.9).

| | `main` | `fastclk` |
|---|---|---|
| CPLD firmware | 1.1b (`$521` EPM570, `$421` EPM240) | 2.2b (`$542` EPM570, `$442` EPM240); 2.1b runs it without reset sensing |
| GPIO4 | ST RESET to the Pi | PI_CLK: GPCLK0 62.5 MHz into the CPLD |
| WR strobe | 4 sets + 2 clears | 2 sets + 1 WR clear (2.x firmware only) |
| CPU-thread bus lock | atomic + dmb sy | lock-free; other threads hand off (by membarrier when they run bus cycles) |
| Reset-button sensing | GPIO4 | status word bit 12 (firmware 2.2b+) |

## Switching

`main` -> `fastclk`:

    cd ~/pistorm-atari-jit
    git fetch origin
    git switch fastclk                     # the first time: tracks origin/fastclk
    git pull
    make                                   # Pi 3: make PIMODEL=PI3, Pi 5: make PIMODEL=PI5
    sudo rtl/pistormflash -s rtl/EPM570_bitstream_dev_2_2b.svf   # EPM570 board
    sudo rtl/pistormflash -s rtl/EPM240_bitstream_dev_2_2b.svf   # EPM240 board (one or the other)

then power-cycle the ST and start through the launcher:

    sh run-pistorm.sh

`run-pistorm.sh` relaunches the emulator after Restart, Ctrl+Alt+Undo, the
ST's reset button (2.2b), ST power loss or an unexpected exit; it shuts
Linux down on Ctrl+Alt+Shift+Undo, and stops at the console on the
taskbar's Shut down or Ctrl+C.

`fastclk` -> `main`:

    # quit the emulator normally first (it stops PI_CLK on exit);
    # after a crash or kill -9, reboot the Pi before flashing 1.x
    sudo rtl/pistormflash -s rtl/EPM570_bitstream_dev_1_1b.svf   # or EPM240_..._1_1b.svf
    git switch main && make

Either binary is safe with either firmware: the Pi only starts PI_CLK when
the CPLD answers major version 2 or later, and 2.x without PI_CLK behaves
as 1.2e (reads released at S7, no fast start).

## Run-time switches (2.x firmware)

| Variable | Effect |
|---|---|
| `PISTORM_FASTCLK=0` | no PI_CLK |
| `PISTORM_STROBE=3` / `4` | longer WR strobe |
| `PISTORM_CPULOCK=0` | CPU thread takes the old lock |
| `PISTORM_HANDOFF_NS=n` | IPL-sample hand-off wait (default 500); cycle hand-offs use membarrier |
| `PISTORM_IPL_HANDOFF=0` | seqlock IPL sampling - crashed Bad Apple, test only |

## Firmware source

`rtl/fastclk/` - Quartus projects for both CPLDs (run `makepi4.bat EPM570`
or `makepi4.bat EPM240` from that folder; the EPM240 build is the same logic
and pinout, ~137 of its 240 LEs). Bitstreams in `rtl/`:
`EPM570_bitstream_dev_2_2b.svf`, `EPM240_bitstream_dev_2_2b.svf`, and the
older `EPM570_bitstream_dev_2_1b.svf`.
2.1b = 1.2e + PI_CLK-timed read release (32-40 ns after S6) + fast start
(AS as soon as TXN has been high 40-48 ns, anywhere in S1/S2).

## Firmware versions

- 2.1b (`$541` / `$441`): fast clock + fast start. EPM570 bitstream only.
- 2.2b (`$542` / `$442`): 2.1b + reset button via status bit 12 (the CPLD's
  filtered "RESET held >= 2 us, not by the Pi"). `rtl/fastclk/*.v` is 2.2b;
  `*_2.1b.v` kept. Bitstreams for both CPLDs; the one to use.

## Open

- sndh44 crashes in its intro (player fine after Space); not yet checked
  against `main` + 1.1b.
- Why seqlock IPL sampling failed is not proven. It has the same
  store->load ordering hole the timed hand-off had (fixed for cycle
  hand-offs with membarrier, 3 Nov 2026 - see BUS OWNERSHIP in
  `gpio/ps_protocol.c`), which would explain it.
- Single-word reads still miss the next slot ~40 % (`ataritest --busprofile`
  read16); 2.2b (`FAST_DLY`/`RD_REL` 0) would add ~24 ns of margin.
