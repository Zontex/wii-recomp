// The Wii's Bluetooth chip, as the game's own Wii Remote code sees it over USB. HCI
// commands arrive as control messages, events go out on one endpoint and data on another.
// Each device has its own connection, numbered from BLUETOOTH_FIRST_HANDLE, and calls
// the console in turn, the next once the one before is fully connected.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bluetooth.h"
#include "gaps.h"
#include "cpu.h"
#include "title.h"
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
  OP_SCAN_ENABLE = 0x0c1a, PAGE_SCAN = 2, LINK_ACL = 1, NAME_BYTES = 248, PARAMETERS = 3, HANDLE_MASK = 0x0fff,
  OP_READ_LOCAL_VERSION = 0x1001, OP_READ_LOCAL_FEATURES = 0x1003, OP_READ_BUFFER_SIZE = 0x1005, OP_READ_ADDRESS = 0x1009,
  STATUS_OK = 0, ACL_PACKET_BYTES = 339, SCO_PACKET_BYTES = 64, ACL_PACKETS = 10, SCO_PACKETS = 0,
};

enum { SETTINGS_COUNT = 4, SETTINGS_OFFSETS = 6, SETTINGS_NAME_MASK = 31, SETTINGS_SIZE_BYTES = 2, PAIRED_RECORD_BYTES = 70,
       REGISTERED_RECORDS = 10 };
