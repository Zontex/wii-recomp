// The disc drive, as programs see it through /dev/di. What's in it is drive.c's job.
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "debug.h"
#include "drive.h"
#include "gaps.h"
#include "ios.h"
#include "ios_vectors.h"

enum {
  INQUIRY = 0x12, READ_DISC_ID = 0x70, READ = 0x71, GET_COVER_REGISTER = 0x7a, CLEAR_COVER_INTERRUPT = 0x86, GET_COVER_STATUS = 0x88, RESET = 0x8a, OPEN_PARTITION = 0x8b, CLOSE_PARTITION = 0x8c, UNENCRYPTED_READ = 0x8d,
  GET_STATUS_REGISTER = 0x95, REQUEST_ERROR = 0xe0, STOP_MOTOR = 0xe3, EJECT_AT = 7, EJECT = 1,
  COVER_OPEN = 1, NO_MEDIUM = 0x01023a00,  // The drive's error for "no disc".
  DRIVE_OK = 1, DRIVE_ERROR = 2, SECURITY_ERROR = 32,
  COVER_STATUS_EMPTY = 1, COVER_STATUS_DISC = 2,  // GetCoverStatus: no disc, or a disc in (Dolphin's DI.cpp).
  DISC_ID_BYTES = 0x20, TMD_LIMIT = 0x49e4, TMD_REGION_LETTER = 0x193, ALL_REGIONS = 'A',
  // The region area: the region, then an age from each of 16 rating boards, 0x80 if unrated.
  REGION_AT = 0x4e000, REGION_BYTES = 0x20, RATINGS_AT = 0x10, RATING_BOARDS = 16, UNRATED = 0x80,
};

static int read_disc(uint64_t offset, uint32_t bytes, uint32_t target) {
  if (!bytes) return DRIVE_OK;
  uint8_t *out = host(target), *last = host(target + bytes - 1);
  if (!out || last != out + bytes - 1) return DRIVE_ERROR;  // Outside memory, or across its end.
  return drive_read(offset, bytes, out) ? DRIVE_OK : DRIVE_ERROR;
}

// Reads go to the data partition, the only one a disc image holds, so opening it only
// hands back its TMD, and an ES result of 0 in the second output. For a region-free
// console, the TMD names the game as made for all regions, as discs ending in A are: the
// Wii Menu checks that letter against its own region. The game itself still sees its own
// ID, from its disc header and from ES.
static int open_partition(uint32_t request) {
  if (!drive_has_disc()) return DRIVE_ERROR;
  IosVectors vectors = ios_vectors(request);
  uint32_t tmd = ios_out(&vectors, 0), tmd_bytes = ios_out_bytes(&vectors, 0), result = ios_out_sized(&vectors, 1, 4);
  if (tmd && tmd_bytes) {
    uint8_t *out = host(tmd), *last = host(tmd + tmd_bytes - 1);
    if (!out || last != out + tmd_bytes - 1) return DRIVE_ERROR;
    memset(out, 0, tmd_bytes);
    if (!drive_tmd(out, tmd_bytes < TMD_LIMIT ? tmd_bytes : TMD_LIMIT)) return DRIVE_ERROR;
    if (tmd_bytes > TMD_REGION_LETTER) out[TMD_REGION_LETTER] = ALL_REGIONS;
  }
  if (result) wr32(result, 0);
  return DRIVE_OK;
}

// The console's region, as a disc's region area gives it, from its GAME setting.
static int console_region(uint32_t *region) {
  static const struct { const char *name; uint32_t code; } regions[] = {{"JP", 0}, {"US", 1}, {"EU", 2}, {"KR", 4}};
  char game[8];
  if (!console_setting("GAME", game, sizeof game)) return 0;
  for (size_t i = 0; i < sizeof regions / sizeof *regions; i++) {
    if (!strcmp(game, regions[i].name)) { *region = regions[i].code; return 1; }
  }
  return 0;
}

