// Vulkan shaders, pipelines and samplers. A pipeline bakes in a shader with its
// blending, depth and culling, so each mix is built once, on first use, and kept.
#include <errno.h>
#include <pthread.h>
#include <shaderc/shaderc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "debug.h"
#include "gx_vulkan.h"

enum {
  FIRST_STAGE_SLOTS = 64, FIRST_SHADER_SLOTS = 64, FIRST_PIPELINE_SLOTS = 256, TEXT_BYTES = 64 * 1024, CLEAR_PUSH_BYTES = 32, MAX_ATTRIBUTES = 16,
  SAMPLER_SLOTS = 8192, LEVEL_STEPS = 16,
};
// Sampling without smaller versions still tells shrinking from growing, as OpenGL's
// plain GL_LINEAR does, only if the level may rise above 0. This is Vulkan's own recipe.
#define NO_MIPMAP_MAX_LOD 0.25f

static const char *const prefix =
  "#version 450\n"
  "#define GX_VARYING(n) layout(location = n)\n"
  "#define GX_BLOCK(n) layout(std140, set = 0, binding = n)\n"
  "#define GX_MAP(n) layout(set = 1, binding = n)\n"
  "#define GX_DEPTH(z, w) ((z) + (w))\n"  // Vulkan's depth runs from 0, not -w.
  "#define GX_EARLY_TESTS\n";  // Every Vulkan device can test depth before the shader.

// A stage is known by a hash of its full text, so a compiled one can be found again
// in a later run, and a pipeline by the hashes of its two stages and its state.
typedef struct {
  uint64_t hash;
  VkShaderModule module;
} Stage;

typedef struct {
  uint32_t vertex, fragment;  // Indexes into stages.
} Shader;

typedef struct {
  uint64_t vertex, fragment;
  GxRenderState state;
} PipelineKey;

typedef struct {
  PipelineKey key;
  VkPipeline pipeline;
} PipelineSlot;

static shaderc_compiler_t compiler;
static shaderc_compile_options_t options;
static Stage *stages;
static uint32_t stage_count, stage_capacity;
static uint32_t *stage_slots;  // Open addressing from a hash to an index into stages, plus 1.
static uint32_t stage_slot_count;
static Shader *shaders;
static uint32_t shader_count, shader_capacity;
static PipelineSlot *pipelines;
static uint32_t pipeline_slots, pipeline_count;
static VkPipelineCache pipeline_cache;
static VkVertexInputBindingDescription vertex_binding;
static VkVertexInputAttributeDescription vertex_attributes[MAX_ATTRIBUTES];
static uint32_t vertex_attribute_count;
static struct { uint64_t key; VkSampler sampler; } samplers[SAMPLER_SLOTS];
static uint32_t sampler_count;
static VkSampler first_sampler;  // What a draw gets once the table is full.

static void create_set_layouts(void) {
  VkDescriptorSetLayoutBinding blocks[GX_CONSTANT_BLOCKS], maps[MAX_TEXTURE_MAPS];
  for (uint32_t i = 0; i < GX_CONSTANT_BLOCKS; i++) {
    blocks[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0};
  }
  for (uint32_t i = 0; i < MAX_TEXTURE_MAPS; i++) {
    maps[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0};
  }
  VkDescriptorSetLayoutCreateInfo constants = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = GX_CONSTANT_BLOCKS, .pBindings = blocks};
  vk_check(vkCreateDescriptorSetLayout(vk.device, &constants, 0, &vk.set_layouts[SET_CONSTANTS]), "creating a set layout");
  // Textures are pushed straight into the commands where the driver can, leaving no sets to manage.
  VkDescriptorSetLayoutCreateInfo textures = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .flags = vk.push_descriptors ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                              .bindingCount = MAX_TEXTURE_MAPS, .pBindings = maps};
  vk_check(vkCreateDescriptorSetLayout(vk.device, &textures, 0, &vk.set_layouts[SET_TEXTURES]), "creating a set layout");
}

