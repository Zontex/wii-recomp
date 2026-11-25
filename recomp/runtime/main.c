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
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "audio.h"
#include "dsp.h"
#include "console.h"
#include "controllers.h"
#include "input.h"
#include "pads.h"
#include "cpu.h"
#include "debug.h"
#include "dispatch.h"
#include "drive.h"
#include "gx.h"
#include "hardware.h"
#include "ios.h"
#include "loader.h"
#include "profile.h"
#include "title.h"
#include "net.h"

#ifndef _WIN32
Cpu cpu;
#endif

#define STACK_BYTES (1024u * 1024u * 1024u)

// PC builds are compiled for x86-64-v3 (build_file.py): AVX2, FMA and BMI, which Intel's
// Haswell, AMD's Zen and later have. On an older processor the first such instruction would
// end the program with no word of why, and a .wrd image carries the code built on another
// computer. So this looks first, before anything built that way runs, and is built for the
// plain x86-64 baseline itself.
#if defined(__x86_64__) && !defined(__APPLE__)
#include <cpuid.h>
__attribute__((constructor(101), target("arch=x86-64"))) static void check_processor(void) {
  unsigned a, b, c, d, low, high;
  int avx = 0, fma = 0, avx2 = 0, bmi = 0;
  if (__get_cpuid(1, &a, &b, &c, &d)) {
    fma = (c >> 12) & 1;
    if (((c >> 27) & 1) && ((c >> 28) & 1)) {  // The system saves AVX registers, and there are some.
      __asm__("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
      avx = (low & 6) == 6;
    }
  }
  if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) avx2 = (b >> 5) & 1, bmi = ((b >> 3) & 1) && ((b >> 8) & 1);
  if (avx && avx2 && fma && bmi) return;
  fprintf(stderr, "stop: this build needs a processor with AVX2, FMA and BMI (Intel Haswell, AMD Zen or later); this one lacks%s%s%s%s\n",
          avx ? "" : " AVX", avx2 ? "" : " AVX2", fma ? "" : " FMA", bmi ? "" : " BMI");
  exit(1);
}
#endif

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
  // The message is a printf format; its first values are here for whoever reads the log.
  fprintf(stderr, " (called from %08x; values %x %x %x)\n", cpu.lr, cpu.r[6], cpu.r[7], cpu.r[8]);
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
#ifdef __linux__
  // The game's thread sleeps a tenth of a millisecond at a time while it waits for the
  // graphics; the kernel's usual rounding added half as much again to each.
  prctl(PR_SET_TIMERSLACK, 1000);
#endif
  stage_sample_thread(0);
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
#elif defined(_WIN32)
#define LIBRARY_SUFFIX "dll"
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

// Saves, compiled shaders, the native code cache and what each title's code did live next
// to the program, so they don't depend on where it was started from.
static void use_folders_beside(const char *program) {
  char full[PATH_MAX], path[PATH_MAX + 16];
  if (!realpath(program, full)) return;
  const char *folder = dirname(full);  // Linux's changes `full`, so it's called once.
  const char *names[][2] = {{"WII_RECOMP_SAVES", "saves"}, {"WII_RECOMP_SHADER_CACHE", "shader-cache"},
                            {"WII_RECOMP_TITLE_DATA", "title-data"}, {"WII_RECOMP_CACHE", "native-cache"},
                            {"WII_RECOMP_GAPS", "gaps.txt"}, {"WII_RECOMP_REMOTE_SLOTS", "remotes.txt"},
                            {"WII_RECOMP_CONTROLLERS", "controllers.ini"}, {"WII_RECOMP_CERTIFICATES", "certificates"},
                            {"WII_RECOMP_MEMORY_CARD_A", "memory-card-a.raw"}};
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    snprintf(path, sizeof path, "%s/%s", folder, names[i][1]);
    setenv(names[i][0], path, 0);
  }
}

