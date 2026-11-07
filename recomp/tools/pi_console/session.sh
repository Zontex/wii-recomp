#!/bin/sh
# The console on the TV, run by cage (a full-screen Wayland compositor) for
# wii-console.service. It keeps the console program's process ID and exit code in
# the service's runtime folder, /run/wii-console, where stop.sh and standby.py read them.
# Installed by pi_setup.sh.
RUN=${RUNTIME_DIRECTORY:-/run/wii-console}
. "${WII_CONSOLE_CONF:-/etc/wii-recomp/console.conf}"
mkdir -p "$CONSOLE/logs"
LOG=$CONSOLE/logs/console.log
[ -f "$LOG" ] && mv "$LOG" "$LOG.1"
"$PROGRAM" --console "$CONSOLE" > "$LOG" 2>&1 &
CHILD=$!
echo "$CHILD" > "$RUN/pid"
wait "$CHILD"
CODE=$?
echo "$CODE" > "$RUN/exit"
exit "$CODE"
