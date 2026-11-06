// Attachment encryption and the Classic Controller's bytes, with made-up tables.
#include "../../recomp/runtime/wii_attachment.c"
#include "../../recomp/runtime/wii_attachment_key.c"
#include "../harness.h"

static uint8_t decrypt(uint8_t value, uint32_t address, const uint8_t *add, const uint8_t *exclusive) {
  return (uint8_t)((value ^ exclusive[address % 8]) + add[address % 8]);
}

int main(void) {
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
  attachment_classic_state(state, BUTTON_CLASSIC_L | BUTTON_CLASSIC_ZR, centred);  // L reads fully down as well.
  CHECK_EQ(state[4], 0xdf);
  CHECK_EQ(state[5], 0xfb);
  CHECK_EQ((state[2] >> 5 & 3) << 3 | state[3] >> 5, 31);
  CHECK_EQ(state[3] & 31, 0);
  attachment_classic_state(state, 0, left_full);
  CHECK_EQ(state[0] & 63, 63);
  CHECK_EQ(state[1] & 63, 63);
  uint8_t id[6], written = 0x55;
  attachment_read(0, 0xfa, id, 6);
  CHECK_EQ(id[2], 0xa4);
  CHECK_EQ(id[4], 1);
  attachment_write(0, 0xf0, &written, 1);
  attachment_read(0, 0xfa, id, 6);
  CHECK_EQ(id[3], 0x20);

  // A key The Last Story wrote, and the tables its own library made from it.
  static const uint8_t key[16] = {0x54, 0x3a, 0x9a, 0x45, 0x7d, 0xff, 0xc4, 0x64, 0x60, 0x6c, 0x29, 0x5e, 0x5b, 0x8c, 0x82, 0x67};
  static const uint8_t want_add[8] = {0x4f, 0xa0, 0x55, 0x65, 0x3c, 0xc9, 0x40, 0x91};
  static const uint8_t want_exclusive[8] = {0x77, 0x0a, 0x73, 0x7b, 0x69, 0xef, 0x24, 0xd0};
  uint8_t made_add[8], made_exclusive[8];
  CHECK_EQ(attachment_key_tables(key, made_add, made_exclusive), 1);
  for (int i = 0; i < 8; i++) {
    CHECK_EQ(made_add[i], want_add[i]);
    CHECK_EQ(made_exclusive[i], want_exclusive[i]);
  }
  // Writing the key turns encryption on with those tables.
  static const uint8_t encrypted = 0xaa;
  attachment_write(0, 0xf0, &encrypted, 1);
  attachment_write(0, 0x40, key, 16);
  uint8_t coded[6];
  attachment_read(0, 0xfa, coded, 6);
  for (int i = 0; i < 6; i++) CHECK_EQ(decrypt(coded[i], 0xfa + (uint32_t)i, want_add, want_exclusive), id[i]);

  // Each remote's attachment is its own: the second player's still reads plainly, and
  // its state comes from that player's controls.
  uint8_t plain[6];
  attachment_read(1, 0xfa, plain, 6);
  for (int i = 0; i < 6; i++) CHECK_EQ(plain[i], id[i]);
  PlayerState second = {.buttons = BUTTON_A, .sticks = {1, 1, 0, 0}};
  attachment_fill(1, &second, plain, 6);
  CHECK_EQ(plain[0] & 63, 63);
  CHECK_EQ(plain[5], 0xef);
  attachment_fill(0, &second, coded, 6);
  CHECK_EQ(decrypt(coded[5], 5, want_add, want_exclusive), 0xef);

  // A Nunchuk: stick in the middle, held still, nothing pressed. Its motion reads zero at
  // 0x200 and one g at 0x2cc, ten bits an axis.
  const float middle[2] = {0, 0}, still[3] = {0, 0, 1};
  uint8_t chuk[6];
  attachment_nunchuk_state(chuk, 0, middle, still);
  CHECK_EQ(chuk[0], 0x80);
  CHECK_EQ(chuk[1], 0x80);
  CHECK_EQ(chuk[2], 0x80);
  CHECK_EQ(chuk[3], 0x80);
  CHECK_EQ(chuk[4], 0xb3);
  CHECK_EQ(chuk[5], 0x03);  // C and Z up.
  const float pushed[2] = {1, -1}, odd[3] = {1.0f / 204, 2.0f / 204, -9};
  attachment_nunchuk_state(chuk, BUTTON_NUNCHUK_Z, pushed, odd);
  CHECK_EQ(chuk[0], 0xe0);
  CHECK_EQ(chuk[1], 0x20);
  CHECK_EQ(chuk[5], 0x02 | 1 << 2 | 2 << 4 | 0 << 6);  // Z held; the motion's lowest bits.
  CHECK_EQ(chuk[4], 0);  // Clamped.
  attachment_nunchuk_state(chuk, BUTTON_NUNCHUK_C, middle, still);
  CHECK_EQ(chuk[5], 0x01);

  // Plugged into the third remote, it says what it is and how it's calibrated, with the
  // check bytes the game's library verifies.
  attachment_plug(2, ATTACHMENT_NUNCHUK);
  uint8_t nunchuk_id[6], calibration[16];
  attachment_read(2, 0xfa, nunchuk_id, 6);
  const uint8_t want_nunchuk[6] = {0, 0, 0xa4, 0x20, 0, 0};
  for (int i = 0; i < 6; i++) CHECK_EQ(nunchuk_id[i], want_nunchuk[i]);
  attachment_read(2, 0x20, calibration, 16);
  uint8_t sum = 0;
  for (int i = 0; i < 14; i++) sum = (uint8_t)(sum + calibration[i]);
  CHECK_EQ(calibration[4], 0xb3);
  CHECK_EQ(calibration[8], 0xe0);
  CHECK_EQ(calibration[14], (uint8_t)(sum + 0x55));
  CHECK_EQ(calibration[15], (uint8_t)(sum + 0xaa));
  PlayerState holding = {.buttons = BUTTON_NUNCHUK_C, .nunchuk_accel = {0, 0, 1}};
  attachment_fill(2, &holding, chuk, 6);
  CHECK_EQ(chuk[5], 0x01);
  // Plugging in again starts afresh: the first remote's encryption is gone.
  attachment_plug(0, ATTACHMENT_CLASSIC);
  attachment_read(0, 0xfa, plain, 6);
  for (int i = 0; i < 6; i++) CHECK_EQ(plain[i], id[i]);
  return finish();
}
