// Runs the game's timer and hardware interrupts. Translated code checks in here
// regularly, and the game's own handlers do the rest.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#include "timeline.h"
#include "audio.h"
#include "host_sleep.h"
#include "bluetooth.h"
#include "title.h"
#include "cpu.h"
#include "debug.h"
#include "dsp.h"
#include "hardware.h"
#include "vi.h"
#include "net.h"

#define CHECK_EVERY 2000
// A title that counts its work (hardware.h) counts down cycles: this many between checks,
// the most Dolphin runs before looking for something due (CoreTiming.cpp, MAX_SLICE_LENGTH).
#define CHECK_EVERY_CYCLES 20000
#define INTERRUPTS_ENABLED 0x8000u
#define EXCEPTION_TABLE 0x80003000u
#define EXCEPTION_EXTERNAL 4
#define EXCEPTION_DECREMENTER 8
#define TIME_BASE_HZ 60750000ull
#define IDLE_SLEEP_TICKS (TIME_BASE_HZ / 250)  // 4 ms, the longest sleep with nothing due.
#define NANOSECONDS 1000000000ull
#define BUSY_SLEEP_TICKS (TIME_BASE_HZ / 10000)  // 100 microseconds.
#define WII_EPOCH_UNIX 946684800ll  // 2000-01-01, when the Wii's time base reads zero.

#ifdef _WIN32
// At a fixed address on Windows (cpu.h), so it's given its first value as the program loads.
__attribute__((constructor(102))) static void budget_start(void) { interrupt_budget = CHECK_EVERY; }
#else
int interrupt_budget = CHECK_EVERY;
#endif

// What the count was last set to, so a check knows how much of it the code ran through.
static int slice_length = CHECK_EVERY;
static _Atomic uint64_t work_cycles;  // Run by a title that counts its work, since last asked.
static uint64_t decrementer_deadline;
static int decrementer_armed;
static uint64_t time_base_offset;
static uint64_t guest_time_offset;

#ifdef __APPLE__
// The game reads the clock constantly, and clock_gettime goes the long way round here.
static uint64_t monotonic_nanoseconds(void) {
  static mach_timebase_info_data_t rate;
  if (!rate.denom) mach_timebase_info(&rate);
  return (uint64_t)((unsigned __int128)mach_absolute_time() * rate.numer / rate.denom);
}
#else
static uint64_t monotonic_nanoseconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * NANOSECONDS + (uint64_t)now.tv_nsec;
}
#endif

static uint64_t monotonic_ticks(void) {
  uint64_t now = monotonic_nanoseconds();
  return now / NANOSECONDS * TIME_BASE_HZ + now % NANOSECONDS * TIME_BASE_HZ / NANOSECONDS;
}

uint64_t wii_ticks_at(int64_t unix_seconds, long utc_offset_seconds) {
  return (uint64_t)(unix_seconds + utc_offset_seconds - WII_EPOCH_UNIX) * TIME_BASE_HZ;
}

// The game reads the time base as ticks since 2000 in local time.
// A fixed offset, set once, keeps every interval and deadline exactly as it was.
void time_start(void) {
  time_t now = time(0);
#ifdef _WIN32
  struct tm local;
  localtime_s(&local, &now);
  long utc_offset = (long)(_mkgmtime(&local) - now);  // Local time read as UTC is ahead by the offset.
  time_base_offset = wii_ticks_at(now, utc_offset) - monotonic_ticks();
#else
  struct tm local;
  localtime_r(&now, &local);
  time_base_offset = wii_ticks_at(now, local.tm_gmtoff) - monotonic_ticks();
#endif
}

uint64_t time_base(void) { return monotonic_ticks() + time_base_offset; }

// In GameCube mode the game's clock runs at a GameCube's 40.5 MHz, two thirds of the
// Wii's 60.75: its time base and decrementer, and nothing else. The console's own
// deadlines stay on time_base.
static uint64_t guest_ticks(uint64_t ticks) { return gamecube_mode ? ticks * 2 / 3 : ticks; }

uint64_t guest_time_base(void) { return guest_ticks(time_base()) + guest_time_offset; }

// A game setting the clock moves what it reads and nothing else. Every deadline here
// runs on time_base, and the decrementer doesn't follow the time base on real hardware.
void guest_time_base_set(int high_half, uint32_t value) {
  uint64_t now = guest_time_base();
  uint64_t wanted = high_half ? (uint64_t)value << 32 | (uint32_t)now : (now & 0xffffffff00000000ull) | value;
  guest_time_offset += wanted - now;
}

void decrementer_set(uint32_t value) {
  int64_t ticks = (int32_t)value;
  decrementer_deadline = time_base() + (uint64_t)(gamecube_mode ? ticks * 3 / 2 : ticks);
  decrementer_armed = 1;
}

uint32_t decrementer_get(void) {
  int64_t left = (int64_t)(decrementer_deadline - time_base());
  return (uint32_t)(gamecube_mode ? left * 2 / 3 : left);
}

