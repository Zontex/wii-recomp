// Records timeline marks (timeline.h) into a fixed ring, from any thread, and writes them
// out once the asked-for frames have passed. WII_RECOMP_TIMELINE asks at start; the control
// pipe's timeline <frames> <file> asks from the next frame shown.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "timeline.h"

enum { MOST_MARKS = 1 << 20 };
typedef struct { uint64_t nanoseconds; const char *what; int thread; char phase; } Mark;

int timeline_on;
static Mark *marks;
static atomic_uint used;
static atomic_int threads;
static _Thread_local int thread_number;
static unsigned frame, first_frame, last_frame;
static char path[4096];

__attribute__((constructor)) static void timeline_start(void) {
  const char *asked = getenv("WII_RECOMP_TIMELINE");
  int consumed = 0;
  unsigned count = 0;
  if (!asked || sscanf(asked, "%u:%u:%n", &first_frame, &count, &consumed) != 2 || !consumed || !count) return;
  snprintf(path, sizeof path, "%s", asked + consumed);
  last_frame = first_frame + count;
  marks = calloc(MOST_MARKS, sizeof *marks);
}

static uint64_t now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void timeline_mark(const char *what, char phase) {
  if (!thread_number) thread_number = atomic_fetch_add(&threads, 1) + 1;
  unsigned at = atomic_fetch_add_explicit(&used, 1, memory_order_relaxed);
  if (at < MOST_MARKS) marks[at] = (Mark){now(), what, thread_number, phase};
}

static void write_out(void) {
  timeline_on = 0;
  FILE *to = fopen(path, "w");
  if (!to) { fprintf(stderr, "timeline: can't write %s\n", path); return; }
  unsigned count = atomic_load(&used);
  if (count > MOST_MARKS) count = MOST_MARKS;
  uint64_t start = count ? marks[0].nanoseconds : 0;
  fprintf(to, "{\"traceEvents\":[\n");
  for (unsigned i = 0; i < count; i++) {
    const Mark *m = &marks[i];
    fprintf(to, "%s{\"name\":\"%s\",\"ph\":\"%c\",\"ts\":%.3f,\"pid\":1,\"tid\":%d%s}\n", i ? "," : "", m->what, m->phase,
            (double)(m->nanoseconds - start) / 1000.0, m->thread, m->phase == 'i' ? ",\"s\":\"t\"" : "");
  }
  fprintf(to, "]}\n");
  fclose(to);
  fprintf(stderr, "timeline: wrote %u marks to %s\n", count, path);
}

// From the control pipe's thread: start at the next frame shown, for a count of frames.
static _Atomic unsigned requested;
static char requested_path[4096];

void timeline_request(unsigned frames, const char *file) {
  if (!frames || atomic_load(&requested) || timeline_on) return;
  snprintf(requested_path, sizeof requested_path, "%s", file);
  atomic_store(&requested, frames);
}

void timeline_frame_shown(void) {
  unsigned frames = atomic_exchange(&requested, 0);
  if (frames) {
    if (!marks) marks = calloc(MOST_MARKS, sizeof *marks);
    atomic_store(&used, 0);
    snprintf(path, sizeof path, "%s", requested_path);
    first_frame = frame + 1;
    last_frame = first_frame + frames;
  }
  if (!marks) return;
  frame++;
  if (frame == first_frame) timeline_on = 1;
  if (timeline_on) timeline_mark("frame shown", 'i');
  if (frame == last_frame) write_out();
}
