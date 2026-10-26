# HOSTDOS host test bench

Runs `atari-tools/hostdos/PSHOSTFS.PRG` on any Linux host - no Pi, no ROM.
Musashi executes the TSR for real (its own install code, trap #1 hooked
through the vector); a small fake GEMDOS stands in for the ROM, and the
HOSTDOS NatFeat is the real `platforms/atari/network/hostdos.cpp` on a
temporary folder. `TEST.PRG` (test.S) then makes ~50 GEMDOS calls and
records each result; `harness.c` checks them and the folder.

    sudo apt install binutils-m68k-linux-gnu    # once
    tools/hostdos-test/build.sh
    tools/hostdos-test/run.sh

`run.sh` covers 68000 and 68030 (long) exception frames and GEMDOS 0.13,
0.15 and 0.19, i.e. all three Pexec paths HOSTDOS uses (5+4, 5+6, 7+6).

The fake ROM answers every call that should NOT have reached it with a
marker (-777 path call, -778 handle call, -779 Fsnext, -780 drive call), so a
call that went the wrong way shows. Its Pexec 4/5/6/7 and Pterm keep TOS's
memory ownership rules, and the bench checks that no block is leaked and no
host file descriptor is left open. `CHILD.PRG` (child.S) is loaded from the
host drive and needs relocation, a cleared BSS, its basepage and the
inherited current folder.

Musashi is built from a copy of `third_party/musashi` with the 010-040
generations switched back on (the STBOX build compiles them out).

`BENCH_VERBOSE=1` lists every passing check; `HOSTDOS_DEBUG=1` shows the
engine's `[HOSTDOS]` trace.

## Strict TOS model for supervisor-mode Pexec

HOSTDOS runs a host-drive program with a Pexec 4/6 issued from *inside*
its trap #1 handler, i.e. in supervisor mode. When the child ends, the
bench's fake TOS now hands back only the supervisor stack and d0: the
user stack pointer stays the child's and d1-d7/a0-a6 are scrambled
(`terminate()` in harness.c, `from_super`). That is the worst real TOS
may do there, and it matches the field crash of 26 Oct 2026 (a program on
S: that Pexec'd another crashed on its next `rts` once the child ended).
TEST.PRG checks its stack pointer and d7/a6 across its Pexec of CHILD.PRG
(slots 50-53), so HOSTDOS must keep the caller's USP and d3-d7/a3-a6
itself - which it does around the Pexec in hostdos.S.
