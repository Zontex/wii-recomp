// Vertex work: positions through the game's matrices, normals, and texture coordinate
// generation. The game's settings become a key, as in gx_tev.c, and each key gets a
// small vertex shader. Matrices live in blocks of the shader's memory, so the game can
// change them between draws and each vertex picks its own through its matrix index.
#include <string.h>

#include "cpu.h"
#include "debug.h"
#include "gx.h"

enum {
  XF_MATRICES = 0, XF_NORMAL_MATRICES = 0x400, XF_POST_MATRICES = 0x500, XF_PROJECTION = 0x1020, PROJECTION_ORTHOGRAPHIC = 6,
  XF_TEXTURE_COUNT = 0x103f, XF_TEXTURE_GENERATION = 0x1040, XF_POST_MATRIX_INDEX = 0x1050, XF_POST_ENABLE = 0x1012,
  TEXTURE_SOURCE_FIRST = 5, SOURCE_POSITION = 0, SOURCE_NORMAL = 1, SOURCE_NONE = 2, SOURCE_TANGENT = 3, SOURCE_BINORMAL = 4,
  GENERATE_REGULAR = 0, GENERATE_EMBOSS = 1, GENERATE_COLOR0 = 2, GENERATE_COLOR1 = 3, POST_NORMALIZE_BIT = 8, POST_MATRIX_MASK = 63,
  NORMAL_MATRIX_MASK = 31, ALL_NORMALS = 3, COORDINATE_INPUT_BIT = 2, TEXGEN_KIND_MASK = 7, EMBOSS_SOURCE_SHIFT = 12, EMBOSS_LIGHT_SHIFT = 15, EMBOSS_MASK = 7,
  POST_LAST_ROW = 2,
  IDENTITY_TEXTURE_MATRIX = 60, WORDS_PER_ROW = 4, NORMAL_WORDS_PER_ROW = 3,
  VALUES_PER_VECTOR = 4,
};

// A matrix index of up to 63 can start a matrix, and its last row is two further on.
#define MATRIX_ROWS 66

// Matches the DrawValues block in the shader.
typedef struct {
  float projection[4][4], material[2][4], ambient[2][4];
  int32_t post_index[2][4];
  float cached_normals[CACHED_NORMALS][4];
  float expand_size[4];
} DrawValues;

// With a post matrix, a coordinate keeps a third value of 1 only if the matrix's last
// row does. Games almost always use the identity here, which spares a divide per pixel.
static int post_keeps_third_one(int index) {
  uint32_t row = (gx.xf[XF_POST_MATRIX_INDEX + index] & POST_MATRIX_MASK) + POST_LAST_ROW;
  const uint32_t *words = &gx.xf[XF_POST_MATRICES + row * WORDS_PER_ROW];
  return !as_float(words[0]) && !as_float(words[1]) && as_float(words[2]) + as_float(words[3]) == 1;
}

// A source the vertex lacks reads as 0, 0, 1, 1, whatever the chip is keeping for lighting.
static int read_source(uint32_t source, int normals) {
  if (source == SOURCE_POSITION) return SOURCE_POSITION;
  if (source == SOURCE_NORMAL) return normals ? SOURCE_NORMAL : SOURCE_NONE;
  if (source == SOURCE_TANGENT || source == SOURCE_BINORMAL) return normals == ALL_NORMALS ? (int)source : SOURCE_NONE;
  if (source >= TEXTURE_SOURCE_FIRST && source < TEXTURE_SOURCE_FIRST + MAX_TEXTURE_MAPS) return (int)source;
  debug_unsupported("texture coordinate source", source);
  return SOURCE_NONE;
}

static void read_coordinate(VertexKey *key, int index, int post_enabled, int normals) {
  CoordinateKey *c = &key->coordinate[index];
  uint32_t control = gx.xf[XF_TEXTURE_GENERATION + index];
  uint32_t kind = (control >> 4) & TEXGEN_KIND_MASK, source = (control >> 7) & 31;
  c->generated = 1;
  if (kind == GENERATE_COLOR0 || kind == GENERATE_COLOR1) {
    c->from_color = (uint8_t)(kind - GENERATE_COLOR0 + 1);
    return;
  }
  if (kind == GENERATE_EMBOSS) {
    c->emboss = 1;
    c->emboss_source = (control >> EMBOSS_SOURCE_SHIFT) & EMBOSS_MASK;
    // Dolphin only takes the light's number when the vertex has the normals to use it.
    c->emboss_light = normals == ALL_NORMALS ? (control >> EMBOSS_LIGHT_SHIFT) & EMBOSS_MASK : 0;
    if (c->emboss_source >= index) debug_unsupported("emboss from a later coordinate", c->emboss_source);
    return;
  }
  if (kind != GENERATE_REGULAR) debug_unsupported("texture coordinate kind", kind);
  c->source = (uint8_t)read_source(source, normals);
  c->keeps_z = (control >> COORDINATE_INPUT_BIT) & 1;
  c->projected = (control >> 1) & 1;
  c->third_varies = c->projected;
  if (!post_enabled) return;
  c->normalized = (gx.xf[XF_POST_MATRIX_INDEX + index] >> POST_NORMALIZE_BIT) & 1;
  if (c->normalized || !post_keeps_third_one(index)) c->third_varies = 1;
}

