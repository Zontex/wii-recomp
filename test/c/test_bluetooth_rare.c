// The virtual Bluetooth chip's rarer commands: inquiry, the console calling a remote,
// pairing with no stored key, sniff mode, channels the console opens and closes, and the
// attachment-only data mode. Compiled with bluetooth.c, bluetooth_channels.c, wii_remote.c
// and gaps.c.
#include <string.h>
#include "harness.h"
#include "bluetooth.h"
#include "ios.h"
#include "input.h"
#include "title.h"
#include "wii_remote.h"

const Title *title = &(Title){.virtual_bluetooth = 1};
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
uint32_t input_buttons(int player) { return 0; }
int input_connected(int player) { return player == 0; }
void input_snapshot(int player, PlayerState *out) { memset(out, 0, sizeof *out); }
Attachment input_attachment(int player) { return ATTACHMENT_CLASSIC; }
const char *input_attachment_name(Attachment attachment) { return "classic"; }
float input_battery_level(int player) { return 1; }
int input_board_enabled(void) { return 1; }
int input_motionplus(int player) { return 0; }
void attachment_plain_state(int device, const PlayerState *player, uint8_t out[6]) { memset(out, 0, 6); }
void input_set_outputs(int player, int rumble, uint8_t lights) {}
void attachment_plug(int device, Attachment kind) {}
void attachment_read(int device, uint32_t address, uint8_t *out, uint32_t bytes) { memset(out, 0, bytes); }
void attachment_write(int device, uint32_t address, const uint8_t *data, uint32_t bytes) {}
void attachment_fill(int device, const PlayerState *player, uint8_t *out, uint32_t bytes) { for (uint32_t i = 0; i < bytes; i++) out[i] = (uint8_t)(0x40 + i); }
void board_read(uint32_t address, uint8_t *out, uint32_t bytes) {}
void board_write(uint32_t address, const uint8_t *data, uint32_t bytes) {}
void board_fill(uint8_t *out, uint32_t bytes) {}
void camera_read(int device, uint32_t address, uint8_t *out, uint32_t bytes) {}
void camera_write(int device, uint32_t address, const uint8_t *data, uint32_t bytes) {}
void camera_fill(int device, float x, float y, uint8_t *out, uint32_t bytes) {}
uint64_t time_base(void) { return 0; }
void link_backend_start(int mode) {}
void link_backend_search(void) {}
void link_backend_handover(void) {}
void link_loopback_start(void) {}

// Each kind of request has its own place, with its vectors after it, as a held one is
// read again when the chip has something for it.
enum { COMMAND_REQUEST = 0x80001000u, EVENTS_REQUEST = 0x80001200u, DATA_REQUEST = 0x80001400u, COMMAND = 0x80002000u,
       ENDPOINT = 0x80002100u, EVENT = 0x80003000u, BUFFER = 0x80003400u };
static const uint8_t remote[6] = {0x01, 0x00, 0x1f, 0x19, 0x00, 0x00};
static int pending[2], ready[2];

void ios_complete(uint32_t request, int result) {
  if (request == EVENTS_REQUEST) ready[0] = 1;
  if (request == DATA_REQUEST) ready[1] = 1;
}

static void set_up(uint32_t request, uint32_t code, uint32_t vector, uint32_t address, uint32_t bytes) {
  memset(host(request), 0, 0x200);
  wr32(request + 12, code);
  wr32(request + 24, (request + 0x100) & 0x7fffffff);
  wr32(request + 0x100 + vector * 8, address & 0x7fffffff);
  wr32(request + 0x100 + vector * 8 + 4, bytes);
}

static void command(int opcode, const uint8_t *parameters, uint32_t bytes) {
  wr8(COMMAND, opcode & 0xff);
  wr8(COMMAND + 1, opcode >> 8);
  wr8(COMMAND + 2, bytes);
  memcpy(host(COMMAND + 3), parameters, bytes);
  set_up(COMMAND_REQUEST, 0, 6, COMMAND, bytes + 3);
  bluetooth_request(IOS_IOCTLV, COMMAND_REQUEST);
}

