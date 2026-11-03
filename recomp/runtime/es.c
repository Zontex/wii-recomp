// /dev/es, the title system: which titles are installed, their metadata and tickets,
// their contents, and who the running program is. Each request is one table entry,
// checked for its number of inputs and outputs before it runs.
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "es.h"
#include "game.h"
#include "ios.h"
#include "titles.h"

enum { DI_GET_TICKET_VIEW = 0x1b, GET_TITLE_DIR = 0x1d, GET_TITLE_ID = 0x20, CHECK_KOREA_REGION = 0x45, TITLE_ID_BYTES = 8, TITLE_DIR_BYTES = 30 };

uint64_t es_read_title(uint32_t address) { return (uint64_t)rd32(address) << 32 | rd32(address + 4); }

void es_write_title(uint32_t address, uint64_t title) {
  wr32(address, (uint32_t)(title >> 32));
  wr32(address + 4, (uint32_t)title);
}

// Where a title keeps its saves.
static int get_title_dir(const IosVectors *vectors) {
  uint32_t title = ios_in_sized(vectors, 0, TITLE_ID_BYTES), out = ios_out_sized(vectors, 0, TITLE_DIR_BYTES);
  if (!title || !out) return ES_EINVAL;
  uint64_t id = es_read_title(title);
  snprintf((char *)host(out), TITLE_DIR_BYTES, "/title/%08x/%08x/data", (uint32_t)(id >> 32), (uint32_t)id);
  return 0;
}

// The running program's own title.
static int get_title_id(const IosVectors *vectors) {
  uint32_t out = ios_out_sized(vectors, 0, TITLE_ID_BYTES);
  if (!out) return ES_EINVAL;
  es_write_title(out, (uint64_t)game_title_high << 32 | game_title_low);
  return 0;
}

// The ticket of the disc that's in, or of the one passed in.
static int di_get_ticket_view(const IosVectors *vectors) {
  static uint8_t ticket[TICKET_BYTES];
  uint32_t out = ios_out_sized(vectors, 0, TICKET_VIEW_BYTES), given = ios_in_sized(vectors, 0, TICKET_BYTES);
  if (!out) return ES_EINVAL;
  if (given) memcpy(ticket, host(given), TICKET_BYTES);
  else if (!dvd_ticket(ticket, TICKET_BYTES)) return ES_EINVAL;
  ticket_view(ticket, host(out));
  return 0;
}

// Whether this is a Korean console. The menu asks, to spot one changed to another region.
static int check_korea_region(const IosVectors *vectors) { return ES_EINVAL; }

static const EsRequest requests[] = {
  {GET_TITLE_DIR, 1, 1, get_title_dir},
  {GET_TITLE_ID, 0, 1, get_title_id},
  {DI_GET_TICKET_VIEW, 1, 1, di_get_ticket_view},
  {CHECK_KOREA_REGION, 0, 0, check_korea_region},
  {0, 0, 0, 0},
};

static const EsRequest *find(const EsRequest *table, uint32_t code) {
  for (; table->run; table++) if (table->code == code) return table;
  return 0;
}

int es_request(uint32_t command, uint32_t request) {
  if (command != IOS_IOCTLV) return IOS_UNHANDLED;
  uint32_t code = rd32(request + 12);
  IosVectors vectors = ios_vectors(request);
  const EsRequest *found = find(requests, code);
  if (!found) found = find(es_title_requests, code);
  if (!found) found = find(es_content_requests, code);
  if (found) {
    if (vectors.inputs != found->inputs || vectors.outputs != found->outputs) {
      fprintf(stderr, "es: request %x came with %u inputs and %u outputs\n", code, vectors.inputs, vectors.outputs);
      return ES_EINVAL;
    }
    return found->run(&vectors);
  }
  return IOS_UNHANDLED;
}
