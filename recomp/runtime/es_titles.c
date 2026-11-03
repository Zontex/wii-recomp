// /dev/es requests about installed titles: lists, metadata, tickets and contents,
// answered from the console's storage through titles.c.
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "es.h"
#include "ios.h"
#include "titles.h"

enum { MAX_TITLES = 1024, HASH_BYTES = 20, MAP_ENTRY_BYTES = 28, MAX_SHARED = 4096 };

static uint8_t tmd[TMD_MAX_BYTES], tickets[TICKET_BYTES * MAX_TICKETS];

static int write_count(const IosVectors *vectors, uint32_t index, uint32_t count) {
  uint32_t out = ios_out_sized(vectors, index, 4);
  if (!out) return ES_EINVAL;
  wr32(out, count);
  return 0;
}

// The count a request asks for, from its input `index`, and room for that many items.
static uint32_t wanted(const IosVectors *vectors, uint32_t index, uint32_t item_bytes, uint32_t *out) {
  uint32_t in = ios_in_sized(vectors, index, 4);
  uint32_t count = in ? rd32(in) : 0;
  *out = ios_out_sized(vectors, 0, count * item_bytes);
  return count;
}

static int list(const IosVectors *vectors, int owned, int with_titles) {
  static uint64_t titles[MAX_TITLES];
  int count = owned ? titles_owned(titles, MAX_TITLES) : titles_installed(titles, MAX_TITLES);
  if (!with_titles) return write_count(vectors, 0, (uint32_t)count);
  uint32_t out;
  uint32_t limit = wanted(vectors, 0, 8, &out);
  if (!out && limit) return ES_EINVAL;
  for (uint32_t i = 0; i < limit && i < (uint32_t)count; i++) es_write_title(out + i * 8, titles[i]);
  return 0;
}

static int get_owned_title_count(const IosVectors *vectors) { return list(vectors, 1, 0); }
static int get_owned_titles(const IosVectors *vectors) { return list(vectors, 1, 1); }
static int get_title_count(const IosVectors *vectors) { return list(vectors, 0, 0); }
static int get_titles(const IosVectors *vectors) { return list(vectors, 0, 1); }

// The title named by input 0, with its TMD loaded. Returns its size, or an error.
static long load_tmd(const IosVectors *vectors, uint64_t *title) {
  uint32_t in = ios_in_sized(vectors, 0, 8);
  if (!in) return ES_EINVAL;
  *title = es_read_title(in);
  long size = title_tmd(*title, tmd, sizeof tmd);
  return size < 0 ? FS_ENOENT : size;
}

// The ids of a TMD's contents that are in storage, up to `limit`. Returns the count.
static uint32_t stored_contents(uint64_t title, const uint8_t *metadata, size_t bytes, uint32_t out, uint32_t limit) {
  int count = tmd_check(metadata, bytes);
  uint32_t found = 0;
  char path[TITLE_PATH_BYTES];
  for (int i = 0; i < count; i++) {
    TmdContent content = tmd_content(metadata, i);
    if (!title_content_path(title, &content, path, sizeof path)) continue;
    if (out && found < limit) wr32(out + found * 4, content.id);
    found++;
  }
  return found;
}

static int get_title_contents_count(const IosVectors *vectors) {
  uint64_t title;
  long size = load_tmd(vectors, &title);
  if (size < 0) return (int)size;
  return write_count(vectors, 0, stored_contents(title, tmd, (size_t)size, 0, 0));
}

static int get_title_contents(const IosVectors *vectors) {
  uint64_t title;
  long size = load_tmd(vectors, &title);
  if (size < 0) return (int)size;
  uint32_t out;
  uint32_t limit = wanted(vectors, 1, 4, &out);
  if (!out && limit) return ES_EINVAL;
  stored_contents(title, tmd, (size_t)size, out, limit);
  return 0;
}

// The same two requests, given a TMD rather than a title.
static int get_stored_content_count(const IosVectors *vectors) {
  uint32_t in = ios_in(vectors, 0), bytes = ios_in_bytes(vectors, 0);
  if (!in || tmd_check(host(in), bytes) < 0) return ES_EINVAL;
  return write_count(vectors, 0, stored_contents(tmd_title(host(in)), host(in), bytes, 0, 0));
}

