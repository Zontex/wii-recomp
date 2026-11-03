#!/bin/sh
# usage: run.sh <name> <seconds> [VAR=value ...]
# Walks the game with SCRIPT (w20.txt unless set) and writes a summary to test/<name>.out:
# seconds spent at each frame rate, then any warnings. Start it with setsid so it outlives the shell.
name=$1 sec=$2; shift 2
dir="${WII_RECOMP_DIR:-$HOME/wii-recomp}"
cd "$dir"
{ rm -rf test/walk-saves test/u-*.png; env WII_RECOMP_DIR="$dir" "$@" sh test/walk2.sh $sec "test/${SCRIPT:-w20.txt}" | tail -2 | head -1
grep -E "^second" test/w.log | awk "{f=\$7; b=(f>=59)?\"60\":(f>=50?\"50-58\":(f>=40?\"40-49\":(f>=30?\"30-39\":\"under 30\"))); c[b]++} END{for(k in c) printf \"%s fps: %d s; \", k, c[k]; print \"\"}"
grep -E "^layer|stop:|panic|vulkan:" test/w.log | sort | uniq -c | head -5 | cut -c1-120
echo finished; } > test/$name.out 2>&1
