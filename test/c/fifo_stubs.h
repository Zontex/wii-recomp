// What the FIFO tests (test_fifo.c, test_fifo_rare.c, test_fifo_reset.c) stand in for around
// the real gx_fifo.c, which each includes first: the rest of the graphics, the hardware's
// registers as plain memory, and the timing and recording hooks. A test's own stubs, for
// what it watches, stay in it.
#pragma once
int gx_efb_pokes_waiting;
void gx_efb_send_pokes(void) {}
void stage_add(int s, uint64_t t) {}
void stage_note(int kind) {}
const Title *title = &(Title){0};
void poll_interrupts(void) {}
void interrupts_turned_on(void) {}
void stage_frame(float g) {}
static uint8_t regs[0x10000];
uint32_t hw_peek(uint32_t a, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | regs[(a + i) & 0xffff]; return v; }
void hw_poke(uint32_t a, uint32_t v, int n) { for (int i = n - 1; i >= 0; i--, v >>= 8) regs[(a + i) & 0xffff] = (uint8_t)v; }
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
uint64_t time_base(void) { return 0; }
void gx_load_palette(void) {}
void debug_unsupported(const char *i, const char *w, uint32_t v) {}
void gx_present(void) {}
void gx_copy_to_texture(uint32_t c, int d) {}
void gx_bounding_box_write(int r, uint32_t d) {}
int gx_bounding_box_read(uint32_t a, uint32_t *v) { return 0; }
void gx_bounding_box_off(void) {}
const GxBackend *gx_backend;
int gx_shows_held;
void gx_clear(void) {}
int gx_draw_wanted(int p, int c) { return 0; }
Vertex *gx_draw_reserve(int c) { return 0; }
void gx_draw(int p, int c) {}
int gx_draw_expanded(int p, int f, const uint8_t *d, int c) { return 0; }
int gx_vertex_format;
GxCalledList gx_called_list;
int gx_read_list_vertices(const uint8_t *d, int f, int c, Vertex *o) { return c; }
uint64_t watch_range(uint32_t a, uint32_t b) { return 0; }
int gx_lists_kept(void) { return 0; }
int gx_recording;
void gx_record_command(const uint8_t *b, uint32_t c) {}
void stage_sample_thread(int graphics) {}
void gx_record_frame_shown(void) {}
void gx_record_list_enter(void) {}
void gx_record_list_leave(void) {}
