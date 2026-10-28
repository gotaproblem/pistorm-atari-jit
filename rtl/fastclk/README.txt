PiStorm SXB EPM570 - fast clock firmware line (2.0b, fwrev $540)
================================================================

What is different from 1.x
  GPIO4 is PI_CLK: the Pi's GPCLK0 at 62.5 MHz into CPLD pin 12 (GCLK0).
  It times the READ release: TXN drops 32-40 ns after the read latches
  are clocked (S6), instead of at S7 (62.5 ns). LS374 clock->Q is 28 ns
  max, so the data is on the Pi's pins before TXN falls.
  Writes release at S6 (as 1.2e). Address latch OE from S1 (as 1.2e).

  GPIO4 is no longer ST RESET to the Pi - the Pi reads RESET_n from the
  status word.

Safety both ways
  - Old Pi binary + 2.0b: GPIO4 stays an input, no clock, every read is
    released at S7 - behaves as 1.2e.
  - New Pi binary (fastclk branch) + old firmware: the Pi only starts
    GPCLK0 when the CPLD answers major version 2+, so it never drives
    GPIO4 against a 1.x PI_RESET output.

Clock
  62.5 MHz = PLLD/12 on a Pi 4 (750 MHz), PLLD/8 on a Pi 3 (500 MHz).
  Integer dividers: no MASH fractional jitter. (64 MHz exactly would need
  a fractional divider.)

Tuning
  localparam RD_REL in pistormSXB_devEPM570.v:
    0 = 24-32 ns   1 = 32-40 ns (default)   2 = 40-48 ns  after S6

Build
  run makepi4.bat EPM570 from THIS folder (it builds the project in the
  current directory: pistormsxb_devEPM570.qpf/.qsf/.sdc here).

Verified
  Icarus simulation (fwsim): 0 bad reads/writes, no address/data timing
  violations, lock on/off, every PI_CLK phase, and with PI_CLK absent.
