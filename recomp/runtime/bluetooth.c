// The Wii's Bluetooth chip, as the game's own Wii Remote code sees it over USB. HCI
// commands arrive as control messages, events go out on one endpoint and data on another.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bluetooth.h"
#include "cpu.h"
#include "ios.h"
#include "wii_remote.h"

enum {
  USB_CONTROL = 0, USB_BULK = 1, USB_INTERRUPT = 2, VECTOR_BYTES = 8, CONTROL_DATA_VECTOR = 6,
  ENDPOINT_EVENTS = 0x81, ENDPOINT_DATA_IN = 0x82,
  EVENT_COMMAND_COMPLETE = 0x0e, EVENT_COMMAND_STATUS = 0x0f, EVENT_BYTES = 0x100, MAX_EVENTS = 32,
  EVENT_CONNECTION_COMPLETE = 0x03, EVENT_CONNECTION_REQUEST = 0x04, EVENT_NAME_COMPLETE = 0x07,
  EVENT_REMOTE_FEATURES = 0x0b, EVENT_REMOTE_VERSION = 0x0c, EVENT_CLOCK_OFFSET = 0x1c, EVENT_LINK_KEY_REQUEST = 0x17,
  OP_ACCEPT_CONNECTION = 0x0409, OP_REMOTE_NAME = 0x0419, OP_REMOTE_FEATURES = 0x041b, OP_REMOTE_VERSION = 0x041d, OP_CLOCK_OFFSET = 0x041f,
  EVENT_DISCONNECTED = 0x05, EVENT_PACKET_TYPE_CHANGED = 0x1d,
  OP_DISCONNECT = 0x0406, OP_PACKET_TYPE = 0x040f, OP_LINK_POLICY = 0x080d, OP_SUPERVISION_TIMEOUT = 0x0c37,
  OP_SCAN_ENABLE = 0x0c1a, PAGE_SCAN = 2, LINK_ACL = 1, HANDLE = BLUETOOTH_HANDLE, NAME_BYTES = 248,
  OP_READ_LOCAL_VERSION = 0x1001, OP_READ_LOCAL_FEATURES = 0x1003, OP_READ_BUFFER_SIZE = 0x1005, OP_READ_ADDRESS = 0x1009,
  STATUS_OK = 0, ACL_PACKET_BYTES = 339, SCO_PACKET_BYTES = 64, ACL_PACKETS = 10, SCO_PACKETS = 0,
};

enum { SETTINGS_COUNT = 4, SETTINGS_OFFSETS = 6, SETTINGS_NAME_MASK = 31, SETTINGS_SIZE_BYTES = 2, PAIRED_RECORD_BYTES = 70 };
#define PAIRED_ITEM "BT.DINF"
enum {
  EVENT_RETURN_LINK_KEYS = 0x15, EVENT_AUTHENTICATION_DONE = 0x06, OP_READ_LINK_KEYS = 0x0c0d, OP_AUTHENTICATE = 0x0411, OP_LINK_KEY_REPLY = 0x040b,
  LINK_KEY_BYTES = 16, MAX_LINK_KEYS = 8,
  EVENT_PACKETS_DONE = 0x13, PACKET_BYTES = 128, MAX_PACKETS = 64,
};

typedef struct {
  uint8_t bytes[EVENT_BYTES];
  uint32_t size;
} Event;

typedef struct {
  uint8_t bytes[PACKET_BYTES];
  uint32_t size;
} Packet;

