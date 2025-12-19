/*
 * Bitcoin Echo — Consensus Engine Interface
 *
 * This is the unified interface to the Bitcoin consensus engine.
 * The consensus engine is the frozen core of Bitcoin Echo—it performs
 * all validation and chain selection logic as pure computation.
 *
 * Key design principles:
 *   - Pure functions: validation takes bytes and context, returns validity
 *   - No system calls: consensus engine never touches disk, network, or time
 *   - No dynamic allocation during validation: memory from caller-provided
 * arena
 *   - Deterministic: same inputs always produce same outputs
 *
 * The consensus boundary is clearly defined:
 *   - Above: Protocol layer (networking, storage, mempool)
 *   - Below: Nothing (consensus is the foundation)
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_CONSENSUS_ENGINE_H
#define ECHO_CONSENSUS_ENGINE_H

#include "block.h"
#include "block_validate.h"
#include "chainstate.h"
#include "echo_types.h"
#include "script.h"
#include "tx.h"
#include "tx_validate.h"
#include "utxo.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * ============================================================================
 * CONSENSUS ENGINE CONFIGURATION
 * ============================================================================
 */

/*
 * Consensus flags controlling validation behavior.
 * These are compile-time constants representing Bitcoin's consensus rules.
 */

/* BIP-16 (P2SH) activation height */
#ifndef CONSENSUS_BIP16_HEIGHT
#define CONSENSUS_BIP16_HEIGHT 173805
#endif

/* BIP-34 (height in coinbase) activation height */
#ifndef CONSENSUS_BIP34_HEIGHT
#define CONSENSUS_BIP34_HEIGHT 227931
#endif

/* BIP-65 (OP_CHECKLOCKTIMEVERIFY) activation height */
#ifndef CONSENSUS_BIP65_HEIGHT
#define CONSENSUS_BIP65_HEIGHT 388381
#endif

/* BIP-66 (strict DER signatures) activation height */
#ifndef CONSENSUS_BIP66_HEIGHT
#define CONSENSUS_BIP66_HEIGHT 363725
#endif

/* BIP-68/112/113 (relative locktimes) activation block */
#ifndef CONSENSUS_CSV_HEIGHT
#define CONSENSUS_CSV_HEIGHT 419328
#endif

/* BIP-141/143/147 (SegWit) activation height */
#ifndef CONSENSUS_SEGWIT_HEIGHT
#define CONSENSUS_SEGWIT_HEIGHT 481824
#endif

/* BIP-341 (Taproot) activation height */
#ifndef CONSENSUS_TAPROOT_HEIGHT
#define CONSENSUS_TAPROOT_HEIGHT 709632
#endif

/*
 * ============================================================================
 * CONSENSUS VALIDATION RESULT
 * ============================================================================
 */

/*
 * Consensus validation error codes.
 * Encompasses both block and transaction validation errors.
 */
