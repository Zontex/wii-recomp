// Host controllers through SDL3, as pads.h describes. SDL starts with controllers and
// sensors only; the window stays GLFW's. A controller that connects goes to the first
// player whose settings take a controller (that one by serial number, first), that
// player's remote calls the console, and from then on each poll passes its buttons,
// sticks, motion and pointer to the player, and the game's rumble and lights back.
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controllers.h"
#include "input.h"
#include "pad_map.h"
#include "pads.h"

enum { PADS = 8, VIRTUAL_PADS = 4, COMMANDS = 64, RUMBLE_REFRESH_MS = 100, RUMBLE_FOR_MS = 250, BATTERY_EVERY_MS = 5000,
       STATS_EVERY_MS = 5000 };
static const double POLL_CONNECTED = 0.004, POLL_IDLE = 0.05;  // Seconds between polls.
static const float TRIGGER_PRESSED = 0.5f, TURN_SPEED = 120;  // Degrees a second, for a virtual controller's turn.
static const Uint64 PRESS_NS = 100000000;  // How long pad press holds a button, as the pipe's press does.

typedef struct {
  SDL_Gamepad *gamepad;
  SDL_JoystickID id;
  int player;  // -1 while no player takes it.
  char name[64], serial[64];
  int single_joycon;
  PadPointer pointer;
  float accel[3], nunchuk_accel[3], gyro[3];  // Latest samples, accel in g and gyro in degrees a second, SDL's axes.
  int has_gyro, has_left_accel;
  SDL_SensorType motion_accel, motion_gyro;
  uint64_t last_gyro_ns;
  uint32_t last_buttons;
  int rumbling;
  uint8_t lights_shown;
  uint64_t rumble_sent_ms, battery_ms;
  int samples;  // Gyro samples since the last stats line.
} Pad;

static Pad pads[PADS];
static int started, pad_count;
static uint64_t last_poll_ns, stats_ms;
static int stats_on;

// Virtual controllers for the control pipe, made through SDL so tests take the real path.
typedef struct {
  SDL_JoystickID id;
  SDL_Joystick *joystick;
  int joycons, number, rumbling;
  float accel[3], gyro[3];  // What it reports each poll: g and degrees a second.
  float turn_left[3];       // Degrees still to turn, about each of SDL's axes, for pad turn.
  int pressed;              // A button pad press holds, let go at release_ns.
  Uint64 release_ns, last_ns;
} VirtualPad;
static VirtualPad virtual_pads[VIRTUAL_PADS];
static char commands[COMMANDS][256];
static int command_count;
static pthread_mutex_t command_lock = PTHREAD_MUTEX_INITIALIZER;

void pads_start(void) {
  // SDL owns no window, so it must take controller input whichever window has focus.
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH2, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "1");  // A pair in the hands is one controller.
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, "0");  // One Joy-Con alone is held sideways.
  if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_SENSOR)) {
    fprintf(stderr, "controllers: SDL couldn't start, so only the keyboard, mouse and real remotes work: %s\n", SDL_GetError());
    return;
  }
  int version = SDL_GetVersion();
  fprintf(stderr, "controllers: SDL %d.%d.%d is reading controllers\n", SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version),
          SDL_VERSIONNUM_MICRO(version));
  stats_on = getenv("WII_RECOMP_INPUT_STATS") != 0;
  started = 1;
}

double pads_wait_seconds(void) { return pad_count ? POLL_CONNECTED : POLL_IDLE; }

static Pad *pad_with(SDL_JoystickID id) {
  for (int i = 0; i < PADS; i++) {
    if (pads[i].gamepad && pads[i].id == id) return &pads[i];
  }
  return 0;
}

static int player_has_pad(int player) {
  for (int i = 0; i < PADS; i++) {
    if (pads[i].gamepad && pads[i].player == player) return 1;
  }
  return 0;
}