static int lights_vertices(const VertexKey *key) {
  for (int channel = 0; channel < 2; channel++) {
    if (key->color[channel].lit || key->alpha[channel].lit) return 1;
  }
  return 0;
}

// Coordinates the stages don't read are still made if an emboss coordinate starts from them.
static unsigned coordinates_needed(unsigned read) {
  unsigned made = read;
  int count = (int)(gx.xf[XF_TEXTURE_COUNT] & 15);
  for (int i = (count < MAX_TEXTURE_MAPS ? count : MAX_TEXTURE_MAPS) - 1; i >= 0; i--) {
    uint32_t control = gx.xf[XF_TEXTURE_GENERATION + i];
    if ((made & (1u << i)) && ((control >> 4) & TEXGEN_KIND_MASK) == GENERATE_EMBOSS) made |= 1u << ((control >> EMBOSS_SOURCE_SHIFT) & EMBOSS_MASK);
  }
  return made;
}

// Only what the color stages read is worked out, so drawing with fewer inputs shares a
// shader. The stages hear which coordinates must be divided by their third value, which
// costs a little at every pixel, so it's only for those that can need it.
void gx_transform_read(TevKey *tev, VertexKey *key) {
  unsigned maps, coordinates, channels;
  memset(key, 0, sizeof *key);
  gx_tev_reads(tev, &maps, &coordinates, &channels);
  key->coordinates_read = (uint8_t)coordinates;
  key->channels_read = (uint8_t)channels;
  coordinates = coordinates_needed(coordinates);
  key->coordinates_made = (uint8_t)coordinates;
  int post_enabled = gx.xf[XF_POST_ENABLE] & 1, count = (int)(gx.xf[XF_TEXTURE_COUNT] & 15), normals = gx_vertex_normals(gx_vertex_format);
  int embossed = 0;
  for (int i = 0; i < count && i < MAX_TEXTURE_MAPS; i++) {
    if (!(coordinates & (1u << i))) continue;
    read_coordinate(key, i, post_enabled, normals);
    const CoordinateKey *c = &key->coordinate[i];
    if (c->from_color) channels |= 1u << (c->from_color - 1);
    else if (c->emboss) embossed = 1;
    else key->post_matrices = (uint8_t)post_enabled;
  }
  for (int i = 0; i < MAX_TEXTURE_MAPS; i++) {
    CoordinateKey *c = &key->coordinate[i];
    if (c->emboss) c->third_varies = key->coordinate[c->emboss_source].third_varies;
    if (c->third_varies && (key->coordinates_read & (1u << i))) key->coordinates_projected |= (uint8_t)(1u << i);
  }
  tev->projected = key->coordinates_projected;
  for (int channel = 0; channel < 2; channel++) {
    if (channels & (1u << channel)) gx_light_read(key, channel);
  }
  key->channels_used = (uint8_t)channels;
  key->expanded = gx_expanding != 0;
  key->embossed = (uint8_t)embossed;
  key->uses_lights = lights_vertices(key) || embossed;
  if (key->uses_lights) {
    key->has_normal = 1;
    key->normals_present = (uint8_t)normals;
  }
}

static void read_projection(float rows[4][4]) {
  float a = as_float(gx.xf[XF_PROJECTION]), b = as_float(gx.xf[XF_PROJECTION + 1]), c = as_float(gx.xf[XF_PROJECTION + 2]);
  float d = as_float(gx.xf[XF_PROJECTION + 3]), e = as_float(gx.xf[XF_PROJECTION + 4]), f = as_float(gx.xf[XF_PROJECTION + 5]);
  rows[0][0] = a; rows[1][1] = c; rows[2][2] = e; rows[2][3] = f;
  if (gx.xf[XF_PROJECTION + PROJECTION_ORTHOGRAPHIC]) {
    rows[0][3] = b; rows[1][3] = d; rows[3][3] = 1;
  } else {
    rows[0][2] = b; rows[1][2] = d; rows[3][2] = -1;
  }
}

