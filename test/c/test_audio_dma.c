// The audio DMA (audio.c) with made-up samples: it reads 32 bytes every 8 samples' time
// from the start address, turns right-first big-endian samples into left and right,
// counts its blocks down, starts over and interrupts, and plays silence while off.
#include "../../recomp/runtime/audio.c"
#include "../harness.h"

static uint64_t now_ticks = 1000000;
uint64_t time_base(void) { return now_ticks; }
static uint32_t ai_control = 0x40;  // 32 kHz.
uint32_t hw_peek(uint32_t address, int bytes) { return address == 0xcc006c00u ? ai_control : 0; }
static int interrupts, gaps;
void audio_dma_interrupt(void) { interrupts++; }
static int unanswered;  // The game has yet to answer the last interrupt.
int audio_dma_interrupt_unanswered(void) { return unanswered; }
void gap_hit(const char *id, uint32_t detail) { gaps++; }
int audio_output_open(void) { return 1; }
uint32_t hw_read(uint32_t address, int bytes) { return 0; }
void hw_write(uint32_t address, uint32_t value, int bytes) {}

#define BLOCK_TICKS (60750000.0 / 4000)
enum { START_HIGH = 0xcc005030u, START_LOW = 0xcc005032u, CONTROL = 0xcc005036u, LEFT = 0xcc00503au };

static void wait_blocks(int blocks) {
  for (int i = 0; i < blocks; i++) {
    now_ticks += (uint64_t)BLOCK_TICKS + 1;
    audio_update(now_ticks);
  }
}

static uint32_t blocks_left(void) {
  uint32_t value = 0;
  audio_dma_read(LEFT, 2, &value);
  return value;
}

int main(void) {
  memory_start();
  // Four blocks of samples: right is the sample's number, left its negative.
  for (int i = 0; i < 32; i++) {
    wr16(0x80100000u + i * 4, (uint16_t)(i + 1));
    wr16(0x80100000u + i * 4 + 2, (uint16_t)-(i + 1));
  }
  audio_update(now_ticks);
  audio_dma_write(START_HIGH, 0x0010, 2);
  audio_dma_write(START_LOW, 0x0000 | 0x1f, 2);  // The low five bits don't stick.
  uint32_t start = 0;
  audio_dma_read(START_LOW, 2, &start);
  CHECK_EQ(start, 0);
  audio_dma_write(CONTROL, 0x8000 | 4, 2);
  CHECK_EQ(interrupts, 0);
  now_ticks += START_INTERRUPT_TICKS;
  audio_update(now_ticks);
  CHECK_EQ(interrupts, 1);  // The first comes just after the start.
  CHECK_EQ(blocks_left(), 3);  // Zero-based.
  wait_blocks(3);
  CHECK_EQ(interrupts, 1);
  CHECK_EQ(blocks_left(), 0);
  // In the ring, left then right: sample 1 is left -1, right 1, and so on.
  CHECK_EQ(written, 24);
  CHECK_EQ((uint16_t)ring[0], (uint16_t)-1);
  CHECK_EQ(ring[1], 1);
  CHECK_EQ((uint16_t)ring[23 * 2], (uint16_t)-24);
  CHECK_EQ(ring[23 * 2 + 1], 24);
  wait_blocks(1);
  CHECK_EQ(interrupts, 2);  // Ran out: back to the start, and an interrupt.
  CHECK_EQ(blocks_left(), 3);
  CHECK_EQ(dma_address, 0x00100000u);
  // A new count waits for the running one to end.
  audio_dma_write(CONTROL, 0x8000 | 2, 2);
  CHECK_EQ(blocks_left(), 3);
  wait_blocks(4);
  CHECK_EQ(interrupts, 3);
  CHECK_EQ(blocks_left(), 1);  // Two blocks, the second now playing.
  // A game that falls behind: it hasn't answered the interrupt that just came, so the
  // new pass waits rather than playing the old buffer again, and the time is let go.
  unanswered = 1;
  uint32_t played_before = written;
  wait_blocks(200);
  CHECK_EQ(written, played_before);  // Nothing more played, nothing repeated.
  CHECK_EQ(interrupts, 3);
  unanswered = 0;  // Answered: on it goes, from now.
  wait_blocks(2);
  CHECK_EQ(written, played_before + 16);
  CHECK_EQ(interrupts, 4);
  // Off: silence, and no more interrupts. A long wait is let go rather than made up.
  audio_dma_write(CONTROL, 0, 2);
  wait_blocks(3000);
  CHECK_EQ(interrupts, 4);
  now_ticks += 60750000ull;  // A second with no checks, as a slow step would leave.
  audio_update(now_ticks);
  CHECK_EQ(gaps, 0);
  // 48 kHz isn't played at its own rate yet, and says so.
  ai_control = 0;
  audio_dma_write(CONTROL, 0x8000 | 4, 2);
  wait_blocks(1);
  CHECK_EQ(gaps, 1);
  return finish();
}
