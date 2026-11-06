// The video interface's timing: how long a field lasts, where the beam is, and when each
// display interrupt fires, all from the timing registers the program sets. Registers and
// sums are Dolphin's (VideoInterface.cpp, GPL-2.0+).
//
// The beam counts half-lines across a whole frame, the odd field then the even. A field
// is 3 EQU + PRB + 2 ACV + PSB half-lines of HLW samples each, at 13.5 or 27 million
// samples a second as the clock select says: 59.94 fields a second for NTSC and 50 for
// PAL. A display interrupt fires as the beam reaches its line, and its half when its
// horizontal count is past the half-line. Each field's start is a retrace for the
// graphics (graphics_retrace).
//
// The 60 Hz modes are shown at the display's own 60 rather than 59.94, so a field is
// stretched by a thousandth: the game's pace and the window's stay in step, with no
// picture repeated every 17 seconds. PAL keeps its 50.
#include <stdint.h>
#include <stdio.h>

#include "cpu.h"
#include "hardware.h"
#include "vi.h"

enum {
  VI_VERTICAL = 0xcc002000u, VI_CONTROL = 0xcc002002u, VI_HORIZONTAL_0 = 0xcc002004u,
  VI_ODD_BLANKING = 0xcc00200cu, VI_EVEN_BLANKING = 0xcc002010u,
  VI_BEAM_VERTICAL = 0xcc00202cu, VI_BEAM_HORIZONTAL = 0xcc00202eu,
  VI_INTERRUPTS = 0xcc002030u, VI_CLOCK = 0xcc00206cu,
  INTERRUPT_COUNT = 4, INTERRUPT_ENABLE = 0x10000000u, INTERRUPT_FLAG = 0x80000000u,
  CONTROL_PROGRESSIVE = 1 << 2, FORMAT_SHIFT = 8, FORMAT_PAL = 1,
};
#define TIME_BASE_HZ 60750000.0
#define SAMPLES_PER_SECOND 13500000.0  // At the 27 MHz clock; twice that at 54.
#define DISPLAY_HZ 60.0

static struct {
  int known;                 // Worked out since the registers last changed.
  double half_line_ticks;    // Time base ticks a half-line lasts.
  uint32_t odd_half_lines, frame_half_lines, half_line_width;
} timing;
static uint64_t origin;      // When the beam was last at the frame's first half-line.
static uint64_t done_through;  // Half-lines since origin already passed, plus 1.

void vi_timing_changed(void) { timing.known = 0; }

static uint32_t reg16(uint32_t address) { return hw_peek(address, 2); }
static uint32_t reg32(uint32_t address) { return hw_peek(address, 4); }

static void work_out_timing(void) {
  uint32_t vertical = reg16(VI_VERTICAL), equalize = vertical & 0xf, active = (vertical >> 4) & 0x3ff;
  uint32_t width = reg32(VI_HORIZONTAL_0) & 0x1ff;
  uint32_t odd = reg32(VI_ODD_BLANKING), even = reg32(VI_EVEN_BLANKING);
  uint32_t odd_lines = 3 * equalize + (odd & 0x3ff) + 2 * active + ((odd >> 16) & 0x3ff);
  uint32_t even_lines = 3 * equalize + (even & 0x3ff) + 2 * active + ((even >> 16) & 0x3ff);
  double samples = SAMPLES_PER_SECOND * ((reg16(VI_CLOCK) & 1) ? 2 : 1);
  if (!width || !odd_lines || !even_lines) {  // Not set up yet: NTSC's numbers.
    width = 429, odd_lines = even_lines = 525;
    samples = SAMPLES_PER_SECOND;
  }
  timing.half_line_width = width;
  timing.half_line_ticks = TIME_BASE_HZ / samples * width;
  timing.odd_half_lines = odd_lines;
  timing.frame_half_lines = odd_lines + even_lines;
  // A 60 Hz mode is shown at the display's 60: its fields are stretched to fit.
  double field_rate = TIME_BASE_HZ / (timing.half_line_ticks * odd_lines);
  if (field_rate > 59.0 && field_rate < 61.0) timing.half_line_ticks = TIME_BASE_HZ / DISPLAY_HZ / odd_lines;
  timing.known = 1;
  static double told_rate;
  static uint32_t told_lines;
  double rate = TIME_BASE_HZ / (timing.half_line_ticks * odd_lines);
  if (rate != told_rate || timing.frame_half_lines != told_lines) {
    told_rate = rate, told_lines = timing.frame_half_lines;
    fprintf(stderr, "vi: %u and %u half-lines a field, %u samples each, %.3f fields a second%s\n", odd_lines, even_lines, width, rate,
            reg16(VI_CONTROL) & CONTROL_PROGRESSIVE ? ", progressive" : "");
  }
}