// Where a small draw lands, in the same units as gl_Position over w. Says no if a
// vertex is behind the camera, where that can't be known this simply.
int gx_transform_screen(const Vertex *vertex, float *x, float *y) {
  float rows[4][4] = {{0}};
  read_projection(rows);
  const float *matrices = (const float *)&gx.xf[XF_MATRICES];
  const float *p = vertex->position, *m = &matrices[vertex->matrix[VERTEX_MATRIX_POSITION] * WORDS_PER_ROW];
  float view[4];
  for (int row = 0; row < 3; row++) view[row] = m[row * 4] * p[0] + m[row * 4 + 1] * p[1] + m[row * 4 + 2] * p[2] + m[row * 4 + 3];
  view[3] = 1;
  float clip[2], w = 0;
  for (int row = 0; row < 2; row++) clip[row] = rows[row][0] * view[0] + rows[row][1] * view[1] + rows[row][2] * view[2] + rows[row][3] * view[3];
  for (int column = 0; column < 4; column++) w += rows[3][column] * view[column];
  if (!(w > 1e-5f)) return 0;
  *x = clip[0] / w;
  *y = clip[1] / w;
  return 1;
}

// The depth of each vertex from 0 to 1 before the viewport's depth range, as the
// shaders' GX_DEPTH gives it on either backend. Says no if a vertex is behind the camera.
int gx_transform_depths(const Vertex *vertices, int count, float *low, float *high) {
  float rows[4][4] = {{0}};
  read_projection(rows);
  const float *matrices = (const float *)&gx.xf[XF_MATRICES];
  *low = 1e9f;
  *high = -1e9f;
  for (int i = 0; i < count; i++) {
    const float *p = vertices[i].position, *m = &matrices[vertices[i].matrix[VERTEX_MATRIX_POSITION] * WORDS_PER_ROW];
    float view[4];
    for (int row = 0; row < 3; row++) view[row] = m[row * 4] * p[0] + m[row * 4 + 1] * p[1] + m[row * 4 + 2] * p[2] + m[row * 4 + 3];
    view[3] = 1;
    float z = 0, w = 0;
    for (int column = 0; column < 4; column++) {
      z += rows[2][column] * view[column];
      w += rows[3][column] * view[column];
    }
    if (!(w > 1e-5f)) return 0;
    float depth = z / w + 1;
    if (depth < *low) *low = depth;
    if (depth > *high) *high = depth;
  }
  return count > 0;
}

int gx_transform_bounds(const Vertex *vertices, int count, float bounds[4]) {
  float left = 1e9f, top = 1e9f, right = -1e9f, bottom = -1e9f;
  for (int i = 0; i < count; i++) {
    float x, y;
    if (!gx_transform_screen(&vertices[i], &x, &y)) return 0;
    if (x < left) left = x;
    if (x > right) right = x;
    if (y < top) top = y;
    if (y > bottom) bottom = y;
  }
  bounds[0] = left; bounds[1] = top; bounds[2] = right; bounds[3] = bottom;
  return 1;
}

// Normals sit tightly in the chip's memory, but a block spaces rows four words apart.
static void send_normal_matrices(void) {
  static uint32_t words_sent[MATRIX_ROWS * NORMAL_WORDS_PER_ROW];
  static float rows[MATRIX_ROWS][4];
  if (memcmp(words_sent, &gx.xf[XF_NORMAL_MATRICES], sizeof words_sent)) {
    memcpy(words_sent, &gx.xf[XF_NORMAL_MATRICES], sizeof words_sent);
    for (int i = 0; i < MATRIX_ROWS; i++) memcpy(rows[i], &words_sent[i * NORMAL_WORDS_PER_ROW], 3 * sizeof(float));
  }
  gx_constants_send(CONSTANTS_NORMALS, rows, sizeof rows);
}

