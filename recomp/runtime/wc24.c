// WiiConnect24's user ID, the "Wii Number". A Wii makes it offline on first use from
// its console number, region and model, and keeps it in nwc24msg.cfg. The algorithm
// and the file layout follow Dolphin's NetKDRequest.cpp and NWC24Config (GPL-2.0+).
#include <stdio.h>
#include <string.h>

#include "ios.h"

enum {
  CONFIG_BYTES = 0x400, CONFIG_ID = 8, CONFIG_GENERATION = 0x10, CONFIG_STAGE = 0x14, CONFIG_CHECKSUM = 0x3fc,
  STAGE_INITIAL = 0, STAGE_GENERATED = 1, STAGE_REGISTERED = 2,
  WC24_OK = 0, WC24_FATAL = -1, WC24_FILE = -16, WC24_ID_GENERATED = -35, WC24_ID_REGISTERED = -36,
  SETTINGS_BYTES = 0x100, MAX_PATH = 1024,
};

#define CONFIG_PATH "/shared2/wc24/nwc24msg.cfg"
#define BACKUP_PATH "/shared2/wc24/nwc24msg.cbk"
#define SETTINGS_PATH "/title/00000001/00000002/data/setting.txt"

static uint32_t get32(const uint8_t *data) { return (uint32_t)data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3]; }
static void put32(uint8_t *data, uint32_t value) {
  for (int i = 0; i < 4; i++) data[i] = (uint8_t)(value >> (24 - 8 * i));
}

static int read_file(const char *path, uint8_t *out, size_t bytes) {
  char full[MAX_PATH];
  if (!files_find(path, full, sizeof full)) return 0;
  FILE *file = fopen(full, "rb");
  size_t got = file ? fread(out, 1, bytes, file) : 0;
  if (file) fclose(file);
  return got == bytes;
}

static int write_file(const char *path, const uint8_t *data, size_t bytes) {
  char full[MAX_PATH], partial[MAX_PATH + 8];
  files_save_path(path, full, sizeof full);
  files_make_parents(full);
  snprintf(partial, sizeof partial, "%s.new", full);
  FILE *file = fopen(partial, "wb");
  int done = file && fwrite(data, 1, bytes, file) == bytes;
  if (file) done = !fclose(file) && done;
  return done && !rename(partial, full);  // A cut-off write never replaces the old file.
}

// A value such as AREA or MODEL from setting.txt, which is lightly scrambled.
static int setting(const char *name, char *out, size_t limit) {
  uint8_t data[SETTINGS_BYTES + 1] = {0};
  if (!read_file(SETTINGS_PATH, data, SETTINGS_BYTES)) return 0;
  uint32_t key = 0x73b5dbfa;
  for (int i = 0; i < SETTINGS_BYTES; i++) {
    data[i] ^= (uint8_t)key;
    key = key >> 31 | key << 1;
  }
  char wanted[32];
  snprintf(wanted, sizeof wanted, "%s=", name);
  for (char *line = (char *)data; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : 0) {
    if (strncmp(line, wanted, strlen(wanted))) continue;
    size_t length = strcspn(line + strlen(wanted), "\r\n");
    if (length >= limit) return 0;
    memcpy(out, line + strlen(wanted), length);
    out[length] = 0;
    return 1;
  }
  return 0;
}

static uint8_t area_code(const char *area) {
  static const struct { const char *name; uint8_t code; } areas[] = {
    {"JPN", 0}, {"USA", 1}, {"EUR", 2}, {"AUS", 2}, {"BRA", 1}, {"TWN", 3}, {"ROC", 3},
    {"KOR", 4}, {"HKG", 5}, {"ASI", 5}, {"LTN", 1}, {"SAF", 2}, {"CHN", 6},
  };
  for (size_t i = 0; i < sizeof areas / sizeof areas[0]; i++) if (!strcmp(area, areas[i].name)) return areas[i].code;
  return 7;
}

