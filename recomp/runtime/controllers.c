// The controller settings controllers.h describes: their defaults, the file the console
// writes from them, reading it back, and setting the players up.
#include <GLFW/glfw3.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controllers.h"
#include "settings_file.h"

enum { KEYS_PER_CONTROL = 4, LIST_MOST = 8 };

static const char *control_names[KEY_CONTROLS] = {
  "up", "down", "left", "right", "a", "b", "one", "two", "plus", "minus", "home",
  "l", "r", "zl", "zr", "c", "z",
  "stick_up", "stick_down", "stick_left", "stick_right",
  "shake", "next_attachment", "motionplus", "board", "lean_left", "lean_right", "lean_forward", "lean_back",
};

static const struct { const char *name; int key; } key_names[] = {
  {"up", GLFW_KEY_UP}, {"down", GLFW_KEY_DOWN}, {"left", GLFW_KEY_LEFT}, {"right", GLFW_KEY_RIGHT},
  {"enter", GLFW_KEY_ENTER}, {"backspace", GLFW_KEY_BACKSPACE}, {"space", GLFW_KEY_SPACE}, {"tab", GLFW_KEY_TAB},
  {"escape", GLFW_KEY_ESCAPE}, {"minus", GLFW_KEY_MINUS}, {"equal", GLFW_KEY_EQUAL}, {"comma", GLFW_KEY_COMMA},
  {"period", GLFW_KEY_PERIOD}, {"slash", GLFW_KEY_SLASH}, {"semicolon", GLFW_KEY_SEMICOLON}, {"apostrophe", GLFW_KEY_APOSTROPHE},
  {"left_bracket", GLFW_KEY_LEFT_BRACKET}, {"right_bracket", GLFW_KEY_RIGHT_BRACKET}, {"backslash", GLFW_KEY_BACKSLASH},
  {"grave", GLFW_KEY_GRAVE_ACCENT}, {"left_shift", GLFW_KEY_LEFT_SHIFT}, {"right_shift", GLFW_KEY_RIGHT_SHIFT},
  {"left_control", GLFW_KEY_LEFT_CONTROL}, {"right_control", GLFW_KEY_RIGHT_CONTROL}, {"left_alt", GLFW_KEY_LEFT_ALT},
  {"right_alt", GLFW_KEY_RIGHT_ALT}, {"insert", GLFW_KEY_INSERT}, {"delete", GLFW_KEY_DELETE}, {"home_key", GLFW_KEY_HOME},
  {"end", GLFW_KEY_END}, {"page_up", GLFW_KEY_PAGE_UP}, {"page_down", GLFW_KEY_PAGE_DOWN},
};

// The controller buttons, by where they sit on a Nintendo controller, for recenter.
static const char *pad_buttons[] = {
  "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus", "home", "capture", "left_stick", "right_stick",
  "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

static const char *default_keys[KEY_CONTROLS] = {
  "up", "down", "left", "right", "enter", "backspace", "1", "2", "equal", "minus", "h",
  "x", "c", "z", "v", "c", "z",
  "up", "down", "left", "right",
  "q", "f2", "f3", "space", "a", "d", "w", "s",
};

static struct {
  int ready;  // Defaults set, at least.
  PlayerSettings players[PLAYERS];
  int board;
  char remote_mode[16];
  int keys[KEY_CONTROLS][KEYS_PER_CONTROL];
  int key_counts[KEY_CONTROLS];
} settings;

const char *controllers_control_name(KeyControl control) { return control >= 0 && control < KEY_CONTROLS ? control_names[control] : "?"; }

// A key by its name in [keys]: a letter, a digit, f1 to f12, or one of the names above.
static int key_named(const char *name) {
  size_t length = strlen(name);
  if (length == 1 && name[0] >= 'a' && name[0] <= 'z') return GLFW_KEY_A + (name[0] - 'a');
  if (length == 1 && name[0] >= '0' && name[0] <= '9') return GLFW_KEY_0 + (name[0] - '0');
  if (name[0] == 'f' && length <= 3) {
    char *end;
    long number = strtol(name + 1, &end, 10);
    if (!*end && number >= 1 && number <= 12) return GLFW_KEY_F1 + (int)number - 1;
  }
  for (size_t i = 0; i < sizeof key_names / sizeof *key_names; i++) {
    if (!strcmp(name, key_names[i].name)) return key_names[i].key;
  }
  return -1;
}

static int bind_keys(KeyControl control, const char *value, char *why, size_t why_bytes) {
  char parts[LIST_MOST][32];
  int count = settings_list(value, parts, KEYS_PER_CONTROL);
  if (count < 0) {
    snprintf(why, why_bytes, "%s takes at most %d keys", control_names[control], KEYS_PER_CONTROL);
    return 0;
  }
  int keys[KEYS_PER_CONTROL];
  for (int i = 0; i < count; i++) {
    keys[i] = strcmp(parts[i], "none") ? key_named(parts[i]) : -1;
    if (keys[i] < 0 && strcmp(parts[i], "none")) {
      snprintf(why, why_bytes, "%s isn't a key: use a letter, a digit, f1 to f12, or a name such as enter, space or left_shift", parts[i]);
      return 0;
    }
  }
  memcpy(settings.keys[control], keys, sizeof keys);
  settings.key_counts[control] = count;
  return 1;
}

static void set_defaults(void) {
  memset(&settings, 0, sizeof settings);
  for (int p = 0; p < PLAYERS; p++) {
    PlayerSettings *player = &settings.players[p];
    player->sources = p == 0 ? SOURCE_KEYBOARD | SOURCE_MOUSE | SOURCE_PAD : SOURCE_PAD;
    player->attachment = ATTACHMENT_NUNCHUK;
    player->pointer = p == 0 ? POINTER_MOUSE | POINTER_GYRO : POINTER_GYRO;
    player->gyro_speed = 1;
    strcpy(player->recenter, "right_stick");
  }
  settings.ready = 1;
  settings.board = 1;
  strcpy(settings.remote_mode, "auto");
  for (int c = 0; c < KEY_CONTROLS; c++) {
    char why[128];
    bind_keys((KeyControl)c, default_keys[c], why, sizeof why);
  }
}

static int one_of(const char *value, const char *const *names, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (!strcmp(value, names[i])) return (int)i;
  }
  return -1;
}

