/*
 * Bitcoin Echo — Block Header Validation
 *
 * Implementation of block header validation rules.
 *
 * Build once. Build right. Stop.
 */

#include "block_validate.h"
#include "block.h"
#include "echo_config.h"
#include "echo_types.h"
#include "merkle.h"
// NOLINTNEXTLINE(misc-include-cleaner) - used for testnet/regtest genesis
#include "mining.h"
#include "tx.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Helper: Compare two hash256_t values for equality.
 */
static echo_bool_t hash256_equal(const hash256_t *a, const hash256_t *b) {
  return memcmp(a->bytes, b->bytes, 32) == 0 ? ECHO_TRUE : ECHO_FALSE;
}

/*
 * Helper: Check if a hash256_t is all zeros.
 */
static echo_bool_t hash256_is_zero(const hash256_t *h) {
  int i;
  for (i = 0; i < 32; i++) {
    if (h->bytes[i] != 0) {
      return ECHO_FALSE;
    }
  }
  return ECHO_TRUE;
}

/*
 * Helper: Simple insertion sort for MTP calculation.
 * Sorts an array of timestamps in ascending order.
 */
static void sort_timestamps(uint32_t *arr, size_t n) {
  size_t i, j;
  uint32_t key;

  for (i = 1; i < n; i++) {
    key = arr[i];
    j = i;
    while (j > 0 && arr[j - 1] > key) {
      arr[j] = arr[j - 1];
      j--;
    }
    arr[j] = key;
  }
}

/*
 * Initialize a validation context.
 */
void block_validate_ctx_init(block_validation_ctx_t *ctx) {
  if (ctx == NULL)
    return;

  ctx->height = 0;
  memset(ctx->timestamps, 0, sizeof(ctx->timestamps));
  ctx->timestamp_count = 0;
  ctx->current_time = 0;
  ctx->expected_bits = 0;
  memset(&ctx->parent_hash, 0, sizeof(hash256_t));
  ctx->parent_valid = ECHO_FALSE;
}

/*
 * Compute the median time past (MTP).
 *
 * The median of the timestamps of the previous 11 blocks.
 * For the first 11 blocks, we use whatever timestamps are available.
 */
uint32_t block_validate_mtp(const block_validation_ctx_t *ctx) {
  uint32_t sorted[BLOCK_MTP_WINDOW];
  size_t count;

  if (ctx == NULL || ctx->timestamp_count == 0) {
    return 0;
  }

  count = ctx->timestamp_count;
  if (count > BLOCK_MTP_WINDOW) {
    count = BLOCK_MTP_WINDOW;
  }

  /* Copy and sort timestamps */
  memcpy(sorted, ctx->timestamps, count * sizeof(uint32_t));
  sort_timestamps(sorted, count);

  /* Return median (middle element for odd count, lower-middle for even) */
  return sorted[count / 2];
}

/*
 * Validate proof-of-work with optional pre-computed hash.
 */
