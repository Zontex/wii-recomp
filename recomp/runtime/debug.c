// Lists the game's threads and where each is waiting. Set WII_RECOMP_DUMP_AFTER to a
// number of seconds to get this report once, while the game runs.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // For dladdr and a timer aimed at one thread (the slow frames' samples).
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#define SLOW_SAMPLES 1
#include <dlfcn.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include "audio.h"
#include "cpu.h"
#include "debug.h"
#include "gaps.h"
#include "gx.h"
#include "hardware.h"

// The game's OS keeps its live threads in a list from ACTIVE_THREADS, and the one running in
// CURRENT_THREAD. A thread is its saved context, then the OS's own fields.
enum {
  ACTIVE_THREADS = 0x800000dc, CURRENT_THREAD = 0x800000e4, THREAD_STATE = 0x2c8, THREAD_PRIORITY = 0x2d0,
  THREAD_BASE_PRIORITY = 0x2d4, THREAD_NEXT_ACTIVE = 0x2fc, CONTEXT_LR = 0x84, CONTEXT_SRR0 = 0x198, STACK_DEPTH = 20,
  MAX_THREADS = 32,
};

static int is_guest(uint32_t address) { return (address >= 0x80000000u && address < 0x81800000u) || (address >= 0x90000000u && address < 0x94000000u); }

