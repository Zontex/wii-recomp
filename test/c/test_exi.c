// The expansion bus's clock chip, through the real exi.c, driven as the SDK drives it.
#include "../harness.h"

uint32_t hw_read(uint32_t address, int bytes) { (void)address; (void)bytes; return 0; }
void hw_write(uint32_t address, uint32_t value, int bytes) { (void)address; (void)value; (void)bytes; }
static int irq_up, gaps;
void raise_irq(int bit) { (void)bit; irq_up = 1; }
void clear_irq(int bit) { (void)bit; irq_up = 0; }
void gap_hit(const char *id, uint32_t detail) { (void)id; (void)detail; gaps++; }
static uint64_t now_ticks = 60750000ull * 1000000;  // A million seconds after 2000.
uint64_t time_base(void) { return now_ticks; }

#include "exi.c"

enum { EXI = 0xcd006800u, CLOCK_CHIP = 2u << 7, BUFFER = 0x80001000u };

static uint32_t control(int dma, int direction, int bytes) { return 1u | (uint32_t)dma << 1 | (uint32_t)direction << 2 | (uint32_t)(bytes - 1) << 4; }

static void select_chip(void) {
  exi_write(EXI + 0x00, 0);
  exi_write(EXI + 0x00, CLOCK_CHIP);
}

static void command(uint32_t word) {
  select_chip();
  exi_write(EXI + 0x10, word);
  exi_write(EXI + 0x0c, control(0, WRITE, 4));
}

static uint32_t read_clock(void) {
  uint32_t value;
  command(0x20000000);
  exi_write(EXI + 0x0c, control(0, READ, 4));
  exi_read(EXI + 0x10, &value);
  return value;
}

static void test_sram(void) {
  command(0x20000100);  // The settings, after the clock.
  exi_write(EXI + 0x04, BUFFER & 0x01ffffff);
  exi_write(EXI + 0x08, 0x40);
  exi_write(EXI + 0x0c, control(1, READ, 1));
  CHECK_EQ(rd16(BUFFER), 0x002c);       // Checksum.
  CHECK_EQ(rd16(BUFFER + 2), 0xffd0);   // Its complement.
  CHECK_EQ(rd8(BUFFER + 19), 0x2c);     // Flags: stereo, set up.
  CHECK_EQ(rd8(BUFFER + 58), 0xff);     // Memory card ID checksums.
  uint32_t value;
  exi_read(EXI + 0x0c, &value);
  CHECK_EQ(value & 1, 0);  // Finished at once.
}

static void test_clock(void) {
  CHECK_EQ(read_clock(), 1000000);
  now_ticks += 60750000ull * 5;
  CHECK_EQ(read_clock(), 1000005);
  command(0xa0000000);  // Write the clock.
  exi_write(EXI + 0x10, 1000105);
  exi_write(EXI + 0x0c, control(0, WRITE, 4));
  CHECK_EQ(read_clock(), 1000105);
  now_ticks += 60750000ull;
  CHECK_EQ(read_clock(), 1000106);  // It keeps running from where it was set.
}

static void test_interrupt(void) {
  exi_write(EXI + 0x00, CLOCK_CHIP | 1u << 2 | 1u << 3);  // Unmask transfer done, and clear it.
  CHECK(!irq_up);
  exi_write(EXI + 0x10, 0x20000000);
  exi_write(EXI + 0x0c, control(0, WRITE, 4));
  CHECK(irq_up);
  uint32_t status;
  exi_read(EXI + 0x00, &status);
  exi_write(EXI + 0x00, status);  // Writing the done bit back acknowledges it.
  CHECK(!irq_up);
}

static void test_no_device(void) {
  exi_write(EXI + 0x14, 1u << 7);  // Channel 1's memory card slot, which is empty.
  exi_write(EXI + 0x14 + 0x0c, control(0, READ, 4));
  uint32_t value;
  exi_read(EXI + 0x14 + 0x10, &value);
  CHECK_EQ(value, 0);
  CHECK_EQ(gaps, 1);
  exi_write(EXI + 0x00, 4u << 7);  // Channel 0's debugging device, which no retail Wii has.
  exi_write(EXI + 0x0c, control(0, READ, 4));
  CHECK_EQ(gaps, 1);
}

int main(void) {
  memory_start();
  test_sram();
  test_clock();
  test_interrupt();
  test_no_device();
  return finish();
}
