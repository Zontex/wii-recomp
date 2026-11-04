// The Wii's color stages (TEV): up to 16 small mixers that combine textures, vertex
// colors and constants. The game's settings become a key, and each distinct key gets
// a small shader written for it in gx_shader.c. Values, such as registers and
// constants, go to the shader in a block, so changing them needs no new shader.
#include <math.h>
#include <string.h>

#include "debug.h"
#include "gx.h"

#define DEPTH_MAX 16777215.0f

enum {
  BP_GEN_MODE = 0x00, TEXTURE_COORDINATES_MASK = 15, BP_CONSTANT_ALPHA = 0x42, CONSTANT_ALPHA_ENABLE_BIT = 8, BP_TEV_ORDER = 0x28, BP_COLOR_STAGE = 0xc0, BP_ALPHA_STAGE = 0xc1, BP_ALPHA_TEST = 0xf3,
  BP_KONST_SELECT = 0xf6, SWAP_TABLES = 4, COLOR_VALUE_MASK = 0x7ff, STAGE_COUNT_SHIFT = 10, STAGE_COUNT_MASK = 15,
  BP_Z_TEXTURE_BIAS = 0xf4, BP_Z_TEXTURE = 0xf5, BP_Z_MODE = 0x40, Z_TEST_BIT = 0, Z_UPDATE_BIT = 4, BP_PIXEL_FORMAT = 0x43, EARLY_DEPTH_BIT = 6,
  BP_INDIRECT_MATRIX = 0x06, BP_INDIRECT_STAGE = 0x10, BP_INDIRECT_SCALE = 0x25, BP_INDIRECT_SOURCE = 0x27,
  BP_TEXTURE_MODE_LOW = 0x80, BP_TEXTURE_MODE_HIGH = 0xa0, MODE_MAGNIFY_BIT = 4, MODE_MIP_SHIFT = 5, MODE_MINIFY_BIT = 7, MODE_DIAGONAL_BIT = 8, MODE_BIAS_SHIFT = 9, BIAS_STEPS = 32, BIASES_PER_VALUE = 4,
  BP_COORDINATE_SCALE = 0x30, COORDINATE_SCALE_MASK = 0xffff, BP_IMAGE_LOW = 0x88, BP_IMAGE_HIGH = 0xa8, MATRIX_WORDS = 3, MATRIX_VALUE_BITS = 11, MATRIX_ONE = 1024, MATRIX_SCALE_ZERO = 17,
  INDIRECT_COUNT_SHIFT = 16, INDIRECT_COUNT_MASK = 7,
  BP_FOG_RANGE = 0xe8, BP_FOG_A = 0xee, BP_FOG_B_MAGNITUDE = 0xef, BP_FOG_B_SHIFT = 0xf0, BP_FOG_C = 0xf1, BP_FOG_COLOR = 0xf2,
  FOG_PROJECTION_BIT = 20, FOG_TYPE_SHIFT = 21, FOG_RANGE_ENABLE_BIT = 10, FOG_RANGE_WORDS = 5, FOG_RANGE_CENTER_MASK = 0x3ff,
  FOG_OFF = 0, FOG_LINEAR = 2, FOG_EXP = 4, FOG_EXP2 = 5, FOG_BACKWARD_EXP = 6, FOG_BACKWARD_EXP2 = 7, FOG_SHIFT_LARGEST = 31,
  XF_VIEWPORT = 0x101a, VIEWPORT_ORIGIN = 342,
  BP_BLEND = 0x41, BLEND_ENABLE_BIT = 0, LOGIC_ENABLE_BIT = 1, LOGIC_MODE_SHIFT = 12,
  LOGIC_CLEAR = 0, LOGIC_INVERT = 10, LOGIC_COPY_INVERTED = 12, LOGIC_SET = 15,
  HELP_NONE = 0, HELP_ZERO = 1, HELP_ONE = 2, HELP_INVERT = 3,
};
enum {
  COLOR_REGISTERS_END = 8, COLOR_TEXTURE = 8, COLOR_TEXTURE_ALPHA = 9, COLOR_RASTER = 10, COLOR_RASTER_ALPHA = 11,
  COLOR_ONE = 12, COLOR_HALF = 13, COLOR_KONST = 14,
  ALPHA_REGISTERS_END = 4, ALPHA_TEXTURE = 4, ALPHA_RASTER = 5, ALPHA_KONST = 6,
  BIAS_ADD_HALF = 1, BIAS_SUBTRACT_HALF = 2, BIAS_COMPARE = 3,
  COMPARE_R8 = 0, COMPARE_GR16 = 1, COMPARE_BGR24 = 2, COMPARE_RGB8 = 3, COMPARE_A8 = 3,
  SCALE_TWO = 1, SCALE_FOUR = 2, SCALE_HALF = 3,
  KONST_FRACTIONS_END = 8, KONST_COLORS_END = 16, KONST_CHANNELS_START = 16,
  ALPHA_NEVER = 0, ALPHA_LESS = 1, ALPHA_EQUAL = 2, ALPHA_LEQUAL = 3, ALPHA_GREATER = 4, ALPHA_NOT_EQUAL = 5,
  ALPHA_GEQUAL = 6, ALPHA_ALWAYS = 7, LOGIC_AND = 0, LOGIC_OR = 1, LOGIC_XOR = 2,
  RASTER_NONE = 2, RASTER_BUMP = 5, RASTER_BUMP_NORMALIZED = 6, RASTER_ZERO = 7,
  DEPTH_OFF = 0, DEPTH_ADD = 1, DEPTH_REPLACE = 2, DEPTH_FORMAT_LOW_BYTE = 0, DEPTH_FORMAT_TWO_BYTES = 1,
  WRAP_NONE = 0, WRAP_ZERO = 6, WRAP_LARGEST = 512,
  INDIRECT_FORMAT_FULL = 0, INDIRECT_BIAS_FULL = -128, INDIRECT_BIAS_SHORT = 1,
  INDIRECT_KIND_FIXED = 0, INDIRECT_KIND_S = 1, INDIRECT_KIND_T = 2, INDIRECT_KINDS = 3,
};
#define SAME_VALUE "0.002"
// What the chip's 11-bit registers hold, in 255ths.
#define REGISTER_RANGE "-1024.0 / 255.0, 1023.0 / 255.0"
#define TEXT_DEPTH_MAX "16777215.0"

static float signed_color(uint32_t bits) {
  int value = (int)(bits & COLOR_VALUE_MASK);
  return (float)(value >= 1024 ? value - 2048 : value) / 255;
}