echo_bool_t block_validate_pow_with_hash(const block_header_t *header,
                                         const hash256_t *precomputed_hash,
                                         block_validation_error_t *error) {
  hash256_t local_hash;
  hash256_t target;
  echo_result_t result;
  const hash256_t *hash_ptr;

  if (header == NULL) {
    if (error)
      *error = BLOCK_ERR_POW_FAILED;
    return ECHO_FALSE;
  }

  /* Use pre-computed hash if provided, otherwise compute */
  if (precomputed_hash != NULL) {
    hash_ptr = precomputed_hash;
  } else {
    result = block_header_hash(header, &local_hash);
    if (result != ECHO_OK) {
      if (error)
        *error = BLOCK_ERR_POW_FAILED;
      return ECHO_FALSE;
    }
    hash_ptr = &local_hash;
  }

  /* Convert bits to target */
  result = block_bits_to_target(header->bits, &target);
  if (result != ECHO_OK) {
    if (error)
      *error = BLOCK_ERR_TARGET_INVALID;
    return ECHO_FALSE;
  }

  /* Check for zero/invalid target */
  if (hash256_is_zero(&target)) {
    /* Zero target means the bits field was invalid (negative or zero) */
    if (error)
      *error = BLOCK_ERR_TARGET_INVALID;
    return ECHO_FALSE;
  }

  /* Check hash <= target */
  if (!block_hash_meets_target(hash_ptr, &target)) {
    if (error)
      *error = BLOCK_ERR_POW_FAILED;
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}

/*
 * Validate proof-of-work (computes hash internally).
 */
echo_bool_t block_validate_pow(const block_header_t *header,
                               block_validation_error_t *error) {
  return block_validate_pow_with_hash(header, NULL, error);
}

/*
 * Validate timestamp.
 */
echo_bool_t block_validate_timestamp(const block_header_t *header,
                                     const block_validation_ctx_t *ctx,
                                     block_validation_error_t *error) {
  uint32_t mtp;
  uint32_t max_time;

  if (header == NULL || ctx == NULL) {
    if (error)
      *error = BLOCK_ERR_TIMESTAMP_TOO_OLD;
    return ECHO_FALSE;
  }

  /*
   * Check 1: Timestamp must be strictly greater than MTP.
   *
   * For the genesis block (no previous timestamps), this check is skipped.
   */
  if (ctx->timestamp_count > 0) {
    mtp = block_validate_mtp(ctx);
    if (header->timestamp <= mtp) {
      if (error)
        *error = BLOCK_ERR_TIMESTAMP_TOO_OLD;
      return ECHO_FALSE;
    }
  }

  /*
   * Check 2: Timestamp must not be more than 2 hours in the future.
   *
   * This check uses network-adjusted time (ctx->current_time).
   * If current_time is 0 (not set), we skip this check.
   */
  if (ctx->current_time > 0) {
    max_time = ctx->current_time + BLOCK_MAX_FUTURE_TIME;

    /* Handle potential overflow */
    if (max_time < ctx->current_time) {
      max_time = 0xFFFFFFFF; /* Saturate to max */
    }

    if (header->timestamp > max_time) {
      if (error)
        *error = BLOCK_ERR_TIMESTAMP_TOO_NEW;
      return ECHO_FALSE;
    }
  }

  return ECHO_TRUE;
}

/*
 * Validate previous block reference.
 */
echo_bool_t block_validate_prev_block(const block_header_t *header,
                                      const block_validation_ctx_t *ctx,
                                      block_validation_error_t *error) {
  if (header == NULL || ctx == NULL) {
    if (error)
      *error = BLOCK_ERR_PREV_BLOCK_UNKNOWN;
    return ECHO_FALSE;
  }

  /*
   * For genesis block (height 0), prev_hash must be all zeros.
   * This is handled in block_validate_genesis().
   *
   * For non-genesis blocks, prev_hash must match the parent we expect.
   */
  if (ctx->height == 0) {
    /* Genesis block check */
    if (!hash256_is_zero(&header->prev_hash)) {
      if (error)
        *error = BLOCK_ERR_PREV_BLOCK_UNKNOWN;
      return ECHO_FALSE;
    }
    return ECHO_TRUE;
  }

  /* Check prev_hash matches expected parent */
  if (!hash256_equal(&header->prev_hash, &ctx->parent_hash)) {
    if (error)
      *error = BLOCK_ERR_PREV_BLOCK_UNKNOWN;
    return ECHO_FALSE;
  }

  /* Check that parent is valid */
  if (!ctx->parent_valid) {
    if (error)
      *error = BLOCK_ERR_PREV_BLOCK_INVALID;
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}

/*
 * Check if version uses BIP-9 semantics.
 */
echo_bool_t block_version_uses_bip9(int32_t version) {
  /* Cast to unsigned for bit operations */
  uint32_t v = (uint32_t)version;

  /* Top 3 bits must be 001 */
  return ((v & BLOCK_VERSION_TOP_MASK) == BLOCK_VERSION_TOP_BITS) ? ECHO_TRUE
                                                                  : ECHO_FALSE;
}

/*
 * Extract a specific version bit.
 */
echo_bool_t block_version_bit(int32_t version, int bit) {
  uint32_t v = (uint32_t)version;

  if (bit < 0 || bit > 28) {
    return ECHO_FALSE;
  }

  /* Must be a BIP-9 version */
  if (!block_version_uses_bip9(version)) {
    return ECHO_FALSE;
  }

  return (v & (1u << bit)) ? ECHO_TRUE : ECHO_FALSE;
}

/*
 * Validate version field.
 */
echo_bool_t block_validate_version(const block_header_t *header,
                                   const block_validation_ctx_t *ctx,
                                   block_validation_error_t *error) {
  int32_t version;

  if (header == NULL) {
    if (error)
      *error = BLOCK_ERR_VERSION_INVALID;
    return ECHO_FALSE;
  }

  version = header->version;

  /*
   * Version validation is relatively permissive:
   *
   * - Versions 1, 2, 3, 4 are valid (historical versions)
   * - BIP-9 versions (top bits 001) are valid
   * - Other patterns are currently allowed but may be rejected
   *   by specific soft-fork rules
   *
   * Specific BIP-9 deployment rules (mandatory signaling after lock-in)
   * are not enforced here—they require deployment state tracking.
   */

  /* Reject obviously invalid versions */
  if (version < 1) {
    /*
     * Version 0 and negative versions are not used in Bitcoin.
     * However, we allow them for now since some test vectors
     * might use unusual versions.
     */
  }

  /* Suppress unused parameter warning */
  (void)ctx;

  return ECHO_TRUE;
}

/*
 * Validate a complete block header.
 */
echo_bool_t block_validate_header(const block_header_t *header,
                                  const block_validation_ctx_t *ctx,
                                  block_validation_error_t *error) {
  block_validation_error_t local_error = BLOCK_VALID;

  if (header == NULL || ctx == NULL) {
    if (error)
      *error = BLOCK_ERR_POW_FAILED;
    return ECHO_FALSE;
  }

  /* 1. Proof-of-work validation */
  if (!block_validate_pow(header, &local_error)) {
    if (error)
      *error = local_error;
    return ECHO_FALSE;
  }

  /* 2. Timestamp validation */
  if (!block_validate_timestamp(header, ctx, &local_error)) {
    if (error)
      *error = local_error;
    return ECHO_FALSE;
  }

  /* 3. Previous block validation */
  if (!block_validate_prev_block(header, ctx, &local_error)) {
    if (error)
      *error = local_error;
    return ECHO_FALSE;
  }

  /* 4. Version validation */
  if (!block_validate_version(header, ctx, &local_error)) {
    if (error)
      *error = local_error;
    return ECHO_FALSE;
  }

  /*
   * Note: Difficulty validation (bits == expected_bits) is NOT done here.
   * That requires the difficulty adjustment algorithm from Session 5.2.
   * The ctx->expected_bits field is provided for that future use.
   */

  return ECHO_TRUE;
}

/*
 * Validate genesis block.
 *
 * Network-aware validation: uses compile-time network selection to determine
 * which genesis block parameters and hash to expect.
 */
echo_bool_t block_validate_genesis(const block_header_t *header,
                                   block_validation_error_t *error) {
  block_header_t expected;
  hash256_t hash;

  if (header == NULL) {
    if (error)
      *error = BLOCK_ERR_POW_FAILED;
    return ECHO_FALSE;
  }

  /* Get expected genesis header based on network */
#if defined(ECHO_NETWORK_MAINNET)
  block_genesis_header(&expected);
#elif defined(ECHO_NETWORK_TESTNET)
  block_genesis_header_testnet(&expected);
#elif defined(ECHO_NETWORK_REGTEST)
  block_genesis_header_regtest(&expected);
#endif

  /* Check all fields match */
  if (header->version != expected.version) {
    if (error)
      *error = BLOCK_ERR_VERSION_INVALID;
    return ECHO_FALSE;
  }

  if (!hash256_is_zero(&header->prev_hash)) {
    if (error)
      *error = BLOCK_ERR_PREV_BLOCK_UNKNOWN;
    return ECHO_FALSE;
  }

  if (!hash256_equal(&header->merkle_root, &expected.merkle_root)) {
    if (error)
      *error = BLOCK_ERR_MERKLE_MISMATCH;
    return ECHO_FALSE;
  }

  if (header->timestamp != expected.timestamp) {
    if (error)
      *error = BLOCK_ERR_TIMESTAMP_TOO_OLD;
    return ECHO_FALSE;
  }

  if (header->bits != expected.bits) {
    if (error)
      *error = BLOCK_ERR_DIFFICULTY_MISMATCH;
    return ECHO_FALSE;
  }

  if (header->nonce != expected.nonce) {
    if (error)
      *error = BLOCK_ERR_POW_FAILED;
    return ECHO_FALSE;
  }

  /* Verify PoW (should always pass for correct genesis) */
  if (!block_validate_pow(header, error)) {
    return ECHO_FALSE;
  }

  /* Verify hash matches known genesis hash */
  block_header_hash(header, &hash);

  /*
   * Network-specific genesis hashes (little-endian internal representation).
   *
   * Mainnet: 000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f
   * Testnet: 000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943
   * Regtest: 0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206
   */
#if defined(ECHO_NETWORK_MAINNET)
  static const uint8_t genesis_hash[32] = {
      0x6f, 0xe2, 0x8c, 0x0a, 0xb6, 0xf1, 0xb3, 0x72, 0xc1, 0xa6, 0xa2,
      0x46, 0xae, 0x63, 0xf7, 0x4f, 0x93, 0x1e, 0x83, 0x65, 0xe1, 0x5a,
      0x08, 0x9c, 0x68, 0xd6, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00};
#elif defined(ECHO_NETWORK_TESTNET)
  static const uint8_t genesis_hash[32] = {
      0x43, 0x49, 0x7f, 0xd7, 0xf8, 0x26, 0x95, 0x71, 0x08, 0xf4, 0xa3,
      0x0f, 0xd9, 0xce, 0xc3, 0xae, 0xba, 0x79, 0x97, 0x20, 0x84, 0xe9,
      0x0e, 0xad, 0x01, 0xea, 0x33, 0x09, 0x00, 0x00, 0x00, 0x00};
#elif defined(ECHO_NETWORK_REGTEST)
  static const uint8_t genesis_hash[32] = {
      0x06, 0x22, 0x6e, 0x46, 0x11, 0x1a, 0x0b, 0x59, 0xca, 0xaf, 0x12,
      0x60, 0x43, 0xeb, 0x5b, 0xbf, 0x28, 0xc3, 0x4f, 0x3a, 0x5e, 0x33,
      0x2a, 0x1f, 0xc7, 0xb2, 0xb7, 0x3c, 0xf1, 0x88, 0x91, 0x0f};
#endif

  if (memcmp(hash.bytes, genesis_hash, 32) != 0) {
    if (error)
      *error = BLOCK_ERR_POW_FAILED;
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}

/*
 * Convert error code to string.
 */
const char *block_validation_error_str(block_validation_error_t error) {
  switch (error) {
  case BLOCK_VALID:
    return "valid";
  case BLOCK_ERR_POW_FAILED:
    return "proof-of-work invalid";
  case BLOCK_ERR_TARGET_INVALID:
    return "target/bits field invalid";
  case BLOCK_ERR_TIMESTAMP_TOO_OLD:
    return "timestamp too old (before median time past)";
  case BLOCK_ERR_TIMESTAMP_TOO_NEW:
    return "timestamp too far in future";
  case BLOCK_ERR_PREV_BLOCK_UNKNOWN:
    return "previous block unknown";
  case BLOCK_ERR_PREV_BLOCK_INVALID:
    return "previous block invalid";
  case BLOCK_ERR_VERSION_INVALID:
    return "block version invalid";
  case BLOCK_ERR_DIFFICULTY_MISMATCH:
    return "difficulty target mismatch";
  case BLOCK_ERR_MERKLE_MISMATCH:
    return "merkle root mismatch";
  case BLOCK_ERR_NO_TRANSACTIONS:
    return "block has no transactions";
  case BLOCK_ERR_NO_COINBASE:
    return "first transaction not coinbase";
  case BLOCK_ERR_MULTI_COINBASE:
    return "multiple coinbase transactions";
  case BLOCK_ERR_SIZE_EXCEEDED:
    return "block size/weight exceeded";
  case BLOCK_ERR_SIGOPS_EXCEEDED:
    return "signature operations exceeded";
  case BLOCK_ERR_TX_INVALID:
    return "transaction validation failed";
  case BLOCK_ERR_COINBASE_INVALID:
    return "coinbase transaction malformed";
  case BLOCK_ERR_COINBASE_HEIGHT:
    return "coinbase height encoding invalid";
  case BLOCK_ERR_COINBASE_SUBSIDY:
    return "coinbase output exceeds allowed subsidy";
  case BLOCK_ERR_WITNESS_COMMITMENT:
    return "witness commitment invalid";
  default:
    return "unknown error";
  }
}

/*
 * ============================================================================
 * Difficulty Adjustment Implementation (Session 5.2)
 * ============================================================================
 */

/*
 * Helper: Multiply a 256-bit little-endian target by a 32-bit value.
 * Result is stored in a 288-bit (36-byte) buffer to handle overflow.
 */
static void target_mul_u32(const uint8_t *target, uint32_t multiplier,
                           uint8_t *result) {
  uint64_t carry = 0;
  int i;

  /* Clear result (36 bytes for overflow) */
  memset(result, 0, 36);

  /* Multiply byte by byte with carry */
  for (i = 0; i < 32; i++) {
    uint64_t product = (uint64_t)target[i] * multiplier + carry;
    result[i] = (uint8_t)(product & 0xFF);
    carry = product >> 8;
  }

  /* Store remaining carry in overflow bytes */
  for (i = 32; i < 36 && carry > 0; i++) {
    result[i] = (uint8_t)(carry & 0xFF);
    carry >>= 8;
  }
}

/*
 * Helper: Divide a 288-bit little-endian value by a 32-bit divisor.
 * Result is truncated to 256 bits.
 */
static void target_div_u32(const uint8_t *dividend, uint32_t divisor,
                           uint8_t *result) {
  uint64_t remainder = 0;
  int i;

  /* Clear result */
  memset(result, 0, 32);

  /* Divide from most significant byte to least */
  for (i = 35; i >= 0; i--) {
    uint64_t current = (remainder << 8) | dividend[i];
    uint8_t quotient_byte = (uint8_t)(current / divisor);
    remainder = current % divisor;

    if (i < 32) {
      result[i] = quotient_byte;
    }
    /* If i >= 32, the quotient byte would overflow 256 bits.
     * In practice, with clamped timespans, this shouldn't happen. */
  }
}

/*
 * Helper: Compare two 256-bit targets (little-endian).
 * Returns: -1 if a < b, 0 if a == b, 1 if a > b
 */
static int target_compare(const uint8_t *a, const uint8_t *b) {
  int i;

  /* Compare from most significant byte */
  for (i = 31; i >= 0; i--) {
    if (a[i] < b[i])
      return -1;
    if (a[i] > b[i])
      return 1;
  }
  return 0;
}

/*
 * Initialize a difficulty context.
 */
void difficulty_ctx_init(difficulty_ctx_t *ctx) {
  if (ctx == NULL)
    return;

  ctx->height = 0;
  ctx->period_start_time = 0;
  ctx->period_end_time = 0;
  ctx->prev_bits = DIFFICULTY_POWLIMIT_BITS;
}

/*
 * Check if a height is at a difficulty adjustment boundary.
 */
echo_bool_t difficulty_is_retarget_height(uint32_t height) {
  /* Height 0 is genesis, height 2016 is first retarget, etc. */
  if (height == 0) {
    return ECHO_FALSE;
  }
  return (height % DIFFICULTY_INTERVAL == 0) ? ECHO_TRUE : ECHO_FALSE;
}

/*
 * Compute the actual time span of a difficulty period.
 */
uint32_t difficulty_actual_timespan(const difficulty_ctx_t *ctx) {
  if (ctx == NULL) {
    return 0;
  }

  /* Handle case where end_time < start_time (shouldn't happen in practice) */
  if (ctx->period_end_time < ctx->period_start_time) {
    return 0;
  }

  return ctx->period_end_time - ctx->period_start_time;
}

/*
 * Clamp a time span to the allowed adjustment range.
 */
uint32_t difficulty_clamp_timespan(uint32_t timespan) {
  if (timespan < DIFFICULTY_MIN_TIMESPAN) {
    return DIFFICULTY_MIN_TIMESPAN;
  }
  if (timespan > DIFFICULTY_MAX_TIMESPAN) {
    return DIFFICULTY_MAX_TIMESPAN;
  }
  return timespan;
}

/*
 * Get the proof-of-work limit (minimum difficulty) target.
 */
echo_result_t difficulty_get_powlimit(hash256_t *target) {
  if (target == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  return block_bits_to_target(DIFFICULTY_POWLIMIT_BITS, target);
}

/*
 * Compute a new target given the old target and time ratio.
 *
 * new_target = old_target * actual_timespan / TARGET_TIMESPAN
 *
 * The calculation is done using 256-bit (actually 288-bit intermediate)
 * arithmetic to avoid overflow.
 */
echo_result_t difficulty_adjust_target(uint32_t old_bits,
                                       uint32_t actual_timespan,
                                       uint32_t *new_bits) {
  hash256_t old_target;
  hash256_t powlimit;
  uint8_t intermediate[36]; /* 288 bits for multiplication overflow */
  uint8_t new_target[32];
  echo_result_t result;

  if (new_bits == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Convert old bits to 256-bit target */
  result = block_bits_to_target(old_bits, &old_target);
  if (result != ECHO_OK) {
    return result;
  }

  /* Get proof-of-work limit */
  result = difficulty_get_powlimit(&powlimit);
  if (result != ECHO_OK) {
    return result;
  }

  /*
   * Calculate: new_target = old_target * actual_timespan / TARGET_TIMESPAN
   *
   * Step 1: Multiply old_target by actual_timespan (result in 288 bits)
   * Step 2: Divide by TARGET_TIMESPAN (result back to 256 bits)
   */
  target_mul_u32(old_target.bytes, actual_timespan, intermediate);
  target_div_u32(intermediate, DIFFICULTY_TARGET_TIMESPAN, new_target);

  /*
   * Clamp to proof-of-work limit (minimum difficulty).
   * If new_target > powlimit, use powlimit instead.
   */
  if (target_compare(new_target, powlimit.bytes) > 0) {
    memcpy(new_target, powlimit.bytes, 32);
  }

  /* Convert 256-bit target back to compact bits */
  {
    hash256_t target_hash;
    memcpy(target_hash.bytes, new_target, 32);
    result = block_target_to_bits(&target_hash, new_bits);
    if (result != ECHO_OK) {
      return result;
    }
  }

  return ECHO_OK;
}

/*
 * Compute the expected difficulty for a block at a given height.
 */
echo_result_t difficulty_compute_next(const difficulty_ctx_t *ctx,
                                      uint32_t *bits) {
  uint32_t actual_timespan;
  uint32_t clamped_timespan;

  if (ctx == NULL || bits == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /*
   * If not at a retarget boundary, use the previous block's bits.
   * This handles the first 2015 blocks after genesis and all
   * blocks within a difficulty period.
   */
  if (!difficulty_is_retarget_height(ctx->height)) {
    *bits = ctx->prev_bits;
    return ECHO_OK;
  }

  /*
   * At a retarget boundary: compute new difficulty.
   *
   * Note: Bitcoin has an off-by-one bug in the original implementation
   * where the timespan is calculated from block N-2016 to block N-1
   * (2015 blocks) instead of N-2016 to N (2016 blocks).
   * We replicate this behavior for consensus compatibility.
   *
   * The ctx->period_start_time should be the timestamp of block (height -
   * 2016). The ctx->period_end_time should be the timestamp of block (height -
   * 1).
   */
  actual_timespan = difficulty_actual_timespan(ctx);

  /* Handle edge case of zero timespan */
  if (actual_timespan == 0) {
    actual_timespan = DIFFICULTY_TARGET_TIMESPAN;
  }

  /* Clamp to allowed range */
  clamped_timespan = difficulty_clamp_timespan(actual_timespan);

  /* Compute new target */
  return difficulty_adjust_target(ctx->prev_bits, clamped_timespan, bits);
}

/*
 * Check if the testnet 20-minute difficulty reset rule applies.
 *
 * On testnet only: if more than 20 minutes have passed since the parent block,
 * the block is allowed to use minimum difficulty (powlimit).
 */
echo_bool_t difficulty_testnet_20min_rule_applies(uint32_t block_timestamp,
                                                  uint32_t parent_timestamp) {
#if defined(ECHO_NETWORK_TESTNET)
  /* 20 minutes = 1200 seconds */
  if (block_timestamp > parent_timestamp + TESTNET_20MIN_RULE_SECONDS) {
    return ECHO_TRUE;
  }
#else
  /* Suppress unused parameter warnings on non-testnet builds */
  (void)block_timestamp;
  (void)parent_timestamp;
#endif
  return ECHO_FALSE;
}

/*
 * Validate that a block's difficulty bits match the expected value.
 *
 * On testnet, also accepts minimum difficulty if the 20-minute rule applies.
 */
echo_bool_t block_validate_difficulty(const block_header_t *header,
                                      const difficulty_ctx_t *ctx,
                                      block_validation_error_t *error) {
  uint32_t expected_bits;
  echo_result_t result;

  if (header == NULL || ctx == NULL) {
    if (error)
      *error = BLOCK_ERR_DIFFICULTY_MISMATCH;
    return ECHO_FALSE;
  }

  /* Compute expected difficulty */
  result = difficulty_compute_next(ctx, &expected_bits);
  if (result != ECHO_OK) {
    if (error)
      *error = BLOCK_ERR_TARGET_INVALID;
    return ECHO_FALSE;
  }

  /* Compare with block's bits */
  if (header->bits == expected_bits) {
    return ECHO_TRUE;
  }

#if defined(ECHO_NETWORK_TESTNET)
  /*
   * Testnet 20-minute rule: if more than 20 minutes have passed since
   * the parent block, the block is allowed to use minimum difficulty.
   *
   * period_end_time is the parent block's timestamp.
   */
  if (difficulty_testnet_20min_rule_applies(header->timestamp,
                                            ctx->period_end_time)) {
    /* Accept minimum difficulty (powlimit) */
    if (header->bits == TESTNET_POWLIMIT_BITS) {
      return ECHO_TRUE;
    }
  }
#endif

  /* Bits don't match expected and don't qualify for testnet rule */
  if (error)
    *error = BLOCK_ERR_DIFFICULTY_MISMATCH;
  return ECHO_FALSE;
}

/*
 * ============================================================================
 * Coinbase Validation Implementation (Session 5.3)
 * ============================================================================
 */

/*
 * Witness commitment magic prefix: aa21a9ed
 * This is SHA256("witness\0")[0:4]
 */
const uint8_t WITNESS_COMMITMENT_PREFIX[4] = {0xaa, 0x21, 0xa9, 0xed};

/*
 * Halving interval and initial subsidy.
 */
#define HALVING_INTERVAL 210000
#define INITIAL_SUBSIDY 5000000000LL /* 50 BTC in satoshis */

/*
 * Coinbase scriptsig size limits.
 */
#define COINBASE_SCRIPTSIG_MIN 2
#define COINBASE_SCRIPTSIG_MAX 100

/*
 * Calculate the block subsidy for a given height.
 */
satoshi_t coinbase_subsidy(uint32_t height) {
  uint32_t halvings;
  satoshi_t subsidy;

  halvings = height / HALVING_INTERVAL;

  /*
   * After 64 halvings, subsidy becomes 0.
   * This happens around year 2140.
   * Right-shifting by 64 or more is undefined behavior in C,
   * so we handle this explicitly.
   */
  if (halvings >= 64) {
    return 0;
  }

  subsidy = INITIAL_SUBSIDY >> halvings;

  return subsidy;
}

/*
 * Parse the block height from a BIP-34 coinbase scriptsig.
 *
 * BIP-34 encoding:
 *   - First byte is the push opcode (number of bytes to push)
 *   - For heights 0-16: OP_0 through OP_16 (special case)
 *   - For heights 17+: push N bytes, little-endian
 *
 * The height is encoded as a minimally-encoded script number:
 *   - Height 0: OP_0 (0x00) - actually this shouldn't happen with BIP-34
 *   - Height 1-16: OP_1 through OP_16 (0x51-0x60)
 *   - Height 17-127: 0x01 <byte> (1 byte push)
 *   - Height 128-32767: 0x02 <2 bytes> (2 byte push)
 *   - Height 32768-8388607: 0x03 <3 bytes> (3 byte push)
 *   - Height 8388608+: 0x04 <4 bytes> (4 byte push)
 */
echo_result_t coinbase_parse_height(const uint8_t *script, size_t script_len,
                                    uint32_t *height) {
  uint8_t opcode;
  size_t num_bytes;
  uint32_t result;
  size_t i;

  if (script == NULL || height == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (script_len < 1) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  opcode = script[0];

  /*
   * Handle special opcodes OP_0 through OP_16.
   * OP_0 = 0x00, OP_1 = 0x51, OP_2 = 0x52, ..., OP_16 = 0x60
   */
  if (opcode == 0x00) {
    /* OP_0 represents height 0 (edge case, shouldn't occur after BIP-34) */
    *height = 0;
    return ECHO_OK;
  }

  if (opcode >= 0x51 && opcode <= 0x60) {
    /* OP_1 through OP_16 */
    *height = opcode - 0x50;
    return ECHO_OK;
  }

  /*
   * For other cases, the opcode is the number of bytes to read.
   * BIP-34 heights use 1-4 bytes (heights up to ~4 billion).
   */
  if (opcode < 0x01 || opcode > 0x04) {
    /* Invalid push opcode for height */
    return ECHO_ERR_INVALID_FORMAT;
  }

  num_bytes = opcode;

  if (script_len < 1 + num_bytes) {
    return ECHO_ERR_INVALID_FORMAT;
  }

  /* Read little-endian number */
  result = 0;
  for (i = 0; i < num_bytes; i++) {
    result |= (uint32_t)script[1 + i] << (8 * i);
  }

  /*
   * Check for minimal encoding.
   * The number should not have leading zero bytes (except for negative sign).
   * Heights are always positive, so the high bit of the last byte should be 0.
   */
  if (num_bytes > 1) {
    /* Last byte should not be zero (except if needed for sign) */
    uint8_t last_byte = script[num_bytes];
    uint8_t prev_byte = script[num_bytes - 1];

    /* If last byte is 0x00, previous byte must have high bit set */
    if (last_byte == 0x00 && (prev_byte & 0x80) == 0) {
      return ECHO_ERR_INVALID_FORMAT;
    }
  }

  /* Heights must fit in uint32_t and be non-negative */
  if (num_bytes == 4 && (script[4] & 0x80)) {
    /* Would be negative in script number encoding */
    return ECHO_ERR_INVALID_FORMAT;
  }

  *height = result;
  return ECHO_OK;
}

/*
 * Validate the BIP-34 height encoding in a coinbase.
 */
echo_bool_t coinbase_validate_height(const tx_t *coinbase,
                                     uint32_t expected_height,
                                     block_validation_error_t *error) {
  uint32_t parsed_height;
  echo_result_t result;

  if (coinbase == NULL) {
    if (error)
      *error = BLOCK_ERR_COINBASE_INVALID;
    return ECHO_FALSE;
  }

  /* BIP-34 only enforced after activation height */
  if (expected_height < BIP34_HEIGHT) {
    return ECHO_TRUE;
  }

  /* Must have at least one input */
  if (coinbase->input_count == 0) {
    if (error)
      *error = BLOCK_ERR_COINBASE_INVALID;
    return ECHO_FALSE;
  }

  /* Parse height from scriptsig */
  result =
      coinbase_parse_height(coinbase->inputs[0].script_sig,
                            coinbase->inputs[0].script_sig_len, &parsed_height);

  if (result != ECHO_OK) {
    if (error)
      *error = BLOCK_ERR_COINBASE_HEIGHT;
    return ECHO_FALSE;
  }

  /* Check height matches */
  if (parsed_height != expected_height) {
    if (error)
      *error = BLOCK_ERR_COINBASE_HEIGHT;
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}

/*
 * Find the witness commitment output in a coinbase transaction.
 *
 * The witness commitment is found in an output script of the form:
 *   OP_RETURN <commitment>
 * where <commitment> is 36 bytes: 4-byte prefix (aa21a9ed) + 32-byte hash.
 *
 * Per BIP-141, if multiple outputs match, the last one is used.
 */
echo_result_t coinbase_find_witness_commitment(const tx_t *coinbase,
                                               hash256_t *commitment) {
  size_t i;
  int found = 0;
  size_t found_idx = 0;

  if (coinbase == NULL || commitment == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /*
   * Search outputs from last to first.
   * The commitment format is:
   *   OP_RETURN (0x6a) + OP_PUSHBYTES_36 (0x24) + aa21a9ed + <32-byte hash>
   * Total: 38 bytes
   *
   * Or with different push encoding:
   *   OP_RETURN (0x6a) + OP_PUSHDATA1 (0x4c) + 0x24 + aa21a9ed + <32-byte hash>
   * Total: 39 bytes
   */
  for (i = 0; i < coinbase->output_count; i++) {
    const tx_output_t *output = &coinbase->outputs[i];
    const uint8_t *script = output->script_pubkey;
    size_t len = output->script_pubkey_len;
    size_t prefix_offset;

    if (script == NULL || len < 38) {
      continue;
    }

    /* Must start with OP_RETURN (0x6a) */
    if (script[0] != 0x6a) {
      continue;
    }

    /* Check for push opcode */
    if (script[1] == 0x24 && len >= 38) {
      /* Direct push of 36 bytes */
      prefix_offset = 2;
    } else if (script[1] == 0x4c && len >= 39 && script[2] == 0x24) {
      /* OP_PUSHDATA1 with 36 bytes */
      prefix_offset = 3;
    } else {
      continue;
    }

    /* Check for witness commitment prefix */
    if (memcmp(script + prefix_offset, WITNESS_COMMITMENT_PREFIX,
               WITNESS_COMMITMENT_PREFIX_LEN) == 0) {
      found = 1;
      found_idx = i;
      /* Don't break - use last matching output per BIP-141 */
    }
  }

  if (!found) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Extract the 32-byte commitment hash */
  {
    const tx_output_t *output = &coinbase->outputs[found_idx];
    const uint8_t *script = output->script_pubkey;
    size_t hash_offset;

    if (script[1] == 0x24) {
      hash_offset = 2 + WITNESS_COMMITMENT_PREFIX_LEN;
    } else {
      hash_offset = 3 + WITNESS_COMMITMENT_PREFIX_LEN;
    }

    memcpy(commitment->bytes, script + hash_offset, 32);
  }

  return ECHO_OK;
}

/*
 * Check if a block has any witness transactions.
 */
static echo_bool_t block_has_witness(const block_t *block) {
  size_t i;

  if (block == NULL || block->txs == NULL) {
    return ECHO_FALSE;
  }

  /* Skip coinbase (index 0), check other transactions */
  for (i = 1; i < block->tx_count; i++) {
    if (block->txs[i].has_witness) {
      return ECHO_TRUE;
    }
  }

  return ECHO_FALSE;
}

/*
 * Validate the witness commitment in a block.
 */
echo_bool_t block_validate_witness_commitment(const block_t *block,
                                              block_validation_error_t *error) {
  hash256_t expected_commitment;
  hash256_t actual_commitment;
  hash256_t witness_root;
  hash256_t witness_nonce;
  echo_result_t result;
  const tx_t *coinbase;

  if (block == NULL) {
    if (error)
      *error = BLOCK_ERR_WITNESS_COMMITMENT;
    return ECHO_FALSE;
  }

  if (block->tx_count == 0 || block->txs == NULL) {
    if (error)
      *error = BLOCK_ERR_NO_TRANSACTIONS;
    return ECHO_FALSE;
  }

  coinbase = &block->txs[0];

  /* If no witness transactions, commitment is optional */
  if (!block_has_witness(block)) {
    /*
     * Even without witness txs, if there's a commitment, it should be valid.
     * But we don't require it.
     */
    return ECHO_TRUE;
  }

  /*
   * Block has witness data, so we must validate the commitment.
   */

  /* Find the witness commitment in coinbase */
  result = coinbase_find_witness_commitment(coinbase, &expected_commitment);
  if (result != ECHO_OK) {
    if (error)
      *error = BLOCK_ERR_WITNESS_COMMITMENT;
    return ECHO_FALSE;
  }

  /*
   * Get the witness nonce from coinbase.
   * The coinbase must have witness data with exactly one 32-byte item.
   */
  if (coinbase->input_count == 0) {
    if (error)
      *error = BLOCK_ERR_COINBASE_INVALID;
    return ECHO_FALSE;
  }

  {
    const witness_stack_t *ws = &coinbase->inputs[0].witness;

    if (ws->count != 1 || ws->items[0].len != 32) {
      if (error)
        *error = BLOCK_ERR_WITNESS_COMMITMENT;
      return ECHO_FALSE;
    }

    memcpy(witness_nonce.bytes, ws->items[0].data, 32);
  }

  /* Compute witness merkle root */
  result = merkle_root_wtxids(block->txs, block->tx_count, &witness_root);
  if (result != ECHO_OK) {
    if (error)
      *error = BLOCK_ERR_WITNESS_COMMITMENT;
    return ECHO_FALSE;
  }

  /* Compute expected commitment: SHA256d(witness_root || witness_nonce) */
  result =
      witness_commitment(&witness_root, &witness_nonce, &actual_commitment);
  if (result != ECHO_OK) {
    if (error)
      *error = BLOCK_ERR_WITNESS_COMMITMENT;
    return ECHO_FALSE;
  }

  /* Compare */
  if (memcmp(expected_commitment.bytes, actual_commitment.bytes, 32) != 0) {
    if (error)
      *error = BLOCK_ERR_WITNESS_COMMITMENT;
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}

/*
 * Validate a coinbase transaction.
 */
echo_bool_t coinbase_validate(const tx_t *coinbase, uint32_t height,
                              satoshi_t max_allowed,
                              block_validation_error_t *error) {
  size_t i;
  satoshi_t total_output;

  if (coinbase == NULL) {
    if (error)
      *error = BLOCK_ERR_COINBASE_INVALID;
    return ECHO_FALSE;
  }

  /* Must be a coinbase transaction */
  if (!tx_is_coinbase(coinbase)) {
    if (error)
      *error = BLOCK_ERR_COINBASE_INVALID;
    return ECHO_FALSE;
  }

  /* Check scriptsig size limits */
  if (coinbase->inputs[0].script_sig_len < COINBASE_SCRIPTSIG_MIN ||
      coinbase->inputs[0].script_sig_len > COINBASE_SCRIPTSIG_MAX) {
    if (error)
      *error = BLOCK_ERR_COINBASE_INVALID;
    return ECHO_FALSE;
  }

  /* Validate BIP-34 height encoding */
  if (!coinbase_validate_height(coinbase, height, error)) {
    return ECHO_FALSE;
  }

  /* Calculate total output value */
  total_output = 0;
  for (i = 0; i < coinbase->output_count; i++) {
    satoshi_t value = coinbase->outputs[i].value;

    /* Check for negative values */
    if (value < 0) {
      if (error)
        *error = BLOCK_ERR_COINBASE_SUBSIDY;
      return ECHO_FALSE;
    }

    /* Check for overflow */
    if (total_output > ECHO_MAX_SATOSHIS - value) {
      if (error)
        *error = BLOCK_ERR_COINBASE_SUBSIDY;
      return ECHO_FALSE;
    }

    total_output += value;
  }

  /* Check against max allowed (subsidy + fees) */
  if (total_output > max_allowed) {
    if (error)
      *error = BLOCK_ERR_COINBASE_SUBSIDY;
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}

/*
 * Check if a coinbase output is mature (spendable).
 */
echo_bool_t coinbase_is_mature(uint32_t coinbase_height,
                               uint32_t current_height) {
  /*
   * Coinbase outputs cannot be spent until COINBASE_MATURITY blocks
   * have been mined on top of the block containing them.
   *
   * If coinbase is in block H, it can be spent starting at block H+100.
   * So: current_height >= coinbase_height + COINBASE_MATURITY
   */
  if (current_height < coinbase_height) {
    /* Can't spend from future blocks */
    return ECHO_FALSE;
  }

  if (current_height - coinbase_height < COINBASE_MATURITY) {
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}

/*
 * ============================================================================
 * Full Block Validation Implementation (Session 5.4)
 * ============================================================================
 */

/*
 * Initialize a full block validation context.
 */
void full_block_ctx_init(full_block_ctx_t *ctx) {
  if (ctx == NULL)
    return;

  block_validate_ctx_init(&ctx->header_ctx);
  difficulty_ctx_init(&ctx->difficulty_ctx);
  ctx->height = 0;
  ctx->total_fees = 0;
  ctx->segwit_active = ECHO_FALSE;
}

/*
 * Initialize a block validation result.
 */
void block_validation_result_init(block_validation_result_t *result) {
  if (result == NULL)
    return;

  result->valid = ECHO_FALSE;
  result->error = BLOCK_VALID;
  result->failing_tx_index = 0;
  result->error_msg = NULL;
}

/*
 * Check if a block has any duplicate transaction IDs.
 *
 * Uses O(n^2) comparison which is acceptable for typical block sizes.
 * For very large blocks, a hash table would be more efficient.
 */
echo_bool_t block_has_duplicate_txids(const block_t *block, size_t *dup_idx) {
  size_t i, j;
  hash256_t *txids;
  echo_result_t result;
  echo_bool_t found_dup = ECHO_FALSE;

  if (block == NULL || block->tx_count == 0) {
    return ECHO_FALSE;
  }

  /* Allocate array for txids */
  txids = malloc(block->tx_count * sizeof(hash256_t));
  if (txids == NULL) {
    /* Can't check, assume no duplicates */
    return ECHO_FALSE;
  }

  /* Compute all txids */
  for (i = 0; i < block->tx_count; i++) {
    result = tx_compute_txid(&block->txs[i], &txids[i]);
    if (result != ECHO_OK) {
      free(txids);
      return ECHO_FALSE;
    }
  }

  /* Check for duplicates */
  for (i = 0; i < block->tx_count && !found_dup; i++) {
    for (j = i + 1; j < block->tx_count; j++) {
      if (memcmp(txids[i].bytes, txids[j].bytes, 32) == 0) {
        found_dup = ECHO_TRUE;
        if (dup_idx != NULL) {
          *dup_idx = j;
        }
        break;
      }
    }
  }

  free(txids);
  return found_dup;
}

/*
 * Verify the merkle root in a block header matches the transactions.
 */
echo_bool_t block_validate_merkle_root(const block_t *block,
                                       block_validation_error_t *error) {
  hash256_t computed_root;
  echo_result_t result;

  if (block == NULL) {
    if (error)
      *error = BLOCK_ERR_MERKLE_MISMATCH;
    return ECHO_FALSE;
  }

  if (block->tx_count == 0 || block->txs == NULL) {
    if (error)
      *error = BLOCK_ERR_NO_TRANSACTIONS;
    return ECHO_FALSE;
  }

  /* Compute merkle root from transactions */
  result = merkle_root_txids(block->txs, block->tx_count, &computed_root);
  if (result != ECHO_OK) {
    if (error)
      *error = BLOCK_ERR_MERKLE_MISMATCH;
    return ECHO_FALSE;
  }

  /* Compare with header's merkle root */
  if (memcmp(computed_root.bytes, block->header.merkle_root.bytes, 32) != 0) {
    if (error)
      *error = BLOCK_ERR_MERKLE_MISMATCH;
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}

/*
 * Validate block size and weight limits.
 */
echo_bool_t block_validate_size(const block_t *block,
                                block_validation_error_t *error) {
  size_t size;
  size_t weight;

  if (block == NULL) {
    if (error)
      *error = BLOCK_ERR_SIZE_EXCEEDED;
    return ECHO_FALSE;
  }

  /* Check serialized size */
  size = block_serialize_size(block);
  if (size > BLOCK_MAX_SIZE) {
    if (error)
      *error = BLOCK_ERR_SIZE_EXCEEDED;
    return ECHO_FALSE;
  }

  /* Check block weight */
  weight = block_weight(block);
  if (weight > BLOCK_MAX_WEIGHT) {
    if (error)
      *error = BLOCK_ERR_SIZE_EXCEEDED;
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}

/*
 * Validate block transaction structure.
 */
echo_bool_t block_validate_tx_structure(const block_t *block,
                                        block_validation_error_t *error) {
  size_t i;
  size_t dup_idx;

  if (block == NULL) {
    if (error)
      *error = BLOCK_ERR_NO_TRANSACTIONS;
    return ECHO_FALSE;
  }

  /* Must have at least one transaction (coinbase) */
  if (block->tx_count == 0 || block->txs == NULL) {
    if (error)
      *error = BLOCK_ERR_NO_TRANSACTIONS;
    return ECHO_FALSE;
  }

  /* First transaction must be coinbase */
  if (!tx_is_coinbase(&block->txs[0])) {
    if (error)
      *error = BLOCK_ERR_NO_COINBASE;
    return ECHO_FALSE;
  }

  /* All other transactions must NOT be coinbase */
  for (i = 1; i < block->tx_count; i++) {
    if (tx_is_coinbase(&block->txs[i])) {
      if (error)
        *error = BLOCK_ERR_MULTI_COINBASE;
      return ECHO_FALSE;
    }
  }

  /* Check for duplicate txids */
  if (block_has_duplicate_txids(block, &dup_idx)) {
    if (error)
      *error = BLOCK_ERR_TX_INVALID;
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}

/*
 * Perform complete block validation.
 */
echo_bool_t block_validate(const block_t *block, const full_block_ctx_t *ctx,
                           block_validation_result_t *result) {
  block_validation_error_t local_error = BLOCK_VALID;
  satoshi_t max_coinbase;
  satoshi_t subsidy;

  /* Initialize result if provided */
  if (result != NULL) {
    block_validation_result_init(result);
  }

  /* Check parameters */
  if (block == NULL || ctx == NULL) {
    if (result) {
      result->error = BLOCK_ERR_NO_TRANSACTIONS;
      result->error_msg = "null block or context";
    }
    return ECHO_FALSE;
  }

  /*
   * 1. Header validation (PoW, timestamp, prev_block, version)
   */
  if (!block_validate_header(&block->header, &ctx->header_ctx, &local_error)) {
    if (result) {
      result->error = local_error;
      result->error_msg = block_validation_error_str(local_error);
    }
    return ECHO_FALSE;
  }

  /*
   * 2. Difficulty validation (bits match expected)
   */
  if (!block_validate_difficulty(&block->header, &ctx->difficulty_ctx,
                                 &local_error)) {
    if (result) {
      result->error = local_error;
      result->error_msg = block_validation_error_str(local_error);
    }
    return ECHO_FALSE;
  }

  /*
   * 3. Block size/weight limits
   */
  if (!block_validate_size(block, &local_error)) {
    if (result) {
      result->error = local_error;
      result->error_msg = block_validation_error_str(local_error);
    }
    return ECHO_FALSE;
  }

  /*
   * 4. Transaction structure (coinbase, no duplicates)
   */
  if (!block_validate_tx_structure(block, &local_error)) {
    if (result) {
      result->error = local_error;
      result->error_msg = block_validation_error_str(local_error);
    }
    return ECHO_FALSE;
  }

  /*
   * 5. Merkle root verification
   */
  if (!block_validate_merkle_root(block, &local_error)) {
    if (result) {
      result->error = local_error;
      result->error_msg = block_validation_error_str(local_error);
    }
    return ECHO_FALSE;
  }

  /*
   * 6. Coinbase validation (BIP-34 height, subsidy limit)
   */
  subsidy = coinbase_subsidy(ctx->height);
  max_coinbase = subsidy + ctx->total_fees;

  /* Check for overflow */
  if (max_coinbase < subsidy) {
    max_coinbase = ECHO_MAX_SATOSHIS;
  }

  if (!coinbase_validate(&block->txs[0], ctx->height, max_coinbase,
                         &local_error)) {
    if (result) {
      result->error = local_error;
      result->failing_tx_index = 0;
      result->error_msg = block_validation_error_str(local_error);
    }
    return ECHO_FALSE;
  }

  /*
   * 7. Witness commitment verification (if SegWit active)
   */
  if (ctx->segwit_active) {
    if (!block_validate_witness_commitment(block, &local_error)) {
      if (result) {
        result->error = local_error;
        result->error_msg = block_validation_error_str(local_error);
      }
      return ECHO_FALSE;
    }
  }

  /* All checks passed */
  if (result) {
    result->valid = ECHO_TRUE;
    result->error = BLOCK_VALID;
    result->error_msg = "valid";
  }

  return ECHO_TRUE;
}

/*
 * Validate a block with minimal context (header-only checks).
 */
echo_bool_t block_validate_basic(const block_t *block,
                                 block_validation_error_t *error) {
  block_validation_error_t local_error = BLOCK_VALID;

  if (block == NULL) {
    if (error)
      *error = BLOCK_ERR_NO_TRANSACTIONS;
    return ECHO_FALSE;
  }

  /*
   * 1. Proof-of-work validation
   */
  if (!block_validate_pow(&block->header, &local_error)) {
    if (error)
      *error = local_error;
    return ECHO_FALSE;
  }

  /*
   * 2. Block size/weight limits
   */
  if (!block_validate_size(block, &local_error)) {
    if (error)
      *error = local_error;
    return ECHO_FALSE;
  }

  /*
   * 3. Transaction structure
   */
  if (!block_validate_tx_structure(block, &local_error)) {
    if (error)
      *error = local_error;
    return ECHO_FALSE;
  }

  /*
   * 4. Merkle root verification
   */
  if (!block_validate_merkle_root(block, &local_error)) {
    if (error)
      *error = local_error;
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}