static const uint8_t chip_address[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static const uint8_t remote_address[6] = {0x01, 0x00, 0x1f, 0x19, 0x00, 0x00};
static const uint8_t remote_class[3] = {0x04, 0x25, 0x00};
static const char remote_name[] = "Nintendo RVL-CNT-01";
static int remote_called;
static Packet packets[MAX_PACKETS];
static int packet_count;
static uint32_t waiting_events, waiting_data;  // Requests held open until there's something to send.
static Event events[MAX_EVENTS];
static int event_count;

static uint32_t guest(uint32_t address) { return address | 0x80000000u; }

static void vector_at(uint32_t request, uint32_t index, uint32_t *address, uint32_t *bytes) {
  uint32_t vectors = guest(rd32(request + 24)) + index * VECTOR_BYTES;
  *address = rd32(vectors);
  *bytes = rd32(vectors + 4);
}

static void send_events(void) {
  while (waiting_events && event_count) {
    uint32_t address, bytes;
    vector_at(waiting_events, 2, &address, &bytes);
    for (uint32_t i = 0; i < events[0].size && i < bytes; i++) wr8(guest(address) + i, events[0].bytes[i]);
    ios_complete(waiting_events, (int)events[0].size);
    waiting_events = 0;
    memmove(events, events + 1, (size_t)--event_count * sizeof events[0]);
  }
}

static void queue_event(int code, const uint8_t *parameters, uint32_t bytes) {
  if (event_count == MAX_EVENTS) { fprintf(stderr, "stop: bluetooth: too many events waiting\n"); exit(11); }
  Event *event = &events[event_count++];
  event->bytes[0] = (uint8_t)code;
  event->bytes[1] = (uint8_t)bytes;
  memcpy(event->bytes + 2, parameters, bytes);
  event->size = bytes + 2;
  send_events();
}

static void command_complete(int opcode, const uint8_t *results, uint32_t bytes) {
  uint8_t parameters[EVENT_BYTES] = {1, (uint8_t)opcode, (uint8_t)(opcode >> 8)};
  memcpy(parameters + 3, results, bytes);
  queue_event(EVENT_COMMAND_COMPLETE, parameters, bytes + 3);
}

// Data from the remote to the game.
static void send_data(void) {
  while (waiting_data && packet_count) {
    uint32_t address, bytes;
    vector_at(waiting_data, 2, &address, &bytes);
    for (uint32_t i = 0; i < packets[0].size && i < bytes; i++) wr8(guest(address) + i, packets[0].bytes[i]);
    ios_complete(waiting_data, (int)packets[0].size);
    waiting_data = 0;
    memmove(packets, packets + 1, (size_t)--packet_count * sizeof packets[0]);
  }
}

void bluetooth_send_packet(const uint8_t *packet, uint32_t bytes) {
  if (packet_count == MAX_PACKETS) return;
  memcpy(packets[packet_count].bytes, packet, bytes);
  packets[packet_count++].size = bytes;
  send_data();
}

static void packet_done(void) {
  const uint8_t parameters[] = {1, HANDLE & 255, HANDLE >> 8, 1, 0};
  queue_event(EVENT_PACKETS_DONE, parameters, sizeof parameters);
}

static void receive_data(uint32_t address, uint32_t bytes) {
  uint8_t packet[PACKET_BYTES];
  for (uint32_t i = 0; i < bytes && i < sizeof packet; i++) packet[i] = (uint8_t)rd8(address + i);
  bluetooth_receive_packet(packet);
  packet_done();
}

void bluetooth_update(uint64_t now) { wii_remote_update(now); }

static void command_status(int opcode) {
  const uint8_t parameters[] = {STATUS_OK, 1, (uint8_t)opcode, (uint8_t)(opcode >> 8)};
  queue_event(EVENT_COMMAND_STATUS, parameters, sizeof parameters);
}

// A real Wii Remote calls the console when a button is pressed.
static void remote_calls(void) {
  uint8_t parameters[10];
  memcpy(parameters, remote_address, sizeof remote_address);
  memcpy(parameters + 6, remote_class, sizeof remote_class);
  parameters[9] = LINK_ACL;
  remote_called = 1;
  queue_event(EVENT_CONNECTION_REQUEST, parameters, sizeof parameters);
}

static void connection_complete(void) {
  uint8_t parameters[11] = {STATUS_OK, HANDLE & 255, HANDLE >> 8};
  memcpy(parameters + 3, remote_address, sizeof remote_address);
  parameters[9] = LINK_ACL;
  queue_event(EVENT_CONNECTION_COMPLETE, parameters, sizeof parameters);
}

static void remote_name_complete(void) {
  uint8_t parameters[7 + NAME_BYTES] = {STATUS_OK};
  memcpy(parameters + 1, remote_address, sizeof remote_address);
  memcpy(parameters + 7, remote_name, sizeof remote_name);
  queue_event(EVENT_NAME_COMPLETE, parameters, sizeof parameters);
}

static void remote_features(void) {
  const uint8_t parameters[] = {STATUS_OK, HANDLE & 255, HANDLE >> 8, 0xbc, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00};
  queue_event(EVENT_REMOTE_FEATURES, parameters, sizeof parameters);
}

static void remote_version(void) {
  const uint8_t parameters[] = {STATUS_OK, HANDLE & 255, HANDLE >> 8, 2, 0x0f, 0, 0x29, 0x02};
  queue_event(EVENT_REMOTE_VERSION, parameters, sizeof parameters);
}

static void clock_offset(void) {
  const uint8_t parameters[] = {STATUS_OK, HANDLE & 255, HANDLE >> 8, 0, 0};
  queue_event(EVENT_CLOCK_OFFSET, parameters, sizeof parameters);
}

static void run_command(uint32_t address, uint32_t bytes) {
  int opcode = (int)(rd8(address) | rd8(address + 1) << 8);
  if (getenv("WII_RECOMP_BLUETOOTH_LOG")) fprintf(stderr, "bluetooth command %04x, %u bytes\n", opcode, bytes);
  if (opcode == OP_READ_BUFFER_SIZE) {
    const uint8_t results[] = {STATUS_OK, ACL_PACKET_BYTES & 255, ACL_PACKET_BYTES >> 8, SCO_PACKET_BYTES, ACL_PACKETS, 0, SCO_PACKETS, 0};
    command_complete(opcode, results, sizeof results);
  } else if (opcode == OP_READ_LOCAL_VERSION) {
    const uint8_t results[] = {STATUS_OK, 3, 0x40, 0x0c, 3, 0x0f, 0, 0x40, 0x0c};
    command_complete(opcode, results, sizeof results);
  } else if (opcode == OP_READ_LOCAL_FEATURES) {
    const uint8_t results[] = {STATUS_OK, 0xff, 0xff, 0x8d, 0xfe, 0x9b, 0xf9, 0x00, 0x80};
    command_complete(opcode, results, sizeof results);
  } else if (opcode == OP_READ_ADDRESS) {
    uint8_t results[7] = {STATUS_OK};
    memcpy(results + 1, chip_address, sizeof chip_address);
    command_complete(opcode, results, sizeof results);
  } else if (opcode == OP_ACCEPT_CONNECTION) {
    command_status(opcode);
    connection_complete();
  } else if (opcode == OP_READ_LINK_KEYS) {
    uint8_t keys[1 + 6 + LINK_KEY_BYTES] = {1};
    const uint8_t results[] = {STATUS_OK, MAX_LINK_KEYS, 0, 1, 0};
    memcpy(keys + 1, remote_address, sizeof remote_address);
    memset(keys + 7, 0x5a, LINK_KEY_BYTES);  // Nothing checks it: both ends of this link are here.
    queue_event(EVENT_RETURN_LINK_KEYS, keys, sizeof keys);
    command_complete(opcode, results, sizeof results);
  } else if (opcode == OP_AUTHENTICATE) {
    command_status(opcode);
    queue_event(EVENT_LINK_KEY_REQUEST, remote_address, sizeof remote_address);
  } else if (opcode == OP_LINK_KEY_REPLY) {
    uint8_t results[7] = {STATUS_OK};
    const uint8_t done[] = {STATUS_OK, HANDLE & 255, HANDLE >> 8};
    memcpy(results + 1, remote_address, sizeof remote_address);
    command_complete(opcode, results, sizeof results);
    queue_event(EVENT_AUTHENTICATION_DONE, done, sizeof done);
  } else if (opcode == OP_PACKET_TYPE) {
    const uint8_t parameters[] = {STATUS_OK, HANDLE & 255, HANDLE >> 8, rd8(address + 5), rd8(address + 6)};
    command_status(opcode);
    queue_event(EVENT_PACKET_TYPE_CHANGED, parameters, sizeof parameters);
  } else if (opcode == OP_LINK_POLICY || opcode == OP_SUPERVISION_TIMEOUT) {
    const uint8_t results[] = {STATUS_OK, HANDLE & 255, HANDLE >> 8};
    command_complete(opcode, results, sizeof results);
    if (opcode == OP_LINK_POLICY) bluetooth_open_channels();  // Whoever called opens the channels.
  } else if (opcode == OP_DISCONNECT) {
    const uint8_t parameters[] = {STATUS_OK, HANDLE & 255, HANDLE >> 8, rd8(address + 5)};
    fprintf(stderr, "bluetooth: the game hung up, reason %02x\n", rd8(address + 5));
    command_status(opcode);
    queue_event(EVENT_DISCONNECTED, parameters, sizeof parameters);
  } else if (opcode == OP_REMOTE_NAME) {
    command_status(opcode);
    remote_name_complete();
  } else if (opcode == OP_REMOTE_FEATURES) {
    command_status(opcode);
    remote_features();
  } else if (opcode == OP_REMOTE_VERSION) {
    command_status(opcode);
    remote_version();
  } else if (opcode == OP_CLOCK_OFFSET) {
    command_status(opcode);
    clock_offset();
  } else {
    const uint8_t results[] = {STATUS_OK};
    command_complete(opcode, results, sizeof results);
    if (opcode == OP_SCAN_ENABLE && (rd8(address + 3) & PAGE_SCAN) && !remote_called) remote_calls();
  }
}

// The settings file lists its items by offset. Each starts with a byte of type and name
// length, then the name, then the value. BT.DINF is a two-byte size, a count of paired
// remotes, then their records.
void bluetooth_register_remote(uint8_t *settings, size_t bytes) {
  uint32_t items = (uint32_t)settings[SETTINGS_COUNT] << 8 | settings[SETTINGS_COUNT + 1];
  for (uint32_t i = 0; i < items; i++) {
    uint32_t at = (uint32_t)settings[SETTINGS_OFFSETS + i * 2] << 8 | settings[SETTINGS_OFFSETS + i * 2 + 1];
    uint32_t name_bytes = (uint32_t)(settings[at] & SETTINGS_NAME_MASK) + 1;
    if (at + 1 + name_bytes + PAIRED_RECORD_BYTES + 3 > bytes || name_bytes != sizeof PAIRED_ITEM - 1) continue;
    if (memcmp(settings + at + 1, PAIRED_ITEM, name_bytes)) continue;
    uint8_t *value = settings + at + 1 + name_bytes + SETTINGS_SIZE_BYTES;
    fprintf(stderr, "bluetooth: registered the simulated remote, was %u paired\n", value[0]);
    value[0] = 1;
    for (size_t n = 0; n < sizeof remote_address; n++) value[1 + n] = remote_address[sizeof remote_address - 1 - n];  // Stored the other way round.
    memcpy(value + 1 + sizeof remote_address, remote_name, sizeof remote_name);
    return;
  }
}

int bluetooth_request(uint32_t command, uint32_t request) {
  if (command != IOS_IOCTLV) return 0;
  uint32_t code = rd32(request + 12), address, bytes, endpoint_address, ignored;
  if (code == USB_CONTROL) {
    vector_at(request, CONTROL_DATA_VECTOR, &address, &bytes);
    run_command(guest(address), bytes);
    return (int)bytes;
  }
  vector_at(request, 0, &endpoint_address, &ignored);
  int endpoint = (int)rd8(guest(endpoint_address));
  if (code == USB_INTERRUPT && endpoint == ENDPOINT_EVENTS) {
    waiting_events = request;
    send_events();
    return IOS_NO_REPLY;
  }
  if (code == USB_BULK && endpoint == ENDPOINT_DATA_IN) {
    waiting_data = request;
    send_data();
    return IOS_NO_REPLY;
  }
  vector_at(request, 2, &address, &bytes);
  if (getenv("WII_RECOMP_BLUETOOTH_LOG")) {
    fprintf(stderr, "bluetooth data out, %u bytes:", bytes);
    for (uint32_t i = 0; i < bytes && i < 24; i++) fprintf(stderr, " %02x", rd8(guest(address) + i));
    fprintf(stderr, "\n");
  }
  receive_data(guest(address), bytes);
  return (int)bytes;
}
