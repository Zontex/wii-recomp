// The time base counts from 2000 in local time, and offsetting it keeps intervals.
#include "../../recomp/runtime/interrupts.c"
#include <unistd.h>
#include "../harness.h"
void debug_poll(uint64_t s) {}
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
  (void)before;
  return finish();
}
