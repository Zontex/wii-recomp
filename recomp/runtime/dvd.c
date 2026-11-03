// The disc drive. Reads go to the extracted game files, found through the disc's
// own file table, so the game sees the same disc layout it was built for.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "debug.h"
#include "ios.h"

enum {
  INQUIRY = 0x12, READ_DISC_ID = 0x70, READ = 0x71, GET_COVER_REGISTER = 0x7a, CLEAR_COVER_INTERRUPT = 0x86, RESET = 0x8a, OPEN_PARTITION = 0x8b,
  GET_STATUS_REGISTER = 0x95, REQUEST_ERROR = 0xe0, STOP_MOTOR = 0xe3,
  COVER_OPEN = 1, NO_MEDIUM = 0x01023a00,  // The drive's error for "no disc".
  DRIVE_OK = 1, DRIVE_ERROR = 2,
  ENTRY_BYTES = 12, NAME_LIMIT = 512, MAX_FILES = 8192, DISC_ID_BYTES = 0x20,
  BOOT_DOL_OFFSET = 0x420, BOOT_FST_OFFSET = 0x424,
};

typedef struct {
  uint64_t offset;
  uint32_t size;
  char path[NAME_LIMIT];
} DiscFile;

static DiscFile files[MAX_FILES];
static int file_count;
static const char *root;
static int reads;
static int has_disc;  // A disc with a game ID. The Wii Menu's own folder has none.