static void read_colors(const uint32_t *words, float out[4][4]) {
  for (int i = 0; i < 4; i++) {
    uint32_t red_alpha = words[i * 2], blue_green = words[i * 2 + 1];
    out[i][0] = signed_color(red_alpha);
    out[i][1] = signed_color(blue_green >> 12);
    out[i][2] = signed_color(blue_green);
    out[i][3] = signed_color(red_alpha >> 12);
  }
}

static uint8_t swap_table(int table) {
  uint32_t red_green = gx.bp[BP_KONST_SELECT + table * 2], blue_alpha = gx.bp[BP_KONST_SELECT + table * 2 + 1];
  return (uint8_t)((red_green & 3) | ((red_green >> 2) & 3) << 2 | (blue_alpha & 3) << 4 | ((blue_alpha >> 2) & 3) << 6);
}

static int uses_input(const TevStage *s, int color_first, int color_last, int alpha_input) {
  for (int i = 0; i < 4; i++) {
    if ((s->color_in[i] >= color_first && s->color_in[i] <= color_last) || s->alpha_in[i] == alpha_input) return 1;
  }
  return 0;
}

static int stage_coordinate(int i, int coordinates_on) {
  uint32_t order = gx.bp[BP_TEV_ORDER + i / 2] >> (i & 1 ? 12 : 0);
  int coordinate = (int)((order >> 3) & 7);
  return coordinate < coordinates_on ? coordinate : 0;
}

// Fills in a stage's indirect fields, which don't depend on it having a texture. A fetch
// past those enabled gives no offset, as Dolphin chose for what is noise on a Wii, and the
// stage's wrapping and adding on to the last stage still count.
static void read_stage_indirect(TevKey *key, TevStage *s, int i, int fetches, int coordinates_on) {
  uint32_t command = gx.bp[BP_INDIRECT_STAGE + i];
  if (!command) return;
  int fetch = (int)(command & 3), matrix = (int)((command >> 9) & 3), kind = (int)((command >> 11) & 3), bump = (int)((command >> 7) & 3);
  s->indirect = s->indirect_used = 1;
  s->coordinate = (uint8_t)stage_coordinate(i, coordinates_on);
  s->wrap_s = (command >> 13) & 7;
  s->wrap_t = (command >> 16) & 7;
  s->add_previous = (command >> 20) & 1;
  if (kind >= INDIRECT_KINDS) {
    debug_unsupported("gx.0x10_0x1f_indirect_stage_commands", "indirect matrix kind", (uint32_t)kind);
    matrix = 0;
  }
  if (fetch >= fetches || (!matrix && !bump)) return;
  s->indirect_read = 1;
  s->indirect_fetch = (uint8_t)fetch;
  s->indirect_format = (command >> 2) & 3;
  s->bump_alpha = (uint8_t)bump;
  if (bump) key->bump_alpha = 1;
  s->indirect_matrix = (uint8_t)matrix;
  if (matrix) {
    s->indirect_kind = (uint8_t)kind;
    s->indirect_bias = (command >> 4) & 7;
  }
  uint32_t source = gx.bp[BP_INDIRECT_SOURCE] >> (fetch * 6), scale = gx.bp[BP_INDIRECT_SCALE + fetch / 2] >> (fetch & 1 ? 8 : 0);
  int coordinate = (int)((source >> 3) & 7);
  TevIndirectSource *to = &key->indirect_source[fetch];
  to->map = source & 7;
  to->coordinate = (uint8_t)(coordinate < coordinates_on ? coordinate : 0);
  to->shift_s = scale & 15;
  to->shift_t = (scale >> 4) & 15;
}

// Fields a stage doesn't use stay zero, so settings that differ only there share a shader.
// The depth texture is the last stage's, so that stage counts as reading its texture.
// A stage reading a coordinate the game never enabled gets the first one instead, as
// Dolphin found with Luigi's Mansion, and with none at all a stage can't sample.
static void read_stage(TevKey *key, int i, int fetches, int needs_texture, int coordinates_on) {
  TevStage *s = &key->stage[i];
  uint32_t color = gx.bp[BP_COLOR_STAGE + i * 2], alpha = gx.bp[BP_ALPHA_STAGE + i * 2];
  uint32_t order = gx.bp[BP_TEV_ORDER + i / 2] >> (i & 1 ? 12 : 0), select = gx.bp[BP_KONST_SELECT + i / 2];
  int shift = i & 1 ? 14 : 4;
  for (int n = 0; n < 4; n++) {
    s->color_in[n] = (color >> (12 - n * 4)) & 15;
    s->alpha_in[n] = (alpha >> (13 - n * 3)) & 7;
  }
  s->color_bias = (color >> 16) & 3;
  s->color_subtract = (color >> 18) & 1;
  s->color_clamp = (color >> 19) & 1;
  s->color_scale = (color >> 20) & 3;
  s->color_target = (color >> 22) & 3;
  s->alpha_bias = (alpha >> 16) & 3;
  s->alpha_subtract = (alpha >> 18) & 1;
  s->alpha_clamp = (alpha >> 19) & 1;
  s->alpha_scale = (alpha >> 20) & 3;
  s->alpha_target = (alpha >> 22) & 3;
  int texture_on = ((order >> 6) & 1) && coordinates_on;
  read_stage_indirect(key, s, i, fetches, coordinates_on);
  if (needs_texture || uses_input(s, COLOR_TEXTURE, COLOR_TEXTURE_ALPHA, ALPHA_TEXTURE)) {
    s->reads_texture = 1;
    if (texture_on) {
      s->texture_on = 1;
      s->map = order & 7;
      s->coordinate = (uint8_t)stage_coordinate(i, coordinates_on);
      s->texture_swap = swap_table((alpha >> 2) & 3);
    }
  }
  if (uses_input(s, COLOR_RASTER, COLOR_RASTER_ALPHA, ALPHA_RASTER)) {
    uint32_t channel = (order >> 7) & 7;
    s->reads_raster = 1;
    s->raster = channel < RASTER_NONE || channel == RASTER_BUMP || channel == RASTER_BUMP_NORMALIZED ? (uint8_t)channel : RASTER_NONE;
    if (channel >= RASTER_NONE && channel < RASTER_BUMP) debug_unsupported("gx.0x28_0x2f_stage_order", "stage raster source", channel);
    s->raster_swap = swap_table(alpha & 3);
  }
  if (uses_input(s, COLOR_KONST, COLOR_KONST, -1)) s->konst_color = (select >> shift) & 31;
  if (uses_input(s, -1, -1, ALPHA_KONST)) s->konst_alpha = (select >> (shift + 5)) & 31;
}

static int always_passes(int first, int second, int logic) {
  if (logic == LOGIC_AND) return first == ALPHA_ALWAYS && second == ALPHA_ALWAYS;
  return logic == LOGIC_OR && (first == ALPHA_ALWAYS || second == ALPHA_ALWAYS);
}

