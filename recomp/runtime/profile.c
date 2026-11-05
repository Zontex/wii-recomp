// See profile.h. A function built to count adds one each time it starts and each time a
// loop in it goes round (emit.py), so a count is roughly the work done there. The file is
// one function a line, "<address> <count>", after a line naming the program it belongs
// to, and holds the total over every run of that program. A function in a module the
// program loads at run time is "<module>+<offset into its code> <count>", as the module
// can be loaded anywhere.
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "profile.h"
#include "title.h"

enum { SAVE_EVERY_SECONDS = 10 };

static uint64_t *earlier;  // Counts from earlier runs, indexed like title->functions.
static uint64_t **earlier_in_module;  // The same for each module's functions, by title->modules.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char file[4096];

int title_data_path(const char *name, char *path, size_t size) {
  const char *folder = getenv("WII_RECOMP_TITLE_DATA");
  if (!folder || !title) return 0;
  char own[4096];
  snprintf(own, sizeof own, "%s/%08x-%08x", folder, title->title_high, title->title_low);
  if ((mkdir(folder, 0755) && errno != EEXIST) || (mkdir(own, 0755) && errno != EEXIST)) {
    fprintf(stderr, "title data: can't make %s: %s\n", own, strerror(errno));
    return 0;
  }
  return snprintf(path, size, "%s/%s", own, name) < (int)size;
}

static int find_function(uint32_t address) {
  int low = 0, high = title->function_count - 1;
  while (low <= high) {
    int middle = (low + high) / 2;
    uint32_t at = title->functions[middle].address;
    if (at == address) return middle;
    if (at < address) low = middle + 1;
    else high = middle - 1;
  }
  return -1;
}

static int find_module_function(const Module *module, uint32_t offset) {
  int low = 0, high = module->function_count - 1;
  while (low <= high) {
    int middle = (low + high) / 2;
    uint32_t at = module->functions[middle].offset;
    if (at == offset) return middle;
    if (at < offset) low = middle + 1;
    else high = middle - 1;
  }
  return -1;
}

// Where an earlier count for "<module>+<offset>" goes, or 0.
static uint64_t *module_slot(const char *place) {
  const char *plus = strrchr(place, '+');
  if (!plus) return 0;
  uint32_t offset = (uint32_t)strtoul(plus + 1, 0, 16);
  for (int m = 0; m < title->module_count; m++) {
    const Module *module = title->modules[m];
    if (!earlier_in_module[m] || strlen(module->name) != (size_t)(plus - place) || strncmp(module->name, place, plus - place)) continue;
    int at = find_module_function(module, offset);
    return at >= 0 ? &earlier_in_module[m][at] : 0;
  }
  return 0;
}

static void load_earlier(void) {
  FILE *in = fopen(file, "r");
  if (!in) return;
  char line[128];
  unsigned long long check = 0;
  if (!fgets(line, sizeof line, in) || sscanf(line, "# program %llx", &check) != 1 || check != title->program_check) {
    fprintf(stderr, "profile: %s is for another program, so it starts again\n", file);
    fclose(in);
    return;
  }
  char place[256];
  unsigned long long count;
  int dropped = 0;
  while (fscanf(in, "%255s %llu", place, &count) == 2) {
    uint64_t *slot = 0;
    if (strchr(place, '+')) slot = module_slot(place);
    else {
      int at = find_function((uint32_t)strtoul(place, 0, 16));
      if (at >= 0) slot = &earlier[at];
    }
    if (slot) *slot += count;
    else dropped++;  // Only if the program's functions were found differently.
  }
  fclose(in);
  if (dropped) fprintf(stderr, "profile: %d functions in %s aren't in this build\n", dropped, file);
}

void profile_save(void) {
  if (!earlier || !earlier_in_module) return;
  pthread_mutex_lock(&lock);
  char partial[sizeof file + 16];
  snprintf(partial, sizeof partial, "%s.partial", file);
  FILE *out = fopen(partial, "w");
  if (!out) {
    fprintf(stderr, "profile: can't write %s: %s\n", partial, strerror(errno));
    pthread_mutex_unlock(&lock);
    return;
  }
  fprintf(out, "# program %016" PRIx64 "\n", title->program_check);
  for (int i = 0; i < title->function_count; i++) {
    uint64_t total = earlier[i] + title->counts[i];  // A torn read is off by one at most.
    if (total) fprintf(out, "%08x %" PRIu64 "\n", title->functions[i].address, total);
  }
  for (int m = 0; m < title->module_count; m++) {
    const Module *module = title->modules[m];
    if (!earlier_in_module[m]) continue;
    for (int i = 0; i < module->function_count; i++) {
      uint64_t total = earlier_in_module[m][i] + module->counts[i];
      if (total) fprintf(out, "%s+%x %" PRIu64 "\n", module->name, module->functions[i].offset, total);
    }
  }
  if (fclose(out) || rename(partial, file)) fprintf(stderr, "profile: can't save %s: %s\n", file, strerror(errno));
  pthread_mutex_unlock(&lock);
}

static void *keep_saving(void *unused) {
  (void)unused;
  for (;;) {
    sleep(SAVE_EVERY_SECONDS);
    profile_save();
  }
  return 0;
}

void profile_start(void) {
  if (!title->counts || !title_data_path("profile.txt", file, sizeof file)) return;
  earlier = calloc((size_t)title->function_count, sizeof *earlier);
  earlier_in_module = calloc((size_t)title->module_count + 1, sizeof *earlier_in_module);
  if (!earlier || !earlier_in_module) return;
  for (int m = 0; m < title->module_count; m++) {
    const Module *module = title->modules[m];
    if (module->counts) earlier_in_module[m] = calloc((size_t)module->function_count + 1, sizeof **earlier_in_module);
  }
  load_earlier();
  atexit(profile_save);
  pthread_t thread;
  pthread_create(&thread, 0, keep_saving, 0);
  pthread_detach(thread);
}
