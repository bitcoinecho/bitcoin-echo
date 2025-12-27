/*
 * Bitcoin Echo — Mining Support
 *
 * This module provides mining-related functionality for regtest testing:
 *   - Coinbase transaction construction
 *   - Block template generation helpers
 *   - Regtest-specific difficulty handling
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_MINING_H
#define ECHO_MINING_H

#include "block.h"
#include "echo_types.h"
#include "tx.h"
#include <stddef.h>
#include <stdint.h>

/*
 * ============================================================================
 * REGTEST DIFFICULTY
 * ============================================================================
 * Regtest uses a trivially easy difficulty target that allows CPU mining.
 */

/* Regtest proof-of-work limit bits (very easy difficulty).
 * This is 0x207fffff which produces target:
 * 0x7fffff0000000000000000000000000000000000000000000000000000000000
 * Mining with this target typically requires only a few nonce attempts. */
#define REGTEST_POWLIMIT_BITS 0x207fffff

/*
 * ============================================================================
 * TESTNET GENESIS BLOCK (testnet3)
 * ============================================================================
 * Testnet3 genesis block uses the same merkle root as mainnet (same coinbase),
 * but with different timestamp, nonce, and bits (same as mainnet bits).
 *
 * Genesis hash: 000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943
 */

/* Testnet genesis block constants */
#define TESTNET_GENESIS_VERSION 1
#define TESTNET_GENESIS_TIMESTAMP 1296688602
#define TESTNET_GENESIS_BITS 0x1d00ffff /* Same as mainnet */
#define TESTNET_GENESIS_NONCE 414098458

/* Testnet proof-of-work limit (same as mainnet) */
#define TESTNET_POWLIMIT_BITS 0x1d00ffff

/*
 * Testnet special difficulty rule (20-minute rule).
 * If no block is found within 20 minutes, the difficulty drops to minimum.
 * This allows testing to continue even if hash power disappears.
 *
 * The rule: If block.timestamp > prev_block.timestamp + 20 minutes,
 * the required PoW is the minimum difficulty (powlimit).
 */
#define TESTNET_20MIN_RULE_SECONDS (20 * 60)

/*
 * ============================================================================
 * REGTEST GENESIS BLOCK
 * ============================================================================
 */

/* Regtest genesis block constants */
#define REGTEST_GENESIS_VERSION 1
#define REGTEST_GENESIS_TIMESTAMP 1296688602
#define REGTEST_GENESIS_BITS REGTEST_POWLIMIT_BITS
#define REGTEST_GENESIS_NONCE 2

/*
 * ============================================================================
 * COINBASE CONSTRUCTION
 * ============================================================================
 */

/**
 * Coinbase construction parameters.
 */
typedef struct {
  /* Block height (required for BIP-34 height encoding) */
  uint32_t height;

  /* Total value to output (subsidy + fees) in satoshis */
  satoshi_t value;

  /* Output script (scriptPubKey).
   * If NULL, uses OP_TRUE (anyone can spend) for testing. */
  const uint8_t *output_script;
  size_t output_script_len;

  /* Extra data to include in coinbase scriptsig (after height).
   * Can be used for pool tags, extra nonce, etc. */
  const uint8_t *extra_data;
  size_t extra_data_len;

  /* Whether to include witness commitment.
   * If true, adds a second output with OP_RETURN witness commitment. */
  echo_bool_t include_witness_commitment;

  /* Witness commitment hash (required if include_witness_commitment is true).
   * This is SHA256d(witness_merkle_root || coinbase_witness_nonce) */
  hash256_t witness_commitment;

} coinbase_params_t;

/**
 * Initialize coinbase parameters to defaults.
 *
 * Defaults:
 *   - height: 0
 *   - value: 0
 *   - output_script: NULL (will use OP_TRUE)
 *   - extra_data: NULL
 *   - include_witness_commitment: false
 *
 * Parameters:
 *   params - Parameters to initialize
 */
