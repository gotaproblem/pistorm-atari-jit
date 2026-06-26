#
# atari-pistorm — Amiberry AArch64 JIT edition
#
#   make            -> Pi 4 (cortex-a72), 64-bit
#   make PIMODEL=PI3 -> Pi 3 (cortex-a53), 64-bit
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
         platforms/atari/idedriver.c \
         platforms/atari/fdd/atari_fdd.c \
         platforms/atari/fdd/platform_atari_fdd.c \
         platforms/atari/audio/dmasnd_hdmi.c \
         platforms/atari/audio/dmasnd_capture.c

# -----------------------------------------------------------------
# C++ source.
#  - emulator.c is compiled as C++ (the JIT/UAE headers require it)
#  - jit/compemu_support.cpp #includes codegen_arm64.cpp,
#    compemu_midfunc_arm64*.cpp and ../compemu_prefs.cpp — do NOT list those.
#  - jit/compemu.cpp / compstbl.cpp / compemu_fpp.cpp ARE separate TUs.
# -----------------------------------------------------------------
PISTORM_CPP = emulator.c \
              platforms/atari/et4000/et4000.c \
              platforms/atari/et4000/pcem/vid_svga.c \
              platforms/atari/et4000/pcem/vid_svga_render.c \
              platforms/atari/et4000/pcem/vid_et4000.c \
              platforms/atari/et4000/pcem/vid_unk_ramdac.c \
              platforms/atari/et4000/pcem/pcem_shim.c \
              platforms/atari/et4000/pcem/et4000_engine.c \
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

# SDL2 (ET4000 display backend). sdl2-config ships with libsdl2-dev.
SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LIBS   = $(shell sdl2-config --libs)

ifeq ($(PIMODEL),PI3)
	PIOPTS = -mcpu=cortex-a53
	PI     = -DPI3
else
	PIOPTS = -mcpu=cortex-a72 -march=armv8-a+crc+simd
	PI     = -DPI4
endif

# Feature/target defines all live in sysconfig.h (UAE, JIT, USE_JIT, AMIBERRY,
# CPU_AARCH64, CPU_64_BIT, WITH_SOFTFLOAT, the CPUEMU_* set, ...). Every C++ TU
# includes sysconfig.h first, so we do NOT repeat them here (doing so triggers
# redefinition warnings). Only _GNU_SOURCE, which sysconfig.h doesn't set.
DEFS = -D_GNU_SOURCE 

# Include order is critical: our dir (.) FIRST so our sysconfig.h / sysdeps.h
# win over Amiberry's. -Ithreaddep makes our pthread thread.h win over the SDL
# one (newcpu.cpp includes both "thread.h" and "threaddep/thread.h", so our copy
# lives at ./threaddep/thread.h and -I. + -Ithreaddep cover both spellings).
# ./include resolves uae/*, newcpu.h, memory.h, options.h; . resolves cputbl.h
# and machdep/maccess.h; -Isoftfloat for the FPU sub-library headers.
INCLUDES = -I. -Ithreaddep -Iinclude -Isoftfloat -Ijit -I/usr/include/libdrm -Ipcem

# Optimization is a separate variable so the big generated files can be built
# lean. -O level affects only emulation speed, not correctness or the aarch64
# code the JIT emits, so the huge cpuemu_*/JIT units are compiled at HEAVY_OPT
# with no debug info to keep peak RAM down on a 4 GB Pi.
#   Tune from the command line, e.g.:  make HEAVY_OPT=-O0   (least RAM/fastest build)
#                                      make HEAVY_OPT=-O2   (fastest emulator, most RAM)
OPT       ?= -O2
HEAVY_OPT ?= -O3 #-fomit-frame-pointer -flto

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
              $(TARGET) ataritest

# -----------------------------------------------------------------
# Rules
# -----------------------------------------------------------------
all: $(TARGET) ataritest

$(TARGET): $(COBJS) $(CPPOBJS)
	$(CXX) -o $@ $^ $(CXXFLAGS) -lpthread -lm -ldl -l:libdrm.a $(SDL_LIBS) -lz -lasound

# emulator.c built as C++
emulator.o: emulator.c
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# et4000.c is built as C (generic %.o:%.c rule); add the SDL2 include path.
platforms/atari/et4000/et4000.o: platforms/atari/et4000/et4000.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -MMD -MP -c -o $@ $<

ataritest: ataritest.c gpio/ps_protocol.c
	$(CC) $^ -o $@ $(CFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -f $(DELETEFILES)

-include $(COBJS:%.o=%.d) $(CPPOBJS:%.o=%.d)