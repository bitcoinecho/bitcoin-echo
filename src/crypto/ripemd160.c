/*
 * Bitcoin Echo — RIPEMD-160 Implementation
 *
 * RIPEMD-160 as specified by Dobbertin, Bosselaers, and Preneel (1996).
 *
 * Reference: "RIPEMD-160: A Strengthened Version of RIPEMD"
 *            https://homes.esat.kuleuven.be/~bosselae/ripemd160.html
 *
 * This implementation prioritizes correctness and clarity.
 * Every operation maps directly to the specification.
 *
 * Build once. Build right. Stop.
 */

#include "ripemd160.h"
#include "sha256.h" /* For hash160 */
#include <stdint.h>
#include <string.h>

/*
 * Initial hash values.
 */
static const uint32_t H_INIT[5] = {0x67452301, 0xefcdab89, 0x98badcfe,
                                   0x10325476, 0xc3d2e1f0};

/*
 * Left rotate a 32-bit value by n bits.
 */
static inline uint32_t rotl32(uint32_t x, int n) {
  return (x << n) | (x >> (32 - n));
}

/*
 * Boolean functions for each round.
 * These are applied differently in the left and right computation paths.
 */
static inline uint32_t f0(uint32_t x, uint32_t y, uint32_t z) {
  return x ^ y ^ z;
}

static inline uint32_t f1(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) | (~x & z);
}

static inline uint32_t f2(uint32_t x, uint32_t y, uint32_t z) {
  return (x | ~y) ^ z;
}

static inline uint32_t f3(uint32_t x, uint32_t y, uint32_t z) {
  return (x & z) | (y & ~z);
}

static inline uint32_t f4(uint32_t x, uint32_t y, uint32_t z) {
  return x ^ (y | ~z);
}

/*
 * Round constants for the left path.
 */
static const uint32_t KL[5] = {
    0x00000000, /* Round 0 */
    0x5a827999, /* Round 1 */
    0x6ed9eba1, /* Round 2 */
    0x8f1bbcdc, /* Round 3 */
    0xa953fd4e  /* Round 4 */
};

/*
 * Round constants for the right path.
 */
static const uint32_t KR[5] = {
    0x50a28be6, /* Round 0 */
    0x5c4dd124, /* Round 1 */
    0x6d703ef3, /* Round 2 */
    0x7a6d76e9, /* Round 3 */
    0x00000000  /* Round 4 */
};

/*
 * Message word selection for left path.
 * r[j] gives the index of the message word to use at step j.
 */
static const int RL[80] = {
    /* Round 0 */
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    /* Round 1 */
    7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8,
    /* Round 2 */
    3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12,
    /* Round 3 */
    1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2,
    /* Round 4 */
    4, 0, 5, 9, 7, 12, 2, 10, 14, 1, 3, 8, 11, 6, 15, 13};

/*
 * Message word selection for right path.
 */
static const int RR[80] = {
    /* Round 0 */
    5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12,
    /* Round 1 */
    6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2,
    /* Round 2 */
    15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13,
    /* Round 3 */
    8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14,
    /* Round 4 */
    12, 15, 10, 4, 1, 5, 8, 7, 6, 2, 13, 14, 0, 3, 9, 11};

/*
 * Rotation amounts for left path.
 */
static const int SL[80] = {
    /* Round 0 */
    11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8,
    /* Round 1 */
    7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12,
    /* Round 2 */
    11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5,
    /* Round 3 */
    11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12,
    /* Round 4 */
    9, 15, 5, 11, 6, 8, 13, 12, 5, 12, 13, 14, 11, 8, 5, 6};

/*
 * Rotation amounts for right path.
 */
static const int SR[80] = {
    /* Round 0 */
    8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6,
    /* Round 1 */
    9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11,
    /* Round 2 */
    9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5,
    /* Round 3 */
    15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8,
    /* Round 4 */
    8, 5, 12, 9, 12, 5, 14, 6, 8, 13, 6, 5, 15, 13, 11, 11};

/*
 * Load 32-bit little-endian value from byte array.
 * RIPEMD-160 uses little-endian byte order.
 */
