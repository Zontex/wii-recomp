// Knowing whether guest memory was written since the runtime last read it, so a texture or
// display list that hasn't changed needn't be read again to find out.
//
// The idea of a write count per piece of memory comes from WiiCompiled (CREDITS.md), which
// counts the game's cache flushes. Here a watched page is made read-only instead, so the
// first write to it from anywhere faults, and guest_memory.c's handler counts it and opens
// the page again. That needs nothing from the game: unflushed writes, DMA and the runtime's
// own copies are all caught. A page written in frame after frame is left open, and its
// readers go back to looking at the bytes.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define WATCH_NONE (~0ull)  // The range can't be watched: read it.

// Starts watching a range of guest memory, if it isn't already, and returns its count now.
uint64_t watch_range(uint32_t address, uint32_t bytes);
// The range's count, which differs from any earlier one if a byte in it may have changed
// since. WATCH_NONE if part of it isn't watched.
uint64_t watch_count(uint32_t address, uint32_t bytes);
// Opens watched pages before the kernel writes there, which a fault can't catch (fread).
void watch_before_write(const void *host_address, size_t bytes);
// Each new picture, so pages written in frame after frame can be told apart.
void watch_next_frame(void);
// Turned off by WII_RECOMP_NO_WATCH=1, when every reader looks at the bytes.
int watch_enabled(void);
// Writes caught so far, each a fault.
unsigned watch_faults(void);

// For guest_memory.c: starts watching with the host's page size, and takes a fault at an
// offset into the window, returning 0 if it isn't a watched page's.
void watch_start(unsigned long page_bytes);
int watch_fault(uint64_t offset);
