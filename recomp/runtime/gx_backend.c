// Picks a graphics backend. Which ones exist depends on the build; see build_file.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_backend.h"

const GxBackend *gx_backend;

// The first one here is the default: the first in build_file.py's list, which is Vulkan
// unless asked otherwise. OpenGL stays as a fallback for machines with no working Vulkan,
// and as a second opinion: a picture wrong in both is wrong above the line.
static const GxBackend *const backends[] = {
#if defined(GX_WITH_OPENGL) && defined(GX_FIRST_OPENGL)
  &gx_backend_opengl,
#endif
#ifdef GX_WITH_VULKAN
  &gx_backend_vulkan,
#endif
#if defined(GX_WITH_OPENGL) && !defined(GX_FIRST_OPENGL)
  &gx_backend_opengl,
#endif
};
enum { BACKEND_COUNT = sizeof backends / sizeof backends[0] };

void gx_backend_choose(void) {
  const char *wanted = getenv("WII_RECOMP_RENDERER");
  gx_backend = backends[0];
  if (!wanted || !*wanted) return;
  for (int i = 0; i < BACKEND_COUNT; i++) {
    if (!strcmp(backends[i]->name, wanted)) {
      gx_backend = backends[i];
      return;
    }
  }
  fprintf(stderr, "stop: this build has no renderer called %s\n", wanted);
  exit(10);
}
