// The simulated Wii Remotes and Balance Board, as HID reports. A device is its slot in
// bluetooth.h: 0 to 3 for remotes, BLUETOOTH_BOARD for the board.
#pragma once
#include <stdint.h>

void wii_remote_connected(int device);
void wii_remote_output(int device, const uint8_t *report, uint32_t bytes);
void wii_remote_update(int device, uint64_t now);
