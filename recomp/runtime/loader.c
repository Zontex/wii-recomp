// Loads the game into guest memory.
#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"
#include "loader.h"

#define SECTIONS 18
#define IOS_VERSION 0x0024000cu
#define IOS_BUILD_DATE 0x00123120u  // December 31, 2020, as 00 MM DD YY.

static uint32_t big32(const uint8_t *data) { return __builtin_bswap32(*(const uint32_t *)data); }

uint32_t load_program(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) { perror(path); exit(1); }
  fseek(file, 0, SEEK_END);
  long length = ftell(file);
  rewind(file);
  uint8_t *data = malloc((size_t)length);
  if (fread(data, 1, (size_t)length, file) != (size_t)length) exit(1);
  fclose(file);
  // The zeroed area overlaps the small data sections, so it must be cleared first.
  uint32_t bss = big32(data + 0xd8), bss_size = big32(data + 0xdc);
  memset(host(bss), 0, bss_size);
  for (int i = 0; i < SECTIONS; i++) {
    uint32_t offset = big32(data + i * 4), address = big32(data + 0x48 + i * 4), size = big32(data + 0x90 + i * 4);
    if (size) memcpy(host(address), data + offset, size);
  }
  uint32_t entry = big32(data + 0xe0);
  free(data);
  return entry;
}

// What the Wii's firmware leaves in low memory before a game starts.
#define MEM1_END 0x81800000u
#define FILE_TABLE_BLOCK 0x10000u  // The table takes whole blocks at the top of main memory, with one spare below.
#define BUS_HZ 243000000u
#define CPU_HZ 729000000u

size_t read_file(const char *folder, const char *name, uint8_t *target, size_t limit) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", folder, name);
  FILE *file = fopen(path, "rb");
  if (!file) { perror(path); exit(1); }
  size_t length = fread(target, 1, limit, file);
  fclose(file);
  return length;
}

static uint32_t file_size(const char *folder, const char *name) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", folder, name);
  FILE *file = fopen(path, "rb");
  if (!file) { perror(path); exit(1); }
  fseek(file, 0, SEEK_END);
  long length = ftell(file);
  fclose(file);
  return (uint32_t)length;
}

void set_up_low_memory(const char *folder) {
  read_file(folder, "boot.bin", host(0x80000000u), 0x20);
  uint32_t table_size = file_size(folder, "fst.bin");
  uint32_t table_address = MEM1_END - ((table_size + FILE_TABLE_BLOCK - 1) & ~(FILE_TABLE_BLOCK - 1));
  uint32_t arena_high = table_address - FILE_TABLE_BLOCK;
  read_file(folder, "fst.bin", host(table_address), table_size);
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
  wr32(0x80003140, IOS_VERSION);
  wr32(0x80003144, IOS_BUILD_DATE);
  wr32(0x80003188, IOS_VERSION);       // The version the game asked to run on.
}
