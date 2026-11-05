// The players' controls and the board's weight, as the host last set them. Writers are
// the window, the control pipe and controller threads; the readers are the simulated
// devices on the emulation thread. One lock covers it all, and a reader takes a whole
// player at once, so a report never mixes two moments.
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "input.h"

enum { PRESS_MS = 100, TICKS_PER_MS = 60750 };

typedef struct {
  int connected;
  uint32_t held, pulsed;
  uint64_t pulse_until;
  float pointer_x, pointer_y;
  float sticks[4];
} Player;

static Player players[PLAYERS] = {{.connected = 1}};
static struct {
  int standing;
  float kilograms, lean_x, lean_y;
} board = {.kilograms = 60};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int valid(int player) { return player >= 0 && player < PLAYERS; }

void input_connect(int player, int connected) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  players[player].connected = connected;
  pthread_mutex_unlock(&lock);
}

int input_connected(int player) {
  if (!valid(player)) return 0;
  pthread_mutex_lock(&lock);
  int connected = players[player].connected;
  pthread_mutex_unlock(&lock);
  return connected;
}

void input_hold(int player, uint32_t buttons, int down) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  Player *p = &players[player];
  p->held = down ? p->held | buttons : p->held & ~buttons;
  pthread_mutex_unlock(&lock);
}

void input_press(int player, uint32_t buttons) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  players[player].pulsed |= buttons;
  players[player].pulse_until = time_base() + PRESS_MS * TICKS_PER_MS;
  pthread_mutex_unlock(&lock);
}

void input_pointer(int player, float x, float y) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  players[player].pointer_x = x;
  players[player].pointer_y = y;
  pthread_mutex_unlock(&lock);
}

void input_stick(int player, float x, float y) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  players[player].sticks[0] = x;
  players[player].sticks[1] = y;
  pthread_mutex_unlock(&lock);
}

// Called with the lock held.
static uint32_t current_buttons(Player *p) {
  if (p->pulsed && time_base() >= p->pulse_until) p->pulsed = 0;
  return p->held | p->pulsed;
}

void input_snapshot(int player, PlayerState *out) {
  memset(out, 0, sizeof *out);
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  Player *p = &players[player];
  out->buttons = current_buttons(p);
  out->pointer_x = p->pointer_x;
  out->pointer_y = p->pointer_y;
  memcpy(out->sticks, p->sticks, sizeof out->sticks);
  pthread_mutex_unlock(&lock);
}

uint32_t input_buttons(int player) {
  if (!valid(player)) return 0;
  pthread_mutex_lock(&lock);
  uint32_t buttons = current_buttons(&players[player]);
  pthread_mutex_unlock(&lock);
  return buttons;
}

void input_board(int standing, float kilograms, float lean_x, float lean_y) {
  pthread_mutex_lock(&lock);
  board.standing = standing;
  board.kilograms = kilograms;
  board.lean_x = lean_x;
  board.lean_y = lean_y;
  pthread_mutex_unlock(&lock);
}

void input_board_nudge(float lean_x, float lean_y) {
  pthread_mutex_lock(&lock);
  board.lean_x = lean_x;
  board.lean_y = lean_y;
  pthread_mutex_unlock(&lock);
}

void input_board_toggle(void) {
  pthread_mutex_lock(&lock);
  board.standing = !board.standing;
  pthread_mutex_unlock(&lock);
}

float input_board_kilograms(void) {
  pthread_mutex_lock(&lock);
  float kilograms = board.kilograms;
  pthread_mutex_unlock(&lock);
  return kilograms;
}

double input_board_total(void) {
  pthread_mutex_lock(&lock);
  double total = board.standing ? board.kilograms : 0;
  pthread_mutex_unlock(&lock);
  return total;
}

// Sensors sit at front right, back right, front left, back left.
void input_board_now(double sensors[4]) {
  pthread_mutex_lock(&lock);
  double total = board.standing ? board.kilograms : 0;
  double right = (1 + board.lean_x) / 2, front = (1 + board.lean_y) / 2;
  pthread_mutex_unlock(&lock);
  sensors[0] = total * right * front;
  sensors[1] = total * right * (1 - front);
  sensors[2] = total * (1 - right) * front;
  sensors[3] = total * (1 - right) * (1 - front);
}

static uint32_t button_named(const char *name) {
  static const struct { const char *name; uint32_t bit; } names[] = {
    {"left", BUTTON_LEFT}, {"right", BUTTON_RIGHT}, {"down", BUTTON_DOWN}, {"up", BUTTON_UP}, {"plus", BUTTON_PLUS},
    {"two", BUTTON_TWO}, {"one", BUTTON_ONE}, {"b", BUTTON_B}, {"a", BUTTON_A}, {"minus", BUTTON_MINUS}, {"home", BUTTON_HOME},
    {"l", BUTTON_CLASSIC_L}, {"r", BUTTON_CLASSIC_R}, {"zl", BUTTON_CLASSIC_ZL}, {"zr", BUTTON_CLASSIC_ZR},
  };
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    if (!strcmp(name, names[i].name)) return names[i].bit;
  }
  return 0;
}

// The controls' commands: press a, hold a, release a, pointer x y, stick x y, board on
// kg x y, board off, connect, disconnect. Any of them but board may start with a
// player, as in "p2 press a"; without one it is player 1's.
int input_run_command(const char *line) {
  char word[16], name[16];
  float x = 0, y = 0, kilograms = input_board_kilograms();
  int player = 0, skipped = 0, number;
  if (sscanf(line, " p%d %n", &number, &skipped) == 1 && skipped) {
    if (number < 1 || number > PLAYERS) return 0;
    player = number - 1;
    line += skipped;
  }
  uint32_t button;
  if (sscanf(line, "press %15s", name) == 1 && (button = button_named(name))) input_press(player, button);
  else if (sscanf(line, "hold %15s", name) == 1 && (button = button_named(name))) input_hold(player, button, 1);
  else if (sscanf(line, "release %15s", name) == 1 && (button = button_named(name))) input_hold(player, button, 0);
  else if (sscanf(line, "pointer %f %f", &x, &y) == 2) input_pointer(player, x, y);
  else if (sscanf(line, "stick %f %f", &x, &y) == 2) input_stick(player, x, y);
  else if (!strncmp(line, "connect", 7)) input_connect(player, 1);
  else if (!strncmp(line, "disconnect", 10)) input_connect(player, 0);
  else if (!skipped && sscanf(line, "board %15s %f %f %f", word, &kilograms, &x, &y) >= 1 && (!strcmp(word, "on") || !strcmp(word, "off"))) {
    input_board(!strcmp(word, "on"), kilograms, x, y);
  } else return 0;
  return 1;
}
