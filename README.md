# Wii Recomp

A static recompiler that runs Nintendo Wii games as ordinary native programs. It translates a game's PowerPC code to C ahead of time, compiles it with clang, and links it to a runtime that stands in for the console: CPU, graphics chip, sound, IOS and controllers.

The goal is a complete native Wii: give it a disc image and it runs, from the real Wii Menu if you like, on real Wii Remotes or a Switch controller, at the game's own frame rate, on a Mac or a Raspberry Pi 5. It isn't there yet. See [`ROADMAP.md`](ROADMAP.md).

This repo holds code only. You need your own copy of each game. Disc images, extracted files, translated code, Wii storage and saves stay on your machine and must never be committed.

## Status

| Title | State |
| --- | --- |
| Wii Fit Plus (RFPE01) | Runs through setup and the Body Test with simulated controllers. Holds 60 fps on an Intel Mac. On a Raspberry Pi 5 at 1080p, the board scene holds 60 fps and the setup screens 45 to 58. |
| The Last Story (SLSEXJ) | Boots to gameplay with a simulated Wii Remote and Classic Controller, through the game's own Bluetooth code. Some surfaces draw wrong, and heavy scenes are slow. |
| Wii Menu 4.3 | Translates and runs to its own error screen, waiting on a native IOS. |

Real controllers aren't supported yet, and this doesn't run every Wii game. Unsupported hardware features are logged by name, so they can be fixed once for all games.

## How it works

1. **Find the code.** `recomp/translate` follows branches and pointers from the entry point, with no symbols needed. `--all-code` also sweeps for code reached only through tables built at run time.
2. **Find system functions by their contents.** The few system functions the runtime replaces are found by hashes of their instructions, so no game needs its own address list. The build stops rather than guess.
3. **Write C.** Each instruction becomes literal C, and clang does the optimizing. Code the translator never saw runs through a PowerPC interpreter instead of stopping the game, and is logged.
4. **Run.** `recomp/runtime` models the hardware the game talks to: the graphics command stream and its timing, the Wii's color stages as shaders drawn with Vulkan or OpenGL, the AX sound mixer, IOS services for disc, storage and Bluetooth, and a simulated Wii Remote with attachments.

The game's own code, including its operating system and thread scheduler, runs almost unchanged.

## Build and run

You need `clang`, `ninja`, Python 3 and GLFW (`brew install glfw` on a Mac). Vulkan needs the Vulkan loader and shaderc, and MoltenVK on a Mac. Then extract your disc and build:

```sh
python3 recomp/tools/extract_disc.py "path/to/game.rvz"
python3 recomp/translate/emit.py extracted/sys/main.dol recomp/build/generated
python3 recomp/translate/build_file.py recomp/build
mkdir -p recomp/build/obj
(cd recomp/build && ninja)
recomp/build/wii-recomp extracted/sys path/to/Wii/NAND
```

`extract_disc.py` reads ISO, RVZ and WBFS images with [nodtool](https://github.com/encounter/nod), and downloads a pinned, checksummed copy if it isn't installed. The program's second argument is a Wii storage folder, such as Dolphin's NAND folder after its system update. It's only read. Saves go to `recomp/build/saves`. Without a storage folder, the console's settings files such as SYSCONF are missing, and games that need them won't start.

A full build takes several minutes. Add `--all-code` to `emit.py` for games that build their C++ method tables at run time, such as The Last Story.

See [`recomp/README.md`](recomp/README.md) for controls, settings, benchmarking and a map of the source.

## Tests

```sh
python3 -m unittest discover test
```

The tests use made-up data, with no Nintendo files. Set `VULKAN_SDK` to a folder holding the Vulkan libraries to also run the graphics replay tests, which draw scenes headless and compare pixels. Translator tests that need a real game are skipped when `extracted/` is missing.

## Documents

- [`ROADMAP.md`](ROADMAP.md): the stages towards a complete native Wii.
- [`docs/hardware_inventory.md`](docs/hardware_inventory.md): every part of the console, with what's done, partial or missing.
- [`docs/native_ios.md`](docs/native_ios.md): the plan for a native IOS that runs the real Wii Menu.

## License

GPL-2.0-or-later. See [`LICENSE`](LICENSE). Parts of the graphics model follow [Dolphin](https://dolphin-emu.org/)'s behaviour, as listed in [`CREDITS.md`](CREDITS.md).

This project isn't affiliated with or endorsed by Nintendo. Wii is a trademark of Nintendo.
