// The console's own certificate authority, added to Opera's list of trusted ones as a
// title reads it. The Wii Shop Channel's browser is Opera, which speaks TLS itself and
// trusts only the authorities in opera/opcacrt6.dat, inside a U8 archive among the
// title's contents. The console's local server (recomp/tools/shop) answers the Shop's
// secure pages with a certificate from the console's own authority, so that authority
// is added to the list as ES hands the archive over: Nintendo's entries stay, the
// stored title is never changed, and any title carrying Opera gets the same.
//
// The archive gets the longer list at its end, and the list's entry points there.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "opera_trust.h"

enum { U8_MAGIC = 0x55aa382d, NODE_BYTES = 12, FOLDER = 1, ALIGN = 0x20, LIST_HEADER_BYTES = 12, MAX_DEPTH = 32 };

static uint32_t be32(const uint8_t *at) { return (uint32_t)at[0] << 24 | at[1] << 16 | at[2] << 8 | at[3]; }

static void put32(uint8_t *at, uint32_t value) {
  at[0] = value >> 24;
  at[1] = value >> 16;
  at[2] = value >> 8;
  at[3] = value;
}

// The node of opera/opcacrt6.dat in a U8 archive, or 0.
static uint8_t *find_list(uint8_t *data, long size) {
  if (size < 0x20 || be32(data) != U8_MAGIC) return 0;
  uint32_t root = be32(data + 4);
  if (root + NODE_BYTES > (uint32_t)size) return 0;
  uint32_t count = be32(data + root + 8);
  uint64_t names = root + (uint64_t)count * NODE_BYTES;
  if (!count || names > (uint64_t)size) return 0;
  // Which folder each node is in: a folder's nodes run up to the index it gives as its end.
  uint32_t ends[MAX_DEPTH];
  const char *folders[MAX_DEPTH];
  int depth = 0;
  for (uint32_t i = 1; i < count; i++) {
    uint8_t *node = data + root + i * NODE_BYTES;
    while (depth && ends[depth - 1] <= i) depth--;
    uint32_t name_at = be32(node) & 0xffffff;
    if (names + name_at >= (uint64_t)size) return 0;
    const char *name = (const char *)data + names + name_at;
    if (node[0] == FOLDER) {
      if (depth == MAX_DEPTH) return 0;
      ends[depth] = be32(node + 8);
      folders[depth++] = name;
    } else if (depth && !strcmp(name, "opcacrt6.dat") && !strcmp(folders[depth - 1], "opera")) {
      return node;
    }
  }
  return 0;
}

int opera_trust_add(uint8_t **data, long *size) {
  uint8_t *node = find_list(*data, *size);
  if (!node) return 0;
  uint32_t at = be32(node + 4), bytes = be32(node + 8);
  if ((uint64_t)at + bytes > (uint64_t)*size || bytes < LIST_HEADER_BYTES) return 0;
  long entry_bytes;
  uint8_t *entry = net_opera_entry(&entry_bytes);
  if (!entry) {
    fprintf(stderr, "net: Opera's trusted list is left as it is, without the console's authority\n");
    return 0;
  }
  if (bytes >= entry_bytes && memmem(*data + at, bytes, entry, (size_t)entry_bytes)) {
    free(entry);
    return 0;  // Already there.
  }
  long list_at = (*size + ALIGN - 1) & ~(long)(ALIGN - 1), new_size = list_at + bytes + entry_bytes;
  uint8_t *grown = realloc(*data, (size_t)new_size);
  if (!grown) {
    free(entry);
    return 0;
  }
  long node_at = node - *data;
  memset(grown + *size, 0, (size_t)(list_at - *size));
  memmove(grown + list_at, grown + at, bytes);
  memcpy(grown + list_at + bytes, entry, (size_t)entry_bytes);
  put32(grown + node_at + 4, (uint32_t)list_at);
  put32(grown + node_at + 8, bytes + (uint32_t)entry_bytes);
  free(entry);
  *data = grown;
  *size = new_size;
  fprintf(stderr, "net: Opera trusts the console's own authority, for its local server\n");
  return 1;
}
