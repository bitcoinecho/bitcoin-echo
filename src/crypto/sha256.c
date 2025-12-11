/*
 * Bitcoin Echo — SHA-256 Implementation
 *
 * SHA-256 as specified in FIPS 180-4 (Secure Hash Standard).
 *
 * Reference: https://csrc.nist.gov/publications/detail/fips/180/4/final
 *
 * This implementation prioritizes correctness and clarity.
 * Every operation maps directly to the specification.
 *
 * Build once. Build right. Stop.
 */

#include "sha256.h"
#include <stdint.h>
#include <string.h>

/*
 * Initial hash values.
 * First 32 bits of the fractional parts of the square roots
 * of the first 8 prime numbers (2, 3, 5, 7, 11, 13, 17, 19).
 */
static const uint32_t H_INIT[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                   0xa54ff53a, 0x510e527f, 0x9b05688c,
                                   0x1f83d9ab, 0x5be0cd19};

/*
 * Round constants.
 * First 32 bits of the fractional parts of the cube roots
 * of the first 64 prime numbers.
 */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

/*
 * Right rotate a 32-bit value by n bits.
 */
static inline uint32_t rotr32(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

/*
 * SHA-256 logical functions (FIPS 180-4, Section 4.1.2).
 */
#define Ch(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define Maj(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define Sigma0(x) (rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22))
#define Sigma1(x) (rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25))
#define sigma0(x) (rotr32(x, 7) ^ rotr32(x, 18) ^ ((x) >> 3))
#define sigma1(x) (rotr32(x, 17) ^ rotr32(x, 19) ^ ((x) >> 10))

/*
 * Load 32-bit big-endian value from byte array.
 */
static inline uint32_t load_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | ((uint32_t)p[3]);
}

/*
 * Store 32-bit value as big-endian bytes.
 */
static inline void store_be32(uint8_t *p, uint32_t x) {
  p[0] = (uint8_t)(x >> 24);
  p[1] = (uint8_t)(x >> 16);
  p[2] = (uint8_t)(x >> 8);
  p[3] = (uint8_t)(x);
}

/*
 * Store 64-bit value as big-endian bytes.
 */
static inline void store_be64(uint8_t *p, uint64_t x) {
  p[0] = (uint8_t)(x >> 56);
  p[1] = (uint8_t)(x >> 48);
  p[2] = (uint8_t)(x >> 40);
  p[3] = (uint8_t)(x >> 32);
  p[4] = (uint8_t)(x >> 24);
  p[5] = (uint8_t)(x >> 16);
  p[6] = (uint8_t)(x >> 8);
  p[7] = (uint8_t)(x);
}

/*
 * Process one 64-byte block.
 *
 * This is the SHA-256 compression function.
 * FIPS 180-4, Section 6.2.2.
 */
static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
  uint32_t W[64];
  uint32_t a, b, c, d, e, f, g, h;
  uint32_t T1, T2;
  int t;

  /*
   * Step 1: Prepare the message schedule W[0..63].
   * W[0..15] are loaded directly from the block.
   * W[16..63] are computed from earlier W values.
   */
  for (t = 0; t < 16; t++) {
    W[t] = load_be32(block + (size_t)(t * 4));
  }
  for (t = 16; t < 64; t++) {
    W[t] = sigma1(W[t - 2]) + W[t - 7] + sigma0(W[t - 15]) + W[t - 16];
  }

  /*
   * Step 2: Initialize working variables.
   */
  a = state[0];
  b = state[1];
  c = state[2];
  d = state[3];
  e = state[4];
  f = state[5];
  g = state[6];
  h = state[7];

  /*
   * Step 3: Perform 64 rounds of compression.
   */
  for (t = 0; t < 64; t++) {
    T1 = h + Sigma1(e) + Ch(e, f, g) + K[t] + W[t];
    T2 = Sigma0(a) + Maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + T1;
    d = c;
    c = b;
    b = a;
    a = T1 + T2;
  }

  /*
   * Step 4: Compute intermediate hash value.
   */
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx) {
  ctx->state[0] = H_INIT[0];
  ctx->state[1] = H_INIT[1];
  ctx->state[2] = H_INIT[2];
  ctx->state[3] = H_INIT[3];
  ctx->state[4] = H_INIT[4];
  ctx->state[5] = H_INIT[5];
  ctx->state[6] = H_INIT[6];
  ctx->state[7] = H_INIT[7];
  ctx->count = 0;
}

