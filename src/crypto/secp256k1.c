/*
 * Bitcoin Echo — secp256k1 Implementation
 *
 * secp256k1 elliptic curve used by Bitcoin.
 *
 * This file implements:
 *   - Field arithmetic (mod p) for Session 2.3
 *   - Group operations (points) for Session 2.4
 *   - ECDSA verification for Session 2.5
 *   - Schnorr verification for Session 2.6
 *
 * The implementation prioritizes correctness over performance.
 * All operations follow the mathematical specifications directly.
 *
 * Build once. Build right. Stop.
 */

#include "secp256k1.h"
#include "sha256.h"
#include <stdint.h>
#include <string.h>

/*
 * ============================================================================
 * Constants
 * ============================================================================
 */

/*
 * The secp256k1 field prime: p = 2^256 - 2^32 - 977
 *
 * In hex: FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE
 * FFFFFC2F
 */
static const uint32_t SECP256K1_P[8] = {0xFFFFFC2F, 0xFFFFFFFE, 0xFFFFFFFF,
                                        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                        0xFFFFFFFF, 0xFFFFFFFF};

/*
 * The curve order: n (number of points on curve)
 *
 * In hex: FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE BAAEDCE6 AF48A03B BFD25E8C
 * D0364141
 */
static const uint32_t SECP256K1_N[8] = {0xD0364141, 0xBFD25E8C, 0xAF48A03B,
                                        0xBAAEDCE6, 0xFFFFFFFE, 0xFFFFFFFF,
                                        0xFFFFFFFF, 0xFFFFFFFF};

/*
 * The generator point G (x-coordinate).
 *
 * Gx = 79BE667E F9DCBBAC 55A06295 CE870B07 029BFCDB 2DCE28D9 59F2815B 16F81798
 */
static const uint32_t SECP256K1_GX[8] = {0x16F81798, 0x59F2815B, 0x2DCE28D9,
                                         0x029BFCDB, 0xCE870B07, 0x55A06295,
                                         0xF9DCBBAC, 0x79BE667E};

/*
 * The generator point G (y-coordinate).
 *
 * Gy = 483ADA77 26A3C465 5DA4FBFC 0E1108A8 FD17B448 A6855419 9C47D08F FB10D4B8
 */
static const uint32_t SECP256K1_GY[8] = {0xFB10D4B8, 0x9C47D08F, 0xA6855419,
                                         0xFD17B448, 0x0E1108A8, 0x5DA4FBFC,
                                         0x26A3C465, 0x483ADA77};

/*
 * ============================================================================
 * Field Element Operations (mod p)
 * ============================================================================
 */

void secp256k1_fe_zero(secp256k1_fe_t *r) {
  int i;
  for (i = 0; i < 8; i++) {
    r->limbs[i] = 0;
  }
}

void secp256k1_fe_one(secp256k1_fe_t *r) {
  r->limbs[0] = 1;
  r->limbs[1] = 0;
  r->limbs[2] = 0;
  r->limbs[3] = 0;
  r->limbs[4] = 0;
  r->limbs[5] = 0;
  r->limbs[6] = 0;
  r->limbs[7] = 0;
}

void secp256k1_fe_set_int(secp256k1_fe_t *r, uint32_t n) {
  r->limbs[0] = n;
  r->limbs[1] = 0;
  r->limbs[2] = 0;
  r->limbs[3] = 0;
  r->limbs[4] = 0;
  r->limbs[5] = 0;
  r->limbs[6] = 0;
  r->limbs[7] = 0;
}

void secp256k1_fe_copy(secp256k1_fe_t *r, const secp256k1_fe_t *a) {
  int i;
  for (i = 0; i < 8; i++) {
    r->limbs[i] = a->limbs[i];
  }
}

/*
 * Compare a field element with the prime p.
 * Returns -1 if a < p, 0 if a == p, 1 if a > p.
 */
static int fe_cmp_p(const secp256k1_fe_t *a) {
  int i;
  for (i = 7; i >= 0; i--) {
    if (a->limbs[i] < SECP256K1_P[i])
      return -1;
    if (a->limbs[i] > SECP256K1_P[i])
      return 1;
  }
  return 0;
}

/*
 * Reduce field element modulo p if >= p.
 * Subtracts p if necessary.
 */
static void fe_reduce(secp256k1_fe_t *r) {
  uint64_t borrow;
  uint32_t tmp[8];
  int i;
  int need_reduce;

  /* Check if r >= p */
  need_reduce = (fe_cmp_p(r) >= 0);

  if (need_reduce) {
    /* Subtract p */
    borrow = 0;
    for (i = 0; i < 8; i++) {
      uint64_t diff = (uint64_t)r->limbs[i] - SECP256K1_P[i] - borrow;
      tmp[i] = (uint32_t)diff;
      borrow = (diff >> 63) & 1; /* 1 if borrow occurred */
    }
    for (i = 0; i < 8; i++) {
      r->limbs[i] = tmp[i];
    }
  }
}

int secp256k1_fe_set_bytes(secp256k1_fe_t *r, const uint8_t bytes[32]) {
  int i, j;

  /* Load big-endian bytes into little-endian limbs */
  for (i = 0; i < 8; i++) {
    j = (7 - i) * 4;
    r->limbs[i] = ((uint32_t)bytes[j] << 24) | ((uint32_t)bytes[j + 1] << 16) |
                  ((uint32_t)bytes[j + 2] << 8) | ((uint32_t)bytes[j + 3]);
  }

  /* Check if value is valid (< p) */
  if (fe_cmp_p(r) >= 0) {
    secp256k1_fe_zero(r);
    return 0;
  }

  return 1;
}

void secp256k1_fe_get_bytes(uint8_t bytes[32], const secp256k1_fe_t *a) {
  int i, j;

  /* Store little-endian limbs as big-endian bytes */
  for (i = 0; i < 8; i++) {
    j = (7 - i) * 4;
    bytes[j] = (uint8_t)(a->limbs[i] >> 24);
    bytes[j + 1] = (uint8_t)(a->limbs[i] >> 16);
    bytes[j + 2] = (uint8_t)(a->limbs[i] >> 8);
    bytes[j + 3] = (uint8_t)(a->limbs[i]);
  }
}

int secp256k1_fe_is_zero(const secp256k1_fe_t *a) {
  uint32_t z = 0;
  int i;

  for (i = 0; i < 8; i++) {
    z |= a->limbs[i];
  }

  return z == 0;
}

int secp256k1_fe_equal(const secp256k1_fe_t *a, const secp256k1_fe_t *b) {
  uint32_t diff = 0;
  int i;

  for (i = 0; i < 8; i++) {
    diff |= a->limbs[i] ^ b->limbs[i];
  }

  return diff == 0;
}

int secp256k1_fe_cmp(const secp256k1_fe_t *a, const secp256k1_fe_t *b) {
  int i;
  for (i = 7; i >= 0; i--) {
    if (a->limbs[i] < b->limbs[i])
      return -1;
    if (a->limbs[i] > b->limbs[i])
      return 1;
  }
  return 0;
}

