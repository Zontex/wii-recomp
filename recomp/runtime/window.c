// The game's window. The Mac needs it created and serviced on the main thread;
// drawing happens on the game's thread.
#ifdef GX_WITH_VULKAN
#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_VULKAN
#else
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>

#include "console.h"
#include "controllers.h"
#include "gx_backend.h"
#include "hardware.h"
#include "pads.h"
#include "input.h"
#include "ios.h"

enum { WINDOW_HEIGHT = 720, POWER_OFF_SECONDS = 5 };

static GLFWwindow *window;
static int frame_width, frame_height;

// What each keyboard control presses on the keyboard's player's remote. C and Z work the
// Nunchuk's buttons, and by default the same keys work the Classic Controller's R and ZL;
// each attachment reads only its own.
static const uint32_t control_buttons[KEY_CONTROLS] = {
  [KEY_UP] = BUTTON_UP, [KEY_DOWN] = BUTTON_DOWN, [KEY_LEFT] = BUTTON_LEFT, [KEY_RIGHT] = BUTTON_RIGHT,
  [KEY_A] = BUTTON_A, [KEY_B] = BUTTON_B, [KEY_ONE] = BUTTON_ONE, [KEY_TWO] = BUTTON_TWO, [KEY_PLUS] = BUTTON_PLUS,
  [KEY_MINUS] = BUTTON_MINUS, [KEY_HOME] = BUTTON_HOME, [KEY_L] = BUTTON_CLASSIC_L, [KEY_R] = BUTTON_CLASSIC_R,
  [KEY_ZL] = BUTTON_CLASSIC_ZL, [KEY_ZR] = BUTTON_CLASSIC_ZR, [KEY_C] = BUTTON_NUNCHUK_C, [KEY_Z] = BUTTON_NUNCHUK_Z,
};

static void on_key(GLFWwindow *from, int key, int scancode, int action, int mods) {
  (void)from; (void)scancode; (void)mods;
  if (action == GLFW_REPEAT) return;
  int down = action == GLFW_PRESS, player = controllers_keyboard_player();
  KeyControl controls[KEY_CONTROLS];
  int count = controllers_controls_for(key, controls, KEY_CONTROLS);
  // The stick keys tilt the Classic Controller's left stick, which games such as Mario
  // Kart steer with, and the Nunchuk's.
  static int stick[4];
  static float lean_x, lean_y;
  for (int i = 0; i < count; i++) {
    KeyControl control = controls[i];
    if (control_buttons[control] && player >= 0) input_hold(player, control_buttons[control], down);
    if (control >= KEY_STICK_UP && control <= KEY_STICK_RIGHT && player >= 0) {
      stick[control - KEY_STICK_UP] = down;
      float x = (float)(stick[KEY_STICK_RIGHT - KEY_STICK_UP] - stick[KEY_STICK_LEFT - KEY_STICK_UP]);
      float y = (float)(stick[0] - stick[KEY_STICK_DOWN - KEY_STICK_UP]);
      input_stick(player, x, y);
      input_nunchuk_stick(player, x, y);
    }
    if (control == KEY_SHAKE && down && player >= 0) input_shake(player);
    // The next attachment: none, a Nunchuk, a Classic Controller.
    if (control == KEY_NEXT_ATTACHMENT && down && player >= 0) {
      input_attach(player, (Attachment)((input_attachment(player) + 1) % ATTACHMENT_KINDS));
    }
    if (control == KEY_MOTIONPLUS && down && player >= 0) input_attach_motionplus(player, !input_motionplus(player));
    if (control == KEY_BOARD && down) input_board_toggle();
    if (control == KEY_LEAN_LEFT) lean_x = down ? -0.6f : 0;
    if (control == KEY_LEAN_RIGHT) lean_x = down ? 0.6f : 0;
    if (control == KEY_LEAN_FORWARD) lean_y = down ? 0.6f : 0;
    if (control == KEY_LEAN_BACK) lean_y = down ? -0.6f : 0;
    if (control >= KEY_LEAN_LEFT && control <= KEY_LEAN_BACK) input_board_nudge(lean_x, lean_y);
  }
}

// A control script drives the remote alone, so a stray mouse can't move or click it.
static int scripted(void) { return getenv("WII_RECOMP_CONTROL") != 0; }

static void on_mouse_move(GLFWwindow *from, double x, double y) {
  if (scripted()) return;
  int width, height;
  glfwGetWindowSize(from, &width, &height);
  int player = controllers_mouse_player();
  if (player >= 0 && controllers_player(player)->pointer & POINTER_MOUSE) {
    input_pointer(player, (float)(x / width * 2 - 1), (float)(y / height * 2 - 1));
  }
}

static void on_mouse_button(GLFWwindow *from, int button, int action, int mods) {
  (void)from; (void)mods;
  if (scripted()) return;
  int player = controllers_mouse_player();
  if (player >= 0 && button == GLFW_MOUSE_BUTTON_LEFT) input_hold(player, BUTTON_A, action == GLFW_PRESS);
  if (player >= 0 && button == GLFW_MOUSE_BUTTON_RIGHT) input_hold(player, BUTTON_B, action == GLFW_PRESS);
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
#ifdef GX_WITH_VULKAN
  // GLFW looks for the Vulkan loader on its own, and on the Mac it doesn't find
  // Homebrew's, so it's handed the one the console is linked with.
  glfwInitVulkanLoader(vkGetInstanceProcAddr);
#endif
  if (!glfwInit()) { fprintf(stderr, "stop: no window system\n"); exit(10); }
  stay_awake();
  gx_backend_choose();
  gx_backend->window_prepare();
  glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);  // The game draws 640x456; 4x the pixels adds cost, not detail.
  // The console's own setting says whether the TV is widescreen (IPL.AR), and games draw
  // for it, so the window has that shape and keeps it when resized.
  uint8_t widescreen = 0;
  files_system_setting("IPL.AR", &widescreen, 1);
  int aspect_width = widescreen ? 16 : 4, aspect_height = widescreen ? 9 : 3;
  window = glfwCreateWindow(WINDOW_HEIGHT * aspect_width / aspect_height, WINDOW_HEIGHT, "Wii Recomp", 0, 0);
  if (!window) { fprintf(stderr, "stop: no window\n"); exit(10); }
  glfwSetWindowAspectRatio(window, aspect_width, aspect_height);
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

// Closing the window is pressing the power button: the program turns the console off
// its own way, writing what it needs to. One that doesn't listen, or takes too long,
// has its power cut, with open files written out as IOS would.
void window_run(void) {
  while (!glfwWindowShouldClose(window)) {
    glfwWaitEventsTimeout(pads_wait_seconds());
    pads_poll();
  }
  if (power_button_press()) {
    fprintf(stderr, "console: the power button is pressed\n");
    for (double until = glfwGetTime() + POWER_OFF_SECONDS; glfwGetTime() < until;) {
      glfwWaitEventsTimeout(0.05);
      pads_poll();
    }
    fprintf(stderr, "console: the program didn't turn off within %d seconds\n", POWER_OFF_SECONDS);
  }
  console_power_off("off");
}
