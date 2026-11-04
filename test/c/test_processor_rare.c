// Traps and floating-point exceptions, through the real processor.c: each is reported.
#define HARNESS_WITH_PROCESSOR
#include "../harness.h"
#include <string.h>

uint32_t hw_read(uint32_t address, int bytes) { return 0; }
void hw_write(uint32_t address, uint32_t value, int bytes) {}

static int gaps;
static char last_gap[64];
static uint32_t last_detail;
void gap_hit(const char *id, uint32_t detail) { gaps++; snprintf(last_gap, sizeof last_gap, "%s", id); last_detail = detail; }

int main(void) {
  memory_start();
  trap_taken(0x80004321u);
  CHECK_EQ(gaps, 1);
  CHECK(!strcmp(last_gap, "cpu.tw_twi"));
  CHECK_EQ(last_detail, 0x80004321u);
  // Exceptions enabled in FPSCR alone, or MSR alone, can't interrupt.
  cpu.msr = 0x2000;
  cpu.fpscr = 0x80;
  floating_point_exceptions_check();
  cpu.msr = 0x2000 | 0x900;
  cpu.fpscr = 0;
  floating_point_exceptions_check();
  CHECK_EQ(gaps, 1);
  cpu.fpscr = 0x10;  // Divide by zero.
  floating_point_exceptions_check();
  CHECK_EQ(gaps, 2);
  CHECK(!strcmp(last_gap, "cpu.floating_point_program"));
  CHECK_EQ(last_detail, 0x10);
  floating_point_exceptions_check();
  CHECK_EQ(gaps, 2);  // Once.
  return finish();
}
