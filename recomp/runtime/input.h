// What the game sees of its controllers. Keys, the mouse and the control pipe all
// set this state; the game reads it through the two functions at the bottom.
#pragma once
#include <stdint.h>

enum {
  BUTTON_LEFT = 0x0001, BUTTON_RIGHT = 0x0002, BUTTON_DOWN = 0x0004, BUTTON_UP = 0x0008, BUTTON_PLUS = 0x0010,
  BUTTON_TWO = 0x0100, BUTTON_ONE = 0x0200, BUTTON_B = 0x0400, BUTTON_A = 0x0800, BUTTON_MINUS = 0x1000,
  BUTTON_HOME = 0x8000,
};

void input_hold(uint32_t buttons, int down);
void input_press(uint32_t buttons);
void input_pointer(float x, float y);
void input_stick(float x, float y);  // The Classic Controller's left stick, -1 to 1 with y growing up.
void input_pointer_now(float *x, float *y);  // Where the pointer is, -1 to 1 with y growing down.
void input_board(int standing, float kilograms, float lean_x, float lean_y);
void input_board_nudge(float lean_x, float lean_y);
void input_board_toggle(void);
// What each of the board's sensors carries now, in kilograms: top right, bottom right,
// top left, bottom left.
void input_board_now(double kilograms[4]);
void input_start_control_pipe(const char *path);
void input_sticks(float out[4]);  // Left then right, x then y, from -1 to 1.
uint32_t input_buttons(void);  // In the Wii Remote's own bit order.

// Replace KPADRead and WPADProbe in the game's code.
void input_read(void);
void input_probe(void);
void input_board_sensors(void);
void input_board_calibration(void);
void input_board_weights(void);