typedef enum {
  CONSENSUS_OK = 0,

  /* Block-level errors */
  CONSENSUS_ERR_BLOCK_HEADER,         /* Header validation failed */
  CONSENSUS_ERR_BLOCK_POW,            /* Proof of work failed */
  CONSENSUS_ERR_BLOCK_TIMESTAMP,      /* Timestamp invalid */
  CONSENSUS_ERR_BLOCK_DIFFICULTY,     /* Difficulty mismatch */
  CONSENSUS_ERR_BLOCK_SIZE,           /* Block size/weight exceeded */
  CONSENSUS_ERR_BLOCK_MERKLE,         /* Merkle root mismatch */
  CONSENSUS_ERR_BLOCK_NO_COINBASE,    /* Missing coinbase */
  CONSENSUS_ERR_BLOCK_MULTI_COINBASE, /* Multiple coinbases */
  CONSENSUS_ERR_BLOCK_TX_ORDER,       /* Coinbase not first */
  CONSENSUS_ERR_BLOCK_DUPLICATE_TX,   /* Duplicate transaction */
  CONSENSUS_ERR_BLOCK_WITNESS,        /* Witness commitment invalid */
  CONSENSUS_ERR_BLOCK_COINBASE,       /* Coinbase validation failed */

  /* Transaction-level errors */
  CONSENSUS_ERR_TX_SYNTAX,            /* Transaction malformed */
  CONSENSUS_ERR_TX_SCRIPT,            /* Script execution failed */
  CONSENSUS_ERR_TX_MISSING_INPUT,     /* Input UTXO not found */
  CONSENSUS_ERR_TX_SPENT_INPUT,       /* Input already spent */
  CONSENSUS_ERR_TX_IMMATURE_COINBASE, /* Spending immature coinbase */
  CONSENSUS_ERR_TX_VALUE_MISMATCH,    /* Output value > input value */
  CONSENSUS_ERR_TX_LOCKTIME,          /* Locktime not satisfied */

  /* Chain state errors */
  CONSENSUS_ERR_INVALID_PREV,  /* Previous block unknown/invalid */
  CONSENSUS_ERR_REORG_FAILED,  /* Reorganization failed */
  CONSENSUS_ERR_UTXO_CONFLICT, /* UTXO set inconsistency */

  /* Internal errors */
  CONSENSUS_ERR_INTERNAL, /* Internal error */
  CONSENSUS_ERR_NOMEM,    /* Out of memory */

} consensus_error_t;

/*
 * Detailed consensus validation result.
 */
typedef struct {
  /* Overall validation result */
  consensus_error_t error;

  /* Index of failing item (transaction or input) */
  size_t failing_index;

  /* Sub-index for input within failing transaction */
  size_t failing_input_index;

  /* Block validation detail (if block error) */
  block_validation_error_t block_error;

  /* Transaction validation detail (if tx error) */
  tx_validate_error_t tx_error;

  /* Script error detail (if script error) */
  script_error_t script_error;

} consensus_result_t;

/*
 * Initialize a consensus result to success state.
 *
 * Parameters:
 *   result - Result to initialize
 */
void consensus_result_init(consensus_result_t *result);

/*
 * Get a human-readable string for a consensus error.
 *
 * Parameters:
 *   error - Error code
 *
 * Returns:
 *   Static string describing the error
 */
const char *consensus_error_str(consensus_error_t error);

/*
 * ============================================================================
 * CONSENSUS ENGINE STATE
 * ============================================================================
 */

/*
 * Consensus engine: the complete validated state.
 *
 * The consensus engine encapsulates:
 *   - Chain state (current tip, accumulated work)
 *   - UTXO set (all spendable outputs)
 *   - Block index (all known headers for fork tracking)
 *
 * This is an opaque structure; all access is through functions.
 */
typedef struct consensus_engine consensus_engine_t;

/*
 * Create a new consensus engine initialized to genesis.
 *
 * Returns:
 *   Newly allocated consensus engine, or NULL on failure
 */
consensus_engine_t *consensus_engine_create(void);

/*
 * Destroy a consensus engine and free all associated memory.
 *
 * Parameters:
 *   engine - Engine to destroy (may be NULL)
 */
void consensus_engine_destroy(consensus_engine_t *engine);

/*
 * ============================================================================
 * CHAIN TIP QUERIES
 * ============================================================================
 */

/*
 * Get the current chain tip information.
 *
 * Parameters:
 *   engine - The consensus engine
 *   tip    - Output: chain tip information
 *
 * Returns:
 *   ECHO_OK on success
 */
echo_result_t consensus_get_chain_tip(const consensus_engine_t *engine,
                                      chain_tip_t *tip);

/*
 * Get the current chain height.
 *
 * Parameters:
 *   engine - The consensus engine
 *
 * Returns:
 *   Current height (0 for genesis only, UINT32_MAX if no blocks)
 */
uint32_t consensus_get_height(const consensus_engine_t *engine);

/*
 * Get the block hash at a specific height.
 *
 * Parameters:
 *   engine - The consensus engine
 *   height - Height to query
 *   hash   - Output: block hash
 *
 * Returns:
 *   ECHO_OK on success, ECHO_ERR_NOT_FOUND if height > tip
 */
