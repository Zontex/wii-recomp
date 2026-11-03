// The Wii's lighting, worked out per vertex as the chip does. Each color channel
// mixes ambient light with up to eight lights, then multiplies by a material color.
// This reads the game's settings and writes the matching part of the vertex shader.
#include <stdio.h>
#include <string.h>

#include "gx.h"

enum {
  XF_AMBIENT = 0x100a, XF_MATERIAL = 0x100c, XF_COLOR_CHANNEL = 0x100e, XF_ALPHA_CHANNEL = 0x1010,
  XF_LIGHTS = 0x600, LIGHT_WORDS = 0x10, LIGHT_COLOR = 3, LIGHT_COSINE = 4, LIGHT_DISTANCE = 7, LIGHT_POSITION = 10,
  LIGHT_DIRECTION = 13, MAX_LIGHTS = 8, LIGHT_VECTORS = 4,
  CONTROL_VERTEX_BASE = 1, CONTROL_LIT = 2, CONTROL_VERTEX_START_BIT = 6,
};

// Matches the Light struct in the shader.
typedef struct {
  float color[4], vectors[LIGHT_VECTORS][4];
} LightValues;

static void unpack_color(uint32_t rgba, float out[4]) {
  out[0] = (float)(rgba >> 24) / 255;
  out[1] = (float)((rgba >> 16) & 255) / 255;
  out[2] = (float)((rgba >> 8) & 255) / 255;
  out[3] = (float)(rgba & 255) / 255;
}

static void read_control(uint32_t control, ChannelKey *key) {
  key->vertex_base = control & CONTROL_VERTEX_BASE;
  if (!(control & CONTROL_LIT)) return;
  key->lit = 1;
  key->vertex_start = (control >> CONTROL_VERTEX_START_BIT) & 1;
  key->lights = (uint8_t)(((control >> 2) & 15) | (((control >> 11) & 15) << 4));
  key->diffuse = (control >> 7) & 3;
  key->attenuation = (control >> 9) & 3;
}

void gx_light_read(VertexKey *key, int channel) {
  read_control(gx.xf[XF_COLOR_CHANNEL + channel], &key->color[channel]);
  read_control(gx.xf[XF_ALPHA_CHANNEL + channel], &key->alpha[channel]);
}

// Whether a channel, unlit, gives the vertex's own color and alpha or a white material, so
// a white vertex means white out.
int gx_light_plain(int channel) {
  uint32_t color = gx.xf[XF_COLOR_CHANNEL + channel], alpha = gx.xf[XF_ALPHA_CHANNEL + channel], material = gx.xf[XF_MATERIAL + channel];
  if ((color | alpha) & CONTROL_LIT) return 0;
  return ((color & CONTROL_VERTEX_BASE) || (material & 0xffffff00) == 0xffffff00) && ((alpha & CONTROL_VERTEX_BASE) || (material & 0xff) == 0xff);
}

void gx_light_read_colors(float material[2][4], float ambient[2][4]) {
  for (int channel = 0; channel < 2; channel++) {
    unpack_color(gx.xf[XF_MATERIAL + channel], material[channel]);
    unpack_color(gx.xf[XF_AMBIENT + channel], ambient[channel]);
  }
}

// Lights change far less often than draws happen, so the words are compared first.
void gx_light_send(void) {
  static uint32_t words_sent[MAX_LIGHTS * LIGHT_WORDS];
  static LightValues lights[MAX_LIGHTS];
  if (memcmp(words_sent, &gx.xf[XF_LIGHTS], sizeof words_sent)) {
    memcpy(words_sent, &gx.xf[XF_LIGHTS], sizeof words_sent);
    static const int vectors[LIGHT_VECTORS] = {LIGHT_COSINE, LIGHT_DISTANCE, LIGHT_POSITION, LIGHT_DIRECTION};
    for (int i = 0; i < MAX_LIGHTS; i++) {
      const uint32_t *base = &gx.xf[XF_LIGHTS + i * LIGHT_WORDS];
      unpack_color(base[LIGHT_COLOR], lights[i].color);
      for (int v = 0; v < LIGHT_VECTORS; v++) memcpy(lights[i].vectors[v], &base[vectors[v]], 3 * sizeof(float));
    }
  }
  gx_constants_send(CONSTANTS_LIGHTS, lights, sizeof lights);
}

static const char lighting_functions[] =
  "float falloff(vec3 terms, float value) { return terms.x + terms.y * value + terms.z * value * value; }\n"
  "float attenuated(float strength, float top, float bottom) {\n"
  "  return bottom != 0.0 && top > 0.0 ? strength * top / bottom : 0.0;\n"
  "}\n"
  "float light_strength(int i, int diffuse, int attenuation, vec3 view, vec3 normal) {\n"
  "  vec3 to_light = lights[i].position.xyz - view;\n"
  "  float distance_squared = dot(to_light, to_light), distance_to_light = sqrt(distance_squared);\n"
  "  to_light = unit(to_light);\n"
  "  float facing = dot(to_light, normal);\n"
  "  float strength = diffuse == 1 ? facing : diffuse == 2 ? (facing > 0.0 ? facing : 0.0) : 1.0;\n"
  "  if (attenuation != 3 && attenuation != 1) return strength;\n"
  "  vec3 cosine = lights[i].cosine.xyz, spread = lights[i].distance_falloff.xyz, direction = lights[i].direction.xyz;\n"
  "  if (attenuation == 1) {\n"
  "    if (facing <= 0.0) return 0.0;\n"
  "    float angle = max(dot(normal, direction), 0.0);\n"
  "    return attenuated(strength, falloff(cosine, angle), falloff(spread, angle));\n"
  "  }\n"
  "  float angle = max(dot(to_light, direction), 0.0);\n"
  "  float bottom = spread.x + spread.y * distance_to_light + spread.z * distance_squared;\n"
  "  return attenuated(strength, falloff(cosine, angle), bottom);\n"
  "}\n";

void gx_light_write_functions(ShaderText *text) {
  gx_text_add(text, "struct Light { vec4 color, cosine, distance_falloff, position, direction; };\n"
                    "GX_BLOCK(%d) uniform Lights { Light lights[8]; };\n%s", CONSTANTS_LIGHTS, lighting_functions);
}

// Writes the components of one channel that a control word covers.
static void add_components(ShaderText *text, const ChannelKey *key, int channel, const char *components) {
  char base[32];
  if (key->vertex_base) snprintf(base, sizeof base, "a_color%d", channel);
  else snprintf(base, sizeof base, "material[%d]", channel);
  if (!key->lit) {
    gx_text_add(text, "  color%d.%s = %s.%s;\n", channel, components, base, components);
    return;
  }
  gx_text_add(text, "  {\n    vec4 light = ");
  if (key->vertex_start) gx_text_add(text, "a_color%d;\n", channel);
  else gx_text_add(text, "ambient[%d];\n", channel);
  for (int i = 0; i < MAX_LIGHTS; i++) {
    if (key->lights & (1u << i)) {
      gx_text_add(text, "    light += lights[%d].color * light_strength(%d, %d, %d, view.xyz, normal);\n", i, i, key->diffuse, key->attenuation);
    }
  }
  gx_text_add(text, "    color%d.%s = %s.%s * clamp(light.%s, 0.0, 1.0);\n  }\n", channel, components, base, components, components);
}

void gx_light_write_channel(ShaderText *text, const VertexKey *key, int channel) {
  gx_text_add(text, "  vec4 color%d;\n", channel);
  add_components(text, &key->color[channel], channel, "rgb");
  add_components(text, &key->alpha[channel], channel, "a");
}