int secp256k1_fe_is_odd(const secp256k1_fe_t *a) {
  return (int)(a->limbs[0] & 1);
}

void secp256k1_fe_neg(secp256k1_fe_t *r, const secp256k1_fe_t *a) {
  uint64_t borrow;
  int i;

  if (secp256k1_fe_is_zero(a)) {
    secp256k1_fe_zero(r);
    return;
  }

  /* r = p - a */
  borrow = 0;
  for (i = 0; i < 8; i++) {
    uint64_t diff = (uint64_t)SECP256K1_P[i] - a->limbs[i] - borrow;
    r->limbs[i] = (uint32_t)diff;
    borrow = (diff >> 63) & 1;
  }
}

void secp256k1_fe_add(secp256k1_fe_t *r, const secp256k1_fe_t *a,
                      const secp256k1_fe_t *b) {
  uint64_t carry = 0;
  int i;

  /* Add limbs with carry */
  for (i = 0; i < 8; i++) {
    carry += (uint64_t)a->limbs[i] + b->limbs[i];
    r->limbs[i] = (uint32_t)carry;
    carry >>= 32;
  }

  /* If carry or result >= p, subtract p */
  if (carry || fe_cmp_p(r) >= 0) {
    uint64_t borrow = 0;
    for (i = 0; i < 8; i++) {
      uint64_t diff = (uint64_t)r->limbs[i] - SECP256K1_P[i] - borrow;
      r->limbs[i] = (uint32_t)diff;
      borrow = (diff >> 63) & 1;
    }
  }
}

void secp256k1_fe_sub(secp256k1_fe_t *r, const secp256k1_fe_t *a,
                      const secp256k1_fe_t *b) {
  uint64_t borrow = 0;
  int i;

  /* Subtract limbs with borrow */
  for (i = 0; i < 8; i++) {
    uint64_t diff = (uint64_t)a->limbs[i] - b->limbs[i] - borrow;
    r->limbs[i] = (uint32_t)diff;
    borrow = (diff >> 63) & 1;
  }

  /* If borrow, add p */
  if (borrow) {
    uint64_t carry = 0;
    for (i = 0; i < 8; i++) {
      carry += (uint64_t)r->limbs[i] + SECP256K1_P[i];
      r->limbs[i] = (uint32_t)carry;
      carry >>= 32;
    }
  }
}

/*
 * Multiply two field elements: r = a * b (mod p)
 *
 * Uses schoolbook multiplication to get a 512-bit product,
 * then reduces modulo p using the special structure of p.
 *
 * p = 2^256 - 2^32 - 977
 * So 2^256 ≡ 2^32 + 977 (mod p)
 */
void secp256k1_fe_mul(secp256k1_fe_t *r, const secp256k1_fe_t *a,
                      const secp256k1_fe_t *b) {
  uint32_t t[16]; /* 512-bit product as 32-bit limbs */
  uint64_t carry;
  int i, j;

  /* Initialize product to zero */
  for (i = 0; i < 16; i++) {
    t[i] = 0;
  }

  /*
   * Schoolbook multiplication, row by row.
   * Process each a[i] against all b[j], propagating carries.
   * This ensures t[] entries never exceed 32 bits.
   */
  for (i = 0; i < 8; i++) {
    carry = 0;
    for (j = 0; j < 8; j++) {
      uint64_t prod = (uint64_t)a->limbs[i] * b->limbs[j];
      uint64_t sum = (uint64_t)t[i + j] + (prod & 0xFFFFFFFF) + carry;
      t[i + j] = (uint32_t)sum;
      carry = (sum >> 32) + (prod >> 32);
    }
    /* Propagate remaining carry through higher limbs */
    for (j = i + 8; j < 16 && carry; j++) {
      uint64_t sum = (uint64_t)t[j] + carry;
      t[j] = (uint32_t)sum;
      carry = sum >> 32;
    }
  }

  /*
   * Reduce modulo p.
   *
   * We have a 512-bit number t[0..15].
   * t = t_high * 2^256 + t_low
   *
   * Since 2^256 ≡ 2^32 + 977 (mod p):
   * t ≡ t_high * (2^32 + 977) + t_low (mod p)
   *   = t_low + t_high * 2^32 + t_high * 977
   *
   * t_high * 2^32 shifts t_high left by one limb position.
   */
  {
    uint64_t c1, c2, overflow;
    uint32_t low[8], high[8];

    /* Split into low and high 256-bit parts */
    for (i = 0; i < 8; i++) {
      low[i] = t[i];
      high[i] = t[i + 8];
    }

    /* Compute low + high * (2^32 + 977) */
    /* = low + high * 2^32 + high * 977 */

    /* First: low + high * 977 */
    c1 = 0;
    for (i = 0; i < 8; i++) {
      c1 += (uint64_t)low[i] + (uint64_t)high[i] * 977;
      low[i] = (uint32_t)c1;
      c1 >>= 32;
    }
    /* c1 contains overflow from high * 977 (contributes to position 8) */

    /* Now add high * 2^32 (high shifted left by 1 limb) */
    /* low[1..7] += high[0..6], and high[7] goes to position 8 */
    c2 = (uint64_t)low[1] + high[0];
    low[1] = (uint32_t)c2;
    c2 >>= 32;

    c2 += (uint64_t)low[2] + high[1];
    low[2] = (uint32_t)c2;
    c2 >>= 32;

    c2 += (uint64_t)low[3] + high[2];
    low[3] = (uint32_t)c2;
    c2 >>= 32;

    c2 += (uint64_t)low[4] + high[3];
    low[4] = (uint32_t)c2;
    c2 >>= 32;

    c2 += (uint64_t)low[5] + high[4];
    low[5] = (uint32_t)c2;
    c2 >>= 32;

    c2 += (uint64_t)low[6] + high[5];
    low[6] = (uint32_t)c2;
    c2 >>= 32;

    c2 += (uint64_t)low[7] + high[6];
    low[7] = (uint32_t)c2;
    c2 >>= 32;

    /* Position 8 overflow = c1 (from *977) + high[7] + c2 (from shift) */
    overflow = c1 + high[7] + c2;

    /* overflow * 2^256 ≡ overflow * (2^32 + 977) (mod p) */
    /* Add overflow * 977 to low[0..] and overflow to low[1..] */
    while (overflow) {
      uint64_t c3 = (uint64_t)low[0] + overflow * 977;
      low[0] = (uint32_t)c3;
      c3 >>= 32;

      c3 += (uint64_t)low[1] + overflow;
      low[1] = (uint32_t)c3;
      c3 >>= 32;

      for (i = 2; i < 8 && c3; i++) {
        c3 += low[i];
        low[i] = (uint32_t)c3;
        c3 >>= 32;
      }
      overflow = c3;
    }

    /* Copy result */
    for (i = 0; i < 8; i++) {
      r->limbs[i] = low[i];
    }

    /* Final reduction if >= p */
    fe_reduce(r);
  }
}

