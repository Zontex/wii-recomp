// The simulated Balance Board: its identity, calibration, and the bytes it sends.
#include "../../recomp/runtime/wii_board.c"
#include "../harness.h"

static double now[4];
void input_board_now(double kilograms[4]) { memcpy(kilograms, now, sizeof now); }

static int big16(const uint8_t *b) { return b[0] << 8 | b[1]; }

int main(void) {
  uint8_t id[6];
  board_read(0xfa, id, 6);
  CHECK_EQ(id[2], 0xa4);
  CHECK_EQ(id[3], 0x20);
  CHECK_EQ(id[4], 0x04);
  CHECK_EQ(id[5], 0x02);

  // Each sensor: 0, 17 and 34 kg, 8 bytes apart.
  uint8_t calibration[24];
  board_read(0x24, calibration, sizeof calibration);
  CHECK_EQ(big16(calibration), 2000);
  CHECK_EQ(big16(calibration + 8), 3700);
  CHECK_EQ(big16(calibration + 16), 5400);
  CHECK_EQ(big16(calibration + 6), 2000);  // Bottom left, at nothing.

  // The game's start-up writes don't touch the identity or calibration.
  static const uint8_t start_up[] = {0x55};
  board_write(0xf0, start_up, 1);
  board_write(0x24, start_up, 1);
  board_read(0x24, calibration, 2);
  CHECK_EQ(big16(calibration), 2000);

  // 60 kg leaning right: more on the right sensors, and 17 kg reads as the 17 kg calibration.
  now[0] = 17; now[1] = 17; now[2] = 13; now[3] = 13;
  uint8_t report[11];
  board_fill(report, sizeof report);
  CHECK_EQ(big16(report), 3700);
  CHECK_EQ(big16(report + 2), 3700);
  CHECK_EQ(big16(report + 4), 3300);
  CHECK_EQ(report[8], 25);  // Room temperature, the same as the reference.

  uint8_t longer[19];
  board_fill(longer, sizeof longer);
  CHECK_EQ(longer[18], 0xff);
  return finish();
}
