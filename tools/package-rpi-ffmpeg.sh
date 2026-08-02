#!/bin/bash
# package-rpi-ffmpeg.sh - turn a finished build-rpi-ffmpeg.sh tree into a
# redistributable tarball, so nobody else has to spend 90 minutes compiling.
#
#     bash tools/package-rpi-ffmpeg.sh [/opt/rpi-ffmpeg]
#
# Produces, in the current directory:
#     pistorm-rpi-ffmpeg-7.1.3-deb13-arm64.tar.gz
#     pistorm-rpi-ffmpeg-7.1.3-deb13-arm64.tar.gz.sha256
#
# Unpacked next to the emulator as ./ffmpeg/, the Makefile finds it and links
# with an $ORIGIN-relative rpath, so the tree can be moved or copied to another
# Pi and still work. Nothing is installed system-wide and nothing needs root.
#
# WHY A TARBALL AND NOT A .deb: these libraries have the same sonames as the
# ones Trixie ships (both are FFmpeg 7.1.3), so the safest place for them is
# visibly next to the binary that uses them, not in a system directory where
# the wrong one can win by search order. A tarball also needs no packaging
# toolchain, no root, and uninstalls with rm -rf.
#
# WHY THE NAME SAYS deb13-arm64: these link against this machine's glibc,
# libdrm and libudev. On a different Debian release that is an ABI mismatch, so
# the filename has to make an inappropriate download obvious rather than
# letting it install and then misbehave.

set -euo pipefail

PREFIX=${1:-/opt/rpi-ffmpeg}
OUTDIR=${OUTDIR:-$PWD}
NAME=${NAME:-pistorm-rpi-ffmpeg}

# /etc/os-release is a shell fragment that defines NAME, VERSION, ID and
# friends. Sourcing it at top level therefore silently overwrites any variable
# of ours with the same name - NAME became "Debian GNU/Linux", which contains a
# SLASH, so tar was handed a path through a directory that does not exist.
# Read it in a subshell and take only the one field wanted.
osr() { ( . /etc/os-release 2>/dev/null; eval "printf '%s' \"\${$1:-}\"" ); }

# The scripts must agree about which commit this is; read it from the builder
# rather than repeating it here, so there is one place to change.
HERE=$(cd "$(dirname "$0")" && pwd)
REF=$(sed -n 's/^REF=${REF:-\([0-9a-f]\{40\}\)}.*/\1/p' "$HERE/build-rpi-ffmpeg.sh" | head -1)
REPO=$(sed -n 's|^REPO=${REPO:-\(.*\)}$|\1|p' "$HERE/build-rpi-ffmpeg.sh" | head -1)
[ -n "$REF" ] || { echo "!! could not read the pinned commit from build-rpi-ffmpeg.sh"; exit 1; }

# The UPSTREAM version, taken from the branch name (v4l2-request-n7.1.3), which
# is the only place it is stated unambiguously. Asking the built binary does
# not work: for a detached git checkout `ffmpeg -version` reports the commit
# describe string, so the third field came out as "fad85d9" rather than
# "7.1.3". get-rpi-ffmpeg.sh reads the same line, so the name it looks for and
# the name produced here cannot drift apart.
FFMPEG_VER=${FFMPEG_VER:-$(sed -n 's/^BRANCH=.*-n\([0-9][0-9.]*\)}.*/\1/p' \
                          "$HERE/build-rpi-ffmpeg.sh" | head -1)}
[ -n "$FFMPEG_VER" ] || { echo "!! could not read the FFmpeg version from build-rpi-ffmpeg.sh"; exit 1; }

# ---- 1. sanity-check the tree we are about to ship ---------------------------
[ -d "$PREFIX/lib" ] || { echo "!! $PREFIX/lib does not exist. Build it first:"; \
                          echo "   bash tools/build-rpi-ffmpeg.sh"; exit 1; }

if [ ! -x "$PREFIX/bin/ffmpeg" ]; then
    echo "!! $PREFIX/bin/ffmpeg is missing - is this really a build prefix?"
    exit 1
fi

# Refuse to ship a build that does not have the one feature it exists for.
# Finding that out after publishing is much more expensive than finding it out
# here, and the failure downstream would look like a decode problem.
if ! LD_LIBRARY_PATH="$PREFIX/lib" "$PREFIX/bin/ffmpeg" -hide_banner -hwaccels 2>/dev/null \
        | grep -q v4l2request; then
    echo "!! this build does not report the v4l2request hwaccel."
    echo "   Packaging it would ship something no better than the distro's."
    exit 1
fi
echo "ok: v4l2request present in $PREFIX"

