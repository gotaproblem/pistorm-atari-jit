
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
- [x] Host MP3 playback via NatFeats (mixed with ST sound, SDL3) [implemented]
- [x] A/V screen recording (hardware H.264 + sound, see capmux.sh) [implemented]
- [x] Host VIDEO playback via NatFeats - MP4/MKV/AVI, hardware H.264, own DRM overlay plane (see VIDEO.md) [implemented]
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
>sudo apt install build-essential g++ make pkg-config cmake git libsdl3-dev libmpg123-dev libjpeg-dev libdrm-dev libslirp-dev zlib1g-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev libpoppler-glib-dev libcairo2-dev poppler-data fonts-urw-base35 libasound2-dev ffmpeg cifs-utils

*(**install-full.sh** installs exactly this list for you. **libzstd-dev** and **libcurl4-openssl-dev** used to be listed here and are not used by anything - dropped. **cifs-utils** is for mounting a media share from a NAS or PC; see INSTALL-README.md.)*

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

##### Optional - hardware HEVC for video playback
Everything plays out of the box: H.264 in hardware, the rest in software. Hardware **H.265/HEVC** additionally needs the rpivid decoder enabled in the kernel, and an FFmpeg carrying the V4L2 Request API hwaccels, which Debian does not ship.
>echo 'dtoverlay=rpivid-v4l2' | sudo tee -a /boot/firmware/config.txt

Reboot, then
>make ffmpeg
make

**make ffmpeg** fetches a prebuilt tarball into **ffmpeg/** if one has been published for your Debian release and architecture, and otherwise builds it from source - 40 to 90 minutes on a Pi 4. Nothing is installed system-wide and nothing is added to the linker cache: trixie ships the same FFmpeg *version*, so the library filenames are identical and only an rpath keeps the two apart. To see which one you have:
>make ffmpeg-status
ldd ./emulator | grep libav

See **VIDEO.md** for the full story, including how to publish a build for other people.

## Configuring
Run the installer to build the file tree and copy files into place (see **INSTALL-README.md**)
>./install-full.sh

Reboot at this point

## Running
>cd pistorm-atari-jit

### Step 1
A good starting point for your confidence is to make sure the PiSTorm hardware is talking between the Pi and Atari
>sudo ./ataritest --reset
sudo ./ataritest --memory tests=rw

If the tests fail then there is no point in continuing until the hardware issues are resolved.

### Step 2
The configuration is one file, **../configs/psctrl.cfg** (the installer puts it there from `configs/psctrl.cfg.default`). It holds a `[psctrl]` block and one `[section]` per **build** - a build is a complete machine. Two come ready: `[gem]` (a plain TOS/EmuTOS desktop on the ST video) and `[apj-os]` (FreeMiNT + XaAES + fVDI on HDMI). The file is annotated.

You do not have to edit it by hand: the **setup page** does that (Step 3), and shows every key the emulator accepts with its choices and ranges.

### Step 3
So the tests pass, run the emulator through its launcher (it asks for sudo itself and relaunches on a Restart from the taskbar)
>sh run-pistorm.sh

The **setup page** comes up first, on the ST monitor and on HDMI: a list of the builds, with a countdown on the one that booted last. Any key stops the countdown. `Enter` boots the build under the cursor, `E` opens it in the editor (seven tabs - Machine, Video, Sound, Input, Drives, Network, Tuning - every key a tick box), `N` makes a new build (empty, or a copy of another), `D` deletes one, `S` saves, `F12` (Help on the ST keyboard) dumps the screen to `../screendumps/`. Nothing is written until you save or boot.

To skip the page and boot the last build straight away: `sh run-pistorm.sh --no-setup`, or set `countdown 0` in the `[psctrl]` block. A flat single-machine .cfg (the old **atari.cfg** style) still works with `--config`, and has no page.

And that is pretty much it... Over to you to enjoy

## TODO
### configuration switches

### Regression test package (run automatically on every new branch)
One command that answers "did I break anything?" before hardware time
is spent. Pieces that already exist and want gathering: the acsi_test
57-check harness, ataritest's memory/acsi suites, the host-side .hfs
wrapper unit test, gcc/g++ -fsyntax-only sweeps of the heavy TUs. Add:
a boot-smoke matrix over the cfg axes that keep biting (cpu 68000/030/
040 x jit on/off x mmu on/off x machine st/ste - boot EmuTOS to
desktop, assert hz200 advancing and no EXCRING fault-class entries),
and an interrupt-storm soak (scripted mouse-motion flood + a Timer A
replay workload) since storms found the last three bugs. Wire as a
make target (make check) plus a git hook or branch script so it runs
without being remembered.

### Unified virtual interrupt controller (design debt)
Virtual interrupt sources have grown organically and each carries its
own copy of the same machinery: kbd_usb keeps IERB/IMRB/ISRB/VR
shadows and its own gating (added after the Petra/Paula stack-overflow
corruption - the virtual channel ignored in-service semantics);
dmasnd_mfp_snoop keeps a second, independent MFP shadow and its own
virtual IACK vector; mfp_note_eoi_write is a third EOI observer in
pistorm_natmem; intlev_ack in jit_glue hand-orders virtual sources
ahead of the real-bus IACK; and the FDD VBL tick rides inside the IPL
task. Consequences: per-source bugs in logic that should exist once
(the in-service fix landed in kbd_usb only - dmasnd's virtual channel
has the same class of hazard), MFP register shadows that can disagree,
and no single place that understands channel priority across real and
virtual sources.
Wanted: one virtual-MFP module owning a single register shadow
(IERA/B, IMRA/B, ISRA/B, VR fed from both write dispatchers), one
arbiter merging the real IPL pins with all virtual channels in MFP
priority order, one IACK resolver, and one host-side timed-event queue
for periodic ticks (FDD VBL, dmasnd frames, future timers) instead of
per-subsystem timing. Migrate kbd_usb and dmasnd onto it first; the
per-source shadows and special cases then delete.

### JIT + MMU via the Pi's own ARM MMU (idea)
The emulated MMU (`mmu enabled`) is interpreter-only: UAE's JIT emits
direct loads/stores with guest address == physical address baked in, so
per-access translation can't be interposed (a QEMU-style software TLB
would be a redesign of the JIT's memory layer). But those direct
accesses already pass through the Pi's ARM MMU on their way to natmem.
For coarse, mostly-static guest mappings - Basilisk II's remapped page
0 is the canonical case - the guest's translation could be mirrored
into host mappings with mmap/mremap aliasing: remap the host page
backing the guest page, JIT'd code lands in the right place at zero
per-access cost, invalid pages arrive as SIGSEGV (which the JIT already
catches) to be turned into guest bus errors. Constraints: guest and
host page size must match (both 4K on Pi4), only worth it for guests
that set up translation once and leave it alone (not MiNT per-process
memory protection churn), and the SIGSEGV -> precise 68040 bus-error
frame plumbing is the hard part. Would give Basilisk-class guests full
JIT speed WITH real translation.

## Known Issues
~~68000, 68010 are not supported~~
~~GEM based games can not be played, they will likely run, but you will not see a meaningful native Atari screen output~~
Bus Arbitration is not working so Blitter, FDD, external ACSI bus devices will not work at the moment.
The high performance in a Mint environemnt may result in keyboard beeping - press the INSERT key to stop it