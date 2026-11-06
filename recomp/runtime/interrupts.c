// Runs the game's timer and hardware interrupts. Translated code checks in here
// regularly, and the game's own handlers do the rest.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#include "timeline.h"
#include "audio.h"
#include "bluetooth.h"
#include "title.h"
#include "cpu.h"
#include "debug.h"
#include "dsp.h"
#include "hardware.h"

#define CHECK_EVERY 2000
#define INTERRUPTS_ENABLED 0x8000u
#define EXCEPTION_TABLE 0x80003000u
#define EXCEPTION_EXTERNAL 4
#define EXCEPTION_DECREMENTER 8
#define TIME_BASE_HZ 60750000ull
#define RETRACE_TICKS (TIME_BASE_HZ / 60)
#define IDLE_SLEEP_TICKS (TIME_BASE_HZ / 250)  // 4 ms, the longest sleep with nothing due.
#define NANOSECONDS 1000000000ull
#define BUSY_SLEEP_TICKS (TIME_BASE_HZ / 10000)  // 100 microseconds.
#define WII_EPOCH_UNIX 946684800ll  // 2000-01-01, when the Wii's time base reads zero.

int interrupt_budget = CHECK_EVERY;

static uint64_t decrementer_deadline;
static int decrementer_armed;
static uint64_t next_retrace;
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
  struct tm local;
  localtime_r(&now, &local);
  time_base_offset = wii_ticks_at(now, local.tm_gmtoff) - monotonic_ticks();
}

uint64_t time_base(void) { return monotonic_ticks() + time_base_offset; }

uint64_t guest_time_base(void) { return time_base() + guest_time_offset; }

// A game setting the clock moves what it reads and nothing else. Every deadline here
// runs on time_base, and the decrementer doesn't follow the time base on real hardware.
void guest_time_base_set(int high_half, uint32_t value) {
  uint64_t now = guest_time_base();
  uint64_t wanted = high_half ? (uint64_t)value << 32 | (uint32_t)now : (now & 0xffffffff00000000ull) | value;
  guest_time_offset += wanted - now;
}

void decrementer_set(uint32_t value) {
  decrementer_deadline = time_base() + (int32_t)value;
  decrementer_armed = 1;
}

uint32_t decrementer_get(void) { return (uint32_t)(decrementer_deadline - time_base()); }

void interrupts_turned_on(void) {
  if (irq_pending()) interrupt_budget = 0;
}

static unsigned long long interrupts_taken, interrupt_polls, interrupt_polls_masked;

void interrupts_describe(void) {
  fprintf(stderr, "state: %llu checks, %llu with interrupts off, %llu interrupts taken\n", interrupt_polls, interrupt_polls_masked, interrupts_taken);
}

void poll_interrupts(void) {
  interrupt_polls++;
  interrupt_budget = CHECK_EVERY;
  uint64_t now = time_base();
  static uint64_t started;
  if (!started) started = now;
  debug_poll((now - started) / TIME_BASE_HZ);
  if (now >= next_retrace) {
    // Counted from the last retrace, so a late check doesn't push every later one back.
    next_retrace += RETRACE_TICKS;
    if (now >= next_retrace) next_retrace = now + RETRACE_TICKS;
    video_retrace();
  }
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

// The game has nothing to run. Sleeps until the next thing that could give it work,
// then checks for interrupts. Waking only then lets the processor rest properly.
// Set WII_RECOMP_IDLE_SPIN=1 to spin as the Wii does.
void idle_wait(void) {
  static int spin = -1;
  if (spin < 0) spin = getenv("WII_RECOMP_IDLE_SPIN") != 0;
  if (spin) { POLL(); return; }
  uint64_t now = time_base(), wait = IDLE_SLEEP_TICKS;
  wait = sooner(wait, now, next_retrace);
  if (decrementer_armed) wait = sooner(wait, now, decrementer_deadline);
  wait = sooner(wait, now, graphics_next_due());
  wait = sooner(wait, now, audio_next_due());
  wait = sooner(wait, now, dsp_next_due());
  if (graphics_busy()) wait = sooner(wait, now, now + BUSY_SLEEP_TICKS);  // A signal may come at any time.
  // An interrupt is taken ahead of the timer, and most sleeps end by raising one, so
  // sleeping past a timer that's due could put it off for good.
  int timer_due = decrementer_armed && decrementer_deadline <= now;
  if (!irq_pending() && !timer_due) {
    struct timespec pause = {0, (long)(wait * NANOSECONDS / TIME_BASE_HZ)};
    TIMELINE_BEGIN("game idle");
    nanosleep(&pause, 0);
    TIMELINE_END("game idle");
    stage_add(STAGE_IDLE, time_base() - now);
  }
  poll_interrupts();
}
