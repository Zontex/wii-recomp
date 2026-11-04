# The recompiler

`translate/` turns a game's PowerPC program into C, and `runtime/` is the native console it runs on. The top-level `README.md` has the overview and the build steps.

Everything in `recomp/build/` is a game in another form, so it stays on your machine and out of the repo.

Needs `extracted/` from `recomp/tools/extract_disc.py`, plus `clang`, `ninja` and GLFW (`brew install glfw`). The second argument is a Wii storage folder, such as Dolphin's NAND folder after its system update.

```sh
python3 recomp/translate/emit.py extracted/sys/main.dol recomp/build/generated
python3 recomp/translate/build_file.py recomp/build
mkdir -p recomp/build/obj
(cd recomp/build && ninja)
recomp/build/wii-recomp extracted/sys path/to/Wii/NAND
```

The build makes two things: `wii-recomp`, the console, which is the same for every game, and `title.dylib` (`title.so` on Linux), the game's translated code, which the console loads from beside itself.

To play from a disc image instead, pack the game and its native code into one file, and start the console with it. Or set up a console with the Wii Menu (see `docs/native_ios.md`) and put the image in its drive:

```sh
python3 recomp/tools/make_disc.py extracted game.wrd --native darwin-x86_64=recomp/build/title.dylib
recomp/build/wii-recomp game.wrd path/to/Wii/NAND
python3 recomp/tools/prepare_console.py path/to/Wii/NAND console --menu recomp/build-menu
cp game.wrd console/drive/ && recomp/build/wii-recomp --console console
```

Keys: arrows for the D-pad, Enter for A, Backspace for B, `1`, `2`, `=` for plus, `-` for minus, `H` for Home. The mouse is the pointer. Space steps on or off the board, and W, A, S, D lean.

Set `WII_RECOMP_CONTROL=<path>` to drive it from a script through a pipe at that path, one command per line: `press a`, `hold a`, `release a`, `pointer x y`, `board on <kg> <lean x> <lean y>`, `board off`. Saves go to `recomp/build/saves`, or to `WII_RECOMP_SAVES`.

Set `WII_RECOMP_MUTE=1` to skip the sound device, and `WII_RECOMP_AUDIO_WAV=<path>` to save what the mixer makes.

Vulkan is the renderer everywhere it's built in. Set `WII_RECOMP_RENDERER=opengl` to use OpenGL instead, on a Mac or wherever Vulkan isn't working. With Vulkan, compiled shaders and pipelines are kept in `shader-cache` beside the program, and everything earlier runs used is built before the game starts. So a scene stutters only the first time it's ever drawn, never on later runs. Delete the folder to start over, or set `WII_RECOMP_NO_SHADER_CACHE=1` to compare without it. On the Mac, pictures reach the window on a second queue, so a window that's slow to take them, as when macOS holds the window's pictures for up to a second, skips pictures instead of stopping the game; `WII_RECOMP_SHOW_IN_FRAME=1` shows them in the frame as before, and `WII_RECOMP_TEST_WINDOW_STALL=<n>` makes the n-th picture wait a second, to check. The game also tells macOS it's doing time-sensitive work, so a covered window doesn't slow it to 10 fps. `WII_RECOMP_IDLE_SPIN=1` makes the game's idle loop spin as it does on a Wii, for comparing against the normal sleep.

On a Raspberry Pi 5 the build draws at 5/2 of the Wii's resolution, enough for full 1080p. A tile-based chip stores and reloads everything a pass covers, so `gx_vulkan.c` keeps passes few and small: clears become how a pass starts, a copy of a waiting clear is a plain clear, work in the strip below the picture runs ahead of the frame, depth is copied as soon as the scene ends, depth a pass won't need isn't loaded or stored, a copy drawn straight back onto the picture it came from is skipped, and a translucent layer with nothing in it, which the game copies out, clears, puts back and lays over the picture up to four times a frame, is skipped whole. A layer with something in it is drawn into a second picture while the first stays put, so the picture isn't copied out and drawn back, and the layer's texture takes the second picture's image instead of a copy of it. Shaders write the second output that dual-source blending reads only when it differs from the first, as each one costs the chip another write of every pixel. Fog that a draw's vertex depths prove adds nothing isn't worked out for every pixel. A copy of depth that was just cleared whole is a fill, and a waiting clear of depth alone doesn't run as its own pass before a color copy. When a layer is given up after its picture copy was drawn back, that copy is left out as for a layer drawn apart, unless the draw at hand reads it, and a later read of it stops the skipping. On Linux, commands run on their own thread, except in games using FIFO breakpoints.