void secp256k1_fe_sqr(secp256k1_fe_t *r, const secp256k1_fe_t *a) {
  /* For now, just use multiplication */
  /* A dedicated squaring routine could be ~1.5x faster */
  secp256k1_fe_mul(r, a, a);
}

/*
 * Compute r = a^e (mod p) using square-and-multiply.
 * e is given as an array of 8 uint32_t in little-endian order.
 */
static void fe_pow(secp256k1_fe_t *r, const secp256k1_fe_t *a,
                   const uint32_t e[8]) {
  secp256k1_fe_t base, result, tmp;
  int i, j;

  secp256k1_fe_copy(&base, a);
  secp256k1_fe_one(&result);

  for (i = 0; i < 8; i++) {
    uint32_t word = e[i];
    for (j = 0; j < 32; j++) {
      if (word & 1) {
        secp256k1_fe_mul(&tmp, &result, &base);
        secp256k1_fe_copy(&result, &tmp);
      }
      secp256k1_fe_sqr(&tmp, &base);
      secp256k1_fe_copy(&base, &tmp);
      word >>= 1;
    }
  }

  secp256k1_fe_copy(r, &result);
}

/*
 * Invert field element: r = a^(-1) (mod p)
 *
 * Uses Fermat's little theorem: a^(-1) = a^(p-2) (mod p)
 *
 * p - 2 = FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE
 * FFFFFC2D
 */
void secp256k1_fe_inv(secp256k1_fe_t *r, const secp256k1_fe_t *a) {
  static const uint32_t P_MINUS_2[8] = {0xFFFFFC2D, 0xFFFFFFFE, 0xFFFFFFFF,
                                        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                        0xFFFFFFFF, 0xFFFFFFFF};

  fe_pow(r, a, P_MINUS_2);
}

/*
 * Compute square root: r = sqrt(a) (mod p), if it exists.
 *
 * Since p ≡ 3 (mod 4), we can use: sqrt(a) = a^((p+1)/4) (mod p)
 *
 * (p + 1) / 4 = 3FFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF
 * BFFFFF0C
 */
int secp256k1_fe_sqrt(secp256k1_fe_t *r, const secp256k1_fe_t *a) {
  static const uint32_t P_PLUS_1_DIV_4[8] = {0xBFFFFF0C, 0xFFFFFFFF, 0xFFFFFFFF,
                                             0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                             0xFFFFFFFF, 0x3FFFFFFF};

  secp256k1_fe_t candidate, check;

  /* Compute candidate = a^((p+1)/4) */
  fe_pow(&candidate, a, P_PLUS_1_DIV_4);

  /* Verify: candidate² should equal a */
  secp256k1_fe_sqr(&check, &candidate);

  if (!secp256k1_fe_equal(&check, a)) {
    /* No square root exists */
    secp256k1_fe_zero(r);
    return 0;
  }

  /* Return the "even" square root (LSB = 0) */
  if (secp256k1_fe_is_odd(&candidate)) {
    secp256k1_fe_neg(r, &candidate);
  } else {
    secp256k1_fe_copy(r, &candidate);
  }

  return 1;
}

/*
 * ============================================================================
 * Scalar Operations (mod n)
 * ============================================================================
 */

/*
 * Compare a scalar with n.
 */
static int scalar_cmp_n(const secp256k1_scalar_t *a) {
  int i;
  for (i = 7; i >= 0; i--) {
    if (a->limbs[i] < SECP256K1_N[i])
      return -1;
    if (a->limbs[i] > SECP256K1_N[i])
      return 1;
  }
  return 0;
}

/*
 * Reduce scalar modulo n if >= n.
 */
static void scalar_reduce(secp256k1_scalar_t *r) {
  uint64_t borrow;
  uint32_t tmp[8];
  int i;

  if (scalar_cmp_n(r) >= 0) {
    borrow = 0;
    for (i = 0; i < 8; i++) {
      uint64_t diff = (uint64_t)r->limbs[i] - SECP256K1_N[i] - borrow;
      tmp[i] = (uint32_t)diff;
      borrow = (diff >> 63) & 1;
    }
    for (i = 0; i < 8; i++) {
      r->limbs[i] = tmp[i];
    }
  }
}

void secp256k1_scalar_set_bytes(secp256k1_scalar_t *r,
                                const uint8_t bytes[32]) {
  int i, j;

  /* Load big-endian bytes into little-endian limbs */
  for (i = 0; i < 8; i++) {
    j = (7 - i) * 4;
    r->limbs[i] = ((uint32_t)bytes[j] << 24) | ((uint32_t)bytes[j + 1] << 16) |
                  ((uint32_t)bytes[j + 2] << 8) | ((uint32_t)bytes[j + 3]);
  }

  /* Reduce if >= n */
  scalar_reduce(r);
}

void secp256k1_scalar_get_bytes(uint8_t bytes[32],
                                const secp256k1_scalar_t *a) {
  int i, j;

  for (i = 0; i < 8; i++) {
    j = (7 - i) * 4;
    bytes[j] = (uint8_t)(a->limbs[i] >> 24);
    bytes[j + 1] = (uint8_t)(a->limbs[i] >> 16);
    bytes[j + 2] = (uint8_t)(a->limbs[i] >> 8);
    bytes[j + 3] = (uint8_t)(a->limbs[i]);
  }
}

int secp256k1_scalar_is_zero(const secp256k1_scalar_t *a) {
  uint32_t z = 0;
  int i;

  for (i = 0; i < 8; i++) {
    z |= a->limbs[i];
  }

  return z == 0;
}

/*
 * ============================================================================
 * Point Operations (to be completed in Session 2.4)
 * ============================================================================
 */

void secp256k1_point_set_infinity(secp256k1_point_t *r) {
  secp256k1_fe_zero(&r->x);
  secp256k1_fe_zero(&r->y);
  secp256k1_fe_zero(&r->z);
}

int secp256k1_point_is_infinity(const secp256k1_point_t *p) {
  return secp256k1_fe_is_zero(&p->z);
}

void secp256k1_point_set_xy(secp256k1_point_t *r, const secp256k1_fe_t *x,
                            const secp256k1_fe_t *y) {
  secp256k1_fe_copy(&r->x, x);
  secp256k1_fe_copy(&r->y, y);
  secp256k1_fe_one(&r->z);
}

void secp256k1_point_get_xy(secp256k1_fe_t *x, secp256k1_fe_t *y,
                            const secp256k1_point_t *p) {
  secp256k1_fe_t z_inv, z_inv2, z_inv3;

  /* Convert from Jacobian (X, Y, Z) to affine (X/Z², Y/Z³) */
  secp256k1_fe_inv(&z_inv, &p->z);
  secp256k1_fe_sqr(&z_inv2, &z_inv);
  secp256k1_fe_mul(&z_inv3, &z_inv2, &z_inv);

  if (x) {
    secp256k1_fe_mul(x, &p->x, &z_inv2);
  }
  if (y) {
    secp256k1_fe_mul(y, &p->y, &z_inv3);
  }
}

