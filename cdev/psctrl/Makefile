# Build PSCHK.TTP with the FreeMiNT cross-compiler
CROSS   = m68k-atari-mint-
CC      = $(CROSS)gcc
CFLAGS  = -m68000 -O2 -fomit-frame-pointer -Wall

PSCHK.TTP: pschk.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f PSCHK.TTP

.PHONY: clean
