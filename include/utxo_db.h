/*
 * Bitcoin Echo — UTXO Database Interface
 *
 * Persistent storage for the Unspent Transaction Output (UTXO) set using
 * SQLite. Implements the schema specified in the whitepaper §6.2.
 *
 * The UTXO database stores all spendable outputs and supports:
 * - Fast lookup by outpoint (txid + vout)
 * - Batch insertion of new outputs
 * - Batch deletion of spent outputs
 * - Atomic updates within SQLite transactions
 *
 * All database changes for a single block occur within a single transaction,
 * guaranteeing consistency even if the process is terminated mid-operation.
 *
 * Schema (per whitepaper §6.2):
 *   CREATE TABLE utxo (
 *       outpoint    BLOB PRIMARY KEY,  -- 36 bytes: txid (32) + vout (4)
 *       value       INTEGER NOT NULL,   -- satoshis
 *       script      BLOB NOT NULL,      -- scriptPubKey
 *       height      INTEGER NOT NULL,   -- block height when created
 *       coinbase    INTEGER NOT NULL    -- 1 if from coinbase, else 0
 *   );
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_UTXO_DB_H
#define ECHO_UTXO_DB_H

#include "db.h"
#include "echo_types.h"
#include "tx.h"
#include "utxo.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * UTXO database handle.
 * Wraps a SQLite database configured for UTXO storage.
 */
typedef struct {
  db_t db;               /* Underlying database handle */
  db_stmt_t lookup_stmt; /* Prepared statement for lookups */
  db_stmt_t insert_stmt; /* Prepared statement for inserts */
  db_stmt_t delete_stmt; /* Prepared statement for deletes */
  bool stmts_prepared;   /* Whether statements are prepared */
} utxo_db_t;

/* ========================================================================
 * Database Lifecycle
 * ======================================================================== */

/**
 * Open or create a UTXO database.
 *
 * Parameters:
 *   udb  - UTXO database handle to initialize
 *   path - Path to database file (will be created if doesn't exist)
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Creates the database file and schema if needed
 *   - Prepares commonly-used statements for efficiency
 *   - Enables WAL mode via the underlying db interface
 */
echo_result_t utxo_db_open(utxo_db_t *udb, const char *path);

/**
 * Close a UTXO database.
 *
 * Parameters:
 *   udb - UTXO database handle to close
 *
 * Notes:
 *   - Finalizes all prepared statements
 *   - Closes the underlying database
 *   - Safe to call on already-closed database
 */
void utxo_db_close(utxo_db_t *udb);

/* ========================================================================
 * UTXO Operations
 * ======================================================================== */

/**
 * Lookup a UTXO by outpoint.
 *
 * Parameters:
 *   udb      - UTXO database handle
 *   outpoint - The outpoint to lookup
 *   entry    - Output: UTXO entry (caller must free with utxo_entry_destroy)
 *
 * Returns:
 *   ECHO_OK if found, ECHO_ERR_NOT_FOUND if not found, error code on failure
 *
 * Notes:
 *   - On success, allocates and populates entry
 *   - Caller is responsible for freeing the entry
 */
echo_result_t utxo_db_lookup(utxo_db_t *udb, const outpoint_t *outpoint,
                             utxo_entry_t **entry);

/**
 * Check if a UTXO exists in the database.
 *
 * Parameters:
 *   udb      - UTXO database handle
 *   outpoint - The outpoint to check
 *   exists   - Output: true if exists, false otherwise
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t utxo_db_exists(utxo_db_t *udb, const outpoint_t *outpoint,
                             bool *exists);

/**
 * Insert a single UTXO into the database.
 *
 * Parameters:
 *   udb   - UTXO database handle
 *   entry - The UTXO entry to insert
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Should be called within a transaction for atomicity
 *   - Will fail if outpoint already exists
 */
echo_result_t utxo_db_insert(utxo_db_t *udb, const utxo_entry_t *entry);

