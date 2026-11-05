// Turns the game's drawing registers into what a backend needs. Colors are worked out
// in gx_tev.c, and nothing here knows which graphics API is doing the work.
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debug.h"
#include "debug.h"
#include "gx_backend.h"
#include "gx_inspect.h"

enum {
  BP_GEN_MODE = 0x00, BP_SCISSOR_FIRST = 0x20, BP_SCISSOR_LAST = 0x21, BP_Z_MODE = 0x40, BP_BLEND = 0x41, BP_CONSTANT_ALPHA = 0x42,
  CONSTANT_ALPHA_ENABLE_BIT = 8, BP_CLEAR_COLOR_AR = 0x4f, BP_CLEAR_COLOR_GB = 0x50, BP_CLEAR_DEPTH = 0x51, BP_SCISSOR_OFFSET = 0x59,
  BP_TEXTURE_MODE_LOW = 0x80, BP_TEXTURE_MODE_HIGH = 0xa0, BP_TEXTURE_LIMITS_LOW = 0x84, BP_TEXTURE_LIMITS_HIGH = 0xa4,
  BP_TEXTURE_CACHE_LOW = 0x8c, BP_TEXTURE_CACHE_HIGH = 0xac, CACHE_MANAGED_BIT = 21,
  MODE_MAGNIFY_BIT = 4, MODE_MIP_SHIFT = 5, MODE_MINIFY_BIT = 7, MODE_EDGE_LEVEL_BIT = 8, MODE_ANISOTROPY_SHIFT = 19, MODE_LEVEL_CLAMP_BIT = 21,
  MIP_FILTER_NONE = 0, MIP_FILTER_LINEAR = 2, WRAP_MODES = 3,
  XF_VIEWPORT = 0x101a, VIEWPORT_ORIGIN = 342, VIEWPORT_Z_RANGE = 2, VIEWPORT_FAR = 5, DEPTH_STEPS = 16777216, VERTICES_PER_QUAD = 4,
  CACHE_SLOTS = 4096, CACHE_SLOT_SHIFT = 20, CACHE_PROBES = 16, BLEND_SUBTRACT_BIT = 11, LOGIC_ENABLE_BIT = 1, LOGIC_MODE_SHIFT = 12,
  LOGIC_NOOP = 5, BP_PIXEL_FORMAT = 0x43, PIXEL_FORMAT_MASK = 7, PIXEL_FORMAT_RGBA6 = 1, PIXEL_FORMAT_DEPTH = 3, PIXEL_FORMAT_UNSEEN = 8, BP_LINE_POINT_SIZE = 0x22, POINT_SIZE_SHIFT = 8, LINE_OFFSET_SHIFT = 16, POINT_OFFSET_SHIFT = 19, ASPECT_ADJUST_BIT = 22,
  BP_COORDINATE_SCALE = 0x30, LINE_OFFSET_BIT = 18, POINT_OFFSET_BIT = 19, SIXTHS = 6, EXPAND_LINES = 1, EXPAND_POINTS = 2,
  EXPANDED_QUADS = MAX_VERTICES / VERTICES_PER_QUAD,
  BP_TEXTURE_ADDRESS = 0x94, BP_TEXTURE_ADDRESS_HIGH = 0xb4, BP_TEXTURE_IMAGE = 0x88, BP_TEXTURE_IMAGE_HIGH = 0xa8,
  BP_TEXTURE_PALETTE = 0x98, BP_TEXTURE_PALETTE_HIGH = 0xb8, MAX_ATTRIBUTES = 16,
};

static unsigned frames;

unsigned gx_frame_number(void) { return frames; }

typedef struct {
  int used;
  uint32_t address, image, palette;
  uint64_t key;
  unsigned checked_frame;
  GxTexture *texture;
} CacheEntry;

static CacheEntry cache[CACHE_SLOTS];
static int started;