// One waiting, or a timer that ran out while interrupts were off, is taken right here, as
// the processor takes it the moment mtmsr turns them on. Mario & Sonic at the London 2012
// Olympic Games keeps two threads waking each other, with interrupts off nearly all the
// time and on only between two instructions: an interrupt left for the next check found
// them off again, and the frames that waited on it came every other retrace, at 30 fps.
static int something_due(void) { return irq_pending() || (decrementer_armed && time_base() >= decrementer_deadline); }

void interrupts_turned_on(void) {
  if (something_due()) poll_interrupts();
}

// The same for a context loaded with interrupts on (threads.c), which is still on the old
// thread's host stack: taken at the next check, which is on the new one.
void interrupts_turned_on_soon(void) {
  if (!something_due() || interrupt_budget <= 0) return;
  slice_length -= interrupt_budget;  // The rest of the count wasn't run, and mustn't read as work.
  interrupt_budget = 0;
}

uint64_t work_cycles_taken(void) { return atomic_exchange_explicit(&work_cycles, 0, memory_order_relaxed); }

static unsigned long long interrupts_taken, interrupt_polls, interrupt_polls_masked;

void interrupts_describe(void) {
  fprintf(stderr, "state: %llu checks, %llu with interrupts off, %llu interrupts taken\n", interrupt_polls, interrupt_polls_masked, interrupts_taken);
}

void poll_interrupts(void) {
  interrupt_polls++;
  if (title_counts_work) atomic_fetch_add_explicit(&work_cycles, (uint64_t)(slice_length - interrupt_budget), memory_order_relaxed);
  slice_length = title_counts_work ? CHECK_EVERY_CYCLES : CHECK_EVERY;
  interrupt_budget = slice_length;
  uint64_t now = time_base();
  static uint64_t started;
  if (!started) started = now;
  debug_poll((now - started) / TIME_BASE_HZ);
  vi_update(now);  // Display interrupts and retraces, as the beam passes them.
  net_update(now);
  if (title->virtual_bluetooth) bluetooth_update(now);
  ipc_update();
  graphics_update();
  audio_update(now);
  hardware_update(now);
  floating_point_exceptions_check();
  if (!(cpu.msr & INTERRUPTS_ENABLED)) { interrupt_polls_masked++; return; }
  if (irq_pending()) {
    interrupts_taken++;
    uint32_t handler = rd32(EXCEPTION_TABLE + EXCEPTION_EXTERNAL * 4);
    TIMELINE_MOMENT("interrupt taken");
    if (handler) enter_exception(EXCEPTION_EXTERNAL, handler);
  } else if (decrementer_armed && now >= decrementer_deadline) {
    decrementer_armed = 0;
    uint32_t handler = rd32(EXCEPTION_TABLE + EXCEPTION_DECREMENTER * 4);
    if (handler) enter_exception(EXCEPTION_DECREMENTER, handler);
  }
}

static uint64_t sooner(uint64_t wait, uint64_t now, uint64_t due) {
  return due > now && due - now < wait ? due - now : wait;
}

// The game's callers as its thread goes idle, for the timeline: up its stack from the
// scheduler, whose own frame hasn't kept its return yet. So a span of idle time says
// which wait it was, such as the one for the next retrace at a frame's end.
static void idle_callers(uint32_t *calls) {
  uint32_t stack = cpu.r[1];
  int found = 0;
  for (int depth = 0; depth < 2 * TIMELINE_CALLS && found < TIMELINE_CALLS; depth++) {
    if (stack < 0x80000000u || stack >= 0x94000000u || !host(stack) || !host(stack + 7)) break;
    uint32_t caller = rd32(stack + 4);
    if (caller) calls[found++] = caller;
    stack = rd32(stack);
  }
}

// The game has nothing to run. Sleeps until the next thing that could give it work,
// then checks for interrupts. Waking only then lets the processor rest properly.
// Set WII_RECOMP_IDLE_SPIN=1 to spin as the Wii does.
void idle_wait(void) {
  static int spin = -1;
  if (spin < 0) spin = getenv("WII_RECOMP_IDLE_SPIN") != 0;
  if (spin) { POLL(); return; }
  uint64_t now = time_base(), wait = IDLE_SLEEP_TICKS;
  wait = sooner(wait, now, vi_next_due(now));
  if (decrementer_armed) wait = sooner(wait, now, decrementer_deadline);
  wait = sooner(wait, now, graphics_next_due());
  wait = sooner(wait, now, audio_next_due());
  wait = sooner(wait, now, dsp_next_due());
  if (graphics_busy() || net_waiting()) wait = sooner(wait, now, now + BUSY_SLEEP_TICKS);  // A signal may come at any time.
  // An interrupt is taken ahead of the timer, and most sleeps end by raising one, so
  // sleeping past a timer that's due could put it off for good.
  int timer_due = decrementer_armed && decrementer_deadline <= now;
  if (!irq_pending() && !timer_due) {
    if (timeline_on) {
      uint32_t calls[TIMELINE_CALLS] = {0};
      idle_callers(calls);
      timeline_mark_calls("game idle", 'B', calls);
    }
    host_sleep(wait * NANOSECONDS / TIME_BASE_HZ);  // Not a plain nanosleep, which a Mac can stretch (host_sleep.h).
    TIMELINE_END("game idle");
    stage_add(STAGE_IDLE, time_base() - now);
  }
  poll_interrupts();
}
