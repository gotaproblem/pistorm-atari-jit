# PSHOSTFS.PRG - HOSTFS drives for plain TOS / EmuTOS (see README.md).
# 68000, no libc, no relocations.

CROSS  ?= m68k-atari-mint-
CC      = $(CROSS)gcc

PSHOSTFS.PRG: hostdos.S
	$(CC) -m68000 -nostdlib -nostartfiles -o $@ hostdos.S

# Same binary from plain binutils (Debian/Ubuntu binutils-m68k-linux-gnu).
# This is how the committed PSHOSTFS.PRG was made.
binutils: hostdos.S mkprg.py
	m68k-linux-gnu-as -m68000 --register-prefix-optional -o hostdos.o hostdos.S
	m68k-linux-gnu-objcopy -O binary -j .text hostdos.o hostdos.bin
	python3 mkprg.py hostdos.bin PSHOSTFS.PRG
	rm -f hostdos.o hostdos.bin

clean:
	rm -f PSHOSTFS.PRG hostdos.o hostdos.bin

.PHONY: clean binutils