static void start(void) {
  GxAttribute attributes[MAX_ATTRIBUTES];
  int count = 0;
  attributes[count++] = (GxAttribute){ATTRIBUTE_POSITION, 3, GX_ATTRIBUTE_FLOAT, offsetof(Vertex, position)};
  attributes[count++] = (GxAttribute){ATTRIBUTE_NORMAL, 3, GX_ATTRIBUTE_FLOAT, offsetof(Vertex, normal)};
  for (int i = 0; i < 2; i++) attributes[count++] = (GxAttribute){ATTRIBUTE_COLOR + i, 4, GX_ATTRIBUTE_FLOAT, offsetof(Vertex, color) + (size_t)i * 4 * sizeof(float)};
  for (int i = 0; i < MAX_TEXTURE_MAPS; i++) attributes[count++] = (GxAttribute){ATTRIBUTE_UV + i, 2, GX_ATTRIBUTE_FLOAT, offsetof(Vertex, uv) + (size_t)i * 2 * sizeof(float)};
  for (int i = 0; i < ATTRIBUTE_MATRIX_GROUPS; i++) attributes[count++] = (GxAttribute){ATTRIBUTE_MATRICES + i, 4, GX_ATTRIBUTE_BYTE, offsetof(Vertex, matrix) + (size_t)i * 4};
  gx_backend->start(attributes, count, sizeof(Vertex));
  started = 1;
  gx_recipes_start();
}

// Finds a texture's slot by what identifies it, and starts using a free one if new.
static uint32_t cache_slot(uint32_t address, uint32_t image, uint32_t palette) {
  uint32_t mixed = (address ^ (image * 31u) ^ (palette * 131u)) * 2654435761u;
  uint32_t first = mixed >> CACHE_SLOT_SHIFT;
  for (uint32_t step = 0; step < CACHE_PROBES; step++) {
    uint32_t slot = (first + step) % CACHE_SLOTS;
    CacheEntry *entry = &cache[slot];
    if (!entry->used || (entry->address == address && entry->image == image && entry->palette == palette)) {
      entry->used = 1;
      entry->address = address;
      entry->image = image;
      entry->palette = palette;
      return slot;
    }
  }
  cache[first].address = address;
  cache[first].image = image;
  cache[first].palette = palette;
  return first;
}

static void report_unsupported_sampling(int map, uint32_t mode) {
  if ((mode >> MODE_LEVEL_CLAMP_BIT) & 1) debug_unsupported("gx.0x84_0x87_0xa4_0xa7_texture_mode_1", "texture level of detail clamp bit", (uint32_t)map);
  if ((gx.bp[map < 4 ? BP_TEXTURE_CACHE_LOW + map : BP_TEXTURE_CACHE_HIGH + map - 4] >> CACHE_MANAGED_BIT) & 1) debug_unsupported("vtx.tmem_preload", "texture preloaded to texture memory", (uint32_t)map);
}

// A wrap mode of 3 clamps, as Dolphin's tests found. With no mip filter only the full
// image is read, and when the largest level is below the smallest, the largest wins.
static void use_texture(int map, GxTexture *texture, int has_levels) {
  static const uint8_t wraps[4] = {GX_WRAP_CLAMP, GX_WRAP_REPEAT, GX_WRAP_MIRROR, GX_WRAP_CLAMP};
  uint32_t mode = gx.bp[map < 4 ? BP_TEXTURE_MODE_LOW + map : BP_TEXTURE_MODE_HIGH + map - 4];
  uint32_t limits = gx.bp[map < 4 ? BP_TEXTURE_LIMITS_LOW + map : BP_TEXTURE_LIMITS_HIGH + map - 4];
  uint32_t filter = (mode >> MODE_MIP_SHIFT) & 3;
  report_unsupported_sampling(map, mode);
  GxSampling sampling;
  memset(&sampling, 0, sizeof sampling);
  sampling.wrap_s = wraps[mode & 3];
  sampling.wrap_t = wraps[(mode >> 2) & 3];
  sampling.magnify_linear = (mode >> MODE_MAGNIFY_BIT) & 1;
  sampling.minify_linear = (mode >> MODE_MINIFY_BIT) & 1;
  // 1 and 2 allow 2 and 4 samples along a slope, as Dolphin's samplers do. 3 means none.
  uint32_t anisotropy = (mode >> MODE_ANISOTROPY_SHIFT) & 3;
  sampling.anisotropy = anisotropy == 3 ? 0 : (uint8_t)anisotropy;
  if (has_levels && filter != MIP_FILTER_NONE) {
    uint8_t smallest = limits & 255, largest = (limits >> 8) & 255;
    sampling.mip_mode = filter == MIP_FILTER_LINEAR ? GX_MIP_LINEAR : GX_MIP_NEAREST;
    sampling.min_level = smallest < largest ? smallest : largest;
    sampling.max_level = largest;
  }
  gx_backend->texture_bind(map, texture, &sampling);
}

