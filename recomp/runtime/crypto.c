// /dev/aes and /dev/sha, IOS's front doors to the Wii's AES and SHA-1 engines. Programs
// hand them a key and data, so no console secret is involved. Both algorithms are
// written here from their standards (FIPS-197 and FIPS-180), small and plain rather
// than fast: programs send these a few kilobytes at a time.
//
// The requests are as Dolphin's AesDevice.cpp and Sha.cpp take them, and WiiBrew's
// /dev/sha page: each is an ioctlv, and anything else answers an error.
#include <string.h>

#include "cpu.h"
#include "ios.h"
#include "ios_vectors.h"

enum {
  AES_COPY = 0, AES_ENCRYPT = 2, AES_DECRYPT = 3, BLOCK = 16, ROUNDS = 10,
  SHA_INIT = 0, SHA_CONTRIBUTE = 1, SHA_FINALIZE = 2, SHA_CHUNK = 64, SHA_CONTEXT_BYTES = 0x1c, SHA_HASH_BYTES = 20,
};

// A pointer to `bytes` of guest memory from `address`, or 0 if any of it is outside memory.
static uint8_t *guest_range(uint32_t address, uint32_t bytes) {
  if (!address) return 0;
  if (!bytes) return host(address);
  uint8_t *start = host(address), *last = host(address + bytes - 1);
  return start && last == start + bytes - 1 ? start : 0;
}

// --- AES-128 ---

static uint8_t sbox[256], inverse_sbox[256];

static uint8_t times2(uint8_t x) { return (uint8_t)(x << 1 ^ (x & 0x80 ? 0x1b : 0)); }

static uint8_t multiply(uint8_t a, uint8_t b) {
  uint8_t product = 0;
  for (; b; b >>= 1, a = times2(a)) if (b & 1) product ^= a;
  return product;
}

// The substitution box: each byte's inverse in GF(2^8), then the standard's affine step.
// Worked out once rather than typed in, so it can't hold a typo.
static void make_sbox(void) {
  if (sbox[0]) return;
  for (int x = 0; x < 256; x++) {
    uint8_t inverse = 0;
    for (int y = 1; y < 256 && x; y++) if (multiply((uint8_t)x, (uint8_t)y) == 1) { inverse = (uint8_t)y; break; }
    uint8_t s = inverse;
    for (int shift = 1; shift < 5; shift++) s ^= (uint8_t)(inverse << shift | inverse >> (8 - shift));
    sbox[x] = s ^ 0x63;
    inverse_sbox[sbox[x]] = (uint8_t)x;
  }
}

static void expand_key(const uint8_t key[BLOCK], uint8_t round_keys[(ROUNDS + 1) * BLOCK]) {
  make_sbox();
  memcpy(round_keys, key, BLOCK);
  uint8_t round_constant = 1;
  for (int i = BLOCK; i < (ROUNDS + 1) * BLOCK; i += 4) {
    uint8_t word[4];
    memcpy(word, round_keys + i - 4, 4);
    if (i % BLOCK == 0) {
      uint8_t first = word[0];
      word[0] = sbox[word[1]] ^ round_constant;
      word[1] = sbox[word[2]];
      word[2] = sbox[word[3]];
      word[3] = sbox[first];
      round_constant = times2(round_constant);
    }
    for (int j = 0; j < 4; j++) round_keys[i + j] = round_keys[i - BLOCK + j] ^ word[j];
  }
}

// The state is in column order, as the bytes arrive: byte r + 4c is row r, column c.
static void shift_rows(uint8_t *state, int inverse) {
  uint8_t copy[BLOCK];
  memcpy(copy, state, BLOCK);
  for (int row = 1; row < 4; row++) {
    for (int column = 0; column < 4; column++) {
      int from = inverse ? (column + 4 - row) % 4 : (column + row) % 4;
      state[row + 4 * column] = copy[row + 4 * from];
    }
  }
}

static void mix_columns(uint8_t *state, int inverse) {
  static const uint8_t forward[4] = {2, 3, 1, 1}, backward[4] = {14, 11, 13, 9};
  const uint8_t *weights = inverse ? backward : forward;
  for (int column = 0; column < 4; column++) {
    uint8_t *c = state + 4 * column, copy[4];
    memcpy(copy, c, 4);
    for (int row = 0; row < 4; row++) {
      c[row] = 0;
      for (int k = 0; k < 4; k++) c[row] ^= multiply(copy[(row + k) % 4], weights[k]);
    }
  }
}

