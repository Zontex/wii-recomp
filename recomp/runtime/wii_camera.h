// The simulated Wii Remote's IR camera, which sees the sensor bar.
#pragma once
#include <stdint.h>

void camera_write(uint32_t address, const uint8_t *data, uint32_t bytes);  // Its registers, 0xb00000 on.
void camera_read(uint32_t address, uint8_t *out, uint32_t bytes);
int camera_mode(void);  // 0 while tracking is off, else 1 basic, 3 extended or 5 full.
// The camera's dots for where the remote points, from -1 to 1 across the screen with y
// growing down, as a data report carries them. Unused bytes stay all ones.
void camera_fill(float pointer_x, float pointer_y, uint8_t *out, uint32_t bytes);
void camera_reset(void);
void camera_set_bar_above(int above);  // From the console setting BT.BAR.

// Exposed for testing: the four dots, 0x3ff for one not seen.
void camera_dots(float pointer_x, float pointer_y, int dots[4][2]);