static void create_pipeline_layout(void) {
  VkPushConstantRange clear = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CLEAR_PUSH_BYTES};
  VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = SET_COUNT,
                                     .pSetLayouts = vk.set_layouts, .pushConstantRangeCount = 1, .pPushConstantRanges = &clear};
  vk_check(vkCreatePipelineLayout(vk.device, &info, 0, &vk.pipeline_layout), "creating a pipeline layout");
  VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, GX_CONSTANT_BLOCKS * FRAMES_IN_FLIGHT};
  VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = FRAMES_IN_FLIGHT, .poolSizeCount = 1, .pPoolSizes = &size};
  vk_check(vkCreateDescriptorPool(vk.device, &pool, 0, &vk.descriptor_pool), "creating a descriptor pool");
}

// ---- The cache on disk ----
//
// Three files in WII_RECOMP_SHADER_CACHE, which main.c puts beside the program:
//   stages.bin     every stage's SPIR-V, by the hash of its text, so shaderc runs once per stage ever
//   pipelines.bin  every pipeline's key, so the next run builds them all before the game starts
//   driver.bin     the driver's own pipeline cache, which makes building them quick
// The first two only ever grow by whole records written at the end, so a run killed
// part way leaves at most one torn record, which loading stops at. driver.bin is
// replaced whole. WII_RECOMP_NO_SHADER_CACHE=1 turns it all off, for comparing.

enum { CACHE_VERSION = 1, SAVE_EVERY_SECONDS = 10, SPIRV_MAGIC = 0x07230203 };
typedef struct { char magic[4]; uint32_t version, record_bytes, unused; } CacheHeader;

static char cache_folder[1024];
static FILE *stages_file, *pipelines_file;
static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;  // The saver and the exit don't write at once.
static int driver_changed;  // Pipelines were built since driver.bin was last written.

static double seconds_now(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)now.tv_sec + now.tv_nsec / 1e9;
}

static uint64_t hash_text(const char *text, size_t bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < bytes; i++) hash = (hash ^ (uint8_t)text[i]) * 1099511628211ull;
  return hash ? hash : 1;  // 0 never names a stage.
}

static void cache_path(char *out, size_t size, const char *name) { snprintf(out, size, "%s/%s", cache_folder, name); }

// Opens a file of records for adding to. If its header is missing or from another
// version, it's started again, and nothing in it is read.
static FILE *open_records(const char *name, const char magic[4], uint32_t record_bytes, FILE **reading) {
  char path[1100];
  cache_path(path, sizeof path, name);
  CacheHeader want = {{magic[0], magic[1], magic[2], magic[3]}, CACHE_VERSION, record_bytes, 0}, have;
  FILE *file = fopen(path, "rb");
  int usable = file && fread(&have, sizeof have, 1, file) == 1 && !memcmp(&have, &want, sizeof want);
  if (file && !usable) fclose(file);
  *reading = usable ? file : 0;
  FILE *adding = fopen(path, usable ? "ab" : "wb");
  if (!adding) {
    fprintf(stderr, "shader cache: can't write %s, so it won't be kept\n", path);
    return 0;
  }
  if (!usable) {
    fwrite(&want, sizeof want, 1, adding);
    fflush(adding);
  }
  return adding;
}

static void *read_whole(const char *name, size_t *bytes) {
  char path[1100];
  cache_path(path, sizeof path, name);
  FILE *file = fopen(path, "rb");
  if (!file) return 0;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  void *data = size > 0 ? malloc((size_t)size) : 0;
  if (data && fread(data, 1, (size_t)size, file) != (size_t)size) {
    free(data);
    data = 0;
  }
  fclose(file);
  *bytes = data ? (size_t)size : 0;
  return data;
}