For comparing, these turn each one off: `WII_RECOMP_DRAW_CLEARS=1`, `WII_RECOMP_NO_STRIP=1`, `WII_RECOMP_NO_EARLY_DEPTH=1`, `WII_RECOMP_KEEP_DEPTH=1`, `WII_RECOMP_KEEP_COPY_BACK=1`, `WII_RECOMP_KEEP_EMPTY_LAYERS=1`, `WII_RECOMP_NO_SIDE=1`, `WII_RECOMP_NO_SWAP=1`, `WII_RECOMP_KEEP_FOG=1`, `WII_RECOMP_NO_FLAT_DEPTH=1`, `WII_RECOMP_KEEP_DRAWN_BACK=1`, `WII_RECOMP_GX_INLINE=1`. `WII_RECOMP_COVER=1` turns one on: when a page proven opaque covers the whole picture, the drawing before it is thrown away. It has been checked on one screen, so it stays off by default. `WII_RECOMP_GPU_TIMES=1` prints what the chip spent on each pass of one frame in every 300.

`recomp/tools/gap_report.py` merges `gaps.txt` files from any number of runs and titles into one table, most common first: `gap_report.py menu=recomp/build-menu/gaps.txt wiifit=recomp/build/gaps.txt`. It lists apart the rows marked done that a game still hit, and IDs with no row.

`recomp/tools/perf_run.py` measures a fixed run with no input from a copy of `recomp/build/saves`: frame rate, worst frame, CPU, the Mac's speed limit, and what each slow frame spent its time on. It works on a Mac only, because it reads `pmset`.

`recomp/tools/replay/` plays a Dolphin FIFO recording through the runtime's graphics files with no game and no window, and saves each frame as a PNG. In Dolphin, with a game running: Tools > FIFO Player, set Frames to Record, press Record, wait for it to finish, then Save.... Recordings hold the game's textures and models, so `*.dff` stays out of the repo. Only file version 6 is read, which is what Dolphin writes now.

```sh
make -C recomp/tools/replay VULKAN_SDK=$PWD/recomp/build/vulkan-sdk
recomp/tools/replay/build/replay scene.dff recomp/build/frames [--frames <count>]
python3 recomp/tools/compare_frames.py <folder a> <folder b> --out <folder for difference pictures>
```

Each frame of the recording gives one PNG, of its last copy to the screen. A frame with none repeats the last picture, as a TV would. Frames are at the picture's `EFB_SCALE`, 2 by default; add `EFB_SCALE=1 BUILD=build-1x` to `make` for the Wii's own size, or pass `--resize` to `compare_frames.py`. The `WII_RECOMP_*` switches above work here too.

- `translate/decode.py` names each instruction.
- `translate/functions.py` finds the functions.
- `translate/emit.py` writes the C. Its `REPLACED` list holds system functions we skip on purpose, and `IDLE_LOOPS` holds waits that sleep instead of spinning.
- `runtime/` holds the native side:
  - `main.c`, `loader.c`: start-up and loading the game.
  - `title.c`: loading a title's native code, and checking it belongs to this console and this program.
  - `console.c`: the console folder, and handing over between the Wii Menu and the disc.
  - `drive.c`, `dvd.c`: what's in the drive, and the drive as programs see it. `apploader.c` runs a disc's apploader as its native code.
  - `dispatch.c`, `interp.c`: indirect calls, and an interpreter for any address with no translated code, such as a module loaded at run time. `dispatch` logs each such address once. `test/test_recomp_differential.py` runs every instruction both ways and compares the results. Run it directly to list any disagreements.
  - `threads.c`, `interrupts.c`: the game's threads, interrupts and clocks.
  - `hardware.c`: hardware registers.
  - `input.c`: the Wii Remote and Balance Board.
  - `audio.c`, `ax.c`, `audio_output_mac.c`: sound. `ax.c` does the audio processor's mixing from the game's own voices and command lists, `audio.c` paces it and hands samples to the output thread, and the output file is the only per-platform part.
  - `ios.c`, `devices.c`, `es.c`, `files.c`, `fs.c`: stand-ins for the Wii's system services and storage.
  - `gx_fifo.c`, `gx_vertex.c`, `gx_transform.c`, `gx_light.c`, `gx_texture.c`, `gx_tev.c`, `gx_shader.c`, `gx_copy.c`, `gx_draw.c`, `window.c`: graphics, from command stream to window. The vertex and color stages become small shaders written for each setup the game uses, cached by key in `gx_shader.c`.
  - `dff.c`: reads Dolphin's FIFO recordings, for `tools/replay`.
  - `debug.c`: set `WII_RECOMP_ACTIVITY=1` for per-second counts and a line for each frame over 20 ms, or `WII_RECOMP_DUMP_AFTER=<seconds>` to list what each thread is waiting for.
  - `gaps.c`: every time a game asks for something we don't do, it's logged once as `gap: <id>`, where the ID is its row in `docs/hardware_inventory.md`. The list goes to `gaps.txt` beside the program (`cache/gaps.txt` in a console folder), or to `WII_RECOMP_GAPS`. `WII_RECOMP_GAPS_STRICT=1` stops at the first one, with exit code 12.
