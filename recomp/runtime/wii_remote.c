// The simulated Wii Remotes and Balance Board: each answers the game's output reports
// and sends its state. A board is a remote inside, with no buttons, motion or camera
// to speak of, and its weight sensors where a remote's attachment would be.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bluetooth.h"
#include "gaps.h"
#include "input.h"
#include "remote_link.h"
#include "wii_attachment.h"
#include "wii_board.h"
#include "wii_camera.h"
#include "wii_motionplus.h"
#include "wii_remote.h"

enum {
  OUTPUT_RUMBLE = 0x10, OUTPUT_LEDS = 0x11, OUTPUT_DATA_MODE = 0x12, OUTPUT_CAMERA_CLOCK = 0x13, OUTPUT_SPEAKER = 0x14, OUTPUT_STATUS = 0x15, OUTPUT_WRITE = 0x16, OUTPUT_READ = 0x17,
  REPORT_STATUS = 0x20, REPORT_MEMORY = 0x21, REPORT_ACK = 0x22, REPORT_BYTES = 22, MEMORY_PIECE = 16, MEMORY_HEADER = 6,
  ADDRESS_SPACE_SHIFT = 24, ATTACHMENT_SPACE = 0x04a4, MOTIONPLUS_SPACE = 0x04a6, CAMERA_SPACE = 0x04b0, WRITE_NO_DEVICE = 7, READ_NO_DEVICE = 7, ACK_WANTED = 2, CONTINUOUS = 4,
  STATUS_BATTERY_LOW = 1, STATUS_ATTACHMENT = 2, STATUS_SPEAKER = 4, STATUS_CAMERA = 8, SWITCH_ON = 4, RUMBLE_ON = 1,
  BATTERY_FULL = 0xc8, CALIBRATION_START = 0x16, CALIBRATION_COPY = 0x20, REPORT_TICKS = 607500,  // 100 a second.
  ACCEL_ZERO_G = 0x80 << 2, ACCEL_ONE_G = 0x9a << 2,  // As the calibration in its memory says: Dolphin's WiimoteEmu.h.
};
static const float BATTERY_LOW = 0.1f;

// Where each data mode puts things: motion, camera and attachment bytes, by position.
typedef struct {
  uint8_t mode, motion, attachment_at, attachment_bytes, bytes, camera_at, camera_bytes;
} Mode;

static const Mode modes[] = {
  {0x30, 0, 0, 0, 3, 0, 0}, {0x31, 1, 0, 0, 6, 0, 0}, {0x32, 0, 3, 8, 11, 0, 0}, {0x33, 1, 0, 0, 18, 6, 12},
  {0x34, 0, 3, 19, 22, 0, 0}, {0x35, 1, 6, 16, 22, 0, 0}, {0x36, 0, 13, 9, 22, 3, 10}, {0x37, 1, 16, 6, 22, 6, 10},
  {0x3d, 0, 1, 21, 22, 0, 0},  // Attachment only: its 21 bytes take the buttons' place too.
};

static struct {
  const Mode *mode;
  int continuous, camera_on, speaker_on, rumble;
  uint8_t lights;
  uint64_t next_report;
  uint8_t last_report[REPORT_BYTES];
  int plug_known, motionplus, pin;  // pin: what the attachment pin last read.
  Attachment plugged;
} remotes[BLUETOOTH_DEVICES];

static int is_board(int device) { return device == BLUETOOTH_BOARD; }

// Where the simulated devices' reports go: to the game, or into the link for loopback.
static void (*sink)(int device, const uint8_t *report, uint32_t bytes) = bluetooth_send_report;
void wii_remote_simulated_sink(void (*to)(int device, const uint8_t *report, uint32_t bytes)) { sink = to; }

// What's plugged into the remote is its player's choice (input_attach), or what
// WII_RECOMP_ATTACHMENT names. A game that sees a Classic Controller may switch its pointer
// off and expect the Classic Controller's buttons.
static void follow_plug(int device, int on_connect);
static int attached(int device) { return remotes[device].plugged != ATTACHMENT_NONE; }

