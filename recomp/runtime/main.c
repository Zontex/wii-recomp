// Starts the translated game on a large stack, with the runtime around it.
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "audio.h"
#include "console.h"
#include "input.h"
#include "cpu.h"
#include "dispatch.h"
#include "drive.h"
#include "gx.h"
#include "hardware.h"
#include "ios.h"
#include "loader.h"
#include "title.h"

Cpu cpu;

#define STACK_BYTES (1024u * 1024u * 1024u)

static void print_guest_text(uint32_t address) {
  for (int i = 0; i < 400; i++) {
    uint32_t c = rd8(address + (uint32_t)i);
    if (!c) break;
    fputc((int)c, stderr);
  }
}

// The game's OSPanic: a source file, a line and a message.
void guest_panic(void) {
  fprintf(stderr, "game panic: ");
  print_guest_text(cpu.r[3]);
  fprintf(stderr, " line %u: ", cpu.r[4]);
  print_guest_text(cpu.r[5]);
  fprintf(stderr, " (called from %08x)\n", cpu.lr);
  exit(7);
}

// The game's OSFatal: colors, then a message for the player.
void guest_fatal(void) {
  fprintf(stderr, "game fatal error: ");
  print_guest_text(cpu.r[5]);
  fprintf(stderr, " (called from %08x)\n", cpu.lr);
  exit(8);
}


static Boot boot;

static void *run(void *unused) {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // A frame can't wait for the system's other work.
#endif
  (void)unused;
  fill_code_slots();
  uint32_t entry = load_program(&boot);
  set_up_low_memory(&boot);
  fprintf(stderr, "running from %08x\n", entry);
  dispatch(entry);
  fprintf(stderr, "the program returned\n");
  return 0;
}

#ifdef __APPLE__
#define LIBRARY_SUFFIX "dylib"
#else
#define LIBRARY_SUFFIX "so"
#endif

// The title's native code: WII_RECOMP_NATIVE if set, or the library built beside the program.
static void load_title_beside(const char *program) {
  char full[PATH_MAX], path[PATH_MAX + 32];
  const char *chosen = getenv("WII_RECOMP_NATIVE");
  if (!chosen) {
    if (!realpath(program, full)) { perror(program); exit(1); }
    snprintf(path, sizeof path, "%s/title." LIBRARY_SUFFIX, dirname(full));
    chosen = path;
  }
  title_load(chosen);
}

// A disc image carries its own native code, which is written out once to the cache.
static void load_title_from_disc(void) {
  char path[PATH_MAX];
  if (!drive_native(getenv("WII_RECOMP_CACHE"), path, sizeof path)) {
    fprintf(stderr, "stop: this disc has no native code for %s\n", drive_platform());
    exit(10);
  }
  title_load(path);
}

// Saves, compiled shaders and the native code cache live next to the program, so they
// don't depend on where it was started from.
static void use_folders_beside(const char *program) {
  char full[PATH_MAX], path[PATH_MAX + 16];
  if (!realpath(program, full)) return;
  const char *folder = dirname(full);  // Linux's changes `full`, so it's called once.
  const char *names[][2] = {{"WII_RECOMP_SAVES", "saves"}, {"WII_RECOMP_SHADER_CACHE", "shader-cache"},
                            {"WII_RECOMP_DISCOVERED", "discovered.txt"}, {"WII_RECOMP_CACHE", "native-cache"},
                            {"WII_RECOMP_GAPS", "gaps.txt"}, {"WII_RECOMP_REMOTE_SLOTS", "remotes.txt"}};
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    snprintf(path, sizeof path, "%s/%s", folder, names[i][1]);
    setenv(names[i][0], path, 0);
  }
}

static int is_folder(const char *path) {
  struct stat info;
  if (stat(path, &info)) { perror(path); exit(1); }
  return S_ISDIR(info.st_mode);
}

int main(int count, char **arguments) {
  // On the Mac, submitting can wait up to a second for the window to free a picture.
  // MoltenVK does that on its own thread with this off, so the game never stalls on it.
  // It reads this as it loads, which is before any window exists, so it's set first.
  setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", 0);
  // Extra queue families, each its own Metal queue, so the window can have one that the
  // window server's waits don't hold the game's frames back on. See gx_vulkan_window.c.
  setenv("MVK_CONFIG_SPECIALIZED_QUEUE_FAMILIES", "1", 0);
  if (count < 2 || (!strcmp(arguments[1], "--console") && count < 3)) {
    fprintf(stderr, "usage: wii-recomp <disc image, or folder holding main.dol, boot.bin and fst.bin> [Wii storage folder]\n"
                    "       wii-recomp --console <console folder> [--boot-disc]\n");
    return 1;
  }
  if (!strcmp(arguments[1], "--console")) {
    console_start(arguments[0], arguments[2], count > 3 && !strcmp(arguments[3], "--boot-disc"), &boot);
    count = 1;  // The console's storage is its own, never a separate folder.
  } else if (!drive_insert(arguments[1])) {
    return 1;
  } else if (is_folder(arguments[1])) {  // An extracted disc or a system title, with its native code built beside the console.
    load_title_beside(arguments[0]);
    boot_from_folder(arguments[1], &boot);
  } else {
    load_title_from_disc();
    boot_from_disc(&boot);
  }
  use_folders_beside(arguments[0]);
  guest_memory_init();
  time_start();
  hardware_start();
  audio_start();
  files_start(count > 2 ? arguments[2] : 0);
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, STACK_BYTES);
  window_create();
  if (getenv("WII_RECOMP_CONTROL")) input_start_control_pipe(getenv("WII_RECOMP_CONTROL"));
  pthread_t thread;
  pthread_create(&thread, &attributes, run, 0);
  window_run();  // The Mac needs the window on this thread.
  return 0;
}
