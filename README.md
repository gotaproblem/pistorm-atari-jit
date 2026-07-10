
# PiSTorm ATARI JIT
To be honest, I never thought this would happen, but after many frustrated years, I have finally got performance out of the pistorm project - Too little, too late? Time will tell.

What started out as a little project, turned in to a major pain-in-the-arse, and then became a labour-of-love! There were many occasions when I'd had enough and walked away from it for a couple of months, but in the end all of that has got me to this milestone.

I hope this drums up intereset and some bright people can continue developing it.
Join the Atari PiSTorm Discord channel ```https://discord.gg/KEtg2QfxQE```

There is a lot of potential with performance. 
- [ ] Direct access needs developing; with that will come huge performance increases. 

Then add-ons can be developed, such as 
- [x] WiFi networking
- [x] DMA Sound
- [x] FDD Emulation
- [ ] Additional SVGA Cards
## Requirements
This is not for newbies, a good amount of linux development knowledge is needed

### Hardware
Development has been done on a Raspberry Pi4 with 2GB. 

In theory, this should work on Pi3A+ and PiZero2W, but I haven't tried.
You need a PiSTorm board (Amiga A500) for DIL 68pin CPUs or a custom designed STe board available online or from Discord pistorm atari channel Users.
### Software
Development used Raspberry Pi OS Lite - Trixie build 64bit AARCH64
#### Building
For those interested in looking at the source, building and developing, fill your boots...
The JIT engine is a clone of the amiberry project, chosen because it was developed for ARM 64bit and included hardware FPU. I did look at aranym; got the JIT interpreter running, but could not get the compiler running. However I have used NatFeats from Aranym.

As is always the case, you must install a bunch of packages and libraries to build the binaries.
>sudo apt install build-essential git libsdl2-dev libzstd-dev libcurl4-openssl-dev libdrm-dev libasound2-dev


To build, simply make. Be warned first build from clean will take over 20 minutes!
>make clean
make

## Configuring
You should have your home directory looking like this
```
pistorm-atari-jit/
    ataritest
    emulator
configs/
    atari.cfg
    master.cfg
dkimages/
    drive0.img
roms/
    etos512uk-14.rom
screendumps/
```
Populate the above directories with the relevent files before running the emulator. 

There are two linux system files that will need ammending before you run the emulator for the first time.
In the **pistorm-atari-jit/configs/** directory you will find **config.txt**.
This file must be copied to **/boot/firmware/**
>cd pistorm-atari-jit
sudo cp configs/config.txt /boot/firmware/

The next file, also located in **pistorm-atari-jit/configs/** is **cmdline.txt**
**NOTE YOU MUST NOT delete or overwrite the file in /boot/firmware/** 
Make a backup of this system file before editing.
>cd /boot/firmware
sudo cp cmdline.txt cmdline-ori.txt

Open **pistorm-atari-jit/configs/cmdline.txt** and copy the contents. Then open for editing **/boot/firmware/cmdline.txt**
**NOTE If you accidently delete or overwrite parts of this line, you will not be able to boot your OS.**

Append the contents of pistorms cmdline.txt to the **end of the line** (not a new line - it must be one long continuous line) in /boot/firmware/cmdline.txt

With these two system files modified, you must reboot

## Running
You must take the time to read the default emulator config file - **atari.cfg**
There is also a **master.cfg** which you should use as a template to create additional configurations. The files are annotated to help you on your way, it should be much simpler now.

>cd pistorm-atari-jit

A good starting point for your confidence is to make sure the PiSTorm hardware is talking between the Pi and Atari
>sudo ./ataritest --reset
sudo ./ataritest --memory tests=rw

If the tests fail then there is no point in contiuing until the hardware issues are resolved.

So the tests pass, run the emulator Note: *you must have **atari.cfg** in your configs/ directory*
>sudo ./emulator

And that is pretty much it... Over to you to enjoy

## Current limitations
68000, 68010 are not supported
GEM based games can not be played, they will likely run, but you will not see a meaningful native Atari screen output