static void add_round_key(uint8_t *state, const uint8_t *key) {
  for (int i = 0; i < BLOCK; i++) state[i] ^= key[i];
}

static void encrypt_block(const uint8_t *round_keys, uint8_t *state) {
  add_round_key(state, round_keys);
  for (int round = 1; round <= ROUNDS; round++) {
    for (int i = 0; i < BLOCK; i++) state[i] = sbox[state[i]];
    shift_rows(state, 0);
    if (round < ROUNDS) mix_columns(state, 0);
    add_round_key(state, round_keys + round * BLOCK);
  }
}

static void decrypt_block(const uint8_t *round_keys, uint8_t *state) {
  add_round_key(state, round_keys + ROUNDS * BLOCK);
  for (int round = ROUNDS - 1; round >= 0; round--) {
    shift_rows(state, 1);
    for (int i = 0; i < BLOCK; i++) state[i] = inverse_sbox[state[i]];
    add_round_key(state, round_keys + round * BLOCK);
    if (round) mix_columns(state, 1);
  }
}

// AES-128 in CBC mode. `chain` starts as the IV and ends as the last ciphertext block, so
// a long message can be sent in pieces. `in` and `out` may be the same.
static void aes_cbc(const uint8_t key[BLOCK], uint8_t chain[BLOCK], const uint8_t *in, uint8_t *out, uint32_t bytes, int decrypt) {
  uint8_t round_keys[(ROUNDS + 1) * BLOCK], block[BLOCK], next[BLOCK];
  expand_key(key, round_keys);
  for (uint32_t at = 0; at + BLOCK <= bytes; at += BLOCK) {
    memcpy(block, in + at, BLOCK);
    if (decrypt) {
      memcpy(next, block, BLOCK);
      decrypt_block(round_keys, block);
      add_round_key(block, chain);
      memcpy(chain, next, BLOCK);
    } else {
      add_round_key(block, chain);
      encrypt_block(round_keys, block);
      memcpy(chain, block, BLOCK);
    }
    memcpy(out + at, block, BLOCK);
  }
}

// Copy: one input, one output. Encrypt and decrypt: the data and a 16-byte key in, the
// result and the 16-byte IV out, where the IV is updated to carry on from. Only whole
// blocks can be done, as the engine works in blocks.
int aes_request(uint32_t command, uint32_t request) {
  if (command != IOS_IOCTLV) return IOS_UNHANDLED;
  uint32_t code = rd32(request + 12);
  IosVectors vectors = ios_vectors(request);
  uint32_t in_bytes = ios_in_bytes(&vectors, 0), out_bytes = ios_out_bytes(&vectors, 0);
  uint32_t bytes = in_bytes < out_bytes ? in_bytes : out_bytes;
  uint8_t *in = guest_range(ios_in(&vectors, 0), in_bytes), *out = guest_range(ios_out(&vectors, 0), out_bytes);
  if (code == AES_COPY) {
    if (vectors.inputs != 1 || vectors.outputs != 1 || !in || !out) return IOS_EINVAL;
    memmove(out, in, bytes);
    return 0;
  }
  if (code != AES_ENCRYPT && code != AES_DECRYPT) return IOS_UNHANDLED;
  uint32_t key_at = ios_in(&vectors, 1), chain_at = ios_out(&vectors, 1);
  if (vectors.inputs != 2 || vectors.outputs != 2 || !in || !out || bytes % BLOCK) return IOS_EINVAL;
  if (ios_in_bytes(&vectors, 1) != BLOCK || ios_out_bytes(&vectors, 1) != BLOCK || (key_at & 3) || (chain_at & 3)) return IOS_EINVAL;
  uint8_t *key = guest_range(key_at, BLOCK), *chain = guest_range(chain_at, BLOCK);
  if (!key || !chain) return IOS_EINVAL;
  aes_cbc(key, chain, in, out, bytes, code == AES_DECRYPT);
  return 0;
}

// --- SHA-1 ---

