/**
 * @file chainstate.h
 * @brief Chain state tracking for Bitcoin consensus
 *
 * The chain state represents the current validated state of the blockchain,
 * including the current tip (best block), accumulated work, and the UTXO set.
 * This module provides operations to apply and revert blocks during chain
 * selection and reorganization.
 *
 * Design principles:
 * - Chain state transitions are atomic (all-or-nothing)
 * - Block application creates a delta that can be reverted
 * - Accumulated work is tracked for chain selection
 * - The UTXO set is updated consistently with block transitions
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_CHAINSTATE_H
#define ECHO_CHAINSTATE_H

#include "block.h"
#include "echo_types.h"
#include "tx.h"
#include "utxo.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Work (chain difficulty) represented as 256-bit unsigned integer.
 * Stored little-endian for consistency with hash256_t.
 *
 * Work for a single block is calculated as:
 *   work = 2^256 / (target + 1)
 *
 * Cumulative work is the sum of work for all blocks in the chain.
 */
typedef struct {
  uint8_t bytes[32];
} work256_t;

/**
 * Block index entry: metadata for a known block header.
 * Used to track the chain of headers without storing full blocks.
 */
typedef struct block_index {
  hash256_t hash;           /* Block hash */
  hash256_t prev_hash;      /* Previous block hash */
  uint32_t height;          /* Height in the chain (genesis = 0) */
  uint32_t timestamp;       /* Block timestamp */
  uint32_t bits;            /* Compact target */
  work256_t chainwork;      /* Cumulative work up to and including this block */
  bool on_main_chain;       /* True if on the active chain */
  struct block_index *prev; /* Pointer to previous block index (may be NULL) */

  /*
   * Block data file position (Session 9.6.7).
   * Tracks where this block's data is stored on disk.
   * UINT32_MAX means "not stored" (header-only).
   */
  uint32_t data_file;       /* File index (blk*.dat) or UINT32_MAX if not stored */
  uint32_t data_pos;        /* Byte offset within file */
} block_index_t;

/* Sentinel value indicating block data is not stored on disk */
#define BLOCK_DATA_NOT_STORED UINT32_MAX

/**
 * Chain tip info: current best chain head.
 */
typedef struct {
  hash256_t hash;      /* Block hash of the tip */
  uint32_t height;     /* Height of the tip */
  work256_t chainwork; /* Cumulative work at the tip */
} chain_tip_t;

/**
 * Block delta: changes made by applying a block.
 * Used for atomic block application and reverting during reorganization.
 */
typedef struct {
  hash256_t block_hash; /* Hash of the applied block */
  uint32_t height;      /* Height of the applied block */

  /* UTXO changes */
  outpoint_t *created;  /* Array of created outpoints */
  size_t created_count; /* Number of created UTXOs */

  utxo_entry_t **spent; /* Array of spent UTXO entries (for undo) */
  size_t spent_count;   /* Number of spent UTXOs */
} block_delta_t;

/**
 * Chain state: the complete validated state of the blockchain.
 * This is an opaque structure; implementation details are in chainstate.c.
 */
typedef struct chainstate chainstate_t;

/* ========================================================================
 * Work (256-bit integer) Operations
 * ======================================================================== */

/**
 * Initialize work to zero.
 * @param work Work value to initialize
 */
void work256_zero(work256_t *work);

/**
 * Check if work is zero.
 * @param work Work value to check
 * @return true if work is zero
 */
bool work256_is_zero(const work256_t *work);

/**
 * Compare two work values.
 * @param a First work value
 * @param b Second work value
 * @return -1 if a < b, 0 if a == b, 1 if a > b
 */
int work256_compare(const work256_t *a, const work256_t *b);

/**
 * Add two work values: result = a + b
 * @param a First addend
 * @param b Second addend
 * @param result Output sum (may alias a or b)
 */
void work256_add(const work256_t *a, const work256_t *b, work256_t *result);

/**
 * Subtract work values: result = a - b
 * @param a Minuend
 * @param b Subtrahend
 * @param result Output difference (may alias a or b)
 * @return ECHO_OK on success, ECHO_ERR_UNDERFLOW if b > a
 */
echo_result_t work256_sub(const work256_t *a, const work256_t *b,
                          work256_t *result);

/**
 * Calculate work from a target (compact bits representation).
 * Work = 2^256 / (target + 1)
 *
 * @param bits Compact target representation
 * @param work Output work value
 * @return ECHO_OK on success
 */
echo_result_t work256_from_bits(uint32_t bits, work256_t *work);