void gx_transform_apply(const VertexKey *key) {
  DrawValues values;
  memset(&values, 0, sizeof values);
  read_projection(values.projection);
  gx_light_read_colors(values.material, values.ambient);
  // Only what the shader reads, as these change with every draw that has normals.
  if (key->has_normal && !key->normals_present) memcpy(values.cached_normals[CACHED_NORMAL], gx_cached_normals[CACHED_NORMAL], sizeof values.cached_normals[0]);
  if (key->embossed && key->normals_present < ALL_NORMALS) {
    memcpy(values.cached_normals[CACHED_TANGENT], gx_cached_normals[CACHED_TANGENT], sizeof values.cached_normals[0] * 2);
  }
  if (key->expanded) memcpy(values.expand_size, gx_expand_size, sizeof gx_expand_size);
  if (key->post_matrices) {
    for (int i = 0; i < MAX_TEXTURE_MAPS; i++) {
      values.post_index[i / VALUES_PER_VECTOR][i % VALUES_PER_VECTOR] = (int32_t)(gx.xf[XF_POST_MATRIX_INDEX + i] & POST_MATRIX_MASK);
    }
    gx_constants_send(CONSTANTS_POST, &gx.xf[XF_POST_MATRICES], MATRIX_ROWS * WORDS_PER_ROW * sizeof(uint32_t));
  }
  gx_constants_send(CONSTANTS_DRAW, &values, sizeof values);
  gx_constants_send(CONSTANTS_MATRICES, &gx.xf[XF_MATRICES], MATRIX_ROWS * WORDS_PER_ROW * sizeof(uint32_t));
  if (key->uses_lights) gx_light_send();
  if (key->has_normal) send_normal_matrices();
}

static void add_matrix_index(ShaderText *text, int slot) {
  gx_text_add(text, "int(a_matrices%d.%c)", slot / VALUES_PER_VECTOR, "xyzw"[slot % VALUES_PER_VECTOR]);
}

static void add_inputs(ShaderText *text, const VertexKey *key) {
  unsigned texture_sources = 0;
  int reads_normal = key->normals_present != 0, reads_others = 0, matrix_groups = 1;
  for (int i = 0; i < MAX_TEXTURE_MAPS; i++) {
    const CoordinateKey *c = &key->coordinate[i];
    if (!c->generated || c->from_color) continue;
    if (c->emboss) {
      reads_others |= key->normals_present == ALL_NORMALS;
      continue;
    }
    if (c->source == SOURCE_NORMAL) reads_normal = 1;
    if (c->source == SOURCE_TANGENT || c->source == SOURCE_BINORMAL) reads_others = 1;
    if (c->source >= TEXTURE_SOURCE_FIRST) texture_sources |= 1u << (c->source - TEXTURE_SOURCE_FIRST);
    int group = (i + 1) / VALUES_PER_VECTOR + 1;
    if (group > matrix_groups) matrix_groups = group;
  }
  if (reads_others) texture_sources |= (0xffu << VERTEX_NORMALS_UV) & 0xffu;
  if (key->expanded && matrix_groups <= VERTEX_CORNER / VALUES_PER_VECTOR) matrix_groups = VERTEX_CORNER / VALUES_PER_VECTOR + 1;
  gx_text_add(text, "layout(location=%d) in vec3 a_position;\n", ATTRIBUTE_POSITION);
  if (reads_normal) gx_text_add(text, "layout(location=%d) in vec3 a_normal;\n", ATTRIBUTE_NORMAL);
  for (int channel = 0; channel < 2; channel++) {
    const ChannelKey *color = &key->color[channel], *alpha = &key->alpha[channel];
    if (color->vertex_base || alpha->vertex_base || color->vertex_start || alpha->vertex_start) {
      gx_text_add(text, "layout(location=%d) in vec4 a_color%d;\n", ATTRIBUTE_COLOR + channel, channel);
    }
  }
  for (int i = 0; i < MAX_TEXTURE_MAPS; i++) {
    if (texture_sources & (1u << i)) gx_text_add(text, "layout(location=%d) in vec2 a_uv%d;\n", ATTRIBUTE_UV + i, i);
  }
  for (int i = 0; i < matrix_groups; i++) gx_text_add(text, "layout(location=%d) in uvec4 a_matrices%d;\n", ATTRIBUTE_MATRICES + i, i);
}

