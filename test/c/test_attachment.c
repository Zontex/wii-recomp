// Attachment encryption and the Classic Controller's bytes, with made-up tables.
#include "../../recomp/runtime/wii_attachment.c"
#include "../harness.h"
const uint32_t game_remote_blocks = 0, game_attachment_add = 0, game_attachment_xor = 0;
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
void input_sticks(float out[4]) { for (int i = 0; i < 4; i++) out[i] = 0; }
uint32_t input_buttons(void) { return 0; }

static uint8_t decrypt(uint8_t value, uint32_t address, const uint8_t *add, const uint8_t *exclusive) {
  return (uint8_t)((value ^ exclusive[address % 8]) + add[address % 8]);
}

int main(void) {
  memory_start();
  static const uint8_t add[8] = {1, 22, 3, 44, 5, 66, 7, 88}, exclusive[8] = {9, 8, 170, 6, 85, 4, 3, 2};
  for (uint32_t start = 0; start < 20; start++) {
    uint8_t plain[32], coded[32];
    for (uint32_t i = 0; i < 32; i++) plain[i] = coded[i] = (uint8_t)(i * 37 + start);
    attachment_encrypt(coded, 32, start, add, exclusive);
    for (uint32_t i = 0; i < 32; i++) CHECK_EQ(decrypt(coded[i], start + i, add, exclusive), plain[i]);
  }
  uint8_t state[6];
  const float centred[4] = {0, 0, 0, 0}, left_full[4] = {1, 1, 0, 0};
  attachment_classic_state(state, 0, centred);
  CHECK_EQ(state[4], 0xff);
  CHECK_EQ(state[5], 0xff);
  CHECK_EQ(state[0] & 63, 32);
  CHECK_EQ(state[1] & 63, 32);
  attachment_classic_state(state, BUTTON_A, centred);
  CHECK_EQ(state[5], 0xef);
  attachment_classic_state(state, BUTTON_DOWN | BUTTON_PLUS, centred);
  CHECK_EQ(state[4], 0xbb);
  attachment_classic_state(state, 0, left_full);
  CHECK_EQ(state[0] & 63, 63);
  CHECK_EQ(state[1] & 63, 63);
  uint8_t id[6], written = 0x55;
  attachment_read(0xfa, id, 6);
  CHECK_EQ(id[2], 0xa4);
  CHECK_EQ(id[4], 1);
  attachment_write(0xf0, &written, 1);
  attachment_read(0xfa, id, 6);
  CHECK_EQ(id[3], 0x20);
  return finish();
}