/* ========================================================================
 * Block Index Operations
 * ======================================================================== */

/**
 * Create a block index entry from a block header.
 * @param header Block header
 * @param prev Previous block index (NULL for genesis)
 * @return Newly allocated block index, or NULL on failure
 */
block_index_t *block_index_create(const block_header_t *header,
                                  block_index_t *prev);

/**
 * Destroy a block index entry.
 * @param index Block index to destroy (may be NULL)
 */
void block_index_destroy(block_index_t *index);

/* ========================================================================
 * Block Delta Operations
 * ======================================================================== */

/**
 * Create an empty block delta.
 * @param block_hash Hash of the block being applied
 * @param height Height of the block
 * @return Newly allocated delta, or NULL on failure
 */
block_delta_t *block_delta_create(const hash256_t *block_hash, uint32_t height);

/**
 * Destroy a block delta and free all associated memory.
 * @param delta Delta to destroy (may be NULL)
 */
void block_delta_destroy(block_delta_t *delta);

/**
 * Record a created UTXO in the delta.
 * @param delta The delta
 * @param outpoint The created outpoint
 * @return ECHO_OK on success, ECHO_ERR_NOMEM on allocation failure
 */
echo_result_t block_delta_add_created(block_delta_t *delta,
                                      const outpoint_t *outpoint);

/**
 * Record a spent UTXO in the delta.
 * The entry is cloned for later restoration during revert.
 * @param delta The delta
 * @param entry The spent UTXO entry
 * @return ECHO_OK on success, ECHO_ERR_NOMEM on allocation failure
 */
echo_result_t block_delta_add_spent(block_delta_t *delta,
                                    const utxo_entry_t *entry);

/* ========================================================================
 * Chain State Operations
 * ======================================================================== */

/**
 * Create a new chain state initialized to genesis.
 * @return Newly allocated chain state, or NULL on failure
 */
chainstate_t *chainstate_create(void);

/**
 * Destroy a chain state and free all associated memory.
 * @param state Chain state to destroy (may be NULL)
 */
void chainstate_destroy(chainstate_t *state);

/**
 * Get the current chain tip.
 * @param state The chain state
 * @param tip Output: current tip info
 * @return ECHO_OK on success
 */
echo_result_t chainstate_get_tip(const chainstate_t *state, chain_tip_t *tip);

/**
 * Get the current chain height.
 * @param state The chain state
 * @return Current height (0 for genesis)
 */
uint32_t chainstate_get_height(const chainstate_t *state);

/**
 * Get the UTXO set (read-only).
 * @param state The chain state
 * @return Pointer to the UTXO set
 */
const utxo_set_t *chainstate_get_utxo_set(const chainstate_t *state);

/**
 * Get the UTXO set (mutable).
 * Used for restoring UTXOs from database during startup.
 * @param state The chain state
 * @return Mutable pointer to the UTXO set
 */
utxo_set_t *chainstate_get_utxo_set_mutable(chainstate_t *state);

/**
 * Apply a block to the chain state.
 *
 * This function:
 * 1. Validates that the block connects to the current tip
 * 2. Creates new UTXOs for all transaction outputs
 * 3. Removes spent UTXOs for all transaction inputs
 * 4. Updates the chain tip and accumulated work
 * 5. Creates a delta for potential reverting
 *
 * Note: This does NOT perform full block validation (PoW, scripts, etc.)
 * That must be done before calling this function.
 *
 * @param state The chain state
 * @param header The block header being applied
 * @param txs Array of transactions in the block
 * @param tx_count Number of transactions
 * @param delta Output: delta for reverting (caller owns, may be NULL)
 * @return ECHO_OK on success, error code on failure
 */
echo_result_t chainstate_apply_block(chainstate_t *state,
                                     const block_header_t *header,
                                     const tx_t *txs, size_t tx_count,
                                     block_delta_t **delta);

/**
 * Apply a block with pre-computed TXIDs.
 *
 * Same as chainstate_apply_block but accepts pre-computed TXIDs
 * to avoid redundant SHA256d computation during block application.
 *
 * @param state The chain state
 * @param header The block header being applied
 * @param txs Array of transactions in the block
 * @param tx_count Number of transactions
 * @param precomputed_txids Pre-computed TXIDs (NULL to compute internally)
 * @param delta Output: delta for reverting (caller owns, may be NULL)
 * @return ECHO_OK on success, error code on failure
 */
