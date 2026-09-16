# atariclean — the Mac litter, out of Atari drives and images

A Mac that copies onto the share leaves `._*` AppleDouble sidecars,
`.DS_Store`, `.smbdeleteXXXX` leftovers and the `.fseventsd` /
`.Spotlight-V100` / `.Trashes` / `.TemporaryItems` folders. They turn up
on the Atari's HOSTFS drive, inside floppy images the Mac mounted, and in
git trees on the share. Three pieces deal with it:

| Where | What | Does |
|---|---|---|
| Pi | `atariclean` (this directory; `/usr/local/bin` after `install-full.sh`) | Cleans directories, `.st` / `.msa` floppy images and AHDI/ICD hard-disk images (`.img`) in place. Dry run unless `-d`. |
| Pi | `atariclean.timer` | Runs `atariclean -d` over the PiSTorm tree nightly at 04:15 (`journalctl -u atariclean`). |
| Pi | Samba `[global]` (`install-full.sh` MACFIX step, `configs/smb.conf`) | `vfs_fruit` keeps Finder metadata in xattrs instead of `._` files; `.DS_Store` is refused. Reconnect the share on the Mac afterwards. |
| Atari | `PSCLEAN.PRG` (`apj-os-tools/psclean`) | Same rules from inside APJ-OS: pick a folder, it counts, asks, deletes. Works on C:, HOSTFS and a floppy alike. |

On the Mac, once: `defaults write com.apple.desktopservices DSDontWriteNetworkStores -bool true`
stops Finder writing `.DS_Store` on network volumes at all.

## atariclean

    atariclean [-d] [-b] [-v] PATH...

    atariclean ~/atari-share                 report
    atariclean -d ~/atari-share              remove
    atariclean -d ../dkimages/fdd/*.st       floppies, in place
    atariclean -d -b ../dkimages/apj-os.img  the hard disk, with a .bak first

Images: every matching directory entry (with its long-name entries) is
marked deleted and its cluster chain freed in every FAT copy — what a
DEL on the Atari does. Hard-disk partitions are read as 16-bit FAT
whatever their size, as TOS made them (dosfstools would guess FAT12 for a
small one; `--fat12`/`--fat16` override). MSA archives are decoded,
cleaned and re-encoded. Nothing is written unless every chain to be freed
walks cleanly first. `.smbdelete*` files younger than ten minutes are
left alone in directories: that is a Mac still deleting an open file.

`tests/run.py` builds a 720K floppy, its MSA twin, an AHDI disk (GEM,
BGM with 1024-byte sectors, an XGM chain of two) and a directory tree,
littered the macOS way, and checks the tool against them; the cleaned
volumes pass `fsck.vfat -n` and the MSA is read back by the emulator's
own decoder (`tests/msachk.c`).
