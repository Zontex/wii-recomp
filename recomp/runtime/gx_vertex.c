// Vertices: decodes each format the game declares into a buffer for the vertex
// shader, which applies the matrices (gx_transform.c) and lights (gx_light.c).
#include <string.h>

#include <stdlib.h>

#include "cpu.h"
#include "debug.h"
#include "gx.h"
#include "memory_watch.h"

enum {
  CP_MATRIX_INDEX = 0x30, CP_DESCRIPTOR_LOW = 0x50, CP_DESCRIPTOR_HIGH = 0x60, CP_FORMAT_A = 0x70,
  CP_FORMAT_B = 0x80, CP_FORMAT_C = 0x90, CP_ARRAY_BASE = 0xa0, CP_ARRAY_STRIDE = 0xb0,
  ARRAY_POSITION = 0, ARRAY_NORMAL = 1, ARRAY_COLOR = 2, ARRAY_TEXTURE = 4,
  KIND_NONE = 0, KIND_DIRECT = 1, KIND_INDEX8 = 2, KIND_INDEX16 = 3,
  COMPONENT_U8 = 0, COMPONENT_S8 = 1, COMPONENT_U16 = 2, COMPONENT_S16 = 3, COMPONENT_FLOAT = 4,
  COLOR_RGB565 = 0, COLOR_RGB8 = 1, COLOR_RGBX8 = 2, COLOR_RGBA4 = 3, COLOR_RGBA6 = 4, COLOR_RGBA8 = 5,
  NORMAL_SHIFT_U8 = 7, NORMAL_SHIFT_S8 = 6, NORMAL_SHIFT_U16 = 15, NORMAL_SHIFT_S16 = 14, MATRIX_INDEX_MASK = 63, TEXTURES = 8, ALL_NORMAL_VALUES = 9,
  NULL_INDEX8 = 0xff, NULL_INDEX16 = 0xffff, DEQUANTIZE_BIT = 30,
};

float gx_cached_normals[CACHED_NORMALS][4];

typedef struct {
  int position, normal, color[2], texture[TEXTURES], position_index, texture_index[TEXTURES];
  int position_components, position_type, position_shift;
  int normal_components, normal_type, normal_indexes;
  int color_type[2], color_alpha[2];
  int texture_components[TEXTURES], texture_type[TEXTURES], texture_shift[TEXTURES];
  float position_scale, normal_scale, texture_scale[TEXTURES];  // From the shifts, worked out once a draw.
} Format;

static uint32_t bits(uint32_t value, int low, int count) { return (value >> low) & ((1u << count) - 1); }

static int normal_shift(int type) {
  return type == COMPONENT_U8 ? NORMAL_SHIFT_U8 : type == COMPONENT_S8 ? NORMAL_SHIFT_S8 : type == COMPONENT_U16 ? NORMAL_SHIFT_U16 : NORMAL_SHIFT_S16;
}