static void bind_texture(int map) {
  uint32_t address = gx.bp[map < 4 ? BP_TEXTURE_ADDRESS + map : BP_TEXTURE_ADDRESS_HIGH + map - 4] & 0xffffff;
  uint32_t image = gx.bp[map < 4 ? BP_TEXTURE_IMAGE + map : BP_TEXTURE_IMAGE_HIGH + map - 4];
  uint32_t palette = gx.bp[map < 4 ? BP_TEXTURE_PALETTE + map : BP_TEXTURE_PALETTE_HIGH + map - 4];
  GxTexture *copied = gx_copy_find(map);
  if (copied) {
    use_texture(map, copied, 0);
    return;
  }
  CacheEntry *entry = &cache[cache_slot(address, image, palette)];
  if (!entry->texture) entry->texture = gx_backend->texture_create();
  use_texture(map, entry->texture, 1);
  // Checking for changed pictures once a frame is plenty.
  if (entry->checked_frame != frames + 1) {
    entry->checked_frame = frames + 1;
    DecodedTexture texture = gx_decode_texture(map, &entry->key);
    if (texture.rgba) gx_backend->texture_upload(entry->texture, texture.width, texture.height, texture.levels, texture.rgba);
  }
}

// No graphics API this runs on everywhere has logic operations, so they are blending that
// gets as close as it can, with the shader's help where the stages' color is replaced. This
// is Dolphin's table from RenderState.cpp, exact for clear, copy, no-op, invert, inverted
// copy and set.
static void approximate_logic(GxRenderState *state, int mode) {
  static const struct { uint8_t on, subtract, source, target; } table[16] = {
    {0, 0, GX_FACTOR_ONE, GX_FACTOR_ZERO},                                    // Clear, with the shader giving 0.
    {1, 0, GX_FACTOR_TARGET_COLOR, GX_FACTOR_ZERO},                           // And.
    {1, 1, GX_FACTOR_ONE, GX_FACTOR_INVERSE_SOURCE_COLOR},                    // And reverse.
    {0, 0, GX_FACTOR_ONE, GX_FACTOR_ZERO},                                    // Copy.
    {1, 1, GX_FACTOR_TARGET_COLOR, GX_FACTOR_ONE},                            // And inverted.
    {1, 0, GX_FACTOR_ZERO, GX_FACTOR_ONE},                                    // No-op.
    {1, 0, GX_FACTOR_INVERSE_TARGET_COLOR, GX_FACTOR_INVERSE_SOURCE_COLOR},   // Xor.
    {1, 0, GX_FACTOR_INVERSE_TARGET_COLOR, GX_FACTOR_ONE},                    // Or.
    {1, 0, GX_FACTOR_INVERSE_TARGET_COLOR, GX_FACTOR_INVERSE_SOURCE_COLOR},   // Nor.
    {1, 0, GX_FACTOR_INVERSE_TARGET_COLOR, GX_FACTOR_ZERO},                   // Equivalent.
    {1, 0, GX_FACTOR_INVERSE_TARGET_COLOR, GX_FACTOR_ZERO},                   // Invert, with the shader giving 1.
    {1, 0, GX_FACTOR_ONE, GX_FACTOR_INVERSE_TARGET_ALPHA},                    // Or reverse.
    {0, 0, GX_FACTOR_ONE, GX_FACTOR_ZERO},                                    // Inverted copy, with the shader inverting.
    {1, 0, GX_FACTOR_INVERSE_TARGET_COLOR, GX_FACTOR_ONE},                    // Or inverted.
    {1, 0, GX_FACTOR_INVERSE_TARGET_COLOR, GX_FACTOR_INVERSE_SOURCE_COLOR},   // Nand.
    {0, 0, GX_FACTOR_ONE, GX_FACTOR_ZERO},                                    // Set, with the shader giving 1.
  };
  static const uint16_t exact = (1u << 0) | (1u << 3) | (1u << 5) | (1u << 10) | (1u << 12) | (1u << 15);
  if (!((exact >> mode) & 1)) debug_unsupported("pixel.logic_ops_16", "logic operation, approximated by blending", (uint32_t)mode);
  state->blend_on = table[mode].on;
  state->blend_subtract = state->blend_subtract_alpha = table[mode].subtract;
  state->blend_source = state->blend_source_alpha = table[mode].source;
  state->blend_target = state->blend_target_alpha = table[mode].target;
}

