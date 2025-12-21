/*
 * Bitcoin Echo — Block Header Validation
 *
 * This module implements block header validation rules as specified in
 * the Bitcoin protocol:
 *
 *   - Proof-of-work: block hash must be <= target derived from bits
 *   - Timestamp: must be > median of previous 11 blocks (MTP)
 *   - Timestamp: must be <= current time + 2 hours (MAX_FUTURE_TIME)
 *   - Previous block: must reference a valid block
 *   - Version: interpreted per BIP-9 version bits semantics
 *
 * These checks are performed without full block data—only the 80-byte
 * header and context about the chain tip are required.
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_BLOCK_VALIDATE_H
#define ECHO_BLOCK_VALIDATE_H

#include "block.h"
#include "echo_types.h"
#include "tx.h"
#include <stdint.h>

/*
 * Timestamp validation constants.
 */

/* Number of blocks used to compute median time past (MTP) */
#define BLOCK_MTP_WINDOW 11

/* Maximum time a block timestamp can be in the future (2 hours in seconds) */
#define BLOCK_MAX_FUTURE_TIME (2 * 60 * 60)

/*
 * BIP-9 version bits constants.
 */

/* Bits 29-31 must be 001 for BIP-9 version bits interpretation */
#define BLOCK_VERSION_TOP_MASK 0xE0000000
#define BLOCK_VERSION_TOP_BITS 0x20000000

/* Individual version bit positions (0-28 available, but only 0-28 used) */
#define BLOCK_VERSION_BIT_MASK 0x1FFFFFFF

/*
 * Difficulty adjustment constants.
 *
 * These match the values in echo_config.h but are duplicated here for
 * module self-containment. Both refer to the same immutable protocol values.
 */

/* Blocks between difficulty adjustments */
#define DIFFICULTY_INTERVAL 2016

/* Target time for a difficulty period (2 weeks in seconds) */
#define DIFFICULTY_TARGET_TIMESPAN 1209600

/* Target time per block (10 minutes in seconds) */
#define DIFFICULTY_TARGET_SPACING 600

/* Minimum and maximum adjustment factors (factor of 4) */
#define DIFFICULTY_MIN_TIMESPAN                                                \
  (DIFFICULTY_TARGET_TIMESPAN / 4) /* ~3.5 days                                \
                                    */
#define DIFFICULTY_MAX_TIMESPAN                                                \
  (DIFFICULTY_TARGET_TIMESPAN * 4) /* ~8 weeks                                 \
                                    */

/* Proof-of-work limit (minimum difficulty target) for mainnet */
/* This is the genesis block target: 0x00000000FFFF0000...0000 */
#define DIFFICULTY_POWLIMIT_BITS 0x1d00ffff

/*
 * Block validation error codes.
 * More specific than the general ECHO_ERR_* codes.
 */
typedef enum {
  BLOCK_VALID = 0,

  /* Header validation failures */
  BLOCK_ERR_POW_FAILED,         /* Hash does not meet target */
  BLOCK_ERR_TARGET_INVALID,     /* Bits field produces invalid target */
  BLOCK_ERR_TIMESTAMP_TOO_OLD,  /* Timestamp <= median time past */
  BLOCK_ERR_TIMESTAMP_TOO_NEW,  /* Timestamp > current time + 2 hours */
  BLOCK_ERR_PREV_BLOCK_UNKNOWN, /* Previous block hash not found */
  BLOCK_ERR_PREV_BLOCK_INVALID, /* Previous block is itself invalid */
  BLOCK_ERR_VERSION_INVALID,    /* Version field invalid for height */

  /* Used in later sessions */
  BLOCK_ERR_DIFFICULTY_MISMATCH, /* Bits don't match expected difficulty */
  BLOCK_ERR_MERKLE_MISMATCH,     /* Merkle root doesn't match txs */
  BLOCK_ERR_NO_TRANSACTIONS,     /* Block has no transactions */
  BLOCK_ERR_NO_COINBASE,         /* First tx is not coinbase */
  BLOCK_ERR_MULTI_COINBASE,      /* Multiple coinbase transactions */
  BLOCK_ERR_SIZE_EXCEEDED,       /* Block size/weight exceeded */
  BLOCK_ERR_SIGOPS_EXCEEDED,     /* Too many signature operations */
  BLOCK_ERR_TX_INVALID,          /* A transaction failed validation */

  /* Coinbase-specific errors (Session 5.3) */
  BLOCK_ERR_COINBASE_INVALID,   /* Coinbase transaction malformed */
  BLOCK_ERR_COINBASE_HEIGHT,    /* BIP-34 height encoding invalid/mismatch */
  BLOCK_ERR_COINBASE_SUBSIDY,   /* Coinbase output exceeds allowed subsidy */
  BLOCK_ERR_WITNESS_COMMITMENT, /* Witness commitment invalid/missing */

} block_validation_error_t;

