/*
 * Bitcoin Echo — Block Index Database Interface
 *
 * Persistent storage for block headers and chain metadata using SQLite.
 * Implements the schema specified in the whitepaper §6.3.
 *
 * The block index database stores all known block headers and supports:
 * - Fast lookup by block hash
 * - Fast lookup by block height
 * - Chain traversal (next/prev block lookups)
 * - Chain comparison (accumulated proof-of-work)
 * - Reorganization support
 *
 * Schema (per whitepaper §6.3):
 *   CREATE TABLE blocks (
 *       hash        BLOB PRIMARY KEY,   -- 32 bytes
 *       height      INTEGER NOT NULL,
 *       header      BLOB NOT NULL,      -- 80 bytes
 *       chainwork   BLOB NOT NULL,      -- 32 bytes, big-endian
 *       status      INTEGER NOT NULL    -- validation status flags
 *   );
 *   CREATE INDEX idx_height ON blocks(height);
 *   CREATE INDEX idx_chainwork ON blocks(chainwork);
 *
 * All database changes occur within transactions for atomicity.
 * Block index updates coordinate with UTXO database updates to ensure
 * consistent chain state.
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_BLOCK_INDEX_DB_H
#define ECHO_BLOCK_INDEX_DB_H

#include "block.h"
#include "chainstate.h"
#include "db.h"
#include "echo_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Block validation status flags.
 * These track the validation state of each block header.
 */
typedef enum {
  BLOCK_STATUS_VALID_HEADER = 0x01,  /* Header has been validated */
  BLOCK_STATUS_VALID_TREE = 0x02,    /* All ancestors are valid */
  BLOCK_STATUS_VALID_SCRIPTS = 0x04, /* All scripts have been validated */
  BLOCK_STATUS_VALID_CHAIN = 0x08,   /* Block is part of best chain */
  BLOCK_STATUS_HAVE_DATA = 0x10,     /* Full block data is stored */
  BLOCK_STATUS_FAILED = 0x20,        /* Block validation failed */
  BLOCK_STATUS_PRUNED = 0x40,        /* Block data has been pruned (Session 9.6.2) */
} block_status_flags_t;

/*
 * Block index entry.
 * Represents all metadata for a block header stored in the database.
 */
typedef struct {
  hash256_t hash;        /* Block hash (32 bytes) */
  uint32_t height;       /* Block height in chain */
  block_header_t header; /* Full 80-byte header */
  work256_t chainwork;   /* Accumulated proof-of-work (32 bytes) */
  uint32_t status;       /* Validation status flags */
  int32_t data_file;     /* Block data file index (-1 = not stored) */
  uint32_t data_pos;     /* Byte offset within file */
} block_index_entry_t;

/*
 * Block index database handle.
 * Wraps a SQLite database configured for block index storage.
 */
typedef struct {
  db_t db;                        /* Underlying database handle */
  db_stmt_t lookup_hash_stmt;     /* Prepared statement for lookup by hash */
  db_stmt_t lookup_height_stmt;   /* Prepared statement for lookup by height */
  db_stmt_t insert_stmt;          /* Prepared statement for inserts */
  db_stmt_t update_status_stmt;   /* Prepared statement for status updates */
  db_stmt_t update_data_pos_stmt; /* Prepared statement for data position */
  db_stmt_t best_chain_stmt;      /* Prepared statement for best chain query */
  bool stmts_prepared;            /* Whether statements are prepared */
} block_index_db_t;

/* ========================================================================
 * Database Lifecycle
 * ======================================================================== */

/**
 * Open or create a block index database.
 *
 * Parameters:
 *   bdb  - Block index database handle to initialize
 *   path - Path to database file (will be created if doesn't exist)
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Creates the database file and schema if needed
 *   - Creates indexes for efficient lookups
 *   - Prepares commonly-used statements for efficiency
 *   - Enables WAL mode via the underlying db interface
 */
echo_result_t block_index_db_open(block_index_db_t *bdb, const char *path);

/**
 * Close a block index database.
 *
 * Parameters:
 *   bdb - Block index database handle to close
 *
 * Notes:
 *   - Finalizes all prepared statements
 *   - Closes the underlying database
 *   - Safe to call on already-closed database
 */
void block_index_db_close(block_index_db_t *bdb);

/**
 * Begin a transaction for batch operations.
 *
 * Parameters:
 *   bdb - Block index database handle
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Use for batching multiple inserts (e.g., header sync)
 *   - Call block_index_db_commit() to commit changes
 */
echo_result_t block_index_db_begin(block_index_db_t *bdb);

/**
 * Commit a transaction.
 *
 * Parameters:
 *   bdb - Block index database handle
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t block_index_db_commit(block_index_db_t *bdb);

/* ========================================================================
 * Block Index Operations
 * ======================================================================== */