#define PAIRED_ITEM "BT.DINF"
enum {
  EVENT_RETURN_LINK_KEYS = 0x15, EVENT_AUTHENTICATION_DONE = 0x06, OP_READ_LINK_KEYS = 0x0c0d, OP_AUTHENTICATE = 0x0411, OP_LINK_KEY_REPLY = 0x040b,
  LINK_KEY_BYTES = 16, MAX_LINK_KEYS = 8, OP_WRITE_LINK_KEYS = 0x0c11, OP_DELETE_LINK_KEYS = 0x0c12,
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
static const uint8_t remote_class[3] = {0x04, 0x25, 0x00};
static const char remote_name[] = "Nintendo RVL-CNT-01", board_name[] = "Nintendo RVL-WBC-01";

// A device's address ends in its slot, so either can be found from the other.
typedef struct {
  int present, called, connected;
  int key_deleted;  // The game deleted its stored link key and hasn't written one since.
  uint8_t address[6];
} Device;

static Device devices[BLUETOOTH_DEVICES];
static int scanning;

static uint32_t handle_of(int device) { return BLUETOOTH_FIRST_HANDLE + (uint32_t)device; }
static const char *name_of(int device) { return device == BLUETOOTH_BOARD ? board_name : remote_name; }

// The simulated remote is always there. The board is there for games built to use one,
// which are the ones with the board's calibration flag.
static void find_devices(void) {
  static int found;
  if (found) return;
  found = 1;
  devices[0].present = 1;
  devices[BLUETOOTH_BOARD].present = title->calibration_flag != 0;
  for (int i = 0; i < BLUETOOTH_DEVICES; i++) {
    static const uint8_t base[6] = {0x01, 0x00, 0x1f, 0x19, 0x00, 0x00};
    memcpy(devices[i].address, base, sizeof base);
    devices[i].address[5] = (uint8_t)i;
  }
}

// From a command's connection handle, or -1 for one that isn't ours.
static int device_by_handle(uint32_t handle) {
  int device = (int)((handle & HANDLE_MASK) - BLUETOOTH_FIRST_HANDLE);
  return device >= 0 && device < BLUETOOTH_DEVICES && devices[device].present ? device : -1;
}

static int device_by_address(const uint8_t *address) {
  for (int i = 0; i < BLUETOOTH_DEVICES; i++) {
    if (devices[i].present && !memcmp(devices[i].address, address, 6)) return i;
  }
  return -1;
}

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

// A data report: on an interrupt channel, a HID input report of the streaming kind.
static int is_data_report(const Packet *packet) {
  enum { HID_BYTE = 8, FIRST_DATA_REPORT = 0x30 };
  return packet->size > HID_BYTE + 1 && packet->bytes[HID_BYTE] == 0xa1 && packet->bytes[HID_BYTE + 1] >= FIRST_DATA_REPORT;
}

// Data from the remote to the game.
static void send_data(void) {
  while (waiting_data && packet_count) {
    uint32_t address, bytes;
    vector_at(waiting_data, 2, &address, &bytes);
    for (uint32_t i = 0; i < packets[0].size && i < bytes; i++) wr8(guest(address) + i, packets[0].bytes[i]);
    if (getenv("WII_RECOMP_BLUETOOTH_LOG") && is_data_report(&packets[0])) {
      static uint8_t delivered[2];
      if (packets[0].bytes[10] != delivered[0] || packets[0].bytes[11] != delivered[1]) {
        fprintf(stderr, "bluetooth: the game collected buttons %02x%02x, %d still waiting\n", packets[0].bytes[11], packets[0].bytes[10], packet_count - 1);
        delivered[0] = packets[0].bytes[10];
        delivered[1] = packets[0].bytes[11];
      }
    }
    ios_complete(waiting_data, (int)packets[0].size);
    waiting_data = 0;
    memmove(packets, packets + 1, (size_t)--packet_count * sizeof packets[0]);
  }
}

// Waits for the game to collect it. If the game falls behind and the queue fills, the
// oldest data report goes, never a newer one: a press then still arrives, and the game
// sees where things are now rather than a backlog. Anything else is never dropped.
void bluetooth_send_packet(const uint8_t *packet, uint32_t bytes) {
  static uint32_t dropped;
  if (packet_count == MAX_PACKETS) {
    int oldest = -1;
    for (int i = 0; i < packet_count && oldest < 0; i++) {
      if (is_data_report(&packets[i])) oldest = i;
    }
    if (oldest < 0) { fprintf(stderr, "stop: bluetooth: the game stopped collecting data, and the queue is full\n"); exit(11); }
    memmove(packets + oldest, packets + oldest + 1, (size_t)(packet_count - oldest - 1) * sizeof packets[0]);
    packet_count--;
    if (!dropped++ || dropped % 1000 == 0) fprintf(stderr, "bluetooth: the game is collecting slowly, %u old reports skipped so far\n", dropped);
  }
  memcpy(packets[packet_count].bytes, packet, bytes);
  packets[packet_count++].size = bytes;
  send_data();
}

static void packet_done(uint32_t handle) {
  const uint8_t parameters[] = {1, (uint8_t)handle, (uint8_t)(handle >> 8), 1, 0};
  queue_event(EVENT_PACKETS_DONE, parameters, sizeof parameters);
}

static void receive_data(uint32_t address, uint32_t bytes) {
  uint8_t packet[PACKET_BYTES];
  for (uint32_t i = 0; i < bytes && i < sizeof packet; i++) packet[i] = (uint8_t)rd8(address + i);
  uint32_t handle = (packet[0] | (uint32_t)packet[1] << 8) & HANDLE_MASK;
  int device = device_by_handle(handle);
  if (device < 0) {
    fprintf(stderr, "bluetooth: data for connection %03x, which isn't ours\n", handle);
    return;
  }
  bluetooth_receive_packet(device, packet);
  packet_done(handle);
}

void bluetooth_update(uint64_t now) {
  for (int i = 0; i < BLUETOOTH_DEVICES; i++) {
    if (devices[i].connected) wii_remote_update(i, now);
  }
}

static void command_status(int opcode) {
  const uint8_t parameters[] = {STATUS_OK, 1, (uint8_t)opcode, (uint8_t)(opcode >> 8)};
  queue_event(EVENT_COMMAND_STATUS, parameters, sizeof parameters);
}

// A real Wii Remote calls the console when a button is pressed. Ours call one at a time.
static void next_device_calls(void) {
  if (!scanning) return;
  for (int i = 0; i < BLUETOOTH_DEVICES; i++) {
    if (devices[i].present && devices[i].called && !devices[i].connected) return;  // Still being answered.
  }
  for (int i = 0; i < BLUETOOTH_DEVICES; i++) {
    if (!devices[i].present || devices[i].called) continue;
    uint8_t parameters[10];
    memcpy(parameters, devices[i].address, 6);
    memcpy(parameters + 6, remote_class, sizeof remote_class);
    parameters[9] = LINK_ACL;
    devices[i].called = 1;
    queue_event(EVENT_CONNECTION_REQUEST, parameters, sizeof parameters);
    return;
  }
}

static void connection_complete(int device) {
  uint8_t parameters[11] = {STATUS_OK, (uint8_t)handle_of(device), (uint8_t)(handle_of(device) >> 8)};
  memcpy(parameters + 3, devices[device].address, 6);
  parameters[9] = LINK_ACL;
  queue_event(EVENT_CONNECTION_COMPLETE, parameters, sizeof parameters);
}

static void remote_name_complete(int device) {
  uint8_t parameters[7 + NAME_BYTES] = {STATUS_OK};
  memcpy(parameters + 1, devices[device].address, 6);
  strcpy((char *)parameters + 7, name_of(device));
  queue_event(EVENT_NAME_COMPLETE, parameters, sizeof parameters);
}

static void remote_features(uint32_t handle) {
  const uint8_t parameters[] = {STATUS_OK, (uint8_t)handle, (uint8_t)(handle >> 8), 0xbc, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00};
  queue_event(EVENT_REMOTE_FEATURES, parameters, sizeof parameters);
}

static void remote_version(uint32_t handle) {
  const uint8_t parameters[] = {STATUS_OK, (uint8_t)handle, (uint8_t)(handle >> 8), 2, 0x0f, 0, 0x29, 0x02};
  queue_event(EVENT_REMOTE_VERSION, parameters, sizeof parameters);
}

static void clock_offset(uint32_t handle) {
  const uint8_t parameters[] = {STATUS_OK, (uint8_t)handle, (uint8_t)(handle >> 8), 0, 0};
  queue_event(EVENT_CLOCK_OFFSET, parameters, sizeof parameters);
}

// A command's parameters start after its opcode and length. Many name a connection or
// an address first; a device that isn't ours gets an error, never a pretend answer.
static int same_address(uint32_t guest, const uint8_t *address) {
  for (int i = 0; i < 6; i++) if (rd8(guest + (uint32_t)i) != address[i]) return 0;
  return 1;
}

// Commands the console sends seldom: calling a device itself, looking for new ones, pairing
// one with no stored key, and power saving. Answers follow the Bluetooth specification's
// events, with the details Dolphin's BTEmu.cpp gives. Returns 0 for a command not here.
enum {
  OP_INQUIRY = 0x0401, OP_INQUIRY_CANCEL = 0x0402, OP_CREATE_CONNECTION = 0x0405, OP_LINK_KEY_NEGATIVE = 0x040c,
  OP_PIN_CODE_REPLY = 0x040d, OP_SNIFF_MODE = 0x0803, EVENT_INQUIRY_COMPLETE = 0x01, EVENT_MODE_CHANGE = 0x14,
  EVENT_PIN_CODE_REQUEST = 0x16, EVENT_LINK_KEY_NOTIFICATION = 0x18, SNIFF = 2, COMBINATION_KEY = 0,
  STATUS_UNKNOWN_CONNECTION = 0x02, STATUS_PAGE_TIMEOUT = 0x04, STATUS_ALREADY_CONNECTED = 0x0b,
};

static int rare_command(int opcode, uint32_t parameters, uint32_t handle, const uint8_t *address) {
  int device = opcode == OP_SNIFF_MODE ? device_by_handle(handle) : device_by_address(address);
  if (opcode == OP_INQUIRY) {
    // Only a device whose sync button was pressed answers, and ours never are, so the
    // search ends finding none: a status, then its end with no answers.
    const uint8_t done[] = {STATUS_OK, 0};
    command_status(opcode);
    queue_event(EVENT_INQUIRY_COMPLETE, done, sizeof done);
  } else if (opcode == OP_INQUIRY_CANCEL) {
    const uint8_t results[] = {STATUS_OK};
    command_complete(opcode, results, sizeof results);
  } else if (opcode == OP_CREATE_CONNECTION) {
    // The console calls a device. One of ours that isn't connected answers at once, and
    // won't call the console as well. Any other address doesn't answer.
    command_status(opcode);
    if (device >= 0 && !devices[device].connected) {
      devices[device].called = 1;
      connection_complete(device);
    } else {
      uint8_t failed[11] = {device >= 0 ? STATUS_ALREADY_CONNECTED : STATUS_PAGE_TIMEOUT};
      memcpy(failed + 3, address, 6);
      failed[9] = LINK_ACL;
      queue_event(EVENT_CONNECTION_COMPLETE, failed, sizeof failed);
    }
  } else if (opcode == OP_LINK_KEY_NEGATIVE) {
    // The console has no key for the device, so the chip pairs them and first asks for a PIN.
    uint8_t results[7] = {STATUS_OK};
    memcpy(results + 1, address, 6);
    command_complete(opcode, results, sizeof results);
    if (device >= 0) queue_event(EVENT_PIN_CODE_REQUEST, address, 6);
  } else if (opcode == OP_PIN_CODE_REPLY) {
    // Paired: the chip hands over the new key, which it then keeps, and the link is trusted.
    uint8_t results[7] = {STATUS_OK};
    memcpy(results + 1, address, 6);
    command_complete(opcode, results, sizeof results);
    if (device >= 0) {
      uint8_t key[6 + LINK_KEY_BYTES + 1];
      memcpy(key, address, 6);
      memset(key + 6, 0x5a, LINK_KEY_BYTES);
      key[6 + LINK_KEY_BYTES] = COMBINATION_KEY;
      devices[device].key_deleted = 0;
      queue_event(EVENT_LINK_KEY_NOTIFICATION, key, sizeof key);
      const uint8_t done[] = {STATUS_OK, (uint8_t)handle_of(device), (uint8_t)(handle_of(device) >> 8)};
      queue_event(EVENT_AUTHENTICATION_DONE, done, sizeof done);
    }
  } else if (opcode == OP_SNIFF_MODE) {
    // The device saves power, reporting at the longest interval the console allows.
    const uint8_t changed[] = {device >= 0 ? STATUS_OK : STATUS_UNKNOWN_CONNECTION, (uint8_t)handle, (uint8_t)(handle >> 8), SNIFF,
                               rd8(parameters + 2), rd8(parameters + 3)};
    command_status(opcode);
    queue_event(EVENT_MODE_CHANGE, changed, sizeof changed);
  } else {
    return 0;
  }
  return 1;
}

static void run_command(uint32_t address, uint32_t bytes) {
  int opcode = (int)(rd8(address) | rd8(address + 1) << 8);
  uint32_t parameters = address + PARAMETERS;
  uint32_t handle = (rd8(parameters) | (uint32_t)rd8(parameters + 1) << 8) & HANDLE_MASK;
  uint8_t named_address[6];
  for (int i = 0; i < 6; i++) named_address[i] = (uint8_t)rd8(parameters + (uint32_t)i);
  if (getenv("WII_RECOMP_BLUETOOTH_LOG")) fprintf(stderr, "bluetooth command %04x, %u bytes\n", opcode, bytes);
  find_devices();
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
    int device = device_by_address(named_address);
    command_status(opcode);
    if (device >= 0) connection_complete(device);
  } else if (opcode == OP_READ_LINK_KEYS) {
    // Every device we have, then how many. Nothing checks the keys: both ends are here.
    int count = 0;
    for (int i = 0; i < BLUETOOTH_DEVICES; i++) {
      if (!devices[i].present || devices[i].key_deleted) continue;
      uint8_t keys[1 + 6 + LINK_KEY_BYTES] = {1};
      memcpy(keys + 1, devices[i].address, 6);
      memset(keys + 7, 0x5a, LINK_KEY_BYTES);
      queue_event(EVENT_RETURN_LINK_KEYS, keys, sizeof keys);
      count++;
    }
    const uint8_t results[] = {STATUS_OK, MAX_LINK_KEYS, 0, (uint8_t)count, 0};
    command_complete(opcode, results, sizeof results);
  } else if (opcode == OP_DELETE_LINK_KEYS) {
    // An address, then whether to delete every key. Answers how many went.
    int all = rd8(parameters + 6), deleted = 0;
    for (int i = 0; i < BLUETOOTH_DEVICES; i++) {
      if (!devices[i].present || devices[i].key_deleted || (!all && !same_address(parameters, devices[i].address))) continue;
      devices[i].key_deleted = 1;
      deleted++;
    }
    const uint8_t results[] = {STATUS_OK, (uint8_t)deleted, 0};
    command_complete(opcode, results, sizeof results);
  } else if (opcode == OP_WRITE_LINK_KEYS) {
    // How many, then each address and key. The chip keeps them, so they're listed again.
    int count = rd8(parameters), written = 0;
    for (int k = 0; k < count; k++) {
      for (int i = 0; i < BLUETOOTH_DEVICES; i++) {
        if (devices[i].present && same_address(parameters + 1 + (uint32_t)k * (6 + LINK_KEY_BYTES), devices[i].address)) {
          devices[i].key_deleted = 0;
          written++;
        }
      }
    }
    const uint8_t results[] = {STATUS_OK, (uint8_t)written};
    command_complete(opcode, results, sizeof results);
  } else if (opcode == OP_AUTHENTICATE) {
    int device = device_by_handle(handle);
    command_status(opcode);
    if (device >= 0) queue_event(EVENT_LINK_KEY_REQUEST, devices[device].address, 6);
  } else if (opcode == OP_LINK_KEY_REPLY) {
    int device = device_by_address(named_address);
    uint8_t results[7] = {STATUS_OK};
    memcpy(results + 1, named_address, 6);
    command_complete(opcode, results, sizeof results);
    if (device >= 0) {
      const uint8_t done[] = {STATUS_OK, (uint8_t)handle_of(device), (uint8_t)(handle_of(device) >> 8)};
      queue_event(EVENT_AUTHENTICATION_DONE, done, sizeof done);
    }
  } else if (opcode == OP_PACKET_TYPE) {
    const uint8_t changed[] = {STATUS_OK, (uint8_t)handle, (uint8_t)(handle >> 8), rd8(parameters + 2), rd8(parameters + 3)};
    command_status(opcode);
    queue_event(EVENT_PACKET_TYPE_CHANGED, changed, sizeof changed);
  } else if (opcode == OP_LINK_POLICY || opcode == OP_SUPERVISION_TIMEOUT) {
    const uint8_t results[] = {STATUS_OK, (uint8_t)handle, (uint8_t)(handle >> 8)};
    int device = device_by_handle(handle);
    command_complete(opcode, results, sizeof results);
    // Setting the link policy is the console's last step, so the device opens its channels.
    if (opcode == OP_LINK_POLICY && device >= 0) {
      devices[device].connected = 1;
      bluetooth_open_channels(device);
      next_device_calls();
    }
  } else if (opcode == OP_DISCONNECT) {
    int device = device_by_handle(handle);
    const uint8_t gone[] = {STATUS_OK, (uint8_t)handle, (uint8_t)(handle >> 8), rd8(parameters + 2)};
    fprintf(stderr, "bluetooth: the game hung up on device %d, reason %02x\n", device, rd8(parameters + 2));
    command_status(opcode);
    queue_event(EVENT_DISCONNECTED, gone, sizeof gone);
    if (device >= 0) {
      devices[device].connected = 0;
      bluetooth_forget_channels(device);
    }
  } else if (opcode == OP_REMOTE_NAME) {
    command_status(opcode);
    if (device_by_address(named_address) >= 0) remote_name_complete(device_by_address(named_address));
  } else if (opcode == OP_REMOTE_FEATURES) {
    command_status(opcode);
    remote_features(handle);
  } else if (opcode == OP_REMOTE_VERSION) {
    command_status(opcode);
    remote_version(handle);
  } else if (opcode == OP_CLOCK_OFFSET) {
    command_status(opcode);
    clock_offset(handle);
  } else if (!rare_command(opcode, parameters, handle, named_address)) {
    const uint8_t results[] = {STATUS_OK};
    command_complete(opcode, results, sizeof results);
    if (opcode == OP_SCAN_ENABLE && (rd8(parameters) & PAGE_SCAN)) {
      scanning = 1;
      next_device_calls();
    }
  }
}