echo_result_t consensus_get_block_hash(const consensus_engine_t *engine,
                                       uint32_t height, hash256_t *hash);

/*
 * Check if a block hash is on the main chain.
 *
 * Parameters:
 *   engine - The consensus engine
 *   hash   - Block hash to check
 *
 * Returns:
 *   true if on main chain, false otherwise
 */
bool consensus_is_main_chain(const consensus_engine_t *engine,
                             const hash256_t *hash);

/*
 * ============================================================================
 * UTXO QUERIES
 * ============================================================================
 */

/*
 * Lookup a UTXO by outpoint.
 *
 * Parameters:
 *   engine   - The consensus engine
 *   outpoint - Outpoint to lookup
 *
 * Returns:
 *   Pointer to UTXO entry if found, NULL otherwise
 */
const utxo_entry_t *consensus_lookup_utxo(const consensus_engine_t *engine,
                                          const outpoint_t *outpoint);

/*
 * Check if a UTXO exists.
 *
 * Parameters:
 *   engine   - The consensus engine
 *   outpoint - Outpoint to check
 *
 * Returns:
 *   true if UTXO exists, false otherwise
 */
bool consensus_utxo_exists(const consensus_engine_t *engine,
                           const outpoint_t *outpoint);

/*
 * Get the number of UTXOs in the set.
 *
 * Parameters:
 *   engine - The consensus engine
 *
 * Returns:
 *   Number of UTXOs
 */
size_t consensus_utxo_count(const consensus_engine_t *engine);

/*
 * ============================================================================
 * BLOCK INDEX QUERIES
 * ============================================================================
 */

/*
 * Lookup a block index by hash.
 *
 * Parameters:
 *   engine - The consensus engine
 *   hash   - Block hash to lookup
 *
 * Returns:
 *   Pointer to block index if found, NULL otherwise
 */
const block_index_t *
consensus_lookup_block_index(const consensus_engine_t *engine,
                             const hash256_t *hash);

/*
 * Get the number of known block headers.
 *
 * Parameters:
 *   engine - The consensus engine
 *
 * Returns:
 *   Number of known headers
 */
size_t consensus_block_index_count(const consensus_engine_t *engine);

/*
 * Get the block index with the most accumulated work.
 *
 * Parameters:
 *   engine - The consensus engine
 *
 * Returns:
 *   Pointer to best block index, or NULL if none
 */
const block_index_t *
consensus_get_best_block_index(const consensus_engine_t *engine);

/*
 * ============================================================================
 * PURE VALIDATION FUNCTIONS
 * ============================================================================
 *
 * These functions perform validation as pure computation.
 * They take bytes/structures and context, return validity.
 * They do NOT modify the consensus state.
 */

/*
 * Validate a block header (without full block data).
 *
 * Checks:
 *   - Proof of work
 *   - Timestamp (MTP and future limit)
 *   - Previous block reference
 *   - Version field
 *   - Difficulty target (if context provided)
 *
 * Parameters:
 *   engine - The consensus engine (for context)
 *   header - Block header to validate
 *   result - Output: detailed validation result
 *
 * Returns:
 *   true if header is valid, false otherwise
 */
bool consensus_validate_header(const consensus_engine_t *engine,
                               const block_header_t *header,
                               consensus_result_t *result);

/*
 * Validate a block header with pre-computed hash.
 *
 * Same as consensus_validate_header but uses a pre-computed hash
 * to avoid redundant SHA256d computation during header sync.
 *
 * Parameters:
 *   engine - Consensus engine
 *   header - Block header to validate
 *   hash   - Pre-computed block hash (NULL to compute internally)
 *   result - Output: detailed validation result
 *
 * Returns:
 *   true if header is valid, false otherwise
 */
bool consensus_validate_header_with_hash(const consensus_engine_t *engine,
                                         const block_header_t *header,
                                         const hash256_t *hash,
                                         consensus_result_t *result);

