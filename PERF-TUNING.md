# PERF-TUNING - the cheap wins, in order

Companion to the bare-metal feasibility report. That report's conclusion was
that most of what Emu68 gets from having no operating system is reachable
from Linux; this file is the list, cheapest first, with what each one is
expected to do to the 68000-mode CoreMark baseline of **734** (Pi 4, 1.5 GHz,
PISSOFF 1024). Measure after each step; the projections are +-15%.

| Step | What | Expected | Measured (7 Sep 2026, jit-perf-hbl-fixes) | Cost |
|---|---|---|---|---|
| 1 | 2 MB pages for natmem, JIT cache, fVDI FB (in code, on by default) | +3-6% | **0 - see below** | none |
| 2 | `jit_cache 16384` (apj-os.cfg) | 0-3% on GEM/MiNT, 0 on CoreMark | not separately measured | 8 MB RAM |
| 3 | `PISTORM_PISSOFF=2048` / `4096` | +1-2% | 783 -> 803 (2048) -> 817 (4096) | wider fallback IRQ window |
| 4 | `arm_freq=2100` + `over_voltage=6` | +35-40% | not yet run | heat; needs a heatsink/fan |
| 5 | hugetlbfs instead of THP (`PISTORM_HUGETLB=1`) | same as 1 | **moot, see below** | 160 MB pinned at boot |

**Steps 1 and 5 do nothing on stock Raspberry Pi OS.** The 6.18 `rpt-rpi-v8`
kernel is built with `CONFIG_TRANSPARENT_HUGEPAGE` and `CONFIG_HUGETLBFS` both
off, so the `[HUGEPAGE]` report shows 0% on 2 MB pages and nothing in user
space can change that. It would not matter anyway: `perf-tlb.sh` under CoreMark
(68040 and 68060 mode) shows ~2.5-3 M L1 TLB refills per 30 G instructions -
one per ~10,000 instructions, under 1% of cycles even at 50 cycles per walk -
with IPC 2.0. The core is not waiting on page walks. The code stays in because
it costs nothing and reports what it finds; a THP-enabled kernel would need a
rebuild (`build-thp-kernel.sh` in the investigation folder) for a gain inside
the noise. What the counters did show: `l1i_cache_refill` at 44-49 M per 10 s,
one per 600-750 instructions, ~4-5% of cycles - the translated code and the
dispatch loop overflow the 48 KB L1I. That is a JIT code-size/layout item.

Step 3 at 4096 is +4.3% over the 783 baseline and was not yet the plateau;
keep it if MiNT/XaAES input and YM/DMA-sound playback stay clean.

---

## 1. Huge pages (in code - inert on the stock kernel)

Check first: `ls /sys/kernel/mm/transparent_hugepage` - if the directory does
not exist the kernel has no THP support and everything below is a no-op.

`pistorm_hugepage.c` backs the three big mappings with 2 MB pages:

| Region | Size | Why it matters |
|---|---|---|
| natmem (ST space + TT-RAM) | 144 MB | every guest load/store the JIT emits lands here |
| JIT translation cache (+popall stubs) | 8-16 MB | every instruction the ARM core fetches while emulating |
| fVDI framebuffer | 16 MB | host-side blits and the DRM upload read it end to end |

The A72 has a 32-entry L1 DTLB, a 48-entry L1 ITLB and a 1024-entry L2 TLB. With
4 KB pages the L2 TLB covers 4 MB; with 2 MB pages it covers 2 GB, i.e. the
whole emulator. That is what Emu68's 1 GB block MMU map gives it for free.

Nothing to configure. On a kernel with THP you get at start-up:

```
[HUGEPAGE] natmem       147456 KB at 0x7f8a000000 - THP madvise(MADV_HUGEPAGE), 2M aligned
[HUGEPAGE] fvdi-fb       16384 KB at 0x7f89000000 - THP madvise(MADV_HUGEPAGE), 2M aligned
[HUGEPAGE] jit-cache     16640 KB at 0x7f88000000 - THP madvise(MADV_HUGEPAGE), 2M aligned
```

and, once the CPU thread has been released by main() (the translation cache
exists and `mlockall()` has faulted everything in):

```
[HUGEPAGE] natmem       147456 KB mapped,   147456 KB resident,   147456 KB on 2M pages (100% of resident)
```

If the last column is near 0% and the start-up lines said "THP unavailable
(madvise failed)", the kernel has no THP (stock Pi OS). If they said THP was
requested, the policy is `never`; the emulator flips it to `madvise` itself
when running as root, but check:

