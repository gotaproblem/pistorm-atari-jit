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
	local cfg=$1 secs=$2 log=$3 until_end=$4 envs=("${RUN_ENVS[@]}")
	# REGRESS_TIMELINE=1: IPL timeline with level 2 (HBL), one per boot,
	# read by tools/ipl-timeline/hbl.py into <boot>.hbl.txt (measurement
	# only: the logging is a store into a ring on the ipl_task core)
	# REGRESS_TIMELINE=4: levels 4 and 6 only (PISTORM_IPL_TIMELINE=1) -
	# every VBL/MFP line change and take, for programs whose HBLs would
	# flood a level-2 log (Bad Apple)
	case "${REGRESS_TIMELINE:-0}" in
	1)	envs+=(PISTORM_IPL_TIMELINE=2 "PISTORM_IPL_TIMELINE_LOG=${log%.log}.tl") ;;
	4)	envs+=(PISTORM_IPL_TIMELINE=1 "PISTORM_IPL_TIMELINE_LOG=${log%.log}.tl") ;;
	esac
	# stdin from /dev/null: the boot loops read their lists on stdin, and
	# anything the emulator read would eat the remaining tests (6 Nov: only
	# fastbobs of three ran)
	sudo env "${envs[@]}" timeout -s INT "$secs" ./emulator --config "$cfg" --no-setup < /dev/null > "$log" 2>&1 &
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
	local tl="$out/$1.tl"
	[ -f "$tl" ] || return 0
	[ "${REGRESS_TIMELINE:-0}" = 1 ] || { gzip -f "$tl"; return 0; }
	python3 "$here/../ipl-timeline/hbl.py" "$tl" > "$out/$1.hbl.txt" 2>&1 ||
		rm -f "$out/$1.hbl.txt"         # no HBL test in this boot
	gzip -f "$tl"
}