// Most textures have no bias, so a shader only reads one for a map that does. It stays out
// of the sampler as Metal has no such setting there, and with no mip filter it has no effect.
static void read_map_biases(TevKey *key, TevValues *values, unsigned maps) {
  for (int map = 0; map < MAX_TEXTURE_MAPS; map++) {
    if (!(maps & (1u << map))) continue;
    uint32_t mode = gx.bp[map < 4 ? BP_TEXTURE_MODE_LOW + map : BP_TEXTURE_MODE_HIGH + map - 4];
    int8_t bias = (int8_t)(mode >> MODE_BIAS_SHIFT);
    // The SDK sets the diagonal bit on nearly every texture, but the level of detail only
    // changes the result where it picks a mip level or chooses between a magnify and a
    // minify filter that differ. Elsewhere the plain lookup is the same and much cheaper:
    // textureGrad costs a single full-screen draw 5 ms on the Pi 5's GPU.
    int levels_matter = ((mode >> MODE_MIP_SHIFT) & 3) || ((mode >> MODE_MAGNIFY_BIT) & 1) != ((mode >> MODE_MINIFY_BIT) & 1);
    if (((mode >> MODE_DIAGONAL_BIT) & 1) && levels_matter) key->diagonal_maps |= (uint8_t)(1u << map);
    if (!((mode >> MODE_MIP_SHIFT) & 3) || !bias) continue;
    key->biased_maps |= (uint8_t)(1u << map);
    values->map_bias[map / BIASES_PER_VALUE][map % BIASES_PER_VALUE] = (float)bias / BIAS_STEPS;
  }
}

// Fog's A and C are floats with 11 bits of fraction. When both are infinite or not
// numbers, the chip fogs fully only if both are positive. See Dolphin's BPMemory.cpp.
static float fog_float(uint32_t word) {
  union { uint32_t bits; float value; } f = {(((word >> 19) & 1) << 31) | (((word >> 11) & 255) << 23) | ((word & 0x7ff) << 12)};
  return f.value;
}

static float xf_value(int address) {
  union { uint32_t bits; float value; } f = {gx.xf[address]};
  return f.value;
}

// Whether fog adds nothing anywhere between two depths, from 0 to 1. Its distance only
// rises or only falls across them, so the ends decide, with room for rounding. The shader
// mixes by the fog rounded to 256ths, so under half of one leaves every pixel as it was;
// the curves all rise with the distance and are no more than 8 ln 2 times as steep.
int gx_fog_none_between(double low, double high) {
  uint32_t a = gx.bp[BP_FOG_A], c = gx.bp[BP_FOG_C];
  int type = (int)((c >> FOG_TYPE_SHIFT) & 7);
  if (type == FOG_OFF || type == FOG_BACKWARD_EXP || type == FOG_BACKWARD_EXP2) return 0;
  double fog_a = fog_float(a), fog_c = fog_float(c);
  if (((a >> 11) & 255) == 255 && ((c >> 11) & 255) == 255) {
    fog_a = 0;
    fog_c = !((a >> 19) & 1) && !((c >> 19) & 1) ? -INFINITY : INFINITY;
  }
  uint32_t shift = gx.bp[BP_FOG_B_SHIFT] & 0xffffff, b = gx.bp[BP_FOG_B_MAGNITUDE] & 0xffffff;
  if (shift > FOG_SHIFT_LARGEST) shift = FOG_SHIFT_LARGEST;
  enum { ROOM = 256, DEPTH_LARGEST = 0xffffff };
  double ends[2] = {floor(low * 16777216.0) - ROOM, ceil(high * 16777216.0) + ROOM}, most = -INFINITY;
  for (int i = 0; i < 2; i++) {
    double depth = ends[i] < 0 ? 0 : ends[i] > DEPTH_LARGEST ? DEPTH_LARGEST : ends[i], distance;
    if ((c >> FOG_PROJECTION_BIT) & 1) {
      distance = fog_a * depth / 16777216.0;
    } else {
      double below = (double)b - (double)((uint32_t)depth >> shift);
      if (!(below > 0)) return 0;
      distance = fog_a * 16777216.0 / below;
    }
    if (distance > most) most = distance;
  }
  // Linear fog reaches the shader's half step at 1/512, and the curves sooner, by at most
  // 8 ln 2 (EXP), with a tenth of a step to spare.
  double half_step = 0.4 / 256 / (type == FOG_LINEAR ? 1 : 8 * 0.6931471805599453);
  return most - fog_c < half_step;
}

// Fog mixes the picture toward a color by how far each pixel is, worked out from its depth
// as Dolphin's PixelShaderGen does. Most draws have none and their shaders never see it.
static void read_fog(TevKey *key, TevValues *values) {
  uint32_t a = gx.bp[BP_FOG_A], c = gx.bp[BP_FOG_C], color = gx.bp[BP_FOG_COLOR], range = gx.bp[BP_FOG_RANGE];
  int type = (int)((c >> FOG_TYPE_SHIFT) & 7);
  if (type == FOG_OFF) return;
  if (type == 1 || type == 3) debug_unsupported("pixel.fog_linear_exp_exp2_backward_exp_backwar", "fog type, drawn as linear", (uint32_t)type);
  key->fog_type = (uint8_t)type;
  key->fog_orthographic = (c >> FOG_PROJECTION_BIT) & 1;
  float fog_a = fog_float(a), fog_c = fog_float(c);
  if (((a >> 11) & 255) == 255 && ((c >> 11) & 255) == 255) {
    fog_a = 0;
    fog_c = !((a >> 19) & 1) && !((c >> 19) & 1) ? -INFINITY : INFINITY;
  }
  uint32_t shift = gx.bp[BP_FOG_B_SHIFT] & 0xffffff;
  values->fog_color[0] = (float)((color >> 16) & 255);
  values->fog_color[1] = (float)((color >> 8) & 255);
  values->fog_color[2] = (float)(color & 255);
  values->fog_settings[0] = fog_a;
  values->fog_settings[1] = fog_c;
  values->fog_settings[2] = (float)(gx.bp[BP_FOG_B_MAGNITUDE] & 0xffffff);
  values->fog_settings[3] = (float)(shift > FOG_SHIFT_LARGEST ? FOG_SHIFT_LARGEST : shift);
  if (!((range >> FOG_RANGE_ENABLE_BIT) & 1)) return;
  // Dolphin calls its reading of these a guess: each word holds two of ten values, low then
  // high, in 256ths, which it scales by 4.
  key->fog_range = 1;
  values->fog_range_center[0] = (float)((int)(range & FOG_RANGE_CENTER_MASK) - VIEWPORT_ORIGIN);
  values->fog_range_center[1] = xf_value(XF_VIEWPORT);
  for (int i = 0; i < FOG_RANGE_WORDS; i++) {
    uint32_t k = gx.bp[BP_FOG_RANGE + 1 + i];
    values->fog_range_k[(i * 2) / 4][(i * 2) % 4] = (float)((k >> 12) & 0xfff) / 256 * 4;
    values->fog_range_k[(i * 2 + 1) / 4][(i * 2 + 1) % 4] = (float)(k & 0xfff) / 256 * 4;
  }
}

