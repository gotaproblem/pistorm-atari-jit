#
# atari-pistorm — Amiberry AArch64 JIT edition
#
#   make                    -> Pi 4 (cortex-a72), 64-bit
#   make PIMODEL=PI3        -> Pi 3 (cortex-a53), 64-bit
#   make PIMODEL=PI02W      -> Pi Zero 2 W (cortex-a53), 64-bit
#
# REQUIRES a 64-bit Raspberry Pi OS + aarch64 g++. The Amiberry JIT backend we
# use is AArch64-only (R_MEMSTART = x27, N_REGS = 18, 64-bit natmem_offset).
#
# Layout assumed (see the copy-in list):
#   ./                      sysconfig.h sysdeps.h jit_glue.cpp pistorm_natmem.cpp
#                           pistorm_stubs.cpp emulator.c cputbl.h
#   ./threaddep/thread.h    our pthread thread.h (NOT amiberry's SDL one)
#   ./include/              <- amiberry/src/include/   (DELETE its sysdeps.h)
#   ./machdep/              <- amiberry/src/machdep/
#   ./softfloat/            <- amiberry/src/softfloat/  (.h + the 3 .cpp below)
#   ./cpu/                  <- amiberry CPU-core .cpp (list below, incl. cpummu*)
#   ./jit/                  Amiberry jit tree (already in your tree)
#   ./gpio/ ./platforms/ ./config_file/   your existing pistorm drivers
#

EXENAME = emulator

# -----------------------------------------------------------------
# Pure C source — your existing pistorm platform drivers (unchanged)
# -----------------------------------------------------------------
CFILES = config_file/config_file.c \
         gpio/ps_protocol.c \
         platforms/atari/IDE.c \
         platforms/atari/machine_cookie.c \
         platforms/atari/idedriver.c \
         platforms/atari/fdd/atari_fdd.c \
         platforms/atari/fdd/platform_atari_fdd.c \
         platforms/atari/network/pistorm_net.c \
         platforms/atari/network/pistorm_net_tap.c \
         platforms/atari/network/pistorm_net_slirp.c \
         platforms/atari/network/platform_atari_network.c \
         platforms/atari/audio/dmasnd_hdmi.c \
         platforms/atari/audio/dmasnd_capture.c \
         platforms/atari/audio/emu2149.c \
         platforms/atari/audio/ym2149.c \
         platforms/atari/st_blitter.c \
         platforms/atari/avrecord.c \
         platforms/atari/video/vidplane.c \
         platforms/atari/video/vidplay.c \
         platforms/atari/kbd_usb.c

# -----------------------------------------------------------------
# Musashi 68000 core for the STBOX game sandbox (third_party/musashi).
# Three TUs + its own softfloat; m68kfpu.c is #included by m68kcpu.c,
# NOT a separate TU. Its softfloat is C so it cannot collide with the
# C++-mangled UAE softfloat. m68kops.c/h are pre-generated - see
# third_party/musashi/VENDOR-NOTES.
# -----------------------------------------------------------------
MUSASHI_C = third_party/musashi/m68kcpu.c \
            third_party/musashi/m68kops.c \
            third_party/musashi/m68kdasm.c \
            third_party/musashi/softfloat/softfloat.c
CFILES += $(MUSASHI_C)

# STBOX - the sandboxed ST that Musashi drives (game sandbox in a GEM window)
CFILES += platforms/atari/stbox/stbox.c \
          platforms/atari/stbox/stbox_host.c \
          platforms/atari/stbox/stbox_psg.c \
          platforms/atari/stbox/stbox_realfdc.c

