/**
 * @file utxo.h
 * @brief UTXO set data structures and interface
 *
 * The UTXO (Unspent Transaction Output) set tracks all spendable coins.
 * This module provides the in-memory representation and operations needed
 * for chain state management. The actual persistence layer uses SQLite,
 * but this interface is database-agnostic.
 *
 * Design principles:
 * - Simple, flat data structures
 * - Minimal memory overhead
 * - Clear ownership semantics
 * - Efficient lookup, insert, and remove operations
 * - Batch operations for block apply/revert
 */

#ifndef ECHO_UTXO_H
#define ECHO_UTXO_H

#include "echo_types.h"
#include "tx.h"
#include <stdbool.h>
#include <stdint.h>

/* Note: outpoint_t is defined in tx.h */

/**
 * UTXO entry: represents a single unspent transaction output
 * Contains all information needed to validate a spending transaction
 */
typedef struct {
  outpoint_t outpoint;    /* Unique identifier (txid + vout) */
  int64_t value;          /* Value in satoshis */
  uint8_t *script_pubkey; /* scriptPubKey (dynamically allocated) */
  size_t script_len;      /* Length of scriptPubKey in bytes */
  uint32_t height;        /* Block height when created */
  bool is_coinbase;       /* True if from coinbase transaction */
} utxo_entry_t;

/**
 * UTXO set: collection of all unspent outputs
 * This is an opaque structure; implementation details are in utxo.c
 *
 * The UTXO set supports:
 * - Fast lookup by outpoint
 * - Insertion of new UTXOs
 * - Removal of spent UTXOs
 * - Batch operations for applying/reverting blocks
 * - Iteration over all entries
 */
typedef struct utxo_set utxo_set_t;

/**
 * UTXO change record: tracks changes for block apply/revert
 * Used to implement reversible UTXO updates during chain reorganization
 */
typedef struct {
  outpoint_t outpoint; /* The outpoint being changed */
  utxo_entry_t *entry; /* The previous entry (NULL if newly created) */
} utxo_change_t;

/**
 * UTXO batch: collection of changes to apply atomically
 * Used when applying or reverting blocks
 */
typedef struct {
  utxo_change_t *changes; /* Array of changes */
  size_t count;           /* Number of changes */
  size_t capacity;        /* Allocated capacity */
} utxo_batch_t;

/* ========================================================================
 * Outpoint Operations
 * ======================================================================== */

/**
 * Compare two outpoints for equality
 * @param a First outpoint
 * @param b Second outpoint
 * @return true if outpoints are equal, false otherwise
 */
bool outpoint_equal(const outpoint_t *a, const outpoint_t *b);

/**
 * Serialize an outpoint to bytes
 * Format: txid (32 bytes) + vout (4 bytes, little-endian)
 * @param op Outpoint to serialize
 * @param out Output buffer (must be at least 36 bytes)
 * @return Number of bytes written (always 36)
 */
size_t outpoint_serialize(const outpoint_t *op, uint8_t *out);

/**
 * Deserialize an outpoint from bytes
 * @param data Input buffer (must be at least 36 bytes)
 * @param op Output outpoint
 * @return Number of bytes read (always 36)
 */
size_t outpoint_deserialize(const uint8_t *data, outpoint_t *op);

/* ========================================================================
 * UTXO Entry Operations
 * ======================================================================== */

/**
 * Create a new UTXO entry
 * Allocates memory for the entry and copies the scriptPubKey
 * @param outpoint The outpoint identifying this UTXO
 * @param value Value in satoshis
 * @param script_pubkey The scriptPubKey bytes
 * @param script_len Length of scriptPubKey
 * @param height Block height when created
 * @param is_coinbase True if from coinbase transaction
 * @return Newly allocated UTXO entry, or NULL on allocation failure
 */
utxo_entry_t *utxo_entry_create(const outpoint_t *outpoint, int64_t value,
                                const uint8_t *script_pubkey, size_t script_len,
                                uint32_t height, bool is_coinbase);

/**
 * Destroy a UTXO entry and free all associated memory
 * @param entry The entry to destroy (may be NULL)
 */
void utxo_entry_destroy(utxo_entry_t *entry);

/**
 * Clone a UTXO entry (deep copy)
 * @param entry The entry to clone
 * @return Newly allocated copy, or NULL on allocation failure
 */
utxo_entry_t *utxo_entry_clone(const utxo_entry_t *entry);

/**
 * Check if a coinbase UTXO is mature (spendable)
 * Coinbase outputs require 100 confirmations before spending
 * @param entry The UTXO entry
 * @param current_height Current blockchain height
 * @return true if mature (or not coinbase), false if immature
 */
bool utxo_entry_is_mature(const utxo_entry_t *entry, uint32_t current_height);

/* ========================================================================
 * UTXO Set Operations
 * ======================================================================== */

/**
 * Create a new empty UTXO set
 * @param initial_capacity Suggested initial capacity (0 for default)
 * @return Newly allocated UTXO set, or NULL on allocation failure
 */