/*
 * ============================================================================
 * Point Operations (Session 2.4)
 * ============================================================================
 *
 * All point operations use Jacobian coordinates for efficiency.
 * A Jacobian point (X, Y, Z) represents the affine point (X/Z², Y/Z³).
 * The point at infinity is represented by Z = 0.
 *
 * For secp256k1: y² = x³ + 7 (a = 0, b = 7)
 */

/*
 * Point doubling: r = 2*p
 *
 * Using formulas for a = 0 (secp256k1):
 *   S = 4 * X * Y²
 *   M = 3 * X²
 *   X' = M² - 2*S
 *   Y' = M * (S - X') - 8 * Y⁴
 *   Z' = 2 * Y * Z
 *
 * Cost: 4M + 4S (approximately)
 */
void secp256k1_point_double(secp256k1_point_t *r, const secp256k1_point_t *p) {
  secp256k1_fe_t s, m, x3, y3, z3;
  secp256k1_fe_t y2, y4, x2, t1, t2;

  /* Handle point at infinity */
  if (secp256k1_point_is_infinity(p)) {
    secp256k1_point_set_infinity(r);
    return;
  }

  /* Handle Y = 0 (point of order 2, result is infinity) */
  if (secp256k1_fe_is_zero(&p->y)) {
    secp256k1_point_set_infinity(r);
    return;
  }

  /* Y² */
  secp256k1_fe_sqr(&y2, &p->y);

  /* S = 4 * X * Y² */
  secp256k1_fe_mul(&s, &p->x, &y2); /* X * Y² */
  secp256k1_fe_add(&s, &s, &s);     /* 2 * X * Y² */
  secp256k1_fe_add(&s, &s, &s);     /* 4 * X * Y² */

  /* X² */
  secp256k1_fe_sqr(&x2, &p->x);

  /* M = 3 * X² (since a = 0 for secp256k1) */
  secp256k1_fe_add(&m, &x2, &x2); /* 2 * X² */
  secp256k1_fe_add(&m, &m, &x2);  /* 3 * X² */

  /* X' = M² - 2*S */
  secp256k1_fe_sqr(&x3, &m);       /* M² */
  secp256k1_fe_add(&t1, &s, &s);   /* 2*S */
  secp256k1_fe_sub(&x3, &x3, &t1); /* M² - 2*S */

  /* Y⁴ */
  secp256k1_fe_sqr(&y4, &y2);

  /* Y' = M * (S - X') - 8 * Y⁴ */
  secp256k1_fe_sub(&t1, &s, &x3);  /* S - X' */
  secp256k1_fe_mul(&y3, &m, &t1);  /* M * (S - X') */
  secp256k1_fe_add(&t2, &y4, &y4); /* 2 * Y⁴ */
  secp256k1_fe_add(&t2, &t2, &t2); /* 4 * Y⁴ */
  secp256k1_fe_add(&t2, &t2, &t2); /* 8 * Y⁴ */
  secp256k1_fe_sub(&y3, &y3, &t2); /* M * (S - X') - 8 * Y⁴ */

  /* Z' = 2 * Y * Z */
  secp256k1_fe_mul(&z3, &p->y, &p->z); /* Y * Z */
  secp256k1_fe_add(&z3, &z3, &z3);     /* 2 * Y * Z */

  /* Store result */
  secp256k1_fe_copy(&r->x, &x3);
  secp256k1_fe_copy(&r->y, &y3);
  secp256k1_fe_copy(&r->z, &z3);
}

/*
 * Point addition: r = p + q
 *
 * Using standard Jacobian addition formulas:
 *   U1 = X1 * Z2²
 *   U2 = X2 * Z1²
 *   S1 = Y1 * Z2³
 *   S2 = Y2 * Z1³
 *   H = U2 - U1
 *   R = S2 - S1
 *   X' = R² - H³ - 2 * U1 * H²
 *   Y' = R * (U1 * H² - X') - S1 * H³
 *   Z' = H * Z1 * Z2
 *
 * Special cases:
 *   - If p is infinity, return q
 *   - If q is infinity, return p
 *   - If H == 0 and R == 0: return double(p)
 *   - If H == 0 and R != 0: return infinity (p = -q)
 */
void secp256k1_point_add(secp256k1_point_t *r, const secp256k1_point_t *p,
                         const secp256k1_point_t *q) {
  secp256k1_fe_t u1, u2, s1, s2, h, rr;
  secp256k1_fe_t z1_2, z1_3, z2_2, z2_3;
  secp256k1_fe_t h2, h3, u1h2;
  secp256k1_fe_t x3, y3, z3, t1;

  /* Handle infinity cases */
  if (secp256k1_point_is_infinity(p)) {
    secp256k1_fe_copy(&r->x, &q->x);
    secp256k1_fe_copy(&r->y, &q->y);
    secp256k1_fe_copy(&r->z, &q->z);
    return;
  }
  if (secp256k1_point_is_infinity(q)) {
    secp256k1_fe_copy(&r->x, &p->x);
    secp256k1_fe_copy(&r->y, &p->y);
    secp256k1_fe_copy(&r->z, &p->z);
    return;
  }

  /* Z1², Z1³ */
  secp256k1_fe_sqr(&z1_2, &p->z);
  secp256k1_fe_mul(&z1_3, &z1_2, &p->z);

  /* Z2², Z2³ */
  secp256k1_fe_sqr(&z2_2, &q->z);
  secp256k1_fe_mul(&z2_3, &z2_2, &q->z);

  /* U1 = X1 * Z2², U2 = X2 * Z1² */
  secp256k1_fe_mul(&u1, &p->x, &z2_2);
  secp256k1_fe_mul(&u2, &q->x, &z1_2);

  /* S1 = Y1 * Z2³, S2 = Y2 * Z1³ */
  secp256k1_fe_mul(&s1, &p->y, &z2_3);
  secp256k1_fe_mul(&s2, &q->y, &z1_3);

  /* H = U2 - U1 */
  secp256k1_fe_sub(&h, &u2, &u1);

  /* R = S2 - S1 */
  secp256k1_fe_sub(&rr, &s2, &s1);

  /* Check for special cases */
  if (secp256k1_fe_is_zero(&h)) {
    if (secp256k1_fe_is_zero(&rr)) {
      /* p == q, do doubling */
      secp256k1_point_double(r, p);
      return;
    } else {
      /* p == -q, result is infinity */
      secp256k1_point_set_infinity(r);
      return;
    }
  }

  /* H², H³ */
  secp256k1_fe_sqr(&h2, &h);
  secp256k1_fe_mul(&h3, &h2, &h);

  /* U1 * H² */
  secp256k1_fe_mul(&u1h2, &u1, &h2);

  /* X' = R² - H³ - 2 * U1 * H² */
  secp256k1_fe_sqr(&x3, &rr);          /* R² */
  secp256k1_fe_sub(&x3, &x3, &h3);     /* R² - H³ */
  secp256k1_fe_add(&t1, &u1h2, &u1h2); /* 2 * U1 * H² */
  secp256k1_fe_sub(&x3, &x3, &t1);     /* R² - H³ - 2 * U1 * H² */

  /* Y' = R * (U1 * H² - X') - S1 * H³ */
  secp256k1_fe_sub(&t1, &u1h2, &x3); /* U1 * H² - X' */
  secp256k1_fe_mul(&y3, &rr, &t1);   /* R * (U1 * H² - X') */
  secp256k1_fe_mul(&t1, &s1, &h3);   /* S1 * H³ */
  secp256k1_fe_sub(&y3, &y3, &t1);   /* R * (U1 * H² - X') - S1 * H³ */

  /* Z' = H * Z1 * Z2 */
  secp256k1_fe_mul(&z3, &p->z, &q->z); /* Z1 * Z2 */
  secp256k1_fe_mul(&z3, &z3, &h);      /* H * Z1 * Z2 */

  /* Store result */
  secp256k1_fe_copy(&r->x, &x3);
  secp256k1_fe_copy(&r->y, &y3);
  secp256k1_fe_copy(&r->z, &z3);
}

