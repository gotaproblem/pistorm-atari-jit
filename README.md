
# PiSTorm ATARI JIT
To be honest, I never thought this would happen, but after many frustrated years, I have finally got performance out of the pistorm project - Too little, too late? Time will tell.

What started out as a little project, turned in to a major pain-in-the-arse, and then became a labour-of-love! There were many occasions when I'd had enough and walked away from it for a couple of months, but in the end all of that has got me to this milestone.

I hope this drums up intereset and some bright people can continue developing it.
Join the Atari PiSTorm Discord channel ```https://discord.gg/KEtg2QfxQE```

There is a lot of potential with performance. 
- [ ] Direct access needs developing; with that will come huge performance increases. 

Then add-ons can be developed, such as 
- [x] WiFi networking [implemented]
- [x] DMA Sound [implemented]
- [x] FDD Emulation [implemented]
- [ ] Additional SVGA Cards
## Requirements
This is not for newbies, a good amount of linux development knowledge is needed.
At some point I will include binaries for those technically challanged ;)

### Hardware
Development has been done on a Raspberry Pi4 with 2GB and various ATARI platforms - STfm, STe, MST 

In theory, this should work on Pi3A+ and PiZero2W, but I haven't tried. ***Update Pi3A+ and Pi02w only have 512MB RAM. As things are, this build will not work on either of these parts. I may get around to looking in to it, but cannot say when.***
You will need either a PiSTorm board (Amiga A500) for DIL 68pin CPUs (STfm, MST) or a custom designed STe board available online or from Discord pistorm atari channel Users.
### Software
Development used Raspberry Pi OS Lite - Trixie build 64bit AARCH64. This is mandatory for JIT.
#### Building
For those interested in looking at the source, building and developing, fill your boots...
The JIT engine is a clone of the amiberry project, chosen because it was developed for ARM 64bit and included hardware FPU. I did look at aranym; got the JIT interpreter running, but could not get the compiler running. However, I am using NatFeats from Aranym.

##### Preperation
Before starting the process in earnest, make sure your O/S is current.
>sudo apt update
sudo apt upgrade

As is always the case, you must install a bunch of packages and libraries to build the binaries.
>sudo apt install build-essential git libsdl2-dev libzstd-dev libcurl4-openssl-dev libdrm-dev libasound2-dev ffmpeg

##### Clone
Everything is ready to clone this repository
>cd ~
>git clone https://github.com/gotaproblem/pistorm-atari-jit.git

This creates the **pistorm-atari-jit/** directory.

##### Build
To build, simply make. Be warned first build from clean will take over 20 minutes!
>make clean
make PIMODEL=PI4

PIMODEL isn't actually needed for PI4, but it's included for clarity if and when PI3 builds start
## Configuring
Run the install.sh script to build the file tree and to copy files in to place
>bash install.sh
Reboot at this point

## Running
>cd pistorm-atari-jit

### Step 1
A good starting point for your confidence is to make sure the PiSTorm hardware is talking between the Pi and Atari
>sudo ./ataritest --reset
sudo ./ataritest --memory tests=rw

If the tests fail then there is no point in continuing until the hardware issues are resolved.

### Step 2
You should take some time to read the default emulator config file - **atari.cfg**. There is also a **master.cfg** which you can use as a template to create additional configurations. The files are annotated to help you on your way, it should be much simpler now.

### Step 3
So the tests pass and you have edited your configuration file, run the emulator Note: *you must have **atari.cfg** in your configs/ directory*
>sudo ./emulator

You are not restricted to using just the **atari.cfg** config file. You can create how ever many you desire; in which case, supply the emulator command with --config *\<your-cfg-name>*

And that is pretty much it... Over to you to enjoy

## TODO
### configuration switches

## Known Issues
~~68000, 68010 are not supported~~
~~GEM based games can not be played, they will likely run, but you will not see a meaningful native Atari screen output~~
Bus Arbitration is not working so Blitter, FDD, external ACSI bus devices will not work at the moment.
The high performance in a Mint environemnt may result in keyboard beeping - press the INSERT key to stop it