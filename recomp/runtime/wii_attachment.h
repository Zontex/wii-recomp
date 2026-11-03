// The attachment on the simulated Wii Remote.
#pragma once
#include <stdint.h>

void attachment_read(uint32_t address, uint8_t *out, uint32_t bytes);
void attachment_write(uint32_t address, const uint8_t *data, uint32_t bytes);
void attachment_fill(uint8_t *out, uint32_t bytes);  // Its current state, as a data report carries it.

// Exposed for testing.
void attachment_encrypt(uint8_t *bytes, uint32_t count, uint32_t address, const uint8_t add[8], const uint8_t exclusive[8]);
void attachment_classic_state(uint8_t *to, uint32_t buttons, const float sticks[4]);
