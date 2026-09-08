#!/usr/bin/env bash
# perf-tlb.sh - is the JIT core paying for page-table walks?
#
# Samples the hardware counters on CPU 2 (the JIT thread's isolated core)
# for N seconds while the guest runs something CPU-bound (CoreMark, a
# GEM redraw loop, whatever you are tuning). Compare runs with and without
# 2 MB pages: l2d_tlb_refill should drop ~10x; if cycles/instruction does
# not move, TLB was not the bottleneck for that workload.
#
#   sudo ./perf-tlb.sh [seconds]      (default 10)
#
# Needs: sudo apt install linux-perf
set -u
SECS="${1:-10}"
CORE=2

if ! pidof emulator >/dev/null; then
  echo "emulator is not running" >&2; exit 1
fi

PERF=$(command -v perf || ls /usr/bin/perf_* 2>/dev/null | head -1)
if [ -z "$PERF" ]; then
  echo "perf not found: sudo apt install linux-perf" >&2; exit 1
fi

echo "== huge-page state"
if [ -d /sys/kernel/mm/transparent_hugepage ]; then
  echo "THP enabled: $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
  echo "THP defrag:  $(cat /sys/kernel/mm/transparent_hugepage/defrag)"
else
  echo "THP: not in this kernel (CONFIG_TRANSPARENT_HUGEPAGE off) - 4K pages only"
fi
awk '/AnonHugePages/ {s+=$2} END {print "emulator AnonHugePages: " s " kB"}' /proc/$(pidof emulator)/smaps

PMU=$(ls /sys/bus/event_source/devices 2>/dev/null | grep -E '^armv8' | head -1)
if [ -z "$PMU" ]; then
  echo "no armv8 PMU under /sys/bus/event_source/devices - hardware counters unavailable" >&2
  ls /sys/bus/event_source/devices >&2; exit 1
fi
echo "PMU: $PMU"

EV="cycles,instructions"
for e in l1d_tlb_refill l1i_tlb_refill l2d_tlb_refill l1d_cache_refill l1i_cache_refill l2d_cache_refill; do
  [ -e "/sys/bus/event_source/devices/$PMU/events/$e" ] && EV="$EV,$PMU/$e/" \
    || echo "  (event $e not offered by this PMU - skipped)"
done

echo "== ${SECS}s of PMU counters on CPU ${CORE}"
"$PERF" stat -e "$EV" -C "$CORE" -- sleep "$SECS"
