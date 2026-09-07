#!/usr/bin/env bash
# perf-tlb.sh - is the JIT core paying for page-table walks?
#
# Samples the hardware counters on CPU 2 (the JIT thread's isolated core)
# for N seconds while the guest runs something CPU-bound (CoreMark, a
# GEM redraw loop, whatever you are tuning). Run it once with
# PISTORM_HUGEPAGE=0 in the environment of the emulator and once without,
# and compare the *_tlb_refill lines. A drop of an order of magnitude in
# l2d_tlb_refill is what 2 MB pages are supposed to buy; if cycles per
# instruction (CPI) does not move, TLB was not the bottleneck for that
# workload and the remaining cost is in the generated code itself.
#
#   sudo ./perf-tlb.sh [seconds]      (default 10)
#
# Needs: sudo apt install linux-perf   (package name varies: linux-perf-*,
# or 'perf' on some images). If perf complains about the event names, list
# what the PMU offers with:  perf list | grep -i tlb
set -u
SECS="${1:-10}"
CORE=2
# The ARM PMU shows up as armv8_pmuv3_0 on the Pi 4 kernel and as
# armv8_cortex_a72 / armv8_cortex_a53 on others; pick whatever is there.
PMU=$(ls /sys/bus/event_source/devices 2>/dev/null | grep -E '^armv8' | head -1)
PMU="${PMU:-armv8_pmuv3_0}"
EV="cycles,instructions"
for e in l1d_tlb_refill l1i_tlb_refill l2d_tlb_refill l1d_cache_refill l1i_cache_refill l2d_cache_refill; do
  EV="$EV,$PMU/$e/"
done
if ! pidof emulator >/dev/null; then
  echo "emulator is not running" >&2; exit 1
fi
echo "== THP state"
cat /sys/kernel/mm/transparent_hugepage/enabled
cat /sys/kernel/mm/transparent_hugepage/defrag
echo "== huge-page coverage of the emulator (AnonHugePages / hugetlb)"
grep -E "AnonHugePages|^Hugetlb" /proc/$(pidof emulator)/status 2>/dev/null || true
awk '/AnonHugePages/ {s+=$2} END {print "AnonHugePages total: " s " kB"}' /proc/$(pidof emulator)/smaps
echo "== ${SECS}s of PMU counters on CPU ${CORE}"
perf stat -e "$EV" -C "$CORE" -- sleep "$SECS" 2>&1 | sed -n '/Performance counter/,$p'
