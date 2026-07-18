#!/bin/bash
#
# PiSTorm ATARI JIT
# installation helper script
#
# cryptodad Jul 2026
# v1.0
#

# backup system files
sudo cp /boot/firmware/config.txt /boot/firmware/config-bak.txt
sudo cp /boot/firmware/cmdline.txt /boot/firmware/cmdline-bak.txt

# create pistorm file structure
cd ~
mkdir roms/
mkdir configs/
mkdir atari-share/
mkdir dkimages/
mkdir dkimages/fdd/
mkdir screendumps/

# copy files
cd pistorm-atari-jit
sudo cp configs/config.txt /boot/firmware/
sudo cat configs/cmdline.txt >> /boot/firmware/cmdline.txt
cp configs/atari.cfg ../configs/
cp configs/master.cfg ../configs/
cp configs/emutos-aranym.rom ../roms/
cp configs/720k.st ../dkimages/fdd/

# create samba 
sudo cp configs/smb.conf /etc/samba/