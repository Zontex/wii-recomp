// Shaders written for what the game has set up, compiled on first use and kept by key.
// Writing them (gx_tev.c, gx_transform.c, gx_light.c) knows no graphics API.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_backend.h"

enum { SHADER_TEXT_BYTES = 64 * 1024, FIRST_SLOTS = 256 };

// A hash table from a key's bytes to what the backend made of it.
typedef struct {
  uint8_t *keys;
  unsigned *objects;
  uint32_t key_bytes, slots, count;
} Cache;

typedef struct {
  TevKey tev;
  VertexKey vertex;
} ProgramKey;

// Each program built, with what it was built from, so a pipeline the backend builds can be
// named by its keys (gx_recipes.c). Only searched when something new is built.
static struct { unsigned program; ProgramKey key; } *built;
static unsigned built_count, built_capacity;

static Cache fragment_shaders = {.key_bytes = sizeof(TevKey)}, vertex_shaders = {.key_bytes = sizeof(VertexKey)};
static Cache programs = {.key_bytes = sizeof(ProgramKey)};

void gx_text_add(ShaderText *text, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  int wrote = vsnprintf(text->characters + text->used, text->size - text->used, format, arguments);
  va_end(arguments);
  if (wrote < 0 || (size_t)wrote >= text->size - text->used) {
    fprintf(stderr, "shader: text too long\n");
    exit(9);
  }
  text->used += (size_t)wrote;
}

static uint32_t hash_bytes(const uint8_t *bytes, uint32_t count) {
  uint32_t hash = 2166136261u;
  for (uint32_t i = 0; i < count; i++) hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}

static uint32_t find_slot(const Cache *cache, const void *key) {
  uint32_t slot = hash_bytes(key, cache->key_bytes) & (cache->slots - 1);
  while (cache->objects[slot] && memcmp(&cache->keys[slot * cache->key_bytes], key, cache->key_bytes)) {
    slot = (slot + 1) & (cache->slots - 1);
  }
  return slot;
}

static void grow(Cache *cache) {
  Cache old = *cache;
  cache->slots = old.slots ? old.slots * 2 : FIRST_SLOTS;
  cache->keys = calloc(cache->slots, cache->key_bytes);
  cache->objects = calloc(cache->slots, sizeof *cache->objects);
  for (uint32_t i = 0; i < old.slots; i++) {
    if (!old.objects[i]) continue;
    uint32_t slot = find_slot(cache, &old.keys[i * old.key_bytes]);
    memcpy(&cache->keys[slot * cache->key_bytes], &old.keys[i * old.key_bytes], cache->key_bytes);
    cache->objects[slot] = old.objects[i];
  }
  free(old.keys);
  free(old.objects);
}

static void store(Cache *cache, const void *key, unsigned object) {
  if ((cache->count + 1) * 2 > cache->slots) grow(cache);
  uint32_t slot = find_slot(cache, key);
  memcpy(&cache->keys[slot * cache->key_bytes], key, cache->key_bytes);
  cache->objects[slot] = object;
  cache->count++;
}

static unsigned find(const Cache *cache, const void *key) { return cache->slots ? cache->objects[find_slot(cache, key)] : 0; }

static unsigned fragment_shader(const TevKey *key) {
  static char characters[SHADER_TEXT_BYTES];
  unsigned shader = find(&fragment_shaders, key);
  if (shader) return shader;
  ShaderText text = {characters, 0, sizeof characters};
  gx_tev_write_shader(key, &text);
  shader = gx_backend->stage_compile(GX_STAGE_FRAGMENT, characters);
  store(&fragment_shaders, key, shader);
  return shader;
}

static unsigned vertex_shader(const VertexKey *key) {
  static char characters[SHADER_TEXT_BYTES];
  unsigned shader = find(&vertex_shaders, key);
  if (shader) return shader;
  ShaderText text = {characters, 0, sizeof characters};
  gx_transform_write_shader(key, &text);
  shader = gx_backend->stage_compile(GX_STAGE_VERTEX, characters);
  store(&vertex_shaders, key, shader);
  return shader;
}

