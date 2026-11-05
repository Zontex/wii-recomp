// Rare commands and registers of the command stream, through the real gx_fifo.c: the
// metrics command, a draw larger than the old 4 MB stream, and the FIFO's read-write distance.
#include "../../recomp/runtime/gx_fifo.c"
#include "../../recomp/runtime/timeline.c"
#include "../harness.h"
void stage_add(int s, uint64_t t) {}
const Title *title = &(Title){0};
void poll_interrupts(void) {}
void stage_frame(float g) {}
static uint8_t regs[0x10000];
uint32_t hw_peek(uint32_t a, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | regs[(a + i) & 0xffff]; return v; }
void hw_poke(uint32_t a, uint32_t v, int n) { for (int i = n - 1; i >= 0; i--, v >>= 8) regs[(a + i) & 0xffff] = (uint8_t)v; }
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
void raise_irq(int b) {}
void clear_irq(int b) {}
uint64_t time_base(void) { return 0; }
void gx_load_palette(void) {}
void debug_unsupported(const char *i, const char *w, uint32_t v) {}
static int gaps;
static const char *last_gap;
void gap_hit(const char *i, uint32_t d) { gaps++; last_gap = i; }
void gx_present(void) {}
void gx_copy_to_texture(uint32_t c, int d) {}
void gx_clear(void) {}
static int vertices_read;
// Every vertex of format 0 is the largest a vertex can be.
uint32_t gx_vertices_size(int f, int c) { return (uint32_t)c * LARGEST_VERTEX_BYTES; }
int gx_read_vertices(const uint8_t *d, int f, int c, Vertex *o) { vertices_read += c; return c; }
int gx_draw_wanted(int p, int c) { return 0; }
Vertex *gx_draw_reserve(int c) { return 0; }
void gx_draw(int p, int c) {}
int gx_draw_expanded(int p, int f, const uint8_t *d, int c) { return 0; }
int gx_vertex_format;
GxCalledList gx_called_list;
int gx_read_list_vertices(const uint8_t *d, int f, int c, Vertex *o) { return c; }
uint64_t watch_range(uint32_t a, uint32_t b) { return 0; }
void gx_record_command(const uint8_t *b, uint32_t c) {}
void gx_record_frame_shown(void) {}
void gx_record_list_enter(void) {}
void gx_record_list_leave(void) {}

enum { BASE = 0x100000, END = BASE + 0x10000 - 4, LIMIT = BASE + 0x10000 };

static void send(uint32_t value, int bytes) { graphics_write(PIPE_FIRST, value, bytes); }

int main(void) {
  memory_start();
  setenv("WII_RECOMP_GX_INLINE", "1", 1);  // The chip reads on this thread, so the test sees it finish.

  // The metrics command is one byte, and the BP command after it lands whole.
  const uint8_t metrics[] = {COMMAND_METRICS, COMMAND_BP, 0xf3, 0x12, 0x34, 0x56};
  gx_parse(metrics, sizeof metrics);
  CHECK_EQ(gx.bp[0xf3], 0x123456);
  CHECK_EQ(gaps, 0);

  // Settings we don't follow are named only when set.
  const uint8_t settings[] = {COMMAND_BP, BP_PRELOAD, 0, 0, 0, COMMAND_BP, BP_GEN_MODE, 0x08, 0, 0};
  gx_parse(settings, sizeof settings);
  CHECK_EQ(gaps, 1);
  CHECK(last_gap && !strcmp(last_gap, "pixel.z_freeze"));
  const uint8_t clip[] = {COMMAND_XF, 0, 1, 0x10, 0x04, 0, 0, 0, 0, 0, 0, 0, 1};  // 0x1004 and 0x1005.
  gx_parse(clip, sizeof clip);
  CHECK(last_gap && !strcmp(last_gap, "gx.0x1005_clip_disable"));

  // The largest draw passes through the FIFO, a ring far smaller than it, in bursts.
  hw_poke(PI_FIFO_BASE, BASE, 4);
  hw_poke(PI_FIFO_END, END, 4);
  hw_poke(PI_FIFO_WRITE, BASE, 4);
  write_pair(CP_FIFO_BASE, BASE);
  write_pair(CP_FIFO_END, END);
  write_pair(CP_FIFO_READ, BASE);
  write_pair(CP_FIFO_WRITE, BASE);
  hw_poke(CP_CONTROL, CONTROL_READ_ENABLE | CONTROL_LINK, 2);
  send(COMMAND_DRAW, 1);
  send(65535, 2);
  for (uint32_t i = 0; i < 65535u * LARGEST_VERTEX_BYTES; i++) send(0, 1);
  send(COMMAND_BP, 1);  // Then something to finish the last burst.
  send(0xf3654321, 4);
  for (int i = 0; i < 27; i++) send(COMMAND_NOP, 1);
  CHECK_EQ(vertices_read, 65535);
  CHECK_EQ(gx.bp[0xf3], 0x654321);
  CHECK_EQ(stream_used, 0);

  // How much the chip has still to read, straight and across the turn.
  write_pair(CP_FIFO_READ, BASE + 0x100);
  write_pair(CP_FIFO_WRITE, BASE + 0x340);
  uint32_t value = 0;
  CHECK(graphics_read(CP_DISTANCE_LOW, &value));
  CHECK_EQ(value, 0x240);
  CHECK(graphics_read(CP_DISTANCE_HIGH, &value));
  CHECK_EQ(value, 0);
  write_pair(CP_FIFO_READ, LIMIT - 0x40);
  write_pair(CP_FIFO_WRITE, BASE + 0x20);
  CHECK(graphics_read(CP_DISTANCE_LOW, &value));
  CHECK_EQ(value, 0x60);
  return finish();
}
