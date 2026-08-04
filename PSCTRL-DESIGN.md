# PSCTRL — a 68000 front end for JIT status, JIT options and floppy swapping

Feasibility investigation against the tree at
`/Volumes/Atari-ST-PiSTorm-JIT/pistorm-atari-jit` (HEAD `482ef1b`).

**Verdict: all three are doable. Two are easy, one has a real trap in it.**

| Feature | Verdict | Where the work is |
|---|---|---|
| Show JIT / CPU / config status | Easy — half a day | Two small accessors in the JIT, one NatFeat handler |
| Change *some* JIT options live | Easy for `pissoff`, **hazardous for the rest** | Needs a deferred-apply hook in `m68k_run_jit()` |
| Change floppy images | Host plumbing already exists | The work is TOS media-change, not the swap itself |

---

## 1. Transport: NatFeats, not a new I/O window

The mechanism you need is already built, already JIT-aware, and already proven by
MP3PLAY/VIDPLAY. Adding a feature costs four edits in
`platforms/atari/network/atari_natfeat.cpp`:

1. `NF_FEATURE_PSCTRL` in `enum nf_feature_index` (line 88)
2. `"PSCTRL"` in `nf_feature_names[]` (line 215)
3. `case NF_FEATURE_PSCTRL: return nf_call_psctrl(subid, params);` in `nf_call()` (line 4869)
4. the handler itself, modelled on `nf_call_mp3()` (line 4746)

Calling convention from the 68k side, unchanged:

```
NF_GET_ID   dc.w  $7300      ; 4(sp) = ptr to "PSCTRL"  -> d0 = base id, 0 = absent
NF_CALL     dc.w  $7301      ; 4(sp) = id|subop, 8(sp).. = longword params -> d0
```

Two properties matter for this project specifically:

- **It works under plain TOS.** Unlike HOSTFS (needs FreeMiNT + `hostfs.xfs`),
  the raw opcodes need nothing installed. A floppy-swapper that only works under
  MiNT would be close to useless.
- **Graceful degradation is free.** `nf_get_id()` (line 1066) returns 0 for an
  unknown name, so the .PRG can probe and exit cleanly on an older emulator
  build. On *real* hardware `$7300` is an illegal `moveq` encoding and will trap
  — worth wrapping the probe in a temporary vector-4 handler out of politeness,
  even though this binary will only ever run on PiStorm.

A memory-mapped mailbox in a spare I/O address is the obvious alternative and is
strictly worse here: you'd burn address-space real estate, add a decode branch to
the hot path in `emulator.c`, and hand-roll the JIT-safety analysis that
NatFeats has already had done to it.

---

## 2. The trap: you cannot flush the JIT cache from inside a NatFeat call

This is the one thing that will bite, and it is documented in your own tree at
`atari_natfeat.cpp:4891`:

> INVARIANT (relied on by the JIT): this must behave as an ordinary
> straight-line instruction. It may not modify the PC other than the +2 below,
> may not branch, and may not raise a 68k exception. `build_comp()` declares
> `0x7300`/`0x7301` as `cflow = fl_normal` on the strength of that, which lets a
> NatFeat call sit in the middle of a translated block instead of terminating it.

The consequence for JIT-option changes is sharper than the comment spells out.
When your 68k program executes `NF_CALL`, **it is running inside a translated
block**, and the host return address points into `compiled_code`. So:

- `check_prefs_changed_comp(false)` → `alloc_cache()` → `vm_release(compiled_code, …)`
  (`jit/arm/compemu_support_arm.cpp:2830`)
- `set_cache_state()` → `flush_icache_hard(3)` (line 2711)

…either of these called from the handler frees or invalidates the code the
caller is about to return into. Best case a `SIGSEGV` in `crash_handler`; worst
case silent corruption.

So the affected options split into two classes:

**Class A — free to change immediately.** Plain integers the JIT re-reads without
recompiling anything:

- `pissoff_value` — the compiled-chain budget (`jit_glue.cpp:219-224`, currently
  fixed at startup from `PISTORM_PISSOFF`). This is genuinely the most
  interesting live knob you have: it trades compiled-run length against
  fallback interrupt latency, it's the thing you actually want to A/B against a
  running game, and changing it costs nothing. Your own note says the failure
  mode is audible ("keyboard beep / erratic mouse = a break got delayed"), which
  makes it perfect for interactive tuning.