// Written beside, then renamed over, so a reader never sees half a file.
static void save_driver_cache(void) {
  pthread_mutex_lock(&save_lock);
  if (cache_folder[0] && __atomic_exchange_n(&driver_changed, 0, __ATOMIC_ACQ_REL)) {
    size_t bytes = 0;
    void *data = 0;
    if (vkGetPipelineCacheData(vk.device, pipeline_cache, &bytes, 0) == VK_SUCCESS && bytes && (data = malloc(bytes)) &&
        vkGetPipelineCacheData(vk.device, pipeline_cache, &bytes, data) == VK_SUCCESS) {
      char path[1100], temporary[1100];
      cache_path(path, sizeof path, "driver.bin");
      cache_path(temporary, sizeof temporary, "driver.bin.new");
      FILE *file = fopen(temporary, "wb");
      if (file && fwrite(data, 1, bytes, file) == bytes && !fclose(file)) rename(temporary, path);
      else if (file) fclose(file);
    }
    free(data);
  }
  pthread_mutex_unlock(&save_lock);
}

// Building a pipeline is the slow part, so the game's thread never waits on saving.
// The driver keeps its cache safe to read while another thread adds to it.
static void *keep_saving(void *unused) {
  (void)unused;
  for (;;) {
    sleep(SAVE_EVERY_SECONDS);
    save_driver_cache();
  }
  return 0;
}

static void open_cache(void) {
  const char *folder = getenv("WII_RECOMP_SHADER_CACHE");
  const char *off = getenv("WII_RECOMP_NO_SHADER_CACHE");
  if (!folder || !*folder || (off && *off && strcmp(off, "0"))) return;
  if (mkdir(folder, 0755) && errno != EEXIST) {
    fprintf(stderr, "shader cache: can't make %s, so it won't be kept\n", folder);
    return;
  }
  snprintf(cache_folder, sizeof cache_folder, "%s", folder);
  atexit(save_driver_cache);
  pthread_t saver;
  pthread_create(&saver, 0, keep_saving, 0);
  pthread_detach(saver);
}

static uint32_t stage_find(uint64_t hash) {
  if (!stage_slot_count) return 0;
  uint32_t slot = (uint32_t)(hash * 0x9e3779b97f4a7c15ull >> 32) & (stage_slot_count - 1);
  while (stage_slots[slot] && stages[stage_slots[slot] - 1].hash != hash) slot = (slot + 1) & (stage_slot_count - 1);
  return stage_slots[slot];
}

static void stage_slots_fill(void) {
  for (uint32_t i = 0; i < stage_count; i++) {
    uint32_t slot = (uint32_t)(stages[i].hash * 0x9e3779b97f4a7c15ull >> 32) & (stage_slot_count - 1);
    while (stage_slots[slot]) slot = (slot + 1) & (stage_slot_count - 1);
    stage_slots[slot] = i + 1;
  }
}

static uint32_t stage_keep(uint64_t hash, const uint32_t *code, size_t bytes) {
  if (stage_count == stage_capacity) {
    stage_capacity = stage_capacity ? stage_capacity * 2 : FIRST_STAGE_SLOTS;
    stages = realloc(stages, stage_capacity * sizeof *stages);
  }
  VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
  vk_check(vkCreateShaderModule(vk.device, &info, 0, &stages[stage_count].module), "creating a shader");
  stages[stage_count].hash = hash;
  stage_count++;
  if (stage_count * 2 > stage_slot_count) {
    free(stage_slots);
    stage_slot_count = stage_slot_count ? stage_slot_count * 2 : FIRST_STAGE_SLOTS * 2;
    stage_slots = calloc(stage_slot_count, sizeof *stage_slots);
    stage_slots_fill();
  } else {
    uint32_t slot = (uint32_t)(hash * 0x9e3779b97f4a7c15ull >> 32) & (stage_slot_count - 1);
    while (stage_slots[slot]) slot = (slot + 1) & (stage_slot_count - 1);
    stage_slots[slot] = stage_count;
  }
  return stage_count;
}

