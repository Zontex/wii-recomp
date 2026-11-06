// The simulated IR camera: its registers, where the dots land, and both report formats.
#include "../../recomp/runtime/wii_camera.c"
#include "../harness.h"

static int unpack_x(const uint8_t *b, int second) { return second ? b[3] | (b[2] & 3) << 8 : b[0] | ((b[2] >> 4) & 3) << 8; }
static int unpack_y(const uint8_t *b, int second) { return second ? b[4] | ((b[2] >> 2) & 3) << 8 : b[1] | ((b[2] >> 6) & 3) << 8; }

int main(void) {
  uint8_t report[12];
  camera_fill(0, 0, 0, 0, report, 12);
  for (int i = 0; i < 12; i++) CHECK_EQ(report[i], 0xff);  // Off until the game turns tracking on.

  static const uint8_t on = 0x08, basic = 1, extended = 3;
  camera_write(0, 0x30, &on, 1);
  camera_write(0, 0x33, &basic, 1);
  CHECK_EQ(camera_mode(0), 1);
  uint8_t back;
  camera_read(0, 0x33, &back, 1);
  CHECK_EQ(back, 1);

  // Pointing at the middle puts the pair either side of the camera's centre.
  camera_fill(0, 0, 0, 0, report, 10);
  CHECK_EQ(unpack_x(report, 0), 512 - 65);
  CHECK_EQ(unpack_x(report, 1), 512 + 65);
  CHECK_EQ(unpack_y(report, 0), 252);  // Above the middle: the sensor bar is below the TV.
  CHECK_EQ(unpack_y(report, 1), 252);
  CHECK_EQ(unpack_x(report + 5, 0), 0x3ff);  // The second pair isn't there.

  // Pointing right and down moves the lights left and down in the camera's picture.
  int dots[4][2];
  camera_dots(1, 1, 0, dots);
  CHECK(dots[0][0] < 512 - 65);
  CHECK(dots[0][1] > 252);
  camera_dots(0, 1, 0, dots);  // The bottom edge: far enough for every game measured.
  CHECK_EQ(dots[0][1], 252 + 226);
  camera_set_bar_above(1);  // A bar above the TV is seen below the middle.
  camera_dots(0, 0, 0, dots);
  CHECK_EQ(dots[0][1], 516);
  camera_set_bar_above(0);

  // Turned a quarter clockwise, the lights stand one above the other, the picture turned
  // the other way about its middle.
  camera_dots(0, 0, (float)M_PI / 2, dots);
  CHECK_EQ(dots[0][0], dots[1][0]);
  CHECK_EQ(dots[0][1] - dots[1][1], 130);
  CHECK_EQ(dots[0][0], 512 - 132);  // The bar's offset below the middle, turned to the left.

  // Far off the screen, the lights leave the camera's view.
  camera_dots(3, 0, 0, dots);
  CHECK_EQ(dots[0][0], 0x3ff);
  CHECK_EQ(dots[1][0], 0x3ff);

  camera_write(0, 0x33, &extended, 1);
  camera_fill(0, 0, 0, 0, report, 12);
  CHECK_EQ(report[0] | ((report[2] >> 4) & 3) << 8, 512 - 65);
  CHECK_EQ(report[1] | ((report[2] >> 6) & 3) << 8, 252);
  CHECK_EQ(report[2] & 15, 3);  // Size.
  CHECK_EQ(report[6], 0xff);    // The third dot isn't there.

  // Each remote has its own camera: the second's is still off.
  CHECK_EQ(camera_mode(1), 0);
  camera_fill(1, 0, 0, 0, report, 12);
  CHECK_EQ(report[0], 0xff);
  camera_reset(0);
  CHECK_EQ(camera_mode(0), 0);
  return finish();
}
