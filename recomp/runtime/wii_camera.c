// The simulated Wii Remote's IR camera. It sees the sensor bar's two lights, and the
// game works out where the remote points from where they appear. The game sets it up
// by writing its registers, then reads the dots in each data report. Formats follow
// WiiBrew and Dolphin's Camera.cpp.
#include <string.h>

#include "wii_camera.h"

enum {
  REGISTERS = 256, TRACKING = 0x30, TRACKING_ON = 0x08, MODE = 0x33, MODE_BASIC = 1, MODE_EXTENDED = 3, MODE_FULL = 5,
  WIDTH = 1024, HEIGHT = 768, NO_DOT = 0x3ff, DOT_SIZE = 3,
};

// How far the dots move across the camera as the pointer goes from the middle to an
// edge of the screen, and how far apart the two lights look. Games turn the dots into a
// cursor at their own scale: the Wii Menu's cursor reaches the side edges at 250 and the
// top and bottom at 190, Wii Fit Plus's at 225 and 226. Each axis takes the larger, so
// every edge is in reach in both, and at worst the cursor gets there a little early.
// (Dolphin's defaults, chosen to reach the edges in most games, come to about 300 and 240.)
static const float REACH_X = 250, REACH_Y = 226, LIGHTS_APART = 130;
// The sensor bar sits beyond the screen's top or bottom edge, so pointing at the middle
// sees it off centre, by this many of the camera's rows. Measured on the Wii Menu with
// the bar below; Wii Fit Plus puts its cursor at the middle of the screen with it too.
static const float BAR_OFFSET = 132;

static uint8_t registers[REGISTERS];
static int bar_above;  // The console setting BT.BAR: 1 above the TV, 0 below.

void camera_set_bar_above(int above) { bar_above = above; }

void camera_write(uint32_t address, const uint8_t *data, uint32_t bytes) {
  for (uint32_t i = 0; i < bytes && address + i < REGISTERS; i++) registers[address + i] = data[i];
}

void camera_read(uint32_t address, uint8_t *out, uint32_t bytes) {
  for (uint32_t i = 0; i < bytes; i++) out[i] = address + i < REGISTERS ? registers[address + i] : 0;
}

int camera_mode(void) { return registers[TRACKING] == TRACKING_ON ? registers[MODE] : 0; }

// Pointing right moves the lights left in the camera's picture, and pointing down moves
// them down, as the Wii Menu reads them.
void camera_dots(float pointer_x, float pointer_y, int dots[4][2]) {
  float centre_x = WIDTH / 2.0f - pointer_x * REACH_X;
  float centre_y = HEIGHT / 2.0f + pointer_y * REACH_Y + (bar_above ? BAR_OFFSET : -BAR_OFFSET);
  for (int i = 0; i < 4; i++) dots[i][0] = dots[i][1] = NO_DOT;
  for (int i = 0; i < 2; i++) {
    int x = (int)(centre_x + (i ? LIGHTS_APART : -LIGHTS_APART) / 2), y = (int)centre_y;
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) continue;  // Out of view, as when pointing off the screen.
    dots[i][0] = x;
    dots[i][1] = y;
  }
}

// Basic: two dots in five bytes, twice. Extended: three bytes a dot.
void camera_fill(float pointer_x, float pointer_y, uint8_t *out, uint32_t bytes) {
  int dots[4][2], mode = camera_mode();
  memset(out, 0xff, bytes);
  if (mode != MODE_BASIC && mode != MODE_EXTENDED) return;  // Off, or full mode, which no data report carries.
  camera_dots(pointer_x, pointer_y, dots);
  if (mode == MODE_BASIC) {
    for (int pair = 0; pair < 2 && pair * 5 + 5 <= (int)bytes; pair++) {
      const int *a = dots[pair * 2], *b = dots[pair * 2 + 1];
      uint8_t *to = out + pair * 5;
      to[0] = (uint8_t)a[0];
      to[1] = (uint8_t)a[1];
      to[2] = (uint8_t)((b[0] >> 8) | (b[1] >> 8) << 2 | (a[0] >> 8) << 4 | (a[1] >> 8) << 6);
      to[3] = (uint8_t)b[0];
      to[4] = (uint8_t)b[1];
    }
  } else {
    for (int i = 0; i < 4 && i * 3 + 3 <= (int)bytes; i++) {
      if (dots[i][0] == NO_DOT) continue;  // Stays all ones.
      uint8_t *to = out + i * 3;
      to[0] = (uint8_t)dots[i][0];
      to[1] = (uint8_t)dots[i][1];
      to[2] = (uint8_t)(DOT_SIZE | (dots[i][0] >> 8) << 4 | (dots[i][1] >> 8) << 6);
    }
  }
}

void camera_reset(void) { memset(registers, 0, sizeof registers); }
