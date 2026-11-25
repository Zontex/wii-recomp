// Sleeping on the host for a set time, and waking on time. A Mac rounds timers together
// when its screen is locked or its user is away: on the 2019 MacBook Pro a 4 ms nanosleep
// then lasted 27 ms on average, up to 36, and the latency-critical activity window.c
// takes didn't change that. The game's waits for its next retrace were stretched the same
// way, so a GameCube game got its retraces in bunches and ran at 20 to 30 fps.
//
// A thread with a real-time policy wakes on time (4.05 ms for 4, measured the same way).
// Switching the sleeping thread itself in and out of one around each sleep worked alone,
// but in a game, with the graphics thread switching too, switching back blocked in the
// kernel for a tenth of the game thread's time. So a helper thread of its own, real-time
// for good and doing nothing but keep time, wakes at the deadline and signals; the
// sleeper waits on a semaphore at its own priority. A wake by signal is never put off.
// For the game's thread (interrupts.c). Elsewhere it's a plain nanosleep.
#pragma once
#include <stdint.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/semaphore.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdatomic.h>

static semaphore_t host_sleep_start, host_sleep_done;
static _Atomic uint64_t host_sleep_deadline;  // In mach_absolute_time's units.
static pthread_once_t host_sleep_once = PTHREAD_ONCE_INIT;
static int host_sleep_ready;
static mach_timebase_info_data_t host_sleep_base;

static void *host_sleep_timer(void *unused) {
  (void)unused;
  double per_millisecond = (double)host_sleep_base.denom / host_sleep_base.numer * 1e6;
  // Its period, its work (a wake and a signal) and when that must be done.
  thread_time_constraint_policy_data_t real_time = {(uint32_t)(per_millisecond * 5), (uint32_t)(per_millisecond * 0.2),
                                                    (uint32_t)(per_millisecond * 1), 1};
  thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&real_time,
                    THREAD_TIME_CONSTRAINT_POLICY_COUNT);
  for (;;) {
    semaphore_wait(host_sleep_start);
    mach_wait_until(atomic_load(&host_sleep_deadline));
    semaphore_signal(host_sleep_done);
  }
  return 0;
}

static void host_sleep_start_timer(void) {
  mach_timebase_info(&host_sleep_base);
  pthread_t timer;
  host_sleep_ready = semaphore_create(mach_task_self(), &host_sleep_start, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS &&
                     semaphore_create(mach_task_self(), &host_sleep_done, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS &&
                     pthread_create(&timer, 0, host_sleep_timer, 0) == 0;
  if (host_sleep_ready) pthread_detach(timer);
}
#endif

static inline void host_sleep(uint64_t nanoseconds) {
  struct timespec pause = {(time_t)(nanoseconds / 1000000000u), (long)(nanoseconds % 1000000000u)};
#ifdef __APPLE__
  pthread_once(&host_sleep_once, host_sleep_start_timer);
  if (host_sleep_ready) {
    enum { SLACK_NANOSECONDS = 50000000 };  // If the helper is held up this long, the wait ends anyway.
    mach_timespec_t none = {0, 0};
    while (semaphore_timedwait(host_sleep_done, none) == KERN_SUCCESS) {}  // A signal left from a wait that gave up.
    atomic_store(&host_sleep_deadline, mach_absolute_time() + nanoseconds * host_sleep_base.denom / host_sleep_base.numer);
    semaphore_signal(host_sleep_start);
    uint64_t most = nanoseconds + SLACK_NANOSECONDS;
    mach_timespec_t fallback = {(unsigned)(most / 1000000000u), (clock_res_t)(most % 1000000000u)};
    semaphore_timedwait(host_sleep_done, fallback);
    return;
  }
#endif
  nanosleep(&pause, 0);
}
