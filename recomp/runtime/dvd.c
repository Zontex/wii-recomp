// The disc drive. Reads go to the extracted game files, found through the disc's
// own file table, so the game sees the same disc layout it was built for.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "debug.h"
#include "ios.h"

enum {
  READ_DISC_ID = 0x70, READ = 0x71, OPEN_PARTITION = 0x8b,
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

int dvd_request(uint32_t command, uint32_t request) {
  uint32_t code = rd32(request + 12);
  if (command == IOS_IOCTLV) return code == OPEN_PARTITION ? DRIVE_OK : DRIVE_OK;
  if (command != IOS_IOCTL) return DRIVE_OK;
  uint32_t input = rd32(request + 16) | 0x80000000u, output = rd32(request + 24) | 0x80000000u;
  if (code == READ_DISC_ID) return read_disc(0, DISC_ID_BYTES, output);
  if (code == READ) {
    uint64_t started = time_base();
    int result = read_disc((uint64_t)rd32(input + 8) << 2, rd32(input + 4), output);
    stage_add(STAGE_DISC, time_base() - started);
    return result;
  }
  uint32_t out_bytes = rd32(request + 28);
  if (output != 0x80000000u) memset(host(output), 0, out_bytes);
  return DRIVE_OK;
}
