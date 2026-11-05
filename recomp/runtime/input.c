// The players' controls and the board's weight, as the host last set them. Writers are
// the window, the control pipe and controller threads; the readers are the simulated
// devices on the emulation thread. One lock covers it all, and a reader takes a whole
// player at once, so a report never mixes two moments.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "input.h"

enum { PRESS_MS = 100, SHAKE_MS = 400, SHAKE_SWING_MS = 50, TICKS_PER_MS = 60750 };
static const float SHAKE_G = 2.5f;  // Well past what KPAD counts as a shake.

typedef struct {
  int connected, attachment_chosen;
  uint32_t held, pulsed, pad;
  uint64_t pulse_until, shake_until;
  float pointer_x, pointer_y, roll;
  float sticks[4];
  float accel[3], nunchuk_stick[2], nunchuk_accel[3];
  Attachment attachment;
  PlayerOutputs outputs;
  float battery;
  uint64_t sampled_at, sample_seen;
  int snapshots, ages_count;
  float ages[1024];  // Milliseconds from a controller's sample to the snapshot that took it.
} Player;

#define AT_REST {0, 0, 1}
static Player players[PLAYERS] = {
  {.connected = 1, .accel = AT_REST, .nunchuk_accel = AT_REST, .battery = 1},
  {.accel = AT_REST, .nunchuk_accel = AT_REST, .battery = 1},
  {.accel = AT_REST, .nunchuk_accel = AT_REST, .battery = 1},
  {.accel = AT_REST, .nunchuk_accel = AT_REST, .battery = 1},
};
static struct {
  int enabled, standing;
  float kilograms, lean_x, lean_y;
} board = {.enabled = 1, .kilograms = 60};
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
  return p->held | p->pulsed | p->pad;
}

void input_pad_buttons(int player, uint32_t buttons) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  players[player].pad = buttons;
  pthread_mutex_unlock(&lock);
}

void input_classic_sticks(int player, const float left[2], const float right[2]) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  memcpy(players[player].sticks, left, sizeof(float) * 2);
  memcpy(players[player].sticks + 2, right, sizeof(float) * 2);
  pthread_mutex_unlock(&lock);
}

static const char *attachment_names[ATTACHMENT_KINDS] = {"none", "nunchuk", "classic"};

const char *input_attachment_name(Attachment attachment) {
  return attachment >= 0 && attachment < ATTACHMENT_KINDS ? attachment_names[attachment] : "?";
}

int input_attachment_named(const char *name, Attachment *out) {
  for (int i = 0; i < ATTACHMENT_KINDS; i++) {
    if (!strcmp(name, attachment_names[i])) {
      *out = (Attachment)i;
      return 1;
    }
  }
  return 0;
}

// Until the settings (controllers.c) or the player plug one in, a remote has a Classic
// Controller. Called with the lock held.
static Attachment attachment_of(Player *p) { return p->attachment_chosen ? p->attachment : ATTACHMENT_CLASSIC; }

void input_snapshot(int player, PlayerState *out) {
  memset(out, 0, sizeof *out);
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  Player *p = &players[player];
  uint64_t now = time_base();
  out->buttons = current_buttons(p);
  out->pointer_x = p->pointer_x;
  out->pointer_y = p->pointer_y;
  out->roll = p->roll;
  memcpy(out->sticks, p->sticks, sizeof out->sticks);
  memcpy(out->accel, p->accel, sizeof out->accel);
  memcpy(out->nunchuk_stick, p->nunchuk_stick, sizeof out->nunchuk_stick);
  memcpy(out->nunchuk_accel, p->nunchuk_accel, sizeof out->nunchuk_accel);
  out->attachment = attachment_of(p);
  p->snapshots++;
  if (p->sampled_at != p->sample_seen) {
    p->sample_seen = p->sampled_at;
    if (p->ages_count < (int)(sizeof p->ages / sizeof *p->ages)) p->ages[p->ages_count++] = (float)(now - p->sampled_at) / TICKS_PER_MS;
  }
  // A shake swings the remote back and forth along its length.
  if (now < p->shake_until) out->accel[1] += (now / (SHAKE_SWING_MS * TICKS_PER_MS)) % 2 ? SHAKE_G : -SHAKE_G;
  pthread_mutex_unlock(&lock);
}

void input_motion(int player, const float accel[3], float roll) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  memcpy(players[player].accel, accel, sizeof players[player].accel);
  players[player].roll = roll;
  pthread_mutex_unlock(&lock);
}

void input_accel(int player, const float accel[3]) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  memcpy(players[player].accel, accel, sizeof players[player].accel);
  pthread_mutex_unlock(&lock);
}

void input_roll(int player, float roll) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  players[player].roll = roll;
  pthread_mutex_unlock(&lock);
}

void input_nunchuk(int player, float stick_x, float stick_y, const float accel[3]) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  players[player].nunchuk_stick[0] = stick_x;
  players[player].nunchuk_stick[1] = stick_y;
  memcpy(players[player].nunchuk_accel, accel, sizeof players[player].nunchuk_accel);
  pthread_mutex_unlock(&lock);
}

void input_nunchuk_stick(int player, float x, float y) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  players[player].nunchuk_stick[0] = x;
  players[player].nunchuk_stick[1] = y;
  pthread_mutex_unlock(&lock);
}

void input_nunchuk_accel(int player, const float accel[3]) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  memcpy(players[player].nunchuk_accel, accel, sizeof players[player].nunchuk_accel);
  pthread_mutex_unlock(&lock);
}