void debug_threads(void) {
  uint32_t thread = rd32(ACTIVE_THREADS);
  gx_stream_report();
  fprintf(stderr, "interrupts: cause %08x mask %08x msr %08x os masks %08x %08x pe status %04x\n", hw_read(0xcc003000u, 4),
          hw_read(0xcc003004u, 4), cpu.msr, rd32(0x800000c4), rd32(0x800000c8), hw_read(0xcc00100au, 2));
  for (int n = 0; n < MAX_THREADS && is_guest(thread); n++) {
    // The thread running now has its registers in the processor, not in its context.
    int running = thread == rd32(CURRENT_THREAD);
    uint32_t stack = running ? cpu.r[1] : rd32(thread + 4);
    fprintf(stderr, "thread %08x state %u priority %d (base %d) pc %08x link %08x calls:", thread, rd16(thread + THREAD_STATE),
            (int)rd32(thread + THREAD_PRIORITY), (int)rd32(thread + THREAD_BASE_PRIORITY), rd32(thread + CONTEXT_SRR0), rd32(thread + CONTEXT_LR));
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

// Once a second, what the sound ring went through (audio.h): always in a second where
// sound was lost, and every second with WII_RECOMP_ACTIVITY set. A game's sound that
// stutters with nothing printed here stuttered before it reached the ring.
static void report_sound(uint64_t second) {
  static uint64_t last;
  static int every = -1;
  if (every < 0) every = getenv("WII_RECOMP_ACTIVITY") != 0;
  if (second == last) return;
  last = second;
  AudioCounts counts;
  audio_take_counts(&counts);
  int lost = counts.ran_dry || counts.dropped || counts.let_go;
  if (!lost && (!every || counts.fill_low == UINT32_MAX)) return;
  fprintf(stderr, "sound: second %llu, the ring held %u to %u samples, ran dry %u times (%u samples of silence), %u samples dropped as it was full, "
          "the clock let time go %u times\n", (unsigned long long)second, counts.fill_low == UINT32_MAX ? 0 : counts.fill_low, counts.fill_high,
          counts.ran_dry, counts.silence, counts.dropped, counts.let_go);
}

static uint64_t stage_ticks[STAGE_COUNT];
static uint64_t longest_idle;
// Counted on the game's thread and read on the graphics thread, a frame at a time: a count
// that lands in the next frame's line now and then doesn't matter for what they are for.
static unsigned notes[NOTE_COUNT];
void stage_note(int kind) { notes[kind]++; }

void stage_add(int stage, uint64_t ticks) {
  stage_ticks[stage] += ticks;
  if (stage == STAGE_IDLE && ticks > longest_idle) longest_idle = ticks;
}

// A function built with WII_RECOMP_EMIT_TRACE calls this as it starts: who called it, with
// what, on which of the game's threads, and when (milliseconds). The first 4,000 calls
// from the count in WII_RECOMP_TRACE_FROM on are printed.
void debug_trace_call(unsigned address) {
  static long seen, from = -1;
  if (from < 0) from = getenv("WII_RECOMP_TRACE_FROM") ? atol(getenv("WII_RECOMP_TRACE_FROM")) : 0;
  if (seen++ < from || seen > from + 4000) return;
  fprintf(stderr, "call %08x from %08x r3 %08x r4 %08x thread %08x at %.2f\n", address, cpu.lr, cpu.r[3], cpu.r[4], rd32(CURRENT_THREAD),
          (double)time_base() / 60750.0);
}

// With the frame log on, the time since the last picture reached the window: what the
// eye gets, where "frame" is when the game finished each.
void stage_shown(void) {
  static uint64_t last;
  static int frame_log = -1;
  if (frame_log < 0) frame_log = getenv("WII_RECOMP_FRAME_LOG") ? atoi(getenv("WII_RECOMP_FRAME_LOG")) : 0;
  if (!frame_log) return;
  uint64_t now = time_base();
  if (last) fprintf(stderr, "shown %.2f\n", (double)(now - last) / 60750.0);
  last = now;
}

#ifdef SLOW_SAMPLES
// Where the game's thread was, sampled by a timer's signal on that thread (see debug.h).
// Samples are kept for one frame. A slow frame's are printed in the order they were taken,
// as runs of one of the game's own threads (its system's "current thread"): how many
// samples, then the places most of them fell in, a place being 64 bytes of code. So the
// line says which of the game's threads had the processor when, and what it was doing.
enum { MOST_SAMPLES = 2048, SAMPLE_NANOSECONDS = 100000, PLACES_SHOWN = 4, PLACE_SHIFT = 6, SHORT_RUN = 3 };
static uintptr_t samples[MOST_SAMPLES];
static uint32_t sample_threads[MOST_SAMPLES];
enum { CHAIN_DEPTH = 7, CHAIN_RUN = 20 };
static uint32_t sample_chains[MOST_SAMPLES][CHAIN_DEPTH];  // The game's callers: its link register, then up its stack.
static _Atomic unsigned samples_taken;
enum { YIELD_BYTES = 48 };
static uintptr_t yield_start = 1;  // The C library's sched_yield.
static int samples_all;  // Every frame's samples are counted too (see count_all).

volatile int stage_samples_paused;

static void take_sample(int signal_number, siginfo_t *info, void *context) {
  (void)signal_number, (void)info;
  if (stage_samples_paused) return;
  ucontext_t *thread = context;
  unsigned at = atomic_fetch_add_explicit(&samples_taken, 1, memory_order_relaxed);
  if (at >= MOST_SAMPLES) return;
#ifdef __x86_64__
  samples[at] = (uintptr_t)thread->uc_mcontext.gregs[REG_RIP];
  // Inside the C library's sched_yield, which keeps nothing on the stack, the place it was
  // called from says more: that is what the thread is spinning on.
  if (samples[at] - yield_start < YIELD_BYTES) samples[at] = *(const uintptr_t *)thread->uc_mcontext.gregs[REG_RSP];
  // With the graphics thread sampled, the same goes for any place outside the console's own code
  // whose caller is on top of the stack: the C library's copying and comparing keep nothing
  // there either, and "memcpy" alone doesn't say whose copying it is.
  extern char __executable_start, etext;
  if (samples_all == 2 && (samples[at] < (uintptr_t)&__executable_start || samples[at] >= (uintptr_t)&etext)) {
    uintptr_t caller = *(const uintptr_t *)thread->uc_mcontext.gregs[REG_RSP];
    if (caller >= (uintptr_t)&__executable_start && caller < (uintptr_t)&etext) samples[at] = caller;
  }
#else
  samples[at] = (uintptr_t)thread->uc_mcontext.pc;
#endif
  sample_threads[at] = rd32(CURRENT_THREAD);
  uint32_t frame = cpu.r[1];
  sample_chains[at][0] = cpu.lr;
  for (int depth = 1; depth < CHAIN_DEPTH; depth++) {
    frame = is_guest(frame) ? rd32(frame) : 0;
    sample_chains[at][depth] = is_guest(frame) ? rd32(frame + 4) : 0;
  }
}

void stage_sample_thread(int graphics) {
  const char *asked = getenv("WII_RECOMP_SLOW_SAMPLES");
  if (!asked || graphics != !strcmp(asked, "graphics")) return;
  samples_all = graphics ? 2 : !strcmp(asked, "all");
  yield_start = (uintptr_t)sched_yield;
  struct sigaction action = {0};
  action.sa_sigaction = take_sample;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGPROF, &action, 0);
  struct sigevent event = {0};
  event.sigev_notify = SIGEV_THREAD_ID;
  event.sigev_signo = SIGPROF;
  event._sigev_un._tid = (pid_t)syscall(SYS_gettid);
  timer_t timer;
  struct itimerspec every = {{0, SAMPLE_NANOSECONDS}, {0, SAMPLE_NANOSECONDS}};
  if (timer_create(CLOCK_MONOTONIC, &event, &timer) || timer_settime(timer, 0, &every, 0)) {
    fprintf(stderr, "slow samples: no timer for the game's thread, so there are none\n");
  }
}

static int by_place(const void *a, const void *b) {
  uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
  return x < y ? -1 : x > y;
}

// A place as " <file>+<offset> <count>": the program or the title's library, and how far in.
static void print_place(uintptr_t address, unsigned count) {
  Dl_info where = {0};
  const char *file = dladdr((void *)address, &where) && where.dli_fname ? where.dli_fname : "?";
  const char *name = strrchr(file, '/');
  fprintf(stderr, " %s+%lx %u", name ? name + 1 : file, (unsigned long)(address - (uintptr_t)where.dli_fbase), count);
}

// The places most of the samples from first up to last fell in.
static void print_places(unsigned first, unsigned last) {
  static uintptr_t places[MOST_SAMPLES];
  static struct { uintptr_t place; unsigned count; } counted[MOST_SAMPLES];
  unsigned kept = last - first, different = 0;
  for (unsigned i = 0; i < kept; i++) places[i] = samples[first + i] >> PLACE_SHIFT;
  qsort(places, kept, sizeof *places, by_place);
  for (unsigned i = 0; i < kept; i++) {
    if (different && counted[different - 1].place == places[i]) counted[different - 1].count++;
    else counted[different].place = places[i], counted[different++].count = 1;
  }
  for (int shown = 0; shown < PLACES_SHOWN; shown++) {
    unsigned most = 0;
    for (unsigned i = 1; i < different; i++) if (counted[i].count > counted[most].count) most = i;
    if (!different || !counted[most].count) break;
    uintptr_t address = counted[most].place << PLACE_SHIFT;
    print_place(address, counted[most].count);
    counted[most].count = 0;
  }
}

// WII_RECOMP_SLOW_SAMPLES=all also counts every frame's samples by place, and every ten
// seconds of frames prints the places most of them fell in: where the time of an ordinary
// frame goes.
enum { TABLE_PLACES = 1 << 15, ALL_SHOWN = 240, ALL_EVERY_FRAMES = 600 };
static struct { uintptr_t place; unsigned count; } table[TABLE_PLACES];
static void count_all(unsigned kept) {
  static unsigned frames, total, lost;
  for (unsigned i = 0; i < kept; i++) {
    uintptr_t place = samples[i] >> PLACE_SHIFT;
    unsigned at = (unsigned)(place * 0x9e3779b97f4a7c15ull >> 49), tries = 0;
    while (table[at].count && table[at].place != place && ++tries < 64) at = (at + 1) & (TABLE_PLACES - 1);
    if (table[at].count && table[at].place != place) { lost++; continue; }
    table[at].place = place, table[at].count++;
  }
  total += kept;
  if (++frames < ALL_EVERY_FRAMES) return;
  fprintf(stderr, "all samples: %u in %u frames (%u not counted):", total, frames, lost);
  for (int shown = 0; shown < ALL_SHOWN; shown++) {
    unsigned most = 0;
    for (unsigned i = 1; i < TABLE_PLACES; i++) if (table[i].count > table[most].count) most = i;
    if (!table[most].count) break;
    uintptr_t address = table[most].place << PLACE_SHIFT;
    print_place(address, table[most].count);
    table[most].count = 0;
  }
  fprintf(stderr, "\n");
  memset(table, 0, sizeof table);
  frames = total = lost = 0;
}

// A frame is over. A slow one's samples are printed.
static void samples_frame(int slow) {
  unsigned taken = atomic_exchange_explicit(&samples_taken, 0, memory_order_relaxed);
  if (samples_all && taken) count_all(taken < MOST_SAMPLES ? taken : MOST_SAMPLES);
  if (!slow || !taken) return;
  unsigned kept = taken < MOST_SAMPLES ? taken : MOST_SAMPLES;
  fprintf(stderr, "slow samples: %u", taken);
  for (unsigned first = 0; first < kept;) {
    unsigned last = first + 1;
    while (last < kept && sample_threads[last] == sample_threads[first]) last++;
    fprintf(stderr, " | thread %08x %u:", sample_threads[first], last - first);
    if (last - first >= SHORT_RUN) print_places(first, last);
    if (last - first >= CHAIN_RUN) {  // The callers seen most often in this run.
      unsigned best = first, best_count = 0;
      for (unsigned i = first; i < last; i++) {
        unsigned count = 0;
        for (unsigned j = first; j < last; j++) count += !memcmp(sample_chains[i] + 1, sample_chains[j] + 1, sizeof sample_chains[i] - sizeof sample_chains[i][0]);
        if (count > best_count) best = i, best_count = count;
      }
      fprintf(stderr, " callers(%u)", best_count);
      for (int depth = 0; depth < CHAIN_DEPTH; depth++) fprintf(stderr, " %08x", sample_chains[best][depth]);
    }
    first = last;
  }
  fprintf(stderr, "\n");
}
#else
volatile int stage_samples_paused;
void stage_sample_thread(int graphics) { (void)graphics; }
static void samples_frame(int slow) { (void)slow; }
#endif

// WII_RECOMP_FRAME_LOG=1 prints the time since the last frame shown, for each frame, so a
// tool can tell an even 60 from frames that come in fits (recomp/tools/race_bench.py).
void stage_frame(float gap_ms) {
  static int frame_log = -1, slow_samples = -1;
  // WII_RECOMP_FRAME_LOG=2 adds how long the game's thread waited with nothing to do in
  // that frame: what's left of a capped frame, so machines can be compared under the cap.
  // And the longest single wait among them, which shows a sleep that ran over.
  if (frame_log < 0) frame_log = getenv("WII_RECOMP_FRAME_LOG") ? atoi(getenv("WII_RECOMP_FRAME_LOG")) : 0;
  if (frame_log == 2) {
    fprintf(stderr, "frame %.2f idle %.2f present %.2f longest %.2f", (double)gap_ms, stage_ticks[STAGE_IDLE] / 60750.0,
            stage_ticks[STAGE_PRESENT] / 60750.0, longest_idle / 60750.0);
    // For a title that counts its work: what the frame's code would take on the console,
    // in milliseconds of a GameCube's 486 MHz. Over a field's 16.7, the console needs two.
    if (title_counts_work) fprintf(stderr, " work %.2f", (double)work_cycles_taken() / 486000.0);
    fprintf(stderr, " token reads %u\n", notes[NOTE_TOKEN_READ]);
  }
  // 3 adds the game's thread about then (its system's "current thread"), for a game
  // whose pictures come from more than one.
  else if (frame_log == 3) fprintf(stderr, "frame %.2f thread %08x\n", (double)gap_ms, rd32(CURRENT_THREAD));
  else if (frame_log) fprintf(stderr, "frame %.2f\n", (double)gap_ms);
  static long switches_before;
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);
  if (gap_ms > 20) {
    double ms = 1 / 60750.0;
    fprintf(stderr, "slow frame %.1f ms: stream %.1f, present %.1f, disc %.1f, audio %.1f, idle %.1f (longest %.1f), preempted %ld, "
            "waited for graphics %.1f (%u peeks, %u pokes, %u box reads, %u copies touched)\n",
            (double)gap_ms, stage_ticks[STAGE_STREAM] * ms, stage_ticks[STAGE_PRESENT] * ms, stage_ticks[STAGE_DISC] * ms,
            stage_ticks[STAGE_AUDIO] * ms, stage_ticks[STAGE_IDLE] * ms, longest_idle * ms, usage.ru_nivcsw - switches_before,
            stage_ticks[STAGE_HANDED] * ms, notes[NOTE_PEEK], notes[NOTE_POKE], notes[NOTE_BOX], notes[NOTE_TOUCH]);
  }
  if (slow_samples < 0) slow_samples = getenv("WII_RECOMP_SLOW_SAMPLES") != 0;
  if (slow_samples) {  // With WII_RECOMP_SLOW_SAMPLES, the waits for the graphics thread over ten seconds of frames.
    static uint64_t waited;
    static unsigned kinds[NOTE_COUNT], frames;
    waited += stage_ticks[STAGE_HANDED];
    for (int kind = 0; kind < NOTE_COUNT; kind++) kinds[kind] += notes[kind];
    if (++frames == 600) {
      fprintf(stderr, "waits for graphics in 600 frames: %.1f ms (%u peeks, %u pokes, %u box reads, %u copies touched)\n", waited / 60750.0,
              kinds[NOTE_PEEK], kinds[NOTE_POKE], kinds[NOTE_BOX], kinds[NOTE_TOUCH]);
      waited = 0, frames = 0;
      memset(kinds, 0, sizeof kinds);
    }
  }
  memset(notes, 0, sizeof notes);
  samples_frame(gap_ms > 20);
  switches_before = usage.ru_nivcsw;
  memset(stage_ticks, 0, sizeof stage_ticks);
  longest_idle = 0;
}

volatile int debug_threads_now;

void debug_poll(uint64_t seconds_running) {
  static int done;
  if (debug_threads_now) {
    debug_threads_now = 0;
    debug_threads();
  }
  guest_memory_report();
  gap_flush();
  report_activity(seconds_running);
  report_sound(seconds_running);
  static const char *after;
  static int looked;
  if (!looked) { looked = 1; after = getenv("WII_RECOMP_DUMP_AFTER"); }
  if (done || !after || seconds_running < (uint64_t)atoi(after)) return;
  done = 1;
  debug_threads();
}