# ---------------------------------------------------------------- suite.txt
run_suite() {
	local name rom progs drives extra secs what
	local RUN_ENVS=(${REGRESS_ENV:-})	# e.g. REGRESS_ENV=PISTORM_WRBEHIND=1
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

# ---------------------------------------------------------------- host checks
# tests.txt's checks column: what the emulator's own reports must show
# while a real program runs. Each check is one @T line; each also leaves a
# measurement. The reports come from RUN_ENVS (run_tests):
#   vbl=N     every [ipl] "vbl: taken=" second after the first 15 is N +-1
#             (PISTORM_IPL_STATS) - Bad Apple ran 12% fast on doubled VBLs
#   gov>=K    every [GOV] 5 s window after the first two reached K kHz
#             (PISTORM_GOV_REPORT) - MOD players and games slow down first
#   noov      no "[dmasnd] STE stream overflow": the capture never queued
#             more than real time (20 Years / FastBobs, 5 Nov 2026)
#   snd       no [dmasnd] SUM second, after the first 10 of play, with the
#             output queue empty while a frame plays (PISTORM_DMASND_DEBUG=2)
#   shmcal    the HDMI mirror's last three [SHM] summaries each accepted a
#             video counter read (PISTORM_SHM_DEBUG=1) - FastBobs: cal=0
#             was the mirror drawing the wrong buffer, half-drawn balls
metric() {            # boot name value: one measurement, this run and history
	echo "$1|$2|$3" >> "$out/.metrics"
	printf '%s  %s  %s  %s  %s\n' "$stamp" "$commit" "$1" "$2" "$3" >> "$here/metrics.txt"
}
host_checks() {
	local name=$1 log=$2 checks=$3 c v n
	for c in $checks; do
		case "$c" in
		vbl)
			# One guest VBL for every real one: each stats second's VBLs
			# taken against the Shifter's own rate that second (the snooped
			# sync, `guest=50.0Hz`/60/71), summed over the run after the
			# first 15 s, must agree to 0.5 a second. Summed: the stats
			# thread's "second" is 1 s of sleep plus its own printing, and a
			# stalled log write stretches one (57 at 50 Hz - FastBobs, 6 Nov)
			# but moves the sum by under 0.2 a second. Lost or doubled VBLs
			# move it by whole VBLs a second (Bad Apple, 60 Hz, 6 Nov: 54.6).
			# The rate comes from the sync register, so a program that
			# switches 50/60 Hz is judged against what it chose.
			read -r n st sh lo hi <<<"$(awk '
				/^\[ipl\]   vbl: taken=/ { sub(/.*taken=/, ""); t = $0 + 0; have = 1; next }
				/^\[ipl\]   shifter:/ && have {
					have = 0
					if (!match($0, /guest=[0-9.]+/)) next
					h = substr($0, RSTART + 6, RLENGTH - 6) + 0
					if (++k > 15) { n++; st += t; sh += h
						if (!m || t < lo) lo = t; if (t > hi) hi = t; m = 1 } }
				END { print (n ? n : -1), st + 0, sh + 0, lo + 0, hi + 0 }' "$log")"
			metric "$name" vbl_min "$lo"; metric "$name" vbl_max "$hi"
			if [ "$n" = -1 ]; then record "$name" vbl FAIL "no [ipl] vbl/shifter lines (PISTORM_IPL_STATS)"
			else
				local dif; dif=$(awk -v t="$st" -v h="$sh" -v n="$n" 'BEGIN { printf "%.2f", (t - h) / n }')
				metric "$name" vbl_per_s_vs_sync "$dif"
				# and no single second over the sync rate: a guest never gets
				# more VBLs than the Shifter makes (Bad Apple at 60 Hz must
				# never read 61 or 70 - Steve, 13 Nov). The stats line is
				# per real second since then, so +1 is rounding only.
				local over; over=$(awk '
					/^\[ipl\]   vbl: taken=/ { sub(/.*taken=/, ""); t = $0 + 0; have = 1; next }
					/^\[ipl\]   shifter:/ && have {
						have = 0
						if (!match($0, /guest=[0-9.]+/)) next
						h = substr($0, RSTART + 6, RLENGTH - 6) + 0
						if (++k > 15 && t > int(h + 1)) n++ }
					END { print n + 0 }' "$log")
				metric "$name" vbl_over_rate "$over"
				if [ "$over" != 0 ]; then
					record "$name" vbl FAIL "$over seconds with more VBLs taken than the sync rate ($st for $sh in $n s; per-second $lo..$hi)"
				elif awk -v d="$dif" 'BEGIN { exit !(d >= -0.5 && d <= 0.5) }'; then
					record "$name" vbl PASS "$st VBLs taken for $sh at the sync rate in $n s ($dif/s; per-second $lo..$hi)"
				else
					record "$name" vbl FAIL "$st VBLs taken for $sh at the sync rate in $n s ($dif/s; per-second $lo..$hi)"
				fi
			fi ;;
		gov\>=*)
			v=${c#gov>=}
			read -r n lo <<<"$(sed -n 's/^\[GOV\] reaching \([0-9]*\) kHz.*/\1/p' "$log" |
				awk -v w="$v" 'NR > 2 { n++; if (!lo || $1 < lo) lo = $1; if ($1 < w) bad++ }
				               END { print (n ? bad + 0 : -1), lo + 0 }')"
			metric "$name" gov_min_khz "$lo"
			if [ "$n" = -1 ]; then record "$name" gov FAIL "no [GOV] windows (governor off? PISTORM_GOV_REPORT)"
			elif [ "$n" = 0 ]; then record "$name" gov PASS "lowest 5 s window $lo kHz (>= $v)"
			else record "$name" gov FAIL "$n windows under $v kHz, lowest $lo"; fi ;;
		noov)
			n=$(grep -c '\[dmasnd\] STE stream overflow' "$log")
			metric "$name" snd_overflows "$n"
			if [ "$n" = 0 ]; then record "$name" noov PASS "no output overflow"
			else record "$name" noov FAIL "$(grep -m1 '\[dmasnd\] STE stream overflow' "$log" | cut -c1-140)"; fi ;;
		snd)
			read -r n tot <<<"$(grep '^\[dmasnd\] SUM' "$log" |
				awk '/ en=1 parked=0 / { p++; if (p > 10) { t++; if ($0 ~ / ring=0$/) bad++ } }
				     END { print bad + 0, t + 0 }')"
			metric "$name" snd_starved_s "$n"
			if [ "$tot" = 0 ]; then record "$name" snd FAIL "no DMA sound played (PISTORM_DMASND_DEBUG=2 SUM lines)"
			elif [ "$n" = 0 ]; then record "$name" snd PASS "$tot s of play, output never ran dry"
			else record "$name" snd FAIL "output ran dry in $n of $tot s of play"; fi ;;
		shmcal)
			n=$(grep '^\[SHM\] [0-9]*Hz P=' "$log" | tail -3 | sed -n 's/.* cal=\([0-9]*\)\/.*/\1/p' |
				awk '{ k++; if ($1 == 0) z++ } END { print (k == 3 ? z + 0 : -1) }')
			metric "$name" shm_cal_zero "$n"
			if [ "$n" = -1 ]; then record "$name" shmcal FAIL "under 3 [SHM] summaries (native_hdmi off? PISTORM_SHM_DEBUG)"
			elif [ "$n" = 0 ]; then record "$name" shmcal PASS "counter reads accepted to the end"
			else record "$name" shmcal FAIL "$n of the last 3 summaries took no counter read (mirror on the wrong buffer?)"; fi ;;
		*)	record "$name" "$c" FAIL "unknown check in tests.txt" ;;
		esac
	done
}

