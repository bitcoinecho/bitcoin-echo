/*
 * Bitcoin Echo — UTXO Database Implementation
 *
 * Persistent storage for the Unspent Transaction Output set using SQLite.
 * Implements the schema specified in whitepaper §6.2.
 *
 * Design principles:
 * - Atomic updates via SQLite transactions
 * - Prepared statements for performance
 * - Simple, direct implementation
 * - Clear error handling
 *
 * The UTXO set is the critical performance bottleneck in blockchain validation.
 * This implementation prioritizes correctness over speed, using straightforward
 * SQL with appropriate indexes.
 *
 * Build once. Build right. Stop.
 */

#include "utxo_db.h"
#include "db.h"
#include "echo_assert.h"
#include "echo_types.h"
#include "tx.h"
#include "utxo.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================
 * Schema Definition
 * ======================================================================== */

/* Create the UTXO table per whitepaper §6.2 */
static const char *UTXO_SCHEMA =
    "CREATE TABLE IF NOT EXISTS utxo ("
    "    outpoint    BLOB PRIMARY KEY," /* 36 bytes: txid (32) + vout (4) */
    "    value       INTEGER NOT NULL," /* satoshis */
    "    script      BLOB NOT NULL,"    /* scriptPubKey */
    "    height      INTEGER NOT NULL," /* block height when created */
    "    coinbase    INTEGER NOT NULL"  /* 1 if from coinbase, else 0 */
    ");";

/* Prepared statement SQL */
static const char *SQL_LOOKUP =
    "SELECT value, script, height, coinbase FROM utxo WHERE outpoint = ?";

static const char *SQL_INSERT =
    "INSERT OR IGNORE INTO utxo (outpoint, value, script, height, coinbase) "
    "VALUES (?, ?, ?, ?, ?)";

static const char *SQL_DELETE = "DELETE FROM utxo WHERE outpoint = ?";

static const char *SQL_COUNT = "SELECT COUNT(*) FROM utxo";

static const char *SQL_TOTAL_VALUE = "SELECT SUM(value) FROM utxo";

static const char *SQL_FOREACH =
    "SELECT outpoint, value, script, height, coinbase FROM utxo";

/* ========================================================================
 * Internal Helper Functions
 * ======================================================================== */

/*
 * Initialize the database schema.
 * Creates the UTXO table if it doesn't exist.
 */
static echo_result_t init_schema(db_t *db) {
  echo_result_t res;

  /* Create the UTXO table */
  res = db_exec(db, UTXO_SCHEMA);
  if (res != ECHO_OK) {
    return res;
  }

  return ECHO_OK;
}

/*
 * Prepare commonly-used statements for efficiency.
 * These statements are reused across multiple operations.
 */
static echo_result_t prepare_statements(utxo_db_t *udb) {
  echo_result_t res;

  /* Prepare lookup statement */
  res = db_prepare(&udb->db, SQL_LOOKUP, &udb->lookup_stmt);
  if (res != ECHO_OK) {
    return res;
  }

  /* Prepare insert statement */
  res = db_prepare(&udb->db, SQL_INSERT, &udb->insert_stmt);
  if (res != ECHO_OK) {
    db_stmt_finalize(&udb->lookup_stmt);
    return res;
  }

  /* Prepare delete statement */
  res = db_prepare(&udb->db, SQL_DELETE, &udb->delete_stmt);
  if (res != ECHO_OK) {
    db_stmt_finalize(&udb->lookup_stmt);
    db_stmt_finalize(&udb->insert_stmt);
    return res;
  }

  udb->stmts_prepared = true;
  return ECHO_OK;
}

/*
 * Finalize all prepared statements.
 */
static void finalize_statements(utxo_db_t *udb) {
  if (udb->stmts_prepared) {
    db_stmt_finalize(&udb->lookup_stmt);
    db_stmt_finalize(&udb->insert_stmt);
    db_stmt_finalize(&udb->delete_stmt);
    udb->stmts_prepared = false;
  }
}

/* ========================================================================
 * Database Lifecycle
 * ======================================================================== */

