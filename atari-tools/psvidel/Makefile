# PSVIDEL.PRG - Falcon Videl + SuperVidel XBIOS for the HDMI output
# (see README.md and ../../PSVIDEL.md). 68000, no libc, no relocations.

CROSS  ?= m68k-atari-mint-
CC      = $(CROSS)gcc

PSVIDEL.PRG: psvidel.S
	$(CC) -m68000 -nostdlib -nostartfiles -o $@ psvidel.S

# Same binary from plain binutils (Debian/Ubuntu binutils-m68k-linux-gnu),
# for a machine without the mint toolchain. This is how the committed
# PSVIDEL.PRG was made.
binutils: psvidel.S mkprg.py
	m68k-linux-gnu-as -m68000 --register-prefix-optional -o psvidel.o psvidel.S
	m68k-linux-gnu-objcopy -O binary -j .text psvidel.o psvidel.bin
	python3 mkprg.py psvidel.bin PSVIDEL.PRG
	rm -f psvidel.o psvidel.bin

clean:
	rm -f PSVIDEL.PRG psvidel.o psvidel.bin

.PHONY: clean binutils