static void apply_blending(void) {
  // Blending reads the color stages' alpha, kept apart from the alpha that gets stored.
  static const uint8_t source_factors[8] = {GX_FACTOR_ZERO, GX_FACTOR_ONE, GX_FACTOR_TARGET_COLOR, GX_FACTOR_INVERSE_TARGET_COLOR,
                                            GX_FACTOR_SOURCE1_ALPHA, GX_FACTOR_INVERSE_SOURCE1_ALPHA, GX_FACTOR_TARGET_ALPHA, GX_FACTOR_INVERSE_TARGET_ALPHA};
  static const uint8_t target_factors[8] = {GX_FACTOR_ZERO, GX_FACTOR_ONE, GX_FACTOR_SOURCE_COLOR, GX_FACTOR_INVERSE_SOURCE_COLOR,
                                            GX_FACTOR_SOURCE1_ALPHA, GX_FACTOR_INVERSE_SOURCE1_ALPHA, GX_FACTOR_TARGET_ALPHA, GX_FACTOR_INVERSE_TARGET_ALPHA};
  static const uint8_t source_alpha_factors[8] = {GX_FACTOR_ZERO, GX_FACTOR_ONE, GX_FACTOR_TARGET_ALPHA, GX_FACTOR_INVERSE_TARGET_ALPHA,
                                                  GX_FACTOR_SOURCE1_ALPHA, GX_FACTOR_INVERSE_SOURCE1_ALPHA, GX_FACTOR_TARGET_ALPHA, GX_FACTOR_INVERSE_TARGET_ALPHA};
  static const uint8_t target_alpha_factors[8] = {GX_FACTOR_ZERO, GX_FACTOR_ONE, GX_FACTOR_SOURCE1_ALPHA, GX_FACTOR_INVERSE_SOURCE1_ALPHA,
                                                  GX_FACTOR_SOURCE1_ALPHA, GX_FACTOR_INVERSE_SOURCE1_ALPHA, GX_FACTOR_TARGET_ALPHA, GX_FACTOR_INVERSE_TARGET_ALPHA};
  uint32_t blend = gx.bp[BP_BLEND], z = gx.bp[BP_Z_MODE];
  // A picture with no alpha keeps none, and reads as full where blending asks, as in
  // Dolphin's RenderState.cpp. A constant output alpha replaces what is stored without blending.
  int target_alpha = gx_target_has_alpha(), constant_alpha = gx_constant_alpha_used();
  GxRenderState state;
  memset(&state, 0, sizeof state);
  state.blend_on = blend & 1;
  // Alpha blends with the same factors as color, with color factors reading alpha.
  // Mii face masks are built on that: two passes add up coverage in the alpha.
  if ((blend >> BLEND_SUBTRACT_BIT) & 1) {
    state.blend_subtract = state.blend_subtract_alpha = 1;
    state.blend_source = state.blend_target = state.blend_source_alpha = state.blend_target_alpha = GX_FACTOR_ONE;
    if (constant_alpha) {
      state.blend_subtract_alpha = 0;
      state.blend_target_alpha = GX_FACTOR_ZERO;
    }
  } else {
    uint32_t source = (blend >> 8) & 7, target = (blend >> 5) & 7;
    state.blend_source = source_factors[source];
    state.blend_target = target_factors[target];
    if (!target_alpha) {
      if (state.blend_source == GX_FACTOR_TARGET_ALPHA || state.blend_source == GX_FACTOR_INVERSE_TARGET_ALPHA) {
        state.blend_source = state.blend_source == GX_FACTOR_TARGET_ALPHA ? GX_FACTOR_ONE : GX_FACTOR_ZERO;
      }
      if (state.blend_target == GX_FACTOR_TARGET_ALPHA || state.blend_target == GX_FACTOR_INVERSE_TARGET_ALPHA) {
        state.blend_target = state.blend_target == GX_FACTOR_TARGET_ALPHA ? GX_FACTOR_ONE : GX_FACTOR_ZERO;
      }
    }
    state.blend_source_alpha = constant_alpha ? GX_FACTOR_ONE : source_alpha_factors[source];
    state.blend_target_alpha = constant_alpha ? GX_FACTOR_ZERO : target_alpha_factors[target];
  }
  state.depth_test_on = z & 1;
  state.depth_test = (z >> 1) & 7;
  state.depth_write = (z >> 4) & 1;
  state.color_write = (blend >> 3) & 1;
  state.alpha_write = ((blend >> 4) & 1) && target_alpha;
  if (!state.blend_on && ((blend >> LOGIC_ENABLE_BIT) & 1)) {
    int mode = (int)((blend >> LOGIC_MODE_SHIFT) & 15);
    // As Dolphin does, a no-op only stops writing, though a constant alpha still lands.
    if (mode == LOGIC_NOOP) {
      state.color_write = 0;
      state.alpha_write = state.alpha_write && constant_alpha;
    } else {
      approximate_logic(&state, mode);
    }
  }
  state.cull = gx_expanding ? GX_CULL_NONE : (gx.bp[BP_GEN_MODE] >> 14) & 3;  // As in Dolphin.
  // Without a second output the stages' alpha is the output's own, the same number.
  extern TevKey gx_last_tev;
  if (!gx_blend_needs_second_output(&gx_last_tev)) {
    uint8_t *factors[4] = {&state.blend_source, &state.blend_target, &state.blend_source_alpha, &state.blend_target_alpha};
    for (int i = 0; i < 4; i++) {
      if (*factors[i] == GX_FACTOR_SOURCE1_ALPHA) *factors[i] = GX_FACTOR_SOURCE_ALPHA;
      else if (*factors[i] == GX_FACTOR_INVERSE_SOURCE1_ALPHA) *factors[i] = GX_FACTOR_INVERSE_SOURCE_ALPHA;
    }
  }
  gx_backend->render_state(&state);
}