void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
  size_t buffered;
  size_t remaining;

  if (len == 0) {
    return;
  }

  /* How many bytes are already buffered? */
  buffered = (size_t)(ctx->count % SHA256_BLOCK_SIZE);

  /* Update total count */
  ctx->count += len;

  /* If we have buffered data, try to complete a block */
  if (buffered > 0) {
    remaining = SHA256_BLOCK_SIZE - buffered;
    if (len < remaining) {
      /* Not enough to complete a block, just buffer */
      memcpy(ctx->buffer + buffered, data, len);
      return;
    }
    /* Complete the buffered block */
    memcpy(ctx->buffer + buffered, data, remaining);
    sha256_transform(ctx->state, ctx->buffer);
    data += remaining;
    len -= remaining;
  }

  /* Process complete blocks directly from input */
  while (len >= SHA256_BLOCK_SIZE) {
    sha256_transform(ctx->state, data);
    data += SHA256_BLOCK_SIZE;
    len -= SHA256_BLOCK_SIZE;
  }

  /* Buffer any remaining data */
  if (len > 0) {
    memcpy(ctx->buffer, data, len);
  }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]) {
  size_t buffered;
  uint64_t bit_count;
  int i;

  /* How many bytes in the buffer? */
  buffered = (size_t)(ctx->count % SHA256_BLOCK_SIZE);

  /* Total message length in bits */
  bit_count = ctx->count * 8;

  /*
   * Padding: append 1 bit, then zeros, then 64-bit length.
   * The padding must result in a message length that is a multiple of 512 bits.
   */

  /* Append 0x80 (1 bit followed by 7 zero bits) */
  ctx->buffer[buffered++] = 0x80;

  /* If not enough room for the length (need 8 bytes), pad and process */
  if (buffered > 56) {
    /* Fill rest of block with zeros and process */
    memset(ctx->buffer + buffered, 0, SHA256_BLOCK_SIZE - buffered);
    sha256_transform(ctx->state, ctx->buffer);
    buffered = 0;
  }

  /* Pad with zeros up to the length field */
  memset(ctx->buffer + buffered, 0, 56 - buffered);

  /* Append the 64-bit message length in bits (big-endian) */
  store_be64(ctx->buffer + 56, bit_count);

  /* Process the final block */
  sha256_transform(ctx->state, ctx->buffer);

  /* Output the hash (big-endian) */
  for (i = 0; i < 8; i++) {
    store_be32(out + (size_t)(i * 4), ctx->state[i]);
  }
}

void sha256(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]) {
  sha256_ctx_t ctx;

  sha256_init(&ctx);
  sha256_update(&ctx, data, len);
  sha256_final(&ctx, out);
}

void sha256d(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]) {
  uint8_t inner[SHA256_DIGEST_SIZE];

  /* SHA256d(x) = SHA256(SHA256(x)) */
  sha256(data, len, inner);
  sha256(inner, SHA256_DIGEST_SIZE, out);
}

void sha256_midstate(const uint8_t data[SHA256_BLOCK_SIZE],
                     uint8_t out[SHA256_DIGEST_SIZE]) {
  sha256_ctx_t ctx;
  int i;

  sha256_init(&ctx);
  sha256_transform(ctx.state, data);

  /* Output the raw state (not finalized) */
  for (i = 0; i < 8; i++) {
    store_be32(out + (size_t)(i * 4), ctx.state[i]);
  }
}
