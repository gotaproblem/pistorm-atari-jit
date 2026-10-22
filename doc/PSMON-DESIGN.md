# PSMON — a GEM/MiNT CPU & memory monitor for PiStorm Atari JIT

Design investigation. Companion to `PSCTRL-DESIGN.md`; shares its `PSCTRL`
NatFeat transport and its phase-1 read-only API.

**Scope agreed:** effective 68k MHz / speed multiple, JIT hit rate %, guest
idle-vs-busy %, guest ST/TT RAM free %, JIT translation-cache fill %. Runs as a
GEM desk accessory and as a MiNT app. No always-on overlay — the meter is
allowed to stall when a non-AES program has the CPU.

---

## 1. The architecture decision that matters most

**Sample host-side on a fixed wall-clock tick. Make the NatFeat read O(1) and
side-effect free.**

The naive design has the 68k app ask "what's the cycle count?" twice and divide
by elapsed time. Don't. Under cooperative GEM the app's polling interval is
wildly irregular — it gets AES time whenever the foreground app happens to
yield — so every rate you compute is divided by an unknown, jittery denominator.
Worse, the act of polling is itself guest load, so the monitor inflates the very
"busy" figure it's reporting.

Instead: the emulator already runs a 50 Hz VBL thread
(`emulator.c:1120`, `fdd_vbl_thread`). Hang the sampler off that. Every 25 ticks
(500 ms) it snapshots the free-running counters, differences them against the
previous snapshot, divides by *its own* measured wall interval, and stores the
finished percentages and rates in a small struct.

`PS_GETINT` then just reads a field. Consequences:

- The numbers are identical whether the app polls at 2 Hz or 0.5 Hz or stalls
  for a minute mid-game. Poll rate cannot distort them.
- The read costs a few longwords — the observer effect drops to the app's own
  AES overhead, which is unavoidable and small.
- When the app *is* frozen behind a non-AES program, the host keeps sampling.
  On the next redraw it shows current truth, not a stale delta.
- A future host-side overlay, or a logging mode, or `PISTORM_STATS=1` on stderr,
  all reuse the same sampler for free.

Keep a 60-slot ring of the 500 ms snapshots too (30 s of history, ~2 KB). Costs
nothing and lets the app draw a scrolling graph later without any API change.

---

## 2. The five numbers

### 2.1 Effective 68k MHz / speed multiple

**Source:** the JIT already emits a per-block cycle decrement. Every block
epilogue subtracts `scaled_cycles(totcycles)` from the countdown
(`compemu_support_arm.cpp:3913`, `:3936`, `:3949`). Accumulate the consumed
countdown at each chain break in `m68k_run_jit()`.

**Correction needed.** `scaled_cycles` is not raw 68k cycles:

```c
/* jit/arm/compemu_arm.h:142 */
#define scaled_cycles(x) (currprefs.m68k_speed<0 ? (((x)/SCALE) ? ... ) : (x))
#define SCALE 2                        /* line 84 */
#define MAXCYCLES (1000 * CYCLE_UNIT)  /* line 85 */
```

Your `m68k_speed` defaults to `-1` (`jit_glue.cpp:159`), so the divide-by-2 path
is live: multiply the accumulated countdown by `SCALE` to recover guest cycles,
then divide by `CYCLE_UNIT`. Two caveats worth putting in a comment:

- Blocks costing more than `MAXCYCLES` are clamped, so very long straight-line
  blocks under-count. In practice these are rare; error is well under 1%.
- If `m68k_speed >= 0` is ever configured, `SCALE` doesn't apply. Read
  `currprefs.m68k_speed` in the sampler and branch.

**Framing.** Be precise about what the number means, because it's easy to
oversell: this is *"you are retiring the instruction mix a real 68k at X MHz
would retire per second"*, not host clock speed. That's the right metric for a
PiStorm accelerator and it's honest. Speed multiple = X ÷ 8 for a stock ST.

**Cost:** one add per chain break (~every 1024 cycle-units). Free.

### 2.2 JIT hit rate %

