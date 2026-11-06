// The game's side of the virtual Bluetooth chip, for tests: HCI commands in, events and
// ACL data out through the USB requests IOS takes, and the steps a game takes to answer
// a device's call. Include after harness.h, in a test built with bluetooth.c.
#pragma once
#include <string.h>
#include "bluetooth.h"
#include "ios.h"

enum { CHIP_COMMAND_REQUEST = 0x80001000u, CHIP_EVENTS_REQUEST = 0x80001200u, CHIP_DATA_REQUEST = 0x80001400u,
       CHIP_COMMAND = 0x80002000u, CHIP_ENDPOINT = 0x80002100u, CHIP_EVENT = 0x80003000u, CHIP_BUFFER = 0x80003400u };
static int chip_pending[2], chip_ready[2];

void ios_complete(uint32_t request, int result) {
  (void)result;
  if (request == CHIP_EVENTS_REQUEST) chip_ready[0] = 1;
  if (request == CHIP_DATA_REQUEST) chip_ready[1] = 1;
}

static void chip_set_up(uint32_t request, uint32_t code, uint32_t vector, uint32_t address, uint32_t bytes) {
  memset(host(request), 0, 0x200);
  wr32(request + 12, code);
  wr32(request + 24, (request + 0x100) & 0x7fffffff);
  wr32(request + 0x100 + vector * 8, address & 0x7fffffff);
  wr32(request + 0x100 + vector * 8 + 4, bytes);
}

static void chip_command(int opcode, const uint8_t *parameters, uint32_t bytes) {
  wr8(CHIP_COMMAND, opcode & 0xff);
  wr8(CHIP_COMMAND + 1, opcode >> 8);
  wr8(CHIP_COMMAND + 2, bytes);
  if (bytes) memcpy(host(CHIP_COMMAND + 3), parameters, bytes);
  chip_set_up(CHIP_COMMAND_REQUEST, 0, 6, CHIP_COMMAND, bytes + 3);
  bluetooth_request(IOS_IOCTLV, CHIP_COMMAND_REQUEST);
}

// The next event (endpoint 0x81) or ACL packet (0x82), copied to CHIP_EVENT; 0 if none yet.
static int chip_collect(int endpoint) {
  int kind = endpoint == 0x82;
  uint32_t request = kind ? CHIP_DATA_REQUEST : CHIP_EVENTS_REQUEST, buffer = CHIP_BUFFER + (uint32_t)kind * 0x200;
  if (!chip_pending[kind]) {
    chip_set_up(request, kind ? 1 : 2, 2, buffer, 0x100);
    wr32(request + 0x100, (CHIP_ENDPOINT + (uint32_t)kind) & 0x7fffffff);
    wr8(CHIP_ENDPOINT + (uint32_t)kind, endpoint);
    chip_pending[kind] = 1;
    bluetooth_request(IOS_IOCTLV, request);
  }
  memset(host(CHIP_EVENT), 0, 0x100);
  if (!chip_ready[kind]) return 0;
  memcpy(host(CHIP_EVENT), host(buffer), 0x100);
  chip_pending[kind] = chip_ready[kind] = 0;
  return 1;
}

static int chip_event(void) { return chip_collect(0x81) ? rd8(CHIP_EVENT) : 0; }
static int chip_signal_back(void) { return chip_collect(0x82) && rd8(CHIP_EVENT + 6) == 1 ? rd8(CHIP_EVENT + 8) : 0; }
// The device a collected ACL packet came from, by its connection handle.
static int chip_packet_device(void) { return (int)((rd8(CHIP_EVENT) | (rd8(CHIP_EVENT + 1) & 0x0f) << 8) - BLUETOOTH_FIRST_HANDLE); }

static void chip_address(int device, uint8_t address[6]) {
  static const uint8_t base[6] = {0x01, 0x00, 0x1f, 0x19, 0x00, 0x00};
  memcpy(address, base, 6);
  address[5] = (uint8_t)device;
}

// The console's L2CAP signal, or HID data, to a device.
static void chip_to_device(int device, int channel, const uint8_t *frame, uint32_t bytes) {
  uint32_t handle = BLUETOOTH_FIRST_HANDLE + (uint32_t)device;
  uint8_t packet[64] = {(uint8_t)handle, (uint8_t)(handle >> 8 | 0x20), (uint8_t)(bytes + 4), 0, (uint8_t)bytes, 0, (uint8_t)channel, 0};
  memcpy(packet + 8, frame, bytes);
  bluetooth_receive_packet(device, packet);
}

static void chip_signal_device(int device, int code, int id, const uint8_t *body, uint32_t bytes) {
  uint8_t frame[32] = {(uint8_t)code, (uint8_t)id, (uint8_t)bytes, 0};
  memcpy(frame + 4, body, bytes);
  chip_to_device(device, 1, frame, bytes + 4);
}

// A HID output report from the game to a device, on its interrupt channel.
static void chip_output(int device, const uint8_t *report, uint32_t bytes) {
  uint8_t frame[32] = {0xa2};
  memcpy(frame + 1, report, bytes);
  chip_to_device(device, 0x41, frame, bytes + 1);
}

// The game answers a device's call, through to both channels open. Each check is
// counted against the caller's line through CHECK, so a failure names the step.
static void chip_answer_call(int device) {
  uint8_t address[6];
  chip_address(device, address);
  chip_command(0x0409, address, 6);
  CHECK_EQ(chip_event(), 0x0f);
  CHECK_EQ(chip_event(), 0x03);
  uint32_t handle = BLUETOOTH_FIRST_HANDLE + (uint32_t)device;
  const uint8_t policy[] = {(uint8_t)handle, (uint8_t)(handle >> 8), 0x0f, 0x00};
  chip_command(0x080d, policy, sizeof policy);
  CHECK_EQ(chip_event(), 0x0e);
  CHECK_EQ(chip_signal_back(), 2);  // The device opens control.
  const uint8_t control_open[] = {0x50, 0x00, 0x40, 0x00, 0, 0, 0, 0}, control_set[] = {0x40, 0x00, 0, 0, 0, 0};
  chip_signal_device(device, 3, 1, control_open, sizeof control_open);
  CHECK_EQ(chip_signal_back(), 4);
  chip_signal_device(device, 5, 2, control_set, sizeof control_set);
  CHECK_EQ(chip_signal_back(), 2);  // Then interrupt.
  const uint8_t interrupt_open[] = {0x51, 0x00, 0x41, 0x00, 0, 0, 0, 0}, interrupt_set[] = {0x41, 0x00, 0, 0, 0, 0};
  chip_signal_device(device, 3, 3, interrupt_open, sizeof interrupt_open);
  CHECK_EQ(chip_signal_back(), 4);
  chip_signal_device(device, 5, 4, interrupt_set, sizeof interrupt_set);
}
