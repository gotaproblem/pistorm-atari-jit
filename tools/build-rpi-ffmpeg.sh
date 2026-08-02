#!/bin/bash
# build-rpi-ffmpeg.sh - build an FFmpeg with V4L2 Request API hardware decoding,
# so the Pi 4's stateless HEVC block (rpivid, /dev/video19) can be used.
#
# WHY: the distro's FFmpeg has no `v4l2request` hwaccel, so H.265 decodes
# entirely in software - about 3 fps for 4K on two A72 cores. The silicon can do
# 4Kp60; it just cannot be reached from stock libavcodec. These out-of-tree
# patches add the hwaccels, and the pinned commit already carries Broadcom
# SAND128 support, which is the tiled format rpivid actually outputs.
#
# TARGET: Raspberry Pi OS / Debian 13 (trixie), arm64.
#
# THE SONAME TRAP. Trixie ships FFmpeg 7.1.3 and this builds FFmpeg 7.1.3, so
# every library here has exactly the same soname as the distro's:
#     libavcodec.so.61  libavformat.so.61  libavutil.so.59
#     libswscale.so.8   libswresample.so.5
# There is nothing in the filename to tell them apart, and the distro one works
# perfectly except that the hwaccel is missing - which is the most annoying
# possible failure, because everything runs and nothing is fast. Two rules
# follow, and this script enforces both:
#   1. NEVER add $PREFIX/lib to the linker cache. Anything on the system that
#      picked these up by accident would silently replace its FFmpeg.
#   2. Bake an rpath into anything that links against them, so the choice is
#      recorded in the binary rather than left to search order. Confirm with
#      `ldd`, never by assuming.
#
# WHAT IT DOES NOT DO: touch your system FFmpeg. Everything installs under
# /opt/rpi-ffmpeg and nothing is added to the linker cache, so apt's ffmpeg,
# VLC and everything else keep using the distro libraries. To undo the whole
# thing: sudo rm -rf /opt/rpi-ffmpeg.
#
# Run it on the Pi:
#     bash tools/build-rpi-ffmpeg.sh
#
# Expect 40-90 minutes on a Pi 4. It leaves one core free so the machine stays
# usable. Then rebuild the emulator normally - the Makefile picks the new
# libraries up automatically if they are there.
#
# To produce a redistributable tarball instead of installing system-wide:
#     PREFIX=/tmp/stage bash tools/build-rpi-ffmpeg.sh
#     bash tools/package-rpi-ffmpeg.sh /tmp/stage

set -euo pipefail

PREFIX=${PREFIX:-/opt/rpi-ffmpeg}
SRC=${SRC:-$HOME/src/rpi-ffmpeg}
JOBS=${JOBS:-$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))}