// The next event (0x81) or data packet (0x82) the chip has, copied to EVENT, or 0. A
// request that gets nothing stays held for the next call.
static int collect(int endpoint) {
  int kind = endpoint == 0x82;
  uint32_t request = kind ? DATA_REQUEST : EVENTS_REQUEST, buffer = BUFFER + (uint32_t)kind * 0x200;
  if (!pending[kind]) {
    set_up(request, kind ? 1 : 2, 2, buffer, 0x100);
    wr32(request + 0x100, (ENDPOINT + (uint32_t)kind) & 0x7fffffff);
    wr8(ENDPOINT + (uint32_t)kind, endpoint);
    pending[kind] = 1;
    bluetooth_request(IOS_IOCTLV, request);
  }
  memset(host(EVENT), 0, 0x100);
  if (!ready[kind]) return 0;
  memcpy(host(EVENT), host(buffer), 0x100);
  pending[kind] = ready[kind] = 0;
  return 1;
}

static int event(void) { return collect(0x81) ? rd8(EVENT) : 0; }

// The console sends the remote an L2CAP signal on its connection.
static void send_signal_to_remote(int code, int id, const uint8_t *body, uint32_t bytes) {
  uint8_t packet[64] = {0x00, 0x21};
  packet[2] = (uint8_t)(bytes + 8);
  packet[4] = (uint8_t)(bytes + 4);
  packet[6] = 1;
  packet[8] = (uint8_t)code;
  packet[9] = (uint8_t)id;
  packet[10] = (uint8_t)bytes;
  memcpy(packet + 12, body, bytes);
  bluetooth_receive_packet(0, packet);
}

// The remote's next L2CAP signal back, as code then body at EVENT + 12.
static int signal_back(void) { return collect(0x82) && rd8(EVENT + 6) == 1 ? rd8(EVENT + 8) : 0; }