/**
 * Lookup a block by its hash.
 *
 * Parameters:
 *   bdb   - Block index database handle
 *   hash  - Block hash to lookup
 *   entry - Output: block index entry (populated on success)
 *
 * Returns:
 *   ECHO_OK if found, ECHO_ERR_NOT_FOUND if not found, error code on failure
 *
 * Notes:
 *   - Entry is populated with all block metadata
 *   - No dynamic memory allocation; entry is stack-allocated by caller
 */
echo_result_t block_index_db_lookup_by_hash(block_index_db_t *bdb,
                                            const hash256_t *hash,
                                            block_index_entry_t *entry);

/**
 * Lookup a block by its height.
 *
 * Parameters:
 *   bdb    - Block index database handle
 *   height - Block height to lookup
 *   entry  - Output: block index entry (populated on success)
 *
 * Returns:
 *   ECHO_OK if found, ECHO_ERR_NOT_FOUND if not found, error code on failure
 *
 * Notes:
 *   - If multiple blocks exist at this height (during reorg), returns one
 *   - For deterministic results, use the best chain query instead
 */
echo_result_t block_index_db_lookup_by_height(block_index_db_t *bdb,
                                              uint32_t height,
                                              block_index_entry_t *entry);

/**
 * Check if a block exists in the database.
 *
 * Parameters:
 *   bdb    - Block index database handle
 *   hash   - Block hash to check
 *   exists - Output: true if exists, false otherwise
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t block_index_db_exists(block_index_db_t *bdb,
                                    const hash256_t *hash, bool *exists);

/**
 * Insert a block into the index.
 *
 * Parameters:
 *   bdb   - Block index database handle
 *   entry - Block index entry to insert
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Should be called within a transaction for atomicity
 *   - Will fail if block hash already exists
 */
echo_result_t block_index_db_insert(block_index_db_t *bdb,
                                    const block_index_entry_t *entry);

/**
 * Update the status flags of a block.
 *
 * Parameters:
 *   bdb    - Block index database handle
 *   hash   - Block hash to update
 *   status - New status flags
 *
 * Returns:
 *   ECHO_OK on success, ECHO_ERR_NOT_FOUND if not found, error code on failure
 *
 * Notes:
 *   - Should be called within a transaction for atomicity
 *   - Replaces existing status flags (does not OR them)
 */
echo_result_t block_index_db_update_status(block_index_db_t *bdb,
                                           const hash256_t *hash,
                                           uint32_t status);

/**
 * Update the block data file position.
 *
 * Called when a block is stored to disk to record where it was written.
 *
 * Parameters:
 *   bdb       - Block index database handle
 *   hash      - Block hash to update
 *   data_file - File index (blk*.dat number)
 *   data_pos  - Byte offset within file
 *
 * Returns:
 *   ECHO_OK on success, ECHO_ERR_NOT_FOUND if not found, error code on failure
 */
echo_result_t block_index_db_update_data_pos(block_index_db_t *bdb,
                                             const hash256_t *hash,
                                             uint32_t data_file,
                                             uint32_t data_pos);

/* ========================================================================
 * Chain Queries
 * ======================================================================== */

/**
 * Get the block with the most accumulated work.
 * This represents the tip of the best chain.
 *
 * Parameters:
 *   bdb   - Block index database handle
 *   entry - Output: block index entry with highest chainwork
 *
 * Returns:
 *   ECHO_OK if found, ECHO_ERR_NOT_FOUND if database is empty, error on failure
 *
 * Notes:
 *   - This is the primary method for determining chain tip
 *   - Ties are broken by first-seen (database insertion order)
 */
echo_result_t block_index_db_get_best_chain(block_index_db_t *bdb,
                                            block_index_entry_t *entry);

/**
 * Get the block at a specific height on the best chain.
 *
 * Parameters:
 *   bdb    - Block index database handle
 *   height - Block height to query
 *   entry  - Output: block index entry at this height on best chain
 *
 * Returns:
 *   ECHO_OK if found, ECHO_ERR_NOT_FOUND if height exceeds tip, error on
 * failure
 *
 * Notes:
 *   - Returns only blocks marked with BLOCK_STATUS_VALID_CHAIN
 *   - More reliable than lookup_by_height during reorganizations
 */
echo_result_t block_index_db_get_chain_block(block_index_db_t *bdb,
                                             uint32_t height,
                                             block_index_entry_t *entry);

/**
 * Get the previous block (parent) of a given block.
 *
 * Parameters:
 *   bdb      - Block index database handle
 *   hash     - Block hash to get parent of
 *   entry    - Output: parent block index entry
 *
 * Returns:
 *   ECHO_OK if found, ECHO_ERR_NOT_FOUND if parent not found, error on failure
 *
 * Notes:
 *   - Looks up parent by reading prev_hash from header
 *   - Useful for chain traversal and reorganization
 */