static uint32_t big(const uint8_t *data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

static uint8_t *load(const char *path, size_t *length) {
  FILE *file = fopen(path, "rb");
  if (!file) { perror(path); exit(1); }
  fseek(file, 0, SEEK_END);
  *length = (size_t)ftell(file);
  rewind(file);
  uint8_t *data = malloc(*length);
  if (fread(data, 1, *length, file) != *length) exit(1);
  fclose(file);
  return data;
}

static void add_file(uint64_t offset, uint32_t size, const char *path) {
  if (file_count == MAX_FILES) return;
  DiscFile *file = &files[file_count++];
  file->offset = offset;
  file->size = size;
  snprintf(file->path, sizeof file->path, "%s", path);
}

// The table lists entries in order; a directory says where it ends.
static int walk(const uint8_t *table, const char *names, int index, int end, const char *folder) {
  while (index < end) {
    const uint8_t *entry = table + (size_t)index * ENTRY_BYTES;
    const char *name = names + (big(entry) & 0xffffff);
    char path[NAME_LIMIT];
    snprintf(path, sizeof path, "%s/%s", folder, name);
    if (entry[0]) {
      index = walk(table, names, index + 1, (int)big(entry + 8), path);
    } else {
      add_file((uint64_t)big(entry + 4) << 2, big(entry + 8), path);
      index++;
    }
  }
  return index;
}

void dvd_start(const char *extracted_system_folder) {
  root = extracted_system_folder;
  char path[NAME_LIMIT], folder[NAME_LIMIT];
  size_t boot_length, table_length, length;
  snprintf(path, sizeof path, "%s/boot.bin", root);
  uint8_t *boot = load(path, &boot_length);
  snprintf(path, sizeof path, "%s/fst.bin", root);
  uint8_t *table = load(path, &table_length);
  has_disc = boot_length >= 4 && big(boot) != 0;
  add_file(0, (uint32_t)boot_length, path);
  snprintf(files[0].path, sizeof files[0].path, "%s/boot.bin", root);
  snprintf(path, sizeof path, "%s/main.dol", root);
  uint8_t *program = load(path, &length);
  free(program);
  add_file((uint64_t)big(boot + BOOT_DOL_OFFSET) << 2, (uint32_t)length, path);
  snprintf(path, sizeof path, "%s/fst.bin", root);
  add_file((uint64_t)big(boot + BOOT_FST_OFFSET) << 2, (uint32_t)table_length, path);
  int total = (int)big(table + 8);
  snprintf(folder, sizeof folder, "%s/../files", root);
  walk(table, (const char *)table + (size_t)total * ENTRY_BYTES, 1, total, folder);
  fprintf(stderr, "disc: %d files\n", file_count);
  free(boot);
  free(table);
}

static DiscFile *file_at(uint64_t offset) {
  for (int i = 0; i < file_count; i++) {
    if (offset >= files[i].offset && offset < files[i].offset + files[i].size) return &files[i];
  }
  return 0;
}

// How much of a read falls in padding before the next file.
static uint32_t padding_bytes(uint64_t offset, uint32_t bytes) {
  uint64_t next = offset + bytes;
  for (int i = 0; i < file_count; i++) {
    if (files[i].offset > offset && files[i].offset < next) next = files[i].offset;
  }
  return (uint32_t)(next - offset);
}

static int read_disc(uint64_t offset, uint32_t bytes, uint32_t target) {
  while (bytes) {
    DiscFile *entry = file_at(offset);
    if (!entry) {
      // Reads are rounded up, so they can run into the padding between files.
      uint32_t gap = padding_bytes(offset, bytes);
      memset(host(target), 0, gap);
      offset += gap;
      target += gap;
      bytes -= gap;
      continue;
    }
    uint64_t inside = offset - entry->offset;
    uint32_t part = (uint32_t)(entry->size - inside < bytes ? entry->size - inside : bytes);
    if (reads++ < 40) fprintf(stderr, "disc read %s +%llx (%u bytes)\n", strrchr(entry->path, '/') + 1, (unsigned long long)inside, part);
    FILE *file = fopen(entry->path, "rb");
    if (!file) { perror(entry->path); return DRIVE_ERROR; }
    fseek(file, (long)inside, SEEK_SET);
    size_t got = fread(host(target), 1, part, file);
    fclose(file);
    if (got != part) return DRIVE_ERROR;
    offset += part;
    target += part;
    bytes -= part;
  }
  return DRIVE_OK;
}

int dvd_ticket(uint8_t *out, unsigned long limit) {
  char path[NAME_LIMIT];
  snprintf(path, sizeof path, "%s/../ticket.bin", root);
  FILE *file = root ? fopen(path, "rb") : 0;
  if (!file) return 0;
  size_t got = fread(out, 1, limit, file);
  fclose(file);
  return got == limit;
}

int dvd_request(uint32_t command, uint32_t request) {
  uint32_t code = rd32(request + 12);
  // The partition is already open: reads go straight to its files.
  if (command == IOS_IOCTLV) return code == OPEN_PARTITION ? DRIVE_OK : IOS_UNHANDLED;
  if (command != IOS_IOCTL) return IOS_UNHANDLED;
  uint32_t input = rd32(request + 16) | 0x80000000u, output = rd32(request + 24) | 0x80000000u;
  uint32_t out_bytes = rd32(request + 28);
  uint32_t word_out = output != 0x80000000u && out_bytes >= 4 ? output : 0;
  switch (code) {
    case RESET: case STOP_MOTOR: return DRIVE_OK;
    case GET_STATUS_REGISTER: if (word_out) wr32(word_out, 0); return DRIVE_OK;
    case REQUEST_ERROR: if (word_out) wr32(word_out, has_disc ? 0 : NO_MEDIUM); return DRIVE_OK;
    case GET_COVER_REGISTER: if (word_out) wr32(word_out, has_disc ? 0 : COVER_OPEN); return DRIVE_OK;
  }
  if (!has_disc && (code == READ_DISC_ID || code == READ)) return DRIVE_ERROR;
  if (code == READ_DISC_ID) return read_disc(0, DISC_ID_BYTES, output);
  if (code == READ) {
    uint64_t started = time_base();
    int result = read_disc((uint64_t)rd32(input + 8) << 2, rd32(input + 4), output);
    stage_add(STAGE_DISC, time_base() - started);
    return result;
  }
  if (code == CLEAR_COVER_INTERRUPT) return DRIVE_OK;  // Discs never change, so it never interrupts.
  if (code == INQUIRY) {  // The drive's model and date. Programs only check the call works.
    if (output != 0x80000000u) memset(host(output), 0, out_bytes);
    return DRIVE_OK;
  }
  return IOS_UNHANDLED;
}
