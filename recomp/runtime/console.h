// The console: one folder holding the Wii's storage, the native code of each installed
// title, and a drive slot. It starts the Wii Menu, or the disc in the slot, and hands
// over between them the way a real Wii does, by starting afresh.
//
//   console/
//     nand/                         the Wii's storage, written by prepare_console.py
//     titles/00000001-00000002/     the Wii Menu: main.dol, boot.bin, fst.bin, title library
//     titles/<high>-<low>/          any other title installed with its native code, the same way
//     drive/                        a disc image here is a disc in the drive
//     cache/                        native code from discs, and compiled shaders
#pragma once
#include "loader.h"

#define CONSOLE_MENU 0x0000000100000002ull
#define CONSOLE_DISC 0ull  // As a title to start: the disc in the drive.

// Sets up the console in `folder` and fills in what to boot: the Wii Menu, the disc in
// the drive (CONSOLE_DISC), or an installed title. Stops with a clear message if it can't.
void console_start(const char *program, const char *folder, uint64_t start, Boot *boot);

// Whether a title is installed with native code, so the console can start it.
int console_has_title(uint64_t title);

// Whether the program was started as a console, rather than straight from a disc.
int console_running(void);

// Remembers how the program was started, so a hot reset can start it the same way.
void console_remember(char **arguments);

// Turns the console off: writes out open files, then exits. `how` is for the log.
void console_power_off(const char *how) __attribute__((noreturn));

// A hot reset, which starts the whole Wii again: the Wii Menu on a console, or the same
// title when it was started on its own. Never returns.
void console_hot_reset(void) __attribute__((noreturn));

// Starts the console again with a title: the Wii Menu, the disc in the drive
// (CONSOLE_DISC) or an installed title. Never returns.
void console_restart(uint64_t start) __attribute__((noreturn));

// Asks for the disc to change, from any thread: `path` is put in, or with 0 the drive is
// emptied. In a console, a name without a folder is an image on its shelf. The change
// happens in console_update_drive, which the game's thread calls.
void console_request_disc(const char *path);
void console_update_drive(void);
// Throws the disc out now, from the game's thread, as when a program ejects it.
void console_eject(void);

// Called with an address the running title has no native code for. If it's where the
// disc's program starts, the Wii Menu has loaded the game and is starting it, so the
// console hands over to the disc. Returns only if it doesn't.
void console_hand_over(uint32_t address);
