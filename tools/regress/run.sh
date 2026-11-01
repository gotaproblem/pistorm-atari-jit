#!/bin/bash
# run.sh - the regression run: fire and forget.
#
#   tools/regress/run.sh                 everything (suite.txt, then tests.txt)
#   tools/regress/run.sh core-206 sndh44 just these (names from either file)
#   nohup tools/regress/run.sh >/dev/null 2>&1 &     and walk away
#
# Run from inside the repo after `make`, as the normal user: only the
# emulator itself is started with sudo. Needs the real board attached - this
# exercises the bus, the CPU core and the chips, which nothing else can.
#
# suite.txt  boots built here from the guest programs (tools/regress/guest):
#            every check reports PASS/FAIL itself, plus measurements
# tests.txt  boots from your own disk images; pass = no crash/panic
#
# Results: tools/regress/results/<commit>/
#   summary.txt      every check, what changed since the last run, metrics
#   <name>.log       the emulator's whole console for that boot
#   <name>.cfg       the config it ran with
#   <name>/          its floppy, disk images and the guest's report
# and one line per check appended to tools/regress/history.txt,
# one line per measurement to tools/regress/metrics.txt:
#   date  commit  boot  check  PASS|FAIL  detail
#   date  commit  boot  metric  value
# Exit status: 0 all passed, 1 something failed, 2 setup error.

here=$(cd "$(dirname "$0")" && pwd) || exit 2
root=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "run from inside the repo"; exit 2; }
cd "$root" || exit 2
[ -x ./emulator ] || { echo "no ./emulator - run make first"; exit 2; }
command -v python3 >/dev/null || { echo "python3 is needed (mkimg.py)"; exit 2; }

cfg_src=../configs/psctrl.cfg
[ -f "$cfg_src" ] || { echo "$cfg_src missing"; exit 2; }
commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
git diff --quiet HEAD -- . ':!tools/regress' 2>/dev/null || commit="$commit+dirty"
out="$here/results/$commit"
mkdir -p "$out"
summary="$out/summary.txt"
stamp=$(date '+%F %T')
touch "$here/history.txt" "$here/metrics.txt"
: > "$out/.checks"                       # boot|check|verdict|detail, this run
: > "$out/.metrics"
names=("$@")

