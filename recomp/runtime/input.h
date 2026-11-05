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
  BUTTON_NUNCHUK_C = 0x100000, BUTTON_NUNCHUK_Z = 0x200000,
};

enum { PLAYERS = 4 };

// What's plugged into a player's remote.
typedef enum { ATTACHMENT_NONE, ATTACHMENT_NUNCHUK, ATTACHMENT_CLASSIC, ATTACHMENT_KINDS } Attachment;

// One player's controls at a moment. Motion is in g, along the remote's own axes as its
// accelerometer reads them: x across its face towards its right, y along its length
// towards the screen, z out of its face. Held still and flat, face up, it reads (0, 0, 1);
// turned clockwise by an angle, (-sin, 0, cos). Games use that with the camera's picture
// to keep the pointer level, so a source that turns the remote moves both together.
typedef struct {
  uint32_t buttons;            // Held or briefly pressed, in the Wii Remote's own bit order.
  float pointer_x, pointer_y;  // -1 to 1 across the screen, y growing down.
  float roll;                  // How far the remote is turned about its length, in radians, clockwise as its user sees it.
  float sticks[4];             // The Classic Controller's left then right stick, x then y, -1 to 1 with y growing up.
  float accel[3];              // The remote's motion.
  float nunchuk_stick[2];      // -1 to 1, y growing up.
  float nunchuk_accel[3];      // The Nunchuk's motion, along the same axes when held beside the remote.
  Attachment attachment;
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
void input_motion(int player, const float accel[3], float roll);  // Both at once, from a controller's sensors.
void input_accel(int player, const float accel[3]);
void input_roll(int player, float roll);
void input_nunchuk(int player, float stick_x, float stick_y, const float accel[3]);
void input_nunchuk_stick(int player, float x, float y);
void input_nunchuk_accel(int player, const float accel[3]);
void input_shake(int player);  // Shakes the remote for a moment, as a swing or a waggle does.
// Plugs an attachment in, or none, as a player does by hand. The remote tells the game.
void input_attach(int player, Attachment attachment);
Attachment input_attachment(int player);
const char *input_attachment_name(Attachment attachment);
int input_attachment_named(const char *name, Attachment *out);  // 0 if the name isn't one.

// What the game does to a player's remote: rumble on or off, which of the four lights
// are lit (bit 0 the first), and the battery level the remote reports (0 to 1).
typedef struct {
  int rumble;
  uint8_t lights;
} PlayerOutputs;
void input_set_outputs(int player, int rumble, uint8_t lights);
PlayerOutputs input_outputs(int player);
void input_battery(int player, float level);
float input_battery_level(int player);

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
