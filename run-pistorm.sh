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
cd "$(dirname "$0")" || exit 1
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