// Logic operations are blending, which can't make a color of its own. See approximate_logic in gx_draw.c.
static void read_logic(TevKey *key) {
  uint32_t blend = gx.bp[BP_BLEND];
  if (((blend >> BLEND_ENABLE_BIT) & 1) || !((blend >> LOGIC_ENABLE_BIT) & 1)) return;
  int mode = (int)((blend >> LOGIC_MODE_SHIFT) & 15);
  if (mode == LOGIC_CLEAR) key->logic_help = HELP_ZERO;
  else if (mode == LOGIC_SET || mode == LOGIC_INVERT) key->logic_help = HELP_ONE;
  else if (mode == LOGIC_COPY_INVERTED) key->logic_help = HELP_INVERT;
}

static float matrix_value(uint32_t word, int shift) {
  int value = (int)((word >> shift) & ((1u << MATRIX_VALUE_BITS) - 1));
  return (float)(value >= MATRIX_ONE ? value - 2 * MATRIX_ONE : value) / MATRIX_ONE;
}

static void read_indirect_matrix(TevValues *values, int i) {
  const uint32_t *words = &gx.bp[BP_INDIRECT_MATRIX + i * MATRIX_WORDS];
  int exponent = 0;
  for (int w = 0; w < MATRIX_WORDS; w++) exponent |= (int)((words[w] >> 22) & 3) << (w * 2);
  float scale = ldexpf(1, exponent - MATRIX_SCALE_ZERO);
  for (int w = 0; w < MATRIX_WORDS; w++) {
    values->indirect_row_s[i][w] = matrix_value(words[w], 0) * scale;
    values->indirect_row_t[i][w] = matrix_value(words[w], MATRIX_VALUE_BITS) * scale;
  }
  values->indirect_row_s[i][MATRIX_WORDS] = scale;  // For the kinds that only take the matrix's scale.
}

static void read_map_size(TevValues *values, int map) {
  uint32_t image = gx.bp[map < 4 ? BP_IMAGE_LOW + map : BP_IMAGE_HIGH + map - 4];
  values->map_size[map][0] = (float)((image & 0x3ff) + 1);
  values->map_size[map][1] = (float)(((image >> 10) & 0x3ff) + 1);
}

// A coordinate reaches the chip's pixels through a scale of its own, usually its texture's size.
static void read_coordinate_scale(TevValues *values, int coordinate) {
  values->map_size[coordinate][2] = (float)((gx.bp[BP_COORDINATE_SCALE + coordinate * 2] & COORDINATE_SCALE_MASK) + 1);
  values->map_size[coordinate][3] = (float)((gx.bp[BP_COORDINATE_SCALE + coordinate * 2 + 1] & COORDINATE_SCALE_MASK) + 1);
}

// A stage can move its texture coordinate by amounts read from another texture
// through a small matrix. The game's lighting tables are made this way: normals
// come from a texture and the matrix holds the light.
static void read_indirect_values(const TevKey *key, TevValues *values) {
  for (int i = 0; i < key->stages; i++) {
    const TevStage *s = &key->stage[i];
    if (!s->indirect) continue;
    read_coordinate_scale(values, s->coordinate);
    if (s->indirect_matrix) read_indirect_matrix(values, s->indirect_matrix - 1);
    if (s->texture_on) read_map_size(values, s->map);
    if (s->indirect_read) {
      const TevIndirectSource *source = &key->indirect_source[s->indirect_fetch];
      read_map_size(values, source->map);
      read_coordinate_scale(values, source->coordinate);
    }
  }
}

// A stage can replace a pixel's depth with its texture's value. The game uses that
// to reset depth from a flat texture, and to put a depth copy back after a layer.
// Whether depth is tested before the color stages run (early z), as the SDK sets it unless told
// otherwise. Bits from Dolphin's BPMemory.h.
static int tests_depth_early(void) {
  return ((gx.bp[BP_PIXEL_FORMAT] >> EARLY_DEPTH_BIT) & 1) && ((gx.bp[BP_Z_MODE] >> Z_TEST_BIT) & 1);
}

static void read_depth(TevKey *key, TevValues *values, int last_map) {
  uint32_t mode = gx.bp[BP_Z_TEXTURE], chosen = (mode >> 2) & 3;
  if (chosen == DEPTH_OFF) return;
  // Depth tested first is stored before any texture could change it, as Dolphin's
  // PixelShaderGen.cpp has it. Fog would still see the texture's depth there.
  if (tests_depth_early()) {
    if ((gx.bp[BP_FOG_C] >> FOG_TYPE_SHIFT) & 7) debug_unsupported("pixel.z_compare_location_early_z", "depth texture seen by fog after an early depth test", chosen);
    return;
  }
  key->depth_mode = chosen == DEPTH_ADD ? DEPTH_ADD : DEPTH_REPLACE;
  key->depth_from_copy = gx_copy_is_depth(last_map) ? 1 : 0;
  if (!key->depth_from_copy) key->depth_format = mode & 3;
  values->alpha_settings[3] = (float)(gx.bp[BP_Z_TEXTURE_BIAS] & 0xffffff) / DEPTH_MAX;
}