**Source:** `execute_normal()` (`cpu/newcpu.cpp:7508`) is the *only* way out of
translated code back into interpretation-plus-compile, and it already computes
`total_cycles` for the block it interprets. So:

```c
/* end of execute_normal() */
g_stats.interp_cycles += total_cycles;
g_stats.interp_calls++;
```

and hit rate is exact, not estimated:

```
hit% = 100 * (1 - interp_cycles / total_cycles_this_window)
```

Both terms come from the same window, both are already being computed. **Cost:
one add on a path that is by definition cold.** This is the cheapest of the five
and arguably the most diagnostic — it's the number that tells you whether the
JIT is working or whether something is thrashing it.

Worth pairing with three plain event counters, shown as rates rather than
percentages:

| Counter | Where | What it tells you |
|---|---|---|
| blocks compiled/s | `compile_block()` — `compemu_support_arm.cpp:3613` | translation churn |
| hard flushes/s | `flush_icache_hard()` — line 2711 / 2826 | cache pressure |
| checksum invalidations/s | the block-invalidation path | self-modifying guest code |

A steady hit rate of 99% with 400 recompiles/sec means something is rewriting
code under the JIT — a state you currently have no way to see.

### 2.3 Guest idle vs busy — the honest version

You picked this knowing my warning; here's how to get the most truthful thing
available rather than a number that reads 100% forever.

**Two separate signals, reported separately, labelled differently.**

**(a) True idle — cycles executed with `regs.stopped != 0`.** Exact, and
meaningful under MiNT, whose idle task does `stop #$2300`. Reads a flat 0 under
plain TOS/GEM, which is *correct* — the ST genuinely is not idle there.

⚠️ **Finding — see §7, this turned out to be a live bug.** This can't be measured
today because the `regs.stopped` guard in the JIT run loop is compiled out by a
one-character typo at `cpu/newcpu.cpp:7820`. Details and consequences in §7.

**(b) Wait-loop detection — for plain GEM, where (a) is structurally useless.**
When GEM idles in the AES event loop it executes a tiny handful of blocks over
and over; real work has a large working set. So sample the guest PC at each
chain break (they're already frequent and already a C-side event, so this is
free), keep the last N samples in a small ring, and compute what fraction fall
within a narrow address span.

```
tight cluster over the window  ->  "WAITING"
dispersed                      ->  "WORKING"
```

This is a heuristic and should be labelled as one. **Call it WAIT in the UI, not
IDLE** — the distinction is real and users will otherwise read it as a bug when
it disagrees with what they expect. Tune the span/threshold against the Desktop
event loop, which is the reference case.

My recommendation for the display: show `IDLE` when (a) is non-zero (you're
under MiNT), otherwise show `WAIT` from (b), and put a one-character marker so
it's obvious which mode is in effect.

### 2.4 Guest ST / TT RAM free — **needs no host changes at all**

Worth saying plainly because it saves work: the app can compute this entirely
by itself.

**Totals**, from sysvars via `Supexec()`:

| Sysvar | Address | Meaning |
|---|---|---|
| `phystop` | `$42E` | top of ST-RAM |
| `ramtop` | `$5A4` | top of TT/Fast RAM (0 if none) |
| `ramvalid` | `$5A8` | `$1357BD13` ⇒ `ramtop` is valid |

**Free**, by GEMDOS call:

- TOS 3.0+ / MiNT: `Mxalloc(-1L, 0)` → largest free ST-RAM block,
  `Mxalloc(-1L, 1)` → largest free TT-RAM block. Two calls, clean.
- TOS 1.x: only `Malloc(-1L)` exists, and it returns the *largest free block*,
  not total free. For a percentage bar that's usually close enough on a
  freshly-booted ST; if you want true total-free you have to `Malloc` in a loop
  until it fails and then `Mfree` everything back, which is intrusive and I'd
  skip it. Gate on `Sversion()` / `_sysbase->os_version >= 0x0300`.