void input_shake(int player) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  players[player].shake_until = time_base() + SHAKE_MS * TICKS_PER_MS;
  pthread_mutex_unlock(&lock);
}

void input_attach(int player, Attachment attachment) {
  if (!valid(player) || attachment < 0 || attachment >= ATTACHMENT_KINDS) return;
  pthread_mutex_lock(&lock);
  players[player].attachment = attachment;
  players[player].attachment_chosen = 1;
  pthread_mutex_unlock(&lock);
}

Attachment input_attachment(int player) {
  if (!valid(player)) return ATTACHMENT_NONE;
  pthread_mutex_lock(&lock);
  Attachment attachment = attachment_of(&players[player]);
  pthread_mutex_unlock(&lock);
  return attachment;
}

void input_set_outputs(int player, int rumble, uint8_t lights) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  players[player].outputs.rumble = rumble;
  players[player].outputs.lights = lights;
  pthread_mutex_unlock(&lock);
}

PlayerOutputs input_outputs(int player) {
  PlayerOutputs outputs = {0};
  if (!valid(player)) return outputs;
  pthread_mutex_lock(&lock);
  outputs = players[player].outputs;
  pthread_mutex_unlock(&lock);
  return outputs;
}

void input_battery(int player, float level) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  players[player].battery = level < 0 ? 0 : level > 1 ? 1 : level;
  pthread_mutex_unlock(&lock);
}

float input_battery_level(int player) {
  if (!valid(player)) return 0;
  pthread_mutex_lock(&lock);
  float level = players[player].battery;
  pthread_mutex_unlock(&lock);
  return level;
}

uint32_t input_buttons(int player) {
  if (!valid(player)) return 0;
  pthread_mutex_lock(&lock);
  uint32_t buttons = current_buttons(&players[player]);
  pthread_mutex_unlock(&lock);
  return buttons;
}

void input_board_enable(int enabled) {
  pthread_mutex_lock(&lock);
  board.enabled = enabled;
  pthread_mutex_unlock(&lock);
}

int input_board_enabled(void) {
  pthread_mutex_lock(&lock);
  int enabled = board.enabled;
  pthread_mutex_unlock(&lock);
  return enabled;
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
    {"c", BUTTON_NUNCHUK_C}, {"z", BUTTON_NUNCHUK_Z},
  };
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    if (!strcmp(name, names[i].name)) return names[i].bit;
  }
  return 0;
}

// The controls' commands: press a, hold a, release a, pointer x y, stick x y, board on
// kg x y, board off, connect, disconnect, accel x y z, roll degrees (held still), shake, nunchuk
// stick x y, nunchuk accel x y z, attach none|nunchuk|classic. Any of them but board may
// start with a player, as in "p2 press a"; without one it is player 1's.
int input_run_command(const char *line) {
  char word[16], name[16];
  float x = 0, y = 0, z = 0, kilograms = input_board_kilograms();
  Attachment attachment;
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
  else if (sscanf(line, "accel %f %f %f", &x, &y, &z) == 3) input_accel(player, (float[3]){x, y, z});
  else if (sscanf(line, "roll %f", &x) == 1) {
    // Turned and otherwise still: gravity moves round to the remote's side as well, which
    // the game uses with the camera's picture to keep the pointer level.
    float turn = x * (float)M_PI / 180;
    input_motion(player, (float[3]){-sinf(turn), 0, cosf(turn)}, turn);
  }
  else if (!strncmp(line, "shake", 5)) input_shake(player);
  else if (sscanf(line, "nunchuk stick %f %f", &x, &y) == 2) input_nunchuk_stick(player, x, y);
  else if (sscanf(line, "nunchuk accel %f %f %f", &x, &y, &z) == 3) {
    input_nunchuk_accel(player, (float[3]){x, y, z});
  } else if (sscanf(line, "attach %15s", name) == 1 && input_attachment_named(name, &attachment)) input_attach(player, attachment);
  else if (!strncmp(line, "connect", 7)) input_connect(player, 1);
  else if (!strncmp(line, "disconnect", 10)) input_connect(player, 0);
  else if (!skipped && sscanf(line, "board %15s %f %f %f", word, &kilograms, &x, &y) >= 1 && (!strcmp(word, "on") || !strcmp(word, "off"))) {
    input_board(!strcmp(word, "on"), kilograms, x, y);
  } else return 0;
  return 1;
}

void input_pad_sampled(int player) {
  if (!valid(player)) return;
  pthread_mutex_lock(&lock);
  players[player].sampled_at = time_base();
  pthread_mutex_unlock(&lock);
}

static int by_size(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

InputStats input_stats(int player) {
  InputStats stats = {0};
  if (!valid(player)) return stats;
  static float ages[1024];
  pthread_mutex_lock(&lock);
  Player *p = &players[player];
  stats.snapshots = p->snapshots;
  stats.samples_seen = p->ages_count;
  memcpy(ages, p->ages, sizeof(float) * (size_t)p->ages_count);
  p->snapshots = p->ages_count = 0;
  pthread_mutex_unlock(&lock);
  if (stats.samples_seen) {
    qsort(ages, (size_t)stats.samples_seen, sizeof *ages, by_size);
    stats.age_p50_ms = ages[stats.samples_seen / 2];
    stats.age_p99_ms = ages[(stats.samples_seen * 99) / 100];
  }
  return stats;
}