static float xf_float(int address) {
  union { uint32_t bits; float value; } word = {gx.xf[address]};
  return word.value;
}

static void viewport_depth(float *depth_near, float *depth_far);

// Fog that a draw's depths prove adds nothing needn't be worked out for every pixel. Pixels
// lie between their triangle's vertices in depth, and the viewport maps that range.
static int fog_adds_nothing(int count) {
  enum { MOST_VERTICES = 1024 };
  float low, high, depth_near, depth_far;
  if (count > MOST_VERTICES || !gx_transform_depths(gx_reserved, count, &low, &high)) return 0;
  if (low < 0) low = 0;
  if (high > 1) high = 1;
  if (low > high) return 0;
  viewport_depth(&depth_near, &depth_far);
  double a = depth_near + (double)(depth_far - depth_near) * low, b = depth_near + (double)(depth_far - depth_near) * high;
  return gx_fog_none_between(a < b ? a : b, a < b ? b : a);
}

// The game's viewport is in pixels of its own frame.
static void apply_viewport(void) {
  float half_width = xf_float(XF_VIEWPORT), half_height = -xf_float(XF_VIEWPORT + 1);
  float center_x = xf_float(XF_VIEWPORT + 3) - VIEWPORT_ORIGIN, center_y = xf_float(XF_VIEWPORT + 4) - VIEWPORT_ORIGIN;
  GxViewport viewport = {center_x - half_width, center_y - half_height, half_width * 2, half_height * 2, 0, 1};
  viewport_depth(&viewport.depth_near, &viewport.depth_far);
  gx_backend->viewport(&viewport);
}