static int yes_or_no(const char *value, int *out, const char *key, char *why, size_t why_bytes) {
  if (!strcmp(value, "yes") || !strcmp(value, "no")) {
    *out = !strcmp(value, "yes");
    return 1;
  }
  snprintf(why, why_bytes, "%s is yes or no", key);
  return 0;
}

static int player_key(PlayerSettings *player, const char *key, const char *value, char *why, size_t why_bytes) {
  char parts[LIST_MOST][32];
  if (!strcmp(key, "source")) {
    int count = settings_list(value, parts, LIST_MOST);
    player->sources = 0;
    player->pad[0] = 0;
    for (int i = 0; i < count; i++) {
      if (!strcmp(parts[i], "keyboard")) player->sources |= SOURCE_KEYBOARD;
      else if (!strcmp(parts[i], "mouse")) player->sources |= SOURCE_MOUSE;
      else if (!strcmp(parts[i], "pad")) player->sources |= SOURCE_PAD;
      else if (!strncmp(parts[i], "pad:", 4) && parts[i][4]) {
        player->sources |= SOURCE_PAD;
        snprintf(player->pad, sizeof player->pad, "%s", parts[i] + 4);
      } else if (strcmp(parts[i], "none")) {
        snprintf(why, why_bytes, "source is keyboard, mouse, pad, pad:<serial number> or none, not %s", parts[i]);
        return 0;
      }
    }
    return 1;
  }
  if (!strcmp(key, "attachment")) {
    if (input_attachment_named(value, &player->attachment)) return 1;
    snprintf(why, why_bytes, "attachment is none, nunchuk or classic");
    return 0;
  }
  if (!strcmp(key, "sideways")) return yes_or_no(value, &player->sideways, key, why, why_bytes);
  if (!strcmp(key, "motionplus")) return yes_or_no(value, &player->motionplus, key, why, why_bytes);
  if (!strcmp(key, "pointer")) {
    int count = settings_list(value, parts, LIST_MOST);
    player->pointer = 0;
    for (int i = 0; i < count; i++) {
      if (!strcmp(parts[i], "mouse")) player->pointer |= POINTER_MOUSE;
      else if (!strcmp(parts[i], "gyro")) player->pointer |= POINTER_GYRO;
      else if (!strcmp(parts[i], "stick")) player->pointer |= POINTER_STICK;
      else if (strcmp(parts[i], "none")) {
        snprintf(why, why_bytes, "pointer is mouse, gyro, stick or none, not %s", parts[i]);
        return 0;
      }
    }
    return 1;
  }
  if (!strcmp(key, "gyro_speed")) {
    char *end;
    errno = 0;
    float speed = strtof(value, &end);
    if (*end || errno || !(speed > 0 && speed <= 20)) {
      snprintf(why, why_bytes, "gyro_speed is a number above 0, up to 20");
      return 0;
    }
    player->gyro_speed = speed;
    return 1;
  }
  if (!strcmp(key, "recenter")) {
    if (one_of(value, pad_buttons, sizeof pad_buttons / sizeof *pad_buttons) < 0 && strcmp(value, "none")) {
      snprintf(why, why_bytes, "recenter is a controller button: a, b, x, y, l, r, zl, zr, plus, minus, home, capture, left_stick, right_stick, a dpad_ direction, or none");
      return 0;
    }
    snprintf(player->recenter, sizeof player->recenter, "%s", value);
    return 1;
  }
  snprintf(why, why_bytes, "a player's settings are source, attachment, motionplus, sideways, pointer, gyro_speed and recenter");
  return 0;
}