echo_result_t chainstate_apply_block_with_txids(chainstate_t *state,
                                                const block_header_t *header,
                                                const tx_t *txs, size_t tx_count,
                                                const hash256_t *precomputed_txids,
                                                block_delta_t **delta);

/**
 * Revert a block from the chain state.
 *
 * This function reverses the effect of chainstate_apply_block(),
 * restoring the UTXO set and chain tip to their previous state.
 *
 * @param state The chain state
 * @param delta The delta from the original apply operation
 * @return ECHO_OK on success, error code on failure
 */
echo_result_t chainstate_revert_block(chainstate_t *state,
                                      const block_delta_t *delta);

/**
 * Check if a block is on the main chain.
 * @param state The chain state
 * @param hash Block hash to check
 * @return true if the block is on the main chain
 */
bool chainstate_is_on_main_chain(const chainstate_t *state,
                                 const hash256_t *hash);

/**
 * Get the block hash at a specific height.
 * @param state The chain state
 * @param height Height to query
 * @param hash Output: block hash at that height
 * @return ECHO_OK on success, ECHO_ERR_NOT_FOUND if height > tip height
 */
echo_result_t chainstate_get_block_at_height(const chainstate_t *state,
                                             uint32_t height, hash256_t *hash);

/**
 * Lookup a UTXO in the chain state.
 * @param state The chain state
 * @param outpoint The outpoint to lookup
 * @return Pointer to UTXO entry if found, NULL otherwise
 */
const utxo_entry_t *chainstate_lookup_utxo(const chainstate_t *state,
                                           const outpoint_t *outpoint);

/**
 * Get statistics about the chain state.
 * @param state The chain state
 * @param utxo_count Output: number of UTXOs (may be NULL)
 * @param total_amount Output: total amount in UTXOs in satoshis (may be NULL)
 */
void chainstate_get_stats(const chainstate_t *state, size_t *utxo_count,
                          int64_t *total_amount);

/* ========================================================================
 * Block Index Map Operations
 * ======================================================================== */

/**
 * Block index map: stores all known block headers for fork tracking.
 * This is an opaque structure; implementation uses a hash table.
 */
typedef struct block_index_map block_index_map_t;

/**
 * Create a new empty block index map.
 * @param initial_capacity Suggested initial capacity (0 for default)
 * @return Newly allocated map, or NULL on failure
 */
block_index_map_t *block_index_map_create(size_t initial_capacity);

/**
 * Destroy a block index map and all contained indices.
 * @param map Map to destroy (may be NULL)
 */
void block_index_map_destroy(block_index_map_t *map);

/**
 * Insert a block index into the map.
 * The map takes ownership of the index.
 * @param map The map
 * @param index The block index to insert
 * @return ECHO_OK on success, ECHO_ERR_EXISTS if already present
 */
echo_result_t block_index_map_insert(block_index_map_t *map,
                                     block_index_t *index);

/**
 * Lookup a block index by hash.
 * @param map The map
 * @param hash Block hash to lookup
 * @return Pointer to block index if found, NULL otherwise
 */
block_index_t *block_index_map_lookup(const block_index_map_t *map,
                                      const hash256_t *hash);

/**
 * Get the number of block indices in the map.
 * @param map The map
 * @return Number of entries
 */
size_t block_index_map_size(const block_index_map_t *map);

/**
 * Find the block index with the most accumulated work.
 * @param map The map
 * @return Pointer to best block index, or NULL if map is empty
 */
block_index_t *block_index_map_find_best(const block_index_map_t *map);

/* ========================================================================
 * Chain Selection Operations (Session 6.3)
 * ======================================================================== */

/**
 * Result of chain comparison.
 */
typedef enum {
  CHAIN_COMPARE_A_BETTER = -1, /* Chain A has more work */
  CHAIN_COMPARE_EQUAL = 0,     /* Chains have equal work */
  CHAIN_COMPARE_B_BETTER = 1   /* Chain B has more work */
} chain_compare_result_t;

/**
 * Compare two chain tips by accumulated work.
 * This implements Nakamoto consensus: the chain with more work wins.
 * In case of a tie, we prefer the chain we saw first (status quo).
 *
 * @param a First block index (chain tip)
 * @param b Second block index (chain tip)
 * @return Comparison result
 */
chain_compare_result_t chain_compare(const block_index_t *a,
                                     const block_index_t *b);

/**
 * Find the common ancestor of two block indices.
 * Walks back from both tips until finding a shared block.
 *
 * @param a First block index
 * @param b Second block index
 * @return Common ancestor block index, or NULL if unrelated
 */