// The settings file lists its items by offset. Each starts with a byte of type and name
// length, then the name, then a two-byte size and the value. BT.DINF holds how many
// devices are paired, ten paired records, then the active ones, where the fifth is
// always the board. Records are an address, stored the other way round, and a name.
static void write_record(uint8_t *record, int device) {
  memset(record, 0, PAIRED_RECORD_BYTES);
  for (int n = 0; n < 6; n++) record[n] = devices[device].address[5 - n];
  memcpy(record + 6, name_of(device), strlen(name_of(device)));
}

void bluetooth_register_remote(uint8_t *settings, size_t bytes) {
  find_devices();
  uint32_t items = (uint32_t)settings[SETTINGS_COUNT] << 8 | settings[SETTINGS_COUNT + 1];
  for (uint32_t i = 0; i < items; i++) {
    uint32_t at = (uint32_t)settings[SETTINGS_OFFSETS + i * 2] << 8 | settings[SETTINGS_OFFSETS + i * 2 + 1];
    uint32_t name_bytes = (uint32_t)(settings[at] & SETTINGS_NAME_MASK) + 1;
    if (at + 1 + name_bytes + SETTINGS_SIZE_BYTES > bytes || name_bytes != sizeof PAIRED_ITEM - 1) continue;
    if (memcmp(settings + at + 1, PAIRED_ITEM, name_bytes)) continue;
    uint8_t *size_at = settings + at + 1 + name_bytes;
    uint32_t value_bytes = ((uint32_t)size_at[0] << 8 | size_at[1]) + 1;
    uint8_t *value = size_at + SETTINGS_SIZE_BYTES;
    uint32_t active_at = 1 + REGISTERED_RECORDS * PAIRED_RECORD_BYTES;
    if (value + value_bytes > settings + bytes || value_bytes < active_at + BLUETOOTH_DEVICES * PAIRED_RECORD_BYTES) {
      fprintf(stderr, "stop: bluetooth: the settings' list of paired devices is %u bytes, too short\n", value_bytes);
      exit(11);
    }
    int registered = 0;
    for (int device = 0; device < BLUETOOTH_DEVICES; device++) {
      if (!devices[device].present) continue;
      write_record(value + 1 + registered++ * PAIRED_RECORD_BYTES, device);
      write_record(value + active_at + device * PAIRED_RECORD_BYTES, device);
    }
    fprintf(stderr, "bluetooth: registered %d simulated devices, was %u paired\n", registered, value[0]);
    value[0] = (uint8_t)registered;
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
