// The simulated Wii MotionPlus wii_motionplus.h describes. Its registers, timings, data
// layout and calibration follow Dolphin's MotionPlus.cpp and MotionPlus.h; the answers
// to the games' check come from wii_motionplus_challenge.h.
#include <math.h>
#include <string.h>

#include "cpu.h"
#include "wii_attachment.h"
#include "wii_motionplus.h"
#include "wii_motionplus_challenge.h"

enum {
  REGISTERS = 256, DATA_BYTES = 21, CALIBRATION = 0x20, CALIBRATION_BYTES = 0x20, PASSED_CALIBRATION = 0x40,
  CHALLENGE_DATA = 0x50, INIT_TRIGGER = 0xf0, CHALLENGE_TYPE = 0xf1, PASSED_ID_4 = 0xf6, CHALLENGE_STATE = 0xf7,
  PASSED_ID_0 = 0xf8, PASSED_ID_5 = 0xf9, IDENTITY = 0xfa, ADDRESS_BYTE = IDENTITY + 2, MODE = IDENTITY + 4,
  ACTIVE_ADDRESS = 0x52 << 1, INACTIVE_ADDRESS = 0x53 << 1,
  MODE_ALONE = 0x04, MODE_NUNCHUK = 0x05, MODE_CLASSIC = 0x07,
  // The check's progress, as the game reads it at 0xf7. Activating is Dolphin's own.
  ACTIVATING = 0x00, PREPARING_X = 0x02, X_READY = 0x0e, PREPARING_Y = 0x14, Y_READY = 0x1a,
  // How long each step takes, in milliseconds, as Dolphin times them.
  CHANGE_MS = 20, PREPARE_X_MS = 500, PREPARE_Y0_MS = 30, PREPARE_Y1_MS = 500, TICKS_PER_MS = 60750,
  // Sixteen-bit calibration around 0x8000; readings have fourteen bits.
  CALIBRATION_ZERO = 0x8000, SCALE_OFFSET = 0x4400, FAST_DEGREES = 1200, SLOW_DEGREES = 270,
  READING_ZERO = CALIBRATION_ZERO >> 2, READING_MOST = (1 << 14) - 1, READING_REACH = 1 << 13,
};

typedef enum { INACTIVE, CHANGING_ON, ACTIVE, CHANGING_OFF } Status;

typedef struct {
  int present;
  uint8_t registers[REGISTERS];
  uint64_t busy_until;  // The end of its current step.
} MotionPlus;

static MotionPlus motionpluses[PLAYERS];

static MotionPlus *at(int device) { return &motionpluses[device >= 0 && device < PLAYERS ? device : 0]; }
static uint64_t ms(int milliseconds) { return (uint64_t)milliseconds * TICKS_PER_MS; }

static Status status_of(const MotionPlus *mp) {
  if (mp->registers[ADDRESS_BYTE] == ACTIVE_ADDRESS) return mp->registers[CHALLENGE_STATE] == ACTIVATING ? CHANGING_ON : ACTIVE;
  return time_base() < mp->busy_until ? CHANGING_OFF : INACTIVE;
}

// zlib's CRC-32, which the calibration's check uses.
static uint32_t crc32_of(const uint8_t *bytes, int count, uint32_t crc) {
  crc = ~crc;
  for (int i = 0; i < count; i++) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; bit++) crc = crc >> 1 ^ (crc & 1 ? 0xedb88320u : 0);
  }
  return ~crc;
}

static void put16(uint8_t *to, int value) {
  to[0] = (uint8_t)(value >> 8);
  to[1] = (uint8_t)value;
}

// Two blocks, fast then slow: each axis's zero and scale (yaw, roll, pitch), then the
// degrees a second the scale stands for, over six; an ID byte; half a CRC-32 of both.
static void write_calibration(uint8_t *to) {
  static const int scales[3] = {CALIBRATION_ZERO - SCALE_OFFSET, CALIBRATION_ZERO + SCALE_OFFSET, CALIBRATION_ZERO - SCALE_OFFSET};
  static const uint8_t ids[2] = {0x0b, 0xe9};
  for (int block = 0; block < 2; block++) {
    uint8_t *b = to + block * 16;
    for (int axis = 0; axis < 3; axis++) {
      put16(b + axis * 2, CALIBRATION_ZERO);
      put16(b + 6 + axis * 2, scales[axis]);
    }
    b[12] = (uint8_t)((block ? SLOW_DEGREES : FAST_DEGREES) / 6);
    b[13] = ids[block];
  }
  uint32_t crc = crc32_of(to, 14, 0);
  crc = crc32_of(to + 16, 14, crc);
  put16(to + 14, (int)(crc >> 16));
  put16(to + 30, (int)(crc & 0xffff));
}

