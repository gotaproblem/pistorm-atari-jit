#!/bin/sh
# run-pistorm.sh - launch the PiStorm emulator with restart support.
#
# The emulator signals what it wants through its exit code:
#   42  relaunch  - the taskbar "Restart" action, Ctrl+Alt+Undo, the ST's
#                   own reset button, ST power loss, and the crash handler.
#                   The relaunch comes back up on the pre-boot setup page,
#                   so Restart is also how you get back to it.
#                   Comes straight back up and re-reads the .cfg, so a
#                   Restart after saving PiStorm settings is a real cold
#                   boot with the new settings.
#   43  halt      - Ctrl+Alt+Shift+Undo: shut Linux down (shutdown -h now).
#    0  stop      - the taskbar "Shut down" action, or Ctrl+C at the
#                   console; drops you here at the console.
#   other         - the emulator ended some other way (an error, a crash
#                   the handler did not catch, the board reset under it):
#                   relaunch too - a physical reset must never leave the
#                   machine at a console. But if it ends that way within
#                   15 s of starting, 3 times running, stop: that is a
#                   genuine failure (bad .cfg, missing ROM, no board), and
#                   relaunching it for ever would hide the message.
#
# ASTOS (platforms/astos/, doc/ASTOS.md). When ./astos is built, it is
# what this script starts, in place of the setup page: the machine boots
# into the ASTOS desktop, and the emulator only ever runs a build ASTOS
# picked. ASTOS's own exit codes:
#   44  launch    - the build named in /run/astos-launch: run
#                   ./emulator --no-setup --build <it> (an `engine metal`
#                   build hands itself to stboxmetal as before). However
#                   that ends - 42 from Ctrl+Alt+Undo, the ST's reset
#                   button, power loss, a crash - ASTOS comes back. Only
#                   43 (Ctrl+Alt+Shift+Undo) still shuts Linux down.
#   42  restart   - start ASTOS again (its Restart, the ST's reset button,
#                   a power loss: ASTOS waits for the power on its way up).
#   43  halt      - ASTOS's Shut down: shut Linux down.
#    0  stop      - ASTOS's Exit to console.
#   other         - ASTOS failed: try again; after 2 failures in a row
#                   start it with --safe (no auto-launch, no HDMI), after
#                   5 stop and leave the console.
# PISTORM_ASTOS=0 in front of the script brings the setup page back.
#
# Launch through this loop from the console instead of running ./emulator
# directly. Extra arguments after the script name are forwarded.
# Absolute path to this script BEFORE the cd, so the sudo re-exec below
# does not depend on PATH (sudo does not look in the current directory)
# or on the script being marked executable.
self_dir=$(cd "$(dirname "$0")" && pwd) || exit 1
self="$self_dir/$(basename "$0")"

cd "$self_dir" || exit 1

# The emulator needs /dev/mem for the GPIO, so it has to run as root. Ask
# for that here rather than letting it exit with "Unable to open /dev/mem".
# sudo resets the environment, so PISTORM_* variables set in front of this
# script (PISTORM_IKBD_DEBUG=1 sh run-pistorm.sh ...) are carried across
# by hand - they are the only ones the emulator reads.
if [ "$(id -u)" -ne 0 ]; then
	echo "[run-pistorm] needs root for /dev/mem - re-running under sudo"
	keep=$(env | grep '^PISTORM_[A-Z0-9_]*=' | tr '\n' ' ')
	exec sudo -- env $keep /bin/sh "$self" "$@"
fi
# ---- ASTOS ---------------------------------------------------------------
if [ -x ./astos ] && [ "${PISTORM_ASTOS:-1}" != "0" ]; then
	cfg=../configs/psctrl.cfg
	afails=0
	safe=""
	while :; do
		rm -f /run/astos-launch
		./astos --config "$cfg" $safe
		rc=$?
		case "$rc" in
		44)
			afails=0
			safe=""
			build=$(head -n 1 /run/astos-launch 2>/dev/null)
			if [ -z "$build" ]; then
				echo "[run-pistorm] ASTOS asked to launch, but named no build"
				sleep 1
				continue
			fi
			# a second line: another config to launch it from (ASTOS's
			# astos-play.cfg - a floppy played from Files)
			lcfg=$(sed -n 2p /run/astos-launch 2>/dev/null)
			if [ -z "$lcfg" ] || [ ! -f "$lcfg" ]; then
				lcfg="$cfg"
			fi
			echo "[run-pistorm] ASTOS: launching [$build] from $lcfg"
			./emulator --config "$lcfg" --no-setup --build "$build" "$@"
			erc=$?
			# for ASTOS's events log: how the build ended
			echo "$build $erc" > /run/astos-last-exit 2>/dev/null
			if [ "$erc" -eq 43 ]; then
				echo "[run-pistorm] Ctrl+Alt+Shift+Undo (43) - shutting Linux down."
				sync
				shutdown -h now
				exit 0
			fi
			echo "[run-pistorm] [$build] ended ($erc) - back to ASTOS"
			sleep 1
			;;
		42)
			afails=0
			safe=""
			echo "[run-pistorm] ASTOS restart (42)"
			sleep 1
			;;
		43)
			echo "[run-pistorm] ASTOS: shut down (43) - shutting Linux down."
			sync
			shutdown -h now
			exit 0
			;;
		0)
			echo "[run-pistorm] ASTOS: exit to console (0)."
			# under pistorm.service (Restart=always) a plain exit would
			# bring ASTOS straight back: stop the service instead
			if [ -n "$INVOCATION_ID" ]; then
				systemctl stop --no-block pistorm.service 2>/dev/null
			fi
			exit 0
			;;
		*)
			afails=$((afails + 1))
			if [ "$afails" -ge 5 ]; then
				echo "[run-pistorm] ASTOS failed ($rc) 5 times running - stopping."
				exit 1
			fi
			if [ "$afails" -ge 2 ]; then
				safe="--safe"
				echo "[run-pistorm] ASTOS failed ($rc) twice running - safe mode"
			else
				echo "[run-pistorm] ASTOS exited ($rc) - starting it again"
			fi
			sleep 2
			;;
		esac
	done
fi

# ---- the setup page (no ASTOS) ---------------------------------------------
# the emulator hands a plain start to this script when ./astos exists;
# here the page is what is wanted, so tell it not to
export PISTORM_ASTOS=0
fails=0
while :; do
	started=$(date +%s)
	./emulator --config ../configs/psctrl.cfg "$@"
	rc=$?
	case "$rc" in
	42)
		fails=0
		echo "[run-pistorm] restart requested (42) - relaunching..."
		sleep 1
		continue
		;;
	43)
		echo "[run-pistorm] Ctrl+Alt+Shift+Undo (43) - shutting Linux down."
		sync
		shutdown -h now
		exit 0
		;;
	0)
		echo "[run-pistorm] emulator stopped (0)."
		break
		;;
	esac
	if [ $(( $(date +%s) - started )) -lt 15 ]; then
		fails=$((fails + 1))
	else
		fails=1
	fi
	if [ "$fails" -ge 3 ]; then
		echo "[run-pistorm] emulator exited ($rc) within 15 s, 3 times running - stopping."
		break
	fi
	echo "[run-pistorm] emulator exited ($rc) - relaunching..."
	sleep 2
done