// The player a new controller is for: one whose settings name its serial number, then the
// first that takes any controller and has none.
static int choose_player(const Pad *pad) {
  for (int p = 0; p < PLAYERS; p++) {
    const PlayerSettings *settings = controllers_player(p);
    if ((settings->sources & SOURCE_PAD) && settings->pad[0] && !strcmp(settings->pad, pad->serial) && !player_has_pad(p)) return p;
  }
  for (int p = 0; p < PLAYERS; p++) {
    const PlayerSettings *settings = controllers_player(p);
    if ((settings->sources & SOURCE_PAD) && !settings->pad[0] && !player_has_pad(p)) return p;
  }
  return -1;
}

static void enable_sensor(Pad *pad, SDL_SensorType type) {
  if (SDL_GamepadHasSensor(pad->gamepad, type)) SDL_SetGamepadSensorEnabled(pad->gamepad, type, true);
}

static void pad_added(SDL_JoystickID id) {
  if (pad_with(id)) return;
  Pad *pad = 0;
  for (int i = 0; i < PADS && !pad; i++) {
    if (!pads[i].gamepad) pad = &pads[i];
  }
  if (!pad) {
    fprintf(stderr, "controllers: %d controllers are connected already, so this one isn't used\n", PADS);
    return;
  }
  SDL_Gamepad *gamepad = SDL_OpenGamepad(id);
  if (!gamepad) {
    fprintf(stderr, "controllers: a controller connected but couldn't be opened: %s\n", SDL_GetError());
    return;
  }
  memset(pad, 0, sizeof *pad);
  pad->gamepad = gamepad;
  pad->id = id;
  pad->player = -1;  // Not yet anyone's, while one is chosen.
  snprintf(pad->name, sizeof pad->name, "%s", SDL_GetGamepadName(gamepad) ? SDL_GetGamepadName(gamepad) : "a controller");
  snprintf(pad->serial, sizeof pad->serial, "%s", SDL_GetGamepadSerial(gamepad) ? SDL_GetGamepadSerial(gamepad) : "");
  SDL_GamepadType type = SDL_GetGamepadType(gamepad);
  pad->single_joycon = type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT;
  pad_pointer_reset(&pad->pointer);
  pad->accel[1] = pad->nunchuk_accel[1] = 1;  // Flat and still until it says otherwise.
  // A Joy-Con pair moves as two hands: the right one is the remote, the left the Nunchuk.
  int pair = SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO_R);
  pad->motion_accel = pair ? SDL_SENSOR_ACCEL_R : SDL_SENSOR_ACCEL;
  pad->motion_gyro = pair ? SDL_SENSOR_GYRO_R : SDL_SENSOR_GYRO;
  pad->has_gyro = SDL_GamepadHasSensor(gamepad, pad->motion_gyro);
  pad->has_left_accel = SDL_GamepadHasSensor(gamepad, SDL_SENSOR_ACCEL_L);
  enable_sensor(pad, pad->motion_accel);
  enable_sensor(pad, pad->motion_gyro);
  if (pad->has_left_accel) enable_sensor(pad, SDL_SENSOR_ACCEL_L);
  pad->player = choose_player(pad);
  pad_count++;
  if (pad->player < 0) {
    fprintf(stderr, "controllers: %s connected, but no player takes a controller: set a player's source to pad in controllers.ini\n", pad->name);
    return;
  }
  input_connect(pad->player, 1);
  fprintf(stderr, "controllers: %s%s%s is player %d%s\n", pad->name, pad->serial[0] ? " " : "", pad->serial, pad->player + 1,
          pad->has_gyro ? ", with its gyro as the pointer" : ", with no gyro");
}