static void ensure_timing(void) {
  if (!timing.known) work_out_timing();
}

double vi_field_rate(void) {
  ensure_timing();
  return TIME_BASE_HZ / (timing.half_line_ticks * timing.odd_half_lines);
}

int vi_is_pal(void) { return ((reg16(VI_CONTROL) >> FORMAT_SHIFT) & 3) == FORMAT_PAL; }
int vi_is_progressive(void) { return (reg16(VI_CONTROL) & CONTROL_PROGRESSIVE) != 0; }

// Half-lines since the frame began, counting on through later frames.
static uint64_t half_lines_at(uint64_t now) {
  if (!origin || now < origin) origin = now;
  return (uint64_t)((double)(now - origin) / timing.half_line_ticks);
}

// The half-line of the frame a display interrupt fires on.
static uint32_t interrupt_half_line(uint32_t value) {
  uint32_t line = (value >> 16) & 0x3ff, horizontal = value & 0x3ff;
  return (line ? line - 1 : 0) * 2 + (horizontal > timing.half_line_width ? 1 : 0);
}

void vi_update(uint64_t now) {
  ensure_timing();
  uint64_t passed = half_lines_at(now) + 1;  // Half-lines up to and including now's.
  if (passed <= done_through) return;
  uint64_t from = done_through;
  done_through = passed;
  // Long behind, as after a pause: only the last frame's events count.
  if (passed - from > timing.frame_half_lines) from = passed - timing.frame_half_lines;
  // Whether a half-line of the frame comes in [from, passed).
  uint64_t frame = timing.frame_half_lines;
  #define COMES(half_line) (from + ((half_line) + frame - from % frame) % frame < passed)
  if (COMES(0) || COMES(timing.odd_half_lines)) graphics_retrace();
  for (int i = 0; i < INTERRUPT_COUNT; i++) {
    uint32_t value = reg32(VI_INTERRUPTS + (uint32_t)i * 4);
    if ((value & INTERRUPT_ENABLE) && COMES(interrupt_half_line(value) % frame)) vi_display_interrupt(i);
  }
  #undef COMES
  // Keep the origin near, so the sums stay exact over hours.
  uint64_t frames = (passed - 1) / timing.frame_half_lines;
  if (frames > 1) {
    uint64_t moved = (frames - 1) * timing.frame_half_lines;
    origin += (uint64_t)(moved * timing.half_line_ticks);
    done_through -= moved;
  }
}

// When the beam next reaches a half-line that does something: a field's start, or an
// enabled display interrupt's line.
uint64_t vi_next_due(uint64_t now) {
  ensure_timing();
  if (!origin) return now;
  uint64_t frame = timing.frame_half_lines, soonest = done_through + frame;
  uint32_t targets[2 + INTERRUPT_COUNT];
  int count = 0;
  targets[count++] = 0;
  targets[count++] = timing.odd_half_lines;
  for (int i = 0; i < INTERRUPT_COUNT; i++) {
    uint32_t value = reg32(VI_INTERRUPTS + (uint32_t)i * 4);
    if (value & INTERRUPT_ENABLE) targets[count++] = interrupt_half_line(value) % (uint32_t)frame;
  }
  for (int i = 0; i < count; i++) {
    uint64_t at = done_through + (targets[i] + frame - done_through % frame) % frame;
    if (at < soonest) soonest = at;
  }
  return origin + (uint64_t)(soonest * timing.half_line_ticks) + 1;
}

int vi_read(uint32_t address, uint32_t *value) {
  if (address != VI_BEAM_VERTICAL && address != VI_BEAM_HORIZONTAL) return 0;
  ensure_timing();
  uint64_t now = time_base();
  double into = (double)(now - (origin && now >= origin ? origin : now)) / timing.half_line_ticks;
  uint32_t half_line = (uint32_t)((uint64_t)into % timing.frame_half_lines);
  if (address == VI_BEAM_VERTICAL) *value = half_line / 2 + 1;
  else *value = (uint32_t)((into - (double)(uint64_t)into + (half_line & 1)) * timing.half_line_width) + 1;
  return 1;
}
