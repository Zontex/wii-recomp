// Loads the game into guest memory.
#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"
#include "drive.h"
#include "title.h"
#include "loader.h"
#include "titles.h"

#define SECTIONS 18
#define IOS_BUILD_DATE 0x00123120u  // December 31, 2020, as 00 MM DD YY.

static uint32_t big32(const uint8_t *data) { return __builtin_bswap32(*(const uint32_t *)data); }

// A program file's size, from its section table: the furthest any section reaches.
static size_t program_bytes(const uint8_t *header) {
  size_t end = 0x100;
  for (int i = 0; i < SECTIONS; i++) {
    size_t reach = (size_t)big32(header + i * 4) + big32(header + 0x90 + i * 4);
    if (big32(header + 0x90 + i * 4) && reach > end) end = reach;
  }
  return end;
}

static uint8_t *read_whole(const char *folder, const char *name, size_t *length) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", folder, name);
  FILE *file = fopen(path, "rb");
  if (!file) { perror(path); exit(1); }
  fseek(file, 0, SEEK_END);
  *length = (size_t)ftell(file);
  rewind(file);
  uint8_t *data = malloc(*length ? *length : 1);
  if (fread(data, 1, *length, file) != *length) { perror(path); exit(1); }
  fclose(file);
  return data;
}

void boot_from_folder(const char *folder, Boot *boot) {
  size_t length;
  uint8_t *header = read_whole(folder, "boot.bin", &length);
  memcpy(boot->header, header, length < sizeof boot->header ? length : sizeof boot->header);
  free(header);
  boot->program = read_whole(folder, "main.dol", &boot->program_bytes);
  boot->table = read_whole(folder, "fst.bin", &boot->table_bytes);
}

static uint8_t *read_disc(uint64_t offset, size_t bytes, const char *what) {
  uint8_t *data = malloc(bytes ? bytes : 1);
  if (!drive_read(offset, (uint32_t)bytes, data)) {
    fprintf(stderr, "stop: the disc's %s can't be read\n", what);
    exit(1);
  }
  return data;
}

void boot_from_disc(Boot *boot) {
  if (!drive_read(0, sizeof boot->header, boot->header)) {
    fprintf(stderr, "stop: the disc's header can't be read\n");
    exit(1);
  }
  uint64_t program_at = (uint64_t)big32(boot->header + 0x420) << 2, table_at = (uint64_t)big32(boot->header + 0x424) << 2;
  uint8_t header[0x100];
  if (!drive_read(program_at, sizeof header, header)) {
    fprintf(stderr, "stop: the disc's program can't be read\n");
    exit(1);
  }
  boot->program_bytes = program_bytes(header);
  boot->program = read_disc(program_at, boot->program_bytes, "program");
  boot->table_bytes = (size_t)big32(boot->header + 0x428) << 2;
  boot->table = read_disc(table_at, boot->table_bytes, "file table");
}

uint32_t load_program(const Boot *boot) {
  const uint8_t *data = boot->program;
  size_t length = program_bytes(data);  // Without any padding after it, which a disc may not keep.
  if (length > boot->program_bytes) {
    fprintf(stderr, "stop: the program is cut short\n");
    exit(1);
  }
  title_verify_program(data, (unsigned long)length);
  // The zeroed area overlaps the small data sections, so it must be cleared first.
  uint32_t bss = big32(data + 0xd8), bss_size = big32(data + 0xdc);
  memset(host(bss), 0, bss_size);
  for (int i = 0; i < SECTIONS; i++) {
    uint32_t offset = big32(data + i * 4), address = big32(data + 0x48 + i * 4), size = big32(data + 0x90 + i * 4);
    if (size) memcpy(host(address), data + offset, size);
  }
  return big32(data + 0xe0);
}

// What the Wii's firmware leaves in low memory before a game starts.
#define MEM1_END 0x81800000u
#define FILE_TABLE_BLOCK 0x10000u  // The table takes whole blocks at the top of main memory, with one spare below.
#define BUS_HZ 243000000u
#define CPU_HZ 729000000u

// The IOS the title asks for, with the revision installed in storage, as 00 VV RR RR.
static uint32_t ios_version(void) {
  static uint8_t tmd[TMD_MAX_BYTES];
  long size = title_tmd(1ull << 32 | title->ios_version, tmd, sizeof tmd);
  uint32_t revision = size >= 0 ? tmd_title_version(tmd) : 0;
  if (size < 0) fprintf(stderr, "IOS %u isn't in storage, so its revision is given as 0\n", title->ios_version);
  return title->ios_version << 16 | revision;
}

void set_up_low_memory(const Boot *boot) {
  memcpy(host(0x80000000u), boot->header, 0x20);
  uint32_t table_size = (uint32_t)boot->table_bytes;
  uint32_t table_address = MEM1_END - ((table_size + FILE_TABLE_BLOCK - 1) & ~(FILE_TABLE_BLOCK - 1));
  uint32_t arena_high = table_address - FILE_TABLE_BLOCK;
  memcpy(host(table_address), boot->table, table_size);
  wr32(0x80000018, 0x5d1c9ea3);   // Wii disc.
  wr32(0x80000020, 0x0d15ea5e);   // Normal boot.
  wr32(0x80000024, 1);
  wr32(0x80000028, MEM1_SIZE);
  wr32(0x8000002c, 0x23);         // Retail console.
  wr32(0x80000030, 0);
  wr32(0x80000034, arena_high);
  wr32(0x80000038, table_address);
  wr32(0x8000003c, table_size);
  wr32(0x800000f0, MEM1_SIZE);
  wr32(0x800000f8, BUS_HZ);
  wr32(0x800000fc, CPU_HZ);
  wr32(0x80003100, MEM1_SIZE);
  wr32(0x80003104, MEM1_SIZE);
  wr32(0x80003108, MEM1_END);       // End of main memory.
  wr32(0x8000310c, 0);                 // Pool start: 0 means straight after the game.
  wr32(0x80003110, arena_high);        // The OS's real memory limit, below the file table.
  wr32(0x80003118, MEM2_SIZE);
  wr32(0x8000311c, MEM2_SIZE);
  wr32(0x80003120, 0x93400000u);
  wr32(0x80003124, 0x90000800u);
  wr32(0x80003128, 0x933e0000u);
  wr32(0x80003130, 0x933e0000u);
  wr32(0x80003134, 0x93400000u);
  wr32(0x80003138, 0x11);         // Graphics chip revision.
  wr32(0x80003140, ios_version());
  wr32(0x80003144, IOS_BUILD_DATE);
  wr32(0x80003188, ios_version());       // The version the game asked to run on.
}
