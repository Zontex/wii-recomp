// The controller settings (controllers.c, settings_file.c): the file the console writes
// reads back as every default, each kind of mistake is caught on its line, the keyboard
// works the controls it's bound to, and the console writes the file on its first run and
// sets the players up from it. Compiled with controllers.c, settings_file.c and input.c.
// The argument is a folder to write files in.
#include <GLFW/glfw3.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "harness.h"
#include "controllers.h"
#include "settings_file.h"

uint64_t time_base(void) { return 0; }

static int line_of(const char *text) {
  char error[256];
  return controllers_load_text(text, error, sizeof error);
}

static int bound(int key, KeyControl control) {
  KeyControl controls[KEY_CONTROLS];
  int count = controllers_controls_for(key, controls, KEY_CONTROLS);
  for (int i = 0; i < count; i++) {
    if (controls[i] == control) return 1;
  }
  return 0;
}

int main(int count, char **arguments) {
  // The reader on its own: sections, comments, trimming, lists.
  char parts[4][32];
  CHECK_EQ(settings_list(" a, b ,,c ", parts, 4), 3);
  CHECK(!strcmp(parts[1], "b") && !strcmp(parts[2], "c"));
  CHECK_EQ(settings_list("a,b,c", parts, 2), -1);

  // Before any file, everything is at its default: the keyboard and mouse on player 1,
  // a Nunchuk in every remote, the board there.
  CHECK_EQ(controllers_keyboard_player(), 0);
  CHECK_EQ(controllers_mouse_player(), 0);
  CHECK_EQ(controllers_player(1)->sources, SOURCE_PAD);
  CHECK_EQ(controllers_player(3)->attachment, ATTACHMENT_NUNCHUK);
  CHECK_EQ(controllers_player(0)->pointer, POINTER_MOUSE | POINTER_GYRO);
  CHECK(controllers_board());
  CHECK(bound(GLFW_KEY_ENTER, KEY_A));
  CHECK(bound(GLFW_KEY_UP, KEY_UP) && bound(GLFW_KEY_UP, KEY_STICK_UP));
  CHECK(bound(GLFW_KEY_C, KEY_R) && bound(GLFW_KEY_C, KEY_C));
  CHECK(bound(GLFW_KEY_F2, KEY_NEXT_ATTACHMENT));
  CHECK(bound(GLFW_KEY_F3, KEY_MOTIONPLUS));

  // The written file is all comments, so it reads as the defaults; with the #s taken
  // away, it reads the same again, so every default it shows is a setting it accepts.
  const char *text = controllers_default_text();
  CHECK_EQ(line_of(text), 0);
  static char uncommented[16384];
  size_t at = 0;
  for (const char *line = text; *line;) {
    size_t length = strcspn(line, "\n");
    // Explanations are "# Words"; settings are "# key = value".
    const char *equals = memchr(line, '=', length);
    int setting = line[0] == '#' && equals && !memchr(line, '.', (size_t)(equals - line)) && !memchr(line, ':', length);
    const char *from = setting ? line + 2 : line;
    size_t bytes = length - (size_t)(from - line);
    if (!setting && line[0] == '#') bytes = 0;
    memcpy(uncommented + at, from, bytes);
    at += bytes;
    uncommented[at++] = '\n';
    line += length + (line[length] == '\n');
  }
  uncommented[at] = 0;
  char error[256];
  CHECK_EQ(controllers_load_text(uncommented, error, sizeof error), 0);
  CHECK(strstr(uncommented, "[player4]\nsource = pad\nattachment = nunchuk\n") != 0);  // Taken from the file, not the defaults.
  CHECK(strstr(uncommented, "zr = v\n") != 0);

  // Settings change what they say.
  CHECK_EQ(line_of("[player1]\nsource = pad\n[player2]\nsource = keyboard, pad:ABC123  # the blue one\n"
                   "attachment = classic\nmotionplus = yes\npointer = stick\ngyro_speed = 2.5\nrecenter = capture\nsideways = yes\n"
                   "[keys]\na = space, enter\nshake = none\n[board]\nsource = none\n[remotes]\nmode = real\n"), 0);
  CHECK_EQ(controllers_keyboard_player(), 1);
  CHECK_EQ(controllers_mouse_player(), -1);
  const PlayerSettings *second = controllers_player(1);
  CHECK_EQ(second->sources, SOURCE_KEYBOARD | SOURCE_PAD);
  CHECK(!strcmp(second->pad, "ABC123"));
  CHECK_EQ(second->attachment, ATTACHMENT_CLASSIC);
  CHECK_EQ(second->motionplus, 1);
  CHECK_EQ(controllers_player(0)->motionplus, 0);
  CHECK_EQ(second->pointer, POINTER_STICK);
  CHECK(second->gyro_speed == 2.5f);
  CHECK(!strcmp(second->recenter, "capture"));
  CHECK_EQ(second->sideways, 1);
  CHECK(!controllers_board());
  CHECK(bound(GLFW_KEY_SPACE, KEY_A) && bound(GLFW_KEY_ENTER, KEY_A));
  CHECK(!bound(GLFW_KEY_Q, KEY_SHAKE));
  CHECK(bound(GLFW_KEY_SPACE, KEY_BOARD));  // Space still steps on the board too.

  // Each mistake is caught on its own line.
  CHECK_EQ(line_of("[player1]\nsource = keyboard\n[player5]\nsource = pad\n"), 4);
  CHECK_EQ(line_of("\n[player1]\nattachment = guitar\n"), 3);
  CHECK_EQ(line_of("[player2]\ncolour = red\n"), 2);
  CHECK_EQ(line_of("[player2]\nsource = joystick\n"), 2);
  CHECK_EQ(line_of("[player2]\ngyro_speed = fast\n"), 2);
  CHECK_EQ(line_of("[player2]\ngyro_speed = 0\n"), 2);
  CHECK_EQ(line_of("[player2]\nrecenter = q\n"), 2);
  CHECK_EQ(line_of("[player2]\nsideways = maybe\n"), 2);
  CHECK_EQ(line_of("[player2]\nmotionplus = inside\n"), 2);
  CHECK_EQ(line_of("[keys]\njump = space\n"), 2);
  CHECK_EQ(line_of("[keys]\na = hyperspace\n"), 2);
  CHECK_EQ(line_of("[keys]\na = a, b, c, d, e\n"), 2);
  CHECK_EQ(line_of("[remotes]\nmode = some\n"), 2);
  CHECK_EQ(line_of("[board]\nsource = pad\n"), 2);
  CHECK_EQ(line_of("[sound]\n"), 0);  // An empty section is no mistake yet...
  CHECK_EQ(line_of("[sound]\nvolume = 3\n"), 2);  // ...but its settings are.
  CHECK_EQ(line_of("[player1\n"), 1);
  CHECK_EQ(line_of("source keyboard\n"), 1);
  CHECK_EQ(line_of(" = keyboard\n"), 1);
  // Two players can't share the keyboard: not one line's mistake.
  CHECK_EQ(line_of("[player2]\nsource = keyboard\n"), -1);
  CHECK_EQ(line_of(""), 0);

  // The console's first run writes the file, and the players are set up from it.
  char path[1024];
  snprintf(path, sizeof path, "%s/controllers.ini", arguments[1]);
  setenv("WII_RECOMP_CONTROLLERS", path, 1);
  unsetenv("WII_RECOMP_REMOTES");
  unsetenv("WII_RECOMP_ATTACHMENT");
  controllers_start();
  FILE *file = fopen(path, "r");
  CHECK(file != 0);
  if (file) fclose(file);
  CHECK(input_connected(0));
  CHECK(!input_connected(1));
  CHECK_EQ(input_attachment(0), ATTACHMENT_NUNCHUK);
  CHECK(!input_motionplus(0));
  CHECK(!strcmp(getenv("WII_RECOMP_REMOTES"), "auto"));
  CHECK(input_board_enabled());

  // A file with a mistake stops the console, naming the line.
  file = fopen(path, "w");
  fputs("[player1]\nsource = keyboard\nattachment = harmonica\n", file);
  fclose(file);
  pid_t child = fork();
  if (!child) {
    freopen("/dev/null", "w", stderr);
    controllers_start();
    _exit(0);
  }
  int status;
  waitpid(child, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
  return finish();
}