/*
 * Block validation context.
 *
 * Provides the chain context needed to validate a block header.
 * This is an opaque interface—the actual chain state management
 * is implemented in later sessions.
 */
typedef struct {
  /* Height of the block being validated (parent height + 1) */
  uint32_t height;

  /* Timestamps of the previous 11 blocks (for MTP calculation).
   * timestamps[0] is the parent block, timestamps[10] is 10 blocks back.
   * If height < 11, only (height) entries are valid. */
  uint32_t timestamps[BLOCK_MTP_WINDOW];
  size_t timestamp_count; /* Number of valid entries (min(height, 11)) */

  /* Current network-adjusted time (Unix timestamp).
   * Used for the "not too far in future" check. */
  uint32_t current_time;

  /* Expected difficulty target (bits) for this height.
   * Set by difficulty adjustment algorithm. */
  uint32_t expected_bits;

  /* Parent block hash (for prev_hash validation) */
  hash256_t parent_hash;

  /* Whether the parent block is valid */
  echo_bool_t parent_valid;

} block_validation_ctx_t;

/*
 * Initialize a validation context to default/safe values.
 *
 * Parameters:
 *   ctx - Context to initialize
 */
void block_validate_ctx_init(block_validation_ctx_t *ctx);

/*
 * Compute the median time past (MTP) from context.
 *
 * The MTP is the median of the timestamps of the previous BLOCK_MTP_WINDOW
 * blocks. For blocks at height < 11, only available timestamps are used.
 *
 * Parameters:
 *   ctx - Validation context with timestamps populated
 *
 * Returns:
 *   Median timestamp, or 0 if ctx is NULL or has no timestamps
 */
uint32_t block_validate_mtp(const block_validation_ctx_t *ctx);

/*
 * Validate a block header's proof-of-work.
 *
 * Checks that the block hash (SHA256d of header) is less than or equal
 * to the target derived from the bits field.
 *
 * Parameters:
 *   header - Block header to validate
 *   error  - Output: specific error if validation fails (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if PoW is valid, ECHO_FALSE otherwise
 */
echo_bool_t block_validate_pow(const block_header_t *header,
                               block_validation_error_t *error);

/*
 * Validate proof-of-work with pre-computed hash.
 *
 * Same as block_validate_pow but uses a pre-computed hash to avoid
 * redundant SHA256d computation during header sync.
 *
 * Parameters:
 *   header - Block header to validate
 *   hash   - Pre-computed block hash (NULL to compute internally)
 *   error  - Output: specific error if validation fails (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if PoW is valid, ECHO_FALSE otherwise
 */
echo_bool_t block_validate_pow_with_hash(const block_header_t *header,
                                         const hash256_t *hash,
                                         block_validation_error_t *error);

