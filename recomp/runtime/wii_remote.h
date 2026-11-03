// The simulated Wii Remote, as HID reports.
#pragma once
#include <stdint.h>

void wii_remote_connected(void);
void wii_remote_output(const uint8_t *report, uint32_t bytes);
void wii_remote_update(uint64_t now);