// The attachment pin: an attachment, or a MotionPlus, which answers for itself.
static int pin_reads(int device) {
  if (remotes[device].motionplus) return motionplus_detect(device, attached(device));
  return attached(device);
}

static int has_attachment(int device) {
  if (is_board(device)) return 1;
  if (!remotes[device].plug_known) follow_plug(device, 1);  // Asked before its first report: what's there now.
  return pin_reads(device);
}

// Remote n is player n+1's. The board has a single button, its power switch, which
// nothing presses.
static void put_buttons(uint32_t buttons, uint8_t *to) {
  to[0] = (uint8_t)(buttons & 0x1f);
  to[1] = (uint8_t)((buttons >> 8) & 0x9f);
}

static void buttons_into(int device, uint8_t *to) { put_buttons(is_board(device) ? 0 : input_buttons(device), to); }

static void report_status(int device) {
  uint8_t report[7] = {REPORT_STATUS};
  buttons_into(device, report + 1);
  // Its flags: an attachment, the speaker and camera as the game last switched them, and its
  // lights; then how full its batteries are.
  float battery = is_board(device) ? 1 : input_battery_level(device);
  report[3] = (uint8_t)((has_attachment(device) ? STATUS_ATTACHMENT : 0) | (remotes[device].speaker_on ? STATUS_SPEAKER : 0) |
                        (remotes[device].camera_on ? STATUS_CAMERA : 0) | remotes[device].lights |
                        (battery < BATTERY_LOW ? STATUS_BATTERY_LOW : 0));
  report[6] = (uint8_t)(battery * BATTERY_FULL + 0.5f);
  sink(device, report, sizeof report);
}

// A player plugging an attachment in or out. The remote stops its data reports and sends
// its status unasked, and the game sets the data mode again once it has looked, as on a
// real remote. On connecting, the port starts fresh with no report.
// A player plugging an attachment or a MotionPlus in or out, or a MotionPlus switching
// on or off. When the attachment pin changes, the remote stops its data reports and sends
// its status unasked, and the game sets the data mode again once it has looked, as on a
// real remote. Behind an active MotionPlus, an attachment changing only shows in the
// MotionPlus's own data. On connecting, everything starts fresh with no report.
static void follow_plug(int device, int on_connect) {
  if (is_board(device)) return;
  Attachment kind = input_attachment(device);
  int motionplus = input_motionplus(device), known = remotes[device].plug_known && !on_connect;
  int kind_changed = known && kind != remotes[device].plugged, motionplus_changed = known && motionplus != remotes[device].motionplus;
  if (!known || kind_changed) {
    remotes[device].plugged = kind;
    if (kind != ATTACHMENT_NONE) attachment_plug(device, kind);
  }
  if (!known || motionplus_changed) {
    remotes[device].motionplus = motionplus;
    motionplus_plug(device, motionplus);
  }
  remotes[device].plug_known = 1;
  if (motionplus) motionplus_update(device, attached(device));
  int pin = pin_reads(device), pin_changed = known && pin != remotes[device].pin;
  remotes[device].pin = pin;
  if (!(pin_changed || motionplus_changed || (kind_changed && !motionplus))) return;
  if (kind_changed || motionplus_changed) {
    fprintf(stderr, "remotes: player %d's remote now has %s%s plugged in\n", device + 1, motionplus ? "a MotionPlus and " : "",
            input_attachment_name(kind));
  }
  remotes[device].mode = 0;
  report_status(device);
}

// The game's rumble and lights, for a controller standing in for the remote to show.
static void tell_outputs(int device) {
  if (!is_board(device)) input_set_outputs(device, remotes[device].rumble, (uint8_t)(remotes[device].lights >> 4));
}

