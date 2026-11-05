// The simulated Balance Board's insides, where a remote's attachment would be: an
// identity that says "board", calibration for its four weight sensors, and their
// readings. Layout from WiiBrew's Wii Balance Board page. Nothing here is encrypted.
//
// Each sensor reads RAW_ZERO with nothing on it and RAW_PER_KILOGRAM more for each
// kilogram, the same at the calibration's three weights, so the game's conversion
// gives back exactly what input.c says is on the board. A game reading the calibration
// itself checks it against the checksum after it, and reads it again until it matches.
#include <string.h>

#include "input.h"
#include "wii_board.h"

enum {
  REGISTERS = 256, CALIBRATION_START = 0x20, CALIBRATION = 0x24, CALIBRATION_WEIGHTS = 3, KILOGRAMS_PER_STEP = 17,
  CHECKSUM = 0x3c, CALIBRATION_END = 0x40, TEMPERATURE_REFERENCE = 0x60, REFERENCE_BATTERY = 0x69,
  RAW_ZERO = 2000, RAW_PER_KILOGRAM = 100, ROOM_TEMPERATURE = 25, BATTERY = 0x83, IDENTITY = 0xfa,
  READING_TEMPERATURE = 8, READING_BATTERY = 10,
};

static uint8_t registers[REGISTERS];
static int started;

static void put_big16(uint8_t *to, int value) {
  to[0] = (uint8_t)(value >> 8);
  to[1] = (uint8_t)value;
}

// The reversed polynomial, as zlib's.
static uint32_t crc32_of(const uint8_t *bytes, int count, uint32_t crc) {
  for (int i = 0; i < count; i++) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; bit++) crc = crc >> 1 ^ (crc & 1 ? 0xedb88320u : 0);
  }
  return crc;
}

// Calibration is each sensor at 0, 17 and 34 kg, in the readings' order, after a byte
// always 1 and the battery level counted as empty. The checksum covers the weights,
// then those two bytes, then the reference temperature and the 1 after it.
static void start(void) {
  static const uint8_t identity[] = {0, 0, 0xa4, 0x20, 0x04, 0x02};
  memcpy(registers + IDENTITY, identity, sizeof identity);
  for (int weight = 0; weight < CALIBRATION_WEIGHTS; weight++) {
    for (int sensor = 0; sensor < 4; sensor++) {
      put_big16(registers + CALIBRATION + weight * 8 + sensor * 2, RAW_ZERO + weight * KILOGRAMS_PER_STEP * RAW_PER_KILOGRAM);
    }
  }
  registers[CALIBRATION_START] = 1;
  registers[CALIBRATION_START + 1] = REFERENCE_BATTERY;
  registers[TEMPERATURE_REFERENCE] = ROOM_TEMPERATURE;
  registers[TEMPERATURE_REFERENCE + 1] = 1;
  uint32_t crc = crc32_of(registers + CALIBRATION, CHECKSUM - CALIBRATION, 0xffffffffu);
  crc = crc32_of(registers + CALIBRATION_START, 2, crc);
  crc = ~crc32_of(registers + TEMPERATURE_REFERENCE, 2, crc);
  for (int i = 0; i < 4; i++) registers[CHECKSUM + i] = (uint8_t)(crc >> (24 - 8 * i));
  started = 1;
}

void board_read(uint32_t address, uint8_t *out, uint32_t bytes) {
  if (!started) start();
  for (uint32_t i = 0; i < bytes; i++) out[i] = registers[(address + i) % REGISTERS];
}

// The game writes the usual attachment start-up bytes. Its identity, calibration and
// reference temperature stay as they are, as on the real board.
void board_write(uint32_t address, const uint8_t *data, uint32_t bytes) {
  if (!started) start();
  for (uint32_t i = 0; i < bytes; i++) {
    uint32_t at = (address + i) % REGISTERS;
    if (at < CALIBRATION_END || at >= IDENTITY || at == TEMPERATURE_REFERENCE || at == TEMPERATURE_REFERENCE + 1) continue;
    registers[at] = data[i];
  }
}

// Top right, bottom right, top left, bottom left, big-endian, then the temperature and
// the battery. Anything past those reads as all ones.
void board_reading(const double kilograms[4], uint8_t out[11]) {
  for (int sensor = 0; sensor < 4; sensor++) {
    int raw = RAW_ZERO + (int)(kilograms[sensor] * RAW_PER_KILOGRAM + 0.5);
    put_big16(out + sensor * 2, raw);
  }
  out[READING_TEMPERATURE] = ROOM_TEMPERATURE;
  out[9] = 0;
  out[READING_BATTERY] = BATTERY;
}

void board_fill(uint8_t *out, uint32_t bytes) {
  double kilograms[4];
  uint8_t reading[11];
  input_board_now(kilograms);
  board_reading(kilograms, reading);
  for (uint32_t i = 0; i < bytes; i++) out[i] = i < sizeof reading ? reading[i] : 0xff;
}
