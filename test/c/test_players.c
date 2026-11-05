// The four players' controls (input.c): each is its own, presses wear off, the pipe's
// commands reach the player they name, and a snapshot never mixes two moments while
// another thread writes. Compiled with input.c.
#include <pthread.h>
#include <string.h>
#include "harness.h"
#include "input.h"

static _Atomic uint64_t now;
uint64_t time_base(void) { return now; }

static _Atomic int writing;
static void *writer(void *unused) {
  (void)unused;
  for (int i = 0; writing; i++) {
    float value = (float)(i % 1000) / 1000;
    input_pointer(2, value, value);
    input_stick(2, value, value);
  }
  return 0;
}

int main(void) {
  // Player 1 is there from the start; the others join.
  CHECK(input_connected(0));
  for (int i = 1; i < PLAYERS; i++) CHECK(!input_connected(i));
  CHECK(!input_connected(PLAYERS));
  CHECK(!input_connected(-1));

  // Each player's buttons, pointer and stick are their own.
  input_hold(1, BUTTON_A, 1);
  input_pointer(3, 0.5f, -0.25f);
  input_stick(2, -1, 1);
  CHECK_EQ(input_buttons(0), 0);
  CHECK_EQ(input_buttons(1), BUTTON_A);
  PlayerState state;
  input_snapshot(3, &state);
  CHECK(state.pointer_x == 0.5f && state.pointer_y == -0.25f);
  CHECK(state.sticks[0] == 0 && state.sticks[1] == 0);
  input_snapshot(2, &state);
  CHECK(state.sticks[0] == -1 && state.sticks[1] == 1);
  CHECK(state.pointer_x == 0);
  input_hold(1, BUTTON_A, 0);
  input_hold(PLAYERS, BUTTON_A, 1);  // No such player: nothing happens.

  // A press lasts a tenth of a second.
  input_press(0, BUTTON_B);
  CHECK_EQ(input_buttons(0), BUTTON_B);
  now += 99 * 60750;
  CHECK_EQ(input_buttons(0), BUTTON_B);
  now += 2 * 60750;
  CHECK_EQ(input_buttons(0), 0);

  // The pipe's commands: player 1 unless one is named.
  CHECK(input_run_command("hold a\n"));
  CHECK_EQ(input_buttons(0), BUTTON_A);
  CHECK(input_run_command("p3 hold zr\n"));
  CHECK_EQ(input_buttons(2), BUTTON_CLASSIC_ZR);
  CHECK(input_run_command("release a\n"));
  CHECK_EQ(input_buttons(0), 0);
  CHECK(input_run_command("p4 pointer 0.25 0.75\n"));
  input_snapshot(3, &state);
  CHECK(state.pointer_x == 0.25f && state.pointer_y == 0.75f);
  CHECK(input_run_command("p2 connect\n"));
  CHECK(input_connected(1));
  CHECK(input_run_command("p2 disconnect\n"));
  CHECK(!input_connected(1));
  CHECK(input_run_command("pointer 0.1 0.2\n"));  // "pointer" isn't a player.
  input_snapshot(0, &state);
  CHECK(state.pointer_x == 0.1f && state.pointer_y == 0.2f);
  CHECK(!input_run_command("p5 hold a\n"));
  CHECK(!input_run_command("p0 hold a\n"));
  CHECK(!input_run_command("hold nothing\n"));  // An unknown button is an unknown command.
  CHECK(!input_run_command("p2 board on 50 0 0\n"));  // The board belongs to no player.
  CHECK(!input_run_command("shot x.png\n"));  // The console's commands are control_pipe.c's.
  CHECK(input_run_command("board on 50 0.5 0\n"));
  double sensors[4];
  input_board_now(sensors);
  CHECK(sensors[0] + sensors[1] + sensors[2] + sensors[3] > 49.99);
  CHECK(sensors[0] > sensors[2]);  // Leaning right.
  CHECK(input_board_total() == 50);
  CHECK(input_run_command("board off\n"));
  CHECK(input_board_total() == 0);

  // Motion, the Nunchuk, and what's plugged in.
  input_snapshot(0, &state);
  CHECK(state.accel[0] == 0 && state.accel[1] == 0 && state.accel[2] == 1);  // Still and flat until moved.
  CHECK(state.nunchuk_accel[2] == 1);
  CHECK_EQ(state.attachment, ATTACHMENT_CLASSIC);  // Unless WII_RECOMP_ATTACHMENT says otherwise.
  CHECK(input_run_command("p2 roll 90\n"));  // Turned on its side: gravity along -x.
  input_snapshot(1, &state);
  CHECK(state.accel[0] < -0.999f && state.accel[2] < 0.001f && state.accel[2] > -0.001f);
  CHECK(input_run_command("p2 accel 0.5 -1 2\n"));
  CHECK(input_run_command("p2 nunchuk stick 0.25 -0.5\n"));
  CHECK(input_run_command("p2 nunchuk accel 1 0 0\n"));
  CHECK(input_run_command("p2 attach nunchuk\n"));
  CHECK(input_run_command("p2 hold c\n"));
  input_snapshot(1, &state);
  CHECK(state.accel[0] == 0.5f && state.accel[1] == -1 && state.accel[2] == 2);
  CHECK(state.roll > 1.57f && state.roll < 1.571f);
  CHECK(state.nunchuk_stick[0] == 0.25f && state.nunchuk_stick[1] == -0.5f);
  CHECK(state.nunchuk_accel[0] == 1 && state.nunchuk_accel[2] == 0);
  CHECK_EQ(state.attachment, ATTACHMENT_NUNCHUK);
  CHECK_EQ(state.buttons, BUTTON_NUNCHUK_C);
  CHECK_EQ(input_attachment(0), ATTACHMENT_CLASSIC);
  CHECK(!input_run_command("p2 attach guitar\n"));
  CHECK(input_run_command("p2 attach none\n"));
  CHECK_EQ(input_attachment(1), ATTACHMENT_NONE);
  Attachment named;
  CHECK(input_attachment_named("classic", &named) && named == ATTACHMENT_CLASSIC);
  CHECK(!strcmp(input_attachment_name(ATTACHMENT_NUNCHUK), "nunchuk"));
  // A shake swings the remote's motion along its length, then stops.
  CHECK(input_run_command("p4 shake\n"));
  float lowest = 0, highest = 0;
  for (int i = 0; i < 8; i++, now += 25 * 60750) {
    input_snapshot(3, &state);
    lowest = state.accel[1] < lowest ? state.accel[1] : lowest;
    highest = state.accel[1] > highest ? state.accel[1] : highest;
  }
  CHECK(lowest <= -2 && highest >= 2);
  now += 500 * 60750;
  input_snapshot(3, &state);
  CHECK(state.accel[1] == 0);
  // Outputs and battery.
  input_set_outputs(2, 1, 0x9);
  CHECK_EQ(input_outputs(2).rumble, 1);
  CHECK_EQ(input_outputs(2).lights, 0x9);
  CHECK(input_battery_level(2) == 1);
  input_battery(2, 3);
  CHECK(input_battery_level(2) == 1);

  // A snapshot taken while another thread moves the pointer holds one moment.
  input_stick(2, 0, 0);
  pthread_t thread;
  writing = 1;
  pthread_create(&thread, 0, writer, 0);
  int mixed = 0;
  for (int i = 0; i < 200000; i++) {
    input_snapshot(2, &state);
    mixed += state.pointer_x != state.pointer_y || state.sticks[0] != state.sticks[1];
  }
  writing = 0;
  pthread_join(thread, 0);
  CHECK_EQ(mixed, 0);
  return finish();
}
