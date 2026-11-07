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
