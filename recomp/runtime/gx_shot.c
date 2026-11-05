// Saves the next picture the game shows, for scripts that need to see the screen.
// Taken from the game's own picture, so it doesn't matter where the window is or
// what covers it. Written as a binary PPM, which needs no library. Vulkan hands the
// picture over through vk_picture_sink; OpenGL asks gx_shot_wanted() as it presents.
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "gx_shot.h"
#ifdef GX_WITH_VULKAN
#include "gx_vulkan.h"
#endif

static char wanted[1024];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int gx_shot_wanted(void) { return wanted[0] != 0; }

void gx_shot_write(const uint8_t *rgba, int width, int height) {
  char path[sizeof wanted];
  pthread_mutex_lock(&lock);
  memcpy(path, wanted, sizeof path);
  wanted[0] = 0;
#ifdef GX_WITH_VULKAN
  vk_picture_sink = 0;
#endif
  pthread_mutex_unlock(&lock);
  if (!path[0]) return;
  char temporary[sizeof path + 8];
  snprintf(temporary, sizeof temporary, "%s.part", path);
  FILE *file = fopen(temporary, "wb");
  if (!file) {
    fprintf(stderr, "shot: can't write %s\n", path);
    return;
  }
  fprintf(file, "P6\n%d %d\n255\n", width, height);
  for (int i = 0; i < width * height; i++) fwrite(rgba + i * 4, 1, 3, file);
  fclose(file);
  rename(temporary, path);  // Whole or not at all, for whoever is waiting on it.
}

void gx_shot(const char *path) {
  pthread_mutex_lock(&lock);
  snprintf(wanted, sizeof wanted, "%s", path);
#ifdef GX_WITH_VULKAN
  vk_picture_sink = gx_shot_write;
#endif
  pthread_mutex_unlock(&lock);
}
