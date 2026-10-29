# Byte-swapped hard-disk images (IDE and ACSI)

Some images are stored with the two bytes of every 16-bit word swapped:
ARAnyM writes its disks that way (e.g. EasyMiNT_*_Aranym.img), and so does a
disk read through a twisted-cable IDE adapter. When an image is attached the
emulator reads sector 0 and decides which way round the FILE is
(`platforms/atari/disk_order.c`):

    [IDE0] HDD0 /home/pistorm/dkimages/1GBIDE.img: stored native - MBR 55AA DOS-parts readable as is
    [ACSI] ID 0 .../EasyMiNT_Master_Aranym.img: stored byte-swapped - AHDI BRA readable swapped

It scores sector 0 as it is and with every word swapped - DOS MBR signature
55 AA, DOS partition entries, AHDI root entries (GEM/BGM/XGM/RAW/LNX/...), a FAT
BPB, a 68000 BRA.S - and the clearly better reading wins. A blank or
unrecognised sector 0 is "order unknown" and the image is used as it is.

## What is done with a swapped image

- ACSI: presented unswapped (every sector swapped on read, swapped back on
  write). The file on disk is never rewritten in another order.
- IDE: depends on `ide_byteorder`, because the IDE data port's legacy order
  is itself a swap of every word:
  - `ide_byteorder native` (default; HDDRIVER / AHDI / TOS): the image is
    presented unswapped, as for ACSI.
  - `ide_byteorder swapped`: nothing is done - the port's swap already undoes
    the file's, exactly as before this change.

Native images are never touched, so every image that worked before still
works the same way.

## Overrides

Per IDE slot / ACSI ID, when the detection is wrong or says "unknown":

    hdd_order 0 swapped      # IDE slot 0 (as in `hdd 0:image`)
    acsi_order 3 native      # ACSI ID 3 (as in `acsi 3:image`)
    hdd_order 1 auto         # the default

`native` / `swapped` describe how the FILE is stored; the rules above then
apply. ACSI overrides need the image pinned to that ID (`acsi 3:image`).

## Checked against (29 Oct 2026, dkimages/)

| Image | Detected |
|---|---|
| 1GB, 1GBIDE, 1GBnew, apple-boot-acsi, Satan256Mb (all 3), apj-os-*, badapple, disktmp*, idetest, falcon*, falc512*, pistorm-jit-hd, freemint/mint-master, mymint, mymint-1, tuxies-Mint | native |
| freemint/EasyMiNT_Master_Aranym, EasyMiNT_Slave_Aranym, card.img | byte-swapped (AHDI root) |
| apple-boot-ide | unknown (sector 0-2 blank) - used as is |
