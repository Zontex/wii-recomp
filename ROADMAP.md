# Roadmap: a complete native Wii

The goal: give the system a disc image and it runs with no per-game work, from the real Wii Menu, on real Wii Remotes or a Switch controller, at each game's designed frame rate.

It started with one game, Wii Fit Plus. The second, The Last Story, showed that a runtime built for one game only covers what that game uses, and much of the rest was quietly simplified. So the work is now organised around the whole console, using [Dolphin](https://dolphin-emu.org/)'s source as the map of what a Wii does.

"Out of the box" means no per-game code and no manual steps. It can't be proven for every disc, so it's measured by a growing list of tested titles, and any unsupported feature stops or logs its name so it's fixed once for every game.

## Rules

1. **Nothing fails silently.** Anything unhandled, in any layer, stops or logs its exact name. Never a fake success, and never a quiet simplification.
2. **Fix causes by measurement.** Before changing rendering or timing, find the exact draw or request and compare it with Dolphin. One change at a time, measured.
3. **A safety line.** Every stage ends with Wii Fit Plus holding 60 fps through a fixed benchmark on a quiet machine, with the CPU speed limit recorded, plus a scripted walk through its Body Test.
4. **Measure on a quiet machine.** Same scene, same input, nothing else building. Numbers taken under other conditions aren't compared.
5. **No Nintendo data in the repo.** Discs, Wii storage, recordings and screenshots stay on your machine. Tests use made-up data.
6. **No per-game code.** Anything specific to a title is found from its contents, or lives in a small settings file the pipeline writes.

## Architecture

    disc image / Wii Menu title
          |  prepare (translate, compile, cache)
    native program + title settings
          |
    translated code <-> interpreter fallback
          |
    hardware model: VI PI MI AI DSP SI EXI DI IPC, graphics FIFO
       |                 |                  |
    graphics thread    sound             native IOS
    (Vulkan)           (mixer or DSP)    ES FS DI STM NET USB
                                            |
                                         virtual Bluetooth
                                            |
                                         controller sources:
                                         real Wii Remote, Balance Board,
                                         Switch controllers, keyboard, script

## Stages

Status: **done**, **in progress** or **planned**.

### 0. Stable base (done)

License and credits, a general benchmark tool, and Wii Fit Plus holding 60 fps on the benchmark.

### 1. Inventory (done)

[`docs/hardware_inventory.md`](docs/hardware_inventory.md) lists every instruction, register, mode, format, IOS request and audio program, from Dolphin's source and WiiBrew, with a status for each and the gaps ordered by what real titles use.

### 2. Tools to measure with (done)

- **Draw inspector** (`WII_RECOMP_GX_INSPECT`): lists every draw of one frame with its formats, textures and stage setup, and can skip any range of draws to show which one paints what.
- **FIFO replay** (`recomp/tools/replay`): plays a Dolphin FIFO Player recording through our renderer with no game running, and saves each frame.
- **Picture comparison** (`recomp/tools/compare_frames.py`): a numeric difference and a difference image per frame.
- **CPU test harness** (`test/test_recomp_differential.py`): runs every instruction family through both the translator and the interpreter and compares the registers.

### 3. CPU complete (in progress)

- Done: a PowerPC interpreter that runs any address with no translated code, instead of stopping, and logs it so the translator can pick it up.
- Planned: every instruction form with exact floating-point behaviour, modules loaded at run time (`.rel`, `.rso`) translated ahead of time, self-modifying code sent to the interpreter, BAT and page-table mapping, running the disc's real apploader, and the `--all-code` sweep on by default.

### 4. Graphics model (in progress)

- **4a. The chip as its own thread** on every platform, with real FIFO behaviour: high and low marks, breakpoints, tokens, draw done and idle, timed as the SDK expects. Planned.
- **4b. The complete feature list.** Done: the final color register, stages with no texture, picture copies only reused while they match memory, every color stage comparison mode, texture coordinate generation, indirect stages, Dolphin's vertex loading, stored mip levels with bias, clamp and filters, fog, logic operations, lines and points, the viewport depth range and destination alpha. Still logged as missing: anisotropy, level of detail from diagonals, converting the picture when its pixel format changes, and textures preloaded into texture memory.
- **4c. Reliability and speed.** Planned: shaders compiled ahead of time and saved per title, so a first run doesn't stutter, and lower cost per draw, aimed at heavy scenes of 150,000 draws a second.

Done when every graphics row is done or marked not needed, and recordings from every available title match Dolphin within a set limit.

### 5. Sound (planned)

Identify a game's audio program by its contents and support the main ones, AX in all its versions and the Zelda family, with a DSP interpreter as the fallback for unknown ones. Streamed disc audio, THP video sound and ADPCM. Muting must not change the game's timing.

### 6. Remaining hardware (planned)

Video modes including PAL, 480p and interlaced, memory card, SRAM and clock, GameCube controller ports, region and language from SYSCONF, and widescreen.

### 7. Native IOS (planned)

[`docs/native_ios.md`](docs/native_ios.md) Stages 0 to 2: a device table, request tracing and strict mode, titles, ES, and storage that behaves like a NAND, in a private console folder. Every game's direct start goes through the same path, and replacements of Nintendo's own library code shrink as the hardware and IOS models take their place.

### 8. Real controllers (in progress)

- Done: a virtual Bluetooth chip with a simulated Wii Remote and Classic Controller, driven by each game's own Bluetooth code, including attachment encryption.
- Planned: real Wii Remotes and the Balance Board by passing their reports straight through to the game, on Linux and on macOS if it allows raw access. Switch Pro Controllers and Joy-Cons through SDL2, mapped as a Classic Controller or a Wii Remote and Nunchuk, with motion as tilt and the gyro as the pointer. Four players, and a settings file with sensible defaults.

### 9. The Wii Menu (planned)

[`docs/native_ios.md`](docs/native_ios.md) Stages 3 to 7: menu input, launching titles, the disc channel, region free, and a clean no-network state. One native program per title, handed over with `exec`. Done when the menu, disc channel, a game and back works 20 times in a row with real controllers.

### 10. Disc image in, game out (planned)

`recomp/tools/add_game.py <image>`: extract, translate with discovery on, find system functions by content, compile, compile shaders ahead of time, and add the title to the console folder. A first run of a new disc can start under the interpreter while compiling continues behind it.

### 11. Frame rate (in progress)

Every test scene holds its designed rate with no second below it. Already done for the Wii Fit benchmark on an Intel Mac, and 1080p on the Raspberry Pi 5 through tile-friendly passes. A 60 fps option for 30 fps games, kept per title only where movement, animation and cutscene timing match at both rates.

### 12. Compatibility (planned)

A list of titles, homebrew and SDK demos, each with an automatic script: boots, reaches gameplay, sound, picture comparison and frame rate, run on every change to shared code.

## Milestones so far

- Wii Fit Plus's own code booted to its first picture in a native window on an Intel Mac, then through setup and the Body Test, driven by script.
- Graphics moved from fixed functions to shaders generated per setup, then to a Vulkan renderer.
- The game's idle loop sleeps instead of spinning, and the 60 Hz signal stopped drifting, taking Wii Fit Plus from 56.7 to a steady 60 fps on the Mac.
- On a Raspberry Pi 5, rendering at 5/2 of the Wii's resolution fills 1080p. Passes are kept few and small for its tile-based chip, and empty translucent layers are skipped whole.
- System functions are found by the hashes of their instructions, so a new game needs no address list.
- The Last Story reached gameplay through a virtual Bluetooth chip, using the game's own Wii Remote code.
- An interpreter now catches any code the translator missed, and a differential test checks both against each other for every instruction family.

## Out of scope

Nintendo's online services, GameCube discs, Windows, and anything that needs a console's private keys.
