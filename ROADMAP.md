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

Each stage is **done**, **in progress** or **planned**, and each item is ticked when it's done.

### 0. Stable base (done)

- [x] License and credits
- [x] A general benchmark tool
- [x] Wii Fit Plus holding 60 fps on the benchmark

### 1. Inventory (done)

- [x] [`docs/hardware_inventory.md`](docs/hardware_inventory.md): every instruction, register, mode, format, IOS request and audio program, from Dolphin's source and WiiBrew, with a status for each
- [x] The gaps ordered by what real titles use

### 2. Tools to measure with (done)

- [x] **Draw inspector** (`WII_RECOMP_GX_INSPECT`): lists every draw of one frame with its formats, textures and stage setup, and can skip any range of draws to show which one paints what
- [x] **FIFO replay** (`recomp/tools/replay`): plays a Dolphin FIFO Player recording through our renderer with no game running, and saves each frame
- [x] **Recording from the console** (`runtime/gx_record.c`): the control pipe's `record <frames> <file>`, or `WII_RECOMP_RECORD`, saves frames of any game in the same format, so a scene needs no menus to reach again
- [x] **Scene bench** (`recomp/tools/scene_bench.py`): plays every recording in a folder, with processor and graphics chip time per frame, the chip's costliest steps, and a picture comparison against an earlier run
- [x] **Picture comparison** (`recomp/tools/compare_frames.py`): a numeric difference and a difference image per frame
- [x] **CPU test harness** (`test/test_recomp_differential.py`): runs every instruction family through both the translator and the interpreter and compares the registers
- [x] **Menu benchmark** (`recomp/tools/menu_bench.py`): tours 16 Wii Menu screens and reports frame rate and the worst frame for each
- [x] **Gap counter** (`runtime/gaps.c`, `recomp/tools/gap_report.py`): names every request the runtime can't do by its inventory row, and ranks them across runs and titles

### 3. CPU complete (in progress)

