// /dev/es requests that read a title's contents: open by content index, read, seek
// and close. Contents are read from storage, already decrypted as Dolphin keeps them.
#include <stdio.h>

#include "memory_watch.h"
#include "cpu.h"
#include "es.h"
#include "title.h"
#include "ios.h"
#include "titles.h"

enum { MAX_CONTENTS = 16, MAX_PATH = 1024, SEEK_SET_MODE = 0, SEEK_CURRENT = 1, SEEK_END_MODE = 2 };

typedef struct {
  FILE *file;
  long size;
} OpenContent;

static OpenContent opened[MAX_CONTENTS];
static uint8_t tmd[TMD_MAX_BYTES];

// Opens a title's content by its index in the TMD. Returns a content handle or an error.
static int open_content(uint64_t title, uint32_t index) {
  long size = title_tmd(title, tmd, sizeof tmd);
  if (size < 0) return FS_ENOENT;
  int count = tmd_check(tmd, (size_t)size);
  for (int i = 0; i < count; i++) {
    TmdContent content = tmd_content(tmd, i);
    if (content.index != index) continue;
    char path[TITLE_PATH_BYTES], full[MAX_PATH];
    if (!title_content_path(title, &content, path, sizeof path) || !files_find(path, full, sizeof full)) return FS_ENOENT;
    for (int handle = 0; handle < MAX_CONTENTS; handle++) {
      if (opened[handle].file) continue;
      FILE *file = fopen(full, "rb");
      if (!file) return FS_ENOENT;
      fseek(file, 0, SEEK_END);
      opened[handle] = (OpenContent){file, ftell(file)};
      rewind(file);
      if (ios_trace_all()) fprintf(stderr, "es: content %u of %016llx is %s\n", index, (unsigned long long)title, path);
      return handle;
    }
    return ES_EINVAL;  // Too many open at once.
  }
  return FS_ENOENT;
}

static OpenContent *handle_at(const IosVectors *vectors) {
  uint32_t in = ios_in_sized(vectors, 0, 4);
  uint32_t handle = in ? rd32(in) : MAX_CONTENTS;
  return handle < MAX_CONTENTS && opened[handle].file ? &opened[handle] : 0;
}

// A content of the running title.
static int open_own_content(const IosVectors *vectors) {
  uint32_t in = ios_in_sized(vectors, 0, 4);
  return in ? open_content((uint64_t)title->title_high << 32 | title->title_low, rd32(in)) : ES_EINVAL;
}

// A content of any title, with its ticket view.
static int open_title_content(const IosVectors *vectors) {
  uint32_t title = ios_in_sized(vectors, 0, 8), index = ios_in_sized(vectors, 2, 4);
  return title && index ? open_content(es_read_title(title), rd32(index)) : ES_EINVAL;
}

static int read_content(const IosVectors *vectors) {
  OpenContent *content = handle_at(vectors);
  uint32_t out = ios_out(vectors, 0), bytes = ios_out_bytes(vectors, 0);
  if (!content || (!out && bytes)) return ES_EINVAL;
  if (bytes) watch_before_write(host(out), bytes);
  return bytes ? (int)fread(host(out), 1, bytes, content->file) : 0;
}

static int seek_content(const IosVectors *vectors) {
  OpenContent *content = handle_at(vectors);
  uint32_t offset = ios_in_sized(vectors, 1, 4), mode = ios_in_sized(vectors, 2, 4);
  if (!content || !offset || !mode) return ES_EINVAL;
  long base = rd32(mode) == SEEK_SET_MODE ? 0 : rd32(mode) == SEEK_CURRENT ? ftell(content->file)
            : rd32(mode) == SEEK_END_MODE ? content->size : -1;
  long position = base + (int32_t)rd32(offset);
  if (base < 0 || position < 0 || position > content->size) return ES_EINVAL;
  fseek(content->file, position, SEEK_SET);
  return (int)position;
}

static int close_content(const IosVectors *vectors) {
  OpenContent *content = handle_at(vectors);
  if (!content) return ES_EINVAL;
  fclose(content->file);
  content->file = 0;
  return 0;
}

const EsRequest es_content_requests[] = {
  {0x09, 1, 0, open_own_content},
  {0x0a, 1, 1, read_content},
  {0x0b, 1, 0, close_content},
  {0x23, 3, 0, seek_content},
  {0x24, 3, 0, open_title_content},
  {0, 0, 0, 0},
};
