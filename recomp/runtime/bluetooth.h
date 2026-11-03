// The virtual Bluetooth chip, which sits between IOS and the simulated Wii Remote.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define BLUETOOTH_HANDLE 0x0100u

// bluetooth.c: USB requests and HCI.
int bluetooth_request(uint32_t command, uint32_t request);
void bluetooth_update(uint64_t now);
void bluetooth_send_packet(const uint8_t *packet, uint32_t bytes);

// Adds the simulated remote to a copy of the console's settings, in memory only.
void bluetooth_register_remote(uint8_t *settings, size_t bytes);

// bluetooth_channels.c: L2CAP.
void bluetooth_open_channels(void);
void bluetooth_receive_packet(const uint8_t *packet);
void bluetooth_send_report(const uint8_t *report, uint32_t bytes);
