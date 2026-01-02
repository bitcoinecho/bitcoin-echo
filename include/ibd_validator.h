/*
 * Bitcoin Echo — IBD Chunk Validator
 *
 * Validates consecutive chunks of blocks during Initial Block Download.
 * Part of the decoupled IBD architecture where downloads and validation
 * are separate phases.
 *
 * Key insight: During IBD, we don't need per-block undo data. If validation
 * fails mid-chunk, we restart from the validated tip. This is simpler and
 * more efficient than maintaining full undo capability.
 *
 * The chunk validator:
 *   1. Loads blocks from disk for a consecutive height range
 *   2. Validates each block (PoW, merkle root, optionally scripts)
 *   3. Tracks UTXO changes across the entire chunk
 *   4. Flushes all changes atomically to the UTXO database
 *
 * Memory model:
 *   - UTXOs created within chunk are kept in memory
 *   - UTXOs spent within chunk (that existed before chunk) are tracked for deletion
 *   - UTXOs created and spent within same chunk cancel out (not written to DB)
 *
 * See IBD-DECOUPLED-ARCHITECTURE.md for the full design.
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_IBD_VALIDATOR_H
#define ECHO_IBD_VALIDATOR_H

#include "block.h"
#include "chainstate.h"
#include "echo_types.h"
#include "utxo.h"
#include "utxo_db.h"
#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
typedef struct node node_t;

/* ============================================================================
 * Constants
 * ============================================================================
 */

/*
 * Maximum blocks to validate in a single chunk.
 *
 * This limits memory usage for UTXO tracking. With average 2000 UTXOs/block
 * and ~100 bytes per UTXO entry, 1000 blocks = ~200MB of UTXO memory.
 *
 * Can be tuned based on available memory.
 */
#define IBD_CHUNK_MAX_BLOCKS 1000

/*
 * Initial capacity for the UTXO batch hash tables.
 * Will grow as needed. Set to handle typical early blocks.
 */
#define IBD_BATCH_INITIAL_CAPACITY 65536

/*
 * Progress logging interval during chunk validation.
 */
#define IBD_PROGRESS_LOG_INTERVAL 100

/* ============================================================================
 * IBD UTXO Batch
 * ============================================================================
 *
 * Tracks UTXO changes across a chunk of blocks for efficient batch flushing.
 *
 * Two types of changes:
 *   1. CREATED: New UTXOs from transaction outputs
 *   2. SPENT: UTXOs consumed by transaction inputs
 *
 * Optimization: UTXOs created and spent within the same chunk are tracked
 * in created_then_spent and never written to the database.
 */

/*
 * UTXO batch for IBD chunk validation.
 *
 * Memory ownership:
 *   - created_utxos: Owned by batch, freed on destroy
 *   - spent_outpoints: Array of outpoints to delete, owned by batch
 */
typedef struct {
  /* UTXOs created by transactions in this chunk */
  utxo_set_t *created_utxos;

  /* Outpoints spent by transactions in this chunk (existing before chunk) */
  outpoint_t *spent_outpoints;
  size_t spent_count;
  size_t spent_capacity;

  /* Count of UTXOs created then spent within same chunk (optimization metric) */
  size_t created_then_spent_count;

  /* Chunk boundaries */
  uint32_t chunk_start_height;
  uint32_t chunk_end_height;

  /* Statistics */
  size_t total_txs_processed;
  size_t total_inputs_processed;
  size_t total_outputs_processed;
} ibd_utxo_batch_t;

/* ============================================================================
 * IBD Validator Context
 * ============================================================================
 *
 * Context for validating a chunk of blocks.
 */

/*
 * Validation result codes.
 */
typedef enum {
  IBD_VALID_OK = 0,            /* Chunk validated successfully */
  IBD_VALID_ERR_LOAD,          /* Failed to load block from disk */
  IBD_VALID_ERR_POW,           /* Proof-of-work check failed */
  IBD_VALID_ERR_MERKLE,        /* Merkle root mismatch */
  IBD_VALID_ERR_STRUCTURE,     /* Block structure invalid */
  IBD_VALID_ERR_UTXO_MISSING,  /* Input references missing UTXO */
  IBD_VALID_ERR_UTXO_DOUBLE,   /* Double-spend detected */
  IBD_VALID_ERR_VALUE,         /* Value accounting error (outputs > inputs) */
  IBD_VALID_ERR_SCRIPT,        /* Script validation failed */
  IBD_VALID_ERR_COINBASE,      /* Coinbase validation failed */
  IBD_VALID_ERR_MEMORY,        /* Memory allocation failed */
  IBD_VALID_ERR_INTERNAL       /* Internal error */
} ibd_valid_result_t;

/*
 * IBD validator context.
 *
 * Created for each chunk validation, destroyed after flush.
 */
typedef struct {
  /* Node for loading blocks and accessing chainstate */
  node_t *node;

  /* UTXO batch tracking changes */
  ibd_utxo_batch_t *batch;

  /* Current chain state for UTXO lookups */
  chainstate_t *chainstate;

  /* UTXO database for lookups of pre-existing UTXOs */
  utxo_db_t *utxo_db;

  /* Chunk range to validate */
  uint32_t start_height;
  uint32_t end_height;

  /* Current position in validation */
  uint32_t current_height;

  /* Validation mode */
  bool skip_script_validation; /* True if below assumevalid height */

  /* Error tracking */
  ibd_valid_result_t last_error;
  uint32_t error_height;
  char error_message[256];
} ibd_validator_t;

/* ============================================================================
 * IBD UTXO Batch API
 * ============================================================================
 */