static int on_key(void *context, const char *section, const char *key, const char *value, char *why, size_t why_bytes) {
  (void)context;
  int player;
  char end;
  if (sscanf(section, "player%d%c", &player, &end) == 1 && player >= 1 && player <= PLAYERS) {
    return player_key(&settings.players[player - 1], key, value, why, why_bytes);
  }
  if (!strcmp(section, "remotes")) {
    static const char *modes[] = {"auto", "real", "simulated", "loopback"};
    if (!strcmp(key, "mode") && one_of(value, modes, 4) >= 0) {
      snprintf(settings.remote_mode, sizeof settings.remote_mode, "%s", value);
      return 1;
    }
    snprintf(why, why_bytes, "[remotes] has mode = auto, real, simulated or loopback");
    return 0;
  }
  if (!strcmp(section, "board")) {
    if (!strcmp(key, "source") && (!strcmp(value, "keyboard") || !strcmp(value, "none"))) {
      settings.board = !strcmp(value, "keyboard");
      return 1;
    }
    snprintf(why, why_bytes, "[board] has source = keyboard or none");
    return 0;
  }
  if (!strcmp(section, "keys")) {
    int control = one_of(key, control_names, KEY_CONTROLS);
    if (control < 0) {
      snprintf(why, why_bytes, "%s isn't a control: they're listed in the file the console writes", key);
      return 0;
    }
    return bind_keys((KeyControl)control, value, why, why_bytes);
  }
  snprintf(why, why_bytes, "the sections are [remotes], [player1] to [player%d], [board] and [keys]", PLAYERS);
  return 0;
}

// Only one player can have the keyboard, or the mouse.
static int check(char *error, size_t error_bytes) {
  for (int source = SOURCE_KEYBOARD; source <= SOURCE_MOUSE; source <<= 1) {
    int holders = 0;
    for (int p = 0; p < PLAYERS; p++) holders += (settings.players[p].sources & source) != 0;
    if (holders > 1) {
      snprintf(error, error_bytes, "only one player can have the %s", source == SOURCE_KEYBOARD ? "keyboard" : "mouse");
      return 0;
    }
  }
  return 1;
}

int controllers_load_text(const char *text, char *error, size_t error_bytes) {
  set_defaults();
  int line = settings_parse(text, on_key, 0, error, error_bytes);
  if (!line && !check(error, error_bytes)) return -1;  // A mistake across lines, not in one.
  return line;
}