# ---------------------------------------------------------------- tests.txt
# Boots from your own images: name | cfg section | hdd image (- = the
# section's own drives) | start | extra cfg | seconds | checks | what.
# Pass = it ran the whole time with no panic, crash or early exit, and
# every check (host_checks above; - = none) held.
#
# start (- = the image boots on its own) builds the boot files from
# `;`-separated directives, twice: a hard disk on ACSI ID 0 (C:) and a
# floppy in A:. EmuTOS takes A: or C: as the boot drive depending on the
# RAM it finds (a first boot picks C: when a hard disk exists, a warm one
# keeps the old _bootdev - bios.c), and only the boot drive's AUTO
# folder, EMUDESK.INF and accessories run, so both carry them. The test
# image goes on ACSI ID 1 = D:, where the programs live:
#   auto PATH [ARGS]  LAUNCH.PRG in C:\AUTO starts PATH with ARGS, in
#                     PATH's folder (TOS programs: FastBobs, Bad Apple)
#   desk PATH         the desktop autostarts PATH (EMUDESK.INF #Z) - GEM
#                     programs, which need the AES (Paula)
#   acc NAME ARG      guest/NAME.PRG goes on C: as NAME.ACC with ARG in
#                     C:\NAME.INF (MPSEND: make Paula play module ARG)
make_start_floppy() {
	local fl=$1 start=$2 d args=() dir
	dir=$(dirname "$fl")
	rm -f "$dir"/RUN.INF "$dir"/EMUDESK.INF "$dir"/*.INF
	local IFS=';'
	for d in $start; do
		d=$(echo "$d" | sed 's/^ *//; s/ *$//')
		local op=${d%% *} rest=${d#* }
		case "$op" in
		auto)	local p=${rest%% *} a=""
			[ "$p" != "$rest" ] && a=${rest#* }
			printf '%s\r\n%s\r\n' "$p" "$a" > "$dir/RUN.INF"
			args+=(--auto "$here/guest/LAUNCH.PRG" --file "RUN.INF=$dir/RUN.INF") ;;
		desk)	printf '#E 1A 01\r\n#Z 01 %s@\r\n' "$rest" > "$dir/EMUDESK.INF"
			args+=(--file "EMUDESK.INF=$dir/EMUDESK.INF") ;;
		acc)	local n=${rest%% *} a=${rest#* }
			printf '%s\r\n' "$a" > "$dir/$n.INF"
			args+=(--file "$n.ACC=$here/guest/$n.PRG" --file "$n.INF=$dir/$n.INF") ;;
		*)	echo "unknown start directive: $d"; return 1 ;;
		esac
	done
	python3 "$here/mkimg.py" hdboot "$fl" "${args[@]}" &&
	python3 "$here/mkimg.py" st "${fl%.img}.st" "${args[@]}"
}