/*
 * Validate a block header's timestamp.
 *
 * Checks:
 *   1. Timestamp > median time past (from ctx)
 *   2. Timestamp <= current_time + MAX_FUTURE_TIME (from ctx)
 *
 * Parameters:
 *   header - Block header to validate
 *   ctx    - Validation context with timestamps and current_time
 *   error  - Output: specific error if validation fails (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if timestamp is valid, ECHO_FALSE otherwise
 */
echo_bool_t block_validate_timestamp(const block_header_t *header,
                                     const block_validation_ctx_t *ctx,
                                     block_validation_error_t *error);

/*
 * Validate a block header's previous block reference.
 *
 * Checks:
 *   1. prev_hash matches ctx->parent_hash
 *   2. Parent block is valid (ctx->parent_valid)
 *
 * Parameters:
 *   header - Block header to validate
 *   ctx    - Validation context with parent information
 *   error  - Output: specific error if validation fails (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if prev_hash is valid, ECHO_FALSE otherwise
 */
echo_bool_t block_validate_prev_block(const block_header_t *header,
                                      const block_validation_ctx_t *ctx,
                                      block_validation_error_t *error);

/*
 * Check if a block version uses BIP-9 version bits.
 *
 * BIP-9 version bits are indicated when the top 3 bits (29-31) are 001.
 *
 * Parameters:
 *   version - Block version field
 *
 * Returns:
 *   ECHO_TRUE if version uses BIP-9 semantics, ECHO_FALSE otherwise
 */
echo_bool_t block_version_uses_bip9(int32_t version);

/*
 * Extract a specific version bit from a BIP-9 version.
 *
 * Parameters:
 *   version - Block version field
 *   bit     - Bit position (0-28)
 *
 * Returns:
 *   ECHO_TRUE if bit is set, ECHO_FALSE otherwise
 */
echo_bool_t block_version_bit(int32_t version, int bit);

/*
 * Validate a block header's version field.
 *
 * Currently performs minimal validation—versions 1-4 are always valid,
 * and BIP-9 versions (with top bits 001) are valid at any height.
 *
 * Note: BIP-9 deployment-specific version requirements (e.g., mandatory
 * signaling after lock-in) are checked separately during activation.
 *
 * Parameters:
 *   header - Block header to validate
 *   ctx    - Validation context with height
 *   error  - Output: specific error if validation fails (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if version is valid, ECHO_FALSE otherwise
 */
echo_bool_t block_validate_version(const block_header_t *header,
                                   const block_validation_ctx_t *ctx,
                                   block_validation_error_t *error);

/*
 * Perform all header validation checks.
 *
 * This is the main entry point for header validation. It performs:
 *   1. Proof-of-work validation
 *   2. Timestamp validation (MTP and future limit)
 *   3. Previous block validation
 *   4. Version validation
 *
 * Note: Difficulty target validation (bits == expected_bits) is NOT
 * performed here—that requires the difficulty adjustment algorithm
 * from Session 5.2.
 *
 * Parameters:
 *   header - Block header to validate
 *   ctx    - Validation context
 *   error  - Output: specific error if validation fails (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if header passes all checks, ECHO_FALSE otherwise
 */
echo_bool_t block_validate_header(const block_header_t *header,
                                  const block_validation_ctx_t *ctx,
                                  block_validation_error_t *error);

/*
 * Validate a genesis block header.
 *
 * The genesis block has special rules:
 *   - prev_hash must be all zeros
 *   - No MTP check (no previous blocks)
 *   - Specific values for mainnet/testnet/regtest
 *
 * Parameters:
 *   header - Block header to validate
 *   error  - Output: specific error if validation fails (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if this is a valid genesis block, ECHO_FALSE otherwise
 */
echo_bool_t block_validate_genesis(const block_header_t *header,
                                   block_validation_error_t *error);

/*
 * Convert a block validation error to a human-readable string.
 *
 * Parameters:
 *   error - Error code to convert
 *
 * Returns:
 *   Static string describing the error
 */
const char *block_validation_error_str(block_validation_error_t error);

/*
 * ============================================================================
 * Difficulty Adjustment (Session 5.2)
 * ============================================================================
 */

