# Native IOS plan

Run the real Wii Menu and the games it launches on our own native IOS, on any machine the runtime supports. Starting a game directly stays the default until the menu route has proven as reliable.

## Where we are

- The menu runs on the Intel Mac for 60+ seconds without crashing and stops at its own error screen.
- Its log says `ES error. [-1017] iplNandManager.cpp line: 72`. Our `es.c` only ever returns 0, so that number didn't come from us. The menu's own ES library made it, most likely after a request "succeeded" with zeros. Faking success hid the real failure, which is why rule 2 exists.
- `/dev/di` and `/dev/fs` do real work. `/dev/usb/oh1/57e/305` has a virtual Bluetooth chip with a simulated Wii Remote. Everything else answers "fine" or never replies.

## Rules

1. **Native, not emulated.** IOS is plain C on the host. No Starlet emulation, no running Nintendo's IOS binaries, no crypto keys. Nothing here needs a key.
2. **Never fake success.** A request we don't handle is logged and answered with an error real IOS could give. Every "fine" must be a deliberate decision with a comment saying why.
3. **Driven by traces.** We implement what the menu actually asks for, in the order it asks. The request lists below come from public documentation and need confirming against WiiBrew and Dolphin's behaviour. Dolphin is GPL, as this project is, so its code may be reused with credit in `CREDITS.md`.
4. **One job per file.** Each device is its own file behind one interface. Guest memory is touched only in device files, so the title and file logic can be tested without a Wii program.
5. **No Nintendo data in the repo.** Tests use made-up fixtures. The source NAND is only ever read.
6. **Wii Fit keeps working.** Every stage ends with the existing Wii Fit and The Last Story checks passing.

## How it fits together

    guest program
         | shared memory, doorbell, interrupt
    ios.c              the kernel: file descriptors, reply queue, interrupts
         |
    devices.c          table of IosDevice { name, match, open, close, request }
         |
    es*.c  fs.c  dvd.c  stm.c  bluetooth.c  net.c  crypto.c  sdio.c
         |
    titles.c           TMD and ticket parsing, title list. No guest memory.
    files.c            the console's storage. No guest memory.
    launch.c           hands over to another program

### New and changed files in `recomp/runtime`

| File | Job |
|---|---|
| `ios.c` | Keeps the kernel. A full reply queue stops with a clear message instead of dropping the reply. Each descriptor knows its device. |
| `devices.c` | Becomes a table. Names match exactly, with explicit prefixes only where IOS has them, so `/dev/net/kd/request` stops counting as `/dev/net`. |
| `ios_vectors.h` | One helper for reading ioctlv inputs and outputs and checking their sizes, replacing the copies in `es.c`, `fs.c` and `bluetooth.c`. |
| `ios_trace.c` | Logs each request once per device and code, with the result. `WII_RECOMP_IOS_TRACE=all` logs everything, and `WII_RECOMP_IOS_STRICT=1` stops at the first unhandled request. |
| `titles.c` | Reads TMDs, tickets, the title list and shared contents from storage. |
| `es.c` | Dispatch table only, keyed by request number, with argument counts checked in one place. |
| `es_titles.c` | Title lists, stored TMDs, ticket views. |
| `es_content.c` | Opening, reading, seeking and closing title contents. |
| `es_launch.c` | Who we are, data folders, launching titles. |
| `launch.c` | Saves state and replaces the running program. |
| `stm.c` | Power, reset, LED and event hook. |
| `net.c` | The `kd`, `ncd`, `wd`, `ip` and `ssl` devices, reporting "no network". |
| `crypto.c`, `sdio.c` | SHA, AES and SD card, only if a trace shows they're needed. |

### Decision: one program per title, handed over with exec

`ES_LaunchTitle` flushes storage, writes a small hand-over record to the console folder, and calls `execv` on the target title's own native program.