static void pad_removed(SDL_JoystickID id) {
  Pad *pad = pad_with(id);
  if (!pad) return;
  int player = pad->player;
  SDL_CloseGamepad(pad->gamepad);
  pad->gamepad = 0;
  pad_count--;
  if (player < 0) return;
  // The player's remote lets go of everything, and leaves unless a keyboard or mouse keeps it.
  input_pad_buttons(player, 0);
  if (!(controllers_player(player)->sources & (SOURCE_KEYBOARD | SOURCE_MOUSE))) input_connect(player, 0);
  fprintf(stderr, "controllers: player %d's %s disconnected\n", player + 1, pad->name);
}

static void sensor_sample(const SDL_GamepadSensorEvent *event) {
  Pad *pad = pad_with(event->which);
  if (!pad) return;
  float *into = event->sensor == pad->motion_accel ? pad->accel : event->sensor == SDL_SENSOR_ACCEL_L ? pad->nunchuk_accel
              : event->sensor == pad->motion_gyro ? pad->gyro : 0;
  if (!into) return;
  int gyro = into == pad->gyro;
  for (int i = 0; i < 3; i++) into[i] = gyro ? event->data[i] * (float)(180 / M_PI) : event->data[i] / SDL_STANDARD_GRAVITY;
  if (!gyro || pad->player < 0) return;
  // Each gyro sample turns the pointer by the time since the last, by the controller's own clock.
  float seconds = pad->last_gyro_ns ? (float)(event->sensor_timestamp - pad->last_gyro_ns) / 1e9f : 0;
  pad->last_gyro_ns = event->sensor_timestamp;
  const PlayerSettings *settings = controllers_player(pad->player);
  pad_pointer_update(&pad->pointer, pad->accel, pad->gyro, seconds, settings->gyro_speed, pad->single_joycon || settings->sideways);
  pad->samples++;
}

static uint32_t buttons_of(SDL_Gamepad *gamepad, float triggers[2]) {
  static const struct { SDL_GamepadButton from; uint32_t to; } buttons[] = {
    {SDL_GAMEPAD_BUTTON_EAST, PAD_A}, {SDL_GAMEPAD_BUTTON_SOUTH, PAD_B}, {SDL_GAMEPAD_BUTTON_NORTH, PAD_X},
    {SDL_GAMEPAD_BUTTON_WEST, PAD_Y}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_R},
    {SDL_GAMEPAD_BUTTON_START, PAD_PLUS}, {SDL_GAMEPAD_BUTTON_BACK, PAD_MINUS}, {SDL_GAMEPAD_BUTTON_GUIDE, PAD_HOME},
    {SDL_GAMEPAD_BUTTON_MISC1, PAD_CAPTURE}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_LEFT_STICK},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_RIGHT_STICK}, {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_UP},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DOWN}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_RIGHT},
  };
  uint32_t held = 0;
  for (size_t i = 0; i < sizeof buttons / sizeof *buttons; i++) {
    if (SDL_GetGamepadButton(gamepad, buttons[i].from)) held |= buttons[i].to;
  }
  triggers[0] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
  triggers[1] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
  if (triggers[0] > TRIGGER_PRESSED) held |= PAD_ZL;
  if (triggers[1] > TRIGGER_PRESSED) held |= PAD_ZR;
  return held;
}

static float axis(SDL_Gamepad *gamepad, SDL_GamepadAxis which, int flip) {
  float value = SDL_GetGamepadAxis(gamepad, which) / 32767.0f;
  if (value < -1) value = -1;
  return flip ? -value : value;
}