The one thing worth adding host-side is a `PS_GETINT` index returning the
*configured* `ttram`/`ttram_size` (`config_file.h:95-96`), as a sanity check —
if `ramvalid` disagrees with the emulator config, the TOS in use isn't
initialising Fast RAM and the user wants to know.

### 2.5 JIT translation-cache fill %

`get_jitted_size()` is already public (`compemu_support_arm.cpp:2723`). The
denominator is `static uae_u32 cache_size` (line 249), so it needs a two-line
accessor:

```c
uae_u32 get_jit_cache_size(void)    { return cache_size; }          /* line 249 */
uae_u32 get_jit_current_cache(void) { return current_cache_size; }  /* line 250 */
```

**Expect a sawtooth, and don't treat it as a bug.** `current_compile_p` grows
monotonically until a hard flush drops it to near zero. The bar will ramp to
~100% and collapse, repeatedly. That *is* the signal: the collapse rate is your
flush frequency, and a fast sawtooth on the default 8192 KB cache
(`jit_glue.cpp:157`) is the thing that explains a sudden framerate cliff. Show
the fill bar and the flush counter together and the two explain each other.

---

## 3. NatFeat API

Reuses the `PSCTRL` feature from `PSCTRL-DESIGN.md` §3 — same indexed `PS_GETINT`
namespace, so PSMON needs no new sub-ops, just new indices. All reads are
snapshot reads from the sampler struct; none of them touch the JIT.

```
NF_GET_ID("PSCTRL")                 -> base id, 0 = emulator too old

PS_GETINT (subop 1), index:
  32  PS_STAT_EPOCH        snapshot serial, bumps every 500ms
  33  PS_STAT_MHZ_X100     effective 68k kHz/10  (e.g. 6842 = 68.42 MHz)
  34  PS_STAT_SPEEDX_X100  multiple of stock 8MHz ST (855 = 8.55x)
  35  PS_STAT_HITRATE_X100 JIT hit rate, 0..10000
  36  PS_STAT_IDLE_X100    true idle (regs.stopped), 0..10000, 0 under plain TOS
  37  PS_STAT_WAIT_X100    wait-loop heuristic, 0..10000
  38  PS_STAT_IDLE_VALID   1 = STOP accounting live (trust 36), else use 37
  39  PS_STAT_CACHE_USED   translation cache bytes in use
  40  PS_STAT_CACHE_SIZE   translation cache bytes total
  41  PS_STAT_COMPILES     blocks compiled in last window
  42  PS_STAT_FLUSHES      hard flushes in last window
  43  PS_STAT_INVALIDATES  checksum invalidations in last window
  44  PS_STAT_INTERP_CALLS execute_normal() calls in last window
  45  PS_CFG_TTRAM_SIZE    configured TT-RAM bytes (0 = disabled)
```

Fixed-point ×100 throughout — no FPU dependency in the 68k app, and a plain
`divu`/`divs` renders it. `PS_STAT_EPOCH` lets the app skip a redraw when
nothing has changed, which matters under cooperative GEM where redraws are
expensive relative to the AES time you get.

---

## 4. The 68k side

### One source, two binaries

Build the same C twice with `m68k-atari-mint-gcc`:

- `PSMON.ACC` — `-DBUILD_ACC`, desk accessory, `menu_register()`, handles
  `AC_OPEN`/`AC_CLOSE`, never calls `appl_exit()`.
- `PSMON.APP` — normal `.PRG`/`.APP`, opens a window, quits properly.

The runtime "am I an accessory?" test (basepage `p_parent == NULL`) exists and
works on plain TOS, but MiNT does give accessories a parent, so it is not
reliable in the environment you most care about. Two binaries from one source
with an `#ifdef` is the boring, correct answer.

### Update loop

`evnt_multi()` with `MU_TIMER | MU_MESAG | MU_BUTTON`, 500 ms timer to match the
sampler cadence. Compare `PS_STAT_EPOCH` before redrawing; if unchanged, go
straight back to `evnt_multi` without touching the VDI.

Under MiNT this is preemptive and behaves like any modern monitor. Under plain
GEM it updates at the Desktop and inside AES-aware programs, and stalls
elsewhere — the accepted limitation. Two small things make the stall much less
irritating:

