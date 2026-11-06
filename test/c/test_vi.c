// The video interface's timing, through the real vi.c: field rates from the registers as
// the SDK sets NTSC and PAL, display interrupts at their own lines, and the beam.
#include <math.h>
#include <string.h>
#include "harness.h"
#include "vi.h"

// What vi.c expects of the rest of the runtime: registers, the clock, and what it fires.
static uint8_t registers[0x100];
static uint64_t now;
static int retraces, fired[4];
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
uint32_t hw_peek(uint32_t address, int bytes) {
  uint32_t at = address - 0xcc002000u, value = 0;
  for (int i = 0; i < bytes; i++) value = value << 8 | registers[at + i];
  return value;
}
static void set(uint32_t address, uint32_t value, int bytes) {
  for (int i = 0; i < bytes; i++) registers[address - 0xcc002000u + i] = (uint8_t)(value >> (8 * (bytes - 1 - i)));
  vi_timing_changed();
}
uint64_t time_base(void) { return now; }
void graphics_retrace(void) { retraces++; }
void vi_display_interrupt(int i) { fired[i]++; }

// NTSC interlaced and PAL interlaced, as the SDK's timing tables set them.
static void ntsc(void) {
  set(0xcc002000u, 6 | 240 << 4, 2);            // EQU 6, ACV 240.
  set(0xcc002002u, 1, 2);                        // On, interlaced, NTSC.
  set(0xcc002004u, 429, 4);                      // HLW 429.
  set(0xcc00200cu, 24 | 3u << 16, 4);           // Blanking: 18 + 24 + 480 + 3 = 525 half-lines a field.
  set(0xcc002010u, 25 | 2u << 16, 4);
}
static void pal(void) {
  set(0xcc002000u, 5 | 287 << 4, 2);            // EQU 5, ACV 287.
  set(0xcc002002u, 1 | 1 << 8, 2);              // PAL.
  set(0xcc002004u, 432, 4);                      // HLW 432.
  set(0xcc00200cu, 35 | 2u << 16, 4);           // Blanking: 15 + 35 + 574 + 2 = 626 half-lines, and 624 the other field.
  set(0xcc002010u, 33 | 2u << 16, 4);
}

int main(void) {
  memory_start();
  // Not set up yet: 60 a second, as the display.
  CHECK(fabs(vi_field_rate() - 60.0) < 0.01);

  // NTSC's 59.94 is shown at 60; PAL keeps its 50.
  ntsc();
  CHECK(fabs(vi_field_rate() - 60.0) < 0.01);
  CHECK(!vi_is_pal());
  pal();
  CHECK(fabs(vi_field_rate() - 50.0) < 0.2);
  CHECK(vi_is_pal());

  // Over a second of PAL, about 50 retraces. A display interrupt at line 1 fires once a
  // frame, two fields, so 25 times; one at line 313 too.
  set(0xcc002030u, 0x10000000u | 1u << 16 | 1, 4);
  set(0xcc002034u, 0x10000000u | 313u << 16 | 1, 4);
  now = 1000;
  vi_update(now);
  retraces = 0;
  memset(fired, 0, sizeof fired);
  for (int step = 0; step < 6075; step++) {  // A second, in steps of 10,000 ticks.
    now += 10000;
    vi_update(now);
  }
  CHECK(retraces >= 49 && retraces <= 51);
  CHECK(fired[0] >= 24 && fired[0] <= 26);
  CHECK(fired[1] >= 24 && fired[1] <= 26);
  CHECK_EQ(fired[2], 0);  // Not enabled.

  // The next thing due is never past the next field.
  uint64_t due = vi_next_due(now);
  CHECK(due > now && due - now <= 60750000 / 50 + 1);

  // The beam moves on with time, within the frame's lines.
  uint32_t line, later;
  vi_read(0xcc00202cu, &line);
  now += 60750000 / 1000;  // A millisecond: about 15 lines.
  vi_read(0xcc00202cu, &later);
  CHECK(line >= 1 && line <= 626 && later >= 1 && later <= 626);
  CHECK(later != line);
  uint32_t horizontal;
  CHECK(vi_read(0xcc00202eu, &horizontal));
  CHECK(horizontal >= 1 && horizontal <= 864);
  CHECK(!vi_read(0xcc002030u, &horizontal));  // Other registers are hardware.c's.
  return finish();
}
