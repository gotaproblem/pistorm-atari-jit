# Fast-clock line (branch `fastclk`, CPLD firmware 2.1b)

ST-RAM at real 68000 bus speed: GEMBench (TOS 2.06, STE) RAM/ROM access
100 %, ST-side memory test ~3.7 MB/s read, 3.2-3.5 MB/s write (was ~1.9).

| | `main` | `fastclk` |
|---|---|---|
| CPLD firmware | 1.1b (`$521`) | 2.1b (`$541`) |
| GPIO4 | ST RESET to the Pi | PI_CLK: GPCLK0 62.5 MHz into the CPLD |
| WR strobe | 4 sets + 2 clears | 2 sets + 1 WR clear (2.x firmware only) |
| CPU-thread bus lock | atomic + dmb sy | lock-free; other threads hand off |
| Reset-button sensing | yes | no (GPIO4 is the clock) |

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

`rtl/fastclk/` - Quartus project (run `makepi4.bat EPM570` from that folder):
2.1b = 1.2e + PI_CLK-timed read release (32-40 ns after S6) + fast start
(AS as soon as TXN has been high 40-48 ns, anywhere in S1/S2).

## Open

- sndh44 crashes in its intro (player fine after Space); not yet checked
  against `main` + 1.1b.
- Why seqlock IPL sampling failed is not understood (hand-off used instead).
- Single-word reads still miss the next slot ~40 % (`ataritest --busprofile`
  read16); 2.2b (`FAST_DLY`/`RD_REL` 0) would add ~24 ns of margin.