/*
 * Point negation: r = -p
 *
 * In Jacobian coordinates: -(X, Y, Z) = (X, -Y, Z)
 */
void secp256k1_point_neg(secp256k1_point_t *r, const secp256k1_point_t *p) {
  secp256k1_fe_copy(&r->x, &p->x);
  secp256k1_fe_neg(&r->y, &p->y);
  secp256k1_fe_copy(&r->z, &p->z);
}

/*
 * Scalar multiplication: r = k * p
 *
 * Uses double-and-add algorithm, processing bits from high to low.
 * This is a simple implementation; constant-time versions exist but
 * are more complex.
 */
void secp256k1_point_mul(secp256k1_point_t *r, const secp256k1_point_t *p,
                         const secp256k1_scalar_t *k) {
  secp256k1_point_t result, base, tmp;
  int i, j;
  int started = 0;

  /* Handle k = 0 */
  if (secp256k1_scalar_is_zero(k)) {
    secp256k1_point_set_infinity(r);
    return;
  }

  /* Handle point at infinity */
  if (secp256k1_point_is_infinity(p)) {
    secp256k1_point_set_infinity(r);
    return;
  }

  /* Copy base point */
  secp256k1_fe_copy(&base.x, &p->x);
  secp256k1_fe_copy(&base.y, &p->y);
  secp256k1_fe_copy(&base.z, &p->z);

  secp256k1_point_set_infinity(&result);

  /* Process bits from high to low */
  for (i = 7; i >= 0; i--) {
    uint32_t word = k->limbs[i];
    for (j = 31; j >= 0; j--) {
      if (started) {
        secp256k1_point_double(&tmp, &result);
        secp256k1_fe_copy(&result.x, &tmp.x);
        secp256k1_fe_copy(&result.y, &tmp.y);
        secp256k1_fe_copy(&result.z, &tmp.z);
      }

      if ((word >> j) & 1) {
        if (started) {
          secp256k1_point_add(&tmp, &result, &base);
          secp256k1_fe_copy(&result.x, &tmp.x);
          secp256k1_fe_copy(&result.y, &tmp.y);
          secp256k1_fe_copy(&result.z, &tmp.z);
        } else {
          secp256k1_fe_copy(&result.x, &base.x);
          secp256k1_fe_copy(&result.y, &base.y);
          secp256k1_fe_copy(&result.z, &base.z);
          started = 1;
        }
      }
    }
  }

  secp256k1_fe_copy(&r->x, &result.x);
  secp256k1_fe_copy(&r->y, &result.y);
  secp256k1_fe_copy(&r->z, &result.z);
}

/*
 * Scalar multiplication with generator: r = k * G
 */
void secp256k1_point_mul_gen(secp256k1_point_t *r,
                             const secp256k1_scalar_t *k) {
  secp256k1_point_t gen;
  secp256k1_fe_t gx, gy;
  int i;

  /* Load generator point G */
  for (i = 0; i < 8; i++) {
    gx.limbs[i] = SECP256K1_GX[i];
    gy.limbs[i] = SECP256K1_GY[i];
  }

  secp256k1_point_set_xy(&gen, &gx, &gy);
  secp256k1_point_mul(r, &gen, k);
}

/*
 * Check if point is on the curve: y² = x³ + 7 (mod p)
 * Returns 1 if valid, 0 otherwise.
 */
int secp256k1_point_is_valid(const secp256k1_point_t *p) {
  secp256k1_fe_t x, y, lhs, rhs, x3, seven;

  /* Infinity is valid */
  if (secp256k1_point_is_infinity(p)) {
    return 1;
  }

  /* Get affine coordinates */
  secp256k1_point_get_xy(&x, &y, p);

  /* Compute y² */
  secp256k1_fe_sqr(&lhs, &y);

  /* Compute x³ + 7 */
  secp256k1_fe_sqr(&x3, &x);
  secp256k1_fe_mul(&x3, &x3, &x);
  secp256k1_fe_set_int(&seven, 7);
  secp256k1_fe_add(&rhs, &x3, &seven);

  return secp256k1_fe_equal(&lhs, &rhs);
}

/*
 * Parse public key from bytes.
 * Supports:
 *   - Compressed (33 bytes, prefix 02/03)
 *   - Uncompressed (65 bytes, prefix 04)
 *
 * Returns 1 on success, 0 on failure.
 */
int secp256k1_pubkey_parse(secp256k1_point_t *p, const uint8_t *data,
                           size_t len) {
  secp256k1_fe_t x, y;

  if (len == 33 && (data[0] == 0x02 || data[0] == 0x03)) {
    /* Compressed format */
    if (!secp256k1_fe_set_bytes(&x, data + 1)) {
      return 0;
    }

    /* Compute y² = x³ + 7 */
    secp256k1_fe_t x3, y2, seven;
    secp256k1_fe_sqr(&x3, &x);
    secp256k1_fe_mul(&x3, &x3, &x);
    secp256k1_fe_set_int(&seven, 7);
    secp256k1_fe_add(&y2, &x3, &seven);

    /* Compute y = sqrt(y²) */
    if (!secp256k1_fe_sqrt(&y, &y2)) {
      return 0; /* No valid y coordinate */
    }

    /* Select correct y based on prefix */
    int y_is_odd = secp256k1_fe_is_odd(&y);
    int want_odd = (data[0] == 0x03);

    if (y_is_odd != want_odd) {
      secp256k1_fe_neg(&y, &y);
    }

    secp256k1_point_set_xy(p, &x, &y);
    return 1;

  } else if (len == 65 && data[0] == 0x04) {
    /* Uncompressed format */
    if (!secp256k1_fe_set_bytes(&x, data + 1)) {
      return 0;
    }
    if (!secp256k1_fe_set_bytes(&y, data + 33)) {
      return 0;
    }

    secp256k1_point_set_xy(p, &x, &y);

    /* Verify point is on curve */
    if (!secp256k1_point_is_valid(p)) {
      return 0;
    }

    return 1;
  }

  return 0; /* Invalid format */
}

