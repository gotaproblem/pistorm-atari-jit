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

## What the guest sees (automatic, per image - nothing to set)

| Sector 0 of the file | IDE | ACSI |
|---|---|---|
| table/boot code native, or nothing to judge | native order (TOS, HDDRIVER, AHDI; EmuTOS copes either way) | as stored |
| everything readable only word-swapped (ARAnyM, twisted-cable dumps) | swapped back per sector - a normal disk | swapped back per sector |
| table native, 68000 boot code word-swapped (PP-prepared, e.g. 1GBIDE.img) | the twisted-cable (legacy) port order, which its boot driver needs | as stored |

Falcon TOS always gets the native IDE order. The file on disk is never
rewritten in another order. This replaces the old cfg `ide_byteorder` (a
global IDE port order): it is ignored with a log line, and the setup page no
longer offers it.

## Overrides (cfg only, for an image the detection gets wrong)

    hdd_order 0 swapped      # IDE slot 0: file stored word-swapped
    hdd_order 1 bootswap     # IDE slot 1: PP-prepared (twisted-cable order)
    acsi_order 3 native      # ACSI ID 3 (image pinned with acsi 3:image)
    hdd_order 2 auto         # the default

## Checked against (29 Oct 2026, dkimages/)

| Image | Detected |
|---|---|
| 1GBIDE | PP-prepared (table native, boot code swapped) |
| 1GB, 1GBnew, apple-boot-acsi, Satan256Mb (all 3), apj-os-*, badapple, disktmp*, idetest, falcon*, falc512*, pistorm-jit-hd, freemint/mint-master, mymint, mymint-1, tuxies-Mint | native |
| freemint/EasyMiNT_Master_Aranym, EasyMiNT_Slave_Aranym, card.img | byte-swapped (AHDI root) |
| apple-boot-ide | unknown (sector 0-2 blank) - used as is |