# -----------------------------------------------------------------
# C++ source.
#  - emulator.c is compiled as C++ (the JIT/UAE headers require it)
#  - jit/compemu_support.cpp #includes codegen_arm64.cpp,
#    compemu_midfunc_arm64*.cpp and ../compemu_prefs.cpp — do NOT list those.
#  - jit/compemu.cpp / compstbl.cpp / compemu_fpp.cpp ARE separate TUs.
# -----------------------------------------------------------------
PISTORM_CPP = emulator.c \
              platforms/atari/et4000/et4000.c \
              platforms/atari/et4000/et4000_drm.c \
              platforms/atari/et4000/pcem/vid_svga.c \
              platforms/atari/et4000/pcem/vid_svga_render.c \
              platforms/atari/et4000/pcem/vid_et4000.c \
              platforms/atari/et4000/pcem/vid_unk_ramdac.c \
              platforms/atari/et4000/pcem/pcem_shim.c \
              platforms/atari/et4000/pcem/et4000_engine.c \
              platforms/atari/network/atari_natfeat.cpp \
              platforms/atari/psctrl/psctrl.cpp \
              platforms/atari/psimg/psimg.cpp \
              jit_glue.cpp \
              pistorm_natmem.cpp \
              pistorm_stubs.cpp

JIT_CPP = jit/compemu.cpp \
          jit/compstbl.cpp \
          jit/compemu_fpp.cpp \
          jit/compemu_support.cpp

CPU_CPP = cpu/newcpu.cpp \
          cpu/newcpu_common.cpp \
          cpu/readcpu.cpp \
          cpu/cpudefs.cpp \
          cpu/cpustbl.cpp \
          cpu/cpuemu_0.cpp \
          cpu/cpuemu_11.cpp \
          cpu/cpuemu_13.cpp \
          cpu/cpuemu_20.cpp \
          cpu/cpuemu_21.cpp \
          cpu/cpuemu_22.cpp \
          cpu/cpuemu_23.cpp \
          cpu/cpuemu_24.cpp \
          cpu/cpuemu_31.cpp \
          cpu/cpuemu_32.cpp \
          cpu/cpuemu_33.cpp \
          cpu/cpuemu_34.cpp \
          cpu/cpuemu_35.cpp \
          cpu/cpuemu_40.cpp \
          cpu/cpuemu_50.cpp \
          cpu/cpummu.cpp \
          cpu/cpummu30.cpp \
          cpu/fpp.cpp \
          cpu/fpp_native.cpp \
          cpu/fpp_softfloat.cpp \
          cpu/events.cpp

# SoftFloat sub-library (FPU). Needs -Isoftfloat (set in INCLUDES).
SOFTFLOAT_CPP = softfloat/softfloat.cpp \
                softfloat/softfloat_decimal.cpp \
                softfloat/softfloat_fpsp.cpp

CPPFILES = $(PISTORM_CPP) $(JIT_CPP) $(CPU_CPP) $(SOFTFLOAT_CPP)

# -----------------------------------------------------------------
# Toolchain (aarch64)
# -----------------------------------------------------------------
CC  = gcc
CXX = g++

# SDL3 (audio backend, dmasnd_hdmi.c only). pkg-config sdl3 ships with libsdl3-dev.
# The display no longer uses SDL (DRM/fbdev + no-op SDL2 shim), so no SDL2 here.
SDL3_CFLAGS = $(shell pkg-config sdl3 --cflags)
SDL3_LIBS   = $(shell pkg-config sdl3 --libs)
# FFmpeg libraries for host video playback (platforms/atari/video/vidplay.c).
# IN-PROCESS decode only - the emulator never spawns an ffmpeg child (see the
# comment at the top of avrecord.c for why).
AV_PKGS   = libavformat libavcodec libavutil libswscale libswresample
#
# WHICH FFmpeg, and why it is worth this much comment. Debian trixie ships
# FFmpeg 7.1.3 and the V4L2-Request build is also FFmpeg 7.1.3, so every soname
# is identical - libavcodec.so.61 either way. Nothing in the filename tells them
# apart, and the distro one works perfectly except that hardware HEVC is
# missing, which is the most annoying failure available: everything runs, and
# nothing is fast. So the choice gets recorded in the binary as an rpath rather
# than left to library search order. Verify with:
#     ldd ./emulator | grep libav
#
# Three places are searched, in this order:
#   ./ffmpeg          an unpacked release tarball, or the result of
#                     `make ffmpeg`. Linked with an $ORIGIN-relative rpath, so
#                     the tree can be moved or copied to another Pi and still
#                     resolve, and so sudo cannot interfere.
#   /opt/rpi-ffmpeg   a system-wide build from tools/build-rpi-ffmpeg.sh.
#   the distro        builds and runs, but software HEVC only.
#
FFMPEG_LOCAL  ?= $(CURDIR)/ffmpeg
FFMPEG_PREFIX ?= /opt/rpi-ffmpeg

