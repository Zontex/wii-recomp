// What the graphics files expect of the rest of the runtime. With no game running there
// are no hardware registers, interrupts or clocks to answer them.
#include "cpu.h"
#include "title.h"
#include "gx.h"
#include "gx_backend.h"
#include "hardware.h"

static const Title no_title;  // No game, so no FIFO breakpoints.
const Title *title = &no_title;

uint64_t time_base(void) { return 0; }
uint32_t hw_read(uint32_t address, int bytes) { (void)address; (void)bytes; return 0; }
void hw_write(uint32_t address, uint32_t value, int bytes) { (void)address; (void)value; (void)bytes; }
uint32_t hw_peek(uint32_t address, int bytes) { (void)address; (void)bytes; return 0; }
void hw_poke(uint32_t address, uint32_t value, int bytes) { (void)address; (void)value; (void)bytes; }
void raise_irq(int bit) { (void)bit; }
void clear_irq(int bit) { (void)bit; }
void poll_interrupts(void) {}
void window_refresh_size(void) {}
void window_size(int *width, int *height) { *width = *height = 0; }

// The player has no title, so it keeps no shader recipes.
void gx_recipe_note(GxShader shader, const GxRenderState *state) { (void)shader; (void)state; }
void gx_recipes_start(void) {}
