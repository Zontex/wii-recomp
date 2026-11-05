// The control pipe (WII_RECOMP_CONTROL): a named pipe that takes one command a line, so
// a script or another program can play. The controls' commands are in input.c; the
// console's are here: reset to press its reset button, shot <file> to save the next
// picture, record <frames> <file> to record frames for recomp/tools/replay, dump and
// skip for the graphics inspector.
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gx_inspect.h"
#ifdef GX_WITH_VULKAN
#include "gx_shot.h"
#endif
#include "gx.h"
#include "hardware.h"
#include "input.h"

static void run_command(const char *line) {
  char path[1024];
  int first, last;
  if (input_run_command(line)) return;
  if (!strncmp(line, "reset", 5)) reset_button_press();
  else if (!strncmp(line, "dump", 4)) gx_inspect_dump();
  else if (sscanf(line, "record %d %1023s", &first, path) == 2) gx_record_request(first, path);
  else if (sscanf(line, "skip %d %d", &first, &last) == 2) gx_inspect_skip(first, last);
  else if (sscanf(line, "shot %1023s", path) == 1) {
#ifdef GX_WITH_VULKAN
    gx_shot(path);
#else
    fprintf(stderr, "control: shot needs the Vulkan renderer\n");
#endif
  } else fprintf(stderr, "control: unknown command: %s", line);
}

static void *control_loop(void *path) {
  // Opened for writing too, so it stays open between senders.
  FILE *pipe_file = fdopen(open(path, O_RDWR), "r");
  char line[1200];  // Room for a shot command with a long path.
  while (pipe_file && fgets(line, sizeof line, pipe_file)) run_command(line);
  return 0;
}

void input_start_control_pipe(const char *path) {
  static pthread_t thread;
  unlink(path);
  mkfifo(path, 0600);
  pthread_create(&thread, 0, control_loop, (void *)path);
}
