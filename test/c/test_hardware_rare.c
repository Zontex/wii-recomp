// Rarely used registers in hardware.c: the reset button, the audio interface's interrupt,
// the accelerator DMA, the DSP control register, the serial I/O buffer and the DTV status.
#include "../../recomp/runtime/hardware.c"
#include "../harness.h"
#include <string.h>

static uint64_t now_ticks = 60750000ull * 1000;
uint64_t time_base(void) { return now_ticks; }
uint64_t audio_next_due(void) { return 0; }
void dsp_update(uint64_t now) {}
int dsp_read(uint32_t a, int n, uint32_t *v) { return 0; }
int dsp_write(uint32_t a, uint32_t v, int n) { return 0; }
uint32_t dsp_control_read(void) { return 0; }
static uint32_t dsp_control_written;
uint32_t dsp_control_write(uint32_t v) { dsp_control_written = v; return 0; }
static int dma_resets;
int audio_dma_read(uint32_t a, int n, uint32_t *v) { return 0; }
int audio_dma_write(uint32_t a, uint32_t v, int n) { return 0; }
void audio_dma_reset(void) { dma_resets++; }
int si_read(uint32_t a, uint32_t *v) { return 0; }
int si_write(uint32_t a, uint32_t v) { return 0; }
int exi_read(uint32_t a, uint32_t *v) { return 0; }
int exi_write(uint32_t a, uint32_t v) { return 0; }
int ipc_read(uint32_t a, uint32_t *v) { return 0; }
int ipc_write(uint32_t a, uint32_t v) { return 0; }
int graphics_read(uint32_t a, uint32_t *v) { return 0; }
int graphics_write(uint32_t a, uint32_t v, int n) { return 0; }
void graphics_retrace(void) {}
static int resets_told, gaps;
void stm_reset_button(void) { resets_told++; }
void gap_hit(const char *id, uint32_t detail) { gaps++; }
static const char *game_setting = "JP";
int console_setting(const char *name, char *out, size_t limit) {
  if (strcmp(name, "GAME")) return 0;
  snprintf(out, limit, "%s", game_setting);
  return 1;
}

enum {
  CAUSE = 0xcc003000u, MASK = 0xcc003004u, DSP = 0xcc00500au,
  AI_CONTROL_AT = 0xcd006c00u, AI_COUNT_AT = 0xcd006c08u, AI_TIMING_AT = 0xcd006c0cu,
};

static void test_reset_button(void) {
  CHECK_EQ(hw_read(CAUSE, 4), 0x10000);  // Up, as at power-on.
  hw_write(MASK, 1u << IRQ_RESET_SWITCH, 4);
  reset_button_press();
  CHECK(!irq_pending());  // Only once the game's thread checks in.
  hardware_update(now_ticks);
  CHECK_EQ(resets_told, 1);
  CHECK(irq_pending());
  CHECK_EQ(hw_read(CAUSE, 4), 1u << IRQ_RESET_SWITCH);  // Held down.
  hw_write(CAUSE, 0xffffffffu, 4);  // Acknowledged, and the button state isn't a flag.
  CHECK(!irq_pending());
  now_ticks += 60750000ull;
  CHECK_EQ(hw_read(CAUSE, 4), 0x10000);  // Up again after half a second.
  hardware_update(now_ticks);
  CHECK_EQ(resets_told, 1);
}

static void test_audio_interrupt(void) {
  hw_write(MASK, 1u << IRQ_AI, 4);
  hw_write(AI_CONTROL_AT, 0x20, 4);  // Counter reset.
  CHECK_EQ(hw_read(AI_COUNT_AT, 4), 0);
  hw_write(AI_TIMING_AT, 480, 4);  // 10 ms at 48 kHz.
  hw_write(AI_CONTROL_AT, 0x01 | 0x04, 4);  // Playing, interrupt enabled.
  hardware_update(now_ticks);
  now_ticks += 1266ull * 479;
  hardware_update(now_ticks);
  CHECK(!irq_pending());
  now_ticks += 1266ull * 2;
  hardware_update(now_ticks);
  CHECK(irq_pending());
  CHECK_EQ(hw_read(AI_CONTROL_AT, 4) & 0x08, 0x08);
  hw_write(AI_CONTROL_AT, 0x01 | 0x04 | 0x08, 4);  // Writing the flag clears it.
  CHECK(!irq_pending());
  CHECK_EQ(hw_read(AI_CONTROL_AT, 4), 0x05);
  now_ticks += 1266ull * 1000;
  hardware_update(now_ticks);
  CHECK(!irq_pending());  // Only once per pass.
  // Stopped: the timing passing by raises nothing.
  hw_write(AI_COUNT_AT, 100, 4);
  CHECK_EQ(hw_read(AI_COUNT_AT, 4), 100);
  hw_write(AI_CONTROL_AT, 0x04, 4);
  now_ticks += 1266ull * 1000;
  hardware_update(now_ticks);
  CHECK(!irq_pending());
}