ifneq ($(wildcard $(FFMPEG_LOCAL)/lib/pkgconfig/libavcodec.pc),)
  AV_PREFIX = $(FFMPEG_LOCAL)
  # $$ORIGIN survives Make (becoming $ORIGIN) and the single quotes stop the
  # shell touching it, so ld.so expands it at RUN time against the binary's own
  # directory. That is also what makes it work under sudo, which resets the
  # environment and would throw away LD_LIBRARY_PATH.
  AV_RPATH  = -Wl,-rpath,'$$ORIGIN/ffmpeg/lib'
  # Same thing without the quoting, purely so ffmpeg-status can print it:
  # echoing AV_RPATH itself would nest single quotes and the shell would eat
  # the very $ORIGIN we are trying to show.
  AV_RPATH_SHOW = $$ORIGIN/ffmpeg/lib (resolved against the binary at run time)
  $(info NOTE: linking against the V4L2-Request FFmpeg in ./ffmpeg)
else ifneq ($(wildcard $(FFMPEG_PREFIX)/lib/pkgconfig/libavcodec.pc),)
  AV_PREFIX = $(FFMPEG_PREFIX)
  AV_RPATH  = -Wl,-rpath,$(FFMPEG_PREFIX)/lib
  AV_RPATH_SHOW = $(FFMPEG_PREFIX)/lib
  $(info NOTE: linking against the V4L2-Request FFmpeg in $(FFMPEG_PREFIX))
else
  AV_PREFIX =
  AV_RPATH  =
  AV_RPATH_SHOW = (none - resolved by soname from the system paths)
  $(info NOTE: using the distro FFmpeg. H.265 will decode in SOFTWARE - run)
  $(info NOTE: 'make ffmpeg' to get the hardware HEVC path on a Pi 4.)
endif

AV_PKGCONFIG = PKG_CONFIG_PATH=$(if $(AV_PREFIX),$(AV_PREFIX)/lib/pkgconfig:)$$PKG_CONFIG_PATH pkg-config
AV_CFLAGS = $(shell $(AV_PKGCONFIG) --cflags $(AV_PKGS))
AV_LIBS   = $(shell $(AV_PKGCONFIG) --libs $(AV_PKGS)) $(AV_RPATH)
SLIRP_CFLAGS = $(shell pkg-config --cflags libslirp 2>/dev/null || pkg-config --cflags slirp 2>/dev/null)
SLIRP_LIBS   = $(shell pkg-config --libs libslirp 2>/dev/null || pkg-config --libs slirp 2>/dev/null)

# -----------------------------------------------------------------
# Dependency preflight - fail early with a clear message instead of
# pages of compiler/linker errors. Skipped for `make clean`.
# (libslirp stays optional: networking is disabled without it.)
# -----------------------------------------------------------------
# `make ffmpeg` is how you SATISFY the FFmpeg dependency, so refusing to run it
# because that dependency is missing would be a closed loop. ffmpeg-status has
# to work on a broken system too - that is when you need it most.
ifeq ($(filter clean ffmpeg ffmpeg-status,$(MAKECMDGOALS)),)

ifeq ($(shell command -v pkg-config 2>/dev/null),)
$(error pkg-config not found. Install the build tools first: sudo apt install build-essential pkg-config)
endif

