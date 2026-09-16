# webbench - phase 0(d): WPE off-screen frame path, measured.
#
#   sudo apt-get install -y build-essential pkg-config libglib2.0-dev \
#        libwayland-dev libwpe-1.0-dev libwpebackend-fdo-1.0-dev \
#        libwpewebkit-2.0-dev
#   make
#   ./webbench https://en.wikipedia.org/wiki/Atari_ST 60 .

WK := $(shell pkg-config --exists wpe-webkit-2.0 && echo wpe-webkit-2.0 || echo wpe-webkit-1.0)
PKGS := $(WK) wpebackend-fdo-1.0 wpe-1.0 wayland-server glib-2.0 gobject-2.0

CFLAGS  ?= -O2 -g -Wall -Wextra
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

webbench: webbench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f webbench *.ppm
.PHONY: clean