echo_result_t block_index_db_get_prev(block_index_db_t *bdb,
                                      const hash256_t *hash,
                                      block_index_entry_t *entry);

/**
 * Find common ancestor of two blocks.
 *
 * Parameters:
 *   bdb      - Block index database handle
 *   hash_a   - First block hash
 *   hash_b   - Second block hash
 *   ancestor - Output: common ancestor block entry
 *
 * Returns:
 *   ECHO_OK if found, ECHO_ERR_NOT_FOUND if no common ancestor, error on
 * failure
 *
 * Notes:
 *   - Critical for reorganization planning
 *   - Walks both chains backward until common block is found
 */
echo_result_t block_index_db_find_common_ancestor(
    block_index_db_t *bdb, const hash256_t *hash_a, const hash256_t *hash_b,
    block_index_entry_t *ancestor);

/* ========================================================================
 * Batch Operations
 * ======================================================================== */

/**
 * Mark a chain of blocks as the best chain.
 * Updates status flags to set BLOCK_STATUS_VALID_CHAIN.
 *
 * Parameters:
 *   bdb    - Block index database handle
 *   hashes - Array of block hashes to mark as best chain
 *   count  - Number of hashes in array
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Should be called within a transaction
 *   - Typically used after reorganization to update chain flags
 */
echo_result_t block_index_db_mark_best_chain(block_index_db_t *bdb,
                                             const hash256_t *hashes,
                                             size_t count);

/**
 * Unmark blocks as being on the best chain.
 * Clears BLOCK_STATUS_VALID_CHAIN flag.
 *
 * Parameters:
 *   bdb    - Block index database handle
 *   hashes - Array of block hashes to unmark
 *   count  - Number of hashes in array
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Should be called within a transaction
 *   - Used during reorganization to unmark old chain
 */
echo_result_t block_index_db_unmark_best_chain(block_index_db_t *bdb,
                                               const hash256_t *hashes,
                                               size_t count);

/* ========================================================================
 * Statistics and Queries
 * ======================================================================== */

/**
 * Get the total number of blocks in the database.
 *
 * Parameters:
 *   bdb   - Block index database handle
 *   count - Output: number of blocks
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t block_index_db_count(block_index_db_t *bdb, size_t *count);

/**
 * Get the height of the best chain.
 *
 * Parameters:
 *   bdb    - Block index database handle
 *   height - Output: height of best chain tip
 *
 * Returns:
 *   ECHO_OK on success, ECHO_ERR_NOT_FOUND if empty, error on failure
 */
echo_result_t block_index_db_get_height(block_index_db_t *bdb,
                                        uint32_t *height);

/**
 * Get total accumulated work on the best chain.
 *
 * Parameters:
 *   bdb       - Block index database handle
 *   chainwork - Output: accumulated work of best chain
 *
 * Returns:
 *   ECHO_OK on success, ECHO_ERR_NOT_FOUND if empty, error on failure
 */
echo_result_t block_index_db_get_chainwork(block_index_db_t *bdb,
                                           work256_t *chainwork);

/* ========================================================================
 * Pruning Operations (Session 9.6.2)
 * ======================================================================== */

/**
 * Mark blocks as pruned.
 *
 * Sets the BLOCK_STATUS_PRUNED flag and clears BLOCK_STATUS_HAVE_DATA
 * for blocks in the specified height range.
 *
 * Parameters:
 *   bdb         - Block index database handle
 *   start_height - Start of height range (inclusive)
 *   end_height   - End of height range (exclusive)
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t block_index_db_mark_pruned(block_index_db_t *bdb,
                                         uint32_t start_height,
                                         uint32_t end_height);

/**
 * Get the lowest block height with available data.
 *
 * Returns the height of the lowest block that has BLOCK_STATUS_HAVE_DATA
 * set and BLOCK_STATUS_PRUNED cleared.
 *
 * Parameters:
 *   bdb    - Block index database handle
 *   height - Output: lowest unpruned block height (0 if none pruned)
 *
 * Returns:
 *   ECHO_OK on success, ECHO_ERR_NOT_FOUND if database is empty
 */
echo_result_t block_index_db_get_pruned_height(block_index_db_t *bdb,
                                               uint32_t *height);

/**
 * Check if a specific block has been pruned.
 *
 * Parameters:
 *   bdb     - Block index database handle
 *   hash    - Block hash to check
 *   pruned  - Output: true if block is pruned, false otherwise
 *
 * Returns:
 *   ECHO_OK on success, ECHO_ERR_NOT_FOUND if block not in index
 */
echo_result_t block_index_db_is_pruned(block_index_db_t *bdb,
                                       const hash256_t *hash,
                                       bool *pruned);

#endif /* ECHO_BLOCK_INDEX_DB_H */