/*
 * Create a new UTXO batch for IBD chunk validation.
 *
 * Parameters:
 *   start_height - First block height in chunk
 *   end_height   - Last block height in chunk (inclusive)
 *
 * Returns:
 *   Newly allocated batch, or NULL on failure
 */
ibd_utxo_batch_t *ibd_utxo_batch_create(uint32_t start_height,
                                        uint32_t end_height);

/*
 * Destroy a UTXO batch and free all resources.
 */
void ibd_utxo_batch_destroy(ibd_utxo_batch_t *batch);

/*
 * Record a created UTXO in the batch.
 *
 * Parameters:
 *   batch - The batch
 *   entry - UTXO entry to add (will be cloned)
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t ibd_utxo_batch_add_created(ibd_utxo_batch_t *batch,
                                         const utxo_entry_t *entry);

/*
 * Record a spent UTXO in the batch.
 *
 * If the UTXO was created within this same chunk, it's removed from
 * created_utxos instead of being added to spent_outpoints (optimization).
 *
 * Parameters:
 *   batch    - The batch
 *   outpoint - Outpoint being spent
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NOT_FOUND if UTXO doesn't exist (either in batch or pre-existing)
 */
echo_result_t ibd_utxo_batch_add_spent(ibd_utxo_batch_t *batch,
                                       const outpoint_t *outpoint);

/*
 * Look up a UTXO in the batch (for spending).
 *
 * Checks if the UTXO was created within this chunk.
 *
 * Parameters:
 *   batch    - The batch
 *   outpoint - Outpoint to look up
 *
 * Returns:
 *   Pointer to UTXO entry if found in batch, NULL otherwise
 */
const utxo_entry_t *ibd_utxo_batch_lookup(const ibd_utxo_batch_t *batch,
                                          const outpoint_t *outpoint);

/*
 * Get batch statistics.
 *
 * Parameters:
 *   batch             - The batch
 *   created_count     - Output: UTXOs to be inserted
 *   spent_count       - Output: UTXOs to be deleted
 *   cancelled_count   - Output: UTXOs created and spent within chunk
 */
void ibd_utxo_batch_get_stats(const ibd_utxo_batch_t *batch,
                              size_t *created_count, size_t *spent_count,
                              size_t *cancelled_count);

/* ============================================================================
 * IBD Validator API
 * ============================================================================
 */

/*
 * Create an IBD validator for a chunk of blocks.
 *
 * Parameters:
 *   node         - The node (for loading blocks)
 *   chainstate   - Chain state (for UTXO lookups)
 *   utxo_db      - UTXO database (for lookups of pre-existing UTXOs)
 *   start_height - First block to validate (must be validated_tip + 1)
 *   end_height   - Last block to validate (inclusive)
 *   skip_scripts - If true, skip script validation (assumevalid)
 *
 * Returns:
 *   Newly allocated validator, or NULL on failure
 */
ibd_validator_t *ibd_validator_create(node_t *node, chainstate_t *chainstate,
                                      utxo_db_t *utxo_db, uint32_t start_height,
                                      uint32_t end_height, bool skip_scripts);

/*
 * Destroy an IBD validator and free all resources.
 *
 * Note: Does NOT flush the batch to database. Call ibd_validator_flush() first.
 */
void ibd_validator_destroy(ibd_validator_t *validator);

/*
 * Validate the next block in the chunk.
 *
 * Loads the block at current_height, validates it, updates UTXO batch,
 * and advances current_height.
 *
 * Parameters:
 *   validator - The validator
 *
 * Returns:
 *   IBD_VALID_OK if block validated successfully
 *   Error code if validation failed (check error_height, error_message)
 */
ibd_valid_result_t ibd_validator_validate_next(ibd_validator_t *validator);

/*
 * Validate all remaining blocks in the chunk.
 *
 * Convenience function that calls ibd_validator_validate_next() until
 * chunk is complete or an error occurs.
 *
 * Parameters:
 *   validator - The validator
 *
 * Returns:
 *   IBD_VALID_OK if entire chunk validated successfully
 *   Error code if any block failed validation
 */
ibd_valid_result_t ibd_validator_validate_chunk(ibd_validator_t *validator);

/*
 * Flush the UTXO batch to the database atomically.
 *
 * This performs a single SQLite transaction containing:
 *   - All UTXO deletions (spent outputs)
 *   - All UTXO insertions (created outputs)
 *   - Update of validated height metadata
 *
 * Parameters:
 *   validator - The validator (must have completed validation successfully)
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t ibd_validator_flush(ibd_validator_t *validator);

/*
 * Get the current validation progress.
 *
 * Parameters:
 *   validator      - The validator
 *   current_height - Output: current block being validated
 *   total_blocks   - Output: total blocks in chunk
 *   blocks_done    - Output: blocks validated so far
 */
void ibd_validator_get_progress(const ibd_validator_t *validator,
                                uint32_t *current_height, uint32_t *total_blocks,
                                uint32_t *blocks_done);

/*
 * Check if validation is complete.
 *
 * Returns:
 *   true if all blocks in chunk have been validated
 */
bool ibd_validator_is_complete(const ibd_validator_t *validator);

/*
 * Get the last error information.
 *
 * Parameters:
 *   validator - The validator
 *   height    - Output: height where error occurred
 *   message   - Output: error message (may be NULL)
 *
 * Returns:
 *   Last error code
 */
ibd_valid_result_t ibd_validator_get_error(const ibd_validator_t *validator,
                                           uint32_t *height,
                                           const char **message);

/*
 * Get human-readable string for validation result.
 */
const char *ibd_valid_result_string(ibd_valid_result_t result);

#endif /* ECHO_IBD_VALIDATOR_H */