void motionplus_plug(int device, int present) {
  MotionPlus *mp = at(device);
  memset(mp, 0, sizeof *mp);
  mp->present = present;
  static const uint8_t identity[6] = {0x00, 0x00, INACTIVE_ADDRESS, 0x20, 0x00, 0x05};
  memcpy(mp->registers + IDENTITY, identity, sizeof identity);
  write_calibration(mp->registers + CALIBRATION);
}

int motionplus_present(int device) { return at(device)->present; }
int motionplus_active(int device) { return at(device)->present && status_of(at(device)) == ACTIVE; }

// While it changes over, the attachment wire answers nothing, and its pin reads nothing.
static void activate(MotionPlus *mp) {
  mp->registers[ADDRESS_BYTE] = ACTIVE_ADDRESS;
  memset(mp->registers, 0, DATA_BYTES);
  mp->registers[CHALLENGE_STATE] = ACTIVATING;
  mp->busy_until = time_base() + ms(CHANGE_MS);
}

static void deactivate(MotionPlus *mp) {
  mp->registers[ADDRESS_BYTE] = INACTIVE_ADDRESS;
  mp->busy_until = time_base() + ms(CHANGE_MS);
}

static void mode_written(MotionPlus *mp) {
  int mode = mp->registers[MODE], wanted = mode == MODE_ALONE || mode == MODE_NUNCHUK || mode == MODE_CLASSIC;
  Status status = status_of(mp);
  if (wanted && status != ACTIVE) activate(mp);
  else if (!wanted && status != INACTIVE) deactivate(mp);
}

int motionplus_detect(int device, int attached) {
  MotionPlus *mp = at(device);
  Status status = status_of(mp);
  return status == INACTIVE ? attached : status == ACTIVE;
}

void motionplus_update(int device, int attached) {
  MotionPlus *mp = at(device);
  if (!mp->present) return;
  uint64_t now = time_base();
  if (status_of(mp) == CHANGING_ON && now >= mp->busy_until) {
    // Active now. Games give it about a minute for x; a real one takes a second and a half.
    mp->registers[CHALLENGE_STATE] = PREPARING_X;
    mp->busy_until = now + ms(PREPARE_X_MS);
  }
  if (status_of(mp) != ACTIVE) return;
  // An attachment plugged in behind it is set up as the MotionPlus does: encryption off,
  // then its identity and calibration copied where the game can read them.
  int seen = mp->registers[4] & 1;
  if (attached != seen) {
    if (attached) {
      const uint8_t plain = 0x55;
      uint8_t identity[6];
      attachment_write(device, INIT_TRIGGER, &plain, 1);
      attachment_read(device, IDENTITY, identity, sizeof identity);
      mp->registers[PASSED_ID_0] = identity[0];
      mp->registers[PASSED_ID_4] = identity[4];
      mp->registers[PASSED_ID_5] = identity[5];
      attachment_read(device, CALIBRATION, mp->registers + PASSED_CALIBRATION, 16);
    }
    mp->registers[4] = (uint8_t)((mp->registers[4] & ~1) | attached);
  }
  if (now < mp->busy_until) return;
  if (mp->registers[CHALLENGE_STATE] == PREPARING_X) {
    memcpy(mp->registers + CHALLENGE_DATA, MOTIONPLUS_X, sizeof MOTIONPLUS_X);
    mp->registers[CHALLENGE_STATE] = X_READY;
  } else if (mp->registers[CHALLENGE_STATE] == PREPARING_Y) {
    memcpy(mp->registers + CHALLENGE_DATA, mp->registers[CHALLENGE_TYPE] ? MOTIONPLUS_Y1 : MOTIONPLUS_Y0, sizeof MOTIONPLUS_Y0);
    mp->registers[CHALLENGE_STATE] = Y_READY;
  }
}

MotionPlusAnswer motionplus_read(int device, int own_address, uint32_t offset, uint8_t *out, uint32_t bytes) {
  MotionPlus *mp = at(device);
  Status status = status_of(mp);
  if (status == INACTIVE && !own_address) return MOTIONPLUS_PASSED_ON;
  if ((status == INACTIVE && own_address) || (status == ACTIVE && !own_address)) {
    for (uint32_t i = 0; i < bytes; i++) out[i] = mp->registers[(offset + i) % REGISTERS];
    return MOTIONPLUS_ANSWERED;
  }
  return MOTIONPLUS_NOT_HERE;  // Active, it doesn't answer at its own address; changing over, at neither.
}