static int get_stored_contents(const IosVectors *vectors) {
  uint32_t in = ios_in(vectors, 0), bytes = ios_in_bytes(vectors, 0), out;
  if (!in || tmd_check(host(in), bytes) < 0) return ES_EINVAL;
  uint32_t limit = wanted(vectors, 1, 4, &out);
  if (!out && limit) return ES_EINVAL;
  stored_contents(tmd_title(host(in)), host(in), bytes, out, limit);
  return 0;
}

static int get_stored_tmd_size(const IosVectors *vectors) {
  uint64_t title;
  long size = load_tmd(vectors, &title);
  return size < 0 ? (int)size : write_count(vectors, 0, (uint32_t)size);
}

static int get_stored_tmd(const IosVectors *vectors) {
  uint64_t title;
  long size = load_tmd(vectors, &title);
  if (size < 0) return (int)size;
  uint32_t out = ios_out_sized(vectors, 0, (uint32_t)size);
  if (!out) return ES_EINVAL;
  memcpy(host(out), tmd, (size_t)size);
  return 0;
}

static int get_tmd_view_size(const IosVectors *vectors) {
  uint64_t title;
  long size = load_tmd(vectors, &title);
  return size < 0 ? (int)size : write_count(vectors, 0, (uint32_t)tmd_view_bytes(tmd, (size_t)size));
}

static int get_tmd_views(const IosVectors *vectors) {
  uint64_t title;
  long size = load_tmd(vectors, &title);
  if (size < 0) return (int)size;
  uint32_t out = ios_out(vectors, 0);
  if (!out || !tmd_view(tmd, (size_t)size, host(out), ios_out_bytes(vectors, 0))) return ES_EINVAL;
  return 0;
}

static int load_tickets(const IosVectors *vectors) {
  uint32_t in = ios_in_sized(vectors, 0, 8);
  if (!in) return ES_EINVAL;
  long size = title_tickets(es_read_title(in), tickets, sizeof tickets);
  return size < 0 ? 0 : (int)(size / TICKET_BYTES);
}

static int get_view_count(const IosVectors *vectors) {
  int count = load_tickets(vectors);
  return count < 0 ? count : write_count(vectors, 0, (uint32_t)count);
}

static int get_views(const IosVectors *vectors) {
  int count = load_tickets(vectors);
  if (count < 0) return count;
  uint32_t out;
  uint32_t limit = wanted(vectors, 1, TICKET_VIEW_BYTES, &out);
  if (!out && limit) return ES_EINVAL;
  for (uint32_t i = 0; i < limit && i < (uint32_t)count; i++) ticket_view(tickets + i * TICKET_BYTES, host(out + i * TICKET_VIEW_BYTES));
  return 0;
}

// Limits on how often a title may be used, for rented titles. Nothing here has any.
static int get_consumption(const IosVectors *vectors) { return write_count(vectors, 1, 0); }

static int get_device_id(const IosVectors *vectors) { return write_count(vectors, 0, IOS_DEVICE_ID); }

// Sets which title's files the program may use. Our storage has no owners yet.
static int set_uid(const IosVectors *vectors) { return ios_in_sized(vectors, 0, 8) ? 0 : ES_EINVAL; }

const EsRequest es_title_requests[] = {
  {0x07, 0, 1, get_device_id},
  {0x0c, 0, 1, get_owned_title_count},
  {0x0d, 1, 1, get_owned_titles},
  {0x0e, 0, 1, get_title_count},
  {0x0f, 1, 1, get_titles},
  {0x10, 1, 1, get_title_contents_count},
  {0x11, 2, 1, get_title_contents},
  {0x12, 1, 1, get_view_count},
  {0x13, 2, 1, get_views},
  {0x14, 1, 1, get_tmd_view_size},
  {0x15, 2, 1, get_tmd_views},
  {0x16, 1, 2, get_consumption},
  {0x21, 1, 0, set_uid},
  {0x32, 1, 1, get_stored_content_count},
  {0x33, 2, 1, get_stored_contents},
  {0x34, 1, 1, get_stored_tmd_size},
  {0x35, 2, 1, get_stored_tmd},
  {0, 0, 0, 0},
};