VER=$FFMPEG_VER
# What the binary calls itself, kept only for the SOURCE file - it is the git
# describe string on a detached checkout, which is informative but is not a
# version number and must never end up in a filename.
BUILDID=$("$PREFIX/bin/ffmpeg" -hide_banner -version 2>/dev/null | head -1 | awk '{print $3}')
BUILDID=${BUILDID:-unknown}

CODENAME=$(osr VERSION_CODENAME)
CODENAME=${CODENAME:-trixie}
case "$CODENAME" in
    trixie)   DEBVER=deb13 ;;
    bookworm) DEBVER=deb12 ;;
    *)        DEBVER=$CODENAME ;;
esac
ARCH=$(dpkg --print-architecture 2>/dev/null || uname -m)
STEM="$NAME-$VER-$DEBVER-$ARCH"

# Belt and braces after the above: a filename is about to be built from four
# variables, any of which could pick up something unexpected from the
# environment. Anything outside this set - a space, and above all a slash -
# turns the tar destination into a path, which is exactly how this failed
# before. Fail here, where the cause is obvious, rather than inside tar.
case "$STEM" in
    *[!A-Za-z0-9._-]*)
        echo "!! refusing to build a tarball named '$STEM'"
        echo "   name=$NAME ver=$VER debver=$DEBVER arch=$ARCH"
        echo "   One of those picked up an unexpected value."
        exit 1 ;;
esac

echo "--- packaging $STEM ---"
echo "    upstream $VER, build id $BUILDID, Debian $CODENAME $ARCH"

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
DEST="$STAGE/ffmpeg"
mkdir -p "$DEST/lib/pkgconfig" "$DEST/include"

# ---- 2. libraries ------------------------------------------------------------
# Copy the .so files and their soname/dev symlinks, preserving link structure.
cp -a "$PREFIX"/lib/lib{av,sw}*.so* "$DEST/lib/"
# Strip only real files, never the symlinks - stripping through a symlink
# replaces it with a regular file and breaks the soname chain.
find "$DEST/lib" -type f -name '*.so*' -exec strip --strip-unneeded {} + 2>/dev/null || true

# ---- 2a. REPOINT THE LIBRARIES AT EACH OTHER ---------------------------------
# These were built with -Wl,-rpath,$PREFIX/lib, so each one carries a DT_RUNPATH
# naming the directory it was built into. On the build machine that is right. In
# a tarball unpacked somewhere else it is a path that does not exist, and the
# consequence is not a clean failure.
#
# DT_RUNPATH is NOT inherited: when libavformat needs libavcodec, the loader
# uses libavformat's OWN runpath and ignores the executable's entirely. So the
# emulator's $ORIGIN/ffmpeg/lib resolves the five libraries it names directly,
# then libavformat goes looking for libavcodec.so.61 down a dead path, falls
# through to the system search, and finds the DISTRO one - same soname, no
# hwaccels, no error message. Exactly the failure this whole arrangement exists
# to prevent, arriving through the back door.
#
# (Verified rather than assumed: with a parent RUNPATH of $ORIGIN/sub and a
# library RUNPATH pointing nowhere, the dependency does not resolve. Setting the
# LIBRARY's runpath to $ORIGIN fixes it outright.)
if command -v patchelf >/dev/null 2>&1; then
    :
else
    echo "--- installing patchelf (needed to make the libraries relocatable) ---"
    sudo apt-get install -y patchelf || true
fi
if command -v patchelf >/dev/null 2>&1; then
    while IFS= read -r so; do
        patchelf --set-rpath '$ORIGIN' "$so"
    done < <(find "$DEST/lib" -type f -name '*.so*')
    echo "ok: library runpaths set to \$ORIGIN (they find each other wherever"
    echo "    the tarball is unpacked)"
else
    echo
    echo "!! patchelf is not available, so the shipped libraries still carry"
    echo "   RUNPATH=$PREFIX/lib. On any machine without that directory,"
    echo "   libavformat will resolve libavcodec from /usr/lib instead - the"
    echo "   distro build, with no hardware HEVC and no error. Do not publish"
    echo "   this tarball: install patchelf and re-run."
    echo
fi

# ---- 3. headers and pkg-config ----------------------------------------------
cp -a "$PREFIX"/include/. "$DEST/include/"

