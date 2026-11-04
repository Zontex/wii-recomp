// See gaps.h. Any thread can call these: the game, the graphics thread and the sound
// thread all meet gaps.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "gaps.h"

enum { SLOTS = 2048, MOST = SLOTS * 3 / 4 };

typedef struct {
  const char *id;
  uint32_t detail, lr;  // The game's link register when it was first met, to find the caller.
  uint64_t count;
} Gap;

static Gap gaps[SLOTS];
static int used, changed, full;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t hash(const char *id, uint32_t detail) {
  uint32_t value = 2166136261u ^ detail;
  for (; *id; id++) value = (value ^ (uint8_t)*id) * 16777619u;
  return value;
}

// The slot for this ID and detail, claimed if it's new. 0 once the table is full.
static Gap *find(const char *id, uint32_t detail, int *fresh) {
  for (uint32_t i = hash(id, detail);; i++) {
    Gap *gap = &gaps[i % SLOTS];
    if (!gap->id) {
      if (used >= MOST) return 0;
      *gap = (Gap){strdup(id), detail, cpu.lr, 0};  // Copied, as callers may build the ID.
      used++;
      *fresh = 1;
      return gap;
    }
    if (gap->detail == detail && !strcmp(gap->id, id)) return gap;
  }
}

void gap_flush(void) {
  static const char *path;
  static int looked;
  if (!looked) { looked = 1; path = getenv("WII_RECOMP_GAPS"); }
  if (!path || !changed) return;
  char temporary[4096];
  snprintf(temporary, sizeof temporary, "%s.new", path);
  FILE *file = fopen(temporary, "w");
  if (!file) return;
  pthread_mutex_lock(&lock);
  changed = 0;
  fprintf(file, "# id detail count first_lr\n");
  for (int i = 0; i < SLOTS; i++) {
    if (gaps[i].id) fprintf(file, "%s 0x%x %llu 0x%08x\n", gaps[i].id, gaps[i].detail, (unsigned long long)gaps[i].count, gaps[i].lr);
  }
  if (full) fprintf(file, "limit.gap_table 0x0 1 0x00000000\n");
  pthread_mutex_unlock(&lock);
  fclose(file);
  rename(temporary, path);
}

static void record(const char *id, uint32_t detail, int say) {
  static int registered;
  int fresh = 0;
  pthread_mutex_lock(&lock);
  if (!registered) { registered = 1; atexit(gap_flush); }
  Gap *gap = find(id, detail, &fresh);
  if (gap) gap->count++;
  else if (!full) { full = 1; changed = 1; fprintf(stderr, "gap: too many kinds to keep, so later ones aren't listed\n"); }
  if (fresh) changed = 1;
  pthread_mutex_unlock(&lock);
  if (!fresh) return;
  if (say) fprintf(stderr, "gap: %s 0x%x\n", id, detail);
  if (getenv("WII_RECOMP_GAPS_STRICT")) {
    gap_flush();
    fprintf(stderr, "stop: %s isn't done yet (see docs/hardware_inventory.md)\n", id);
    exit(12);
  }
}

void gap_hit(const char *id, uint32_t detail) { record(id, detail, 1); }
void gap_note(const char *id, uint32_t detail) { record(id, detail, 0); }