# PINNED, not a branch name. A moving branch means this script and any tarball
# built from it stop describing the same code the moment upstream pushes, and
# the LGPL obligation is specifically to be able to hand someone THE source
# that produced THESE binaries. Override REF to try something newer.
REPO=${REPO:-https://github.com/Kwiboo/FFmpeg.git}
REF=${REF:-fad85d9c76611c09b561167ab405c667a0dcdeb7}
BRANCH=${BRANCH:-v4l2-request-n7.1.3}   # only used to fetch the ref cheaply

echo "=== rpi-ffmpeg build ==========================================="
echo "  repo   : $REPO"
echo "  commit : $REF"
echo "  source : $SRC"
echo "  prefix : $PREFIX   (isolated; system FFmpeg untouched)"
echo "  jobs   : $JOBS"
echo

# ---- 0. is this the machine we think it is? ----------------------------------
# Not fatal - people build in containers and chroots - but a mismatch is worth
# knowing about before an hour of compiling, because the result is only usable
# on the release it was built against.
if [ -r /etc/os-release ]; then
    # In a subshell: /etc/os-release defines NAME, VERSION and friends, and
    # sourcing it at top level would quietly overwrite same-named variables.
    OS_CODENAME=$( . /etc/os-release 2>/dev/null; printf '%s' "${VERSION_CODENAME:-}" )
    if [ "${OS_CODENAME:-}" != "trixie" ]; then
        echo "note: this is '${OS_CODENAME:-unknown}', not trixie."
        echo "      The build will work, but the libraries link against THIS"
        echo "      system's glibc and will not run on a different release."
    fi
fi
if [ "$(uname -m)" != "aarch64" ]; then
    echo "note: architecture is $(uname -m), not aarch64."
fi

# ---- 1. the kernel side must be live -----------------------------------------
if [ ! -e /dev/video19 ]; then
    echo "!! /dev/video19 is missing - the rpivid HEVC decoder is not enabled."
    echo "   Add this to /boot/firmware/config.txt and reboot:"
    echo "       dtoverlay=rpivid-v4l2"
    exit 1
fi
if command -v v4l2-ctl >/dev/null 2>&1; then
    if ! v4l2-ctl -d /dev/video19 --list-formats-out 2>/dev/null | grep -q S265; then
        echo "!! /dev/video19 does not advertise S265 (HEVC slice data)."
        echo "   Hardware HEVC will not work; aborting before a long build."
        exit 1
    fi
    echo "ok: /dev/video19 advertises S265 (HEVC parsed slice data)"
fi

# ---- 2. build dependencies ---------------------------------------------------
# libdrm + libudev are what the v4l2_request code needs; the rest is the usual
# FFmpeg build set. Kernel headers must be recent enough to define
# V4L2_CID_STATELESS_HEVC_SPS - configure checks this and will tell you.
echo
echo "--- installing build dependencies ---"
sudo apt update
sudo apt install -y build-essential pkg-config git \
                    libdrm-dev libudev-dev \
                    v4l-utils

# ---- 3. source ---------------------------------------------------------------
# Fetch the branch shallowly, then check out the pinned commit and verify we
# landed on it. If upstream has moved the branch past our pin the fetch still
# succeeds but the checkout fails, which is the correct outcome: it means this
# script no longer matches the code it claims to build.
echo
if [ -d "$SRC/.git" ]; then
    echo "--- updating existing checkout ---"
    git -C "$SRC" fetch --depth 50 origin "$BRANCH"
else
    echo "--- cloning (shallow) ---"
    mkdir -p "$(dirname "$SRC")"
    git clone --depth 50 --branch "$BRANCH" "$REPO" "$SRC"
fi
cd "$SRC"
if ! git checkout -q --detach "$REF" 2>/dev/null; then
    echo "!! commit $REF is not in the fetched history."
    echo "   Upstream may have force-pushed or moved past the shallow depth."
    echo "   Retry with a full clone:  rm -rf $SRC && SRC=$SRC bash \$0"
    exit 1
fi
GOT=$(git rev-parse HEAD)
if [ "$GOT" != "$REF" ]; then
    echo "!! checked out $GOT, expected $REF"; exit 1
fi
echo "at: $(git log --oneline -1)"

# ---- 4. configure ------------------------------------------------------------
# Lean on purpose: we need demux + decode + scale + resample, nothing else.
# ffmpeg/ffprobe are kept because they are how you test that this worked.
#
# NOTE the absence of --enable-gpl and --enable-nonfree. That keeps the result
# LGPL-2.1-or-later, which is what makes it redistributable as shared libraries
# alongside this MIT-licensed emulator. Do not add them without understanding
# what it does to the licence of anything you ship.
echo
echo "--- configure ---"
./configure \
    --prefix="$PREFIX" \
    --enable-shared --disable-static \
    --disable-doc --disable-ffplay \
    --enable-v4l2-request --enable-libudev --enable-libdrm \
    --enable-v4l2-m2m \
    --disable-vaapi --disable-vdpau --disable-vulkan \
    --disable-debug \
    --extra-cflags="-mcpu=cortex-a72" \
    --extra-ldflags="-Wl,-rpath,$PREFIX/lib" \
    || { echo; echo "!! configure failed - check ffbuild/config.log"; exit 1; }

# Fail early and loudly rather than after an hour of compiling.
if ! grep -q "^CONFIG_HEVC_V4L2REQUEST_HWACCEL=yes" ffbuild/config.mak; then
    echo
    echo "!! configure did NOT enable the HEVC v4l2request hwaccel."
    echo "   Usually this means the kernel headers are too old:"
    echo "   V4L2_CID_STATELESS_HEVC_SPS must exist in linux/videodev2.h."
    echo "   Look for 'hevc_v4l2_request' in ffbuild/config.log."
    exit 1
fi
echo "ok: hevc_v4l2request hwaccel is enabled"

# ---- 5. build ----------------------------------------------------------------
echo
echo "--- building with $JOBS jobs (this is the slow part) ---"
make -j"$JOBS"

# Installing into a staging prefix under $HOME or /tmp does not need root; only
# a real system prefix does. Deciding by writability keeps the packaging path
# from prompting for a password it has no use for.
INSTALL_DIR=$(dirname "$PREFIX")
if [ -w "$INSTALL_DIR" ] || [ -w "$PREFIX" ]; then
    make install
else
    sudo make install
fi

# Deliberately NO ldconfig entry for $PREFIX - see THE SONAME TRAP at the top.
# That is also why --extra-ldflags bakes an rpath into the installed binaries:
# without it, $PREFIX/bin/ffmpeg starts fine but ld.so resolves libavcodec.so.61
# from /usr/lib by soname, so you end up silently exercising the DISTRO library
# and wondering why the new hwaccel is missing.

# ---- 6. prove it actually works ----------------------------------------------
echo
echo "--- verifying the installed binary uses ITS OWN libraries ---"
ldd "$PREFIX/bin/ffmpeg" | grep -E "libav(codec|util)" || true
if ldd "$PREFIX/bin/ffmpeg" | grep -q "/usr/lib.*libavcodec"; then
    echo "!! $PREFIX/bin/ffmpeg is loading the SYSTEM libavcodec, not ours."
    echo "   Run it with:  LD_LIBRARY_PATH=$PREFIX/lib $PREFIX/bin/ffmpeg ..."
fi
echo
echo "--- hwaccels reported by the new build ---"
LD_LIBRARY_PATH="$PREFIX/lib" "$PREFIX/bin/ffmpeg" -hide_banner -hwaccels || true
if LD_LIBRARY_PATH="$PREFIX/lib" "$PREFIX/bin/ffmpeg" -hide_banner -hwaccels 2>/dev/null \
        | grep -q v4l2request; then
    echo "ok: v4l2request is present"
else
    echo "!! v4l2request is NOT present - the build did not take effect"
fi

echo
echo "=== done ======================================================="
echo
echo "Benchmark - compare elapsed time against the file's duration."
echo "NOTE the LD_LIBRARY_PATH: without it you are testing the distro FFmpeg."
echo "NOTE -hwaccel_output_format drm_prime: the Pi's decoder emits Broadcom"
echo "     SAND-tiled frames which CANNOT be downloaded to linear CPU memory,"
echo "     so without this the CLI fails with 'Failed to transfer data'. The"
echo "     emulator never downloads them - it scans the dmabuf out directly."
echo "  time LD_LIBRARY_PATH=$PREFIX/lib $PREFIX/bin/ffmpeg -v error \\"
echo "       -hwaccel v4l2request -hwaccel_output_format drm_prime \\"
echo "       -i /home/pistorm/atari-share/media/YOURFILE.MP4 -an -f null -"
echo
echo "Then rebuild the emulator; the Makefile finds $PREFIX automatically."
echo "To make a redistributable tarball from this build:"
echo "  bash tools/package-rpi-ffmpeg.sh $PREFIX"