/*
 * Context for difficulty adjustment calculation.
 *
 * This structure holds the information needed to compute the expected
 * difficulty for a block at a given height.
 */
typedef struct {
  /* Height of the block we're computing difficulty for */
  uint32_t height;

  /* Timestamp of the first block in the current difficulty period.
   * This is the block at height (height - (height % 2016)).
   * For the first period (heights 0-2015), this is the genesis timestamp. */
  uint32_t period_start_time;

  /* Timestamp of the last block before this one (parent block).
   * Used to compute actual time span of the period. */
  uint32_t period_end_time;

  /* The difficulty bits of the previous period.
   * For heights 0-2015, this is the genesis difficulty.
   * For subsequent periods, this is the bits from the last retarget block. */
  uint32_t prev_bits;

} difficulty_ctx_t;

/*
 * Initialize a difficulty context.
 *
 * Parameters:
 *   ctx - Context to initialize
 */
void difficulty_ctx_init(difficulty_ctx_t *ctx);

/*
 * Check if a block height is at a difficulty adjustment boundary.
 *
 * Difficulty adjusts every 2016 blocks (heights 2016, 4032, 6048, etc.).
 *
 * Parameters:
 *   height - Block height to check
 *
 * Returns:
 *   ECHO_TRUE if this height triggers a difficulty adjustment
 */
echo_bool_t difficulty_is_retarget_height(uint32_t height);

/*
 * Compute the expected difficulty (bits) for a block at a given height.
 *
 * The difficulty adjustment algorithm:
 *   1. If not at a retarget boundary, use previous block's bits
 *   2. At retarget (every 2016 blocks):
 *      a. Calculate actual time span of the previous 2016 blocks
 *      b. Clamp time span to [TARGET_TIMESPAN/4, TARGET_TIMESPAN*4]
 *      c. new_target = old_target * actual_time / TARGET_TIMESPAN
 *      d. Ensure new_target <= proof-of-work limit (minimum difficulty)
 *
 * Parameters:
 *   ctx  - Difficulty context with period information
 *   bits - Output: computed difficulty bits
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if ctx or bits is NULL
 */
echo_result_t difficulty_compute_next(const difficulty_ctx_t *ctx,
                                      uint32_t *bits);

/*
 * Validate that a block's difficulty bits match the expected value.
 *
 * On testnet, this also accepts minimum difficulty if the 20-minute rule
 * applies (block timestamp > parent timestamp + 20 minutes).
 *
 * Parameters:
 *   header - Block header to validate
 *   ctx    - Difficulty context
 *   error  - Output: specific error if validation fails (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if bits match expected, ECHO_FALSE otherwise
 */
echo_bool_t block_validate_difficulty(const block_header_t *header,
                                      const difficulty_ctx_t *ctx,
                                      block_validation_error_t *error);

/*
 * Check if the testnet 20-minute difficulty reset rule applies.
 *
 * On testnet, if no block is found within 20 minutes of the previous block,
 * the next block is allowed to use minimum difficulty (powlimit).
 *
 * This function is only meaningful on testnet; on mainnet/regtest it
 * always returns ECHO_FALSE.
 *
 * Parameters:
 *   block_timestamp  - Timestamp of the block being validated
 *   parent_timestamp - Timestamp of the parent block
 *
 * Returns:
 *   ECHO_TRUE if the 20-minute rule applies (testnet only)
 *   ECHO_FALSE on mainnet/regtest or if rule doesn't apply
 */
echo_bool_t difficulty_testnet_20min_rule_applies(uint32_t block_timestamp,
                                                  uint32_t parent_timestamp);

/*
 * Compute the actual time span of a difficulty period.
 *
 * This is simply: period_end_time - period_start_time
 *
 * Parameters:
 *   ctx - Difficulty context
 *
 * Returns:
 *   Time span in seconds, or 0 if ctx is NULL
 */