```
cat /sys/kernel/mm/transparent_hugepage/enabled    # want [madvise] or [always]
cat /sys/kernel/mm/transparent_hugepage/defrag     # anything but [never]
```

Implementation note: the regions are mapped `PROT_NONE`, 2 MB-trimmed,
`madvise(MADV_HUGEPAGE)`d and only then `mprotect()`ed to their real
permissions. `cpu_task` runs `mlockall(MCL_FUTURE)`, and the JIT cache is
allocated from the main thread while that may already be in force; a readable
mapping would be populated at `mmap()` time, before the hint is on the VMA,
and land on 4 KB pages. `PROT_NONE` cannot be populated, so the first touch
after `mprotect()` gets 2 MB pages. Verified 100% under `mlockall`.

Switches:

* `PISTORM_HUGEPAGE=0` - plain 4 KB mmap, for A/B measurements.
* `PISTORM_HUGETLB=1`  - use hugetlbfs pages instead of THP. Guaranteed 2 MB,
  never split by memory pressure, no khugepaged background work on the
  isolated cores. Needs a pool reserved at boot: append to `cmdline.txt`
  `hugepagesz=2M hugepages=96` (192 MB: 72 for natmem, 9 for the cache, 8
  for fVDI, the rest headroom). Falls back to THP if the pool is missing.

Not covered: the ST-RAM `memfd` backing used on sub-4 MB boards (bank
aliasing). That is a shared mapping; shmem THP is a separate knob
(`shmem_enabled`) and the region is 4 MB at most, so it was not worth the
complication. The ET4000 VRAM/IO `PROT_NONE` windows and the JIT's 4 KB
vector-page trap split the huge page they land in and nothing else.

## 2. JIT cache size

`jit_cache 16384` is now set in `apj-os.cfg`. 16384 KB is `MAX_JIT_CACHE`
(include/newcpu.h). Use PSMON's "hard flushes/s" to see whether it helps your
session; CoreMark itself fits in 8 MB and will not move.

## 3. Compiled-chain budget

`PISTORM_PISSOFF=2048` in the emulator's environment (add
`Environment=PISTORM_PISSOFF=2048` to the `[Service]` block of
`/etc/systemd/system/pistorm.service`). The default of 1024 keeps 98% of the
gain with twice the interrupt-fallback margin; 2048 is the measured plateau.
If the keyboard starts beeping under MiNT or the mouse gets erratic, a chain
break was late: go back to 1024.

## 4. Clock

`configs/config.txt` now carries the overclock as two commented lines under
`[pi4]`:

```
#over_voltage=6
#arm_freq=2100
```

Uncomment both. `force_turbo=1` is already set so the clock is pinned, and
`temp_limit=80` throttles before damage. Use `vcgencmd measure_clock arm` and
`vcgencmd get_throttled` (0x0 = clean) after a long session; a `0x80000`
means it throttled at some point and you need better cooling or a lower
frequency. 2000 is the conservative choice; 2200 works on many Pi 4B boards
with a fan. CM4 modules are generally happy at 1800-2000.

PSCTRL reports the *requested* clock, so look at `vcgencmd` for the truth.

## 5. Measuring

`sudo ./perf-tlb.sh 10` prints the THP state, the emulator's AnonHugePages
total, and ten seconds of PMU counters on CPU 2 (cycles, instructions,
L1 TLB refills, cache refills; the Cortex-A72 PMU does not expose an L2 TLB
refill event, so that line is skipped). Run a CoreMark or a GEM stress loop on the
Atari while it samples. The numbers to compare with `PISTORM_HUGEPAGE=0`:

* `l1d_tlb_refill` + `l1i_tlb_refill` x ~50 cycles / `cycles` - the upper
  bound on what any page-size change can recover. Measured: under 1%.
* `instructions / cycles` (IPC) - 2.0 measured; the core is not stalling on
  memory. The next lever is the JIT's code quality (instructions per emulated
  instruction, L1I footprint), not the memory map.

For the CoreMark number itself, run the same binary you used for 734, in
68040 + TT-RAM mode as well as 68000 mode, and record both: the 68040 figure
is the one comparable to Emu68's 68020 build.

## What is deliberately not here

* Big-endian host execution: Linux/arm64 on the Pi is little-endian only.
  This is the one Emu68 trick that needs bare metal (or a core partition).
* CPLD/GPIO transaction cost: unchanged by anything on this page; that is
  firmware and bus work.
* JIT register allocation / flag handling: months of work, +15-30%,
  the next project after these are banked.