/**
 * Delete a single UTXO from the database.
 *
 * Parameters:
 *   udb      - UTXO database handle
 *   outpoint - The outpoint to delete
 *
 * Returns:
 *   ECHO_OK on success, ECHO_ERR_NOT_FOUND if not found, error code on failure
 *
 * Notes:
 *   - Should be called within a transaction for atomicity
 */
echo_result_t utxo_db_delete(utxo_db_t *udb, const outpoint_t *outpoint);

/* ========================================================================
 * Batch Operations
 * ======================================================================== */

/**
 * Insert multiple UTXOs in a batch.
 *
 * Parameters:
 *   udb     - UTXO database handle
 *   entries - Array of UTXO entries to insert
 *   count   - Number of entries in array
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Should be called within a transaction for atomicity
 *   - All insertions succeed or all fail (within transaction)
 */
echo_result_t utxo_db_insert_batch(utxo_db_t *udb, const utxo_entry_t **entries,
                                   size_t count);

/**
 * Delete multiple UTXOs in a batch.
 *
 * Parameters:
 *   udb       - UTXO database handle
 *   outpoints - Array of outpoints to delete
 *   count     - Number of outpoints in array
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Should be called within a transaction for atomicity
 *   - All deletions succeed or all fail (within transaction)
 *   - It's not an error if some outpoints don't exist
 */
echo_result_t utxo_db_delete_batch(utxo_db_t *udb, const outpoint_t *outpoints,
                                   size_t count);

/* ========================================================================
 * Block Application
 * ======================================================================== */

/**
 * Apply a block's changes to the UTXO set atomically.
 * This combines insertions (new outputs) and deletions (spent outputs)
 * in a single transaction.
 *
 * Parameters:
 *   udb         - UTXO database handle
 *   new_utxos   - Array of new UTXO entries to insert
 *   new_count   - Number of new UTXOs
 *   spent_utxos - Array of outpoints being spent
 *   spent_count - Number of spent UTXOs
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Wraps all changes in a single transaction
 *   - Either all changes succeed or none do
 *   - This is the primary interface for updating UTXO state
 */
echo_result_t utxo_db_apply_block(utxo_db_t *udb,
                                  const utxo_entry_t **new_utxos,
                                  size_t new_count,
                                  const outpoint_t *spent_utxos,
                                  size_t spent_count);

/* ========================================================================
 * Database Maintenance
 * ======================================================================== */

/**
 * Clear all UTXOs from the database.
 *
 * Parameters:
 *   udb - UTXO database handle
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Should be called within a transaction for atomicity
 *   - Used during shutdown flush to rebuild from memory
 */
echo_result_t utxo_db_clear(utxo_db_t *udb);

/* ========================================================================
 * Statistics and Queries
 * ======================================================================== */

/**
 * Get the total number of UTXOs in the database.
 *
 * Parameters:
 *   udb   - UTXO database handle
 *   count - Output: number of UTXOs
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t utxo_db_count(utxo_db_t *udb, size_t *count);

/**
 * Get the total value of all UTXOs in the database.
 *
 * Parameters:
 *   udb   - UTXO database handle
 *   total - Output: total value in satoshis
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - This is a full table scan and may be slow
 *   - Used primarily for testing and validation
 */
echo_result_t utxo_db_total_value(utxo_db_t *udb, int64_t *total);

/* ========================================================================
 * Iteration
 * ======================================================================== */

/**
 * Iterate over all UTXOs in the database.
 *
 * Parameters:
 *   udb       - UTXO database handle
 *   callback  - Function to call for each UTXO
 *   user_data - User data to pass to callback
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Reads all UTXOs from database in unspecified order
 *   - Callback can return false to stop iteration early
 *   - Not recommended for large UTXO sets (full table scan)
 */
echo_result_t utxo_db_foreach(utxo_db_t *udb, utxo_iterator_fn callback,
                              void *user_data);

#endif /* ECHO_UTXO_DB_H */
