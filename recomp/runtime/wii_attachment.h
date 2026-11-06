// The attachment on each simulated Wii Remote, by its slot (0 to 3).
#pragma once
#include <stdint.h>
#include "input.h"

void attachment_plug(int device, Attachment kind);  // What's in the port now, fresh.
void attachment_read(int device, uint32_t address, uint8_t *out, uint32_t bytes);
void attachment_write(int device, uint32_t address, const uint8_t *data, uint32_t bytes);
// Its current state from the player's controls, as a data report carries it.
void attachment_fill(int device, const PlayerState *player, uint8_t *out, uint32_t bytes);
// Its six bytes of state, never encrypted, as a MotionPlus reads them to pass through.
void attachment_plain_state(int device, const PlayerState *player, uint8_t out[6]);

// Exposed for testing.
void attachment_encrypt(uint8_t *bytes, uint32_t count, uint32_t address, const uint8_t add[8], const uint8_t exclusive[8]);
void attachment_classic_state(uint8_t *to, uint32_t buttons, const float sticks[4]);
void attachment_nunchuk_state(uint8_t *to, uint32_t buttons, const float stick[2], const float accel[3]);
