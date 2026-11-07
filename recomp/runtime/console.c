// Starts the console's titles, and hands over between them with exec, so every title
// starts as cleanly as on a real Wii: see "one program per title" in docs/native_ios.md.
#include <dirent.h>
#include <pthread.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "console.h"
#include "drive.h"
#include "ios.h"
#include "profile.h"
#include "remote_link.h"
#include "title.h"

#ifdef __APPLE__
#define LIBRARY_NAME "title.dylib"
#else
#define LIBRARY_NAME "title.so"
#endif

static char program_path[PATH_MAX], console_folder[PATH_MAX];
static char **started_with;
static char inserted[PATH_MAX * 3];  // The image in the drive, in a console.

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
  if (drive_insert(chosen)) snprintf(inserted, sizeof inserted, "%s", chosen);
}

// Where an installed title's program and native code are.
static void title_folder(uint64_t title, char *out, size_t size) {
  snprintf(out, size, "%s/titles/%08x-%08x", console_folder, (uint32_t)(title >> 32), (uint32_t)title);
}

void console_start(const char *program, const char *folder, uint64_t start, Boot *boot) {
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
  set_default("WII_RECOMP_CERTIFICATES", "certificates");
  char path[PATH_MAX * 2];
  snprintf(path, sizeof path, "%s/cache", console_folder);
  mkdir(path, 0755);
  insert_from_slot();
  if (start == CONSOLE_DISC) {
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
  char folder_path[PATH_MAX * 2];
  title_folder(start, folder_path, sizeof folder_path);
  snprintf(path, sizeof path, "%s/" LIBRARY_NAME, folder_path);
  if (start != CONSOLE_MENU && !console_has_title(start)) {
    fprintf(stderr, "stop: %s isn't installed with native code\n", folder_path);
    exit(10);
  }
  title_load(path);
  boot_from_folder(folder_path, boot);
}

int console_has_title(uint64_t title) {
  char folder[PATH_MAX * 2], path[PATH_MAX * 3];
  struct stat info;
  title_folder(title, folder, sizeof folder);
  snprintf(path, sizeof path, "%s/" LIBRARY_NAME, folder);
  return console_running() && !stat(path, &info);
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
  if (!console_running() || !drive_has_disc()) return;
  uint32_t entry = disc_entry();  // Read each time: the disc can change.
  if (entry && address == entry) console_restart(CONSOLE_DISC);
}

void console_restart(uint64_t start) {
  char title_argument[24];
  snprintf(title_argument, sizeof title_argument, "%08x-%08x", (uint32_t)(start >> 32), (uint32_t)start);
  char *arguments[] = {program_path, "--console", console_folder, 0, 0, 0};
  if (start == CONSOLE_DISC) arguments[3] = "--boot-disc";
  else if (start != CONSOLE_MENU) arguments[3] = "--title", arguments[4] = title_argument;
  fprintf(stderr, "console: starting %s\n", start == CONSOLE_DISC ? "the disc" : start == CONSOLE_MENU ? "the Wii Menu" : title_argument);
  files_save_all();  // What the title wrote, such as NANDBOOTINFO, is on disk for the next.
  link_prepare_handover();  // Real remotes stay connected into the next title.
  profile_save();  // exec skips the exit handlers.
  fflush(stderr);
  execv(program_path, arguments);
  perror(program_path);
  exit(1);
}

void console_remember(char **arguments) { started_with = arguments; }

void console_power_off(const char *how) {
  fprintf(stderr, "console: turning %s\n", how);
  files_save_all();
  exit(0);
}

void console_hot_reset(void) {
  if (console_running()) console_restart(CONSOLE_MENU);
  fprintf(stderr, "console: hot reset, starting again\n");
  files_save_all();
  profile_save();
  fflush(stderr);
  char path[PATH_MAX];
  execv(realpath(started_with[0], path) ? path : started_with[0], started_with);
  perror(started_with[0]);
  exit(1);
}

// Changing the disc. The control pipe asks from its own thread; the change happens on the
// game's, between its reads, in console_update_drive. In a console, the image moves
// between the drive and shelf folders, so the drive still holds it after a restart.
static pthread_mutex_t change_lock = PTHREAD_MUTEX_INITIALIZER;
static int change_wanted;           // 1 to eject, 2 to insert change_path.
static char change_path[PATH_MAX];

void console_request_disc(const char *path) {
  pthread_mutex_lock(&change_lock);
  change_wanted = path ? 2 : 1;
  snprintf(change_path, sizeof change_path, "%s", path ? path : "");
  pthread_mutex_unlock(&change_lock);
}

// Moves an image between a console's drive and shelf folders, by name.
static int move_image(const char *from_folder, const char *name, const char *to_folder, char *moved, size_t size) {
  char from[PATH_MAX * 3];
  snprintf(from, sizeof from, "%s/%s/%s", console_folder, from_folder, name);
  snprintf(moved, size, "%s/%s/%s", console_folder, to_folder, name);
  if (!strcmp(from, moved)) return 1;
  char folder[PATH_MAX * 2];
  snprintf(folder, sizeof folder, "%s/%s", console_folder, to_folder);
  mkdir(folder, 0755);
  if (rename(from, moved)) { perror(from); return 0; }
  return 1;
}

static void eject_now(void) {
  if (console_running() && inserted[0]) {
    const char *name = strrchr(inserted, '/') ? strrchr(inserted, '/') + 1 : inserted;
    char moved[PATH_MAX * 3];
    move_image("drive", name, "shelf", moved, sizeof moved);
    inserted[0] = 0;
  }
  drive_eject();
  fprintf(stderr, "console: the disc is out\n");
}

void console_eject(void) { eject_now(); }

void console_update_drive(void) {
  if (!change_wanted) return;
  pthread_mutex_lock(&change_lock);
  int wanted = change_wanted;
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s", change_path);
  change_wanted = 0;
  pthread_mutex_unlock(&change_lock);
  if (drive_has_disc()) eject_now();
  if (wanted != 2) return;
  // In a console, a bare name is an image on the shelf, which goes into the drive.
  char chosen[PATH_MAX * 3];
  if (console_running() && !strchr(path, '/')) {
    if (!move_image("shelf", path, "drive", chosen, sizeof chosen)) return;
    snprintf(inserted, sizeof inserted, "%s", chosen);
  } else {
    snprintf(chosen, sizeof chosen, "%s", path);
  }
  if (drive_insert(chosen)) fprintf(stderr, "console: %s is in the drive\n", chosen);
}