static uint32_t rotate(uint32_t x, int n) { return x << n | x >> (32 - n); }

static uint32_t big_endian(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void put_big_endian(uint8_t *p, uint32_t value) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (24 - 8 * i));
}

static void sha1_chunk(uint32_t state[5], const uint8_t *chunk) {
  uint32_t w[80], a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
  for (int i = 0; i < 16; i++) w[i] = big_endian(chunk + 4 * i);
  for (int i = 16; i < 80; i++) w[i] = rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) f = (b & c) | (~b & d), k = 0x5a827999;
    else if (i < 40) f = b ^ c ^ d, k = 0x6ed9eba1;
    else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
    else f = b ^ c ^ d, k = 0xca62c1d6;
    uint32_t next = rotate(a, 5) + f + e + k + w[i];
    e = d, d = c, c = rotate(b, 30), b = a, a = next;
  }
  state[0] += a, state[1] += b, state[2] += c, state[3] += d, state[4] += e;
}

// The context is the game's: five state words, then the length so far in bits as two
// words, high first, all big-endian, as the PowerPC stores them. It keeps no partial
// chunk, so data before the last piece must come in whole 64-byte chunks.
static int sha_step(uint32_t code, uint8_t *context, const uint8_t *data, uint32_t bytes, uint8_t *hash) {
  static const uint32_t start[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
  uint32_t state[5];
  if (code == SHA_INIT) {
    for (int i = 0; i < 5; i++) put_big_endian(context + 4 * i, start[i]);
    memset(context + 20, 0, 8);
    return 0;
  }
  if (code == SHA_CONTRIBUTE && bytes % SHA_CHUNK) return IOS_EINVAL;
  for (int i = 0; i < 5; i++) state[i] = big_endian(context + 4 * i);
  uint64_t bits = (uint64_t)big_endian(context + 20) << 32 | big_endian(context + 24);
  uint32_t whole = bytes - bytes % SHA_CHUNK;
  for (uint32_t at = 0; at < whole; at += SHA_CHUNK) sha1_chunk(state, data + at);
  bits += (uint64_t)bytes * 8;
  if (code == SHA_FINALIZE) {
    // The rest, a one bit, zeros, and the length: one chunk, or two if the length won't fit.
    uint8_t tail[2 * SHA_CHUNK] = {0};
    uint32_t left = bytes - whole, size = left + 9 <= SHA_CHUNK ? SHA_CHUNK : 2 * SHA_CHUNK;
    memcpy(tail, data + whole, left);
    tail[left] = 0x80;
    for (int i = 0; i < 8; i++) tail[size - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (uint32_t at = 0; at < size; at += SHA_CHUNK) sha1_chunk(state, tail + at);
    for (int i = 0; i < 5; i++) put_big_endian(hash + 4 * i, state[i]);
  }
  for (int i = 0; i < 5; i++) put_big_endian(context + 4 * i, state[i]);
  put_big_endian(context + 20, (uint32_t)(bits >> 32));
  put_big_endian(context + 24, (uint32_t)bits);
  return 0;
}

// Each request: the data in, then the context and the hash out.
int sha_request(uint32_t command, uint32_t request) {
  if (command != IOS_IOCTLV) return IOS_UNHANDLED;
  uint32_t code = rd32(request + 12);
  if (code != SHA_INIT && code != SHA_CONTRIBUTE && code != SHA_FINALIZE) return IOS_UNHANDLED;  // HMAC isn't done.
  IosVectors vectors = ios_vectors(request);
  if (vectors.inputs != 1 || vectors.outputs != 2) return IOS_EINVAL;
  uint32_t bytes = ios_in_bytes(&vectors, 0);
  uint8_t *data = guest_range(ios_in(&vectors, 0), bytes);
  uint8_t *context = guest_range(ios_out_sized(&vectors, 0, SHA_CONTEXT_BYTES), SHA_CONTEXT_BYTES);
  uint8_t *hash = guest_range(ios_out_sized(&vectors, 1, SHA_HASH_BYTES), SHA_HASH_BYTES);
  if (!context || (code != SHA_INIT && bytes && !data) || (code == SHA_FINALIZE && !hash)) return IOS_EINVAL;
  return sha_step(code, context, data, bytes, hash);
}
