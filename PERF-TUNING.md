# PERF-TUNING - the cheap wins, in order

Companion to the bare-metal feasibility report. That report's conclusion was
that most of what Emu68 gets from having no operating system is reachable
from Linux; this file is the list, cheapest first, with what each one is
expected to do to the 68000-mode CoreMark baseline of **734** (Pi 4, 1.5 GHz,
PISSOFF 1024). Measure after each step; the projections are +-15%.

| Step | What | Expected | Cost |
|---|---|---|---|
| 1 | 2 MB pages for natmem, JIT cache, fVDI FB (this commit, on by default) | +3-6% | none |
| 2 | `jit_cache 16384` (apj-os.cfg, this commit) | 0-3% on GEM/MiNT, 0 on CoreMark | 8 MB RAM |
| 3 | `PISTORM_PISSOFF=2048` | +1-2% | slightly wider fallback IRQ window |
| 4 | `arm_freq=2100` + `over_voltage=6` | +35-40% | heat; needs a heatsink/fan |
| 5 | hugetlbfs instead of THP (`PISTORM_HUGETLB=1`) | same as 1, but deterministic | 160 MB pinned at boot |

Steps 1-3 together: ~790. Step 4 on top: ~1,100. That is the "S1 Linux
tuned" row of the report.

---

## 1. Huge pages (done in code)

`pistorm_hugepage.c` backs the three big mappings with 2 MB pages:

| Region | Size | Why it matters |
|---|---|---|
| natmem (ST space + TT-RAM) | 144 MB | every guest load/store the JIT emits lands here |
| JIT translation cache (+popall stubs) | 8-16 MB | every instruction the ARM core fetches while emulating |
| fVDI framebuffer | 16 MB | host-side blits and the DRM upload read it end to end |

The A72 has a 32-entry L1 DTLB, a 48-entry L1 ITLB and a 1024-entry L2 TLB. With
4 KB pages the L2 TLB covers 4 MB; with 2 MB pages it covers 2 GB, i.e. the
whole emulator. That is what Emu68's 1 GB block MMU map gives it for free.

Nothing to configure. On start-up you get:

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

If the last column is near 0%, the kernel's THP policy is `never`; the
emulator flips it to `madvise` itself when running as root, but check:

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
L1/L2 TLB refills, cache refills). Run a CoreMark or a GEM stress loop on the
Atari while it samples. The numbers to compare with `PISTORM_HUGEPAGE=0`:

* `l2d_tlb_refill` - should drop by roughly 10x with huge pages.
* `instructions / cycles` (IPC) - if this does not improve while TLB refills
  do, the workload was not TLB-bound and the next lever is the JIT's code
  quality, not the memory map.

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