uint32_t difficulty_actual_timespan(const difficulty_ctx_t *ctx);

/*
 * Clamp a time span to the allowed adjustment range.
 *
 * The time span is clamped to [TARGET_TIMESPAN/4, TARGET_TIMESPAN*4]
 * to prevent extreme difficulty adjustments.
 *
 * Parameters:
 *   timespan - Raw time span in seconds
 *
 * Returns:
 *   Clamped time span
 */
uint32_t difficulty_clamp_timespan(uint32_t timespan);

/*
 * Compute a new target given the old target and time ratio.
 *
 * new_target = old_target * actual_timespan / TARGET_TIMESPAN
 *
 * The result is clamped to not exceed the proof-of-work limit.
 *
 * Parameters:
 *   old_bits        - Previous difficulty bits
 *   actual_timespan - Actual time span (already clamped)
 *   new_bits        - Output: computed new difficulty bits
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if new_bits is NULL
 */
echo_result_t difficulty_adjust_target(uint32_t old_bits,
                                       uint32_t actual_timespan,
                                       uint32_t *new_bits);

/*
 * Get the proof-of-work limit (minimum difficulty) as a 256-bit target.
 *
 * Parameters:
 *   target - Output: 256-bit target (32 bytes)
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if target is NULL
 */
echo_result_t difficulty_get_powlimit(hash256_t *target);

/*
 * ============================================================================
 * Coinbase Validation (Session 5.3)
 * ============================================================================
 */

/*
 * Coinbase maturity constants.
 * Coinbase outputs cannot be spent until this many blocks have passed.
 */
#define COINBASE_MATURITY 100

/*
 * BIP-34 activation height (mainnet).
 * After this height, coinbase must encode block height as first item.
 */
#define BIP34_HEIGHT 227931

/*
 * Witness commitment magic prefix.
 * OP_RETURN outputs containing witness commitment start with these 4 bytes.
 */
#define WITNESS_COMMITMENT_PREFIX_LEN 4
extern const uint8_t WITNESS_COMMITMENT_PREFIX[4];

/*
 * Calculate the block subsidy for a given height.
 *
 * The initial subsidy is 50 BTC (5,000,000,000 satoshis).
 * It halves every 210,000 blocks:
 *   Heights 0-209,999:        50 BTC
 *   Heights 210,000-419,999:  25 BTC
 *   Heights 420,000-629,999:  12.5 BTC
 *   etc.
 *
 * Parameters:
 *   height - Block height
 *
 * Returns:
 *   Block subsidy in satoshis
 */
satoshi_t coinbase_subsidy(uint32_t height);

/*
 * Parse the block height from a BIP-34 coinbase scriptsig.
 *
 * BIP-34 requires the coinbase scriptsig to begin with the block height
 * encoded as a minimally-encoded script number (CScriptNum).
 *
 * Parameters:
 *   script     - Coinbase scriptsig bytes
 *   script_len - Length of scriptsig
 *   height     - Output: extracted block height
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if script or height is NULL
 *   ECHO_ERR_INVALID_FORMAT if scriptsig doesn't encode height properly
 */
echo_result_t coinbase_parse_height(const uint8_t *script, size_t script_len,
                                    uint32_t *height);

/*
 * Validate the BIP-34 height encoding in a coinbase.
 *
 * Checks that the scriptsig correctly encodes the expected height.
 * Only enforced after BIP34_HEIGHT.
 *
 * Parameters:
 *   coinbase        - The coinbase transaction
 *   expected_height - Expected block height
 *   error           - Output: specific error (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if height is valid or BIP-34 not active
 *   ECHO_FALSE if height encoding is invalid
 */
echo_bool_t coinbase_validate_height(const tx_t *coinbase,
                                     uint32_t expected_height,
                                     block_validation_error_t *error);