- [x] A PowerPC interpreter that runs any address with no translated code, instead of stopping, and logs it
- [x] Code the interpreter ran is translated on the next build
- [x] Modules loaded at run time (`.rso`, such as the Wii Menu's web library) found in the title's contents and translated ahead of time, used only if their code in memory matches
- [x] The disc's own apploader, translated with the game and run natively when the Wii Menu starts a disc
- [ ] Every instruction form with exact floating-point behaviour
- [ ] `.rel` modules translated ahead of time
- [ ] Self-modifying code sent to the interpreter
- [ ] BAT and page-table mapping
- [x] The `--all-code` sweep on by default, through `add_game.py`

### 4. Graphics model (in progress)

**4a. The chip as its own thread**

- [x] Commands on their own thread on Linux, except in games using FIFO breakpoints
- [ ] The same on every platform, with real FIFO behaviour: high and low marks, breakpoints, tokens, draw done and idle, timed as the SDK expects

**4b. The complete feature list**

- [x] The final color register, stages with no texture, and every color stage comparison mode
- [x] Picture copies only reused while they match memory
- [x] Texture coordinate generation and indirect stages
- [x] Dolphin's vertex loading
- [x] Stored mip levels with bias, clamp and filters
- [x] Fog, logic operations, lines and points
- [x] The viewport depth range and destination alpha
- [ ] Anisotropy
- [ ] Level of detail from diagonals
- [ ] Converting the picture when its pixel format changes
- [ ] Textures preloaded into texture memory

**4c. Reliability and speed**

- [x] Vulkan as the default renderer, with OpenGL as a fallback
- [x] Compiled shaders and pipelines kept between runs, so a scene stutters only the first time it's ever drawn
- [x] On the Mac, pictures reach the window on a queue of their own, so macOS holding the window's pictures skips pictures instead of freezing the game for up to a second
- [ ] Shaders compiled ahead of time from the disc alone, so even a game's very first run doesn't stutter. Until then, recipes recorded by earlier runs and carried in the disc image build everything they saw before the first picture, on any computer (stage 10)
- [ ] Lower cost per draw, aimed at heavy scenes of 150,000 draws a second
- [ ] **GPU pipe writes compiled to direct calls.** Each store the game makes to the pipe (`0xCC008000`) goes through `hw_write`, its register checks and a byte loop before the commands are read back. In three steps, each measured before the next:
  1. The translator spots stores to the pipe's fixed address and calls a native pipe write directly. Same bytes in the same order, so exact.
  2. Bursts are read as they're made, without the copy into the game's FIFO memory, where the game uses no FIFO breakpoints and never reads its FIFO back.
  3. Whole `GXBegin` to `GXEnd` sequences fill vertex buffers directly, checked at run time against the vertex format they assume, falling back to the command reader when it differs.

Done when every graphics row is done or marked not needed, and recordings from every available title match Dolphin within a set limit.

### 5. Sound (in progress)

- [x] Identify a game's audio program by its contents
- [ ] AX in all its versions, and the Zelda family. AX is done: equal to the real program on Mario Kart Wii recordings, the versions no recording reaches checked against Dolphin's code. The Zelda family's C version waits for a Super Mario Galaxy 2 recording to be checked against
- [x] A DSP interpreter as the fallback for unknown programs
- [ ] Streamed disc audio, THP video sound and ADPCM
- [x] Muting leaves the game's timing unchanged

### 6. Remaining hardware (planned)

- [x] GameCube controller ports, answering "no controller"
- [ ] Video modes including PAL, 480p and interlaced
- [ ] Memory card, SRAM and clock
- [ ] Region and language from SYSCONF
- [ ] Widescreen

### 7. Native IOS (in progress)

From [`docs/native_ios.md`](docs/native_ios.md) Stages 0 to 2.

- [x] Request tracing, and unknown requests answered with an error and a log line, never a fake success
- [x] Titles, tickets and ES requests as the Wii Menu uses them
- [x] A private console folder holding its own copy of the Wii storage, the Wii Menu and a drive slot (`recomp/tools/prepare_console.py`)
- [x] Clean "no network" answers for WiiConnect24, the SD card slot and sockets
- [x] Each IOS version's own memory at the top of MEM2, so a game's heap gets the full arena it was sized for
- [x] Your own Wii's BootMii NAND backup and keys decrypted into the storage the console reads (`recomp/tools/import_nand.py`, `wii/`)
- [ ] Miis and saves from that backup used by every title, so a game picks up where it was left on the Wii
- [ ] Storage that behaves like a NAND: open modes, owners and permissions, real usage, and writes a power cut can't corrupt
- [ ] Every game's direct start through the same path
- [ ] Fewer replacements of Nintendo's own library code, as the hardware and IOS models take their place

### 8. Real controllers (in progress)

- [x] A virtual Bluetooth chip, driven by each game's own Bluetooth code, with four remote slots and the Balance Board's
- [x] A simulated Wii Remote with an IR camera for the pointer, a Classic Controller with attachment encryption, and a simulated Balance Board
- [x] Real Wii Remotes and the Balance Board passed straight through to the game's own code: raw Bluetooth sockets on Linux, IOKit's HID on the Mac, the board in its own slot, remotes keeping their player number, waking with a button, and staying connected across the console's hand-over. Tested with stand-ins and with Wii Fit Plus on `WII_RECOMP_REMOTES=loopback`
- [ ] Real remotes and a real board tried on the Pi and the Mac, with the report rate measured
- [ ] Switch Pro Controllers and Joy-Cons through SDL2, as a Classic Controller or a Wii Remote and Nunchuk, with motion as tilt and the gyro as the pointer
- [x] The keyboard steers the Classic Controller's left stick with the arrow keys
- [ ] Keys for the Classic Controller's L, R, ZL and ZR, for drifting and items
- [ ] Four players, and a settings file with sensible defaults

### 9. The Wii Menu (in progress)

From [`docs/native_ios.md`](docs/native_ios.md) Stages 3 to 7.

- [x] The menu's own Wii Remote code takes input
- [x] The health warning, first-time setup, the channel grid, Wii Options, Wii Settings, Data Management and the message board
- [x] 60 fps on every one of 16 screens on an Intel Mac
- [x] A recompiled disc image in the drive shows its banner, and Start hands over to the game, running natively
- [x] Region free, without changing the disc
- [ ] Back to the menu from a game: built, not yet tried end to end
- [ ] Launching channels
- [ ] The real clock
- [ ] Menu, disc channel, a game and back, 20 times in a row with real controllers

### 10. Disc image in, game out (in progress)

- [x] One console program for every title, loading each title's translated code as a library
- [x] Recompiled disc images (`recomp/tools/make_disc.py`): the disc with its native code for each platform, in one file
- [x] `recomp/tools/add_game.py <image>`: extract, translate with discovery on, find system functions by content, compile, pack a recompiled disc image and put it in a console's drive, in one command. Code the interpreter ran comes back as function starts on the next build
- [x] Two build tiers. A first build compiles everything about five times faster, and each function counts how often it runs; running `add_game.py` again compiles the functions that did the work for speed, from counts added up over every run. On a Raspberry Pi 5, Wii Fit Plus goes from an extracted disc to a playable image in under a minute and a half instead of over seven minutes, the second build takes about a minute, and its Body Test walk runs as fast as a build compiled entirely for speed
- [x] Shaders built before the first picture from the recipes every earlier run of the game recorded, which the disc image carries (4c)
- [ ] A first run of a new disc starts under the interpreter while compiling continues behind it
- [x] The files the Wii Menu makes on a new Wii's first start (WiiConnect24's settings, lists and mailboxes, and an empty Mii database), made in the save folder when missing
- [ ] REL modules, the format most games load the bulk of their code in through the SDK's `OSLink`, translated ahead of time like the Wii Menu's RSO modules. Mario Kart Wii runs nearly all of its game from one, `StaticR.rel`, under the interpreter today
- [ ] Module code in both build tiers, its counts kept by module name and offset so they hold wherever the module is loaded

