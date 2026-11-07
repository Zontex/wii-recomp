#!/bin/sh
# Stopping wii-console.service presses the Wii's power button: the console program turns
# off its own way (it takes SIGTERM as the button) and this waits for it, so systemd
# doesn't end it before it has saved. Installed by pi_setup.sh.
PID=$(cat "${RUNTIME_DIRECTORY:-/run/wii-console}/pid" 2>/dev/null) || exit 0
[ "$(cat "/proc/$PID/comm" 2>/dev/null)" = wii-recomp ] || exit 0
kill -TERM "$PID" 2>/dev/null || exit 0
i=0
while kill -0 "$PID" 2>/dev/null && [ "$i" -lt 150 ]; do sleep 0.1; i=$((i + 1)); done
