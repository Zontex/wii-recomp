// The disc interface's registers in GameCube mode (di.c), driven as the GameCube SDK's
// DVD library drives them, over a made-up disc.
#include <string.h>
#include "../harness.h"

int gamecube_mode;
static int irq_up, gaps;
void raise_irq(int bit) { CHECK_EQ(bit, 2); irq_up = 1; }
void clear_irq(int bit) { (void)bit; irq_up = 0; }
void gap_hit(const char *id, uint32_t detail) { (void)id; (void)detail; gaps++; }

enum { DISC_BYTES = 0x1000 };
static uint8_t disc[DISC_BYTES];
int drive_has_disc(void) { return 1; }
int drive_read(uint64_t offset, uint32_t bytes, uint8_t *out) {
  if (offset + bytes > DISC_BYTES) return 0;
  memcpy(out, disc + offset, bytes);
  return 1;
}
#include "di.c"

enum {
  DI = 0xcc006000u, BUFFER = 0x80001000u,
  STATUS_AT = DI, COVER_AT = DI + 4, COMMAND_AT = DI + 8, ADDRESS_AT = DI + 0x14, LENGTH_AT = DI + 0x18, CONTROL_AT = DI + 0x1c,
  IMMEDIATE_AT = DI + 0x20,
};

static uint32_t reg(uint32_t address) {
  uint32_t value = 0;
  CHECK(di_read(address, &value));
  return value;
}

static void command(uint32_t first, uint32_t second, uint32_t third, uint32_t length, int dma) {
  di_write(COMMAND_AT, first);
  di_write(COMMAND_AT + 4, second);
  di_write(COMMAND_AT + 8, third);
  di_write(ADDRESS_AT, BUFFER & 0x03ffffff);
  di_write(LENGTH_AT, length);
  di_write(CONTROL_AT, (dma ? 2u : 0u) | 1u);
}

static void test_read(void) {
  di_write(STATUS_AT, 1u << 3 | 1u << 1);  // Unmask transfers done and errors.
  command(0xa8000000u, 0x100 >> 2, 0x40, 0x40, 1);
  CHECK(!memcmp(host(BUFFER), disc + 0x100, 0x40));
  CHECK_EQ(reg(STATUS_AT) & 1u << 4, 1u << 4);  // Done, and the interrupt up.
  CHECK(irq_up);
  CHECK_EQ(reg(CONTROL_AT) & 1, 0);
  CHECK_EQ(reg(ADDRESS_AT), (BUFFER & 0x03ffffff) + 0x40);  // Moved on by the length, as Dolphin's.
  CHECK_EQ(reg(LENGTH_AT), 0);
  di_write(STATUS_AT, 1u << 4 | 1u << 3 | 1u << 1);  // Acknowledged.
  CHECK(!irq_up);
  CHECK_EQ(reg(STATUS_AT) & 1u << 4, 0);
}

static void test_disc_id_and_errors(void) {
  memset(host(BUFFER), 0, 0x20);
  command(0xa8000040u, 0, 0x20, 0x20, 1);
  CHECK(!memcmp(host(BUFFER), disc, 0x20));
  di_write(STATUS_AT, 1u << 4 | 1u << 3 | 1u << 1);
  command(0xa8000000u, DISC_BYTES >> 2, 0x20, 0x20, 1);  // Past the end.
  CHECK_EQ(reg(STATUS_AT) & 1u << 2, 1u << 2);
  CHECK(irq_up);
  di_write(STATUS_AT, 1u << 2 | 1u << 3 | 1u << 1);
  command(0xe0000000u, 0, 0, 0, 0);  // Request error: the last one, then cleared.
  CHECK_EQ(reg(IMMEDIATE_AT), 0x52100);
  command(0xe0000000u, 0, 0, 0, 0);
  CHECK_EQ(reg(IMMEDIATE_AT), 0);
  int before = gaps;
  command(0xfe000000u, 0, 0, 0, 0);  // A debug command the model doesn't know.
  CHECK(gaps > before);
  CHECK_EQ(reg(STATUS_AT) & 1u << 2, 1u << 2);
  di_write(STATUS_AT, 1u << 2 | 1u << 3 | 1u << 1);
}

static void test_streaming_answers_none(void) {
  command(0xe2000000u, 0, 0, 0, 0);  // Streaming wasn't set up: no audio buffer.
  CHECK_EQ(reg(STATUS_AT) & 1u << 2, 1u << 2);
  di_write(STATUS_AT, 1u << 2 | 1u << 3 | 1u << 1);
  command(0xe4010000u, 0, 0, 0, 0);  // Set it up, then ask: nothing is playing.
  di_write(IMMEDIATE_AT, 0xffffffffu);
  command(0xe2000000u, 0, 0, 0, 0);
  CHECK_EQ(reg(IMMEDIATE_AT), 0);
  int before = gaps;
  command(0xe1000000u, 0x1000, 0x8000, 0, 0);  // Starting a stream is reported.
  CHECK(gaps > before);
}

int main(void) {
  memory_start();
  for (int i = 0; i < DISC_BYTES; i++) disc[i] = (uint8_t)(i * 7 + 1);
  disc[8] = 0;  // The header doesn't ask for audio streaming.
  uint32_t value;
  CHECK(!di_read(STATUS_AT, &value));  // Not GameCube mode: not these registers'.
  gamecube_mode = 1;
  di_start();
  CHECK_EQ(reg(COVER_AT), 0);  // Closed, with a disc in.
  CHECK_EQ(reg(DI + 0x24), 1);
  test_read();
  test_disc_id_and_errors();
  test_streaming_answers_none();
  return finish();
}
