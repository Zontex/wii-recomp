// Loads the game into guest memory.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "drive.h"
#include "hardware.h"
#include "ios.h"
#include "title.h"
#include "loader.h"
#include "titles.h"

#define SECTIONS 18
#define IOS_BUILD_DATE 0x00123120u  // December 31, 2020, as 00 MM DD YY.

static uint32_t big32(const uint8_t *data) { return __builtin_bswap32(*(const uint32_t *)data); }

int gamecube_mode;

// A GameCube disc's ID says so at 0x1c. Its header and file table give offsets and sizes
// in bytes, where a Wii disc's give 4-byte words.
static int set_mode_from(const uint8_t *header) {
  gamecube_mode = big32(header + 0x1c) == 0xc2339f3du;
  if (gamecube_mode) fprintf(stderr, "console: a GameCube disc, so the console runs in GameCube mode\n");
  return gamecube_mode ? 0 : 2;
}

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
  set_mode_from(boot->header);
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
  int shift = set_mode_from(boot->header);
  uint64_t program_at = (uint64_t)big32(boot->header + 0x420) << shift, table_at = (uint64_t)big32(boot->header + 0x424) << shift;
  uint8_t header[0x100];
  if (!drive_read(program_at, sizeof header, header)) {
    fprintf(stderr, "stop: the disc's program can't be read\n");
    exit(1);
  }
  boot->program_bytes = program_bytes(header);
  boot->program = read_disc(program_at, boot->program_bytes, "program");
  boot->table_bytes = (size_t)big32(boot->header + 0x428) << shift;
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
// The top of the arena as the system menu leaves it, which the apploader puts the disc's
// file table just below, 32-byte aligned, and lowers the arena to (Dolphin's SetupWiiMemory).
// The table used to take whole 64 KB blocks with one spare below, which cost Super Smash
// Bros. Brawl 89 KB: its first fight then couldn't make a 0.6 MB heap and stopped.
#define ARENA_HIGH_BEFORE_TABLE 0x817fec60u
#define BUS_HZ 243000000u
#define CPU_HZ 729000000u
// In GameCube mode the Wii runs at a GameCube's speeds, and has its 16 MB of audio memory.
#define GAMECUBE_BUS_HZ 162000000u
#define GAMECUBE_CPU_HZ 486000000u
#define GAMECUBE_ARAM_SIZE 0x01000000u
// What a GameCube game is told it runs on: Dolphin's choice, a development kit, as some
// games take other paths on a retail ID that Dolphin hasn't followed (its SetupGCMemory).
#define GAMECUBE_CONSOLE_TYPE 0x10000006u

// The IOS the title asks for, with the revision installed in storage, as 00 VV RR RR.
static uint32_t ios_version(void) {
  static uint8_t tmd[TMD_MAX_BYTES];
  long size = title_tmd(1ull << 32 | title->ios_version, tmd, sizeof tmd);
  uint32_t revision = size >= 0 ? tmd_title_version(tmd) : 0;
  if (size < 0) fprintf(stderr, "IOS %u isn't in storage, so its revision is given as 0\n", title->ios_version);
  return title->ios_version << 16 | revision;
}

// Where each IOS keeps its own memory at the top of MEM2, which is where the game's
// arena ends. From Dolphin's VersionInfo.cpp: up to IOS 22 it starts at 0x93400000,
// IOS 28 at 0x93800000, and from IOS 30 on at 0x93600000. Games size their heaps to the
// arena, so a top set too low leaves a game short: Wii Fit Plus (IOS 53) is 2 MB short
// and can't fit its main menu.
static uint32_t ios_mem2_end(uint32_t ios) {
  if (ios <= 22) return 0x93400000u;
  if (ios == 28) return 0x93800000u;
  return 0x93600000u;
}