const char *controllers_default_text(void) {
  static char text[8192];
  size_t at = 0;
#define PUT(...) at += (size_t)snprintf(text + at, sizeof text - at, __VA_ARGS__)
  PUT("# Wii Recomp's controllers. Every setting is shown at its default, after a #. To change\n"
      "# one, take its # away and edit it. The environment variables WII_RECOMP_REMOTES and\n"
      "# WII_RECOMP_ATTACHMENT, when set, win over [remotes] mode and player 1's attachment.\n\n"
      "[remotes]\n"
      "# Real Wii Remotes and Balance Boards: auto takes real ones as they link and simulates\n"
      "# the rest; real, simulated or loopback.\n"
      "# mode = auto\n");
  for (int p = 0; p < PLAYERS; p++) {
    PUT("\n[player%d]\n", p + 1);
    if (p == 0) {
      PUT("# What drives this player: keyboard, mouse, pad (the next controller to connect),\n"
          "# pad:<serial number> (that one controller), or none. Several, with commas.\n");
    }
    PUT("# source = %s\n", p == 0 ? "keyboard, mouse, pad" : "pad");
    if (p == 0) PUT("# What's plugged into the remote: none, nunchuk or classic. F2 changes it while playing.\n");
    PUT("# attachment = nunchuk\n");
    if (p == 0) PUT("# A Wii MotionPlus between the remote and its attachment, for the games that use one: yes or no. F3 changes it.\n");
    PUT("# motionplus = no\n");
    if (p == 0) PUT("# A controller held sideways, as a Wii Remote alone is in some games: yes or no.\n");
    PUT("# sideways = no\n");
    if (p == 0) PUT("# What moves the pointer: mouse, gyro, stick (a controller's right stick) or none.\n");
    PUT("# pointer = %s\n", p == 0 ? "mouse, gyro" : "gyro");
    if (p == 0) PUT("# How fast the gyro moves the pointer, and the button that brings it back to the middle.\n");
    PUT("# gyro_speed = 1.0\n# recenter = right_stick\n");
  }
  PUT("\n[board]\n"
      "# The simulated Balance Board, in games made for one: keyboard, or none.\n"
      "# source = keyboard\n\n"
      "[keys]\n"
      "# The keyboard's controls, for the player it drives, and their keys: letters, digits,\n"
      "# f1 to f12, up, down, left, right, enter, backspace, space, tab, escape, minus, equal,\n"
      "# left_shift and so on, or none. Up to %d keys each, with commas.\n", KEYS_PER_CONTROL);
  for (int c = 0; c < KEY_CONTROLS; c++) PUT("# %s = %s\n", control_names[c], default_keys[c]);
#undef PUT
  return text;
}

// Before the file is read, as in tests, everything is at its default.
static void ready(void) {
  if (!settings.ready) set_defaults();
}

const PlayerSettings *controllers_player(int player) {
  ready();
  return &settings.players[player >= 0 && player < PLAYERS ? player : 0];
}

static int player_with(int source) {
  for (int p = 0; p < PLAYERS; p++) {
    if (controllers_player(p)->sources & source) return p;
  }
  return -1;
}

int controllers_keyboard_player(void) { return player_with(SOURCE_KEYBOARD); }
int controllers_mouse_player(void) { return player_with(SOURCE_MOUSE); }
int controllers_board(void) {
  ready();
  return settings.board;
}


int controllers_controls_for(int key, KeyControl controls[], int most) {
  ready();
  int count = 0;
  for (int c = 0; c < KEY_CONTROLS && count < most; c++) {
    for (int i = 0; i < settings.key_counts[c]; i++) {
      if (settings.keys[c][i] == key) {
        controls[count++] = (KeyControl)c;
        break;
      }
    }
  }
  return count;
}

static void stop(const char *path, int line, const char *why) {
  if (line > 0) fprintf(stderr, "stop: %s line %d: %s\n", path, line, why);
  else fprintf(stderr, "stop: %s: %s\n", path, why);
  exit(1);
}

void controllers_start(void) {
  const char *path = getenv("WII_RECOMP_CONTROLLERS");
  if (!path) path = "controllers.ini";
  FILE *file = fopen(path, "r");
  if (!file) {
    file = fopen(path, "w");
    if (!file) stop(path, 0, strerror(errno));
    fputs(controllers_default_text(), file);
    fclose(file);
    fprintf(stderr, "controllers: wrote %s with every setting at its default\n", path);
    file = fopen(path, "r");
    if (!file) stop(path, 0, strerror(errno));
  }
  static char text[65536];
  size_t bytes = fread(text, 1, sizeof text - 1, file);
  int too_long = !feof(file);
  fclose(file);
  if (too_long) stop(path, 0, "it's longer than a settings file can be");
  text[bytes] = 0;
  char error[256];
  int line = controllers_load_text(text, error, sizeof error);
  if (line) stop(path, line, error);
  setenv("WII_RECOMP_REMOTES", settings.remote_mode, 0);  // For remote_link.c, unless it's set already.
  input_board_enable(settings.board);
  // Players with a keyboard or mouse are there from the start; the others come with a controller.
  const char *chosen = getenv("WII_RECOMP_ATTACHMENT");
  for (int p = 0; p < PLAYERS; p++) {
    const PlayerSettings *player = &settings.players[p];
    input_connect(p, (player->sources & (SOURCE_KEYBOARD | SOURCE_MOUSE)) != 0);
    Attachment attachment = player->attachment;
    if (p == 0 && chosen && !input_attachment_named(chosen, &attachment)) {
      fprintf(stderr, "stop: WII_RECOMP_ATTACHMENT is none, nunchuk or classic, not %s\n", chosen);
      exit(1);
    }
    input_attach(p, attachment);
    input_attach_motionplus(p, player->motionplus);
  }
}