/*
 * Find the witness commitment output in a coinbase transaction.
 *
 * The witness commitment is in an OP_RETURN output with the format:
 *   OP_RETURN <commitment>
 * Where <commitment> starts with the 4-byte magic prefix aa21a9ed
 * followed by the 32-byte commitment hash.
 *
 * Parameters:
 *   coinbase   - The coinbase transaction
 *   commitment - Output: 32-byte witness commitment (if found)
 *
 * Returns:
 *   ECHO_OK if commitment found and extracted
 *   ECHO_ERR_NOT_FOUND if no witness commitment output exists
 *   ECHO_ERR_NULL_PARAM if coinbase or commitment is NULL
 */
echo_result_t coinbase_find_witness_commitment(const tx_t *coinbase,
                                               hash256_t *commitment);

/*
 * Validate the witness commitment in a block.
 *
 * For blocks with SegWit transactions, verifies:
 *   1. Coinbase has a witness commitment output
 *   2. Coinbase witness stack has exactly one 32-byte item (the nonce)
 *   3. SHA256d(witness_merkle_root || nonce) matches the commitment
 *
 * Parameters:
 *   block - The full block
 *   error - Output: specific error (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if witness commitment is valid or not required
 *   ECHO_FALSE if commitment is invalid
 */
echo_bool_t block_validate_witness_commitment(const block_t *block,
                                              block_validation_error_t *error);

/*
 * Validate a coinbase transaction.
 *
 * Checks:
 *   1. Exactly one input with null outpoint
 *   2. BIP-34 height encoding (if active)
 *   3. Total output value <= subsidy + fees
 *   4. Scriptsig size within limits (2-100 bytes)
 *
 * Parameters:
 *   coinbase        - The coinbase transaction
 *   height          - Block height
 *   max_allowed     - Maximum allowed output value (subsidy + fees)
 *   error           - Output: specific error (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if coinbase is valid
 *   ECHO_FALSE if validation fails
 */
echo_bool_t coinbase_validate(const tx_t *coinbase, uint32_t height,
                              satoshi_t max_allowed,
                              block_validation_error_t *error);

/*
 * Check if a coinbase output is mature (spendable).
 *
 * Coinbase outputs cannot be spent until COINBASE_MATURITY blocks
 * have been mined on top of the block containing them.
 *
 * Parameters:
 *   coinbase_height - Height of block containing the coinbase
 *   current_height  - Current chain height
 *
 * Returns:
 *   ECHO_TRUE if mature (current_height - coinbase_height >= COINBASE_MATURITY)
 *   ECHO_FALSE if immature
 */
echo_bool_t coinbase_is_mature(uint32_t coinbase_height,
                               uint32_t current_height);

/*
 * ============================================================================
 * Full Block Validation (Session 5.4)
 * ============================================================================
 */

/*
 * Full block validation context.
 *
 * Extends the header validation context with additional information
 * needed for complete block validation.
 */
typedef struct {
  /* Header validation context */
  block_validation_ctx_t header_ctx;

  /* Difficulty context (for difficulty validation) */
  difficulty_ctx_t difficulty_ctx;

  /* Block height (same as header_ctx.height, for convenience) */
  uint32_t height;

  /* Total fees available from block transactions.
   * For full validation, this is sum(inputs) - sum(outputs) for all
   * non-coinbase txs. Set to 0 if not computing (subsidy-only validation). */
  satoshi_t total_fees;

  /* Whether this is a SegWit-active block */
  echo_bool_t segwit_active;

} full_block_ctx_t;

/*
 * Block validation result with detailed information.
 */
typedef struct {
  /* Overall validation result */
  echo_bool_t valid;

  /* Specific error code */
  block_validation_error_t error;

  /* Index of failing transaction (if error is TX_INVALID or similar) */
  size_t failing_tx_index;

  /* Additional error information (for debugging) */
  const char *error_msg;

} block_validation_result_t;

/*
 * Initialize a full block validation context.
 *
 * Parameters:
 *   ctx - Context to initialize
 */
void full_block_ctx_init(full_block_ctx_t *ctx);

