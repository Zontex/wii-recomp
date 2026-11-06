// The virtual Bluetooth chip, which sits between IOS and the devices: simulated Wii
// Remotes and a Balance Board, each on its own connection.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Four remotes, then the board, in the order the console's settings keep them.
enum { BLUETOOTH_DEVICES = 5, BLUETOOTH_BOARD = 4 };
#define BLUETOOTH_FIRST_HANDLE 0x0100u  // Device n's connection is this plus n.

// bluetooth.c: USB requests and HCI.
int bluetooth_request(uint32_t command, uint32_t request);
void bluetooth_closed(void);  // The game closed its handle on the chip.
void bluetooth_update(uint64_t now);
void bluetooth_send_packet(const uint8_t *packet, uint32_t bytes);

// Adds the simulated devices to a copy of the console's settings, in memory only.
void bluetooth_register_remote(uint8_t *settings, size_t bytes);

// bluetooth_channels.c: L2CAP, for each device.
void bluetooth_open_channels(int device);
void bluetooth_receive_packet(int device, const uint8_t *packet);
void bluetooth_send_report(int device, const uint8_t *report, uint32_t bytes);
void bluetooth_forget_channels(int device);