utxo_set_t *utxo_set_create(size_t initial_capacity);

/**
 * Destroy a UTXO set and free all associated memory
 * @param set The UTXO set to destroy (may be NULL)
 */
void utxo_set_destroy(utxo_set_t *set);

/**
 * Get the number of UTXOs in the set
 * @param set The UTXO set
 * @return Number of entries
 */
size_t utxo_set_size(const utxo_set_t *set);

/**
 * Lookup a UTXO by outpoint
 * @param set The UTXO set
 * @param outpoint The outpoint to lookup
 * @return Pointer to UTXO entry if found, NULL otherwise
 *         The returned pointer is owned by the set; do not free
 */
const utxo_entry_t *utxo_set_lookup(const utxo_set_t *set,
                                    const outpoint_t *outpoint);

/**
 * Check if a UTXO exists in the set
 * @param set The UTXO set
 * @param outpoint The outpoint to check
 * @return true if exists, false otherwise
 */
bool utxo_set_exists(const utxo_set_t *set, const outpoint_t *outpoint);

/**
 * Insert a UTXO into the set
 * The entry is cloned; the caller retains ownership of the input
 * @param set The UTXO set
 * @param entry The entry to insert
 * @return ECHO_OK on success, error code on failure
 *         ECHO_ERR_EXISTS if outpoint already exists
 *         ECHO_ERR_NOMEM if allocation fails
 */
echo_result_t utxo_set_insert(utxo_set_t *set, const utxo_entry_t *entry);

/**
 * Insert a UTXO into the set, taking ownership of the entry
 * IBD optimization: skips duplicate check and cloning for ~60% fewer mallocs.
 * Caller transfers ownership; entry will be freed when removed from set.
 * @param set The UTXO set
 * @param entry The entry to insert (ownership transferred to set)
 * @return ECHO_OK on success, ECHO_ERR_NOMEM if bucket allocation fails
 * @warning Caller must guarantee outpoint uniqueness (no duplicate check!)
 */
echo_result_t utxo_set_insert_owned(utxo_set_t *set, utxo_entry_t *entry);

/**
 * Remove a UTXO from the set
 * @param set The UTXO set
 * @param outpoint The outpoint to remove
 * @return ECHO_OK on success, ECHO_ERR_NOT_FOUND if not found
 */
echo_result_t utxo_set_remove(utxo_set_t *set, const outpoint_t *outpoint);

/**
 * Clear all entries from the UTXO set
 * @param set The UTXO set
 */
void utxo_set_clear(utxo_set_t *set);

/* ========================================================================
 * Batch Operations
 * ======================================================================== */

/**
 * Create a new empty batch
 * @return Newly allocated batch, or NULL on allocation failure
 */
utxo_batch_t *utxo_batch_create(void);

/**
 * Destroy a batch and free all associated memory
 * @param batch The batch to destroy (may be NULL)
 */
void utxo_batch_destroy(utxo_batch_t *batch);

/**
 * Record an insertion in the batch
 * @param batch The batch
 * @param entry The entry being inserted (will be cloned)
 * @return ECHO_OK on success, ECHO_ERR_NOMEM on allocation failure
 */
echo_result_t utxo_batch_insert(utxo_batch_t *batch, const utxo_entry_t *entry);

/**
 * Record a removal in the batch
 * @param batch The batch
 * @param outpoint The outpoint being removed
 * @param old_entry The previous entry (will be cloned for undo)
 * @return ECHO_OK on success, ECHO_ERR_NOMEM on allocation failure
 */
echo_result_t utxo_batch_remove(utxo_batch_t *batch, const outpoint_t *outpoint,
                                const utxo_entry_t *old_entry);

/**
 * Apply a batch of changes to the UTXO set
 * All changes are applied atomically; if any operation fails,
 * the entire batch is rolled back and the set is left unchanged
 * @param set The UTXO set
 * @param batch The batch of changes to apply
 * @return ECHO_OK on success, error code on failure
 */
echo_result_t utxo_set_apply_batch(utxo_set_t *set, const utxo_batch_t *batch);

/**
 * Revert a batch of changes (undo)
 * This reverses the operations in the batch, restoring the previous state
 * @param set The UTXO set
 * @param batch The batch to revert
 * @return ECHO_OK on success, error code on failure
 */
echo_result_t utxo_set_revert_batch(utxo_set_t *set, const utxo_batch_t *batch);

/* ========================================================================
 * Iteration
 * ======================================================================== */

/**
 * Iterator callback function type
 * @param entry The current UTXO entry
 * @param user_data User-provided data passed to iterator
 * @return true to continue iteration, false to stop
 */
typedef bool (*utxo_iterator_fn)(const utxo_entry_t *entry, void *user_data);

/**
 * Iterate over all UTXOs in the set
 * @param set The UTXO set
 * @param callback Function to call for each entry
 * @param user_data User data to pass to callback
 */
void utxo_set_foreach(const utxo_set_t *set, utxo_iterator_fn callback,
                      void *user_data);

#endif /* ECHO_UTXO_H */