want() {                                 # is boot $1 one of the names asked for?
	[ ${#names[@]} -eq 0 ] && return 0
	local n
	for n in "${names[@]}"; do [ "$n" = "$1" ] && return 0; done
	return 1
}

# [psctrl]'s path lines (rom_path / disk_path / fdd_path), ~ expanded: the
# emulator runs under sudo, and bare file names are resolved against them
psctrl_paths() {
	awk -v home="$HOME" '
		/^\[/ { on = ($0 == "[psctrl]"); next }
		on && ($1 == "rom_path" || $1 == "disk_path" || $1 == "fdd_path") {
			sub(/^~/, home, $2); print $1, $2 }' "$cfg_src"
}

# crash markers in an emulator log -> a reason, or nothing
crash_reason() {
	local log=$1
	if grep -q 'Panic:' "$log"; then
		echo "guest panic: $(grep -m1 -A3 'Panic:' "$log" | tr -s ' \n' ' ' | cut -c1-120)"
	elif grep -q '^\[SIGSEGV\]' "$log"; then
		echo "host crash: $(grep -m1 '^\[SIGSEGV\]' "$log" | cut -c1-100)"
	elif grep -q 'CPU halted' "$log"; then
		echo "guest halted: $(grep -m1 'CPU halted' "$log" | cut -c1-100)"
	elif grep -q 'instruction fetch from undecoded' "$log"; then
		echo "guest ran away: $(grep -m1 'instruction fetch from undecoded' "$log" | cut -c1-100)"
	fi
}

# record: boot check verdict detail
record() {
	echo "$1|$2|$3|$4" >> "$out/.checks"
	printf '%s  %s  %s  %s  %s  %s\n' "$stamp" "$commit" "$1" "$2" "$3" "$4" >> "$here/history.txt"
}

# run the emulator on cfg $1 for at most $2 s, console to $3; with $4 = 1,
# stop it as soon as the guest says @END. Returns 0 when stopped after
# @END, else the emulator's exit status (124 = the time ran out).
run_emulator() {
	local cfg=$1 secs=$2 log=$3 until_end=$4 envs=()
	# REGRESS_TIMELINE=1: IPL timeline with level 2 (HBL), one per boot,
	# read by tools/ipl-timeline/hbl.py into <boot>.hbl.txt (measurement
	# only: the logging is a store into a ring on the ipl_task core)
	[ "${REGRESS_TIMELINE:-0}" = 1 ] &&
		envs=(PISTORM_IPL_TIMELINE=2 "PISTORM_IPL_TIMELINE_LOG=${log%.log}.tl")
	sudo env "${envs[@]}" timeout -s INT "$secs" ./emulator --config "$cfg" --no-setup > "$log" 2>&1 &
	local pid=$! rc
	if [ "$until_end" = 1 ]; then
		while kill -0 $pid 2>/dev/null; do
			sleep 2
			if grep -q '\[NF_STDERR\] @END' "$log"; then
				sleep 1
				sudo pkill -INT -f -- "--config $cfg --no-setup"
				wait $pid
				sleep 2                  # let the board settle between boots
				return 0
			fi
		done
	fi
	wait $pid; rc=$?
	sleep 2
	return $rc
}

# REGRESS_TIMELINE=1: what hbl.py makes of boot $1's timeline
timeline_report() {
	[ "${REGRESS_TIMELINE:-0}" = 1 ] || return 0
	local tl="$out/$1.tl"
	[ -f "$tl" ] || return 0
	python3 "$here/../ipl-timeline/hbl.py" "$tl" > "$out/$1.hbl.txt" 2>&1 ||
		rm -f "$out/$1.hbl.txt"         # no HBL test in this boot
	gzip -f "$tl"
}

# ---------------------------------------------------------------- suite.txt
run_suite() {
	local name rom progs drives extra secs what
	while IFS='|' read -r name rom progs drives extra secs what; do
		name=$(echo $name); rom=$(echo $rom); drives=$(echo $drives)
		secs=$(echo $secs); extra=$(echo "$extra" | sed 's/^ *//; s/ *$//')
		want "$name" || continue
		local d="$out/$name"; mkdir -p "$d"
		local args=() p missing=""
		for p in $progs END; do
			[ -f "$here/guest/$p.PRG" ] || missing="$missing $p"
			args+=(--auto "$here/guest/$p.PRG")
		done
		if [ -n "$missing" ]; then
			record "$name" setup FAIL "no guest program(s):$missing - run tools/regress/build.sh"
			continue
		fi
		python3 "$here/mkimg.py" st "$d/boot.st" --pattern PATTERN.DAT=300000 "${args[@]}" ||
			{ record "$name" setup FAIL "mkimg.py st failed"; continue; }
		local cfg="$out/$name.cfg" log="$out/$name.log"
		{
			psctrl_paths
			echo "cpu 68000"
			echo "cpu_compatible enabled"
			echo "cpu_speed 8mhz"
			echo "jit_power 0"
			echo "ym2149"
			echo "rom $rom"
			echo "fdd A:$d/boot.st"
			case "$drives" in *ide*)
				python3 "$here/mkimg.py" hd "$d/ide.img" 16384
				echo "ide"; echo "hdd 0:$d/ide.img" ;;
			*)	echo "ide disabled" ;;
			esac
			case "$drives" in *acsi*)
				python3 "$here/mkimg.py" hd "$d/acsi.img" 16384
				echo "acsi enabled"; echo "acsi 0:$d/acsi.img" ;;
			*)	echo "acsi disabled" ;;
			esac
			[ "$extra" = "-" ] || echo "$extra" | tr ';' '\n' | sed 's/^ *//'
		} > "$cfg"

		echo "$name: booting (at most ${secs}s) on $commit ..."
		run_emulator "$cfg" "$secs" "$log" 1
		local rc=$?
		timeline_report "$name"

		# the guest's own lines, prefix stripped
		sed -n 's/^.*\[NF_STDERR\] \(@.*\)$/\1/p' "$log" > "$d/report.txt"
		local tag a b rest got=0
		while read -r tag a b rest; do
			case "$tag" in
			@T) record "$name" "$a" "$b" "$(echo "$rest" | cut -c1-160)"
			    got=$((got + 1)) ;;
			@M) echo "$name|$a|$b" >> "$out/.metrics"
			    printf '%s  %s  %s  %s  %s\n' "$stamp" "$commit" "$name" "$a" "$b" >> "$here/metrics.txt" ;;
			esac
		done < "$d/report.txt"

		local why; why=$(crash_reason "$log")
		if [ -n "$why" ]; then
			record "$name" boot FAIL "$why"
		elif ! grep -q '^@END' "$d/report.txt"; then
			local last; last=$(grep '^@S' "$d/report.txt" | tail -1 | cut -d' ' -f2)
			if [ $got -eq 0 ] && [ -z "$last" ]; then
				record "$name" boot FAIL "no output from the guest (floppy not booted? NatFeats off?) rc=$rc"
			else
				record "$name" boot FAIL "did not finish: stuck in ${last:-?} after $got checks (rc=$rc)"
			fi
		else
			record "$name" boot PASS "$got checks"
		fi
	done < <(grep -v '^[[:space:]]*#' "$here/suite.txt" | grep '|')
}