static void add_blocks(ShaderText *text, const VertexKey *key) {
  gx_text_add(text, "GX_BLOCK(%d) uniform DrawValues { vec4 projection[4]; vec4 material[2]; vec4 ambient[2]; ivec4 post_index[2]; vec4 cached_normal, cached_tangent, cached_binormal; vec4 expand_size; };\n", CONSTANTS_DRAW);
  gx_text_add(text, "GX_BLOCK(%d) uniform Matrices { vec4 matrix_rows[%d]; };\n", CONSTANTS_MATRICES, MATRIX_ROWS);
  if (key->post_matrices) gx_text_add(text, "GX_BLOCK(%d) uniform PostMatrices { vec4 post_rows[%d]; };\n", CONSTANTS_POST, MATRIX_ROWS);
  if (key->has_normal) gx_text_add(text, "GX_BLOCK(%d) uniform NormalMatrices { vec4 normal_rows[%d]; };\n", CONSTANTS_NORMALS, MATRIX_ROWS);
}

static void add_position(ShaderText *text) {
  gx_text_add(text, "  int matrix = ");
  add_matrix_index(text, VERTEX_MATRIX_POSITION);
  gx_text_add(text,
    ";\n  vec4 object = vec4(a_position, 1.0);\n"
    "  vec4 view = vec4(dot(matrix_rows[matrix], object), dot(matrix_rows[matrix + 1], object), dot(matrix_rows[matrix + 2], object), 1.0);\n"
    "  vec4 clip = vec4(dot(projection[0], view), dot(projection[1], view), dot(projection[2], view), dot(projection[3], view));\n"
    "  gl_Position = vec4(clip.xy, GX_DEPTH(clip.z, clip.w), clip.w);\n");
}

static void add_source(ShaderText *text, const CoordinateKey *c) {
  gx_text_add(text, "    vec4 from_value = ");
  if (c->source == SOURCE_POSITION) gx_text_add(text, "vec4(a_position, 1.0);\n");
  else if (c->source == SOURCE_NORMAL) gx_text_add(text, "vec4(a_normal, 1.0);\n");
  else if (c->source == SOURCE_TANGENT) gx_text_add(text, "vec4(TANGENT_READ, 1.0);\n");
  else if (c->source == SOURCE_BINORMAL) gx_text_add(text, "vec4(BINORMAL_READ, 1.0);\n");
  else if (c->source >= TEXTURE_SOURCE_FIRST) gx_text_add(text, "vec4(a_uv%d, 1.0, 1.0);\n", c->source - TEXTURE_SOURCE_FIRST);
  else gx_text_add(text, "vec4(0.0, 0.0, 1.0, 1.0);\n");
  if (!c->keeps_z) gx_text_add(text, "    from_value.z = 1.0;\n");
}

// An emboss coordinate is another coordinate moved along the surface, towards a light.
static void add_emboss(ShaderText *text, const VertexKey *key, const CoordinateKey *c, int index) {
  int stored = key->normals_present == ALL_NORMALS;
  gx_text_add(text, "    vec3 to_light = unit(lights[%d].position.xyz - view.xyz);\n", c->emboss_light);
  gx_text_add(text, "    vec3 along = turned_normal(%s), across = turned_normal(%s);\n", stored ? "TANGENT_READ" : "cached_tangent.xyz", stored ? "BINORMAL_READ" : "cached_binormal.xyz");
  gx_text_add(text, "    made%d = made%d + vec3(dot(to_light, along), dot(to_light, across), 0.0);\n  }\n", index, c->emboss_source);
}

// Each output coordinate comes from a texture coordinate, the position, the normal
// or a lit color, passed through its matrices. Shading ramps rely on the last two.
static void add_coordinate(ShaderText *text, const VertexKey *key, int index) {
  const CoordinateKey *c = &key->coordinate[index];
  gx_text_add(text, "  vec3 made%d;\n", index);
  if (!c->generated) {
    gx_text_add(text, "  made%d = vec3(0.0, 0.0, 1.0);\n", index);
    return;
  }
  if (c->from_color) {
    gx_text_add(text, "  made%d = vec3(color%d.rg, 1.0);\n", index, c->from_color - 1);
    return;
  }
  gx_text_add(text, "  {\n");
  if (c->emboss) {
    add_emboss(text, key, c, index);
    return;
  }
  add_source(text, c);
  gx_text_add(text, "    vec3 result = vec3(from_value.xy, %s);\n    int index = ", c->projected ? "from_value.z" : "1.0");
  add_matrix_index(text, VERTEX_MATRIX_COORDINATE + index);
  gx_text_add(text, ";\n    if (index != %d) {\n", IDENTITY_TEXTURE_MATRIX);
  gx_text_add(text, "      result.x = dot(matrix_rows[index], from_value);\n      result.y = dot(matrix_rows[index + 1], from_value);\n");
  if (c->projected) gx_text_add(text, "      result.z = dot(matrix_rows[index + 2], from_value);\n");
  gx_text_add(text, "    }\n");
  if (key->post_matrices) {
    if (c->normalized) gx_text_add(text, "    result = unit(result);\n");
    gx_text_add(text, "    int post = post_index[%d][%d];\n", index / VALUES_PER_VECTOR, index % VALUES_PER_VECTOR);
    gx_text_add(text, "    vec4 first = vec4(result, 1.0);\n");
    gx_text_add(text, "    result = vec3(dot(post_rows[post], first), dot(post_rows[post + 1], first), dot(post_rows[post + 2], first));\n");
  }
  // Dolphin found this case in hardware tests, and it matters for a shadow cast sideways.
  if (c->third_varies) gx_text_add(text, "    if (result.z == 0.0) result.xy = clamp(result.xy / 2.0, vec2(-1.0), vec2(1.0));\n");
  gx_text_add(text, "    made%d = result;\n  }\n", index);
}