MISSING_DEPS :=
ifeq ($(shell pkg-config --exists sdl3 && echo ok),)
MISSING_DEPS += libsdl3-dev
endif
ifeq ($(shell pkg-config --exists libmpg123 && echo ok),)
MISSING_DEPS += libmpg123-dev
endif
ifeq ($(shell pkg-config --exists libdrm && echo ok),)
MISSING_DEPS += libdrm-dev
endif
ifeq ($(shell pkg-config --exists zlib && echo ok),)
MISSING_DEPS += zlib1g-dev
endif
ifeq ($(shell pkg-config --exists libjpeg && echo ok),)
MISSING_DEPS += libjpeg-dev
endif
ifeq ($(shell $(AV_PKGCONFIG) --exists libavformat && echo ok),)
MISSING_DEPS += libavformat-dev
endif
ifeq ($(shell $(AV_PKGCONFIG) --exists libavcodec && echo ok),)
MISSING_DEPS += libavcodec-dev
endif
ifeq ($(shell pkg-config --exists libswscale && echo ok),)
MISSING_DEPS += libswscale-dev
endif
ifeq ($(shell pkg-config --exists libswresample && echo ok),)
MISSING_DEPS += libswresample-dev
endif
ifneq ($(strip $(MISSING_DEPS)),)
$(error Missing build dependencies: $(MISSING_DEPS)  ->  sudo apt install $(MISSING_DEPS))
endif

ifeq ($(strip $(SLIRP_LIBS)),)
$(info NOTE: libslirp-dev not found - building WITHOUT WiFi networking support)
endif

endif # not clean

PIMODEL ?= PI4
PIMODEL_CANON := $(shell printf '%s' '$(PIMODEL)' | tr '[:lower:]' '[:upper:]')

ifeq ($(filter $(PIMODEL_CANON),PI4 RPI4 RASPI4),$(PIMODEL_CANON))
	PIOPTS = -mcpu=cortex-a72 -march=armv8-a+crc+simd
	PI     = -DPI4
else ifeq ($(filter $(PIMODEL_CANON),PI3 RPI3 RASPI3),$(PIMODEL_CANON))
	PIOPTS = -mcpu=cortex-a53 -march=armv8-a+crc+simd
	PI     = -DPI3
else ifeq ($(filter $(PIMODEL_CANON),PI02W PI0W2 ZERO2W ZERO2 PI_ZERO2W PIZERO2W),$(PIMODEL_CANON))
	PIOPTS = -mcpu=cortex-a53 -march=armv8-a+crc+simd
	PI     = -DPI3 -DPI02W
else
$(error Unknown PIMODEL '$(PIMODEL)'; use PI4, PI3, or PI02W)
endif

# Feature/target defines all live in sysconfig.h (UAE, JIT, USE_JIT, AMIBERRY,
# CPU_AARCH64, CPU_64_BIT, WITH_SOFTFLOAT, the CPUEMU_* set, ...). Every C++ TU
# includes sysconfig.h first, so we do NOT repeat them here (doing so triggers
# redefinition warnings). Only _GNU_SOURCE, which sysconfig.h doesn't set.
DEFS = -D_GNU_SOURCE
ifneq ($(strip $(SLIRP_LIBS)),)
DEFS += -DHAVE_LIBSLIRP
endif

# Latency diagnostics (LAT/STALLW/JIT/FVDI instrumentation).
# Off by default; enable with:  make DIAG=1
ifeq ($(DIAG),1)
DEFS += -DATARI_LAT_DIAG
endif

# Focused storage diagnostics (FDC load trace + real-time IDE register
# access log), WITHOUT the LAT/STALL/IRQHIST/FVDI firehose. Enable with:
#   make IDEDBG=1
ifeq ($(IDEDBG),1)
DEFS += -DATARI_IDE_DIAG
endif

# Include order is critical: our dir (.) FIRST so our sysconfig.h / sysdeps.h
# win over Amiberry's. -Ithreaddep makes our pthread thread.h win over the SDL
# one (newcpu.cpp includes both "thread.h" and "threaddep/thread.h", so our copy
# lives at ./threaddep/thread.h and -I. + -Ithreaddep cover both spellings).
# ./include resolves uae/*, newcpu.h, memory.h, options.h; . resolves cputbl.h
# and machdep/maccess.h; -Isoftfloat for the FPU sub-library headers.
INCLUDES = -I. -Igpio -Ithreaddep -Iinclude -Isoftfloat -Ijit -I/usr/include/libdrm -Ipcem $(SLIRP_CFLAGS)