- `currprefs.m68k_speed`, `cpu_clock_multiplier` — cosmetic-ish but harmless.

**Class B — must be deferred.** `cachesize` (including JIT off = 0), `compnf`,
`comp_constjump`, `compfpu`, `comptrust*`, and an explicit "flush cache now"
button.

### Deferred-apply hook

The safe apply point already exists and there is precedent for it three lines
away. In `cpu/newcpu.cpp:m68k_run_jit()` (line 7730), immediately after
`((compiled_handler *)(pushall_call_handler))()` returns, the loop enters an
`if (regs.spcflags)` block that sets `jit_in_compiled_code = false` — and a few
lines below, the T0/T1/M path calls `flush_icache(3)` right there. That is
outside compiled code, with no block on the host stack.

Pattern:

```c
/* atari_natfeat.cpp — handler side, runs inside a translated block */
case NF_PS_SET_JITOPT:
    g_psctrl_pending.what  = nf_get_param(params, 0);
    g_psctrl_pending.value = nf_get_param(params, 1);
    g_psctrl_pending.armed = 1;
    jit_request_cpu_exit();      /* jit_glue.cpp:467 — SPCFLAG_BRK + pissoff = -1 */
    return 0;                    /* "accepted", not "applied" */
```

```c
/* newcpu.cpp — in m68k_run_jit, in the post-block spcflags block */
if (g_psctrl_pending.armed)
    psctrl_apply_pending();      /* touches changed_prefs, then
                                    check_prefs_changed_comp(false) */
```

`jit_request_cpu_exit()` already does exactly the right thing (`set_special(SPCFLAG_BRK)`
plus `pissoff = -1`), so the block terminates on the next boundary and the apply
lands within microseconds.

Because of this split I'd stage the work so that **read-only status ships
first** — it touches nothing, carries no risk, and gives you the display you
want on its own.

---

## 3. What "JIT status" can actually show

Everything below is a cheap read. Grouped by where it lives.

**Already public, no core changes:**

| Value | Source |
|---|---|
| Bytes of translation cache consumed | `get_jitted_size()` — `compemu_support_arm.cpp:2723` |
| JIT enabled/disabled | `get_cache_state()` — line 2718 |
| Cache size (KB), CPU/FPU model, addr24, comptrust×4, `compnf`, `comp_constjump`, `compfpu`, `comp_hardflush`, `m68k_speed`, `cpu_clock_multiplier` | `currprefs.*` |
| `pissoff_value`, live `pissoff` | globals |
| MFP interrupt histogram + last vector | `pistorm_mfp_iack_counts[16]`, `pistorm_mfp_last_iack_vector` — `jit_glue.cpp:378-381`, already `extern "C"` and already being maintained |
| Config flags (blitter mode, stram cache/direct, native HDMI, fVDI, ET4000, fps) | `config_file.h:147-156` accessors |

**Needs a two-line accessor added** (these are `static` in `compemu_support_arm.cpp`):

```c
uae_u32 get_jit_cache_size(void)      { return cache_size; }          /* line 249 */
uae_u32 get_jit_current_cache(void)   { return current_cache_size; }  /* line 250 */
```

**Worth adding if you want the display to be interesting rather than merely
factual** — none of these exist yet, all are a `++` in a hot-ish path:

- blocks compiled / blocks flushed since start (in `compile_block()`,
  `compemu_support_arm.cpp:~3436`, and in `flush_icache_hard()`)
- cache-full → hard-flush event count (the thing that actually explains a
  sudden framerate cliff)
