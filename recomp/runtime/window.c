// The game's window. The Mac needs it created and serviced on the main thread;
// drawing happens on the game's thread.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
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
  };
  for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
    if (key == keys[i].key) input_hold(keys[i].button, action == GLFW_PRESS);
  }
  int down = action == GLFW_PRESS;
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
  input_pointer((float)(x / width * 2 - 1), (float)(y / height * 2 - 1));
}

static void on_mouse_button(GLFWwindow *from, int button, int action, int mods) {
  (void)from; (void)mods;
  if (scripted()) return;
  if (button == GLFW_MOUSE_BUTTON_LEFT) input_hold(BUTTON_A, action == GLFW_PRESS);
  if (button == GLFW_MOUSE_BUTTON_RIGHT) input_hold(BUTTON_B, action == GLFW_PRESS);
}

void window_create(void) {
  if (!glfwInit()) { fprintf(stderr, "stop: no window system\n"); exit(10); }
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
