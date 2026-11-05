// gx_recipes.c: what a title drew with, kept in its data folder, added to by each run,
// built before the first picture, and merged with what the disc image carries. Run once
// per stage with WII_RECOMP_TITLE_DATA set; the file is kept between stages.
//   test_recipes <stage> <recipes the start should build> <recipes the file should hold after>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../harness.h"
#include "gx_backend.h"
#include "profile.h"
#include "title.h"

static Title made_up = {.title_high = 0x00010004, .title_low = 0x54455354};
const Title *title = &made_up;

// Shader n was built from a recipe that names n, and a draw's state is its own.
static GxRecipe made(int n, int cull) {
  GxRecipe recipe;
  memset(&recipe, 0, sizeof recipe);
  recipe.tev.stages = (uint8_t)n;
  recipe.state.cull = (uint8_t)cull;
  return recipe;
}

int gx_shader_recipe(GxShader program, const GxRenderState *state, GxRecipe *recipe) {
  if (program > 100) return 0;  // The backend's own.
  *recipe = made((int)program, state->cull);
  return 1;
}

static unsigned prepared = 0, prepared_names = 0;
void gx_shader_prepare(const GxRecipe *recipes, unsigned count) {
  prepared = count;
  for (unsigned i = 0; i < count; i++) prepared_names |= 1u << recipes[i].tev.stages;
}

// What the disc image carries, in the file's own format.
static int carry;
void *drive_shaders(size_t *bytes) {
  if (!carry) return 0;
  struct { char magic[4]; uint32_t version, record_bytes, title_high, title_low, unused[3]; } header = {
      {'W', 'R', 'S', 'R'}, 1, sizeof(GxRecipe), made_up.title_high, carry == 2 ? 0x4f544852 : made_up.title_low, {0}};
  GxRecipe carried[2] = {made(4, 0), made(1, 0)};
  *bytes = sizeof header + sizeof carried;
  uint8_t *data = malloc(*bytes);
  memcpy(data, &header, sizeof header);
  memcpy(data + sizeof header, carried, sizeof carried);
  return data;
}

static void draw_with(GxShader shader, int cull) {
  GxRenderState state;
  memset(&state, 0, sizeof state);
  state.cull = (uint8_t)cull;
  gx_recipe_note(shader, &state);
}

int main(int count, char **arguments) {
  if (count < 4) return 2;
  const char *stage = arguments[1];
  unsigned built_at_start = (unsigned)atoi(arguments[2]), held_after = (unsigned)atoi(arguments[3]);
  carry = !strcmp(stage, "disc") ? 1 : !strcmp(stage, "other-title") ? 2 : 0;
  gx_recipes_start();
  CHECK_EQ(prepared, built_at_start);
  if (!strcmp(stage, "first")) {
    draw_with(1, 0);
    draw_with(2, 0);
    draw_with(1, 0);  // Already kept.
    draw_with(1, 1);  // The same shader in another state is another pipeline.
    draw_with(200, 0);  // Not one of the title's.
  }
  if (!strcmp(stage, "disc")) CHECK_EQ(prepared_names, 1u << 1 | 1u << 2 | 1u << 4);
  char path[4096];
  CHECK(title_data_path("shaders.bin", path, sizeof path));
  struct stat info;
  CHECK(!stat(path, &info));
  CHECK_EQ((unsigned long long)info.st_size, 32 + held_after * sizeof(GxRecipe));
  return finish();
}