// Each record: the hash, the SPIR-V's length in bytes, then the SPIR-V.
static void load_stages(FILE *file) {
  uint64_t hash;
  uint32_t bytes;
  while (fread(&hash, sizeof hash, 1, file) == 1 && fread(&bytes, sizeof bytes, 1, file) == 1) {
    if (!bytes || bytes % 4 || bytes > 16 * 1024 * 1024) break;
    uint32_t *code = malloc(bytes);
    int whole = fread(code, 1, bytes, file) == bytes && code[0] == SPIRV_MAGIC;
    if (whole && !stage_find(hash)) stage_keep(hash, code, bytes);
    free(code);
    if (!whole) break;
  }
  fclose(file);
}

static PipelineSlot *find_slot(const PipelineKey *key);
static void grow_pipelines(void);
static VkPipeline build_pipeline(const PipelineKey *key);

static void load_pipelines(FILE *file) {
  PipelineKey key;
  while (fread(&key, sizeof key, 1, file) == 1) {
    if (!stage_find(key.vertex) || !stage_find(key.fragment)) continue;  // Its stage didn't survive.
    if ((pipeline_count + 1) * 2 > pipeline_slots) grow_pipelines();
    PipelineSlot *slot = find_slot(&key);
    if (slot->pipeline) continue;
    slot->key = key;
    slot->pipeline = build_pipeline(&key);
    pipeline_count++;
  }
  fclose(file);
}

// Everything the last runs used is ready before the game draws anything.
static void load_cache(void) {
  if (!cache_folder[0]) return;
  double start = seconds_now();
  FILE *reading;
  stages_file = open_records("stages.bin", "WRST", 0, &reading);
  if (reading) load_stages(reading);
  pipelines_file = open_records("pipelines.bin", "WRPL", sizeof(PipelineKey), &reading);
  if (reading) load_pipelines(reading);
  if (stage_count || pipeline_count) {
    fprintf(stderr, "shader cache: %u stages and %u pipelines ready in %.0f ms\n", stage_count, pipeline_count, (seconds_now() - start) * 1000);
  }
}

void vk_pipelines_start(const GxAttribute *attributes, int attribute_count, size_t vertex_bytes) {
  create_set_layouts();
  create_pipeline_layout();
  compiler = shaderc_compiler_initialize();
  options = shaderc_compile_options_initialize();
  shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
  open_cache();
  size_t driver_bytes = 0;
  void *driver_data = cache_folder[0] ? read_whole("driver.bin", &driver_bytes) : 0;
  // The driver checks the data's own header, and starts empty if it came from another device or version.
  VkPipelineCacheCreateInfo cache = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = driver_bytes, .pInitialData = driver_data};
  if (vkCreatePipelineCache(vk.device, &cache, 0, &pipeline_cache) != VK_SUCCESS) {
    cache.initialDataSize = 0;
    cache.pInitialData = 0;
    vk_check(vkCreatePipelineCache(vk.device, &cache, 0, &pipeline_cache), "creating a pipeline cache");
  }
  free(driver_data);
  vertex_binding = (VkVertexInputBindingDescription){0, (uint32_t)vertex_bytes, VK_VERTEX_INPUT_RATE_VERTEX};
  static const VkFormat floats[] = {[2] = VK_FORMAT_R32G32_SFLOAT, [3] = VK_FORMAT_R32G32B32_SFLOAT, [4] = VK_FORMAT_R32G32B32A32_SFLOAT};
  static const VkFormat bytes[] = {[1] = VK_FORMAT_R8_UINT, [2] = VK_FORMAT_R8G8_UINT, [4] = VK_FORMAT_R8G8B8A8_UINT};
  vertex_attribute_count = (uint32_t)attribute_count;
  for (int i = 0; i < attribute_count; i++) {
    int components = attributes[i].components;
    VkFormat format = attributes[i].kind == GX_ATTRIBUTE_BYTE ? bytes[components] : floats[components];
    vertex_attributes[i] = (VkVertexInputAttributeDescription){(uint32_t)attributes[i].location, 0, format, (uint32_t)attributes[i].offset};
  }
  load_cache();
}

