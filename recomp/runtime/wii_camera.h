// The simulated Wii Remote's IR camera, which sees the sensor bar.
#pragma once
#include <stdint.h>

// Each remote's own camera, by its slot (0 to 3).
void camera_write(int device, uint32_t address, const uint8_t *data, uint32_t bytes);  // Its registers, 0xb00000 on.
void camera_read(int device, uint32_t address, uint8_t *out, uint32_t bytes);
int camera_mode(int device);  // 0 while tracking is off, else 1 basic, 3 extended or 5 full.
// The camera's dots for where the remote points, from -1 to 1 across the screen with y
// growing down, as a data report carries them. Unused bytes stay all ones.
void camera_fill(int device, float pointer_x, float pointer_y, float roll, uint8_t *out, uint32_t bytes);
void camera_reset(int device);
void camera_set_bar_above(int above);  // From the console setting BT.BAR.

// Exposed for testing: the four dots, 0x3ff for one not seen.
void camera_dots(float pointer_x, float pointer_y, float roll, int dots[4][2]);