// Motion as the report carries it: 10 bits an axis, zero at 0x200 and one g 0x68 above.
// The top eight bits have bytes of their own; x's lowest two and the second-lowest of y
// and z sit in the buttons' spare bits.
void wii_remote_accel_bytes(const float accel[3], uint8_t buttons[2], uint8_t out[3]) {
  uint32_t value[3];
  for (int axis = 0; axis < 3; axis++) {
    float raw = ACCEL_ZERO_G + accel[axis] * (ACCEL_ONE_G - ACCEL_ZERO_G);
    value[axis] = raw < 0 ? 0 : raw > 0x3ff ? 0x3ff : (uint32_t)(raw + 0.5f);
    out[axis] = (uint8_t)(value[axis] >> 2);
  }
  buttons[0] = (uint8_t)((buttons[0] & 0x9f) | (value[0] & 3) << 5);
  buttons[1] = (uint8_t)((buttons[1] & 0x9f) | (value[1] >> 1 & 1) << 5 | (value[2] >> 1 & 1) << 6);
}

// An output report's acknowledgement, with an error for a write nothing answered.
static void acknowledge_with(int device, int output, int error) {
  uint8_t report[5] = {REPORT_ACK};
  buttons_into(device, report + 1);
  report[3] = (uint8_t)output;
  report[4] = (uint8_t)error;
  sink(device, report, sizeof report);
}

static void acknowledge(int device, int output) { acknowledge_with(device, output, 0); }

// Calibration sits at the start of memory: zero and gravity for the three axes, which is
// what makes a still remote read as 1 g, then a check byte the library verifies.
static void read_memory(uint32_t address, uint8_t *out, uint32_t bytes) {
  uint8_t calibration[] = {0x80, 0x80, 0x80, 0, 0x9a, 0x9a, 0x9a, 0, 0x40, 0x55};
  for (uint32_t i = 0; i + 1 < sizeof calibration; i++) calibration[sizeof calibration - 1] += calibration[i];
  for (uint32_t i = 0; i < bytes; i++) {
    uint32_t at = address + i;
    out[i] = 0;
    if (at >= CALIBRATION_START && at < CALIBRATION_START + sizeof calibration) out[i] = calibration[at - CALIBRATION_START];
    else if (at >= CALIBRATION_COPY && at < CALIBRATION_COPY + sizeof calibration) out[i] = calibration[at - CALIBRATION_COPY];
  }
}

// The attachment wire, at the attachment's address (0xa4) or a MotionPlus's own (0xa6):
// the board's registers, a MotionPlus's, or what's plugged in behind it. 0 when nothing
// there answers.
static int wire_read(int device, int motionplus_address, uint32_t offset, uint8_t *out, uint32_t bytes) {
  if (is_board(device)) {
    if (!motionplus_address) board_read(offset, out, bytes);
    return !motionplus_address;
  }
  if (remotes[device].motionplus) {
    MotionPlusAnswer answer = motionplus_read(device, motionplus_address, offset, out, bytes);
    if (answer != MOTIONPLUS_PASSED_ON) return answer == MOTIONPLUS_ANSWERED;
  } else if (motionplus_address) {
    return 0;
  }
  if (!attached(device)) return 0;
  attachment_read(device, offset, out, bytes);
  return 1;
}

static int wire_write(int device, int motionplus_address, uint32_t offset, const uint8_t *data, uint32_t bytes) {
  if (is_board(device)) {
    if (!motionplus_address) board_write(offset, data, bytes);
    return !motionplus_address;
  }
  if (remotes[device].motionplus) {
    MotionPlusAnswer answer = motionplus_write(device, motionplus_address, offset, data, bytes);
    if (answer != MOTIONPLUS_PASSED_ON) return answer == MOTIONPLUS_ANSWERED;
  } else if (motionplus_address) {
    return 0;
  }
  if (!attached(device)) return 0;
  attachment_write(device, offset, data, bytes);
  return 1;
}

