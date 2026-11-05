// The console's controller settings: controllers.ini in the console folder (beside the
// program without one), or the file WII_RECOMP_CONTROLLERS names. The console writes it
// with every setting at its default, commented out, the first time it runs, so there's
// nothing to set up; a line with a mistake stops the console and says which line.
#pragma once
#include <stddef.h>
#include "input.h"

enum { SOURCE_KEYBOARD = 1, SOURCE_MOUSE = 2, SOURCE_PAD = 4 };
enum { POINTER_MOUSE = 1, POINTER_GYRO = 2, POINTER_STICK = 4 };

typedef struct {
  int sources;          // SOURCE_*: what drives this player.
  char pad[64];         // A controller kept for this player, by serial number, or "" for the next one.
  Attachment attachment;
  int sideways;         // A controller held sideways, as a Wii Remote alone is in some games.
  int pointer;          // POINTER_*: what moves the pointer.
  float gyro_speed;
  char recenter[16];    // The controller button that brings the pointer back to the middle.
} PlayerSettings;

// The keyboard's controls, each bound to keys in [keys].
typedef enum {
  KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_A, KEY_B, KEY_ONE, KEY_TWO, KEY_PLUS, KEY_MINUS, KEY_HOME,
  KEY_L, KEY_R, KEY_ZL, KEY_ZR, KEY_C, KEY_Z,
  KEY_STICK_UP, KEY_STICK_DOWN, KEY_STICK_LEFT, KEY_STICK_RIGHT,
  KEY_SHAKE, KEY_NEXT_ATTACHMENT, KEY_BOARD, KEY_LEAN_LEFT, KEY_LEAN_RIGHT, KEY_LEAN_FORWARD, KEY_LEAN_BACK,
  KEY_CONTROLS,
} KeyControl;

// Reads the file, writing it first if it isn't there, and sets the players up. Stops the
// console on a mistake.
void controllers_start(void);
// Back to every default, then the settings in `text`. Returns 0, or the line that's wrong
// with why in `error`. For tests, and used by controllers_start.
int controllers_load_text(const char *text, char *error, size_t error_bytes);
const char *controllers_default_text(void);

const PlayerSettings *controllers_player(int player);
int controllers_keyboard_player(void);  // Which player the keyboard drives, or -1.
int controllers_mouse_player(void);
int controllers_board(void);  // Whether the simulated board is there, in games made for one.
// The controls a key works (GLFW key codes), into `controls`; returns how many.
int controllers_controls_for(int key, KeyControl controls[], int most);
const char *controllers_control_name(KeyControl control);