// A stage already compiled, in this run or an earlier one, is found by its text's hash.
// The hash covers the prefix too, so a change to it or to the device's features is a new stage.
GxStage vk_stage_compile(int kind, const char *text) {
  static char source[TEXT_BYTES];
  size_t bytes = (size_t)snprintf(source, sizeof source, "%s%s%s", prefix, vk.dual_source ? "" : "#define GX_NO_DUAL_SOURCE\n", text);
  if (bytes >= sizeof source) {
    fprintf(stderr, "shader: text too long\n");
    exit(9);
  }
  uint64_t hash = hash_text(source, bytes) ^ (uint64_t)(kind + 1);
  if (!hash) hash = 1;
  uint32_t found = stage_find(hash);
  if (found) return found;
  shaderc_shader_kind shaderc_kind = kind == GX_STAGE_VERTEX ? shaderc_vertex_shader : shaderc_fragment_shader;
  shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler, source, bytes, shaderc_kind, "wii-recomp", "main", options);
  if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
    fprintf(stderr, "shader: %s\n", shaderc_result_get_error_message(result));
    exit(9);
  }
  const uint32_t *code = (const uint32_t *)shaderc_result_get_bytes(result);
  uint32_t code_bytes = (uint32_t)shaderc_result_get_length(result);
  uint32_t stage = stage_keep(hash, code, code_bytes);
  if (stages_file) {
    fwrite(&hash, sizeof hash, 1, stages_file);
    fwrite(&code_bytes, sizeof code_bytes, 1, stages_file);
    fwrite(code, 1, code_bytes, stages_file);
    fflush(stages_file);
  }
  shaderc_result_release(result);
  return stage;
}

// Nothing is built until a pipeline needs it, as only then is the rest of the state known.
GxShader vk_shader_link(GxStage vertex, GxStage fragment) {
  if (shader_count == shader_capacity) {
    shader_capacity = shader_capacity ? shader_capacity * 2 : FIRST_SHADER_SLOTS;
    shaders = realloc(shaders, shader_capacity * sizeof *shaders);
  }
  shaders[shader_count] = (Shader){vertex - 1, fragment - 1};
  return ++shader_count;
}

static VkBlendFactor blend_factor(int which) {
  static const VkBlendFactor factors[] = {
    [GX_FACTOR_ZERO] = VK_BLEND_FACTOR_ZERO, [GX_FACTOR_ONE] = VK_BLEND_FACTOR_ONE, [GX_FACTOR_SOURCE_COLOR] = VK_BLEND_FACTOR_SRC_COLOR,
    [GX_FACTOR_INVERSE_SOURCE_COLOR] = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, [GX_FACTOR_TARGET_COLOR] = VK_BLEND_FACTOR_DST_COLOR,
    [GX_FACTOR_INVERSE_TARGET_COLOR] = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, [GX_FACTOR_SOURCE_ALPHA] = VK_BLEND_FACTOR_SRC_ALPHA,
    [GX_FACTOR_INVERSE_SOURCE_ALPHA] = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, [GX_FACTOR_TARGET_ALPHA] = VK_BLEND_FACTOR_DST_ALPHA,
    [GX_FACTOR_INVERSE_TARGET_ALPHA] = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, [GX_FACTOR_SOURCE1_ALPHA] = VK_BLEND_FACTOR_SRC1_ALPHA,
    [GX_FACTOR_INVERSE_SOURCE1_ALPHA] = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
  };
  if (!vk.dual_source && which == GX_FACTOR_SOURCE1_ALPHA) return VK_BLEND_FACTOR_SRC_ALPHA;
  if (!vk.dual_source && which == GX_FACTOR_INVERSE_SOURCE1_ALPHA) return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  return factors[which];
}