static void show_outputs(Pad *pad, uint64_t now_ms) {
  PlayerOutputs outputs = input_outputs(pad->player);
  // SDL lets a rumble run out after its time, so a long one is sent again while it lasts.
  if (outputs.rumble && (!pad->rumbling || now_ms - pad->rumble_sent_ms >= RUMBLE_REFRESH_MS)) {
    SDL_RumbleGamepad(pad->gamepad, 0x8000, 0xc000, RUMBLE_FOR_MS);
    pad->rumble_sent_ms = now_ms;
  } else if (!outputs.rumble && pad->rumbling) {
    SDL_RumbleGamepad(pad->gamepad, 0, 0, 0);
  }
  pad->rumbling = outputs.rumble;
  // A Switch controller has four player lights too: the remote's first lit one shows.
  if (outputs.lights != pad->lights_shown) {
    int index = -1;
    for (int i = 0; i < 4 && index < 0; i++) {
      if (outputs.lights & (1 << i)) index = i;
    }
    SDL_SetGamepadPlayerIndex(pad->gamepad, index);
    pad->lights_shown = outputs.lights;
  }
  if (!pad->battery_ms || now_ms - pad->battery_ms >= BATTERY_EVERY_MS) {
    int percent = -1;
    SDL_PowerState state = SDL_GetGamepadPowerInfo(pad->gamepad, &percent);
    input_battery(pad->player, state == SDL_POWERSTATE_ON_BATTERY && percent >= 0 ? percent / 100.0f : 1);
    pad->battery_ms = now_ms;
  }
}

static void pass_to_player(Pad *pad, float seconds, uint64_t now_ms) {
  int player = pad->player;
  const PlayerSettings *settings = controllers_player(player);
  Attachment attachment = input_attachment(player);
  int sideways = pad->single_joycon || settings->sideways;
  PadLayout layout = pad_layout(attachment, sideways);
  PadInput state = {0};
  state.buttons = buttons_of(pad->gamepad, state.triggers);
  state.left[0] = axis(pad->gamepad, SDL_GAMEPAD_AXIS_LEFTX, 0);
  state.left[1] = axis(pad->gamepad, SDL_GAMEPAD_AXIS_LEFTY, 1);  // SDL's y grows down.
  state.right[0] = axis(pad->gamepad, SDL_GAMEPAD_AXIS_RIGHTX, 0);
  state.right[1] = axis(pad->gamepad, SDL_GAMEPAD_AXIS_RIGHTY, 1);
  uint32_t pressed = state.buttons & ~pad->last_buttons;
  pad->last_buttons = state.buttons;
  input_pad_buttons(player, pad_wii_buttons(&state, layout));
  if (layout == PAD_AS_CLASSIC) input_classic_sticks(player, state.left, state.right);
  else if (layout == PAD_AS_REMOTE) input_nunchuk_stick(player, state.left[0], state.left[1]);
  if (layout == PAD_AS_REMOTE && (pressed & PAD_R)) input_shake(player);  // R, unused upright, waggles the remote.
  uint32_t recenter = pad_button_named(settings->recenter);
  if (recenter && (pressed & recenter)) pad_pointer_centre(&pad->pointer);
  if ((settings->pointer & POINTER_STICK) && layout == PAD_AS_REMOTE) pad_pointer_stick(&pad->pointer, state.right, seconds, settings->gyro_speed);
  if (settings->pointer & (POINTER_GYRO | POINTER_STICK)) input_pointer(player, pad->pointer.x, pad->pointer.y);
  if (pad->has_gyro || SDL_GamepadHasSensor(pad->gamepad, pad->motion_accel)) {
    float remote[3], nunchuk[3], turn[3], turn_remote[3];
    pad_remote_axes(pad->accel, sideways, remote);
    input_motion(player, remote, pad_roll(&pad->pointer, sideways));
    // A turn is turned into the remote's axes as motion is, less the gyro's zero, for a MotionPlus.
    for (int i = 0; i < 3; i++) turn[i] = pad->gyro[i] - pad->pointer.bias[i];
    pad_remote_axes(turn, sideways, turn_remote);
    input_gyro(player, turn_remote);
    if (pad->has_left_accel && layout == PAD_AS_REMOTE) {
      pad_remote_axes(pad->nunchuk_accel, 0, nunchuk);
      input_nunchuk_accel(player, nunchuk);
    }
  }
  input_pad_sampled(player);
  show_outputs(pad, now_ms);
}

// --- Virtual controllers, for the control pipe ---