static inline uint32_t load_le32(const uint8_t *p) {
  return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/*
 * Store 32-bit value as little-endian bytes.
 */
static inline void store_le32(uint8_t *p, uint32_t x) {
  p[0] = (uint8_t)(x);
  p[1] = (uint8_t)(x >> 8);
  p[2] = (uint8_t)(x >> 16);
  p[3] = (uint8_t)(x >> 24);
}

/*
 * Store 64-bit value as little-endian bytes.
 */
static inline void store_le64(uint8_t *p, uint64_t x) {
  p[0] = (uint8_t)(x);
  p[1] = (uint8_t)(x >> 8);
  p[2] = (uint8_t)(x >> 16);
  p[3] = (uint8_t)(x >> 24);
  p[4] = (uint8_t)(x >> 32);
  p[5] = (uint8_t)(x >> 40);
  p[6] = (uint8_t)(x >> 48);
  p[7] = (uint8_t)(x >> 56);
}

/*
 * Process one 64-byte block.
 *
 * This is the RIPEMD-160 compression function.
 * It processes data in two parallel paths (left and right)
 * and combines them at the end.
 */
static void ripemd160_transform(uint32_t state[5], const uint8_t block[64]) {
  uint32_t X[16];
  uint32_t al, bl, cl, dl, el; /* Left path */
  uint32_t ar, br, cr, dr, er; /* Right path */
  uint32_t t;
  int j;

  /* Load message block as 16 little-endian 32-bit words */
  for (j = 0; j < 16; j++) {
    X[j] = load_le32(block + (size_t)(j * 4));
  }

  /* Initialize working variables */
  al = ar = state[0];
  bl = br = state[1];
  cl = cr = state[2];
  dl = dr = state[3];
  el = er = state[4];

  /* 80 rounds, split into 5 groups of 16 */
  for (j = 0; j < 80; j++) {
    int round = j / 16;

    /* Left path */
    switch (round) {
    case 0:
      t = f0(bl, cl, dl);
      break;
    case 1:
      t = f1(bl, cl, dl);
      break;
    case 2:
      t = f2(bl, cl, dl);
      break;
    case 3:
      t = f3(bl, cl, dl);
      break;
    case 4:
      t = f4(bl, cl, dl);
      break;
    default:
      t = 0;
      break; /* Never reached */
    }
    t = rotl32(al + t + X[RL[j]] + KL[round], SL[j]) + el;
    al = el;
    el = dl;
    dl = rotl32(cl, 10);
    cl = bl;
    bl = t;

    /* Right path */
    switch (round) {
    case 0:
      t = f4(br, cr, dr);
      break;
    case 1:
      t = f3(br, cr, dr);
      break;
    case 2:
      t = f2(br, cr, dr);
      break;
    case 3:
      t = f1(br, cr, dr);
      break;
    case 4:
      t = f0(br, cr, dr);
      break;
    default:
      t = 0;
      break; /* Never reached */
    }
    t = rotl32(ar + t + X[RR[j]] + KR[round], SR[j]) + er;
    ar = er;
    er = dr;
    dr = rotl32(cr, 10);
    cr = br;
    br = t;
  }

  /* Combine results */
  t = state[1] + cl + dr;
  state[1] = state[2] + dl + er;
  state[2] = state[3] + el + ar;
  state[3] = state[4] + al + br;
  state[4] = state[0] + bl + cr;
  state[0] = t;
}

void ripemd160_init(ripemd160_ctx_t *ctx) {
  ctx->state[0] = H_INIT[0];
  ctx->state[1] = H_INIT[1];
  ctx->state[2] = H_INIT[2];
  ctx->state[3] = H_INIT[3];
  ctx->state[4] = H_INIT[4];
  ctx->count = 0;
}

void ripemd160_update(ripemd160_ctx_t *ctx, const uint8_t *data, size_t len) {
  size_t buffered;
  size_t remaining;

  if (len == 0) {
    return;
  }

  /* How many bytes are already buffered? */
  buffered = (size_t)(ctx->count % RIPEMD160_BLOCK_SIZE);

  /* Update total count */
  ctx->count += len;

  /* If we have buffered data, try to complete a block */
  if (buffered > 0) {
    remaining = RIPEMD160_BLOCK_SIZE - buffered;
    if (len < remaining) {
      /* Not enough to complete a block, just buffer */
      memcpy(ctx->buffer + buffered, data, len);
      return;
    }
    /* Complete the buffered block */
    memcpy(ctx->buffer + buffered, data, remaining);
    ripemd160_transform(ctx->state, ctx->buffer);
    data += remaining;
    len -= remaining;
  }

  /* Process complete blocks directly from input */
  while (len >= RIPEMD160_BLOCK_SIZE) {
    ripemd160_transform(ctx->state, data);
    data += RIPEMD160_BLOCK_SIZE;
    len -= RIPEMD160_BLOCK_SIZE;
  }

  /* Buffer any remaining data */
  if (len > 0) {
    memcpy(ctx->buffer, data, len);
  }
}

void ripemd160_final(ripemd160_ctx_t *ctx, uint8_t out[RIPEMD160_DIGEST_SIZE]) {
  size_t buffered;
  uint64_t bit_count;
  int i;

  /* How many bytes in the buffer? */
  buffered = (size_t)(ctx->count % RIPEMD160_BLOCK_SIZE);

  /* Total message length in bits */
  bit_count = ctx->count * 8;

  /*
   * Padding: append 1 bit, then zeros, then 64-bit length (little-endian).
   * The padding must result in a message length that is a multiple of 512 bits.
   */

  /* Append 0x80 (1 bit followed by 7 zero bits) */
  ctx->buffer[buffered++] = 0x80;

  /* If not enough room for the length (need 8 bytes), pad and process */
  if (buffered > 56) {
    /* Fill rest of block with zeros and process */
    memset(ctx->buffer + buffered, 0, RIPEMD160_BLOCK_SIZE - buffered);
    ripemd160_transform(ctx->state, ctx->buffer);
    buffered = 0;
  }

  /* Pad with zeros up to the length field */
  memset(ctx->buffer + buffered, 0, 56 - buffered);

  /* Append the 64-bit message length in bits (little-endian) */
  store_le64(ctx->buffer + 56, bit_count);

  /* Process the final block */
  ripemd160_transform(ctx->state, ctx->buffer);

  /* Output the hash (little-endian) */
  for (i = 0; i < 5; i++) {
    store_le32(out + (size_t)(i * 4), ctx->state[i]);
  }
}

void ripemd160(const uint8_t *data, size_t len,
               uint8_t out[RIPEMD160_DIGEST_SIZE]) {
  ripemd160_ctx_t ctx;

  ripemd160_init(&ctx);
  ripemd160_update(&ctx, data, len);
  ripemd160_final(&ctx, out);
}

void hash160(const uint8_t *data, size_t len,
             uint8_t out[RIPEMD160_DIGEST_SIZE]) {
  uint8_t sha256_hash[SHA256_DIGEST_SIZE];

  /* HASH160(x) = RIPEMD160(SHA256(x)) */
  sha256(data, len, sha256_hash);
  ripemd160(sha256_hash, SHA256_DIGEST_SIZE, out);
}
