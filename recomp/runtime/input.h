// What the game sees of its controllers: four players and a Balance Board. Keys, the
// mouse, the control pipe and host controllers set this state from their own threads;
// the simulated devices read it, one consistent snapshot per report.
#pragma once
#include <stdint.h>

enum {
  BUTTON_LEFT = 0x0001, BUTTON_RIGHT = 0x0002, BUTTON_DOWN = 0x0004, BUTTON_UP = 0x0008, BUTTON_PLUS = 0x0010,
  BUTTON_TWO = 0x0100, BUTTON_ONE = 0x0200, BUTTON_B = 0x0400, BUTTON_A = 0x0800, BUTTON_MINUS = 0x1000,
  BUTTON_HOME = 0x8000,
  // The Classic Controller's shoulder buttons, which the Wii Remote doesn't have.
  BUTTON_CLASSIC_L = 0x10000, BUTTON_CLASSIC_R = 0x20000, BUTTON_CLASSIC_ZL = 0x40000, BUTTON_CLASSIC_ZR = 0x80000,
};

enum { PLAYERS = 4 };

// One player's controls at a moment.
typedef struct {
  uint32_t buttons;            // Held or briefly pressed, in the Wii Remote's own bit order.
  float pointer_x, pointer_y;  // -1 to 1 across the screen, y growing down.
  float sticks[4];             // Left then right, x then y, -1 to 1 with y growing up.
} PlayerState;

// Player 1 is there from the start, for the keyboard and mouse; the others join when a
// controller or the pipe connects them. A player who joins makes their remote call the console.
void input_connect(int player, int connected);
int input_connected(int player);

void input_hold(int player, uint32_t buttons, int down);
void input_press(int player, uint32_t buttons);  // Held for a tenth of a second.
void input_pointer(int player, float x, float y);
void input_stick(int player, float x, float y);  // The left stick, -1 to 1 with y growing up.
void input_snapshot(int player, PlayerState *out);
uint32_t input_buttons(int player);

void input_board(int standing, float kilograms, float lean_x, float lean_y);
void input_board_nudge(float lean_x, float lean_y);
void input_board_toggle(void);
float input_board_kilograms(void);
double input_board_total(void);  // All of the weight on the board now, in kilograms.
// What each of the board's sensors carries now, in kilograms: top right, bottom right,
// top left, bottom left.
void input_board_now(double kilograms[4]);

// The control pipe (control_pipe.c): one command a line, from a script or another program.
void input_start_control_pipe(const char *path);
int input_run_command(const char *line);  // 0 for a line it doesn't know. Exposed for tests.

// Replace KPADRead and WPADProbe in the game's code (input_direct.c).
void input_read(void);
void input_probe(void);
void input_board_sensors(void);
void input_board_calibration(void);
void input_board_weights(void);
