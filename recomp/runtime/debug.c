// Lists the game's threads and where each is waiting. Set WII_RECOMP_DUMP_AFTER to a
// number of seconds to get this report once, while the game runs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "cpu.h"
#include "debug.h"
#include "gaps.h"
#include "gx.h"

enum {
  ACTIVE_THREADS = 0x800000dc, THREAD_STATE = 0x2c8, THREAD_NEXT_ACTIVE = 0x2fc, CONTEXT_LR = 0x84,
  CONTEXT_SRR0 = 0x198, STACK_DEPTH = 12, MAX_THREADS = 32,
};

static int is_guest(uint32_t address) { return (address >= 0x80000000u && address < 0x81800000u) || (address >= 0x90000000u && address < 0x94000000u); }

void debug_threads(void) {
  uint32_t thread = rd32(ACTIVE_THREADS);
  gx_stream_report();
  fprintf(stderr, "interrupts: cause %08x mask %08x msr %08x os masks %08x %08x pe status %04x\n", hw_read(0xcc003000u, 4),
          hw_read(0xcc003004u, 4), cpu.msr, rd32(0x800000c4), rd32(0x800000c8), hw_read(0xcc00100au, 2));
  for (int n = 0; n < MAX_THREADS && is_guest(thread); n++) {
    uint32_t stack = rd32(thread + 4);
    fprintf(stderr, "thread %08x state %u pc %08x link %08x calls:", thread, rd16(thread + THREAD_STATE),
            rd32(thread + CONTEXT_SRR0), rd32(thread + CONTEXT_LR));
    for (int depth = 0; depth < STACK_DEPTH && is_guest(stack); depth++) {
      fprintf(stderr, " %08x", rd32(stack + 4));
      stack = rd32(stack);
    }
    fprintf(stderr, "\n");
    thread = rd32(thread + THREAD_NEXT_ACTIVE);
  }
}

// Linear, as this is only ever reached on the way to something unusual.
void debug_unsupported(const char *id, const char *what, uint32_t value) {
  enum { REMEMBERED = 256 };
  static struct { const char *what; uint32_t value; } seen[REMEMBERED];
  static int count;
  gap_note(id, value);
  for (int i = 0; i < count; i++) {
    if (seen[i].value == value && !strcmp(seen[i].what, what)) return;
  }
  if (count == REMEMBERED) return;
  seen[count].what = what;
  seen[count++].value = value;
  fprintf(stderr, "unsupported graphics: %s = 0x%x (%s)\n", what, value, id);
}

static void report_activity(uint64_t second) {
  static uint64_t last;
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("WII_RECOMP_ACTIVITY") != 0;
  if (second == last || !enabled) return;
  last = second;
  fprintf(stderr, "second %llu: %u draws, %u copies, %u to screen, %u draw done, %u new shaders, slowest %.1f ms, worst frame %.1f ms, passes %u, new pipelines %u, drains %u, waited reads %u\n",
          (unsigned long long)second, gx_stats.draws, gx_stats.copies, gx_stats.to_screen, gx_stats.draw_done,
          gx_stats.new_shaders, (double)gx_stats.slowest_shader_ms, (double)gx_stats.worst_frame_ms, gx_stats.passes,
          gx_stats.new_pipelines, gx_stats.drains, gx_stats.waited_reads);
  gx_stats = (GxStats){0};
}

static uint64_t stage_ticks[STAGE_COUNT];
static uint64_t longest_idle;

void stage_add(int stage, uint64_t ticks) {
  stage_ticks[stage] += ticks;
  if (stage == STAGE_IDLE && ticks > longest_idle) longest_idle = ticks;
}

// WII_RECOMP_FRAME_LOG=1 prints the time since the last frame shown, for each frame, so a
// tool can tell an even 60 from frames that come in fits (recomp/tools/race_bench.py).
void stage_frame(float gap_ms) {
  static int frame_log = -1;
  if (frame_log < 0) frame_log = getenv("WII_RECOMP_FRAME_LOG") != 0;
  if (frame_log) fprintf(stderr, "frame %.2f\n", (double)gap_ms);
  static long switches_before;
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);
  if (gap_ms > 20) {
    double ms = 1 / 60750.0;
    fprintf(stderr, "slow frame %.1f ms: stream %.1f, present %.1f, disc %.1f, audio %.1f, idle %.1f (longest %.1f), preempted %ld\n",
            (double)gap_ms, stage_ticks[STAGE_STREAM] * ms, stage_ticks[STAGE_PRESENT] * ms, stage_ticks[STAGE_DISC] * ms,
            stage_ticks[STAGE_AUDIO] * ms, stage_ticks[STAGE_IDLE] * ms, longest_idle * ms, usage.ru_nivcsw - switches_before);
  }
  switches_before = usage.ru_nivcsw;
  memset(stage_ticks, 0, sizeof stage_ticks);
  longest_idle = 0;
}

void debug_poll(uint64_t seconds_running) {
  static int done;
  guest_memory_report();
  gap_flush();
  report_activity(seconds_running);
  static const char *after;
  static int looked;
  if (!looked) { looked = 1; after = getenv("WII_RECOMP_DUMP_AFTER"); }
  if (done || !after || seconds_running < (uint64_t)atoi(after)) return;
  done = 1;
  debug_threads();
}
