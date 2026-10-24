#!/bin/sh
# Build NetSurf (framebuffer front end, VNC surface) into $WS/inst - nothing
# is installed system-wide. Usage: nsbuild.sh [workspace]  (default ~/netsurf-eval)
set -e
WS=${1:-$HOME/netsurf-eval}
INST=$WS/inst
GH=https://github.com/netsurf-browser
LIBS="libwapcaplet libparserutils libhubbub libdom libcss libnsgif libnsbmp libnsutils libnspsl libnslog libsvgtiny libnsfb"
JOBS=$(nproc)
mkdir -p "$WS" && cd "$WS"
for r in buildsystem nsgenbind $LIBS netsurf; do
    [ -d $r ] || git clone -q --depth 1 $GH/$r
    printf '%-16s %s\n' $r "$(git -C $r log -1 --format='%h %cs')"
done
export PKG_CONFIG_PATH=$INST/lib/pkgconfig
export PATH=$INST/bin:$PATH
export LD_LIBRARY_PATH=$INST/lib
make -C buildsystem install PREFIX=$INST >/dev/null
make -C nsgenbind -j$JOBS install PREFIX=$INST >/dev/null
for r in $LIBS; do
    echo "building $r"
    make -C $r -j$JOBS install PREFIX=$INST >/dev/null
done
echo "building netsurf"
make -C netsurf -j$JOBS TARGET=framebuffer PREFIX=$INST NETSURF_FB_FRONTEND=vnc NETSURF_FB_FONTLIB=freetype >/dev/null
ls -l netsurf/nsfb
