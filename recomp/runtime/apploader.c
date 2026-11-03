// Runs a disc's apploader as native code. The Wii Menu reads the apploader into memory
// and calls it; the first call reaches here, the disc's native library is opened beside
// the menu's, and the apploader's functions are handed to dispatch once its bytes in
// memory are checked to be exactly what was translated.
//
// An apploader also copies a little of its own code elsewhere and runs it there: its
// init, main and close "return" through a stub copied below the Wii Menu, which clears
// the instruction cache and then really returns. A copy is bound to the native code of
// the function it was copied from, once its words are checked to be that function's, up
// to its return, and to hold no branch that would land somewhere else from the new place.
//
// The drive's disc stays the same while the console runs, so binding happens once.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "apploader.h"
#include "drive.h"
#include "modules.h"
#include "title.h"

enum { RETURN = 0x4e800020, MAX_COPY_WORDS = 64 };

static const Title *disc;
static int bound;

static int in_apploader(uint32_t address) { return address - disc->apploader_at < disc->apploader_bytes; }

static int bind_apploader(void) {
  uint8_t *code = host(disc->apploader_at), *last = host(disc->apploader_at + disc->apploader_bytes - 1);
  if (!code || last != code + disc->apploader_bytes - 1 || title_check(code, disc->apploader_bytes) != disc->apploader_check) {
    fprintf(stderr, "stop: the apploader in memory at %08x isn't the one on the disc\n", disc->apploader_at);
    exit(4);
  }
  for (int i = 0; i < disc->apploader_function_count; i++) dispatch_register(disc->apploader_functions[i].address, disc->apploader_functions[i].run);
  fprintf(stderr, "apploader: %d native functions ready at %08x\n", disc->apploader_function_count, disc->apploader_at);
  bound = 1;
  return 1;
}

// Whether a word means the same at `copy` as at `original`: anything but a relative
// branch, or one that stays inside the copied words.
static int same_from_anywhere(uint32_t word, uint32_t offset, uint32_t words) {
  uint32_t primary = word >> 26;
  if (primary == 18 || primary == 16) {  // b and bc: absolute ones mean the same anywhere.
    if (word & 2) return 1;
    if (word & 1) return 0;  // A call would return to an address that differs.
    int32_t displacement = primary == 18 ? (int32_t)(word << 6) >> 6 & ~3 : (int16_t)(word & 0xfffc);
    int64_t target = (int64_t)offset + displacement;
    return target >= 0 && target < (int64_t)words * 4;
  }
  return 1;
}

// A function of the apploader, copied whole to `address`.
static int bind_copy(uint32_t address) {
  for (int i = 0; i < disc->apploader_function_count; i++) {
    uint32_t original = disc->apploader_functions[i].address, words = 0;
    while (words < MAX_COPY_WORDS && in_apploader(original + words * 4) && rd32(original + words * 4) != RETURN) words++;
    if (words == MAX_COPY_WORDS || !in_apploader(original + words * 4)) continue;
    words++;  // Up to and including its return.
    int same = 1;
    for (uint32_t w = 0; same && w < words; w++) {
      uint32_t word = rd32(original + w * 4);
      same = host(address + w * 4) && rd32(address + w * 4) == word && same_from_anywhere(word, w * 4, words);
    }
    if (!same) continue;
    dispatch_register(address, disc->apploader_functions[i].run);
    fprintf(stderr, "apploader: its code from %08x, copied to %08x, runs natively there\n", original, address);
    return 1;
  }
  return 0;
}

int apploader_bind(uint32_t address) {
  static int tried;
  if (!tried) {
    tried = 1;
    char path[PATH_MAX];
    if (drive_has_disc() && getenv("WII_RECOMP_CACHE") && drive_native(getenv("WII_RECOMP_CACHE"), path, sizeof path)) disc = title_open(path);
  }
  if (!disc || !disc->apploader_bytes) return 0;
  if (!bound) return in_apploader(address) && bind_apploader();
  return !in_apploader(address) && bind_copy(address);
}