unsigned gx_tev_read(TevKey *key, TevValues *values) {
  unsigned coordinates, channels, maps;
  memset(key, 0, sizeof *key);
  memset(values, 0, sizeof *values);
  key->stages = (uint8_t)(((gx.bp[BP_GEN_MODE] >> STAGE_COUNT_SHIFT) & STAGE_COUNT_MASK) + 1);
  int last = key->stages - 1, fetches = (int)((gx.bp[BP_GEN_MODE] >> INDIRECT_COUNT_SHIFT) & INDIRECT_COUNT_MASK);
  int coordinates_on = (int)(gx.bp[BP_GEN_MODE] & TEXTURE_COORDINATES_MASK);
  key->no_coordinates = !coordinates_on;
  read_depth(key, values, (int)((gx.bp[BP_TEV_ORDER + last / 2] >> (last & 1 ? 12 : 0)) & 7));
  for (int i = 0; i < key->stages; i++) read_stage(key, i, fetches, key->depth_mode && i == last, coordinates_on);
  // A stage's coordinate only matters to the one after if that one adds on to it.
  for (int i = last; i > 0; i--) {
    TevStage *before = &key->stage[i - 1];
    if (!key->stage[i].add_previous || before->indirect) continue;
    before->indirect = 1;
    before->coordinate = (uint8_t)stage_coordinate(i - 1, coordinates_on);
  }
  key->wrap_output = !key->stage[last].color_clamp || !key->stage[last].alpha_clamp;
  read_indirect_values(key, values);
  read_colors(gx.tev_color, values->registers);
  read_colors(gx.tev_konst, values->constants);
  uint32_t test = gx.bp[BP_ALPHA_TEST];
  int first = (test >> 16) & 7, second = (test >> 19) & 7, logic = (test >> 22) & 3;
  if (!always_passes(first, second, logic)) {
    key->alpha_test = 1;
    key->alpha_first = (uint8_t)first;
    key->alpha_second = (uint8_t)second;
    key->alpha_logic = (uint8_t)logic;
    values->alpha_settings[0] = (float)(test & 255) / 255;
    values->alpha_settings[1] = (float)((test >> 8) & 255) / 255;
    // Tested first, depth is also stored where the alpha test then fails.
    key->early_depth = tests_depth_early() && ((gx.bp[BP_Z_MODE] >> Z_UPDATE_BIT) & 1);
  }
  uint32_t output_alpha = gx.bp[BP_CONSTANT_ALPHA];
  if (gx_constant_alpha_used()) {
    key->constant_alpha = 1;
    values->alpha_settings[2] = (float)(output_alpha & 255) / 255;
  }
  read_fog(key, values);
  read_logic(key);
  gx_tev_reads(key, &maps, &coordinates, &channels);
  read_map_biases(key, values, maps);
  return maps;
}

// Tells which texture maps, texture coordinates and vertex colors a set of stages reads.
void gx_tev_reads(const TevKey *key, unsigned *maps, unsigned *coordinates, unsigned *channels) {
  *maps = *coordinates = *channels = 0;
  for (int i = 0; i < key->stages; i++) {
    const TevStage *s = &key->stage[i];
    if (s->texture_on) { *maps |= 1u << s->map; *coordinates |= 1u << s->coordinate; }
    if (s->indirect) *coordinates |= 1u << s->coordinate;
    if (s->indirect_read) {
      const TevIndirectSource *source = &key->indirect_source[s->indirect_fetch];
      *maps |= 1u << source->map;
      *coordinates |= 1u << source->coordinate;
    }
    if (s->reads_raster && s->raster < RASTER_NONE) *channels |= 1u << s->raster;
  }
  if (key->no_coordinates) *coordinates = 0;  // The chip reads them all as 0.
}

// Writes one color input as an expression.
static void add_color_input(ShaderText *text, int input) {
  static const char *const named[] = {"T.rgb", "vec3(T.a)", "C.rgb", "vec3(C.a)", "vec3(1.0)", "vec3(0.5)", "K", "vec3(0.0)"};
  if (input >= COLOR_REGISTERS_END) gx_text_add(text, "%s", named[input - COLOR_TEXTURE]);
  else if (input & 1) gx_text_add(text, "vec3(R%d.a)", input >> 1);
  else gx_text_add(text, "R%d.rgb", input >> 1);
}

static void add_alpha_input(ShaderText *text, int input) {
  static const char *const named[] = {"T.a", "C.a", "KA", "0.0"};
  if (input >= ALPHA_REGISTERS_END) gx_text_add(text, "%s", named[input - ALPHA_TEXTURE]);
  else gx_text_add(text, "R%d.a", input);
}

static void add_swizzle(ShaderText *text, uint8_t swap) {
  for (int i = 0; i < 4; i++) gx_text_add(text, "%c", "rgba"[(swap >> (i * 2)) & 3]);
}

static double konst_fraction(int select) { return select == 0 ? 1.0 : (double)(KONST_FRACTIONS_END - select) / KONST_FRACTIONS_END; }

static void add_konst(ShaderText *text, int color, int alpha) {
  gx_text_add(text, "    vec3 K = ");
  if (color < KONST_FRACTIONS_END) gx_text_add(text, "vec3(%.3f);\n", konst_fraction(color));
  else if (color < KONST_COLORS_END) gx_text_add(text, "konst[%d].rgb;\n", color & 3);
  else gx_text_add(text, "vec3(konst[%d].%c);\n", color & 3, "rgba"[(color - KONST_CHANNELS_START) >> 2]);
  gx_text_add(text, "    float KA = ");
  if (alpha < KONST_FRACTIONS_END) gx_text_add(text, "%.3f;\n", konst_fraction(alpha));
  else if (alpha < KONST_CHANNELS_START) gx_text_add(text, "konst[%d].a;\n", alpha & 3);  // Reserved by the chip.
  else gx_text_add(text, "konst[%d].%c;\n", alpha & 3, "rgba"[(alpha - KONST_CHANNELS_START) >> 2]);
}

static const char *bias_text(int bias) { return bias == BIAS_ADD_HALF ? " + 0.5" : bias == BIAS_SUBTRACT_HALF ? " - 0.5" : ""; }
static const char *scale_text(int scale) {
  return scale == SCALE_TWO ? "2.0" : scale == SCALE_FOUR ? "4.0" : scale == SCALE_HALF ? "0.5" : "1.0";
}

static void add_wrapped(ShaderText *text, char axis, int mode) {
  gx_text_add(text, "    texels.%c = ", axis);
  if (mode == WRAP_NONE) gx_text_add(text, "texels.%c;\n", axis);
  else if (mode >= WRAP_ZERO) gx_text_add(text, "0.0;\n");
  else gx_text_add(text, "mod(texels.%c, %d.0);\n", axis, WRAP_LARGEST >> mode);
}

