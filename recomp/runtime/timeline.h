// A timeline of what each thread does across a few frames, for finding what waits on
// what: WII_RECOMP_TIMELINE=<first frame>:<frames>:<file> writes it as a Chrome trace,
// which Perfetto (ui.perfetto.dev) or chrome://tracing shows. Marks cost a load and a
// branch when off.
#pragma once
#include <stdint.h>
#include <time.h>

extern int timeline_on;
void timeline_mark(const char *what, char phase);  // 'B' begins a span, 'E' ends it, 'i' is a moment.
// The same, with the game's callers at that moment (zero ends the list early), written
// with the mark as "calls": which of the game's waits a span of its thread belongs to.
enum { TIMELINE_CALLS = 4 };
void timeline_mark_calls(const char *what, char phase, const uint32_t *calls);
void timeline_request(unsigned frames, const char *file);  // Any thread: from the next frame shown.
void timeline_frame_shown(void);                    // Counts frames, to know when to start and stop.

#define TIMELINE_BEGIN(what) do { if (timeline_on) timeline_mark(what, 'B'); } while (0)
#define TIMELINE_END(what) do { if (timeline_on) timeline_mark(what, 'E'); } while (0)
#define TIMELINE_MOMENT(what) do { if (timeline_on) timeline_mark(what, 'i'); } while (0)

// The host's steady clock, in nanoseconds: what the marks are timed by, and what the graphics
// thread holds pictures for the screen's fields by.
static inline uint64_t host_nanoseconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}
