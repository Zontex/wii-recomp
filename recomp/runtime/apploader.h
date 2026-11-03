// A disc's apploader, run by the Wii Menu to load the game. It's disc code, so it runs
// as the disc's own native code, translated ahead of time with the game: see emit.py.
#pragma once
#include <stdint.h>

// Called with an address nothing is translated for. Returns 1 if it's in the apploader of
// the disc in the drive, whose native code is then made ready, and 0 if not. Stops if
// the code in memory isn't the disc's apploader.
int apploader_bind(uint32_t address);