// The chip's depth is far + range * z / w, from 0 to 2^24. Dolphin's BPFunctions.cpp works the
// same two ends out. Ends outside the depth buffer's range would need the shader to do it.
static void viewport_depth(float *depth_near, float *depth_far) {
  float range = xf_float(XF_VIEWPORT + VIEWPORT_Z_RANGE), far = xf_float(XF_VIEWPORT + VIEWPORT_FAR);
  *depth_near = (far - range) / DEPTH_STEPS;
  *depth_far = far / DEPTH_STEPS;
  if (!(*depth_near >= 0 && *depth_near <= 1 && *depth_far >= 0 && *depth_far <= 1)) {
    debug_unsupported("pixel.depth_range", "viewport depth past the depth buffer", gx.xf[XF_VIEWPORT + VIEWPORT_FAR]);
    *depth_near = *depth_near > 0 ? (*depth_near < 1 ? *depth_near : 1) : 0;
    *depth_far = *depth_far > 0 ? (*depth_far < 1 ? *depth_far : 1) : 0;
  }
}

static void apply_scissor(void) {
  uint32_t first = gx.bp[BP_SCISSOR_FIRST], last = gx.bp[BP_SCISSOR_LAST], offset = gx.bp[BP_SCISSOR_OFFSET];
  int offset_x = (int)(offset & 0x3ff) * 2, offset_y = (int)((offset >> 10) & 0x3ff) * 2;
  int left = (int)((first >> 12) & 0x7ff) - offset_x, top = (int)(first & 0x7ff) - offset_y;
  int right = (int)((last >> 12) & 0x7ff) - offset_x + 1, bottom = (int)(last & 0x7ff) - offset_y + 1;
  GxRectangle scissor = {left, top, right - left, bottom - top};
  gx_backend->scissor(&scissor);
}

int gx_draw_wanted(int primitive, int count) {
  if (primitive == PRIM_QUADS || primitive == PRIM_QUADS2) return count >= VERTICES_PER_QUAD;
  return (primitive == PRIM_TRIANGLES || primitive == PRIM_STRIP || primitive == PRIM_FAN) && count >= 3;
}

Vertex *gx_reserved;
int gx_vertex_format;
TevKey gx_last_tev;
unsigned gx_used_maps;
int gx_bounds_known;
float gx_bounds[4];
enum { BOUNDED_VERTICES = 128 };

Vertex *gx_draw_reserve(int count) {
  if (!started) start();
  return gx_reserved = gx_backend->vertices_reserve(count);
}

// Whether the color stages must give full alpha, whatever the texture holds. Only one
// plain stage is worked out, and anything else counts as not known.
int gx_output_is_opaque(void) {
  const TevKey *key = &gx_last_tev;
  if (key->stages != 1 || key->alpha_test || key->early_depth || key->constant_alpha) return 0;
  const TevStage *s = &key->stage[0];
  if (s->alpha_subtract || s->alpha_bias || s->alpha_scale || s->alpha_target) return 0;
  float least[4];
  for (int i = 0; i < 4; i++) least[i] = s->alpha_in[i] < 4 ? gx_last_registers_alpha[s->alpha_in[i]] : 0;
  // a and b are mixed by c, so the result is never below the lesser of them.
  float mixed = least[0] < least[1] ? least[0] : least[1];
  return mixed + least[3] >= 0.999f;
}

// Draws the vertices just decoded into the reserved room.
void gx_draw(int primitive, int count) {
  if (!gx_draw_wanted(primitive, count)) {
    // Too many were left out. A backend can still have reserved room to let go of.
    gx_backend->draw(PRIM_TRIANGLES, 0);
    return;
  }
  TevKey tev;
  VertexKey vertex;
  unsigned maps = gx_tev_apply(&tev);
  static int keep_fog = -1;
  if (keep_fog < 0) keep_fog = getenv("WII_RECOMP_KEEP_FOG") != 0;
  if (tev.fog_type && !tev.depth_mode && !tev.fog_range && !keep_fog) tev.fog_none = (uint8_t)fog_adds_nothing(count);
  gx_transform_read(&tev, &vertex);
  gx_transform_apply(&vertex);
  gx_shader_use(&tev, &vertex);
  gx_last_tev = tev;
  gx_used_maps = maps;
  for (int map = 0; map < MAX_TEXTURE_MAPS; map++) {
    if (maps & (1u << map)) bind_texture(map);
  }
  apply_blending();
  apply_viewport();
  apply_scissor();
  if (gx_inspect_draw(&tev, maps, gx_vertex_format, primitive, count, gx_reserved)) gx_backend->draw(primitive, count);
}