// Writes the shader for one vertex setup. As with the color stages, every setting has
// its line here, so there is no general shader behind it.
void gx_transform_write_shader(const VertexKey *key, ShaderText *text) {
  add_inputs(text, key);
  add_blocks(text, key);
  for (int i = 0; i < MAX_TEXTURE_MAPS; i++) {
    if (key->coordinates_read & (1u << i)) gx_text_add(text, "GX_VARYING(%d) out vec%d v_uv%d;\n", GX_VARYING_UV + i, key->coordinates_projected & (1u << i) ? 3 : 2, i);
  }
  for (int channel = 0; channel < 2; channel++) {
    if (key->channels_read & (1u << channel)) gx_text_add(text, "GX_VARYING(%d) out vec4 v_color%d;\n", GX_VARYING_COLOR + channel, channel);
  }
  // The built-in normalize gives no answer for a zero length, and the chip leaves those alone.
  gx_text_add(text, "vec3 unit(vec3 v) { float length_of_v = length(v); return length_of_v > 0.0 ? v / length_of_v : v; }\n");
  if (key->uses_lights) gx_light_write_functions(text);
  gx_text_add(text, "#define TANGENT_READ vec3(a_uv%d, a_uv%d.x)\n#define BINORMAL_READ vec3(a_uv%d.y, a_uv%d)\n",
              VERTEX_NORMALS_UV, VERTEX_NORMALS_UV + 1, VERTEX_NORMALS_UV + 1, VERTEX_NORMALS_UV + 2);
  if (key->has_normal) {
    gx_text_add(text, "uint normal_matrix;\nvec3 turned_normal(vec3 v) {\n"
                      "  return vec3(dot(normal_rows[normal_matrix].xyz, v), dot(normal_rows[normal_matrix + 1u].xyz, v), dot(normal_rows[normal_matrix + 2u].xyz, v));\n}\n");
  }
  gx_text_add(text, "void main() {\n");
  add_position(text);
  if (key->expanded) {
    // Each corner moves -1, 0 or 1 times the half size on each axis, kept as 0 to 2.
    gx_text_add(text, "  uint corner = a_matrices%d.%c;\n", VERTEX_CORNER / VALUES_PER_VECTOR, "xyzw"[VERTEX_CORNER % VALUES_PER_VECTOR]);
    gx_text_add(text, "  gl_Position.xy += (vec2(float(corner & 3u), float((corner >> 2) & 3u)) - 1.0) * expand_size.xy * gl_Position.w;\n");
  }
  if (key->has_normal) {
    gx_text_add(text, "  normal_matrix = uint(matrix) & %du;\n  vec3 normal = unit(turned_normal(%s));\n", NORMAL_MATRIX_MASK, key->normals_present ? "a_normal" : "cached_normal.xyz");
  }
  for (int channel = 0; channel < 2; channel++) {
    if (key->channels_used & (1u << channel)) gx_light_write_channel(text, key, channel);
  }
  for (int i = 0; i < MAX_TEXTURE_MAPS; i++) {
    if (key->coordinates_made & (1u << i)) add_coordinate(text, key, i);
    if (key->coordinates_read & (1u << i)) gx_text_add(text, "  v_uv%d = made%d%s;\n", i, i, key->coordinates_projected & (1u << i) ? "" : ".xy");
  }
  for (int channel = 0; channel < 2; channel++) {
    if (key->channels_read & (1u << channel)) gx_text_add(text, "  v_color%d = color%d;\n", channel, channel);
  }
  gx_text_add(text, "}\n");
}