- On the first redraw after a stall, the numbers are *current* (host-side
  sampling, §1), not a stale delta. No garbage frame.
- Track wall time between redraws via `_hz_200` (`$4BA`) and, if the gap exceeds
  ~2 s, grey the graph region for the missing span rather than drawing a
  straight line across it. A visibly honest gap beats a fabricated one.

### Drawing

Keep it cheap — this thing runs constantly.

- Bars: `vsf_color()` + `v_bar()` inside the work rect. Don't rebuild an object
  tree per frame.
- Redraw only changed digits. Clip to the changed rect, not the window.
- Proper `wind_update(BEG_UPDATE)` / `wind_get(WF_FIRSTXYWH…NEXTXYWH)` rectangle
  walk — an accessory that skips this corrupts the Desktop.
- Monospace via the system font; `v_gtext` is fine at this update rate.

Sketch:

```
+------------------------------------------+
| PiStorm Monitor                          |
+------------------------------------------+
| Speed   68.4 MHz    8.55x ST             |
| JIT hit [#########.]  99.2%              |
| WAIT*   [##........]  18%                |
|                                          |
| JIT cache [######....] 61%   4998K/8192K |
|   compiles/s 312   flushes/s 0   inval 4 |
|                                          |
| ST RAM  [#####.....] 52%  2048K/4096K    |
| TT RAM  [##........] 21%  ...            |
+------------------------------------------+
  * heuristic - no STOP accounting on this host
```

---

## 5. Cost and staging

| Phase | Work | Risk | Size |
|---|---|---|---|
| 1 | Host sampler struct + 500 ms tick in the existing VBL thread; two accessors in `compemu_support_arm.cpp` | none | ~150 lines |
| 2 | Cycle accumulator at chain break (§2.1) + `interp_cycles` in `execute_normal` (§2.2) | none — both are single adds | ~20 lines |
| 3 | `PSCTRL` feature + `PS_GETINT` indices; a `.TTP` that dumps one snapshot | none | ~150 lines |
| 4 | Counters in `compile_block` / `flush_icache_hard` / invalidation | none | ~15 lines |
| 5 | Wait-loop PC-clustering heuristic; decide what to do about `PISTORM_ATARI_` at `newcpu.cpp:7820` | low, needs tuning | ~60 lines |
| 6 | `PSMON.ACC` / `PSMON.APP` | low | ~900 lines 68k |

Phases 1–4 are all single-add instrumentation on cold or infrequent paths and
carry no measurable performance cost. Phase 5 is the only one needing
judgement. Phase 6 is the bulk of the work and is ordinary GEM programming.

Note phases 1 and 3 overlap almost entirely with phase 1 of `PSCTRL-DESIGN.md` —
if you build the control panel first, PSMON is mostly just new indices and a
second front end. Worth doing them in that order.

---

## 6. Open questions

1. **Which MiNT?** FreeMiNT with XaAES behaves differently from plain MiNT +
   the TOS AES for accessory scheduling. The `.ACC` needs testing on whichever
   you actually run.
2. **Is the wait-loop heuristic worth it**, or would you rather ship `IDLE` as
   MiNT-only and leave it blank under plain GEM? The heuristic is ~60 lines and
   some tuning; the honest alternative is showing nothing rather than something
   approximate. **Note the answer may change once §7 is fixed** — with STOP
   accounting live, MiNT gives a real `IDLE` and the heuristic is only needed
   for plain GEM.
3. **Host Pi core load** — you didn't pick it, and I think that's right for a
   68k-side monitor. But it's ~20 lines via `clock_gettime(CLOCK_THREAD_CPUTIME_ID)`
   in the same sampler, and on a 2 GB Pi 4 running video playback it's the
   number that explains stutter the guest-side metrics can't see. Cheap to add
   later if you ever want it. **It would also be the fastest way to confirm the
   §7 fix works** — the spinning core should visibly drop.

---

## 7. `newcpu.cpp:7820` — a one-character typo disabling STOP in the JIT loop