static VkPipeline build_pipeline(const PipelineKey *key) {
  static const VkCompareOp tests[] = {
    [GX_TEST_NEVER] = VK_COMPARE_OP_NEVER, [GX_TEST_LESS] = VK_COMPARE_OP_LESS, [GX_TEST_EQUAL] = VK_COMPARE_OP_EQUAL,
    [GX_TEST_LESS_EQUAL] = VK_COMPARE_OP_LESS_OR_EQUAL, [GX_TEST_GREATER] = VK_COMPARE_OP_GREATER, [GX_TEST_NOT_EQUAL] = VK_COMPARE_OP_NOT_EQUAL,
    [GX_TEST_GREATER_EQUAL] = VK_COMPARE_OP_GREATER_OR_EQUAL, [GX_TEST_ALWAYS] = VK_COMPARE_OP_ALWAYS,
  };
  static const VkCullModeFlags culls[] = {[GX_CULL_NONE] = VK_CULL_MODE_NONE, [GX_CULL_FRONT] = VK_CULL_MODE_FRONT_BIT,
                                          [GX_CULL_BACK] = VK_CULL_MODE_BACK_BIT, [GX_CULL_ALL] = VK_CULL_MODE_FRONT_AND_BACK};
  const GxRenderState *state = &key->state;
  VkShaderModule vertex = stages[stage_find(key->vertex) - 1].module, fragment = stages[stage_find(key->fragment) - 1].module;
  VkPipelineShaderStageCreateInfo stage_info[2] = {
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main"},
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"},
  };
  VkPipelineVertexInputStateCreateInfo input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1,
                                                .pVertexBindingDescriptions = &vertex_binding, .vertexAttributeDescriptionCount = vertex_attribute_count,
                                                .pVertexAttributeDescriptions = vertex_attributes};
  VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
  // The viewport's negative height keeps triangles facing as they do in OpenGL.
  VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
                                                   .cullMode = culls[state->cull], .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
  VkPipelineMultisampleStateCreateInfo multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
  VkPipelineDepthStencilStateCreateInfo depth = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = state->depth_test_on,
                                                 .depthWriteEnable = state->depth_write, .depthCompareOp = tests[state->depth_test]};
  VkColorComponentFlags color = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
  VkBlendOp operation = state->blend_subtract ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
  VkBlendOp alpha_operation = state->blend_subtract_alpha ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
  VkPipelineColorBlendAttachmentState blend = {
    .blendEnable = state->blend_on, .srcColorBlendFactor = blend_factor(state->blend_source), .dstColorBlendFactor = blend_factor(state->blend_target),
    .colorBlendOp = operation, .srcAlphaBlendFactor = blend_factor(state->blend_source_alpha),
    .dstAlphaBlendFactor = blend_factor(state->blend_target_alpha), .alphaBlendOp = alpha_operation,
    .colorWriteMask = (state->color_write ? color : 0) | (state->alpha_write ? VK_COLOR_COMPONENT_A_BIT : 0),
  };
  VkPipelineColorBlendStateCreateInfo blending = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend};
  static const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dynamics};
  VkGraphicsPipelineCreateInfo info = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stage_info, .pVertexInputState = &input,
    .pInputAssemblyState = &assembly, .pViewportState = &viewport, .pRasterizationState = &raster, .pMultisampleState = &multisample,
    .pDepthStencilState = &depth, .pColorBlendState = &blending, .pDynamicState = &dynamic, .layout = vk.pipeline_layout, .renderPass = vk.render_pass,
  };
  VkPipeline pipeline;
  vk_check(vkCreateGraphicsPipelines(vk.device, pipeline_cache, 1, &info, 0, &pipeline), "creating a pipeline");
  __atomic_store_n(&driver_changed, 1, __ATOMIC_RELEASE);
  return pipeline;
}

static uint32_t hash_key(const PipelineKey *key) {
  const uint8_t *bytes = (const uint8_t *)key;
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < sizeof *key; i++) hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}

static PipelineSlot *find_slot(const PipelineKey *key) {
  uint32_t slot = hash_key(key) & (pipeline_slots - 1);
  while (pipelines[slot].pipeline && memcmp(&pipelines[slot].key, key, sizeof *key)) slot = (slot + 1) & (pipeline_slots - 1);
  return &pipelines[slot];
}