/*
 * Validate a complete block (pure function).
 *
 * This is the main validation entry point. It performs:
 *   1. Header validation
 *   2. Block structure validation
 *   3. Merkle root verification
 *   4. Coinbase validation
 *   5. All transaction validation
 *   6. Script execution for all inputs
 *   7. UTXO availability checks
 *   8. Witness commitment verification
 *
 * This is a PURE function—it does not modify the engine state.
 * Use consensus_apply_block() to commit a validated block.
 *
 * Parameters:
 *   engine - The consensus engine (for UTXO lookups)
 *   block  - Block to validate
 *   result - Output: detailed validation result
 *
 * Returns:
 *   true if block is valid, false otherwise
 */
bool consensus_validate_block(const consensus_engine_t *engine,
                              const block_t *block, consensus_result_t *result);

/*
 * Validate a transaction against the current UTXO set.
 *
 * This validates:
 *   - Syntactic validity
 *   - All inputs exist in UTXO set
 *   - No coinbase input immaturity
 *   - Script execution succeeds for all inputs
 *   - Input value >= output value
 *   - Locktime satisfied
 *
 * Parameters:
 *   engine       - The consensus engine
 *   tx           - Transaction to validate
 *   block_height - Current block height (for locktime)
 *   block_time   - Current block time (for locktime)
 *   result       - Output: detailed validation result
 *
 * Returns:
 *   true if transaction is valid, false otherwise
 */
bool consensus_validate_tx(const consensus_engine_t *engine, const tx_t *tx,
                           uint32_t block_height, uint32_t block_time,
                           consensus_result_t *result);

/*
 * ============================================================================
 * STATE MODIFICATION FUNCTIONS
 * ============================================================================
 *
 * These functions modify the consensus state.
 * They should only be called with pre-validated data.
 */

/*
 * Add a block header to the index (without applying).
 *
 * This registers a header for chain selection but does NOT:
 *   - Validate the header (caller must validate first)
 *   - Update the UTXO set
 *   - Change the active chain tip
 *
 * Use this for headers-first sync or orphan tracking.
 *
 * Parameters:
 *   engine    - The consensus engine
 *   header    - Header to add
 *   index_out - Output: created block index (may be NULL)
 *
 * Returns:
 *   ECHO_OK on success, ECHO_ERR_EXISTS if already known
 */
echo_result_t consensus_add_header(consensus_engine_t *engine,
                                   const block_header_t *header,
                                   block_index_t **index_out);

/*
 * Apply a validated block to the chain state.
 *
 * This function:
 *   1. Adds the block to the index (if not already present)
 *   2. Updates the UTXO set (remove spent, add created)
 *   3. Updates the chain tip
 *   4. Stores undo data for potential reorganization
 *
 * The block MUST have been validated first via consensus_validate_block().
 * Applying an invalid block results in undefined behavior.
 *
 * Parameters:
 *   engine - The consensus engine
 *   block  - Block to apply (must be validated)
 *   result - Output: detailed result (may be NULL)
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t consensus_apply_block(consensus_engine_t *engine,
                                    const block_t *block,
                                    consensus_result_t *result);

/*
 * Check if a block would trigger a chain reorganization.
 *
 * Returns true if the block (when applied) would have more accumulated
 * work than the current tip, requiring blocks to be disconnected.
 *
 * Parameters:
 *   engine - The consensus engine
 *   header - Block header to check
 *
 * Returns:
 *   true if reorg needed, false if extends current tip or less work
 */
bool consensus_would_reorg(const consensus_engine_t *engine,
                           const block_header_t *header);

