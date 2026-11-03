// The buffers an ioctlv request carries: its inputs, then its outputs, each an
// address and a size. Reading them out of range gives 0, never memory past the list.
#pragma once
#include <stdint.h>

#include "cpu.h"

typedef struct {
  uint32_t inputs, outputs, list;
} IosVectors;

enum { IOS_VECTOR_BYTES = 8 };

static inline IosVectors ios_vectors(uint32_t request) {
  IosVectors vectors = {rd32(request + 16), rd32(request + 20), rd32(request + 24)};
  return vectors;
}

// The guest address of a vector's buffer, or 0 if there's no such vector or no buffer.
static inline uint32_t ios_vector_address(const IosVectors *vectors, uint32_t index) {
  if (!vectors->list || index >= vectors->inputs + vectors->outputs) return 0;
  uint32_t address = rd32((vectors->list | 0x80000000u) + index * IOS_VECTOR_BYTES);
  return address ? address | 0x80000000u : 0;
}

static inline uint32_t ios_vector_bytes(const IosVectors *vectors, uint32_t index) {
  if (!vectors->list || index >= vectors->inputs + vectors->outputs) return 0;
  return rd32((vectors->list | 0x80000000u) + index * IOS_VECTOR_BYTES + 4);
}

static inline uint32_t ios_in(const IosVectors *vectors, uint32_t index) {
  return index < vectors->inputs ? ios_vector_address(vectors, index) : 0;
}

static inline uint32_t ios_in_bytes(const IosVectors *vectors, uint32_t index) {
  return index < vectors->inputs ? ios_vector_bytes(vectors, index) : 0;
}

static inline uint32_t ios_out(const IosVectors *vectors, uint32_t index) {
  return index < vectors->outputs ? ios_vector_address(vectors, vectors->inputs + index) : 0;
}

static inline uint32_t ios_out_bytes(const IosVectors *vectors, uint32_t index) {
  return index < vectors->outputs ? ios_vector_bytes(vectors, vectors->inputs + index) : 0;
}

// An input or output with room for at least `bytes`, or 0.
static inline uint32_t ios_in_sized(const IosVectors *vectors, uint32_t index, uint32_t bytes) {
  return ios_in_bytes(vectors, index) >= bytes ? ios_in(vectors, index) : 0;
}

static inline uint32_t ios_out_sized(const IosVectors *vectors, uint32_t index, uint32_t bytes) {
  return ios_out_bytes(vectors, index) >= bytes ? ios_out(vectors, index) : 0;
}
