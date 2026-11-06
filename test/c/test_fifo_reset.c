// GXAbortFrame's FIFO reset, through the real gx_fifo.c.
#include "../../recomp/runtime/gx_fifo.c"
#include "../../recomp/runtime/timeline.c"
#include "../harness.h"
void stage_add(int s, uint64_t t) {}
const Title *title = &(Title){0};
void poll_interrupts(void) {}
void interrupts_turned_on(void) {}
void stage_frame(float g) {}
static uint8_t regs[0x10000];
uint32_t hw_peek(uint32_t a, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | regs[(a + i) & 0xffff]; return v; }
void hw_poke(uint32_t a, uint32_t v, int n) { for (int i = n - 1; i >= 0; i--, v >>= 8) regs[(a + i) & 0xffff] = (uint8_t)v; }
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
static int cp_irq;
void raise_irq(int b) { if (b == IRQ_CP) cp_irq = 1; }
void clear_irq(int b) { if (b == IRQ_CP) cp_irq = 0; }
uint64_t time_base(void) { return 0; }
void gx_load_palette(void) {}
void debug_unsupported(const char *i, const char *w, uint32_t v) {}
void gap_hit(const char *i, uint32_t d) {}
void gx_present(void) {}
void gx_copy_to_texture(uint32_t c, int d) {}
void gx_bounding_box_write(int r, uint32_t d) {}
int gx_bounding_box_read(uint32_t a, uint32_t *v) { return 0; }
void gx_bounding_box_off(void) {}
const GxBackend *gx_backend;
void gx_clear(void) {}
uint32_t gx_vertices_size(int f, int c) { return 0; }
int gx_read_vertices(const uint8_t *d, int f, int c, Vertex *o) { return c; }
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

int main(void) {
  memory_start();
  hw_poke(CP_CONTROL, CONTROL_READ_ENABLE | CONTROL_INTERRUPT_ENABLE | CONTROL_LINK, 2);
  graphics_write(PIPE_FIRST, 0x61000000u, 4);  // Half a burst, not yet sent.
  CHECK_EQ(burst_used, 4);
  fifo_flags = CP_STATUS_OVERFLOW;
  cp_irq = 1;
  CHECK(graphics_write(PI_FIFO_RESET, 0, 4));  // Only the reset bit acts.
  CHECK_EQ(burst_used, 4);
  CHECK(graphics_write(PI_FIFO_RESET, 1, 4));
  CHECK_EQ(burst_used, 0);
  CHECK_EQ(stream_used, 0);
  CHECK_EQ(fifo_flags, 0);
  CHECK(!cp_irq);
  CHECK_EQ(hw_peek(CP_CONTROL, 2), CONTROL_LINK);  // Linked, with reads and interrupts off.
  CHECK_EQ(read_pair(CP_HIGH_MARK), 0x1fffffe0u);
  CHECK_EQ(read_pair(CP_LOW_MARK), 0);
  return finish();
}
