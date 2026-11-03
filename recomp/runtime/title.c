// Loads a title's native code. The console is one program; each title's translated code
// is a library beside it or inside its disc image, and it only runs on the console it
// was built for, with the program it was translated from.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "title.h"

const Title *title;

uint64_t title_check(const uint8_t *data, unsigned long length) {
  uint64_t value = 0xcbf29ce484222325ull;
  for (unsigned long i = 0; i < length; i++) value = (value ^ data[i]) * 0x100000001b3ull;
  return value;
}

const Title *title_open(const char *path) {
  void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    fprintf(stderr, "stop: can't load the title's native code: %s\n", dlerror());
    exit(10);
  }
  const Title *found = dlsym(library, TITLE_SYMBOL);
  if (!found) {
    fprintf(stderr, "stop: %s isn't a title's native code\n", path);
    exit(10);
  }
  if (found->interface != TITLE_INTERFACE) {
    fprintf(stderr, "stop: %s was built for another version of the console (%016llx, this is %016llx); build it again\n", path,
            (unsigned long long)found->interface, (unsigned long long)TITLE_INTERFACE);
    exit(10);
  }
  return found;
}

void title_load(const char *path) {
  title = title_open(path);
  fprintf(stderr, "title %08x-%08x: %d native functions\n", title->title_high, title->title_low, title->function_count);
}

void title_verify_program(const uint8_t *data, unsigned long length) {
  uint64_t check = title_check(data, length);
  if (check != title->program_check) {
    fprintf(stderr, "stop: the program isn't the one this native code was translated from (%016llx, expected %016llx)\n",
            (unsigned long long)check, (unsigned long long)title->program_check);
    exit(10);
  }
}