// Reads come back in pieces of at most 16 bytes.
static void report_read(int device, uint32_t address, uint32_t bytes) {
  int on_wire = address >> 16 == ATTACHMENT_SPACE || address >> 16 == MOTIONPLUS_SPACE;
  int from_camera = address >> 16 == CAMERA_SPACE && !is_board(device);
  uint32_t space = address >> ADDRESS_SPACE_SHIFT, offset = address & 0xffff, done = 0;
  do {
    uint32_t part = bytes - done > MEMORY_PIECE ? MEMORY_PIECE : bytes - done;
    uint8_t report[REPORT_BYTES] = {REPORT_MEMORY};
    buttons_into(device, report + 1);
    report[3] = (uint8_t)((part - 1) << 4);
    report[4] = (uint8_t)((offset + done) >> 8);
    report[5] = (uint8_t)(offset + done);
    int answered = on_wire ? wire_read(device, address >> 16 == MOTIONPLUS_SPACE, offset + done, report + MEMORY_HEADER, part)
                           : !space || from_camera;
    if (!answered) {
      // Registers of devices that aren't there, or aren't answering just now.
      memset(report + MEMORY_HEADER, 0, part);
      report[3] |= READ_NO_DEVICE;
      sink(device, report, sizeof report);
      return;
    }
    if (from_camera) camera_read(device, offset + done, report + MEMORY_HEADER, part);
    else if (!on_wire) read_memory(offset + done, report + MEMORY_HEADER, part);
    sink(device, report, sizeof report);
    done += part;
  } while (done < bytes);
}

static uint32_t big32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void wii_remote_simulated_connected(int device) {
  remotes[device].mode = 0;
  remotes[device].lights = 0;
  remotes[device].rumble = 0;
  follow_plug(device, 1);
  tell_outputs(device);
  report_status(device);
}

void wii_remote_simulated_output(int device, const uint8_t *data, uint32_t bytes) {
  int report = data[0];
  if (!is_board(device) && !remotes[device].plug_known) follow_plug(device, 1);
  // Every report the game sends carries the rumble switch in its first bit.
  if (bytes >= 2 && (data[1] & RUMBLE_ON) != remotes[device].rumble) {
    remotes[device].rumble = data[1] & RUMBLE_ON;
    tell_outputs(device);
  }
  if (report == OUTPUT_DATA_MODE) {
    remotes[device].continuous = data[1] & CONTINUOUS;
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
      if (modes[i].mode == data[2]) remotes[device].mode = &modes[i];
    }
    if (data[2] == 0x3e || data[2] == 0x3f) gap_hit("ctl.0x3e_0x3f_interleaved", data[2]);
    memset(remotes[device].last_report, 0, sizeof remotes[device].last_report);
    acknowledge(device, report);
  } else if (report == OUTPUT_STATUS) {
    report_status(device);
  } else if (report == OUTPUT_READ) {
    report_read(device, big32(data + 1), (uint32_t)data[5] << 8 | data[6]);
  } else if (report == OUTPUT_WRITE) {
    uint32_t address = big32(data + 1);
    uint32_t bytes_written = data[5] > MEMORY_PIECE ? MEMORY_PIECE : data[5];
    int answered = 1;
    if (address >> 16 == ATTACHMENT_SPACE || address >> 16 == MOTIONPLUS_SPACE) {
      answered = wire_write(device, address >> 16 == MOTIONPLUS_SPACE, address & 0xffff, data + 6, bytes_written);
    } else if (address >> 16 == CAMERA_SPACE && !is_board(device)) {
      camera_write(device, address & 0xffff, data + 6, bytes_written);
    }
    acknowledge_with(device, report, answered ? 0 : WRITE_NO_DEVICE);
  } else if (report == OUTPUT_LEDS || report == OUTPUT_RUMBLE) {
    if (report == OUTPUT_LEDS) {
      remotes[device].lights = data[1] & 0xf0;
      tell_outputs(device);
    }
    if (data[1] & ACK_WANTED) acknowledge(device, report);
  } else if (report == OUTPUT_CAMERA_CLOCK || report == OUTPUT_SPEAKER) {
    if (report == OUTPUT_CAMERA_CLOCK) remotes[device].camera_on = (data[1] & SWITCH_ON) != 0;
    else remotes[device].speaker_on = (data[1] & SWITCH_ON) != 0;
    if (report == OUTPUT_SPEAKER && remotes[device].speaker_on) gap_hit("ctl.0x14_speaker_enable", data[1]);  // Its sound isn't played.
    if (data[1] & ACK_WANTED) acknowledge(device, report);
  } else {
    if (report == 0x18) gap_hit("ctl.0x18_speaker_data", 0);
    acknowledge(device, report);
  }
}

