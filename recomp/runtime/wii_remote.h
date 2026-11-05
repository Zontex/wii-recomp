// The Wii Remotes and Balance Board, as HID reports. A device is its slot in bluetooth.h:
// 0 to 3 for remotes, BLUETOOTH_BOARD for the board. A slot a real device holds
// (remote_link.h) passes reports through; any other is simulated here.
#pragma once
#include <stdint.h>

void wii_remote_connected(int device);
void wii_remote_output(int device, const uint8_t *report, uint32_t bytes);
void wii_remote_update(int device, uint64_t now);

// The simulated devices alone, which the loopback link drives from its own thread.
void wii_remote_simulated_connected(int device);
void wii_remote_simulated_output(int device, const uint8_t *report, uint32_t bytes);
void wii_remote_simulated_update(int device, uint64_t now);
void wii_remote_simulated_sink(void (*to)(int device, const uint8_t *report, uint32_t bytes));

// Exposed for testing: motion in g as a data report carries it, into its three motion
// bytes and the spare bits of its two button bytes.
void wii_remote_accel_bytes(const float accel[3], uint8_t buttons[2], uint8_t out[3]);