// The console is region free: whatever region a disc was made for, the Wii Menu reads
// it as made for this console. Its region area says this console's region, and every
// rating board that didn't rate the disc gets the youngest age one that did, as the menu
// refuses a disc no board of its region has rated. This is the only place regions are
// decided, and the disc itself is never changed.
static void make_region_free(uint8_t *area) {
  uint32_t region;
  if (console_region(&region)) area[0] = (uint8_t)(region >> 24), area[1] = (uint8_t)(region >> 16), area[2] = (uint8_t)(region >> 8), area[3] = (uint8_t)region;
  uint8_t youngest = UNRATED;
  for (int i = 0; i < RATING_BOARDS; i++) {
    uint8_t age = area[RATINGS_AT + i];
    if (!(age & UNRATED) && (youngest & UNRATED || age < youngest)) youngest = age;
  }
  if (youngest & UNRATED) youngest = 0;  // Rated nowhere: suitable for everyone, as on an unrated disc.
  for (int i = 0; i < RATING_BOARDS; i++) {
    if (area[RATINGS_AT + i] & UNRATED) area[RATINGS_AT + i] = youngest;
  }
}

static int read_unencrypted(uint64_t offset, uint32_t bytes, uint32_t target) {
  if (!bytes) return DRIVE_OK;
  uint8_t *out = host(target), *last = host(target + bytes - 1);
  if (!out || last != out + bytes - 1) return DRIVE_ERROR;
  if (!drive_read_raw(offset, bytes, out)) {
    fprintf(stderr, "drive: unencrypted read of %u bytes at %llx is outside the disc's start\n", bytes, (unsigned long long)offset);
    return DRIVE_ERROR;
  }
  if (offset < REGION_AT + REGION_BYTES && offset + bytes > REGION_AT) {
    uint8_t area[REGION_BYTES];
    drive_read_raw(REGION_AT, REGION_BYTES, area);
    make_region_free(area);
    uint64_t from = offset > REGION_AT ? offset : REGION_AT, to = offset + bytes < REGION_AT + REGION_BYTES ? offset + bytes : REGION_AT + REGION_BYTES;
    memcpy(out + (from - offset), area + (from - REGION_AT), (size_t)(to - from));
  }
  return DRIVE_OK;
}

int dvd_request(uint32_t command, uint32_t request) {
  uint32_t code = rd32(request + 12);
  if (command == IOS_IOCTLV) return code == OPEN_PARTITION ? open_partition(request) : IOS_UNHANDLED;
  if (command != IOS_IOCTL) return IOS_UNHANDLED;
  uint32_t input = rd32(request + 16) | 0x80000000u, output = rd32(request + 24) | 0x80000000u;
  uint32_t out_bytes = rd32(request + 28);
  uint32_t word_out = output != 0x80000000u && out_bytes >= 4 ? output : 0;
  switch (code) {
    case STOP_MOTOR:
      // Throwing the disc out, so another can go in. The disc here never changes.
      if (input != 0x80000000u && rd8(input + EJECT_AT) & EJECT) gap_hit("hw.second_disc_layer_disc_swap", code);
      return DRIVE_OK;
    case RESET: return DRIVE_OK;
    case GET_STATUS_REGISTER: if (word_out) wr32(word_out, 0); return DRIVE_OK;
    case REQUEST_ERROR: if (word_out) wr32(word_out, drive_has_disc() ? 0 : NO_MEDIUM); return DRIVE_OK;
    case GET_COVER_REGISTER: if (word_out) wr32(word_out, drive_has_disc() ? 0 : COVER_OPEN); return DRIVE_OK;
    // Too small a buffer for the answer is a security error, as IOS gives.
    case GET_COVER_STATUS:
      if (!word_out) return SECURITY_ERROR;
      wr32(word_out, drive_has_disc() ? COVER_STATUS_DISC : COVER_STATUS_EMPTY);
      return DRIVE_OK;
  }
  if (!drive_has_disc() && (code == READ_DISC_ID || code == READ || code == UNENCRYPTED_READ)) return DRIVE_ERROR;
  if (code == READ_DISC_ID) return read_disc(0, DISC_ID_BYTES, output);
  if (code == UNENCRYPTED_READ) return read_unencrypted((uint64_t)rd32(input + 8) << 2, rd32(input + 4), output);
  if (code == READ) {
    uint64_t started = time_base();
    int result = read_disc((uint64_t)rd32(input + 8) << 2, rd32(input + 4), output);
    stage_add(STAGE_DISC, time_base() - started);
    return result;
  }
  if (code == CLOSE_PARTITION) return DRIVE_OK;  // Opening it changes nothing, so neither does closing.
  if (code == CLEAR_COVER_INTERRUPT) return DRIVE_OK;  // Discs never change, so it never interrupts.
  if (code == INQUIRY) {  // The drive's model and date. Programs only check the call works.
    if (output != 0x80000000u) memset(host(output), 0, out_bytes);
    return DRIVE_OK;
  }
  return IOS_UNHANDLED;
}