// Sends a device's current state as often as a real one does. Out of continuous mode,
// only when something in the report changed, such as a button, where it points, or the
// weight on the board.
void wii_remote_simulated_update(int device, uint64_t now) {
  follow_plug(device, 0);
  const Mode *mode = remotes[device].mode;
  if (!mode || now < remotes[device].next_report) return;
  remotes[device].next_report = now + REPORT_TICKS;
  uint8_t report[REPORT_BYTES] = {mode->mode};
  PlayerState player = {0};
  if (!is_board(device)) input_snapshot(device, &player);
  put_buttons(player.buttons, report + 1);
  if (mode->motion) wii_remote_accel_bytes(player.accel, report + 1, report + 3);
  if (mode->camera_bytes && !is_board(device)) {
    camera_fill(device, player.pointer_x, player.pointer_y, player.roll, report + mode->camera_at, mode->camera_bytes);
  } else if (mode->camera_bytes) {
    memset(report + mode->camera_at, 0xff, mode->camera_bytes);
  }
  if (mode->attachment_bytes && !has_attachment(device)) {
    memset(report + mode->attachment_at, 0, mode->attachment_bytes);
  } else if (mode->attachment_bytes) {
    if (is_board(device)) board_fill(report + mode->attachment_at, mode->attachment_bytes);
    else if (remotes[device].motionplus && motionplus_active(device)) {
      motionplus_fill(device, &player, attached(device), report + mode->attachment_at, mode->attachment_bytes);
    } else attachment_fill(device, &player, report + mode->attachment_at, mode->attachment_bytes);
  }
  if (!remotes[device].continuous && !memcmp(report, remotes[device].last_report, mode->bytes)) return;
  if (getenv("WII_RECOMP_BLUETOOTH_LOG") && (report[1] != remotes[device].last_report[1] || report[2] != remotes[device].last_report[2])) {
    fprintf(stderr, "bluetooth: device %d sends buttons %02x%02x at %llu\n", device, report[2], report[1], (unsigned long long)now);
  }
  memcpy(remotes[device].last_report, report, sizeof report);
  sink(device, report, mode->bytes);
}

// --- Each slot: a real device through the link, or a simulated one here ---

enum { OUTPUT_SPEAKER_DATA = 0x18, OUTPUT_RUMBLE_ONLY = 0x10, RUMBLE_BIT = 1 };

// A real device hears from the game as soon as it's connected, as the simulated one does:
// the status a remote sends when asked.
void wii_remote_connected(int device) {
  if (!link_linked(device)) {
    wii_remote_simulated_connected(device);
    return;
  }
  const uint8_t ask[2] = {OUTPUT_STATUS, 0};
  link_send(device, ask, sizeof ask);
}

// The game's reports go to the real device unchanged. Speaker data does too, unless
// WII_RECOMP_SPEAKER=off: then only its rumble bit is passed on, as a rumble report.
void wii_remote_output(int device, const uint8_t *report, uint32_t bytes) {
  if (!link_linked(device)) {
    wii_remote_simulated_output(device, report, bytes);
    return;
  }
  static int speaker_off = -1;
  if (speaker_off < 0) speaker_off = getenv("WII_RECOMP_SPEAKER") && !strcmp(getenv("WII_RECOMP_SPEAKER"), "off");
  static uint8_t rumble[BLUETOOTH_DEVICES];
  if (bytes >= 2) rumble[device] = report[1] & RUMBLE_BIT;
  if (speaker_off && report[0] == OUTPUT_SPEAKER_DATA) {
    const uint8_t only_rumble[2] = {OUTPUT_RUMBLE_ONLY, rumble[device]};
    link_send(device, only_rumble, sizeof only_rumble);
    return;
  }
  link_send(device, report, bytes);
}

void wii_remote_update(int device, uint64_t now) {
  if (!link_linked(device)) {
    wii_remote_simulated_update(device, now);
    return;
  }
  uint8_t report[LINK_REPORT_BYTES];
  for (int bytes; (bytes = link_poll(device, report)) > 0;) bluetooth_send_report(device, report, (uint32_t)bytes);
}
