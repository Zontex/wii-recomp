// A Wii MotionPlus on each simulated remote, between the remote and its attachment. It
// answers on the attachment wire at its own address (0xa6) until the game activates it;
// then it takes the attachment's address (0xa4), sends its gyro's readings in the
// attachment's place, and can pass a Nunchuk's or Classic Controller's state through,
// every other report. Following Dolphin's MotionPlus.cpp.
#pragma once
#include <stdint.h>
#include "input.h"

typedef enum { MOTIONPLUS_NOT_HERE, MOTIONPLUS_ANSWERED, MOTIONPLUS_PASSED_ON } MotionPlusAnswer;

void motionplus_plug(int device, int present);  // Fresh, and not yet active.
int motionplus_present(int device);
// Moves its activation and its check's answers along, and notices the attachment behind
// it coming and going. `attached`: whether an attachment is plugged in behind it.
void motionplus_update(int device, int attached);
// What the remote's attachment pin reads: the attachment behind it while inactive, then
// nothing for a moment while it changes over, then itself while active.
int motionplus_detect(int device, int attached);
int motionplus_active(int device);
// A read or write at the attachment's address (0xa4) or the MotionPlus's own (0xa6).
// PASSED_ON means the attachment behind it takes it.
MotionPlusAnswer motionplus_read(int device, int own_address, uint32_t offset, uint8_t *out, uint32_t bytes);
MotionPlusAnswer motionplus_write(int device, int own_address, uint32_t offset, const uint8_t *data, uint32_t bytes);
// While active, what a data report carries in the attachment's place.
void motionplus_fill(int device, const PlayerState *player, int attached, uint8_t *out, uint32_t bytes);

// Exposed for testing: the six bytes of a gyro reading, in degrees a second about the
// remote's axes, and a passing-through attachment's six bytes as the MotionPlus changes them.
void motionplus_gyro_bytes(const float gyro[3], int attached, uint8_t out[6]);
void motionplus_pass_through(Attachment kind, uint8_t data[6]);