# Optimization is a separate variable so the big generated files can be built
# lean. -O level affects only emulation speed, not correctness or the aarch64
# code the JIT emits, so the huge cpuemu_*/JIT units are compiled at HEAVY_OPT
# with no debug info to keep peak RAM down on a 4 GB Pi.
#   Tune from the command line, e.g.:  make HEAVY_OPT=-O0   (least RAM/fastest build)
#                                      make HEAVY_OPT=-O2   (fastest emulator, most RAM)
OPT       ?= -O3 -g1 -fno-omit-frame-pointer
HEAVY_OPT ?= -O3 -g1 -fno-omit-frame-pointer

COMMON_FLAGS = $(PIOPTS) $(OPT) -rdynamic -pthread $(PI) $(DEFS) $(INCLUDES) \
               -fno-strict-aliasing

CFLAGS   = $(COMMON_FLAGS)


# NOTE: do NOT add -fno-exceptions. newcpu.cpp's MMU bus-error path uses C++
# try/catch (the TRY/CATCH macros in mmu_common.h wrap m68k_exception), so
# exceptions must stay enabled (g++ default).
CXXFLAGS = $(COMMON_FLAGS) -std=gnu++17

TARGET  = $(EXENAME)
COBJS   = $(CFILES:%.c=%.o)
#OBJS    += $(patsubst %.S,%.o,$(wildcard platforms/atari/et4000/*.S))
CPPOBJS = $(CPPFILES:%.cpp=%.o)
CPPOBJS := $(CPPOBJS:%.c=%.o)

# The memory-hungry units: the 1-4 MB generated cpuemu_*/newcpu/fpp/cpummu and
# the JIT opcode-table TUs. Build these lean (HEAVY_OPT, -g0) so a single
# compile doesn't blow past available RAM and start swapping. The smaller
# pistorm glue, softfloat and C drivers stay at the default -O2 -g.
HEAVY_OBJS = $(CPU_CPP:.cpp=.o) $(JIT_CPP:.cpp=.o)
$(HEAVY_OBJS): OPT := $(HEAVY_OPT)

DELETEFILES = $(COBJS) $(CPPOBJS) $(COBJS:%.o=%.d) $(CPPOBJS:%.o=%.d) \
              $(TARGET) ataritest .ffmpeg-choice

# -----------------------------------------------------------------
# Rules
# -----------------------------------------------------------------
all: $(TARGET) ataritest

# WHY THE STAMP FILE. Which FFmpeg gets linked is decided by an rpath, and an
# rpath is baked in at LINK time - but nothing in the object files changes when
# you unpack ./ffmpeg or remove it. So `make` after either would say there was
# nothing to do, leave the old rpath in place, and the emulator would carry on
# resolving libraries from wherever it was told LAST time. Move that directory
# away afterwards and it falls back to the distro build: software HEVC, about
# 3 fps for 4K, and no error anywhere to explain it.
#
# The stamp records which FFmpeg was chosen and is only rewritten when that
# answer changes, so the relink happens exactly when it needs to and never
# otherwise. Note the explicit object list below rather than $^ - the stamp is
# a prerequisite and must not be handed to the linker.
AV_STAMP := .ffmpeg-choice
.PHONY: FORCE
FORCE:
$(AV_STAMP): FORCE
	@echo '$(AV_PREFIX) | $(AV_RPATH_SHOW)' | cmp -s - $@ 2>/dev/null || { \
	   echo '$(AV_PREFIX) | $(AV_RPATH_SHOW)' > $@; \
	   echo "NOTE: FFmpeg choice changed - the emulator will be relinked"; }