- **Why:** every title is compiled for the same guest addresses, so they can't share one `function_table`. Each program also keeps state in dozens of runtime statics. `exec` gives every title the clean start a real Wii gives it, with nothing to reset by hand and no way for one title to leak into the next.
- **Cost:** the window closes and reopens, which looks like the black screen a real Wii shows. If that looks bad, a small parent process can own the window later without changing IOS.
- **Rejected:** loading titles as shared libraries, because every runtime static would need a reset and any miss becomes a rare, confusing bug.

A title with no native program is refused with a clear message on the menu's side, never a crash. Channels and games must be translated ahead of time.

### Decision: one private console folder

    console/
      nand/                    writable copy of the Wii's storage
      programs/<high>-<low>/   native program plus its disc folder
      handover.bin             what the next program needs to know

- `recomp/tools/prepare_console.py <NAND> <console folder>` copies the NAND once, then translates and builds each listed title with `prepare_title.py`, `emit.py`, `build_file.py` and `ninja`. The source NAND is only read.
- **Why a copy:** today's split of "read from the NAND, write to saves" can't delete a file that exists only in the NAND. `fs.c` reports success and the file is still there. The menu deletes and renames constantly, so a real writable copy fixes this whole class of bug.
- Wii Fit's direct start, with an optional storage folder, stays as it is.

### Decision: TMD drives the start-up state

`set_up_low_memory` in `loader.c` hard-codes IOS 36. It should read the required IOS from the title's TMD (offset 0x184), so the menu sees the IOS it expects. `game_title_high` and `game_data_folder` stay as they are. Each native program is one title, so they're still correct. Start-up gains one step: read `handover.bin` if it exists.

## Stages

Each stage ends with something visible. Stop and re-plan if a trace contradicts a later stage.

### Stage 0: Foundation, no new behaviour

Starts from a committed base, with no other work in progress in `recomp/runtime`.

- Add the device table, `ios_vectors.h` and `ios_trace.c`, and split `es.c`.
- Change unknown requests to an error plus a log line, but keep today's "fine" for Wii Fit requests that the trace shows it relies on, each with a comment.
- Stop dropping replies when the queue is full.

Done when: Wii Fit and The Last Story behave exactly as before, and a menu run prints every request it makes.

### Stage 1: Past the error screen

- Build `titles.c` with tests.
- Build the ES requests the trace shows, expected to be most of: `GetTitleCount`/`GetTitles` (0x0e, 0x0f), `GetViewCount`/`GetViews` (0x12, 0x13), `GetTMDViewSize`/`GetTMDViews` (0x14, 0x15), `GetStoredTMDSize`/`GetStoredTMD` (0x3a, 0x3b), `GetStoredContentCount`/`GetStoredContents` (0x38, 0x39), `GetTitleContentsCount`/`GetTitleContents` (0x10, 0x11), `GetTitleDir` (0x1d), `GetTitleID` (0x20), `SetUID` (0x21), `GetDeviceID` (0x07), `GetConsumption` (0x16).
- Build `es_content.c`: 0x09, 0x0a, 0x0b, 0x23, 0x24.
- Add `prepare_console.py` with the NAND copy.
- Use the TMD's IOS version in low memory.

Done when: the error screen is gone, `nanderr.log` gets no new lines, and the menu reaches its next real screen, likely the warning or the channel grid.

### Stage 2: Storage that behaves like a NAND

- Honour open modes, so a read-only open can't write and a missing file gives the right error.
- Store owner, group and permissions, with `SetAttr` and `GetAttr` real, and report real usage from `GetUsage` and `GetStats`.
- Write files atomically: temporary file, sync, rename. A power cut can't corrupt a save.
- Decide folder and file limits by trace. The real 12-character name limit stays.

Done when: `test/c/test_storage.c` covers each of these, and the menu's settings survive pulling the power.

### Stage 3: The menu responds to input

- Build the menu with `--virtual-bluetooth`, so its own Wii Remote code talks to `bluetooth.c`. This replaces the `KPADRead` stub, and it means we never need to map the older KPAD layout.
- Make sure `Bluetooth start` isn't replaced for the menu, and that `bluetooth_register_remote` works with the menu's own use of SYSCONF.
- Pointer data comes from a mouse or the control pipe on the Mac, and from the real remote later.
- Build `stm.c` for the Home button's reset and power paths.