/*
 * Serialize public key to bytes.
 * If compressed: outputs 33 bytes (prefix 02/03 + x)
 * If uncompressed: outputs 65 bytes (prefix 04 + x + y)
 */
void secp256k1_pubkey_serialize(uint8_t *out, const secp256k1_point_t *p,
                                int compressed) {
  secp256k1_fe_t x, y;

  secp256k1_point_get_xy(&x, &y, p);

  if (compressed) {
    out[0] = secp256k1_fe_is_odd(&y) ? 0x03 : 0x02;
    secp256k1_fe_get_bytes(out + 1, &x);
  } else {
    out[0] = 0x04;
    secp256k1_fe_get_bytes(out + 1, &x);
    secp256k1_fe_get_bytes(out + 33, &y);
  }
}

/*
 * ============================================================================
 * Additional Scalar Operations (Session 2.5)
 * ============================================================================
 */

/*
 * Scalar addition: r = a + b (mod n)
 */
void secp256k1_scalar_add(secp256k1_scalar_t *r, const secp256k1_scalar_t *a,
                          const secp256k1_scalar_t *b) {
  uint64_t carry = 0;
  int i;

  /* Add limbs with carry */
  for (i = 0; i < 8; i++) {
    carry += (uint64_t)a->limbs[i] + b->limbs[i];
    r->limbs[i] = (uint32_t)carry;
    carry >>= 32;
  }

  /* If carry or result >= n, subtract n */
  if (carry || scalar_cmp_n(r) >= 0) {
    uint64_t borrow = 0;
    for (i = 0; i < 8; i++) {
      uint64_t diff = (uint64_t)r->limbs[i] - SECP256K1_N[i] - borrow;
      r->limbs[i] = (uint32_t)diff;
      borrow = (diff >> 63) & 1;
    }
  }
}

/*
 * Scalar multiplication: r = a * b (mod n)
 *
 * Uses schoolbook multiplication followed by reduction using
 * the identity: 2^256 ≡ (2^256 - n) (mod n)
 *
 * 2^256 - n = 0x14551231950B75FC4402DA1732FC9BEBF
 */
void secp256k1_scalar_mul(secp256k1_scalar_t *r, const secp256k1_scalar_t *a,
                          const secp256k1_scalar_t *b) {
  uint32_t t[16]; /* 512-bit product */
  uint64_t carry;
  int i, j;

  /*
   * 2^256 - n in little-endian 32-bit limbs:
   * n     = FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE BAAEDCE6 AF48A03B BFD25E8C
   * D0364141 2^256 - n = 00000000 00000000 00000000 00000001 45512319 50B75FC4
   * 402DA173 2FC9BEBF
   */
  static const uint32_t K[8] = {0x2FC9BEBF, 0x402DA173, 0x50B75FC4, 0x45512319,
                                0x00000001, 0x00000000, 0x00000000, 0x00000000};

  /* Initialize product to zero */
  for (i = 0; i < 16; i++) {
    t[i] = 0;
  }

  /* Schoolbook multiplication with immediate carry propagation */
  for (i = 0; i < 8; i++) {
    carry = 0;
    for (j = 0; j < 8; j++) {
      uint64_t prod = (uint64_t)a->limbs[i] * b->limbs[j];
      uint64_t sum = (uint64_t)t[i + j] + (prod & 0xFFFFFFFF) + carry;
      t[i + j] = (uint32_t)sum;
      carry = (sum >> 32) + (prod >> 32);
    }
    /* Propagate remaining carry */
    for (j = i + 8; j < 16 && carry; j++) {
      uint64_t sum = (uint64_t)t[j] + carry;
      t[j] = (uint32_t)sum;
      carry = sum >> 32;
    }
  }

  /*
   * Reduce: t = t_low + t_high * 2^256
   *           ≡ t_low + t_high * K (mod n)
   *
   * We iterate until high part is zero.
   */
  while (t[8] || t[9] || t[10] || t[11] || t[12] || t[13] || t[14] || t[15]) {
    uint32_t high[8];
    uint64_t acc;

    /* Save high part */
    for (i = 0; i < 8; i++) {
      high[i] = t[i + 8];
      t[i + 8] = 0;
    }

    /* t_low += high * K */
    for (i = 0; i < 8; i++) {
      if (high[i] == 0)
        continue;
      carry = 0;
      for (j = 0; j < 5; j++) { /* K only has 5 non-zero limbs (0-4) */
        if (i + j < 16) {
          acc = (uint64_t)t[i + j] + (uint64_t)high[i] * K[j] + carry;
          t[i + j] = (uint32_t)acc;
          carry = acc >> 32;
        }
      }
      /* Propagate carry */
      for (j = i + 5; j < 16 && carry; j++) {
        acc = (uint64_t)t[j] + carry;
        t[j] = (uint32_t)acc;
        carry = acc >> 32;
      }
    }
  }

  /* Copy low 256 bits to result */
  for (i = 0; i < 8; i++) {
    r->limbs[i] = t[i];
  }

  /* Final reduction: subtract n if >= n */
  scalar_reduce(r);
}

/*
 * Scalar inversion: r = a^(-1) (mod n)
 *
 * Uses Fermat's little theorem: a^(-1) = a^(n-2) (mod n)
 */
void secp256k1_scalar_inv(secp256k1_scalar_t *r, const secp256k1_scalar_t *a) {
  /*
   * n - 2 = FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE BAAEDCE6 AF48A03B BFD25E8C
   * D036413F
   */
  static const uint32_t N_MINUS_2[8] = {0xD036413F, 0xBFD25E8C, 0xAF48A03B,
                                        0xBAAEDCE6, 0xFFFFFFFE, 0xFFFFFFFF,
                                        0xFFFFFFFF, 0xFFFFFFFF};

  secp256k1_scalar_t base, result, tmp;
  int i, j, k;

  /* Copy input */
  for (i = 0; i < 8; i++) {
    base.limbs[i] = a->limbs[i];
  }

  /* result = 1 */
  result.limbs[0] = 1;
  for (i = 1; i < 8; i++) {
    result.limbs[i] = 0;
  }

  /* Square-and-multiply */
  for (i = 0; i < 8; i++) {
    uint32_t word = N_MINUS_2[i];
    for (j = 0; j < 32; j++) {
      if (word & 1) {
        secp256k1_scalar_mul(&tmp, &result, &base);
        for (k = 0; k < 8; k++) {
          result.limbs[k] = tmp.limbs[k];
        }
      }
      secp256k1_scalar_mul(&tmp, &base, &base);
      for (k = 0; k < 8; k++) {
        base.limbs[k] = tmp.limbs[k];
      }
      word >>= 1;
    }
  }

  for (i = 0; i < 8; i++) {
    r->limbs[i] = result.limbs[i];
  }
}