# ---------------------------------------------------------------- tests.txt
# Boots from your own images: name | cfg section | hdd image (- = the
# section's own drives) | seconds | what. Pass = it ran the whole time with
# no panic, crash or early exit.
run_tests() {
	local name section image secs what
	while IFS='|' read -r name section image secs what; do
		name=$(echo $name); section=$(echo $section); image=$(echo $image); secs=$(echo $secs)
		want "$name" || continue
		if [ "$image" != "-" ] && [ ! -f "$image" ]; then
			echo "$name: SKIP - $image not found"
			continue
		fi
		grep -q "^\[$section\]" "$cfg_src" || { echo "$name: SKIP - no [$section] in $cfg_src"; continue; }
		local cfg="$out/$name.cfg" log="$out/$name.log"
		psctrl_paths > "$cfg"
		awk -v s="[$section]" -v keep="$image" '
			/^\[/ { on = ($0 == s); next }
			on && (keep == "-" || $1 != "hdd") { print }' "$cfg_src" >> "$cfg"
		[ "$image" = "-" ] || echo "hdd $image" >> "$cfg"

		echo "$name: running ${secs}s on $commit ..."
		run_emulator "$cfg" "$secs" "$log" 0
		local rc=$? why
		why=$(crash_reason "$log")
		[ -z "$why" ] && [ $rc -ne 124 ] && why="emulator exited early (rc=$rc)"
		if [ -z "$why" ]; then
			record "$name" run PASS "ran ${secs}s"
		else
			record "$name" run FAIL "$why"
		fi
	done < <(grep -v '^[[:space:]]*#' "$here/tests.txt" | grep '|')
}

run_suite
run_tests

# ---------------------------------------------------------------- summary
# The previous result of each check (history.txt, before this run) tells
# NEW failures from old ones; measurements are set against the previous run.
prev_verdict() {      # boot check -> the verdict before this run, or nothing
	grep -F "  $1  $2  " "$here/history.txt" | grep -vF "$stamp  $commit" | tail -1 | awk '{print $6}'
}
prev_metric() {
	grep -F "  $1  $2  " "$here/metrics.txt" | grep -vF "$stamp  $commit" | tail -1 | awk '{print $6}'
}
total=$(wc -l < "$out/.checks")
failed=$(grep -c '|FAIL|' "$out/.checks")
{
	echo "Regression run $stamp on $commit"
	echo
	echo "$((total - failed)) of $total checks passed"
	echo
	if [ "$failed" -gt 0 ]; then
		echo "FAILED  (NEW = passed last time; first = never run before)"
		while IFS='|' read -r b c v d; do
			[ "$v" = FAIL ] || continue
			p=$(prev_verdict "$b" "$c")
			tag="        "
			[ "$p" = PASS ] && tag="NEW     "
			[ -z "$p" ] && tag="first   "
			echo "  $tag$b / $c: $d"
		done < "$out/.checks"
		echo
	fi
	fixed=$(while IFS='|' read -r b c v d; do
		[ "$v" = PASS ] && [ "$(prev_verdict "$b" "$c")" = FAIL ] && echo "  $b / $c"
	done < "$out/.checks")
	[ -n "$fixed" ] && { echo "FIXED since the last run"; echo "$fixed"; echo; }
	echo "All checks"
	while IFS='|' read -r b c v d; do
		printf '  %-4s %-10s %-22s %s\n' "$v" "$b" "$c" "$d"
	done < "$out/.checks"
	echo
	for h in "$out"/*.hbl.txt; do
		[ -f "$h" ] || continue
		echo "HBL timeline, $(basename "$h" .hbl.txt)"
		sed 's/^/  /' "$h"
		echo
	done
	echo "Measurements (the previous run's in brackets)"
	while IFS='|' read -r b m val; do
		p=$(prev_metric "$b" "$m")
		printf '  %-10s %-30s %10s  %s\n' "$b" "$m" "$val" "${p:+($p)}"
	done < "$out/.metrics"
} > "$summary"
cat "$summary"
rm -f "$out/.checks" "$out/.metrics"
[ "$failed" -gt 0 ] && exit 1
exit 0
