# psweb - the web engine service for the PiSTorm Atari (psweb-design.md)
#
#   sudo apt-get install -y build-essential pkg-config libglib2.0-dev \
#        libwayland-dev libwpe-1.0-dev libwpebackend-fdo-1.0-dev \
#        libwpewebkit-2.0-dev
#   make
#   PSWEB_DEBUG=1 ./psweb          # then start the emulator, or WEBCHK

WK := $(shell pkg-config --exists wpe-webkit-2.0 && echo wpe-webkit-2.0 || echo wpe-webkit-1.0)
PKGS := $(WK) wpebackend-fdo-1.0 wpe-1.0 wayland-server glib-2.0 gobject-2.0 gio-2.0 gio-unix-2.0

CFLAGS  ?= -O2 -g -Wall -Wextra
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lrt

psweb: psweb.c ../platforms/atari/web/psweb_proto.h
	$(CC) $(CFLAGS) -o $@ psweb.c $(LDLIBS)

clean:
	rm -f psweb
.PHONY: clean