// Reads a stage's fetch, then turns the bytes into an offset. Fixed matrices multiply them.
// The other two kinds multiply the coordinate by one byte and the matrix's scale, so an
// offset can grow across the picture, as water does towards the horizon.
static void add_indirect_read(ShaderText *text, const TevKey *key, const TevStage *s) {
  static const int offset_shifts[4] = {0, 3, 4, 5}, bump_shifts[4] = {0, 5, 4, 3};
  const TevIndirectSource *source = &key->indirect_source[s->indirect_fetch];
  int matrix = s->indirect_matrix - 1, bias = s->indirect_format == INDIRECT_FORMAT_FULL ? INDIRECT_BIAS_FULL : INDIRECT_BIAS_SHORT;
  gx_text_add(text, "    vec2 fetch_at = uv%d * map_size[%d].zw / vec2(%d.0, %d.0) / map_size[%d].xy;\n", source->coordinate, source->coordinate,
              1 << source->shift_s, 1 << source->shift_t, source->map);
  gx_text_add(text, "    ivec3 bytes = ivec3(floor(texture(map%d, fetch_at).abg * 255.0 + 0.5));\n", source->map);
  if (s->bump_alpha) gx_text_add(text, "    bump_alpha = (bytes.%c << %d) & 248;\n", "xyz"[s->bump_alpha - 1], bump_shifts[s->indirect_format]);
  if (!s->indirect_matrix) return;
  gx_text_add(text, "    vec3 values = vec3(bytes >> %d) + vec3(%d.0, %d.0, %d.0);\n", offset_shifts[s->indirect_format], s->indirect_bias & 1 ? bias : 0,
              s->indirect_bias & 2 ? bias : 0, s->indirect_bias & 4 ? bias : 0);
  if (s->indirect_kind == INDIRECT_KIND_FIXED) {
    gx_text_add(text, "    texels += vec2(dot(indirect_row_s[%d].xyz, values), dot(indirect_row_t[%d].xyz, values));\n", matrix, matrix);
  } else {
    gx_text_add(text, "    texels += scaled * values.%c / 256.0 * indirect_row_s[%d].w;\n", s->indirect_kind == INDIRECT_KIND_S ? 'x' : 'y', matrix);
  }
}

// Moves a stage's coordinate in whole texels, kept in `stepped` as the next stage can add to it.
static void add_indirect_coordinate(ShaderText *text, const TevKey *key, const TevStage *s) {
  gx_text_add(text, "    vec2 scaled = uv%d * map_size[%d].zw, texels = scaled;\n", s->coordinate, s->coordinate);
  add_wrapped(text, 'x', s->wrap_s);
  add_wrapped(text, 'y', s->wrap_t);
  if (s->indirect_read) add_indirect_read(text, key, s);
  gx_text_add(text, "    stepped = %s;\n", s->add_previous ? "stepped + texels" : "texels");
}

// With a stage's bias set to compare, what was its scale picks how many channels of a
// and b make up the two numbers, and what was its subtract asks for equal, not greater.
// These work on whole bytes, as an input's low byte is all the comparison sees.
static void add_compared_value(ShaderText *text, char which, int mode) {
  if (mode == COMPARE_R8) gx_text_add(text, "i%c.r", which);
  else if (mode == COMPARE_GR16) gx_text_add(text, "(i%c.r + i%c.g * 256)", which, which);
  else gx_text_add(text, "(i%c.r + i%c.g * 256 + i%c.b * 65536)", which, which, which);
}

static void add_comparison(ShaderText *text, int equal, int mode) {
  gx_text_add(text, "(");
  add_compared_value(text, 'a', mode);
  gx_text_add(text, equal ? " == " : " > ");
  add_compared_value(text, 'b', mode);
  gx_text_add(text, " ? ");
}

static void add_color_compare(ShaderText *text, const TevStage *s) {
  if (s->color_scale == COMPARE_RGB8) {
    gx_text_add(text, "vec3(%s(ia.rgb, ib.rgb)) * vec3(ic.rgb) / 255.0", s->color_subtract ? "equal" : "greaterThan");
    return;
  }
  add_comparison(text, s->color_subtract, s->color_scale);
  gx_text_add(text, "vec3(ic.rgb) / 255.0 : vec3(0.0))");
}

static void add_alpha_compare(ShaderText *text, const TevStage *s) {
  if (s->alpha_scale == COMPARE_A8) gx_text_add(text, "(ia.a %s ib.a ? ", s->alpha_subtract ? "==" : ">");
  else add_comparison(text, s->alpha_subtract, s->alpha_scale);
  gx_text_add(text, "float(ic.a) / 255.0 : 0.0)");
}

static void add_stage(ShaderText *text, const TevKey *key, int index) {
  const TevStage *s = &key->stage[index];
  gx_text_add(text, "  {  // Stage %d.\n", index);
  if (s->indirect) add_indirect_coordinate(text, key, s);
  if (s->indirect_used && s->texture_on) {
    gx_text_add(text, "    vec4 T = textureLod(map%d, stepped / map_size[%d].xy, 0.0).", s->map, s->map);
    add_swizzle(text, s->texture_swap);
    gx_text_add(text, ";\n");
  } else if (s->texture_on && (key->diagonal_maps & (1u << s->map))) {
    // The chip's level of detail adds each coordinate's change across and down a pixel,
    // where GPUs take the longer of two slopes (Dolphin's software Rasterizer.cpp,
    // CalculateLOD). Slopes along each axis, each that sum long, give the chip's value.
    gx_text_add(text, "    vec2 D = abs(dFdx(uv%d)) + abs(dFdy(uv%d));\n", s->coordinate, s->coordinate);
    if (key->biased_maps & (1u << s->map)) gx_text_add(text, "    D *= exp2(map_bias[%d].%c);\n", s->map / BIASES_PER_VALUE, "xyzw"[s->map % BIASES_PER_VALUE]);
    gx_text_add(text, "    vec4 T = textureGrad(map%d, uv%d, vec2(D.x, 0.0), vec2(0.0, D.y)).", s->map, s->coordinate);
    add_swizzle(text, s->texture_swap);
    gx_text_add(text, ";\n");
  } else if (s->texture_on) {
    gx_text_add(text, "    vec4 T = texture(map%d, uv%d", s->map, s->coordinate);
    if (key->biased_maps & (1u << s->map)) gx_text_add(text, ", map_bias[%d].%c", s->map / BIASES_PER_VALUE, "xyzw"[s->map % BIASES_PER_VALUE]);
    gx_text_add(text, ").");
    add_swizzle(text, s->texture_swap);
    gx_text_add(text, ";\n");
  } else if (s->reads_texture) {
    // White with coordinates on, and all zero with none. Dolphin's tests say both, though
    // it notes the second as needing more of them.
    gx_text_add(text, "    vec4 T = vec4(%s);\n", key->no_coordinates ? "0.0" : "1.0");
  }
  if (key->depth_mode && index == key->stages - 1) gx_text_add(text, "    depth_texture = T;\n");
  if (s->reads_raster && s->raster == RASTER_NONE) gx_text_add(text, "    vec4 C = vec4(0.0);\n");
  else if (s->reads_raster && s->raster == RASTER_BUMP) gx_text_add(text, "    vec4 C = vec4(float(bump_alpha) / 255.0);\n");
  else if (s->reads_raster && s->raster == RASTER_BUMP_NORMALIZED) gx_text_add(text, "    vec4 C = vec4(float(bump_alpha | (bump_alpha >> 5)) / 255.0);\n");
  else if (s->reads_raster) {
    gx_text_add(text, "    vec4 C = v_color%d.", s->raster);
    add_swizzle(text, s->raster_swap);
    gx_text_add(text, ";\n");
  }
  add_konst(text, s->konst_color, s->konst_alpha);
  for (int i = 0; i < 4; i++) {
    gx_text_add(text, "    vec3 c%c = ", "abcd"[i]);
    add_color_input(text, s->color_in[i]);
    gx_text_add(text, "; float a%c = ", "abcd"[i]);
    add_alpha_input(text, s->alpha_in[i]);
    gx_text_add(text, ";\n");
  }
  if (s->color_bias == BIAS_COMPARE || s->alpha_bias == BIAS_COMPARE) {
    gx_text_add(text, "    ivec4 ia = ivec4(floor(vec4(ca, aa) * 255.0 + 0.5)) & 255, ib = ivec4(floor(vec4(cb, ab) * 255.0 + 0.5)) & 255;\n"
                      "    ivec4 ic = ivec4(floor(vec4(cc, ac) * 255.0 + 0.5)) & 255;\n");
  }
  if (s->color_bias == BIAS_COMPARE) {
    gx_text_add(text, "    vec3 color = cd + ");
    add_color_compare(text, s);
    gx_text_add(text, ";\n");
  } else {
    gx_text_add(text, "    vec3 color = (cd %c mix(ca, cb, cc)%s) * %s;\n", s->color_subtract ? '-' : '+',
                bias_text(s->color_bias), scale_text(s->color_scale));
  }
  if (s->alpha_bias == BIAS_COMPARE) {
    gx_text_add(text, "    float alpha = ad + ");
    add_alpha_compare(text, s);
    gx_text_add(text, ";\n");
  } else {
    gx_text_add(text, "    float alpha = (ad %c mix(aa, ab, ac)%s) * %s;\n", s->alpha_subtract ? '-' : '+',
                bias_text(s->alpha_bias), scale_text(s->alpha_scale));
  }
  gx_text_add(text, "    R%d.rgb = clamp(color, %s);\n", s->color_target, s->color_clamp ? "0.0, 1.0" : REGISTER_RANGE);
  gx_text_add(text, "    R%d.a = clamp(alpha, %s);\n  }\n", s->alpha_target, s->alpha_clamp ? "0.0, 1.0" : REGISTER_RANGE);
}