Done when: from the control pipe, the pointer moves, a channel opens, and settings can be changed and saved.

### Stage 4: Launching titles

- Build `launch.c` and `es_launch.c`.
- Trace how the menu starts a channel and handle it.
- Handle `OSReturnToMenu`, which on a real Wii is also a title launch, and read `/shared2/sys/NANDBOOTINFO` if the trace uses it.
- Check that a title returning to the menu keeps saves and settings.
- Add the "not translated" refusal.

Done when: a system channel opens from the menu and returns 20 times in a row with no lost saves. First target is whichever channel in the NAND is simplest, because this NAND has no Wii Fit Channel installed.

### Stage 5: Disc channel and Wii Fit

- Give `dvd.c` cover status, inserted-disc state, and the disc-only ES requests (0x19, 0x1a, 0x1b, 0x1c).
- Present a translated game as the inserted disc, from `programs/`.
- Show the real banner from the game's `opening.bnr`.
- Find how the menu boots a disc. It may run the apploader itself rather than call `ES_LaunchTitle`, so we may need a system-function rule in `system_functions.json` that hands over at that point. This is the biggest unknown left in the plan.

Done when: from the menu, the disc channel shows Wii Fit Plus, it starts, a full activity plays, and it returns to the menu with the save intact.

### Stage 6: Region free

Apply it in one place, after tracing what the menu compares. The expected source is the disc header against `setting.txt` and SYSCONF.

Done when: a disc from another region starts from the EU menu, using a system-function rule or a data answer, never a change to the private program file.

### Stage 7: Complete and calm

- `net.c` gives clean "no network" answers, so the menu's WiiConnect24 work stops retrying. Online services stay out.
- Build `crypto.c` and `sdio.c` only if traces ask for them.
- Run the menu for an hour and look for repeated errors, leaks and slow frames.
- Measure on the Raspberry Pi 5.

Done when: an hour of normal use has no unhandled requests in strict mode.

## Testing

- **Unit tests in `test/c`**, using the existing `test/harness.h`: `test_titles.c`, `test_es.c`, `test_ios_kernel.c` and more of `test_storage.c`. `recomp/tools/make_test_nand.py` writes a made-up NAND with fake titles, so bad sizes, missing tickets and truncated TMDs are all tested with no Nintendo data.
- **Every request answers correctly when badly formed:** wrong vector counts, short buffers and null pointers give an error, never a crash.
- **Regression:** after each stage, compare `signatures.py` output for Wii Fit and The Last Story with the saved baseline, and run Wii Fit to its save message.
- **Trace check:** the same menu run twice should give the same request list, so a changed list in review means a changed behaviour.
- **Hours-long run:** Stage 7's run, repeated before each release.

## Risks

| Risk | Answer |
|---|---|
| The menu boots discs without `ES_LaunchTitle` | Trace in Stage 5, then hand over at the function that does it |
| A channel relies on hardware we don't have, such as the wireless chip | Refuse that channel. Each is optional |
| Menu speed on the Pi | Unknown. It has 24,014 functions and slow frames on the Mac. Measure on the Pi once the menu runs |
| Build time and size per title | The menu is 102 MB of objects and takes minutes. One program per title keeps rebuilds separate |

## Out of scope

Nintendo's online services, GameCube mode, running untranslated discs (that needs a JIT), Wii Shop purchases, real title installs and updates, and anything that needs the console's keys. A Switch port needs no IOS work, because it would run the same Linux program. It does need its own `-mcpu` value in `build_file.py`.

## Done when

- From a cold start: the health warning, the channel grid, the disc channel, Wii Fit Plus, a full activity, then back to the menu, with no keyboard.
- Saves and settings survive 20 round trips and a power cut.
- An hour in strict mode shows no unhandled requests.
- Wii Fit started directly still works exactly as it did.
- All of it builds from `prepare_console.py` and a NAND folder, with nothing private in the repo.
