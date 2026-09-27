# FVDIMODE.ACC / FVDIMODE.PRG - pick the fVDI (aranym.sys) screen mode.
#
# 68k Atari program, built with the m68k-atari-mint cross toolchain and
# gemlib (crossmint: tho-otto.de/crossmint.php) - NOT part of the
# emulator's ARM build. From this directory:
#
#     make            build FVDIMODE.ACC and FVDIMODE.PRG (same binary -
#                     mintlib's startup tells an accessory from a program)
#     make clean
#
# Copy FVDIMODE.ACC to the boot drive root (accessory), or run
# FVDIMODE.PRG from the desktop.

CROSS  ?= m68k-atari-mint-
CC      = $(CROSS)gcc
STRIP   = $(CROSS)strip

CFLAGS  = -O2 -Wall -Wextra -fomit-frame-pointer
LDLIBS  = -lgem

all: FVDIMODE.ACC FVDIMODE.PRG

FVDIMODE.PRG: fvdimode.c
	$(CC) $(CFLAGS) -o $@ fvdimode.c $(LDLIBS)
	$(STRIP) $@

FVDIMODE.ACC: FVDIMODE.PRG
	cp FVDIMODE.PRG $@

clean:
	rm -f FVDIMODE.PRG FVDIMODE.ACC

.PHONY: all clean
