// The simulated Balance Board's registers and readings, where a remote's attachment is.
#pragma once
#include <stdint.h>

void board_read(uint32_t address, uint8_t *out, uint32_t bytes);
void board_write(uint32_t address, const uint8_t *data, uint32_t bytes);
void board_fill(uint8_t *out, uint32_t bytes);  // Its current reading, as a data report carries it.

// Exposed for testing: four sensors in kilograms, as the eleven bytes the board sends.
void board_reading(const double kilograms[4], uint8_t out[11]);
