# pdfbench - host-only PDF render benchmark (phase 0 of the PDF viewer)
#
#   sudo apt install libpoppler-glib-dev libcairo2-dev pkg-config
#   make
#   taskset -c 1 ./pdfbench /path/to/*.pdf

CC      ?= cc
PKGS     = poppler-glib cairo
CFLAGS  += -O2 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

pdfbench: pdfbench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f pdfbench

.PHONY: clean
