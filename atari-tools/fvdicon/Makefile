# FVDICON.PRG - TOS console on the fVDI screen (see README.md).
# 68k Atari program, m68k-atari-mint cross toolchain. No libc.

CROSS  ?= m68k-atari-mint-
CC      = $(CROSS)gcc

FVDICON.PRG: fvdicon.S
	$(CC) -m68000 -nostdlib -nostartfiles -o $@ fvdicon.S

clean:
	rm -f FVDICON.PRG

.PHONY: clean
