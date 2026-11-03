// Vulkan shaders, pipelines and samplers. A pipeline bakes in a shader with its
// blending, depth and culling, so each mix is built once, on first use, and kept.
#include <shaderc/shaderc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  "#define GX_DEPTH(z, w) ((z) + (w))\n";  // Vulkan's depth runs from 0, not -w.

typedef struct {
  VkShaderModule vertex, fragment;
} Shader;

typedef struct {
  GxShader shader;
  GxRenderState state;
} PipelineKey;

typedef struct {
  PipelineKey key;
  VkPipeline pipeline;
} PipelineSlot;

static shaderc_compiler_t compiler;
static shaderc_compile_options_t options;
static VkShaderModule *stages;
static uint32_t stage_count, stage_capacity;
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

void vk_pipelines_start(const GxAttribute *attributes, int attribute_count, size_t vertex_bytes) {
  create_set_layouts();
  create_pipeline_layout();
  compiler = shaderc_compiler_initialize();
  options = shaderc_compile_options_initialize();
  shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
  VkPipelineCacheCreateInfo cache = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  vk_check(vkCreatePipelineCache(vk.device, &cache, 0, &pipeline_cache), "creating a pipeline cache");
  vertex_binding = (VkVertexInputBindingDescription){0, (uint32_t)vertex_bytes, VK_VERTEX_INPUT_RATE_VERTEX};
  static const VkFormat floats[] = {[2] = VK_FORMAT_R32G32_SFLOAT, [3] = VK_FORMAT_R32G32B32_SFLOAT, [4] = VK_FORMAT_R32G32B32A32_SFLOAT};
  static const VkFormat bytes[] = {[1] = VK_FORMAT_R8_UINT, [2] = VK_FORMAT_R8G8_UINT, [4] = VK_FORMAT_R8G8B8A8_UINT};
  vertex_attribute_count = (uint32_t)attribute_count;
  for (int i = 0; i < attribute_count; i++) {
    int components = attributes[i].components;
    VkFormat format = attributes[i].kind == GX_ATTRIBUTE_BYTE ? bytes[components] : floats[components];
    vertex_attributes[i] = (VkVertexInputAttributeDescription){(uint32_t)attributes[i].location, 0, format, (uint32_t)attributes[i].offset};
  }
}

static VkShaderModule compile(shaderc_shader_kind kind, const char *text) {
  static char source[TEXT_BYTES];
  size_t bytes = (size_t)snprintf(source, sizeof source, "%s%s%s", prefix, vk.dual_source ? "" : "#define GX_NO_DUAL_SOURCE\n", text);
  if (bytes >= sizeof source) {
    fprintf(stderr, "shader: text too long\n");
    exit(9);
  }
  shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler, source, bytes, kind, "wii-recomp", "main", options);
  if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
    fprintf(stderr, "shader: %s\n", shaderc_result_get_error_message(result));
    exit(9);
  }
  VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = shaderc_result_get_length(result),
                                   .pCode = (const uint32_t *)shaderc_result_get_bytes(result)};
  VkShaderModule module;
  vk_check(vkCreateShaderModule(vk.device, &info, 0, &module), "creating a shader");
  shaderc_result_release(result);
  return module;
}

GxStage vk_stage_compile(int kind, const char *text) {
  if (stage_count == stage_capacity) {
    stage_capacity = stage_capacity ? stage_capacity * 2 : FIRST_STAGE_SLOTS;
    stages = realloc(stages, stage_capacity * sizeof *stages);
  }
  stages[stage_count] = compile(kind == GX_STAGE_VERTEX ? shaderc_vertex_shader : shaderc_fragment_shader, text);
  return ++stage_count;
}

// Nothing is built until a pipeline needs it, as only then is the rest of the state known.
GxShader vk_shader_link(GxStage vertex, GxStage fragment) {
  if (shader_count == shader_capacity) {
    shader_capacity = shader_capacity ? shader_capacity * 2 : FIRST_SHADER_SLOTS;
    shaders = realloc(shaders, shader_capacity * sizeof *shaders);
  }
  shaders[shader_count] = (Shader){stages[vertex - 1], stages[fragment - 1]};
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
  const Shader *shader = &shaders[key->shader - 1];
  VkPipelineShaderStageCreateInfo stages[2] = {
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader->vertex, .pName = "main"},
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader->fragment, .pName = "main"},
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
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &input,
    .pInputAssemblyState = &assembly, .pViewportState = &viewport, .pRasterizationState = &raster, .pMultisampleState = &multisample,
    .pDepthStencilState = &depth, .pColorBlendState = &blending, .pDynamicState = &dynamic, .layout = vk.pipeline_layout, .renderPass = vk.render_pass,
  };
  VkPipeline pipeline;
  vk_check(vkCreateGraphicsPipelines(vk.device, pipeline_cache, 1, &info, 0, &pipeline), "creating a pipeline");
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
  key.shader = shader;
  key.state = *state;
  if ((pipeline_count + 1) * 2 > pipeline_slots) grow_pipelines();
  PipelineSlot *slot = find_slot(&key);
  if (!slot->pipeline) {
    slot->key = key;
    slot->pipeline = build_pipeline(&key);
    pipeline_count++;
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
  VkSampler sampler;
  vk_check(vkCreateSampler(vk.device, &info, 0, &sampler), "creating a sampler");
  return sampler;
}

// Every draw asks for all eight maps, so finding one must be quick.
VkSampler vk_sampler_find(const GxSampling *sampling, int depth) {
  int nearest_only = depth && !vk.depth_filters_linear;
  GxSampling keyed = *sampling;
  keyed.unused = (uint8_t)(nearest_only + 1);  // So that no key is ever 0, which marks a free slot.
  uint64_t key;
  memcpy(&key, &keyed, sizeof key);
  uint32_t slot = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 40) & (SAMPLER_SLOTS - 1);
  while (samplers[slot].key && samplers[slot].key != key) slot = (slot + 1) & (SAMPLER_SLOTS - 1);
  if (samplers[slot].key) return samplers[slot].sampler;
  if (sampler_count >= vk.limits.maxSamplerAllocationCount || sampler_count >= SAMPLER_SLOTS / 2) {
    debug_unsupported("more texture samplers than the device allows", SAMPLER_SLOTS / 2);
    return first_sampler;
  }
  samplers[slot].key = key;
  samplers[slot].sampler = create_sampler(sampling, nearest_only);
  if (!sampler_count++) first_sampler = samplers[slot].sampler;
  return samplers[slot].sampler;
}
