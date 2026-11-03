// The two L2CAP channels a Wii Remote opens to the console, carried in ACL packets. Control
// comes up first, then interrupt, which carries the HID reports.
#include <stdio.h>
#include <string.h>

#include "bluetooth.h"
#include "wii_remote.h"

enum {
  ACL_HEADER = 4, L2CAP_HEADER = 4, SIGNAL_HEADER = 4, ACL_FIRST_FRAGMENT = 0x2000, FRAME_BYTES = 64,
  CID_SIGNAL = 1, CID_CONTROL = 0x40, CID_INTERRUPT = 0x41, PSM_CONTROL = 0x11, PSM_INTERRUPT = 0x13,
  SIGNAL_CONNECT = 2, SIGNAL_CONNECT_RESPONSE = 3, SIGNAL_CONFIGURE = 4, SIGNAL_CONFIGURE_RESPONSE = 5,
  HID_INPUT = 0xa1, HID_OUTPUT = 0xa2,
};

static uint32_t host_control, host_interrupt, signal_id;

static void put16(uint8_t *to, uint32_t value) {
  to[0] = (uint8_t)value;
  to[1] = (uint8_t)(value >> 8);
}

static uint32_t get16(const uint8_t *from) { return (uint32_t)from[0] | (uint32_t)from[1] << 8; }

static void send_frame(uint32_t channel, const uint8_t *payload, uint32_t bytes) {
  uint8_t packet[ACL_HEADER + L2CAP_HEADER + FRAME_BYTES];
  put16(packet, BLUETOOTH_HANDLE | ACL_FIRST_FRAGMENT);
  put16(packet + 2, bytes + L2CAP_HEADER);
  put16(packet + 4, bytes);
  put16(packet + 6, channel);
  memcpy(packet + ACL_HEADER + L2CAP_HEADER, payload, bytes);
  bluetooth_send_packet(packet, ACL_HEADER + L2CAP_HEADER + bytes);
}

static void send_signal(int code, int id, const uint8_t *body, uint32_t bytes) {
  uint8_t frame[SIGNAL_HEADER + 16] = {(uint8_t)code, (uint8_t)id};
  put16(frame + 2, bytes);
  memcpy(frame + SIGNAL_HEADER, body, bytes);
  send_frame(CID_SIGNAL, frame, SIGNAL_HEADER + bytes);
}

static void open_channel(int psm, uint32_t local) {
  uint8_t body[4];
  put16(body, (uint32_t)psm);
  put16(body + 2, local);
  send_signal(SIGNAL_CONNECT, (int)++signal_id, body, sizeof body);
}

static void configure_channel(uint32_t host_channel) {
  uint8_t body[4];
  put16(body, host_channel);
  put16(body + 2, 0);
  send_signal(SIGNAL_CONFIGURE, (int)++signal_id, body, sizeof body);
}

void bluetooth_open_channels(void) {
  if (!host_control) open_channel(PSM_CONTROL, CID_CONTROL);
}

void bluetooth_send_report(const uint8_t *report, uint32_t bytes) {
  uint8_t frame[FRAME_BYTES] = {HID_INPUT};
  memcpy(frame + 1, report, bytes);
  send_frame(host_interrupt, frame, bytes + 1);
}

static void handle_signal(const uint8_t *frame) {
  int code = frame[0], id = frame[1];
  const uint8_t *body = frame + SIGNAL_HEADER;
  if (code == SIGNAL_CONNECT_RESPONSE) {
    uint32_t host = get16(body), mine = get16(body + 2);
    if (body[4] || body[5]) return;
    if (mine == CID_CONTROL) host_control = host; else host_interrupt = host;
    configure_channel(host);
  } else if (code == SIGNAL_CONFIGURE) {
    uint8_t reply[6] = {0};
    put16(reply, get16(body) == CID_CONTROL ? host_control : host_interrupt);
    send_signal(SIGNAL_CONFIGURE_RESPONSE, id, reply, sizeof reply);
  } else if (code == SIGNAL_CONFIGURE_RESPONSE) {
    if (get16(body) == CID_CONTROL) open_channel(PSM_INTERRUPT, CID_INTERRUPT);
    else wii_remote_connected();
  } else {
    fprintf(stderr, "bluetooth: L2CAP signal %02x\n", code);
  }
}

void bluetooth_receive_packet(const uint8_t *packet) {
  uint32_t length = get16(packet + 4), channel = get16(packet + 6);
  const uint8_t *frame = packet + ACL_HEADER + L2CAP_HEADER;
  if (channel == CID_SIGNAL) handle_signal(frame);
  else if (length && frame[0] == HID_OUTPUT) wii_remote_output(frame + 1, length - 1);
}