/*
 * Perform a chain reorganization.
 *
 * This function:
 *   1. Identifies the common ancestor with the new chain
 *   2. Reverts blocks from current tip to ancestor
 *   3. Applies blocks from ancestor to new tip
 *
 * Requires a callback to retrieve full block data for blocks being applied.
 *
 * Parameters:
 *   engine        - The consensus engine
 *   new_tip_hash  - Hash of the new chain tip
 *   get_block     - Callback to get block data
 *   user_data     - User data for callback
 *   result        - Output: detailed result
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
typedef echo_result_t (*consensus_get_block_fn)(const hash256_t *block_hash,
                                                block_t *block_out,
                                                void *user_data);

echo_result_t consensus_reorganize(consensus_engine_t *engine,
                                   const hash256_t *new_tip_hash,
                                   consensus_get_block_fn get_block,
                                   void *user_data, consensus_result_t *result);

/*
 * ============================================================================
 * VALIDATION CONTEXT BUILDING
 * ============================================================================
 *
 * Helper functions to build validation contexts from consensus state.
 */

/*
 * Build a block validation context for a block at a given height.
 *
 * Populates the context with:
 *   - Parent hash and validity
 *   - Previous timestamps for MTP
 *   - Expected difficulty bits
 *
 * Parameters:
 *   engine - The consensus engine
 *   height - Height of the block being validated
 *   ctx    - Output: validation context
 *
 * Returns:
 *   ECHO_OK on success
 */
echo_result_t consensus_build_validation_ctx(const consensus_engine_t *engine,
                                             uint32_t height,
                                             full_block_ctx_t *ctx);

/*
 * Build a transaction validation context.
 *
 * Populates the context with:
 *   - UTXO information for all inputs
 *   - Block height and time
 *   - Script verification flags
 *
 * Parameters:
 *   engine       - The consensus engine
 *   tx           - Transaction to build context for
 *   block_height - Block height for validation
 *   block_time   - Block time for validation
 *   ctx          - Output: validation context
 *
 * Returns:
 *   ECHO_OK on success, ECHO_ERR_NOT_FOUND if UTXO missing
 */
echo_result_t consensus_build_tx_ctx(const consensus_engine_t *engine,
                                     const tx_t *tx, uint32_t block_height,
                                     uint32_t block_time,
                                     tx_validate_ctx_t *ctx);

/*
 * Free resources allocated by consensus_build_tx_ctx.
 *
 * Parameters:
 *   ctx - Context to clean up
 */
void consensus_free_tx_ctx(tx_validate_ctx_t *ctx);

/*
 * ============================================================================
 * SCRIPT VERIFICATION FLAGS
 * ============================================================================
 *
 * Get the appropriate script verification flags for a given block height.
 */

/*
 * Get script verification flags for a block at a given height.
 *
 * Returns the combination of SCRIPT_VERIFY_* flags that should be
 * applied based on which soft forks are active at that height.
 *
 * Parameters:
 *   height - Block height
 *
 * Returns:
 *   Bitfield of script verification flags
 */
uint32_t consensus_get_script_flags(uint32_t height);

/*
 * ============================================================================
 * STATISTICS AND DEBUGGING
 * ============================================================================
 */

/*
 * Consensus engine statistics.
 */
typedef struct {
  uint32_t height;          /* Current chain height */
  work256_t total_work;     /* Cumulative chain work */
  size_t utxo_count;        /* Number of UTXOs */
  size_t block_index_count; /* Number of known headers */
  int64_t total_coins;      /* Total coins in existence (satoshis) */
} consensus_stats_t;

/*
 * Get consensus engine statistics.
 *
 * Parameters:
 *   engine - The consensus engine
 *   stats  - Output: statistics
 */
void consensus_get_stats(const consensus_engine_t *engine,
                         consensus_stats_t *stats);

/*
 * Get the underlying chain state (for advanced use).
 *
 * This provides direct access to the chain state for operations
 * not exposed through the consensus API. Use with caution.
 *
 * Parameters:
 *   engine - The consensus engine
 *
 * Returns:
 *   Pointer to chain state (non-const for modification)
 */
chainstate_t *consensus_get_chainstate(consensus_engine_t *engine);

/*
 * Get the underlying UTXO set (read-only).
 *
 * Parameters:
 *   engine - The consensus engine
 *
 * Returns:
 *   Pointer to UTXO set
 */
const utxo_set_t *consensus_get_utxo_set(const consensus_engine_t *engine);

#endif /* ECHO_CONSENSUS_ENGINE_H */