static void test_accelerator_dma(void) {
  for (uint32_t i = 0; i < 64; i++) wr8(0x80001000u + i, (uint8_t)(i + 1));
  hw_write(MASK, 1u << IRQ_DSP, 4);
  hw_write(DSP, 0x0040, 2);  // Accelerator interrupt enabled.
  hw_write(0xcc005020u, 0x00001000u, 4);
  hw_write(0xcc005024u, 0x00200000u, 4);
  hw_write(0xcc005028u, 0, 2);
  hw_write(0xcc00502au, 64, 2);  // Main memory to MEM2.
  CHECK_EQ(rd32(0x90200000u), 0x01020304u);
  CHECK_EQ(rd8(0x9020003fu), 64);
  CHECK_EQ(hw_read(DSP, 2) & 0x20, 0x20);
  CHECK(irq_pending());
  CHECK_EQ(hw_read(0xcc005028u, 4), 0);  // Finished.
  hw_write(DSP, 0x0040 | 0x20, 2);  // Acknowledged.
  CHECK(!irq_pending());
  hw_write(0xcc005020u, 0x00002000u, 4);
  hw_write(0xcc005024u, 0x00200000u, 4);
  hw_write(0xcc005028u, 0x80000020u, 4);  // MEM2 to main memory, in one write.
  CHECK_EQ(rd32(0x80002000u), 0x01020304u);
  CHECK_EQ(rd8(0x80002020u), 0);
  CHECK(irq_pending());
  hw_write(DSP, 0x20, 2);
  CHECK_EQ(gaps, 0);
}

// The processor's own bits go to dsp.c. Its mail interrupt shares the line, and a reset
// stops audio DMA, as Dolphin's DSP.cpp does.
static void test_dsp_control(void) {
  hw_write(MASK, 1u << IRQ_DSP, 4);
  hw_write(DSP, 0x0100 | 0x0004, 2);  // Mail interrupt enabled, processor halted.
  CHECK_EQ(dsp_control_written, 0x0104);
  CHECK_EQ(dma_resets, 0);
  dsp_interrupt();
  CHECK_EQ(hw_read(DSP, 2) & 0x80, 0x80);
  CHECK(irq_pending());
  hw_write(DSP, 0x0100 | 0x0080, 2);  // Acknowledged.
  CHECK(!irq_pending());
  CHECK_EQ(hw_read(DSP, 2) & 0x80, 0);
  hw_write(DSP, 0x0001, 2);  // Reset.
  CHECK_EQ(dma_resets, 1);
  dsp_interrupt();  // Disabled now.
  CHECK(!irq_pending());
  hw_write(DSP, 0x0080, 2);
  CHECK_EQ(gaps, 0);
}

static void test_serial_buffer(void) {
  hw_write(0xcd006480u, 0x00400300u, 4);
  hw_write(0xcd0064fcu, 0x12345678u, 4);
  CHECK_EQ(hw_read(0xcd006480u, 4), 0x00400300u);
  CHECK_EQ(hw_read(0xcd0064feu, 2), 0x5678);
}

static void test_dtv_status(void) {
  CHECK_EQ(hw_read(0xcc00206eu, 2), 2);  // A Japanese console, and no component cable.
}

int main(void) {
  memory_start();
  hardware_start();
  test_reset_button();
  test_audio_interrupt();
  test_accelerator_dma();
  test_dsp_control();
  test_serial_buffer();
  test_dtv_status();
  return finish();
}