MotionPlusAnswer motionplus_write(int device, int own_address, uint32_t offset, const uint8_t *data, uint32_t bytes) {
  MotionPlus *mp = at(device);
  Status status = status_of(mp);
  if (status == INACTIVE && !own_address) return MOTIONPLUS_PASSED_ON;
  if (!((status == INACTIVE && own_address) || (status == ACTIVE && !own_address))) return MOTIONPLUS_NOT_HERE;
  for (uint32_t i = 0; i < bytes; i++) mp->registers[(offset + i) % REGISTERS] = data[i];
  if (status == INACTIVE) {
    if (offset == MODE) mode_written(mp);
    return MOTIONPLUS_ANSWERED;
  }
  if (offset == INIT_TRIGGER) {
    // Any write here turns it off, and is passed on: the attachment behind it starts up.
    deactivate(mp);
    return MOTIONPLUS_PASSED_ON;
  }
  if (offset == CHALLENGE_TYPE && mp->registers[CHALLENGE_STATE] == X_READY) {
    mp->busy_until = time_base() + ms(mp->registers[CHALLENGE_TYPE] ? PREPARE_Y1_MS : PREPARE_Y0_MS);
    mp->registers[CHALLENGE_STATE] = PREPARING_Y;
  }
  if (offset == MODE) mode_written(mp);  // Games sometimes turn it off by writing 0 here.
  return MOTIONPLUS_ANSWERED;
}

// Fourteen bits an axis around 0x2000: fine steps up to about 500 degrees a second, coarse
// ones beyond, each axis saying which. Yaw, roll and pitch, then their top bits and flags.
void motionplus_gyro_bytes(const float gyro[3], int attached, uint8_t out[6]) {
  const double units = (SCALE_OFFSET >> 2) * 180 / M_PI;  // Dolphin's VALUE_SCALE_DEGREES: the scale in fourteen bits.
  const double slow_scale = units / SLOW_DEGREES, fast_scale = units / FAST_DEGREES;
  int value[3], slow[3];
  for (int axis = 0; axis < 3; axis++) {
    double radians = gyro[axis] * M_PI / 180;
    slow[axis] = fabs(radians) < READING_REACH / slow_scale;
    long reading = lround(radians * (slow[axis] ? slow_scale : fast_scale)) + READING_ZERO;
    value[axis] = (int)(reading < 0 ? 0 : reading > READING_MOST ? READING_MOST : reading);
  }
  int pitch = value[0], roll = value[1], yaw = value[2];
  out[0] = (uint8_t)yaw;
  out[1] = (uint8_t)roll;
  out[2] = (uint8_t)pitch;
  out[3] = (uint8_t)(slow[0] | slow[2] << 1 | (yaw >> 8) << 2);
  out[4] = (uint8_t)(attached | slow[1] << 1 | (roll >> 8) << 2);
  out[5] = (uint8_t)(1 << 1 | (pitch >> 8) << 2);  // A MotionPlus reading, not the attachment's.
}

static void set_bit(uint8_t *byte, int bit, int value) { *byte = (uint8_t)((*byte & ~(1 << bit)) | (value & 1) << bit); }
static int get_bit(uint8_t byte, int bit) { return byte >> bit & 1; }

// Passing through, the attachment's bytes lose some low bits to make room for the
// MotionPlus's flags, moved as wiibrew and Dolphin describe.
void motionplus_pass_through(Attachment kind, uint8_t data[6]) {
  if (kind == ATTACHMENT_NUNCHUK) {
    set_bit(&data[5], 6, get_bit(data[5], 7));
    set_bit(&data[5], 7, get_bit(data[4], 0));
    set_bit(&data[5], 4, get_bit(data[5], 3));
    set_bit(&data[5], 3, get_bit(data[5], 1));
    set_bit(&data[5], 2, get_bit(data[5], 0));
  } else if (kind == ATTACHMENT_CLASSIC) {
    set_bit(&data[0], 0, get_bit(data[5], 0));
    set_bit(&data[1], 0, get_bit(data[5], 1));
  }
}

// Every other report, when the mode passes one through and one is there, the attachment's
// state; otherwise the gyro's.
void motionplus_fill(int device, const PlayerState *player, int attached, uint8_t *out, uint32_t bytes) {
  MotionPlus *mp = at(device);
  uint8_t *data = mp->registers;
  int connected = data[4] & 1, gyro_turn = !get_bit(data[5], 1);
  int mode = data[MODE];
  if (!gyro_turn && (mode == MODE_NUNCHUK || mode == MODE_CLASSIC) && attached) {
    attachment_plain_state(device, player, data);
    motionplus_pass_through(mode == MODE_NUNCHUK ? ATTACHMENT_NUNCHUK : ATTACHMENT_CLASSIC, data);
    set_bit(&data[5], 1, 0);
  } else {
    motionplus_gyro_bytes(player->gyro, connected, data);
  }
  set_bit(&data[4], 0, connected);
  set_bit(&data[5], 0, 0);
  for (uint32_t i = 0; i < bytes; i++) out[i] = i < DATA_BYTES ? data[i] : 0;
}
