// The time base counts from 2000 in local time, and offsetting it keeps intervals.
#include "../../recomp/runtime/interrupts.c"
#include "../../recomp/runtime/timeline.c"
#include <unistd.h>
#include "../harness.h"
void debug_poll(uint64_t s) {}
int gamecube_mode;
int title_counts_work;
const Title *title = &(Title){0};
void bluetooth_update(uint64_t now) {}
void stage_add(int s, uint64_t t) {}
void video_retrace(void) {}
void ipc_update(void) {}
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
void graphics_update(void) {}
uint64_t graphics_next_due(void) { return 0; }
int graphics_busy(void) { return 0; }
void audio_update(uint64_t now) {}
void hardware_update(uint64_t now) {}
void floating_point_exceptions_check(void) {}
uint64_t audio_next_due(void) { return 0; }
uint64_t dsp_next_due(void) { return 0; }
int irq_pending(void) { return 0; }
void vi_update(uint64_t now) {}
uint64_t vi_next_due(uint64_t now) { return 0; }
void net_update(uint64_t now) {}
int net_waiting(void) { return 0; }
void enter_exception(uint32_t k, uint32_t h) {}
int main(void) {
  memory_start();
  CHECK_EQ(wii_ticks_at(946684800, 0), 0);
  CHECK_EQ(wii_ticks_at(946684801, 0), 60750000);
  CHECK_EQ(wii_ticks_at(946684800, 7200), 7200ull * 60750000);
  time_t now = time(0);
  struct tm local;
  localtime_r(&now, &local);
  uint64_t before = monotonic_ticks();
  time_start();
  uint64_t a = time_base(), b;
  uint64_t expected = wii_ticks_at(now, local.tm_gmtoff);
  CHECK(a + 2 * TIME_BASE_HZ > expected && a < expected + 2 * TIME_BASE_HZ);
  usleep(20000);
  b = time_base();
  CHECK(b - a >= TIME_BASE_HZ / 50 && b - a < TIME_BASE_HZ);  // 20ms, as ticks.
  // Decrementer: same relative meaning near a large time base.
  decrementer_set(TIME_BASE_HZ);
  uint32_t left = decrementer_get();
  CHECK(left <= TIME_BASE_HZ && left > TIME_BASE_HZ / 2);
  // A game setting its clock moves what it reads, and leaves every deadline alone.
  uint64_t runtime_before = time_base();
  guest_time_base_set(1, 0x12345678u);
  guest_time_base_set(0, 0x40000000u);
  uint64_t set = guest_time_base();
  CHECK(set >= 0x1234567840000000ull && set < 0x1234567840000000ull + TIME_BASE_HZ);
  uint64_t runtime_after = time_base();
  CHECK(runtime_after - runtime_before < TIME_BASE_HZ);
  left = decrementer_get();
  CHECK(left <= TIME_BASE_HZ && left > TIME_BASE_HZ / 2);
  // In GameCube mode the game's clock runs at 40.5 MHz, two thirds of the Wii's, and its
  // decrementer counts the same ticks.
  gamecube_mode = 1;
  uint64_t wii = time_base(), cube = guest_time_base() - guest_time_offset;
  CHECK(cube >= wii * 2 / 3 && cube < wii * 2 / 3 + TIME_BASE_HZ / 100);
  decrementer_set(40500000);  // A GameCube second.
  left = decrementer_get();
  CHECK(left <= 40500000 && left > 40500000 / 2);
  CHECK(decrementer_deadline - time_base() > TIME_BASE_HZ / 2);  // A second of the console's own clock.
  gamecube_mode = 0;
  // A title that counts its work takes cycles off the count, and each check adds up what
  // it ran through: here 700 of a full count, then the count's overrun as well.
  title_counts_work = 1;
  poll_interrupts();
  work_cycles_taken();
  CHECK_EQ(interrupt_budget, CHECK_EVERY_CYCLES);
  interrupt_budget -= 700;
  poll_interrupts();
  CHECK_EQ(work_cycles_taken(), 700);
  interrupt_budget -= CHECK_EVERY_CYCLES + 9;
  poll_interrupts();
  CHECK_EQ(work_cycles_taken(), CHECK_EVERY_CYCLES + 9);
  CHECK_EQ(work_cycles_taken(), 0);
  // A check asked for early ends the count where it stands: the cycles not run aren't work.
  interrupt_budget -= 300;
  decrementer_set(0);
  interrupts_turned_on_soon();
  CHECK_EQ(interrupt_budget, 0);
  poll_interrupts();
  CHECK_EQ(work_cycles_taken(), 300);
  // A title that doesn't count keeps its checks as they were, and reports no work.
  title_counts_work = 0;
  poll_interrupts();
  CHECK_EQ(interrupt_budget, CHECK_EVERY);
  interrupt_budget -= 50;
  poll_interrupts();
  CHECK_EQ(work_cycles_taken(), 0);
  (void)before;
  return finish();
}
