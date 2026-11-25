# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Wii Recomp statically translates a Wii game's PowerPC code to C and runs it on a native "console" runtime. Python translator in `recomp/translate/`, C runtime in `recomp/runtime/`. Targets: Intel Mac and Raspberry Pi 5, and Windows 10/11 and Linux x86-64 PCs (`docs/windows.md`; PC builds use `-march=x86-64-v3`, and the console stops on a processor without it). Full walkthrough in README.md ("Build and run").

## Build, run, test

```
python3 recomp/tools/add_game.py <image|extracted folder|game ID> [--console DIR]   # -> games/<ID>.wrd
recomp/build/wii-recomp games/<ID>.wrd
```

`add_game.py` is the one-command path: console in `recomp/build`, each game in `recomp/build-<ID>/` (settings.json, profile.txt), always `--all-code --virtual-bluetooth --profile`. The same steps by hand, for one game built beside the console:

```
python3 recomp/tools/extract_disc.py "path/to/game.rvz"                         # -> extracted/
python3 recomp/translate/emit.py extracted/sys/main.dol recomp/build/generated  # --all-code for runtime-built vtables
python3 recomp/translate/build_file.py recomp/build [vulkan,opengl]             # writes build.ninja
mkdir -p recomp/build/obj && (cd recomp/build && ninja)
recomp/build/wii-recomp extracted/sys
python3 -m unittest discover test        # unittest, not pytest
python3 -m ruff check                    # Python lint, bug rules only (ruff.toml); no C linter
```

- Two build tiers with `emit.py --profile`, both compiled at `-O2` so code first run in play doesn't stutter: `cold*.c` also counts its runs via `POLL_COUNTED`, `part*.c` doesn't. The console saves counts to `<title data>/<title ID>/profile.txt` (`title-data/` beside the console, `cache/titles/` in a console folder), with `discovered.txt` and `shaders.bin` (shader recipes, `gx_recipes.c`, packed into the `.wrd`) beside it; rerunning `add_game.py` promotes every function that ran (one-off code at a level's start caused hitches when only the most-run share was promoted).
- Graphics work is measured on recordings, not by driving a game: `record <frames> <file>` through `WII_RECOMP_CONTROL` (or `WII_RECOMP_RECORD=<frame>:<frames>:<file>`) saves a scene, and `python3 recomp/tools/scene_bench.py <folder> [--reference DIR | --save-reference DIR]` times every recording in a folder and compares pictures. Recordings hold game data: keep them outside the repo. Memory is taken once a frame, so they suit timing and comparing our own versions, not cycle-exact checks.
- The console links SDL3 for controllers: `brew install sdl3` on the Mac, `recomp/tools/sdl3_setup.sh --prefix ~/.local` on the Pi (no root); `build_file.py` finds either.
- No CMake/top-level Makefile: `build_file.py` generates the ninja file. Pi vs Mac flags (cortex-a76, `EFB_SCALE=5/2`, Vulkan-only on Pi) all live there.
- Output is `wii-recomp` (same for every game) + `title.dylib`/`title.so` (the game). Headers in `INTERFACE_HEADERS` (`cpu.h guest_memory.h title.h modules.h`) are hashed; a title built against different versions is refused, so rebuild both after touching them.
- Builds compile with `-w`; warnings won't surface. `-fsigned-char -ffp-contract=off` keep Arm matching the Mac — don't drop them.
- Tests use made-up data. C tests need clang, graphics replay tests need `VULKAN_SDK`, game-dependent tests skip without `extracted/`.
- GameCube discs are an experiment (README "GameCube discs"): the console runs one in `gamecube_mode` (`loader.c`), with the disc drive (`di.c`), controller (`si.c`) and memory card (`exi.c`) at their registers, and `emit.py` has a GameCube title count its cycles (`CHARGED`), which the console adds up (`title_counts_work`) and `WII_RECOMP_FRAME_LOG=2` prints as `work`.
- Controllers are set in `controllers.ini` beside the console (written on first run, every default commented). Remotes get a Nunchuk by default; `WII_RECOMP_ATTACHMENT=classic` puts a Classic Controller in, which makes Wii Fit Plus hide its pointer.
- On Linux, `sudo recomp/tools/remotes_setup.sh` once sets up real remotes (through the kernel's HID) and Switch controllers; only its older `--sockets` way needs rerunning after every build. Delete `shader-cache/` beside the binary to reset shaders.

## Rules (from ROADMAP.md)

- **No per-game code.** Anything title-specific is derived from the title's contents or a small generated settings file. Keep tools and engine fixes general.
- **Nothing fails silently.** Unhandled features log `gap: <id>` where the id is a row in `docs/hardware_inventory.md` — add a row rather than inventing ids or faking success. `WII_RECOMP_GAPS_STRICT=1` stops at the first gap (exit 12).
- **Fix causes by measurement**, comparing against Dolphin, one change at a time.
- **Performance bar:** Wii Fit Plus holds 60 fps on the fixed benchmark (`recomp/tools/perf_run.py`, Mac only — reads `pmset`) plus a scripted Body Test walk. Only compare numbers from a quiet machine: screen unlocked, CPU speed limit 100, nothing building.
- **A game started from a background service runs at low priority** (20 on a Mac, against 31 to 47 for one started from Terminal), and every other program takes the processor from it: frames double and it looks like a slow game. `python3 recomp/tools/machine_state.py` says whether the machine is fit for timing, `autoplay.py --timed` ends a run as VOID when it wasn't, and a game for someone to play is started from Terminal (a `.command` script through `open -a Terminal`).
- **No Nintendo data in the repo.** Discs (`*.rvz *.iso *.wbfs *.wad *.wrd`), `extracted/`, NAND/keys (`wii/`, `nand.bin`, `keys.bin`), recordings, screenshots, autoplay images and `pi_walk/ref/` images stay local. Never `git add -f` these.
- The project is GPL-3.0-or-later. Code taken from Dolphin (GPL-2.0-or-later) or WiiCompiled (GPL-3.0) must be listed in CREDITS.md with the file it came from.

## Git

- Work on a feature branch and open a PR to `main`; don't commit to `main` directly.
- Commit subjects are plain-English sentences describing the outcome, no prefixes (e.g. "Hold 60 fps on the Pi's step-off screen"); bodies are short prose paragraphs.
- Docs are written in plain, jargon-light prose — match that voice.
