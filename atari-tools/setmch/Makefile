# SETMCH.PRG - force the _MCH machine cookie (Bespoke / APJ-OS).
#
# 68k Atari program, built with the m68k-atari-mint cross toolchain
# (crossmint: tho-otto.de/crossmint.php) - NOT part of the emulator's
# ARM build. From this directory:
#
#     make            build SETMCH.PRG
#     make clean
#
# Then copy SETMCH.PRG into the Atari AUTO folder (see setmch.c header).

CROSS  ?= m68k-atari-mint-
CC      = $(CROSS)gcc
STRIP   = $(CROSS)strip

CFLAGS  = -O2 -Wall -fomit-frame-pointer

TARGET  = SETMCH.PRG

$(TARGET): setmch.c
	$(CC) $(CFLAGS) -o $@ setmch.c
	$(STRIP) $@

clean:
	rm -f $(TARGET)

.PHONY: clean
