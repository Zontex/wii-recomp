// Turns the key a game writes into an attachment's registers 0x40 to 0x4f into the two
// tables the attachment encrypts with, as a Nintendo attachment does. The game makes
// the same tables from the same key to decrypt, so nothing about the game needs to be
// known. Ported from Dolphin's WiimoteEmu/Encryption.cpp (GPL-2.0-or-later, Dolphin
// Emulator Project, from work by Hector Martin); see CREDITS.md.
#include <stdint.h>

#include "wii_attachment_key.h"
#include "wii_attachment_tables.h"

enum { RANDOM_BYTES = 10, KEY_BYTES = 6, INDEXES = 7, FALLBACK_XOR = 0x17 };

static uint8_t rotate_right(uint8_t value, uint8_t by) {
  by &= 7;
  return (uint8_t)(value >> by | value << ((8 - by) & 7));
}

// The key the Wii would have made from these random bytes with table index idx.
static void key_for(const uint8_t random[RANDOM_BYTES], int idx, uint8_t key[KEY_BYTES]) {
  static const uint8_t answers[INDEXES][KEY_BYTES] = {
    {0xa8, 0x77, 0xa6, 0xe0, 0xf7, 0x43}, {0x5a, 0x35, 0x85, 0xe2, 0x72, 0x97}, {0x8f, 0xb7, 0x1a, 0x62, 0x87, 0x38},
    {0x0d, 0x67, 0xc7, 0xbe, 0x4f, 0x3e}, {0x20, 0x76, 0x37, 0x8f, 0x68, 0xb7}, {0xa9, 0x26, 0x3f, 0x2b, 0x10, 0xe3},
    {0x30, 0x7e, 0x90, 0x0e, 0x85, 0x0a},
  };
  const uint8_t *a = answers[idx];
  uint8_t t[RANDOM_BYTES];
  for (int i = 0; i < RANDOM_BYTES; i++) t[i] = keygen_sbox[random[i]];
  key[0] = (uint8_t)((uint8_t)(rotate_right(a[0] ^ t[5], t[2]) - t[9]) ^ t[4]);
  key[1] = (uint8_t)((uint8_t)(rotate_right(a[1] ^ t[1], t[0]) - t[5]) ^ t[7]);
  key[2] = (uint8_t)((uint8_t)(rotate_right(a[2] ^ t[6], t[8]) - t[2]) ^ t[0]);
  key[3] = (uint8_t)((uint8_t)(rotate_right(a[3] ^ t[4], t[7]) - t[3]) ^ t[2]);
  key[4] = (uint8_t)((uint8_t)(rotate_right(a[4] ^ t[1], t[6]) - t[3]) ^ t[4]);
  key[5] = (uint8_t)((uint8_t)(rotate_right(a[5] ^ t[7], t[8]) - t[5]) ^ t[9]);
}

static void make_tables(const uint8_t r[RANDOM_BYTES], const uint8_t k[KEY_BYTES], const uint8_t *a, const uint8_t *b,
                        uint8_t add[8], uint8_t exclusive[8]) {
  add[0] = a[k[4]] ^ b[r[3]];
  add[1] = a[k[2]] ^ b[r[5]];
  add[2] = a[k[5]] ^ b[r[7]];
  add[3] = a[k[0]] ^ b[r[2]];
  add[4] = a[k[1]] ^ b[r[4]];
  add[5] = a[k[3]] ^ b[r[9]];
  add[6] = a[r[0]] ^ b[r[6]];
  add[7] = a[r[1]] ^ b[r[8]];
  exclusive[0] = a[k[0]] ^ b[r[1]];
  exclusive[1] = a[k[5]] ^ b[r[4]];
  exclusive[2] = a[k[3]] ^ b[r[0]];
  exclusive[3] = a[k[2]] ^ b[r[9]];
  exclusive[4] = a[k[4]] ^ b[r[7]];
  exclusive[5] = a[k[1]] ^ b[r[8]];
  exclusive[6] = a[r[3]] ^ b[r[5]];
  exclusive[7] = a[r[2]] ^ b[r[6]];
}

// The 16 bytes hold ten random bytes then a six-byte key, each stored the other way
// round. The Wii picked one of seven tables to make the key, and doesn't say which, so
// each is tried until one makes the same key. Retail games always match one; for a key
// that matches none, such as all zeros from homebrew, real attachments use other tables.
int attachment_key_tables(const uint8_t written[16], uint8_t add[8], uint8_t exclusive[8]) {
  uint8_t random[RANDOM_BYTES], key[KEY_BYTES], made[KEY_BYTES];
  for (int i = 0; i < RANDOM_BYTES; i++) random[RANDOM_BYTES - 1 - i] = written[i];
  for (int i = 0; i < KEY_BYTES; i++) key[KEY_BYTES - 1 - i] = written[RANDOM_BYTES + i];
  for (int idx = 0; idx < INDEXES; idx++) {
    key_for(random, idx, made);
    int same = 1;
    for (int i = 0; i < KEY_BYTES; i++) same &= made[i] == key[i];
    if (!same) continue;
    make_tables(random, key, table_sboxes[idx], table_sboxes[idx + 1], add, exclusive);
    return 1;
  }
  make_tables(random, key, fallback_sbox_a, fallback_sbox_b, add, exclusive);
  for (int i = 0; i < 8; i++) {
    add[i] ^= FALLBACK_XOR;
    exclusive[i] ^= FALLBACK_XOR;
  }
  return 0;
}