/*
 * Initialize a block validation result.
 *
 * Parameters:
 *   result - Result to initialize
 */
void block_validation_result_init(block_validation_result_t *result);

/*
 * Check if a block has any duplicate transaction IDs.
 *
 * Duplicate txids are not allowed in a block.
 *
 * Parameters:
 *   block    - Block to check
 *   dup_idx  - Output: index of first duplicate (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if duplicates found, ECHO_FALSE if all txids unique
 */
echo_bool_t block_has_duplicate_txids(const block_t *block, size_t *dup_idx);

/*
 * Verify the merkle root in a block header matches the transactions.
 *
 * Parameters:
 *   block - Block to validate
 *   error - Output: specific error (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if merkle root matches, ECHO_FALSE otherwise
 */
echo_bool_t block_validate_merkle_root(const block_t *block,
                                       block_validation_error_t *error);

/*
 * Verify merkle root using pre-computed TXIDs.
 *
 * Same as block_validate_merkle_root but accepts pre-computed TXIDs
 * to avoid redundant SHA256d computation during block validation.
 *
 * Parameters:
 *   block - Block to validate
 *   txids - Pre-computed TXIDs (NULL to compute internally)
 *   error - Output: specific error (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if merkle root matches, ECHO_FALSE otherwise
 */
echo_bool_t block_validate_merkle_root_with_txids(const block_t *block,
                                                  const hash256_t *txids,
                                                  block_validation_error_t *error);

/*
 * Validate block size and weight limits.
 *
 * Checks:
 *   - Block size <= 4MB (serialized size)
 *   - Block weight <= 4M weight units
 *
 * Parameters:
 *   block - Block to validate
 *   error - Output: specific error (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if within limits, ECHO_FALSE if exceeded
 */
echo_bool_t block_validate_size(const block_t *block,
                                block_validation_error_t *error);

/*
 * Validate block transaction structure.
 *
 * Checks:
 *   - Block has at least one transaction
 *   - First transaction is coinbase
 *   - All other transactions are non-coinbase
 *   - No duplicate transaction IDs
 *
 * Parameters:
 *   block  - Block to validate
 *   error  - Output: specific error (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if structure is valid, ECHO_FALSE otherwise
 */
echo_bool_t block_validate_tx_structure(const block_t *block,
                                        block_validation_error_t *error);

/*
 * Perform complete block validation.
 *
 * This is the main entry point for full block validation. It performs:
 *   1. Header validation (PoW, timestamp, prev_block, version)
 *   2. Difficulty validation (bits match expected)
 *   3. Block size/weight limits
 *   4. Transaction structure (coinbase, no duplicates)
 *   5. Merkle root verification
 *   6. Coinbase validation (BIP-34 height, subsidy limit)
 *   7. Witness commitment verification (if SegWit active)
 *
 * Note: This does NOT validate individual transaction scripts or
 * UTXO availability—that requires the UTXO set from Phase 6.
 *
 * Parameters:
 *   block  - Block to validate
 *   ctx    - Full validation context
 *   result - Output: detailed validation result
 *
 * Returns:
 *   ECHO_TRUE if block passes all checks, ECHO_FALSE otherwise
 */
echo_bool_t block_validate(const block_t *block, const full_block_ctx_t *ctx,
                           block_validation_result_t *result);

/*
 * Validate a block with minimal context (header-only checks).
 *
 * This performs only the checks that don't require chain context:
 *   - Header validation (PoW, basic timestamp)
 *   - Block structure (coinbase, size)
 *   - Merkle root
 *
 * Parameters:
 *   block  - Block to validate
 *   error  - Output: specific error (may be NULL)
 *
 * Returns:
 *   ECHO_TRUE if block passes basic checks, ECHO_FALSE otherwise
 */
echo_bool_t block_validate_basic(const block_t *block,
                                 block_validation_error_t *error);

#endif /* ECHO_BLOCK_VALIDATE_H */
