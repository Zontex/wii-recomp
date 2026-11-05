// Starts the console's titles, and hands over between them with exec, so every title
// starts as cleanly as on a real Wii: see "one program per title" in docs/native_ios.md.
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "console.h"
#include "drive.h"
#include "profile.h"
#include "remote_link.h"
#include "title.h"

#define MENU_TITLE "00000001-00000002"
#ifdef __APPLE__
#define LIBRARY_NAME "title.dylib"
#else
#define LIBRARY_NAME "title.so"
#endif

static char program_path[PATH_MAX], console_folder[PATH_MAX];

int console_running(void) { return console_folder[0] != 0; }

static void set_default(const char *name, const char *child) {
  char path[PATH_MAX * 2];
  snprintf(path, sizeof path, "%s/%s", console_folder, child);
  setenv(name, path, 0);
}

static int ends_with(const char *text, const char *end) {
  size_t length = strlen(text), end_length = strlen(end);
  return length >= end_length && !strcmp(text + length - end_length, end);
}

// The first disc image in the slot, by name, or nothing.
static void insert_from_slot(void) {
  char slot[PATH_MAX * 2], chosen[PATH_MAX * 3] = "";
  snprintf(slot, sizeof slot, "%s/drive", console_folder);
  DIR *folder = opendir(slot);
  if (!folder) {
    fprintf(stderr, "console: no drive folder at %s, so no disc is in\n", slot);
    return;
  }
  struct dirent *entry;
  char best[256] = "";
  while ((entry = readdir(folder))) {
    if (entry->d_name[0] != '.' && ends_with(entry->d_name, ".wrd") && (!best[0] || strcmp(entry->d_name, best) < 0)) {
      snprintf(best, sizeof best, "%s", entry->d_name);
    }
  }
  closedir(folder);
  if (!best[0]) return;
  snprintf(chosen, sizeof chosen, "%s/%s", slot, best);
  drive_insert(chosen);
}

void console_start(const char *program, const char *folder, int boot_disc, Boot *boot) {
  if (!realpath(program, program_path) || !realpath(folder, console_folder)) {
    perror(folder);
    exit(1);
  }
  set_default("WII_RECOMP_SAVES", "nand");
  set_default("WII_RECOMP_SHADER_CACHE", "cache/shaders");
  set_default("WII_RECOMP_CACHE", "cache/native");
  set_default("WII_RECOMP_TITLE_DATA", "cache/titles");
  set_default("WII_RECOMP_GAPS", "cache/gaps.txt");
  set_default("WII_RECOMP_REMOTE_SLOTS", "remotes.txt");
  set_default("WII_RECOMP_CONTROLLERS", "controllers.ini");
  char path[PATH_MAX * 2];
  snprintf(path, sizeof path, "%s/cache", console_folder);
  mkdir(path, 0755);
  insert_from_slot();
  if (boot_disc) {
    if (!drive_has_disc()) {
      fprintf(stderr, "stop: there's no disc in the drive to start\n");
      exit(10);
    }
    if (!drive_native(getenv("WII_RECOMP_CACHE"), path, sizeof path)) {
      fprintf(stderr, "stop: this disc has no native code for %s\n", drive_platform());
      exit(10);
    }
    title_load(path);
    boot_from_disc(boot);
    return;
  }
  char menu[PATH_MAX * 2];
  snprintf(menu, sizeof menu, "%s/titles/" MENU_TITLE, console_folder);
  snprintf(path, sizeof path, "%s/" LIBRARY_NAME, menu);
  title_load(path);
  boot_from_folder(menu, boot);
}

// Where the disc's program starts, from its header, or 0.
static uint32_t disc_entry(void) {
  uint8_t header[4], program[4];
  if (!drive_read(0x420, 4, header)) return 0;
  uint64_t at = (uint64_t)((uint32_t)header[0] << 24 | header[1] << 16 | header[2] << 8 | header[3]) << 2;
  if (!drive_read(at + 0xe0, 4, program)) return 0;
  return (uint32_t)program[0] << 24 | program[1] << 16 | program[2] << 8 | program[3];
}

void console_hand_over(uint32_t address) {
  static uint32_t entry;
  if (!console_running() || !drive_has_disc()) return;
  if (!entry) entry = disc_entry();
  if (entry && address == entry) console_restart(1);
}

void console_restart(int boot_disc) {
  char *arguments[] = {program_path, "--console", console_folder, boot_disc ? "--boot-disc" : 0, 0};
  fprintf(stderr, "console: starting %s\n", boot_disc ? "the disc" : "the Wii Menu");
  link_prepare_handover();  // Real remotes stay connected into the next title.
  profile_save();  // exec skips the exit handlers.
  fflush(stderr);
  execv(program_path, arguments);
  perror(program_path);
  exit(1);
}
