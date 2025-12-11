/*
 * Bitcoin Echo — SHA-1 Implementation
 *
 * SHA-1 as specified in FIPS 180-4.
 *
 * Note: SHA-1 is cryptographically broken. This implementation is
 * provided only for Bitcoin Script compatibility (OP_SHA1).
 *
 * Build once. Build right. Stop.
 */

#include "sha1.h"
#include <stdint.h>
#include <string.h>

/*
 * SHA-1 initial hash values (FIPS 180-4 section 5.3.1).
 */
static const uint32_t sha1_init_state[5] = {0x67452301, 0xefcdab89, 0x98badcfe,
                                            0x10325476, 0xc3d2e1f0};

/*
 * Rotate left (circular left shift).
 */
static inline uint32_t rotl32(uint32_t x, int n) {
  return (x << n) | (x >> (32 - n));
}

/*
 * Process a single 512-bit block.
 */
static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
  uint32_t w[80];
  uint32_t a, b, c, d, e;

  /* Prepare message schedule */
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4 + 0] << 24) |
           ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
  }
  for (int i = 16; i < 80; i++) {
    w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }

  /* Initialize working variables */
  a = state[0];
  b = state[1];
  c = state[2];
  d = state[3];
  e = state[4];

  /* 80 rounds */
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;

    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5a827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ed9eba1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8f1bbcdc;
    } else {
      f = b ^ c ^ d;
      k = 0xca62c1d6;
    }

    uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl32(b, 30);
    b = a;
    a = temp;
  }

  /* Add to hash state */
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

/*
 * Initialize SHA-1 context.
 */
void sha1_init(sha1_ctx_t *ctx) {
  if (ctx == NULL)
    return;
  memcpy(ctx->state, sha1_init_state, sizeof(sha1_init_state));
  ctx->count = 0;
  memset(ctx->buffer, 0, SHA1_BLOCK_SIZE);
}

/*
 * Feed data into SHA-1 context.
 */
void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len) {
  if (ctx == NULL || (data == NULL && len > 0))
    return;

  size_t buffer_used = (size_t)(ctx->count % SHA1_BLOCK_SIZE);
  ctx->count += len;

  /* Fill buffer if we have partial data */
  if (buffer_used > 0) {
    size_t buffer_free = SHA1_BLOCK_SIZE - buffer_used;
    if (len < buffer_free) {
      memcpy(ctx->buffer + buffer_used, data, len);
      return;
    }
    memcpy(ctx->buffer + buffer_used, data, buffer_free);
    sha1_transform(ctx->state, ctx->buffer);
    data += buffer_free;
    len -= buffer_free;
  }

  /* Process complete blocks */
  while (len >= SHA1_BLOCK_SIZE) {
    sha1_transform(ctx->state, data);
    data += SHA1_BLOCK_SIZE;
    len -= SHA1_BLOCK_SIZE;
  }

  /* Save remaining data */
  if (len > 0) {
    memcpy(ctx->buffer, data, len);
  }
}

/*
 * Finalize SHA-1 and retrieve digest.
 */
void sha1_final(sha1_ctx_t *ctx, uint8_t out[SHA1_DIGEST_SIZE]) {
  if (ctx == NULL || out == NULL)
    return;

  size_t buffer_used = (size_t)(ctx->count % SHA1_BLOCK_SIZE);
  uint64_t bit_count = ctx->count * 8;

  /* Padding: append 1 bit, then zeros, then 64-bit length */
  ctx->buffer[buffer_used++] = 0x80;

  if (buffer_used > 56) {
    /* Not enough room for length - need extra block */
    memset(ctx->buffer + buffer_used, 0, SHA1_BLOCK_SIZE - buffer_used);
    sha1_transform(ctx->state, ctx->buffer);
    buffer_used = 0;
  }

  memset(ctx->buffer + buffer_used, 0, 56 - buffer_used);

  /* Append length in big-endian */
  ctx->buffer[56] = (uint8_t)(bit_count >> 56);
  ctx->buffer[57] = (uint8_t)(bit_count >> 48);
  ctx->buffer[58] = (uint8_t)(bit_count >> 40);
  ctx->buffer[59] = (uint8_t)(bit_count >> 32);
  ctx->buffer[60] = (uint8_t)(bit_count >> 24);
  ctx->buffer[61] = (uint8_t)(bit_count >> 16);
  ctx->buffer[62] = (uint8_t)(bit_count >> 8);
  ctx->buffer[63] = (uint8_t)(bit_count);

  sha1_transform(ctx->state, ctx->buffer);

  /* Output in big-endian */
  for (int i = 0; i < 5; i++) {
    out[i * 4 + 0] = (uint8_t)(ctx->state[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
  }
}

/*
 * Compute SHA-1 of data in one call.
 */
void sha1(const uint8_t *data, size_t len, uint8_t out[SHA1_DIGEST_SIZE]) {
  sha1_ctx_t ctx;
  sha1_init(&ctx);
  sha1_update(&ctx, data, len);
  sha1_final(&ctx, out);
}