int gx_target_has_alpha(void) {
  uint32_t format = gx.bp[BP_PIXEL_FORMAT] & PIXEL_FORMAT_MASK;
  // The picture's pixels aren't turned into the new format's, as the chip and Dolphin do.
  static uint32_t last_format = PIXEL_FORMAT_UNSEEN;
  if (format != last_format) {
    int had_alpha = last_format == PIXEL_FORMAT_RGBA6, has_alpha = format == PIXEL_FORMAT_RGBA6;
    if (last_format != PIXEL_FORMAT_UNSEEN && had_alpha != has_alpha && format != PIXEL_FORMAT_DEPTH) debug_unsupported("pixel.reinterpreting_efb_after_a_format_change", "pixel format change, read without conversion", format);
    last_format = format;
  }
  return format == PIXEL_FORMAT_RGBA6;
}

// The stored alpha leaves the stages' only for a constant alpha or a logic operation's help.
int gx_blend_needs_second_output(const TevKey *key) { return key->constant_alpha || key->logic_help; }

int gx_constant_alpha_used(void) {
  return ((gx.bp[BP_CONSTANT_ALPHA] >> CONSTANT_ALPHA_ENABLE_BIT) & 1) && ((gx.bp[BP_BLEND] >> 4) & 1) && gx_target_has_alpha();
}

int gx_expanding;
float gx_expand_size[2];
static Vertex expand_from[MAX_VERTICES];

// Corners keep -1, 0 or 1 on each axis as 0 to 2, in a spare matrix slot the shader reads.
static void put_corner(Vertex *to, const Vertex *from, int x, int y) {
  *to = *from;
  to->matrix[VERTEX_CORNER] = (uint8_t)((x + 1) | ((y + 1) << 2));
}

// Texture coordinates nudged across a line or point aren't done.
static void report_unsupported_expansion(int lines, uint32_t sizes) {
  int shift = lines ? LINE_OFFSET_SHIFT : POINT_OFFSET_SHIFT, bit = lines ? LINE_OFFSET_BIT : POINT_OFFSET_BIT;
  if ((sizes >> shift) & 7) {
    for (int i = 0; i < MAX_TEXTURE_MAPS; i++) {
      if ((gx.bp[BP_COORDINATE_SCALE + i * 2] >> bit) & 1) debug_unsupported(lines ? "gx.draw_lines_line_strip" : "gx.draw_points", lines ? "line texture offset" : "point texture offset", (sizes >> shift) & 7);
    }
  }
  if ((sizes >> ASPECT_ADJUST_BIT) & 1) debug_unsupported("gx.draw_lines_line_strip", "line and point aspect adjustment", sizes);
}