static void add_test(ShaderText *text, int mode, char reference) {
  static const char *const formats[] = {
    [ALPHA_LESS] = "result.a < alpha_settings.%c", [ALPHA_EQUAL] = "abs(result.a - alpha_settings.%c) < " SAME_VALUE,
    [ALPHA_LEQUAL] = "result.a <= alpha_settings.%c", [ALPHA_GREATER] = "result.a > alpha_settings.%c",
    [ALPHA_NOT_EQUAL] = "abs(result.a - alpha_settings.%c) >= " SAME_VALUE, [ALPHA_GEQUAL] = "result.a >= alpha_settings.%c",
  };
  if (mode == ALPHA_NEVER || mode == ALPHA_ALWAYS) gx_text_add(text, mode == ALPHA_NEVER ? "false" : "true");
  else gx_text_add(text, formats[mode], reference);
}

// The bytes of a depth texture run from the low end of the 24-bit value up.
static void add_depth(ShaderText *text, const TevKey *key) {
  if (key->depth_from_copy) {
    gx_text_add(text, "  float texture_depth = depth_texture.r;\n");
  } else {
    gx_text_add(text, "  vec4 depth_bytes = floor(depth_texture * 255.0 + 0.5);\n");
    if (key->depth_format == DEPTH_FORMAT_LOW_BYTE) gx_text_add(text, "  float texture_depth = depth_bytes.a / " TEXT_DEPTH_MAX ";\n");
    else if (key->depth_format == DEPTH_FORMAT_TWO_BYTES) gx_text_add(text, "  float texture_depth = (depth_bytes.r + depth_bytes.a * 256.0) / " TEXT_DEPTH_MAX ";\n");
    else gx_text_add(text, "  float texture_depth = (depth_bytes.r * 65536.0 + depth_bytes.g * 256.0 + depth_bytes.b) / " TEXT_DEPTH_MAX ";\n");
  }
  gx_text_add(text, "  gl_FragDepth = mod(alpha_settings.w + texture_depth%s, 16777216.0 / " TEXT_DEPTH_MAX ");\n",
              key->depth_mode == DEPTH_ADD ? " + gl_FragCoord.z" : "");
}

// The depth is the one stored, after any depth texture. Colors mix as whole numbers.
static void add_fog(ShaderText *text, const TevKey *key) {
  // With none, the colors are still made whole numbers, as they are below.
  if (key->fog_none) {
    gx_text_add(text, "  result.rgb = floor(result.rgb * 255.0 + 0.5) / 255.0;\n");
    return;
  }
  if (key->depth_mode) gx_text_add(text, "  int fog_depth = int(gl_FragDepth * " TEXT_DEPTH_MAX " + 0.5);\n");
  else gx_text_add(text, "  int fog_depth = clamp(int(gl_FragCoord.z * 16777216.0), 0, 0xffffff);\n");
  if (key->fog_orthographic) gx_text_add(text, "  float fog_distance = fog_settings.x * float(fog_depth) / 16777216.0;\n");
  else gx_text_add(text, "  float fog_distance = fog_settings.x * 16777216.0 / float(int(fog_settings.z) - (fog_depth >> int(fog_settings.w)));\n");
  if (key->fog_range) {
    // Farther toward the sides, by a factor from a table running from the edge to the center.
    gx_text_add(text,
      "  float fog_offset = (gl_FragCoord.x / %d.0 - fog_range_center.x) / fog_range_center.y;\n"
      "  float fog_index = clamp(9.0 - abs(fog_offset) * 9.0, 0.0, 9.0);\n"
      "  int fog_lower = int(fog_index), fog_upper = min(fog_lower + 1, 9);\n"
      "  float fog_k = mix(fog_range_k[fog_lower >> 2][fog_lower & 3], fog_range_k[fog_upper >> 2][fog_upper & 3], fract(fog_index));\n"
      "  fog_distance *= sqrt(fog_offset * fog_offset + fog_k * fog_k) / fog_k;\n", EFB_SCALE);
  }
  gx_text_add(text, "  float fog = clamp(fog_distance - fog_settings.y, 0.0, 1.0);\n");
  if (key->fog_type == FOG_EXP) gx_text_add(text, "  fog = 1.0 - exp2(-8.0 * fog);\n");
  else if (key->fog_type == FOG_EXP2) gx_text_add(text, "  fog = 1.0 - exp2(-8.0 * fog * fog);\n");
  else if (key->fog_type == FOG_BACKWARD_EXP) gx_text_add(text, "  fog = exp2(-8.0 * (1.0 - fog));\n");
  else if (key->fog_type == FOG_BACKWARD_EXP2) gx_text_add(text, "  fog = exp2(-8.0 * (1.0 - fog) * (1.0 - fog));\n");
  gx_text_add(text,
    "  int fog_amount = int(round(fog * 256.0));\n"
    "  ivec3 fogged = (ivec3(floor(result.rgb * 255.0 + 0.5)) * (256 - fog_amount) + ivec3(fog_color.rgb) * fog_amount) >> 8;\n"
    "  result.rgb = vec3(fogged) / 255.0;\n");
}

