// See profile.h. A function built to count adds one each time it starts and each time a
// loop in it goes round (emit.py), so a count is roughly the work done there. The file is
// one function a line, "<address> <count>", after a line naming the program it belongs
// to, and holds the total over every run of that program.
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
  unsigned address;
  unsigned long long count;
  int dropped = 0;
  while (fscanf(in, "%x %llu", &address, &count) == 2) {
    int at = find_function(address);
    if (at >= 0) earlier[at] += count;
    else dropped++;  // Only if the program's functions were found differently.
  }
  fclose(in);
  if (dropped) fprintf(stderr, "profile: %d functions in %s aren't in this build\n", dropped, file);
}

void profile_save(void) {
  if (!earlier) return;
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
  if (!earlier) return;
  load_earlier();
  atexit(profile_save);
  pthread_t thread;
  pthread_create(&thread, 0, keep_saving, 0);
  pthread_detach(thread);
}