static unsigned build_program(const ProgramKey *key) {
  double start = glfwGetTime();
  unsigned program = gx_backend->shader_link(vertex_shader(&key->vertex), fragment_shader(&key->tev));
  float milliseconds = (float)((glfwGetTime() - start) * 1000);
  gx_stats.new_shaders++;
  if (milliseconds > gx_stats.slowest_shader_ms) gx_stats.slowest_shader_ms = milliseconds;
  if (built_count == built_capacity) {
    built_capacity = built_capacity ? built_capacity * 2 : FIRST_SLOTS;
    built = realloc(built, built_capacity * sizeof *built);
  }
  built[built_count].program = program;
  built[built_count++].key = *key;
  return program;
}

static unsigned program_for(const ProgramKey *key) {
  unsigned program = find(&programs, key);
  if (!program) {
    program = build_program(key);
    store(&programs, key, program);
  }
  return program;
}

int gx_shader_recipe(GxShader program, const GxRenderState *state, GxRecipe *recipe) {
  for (unsigned i = built_count; i-- > 0;) {
    if (built[i].program != program) continue;
    memset(recipe, 0, sizeof *recipe);
    recipe->tev = built[i].key.tev;
    recipe->vertex = built[i].key.vertex;
    recipe->state = *state;
    return 1;
  }
  return 0;  // One of the backend's own, such as for clearing.
}

// Every stage's text first, so a backend can compile them side by side, then each
// program and what's built around it, all found again in the caches when drawing.
void gx_shader_prepare(const GxRecipe *recipes, unsigned count) {
  if (gx_backend->stages_prepare) {
    int *kinds = malloc(2 * count * sizeof *kinds);
    char **texts = malloc(2 * count * sizeof *texts);
    unsigned texts_count = 0;
    for (unsigned i = 0; i < count; i++) {
      ProgramKey whole = {recipes[i].tev, recipes[i].vertex}, *key = &whole;
      if (find(&programs, key)) continue;
      for (int kind = 0; kind < 2; kind++) {
        if (kind == GX_STAGE_VERTEX ? find(&vertex_shaders, &key->vertex) != 0 : find(&fragment_shaders, &key->tev) != 0) continue;
        ShaderText text = {malloc(SHADER_TEXT_BYTES), 0, SHADER_TEXT_BYTES};
        if (kind == GX_STAGE_VERTEX) gx_transform_write_shader(&key->vertex, &text);
        else gx_tev_write_shader(&key->tev, &text);
        kinds[texts_count] = kind;
        texts[texts_count++] = text.characters;
      }
    }
    gx_backend->stages_prepare((int)texts_count, kinds, (const char *const *)texts);
    for (unsigned i = 0; i < texts_count; i++) free(texts[i]);
    free(kinds);
    free(texts);
  }
  for (unsigned i = 0; i < count; i++) {
    ProgramKey key = {recipes[i].tev, recipes[i].vertex};
    unsigned program = program_for(&key);
    if (gx_backend->pipeline_prepare) gx_backend->pipeline_prepare(program, &recipes[i].state);
  }
}

void gx_shader_use(const TevKey *tev, const VertexKey *vertex) {
  static ProgramKey in_use;
  static int anything_used;
  ProgramKey key;
  if (anything_used && !memcmp(&in_use.tev, tev, sizeof *tev) && !memcmp(&in_use.vertex, vertex, sizeof *vertex)) return;
  key.tev = *tev;
  key.vertex = *vertex;
  unsigned program = program_for(&key);
  gx_backend->shader_use(program);
  in_use = key;
  anything_used = 1;
}

void gx_constants_send(int binding, const void *data, size_t bytes) { gx_backend->constants_send(binding, data, bytes); }
