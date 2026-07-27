#!/bin/sh
# capmux.sh - turn a finished PiStorm screen capture into a playable video.
#
# The recorder writes capture.h264 (hardware-encoded video) + capture.wav
# (mixed audio) plus a small "complete" marker into a capture directory.
# Run this afterwards to mux them into one .mkv (stream copy - instant,
# no re-encode, no quality loss).
#
# Usage:
#   ./capmux.sh <capture-dir> [output.mkv]
#   ./capmux.sh --latest                   newest capture under the base dir
#   ./capmux.sh --all                      every un-muxed capture in the base
#
#   CAPDIR=/some/where ./capmux.sh --latest  override base dir
#                                          (default: /home/pistorm/screendumps)
#   KEEP_RAW=1 ./capmux.sh <dir>           keep capture.h264/.wav after muxing

# default capture base: ../screendumps relative to this script (the runtime
# tree the installer builds next to the emulator directory); CAPDIR overrides.
BASE="${CAPDIR:-$(dirname "$0")/../screendumps}"

usage() {
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

command -v ffmpeg >/dev/null 2>&1 || {
    echo "capmux: ffmpeg not found on PATH" >&2
    exit 1
}

mux_one() {
    d="$1"
    out="$2"
    [ -d "$d" ] || { echo "capmux: no such directory: $d" >&2; return 1; }
    [ -f "$d/capture.wav" ]  || { echo "capmux: no capture.wav in $d"  >&2; return 1; }
    [ -f "$d/complete" ] || echo "capmux: warning: no 'complete' marker in $d" \
                                 "(recording may have been cut short)" >&2
    [ -n "$out" ] || out="$d/capture.mkv"
    if [ -f "$out" ]; then
        echo "capmux: $out already exists, skipping (delete it to re-mux)" >&2
        return 1
    fi

    fps=$(sed -n 's/^fps=//p' "$d/complete" 2>/dev/null)
    [ -n "$fps" ] || fps=25

    if [ -f "$d/capture.h264" ]; then
        # hardware-encoded capture: pure stream copy, instant
        ok=0
        ffmpeg -y -hide_banner -loglevel error \
               -framerate "$fps" -i "$d/capture.h264" \
               -i "$d/capture.wav" -c copy -f matroska "$out.tmp" && ok=1
        raw="$d/capture.h264"
    elif [ -f "$d/capture.mjpg" ]; then
        # MJPEG capture: encode with x264 now (offline)
        ok=0
        ffmpeg -y -hide_banner -loglevel error \
               -framerate "$fps" -i "$d/capture.mjpg" \
               -i "$d/capture.wav" \
               -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p \
               -c:a copy -f matroska "$out.tmp" && ok=1
        raw="$d/capture.mjpg"
    elif [ -f "$d/frame_0000.png" ]; then
        # PNG-frame capture: encode with x264 now (offline, takes a while)
        ok=0
        ffmpeg -y -hide_banner -loglevel error \
               -framerate "$fps" -i "$d/frame_%04d.png" \
               -i "$d/capture.wav" \
               -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p \
               -c:a copy -f matroska "$out.tmp" && ok=1
        raw="$d/frame_*.png"
    else
        echo "capmux: no capture.h264 or frame_0000.png in $d" >&2
        return 1
    fi

    if [ "$ok" = "1" ]; then
        mv "$out.tmp" "$out"
        echo "capmux: wrote $out"
        if [ "${KEEP_RAW:-0}" != "1" ]; then
            rm -f $raw "$d/capture.wav"
        fi
        return 0
    fi
    rm -f "$out.tmp"
    echo "capmux: mux failed for $d (raw files kept)" >&2
    return 1
}

case "$1" in
    "" | -h | --help)
        usage
        ;;
    --all)
        rc=0
        found=0
        for d in "$BASE"/capture*; do
            [ -d "$d" ] || continue
            [ -f "$d/capture.h264" ] || [ -f "$d/capture.mjpg" ] || \
                [ -f "$d/frame_0000.png" ] || continue
            found=1
            mux_one "$d" || rc=1
        done
        [ "$found" = "1" ] || echo "capmux: nothing to mux under $BASE" >&2
        exit $rc
        ;;
    --latest)
        d=$(ls -dt "$BASE"/capture* 2>/dev/null | head -1)
        [ -n "$d" ] || { echo "capmux: no captures under $BASE" >&2; exit 1; }
        mux_one "$d" "$2"
        ;;
    *)
        mux_one "$1" "$2"
        ;;
esac