- checksum-invalidation count (self-modifying code churn — Atari games do this
  a lot and it's invisible today)

That last group is what turns the front end from "a settings box" into a
diagnostic tool, and it's the part I'd argue is worth the extra effort.

### Suggested API shape

One `GETINT` sub-op with an index namespace beats twenty sub-ops — it keeps the
68k side to a single wrapper and lets you add values without touching the .PRG:

```
subop 0  PS_VERSION                        -> API version
subop 1  PS_GETINT   p0 = index            -> value          (0..63 = JIT/CPU)
subop 2  PS_GETSTR   p0 = index, p1 = buf, p2 = len -> length (rom path, cfg name…)
subop 3  PS_SETINT   p0 = index, p1 = value -> 0 ok, -1 rejected, 1 deferred
```

Returning **1 = deferred** for Class B is worth doing: the GEM dialog can then
grey the control and say "applies on next block boundary" rather than lying.

---

## 4. Floppy image swapping

### The plumbing is already there

`platforms/atari/fdd/atari_fdd.h:226-228` gives you exactly what's needed:

```c
int  fdd_insert_disk(int drive, const char *image_path, bool write_protect);
void fdd_eject_disk(int drive);
void fdd_set_write_protect(int drive, bool wp);
```

`fdd_insert_disk()` (`atari_fdd.c:162`) re-derives geometry from the BPB on every
insert, with a size-based fallback and a FAT-location scan for images with extra
boot-sector copies — so a DD↔HD or SS↔DS swap is already handled. It opens
`O_RDWR` unless write-protected, so writes go back to the image file.

**Threading is fine.** All the mutex calls in `atari_fdd.c` are commented out,
which looks alarming, but the NatFeat handler runs on the CPU thread — the same
thread as `fdd_io_read`/`fdd_io_write`. The only other toucher is `fdd_vbl()`
(motor timeout) on the VBL thread. So a swap is atomic with respect to FDC I/O
by construction. Still: **refuse the swap while `FDC_STATUS_BUSY` is set or a
DMA is in flight**, and return an error the front end can show.

### The actual problem: TOS won't notice

`media_changed` exists in `fdd_drive_t` (`atari_fdd.h:157`), is set by
`fdd_eject_disk()` and cleared by `fdd_insert_disk()` — and is **read by
nothing**. Grep confirms: `atari_fdd.c:259`, `:279`, and a commented-out debug
print at `:355`. It is dead today.

Which means, as things stand, swapping an image under a running TOS gives you a
filesystem with the *old* disk's FAT and directory cached in RAM and the *new*
disk's data on the media. First write corrupts the image.

The ST has no disk-change line. TOS detects media change through two heuristics,
both of which have to be fed:

1. **Write-protect line flicker.** `flopvbl` polls the WP state; a transition is
   what makes GEMDOS's `Mediach()` return 2 ("definitely changed"). The fix is to
   consume `media_changed` in the status path — around `atari_fdd.c:1201`, where
   `FDC_STATUS_WRTPROT` is set — so that the first Type I status read after a
   swap reports a toggled WP, then clears the flag.
2. **Serial number in the boot sector.** TOS compares the 24-bit serial at offset
   `$08`. Images built from the same source often share it, so this alone is not
   reliable — but it's a useful belt-and-braces, and the host side could
   optionally rewrite the in-memory copy.

Belt and braces from the 68k side, which I'd do regardless: after a successful
swap the .PRG calls `Getbpb(drive)` to force a BPB reload and then walks the root
with `Fsfirst`/`Fsnext` to flush the directory cache. On plain TOS that
combination plus the WP flicker is what Hatari-style swaps rely on.

**This is the part that needs real testing on hardware, and it's the only part of
the whole project I can't reason my way to confidence on from source alone.**
TOS 1.02 / 1.62 / 2.06 / EmuTOS all differ slightly here.

### Choosing an image from the Atari side

The images live on the Pi's SD card. Two ways to let the user pick one:

- **HOSTFS + `fsel_input()`** — natural, but requires FreeMiNT + `hostfs.xfs`,
  which throws away the "works under plain TOS" property that made NatFeats the
  right transport in the first place.
- **Host-side directory enumeration over NatFeats** — a handful more sub-ops,
  works everywhere, and mirrors `mp3_gemdos_to_host()` (line 4706) in reverse:

```
subop 16  FDD_SETDIR   p0 = ptr to host path, or 0 = configured default
subop 17  FDD_COUNT                          -> number of images found
subop 18  FDD_NAME     p0 = index, p1 = buf, p2 = len
subop 19  FDD_INSERT   p0 = drive(0/1), p1 = index, p2 = wp  -> 0 / -1
subop 20  FDD_INSERTP  p0 = drive, p1 = ptr to path, p2 = wp -> 0 / -1
subop 21  FDD_EJECT    p0 = drive
subop 22  FDD_STATUS   p0 = drive  -> bit0 inserted, bit1 wp, bit2 busy, bits8+ type
subop 23  FDD_CURNAME  p0 = drive, p1 = buf, p2 = len
```

I'd go with the second. The host side should filter the listing to images
`fdd_insert_disk()` can actually open — that means **raw `.ST`/`.IMG` only**;
`.MSA` and `.STX` are not supported by the current code, and silently listing
them would produce a confusing failure. Adding MSA decompression host-side is a
small, self-contained follow-on if you want it.

Default image directory belongs in the config file next to the existing
`FDD_s { bool enabled; char img_path[256]; }` (`config_file.h:68-72`) —
something like `fdd dir /home/pistorm/floppies`.

---

## 5. The GEM front end

Toolchain: `m68k-atari-mint-gcc`, matching how `MP3GEM.PRG` / `VIDGEM.PRG` were
built. Note the `cdev/` sources referenced in `NATFEATS.md:96` and `:135` are
**not in this repo** — only the built binaries under `configs/gem-binaries/`. If
you want PSCTRL to live alongside them in-tree, that's worth fixing at the same
time.

Shape, as a `.PRG`:

```
+--------------------------------------------------+
|  PiStorm Control                            [ok] |
+--------------------------------------------------+
| JIT   [ON ]  cache 8192K   used 1204K  (14%)     |
| CPU   68030   FPU 68882 (JIT FPU on)             |
| flags nf:1  constjump:1  trust b/w/l/n: 0/0/0/0  |
| chain budget (pissoff)  [ - ]  1024  [ + ]       |
| blocks 18432   flushes 3   invalidations 91      |
+--------------------------------------------------+
| Drive A:  GAME_D1.ST          [Eject] [Change..] |
| Drive B:  (empty)             [Eject] [Change..] |
+--------------------------------------------------+
```

- The `pissoff` +/- buttons act instantly (Class A). Everything else in the JIT
  block is either display-only or shows "applies shortly" when set (Class B).
- `[Change..]` opens a scrolling list populated from `FDD_COUNT`/`FDD_NAME` —
  not `fsel_input()`, since the images aren't on a GEMDOS drive.
- Probe `NF_GET_ID("PSCTRL")` at startup; on 0, put up an alert and exit.
- Poll `PS_GETINT` on a timer (`evnt_multi` with `MU_TIMER`) so the counters move
  while it's open — that's what makes the diagnostic counters worth having.

One caveat on the disk-swap UI: for the classic multi-floppy game case, the user
is *in the game*, not at the Desktop, so a `.PRG` can't help them. A desk
accessory would, but only in GEM-aware software. The honest answer for
"Insert disk 2" prompts inside a game is a host-side hotkey (the USB keyboard
path in `kbd_usb.c` already has the hooks) rather than any 68k program. Worth
knowing before you build the GEM version and discover it doesn't cover the case
you wanted it for. The `.PRG` remains the right thing for setup and for JIT
tuning between runs.

---

## 6. Staging

| Phase | Content | Risk | Rough size |
|---|---|---|---|
| 1 | `PSCTRL` feature + `PS_GETINT`/`PS_GETSTR` read-only; two accessors in `compemu_support_arm.cpp`; `.TTP` that dumps status | none | ~250 lines host, ~150 lines 68k |
| 2 | Floppy sub-ops + `media_changed` consumption in the FDC status path + `Getbpb`/`Fsfirst` refresh in the front end | **medium — this is the one to test hard** | ~300 lines host |
| 3 | Live `pissoff` via `PS_SETINT` (Class A only) | low | ~30 lines |
| 4 | Deferred-apply hook in `m68k_run_jit()`; Class B options incl. JIT on/off and manual flush | medium — touches the run loop | ~80 lines |
| 5 | New JIT counters (blocks compiled/flushed/invalidated) | low | ~20 lines |
| 6 | GEM `.PRG` | low | ~800 lines 68k |

Phases 1–3 are independently useful and carry essentially no risk to a working
emulator. Phase 4 is where you'd want to be careful, and phase 2 is where the
surprises will be.

---

## 7. Open questions

1. **Which TOS versions must the swap work on?** EmuTOS is the most forgiving
   and the easiest to instrument; TOS 1.02 the least. This determines how much
   effort the media-change work is.
2. **Should the `.PRG` sources live in this repo?** Right now the 68k side is
   binaries-only, which makes any of this hard for anyone else to pick up — and
   the README explicitly hopes other people will.
3. **MSA/STX support** — out of scope for a first cut, but it changes the
   directory-listing filter if you want it later.
4. **Is a host-side hotkey for disk swap actually the feature you want?** See the
   caveat in §5. It's a different (smaller) piece of work in `kbd_usb.c`.