static uint8_t hardware_model(const char *model) {
  if (!strncmp(model, "RVL", 3)) return 1;
  if (!strncmp(model, "RVD", 3)) return 2;
  if (!strncmp(model, "RVT", 3) || !strncmp(model, "RVV", 3)) return 0;
  return 7;
}

static int make_user_id(uint64_t *id, uint32_t console, uint16_t counter, uint8_t model, uint8_t area) {
  static const uint8_t positions[8] = {1, 5, 0, 4, 2, 3, 6, 7};
  static const uint8_t nibbles[16] = {4, 11, 7, 9, 15, 1, 13, 3, 12, 2, 6, 14, 8, 0, 10, 5};
  uint64_t mix = (uint64_t)area << 50 | (uint64_t)model << 47 | (uint64_t)console << 15 | (uint64_t)counter << 10;
  const uint64_t original = mix;
  for (int bit = 0; bit <= 42; bit++) {
    if ((mix >> (52 - bit)) & 1) mix ^= 0x635ull << (42 - bit);
  }
  mix = (original | (mix & 0xffffffffull)) ^ 0x0000b3b3b3b3b3b3ull;
  mix = mix >> 10 | (mix & 0x3ff) << (11 + 32);
  for (int i = 0; i <= 5; i++) {
    uint8_t byte = (uint8_t)(mix >> (i * 8));
    byte = (uint8_t)(nibbles[byte >> 4] << 4 | nibbles[byte & 15]);
    mix = (mix & ~(0xffull << (i * 8))) | (uint64_t)byte << (i * 8);
  }
  const uint64_t shuffled = mix;
  for (int i = 0; i <= 5; i++) {
    uint8_t byte = (uint8_t)(shuffled >> (i * 8));
    mix = (mix & ~(0xffull << (positions[i] * 8))) | (uint64_t)byte << (positions[i] * 8);
  }
  mix &= 0x001fffffffffffffull;
  mix = mix << 1 | ((mix >> 52) & 1);
  mix ^= 0x00005e5e5e5e5e5eull;
  mix &= 0x001fffffffffffffull;
  *id = mix;
  return mix > 9999999999999999ull ? WC24_FATAL : WC24_OK;
}

// Makes the ID if there isn't one yet. Writes a result word, the ID and the stage to `out`.
void wc24_request_user_id(uint8_t *out) {
  uint8_t config[CONFIG_BYTES];
  int32_t result;
  if (!read_file(CONFIG_PATH, config, sizeof config)) {
    memset(out, 0, 16);
    put32(out, (uint32_t)WC24_FILE);
    return;
  }
  uint32_t stage = get32(config + CONFIG_STAGE);
  if (stage == STAGE_INITIAL) {
    char area[16], model[16];
    if (setting("AREA", area, sizeof area) && setting("MODEL", model, sizeof model)) {
      uint64_t id;
      uint32_t generation = get32(config + CONFIG_GENERATION);
      result = make_user_id(&id, IOS_DEVICE_ID, (uint16_t)generation, hardware_model(model), area_code(area));
      put32(config + CONFIG_ID, (uint32_t)(id >> 32));
      put32(config + CONFIG_ID + 4, (uint32_t)id);
      put32(config + CONFIG_GENERATION, generation + 1);
      put32(config + CONFIG_STAGE, STAGE_GENERATED);
      uint32_t sum = 0;
      for (int i = 0; i < CONFIG_CHECKSUM; i += 4) sum += get32(config + i);
      put32(config + CONFIG_CHECKSUM, sum);
      if (!write_file(CONFIG_PATH, config, sizeof config) || !write_file(BACKUP_PATH, config, sizeof config)) result = WC24_FILE;
      fprintf(stderr, "wc24: made this console's user ID\n");
    } else {
      result = WC24_FATAL;
    }
  } else {
    result = stage == STAGE_GENERATED ? WC24_ID_GENERATED : WC24_ID_REGISTERED;
  }
  memset(out, 0, 16);
  put32(out, (uint32_t)result);
  memcpy(out + 4, config + CONFIG_ID, 8);
  memcpy(out + 12, config + CONFIG_STAGE, 4);
}