run_tests() {
	local name section image start extra secs checks what
	# REGRESS_ENV="A=1 B=2": extra emulator environment for this run, and
	# REGRESS_TAG=x names its results <name>-x, so an A/B pair of runs of
	# one test sits side by side (e.g. REGRESS_ENV=PISTORM_SHM=0 REGRESS_TAG=noshm)
	local RUN_ENVS=(PISTORM_IPL_STATS=1 PISTORM_GOV_REPORT=1 PISTORM_SHM_DEBUG=1 PISTORM_DMASND_DEBUG=2 ${REGRESS_ENV:-})
	while IFS='|' read -r name section image start extra secs checks what; do
		name=$(echo $name); section=$(echo $section); image=$(echo $image); secs=$(echo $secs)
		start=$(echo "$start" | sed 's/^ *//; s/ *$//'); extra=$(echo "$extra" | sed 's/^ *//; s/ *$//')
		checks=$(echo $checks); [ "$checks" = "-" ] && checks=""
		want "$name" || continue
		[ -n "${REGRESS_TAG:-}" ] && name="$name-$REGRESS_TAG"
		if [ "$image" != "-" ] && [ ! -f "$image" ]; then
			echo "$name: SKIP - $image not found"
			continue
		fi
		if ! grep -q "^\[$section\]" "$cfg_src"; then
			echo "$name: SKIP - no [$section] in $cfg_src"
			[ "$section" = regression ] &&
				echo "  append tools/regress/psctrl-regression.cfg to $cfg_src"
			continue
		fi
		# absolute: a relative image would be taken from disk_path
		[ "$image" = "-" ] || image=$(realpath "$image")
		local cfg="$out/$name.cfg" log="$out/$name.log" fl=""
		if [ "$start" != "-" ]; then
			mkdir -p "$out/$name.d"
			fl="$out/$name.d/boot.img"
			make_start_floppy "$fl" "$start" > /dev/null ||
				{ record "$name" setup FAIL "boot disk: $start"; continue; }
		fi
		# With a start, the boot disk goes on ACSI ID 0 (C:) and in A:, the
		# image on ID 1 (D:) - ACSI drives come before IDE - and the
		# section's own hdd/acsi images and fdd lines are dropped. A real ACSI device
		# on another ID stays on the bus (per-ID passthrough) and lands
		# after D:. Without a start the image replaces the section's hdd
		# lines.
		psctrl_paths > "$cfg"
		# a key set in extra replaces the section's line(s) for it
		local xkeys=""
		[ "$extra" = "-" ] || xkeys=$(echo "$extra" | tr ';' '\n' | awk '{ printf " %s ", $1 }')
		awk -v s="[$section]" -v keep="$image" -v fl="$fl" -v xk="$xkeys" '
			/^\[/ { on = ($0 == s); next }
			!on { next }
			NF && index(xk, " " $1 " ") { next }
			keep != "-" && $1 == "hdd" { next }
			fl != "" && $1 ~ /^(fdd|acsi|acsi_order)$/ { next }
			{ print }' "$cfg_src" >> "$cfg"
		if [ -n "$fl" ]; then
			printf 'fdd A:%s\nacsi enabled\nacsi 0:%s\n' "${fl%.img}.st" "$fl" >> "$cfg"
			[ "$image" = "-" ] || echo "acsi 1:$image" >> "$cfg"
		else
			[ "$image" = "-" ] || echo "hdd $image" >> "$cfg"
		fi
		[ "$extra" = "-" ] || echo "$extra" | tr ';' '\n' | sed 's/^ *//' >> "$cfg"

		echo "$name: running ${secs}s on $commit ..."
		run_emulator "$cfg" "$secs" "$log" 0
		local rc=$? why
		why=$(crash_reason "$log")
		[ -z "$why" ] && [ $rc -ne 124 ] && why="emulator exited early (rc=$rc)"
		# an auto start must be seen to launch (else the boot sat at the desktop)
		case "$start" in auto*|*\;\ auto*|*\;auto*)
			if [ -z "$why" ] && ! grep -q '\[NF_STDERR\] @I launch' "$log"; then
				why="LAUNCH.PRG never ran (no @I launch line - the boot drive's AUTO not run, or no NatFeats)"
			elif [ -z "$why" ] && grep -q '\[NF_STDERR\] @T launch FAIL' "$log"; then
				why="$(grep -m1 '\[NF_STDERR\] @T launch FAIL' "$log" | cut -c1-120)"
			fi ;;
		esac
		if [ -z "$why" ]; then
			record "$name" run PASS "ran ${secs}s"
		else
			record "$name" run FAIL "$why"
		fi
		[ -n "$checks" ] && host_checks "$name" "$log" "$checks"
		timeline_report "$name"
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