// With no Wii storage folder named, the one import_nand.py makes from a BootMii backup
// in wii/, if it's there.
static const char *default_storage(void) {
  struct stat info;
  if (!stat("wii/nand", &info) && S_ISDIR(info.st_mode)) return "wii/nand";
  if (!stat("wii/nand.bin", &info)) fprintf(stderr, "storage: wii/nand.bin isn't decrypted yet: run python3 recomp/tools/import_nand.py\n");
  return 0;
}

static int is_folder(const char *path) {
  struct stat info;
  if (stat(path, &info)) { perror(path); exit(1); }
  return S_ISDIR(info.st_mode);
}

// What a console starts with: the Wii Menu, the disc (--boot-disc) or a title (--title).
static uint64_t console_start_argument(int count, char **arguments) {
  unsigned high, low;
  if (count >= 1 && !strcmp(arguments[0], "--boot-disc")) return CONSOLE_DISC;
  if (count >= 2 && !strcmp(arguments[0], "--title") && sscanf(arguments[1], "%8x-%8x", &high, &low) == 2) {
    return (uint64_t)high << 32 | low;
  }
  if (count >= 1) {
    fprintf(stderr, "stop: a console starts with nothing, --boot-disc or --title <high>-<low>, not %s\n", arguments[0]);
    exit(1);
  }
  return CONSOLE_MENU;
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
    fprintf(stderr, "usage: wii-recomp <disc image, or folder holding main.dol, boot.bin and fst.bin> [Wii storage folder, wii/nand by default]\n"
                    "       wii-recomp --console <console folder> [--boot-disc | --title <high>-<low>]\n");
    return 1;
  }
#ifdef _WIN32
  setenv("WII_RECOMP_SHOP_SERVER", "off", 0);  // No network on Windows yet: net_win.c.
  setvbuf(stderr, 0, _IONBF, 0);
#endif
  console_remember(arguments);
  net_remember_program(arguments[0]);
  if (!strcmp(arguments[1], "--console")) {
    console_start(arguments[0], arguments[2], console_start_argument(count - 3, arguments + 3), &boot);
    count = 1;  // The console's storage is its own, never a separate folder.
  } else {
    use_folders_beside(arguments[0]);  // Before the title loads, as a disc image's native code goes to the cache.
    if (!drive_insert(arguments[1])) return 1;
    if (is_folder(arguments[1])) {  // An extracted disc or a system title, with its native code built beside the console.
      load_title_beside(arguments[0]);
      boot_from_folder(arguments[1], &boot);
    } else {
      load_title_from_disc();
      boot_from_disc(&boot);
    }
  }
  use_folders_beside(arguments[0]);  // For the console, whatever its own folders leave unset.
  gx_choose_scale();
  profile_start();
  guest_memory_init();
  if (gamecube_mode) guest_memory_open_virtual();
  time_start();
  hardware_start();
  dsp_start();
  audio_start();
  files_start(count > 2 ? arguments[2] : count == 2 ? default_storage() : 0);
  system_files_make();
  console_settings_make((char)boot.header[3]);
  if (!gamecube_mode) files_make_title_folders((uint64_t)title->title_high << 32 | title->title_low);  // Wii storage, which a GameCube game never uses.
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, STACK_BYTES);
  controllers_start();
  // In GameCube mode a controller maps as a Classic Controller, whose buttons sit where a
  // GameCube controller's do, and si.c reads player 1 as one.
  if (gamecube_mode) input_attach(0, ATTACHMENT_CLASSIC);
  window_create();
  pads_start();  // On this thread, which the Mac's controller frameworks need, as the window does.
  if (getenv("WII_RECOMP_CONTROL")) input_start_control_pipe(getenv("WII_RECOMP_CONTROL"));
  pthread_t thread;
  pthread_create(&thread, &attributes, run, 0);
  window_run();  // The Mac needs the window on this thread.
  return 0;
}
