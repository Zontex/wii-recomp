// The FIFO ring limit and 32-byte write bursts, through the real gx_fifo.c.
#include "../../recomp/runtime/gx_fifo.c"
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
void gap_hit(const char *i, uint32_t d) {}
void gx_present(void) {}
void gx_copy_to_texture(uint32_t c, int d) {}
void gx_clear(void) {}
uint32_t gx_vertices_size(int f, int c) { return 0; }
int gx_read_vertices(const uint8_t *d, int f, int c, Vertex *o) { return c; }
int gx_draw_wanted(int p, int c) { return 0; }
Vertex *gx_draw_reserve(int c) { return 0; }
void gx_draw(int p, int c) {}
int gx_draw_expanded(int p, int f, const uint8_t *d, int c) { return 0; }
int gx_vertex_format;

enum { BASE = 0x100000, END = BASE + 0x10000 - 4, LIMIT = BASE + 0x10000 };
int main(void) {
  memory_start();
  CHECK_EQ(ring_limit(END), LIMIT);
  CHECK_EQ(ring_limit(LIMIT), LIMIT);
  CHECK_EQ(ring_limit(LIMIT + 1), LIMIT + 32);

  hw_poke(PI_FIFO_BASE, BASE, 4);
  hw_poke(PI_FIFO_END, END, 4);
  hw_poke(PI_FIFO_WRITE, BASE, 4);
  // Bytes wait until a whole burst is there, and only then land in memory.
  for (int i = 0; i < 31; i++) graphics_write(PIPE_FIRST, 0, 1);
  CHECK_EQ(hw_peek(PI_FIFO_WRITE, 4), BASE);
  graphics_write(PIPE_FIRST, 0x7f, 1);
  CHECK_EQ(hw_peek(PI_FIFO_WRITE, 4), BASE + 32);
  CHECK_EQ(rd8(0x80000000u | (BASE + 31)), 0x7f);
  // A word split across two bursts keeps its order.
  for (int i = 0; i < 30; i++) graphics_write(PIPE_FIRST, 0, 1);
  graphics_write(PIPE_FIRST, 0x11223344, 4);
  CHECK_EQ(rd8(0x80000000u | (BASE + 63)), 0x22);
  CHECK_EQ(burst[0], 0x33);
  CHECK_EQ(burst_used, 2);
  burst_used = 0;
  // A display list filling its buffer keeps its end position, as the game reads its
  // size from there, and only turns over on the next write.
  hw_poke(PI_FIFO_WRITE, LIMIT - 32, 4);
  for (int i = 0; i < 8; i++) graphics_write(PIPE_FIRST, 0, 4);
  CHECK_EQ(hw_peek(PI_FIFO_WRITE, 4), LIMIT);
  for (int i = 0; i < 8; i++) graphics_write(PIPE_FIRST, 0, 4);
  CHECK_EQ(hw_peek(PI_FIFO_WRITE, 4), BASE + 32);

  // The live FIFO turns over at once, at the 32-byte boundary, not at END.
  write_pair(CP_FIFO_BASE, BASE);
  hw_poke(CP_CONTROL, CONTROL_LINK, 2);
  hw_poke(PI_FIFO_WRITE, LIMIT - 32, 4);
  for (int i = 0; i < 8; i++) graphics_write(PIPE_FIRST, 0, 4);
  CHECK_EQ(hw_peek(PI_FIFO_WRITE, 4), BASE);

  // The chip's reader wraps at the same place, past END's last four bytes.
  write_pair(CP_FIFO_BASE, BASE);
  write_pair(CP_FIFO_END, END);
  write_pair(CP_FIFO_READ, LIMIT - 32);
  write_pair(CP_FIFO_WRITE, BASE + 32);
  stream_used = 0;
  graphics_write(CP_CONTROL, CONTROL_READ_ENABLE, 2);
  CHECK_EQ(read_pair(CP_FIFO_READ), BASE + 32);
  return finish();
}
