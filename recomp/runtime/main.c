// Starts the translated game on a large stack, with the runtime around it.
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <stdio.h>
#include <stdlib.h>

#include "audio.h"
#include "input.h"
#include "cpu.h"
#include "dispatch.h"
#include "gx.h"
#include "hardware.h"
#include "ios.h"
#include "loader.h"

Cpu cpu;
uint8_t *mem1;
uint8_t *mem2;

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

void system_call(uint32_t address) { (void)address; }

static const char *system_folder;

static void *run(void *unused) {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // A frame can't wait for the system's other work.
#endif
  (void)unused;
  static char path[1024];
  snprintf(path, sizeof path, "%s/main.dol", system_folder);
  fill_code_slots();
  uint32_t entry = load_program(path);
  set_up_low_memory(system_folder);
  fprintf(stderr, "running from %08x\n", entry);
  dispatch(entry);
  fprintf(stderr, "the program returned\n");
  return 0;
}

// Saves and compiled shaders live next to the program, so they don't depend on where
// it was started from, and each title's build keeps its own.
static void use_save_folder_beside(const char *program) {
  char full[PATH_MAX], saves[PATH_MAX + 16], shaders[PATH_MAX + 16];
  if (!realpath(program, full)) return;
  const char *folder = dirname(full);  // Linux's changes `full`, so it's called once.
  snprintf(saves, sizeof saves, "%s/saves", folder);
  snprintf(shaders, sizeof shaders, "%s/shader-cache", folder);
  setenv("WII_RECOMP_SAVES", saves, 0);
  setenv("WII_RECOMP_SHADER_CACHE", shaders, 0);
  snprintf(saves, sizeof saves, "%s/discovered.txt", folder);
  setenv("WII_RECOMP_DISCOVERED", saves, 0);
}

int main(int count, char **arguments) {
  // On the Mac, submitting can wait up to a second for the window to free a picture.
  // MoltenVK does that on its own thread with this off, so the game never stalls on it.
  // It reads this as it loads, which is before any window exists, so it's set first.
  setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", 0);
  if (count < 2) {
    fprintf(stderr, "usage: wii-recomp <folder holding main.dol, boot.bin and fst.bin> [Wii storage folder]\n");
    return 1;
  }
  system_folder = arguments[1];
  mem1 = calloc(1, MEM1_SIZE);
  mem2 = calloc(1, MEM2_SIZE);
  time_start();
  hardware_start();
  audio_start();
  dvd_start(system_folder);
  use_save_folder_beside(arguments[0]);
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