static void grow_pipelines(void) {
  PipelineSlot *old = pipelines;
  uint32_t old_slots = pipeline_slots;
  pipeline_slots = old_slots ? old_slots * 2 : FIRST_PIPELINE_SLOTS;
  pipelines = calloc(pipeline_slots, sizeof *pipelines);
  for (uint32_t i = 0; i < old_slots; i++) {
    if (old[i].pipeline) *find_slot(&old[i].key) = old[i];
  }
  free(old);
}

VkPipeline vk_pipeline_find(GxShader shader, const GxRenderState *state) {
  PipelineKey key;
  memset(&key, 0, sizeof key);
  key.vertex = stages[shaders[shader - 1].vertex].hash;
  key.fragment = stages[shaders[shader - 1].fragment].hash;
  key.state = *state;
  if ((pipeline_count + 1) * 2 > pipeline_slots) grow_pipelines();
  PipelineSlot *slot = find_slot(&key);
  if (!slot->pipeline) {
    slot->key = key;
    slot->pipeline = build_pipeline(&key);
    pipeline_count++;
    if (pipelines_file) {
      fwrite(&key, sizeof key, 1, pipelines_file);
      fflush(pipelines_file);
    }
  }
  return slot->pipeline;
}

// Bias stays out of samplers, as Vulkan on Metal has none. See read_map_biases in gx_tev.c.
static VkSampler create_sampler(const GxSampling *sampling, int nearest_only) {
  static const VkSamplerAddressMode wraps[] = {[GX_WRAP_CLAMP] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, [GX_WRAP_REPEAT] = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                               [GX_WRAP_MIRROR] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
  VkSamplerCreateInfo info = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = sampling->magnify_linear && !nearest_only ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
    .minFilter = sampling->minify_linear && !nearest_only ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
    .mipmapMode = sampling->mip_mode == GX_MIP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
    .addressModeU = wraps[sampling->wrap_s], .addressModeV = wraps[sampling->wrap_t], .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
    .minLod = (float)sampling->min_level / LEVEL_STEPS,
    .maxLod = sampling->mip_mode == GX_MIP_NONE ? NO_MIPMAP_MAX_LOD : (float)sampling->max_level / LEVEL_STEPS,
  };
  if (sampling->anisotropy && vk.max_anisotropy > 1 && !nearest_only) {
    float wanted = (float)(1 << sampling->anisotropy);
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy = wanted < vk.max_anisotropy ? wanted : vk.max_anisotropy;
  }
  VkSampler sampler;
  vk_check(vkCreateSampler(vk.device, &info, 0, &sampler), "creating a sampler");
  return sampler;
}

// Every draw asks for all eight maps, so finding one must be quick.
VkSampler vk_sampler_find(const GxSampling *sampling, int depth) {
  int nearest_only = depth && !vk.depth_filters_linear;
  GxSampling keyed = *sampling;
  keyed.anisotropy = (uint8_t)(sampling->anisotropy | GX_SAMPLING_IN_USE | nearest_only << 6);  // No key is ever 0, a free slot.
  uint64_t key;
  memcpy(&key, &keyed, sizeof key);
  uint32_t slot = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 40) & (SAMPLER_SLOTS - 1);
  while (samplers[slot].key && samplers[slot].key != key) slot = (slot + 1) & (SAMPLER_SLOTS - 1);
  if (samplers[slot].key) return samplers[slot].sampler;
  if (sampler_count >= vk.limits.maxSamplerAllocationCount || sampler_count >= SAMPLER_SLOTS / 2) {
    debug_unsupported("limit.texture_samplers", "more texture samplers than the device allows", SAMPLER_SLOTS / 2);
    return first_sampler;
  }
  samplers[slot].key = key;
  samplers[slot].sampler = create_sampler(sampling, nearest_only);
  if (!sampler_count++) first_sampler = samplers[slot].sampler;
  return samplers[slot].sampler;
}
