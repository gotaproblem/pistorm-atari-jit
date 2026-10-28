# Fast-clock line (branch `fastclk`, CPLD firmware 2.1b)

ST-RAM at real 68000 bus speed: GEMBench (TOS 2.06, STE) RAM/ROM access
100 %, ST-side memory test ~3.7 MB/s read, 3.2-3.5 MB/s write (was ~1.9).

| | `main` | `fastclk` |
|---|---|---|
| CPLD firmware | 1.1b (`$521` EPM570, `$421` EPM240) | 2.1b (`$541` EPM570, `$441` EPM240) |
| GPIO4 | ST RESET to the Pi | PI_CLK: GPCLK0 62.5 MHz into the CPLD |
| WR strobe | 4 sets + 2 clears | 2 sets + 1 WR clear (2.x firmware only) |
| CPU-thread bus lock | atomic + dmb sy | lock-free; other threads hand off |
| Reset-button sensing | GPIO4 | status word bit 12 (firmware 2.2b+) |

## Switching

`main` -> `fastclk` (EPM570 board):

    git checkout fastclk && git pull
    make                                   # emulator + ataritest
    sudo rtl/pistormflash -s rtl/EPM570_bitstream_dev_2_1b.svf

`fastclk` -> `main`:

    # quit the emulator normally first (it stops PI_CLK on exit);
    # after a crash or kill -9, reboot the Pi before flashing 1.x
    sudo rtl/pistormflash -s rtl/EPM570_bitstream_dev_1_1b.svf
    git checkout main && make

Either binary is safe with either firmware: the Pi only starts PI_CLK when
the CPLD answers major version 2 or later, and 2.1b without PI_CLK behaves
as 1.2e (reads released at S7, no fast start).

## Run-time switches (2.x firmware)

| Variable | Effect |
|---|---|
| `PISTORM_FASTCLK=0` | no PI_CLK |
| `PISTORM_STROBE=3` / `4` | longer WR strobe |
| `PISTORM_CPULOCK=0` | CPU thread takes the old lock |
| `PISTORM_HANDOFF_NS=n` | hand-off wait (default 500) |
| `PISTORM_IPL_HANDOFF=0` | seqlock IPL sampling - crashed Bad Apple, test only |

## Firmware source

`rtl/fastclk/` - Quartus projects for both CPLDs (run `makepi4.bat EPM570`
or `makepi4.bat EPM240` from that folder; the EPM240 build is the same logic
and pinout, ~137 of its 240 LEs). Only the EPM570 bitstream has been built
and tested so far (`rtl/EPM570_bitstream_dev_2_1b.svf`).
2.1b = 1.2e + PI_CLK-timed read release (32-40 ns after S6) + fast start
(AS as soon as TXN has been high 40-48 ns, anywhere in S1/S2).

## Firmware versions

- 2.1b (`$541` / `$441`): fast clock + fast start. Built and tested (EPM570).
- 2.2b (`$542` / `$442`): 2.1b + reset button via status bit 12 (the CPLD's
  filtered "RESET held >= 2 us, not by the Pi"). `rtl/fastclk/*.v` is 2.2b;
  `*_2.1b.v` kept. Not yet built.

## Open

- sndh44 crashes in its intro (player fine after Space); not yet checked
  against `main` + 1.1b.
- Why seqlock IPL sampling failed is not understood (hand-off used instead).
- Single-word reads still miss the next slot ~40 % (`ataritest --busprofile`
  read16); 2.2b (`FAST_DLY`/`RD_REL` 0) would add ~24 ns of margin.
