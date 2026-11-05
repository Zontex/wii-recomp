// Controllers through SDL (pads.c), with SDL's virtual controllers standing in for real
// ones: a Pro Controller joins player 1, its buttons, stick, motion and gyro reach the
// player as the settings say, the game's rumble and lights reach it, and a second
// controller makes player 2 join and leave. Compiled with pads.c, pad_map.c, input.c,
// controllers.c and settings_file.c, and SDL3.
#include <math.h>
#include <string.h>
#include <time.h>
#include "harness.h"
#include "controllers.h"
#include "input.h"
#include "pads.h"

static _Atomic uint64_t now;
uint64_t time_base(void) { return now; }

// Polls for this long, as the window's loop does.
static void run(float seconds) {
  for (int i = 0; i < (int)(seconds * 250); i++) {
    pads_poll();
    now += 4 * 60750;
    nanosleep(&(struct timespec){0, 4000000}, 0);
  }
}

static void send(const char *line) {
  CHECK(pads_command(line));
  run(0.05f);
}

int main(void) {
  CHECK(!pads_command("press a\n"));  // Not a controller command.
  pads_start();
  input_connect(0, 1);  // As controllers_start leaves player 1 with the defaults.
  input_attach(0, ATTACHMENT_NUNCHUK);
  send("pad add pro\n");
  CHECK(input_connected(0));
  CHECK(pads_wait_seconds() < 0.01);  // Polled often while a controller is connected.

  // Upright with a Nunchuk: A is A, ZR is B, L is C, the left stick is the Nunchuk's.
  send("pad hold 1 a\n");
  send("pad hold 1 zr\n");
  send("pad hold 1 l\n");
  send("pad stick 1 left 1 0\n");
  CHECK_EQ(input_buttons(0), BUTTON_A | BUTTON_B | BUTTON_NUNCHUK_C);
  PlayerState state;
  input_snapshot(0, &state);
  CHECK(state.nunchuk_stick[0] > 0.99f);
  send("pad release 1 a\n");
  send("pad release 1 zr\n");
  send("pad release 1 l\n");
  send("pad stick 1 left 0 0\n");
  CHECK_EQ(input_buttons(0), 0);
  // The keyboard's holds and the controller's are each their own.
  input_hold(0, BUTTON_TWO, 1);
  send("pad hold 1 a\n");
  send("pad release 1 a\n");
  CHECK_EQ(input_buttons(0), BUTTON_TWO);
  input_hold(0, BUTTON_TWO, 0);

  // With a Classic Controller, its buttons and both sticks.
  input_attach(0, ATTACHMENT_CLASSIC);
  send("pad hold 1 x\n");
  send("pad stick 1 right -1 0\n");
  CHECK_EQ(input_buttons(0), BUTTON_ONE);
  input_snapshot(0, &state);
  CHECK(state.sticks[2] < -0.99f);
  send("pad release 1 x\n");
  send("pad stick 1 right 0 0\n");
  input_attach(0, ATTACHMENT_NUNCHUK);

  // Turned clockwise and still: the remote's motion and roll follow.
  send("pad accel 1 -0.5 0.866 0\n");
  run(0.5f);
  input_snapshot(0, &state);
  CHECK(fabsf(state.accel[0] + 0.5f) < 0.01f && fabsf(state.accel[2] - 0.866f) < 0.01f);
  CHECK(fabsf(state.roll - (float)M_PI / 6) < 0.05f);
  send("pad accel 1 0 1 0\n");
  run(0.5f);

  // Turning right moves the pointer right; the recenter button brings it back.
  send("pad hold 1 right_stick\n");
  send("pad release 1 right_stick\n");
  send("pad gyro 1 0 -20 0\n");
  run(0.5f);
  send("pad gyro 1 0 0 0\n");
  input_snapshot(0, &state);
  CHECK(state.pointer_x > 0.35f && state.pointer_x < 0.65f);  // Half the reach in half a second.
  CHECK(fabsf(state.pointer_y) < 0.05f);
  send("pad hold 1 right_stick\n");
  send("pad release 1 right_stick\n");
  input_snapshot(0, &state);
  CHECK(fabsf(state.pointer_x) < 0.01f);

  // A script's turn lands exactly: ten degrees right is half the way to the side, and
  // six up is two fifths of the way to the top (the picture is 4:3).
  send("pad turn 1 10 6\n");
  run(0.3f);
  input_snapshot(0, &state);
  CHECK(fabsf(state.pointer_x - 0.5f) < 0.005f);
  CHECK(fabsf(state.pointer_y + 0.4f) < 0.005f);
  // A press lets go by itself.
  CHECK(pads_command("pad press 1 a\n"));
  run(0.03f);
  CHECK_EQ(input_buttons(0), BUTTON_A);
  run(0.2f);
  CHECK_EQ(input_buttons(0), 0);

  // The game's rumble and lights reach the controller; the log shows what it was told.
  input_set_outputs(0, 1, 0x2);
  run(0.1f);
  input_set_outputs(0, 0, 0x2);
  run(0.1f);

  // A second controller: player 2 joins with it and leaves with it. Player 1 stays for
  // the keyboard when theirs goes.
  send("pad add joycons\n");
  CHECK(input_connected(1));
  send("pad hold 2 b\n");
  CHECK_EQ(input_buttons(1), BUTTON_B);
  send("pad remove 2\n");
  CHECK(!input_connected(1));
  CHECK_EQ(input_buttons(1), 0);
  send("pad remove 1\n");
  CHECK(input_connected(0));
  CHECK(pads_wait_seconds() > 0.01);
  send("pad remove 1\n");  // Gone already: said so in the log.
  return finish();
}