/*
 * ============================================================================
 * ECDSA Signature Operations (Session 2.5)
 * ============================================================================
 */

/*
 * Parse DER-encoded ECDSA signature with strict BIP-66 validation.
 *
 * DER format:
 *   0x30 [total-length] 0x02 [r-length] [r] 0x02 [s-length] [s]
 *
 * BIP-66 rules:
 *   1. Total length must match actual content
 *   2. r and s must be valid positive integers
 *   3. No unnecessary leading zero bytes
 *   4. r and s must be in range [1, n-1]
 */
int secp256k1_ecdsa_sig_parse_der(secp256k1_ecdsa_sig_t *sig,
                                  const uint8_t *data, size_t len) {
  size_t pos = 0;
  size_t r_len, s_len;
  uint8_t r_bytes[33], s_bytes[33];
  uint8_t padded[32];
  int i;

  /* Minimum valid signature: 30 06 02 01 XX 02 01 XX = 8 bytes */
  if (len < 8 || len > 73) {
    return 0;
  }

  /* Check SEQUENCE tag */
  if (data[pos++] != 0x30) {
    return 0;
  }

  /* Check length byte */
  if (data[pos] != len - 2) {
    return 0;
  }
  pos++;

  /* Parse r INTEGER */
  if (data[pos++] != 0x02) {
    return 0;
  }

  r_len = data[pos++];
  if (r_len == 0 || r_len > 33) {
    return 0;
  }
  if (pos + r_len > len) {
    return 0;
  }

  /* Check for negative (high bit set without leading zero) */
  if (data[pos] & 0x80) {
    return 0; /* Negative integer not allowed */
  }

  /* Check for unnecessary leading zero */
  if (r_len > 1 && data[pos] == 0x00 && !(data[pos + 1] & 0x80)) {
    return 0; /* Unnecessary leading zero */
  }

  /* Copy r bytes */
  for (i = 0; i < (int)r_len && i < 33; i++) {
    r_bytes[i] = data[pos + (size_t)i];
  }
  pos += r_len;

  /* Parse s INTEGER */
  if (pos >= len || data[pos++] != 0x02) {
    return 0;
  }

  s_len = data[pos++];
  if (s_len == 0 || s_len > 33) {
    return 0;
  }
  if (pos + s_len != len) {
    return 0; /* Must consume exactly all bytes */
  }

  /* Check for negative */
  if (data[pos] & 0x80) {
    return 0;
  }

  /* Check for unnecessary leading zero */
  if (s_len > 1 && data[pos] == 0x00 && !(data[pos + 1] & 0x80)) {
    return 0;
  }

  /* Copy s bytes */
  for (i = 0; i < (int)s_len && i < 33; i++) {
    s_bytes[i] = data[pos + (size_t)i];
  }

  /* Convert r to scalar (pad to 32 bytes) */
  memset(padded, 0, 32);
  if (r_len <= 32) {
    memcpy(padded + (32 - r_len), r_bytes, r_len);
  } else {
    /* 33 bytes with leading zero - skip the zero */
    memcpy(padded, r_bytes + 1, 32);
  }
  secp256k1_scalar_set_bytes(&sig->r, padded);

  /* Check r != 0 */
  if (secp256k1_scalar_is_zero(&sig->r)) {
    return 0;
  }

  /* Convert s to scalar */
  memset(padded, 0, 32);
  if (s_len <= 32) {
    memcpy(padded + (32 - s_len), s_bytes, s_len);
  } else {
    memcpy(padded, s_bytes + 1, 32);
  }
  secp256k1_scalar_set_bytes(&sig->s, padded);

  /* Check s != 0 */
  if (secp256k1_scalar_is_zero(&sig->s)) {
    return 0;
  }

  return 1;
}

/*
 * ECDSA verification.
 *
 * Algorithm:
 *   1. Check r, s in [1, n-1] (already done in parse)
 *   2. e = message hash as scalar
 *   3. w = s^(-1) mod n
 *   4. u1 = e * w mod n
 *   5. u2 = r * w mod n
 *   6. R = u1 * G + u2 * P
 *   7. If R = infinity, reject
 *   8. Accept if R.x mod n == r
 */
int secp256k1_ecdsa_verify(const secp256k1_ecdsa_sig_t *sig,
                           const uint8_t msg_hash[32],
                           const secp256k1_point_t *pubkey) {
  secp256k1_scalar_t e, w, u1, u2;
  secp256k1_point_t R1, R2, R;
  secp256k1_fe_t rx;
  uint8_t rx_bytes[32];
  secp256k1_scalar_t rx_scalar;
  int i;

  /* Check pubkey is not infinity */
  if (secp256k1_point_is_infinity(pubkey)) {
    return 0;
  }

  /* Check pubkey is on curve */
  if (!secp256k1_point_is_valid(pubkey)) {
    return 0;
  }

  /* e = message hash as scalar (reduced mod n) */
  secp256k1_scalar_set_bytes(&e, msg_hash);

  /* w = s^(-1) mod n */
  secp256k1_scalar_inv(&w, &sig->s);

  /* u1 = e * w mod n */
  secp256k1_scalar_mul(&u1, &e, &w);

  /* u2 = r * w mod n */
  secp256k1_scalar_mul(&u2, &sig->r, &w);

  /* R1 = u1 * G */
  secp256k1_point_mul_gen(&R1, &u1);

  /* R2 = u2 * P */
  secp256k1_point_mul(&R2, pubkey, &u2);

  /* R = R1 + R2 */
  secp256k1_point_add(&R, &R1, &R2);

  /* If R = infinity, reject */
  if (secp256k1_point_is_infinity(&R)) {
    return 0;
  }

  /* Get R.x in affine coordinates */
  secp256k1_point_get_xy(&rx, NULL, &R);

  /* Convert to bytes, then to scalar (to reduce mod n) */
  secp256k1_fe_get_bytes(rx_bytes, &rx);
  secp256k1_scalar_set_bytes(&rx_scalar, rx_bytes);

  /* Accept if rx_scalar == r */
  for (i = 0; i < 8; i++) {
    if (rx_scalar.limbs[i] != sig->r.limbs[i]) {
      return 0;
    }
  }

  return 1;
}

/*
 * ============================================================================
 * Schnorr Signature Operations — BIP-340 (Session 2.6)
 * ============================================================================
 */

/*
 * Lift x-only public key to curve point.
 *
 * BIP-340 "lift_x" operation:
 *   1. Check x < p
 *   2. Compute y² = x³ + 7 (mod p)
 *   3. Compute y = sqrt(y²), fail if no square root
 *   4. Return point with even y
 */