### 11. Frame rate (in progress)

- [x] Wii Fit Plus at a steady 60 fps on the benchmark on an Intel Mac
- [x] The Wii Menu at 60 fps across 16 screens on an Intel Mac
- [x] A covered window no longer slowed to 10 fps by macOS
- [x] 1080p on the Raspberry Pi 5, through tile-friendly passes
- [x] The Wii's memory as one window at a fixed address: a load or store is a few instructions with no pointer to load, and the stack skips the hardware check. Wii Fit Plus's translated code shrank by about 40%
- [ ] Every Wii Fit Plus scene at 60 fps on the Pi: the step-off screen and the Body Test hold 60, and a step-on transition drops to about 32
- [ ] A 60 fps option for 30 fps games, kept per title only where movement, animation and cutscene timing match at both rates
- [ ] Mario Kart Wii at a steady 60 fps through the menus and a race on an Intel Mac. The menus hold 60 already; the Single Player screen runs at about 28 while its code is interpreted

### 12. Compatibility (planned)

- [ ] A list of titles, homebrew and SDK demos
- [ ] Mario Kart Wii from a fresh console: Grand Prix, Time Trials with ghosts, VS and Battle, saving and reloading, sound in time, and back to the Wii Menu. Online play stays out of scope; the game should say it can't connect
- [x] Automatic play for any title (`recomp/tools/autoplay/`): scripted sessions per game that read the screen and act, with Wii Fit Plus's first-time setup as the first
- [x] Compatibility runs (`recomp/tools/compat_run.py`): every game played unattended by the generic explorer, given a tier from measured signals, with the gaps ranked across all of them
- [ ] An automatic script for each: boots, reaches gameplay, sound, picture comparison and frame rate
- [ ] Run on every change to shared code

## Milestones so far

- Wii Fit Plus's own code booted to its first picture in a native window on an Intel Mac, then through setup and the Body Test, driven by script.
- Graphics moved from fixed functions to shaders generated per setup, then to a Vulkan renderer.
- The game's idle loop sleeps instead of spinning, and the 60 Hz signal stopped drifting, taking Wii Fit Plus from 56.7 to a steady 60 fps on the Mac.
- On a Raspberry Pi 5, rendering at 5/2 of the Wii's resolution fills 1080p. Passes are kept few and small for its tile-based chip, and empty translucent layers are skipped whole.
- System functions are found by the hashes of their instructions, so a new game needs no address list.
- The Last Story reached gameplay through a virtual Bluetooth chip, using the game's own Wii Remote code.
- An interpreter now catches any code the translator missed, and a differential test checks both against each other for every instruction family.
- The recompiled Wii Menu runs at 60 fps through its screens, and starts Wii Fit Plus natively from a recompiled disc image in its drive.
- On the Mac, the game no longer freezes for a second when macOS holds on to the window's pictures.
- Real Wii Remotes and the Balance Board can drive a game's own Bluetooth code, and the Wii's memory became one window at a fixed address, making each load or store a few instructions.
- One command turns a disc into a playable image. Code that has barely run compiles quickly and counts its runs, and the next build compiles what did the work for speed: Wii Fit Plus builds in under a minute and a half on a Raspberry Pi 5.
- Mario Kart Wii, from the user's own disc, reaches its menus on a console that has never run the Wii Menu, once the files a new Wii makes on first start are there.

## Out of scope

Nintendo's online services, GameCube discs, Windows, and anything that needs a console's private keys.