block_index_t *chain_find_common_ancestor(block_index_t *a, block_index_t *b);

/**
 * Reorganization data: describes the blocks to revert and apply.
 */
typedef struct {
  /* Blocks to disconnect (revert), ordered tip to ancestor */
  block_index_t **disconnect;
  size_t disconnect_count;

  /* Blocks to connect (apply), ordered ancestor to new tip */
  block_index_t **connect;
  size_t connect_count;

  /* The common ancestor */
  block_index_t *ancestor;
} chain_reorg_t;

/**
 * Create a reorganization plan from current tip to new tip.
 * Does NOT execute the reorganization, just computes what's needed.
 *
 * @param current Current chain tip
 * @param new_tip Proposed new chain tip
 * @return Reorg plan, or NULL on failure
 */
chain_reorg_t *chain_reorg_create(block_index_t *current,
                                  block_index_t *new_tip);

/**
 * Destroy a reorganization plan.
 * @param reorg Reorg plan to destroy (may be NULL)
 */
void chain_reorg_destroy(chain_reorg_t *reorg);

/**
 * Execute a chain reorganization on the chain state.
 *
 * This function:
 * 1. Reverts blocks from current tip to common ancestor
 * 2. Applies blocks from ancestor to new tip
 * 3. Updates the main chain markers in block indices
 *
 * Requires block data callback to get transaction data for each block
 * during revert/apply operations.
 *
 * @param state The chain state
 * @param reorg The reorganization plan
 * @param get_block_txs Callback to get transaction data for a block
 * @param user_data User data for callback
 * @return ECHO_OK on success, error code on failure (state may be inconsistent)
 */
typedef echo_result_t (*get_block_txs_fn)(const hash256_t *block_hash,
                                          const tx_t **txs_out,
                                          size_t *tx_count_out,
                                          void *user_data);

echo_result_t chain_reorganize(chainstate_t *state, chain_reorg_t *reorg,
                               get_block_txs_fn get_block_txs, void *user_data);

/**
 * Add a new block header to chain state tracking.
 * This registers the block in the index map but does NOT apply it.
 * Use this for headers-first sync or when receiving orphan blocks.
 *
 * @param state The chain state
 * @param header The block header to add
 * @param index_out Output: the created block index (may be NULL)
 * @return ECHO_OK if added, ECHO_ERR_EXISTS if already known
 */
echo_result_t chainstate_add_header(chainstate_t *state,
                                    const block_header_t *header,
                                    block_index_t **index_out);

/**
 * Add a block header with pre-computed hash.
 *
 * Same as chainstate_add_header but uses a pre-computed hash
 * to avoid redundant SHA256d computation during header sync.
 *
 * @param state The chain state
 * @param header The block header to add
 * @param hash Pre-computed block hash (NULL to compute internally)
 * @param index_out Output: the created block index (may be NULL)
 * @return ECHO_OK if added, ECHO_ERR_EXISTS if already known
 */
echo_result_t chainstate_add_header_with_hash(chainstate_t *state,
                                              const block_header_t *header,
                                              const hash256_t *hash,
                                              block_index_t **index_out);

/**
 * Get the block index map from chain state.
 * @param state The chain state
 * @return Pointer to block index map
 */
block_index_map_t *chainstate_get_block_index_map(chainstate_t *state);

/**
 * Get the block index for the current tip.
 * @param state The chain state
 * @return Block index for tip, or NULL if no blocks yet
 */
block_index_t *chainstate_get_tip_index(const chainstate_t *state);

/**
 * Set the current tip to a specific block index.
 * Used during reorganization. Does NOT validate or update UTXO set.
 * @param state The chain state
 * @param index The new tip block index
 */
void chainstate_set_tip_index(chainstate_t *state, block_index_t *index);

/**
 * Set the best header index without updating the validated tip.
 *
 * This is used when loading headers from the database. It sets tip_index
 * (used for sync locator building) without updating tip (the validated
 * chain tip used for block validation).
 *
 * @param state Chain state
 * @param index Best header block index
 */
void chainstate_set_best_header_index(chainstate_t *state,
                                      block_index_t *index);

/**
 * Check if a block should trigger a reorganization.
 * Returns true if the new block has more work than current tip.
 *
 * @param state The chain state
 * @param new_index Block index of potentially better chain tip
 * @return true if reorganization is needed
 */
bool chainstate_should_reorg(const chainstate_t *state,
                             const block_index_t *new_index);

#endif /* ECHO_CHAINSTATE_H */