int main(void) {
  memory_start();

  // Nothing is discoverable, so a search ends at once with nothing found.
  const uint8_t inquiry[] = {0x33, 0x8b, 0x9e, 8, 0};
  command(0x0401, inquiry, sizeof inquiry);
  CHECK_EQ(event(), 0x0f);
  CHECK_EQ(event(), 0x01);
  CHECK_EQ(rd8(EVENT + 2), 0);
  CHECK_EQ(rd8(EVENT + 3), 0);
  command(0x0402, 0, 0);
  CHECK_EQ(event(), 0x0e);

  // The console calls the remote: it answers. A stranger's address doesn't, and a remote
  // already connected says so.
  uint8_t call[13] = {0};
  memcpy(call, remote, 6);
  command(0x0405, call, sizeof call);
  CHECK_EQ(event(), 0x0f);
  CHECK_EQ(event(), 0x03);
  CHECK_EQ(rd8(EVENT + 2), 0);
  CHECK_EQ(rd8(EVENT + 3) | rd8(EVENT + 4) << 8, 0x100);
  call[0] = 0x77;
  command(0x0405, call, sizeof call);
  CHECK_EQ(event(), 0x0f);
  CHECK_EQ(event(), 0x03);
  CHECK_EQ(rd8(EVENT + 2), 0x04);

  // No stored key: the chip asks for a PIN, then hands over the new key.
  command(0x040c, remote, 6);
  CHECK_EQ(event(), 0x0e);
  CHECK(!memcmp(host(EVENT + 6), remote, 6));
  CHECK_EQ(event(), 0x16);
  CHECK(!memcmp(host(EVENT + 2), remote, 6));
  uint8_t pin[23] = {0};
  memcpy(pin, remote, 6);
  pin[6] = 6;
  command(0x040d, pin, sizeof pin);
  CHECK_EQ(event(), 0x0e);
  CHECK_EQ(event(), 0x18);
  CHECK_EQ(event(), 0x06);
  CHECK_EQ(rd8(EVENT + 2), 0);

  // Sniff mode: the remote changes mode at the longest interval allowed.
  const uint8_t sniff[] = {0x00, 0x01, 0x10, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00};
  command(0x0803, sniff, sizeof sniff);
  CHECK_EQ(event(), 0x0f);
  CHECK_EQ(event(), 0x14);
  static const uint8_t changed[] = {0x14, 6, 0, 0x00, 0x01, 2, 0x10, 0x00};
  CHECK(!memcmp(host(EVENT), changed, sizeof changed));
  CHECK_EQ(event(), 0);

  // The console opens the remote's control channel: accepted, then configured. A second
  // one to the same service and service discovery are refused.
  const uint8_t control[] = {0x11, 0, 0x70, 0};
  send_signal_to_remote(2, 1, control, sizeof control);
  CHECK_EQ(signal_back(), 3);
  static const uint8_t accepted[] = {0x40, 0, 0x70, 0, 0, 0, 0, 0};
  CHECK(!memcmp(host(EVENT + 12), accepted, sizeof accepted));
  CHECK_EQ(signal_back(), 4);
  CHECK_EQ(rd8(EVENT + 12), 0x70);
  send_signal_to_remote(2, 2, control, sizeof control);
  CHECK_EQ(signal_back(), 3);
  CHECK_EQ(rd8(EVENT + 16), 4);
  const uint8_t discovery[] = {0x01, 0, 0x72, 0};
  send_signal_to_remote(2, 3, discovery, sizeof discovery);
  CHECK_EQ(signal_back(), 3);
  CHECK_EQ(rd8(EVENT + 16), 2);
  const uint8_t done[] = {0x40, 0, 0, 0, 0, 0};
  send_signal_to_remote(5, 4, done, sizeof done);  // Configured: the console opens interrupt itself.
  CHECK_EQ(signal_back(), 0);

  // Interrupt too, then the attachment-only mode: 21 bytes of attachment, no buttons.
  const uint8_t interrupt[] = {0x13, 0, 0x71, 0};
  send_signal_to_remote(2, 5, interrupt, sizeof interrupt);
  CHECK_EQ(signal_back(), 3);
  CHECK_EQ(signal_back(), 4);
  const uint8_t interrupt_done[] = {0x41, 0, 0, 0, 0, 0};
  send_signal_to_remote(5, 6, interrupt_done, sizeof interrupt_done);
  CHECK(collect(0x82));  // The status report a newly connected remote sends.
  CHECK_EQ(rd8(EVENT + 9), 0x20);
  const uint8_t mode[] = {0x12, 0x00, 0x3d};
  wii_remote_output(0, mode, sizeof mode);
  CHECK(collect(0x82));
  CHECK_EQ(rd8(EVENT + 9), 0x22);
  wii_remote_update(0, 1);
  CHECK(collect(0x82));
  CHECK_EQ(rd8(EVENT + 2), 27);  // The channel header and 23 bytes: the HID byte, the ID and 21.
  CHECK_EQ(rd8(EVENT + 6), 0x71);
  CHECK_EQ(rd8(EVENT + 9), 0x3d);
  CHECK_EQ(rd8(EVENT + 10), 0x40);
  CHECK_EQ(rd8(EVENT + 30), 0x54);

  // The console closes interrupt: the same channel numbers come back, and reports stop.
  const uint8_t closing[] = {0x41, 0, 0x71, 0};
  send_signal_to_remote(6, 7, closing, sizeof closing);
  CHECK_EQ(signal_back(), 7);
  CHECK(!memcmp(host(EVENT + 12), closing, sizeof closing));
  wii_remote_update(0, 1000000000);
  CHECK(!collect(0x82));
  return finish();
}
