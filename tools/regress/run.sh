#!/bin/sh
# run.sh - run every test in tests.txt (or the names given) on this Pi and
# record PASS/FAIL against the commit that was built.
#
#   tools/regress/run.sh            all tests
#   tools/regress/run.sh sndh44     just one
#
# Run from the repo root after `make`, as the normal user - only the
# emulator itself is started with sudo, so no root-owned files end up in
# the tree. Needs the real board attached - this
# exercises the bus, the CPU core and the chips, which nothing else can.
#
# Results: tools/regress/results/<commit>/<test>.log and a line per test
# appended to tools/regress/history.txt:
#   date  commit  test  PASS|FAIL  reason
# Exit status: 0 all passed, 1 something failed, 2 setup error.

# $here holds tests.txt, results/ and history.txt. It can be a copy outside
# the tree (bisect checks out commits that predate tools/regress), so the
# repo is wherever we are run from, not where this script lives.
here=$(cd "$(dirname "$0")" && pwd) || exit 2
root=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "run from inside the repo"; exit 2; }
cd "$root" || exit 2
[ -x ./emulator ] || { echo "no ./emulator - run make first"; exit 2; }

cfg_src=../configs/psctrl.cfg
[ -f "$cfg_src" ] || { echo "$cfg_src missing"; exit 2; }
commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
git diff --quiet HEAD -- . 2>/dev/null || commit="$commit+dirty"
out="$here/results/$commit"
mkdir -p "$out"

fails="$out/.fails"; : > "$fails"
grep -v '^[[:space:]]*#' "$here/tests.txt" | grep '|' |
while IFS='|' read -r name section image secs what; do
	name=$(echo $name); section=$(echo $section); image=$(echo $image); secs=$(echo $secs)
	if [ $# -gt 0 ]; then
		want=0; for n in "$@"; do [ "$n" = "$name" ] && want=1; done
		[ $want -eq 1 ] || continue
	fi
	log="$out/$name.log"
	if [ ! -f "$image" ]; then
		echo "$name: SKIP - $image not found"
		continue
	fi
	cfg="$out/$name.cfg"
	# the section body, verbatim, minus its hdd lines; then the test disk
	awk -v s="[$section]" '
		/^\[/ { on = ($0 == s); next }
		on && $1 != "hdd" { print }' "$cfg_src" > "$cfg"
	[ -s "$cfg" ] || { echo "$name: SKIP - no [$section] in $cfg_src"; continue; }
	echo "hdd $image" >> "$cfg"

	echo "$name: running ${secs}s on $commit ..."
	sudo timeout -s INT "$secs" ./emulator --config "$cfg" --no-setup > "$log" 2>&1
	rc=$?
	sleep 2                              # let the board settle between tests

	reason=""
	if grep -q 'Panic:' "$log"; then
		reason="guest panic: $(grep -m1 -A3 'Panic:' "$log" | tr -s ' \n' ' ' | cut -c1-120)"
	elif [ $rc -ne 124 ]; then
		reason="emulator exited early (rc=$rc)"
	fi
	if [ -z "$reason" ]; then
		verdict=PASS
	else
		verdict=FAIL
	fi
	line="$(date '+%F %T')  $commit  $name  $verdict  $reason"
	echo "$line" | tee -a "$here/history.txt"
	[ $verdict = PASS ] || { echo "$name FAILED - log: $log"; echo "$name" >> "$fails"; }
done

[ -s "$fails" ] && exit 1
exit 0