echo_result_t utxo_db_open(utxo_db_t *udb, const char *path) {
  ECHO_ASSERT(udb != NULL);
  ECHO_ASSERT(path != NULL);

  echo_result_t res;

  /* Initialize structure */
  memset(udb, 0, sizeof(*udb));

  /* Open the database */
  res = db_open(&udb->db, path);
  if (res != ECHO_OK) {
    return res;
  }

  /* Initialize schema */
  res = init_schema(&udb->db);
  if (res != ECHO_OK) {
    db_close(&udb->db);
    return res;
  }

  /* Prepare commonly-used statements */
  res = prepare_statements(udb);
  if (res != ECHO_OK) {
    db_close(&udb->db);
    return res;
  }

  return ECHO_OK;
}

void utxo_db_close(utxo_db_t *udb) {
  if (udb == NULL) {
    return;
  }

  finalize_statements(udb);
  db_close(&udb->db);
  memset(udb, 0, sizeof(*udb));
}

/* ========================================================================
 * UTXO Operations
 * ======================================================================== */

echo_result_t utxo_db_lookup(utxo_db_t *udb, const outpoint_t *outpoint,
                             utxo_entry_t **entry) {
  ECHO_ASSERT(udb != NULL);
  ECHO_ASSERT(outpoint != NULL);
  ECHO_ASSERT(entry != NULL);

  echo_result_t res;
  uint8_t outpoint_bytes[36];

  /* Serialize outpoint for database lookup */
  outpoint_serialize(outpoint, outpoint_bytes);

  /* Reset and bind parameters */
  res = db_stmt_reset(&udb->lookup_stmt);
  if (res != ECHO_OK) {
    return res;
  }

  res = db_bind_blob(&udb->lookup_stmt, 1, outpoint_bytes, 36);
  if (res != ECHO_OK) {
    return res;
  }

  /* Execute query */
  res = db_step(&udb->lookup_stmt);
  if (res == ECHO_DONE) {
    /* No row found */
    return ECHO_ERR_NOT_FOUND;
  }
  if (res != ECHO_OK) {
    return res;
  }

  /* Extract results */
  int64_t value = db_column_int64(&udb->lookup_stmt, 0);
  const void *script_data = db_column_blob(&udb->lookup_stmt, 1);
  int script_len = db_column_bytes(&udb->lookup_stmt, 1);
  uint32_t height = (uint32_t)db_column_int(&udb->lookup_stmt, 2);
  int coinbase = db_column_int(&udb->lookup_stmt, 3);

  /* Create UTXO entry */
  *entry = utxo_entry_create(outpoint, value, script_data, (size_t)script_len,
                             height, coinbase != 0);

  if (*entry == NULL) {
    return ECHO_ERR_NOMEM;
  }

  return ECHO_OK;
}

echo_result_t utxo_db_exists(utxo_db_t *udb, const outpoint_t *outpoint,
                             bool *exists) {
  ECHO_ASSERT(udb != NULL);
  ECHO_ASSERT(outpoint != NULL);
  ECHO_ASSERT(exists != NULL);

  utxo_entry_t *entry = NULL;
  echo_result_t res = utxo_db_lookup(udb, outpoint, &entry);

  if (res == ECHO_OK) {
    *exists = true;
    utxo_entry_destroy(entry);
    return ECHO_OK;
  } else if (res == ECHO_ERR_NOT_FOUND) {
    *exists = false;
    return ECHO_OK;
  } else {
    return res;
  }
}

