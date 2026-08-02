#!/bin/bash
# get-rpi-ffmpeg.sh - put a V4L2-Request-capable FFmpeg into ./ffmpeg.
#
# This is what `make ffmpeg` runs. It tries, in order:
#   1. nothing, if ./ffmpeg is already there and looks right
#   2. the prebuilt tarball from the project's releases (seconds)
#   3. building from source with build-rpi-ffmpeg.sh (40-90 minutes)
#
# The download is verified against a SHA256 published alongside it. If the
# checksum file is missing the download is refused rather than trusted: these
# are shared libraries that the emulator will execute, and "probably the right
# file" is not a standard worth adopting for that.
#
# Override anything:
#   FFMPEG_TAG=ffmpeg-7.1.3  FFMPEG_URL=...  bash tools/get-rpi-ffmpeg.sh
#   FORCE_BUILD=1            bash tools/get-rpi-ffmpeg.sh   (skip the download)

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
DEST="$ROOT/ffmpeg"

# ---- 0. already have it? -----------------------------------------------------
if [ -e "$DEST/lib/pkgconfig/libavcodec.pc" ]; then
    echo "ffmpeg/ is already present. Remove it to start again:"
    echo "    rm -rf $DEST"
    exit 0
fi

# ---- 1. work out what we would be downloading --------------------------------
# /etc/os-release defines NAME, VERSION and friends, so sourcing it at top
# level overwrites same-named variables of ours. Read it in a subshell.
osr() { ( . /etc/os-release 2>/dev/null; eval "printf '%s' \"\${$1:-}\"" ); }

CODENAME=$(osr VERSION_CODENAME)
CODENAME=${CODENAME:-trixie}
case "$CODENAME" in
    trixie)   DEBVER=deb13 ;;
    bookworm) DEBVER=deb12 ;;
    *)        DEBVER=$CODENAME ;;
esac
ARCH=$(dpkg --print-architecture 2>/dev/null || uname -m)
# Read the version from the same line package-rpi-ffmpeg.sh reads it from, so
# the name looked for here and the name produced there cannot drift apart.
FFMPEG_VER=${FFMPEG_VER:-$(sed -n 's/^BRANCH=.*-n\([0-9][0-9.]*\)}.*/\1/p' \
                          "$HERE/build-rpi-ffmpeg.sh" | head -1)}
FFMPEG_VER=${FFMPEG_VER:-7.1.3}
STEM="pistorm-rpi-ffmpeg-$FFMPEG_VER-$DEBVER-$ARCH"

# Derive the release URL from whatever this clone's origin is, so a fork does
# not silently pull the upstream author's binaries.
FFMPEG_TAG=${FFMPEG_TAG:-ffmpeg-$FFMPEG_VER}
if [ -z "${FFMPEG_URL:-}" ]; then
    ORIGIN=$(git -C "$ROOT" remote get-url origin 2>/dev/null || echo "")
    ORIGIN=${ORIGIN%.git}
    ORIGIN=${ORIGIN/git@github.com:/https://github.com/}
    if [ -n "$ORIGIN" ]; then
        FFMPEG_URL="$ORIGIN/releases/download/$FFMPEG_TAG/$STEM.tar.gz"
    fi
fi

build_from_source() {
    echo
    echo "--- building from source instead (40-90 minutes on a Pi 4) ---"
    STAGE="$ROOT/.ffmpeg-build"
    rm -rf "$STAGE"
    PREFIX="$STAGE" bash "$HERE/build-rpi-ffmpeg.sh"
    mkdir -p "$DEST"
    cp -a "$STAGE"/. "$DEST/"
    rm -rf "$STAGE"
    # The libraries were built with an rpath naming the staging directory,
    # which has just been deleted. That matters more than it looks: DT_RUNPATH
    # is not inherited, so the emulator's own $ORIGIN rpath does NOT cover
    # libavformat's search for libavcodec. Left alone, that search would fall
    # through to /usr/lib and quietly load the distro build - same soname, no
    # hwaccels, no error. Point each library at its own directory instead.
    if command -v patchelf >/dev/null 2>&1 || sudo apt-get install -y patchelf; then
        find "$DEST/lib" -type f -name '*.so*' \
            -exec patchelf --set-rpath '$ORIGIN' {} \;
    else
        echo "!! patchelf unavailable - ffmpeg/lib libraries still point at a"
        echo "   deleted build directory. Install patchelf and re-run."
    fi
    echo
    echo "ok: ffmpeg/ built from source"
}

if [ "${FORCE_BUILD:-0}" = "1" ] || [ -z "${FFMPEG_URL:-}" ]; then
    build_from_source
    exit 0
fi

# ---- 2. try the prebuilt tarball ---------------------------------------------
echo "--- looking for a prebuilt $STEM ---"
echo "    $FFMPEG_URL"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if ! curl -fsSL --retry 2 -o "$TMP/$STEM.tar.gz" "$FFMPEG_URL"; then
    echo "note: no prebuilt tarball at that URL."
    echo "      That is expected if no release has been published for this"
    echo "      distro and architecture yet ($DEBVER $ARCH)."
    build_from_source
    exit 0
fi

if ! curl -fsSL --retry 2 -o "$TMP/$STEM.tar.gz.sha256" "$FFMPEG_URL.sha256"; then
    echo "!! the tarball downloaded but its .sha256 did not."
    echo "   Refusing to unpack unverified shared libraries. Either publish the"
    echo "   checksum next to the tarball, or build from source:"
    echo "       FORCE_BUILD=1 make ffmpeg"
    exit 1
fi

echo "--- verifying checksum ---"
( cd "$TMP" && sha256sum -c "$STEM.tar.gz.sha256" ) || {
    echo "!! checksum mismatch - not unpacking."
    exit 1
}

tar xzf "$TMP/$STEM.tar.gz" -C "$ROOT"
[ -e "$DEST/lib/pkgconfig/libavcodec.pc" ] || {
    echo "!! the tarball did not contain ffmpeg/lib/pkgconfig - wrong layout?"
    exit 1
}
echo
echo "ok: ffmpeg/ unpacked from the prebuilt release"
echo
echo "Now run 'make'. Afterwards confirm the emulator really uses it -"
echo "the distro libraries have identical names, so this is the only proof:"
echo "    ldd ./emulator | grep libav"
