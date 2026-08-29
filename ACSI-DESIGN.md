# ACSI — an emulated hard-disk target on the DMA port

Goal: the guest boots and runs hard disks over the Atari's native ACSI
interface, served from image files — the way every pre-IDE machine and
every period driver (AHDI, ICD, PP) expects, and the way **Spectre
128/GCR** reaches its Mac partitions. The emulated IDE at $F00000 stays;
this is the second, period-correct storage path, and the only one
Spectre can see.

## Image files and the `.hfs` suffix

Two image flavours, told apart by suffix — this is a deliberate design
decision, not a convenience:

| suffix | content | served as |
|---|---|---|
| `.img` (or anything else) | raw Atari ACSI disk: AHDI root sector, partitions, the works | sectors passed through 1:1 |
| `.hfs` | a BARE Macintosh HFS volume — no Atari structures at all | the emulator SYNTHESIZES sector 0 (an AHDI root sector holding one `MAC`-type partition starting at sector 1) and maps LBA n≥1 to file offset (n-1)*512 |

Why: a bare `.hfs` file is a plain HFS filesystem image, directly usable
by Basilisk II ("disk" pref), by hfsutils on the Pi (`hmount`/`hcopy`
for moving files in and out from Linux), and by Mini vMac. One file,
three worlds. Spectre on the Atari side sees a standard ACSI disk whose
root sector advertises a `MAC` partition — which is exactly what its
own formatter would have created — and mounts it. The synthesized
sector 0 is generated at open time and read-only; writes to LBA 0 of an
`.hfs` image are ignored with a log line.

Config:

```
# up to 8 targets, ACSI IDs assigned in order from 0 (like hdd lines)
acsi ../dkimages/atari-acsi.img
acsi ../dkimages/spectre-system6.hfs
```

## Hardware contract (all already documented in-repo)

The ACSI bus lives behind the same DMA port the FDC uses:

- `$FF8604` r/w — data register (CDB bytes / status / sector count via
  SCREG mode), `$FF8606` r — DMA status, w — DMA mode control. Bit
  semantics incl. `DMA_MODE_FDC_HDC` (bit 3: HDC vs FDC routing) and
  SCREG are in `platforms/atari/fdd/atari_fdd.h`.
- A1 line: mode-register bit distinguishing the FIRST CDB byte (device
  select + opcode) from the rest.
- DMA base/counter regs `$FF8609/0B/0D` — shared with the FDC path;
  the FDD emulation already models them.
- IRQ: the ACSI target pulls the DMA interrupt, which the guest sees as
  MFP GPIP5 low ($FFFA01 bit 5). Same shim pattern as the USB keyboard's
  GPIP4: `mfp_gpip_shim` presents the bit, ipl raises level 6 when the
  guest's IERB/IMRB allow — machinery exists, one more bit.

Reference implementation for command semantics: **Hatari `hdc.c`** — it
models the Adaptec ACB-4000-bridge flavour of ACSI that real ST drives
(and Spectre under Hatari, proven) expect.

## Command set (phase 1)

TEST UNIT READY, REQUEST SENSE, INQUIRY, READ(6), WRITE(6), SEEK(6),
MODE SENSE, FORMAT UNIT (no-op, returns good). Six-byte CDBs limit LBA
to 21 bits = 1 GB; the ICD extended-command convention (opcode $1F
prefix wrapping full SCSI CDBs) is phase 2 for bigger disks.

Status byte + sense data per Hatari's model (check condition on bad
LBA, sense 0x21/0x25 etc. — period drivers do look at these).

## Integration points

- Dispatch: `hw_bput/hw_wput` → `HW_PAGE_FDD_DMA` → `hw_fdd_bput` →
  `fdd_io_write`. The mode register's FDC/HDC bit routes: HDC-selected
  accesses go to the new `acsi.c` when the addressed ID is emulated;
  FDC-selected accesses keep going to the FDD emulation. IDs NOT in the
  cfg pass through to the real bus untouched — an emulated ID 0 can
  coexist with a real UltraSatan on ID 1..7, and vice versa. (The real
  ACSI bus needs the arbitration firmware; the emulated targets do not.)
- Data path: reuse the FDD's sector delivery — write sectors to the
  natmem mirror AND through to real ST-RAM (the shifter/DMA-visibility
  rules from the STRAM work apply; the screen write-through and
  sub-4MB fold lessons are already encoded in sr_*).
- DMA counter/base semantics: shared code with FDD; the counter
  decrements per 512-byte block and the status register's count-zero
  bit is what drivers poll.
- Boot: TOS's ACSI boot ROM reads LBA 0 of each ID at boot; serving a
  bootable AHDI root sector makes `.img` disks bootable with no help.

## Spectre notes

- Spectre reads the AHDI root sector looking for `MAC`-ID partitions;
  both flavours provide one (`.img` if the user made one; `.hfs`
  always).
- Spectre GCR floppy features needed its cartridge hardware; hard-disk
  Mac volumes need only this target.
- Machine config: `games-st.cfg` class (68000, interpreter, machine st,
  plain TOS) — the config the MST already runs games on.

## Testing ladder

1. New `ataritest --acsi` subtest: raw CDB exercise of INQUIRY/READ/
   WRITE against an image, no TOS involved.
2. TOS 2.06 + AHDI 6 on the MST: partition, format, desktop copy tests.
3. ICD/PP driver boot from the image (most common real-world driver).
4. Spectre 128 with a `.hfs` image: format check (it should NOT need to
   format — the volume is premade), System 6 boot, file copy, reboot
   persistence, then read the same `.hfs` under Basilisk II on the Pi
   to prove the interop claim.

## Effort

Phase 1 (bootable read/write target + `.hfs` wrapper): the command
engine is small; the risk is all in the command-phase handshake timing
(A1/IRQ pacing against real driver polling loops). The FDD emulation
crossed the same river for the WD1772; its structure is the template.