// Lines and points become quads, as in Dolphin's GeometryShaderGen.cpp and ShaderGenCommon.cpp.
// A line is widened up and down, or left and right if it runs more tall than wide in
// pixels, so its ends are cut square to the screen rather than to the line. Sizes count in
// sixths of a pixel. Which way to widen is worked out here, and the shader moves each corner.
int gx_draw_expanded(int primitive, int format, const uint8_t *data, int count) {
  if (primitive != PRIM_LINES && primitive != PRIM_LINE_STRIP && primitive != PRIM_POINTS) return 0;
  int lines = primitive != PRIM_POINTS, kept = gx_read_vertices(data, format, count, expand_from);
  int shapes = primitive == PRIM_LINES ? kept / 2 : primitive == PRIM_LINE_STRIP ? kept - 1 : kept;
  if (shapes <= 0) return 1;
  if (!started) start();
  uint32_t sizes = gx.bp[BP_LINE_POINT_SIZE];
  report_unsupported_expansion(lines, sizes);
  float size = (float)((sizes >> (lines ? 0 : POINT_SIZE_SHIFT)) & 255) / SIXTHS;
  float half_width = fabsf(xf_float(XF_VIEWPORT)), half_height = fabsf(xf_float(XF_VIEWPORT + 1));
  if (half_width <= 0 || half_height <= 0) return 1;
  gx_expand_size[0] = size / (2 * half_width);
  gx_expand_size[1] = size / (2 * half_height);
  gx_expanding = lines ? EXPAND_LINES : EXPAND_POINTS;
  for (int first = 0; first < shapes; first += EXPANDED_QUADS) {
    int part = shapes - first < EXPANDED_QUADS ? shapes - first : EXPANDED_QUADS;
    Vertex *out = gx_draw_reserve(part * VERTICES_PER_QUAD);
    for (int i = 0; i < part; i++, out += VERTICES_PER_QUAD) {
      int shape = first + i;
      if (!lines) {
        const Vertex *p = &expand_from[shape];
        put_corner(&out[0], p, -1, -1);
        put_corner(&out[1], p, 1, -1);
        put_corner(&out[2], p, 1, 1);
        put_corner(&out[3], p, -1, 1);
        continue;
      }
      const Vertex *a = &expand_from[primitive == PRIM_LINES ? shape * 2 : shape], *b = a + 1;
      float ax = 0, ay = 0, bx = 0, by = 0;
      gx_transform_screen(a, &ax, &ay);
      gx_transform_screen(b, &bx, &by);
      int tall = fabsf(ay - by) * half_height > fabsf(ax - bx) * half_width;
      int x = tall ? 1 : 0, y = tall ? 0 : 1;
      put_corner(&out[0], a, -x, -y);
      put_corner(&out[1], a, x, y);
      put_corner(&out[2], b, x, y);
      put_corner(&out[3], b, -x, -y);
    }
    gx_draw(PRIM_QUADS, part * VERTICES_PER_QUAD);
  }
  gx_expanding = 0;
  return 1;
}

void gx_clear(void) {
  if (!started) start();
  uint32_t ar = gx.bp[BP_CLEAR_COLOR_AR], gb = gx.bp[BP_CLEAR_COLOR_GB], blend = gx.bp[BP_BLEND];
  GxClear clear;
  gx_copy_rectangle(&clear.area.x, &clear.area.y, &clear.area.width, &clear.area.height);
  clear.color[0] = (float)(ar & 255) / 255;
  clear.color[1] = (float)((gb >> 8) & 255) / 255;
  clear.color[2] = (float)(gb & 255) / 255;
  clear.color[3] = (float)((ar >> 8) & 255) / 255;
  clear.depth = (double)(gx.bp[BP_CLEAR_DEPTH] & 0xffffff) / 0xffffff;
  clear.color_write = (blend >> 3) & 1;
  clear.alpha_write = ((blend >> 4) & 1) && gx_target_has_alpha();  // As in Dolphin's ClearScreen.
  clear.depth_write = (gx.bp[BP_Z_MODE] >> 4) & 1;
  gx_backend->clear(&clear);
}

// With WII_RECOMP_FPS_LOG set, the frames shown in each second of real time.
static void log_frame_rate(void) {
  static int wanted = -1;
  static struct timespec second_began;
  static unsigned shown;
  if (wanted < 0) wanted = getenv("WII_RECOMP_FPS_LOG") != 0;
  if (!wanted) return;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (!second_began.tv_sec) second_began = now;
  shown++;
  double seconds = (double)(now.tv_sec - second_began.tv_sec) + (now.tv_nsec - second_began.tv_nsec) / 1e9;
  if (seconds < 1) return;
  fprintf(stderr, "fps %.1f\n", shown / seconds);
  shown = 0;
  second_began = now;
}

void gx_present(void) {
  if (!started) start();
  gx_inspect_frame();
  GxRectangle from;
  gx_copy_rectangle(&from.x, &from.y, &from.width, &from.height);
  gx_backend->present(&from);
  if (++frames <= 3 || frames % 300 == 0) fprintf(stderr, "frame %u\n", frames);
  log_frame_rate();
}