// Writes the shader for one set of color stages. Anything the stages can be set to
// has a line here, so there is no general shader to fall back on.
void gx_tev_write_shader(const TevKey *key, ShaderText *text) {
  const TevStage *last = &key->stage[key->stages - 1];
  unsigned maps, coordinates, channels;
  gx_tev_reads(key, &maps, &coordinates, &channels);
  // A backend that can test depth before the shader runs, so a discard leaves depth written, defines this.
  if (key->early_depth) gx_text_add(text, "#ifdef GX_EARLY_TESTS\nlayout(early_fragment_tests) in;\n#endif\n");
  gx_text_add(text,
    "GX_BLOCK(%d) uniform PixelValues {\n"
    "  vec4 reg[4]; vec4 konst[4]; vec4 alpha_settings;\n"
    "  vec4 indirect_row_s[%d]; vec4 indirect_row_t[%d]; vec4 map_size[%d]; vec4 map_bias[%d];\n"
    "  vec4 fog_color, fog_settings, fog_range_center; vec4 fog_range_k[3];\n};\n",
    CONSTANTS_PIXEL, TEV_INDIRECT_MATRICES, TEV_INDIRECT_MATRICES, MAX_TEXTURE_MAPS, MAX_TEXTURE_MAPS / BIASES_PER_VALUE);
  for (int i = 0; i < MAX_TEXTURE_MAPS; i++) {
    if (maps & (1u << i)) gx_text_add(text, "GX_MAP(%d) uniform sampler2D map%d;\n", i, i);
    if (coordinates & (1u << i)) gx_text_add(text, "GX_VARYING(%d) in vec%d v_uv%d;\n", GX_VARYING_UV + i, key->projected & (1u << i) ? 3 : 2, i);
  }
  for (int i = 0; i < 2; i++) {
    if (channels & (1u << i)) gx_text_add(text, "GX_VARYING(%d) in vec4 v_color%d;\n", GX_VARYING_COLOR + i, i);
  }
  gx_text_add(text, "layout(location = 0, index = 0) out vec4 out_color;\n");
  // Blending reads a second output only when the stored alpha differs from the stages'.
  // A second output costs a tile-based chip a second write of every pixel.
  if (gx_blend_needs_second_output(key)) {
    gx_text_add(text, "#ifdef GX_NO_DUAL_SOURCE\nvec4 blend_alpha;\n#else\nlayout(location = 0, index = 1) out vec4 blend_alpha;\n#endif\n");
  } else {
    gx_text_add(text, "vec4 blend_alpha;\n");
  }
  gx_text_add(text, "void main() {\n  vec4 R0 = reg[0], R1 = reg[1], R2 = reg[2], R3 = reg[3];\n  vec2 stepped = vec2(0.0);\n");
  if (key->bump_alpha) gx_text_add(text, "  int bump_alpha = 0;\n");
  if (key->no_coordinates) gx_text_add(text, "  vec2 uv0 = vec2(0.0);\n");
  // The third value must divide here, not at the vertices, or perspective is lost.
  for (int i = 0; i < MAX_TEXTURE_MAPS; i++) {
    if (!(coordinates & (1u << i))) continue;
    if (key->projected & (1u << i)) gx_text_add(text, "  vec2 uv%d = v_uv%d.z != 0.0 ? v_uv%d.xy / v_uv%d.z : v_uv%d.xy;\n", i, i, i, i, i);
    else gx_text_add(text, "  vec2 uv%d = v_uv%d;\n", i, i);
  }
  if (key->depth_mode) gx_text_add(text, "  vec4 depth_texture;\n");
  for (int i = 0; i < key->stages; i++) add_stage(text, key, i);
  // The picture takes the last stage's own output, whichever register that was.
  gx_text_add(text, "  vec4 result = vec4(R%d.rgb, R%d.a);\n", last->color_target, last->alpha_target);
  if (key->wrap_output) gx_text_add(text, "  result = vec4(ivec4(floor(result * 255.0 + 0.5)) & 255) / 255.0;\n");
  if (key->alpha_test) {
    static const char *const joins[] = {" && ", " || ", " != ", " == "};
    gx_text_add(text, "  if (!((");
    add_test(text, key->alpha_first, 'x');
    gx_text_add(text, ")%s(", joins[key->alpha_logic]);
    add_test(text, key->alpha_second, 'y');
    gx_text_add(text, "))) discard;\n");
  }
  // Blending reads the stages' alpha, kept apart from the alpha that gets stored.
  gx_text_add(text, "  blend_alpha = result;\n");
  if (key->constant_alpha) gx_text_add(text, "  result.a = alpha_settings.z;\n");
  if (key->depth_mode) add_depth(text, key);
  if (key->fog_type) add_fog(text, key);
  // After fog, and leaving a constant output alpha alone, as in Dolphin.
  if (key->logic_help) {
    const char *channels = key->constant_alpha ? "rgb" : "rgba";
    if (key->logic_help == HELP_INVERT) gx_text_add(text, "  result.%s = 1.0 - clamp(result.%s, 0.0, 1.0);\n", channels, channels);
    else gx_text_add(text, "  result.%s = vec4(%s).%s;\n", channels, key->logic_help == HELP_ZERO ? "0.0" : "1.0", channels);
  }
  gx_text_add(text, "  out_color = result;\n}\n");
}

// Reads the game's settings, sends their values, and returns the texture maps in use.
float gx_last_registers_alpha[4];
unsigned gx_tev_apply(TevKey *key) {
  TevValues values;
  unsigned maps = gx_tev_read(key, &values);
  gx_constants_send(CONSTANTS_PIXEL, &values, sizeof values);
  gx_last_registers_alpha[0] = values.registers[0][3];
  gx_last_registers_alpha[1] = values.registers[1][3];
  gx_last_registers_alpha[2] = values.registers[2][3];
  gx_last_registers_alpha[3] = values.registers[3][3];
  return maps;
}
