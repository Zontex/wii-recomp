// The console: one folder holding the Wii's storage, the native code of each installed
// title, and a drive slot. It starts the Wii Menu, or the disc in the slot, and hands
// over between them the way a real Wii does, by starting afresh.
//
//   console/
//     nand/                         the Wii's storage, written by prepare_console.py
//     titles/00000001-00000002/     the Wii Menu: main.dol, boot.bin, fst.bin, title library
//     drive/                        a disc image here is a disc in the drive
//     cache/                        native code from discs, and compiled shaders
#pragma once
#include "loader.h"

// Sets up the console in `folder` and fills in what to boot: the Wii Menu, or with
// `boot_disc` the disc in the drive. Stops with a clear message if it can't.
void console_start(const char *program, const char *folder, int boot_disc, Boot *boot);

// Whether the program was started as a console, rather than straight from a disc.
int console_running(void);

// Starts the console again, booting the disc in the drive or the Wii Menu. Never returns.
void console_restart(int boot_disc);

// Called with an address the running title has no native code for. If it's where the
// disc's program starts, the Wii Menu has loaded the game and is starting it, so the
// console hands over to the disc. Returns only if it doesn't.
void console_hand_over(uint32_t address);