void coinbase_params_init(coinbase_params_t *params);

/**
 * Create a coinbase transaction.
 *
 * The coinbase transaction has:
 *   - One input with null outpoint (txid=0, vout=0xffffffff)
 *   - BIP-34 height encoding in scriptsig
 *   - One or more outputs (main output + optional witness commitment)
 *   - Version 1, locktime 0
 *
 * Parameters:
 *   params - Coinbase construction parameters
 *   tx     - Output: constructed coinbase transaction
 *            Caller must call tx_free() when done.
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if params or tx is NULL
 *   ECHO_ERR_OUT_OF_MEMORY if allocation fails
 */
echo_result_t coinbase_create(const coinbase_params_t *params, tx_t *tx);

/**
 * Encode block height as BIP-34 scriptsig bytes.
 *
 * BIP-34 requires the block height to be encoded as the first element
 * of the coinbase scriptsig using minimal push encoding.
 *
 * Parameters:
 *   height - Block height to encode
 *   buf    - Output buffer (must be at least 9 bytes for safety)
 *   buf_len - Size of buffer
 *   written - Output: bytes written
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NULL_PARAM if buf or written is NULL
 *   ECHO_ERR_BUFFER_TOO_SMALL if buffer is too small
 */
echo_result_t coinbase_encode_height(uint32_t height, uint8_t *buf,
                                     size_t buf_len, size_t *written);

/*
 * ============================================================================
 * BLOCK TEMPLATE HELPERS
 * ============================================================================
 */

/**
 * Block template structure for getblocktemplate response.
 */
typedef struct {
  /* Block header fields */
  int32_t version;
  hash256_t prev_hash;
  uint32_t curtime;
  uint32_t bits;
  uint32_t height;

  /* Minimum time (MTP + 1) */
  uint32_t mintime;

  /* Coinbase value (subsidy + fees) */
  satoshi_t coinbase_value;

  /* Target as 256-bit hash for comparison */
  hash256_t target;

} block_template_t;

/**
 * Initialize block template to defaults.
 *
 * Parameters:
 *   tmpl - Template to initialize
 */
void block_template_init(block_template_t *tmpl);

/**
 * Get the testnet3 genesis block header.
 *
 * The testnet3 genesis block uses the same merkle root as mainnet but
 * with different timestamp and nonce.
 *
 * Parameters:
 *   header - Output: testnet genesis block header
 */
void block_genesis_header_testnet(block_header_t *header);

/**
 * Get the regtest genesis block header.
 *
 * The regtest genesis block uses the same structure as mainnet but
 * with different parameters (trivial difficulty, different nonce).
 *
 * Parameters:
 *   header - Output: regtest genesis block header
 */
void block_genesis_header_regtest(block_header_t *header);

/**
 * Get network-specific difficulty bits.
 *
 * For regtest, always returns REGTEST_POWLIMIT_BITS (no adjustment).
 * For other networks, this would consult the difficulty context.
 *
 * Parameters:
 *   height    - Block height
 *   is_regtest - Whether this is regtest network
 *
 * Returns:
 *   Difficulty bits for the given height
 */
uint32_t mining_get_difficulty_bits(uint32_t height, echo_bool_t is_regtest);

/**
 * Mine a block header (for regtest testing).
 *
 * Iterates through nonces until finding one that produces a hash
 * meeting the target difficulty. Only suitable for regtest where
 * difficulty is trivial.
 *
 * Parameters:
 *   header     - Block header to mine (nonce field will be modified)
 *   max_nonce  - Maximum nonce to try (use 0xFFFFFFFF for full range)
 *
 * Returns:
 *   ECHO_OK if valid nonce found
 *   ECHO_ERR_NULL_PARAM if header is NULL
 *   ECHO_ERR_NOT_FOUND if no valid nonce found in range
 */
echo_result_t mining_find_nonce(block_header_t *header, uint32_t max_nonce);

#endif /* ECHO_MINING_H */