$(TARGET): $(COBJS) $(CPPOBJS) $(AV_STAMP)
	$(CXX) -o $@ $(COBJS) $(CPPOBJS) $(CXXFLAGS) -lpthread -lm -ldl -l:libdrm.a $(SLIRP_LIBS) -lz $(SDL3_LIBS) -lmpg123 -ljpeg $(AV_LIBS)
	@ldd $@ 2>/dev/null | grep -qE '/usr/lib.*libavcodec' && { \
	   echo; \
	   echo "WARNING: this build resolves libavcodec from /usr/lib - the DISTRO"; \
	   echo "         FFmpeg. H.265 will decode in software (~3 fps for 4K)."; \
	   echo "         Check with: make ffmpeg-status"; \
	   echo; } || true

# emulator.c built as C++
emulator.o: emulator.c
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# et4000.c is built as C. It uses the DRM/fbdev backends plus the no-op SDL2
# shim (et4000_sdl_stub.h) - no libSDL2, no SDL2 include path needed.
platforms/atari/et4000/et4000.o: platforms/atari/et4000/et4000.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# dmasnd_hdmi.c is the SDL3 audio backend: real SDL3 header (-DPISTORM_REAL_SDL3
# makes include/SDL3/SDL.h forward to the system header) + sdl3 pkg-config cflags.
platforms/atari/audio/dmasnd_hdmi.o: platforms/atari/audio/dmasnd_hdmi.c
	$(CC) $(CFLAGS) -DPISTORM_REAL_SDL3 $(SDL3_CFLAGS) -MMD -MP -c -o $@ $<

# ym2149.c binds a third stream to the same SDL3 device (emu2149 core is
# plain C with no SDL dependency and uses the default %.o rule).
platforms/atari/audio/ym2149.o: platforms/atari/audio/ym2149.c
	$(CC) $(CFLAGS) -DPISTORM_REAL_SDL3 $(SDL3_CFLAGS) -MMD -MP -c -o $@ $<

# Host video player: SDL3 (audio stream on the shared device) + FFmpeg libs.
platforms/atari/video/vidplay.o: platforms/atari/video/vidplay.c
	$(CC) $(CFLAGS) -DPISTORM_REAL_SDL3 $(SDL3_CFLAGS) $(AV_CFLAGS) -MMD -MP -c -o $@ $<

# The sandbox PSG binds a stream to the same SDL3 device as ym2149.c.
platforms/atari/stbox/stbox_psg.o: platforms/atari/stbox/stbox_psg.c
	$(CC) $(CFLAGS) -DPISTORM_REAL_SDL3 $(SDL3_CFLAGS) -MMD -MP -c -o $@ $<

# The video overlay plane only needs libdrm (already on the include path).
platforms/atari/video/vidplane.o: platforms/atari/video/vidplane.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

ataritest: ataritest.c gpio/ps_protocol.c
	$(CC) $^ -o $@ $(CFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -f $(DELETEFILES)

# Fetch (or, failing that, build) the V4L2-Request FFmpeg into ./ffmpeg, which
# is what gives the Pi 4 hardware HEVC. Deliberately NOT a dependency of `all`:
# it reaches the network and can take an hour, and the emulator builds fine
# without it. Deliberately not `.PHONY` either - the directory IS the target,
# so a second `make ffmpeg` costs nothing.
ffmpeg:
	@bash tools/get-rpi-ffmpeg.sh

# Say which FFmpeg a build would actually use, without building anything.
# Single quotes throughout: AV_RPATH contains a literal $ORIGIN that the shell
# must not expand, here or in the link line.
ffmpeg-status:
	@echo 'prefix : $(if $(AV_PREFIX),$(AV_PREFIX),(distro FFmpeg))'
	@echo 'rpath  : $(AV_RPATH_SHOW)'
	@for m in $(AV_PKGS); do \
	    v=`$(AV_PKGCONFIG) --modversion $$m 2>/dev/null` || v='not found'; \
	    echo "  $$m $$v"; \
	 done
	@if [ -x ./emulator ]; then \
	    echo 'linked :'; ldd ./emulator | grep -E 'libav|libsw' || true; \
	 else echo 'linked : (no emulator binary yet - run make)'; fi

.PHONY: all clean ffmpeg-status

-include $(COBJS:%.o=%.d) $(CPPOBJS:%.o=%.d)