int pads_command(const char *line) {
  if (strncmp(line, "pad ", 4)) return 0;
  pthread_mutex_lock(&command_lock);
  int room = command_count < COMMANDS;
  if (room) snprintf(commands[command_count++], sizeof commands[0], "%s", line + 4);
  pthread_mutex_unlock(&command_lock);
  if (!room) fprintf(stderr, "control: too many pad commands waiting, so this one is dropped: %s", line);
  return 1;
}

static const struct { const char *name; SDL_GamepadButton button; } virtual_buttons[] = {
  {"a", SDL_GAMEPAD_BUTTON_EAST}, {"b", SDL_GAMEPAD_BUTTON_SOUTH}, {"x", SDL_GAMEPAD_BUTTON_NORTH}, {"y", SDL_GAMEPAD_BUTTON_WEST},
  {"l", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"r", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"plus", SDL_GAMEPAD_BUTTON_START},
  {"minus", SDL_GAMEPAD_BUTTON_BACK}, {"home", SDL_GAMEPAD_BUTTON_GUIDE}, {"capture", SDL_GAMEPAD_BUTTON_MISC1},
  {"left_stick", SDL_GAMEPAD_BUTTON_LEFT_STICK}, {"right_stick", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
  {"dpad_up", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"dpad_down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
  {"dpad_left", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"dpad_right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};

// What a virtual controller is told to do shows in the log, for tests to see.
static bool SDLCALL virtual_rumble(void *userdata, Uint16 low, Uint16 high) {
  VirtualPad *pad = userdata;
  int rumbling = low || high;
  if (rumbling != pad->rumbling) fprintf(stderr, "control: virtual controller %d %s\n", pad->number, rumbling ? "rumbles" : "stops rumbling");
  pad->rumbling = rumbling;
  return true;
}

static void SDLCALL virtual_player(void *userdata, int index) {
  VirtualPad *pad = userdata;
  fprintf(stderr, "control: virtual controller %d shows player light %d\n", pad->number, index + 1);
}

static void virtual_add(const char *kind) {
  int slot = -1;
  for (int i = 0; i < VIRTUAL_PADS && slot < 0; i++) {
    if (!virtual_pads[i].joystick) slot = i;
  }
  int joycons = !strcmp(kind, "joycons");
  if (slot < 0 || (strcmp(kind, "pro") && !joycons)) {
    fprintf(stderr, "control: pad add takes pro or joycons, with at most %d at once\n", VIRTUAL_PADS);
    return;
  }
  static const SDL_VirtualJoystickSensorDesc pro_sensors[] = {{SDL_SENSOR_ACCEL, 200}, {SDL_SENSOR_GYRO, 200}};
  static const SDL_VirtualJoystickSensorDesc pair_sensors[] = {{SDL_SENSOR_ACCEL_R, 200}, {SDL_SENSOR_GYRO_R, 200},
                                                               {SDL_SENSOR_ACCEL_L, 200}, {SDL_SENSOR_GYRO_L, 200}};
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
  desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
  desc.nsensors = joycons ? 4 : 2;
  desc.sensors = joycons ? pair_sensors : pro_sensors;
  desc.name = joycons ? "Virtual Joy-Con pair" : "Virtual Pro Controller";
  VirtualPad *pad = &virtual_pads[slot];
  memset(pad, 0, sizeof *pad);
  pad->number = slot + 1;
  desc.userdata = pad;
  desc.Rumble = virtual_rumble;
  desc.SetPlayerIndex = virtual_player;
  SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  SDL_Joystick *joystick = id ? SDL_OpenJoystick(id) : 0;
  if (!joystick) {
    fprintf(stderr, "control: SDL couldn't make a virtual controller: %s\n", SDL_GetError());
    return;
  }
  pad->id = id;
  pad->joystick = joystick;
  pad->joycons = joycons;
  pad->accel[1] = 1;
  fprintf(stderr, "control: virtual controller %d is a %s\n", slot + 1, desc.name);
}

static VirtualPad *virtual_numbered(int number) {
  if (number >= 1 && number <= VIRTUAL_PADS && virtual_pads[number - 1].joystick) return &virtual_pads[number - 1];
  fprintf(stderr, "control: there's no virtual controller %d\n", number);
  return 0;
}

static void run_virtual(const char *line) {
  char word[32], name[32];
  int number;
  float x, y, z;
  VirtualPad *pad;
  if (sscanf(line, "add %31s", word) == 1) {
    virtual_add(word);
  } else if (sscanf(line, "remove %d", &number) == 1) {
    if (!(pad = virtual_numbered(number))) return;
    SDL_CloseJoystick(pad->joystick);
    SDL_DetachVirtualJoystick(pad->id);
    pad->joystick = 0;
  } else if (sscanf(line, "%31s %d %31s", word, &number, name) == 3 && (!strcmp(word, "hold") || !strcmp(word, "release"))) {
    if (!(pad = virtual_numbered(number))) return;
    size_t i = 0;
    while (i < sizeof virtual_buttons / sizeof *virtual_buttons && strcmp(name, virtual_buttons[i].name)) i++;
    if (i < sizeof virtual_buttons / sizeof *virtual_buttons) {
      SDL_SetJoystickVirtualButton(pad->joystick, virtual_buttons[i].button, !strcmp(word, "hold"));
    } else if (!strcmp(name, "zl") || !strcmp(name, "zr")) {
      SDL_SetJoystickVirtualAxis(pad->joystick, !strcmp(name, "zl") ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                                 !strcmp(word, "hold") ? SDL_JOYSTICK_AXIS_MAX : 0);
    } else {
      fprintf(stderr, "control: %s isn't a controller button\n", name);
    }
  } else if (sscanf(line, "press %d %31s", &number, name) == 2) {
    if (!(pad = virtual_numbered(number))) return;
    size_t i = 0;
    while (i < sizeof virtual_buttons / sizeof *virtual_buttons && strcmp(name, virtual_buttons[i].name)) i++;
    if (i == sizeof virtual_buttons / sizeof *virtual_buttons) {
      fprintf(stderr, "control: %s isn't a controller button pad press takes\n", name);
      return;
    }
    SDL_SetJoystickVirtualButton(pad->joystick, virtual_buttons[i].button, true);
    pad->pressed = virtual_buttons[i].button;
    pad->release_ns = SDL_GetTicksNS() + PRESS_NS;
  } else if (sscanf(line, "turn %d %f %f", &number, &x, &y) == 3) {
    // Right and up by these many degrees, as a person turning it would: SDL counts turns
    // counter-clockwise, so right is about -y and up about +x.
    if (!(pad = virtual_numbered(number))) return;
    pad->turn_left[1] -= x;
    pad->turn_left[0] += y;
  } else if (sscanf(line, "stick %d %31s %f %f", &number, name, &x, &y) == 4) {
    if (!(pad = virtual_numbered(number))) return;
    int right = !strcmp(name, "right");
    SDL_SetJoystickVirtualAxis(pad->joystick, right ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX, (Sint16)(x * 32767));
    SDL_SetJoystickVirtualAxis(pad->joystick, right ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY, (Sint16)(-y * 32767));
  } else if (sscanf(line, "accel %d %f %f %f", &number, &x, &y, &z) == 4) {
    if (!(pad = virtual_numbered(number))) return;
    pad->accel[0] = x, pad->accel[1] = y, pad->accel[2] = z;
  } else if (sscanf(line, "gyro %d %f %f %f", &number, &x, &y, &z) == 4) {
    if (!(pad = virtual_numbered(number))) return;
    pad->gyro[0] = x, pad->gyro[1] = y, pad->gyro[2] = z;
  } else {
    fprintf(stderr, "control: unknown pad command: pad %s", line);
  }
}

// Each poll, every virtual controller reports its motion, as a real one does all the
// time. A turn it's asked for goes at TURN_SPEED, in exact steps by the samples' own
// times, so the pointer lands where a script means it to.
static void virtual_sensors(void) {
  Uint64 now = SDL_GetTicksNS();
  for (int i = 0; i < VIRTUAL_PADS; i++) {
    VirtualPad *pad = &virtual_pads[i];
    if (!pad->joystick) continue;
    if (pad->release_ns && now >= pad->release_ns) {
      SDL_SetJoystickVirtualButton(pad->joystick, pad->pressed, false);
      pad->release_ns = 0;
    }
    float seconds = pad->last_ns ? (float)(now - pad->last_ns) / 1e9f : 0;
    pad->last_ns = now;
    float accel[3], gyro[3];
    for (int axis = 0; axis < 3; axis++) {
      float step = 0;
      if (seconds > 0 && pad->turn_left[axis] != 0) {
        float most = TURN_SPEED * seconds;
        step = pad->turn_left[axis] > most ? most : pad->turn_left[axis] < -most ? -most : pad->turn_left[axis];
        pad->turn_left[axis] -= step;
      }
      accel[axis] = pad->accel[axis] * SDL_STANDARD_GRAVITY;
      gyro[axis] = (pad->gyro[axis] + (seconds > 0 ? step / seconds : 0)) * (float)(M_PI / 180);
    }
    SDL_SendJoystickVirtualSensorData(pad->joystick, pad->joycons ? SDL_SENSOR_ACCEL_R : SDL_SENSOR_ACCEL, now, accel, 3);
    SDL_SendJoystickVirtualSensorData(pad->joystick, pad->joycons ? SDL_SENSOR_GYRO_R : SDL_SENSOR_GYRO, now, gyro, 3);
  }
}

static void print_stats(uint64_t now_ms) {
  float seconds = (float)(now_ms - stats_ms) / 1000;
  stats_ms = now_ms;
  for (int p = 0; p < PLAYERS; p++) {
    InputStats stats = input_stats(p);
    for (int i = 0; i < PADS; i++) {
      Pad *pad = &pads[i];
      if (!pad->gamepad || pad->player != p) continue;
      fprintf(stderr, "input: player %d (%s): %.0f gyro samples a second, %.0f remote reads a second, sample age %.1f ms (p50) %.1f ms (p99), "
                      "gyro zero %.2f %.2f %.2f\n", p + 1, pad->name, pad->samples / seconds, stats.snapshots / seconds,
              stats.age_p50_ms, stats.age_p99_ms, pad->pointer.bias[0], pad->pointer.bias[1], pad->pointer.bias[2]);
      pad->samples = 0;
    }
  }
}

void pads_poll(void) {
  if (!started) return;
  pthread_mutex_lock(&command_lock);
  static char taken[COMMANDS][256];
  int count = command_count;
  memcpy(taken, commands, sizeof commands[0] * (size_t)count);
  command_count = 0;
  pthread_mutex_unlock(&command_lock);
  for (int i = 0; i < count; i++) run_virtual(taken[i]);
  virtual_sensors();
  SDL_UpdateGamepads();
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) pad_added(event.gdevice.which);
    else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) pad_removed(event.gdevice.which);
    else if (event.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE) sensor_sample(&event.gsensor);
  }
  uint64_t now_ns = SDL_GetTicksNS(), now_ms = now_ns / 1000000;
  float seconds = last_poll_ns ? (float)(now_ns - last_poll_ns) / 1e9f : 0;
  last_poll_ns = now_ns;
  for (int i = 0; i < PADS; i++) {
    if (pads[i].gamepad && pads[i].player >= 0) pass_to_player(&pads[i], seconds, now_ms);
  }
  if (stats_on && now_ms - stats_ms >= STATS_EVERY_MS) print_stats(now_ms);
}