Investigated after the first draft. This is a real bug, not a design choice.

### The evidence

There is **exactly one** occurrence of `PISTORM_ATARI_` in the entire tree,
against 18 correct `PISTORM_ATARI` (defined at `sysconfig.h:45`). The
`_NETWORK_H` / `_NATFEAT_H` style hits are unrelated include guards.

More decisively, the same STOP guard appears **four times** — once per run loop —
and only the JIT one is misspelled:

| Line | Run loop | Guard |
|---|---|---|
| **7820** | **`m68k_run_jit`** | **`#ifdef PISTORM_ATARI_`** ← |
| 8876 | interpreter | `#ifdef PISTORM_ATARI` |
| 8936 | interpreter | `#ifdef PISTORM_ATARI` |
| 9008 | interpreter | `#ifdef PISTORM_ATARI` |

Four siblings, same author, same commit (`4485740`, "stability and performance
updates"), same body shape — three right, one with a trailing underscore. That
is a slip, most likely a temporary disable during bring-up that never got
reverted.

### What actually happens today

Not a hang — but wasteful. The chain is:

1. `SPCFLAG_STOP` **does not exist in this tree** (grep returns nothing). The
   stopped state is carried purely by `regs.stopped`.
2. The JIT refuses to compile STOP — `gencomp_arm.c:4220` is
   `case i_STOP: isjump; failure;` — so a STOP always terminates the block and
   is handled interpretively by `execute_normal()`.
3. The interpreted STOP runs `do_cycles_stop(4)` then `m68k_setstopped(1)`
   (`cpuemu_0.cpp:19112-19113`). PC stays on the STOP;
   `m68k_resumestopped()` only advances it on wake.
4. Back in `m68k_run_jit`, `regs.stopped` is never tested (block disabled) and
   `regs.spcflags` was never set. So the loop re-dispatches, hits the same PC,
   and goes through `execute_normal()` again — cache-miss check, `pc_hist`
   array, block-candidacy accounting, the lot.
5. That repeats at full host speed until `g_irq > regs.intmask` at the check
   just below the dispatch fires `intlev()`.

So the guest wakes correctly, and the `do_cycles_stop()` v3 fix does still fire
(it's reached from the interpreted opcode) — but instead of the intended
`do_cycles_stop(4); intlev(); continue;` three-line wait, every idle iteration
drags the full interpreter-and-recompile-check path. **A Pi core sits pinned at
100% for the whole time the guest is stopped.**

Why it went unnoticed is obvious once you see it: plain TOS barely uses STOP, so
the ST desktop looks fine. FreeMiNT's idle task *does* STOP — which is precisely
the configuration where you'd notice, and precisely the one this monitor targets.

### The fix

Delete one character at `newcpu.cpp:7820`. Then:

- `IDLE` becomes measurable, exactly, by accumulating cycles across that branch —
  the cheapest possible place to instrument it, since you're already taking the
  branch.
- A core stops spinning under MiNT. On a 2 GB Pi 4 that is thermal headroom and
  video-decode headroom you are currently giving away.
- The "Atari STOP-wake fix (v3)" at `newcpu.cpp:10814`, whose comment describes
  starving *"the JIT dispatcher's g_irq check"*, finally guards the path it was
  written for rather than only the interpreter's.

### Test it before trusting it

The v3 comment records a v1 and a v2 that both misbehaved (*"v2 kicked from
checkint on every MOVE-to-SR too, storming the run-loop relaunch safety net
during boot"*), so this area has a history of subtle breakage. Re-enabling the
block puts a `continue` back into the JIT dispatch loop that has not executed in
this configuration since that commit. Watch specifically for:

- boot storms — the `[JITGLUE] escaped guest exception caught at top level`
  message from `jit_glue.cpp:351`
- MFP latency regressions: keyboard beeping, erratic mouse (your own documented
  tell for a delayed block break)
- wake latency from STOP, under both MiNT and plain TOS

Measure the win at the same time: `top -H` on the Pi, MiNT sitting idle at the
desktop, before and after.
