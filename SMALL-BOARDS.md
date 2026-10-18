# Pi 3A+ / Zero 2 W (512 MB) - what it took, and what to expect

Verified on a Pi 3A+ on an H5 Phoenix board, 18 Oct 2026. One SD card and one
`emulator` binary (built on the Pi 4 with the default `PIMODEL=PI4`) run on the
Pi 4, 3B, 3A+ and Zero 2 W. Branch `small-boards`.

## Code

| Change | Why |
|---|---|
| `ps_peri_base()` reads the peripheral base from the device tree (`gpio/ps_protocol.c`) | `-DPI3` picked 0x3F000000 vs 0xFE000000 at compile time, so a Pi 4 build could never talk to the CPLD on a Pi 3. The ARMv8-A code for cortex-a72 already runs on the A53. |
| `mlockall(... \| MCL_ONFAULT)` (`emulator.c`) | `MCL_CURRENT` pre-faulted every mapping, defeating `MAP_NORESERVE` on the 272 MB natmem reserve, 16 MB fVDI FB and 112 MB PSVIDEL VRAM: ~300-410 MB locked whatever the cfg said. Now only touched pages are locked (120 MB resident at the APJ-OS desktop). `PISTORM_MLOCK=all` restores the old behaviour. |
| `ps_rd_settle` = 2 on BCM2837, 0 on BCM2711 (`gpio/ps_protocol.c`) | BCM2837 reads GPLEV fast enough to sample the data bus on the TXN edge: ~0.02 % of reads wrong (`ataritest --memory`). Sweep: 0 fails, 1-3 clean, same throughput. `PISTORM_RD_SETTLE=n` overrides. |
| Live-IPL check before a real level-6 IACK; MFP in-service vector delivered at once on a no-ack (`jit_glue.cpp`, `cpu/newcpu.cpp`) | The IACK cycle fails back to the Pi ~5e-5 halted / ~1e-3 live while the 68901 HAS serviced it (set ISR, dropped IRQ). The retry path then fired two more IACKs into an idle MFP: erratic mouse, keyboard beeps, Timer C orphaned in service. Mitigation, not root cause - the cycle itself is CPLD-vs-MFP IACK termination timing (firmware); the Pi 4 shows the same fault at a lower rate ("garbled-IACK wedge"). Reproducer: `ataritest --mfp iack=yes count=1000000`. |
| `ataritest --mfp [count=N] [irq=yes] [iack=yes]` | Read-stability test of MFP/ACIA registers with IPL held / with real IACK cycles. |
| `gpio/ps_protocol.o: -fno-schedule-insns2` (Makefile) | GCC 14 ICE with the cortex-a53 pipeline model only. |

## config.txt (per board, see `configs/config.txt`)

* **No `force_turbo` on BCM2837.** With it, the firmware refuses vc4-hdmi's
  pixel-clock request (fw-clk-pixel 2.4 GHz, pllh_pix idle 86.4 MHz) and the
  monitor says "invalid format" from the moment KMS binds. Pi 4 keeps it.
* Stock `core_freq` (400 on a 3A+). The old 500 was a 3B+ figure.
* `gpu_mem=64` + `dtoverlay=vc4-kms-v3d,cma-128` for `[pi3]`/`[pi02]`;
  `gpu_mem=128` + plain overlay for `[pi4]`/`[pi5]`. The overlay is loaded
  once per board section, never in `[all]`: loading it twice = 7 green flashes.
  Before: 150 MB for Linux. After: 287 MB.

## Memory budget at the APJ-OS desktop (3A+, 1080p HDMI, ttram 256M)

    kernel sees 287 MB; emulator RSS 120 MB (all mlocked); available 131 MB

Services worth stopping on this board: docker+containerd (48 MB), winbind
(~20 MB, only for AD domains). Keep smbd/nmbd (the Mac share). `cfg`:
`ttram 32M`, `jit_cache 8192`, `psvidel` off.

## Not for these boards

VIDPLAY / avrecord (need gpu_mem=128), psweb (WebKit), psdos, STE DMA sound
streaming at 1080p + STBOX together. A 1080p desktop and games work.

## Recovery notes

* Building `cpu/newcpu.cpp` or the JIT on 512 MB will OOM. Build on the Pi 4
  and copy `emulator` over.
* The card's ext4 journal was torn once (kernel panic under memory pressure).
  Repair from the Mac with Homebrew e2fsprogs on `/dev/diskN` (the buffered
  node - `rdiskN` returns EINVAL on e2fsck's unaligned superblock write).
  Image the card: `sudo dd if=/dev/rdiskN bs=4m | gzip -1 > card.img.gz`.
