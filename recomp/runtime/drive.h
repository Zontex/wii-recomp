// The disc drive's contents. A disc is either a recompiled disc image (recomp/tools/
// make_disc.py), which carries its own native code, or an extracted disc's system
// folder, used while developing. Programs reach it through /dev/di (dvd.c); the console
// reads it directly to boot a disc.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Puts a disc in the drive: an image file, or a folder holding boot.bin, fst.bin and
// main.dol beside a files folder. A folder whose boot.bin has no game ID, as a system
// title's has, leaves the drive empty. Returns 0, leaving it empty, if it can't be read.
int drive_insert(const char *path);
void drive_eject(void);
int drive_has_disc(void);

// Reads the data partition, at its own offsets. Returns 0 if any of it can't be read.
int drive_read(uint64_t offset, uint32_t bytes, uint8_t *out);

// Reads the disc's unencrypted area: its header, partition table and region. Like a real
// drive, only the start of the disc can be read this way. Returns 0 outside it.
int drive_read_raw(uint64_t offset, uint32_t bytes, uint8_t *out);

// The disc's ticket and TMD. Returns the bytes read, or 0 if it has none.
size_t drive_ticket(uint8_t *out, size_t limit);
size_t drive_tmd(uint8_t *out, size_t limit);

// Writes the image's native code for this platform to `folder`, under a name that
// changes with its contents, and puts that path in `path`. Returns 0 if the disc has
// none for this platform, or isn't an image.
int drive_native(const char *folder, char *path, size_t limit);

// The shader recipes the image carries (gx_recipes.c), in a buffer the caller frees, or 0.
void *drive_shaders(size_t *bytes);

// This platform's name in an image, such as darwin-x86_64.
const char *drive_platform(void);
