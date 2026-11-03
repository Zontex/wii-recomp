// The parts of /dev/es shared between its files.
#pragma once
#include <stdint.h>

#include "ios_vectors.h"

// ES's own answer for a request with the wrong buffers.
enum { ES_EINVAL = -1017 };

typedef struct {
  uint32_t code, inputs, outputs;
  int (*run)(const IosVectors *vectors);
} EsRequest;

// Requests about installed titles, from es_titles.c. Ends with an empty entry.
extern const EsRequest es_title_requests[];
// Requests that read contents, from es_content.c.
extern const EsRequest es_content_requests[];

uint64_t es_read_title(uint32_t address);
void es_write_title(uint32_t address, uint64_t title);