static void read_format(int v, Format *f) {
  uint32_t low = gx.cp[CP_DESCRIPTOR_LOW], high = gx.cp[CP_DESCRIPTOR_HIGH];
  uint32_t a = gx.cp[CP_FORMAT_A + v], b = gx.cp[CP_FORMAT_B + v], c = gx.cp[CP_FORMAT_C + v];
  f->position_index = (int)(low & 1);
  for (int i = 0; i < TEXTURES; i++) {
    f->texture_index[i] = (int)bits(low, 1 + i, 1);
    f->texture[i] = (int)bits(high, i * 2, 2);
  }
  f->position = (int)bits(low, 9, 2);
  f->normal = (int)bits(low, 11, 2);
  f->color[0] = (int)bits(low, 13, 2);
  f->color[1] = (int)bits(low, 15, 2);
  f->position_components = bits(a, 0, 1) ? 3 : 2;
  f->position_type = (int)bits(a, 1, 3);
  f->position_shift = (int)bits(a, 4, 5);
  f->normal_components = bits(a, 9, 1) ? 9 : 3;
  f->normal_type = (int)bits(a, 10, 3);
  // Three normals can each have their own index, one after the other.
  f->normal_indexes = bits(a, 31, 1) && f->normal_components == ALL_NORMAL_VALUES && f->normal >= KIND_INDEX8 ? 3 : 1;
  f->color_alpha[0] = (int)bits(a, 13, 1);
  f->color_type[0] = (int)bits(a, 14, 3);
  f->color_alpha[1] = (int)bits(a, 17, 1);
  f->color_type[1] = (int)bits(a, 18, 3);
  static const struct { int word, count, type, shift; } layout[TEXTURES] = {
    {0, 21, 22, 25}, {1, 0, 1, 4}, {1, 9, 10, 13}, {1, 18, 19, 22},
    {1, 27, 28, -1}, {2, 5, 6, 9}, {2, 14, 15, 18}, {2, 23, 24, 27},
  };
  const uint32_t words[3] = {a, b, c};
  for (int i = 0; i < TEXTURES; i++) {
    uint32_t word = words[layout[i].word];
    f->texture_components[i] = bits(word, layout[i].count, 1) ? 2 : 1;
    f->texture_type[i] = (int)bits(word, layout[i].type, 3);
    f->texture_shift[i] = layout[i].shift < 0 ? (int)bits(c, 0, 5) : (int)bits(word, layout[i].shift, 5);
  }
  // With the dequantize bit clear, whole-number positions and coordinates keep their value
  // whatever their shift, as Dolphin's compiled vertex loaders read it. The SDK always sets it.
  if (!bits(a, DEQUANTIZE_BIT, 1)) {
    f->position_shift = 0;
    for (int i = 0; i < TEXTURES; i++) f->texture_shift[i] = 0;
  }
  f->position_scale = 1.0f / (float)(1u << f->position_shift);
  for (int i = 0; i < TEXTURES; i++) f->texture_scale[i] = 1.0f / (float)(1u << f->texture_shift[i]);
  f->normal_scale = 1.0f / (float)(1u << normal_shift(f->normal_type));
}

static uint32_t vertex_bytes(const Format *f);

// Each format as last worked out, until the game writes a vertex register again. Every
// draw asks three times: for its size, its normals and its vertices.
static Format formats[8];
static uint32_t format_bytes[8];
static unsigned format_written[8];

static const Format *format_for(int v) {
  if (format_written[v] != gx_cp_written) {
    read_format(v, &formats[v]);
    format_bytes[v] = vertex_bytes(&formats[v]);
    format_written[v] = gx_cp_written;
  }
  return &formats[v];
}

static uint32_t component_bytes(int type) {
  return type == COMPONENT_FLOAT ? 4 : type >= COMPONENT_U16 ? 2 : 1;
}

static uint32_t color_bytes(int type) {
  static const uint32_t sizes[] = {2, 3, 4, 2, 3, 4};
  return sizes[type > COLOR_RGBA8 ? 0 : type];
}

static uint32_t attribute_bytes(int kind, uint32_t direct) {
  if (kind == KIND_DIRECT) return direct;
  return kind == KIND_INDEX8 ? 1 : kind == KIND_INDEX16 ? 2 : 0;
}

static uint32_t vertex_bytes(const Format *f) {
  uint32_t size = (uint32_t)f->position_index;
  for (int i = 0; i < TEXTURES; i++) size += (uint32_t)f->texture_index[i];
  size += attribute_bytes(f->position, (uint32_t)f->position_components * component_bytes(f->position_type));
  size += attribute_bytes(f->normal, (uint32_t)f->normal_components * component_bytes(f->normal_type)) * (uint32_t)f->normal_indexes;
  for (int i = 0; i < 2; i++) size += attribute_bytes(f->color[i], color_bytes(f->color_type[i]));
  for (int i = 0; i < TEXTURES; i++) {
    size += attribute_bytes(f->texture[i], (uint32_t)f->texture_components[i] * component_bytes(f->texture_type[i]));
  }
  return size;
}

int gx_vertex_normals(int format) {
  const Format *f = format_for(format & 7);
  return f->normal == KIND_NONE ? 0 : f->normal_components == ALL_NORMAL_VALUES ? 3 : 1;
}

uint32_t gx_vertices_size(int format, int count) {
  format_for(format & 7);
  return format_bytes[format & 7] * (uint32_t)count;
}

