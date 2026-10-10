# psdos - the DOS service for the PiSTorm Atari (psdos-design.md)
#
# psdos is a small libretro front end; the DOS machine is the DOSBox Pure
# core, built separately (install-full.sh DOS=1 does both):
#
#   make                 psdos
#   make core            fetch DOSBox Pure 1.0-preview6 and build
#                        dosbox_pure_libretro.so next to psdos (~30 min on
#                        the Pi's two Linux cores)
#   ./psdos --selftest   run the core headless for 5 s and print the fps
#
# GPL-2.0-or-later (it loads a GPL core). libretro.h is MIT.

CFLAGS  ?= -O2 -g -Wall -Wextra
LDLIBS  += -ldl

DBP_URL ?= https://github.com/schellingb/dosbox-pure
DBP_TAG ?= 1.0-preview6
DBP_DIR ?= dosbox-pure-$(DBP_TAG)
JOBS    ?= 2

psdos: psdos.c libretro.h ../platforms/atari/dos/psdos_proto.h
	$(CC) $(CFLAGS) -o $@ psdos.c $(LDLIBS)

core: dosbox_pure_libretro.so

dosbox_pure_libretro.so:
	[ -d $(DBP_DIR) ] || git clone --depth 1 -b $(DBP_TAG) $(DBP_URL) $(DBP_DIR)
	$(MAKE) -C $(DBP_DIR) -j$(JOBS)
	cp $(DBP_DIR)/dosbox_pure_libretro.so $@

clean:
	rm -f psdos psdos-selftest.ppm

distclean: clean
	rm -rf dosbox_pure_libretro.so dosbox-pure-*

.PHONY: core clean distclean
