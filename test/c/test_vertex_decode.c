// The planned vertex decoder against the general one (gx_vertex.c), on made-up formats:
// every kind, type and count of each part, arrays starting anywhere in memory or outside
// it, and streams of random bytes, so null and far indexes come up too. Each draw must
// decode to the same bytes either way, and leave the same kept normals.
#include "../../recomp/runtime/gx_vertex.c"
#include "harness.h"

GxState gx;
unsigned gx_cp_written = 1;
void debug_unsupported(const char *id, const char *what, uint32_t value) { (void)id, (void)what, (void)value; }
int watch_enabled(void) { return 0; }
uint64_t watch_range(uint32_t address, uint32_t bytes) { (void)address, (void)bytes; return WATCH_NONE; }
uint64_t watch_count(uint32_t address, uint32_t bytes) { (void)address, (void)bytes; return WATCH_NONE; }

static uint32_t seed = 12345;
static uint32_t next(void) {
  seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
  return seed;
}

enum { MOST = 48, DRAWS = 200000 };

// Where an array may start: memory 1 and 2 anywhere, near their ends, the locked cache,
// and the gaps between, with the high bits games set or not. Near an end the elements are
// 64 bytes apart from a 64-byte boundary, so the last one starts at least 64 bytes before
// the end and none runs past it; whether an index is past the end is still tested.
static void some_array(int array) {
  static const uint32_t places[] = {0x00000000u, 0x00100000u, MEM2_START, 0x01900000u, 0x0ff00000u, 0x30000000u};
  static const uint32_t ends[] = {MEM1_SIZE - 0x2000u, MEM1_SIZE - 0x40u, MEM2_START + MEM2_SIZE - 0x3000u, LOCKED_CACHE_PHYSICAL};
  uint32_t base, stride;
  if (next() % 3) {
    base = places[next() % (sizeof places / sizeof *places)] + (next() % 0x1000) * 4, stride = next() & 0xff;
  } else {
    base = ends[next() % (sizeof ends / sizeof *ends)] + (next() % 4) * 0x40, stride = 0x40;
  }
  gx.cp[CP_ARRAY_BASE + array] = next() % 4 == 0 ? base | 0x80000000u : base;
  gx.cp[CP_ARRAY_STRIDE + array] = stride;
}

int main(void) {
  memory_start();
  for (uint32_t at = 0; at < MEM1_SIZE; at += 4) memcpy(host(at), &(uint32_t){next()}, 4);
  for (uint32_t at = 0; at < MEM2_SIZE; at += 4) memcpy(host(MEM2_START + at), &(uint32_t){next()}, 4);
  for (uint32_t at = 0; at < LOCKED_CACHE_SIZE; at += 4) memcpy(host(LOCKED_CACHE_PHYSICAL + at), &(uint32_t){next()}, 4);

  static uint8_t stream[MOST * 64];
  static Vertex planned[MOST], general[MOST];
  int fast_draws = 0;
  for (int draw = 0; draw < DRAWS; draw++) {
    int format = (int)(next() % 8);
    // Most draws as games send them, all four parts indexed by 16 bits; the rest anything.
    if (draw % 2) {
      gx.cp[CP_DESCRIPTOR_LOW] = 0x7e00u | (next() & 1), gx.cp[CP_DESCRIPTOR_HIGH] = 3;
    } else {
      gx.cp[CP_DESCRIPTOR_LOW] = next() & 0x1ffffu, gx.cp[CP_DESCRIPTOR_HIGH] = next() & 0xffffu;
    }
    gx.cp[CP_FORMAT_A + format] = next(), gx.cp[CP_FORMAT_B + format] = next(), gx.cp[CP_FORMAT_C + format] = next();
    gx.cp[CP_MATRIX_INDEX] = next(), gx.cp[CP_MATRIX_INDEX + 0x10] = next();
    for (int a = 0; a < ARRAYS; a++) some_array(a);
    gx_cp_written++;
    int count = 1 + (int)(next() % MOST);
    for (size_t i = 0; i < sizeof stream; i++) stream[i] = (uint8_t)next();
    // Indexes near the start of their arrays, or all ones, more often than random bytes give.
    if (next() % 2) {
      for (size_t i = 0; i + 1 < sizeof stream; i += 2) stream[i] = next() % 8 ? 0 : 0xff;
    }
    memset(planned, 0x5a, sizeof planned);
    memset(general, 0x5a, sizeof general);
    float normals[CACHED_NORMALS][4];
    int kept_planned = gx_read_vertices(stream, format, count, planned);
    memcpy(normals, gx_cached_normals, sizeof normals);
    int kept_general = read_vertices_general(stream, format, count, general);
    fast_draws += plans[format].ready;
    if (kept_planned != kept_general || memcmp(planned, general, sizeof planned) || memcmp(normals, gx_cached_normals, sizeof normals)) {
      printf("FAILED draw %d: format %d, descriptor %05x %04x, words %08x %08x %08x, %d vertices: kept %d and %d\n", draw, format,
             gx.cp[CP_DESCRIPTOR_LOW], gx.cp[CP_DESCRIPTOR_HIGH], gx.cp[CP_FORMAT_A + format], gx.cp[CP_FORMAT_B + format],
             gx.cp[CP_FORMAT_C + format], count, kept_planned, kept_general);
      for (int v = 0; v < MOST; v++) {
        if (memcmp(&planned[v], &general[v], sizeof(Vertex))) {
          const uint8_t *a = (const uint8_t *)&planned[v], *b = (const uint8_t *)&general[v];
          for (size_t i = 0; i < sizeof(Vertex); i++) {
            if (a[i] != b[i]) {
              printf("  vertex %d differs first at byte %zu\n", v, i);
              break;
            }
          }
          break;
        }
      }
      failures++;
      if (failures > 3) break;
    }
  }
  CHECK(fast_draws > DRAWS / 3);  // The planned decoder, not the general one, decoded most.
  return finish();
}