static inline float read_component(const uint8_t *p, int type, float scale) {
  switch (type) {
    case COMPONENT_U8: return (float)p[0] * scale;
    case COMPONENT_S8: return (float)(int8_t)p[0] * scale;
    case COMPONENT_U16: return (float)(uint16_t)((p[0] << 8) | p[1]) * scale;
    case COMPONENT_S16: return (float)(int16_t)((p[0] << 8) | p[1]) * scale;
    default: return as_float(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
  }
}

static void read_color(const uint8_t *p, int type, float out[4]) {
  uint32_t v = 0;
  switch (type) {
    case COLOR_RGB565:
      v = ((uint32_t)p[0] << 8) | p[1];
      out[0] = (float)(v >> 11) / 31; out[1] = (float)((v >> 5) & 63) / 63; out[2] = (float)(v & 31) / 31; out[3] = 1;
      break;
    case COLOR_RGB8: case COLOR_RGBX8:
      out[0] = (float)p[0] / 255; out[1] = (float)p[1] / 255; out[2] = (float)p[2] / 255; out[3] = 1;
      break;
    case COLOR_RGBA4:
      out[0] = (float)(p[0] >> 4) / 15; out[1] = (float)(p[0] & 15) / 15;
      out[2] = (float)(p[1] >> 4) / 15; out[3] = (float)(p[1] & 15) / 15;
      break;
    case COLOR_RGBA6:
      v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
      for (int i = 0; i < 4; i++) out[i] = (float)((v >> (18 - i * 6)) & 63) / 63;
      break;
    default:
      for (int i = 0; i < 4; i++) out[i] = (float)p[i] / 255;
  }
}

enum { ARRAYS = 12 };
// While a display list's draw is decoded to be kept, the lowest and highest index read
// from each array, so the memory it came from can be watched.
static struct { int on; uint32_t low[ARRAYS], high[ARRAYS], used; } spans;

// Returns where an attribute's data is, in the stream or in an array.
static const uint8_t *locate(const uint8_t **p, int kind, int array, uint32_t direct) {
  const uint8_t *at = *p;
  if (kind == KIND_DIRECT) {
    *p += direct;
    return at;
  }
  uint32_t index = kind == KIND_INDEX8 ? at[0] : ((uint32_t)at[0] << 8) | at[1];
  *p += kind == KIND_INDEX8 ? 1 : 2;
  if (spans.on) {
    if (!(spans.used & (1u << array)) || index < spans.low[array]) spans.low[array] = index;
    if (!(spans.used & (1u << array)) || index > spans.high[array]) spans.high[array] = index;
    spans.used |= 1u << array;
  }
  return host((gx.cp[CP_ARRAY_BASE + array] + index * (gx.cp[CP_ARRAY_STRIDE + array] & 0xff)) | 0x80000000u);
}

// Several components of one type, the type chosen once rather than for each.
static inline void read_components(const uint8_t *p, int type, float scale, int count, float *out) {
  switch (type) {
    case COMPONENT_U8: for (int i = 0; i < count; i++) out[i] = (float)p[i] * scale; break;
    case COMPONENT_S8: for (int i = 0; i < count; i++) out[i] = (float)(int8_t)p[i] * scale; break;
    case COMPONENT_U16: for (int i = 0; i < count; i++) out[i] = (float)(uint16_t)((p[i * 2] << 8) | p[i * 2 + 1]) * scale; break;
    case COMPONENT_S16: for (int i = 0; i < count; i++) out[i] = (float)(int16_t)((p[i * 2] << 8) | p[i * 2 + 1]) * scale; break;
    default:
      for (int i = 0; i < count; i++) out[i] = read_component(p + i * 4, COMPONENT_FLOAT, scale);
  }
}

static void read_normal(const uint8_t *source, const Format *f, float out[3]) {
  read_components(source, f->normal_type, f->normal_scale, 3, out);
}

static void read_normals(const uint8_t **p, const Format *f, Vertex *v) {
  uint32_t size = component_bytes(f->normal_type), values = (uint32_t)f->normal_components;
  float *parts[3] = {v->normal, v->tangent, v->binormal};
  if (f->normal_indexes == 1) {
    const uint8_t *source = locate(p, f->normal, ARRAY_NORMAL, values * size);
    for (uint32_t part = 0; source && part < values / 3; part++) read_normal(source + part * 3 * size, f, parts[part]);
    return;
  }
  for (uint32_t part = 0; part < 3; part++) {
    const uint8_t *source = locate(p, f->normal, ARRAY_NORMAL, 0);
    if (source) read_normal(source + part * 3 * size, f, parts[part]);
  }
}

// An index of all ones in the position leaves the vertex out, and those after move up.
static int skips_vertex(const uint8_t *p, int kind) {
  if (kind == KIND_INDEX8) return p[0] == NULL_INDEX8;
  return kind == KIND_INDEX16 && (uint32_t)((p[0] << 8) | p[1]) == NULL_INDEX16;
}

// The chip keeps each normal of the last vertex of a draw, for the vertices of later
// draws with none. That counts even if the vertex was left out.
static void keep_last_normals(const Format *f, const Vertex *last) {
  if (f->normal == KIND_NONE) return;
  memcpy(gx_cached_normals[CACHED_NORMAL], last->normal, sizeof last->normal);
  if (f->normal_components != ALL_NORMAL_VALUES) return;
  memcpy(gx_cached_normals[CACHED_TANGENT], last->tangent, sizeof last->tangent);
  memcpy(gx_cached_normals[CACHED_BINORMAL], last->binormal, sizeof last->binormal);
}

int gx_read_vertices(const uint8_t *data, int format, int count, Vertex *out) {
  const Format f = *format_for(format & 7);
  if (f.normal_components == ALL_NORMAL_VALUES && f.normal != KIND_NONE && (f.texture[VERTEX_NORMALS_UV] || f.texture[VERTEX_NORMALS_UV + 1] || f.texture[VERTEX_NORMALS_UV + 2])) {
    debug_unsupported("vtx.normal_nbt_9_values", "texture coordinates 5 to 7 beside binormal and tangent", (uint32_t)format);
  }
  uint32_t matrices = gx.cp[CP_MATRIX_INDEX], matrices_high = gx.cp[CP_MATRIX_INDEX + 0x10];
  uint8_t matrix_defaults[VERTEX_MATRICES];
  matrix_defaults[VERTEX_MATRIX_POSITION] = (uint8_t)(matrices & MATRIX_INDEX_MASK);
  for (int i = 0; i < TEXTURES; i++) {
    uint32_t index = i < 4 ? bits(matrices, 6 + i * 6, 6) : bits(matrices_high, (i - 4) * 6, 6);
    matrix_defaults[VERTEX_MATRIX_COORDINATE + i] = (uint8_t)index;
  }
  // Color 1 without color 0 feeds the first channel, and the second reads as missing, as
  // Dolphin's VertexShaderGen.cpp has it.
  int color_slot[2] = {0, f.color[0] == KIND_NONE ? 0 : 1};
  // What every vertex starts as: no position, normal or coordinates, white, and the
  // draw's matrices. Copied whole for each, which costs less than setting each part.
  Vertex start;
  memset(&start, 0, sizeof start);
  memcpy(start.matrix, matrix_defaults, sizeof matrix_defaults);
  for (int i = 0; i < 2; i++) {
    for (int c = 0; c < 4; c++) start.color[i][c] = 1;
  }
  uint32_t position_bytes = (uint32_t)f.position_components * component_bytes(f.position_type);
  uint32_t texture_bytes[TEXTURES];
  for (int i = 0; i < TEXTURES; i++) texture_bytes[i] = (uint32_t)f.texture_components[i] * component_bytes(f.texture_type[i]);
  const uint8_t *p = data;
  int kept = 0;
  for (int n = 0; n < count; n++) {
    Vertex *v = &out[kept];
    *v = start;
    if (f.position_index) v->matrix[VERTEX_MATRIX_POSITION] = *p++ & MATRIX_INDEX_MASK;
    for (int i = 0; i < TEXTURES; i++) {
      if (f.texture_index[i]) v->matrix[VERTEX_MATRIX_COORDINATE + i] = *p++ & MATRIX_INDEX_MASK;
    }
    int skipped = skips_vertex(p, f.position);
    if (f.position != KIND_NONE) {
      const uint8_t *source = locate(&p, f.position, ARRAY_POSITION, position_bytes);
      if (source) read_components(source, f.position_type, f.position_scale, f.position_components, v->position);
    }
    if (f.normal != KIND_NONE) read_normals(&p, &f, v);
    for (int i = 0; i < 2; i++) {
      if (f.color[i] == KIND_NONE) continue;
      const uint8_t *source = locate(&p, f.color[i], ARRAY_COLOR + i, color_bytes(f.color_type[i]));
      if (source) read_color(source, f.color_type[i], v->color[color_slot[i]]);
    }
    for (int i = 0; i < TEXTURES; i++) {
      if (f.texture[i] == KIND_NONE) continue;
      const uint8_t *source = locate(&p, f.texture[i], ARRAY_TEXTURE + i, texture_bytes[i]);
      if (source) read_components(source, f.texture_type[i], f.texture_scale[i], f.texture_components[i], v->uv[i]);
    }
    if (n == count - 1) keep_last_normals(&f, v);
    if (!skipped) kept++;
  }
  return kept;
}

// Display lists: models are drawn by calling the same lists frame after frame, and their
// vertices decode the same way each time. So each draw inside a called list keeps what
// it decoded, and the next call reuses it while three things hold: the list's bytes are
// unwritten, so the draw is the same command; the vertex registers it was decoded with are
// the same; and the arrays it indexed are unwritten (memory_watch.h). WiiCompiled keeps
// a scan of each list in the same way (CREDITS.md). WII_RECOMP_LIST_CACHE=1 turns it on.
GxCalledList gx_called_list;
unsigned gx_list_draws_reused, gx_list_draws_decoded;

enum { KEPT_SLOTS = 8192, KEPT_PROBES = 8, KEPT_BYTES_MOST = 64 << 20, VERTEX_REGISTERS = 7 };
typedef struct {
  uint32_t list, list_bytes, offset;
  uint64_t list_watched;
  uint32_t registers[VERTEX_REGISTERS], used, base[ARRAYS], stride[ARRAYS], first[ARRAYS], bytes[ARRAYS];
  uint64_t watched[ARRAYS];
  int format, count, kept, sets_normals;
  float normals[CACHED_NORMALS][4];
  Vertex *vertices;
} KeptDraw;
static KeptDraw kept_draws[KEPT_SLOTS];
static size_t kept_bytes;
static int keeping = -1;

static void vertex_registers(int format, uint32_t out[VERTEX_REGISTERS]) {
  out[0] = gx.cp[CP_DESCRIPTOR_LOW], out[1] = gx.cp[CP_DESCRIPTOR_HIGH];
  out[2] = gx.cp[CP_FORMAT_A + format], out[3] = gx.cp[CP_FORMAT_B + format], out[4] = gx.cp[CP_FORMAT_C + format];
  out[5] = gx.cp[CP_MATRIX_INDEX], out[6] = gx.cp[CP_MATRIX_INDEX + 0x10];
}

static int still_holds(const KeptDraw *d, int format, int count) {
  if (d->list != gx_called_list.address || d->list_bytes != gx_called_list.bytes || d->list_watched != gx_called_list.watched) return 0;
  if (d->format != format || d->count != count) return 0;
  uint32_t registers[VERTEX_REGISTERS];
  vertex_registers(format, registers);
  if (memcmp(registers, d->registers, sizeof registers)) return 0;
  for (int a = 0; a < ARRAYS; a++) {
    if (!(d->used & (1u << a))) continue;
    if (gx.cp[CP_ARRAY_BASE + a] != d->base[a] || (gx.cp[CP_ARRAY_STRIDE + a] & 0xff) != d->stride[a]) return 0;
    if (watch_count(d->first[a], d->bytes[a]) != d->watched[a]) return 0;
  }
  return 1;
}

static KeptDraw *kept_slot(uint32_t offset) {
  uint32_t mixed = (gx_called_list.address ^ (offset * 0x9e3779b1u)) * 2654435761u, first = mixed % KEPT_SLOTS;
  for (int step = 0; step < KEPT_PROBES; step++) {
    KeptDraw *d = &kept_draws[(first + (uint32_t)step) % KEPT_SLOTS];
    if (!d->vertices || (d->list == gx_called_list.address && d->offset == offset)) return d;
  }
  return &kept_draws[first];
}

// Keeps a draw just decoded, if everything it read can be watched.
static void keep(KeptDraw *d, uint32_t offset, int format, int count, int kept, const Vertex *out) {
  if (gx_called_list.watched == WATCH_NONE) return;
  uint32_t used = spans.used, first[ARRAYS], bytes[ARRAYS];
  uint64_t watched[ARRAYS];
  for (int a = 0; a < ARRAYS; a++) {
    if (!(used & (1u << a))) continue;
    uint32_t stride = gx.cp[CP_ARRAY_STRIDE + a] & 0xff;
    // An element is at most a stride long, or 36 bytes of floats where the stride is less.
    uint32_t element = stride > 36 ? stride : 36;
    first[a] = (gx.cp[CP_ARRAY_BASE + a] + spans.low[a] * stride) | 0x80000000u;
    bytes[a] = (spans.high[a] - spans.low[a]) * stride + element;
    watched[a] = watch_range(first[a], bytes[a]);
    if (watched[a] == WATCH_NONE) return;
  }
  size_t room = (size_t)count * sizeof(Vertex);
  if (d->vertices && d->count != count) {
    free(d->vertices);
    kept_bytes -= (size_t)d->count * sizeof(Vertex);
    d->vertices = 0;
  }
  if (!d->vertices) {
    if (kept_bytes + room > KEPT_BYTES_MOST) return;
    d->vertices = malloc(room);
    if (!d->vertices) return;
    kept_bytes += room;
  }
  memcpy(d->vertices, out, (size_t)kept * sizeof(Vertex));
  d->list = gx_called_list.address, d->list_bytes = gx_called_list.bytes, d->offset = offset;
  d->list_watched = gx_called_list.watched;
  vertex_registers(format, d->registers);
  d->used = used;
  for (int a = 0; a < ARRAYS; a++) {
    if (!(used & (1u << a))) continue;
    d->base[a] = gx.cp[CP_ARRAY_BASE + a], d->stride[a] = gx.cp[CP_ARRAY_STRIDE + a] & 0xff;
    d->first[a] = first[a], d->bytes[a] = bytes[a], d->watched[a] = watched[a];
  }
  d->format = format, d->count = count, d->kept = kept;
  d->sets_normals = formats[format].normal != KIND_NONE;
  memcpy(d->normals, gx_cached_normals, sizeof d->normals);
}

int gx_read_list_vertices(const uint8_t *data, int format, int count, Vertex *out) {
  format &= 7;
  // Off unless asked for: on the Pi, half a race's list draws miss, as characters' vertices
  // are rewritten every frame, and the lookups and the extra copy of each kept vertex cost
  // more than decoding saves (a race frame's commands took 13.1 ms with it, 11.5 without).
  // Keeping the vertices on the graphics card instead, so a hit copies nothing, is what
  // would pay.
  if (keeping < 0) keeping = watch_enabled() && getenv("WII_RECOMP_LIST_CACHE");
  if (!keeping) return gx_read_vertices(data, format, count, out);
  uint32_t offset = (uint32_t)(data - gx_called_list.start);
  KeptDraw *d = kept_slot(offset);
  if (d->vertices && d->offset == offset && still_holds(d, format, count)) {
    memcpy(out, d->vertices, (size_t)d->kept * sizeof(Vertex));
    if (d->sets_normals) memcpy(gx_cached_normals, d->normals, sizeof d->normals);
    gx_list_draws_reused++;
    return d->kept;
  }
  gx_list_draws_decoded++;
  spans.on = 1, spans.used = 0;
  int kept = gx_read_vertices(data, format, count, out);
  spans.on = 0;
  keep(d, offset, format, count, kept, out);
  return kept;
}
