#!/bin/sh
# run-pistorm.sh - launch the PiStorm emulator with restart support.
#
# The emulator signals what it wants through its exit code:
#   42  relaunch  - the taskbar "Restart" action, and the crash handler.
#                   The relaunch comes back up on the pre-boot setup page,
#                   so Restart is also how you get back to it.
#                   Comes straight back up and re-reads the .cfg, so a
#                   Restart after saving PiStorm settings is a real cold
#                   boot with the new settings.
#    0  stop       - the taskbar "Shut down" action; drops you here at the
#                   console.
#   other          - an error; also stops, so a genuine failure does not
#                   spin in a relaunch loop.
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
while :; do
	./emulator --config ../configs/psctrl.cfg "$@"
	rc=$?
	if [ "$rc" -eq 42 ]; then
		echo "[run-pistorm] restart requested (42) - relaunching..."
		sleep 1
		continue
	fi
	echo "[run-pistorm] emulator exited ($rc) - stopping."
	break
done