echo_result_t utxo_db_insert(utxo_db_t *udb, const utxo_entry_t *entry) {
  ECHO_ASSERT(udb != NULL);
  ECHO_ASSERT(entry != NULL);

  echo_result_t res;
  uint8_t outpoint_bytes[36];

  /* Serialize outpoint */
  outpoint_serialize(&entry->outpoint, outpoint_bytes);

  /* Reset and bind parameters */
  res = db_stmt_reset(&udb->insert_stmt);
  if (res != ECHO_OK) {
    return res;
  }

  res = db_bind_blob(&udb->insert_stmt, 1, outpoint_bytes, 36);
  if (res != ECHO_OK) {
    return res;
  }

  res = db_bind_int64(&udb->insert_stmt, 2, entry->value);
  if (res != ECHO_OK) {
    return res;
  }

  res = db_bind_blob(&udb->insert_stmt, 3, entry->script_pubkey,
                     entry->script_len);
  if (res != ECHO_OK) {
    return res;
  }

  res = db_bind_int(&udb->insert_stmt, 4, (int)entry->height);
  if (res != ECHO_OK) {
    return res;
  }

  res = db_bind_int(&udb->insert_stmt, 5, entry->is_coinbase ? 1 : 0);
  if (res != ECHO_OK) {
    return res;
  }

  /* Execute insert */
  res = db_step(&udb->insert_stmt);
  if (res == ECHO_DONE) {
    return ECHO_OK;
  }

  return res;
}

echo_result_t utxo_db_delete(utxo_db_t *udb, const outpoint_t *outpoint) {
  ECHO_ASSERT(udb != NULL);
  ECHO_ASSERT(outpoint != NULL);

  echo_result_t res;
  uint8_t outpoint_bytes[36];

  /* Serialize outpoint */
  outpoint_serialize(outpoint, outpoint_bytes);

  /* Reset and bind parameters */
  res = db_stmt_reset(&udb->delete_stmt);
  if (res != ECHO_OK) {
    return res;
  }

  res = db_bind_blob(&udb->delete_stmt, 1, outpoint_bytes, 36);
  if (res != ECHO_OK) {
    return res;
  }

  /* Execute delete */
  res = db_step(&udb->delete_stmt);
  if (res == ECHO_DONE) {
    /* Check if any rows were deleted */
    int changes = db_changes(&udb->db);
    if (changes == 0) {
      return ECHO_ERR_NOT_FOUND;
    }
    return ECHO_OK;
  }

  return res;
}

/* ========================================================================
 * Batch Operations
 * ======================================================================== */

echo_result_t utxo_db_insert_batch(utxo_db_t *udb, const utxo_entry_t **entries,
                                   size_t count) {
  ECHO_ASSERT(udb != NULL);
  ECHO_ASSERT(entries != NULL || count == 0);

  echo_result_t res;

  /* Insert each entry */
  for (size_t i = 0; i < count; i++) {
    res = utxo_db_insert(udb, entries[i]);
    if (res != ECHO_OK) {
      return res;
    }
  }

  return ECHO_OK;
}

echo_result_t utxo_db_delete_batch(utxo_db_t *udb, const outpoint_t *outpoints,
                                   size_t count) {
  ECHO_ASSERT(udb != NULL);
  ECHO_ASSERT(outpoints != NULL || count == 0);

  echo_result_t res;

  /* Delete each outpoint */
  for (size_t i = 0; i < count; i++) {
    res = utxo_db_delete(udb, &outpoints[i]);
    /* It's okay if some don't exist, but other errors are fatal */
    if (res != ECHO_OK && res != ECHO_ERR_NOT_FOUND) {
      return res;
    }
  }

  return ECHO_OK;
}

/* ========================================================================
 * Block Application
 * ======================================================================== */

echo_result_t utxo_db_apply_block(utxo_db_t *udb,
                                  const utxo_entry_t **new_utxos,
                                  size_t new_count,
                                  const outpoint_t *spent_utxos,
                                  size_t spent_count) {
  ECHO_ASSERT(udb != NULL);

  echo_result_t res;

  /* Begin transaction for atomicity */
  res = db_begin(&udb->db);
  if (res != ECHO_OK) {
    return res;
  }

  /* Delete spent UTXOs first */
  res = utxo_db_delete_batch(udb, spent_utxos, spent_count);
  if (res != ECHO_OK) {
    db_rollback(&udb->db);
    return res;
  }

  /* Insert new UTXOs */
  res = utxo_db_insert_batch(udb, new_utxos, new_count);
  if (res != ECHO_OK) {
    db_rollback(&udb->db);
    return res;
  }

  /* Commit transaction */
  res = db_commit(&udb->db);
  if (res != ECHO_OK) {
    db_rollback(&udb->db);
    return res;
  }

  return ECHO_OK;
}

