# Walking the game on the Raspberry Pi

These play the game with no screen attached, for measuring frame rates at 1080p. They run from `test/` in the checkout on the Pi, `~/wii-recomp` unless `WII_RECOMP_DIR` says otherwise, next to the build in `recomp/build`.

- `walk2.sh <seconds> <script>` starts a headless labwc and the game, then sends the script's commands to the control pipe at their times. A line is `<second> <command>`, where the command is anything the pipe takes (`press a`, `pointer x y`, `board on kg x y`, `board off`), `shot <name>` for a screenshot, or `react` to hand over to `react.py`. It prints the frame rates it saw.
- `w20.txt` creates a Mii and gets to the board's setup, then reacts from there.
- `mii.txt` stops on the Mii page at 1080p's slowest steady screen; a 68-second walk is enough, with a screenshot `w-m.png` at 60 seconds.
- `run.sh <name> <seconds> [VAR=value ...]` runs a walk (`SCRIPT=mii.txt` for the Mii page) and writes `test/<name>.out` with the seconds spent at each frame rate and any warnings. Add `WII_RECOMP_GPU_TIMES=1` for what the chip spent on each pass. Start it with `setsid`, as Wi-Fi on the Pi can drop under load.
- `react.py` watches the dialog box and answers like a person: it steps on when asked, steps off and presses A when the game wants it off, and clicks the right button on screens where A alone does nothing. Anything else gets A every 6 seconds. Screens it doesn't know are saved once as `u-NN.png`, and its decisions go to `react.log`.

`react.py` recognizes screens by comparing the dialog box with images in `ref/`, cut from screenshots at (60, 186, 560, 240) of a 640x360 shot. Those are pictures of the game, so they stay on the Pi and aren't in this repo. Make them from a run's `u-NN.png` files and name each one after what the script should do: `stepon`, `stepoff`, `cantfind`, `reboot` or `clothing`.

A walk to the end of the Body Test takes about 13 minutes: `sh walk2.sh 780 w20.txt`.
