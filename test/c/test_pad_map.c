// How a host controller stands in for a Wii Remote (pad_map.c), with made-up samples:
// each layout's buttons, motion turned into the remote's axes, the remote's roll, the
// gyro pointer, and the gyro's drift taken out while the controller is still. Compiled
// with pad_map.c.
#include <math.h>
#include <stdlib.h>
#include "harness.h"
#include "pad_map.h"

static int near(float got, float want, float within) { return fabsf(got - want) <= within; }

// Seconds of the same reading at 200 samples a second.
static void hold(PadPointer *pointer, const float accel[3], const float gyro[3], float seconds, int sideways) {
  for (int i = 0; i < (int)(seconds * 200 + 0.5f); i++) pad_pointer_update(pointer, accel, gyro, 0.005f, 1, sideways);
}

int main(void) {
  CHECK_EQ(pad_button_named("zr"), PAD_ZR);
  CHECK_EQ(pad_button_named("right_stick"), PAD_RIGHT_STICK);
  CHECK_EQ(pad_button_named("none"), 0);
  CHECK_EQ(pad_layout(ATTACHMENT_CLASSIC, 1), PAD_AS_CLASSIC);
  CHECK_EQ(pad_layout(ATTACHMENT_NUNCHUK, 0), PAD_AS_REMOTE);
  CHECK_EQ(pad_layout(ATTACHMENT_NONE, 1), PAD_AS_SIDEWAYS);

  // Upright: A on the right, B under ZR (and on b), 1 and 2 on y and x, C and Z on the
  // left shoulders.
  PadInput pad = {.buttons = PAD_A | PAD_ZR | PAD_Y | PAD_L | PAD_ZL | PAD_UP | PAD_HOME};
  CHECK_EQ(pad_wii_buttons(&pad, PAD_AS_REMOTE), BUTTON_A | BUTTON_B | BUTTON_ONE | BUTTON_NUNCHUK_C | BUTTON_NUNCHUK_Z | BUTTON_UP | BUTTON_HOME);
  pad = (PadInput){.buttons = PAD_X | PAD_B | PAD_PLUS};
  CHECK_EQ(pad_wii_buttons(&pad, PAD_AS_REMOTE), BUTTON_TWO | BUTTON_B | BUTTON_PLUS);
  // The Classic Controller where a Nintendo controller has them; its x and y are 1 and 2.
  pad = (PadInput){.buttons = PAD_X | PAD_Y | PAD_L | PAD_R | PAD_ZL | PAD_ZR | PAD_MINUS | PAD_LEFT};
  CHECK_EQ(pad_wii_buttons(&pad, PAD_AS_CLASSIC), BUTTON_ONE | BUTTON_TWO | BUTTON_CLASSIC_L | BUTTON_CLASSIC_R |
                                                  BUTTON_CLASSIC_ZL | BUTTON_CLASSIC_ZR | BUTTON_MINUS | BUTTON_LEFT);
  // Sideways, a quarter turn: the way up in the hand is the remote's right, and the stick works the D-pad.
  pad = (PadInput){.buttons = PAD_UP | PAD_B | PAD_A};
  CHECK_EQ(pad_wii_buttons(&pad, PAD_AS_SIDEWAYS), BUTTON_RIGHT | BUTTON_ONE | BUTTON_TWO);
  pad = (PadInput){.left = {-0.9f, 0}};
  CHECK_EQ(pad_wii_buttons(&pad, PAD_AS_SIDEWAYS), BUTTON_UP);
  pad = (PadInput){.left = {0.2f, -0.8f}};
  CHECK_EQ(pad_wii_buttons(&pad, PAD_AS_SIDEWAYS), BUTTON_LEFT);
  CHECK_EQ(pad_wii_buttons(&pad, PAD_AS_REMOTE), 0);  // Upright, the stick is the Nunchuk's.

  // Motion into the remote's axes: flat, pointing up, turned clockwise.
  float remote[3];
  pad_remote_axes((float[3]){0, 1, 0}, 0, remote);
  CHECK(near(remote[0], 0, 1e-6f) && near(remote[1], 0, 1e-6f) && near(remote[2], 1, 1e-6f));
  pad_remote_axes((float[3]){0, 0, -1}, 0, remote);  // Gravity's pull along the controller's far side: pointing up.
  CHECK(near(remote[1], 1, 1e-6f));
  float turn = 0.5f;
  pad_remote_axes((float[3]){-sinf(turn), cosf(turn), 0}, 0, remote);
  CHECK(near(remote[0], -sinf(turn), 1e-6f) && near(remote[2], cosf(turn), 1e-6f));  // As the pipe's roll gives.
  pad_remote_axes((float[3]){1, 0, 0}, 1, remote);  // Sideways, the far end is the controller's right.
  CHECK(near(remote[1], 1, 1e-6f));

  // Roll from gravity, turned clockwise as its holder sees it.
  PadPointer pointer;
  pad_pointer_reset(&pointer);
  hold(&pointer, (float[3]){-sinf(turn), cosf(turn), 0}, (float[3]){0, 0, 0}, 0.5f, 0);
  CHECK(near(pad_roll(&pointer, 0), turn, 0.01f));
  hold(&pointer, (float[3]){0, 1, 0}, (float[3]){0, 0, 0}, 0.5f, 0);
  CHECK(near(pad_roll(&pointer, 0), 0, 0.01f));

  // Turning right by the screen's reach moves the pointer to its right side; tipping the
  // front up moves it up. SDL counts turns counter-clockwise, so right is negative.
  pad_pointer_reset(&pointer);
  hold(&pointer, (float[3]){0, 1, 0}, (float[3]){0, -20, 0}, 1, 0);
  CHECK(near(pointer.x, 1, 0.01f));
  CHECK(near(pointer.y, 0, 0.01f));
  hold(&pointer, (float[3]){0, 1, 0}, (float[3]){15, 0, 0}, 1, 0);
  CHECK(near(pointer.y, -1, 0.01f));
  pad_pointer_centre(&pointer);
  CHECK(pointer.x == 0 && pointer.y == 0);
  // Far past the edge, it stops where pointing back brings it in again soon.
  hold(&pointer, (float[3]){0, 1, 0}, (float[3]){0, 90, 0}, 2, 0);
  CHECK(near(pointer.x, -1.6f, 0.001f));
  // Held on its side, turning right is still about the way up: the controller's x.
  pad_pointer_reset(&pointer);
  hold(&pointer, (float[3]){-1, 0, 0}, (float[3]){0, 0, 0}, 0.3f, 0);
  pad_pointer_centre(&pointer);
  hold(&pointer, (float[3]){-1, 0, 0}, (float[3]){20, 0, 0}, 1, 0);
  CHECK(near(pointer.x, 1, 0.03f));

  // Drift: a gyro reading half a degree a second or so when still, with noise. After a
  // second of stillness that's taken as zero, and a minute later the pointer has moved
  // less than one percent of the screen.
  pad_pointer_reset(&pointer);
  srand(7);
  const float bias[3] = {0.5f, -0.8f, 0.3f};
  for (int second = 0; second < 62; second++) {
    if (second == 2) pad_pointer_centre(&pointer);
    for (int i = 0; i < 200; i++) {
      float gyro[3];
      for (int axis = 0; axis < 3; axis++) gyro[axis] = bias[axis] + ((float)rand() / RAND_MAX - 0.5f) * 0.6f;
      pad_pointer_update(&pointer, (float[3]){0, 1, 0}, gyro, 0.005f, 1, 0);
    }
  }
  CHECK(fabsf(pointer.x) < 0.02f);
  CHECK(fabsf(pointer.y) < 0.02f);
  CHECK(near(pointer.bias[1], -0.8f, 0.05f));
  // A real turn isn't taken for drift.
  hold(&pointer, (float[3]){0, 1, 0}, (float[3]){0, -10, 0}, 1.5f, 0);
  CHECK(near(pointer.bias[1], -0.8f, 0.05f));

  // A stick moves the pointer as a mouse would, and stays on the screen.
  pad_pointer_reset(&pointer);
  for (int i = 0; i < 100; i++) pad_pointer_stick(&pointer, (float[2]){1, 1}, 0.01f, 1);
  CHECK(near(pointer.x, 1, 0.001f) && near(pointer.y, -1, 0.001f));
  return finish();
}
