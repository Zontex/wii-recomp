#!/bin/sh
# usage: walk.sh seconds script-file ; lines: "<at-second> <command>" or "<at-second> shot name"
cd "${WII_RECOMP_DIR:-$HOME/wii-recomp}"; export XDG_RUNTIME_DIR=/run/user/$(id -u); SEC=$1
[ -d test/walk-saves ] || cp -r test/mac-saves test/walk-saves
rm -f test/control test/w-*.png
# The system's labwc settings without its autostart, so no panel or desktop joins in and
# the window sits where the reference pictures were cut, desktop session or not.
mkdir -p test/labwc; for f in rc.xml menu.xml environment; do [ -f /etc/xdg/labwc/$f ] && cp /etc/xdg/labwc/$f test/labwc/; done; rm -f test/labwc/autostart
env -u WAYLAND_DISPLAY WLR_BACKENDS=headless WLR_RENDERER=gles2 WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 WLR_HEADLESS_OUTPUTS=1 labwc -C test/labwc >test/labwc.log 2>&1 &
L=$!; sleep 2
export WAYLAND_DISPLAY=$(ls -t "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | head -1) WII_RECOMP_ACTIVITY=1 WII_RECOMP_SAVES=$PWD/test/walk-saves WII_RECOMP_CONTROL=$PWD/test/control WII_RECOMP_MUTE=1
timeout $SEC recomp/build/wii-recomp ${WII_RECOMP_GAME:-extracted/sys} nand > test/w.log 2>&1 &
P=$!
T0=$(date +%s)
while read at cmd rest; do
  # Lines past the run's end would wait forever on a pipe nobody reads.
  [ "$at" -lt "$SEC" ] || break
  while [ $(( $(date +%s)-T0 )) -lt $at ]; do sleep 0.2; done
  if [ "$cmd" = react ]; then python3 test/react.py $T0 $(( SEC - at )) & elif [ "$cmd" = shot ]; then grim -s 0.5 test/w-$rest.png; else printf '%s %s\n' "$cmd" "$rest" > test/control; fi
done < "$2"
wait $P; kill $L 2>/dev/null
grep -E 'stop:|panic|fatal|^strip:' test/w.log | sort | uniq -c | head -6 | cut -c1-110
grep -E '^second' test/w.log | awk '$3>2000 {print $7}' | sort -n | uniq -c | awk '{printf "%sx%s ", $2,$1} END{print "fps"}'
grep -c '^slow frame' test/w.log