static void set_up_video(const Boot *boot) {
  // The TV standard, which the system menu leaves here from the console's settings and
  // the SDK's VIInit and VIConfigure hold a game to: 0 is NTSC, 1 PAL. A console's own
  // setting.txt says which (VIDEO); without one it follows the disc's region letter, as
  // Dolphin's boot does. Left at NTSC, VIConfigure stops a PAL game that asks for a PAL or
  // EuRGB60 mode. Like Dolphin's boot, this writes only 0 or 1: a PAL console set to 60 Hz
  // says so with IPL.E60 in its settings, which is where the SDK looks for EuRGB60, and
  // the video registers keep the NTSC timing such a console runs at.
  char video[8];
  uint8_t sixty = 1;
  int pal = !strchr("EJKW", (char)boot->header[3]);
  if (console_setting("VIDEO", video, sizeof video)) pal = !strcmp(video, "PAL");
  files_system_setting("IPL.E60", &sixty, 1);
  if (pal && !sixty) fprintf(stderr, "vi: a PAL console at 50 Hz isn't modelled; EuRGB60 is given\n");
  wr32(0x800000cc, pal ? 1 : 0);  // PAL, as Dolphin's boot fakes the IPL's VIInit; 60 Hz comes from IPL.E60.
  // The system menu also leaves the video interface running. VIInit, finding it off,
  // starts it as NTSC and writes 0 over the word above, so for a PAL game it's left as the
  // menu's own VIInit would have it: switched on (the control register's low bit), and
  // the two display interrupts enabled, one a field: the first at line 263, sample 430,
  // the second at line 1, sample 1, which is what the SDK sets for 525-line timing and
  // what Dolphin's VideoInterface presets. A game that waits for a retrace before it
  // configures the picture needs them, and VIInit doesn't set them itself when it finds
  // the interface on: with only the first, every game ran at half its frame rate.
  if (pal) {
    hw_write(0xcc002002u, 1, 2);
    hw_write(0xcc002030u, (1u << 28) | (263u << 16) | 430u, 4);
    hw_write(0xcc002034u, (1u << 28) | (1u << 16) | 1u, 4);
  }
}

// What a GameCube's boot program and the disc's apploader leave, as Dolphin's GameCube
// boot does (Boot_BS2Emu.cpp, SetupGCMemory and the apploader's run): the disc ID, the
// file table at the top of main memory with the arena below it, the memory sizes and the
// GameCube's clocks. Nothing of the Wii's: no IOS, no MEM2, no settings.
static void set_up_gamecube_memory(const Boot *boot) {
  memcpy(host(0x80000000u), boot->header, 0x20);
  uint32_t table_size = (uint32_t)boot->table_bytes, table_address = (MEM1_END - table_size) & ~31u;
  memcpy(host(table_address), boot->table, table_size);
  wr32(0x80000020, 0x0d15ea5e);   // Normal boot.
  wr32(0x80000024, 1);
  wr32(0x80000028, MEM1_SIZE);
  wr32(0x8000002c, GAMECUBE_CONSOLE_TYPE);
  set_up_video(boot);
  wr32(0x80000030, 0);
  wr32(0x80000034, table_address);
  wr32(0x80000038, table_address);
  wr32(0x8000003c, table_size);
  wr32(0x800000d0, GAMECUBE_ARAM_SIZE);
  wr32(0x800000f0, MEM1_SIZE);
  wr32(0x800000f8, GAMECUBE_BUS_HZ);
  wr32(0x800000fc, GAMECUBE_CPU_HZ);
}

void set_up_low_memory(const Boot *boot) {
  if (gamecube_mode) {
    set_up_gamecube_memory(boot);
    return;
  }
  memcpy(host(0x80000000u), boot->header, 0x20);
  uint32_t table_size = (uint32_t)boot->table_bytes;
  uint32_t table_address = (ARENA_HIGH_BEFORE_TABLE - table_size) & ~31u;
  uint32_t arena_high = table_address;
  memcpy(host(table_address), boot->table, table_size);
  wr32(0x80000018, 0x5d1c9ea3);   // Wii disc.
  wr32(0x80000020, 0x0d15ea5e);   // Normal boot.
  wr32(0x80000024, 1);
  wr32(0x80000028, MEM1_SIZE);
  wr32(0x8000002c, 0x23);         // Retail console.
  uint8_t settings[0x100];
  if (console_settings_raw(settings)) memcpy(host(0x80003800u), settings, sizeof settings);  // As the system menu leaves it.
  set_up_video(boot);
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
  // The IOS's IPC buffer is the 128 KB below its memory, and the arena stops there.
  uint32_t mem2_end = ios_mem2_end(title->ios_version), ipc_buffer = mem2_end - 0x20000u;
  wr32(0x80003120, mem2_end);
  wr32(0x80003124, 0x90000800u);
  wr32(0x80003128, ipc_buffer);        // End of the MEM2 arena.
  wr32(0x80003130, ipc_buffer);
  wr32(0x80003134, mem2_end);
  wr32(0x80003138, 0x11);         // Graphics chip revision.
  wr32(0x80003140, ios_version());
  wr32(0x80003144, IOS_BUILD_DATE);
  // From IOS 28 on, the 128 KB above it is the IOS's own too.
  if (title->ios_version >= 28) {
    wr32(0x80003148, mem2_end);
    wr32(0x8000314c, mem2_end + 0x20000u);
  }
  wr32(0x80003188, ios_version());       // The version the game asked to run on.
  // The game's code, the disc ID's first four letters, which the apploader leaves here
  // (Dolphin's Boot_BS2Emu.cpp copies it from 0x80000000). Mario Party 9 reads its region
  // from it and stops with "Region Error!" on zeros.
  memcpy(host(0x80003180u), boot->header, 4);
}
