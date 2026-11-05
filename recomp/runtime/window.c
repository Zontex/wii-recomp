// The game's window. The Mac needs it created and serviced on the main thread;
// drawing happens on the game's thread.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>

#include "gx_backend.h"
#include "input.h"

enum { WINDOW_WIDTH = 1280, WINDOW_HEIGHT = 720 };

static GLFWwindow *window;
static int frame_width, frame_height;

static void on_key(GLFWwindow *from, int key, int scancode, int action, int mods) {
  (void)from; (void)scancode; (void)mods;
  if (action == GLFW_REPEAT) return;
  static const struct { int key; uint32_t button; } keys[] = {
    {GLFW_KEY_LEFT, BUTTON_LEFT}, {GLFW_KEY_RIGHT, BUTTON_RIGHT}, {GLFW_KEY_UP, BUTTON_UP}, {GLFW_KEY_DOWN, BUTTON_DOWN},
    {GLFW_KEY_ENTER, BUTTON_A}, {GLFW_KEY_BACKSPACE, BUTTON_B}, {GLFW_KEY_1, BUTTON_ONE}, {GLFW_KEY_2, BUTTON_TWO},
    {GLFW_KEY_EQUAL, BUTTON_PLUS}, {GLFW_KEY_MINUS, BUTTON_MINUS}, {GLFW_KEY_H, BUTTON_HOME},
    // The Classic Controller's shoulders: in Mario Kart, L uses an item and R drifts. C and
    // Z are the Nunchuk's buttons too; each attachment reads only its own.
    {GLFW_KEY_X, BUTTON_CLASSIC_L}, {GLFW_KEY_C, BUTTON_CLASSIC_R | BUTTON_NUNCHUK_C}, {GLFW_KEY_Z, BUTTON_CLASSIC_ZL | BUTTON_NUNCHUK_Z},
    {GLFW_KEY_V, BUTTON_CLASSIC_ZR},
  };
  for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
    if (key == keys[i].key) input_hold(0, keys[i].button, action == GLFW_PRESS);
  }
  int down = action == GLFW_PRESS;
  // The arrows also tilt the Classic Controller's left stick, which games such as Mario
  // Kart steer with, and the Nunchuk's.
  static int left, right, up, low;
  if (key == GLFW_KEY_LEFT) left = down;
  if (key == GLFW_KEY_RIGHT) right = down;
  if (key == GLFW_KEY_UP) up = down;
  if (key == GLFW_KEY_DOWN) low = down;
  if (key >= GLFW_KEY_RIGHT && key <= GLFW_KEY_UP) {
    input_stick(0, (float)(right - left), (float)(up - low));
    input_nunchuk_stick(0, (float)(right - left), (float)(up - low));
  }
  if (key == GLFW_KEY_Q && down) input_shake(0);
  // F2 plugs the next attachment into player 1's remote: none, a Nunchuk, a Classic Controller.
  if (key == GLFW_KEY_F2 && down) input_attach(0, (Attachment)((input_attachment(0) + 1) % ATTACHMENT_KINDS));
  if (key == GLFW_KEY_SPACE && down) input_board_toggle();
  static float lean_x, lean_y;
  if (key == GLFW_KEY_A) lean_x = down ? -0.6f : 0;
  if (key == GLFW_KEY_D) lean_x = down ? 0.6f : 0;
  if (key == GLFW_KEY_W) lean_y = down ? 0.6f : 0;
  if (key == GLFW_KEY_S) lean_y = down ? -0.6f : 0;
  input_board_nudge(lean_x, lean_y);
}

// A control script drives the remote alone, so a stray mouse can't move or click it.
static int scripted(void) { return getenv("WII_RECOMP_CONTROL") != 0; }

static void on_mouse_move(GLFWwindow *from, double x, double y) {
  if (scripted()) return;
  int width, height;
  glfwGetWindowSize(from, &width, &height);
  input_pointer(0, (float)(x / width * 2 - 1), (float)(y / height * 2 - 1));
}

static void on_mouse_button(GLFWwindow *from, int button, int action, int mods) {
  (void)from; (void)mods;
  if (scripted()) return;
  if (button == GLFW_MOUSE_BUTTON_LEFT) input_hold(0, BUTTON_A, action == GLFW_PRESS);
  if (button == GLFW_MOUSE_BUTTON_RIGHT) input_hold(0, BUTTON_B, action == GLFW_PRESS);
}

// A Mac slows down a program whose window is covered or out of sight, which stretches
// the game's waits from milliseconds to a tenth of a second and drops it to 10 fps.
// A game keeps its own time with sound playing, as a console does, so it says so.
static void stay_awake(void) {
#ifdef __APPLE__
  // NSActivityUserInitiatedAllowingIdleSystemSleep and NSActivityLatencyCritical.
  const unsigned long long ACTIVITY_USER_INITIATED = 0x00FFFFFFULL & ~(1ULL << 20), ACTIVITY_LATENCY_CRITICAL = 0xFF00000000ULL;
  id info = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSProcessInfo"), sel_registerName("processInfo"));
  id reason = ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "running a game");
  id activity = ((id (*)(id, SEL, unsigned long long, id))objc_msgSend)(info, sel_registerName("beginActivityWithOptions:reason:"),
                                                                       ACTIVITY_USER_INITIATED | ACTIVITY_LATENCY_CRITICAL, reason);
  ((id (*)(id, SEL))objc_msgSend)(activity, sel_registerName("retain"));  // Held for as long as the program runs.
#endif
}

void window_create(void) {
  if (!glfwInit()) { fprintf(stderr, "stop: no window system\n"); exit(10); }
  stay_awake();
  gx_backend_choose();
  gx_backend->window_prepare();
  glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);  // The game draws 640x456; 4x the pixels adds cost, not detail.
  window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Wii Recomp", 0, 0);
  if (!window) { fprintf(stderr, "stop: no window\n"); exit(10); }
  glfwSetKeyCallback(window, on_key);
  glfwSetCursorPosCallback(window, on_mouse_move);
  glfwSetMouseButtonCallback(window, on_mouse_button);
  gx_backend->window_ready(window);
}

struct GLFWwindow *window_handle(void) { return window; }

void window_refresh_size(void) { glfwGetFramebufferSize(window, &frame_width, &frame_height); }

void window_size(int *width, int *height) {
  if (!frame_width) window_refresh_size();
  *width = frame_width;
  *height = frame_height;
}

void window_run(void) {
  while (!glfwWindowShouldClose(window)) glfwWaitEventsTimeout(0.05);
  exit(0);
}