int secp256k1_xonly_pubkey_parse(secp256k1_point_t *p,
                                 const uint8_t xonly[32]) {
  secp256k1_fe_t x, y, x3, y2, seven;

  /* Load x-coordinate */
  if (!secp256k1_fe_set_bytes(&x, xonly)) {
    return 0; /* x >= p */
  }

  /* Compute y² = x³ + 7 */
  secp256k1_fe_sqr(&x3, &x);
  secp256k1_fe_mul(&x3, &x3, &x);
  secp256k1_fe_set_int(&seven, 7);
  secp256k1_fe_add(&y2, &x3, &seven);

  /* Compute y = sqrt(y²) */
  if (!secp256k1_fe_sqrt(&y, &y2)) {
    return 0; /* Not a valid x-coordinate */
  }

  /* Ensure y is even (BIP-340 convention) */
  if (secp256k1_fe_is_odd(&y)) {
    secp256k1_fe_neg(&y, &y);
  }

  secp256k1_point_set_xy(p, &x, &y);
  return 1;
}

/*
 * Serialize point to x-only format (32 bytes).
 */
void secp256k1_xonly_pubkey_serialize(uint8_t xonly[32],
                                      const secp256k1_point_t *p) {
  secp256k1_fe_t x;

  secp256k1_point_get_xy(&x, NULL, p);
  secp256k1_fe_get_bytes(xonly, &x);
}

/*
 * BIP-340 tagged hash: SHA256(SHA256(tag) || SHA256(tag) || msg)
 *
 * The double SHA256(tag) prefix provides domain separation and allows
 * optimized implementations to precompute the midstate.
 */
void secp256k1_schnorr_tagged_hash(uint8_t out[32], const char *tag,
                                   const uint8_t *msg, size_t msg_len) {
  sha256_ctx_t ctx;
  uint8_t tag_hash[32];
  size_t tag_len;

  /* Compute SHA256(tag) */
  tag_len = strlen(tag);
  sha256((const uint8_t *)tag, tag_len, tag_hash);

  /* Compute SHA256(tag_hash || tag_hash || msg) */
  sha256_init(&ctx);
  sha256_update(&ctx, tag_hash, 32);
  sha256_update(&ctx, tag_hash, 32);
  sha256_update(&ctx, msg, msg_len);
  sha256_final(&ctx, out);
}

/*
 * Negate scalar: r = -a (mod n)
 */
static void scalar_negate(secp256k1_scalar_t *r, const secp256k1_scalar_t *a) {
  uint64_t borrow = 0;
  int i;

  if (secp256k1_scalar_is_zero(a)) {
    for (i = 0; i < 8; i++) {
      r->limbs[i] = 0;
    }
    return;
  }

  /* r = n - a */
  for (i = 0; i < 8; i++) {
    uint64_t diff = (uint64_t)SECP256K1_N[i] - a->limbs[i] - borrow;
    r->limbs[i] = (uint32_t)diff;
    borrow = (diff >> 63) & 1;
  }
}

/*
 * Verify BIP-340 Schnorr signature.
 *
 * Algorithm:
 *   1. P = lift_x(pk)
 *   2. r = int(sig[0:32]), s = int(sig[32:64])
 *   3. If r >= p, fail
 *   4. If s >= n, fail
 *   5. e = int(tagged_hash("BIP0340/challenge", r || pk || msg)) mod n
 *   6. R = s*G - e*P
 *   7. If R is infinity or has_even_y(R) is false or x(R) != r, fail
 *   8. Return success
 */
int secp256k1_schnorr_verify(const uint8_t sig[64], const uint8_t *msg,
                             size_t msg_len, const uint8_t pubkey[32]) {
  secp256k1_point_t P, R, sG, eP;
  secp256k1_fe_t r_fe, Rx, Ry;
  secp256k1_scalar_t s, e, neg_e;
  uint8_t
      challenge_input[32 + 32 + 1024]; /* r || pk || msg (max 1024 for msg) */
  uint8_t challenge_hash[32];
  uint8_t r_bytes[32], Rx_bytes[32];
  int i;

  /* For very large messages, we need dynamic allocation or streaming.
   * For simplicity, we limit message size here. BIP-340 allows any size. */
  if (msg_len > 1024) {
    /* For messages > 1024 bytes, use streaming approach */
    sha256_ctx_t ctx;
    uint8_t tag_hash[32];

    sha256((const uint8_t *)"BIP0340/challenge", 17, tag_hash);
    sha256_init(&ctx);
    sha256_update(&ctx, tag_hash, 32);
    sha256_update(&ctx, tag_hash, 32);
    sha256_update(&ctx, sig, 32);      /* r */
    sha256_update(&ctx, pubkey, 32);   /* pk */
    sha256_update(&ctx, msg, msg_len); /* msg */
    sha256_final(&ctx, challenge_hash);
  } else {
    /* Build challenge input: r || pk || msg */
    memcpy(challenge_input, sig, 32);           /* r */
    memcpy(challenge_input + 32, pubkey, 32);   /* pk */
    memcpy(challenge_input + 64, msg, msg_len); /* msg */

    /* e = tagged_hash("BIP0340/challenge", r || pk || msg) */
    secp256k1_schnorr_tagged_hash(challenge_hash, "BIP0340/challenge",
                                  challenge_input, 64 + msg_len);
  }

  /* 1. P = lift_x(pk) */
  if (!secp256k1_xonly_pubkey_parse(&P, pubkey)) {
    return 0;
  }

  /* 2. Parse r and s from signature */
  /* r is just bytes, check later if valid x-coordinate */
  memcpy(r_bytes, sig, 32);

  /* Check r < p by trying to load as field element */
  if (!secp256k1_fe_set_bytes(&r_fe, r_bytes)) {
    return 0; /* r >= p */
  }

  /* Load s as scalar (automatically reduced mod n) */
  secp256k1_scalar_set_bytes(&s, sig + 32);

  /* Check s < n by comparing original bytes with what we loaded */
  {
    uint8_t s_check[32];
    secp256k1_scalar_get_bytes(s_check, &s);
    if (memcmp(s_check, sig + 32, 32) != 0) {
      return 0; /* s >= n (was reduced) */
    }
  }

  /* 5. e = int(challenge_hash) mod n */
  secp256k1_scalar_set_bytes(&e, challenge_hash);

  /* 6. R = s*G - e*P = s*G + (-e)*P */
  scalar_negate(&neg_e, &e);

  /* Compute s*G */
  secp256k1_point_mul_gen(&sG, &s);

  /* Compute (-e)*P */
  secp256k1_point_mul(&eP, &P, &neg_e);

  /* R = sG + (-e)P */
  secp256k1_point_add(&R, &sG, &eP);

  /* 7a. If R is infinity, fail */
  if (secp256k1_point_is_infinity(&R)) {
    return 0;
  }

  /* 7b. Get R coordinates */
  secp256k1_point_get_xy(&Rx, &Ry, &R);

  /* 7c. Check has_even_y(R) */
  if (secp256k1_fe_is_odd(&Ry)) {
    return 0;
  }

  /* 7d. Check x(R) == r */
  secp256k1_fe_get_bytes(Rx_bytes, &Rx);
  for (i = 0; i < 32; i++) {
    if (Rx_bytes[i] != r_bytes[i]) {
      return 0;
    }
  }

  /* 8. Success */
  return 1;
}
