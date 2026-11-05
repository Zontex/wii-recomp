// A timeline of what each thread does across a few frames, for finding what waits on
// what: WII_RECOMP_TIMELINE=<first frame>:<frames>:<file> writes it as a Chrome trace,
// which Perfetto (ui.perfetto.dev) or chrome://tracing shows. Marks cost a load and a
// branch when off.
#pragma once

extern int timeline_on;
void timeline_mark(const char *what, char phase);  // 'B' begins a span, 'E' ends it, 'i' is a moment.
void timeline_frame_shown(void);                    // Counts frames, to know when to start and stop.

#define TIMELINE_BEGIN(what) do { if (timeline_on) timeline_mark(what, 'B'); } while (0)
#define TIMELINE_END(what) do { if (timeline_on) timeline_mark(what, 'E'); } while (0)
#define TIMELINE_MOMENT(what) do { if (timeline_on) timeline_mark(what, 'i'); } while (0)