/* ========================================================================
 * Database Maintenance
 * ======================================================================== */

echo_result_t utxo_db_clear(utxo_db_t *udb) {
  ECHO_ASSERT(udb != NULL);

  /* Execute DELETE FROM utxo to clear all entries */
  echo_result_t res = db_exec(&udb->db, "DELETE FROM utxo");
  if (res != ECHO_OK) {
    return res;
  }

  return ECHO_OK;
}

/* ========================================================================
 * Statistics and Queries
 * ======================================================================== */

echo_result_t utxo_db_count(utxo_db_t *udb, size_t *count) {
  ECHO_ASSERT(udb != NULL);
  ECHO_ASSERT(count != NULL);

  echo_result_t res;
  db_stmt_t stmt;

  /* Prepare count query */
  res = db_prepare(&udb->db, SQL_COUNT, &stmt);
  if (res != ECHO_OK) {
    return res;
  }

  /* Execute query */
  res = db_step(&stmt);
  if (res != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return res;
  }

  /* Get result */
  *count = (size_t)db_column_int64(&stmt, 0);

  db_stmt_finalize(&stmt);
  return ECHO_OK;
}

echo_result_t utxo_db_total_value(utxo_db_t *udb, int64_t *total) {
  ECHO_ASSERT(udb != NULL);
  ECHO_ASSERT(total != NULL);

  echo_result_t res;
  db_stmt_t stmt;

  /* Prepare sum query */
  res = db_prepare(&udb->db, SQL_TOTAL_VALUE, &stmt);
  if (res != ECHO_OK) {
    return res;
  }

  /* Execute query */
  res = db_step(&stmt);
  if (res != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return res;
  }

  /* Get result (NULL if table is empty) */
  *total = db_column_int64(&stmt, 0);

  db_stmt_finalize(&stmt);
  return ECHO_OK;
}

/* ========================================================================
 * Iteration
 * ======================================================================== */

echo_result_t utxo_db_foreach(utxo_db_t *udb, utxo_iterator_fn callback,
                              void *user_data) {
  ECHO_ASSERT(udb != NULL);
  ECHO_ASSERT(callback != NULL);

  echo_result_t res;
  db_stmt_t stmt;

  /* Prepare foreach query */
  res = db_prepare(&udb->db, SQL_FOREACH, &stmt);
  if (res != ECHO_OK) {
    return res;
  }

  /* Iterate over all rows */
  while (true) {
    res = db_step(&stmt);
    if (res == ECHO_DONE) {
      break; /* No more rows */
    }
    if (res != ECHO_OK) {
      db_stmt_finalize(&stmt);
      return res;
    }

    /* Parse row into UTXO entry */
    const void *outpoint_data = db_column_blob(&stmt, 0);
    int64_t value = db_column_int64(&stmt, 1);
    const void *script_data = db_column_blob(&stmt, 2);
    int script_len = db_column_bytes(&stmt, 2);
    uint32_t height = (uint32_t)db_column_int(&stmt, 3);
    int coinbase = db_column_int(&stmt, 4);

    /* Deserialize outpoint */
    outpoint_t outpoint;
    outpoint_deserialize(outpoint_data, &outpoint);

    /* Create temporary entry */
    utxo_entry_t *entry =
        utxo_entry_create(&outpoint, value, script_data, (size_t)script_len,
                          height, coinbase != 0);

    if (entry == NULL) {
      db_stmt_finalize(&stmt);
      return ECHO_ERR_NOMEM;
    }

    /* Call callback */
    bool continue_iter = callback(entry, user_data);
    utxo_entry_destroy(entry);

    if (!continue_iter) {
      break; /* Callback requested early termination */
    }
  }

  db_stmt_finalize(&stmt);
  return ECHO_OK;
}