# Rewrite the .pc files to locate themselves. pkg-config expands ${pcfiledir}
# to the directory the .pc file is in, so prefix becomes wherever the tree was
# unpacked - which is the whole point of shipping it as a movable directory.
for pc in "$PREFIX"/lib/pkgconfig/*.pc; do
    [ -e "$pc" ] || continue
    sed -e 's|^prefix=.*|prefix=${pcfiledir}/../..|' \
        -e 's|^exec_prefix=.*|exec_prefix=${prefix}|' \
        -e 's|^libdir=.*|libdir=${prefix}/lib|' \
        -e 's|^includedir=.*|includedir=${prefix}/include|' \
        "$pc" > "$DEST/lib/pkgconfig/$(basename "$pc")"
done

# ---- 4. the paperwork the LGPL actually requires -----------------------------
# Shipping these as shared libraries is fine, but only if the corresponding
# source is obtainable and the licence travels with the binaries. A commit
# hash plus the exact script that consumed it is what makes that true.
LIC=""
for c in "${SRC:-}/COPYING.LGPLv2.1" \
         "$HOME/src/rpi-ffmpeg/COPYING.LGPLv2.1" \
         "$PREFIX/../src/rpi-ffmpeg/COPYING.LGPLv2.1" \
         "$PREFIX/share/doc/ffmpeg/COPYING.LGPLv2.1"; do
    [ -n "$c" ] && [ -r "$c" ] && { LIC=$c; break; }
done
if [ -n "$LIC" ]; then
    cp "$LIC" "$DEST/"
    echo "ok: licence text from $LIC"
else
    echo
    echo "!! COPYING.LGPLv2.1 was not found, so the tarball has no licence text."
    echo "   The tarball is still built, but do not PUBLISH it like this:"
    echo "   distributing LGPL shared libraries requires the licence to travel"
    echo "   with them. Copy it in from your FFmpeg checkout and re-run, or"
    echo "   point SRC at the checkout:  SRC=\$HOME/src/rpi-ffmpeg $0 $PREFIX"
    echo
fi
cp "$HERE/build-rpi-ffmpeg.sh" "$DEST/"

cat > "$DEST/SOURCE" <<EOF
These libraries are FFmpeg $VER, built from:

    repository : $REPO
    commit     : $REF
    build id   : $BUILDID   (what 'ffmpeg -version' reports)

with the configure line in the accompanying build-rpi-ffmpeg.sh, on Debian
$CODENAME $ARCH. No --enable-gpl and no --enable-nonfree, so the result is
LGPL-2.1-or-later; see COPYING.LGPLv2.1.

To reproduce exactly:
    REF=$REF bash build-rpi-ffmpeg.sh

These are NOT the FFmpeg libraries Debian ships. Debian $CODENAME ships the
same upstream version, $VER, so the sonames are identical - the difference is
that these have the V4L2 Request API hwaccels compiled in, which is what lets
the Pi 4's rpivid block decode HEVC in hardware.

Because the sonames collide, do not put this directory on the library search
path or in /etc/ld.so.conf.d. It is meant to be found through an rpath by the
one binary that wants it. Check with: ldd ./emulator | grep libav
EOF

cat > "$DEST/README" <<EOF
$STEM

Unpack this directory next to the emulator, so that ./ffmpeg/lib sits beside
the binary:

    cd pistorm-atari-jit
    tar xzf $STEM.tar.gz
    make

The Makefile finds ./ffmpeg automatically and links with an \$ORIGIN-relative
rpath, so this tree can be moved or copied wholesale and still resolve.

Confirm the emulator really picked these up - the distro libraries have the
same names, so this is the only way to be sure:

    ldd ./emulator | grep libav

Every line should point inside ./ffmpeg/lib. If any point at /usr/lib, the
rpath did not take and you are running the distro FFmpeg without the hwaccels.

This build is for Debian $CODENAME on $ARCH. On any other release the glibc it
links against will not match; build your own with build-rpi-ffmpeg.sh.

It also needs the kernel side, which no tarball can provide. Add to
/boot/firmware/config.txt and reboot:

    dtoverlay=rpivid-v4l2

Then /dev/video19 should exist and advertise S265.

See SOURCE for the exact upstream commit and the licence position.
EOF

# ---- 5. tar it up ------------------------------------------------------------
mkdir -p "$OUTDIR"
TARBALL="$OUTDIR/$STEM.tar.gz"
tar czf "$TARBALL" -C "$STAGE" ffmpeg
( cd "$OUTDIR" && sha256sum "$STEM.tar.gz" > "$STEM.tar.gz.sha256" )

echo
echo "=== done ======================================================="
echo "  $TARBALL"
echo "  $(du -h "$TARBALL" | cut -f1)"
echo "  $(cat "$OUTDIR/$STEM.tar.gz.sha256")"
echo
echo "Attach BOTH files to a GitHub release tagged ffmpeg-$VER."
echo "get-rpi-ffmpeg.sh derives that URL from the clone's own git origin, so"
echo "'make ffmpeg' will find them with nothing to configure. It refuses to"
echo "unpack if the .sha256 is missing, so upload that one too."
