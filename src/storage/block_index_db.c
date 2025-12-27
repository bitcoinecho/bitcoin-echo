/*
 * Bitcoin Echo — Block Index Database Implementation
 *
 * Persistent storage for block headers using SQLite.
 * See block_index_db.h for interface documentation.
 *
 * Build once. Build right. Stop.
 */

#include "block_index_db.h"
#include "block.h"
#include "chainstate.h"
#include "db.h"
#include "echo_types.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/*
 * Create the block index schema.
 */
static echo_result_t create_schema(db_t *db) {
  echo_result_t result;

  /* Create blocks table per whitepaper §6.3 */
  result = db_exec(
      db, "CREATE TABLE IF NOT EXISTS blocks ("
          "    hash        BLOB PRIMARY KEY," /* 32 bytes */
          "    height      INTEGER NOT NULL,"
          "    header      BLOB NOT NULL,"   /* 80 bytes */
          "    chainwork   BLOB NOT NULL,"   /* 32 bytes, big-endian */
          "    status      INTEGER NOT NULL," /* validation status flags */
          "    data_file   INTEGER DEFAULT -1," /* block file index, -1=not stored */
          "    data_pos    INTEGER DEFAULT 0"   /* byte offset in file */
          ");");
  if (result != ECHO_OK) {
    return result;
  }

  /*
   * Migration for existing databases: add data_file and data_pos columns.
   * ALTER TABLE ADD COLUMN silently fails if column already exists in SQLite.
   */
  db_exec(db, "ALTER TABLE blocks ADD COLUMN data_file INTEGER DEFAULT -1");
  db_exec(db, "ALTER TABLE blocks ADD COLUMN data_pos INTEGER DEFAULT 0");

  /* Create height index for sequential chain navigation */
  result =
      db_exec(db, "CREATE INDEX IF NOT EXISTS idx_height ON blocks(height);");
  if (result != ECHO_OK) {
    return result;
  }

  /* Create chainwork index for chain comparison */
  result = db_exec(
      db, "CREATE INDEX IF NOT EXISTS idx_chainwork ON blocks(chainwork);");
  if (result != ECHO_OK) {
    return result;
  }

  /*
   * Create metadata table for chainstate persistence.
   * Stores key-value pairs like 'validated_tip_height' for restart recovery.
   */
  result = db_exec(db, "CREATE TABLE IF NOT EXISTS metadata ("
                       "    key   TEXT PRIMARY KEY,"
                       "    value BLOB"
                       ");");
  if (result != ECHO_OK) {
    return result;
  }

  /*
   * Migration: Set HAVE_DATA flag for blocks that have stored data.
   * This fixes blocks stored before the flag was properly set.
   * Safe to run multiple times (idempotent).
   */
  db_exec(db, "UPDATE blocks SET status = status | 16 "
              "WHERE data_file >= 0 AND (status & 16) = 0");

  return ECHO_OK;
}

/*
 * Prepare commonly-used statements.
 */
static echo_result_t prepare_statements(block_index_db_t *bdb) {
  echo_result_t result;

  /* Lookup by hash */
  result = db_prepare(&bdb->db,
                      "SELECT hash, height, header, chainwork, status, "
                      "data_file, data_pos FROM blocks WHERE hash = ?",
                      &bdb->lookup_hash_stmt);
  if (result != ECHO_OK)
    return result;

  /* Lookup by height */
  result = db_prepare(&bdb->db,
                      "SELECT hash, height, header, chainwork, status, "
                      "data_file, data_pos FROM blocks WHERE height = ? LIMIT 1",
                      &bdb->lookup_height_stmt);
  if (result != ECHO_OK)
    return result;

  /* Insert (OR IGNORE to silently skip duplicates on hash primary key) */
  result = db_prepare(&bdb->db,
                      "INSERT OR IGNORE INTO blocks (hash, height, header, "
                      "chainwork, status, data_file, data_pos) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?)",
                      &bdb->insert_stmt);
  if (result != ECHO_OK)
    return result;

  /* Update status */
  result = db_prepare(&bdb->db, "UPDATE blocks SET status = ? WHERE hash = ?",
                      &bdb->update_status_stmt);
  if (result != ECHO_OK)
    return result;

  /* Update block data position and set HAVE_DATA flag (when block is stored) */
  result = db_prepare(&bdb->db,
                      "UPDATE blocks SET data_file = ?, data_pos = ?, "
                      "status = status | ? WHERE hash = ?",
                      &bdb->update_data_pos_stmt);
  if (result != ECHO_OK)
    return result;

  /*
   * Best chain tip (highest height with VALID_CHAIN status).
   *
   * Note: We use ORDER BY height DESC rather than chainwork DESC because
   * chainwork is stored in little-endian format which doesn't sort correctly
   * in SQLite's byte-by-byte blob comparison. For headers-first sync where
   * all headers are on the main chain, height ordering is equivalent.
   *
   * TODO: For proper fork handling, store chainwork in big-endian format.
   */
  result =
      db_prepare(&bdb->db,
                 "SELECT hash, height, header, chainwork, status, data_file, "
                 "data_pos FROM blocks "
                 "WHERE (status & ?) != 0 ORDER BY height DESC LIMIT 1",
                 &bdb->best_chain_stmt);
  if (result != ECHO_OK)
    return result;

  bdb->stmts_prepared = true;
  return ECHO_OK;
}

/*
 * Finalize all prepared statements.
 */
static void finalize_statements(block_index_db_t *bdb) {
  if (bdb->stmts_prepared) {
    db_stmt_finalize(&bdb->lookup_hash_stmt);
    db_stmt_finalize(&bdb->lookup_height_stmt);
    db_stmt_finalize(&bdb->insert_stmt);
    db_stmt_finalize(&bdb->update_status_stmt);
    db_stmt_finalize(&bdb->update_data_pos_stmt);
    db_stmt_finalize(&bdb->best_chain_stmt);
    bdb->stmts_prepared = false;
  }
}

/*
 * Serialize a block header to 80 bytes.
 */
static void serialize_header(const block_header_t *header, uint8_t *buf) {
  size_t pos = 0;

  /* version (4 bytes, little-endian) */
  buf[pos++] = (uint8_t)(header->version);
  buf[pos++] = (uint8_t)(header->version >> 8);
  buf[pos++] = (uint8_t)(header->version >> 16);
  buf[pos++] = (uint8_t)(header->version >> 24);

  /* prev_hash (32 bytes) */
  memcpy(buf + pos, header->prev_hash.bytes, 32);
  pos += 32;

  /* merkle_root (32 bytes) */
  memcpy(buf + pos, header->merkle_root.bytes, 32);
  pos += 32;

  /* timestamp (4 bytes, little-endian) */
  buf[pos++] = (uint8_t)(header->timestamp);
  buf[pos++] = (uint8_t)(header->timestamp >> 8);
  buf[pos++] = (uint8_t)(header->timestamp >> 16);
  buf[pos++] = (uint8_t)(header->timestamp >> 24);

  /* bits (4 bytes, little-endian) */
  buf[pos++] = (uint8_t)(header->bits);
  buf[pos++] = (uint8_t)(header->bits >> 8);
  buf[pos++] = (uint8_t)(header->bits >> 16);
  buf[pos++] = (uint8_t)(header->bits >> 24);

  /* nonce (4 bytes, little-endian) */
  buf[pos++] = (uint8_t)(header->nonce);
  buf[pos++] = (uint8_t)(header->nonce >> 8);
  buf[pos++] = (uint8_t)(header->nonce >> 16);
  buf[pos++] = (uint8_t)(header->nonce >> 24);
}

/*
 * Deserialize a block header from 80 bytes.
 */
static void deserialize_header(const uint8_t *buf, block_header_t *header) {
  size_t pos = 0;

  /* version (4 bytes, little-endian) */
  header->version = (int32_t)(buf[pos] | (buf[pos + 1] << 8) |
                              (buf[pos + 2] << 16) | (buf[pos + 3] << 24));
  pos += 4;

  /* prev_hash (32 bytes) */
  memcpy(header->prev_hash.bytes, buf + pos, 32);
  pos += 32;

  /* merkle_root (32 bytes) */
  memcpy(header->merkle_root.bytes, buf + pos, 32);
  pos += 32;

  /* timestamp (4 bytes, little-endian) */
  header->timestamp = (uint32_t)(buf[pos] | (buf[pos + 1] << 8) |
                                 (buf[pos + 2] << 16) | (buf[pos + 3] << 24));
  pos += 4;

  /* bits (4 bytes, little-endian) */
  header->bits = (uint32_t)(buf[pos] | (buf[pos + 1] << 8) |
                            (buf[pos + 2] << 16) | (buf[pos + 3] << 24));
  pos += 4;

  /* nonce (4 bytes, little-endian) */
  header->nonce = (uint32_t)(buf[pos] | (buf[pos + 1] << 8) |
                             (buf[pos + 2] << 16) | (buf[pos + 3] << 24));
}

/*
 * Populate a block_index_entry_t from a prepared statement row.
 */
static void populate_entry_from_row(db_stmt_t *stmt,
                                    block_index_entry_t *entry) {
  const void *hash_blob;
  const void *header_blob;
  const void *chainwork_blob;

  /* hash (column 0) */
  hash_blob = db_column_blob(stmt, 0);
  memcpy(entry->hash.bytes, hash_blob, 32);

  /* height (column 1) */
  entry->height = (uint32_t)db_column_int(stmt, 1);

  /* header (column 2) */
  header_blob = db_column_blob(stmt, 2);
  deserialize_header((const uint8_t *)header_blob, &entry->header);

  /* chainwork (column 3) */
  chainwork_blob = db_column_blob(stmt, 3);
  memcpy(entry->chainwork.bytes, chainwork_blob, 32);

  /* status (column 4) */
  entry->status = (uint32_t)db_column_int(stmt, 4);

  /* data_file (column 5) - may be NULL/-1 for header-only entries */
  entry->data_file = db_column_int(stmt, 5);

  /* data_pos (column 6) */
  entry->data_pos = (uint32_t)db_column_int(stmt, 6);
}

/* ========================================================================
 * Database Lifecycle
 * ======================================================================== */

echo_result_t block_index_db_open(block_index_db_t *bdb, const char *path) {
  echo_result_t result;

  /* Initialize structure */
  memset(bdb, 0, sizeof(block_index_db_t));
  bdb->stmts_prepared = false;

  /* Open database */
  result = db_open(&bdb->db, path);
  if (result != ECHO_OK) {
    return result;
  }

  /* Create schema */
  result = create_schema(&bdb->db);
  if (result != ECHO_OK) {
    db_close(&bdb->db);
    return result;
  }

  /* Prepare statements */
  result = prepare_statements(bdb);
  if (result != ECHO_OK) {
    db_close(&bdb->db);
    return result;
  }

  return ECHO_OK;
}

void block_index_db_close(block_index_db_t *bdb) {
  if (bdb) {
    finalize_statements(bdb);
    db_close(&bdb->db);
    memset(bdb, 0, sizeof(block_index_db_t));
  }
}

echo_result_t block_index_db_begin(block_index_db_t *bdb) {
  if (!bdb) {
    return ECHO_ERR_NULL_PARAM;
  }
  return db_begin(&bdb->db);
}

echo_result_t block_index_db_commit(block_index_db_t *bdb) {
  if (!bdb) {
    return ECHO_ERR_NULL_PARAM;
  }
  return db_commit(&bdb->db);
}

/* ========================================================================
 * Block Index Operations
 * ======================================================================== */

echo_result_t block_index_db_lookup_by_hash(block_index_db_t *bdb,
                                            const hash256_t *hash,
                                            block_index_entry_t *entry) {
  echo_result_t result;

  /* Reset statement */
  result = db_stmt_reset(&bdb->lookup_hash_stmt);
  if (result != ECHO_OK)
    return result;

  /* Bind hash parameter */
  result = db_bind_blob(&bdb->lookup_hash_stmt, 1, hash->bytes, 32);
  if (result != ECHO_OK)
    return result;

  /* Execute query */
  result = db_step(&bdb->lookup_hash_stmt);
  if (result == ECHO_DONE) {
    return ECHO_ERR_NOT_FOUND;
  }
  if (result != ECHO_OK) {
    return result;
  }

  /* Populate entry from result row */
  populate_entry_from_row(&bdb->lookup_hash_stmt, entry);

  return ECHO_OK;
}

echo_result_t block_index_db_lookup_by_height(block_index_db_t *bdb,
                                              uint32_t height,
                                              block_index_entry_t *entry) {
  echo_result_t result;

  /* Reset statement */
  result = db_stmt_reset(&bdb->lookup_height_stmt);
  if (result != ECHO_OK)
    return result;

  /* Bind height parameter */
  result = db_bind_int(&bdb->lookup_height_stmt, 1, (int)height);
  if (result != ECHO_OK)
    return result;

  /* Execute query */
  result = db_step(&bdb->lookup_height_stmt);
  if (result == ECHO_DONE) {
    return ECHO_ERR_NOT_FOUND;
  }
  if (result != ECHO_OK) {
    return result;
  }

  /* Populate entry from result row */
  populate_entry_from_row(&bdb->lookup_height_stmt, entry);

  return ECHO_OK;
}

echo_result_t block_index_db_exists(block_index_db_t *bdb,
                                    const hash256_t *hash, bool *exists) {
  block_index_entry_t entry;
  echo_result_t result;

  result = block_index_db_lookup_by_hash(bdb, hash, &entry);
  if (result == ECHO_OK) {
    *exists = true;
    return ECHO_OK;
  } else if (result == ECHO_ERR_NOT_FOUND) {
    *exists = false;
    return ECHO_OK;
  } else {
    return result;
  }
}

echo_result_t block_index_db_insert(block_index_db_t *bdb,
                                    const block_index_entry_t *entry) {
  echo_result_t result;
  uint8_t header_buf[80];

  /* Reset statement */
  result = db_stmt_reset(&bdb->insert_stmt);
  if (result != ECHO_OK)
    return result;

  /* Bind hash (parameter 1) */
  result = db_bind_blob(&bdb->insert_stmt, 1, entry->hash.bytes, 32);
  if (result != ECHO_OK)
    return result;

  /* Bind height (parameter 2) */
  result = db_bind_int(&bdb->insert_stmt, 2, (int)entry->height);
  if (result != ECHO_OK)
    return result;

  /* Bind header (parameter 3) */
  serialize_header(&entry->header, header_buf);
  result = db_bind_blob(&bdb->insert_stmt, 3, header_buf, 80);
  if (result != ECHO_OK)
    return result;

  /* Bind chainwork (parameter 4) */
  result = db_bind_blob(&bdb->insert_stmt, 4, entry->chainwork.bytes, 32);
  if (result != ECHO_OK)
    return result;

  /* Bind status (parameter 5) */
  result = db_bind_int(&bdb->insert_stmt, 5, (int)entry->status);
  if (result != ECHO_OK)
    return result;

  /* Bind data_file (parameter 6) */
  result = db_bind_int(&bdb->insert_stmt, 6, entry->data_file);
  if (result != ECHO_OK)
    return result;

  /* Bind data_pos (parameter 7) */
  result = db_bind_int(&bdb->insert_stmt, 7, (int)entry->data_pos);
  if (result != ECHO_OK)
    return result;

  /* Execute insert */
  result = db_step(&bdb->insert_stmt);
  if (result == ECHO_DONE) {
    return ECHO_OK;
  }

  return result;
}

echo_result_t block_index_db_update_data_pos(block_index_db_t *bdb,
                                             const hash256_t *hash,
                                             uint32_t data_file,
                                             uint32_t data_pos) {
  echo_result_t result;

  /* Reset statement */
  result = db_stmt_reset(&bdb->update_data_pos_stmt);
  if (result != ECHO_OK)
    return result;

  /* Bind data_file (parameter 1) */
  result = db_bind_int(&bdb->update_data_pos_stmt, 1, (int)data_file);
  if (result != ECHO_OK)
    return result;

  /* Bind data_pos (parameter 2) */
  result = db_bind_int(&bdb->update_data_pos_stmt, 2, (int)data_pos);
  if (result != ECHO_OK)
    return result;

  /* Bind HAVE_DATA flag (parameter 3) */
  result = db_bind_int(&bdb->update_data_pos_stmt, 3, BLOCK_STATUS_HAVE_DATA);
  if (result != ECHO_OK)
    return result;

  /* Bind hash (parameter 4) */
  result = db_bind_blob(&bdb->update_data_pos_stmt, 4, hash->bytes, 32);
  if (result != ECHO_OK)
    return result;

  /* Execute update */
  result = db_step(&bdb->update_data_pos_stmt);
  if (result == ECHO_DONE) {
    return ECHO_OK;
  }

  return result;
}

echo_result_t block_index_db_update_status(block_index_db_t *bdb,
                                           const hash256_t *hash,
                                           uint32_t status) {
  echo_result_t result;

  /* Reset statement */
  result = db_stmt_reset(&bdb->update_status_stmt);
  if (result != ECHO_OK)
    return result;

  /* Bind status (parameter 1) */
  result = db_bind_int(&bdb->update_status_stmt, 1, (int)status);
  if (result != ECHO_OK)
    return result;

  /* Bind hash (parameter 2) */
  result = db_bind_blob(&bdb->update_status_stmt, 2, hash->bytes, 32);
  if (result != ECHO_OK)
    return result;

  /* Execute update */
  result = db_step(&bdb->update_status_stmt);
  if (result == ECHO_DONE) {
    /* Check if any rows were updated */
    int changes = db_changes(&bdb->db);
    if (changes == 0) {
      return ECHO_ERR_NOT_FOUND;
    }
    return ECHO_OK;
  }

  return result;
}

/* ========================================================================
 * Chain Queries
 * ======================================================================== */

echo_result_t block_index_db_get_best_chain(block_index_db_t *bdb,
                                            block_index_entry_t *entry) {
  echo_result_t result;

  /* Reset statement */
  result = db_stmt_reset(&bdb->best_chain_stmt);
  if (result != ECHO_OK)
    return result;

  /* Bind BLOCK_STATUS_VALID_CHAIN filter (parameter 1) */
  result = db_bind_int(&bdb->best_chain_stmt, 1, BLOCK_STATUS_VALID_CHAIN);
  if (result != ECHO_OK)
    return result;

  /* Execute query */
  result = db_step(&bdb->best_chain_stmt);
  if (result == ECHO_DONE) {
    return ECHO_ERR_NOT_FOUND; /* Database is empty */
  }
  if (result != ECHO_OK) {
    return result;
  }

  /* Populate entry from result row */
  populate_entry_from_row(&bdb->best_chain_stmt, entry);

  return ECHO_OK;
}

echo_result_t block_index_db_get_chain_block(block_index_db_t *bdb,
                                             uint32_t height,
                                             block_index_entry_t *entry) {
  echo_result_t result;
  db_stmt_t stmt;

  /*
   * Get block at height, preferring VALID_CHAIN status but falling back
   * to any block at this height. This handles pruned blocks that may have
   * lost their VALID_CHAIN status. Order by status DESC so VALID_CHAIN
   * blocks (higher status values) come first.
   */
  result =
      db_prepare(&bdb->db,
                 "SELECT hash, height, header, chainwork, status, data_file, "
                 "data_pos FROM blocks "
                 "WHERE height = ? ORDER BY status DESC LIMIT 1",
                 &stmt);
  if (result != ECHO_OK)
    return result;

  /* Bind height */
  result = db_bind_int(&stmt, 1, (int)height);
  if (result != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return result;
  }

  /* Execute */
  result = db_step(&stmt);
  if (result == ECHO_DONE) {
    db_stmt_finalize(&stmt);
    return ECHO_ERR_NOT_FOUND;
  }
  if (result != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return result;
  }

  /* Populate entry */
  populate_entry_from_row(&stmt, entry);

  db_stmt_finalize(&stmt);
  return ECHO_OK;
}

echo_result_t block_index_db_get_prev(block_index_db_t *bdb,
                                      const hash256_t *hash,
                                      block_index_entry_t *entry) {
  echo_result_t result;
  block_index_entry_t current;

  /* Lookup current block to get prev_hash */
  result = block_index_db_lookup_by_hash(bdb, hash, &current);
  if (result != ECHO_OK)
    return result;

  /* Lookup parent by prev_hash from header */
  result = block_index_db_lookup_by_hash(bdb, &current.header.prev_hash, entry);
  return result;
}

echo_result_t block_index_db_find_common_ancestor(
    block_index_db_t *bdb, const hash256_t *hash_a, const hash256_t *hash_b,
    block_index_entry_t *ancestor) {
  echo_result_t result;
  block_index_entry_t entry_a, entry_b;

  /* Lookup both blocks */
  result = block_index_db_lookup_by_hash(bdb, hash_a, &entry_a);
  if (result != ECHO_OK)
    return result;

  result = block_index_db_lookup_by_hash(bdb, hash_b, &entry_b);
  if (result != ECHO_OK)
    return result;

  /*
   * Walk both chains backward until we find a common block.
   * First, bring both to the same height by walking the higher one backward.
   */
  while (entry_a.height > entry_b.height) {
    result = block_index_db_get_prev(bdb, &entry_a.hash, &entry_a);
    if (result != ECHO_OK)
      return result;
  }

  while (entry_b.height > entry_a.height) {
    result = block_index_db_get_prev(bdb, &entry_b.hash, &entry_b);
    if (result != ECHO_OK)
      return result;
  }

  /* Now both are at the same height, walk backward together */
  while (memcmp(entry_a.hash.bytes, entry_b.hash.bytes, 32) != 0) {
    result = block_index_db_get_prev(bdb, &entry_a.hash, &entry_a);
    if (result != ECHO_OK)
      return result;

    result = block_index_db_get_prev(bdb, &entry_b.hash, &entry_b);
    if (result != ECHO_OK)
      return result;
  }

  /* Found common ancestor */
  *ancestor = entry_a;
  return ECHO_OK;
}

/* ========================================================================
 * Batch Operations
 * ======================================================================== */

echo_result_t block_index_db_mark_best_chain(block_index_db_t *bdb,
                                             const hash256_t *hashes,
                                             size_t count) {
  echo_result_t result;
  db_stmt_t stmt;
  size_t i;

  /* Prepare update statement */
  result = db_prepare(
      &bdb->db, "UPDATE blocks SET status = status | ? WHERE hash = ?", &stmt);
  if (result != ECHO_OK)
    return result;

  /* Update each block */
  for (i = 0; i < count; i++) {
    result = db_stmt_reset(&stmt);
    if (result != ECHO_OK) {
      db_stmt_finalize(&stmt);
      return result;
    }

    /* Bind BLOCK_STATUS_VALID_CHAIN flag */
    result = db_bind_int(&stmt, 1, BLOCK_STATUS_VALID_CHAIN);
    if (result != ECHO_OK) {
      db_stmt_finalize(&stmt);
      return result;
    }

    /* Bind hash */
    result = db_bind_blob(&stmt, 2, hashes[i].bytes, 32);
    if (result != ECHO_OK) {
      db_stmt_finalize(&stmt);
      return result;
    }

    /* Execute */
    result = db_step(&stmt);
    if (result != ECHO_DONE) {
      db_stmt_finalize(&stmt);
      return result;
    }
  }

  db_stmt_finalize(&stmt);
  return ECHO_OK;
}

echo_result_t block_index_db_unmark_best_chain(block_index_db_t *bdb,
                                               const hash256_t *hashes,
                                               size_t count) {
  echo_result_t result;
  db_stmt_t stmt;
  size_t i;

  /* Prepare update statement */
  result = db_prepare(
      &bdb->db, "UPDATE blocks SET status = status & ? WHERE hash = ?", &stmt);
  if (result != ECHO_OK)
    return result;

  /* Compute bitmask to clear BLOCK_STATUS_VALID_CHAIN */
  int clear_mask = ~BLOCK_STATUS_VALID_CHAIN;

  /* Update each block */
  for (i = 0; i < count; i++) {
    result = db_stmt_reset(&stmt);
    if (result != ECHO_OK) {
      db_stmt_finalize(&stmt);
      return result;
    }

    /* Bind clear mask */
    result = db_bind_int(&stmt, 1, clear_mask);
    if (result != ECHO_OK) {
      db_stmt_finalize(&stmt);
      return result;
    }

    /* Bind hash */
    result = db_bind_blob(&stmt, 2, hashes[i].bytes, 32);
    if (result != ECHO_OK) {
      db_stmt_finalize(&stmt);
      return result;
    }

    /* Execute */
    result = db_step(&stmt);
    if (result != ECHO_DONE) {
      db_stmt_finalize(&stmt);
      return result;
    }
  }

  db_stmt_finalize(&stmt);
  return ECHO_OK;
}

/* ========================================================================
 * Statistics and Queries
 * ======================================================================== */

echo_result_t block_index_db_count(block_index_db_t *bdb, size_t *count) {
  echo_result_t result;
  db_stmt_t stmt;

  result = db_prepare(&bdb->db, "SELECT COUNT(*) FROM blocks", &stmt);
  if (result != ECHO_OK)
    return result;

  result = db_step(&stmt);
  if (result != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return result;
  }

  *count = (size_t)db_column_int(&stmt, 0);

  db_stmt_finalize(&stmt);
  return ECHO_OK;
}

echo_result_t block_index_db_get_height(block_index_db_t *bdb,
                                        uint32_t *height) {
  echo_result_t result;
  block_index_entry_t entry;

  result = block_index_db_get_best_chain(bdb, &entry);
  if (result != ECHO_OK)
    return result;

  *height = entry.height;
  return ECHO_OK;
}

echo_result_t block_index_db_get_chainwork(block_index_db_t *bdb,
                                           work256_t *chainwork) {
  echo_result_t result;
  block_index_entry_t entry;

  result = block_index_db_get_best_chain(bdb, &entry);
  if (result != ECHO_OK)
    return result;

  *chainwork = entry.chainwork;
  return ECHO_OK;
}

/* ========================================================================
 * Pruning Operations
 * ======================================================================== */

echo_result_t block_index_db_mark_pruned(block_index_db_t *bdb,
                                         uint32_t start_height,
                                         uint32_t end_height) {
  echo_result_t result;
  db_stmt_t stmt;

  if (bdb == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (start_height >= end_height) {
    return ECHO_ERR_INVALID_PARAM;
  }

  /*
   * Update status for all blocks in the height range:
   * - Set BLOCK_STATUS_PRUNED flag
   * - Clear BLOCK_STATUS_HAVE_DATA flag
   */
  result = db_prepare(
      &bdb->db,
      "UPDATE blocks SET status = (status | ?) & ? WHERE height >= ? AND height < ?",
      &stmt);
  if (result != ECHO_OK) {
    return result;
  }

  /* Bind BLOCK_STATUS_PRUNED to set (parameter 1) */
  result = db_bind_int(&stmt, 1, BLOCK_STATUS_PRUNED);
  if (result != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return result;
  }

  /* Bind mask to clear BLOCK_STATUS_HAVE_DATA (parameter 2) */
  int clear_mask = ~BLOCK_STATUS_HAVE_DATA;
  result = db_bind_int(&stmt, 2, clear_mask);
  if (result != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return result;
  }

  /* Bind start_height (parameter 3) */
  result = db_bind_int(&stmt, 3, (int)start_height);
  if (result != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return result;
  }

  /* Bind end_height (parameter 4) */
  result = db_bind_int(&stmt, 4, (int)end_height);
  if (result != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return result;
  }

  /* Execute update */
  result = db_step(&stmt);
  db_stmt_finalize(&stmt);

  if (result == ECHO_DONE) {
    return ECHO_OK;
  }

  return result;
}

echo_result_t block_index_db_get_pruned_height(block_index_db_t *bdb,
                                               uint32_t *height) {
  echo_result_t result;
  db_stmt_t stmt;

  if (bdb == NULL || height == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /*
   * Find the lowest block height that has BLOCK_STATUS_HAVE_DATA set
   * and does NOT have BLOCK_STATUS_PRUNED set.
   * This represents the first block with available data.
   */
  result = db_prepare(
      &bdb->db,
      "SELECT MIN(height) FROM blocks WHERE (status & ?) != 0 AND (status & ?) = 0",
      &stmt);
  if (result != ECHO_OK) {
    return result;
  }

  /* Bind BLOCK_STATUS_HAVE_DATA (parameter 1) */
  result = db_bind_int(&stmt, 1, BLOCK_STATUS_HAVE_DATA);
  if (result != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return result;
  }

  /* Bind BLOCK_STATUS_PRUNED (parameter 2) */
  result = db_bind_int(&stmt, 2, BLOCK_STATUS_PRUNED);
  if (result != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return result;
  }

  /* Execute query */
  result = db_step(&stmt);
  if (result == ECHO_DONE) {
    /* No results - database is empty */
    db_stmt_finalize(&stmt);
    return ECHO_ERR_NOT_FOUND;
  }
  if (result != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return result;
  }

  /* Get the minimum height (may be NULL if no unpruned blocks) */
  int min_height = db_column_int(&stmt, 0);
  db_stmt_finalize(&stmt);

  /* If result is 0 and we have blocks, check if it's real 0 or NULL */
  if (min_height == 0) {
    /* Check if genesis block has data */
    block_index_entry_t genesis;
    hash256_t zero_hash;
    memset(&zero_hash, 0, sizeof(zero_hash));

    /* Query for block at height 0 */
    result = block_index_db_lookup_by_height(bdb, 0, &genesis);
    if (result == ECHO_OK) {
      if ((genesis.status & BLOCK_STATUS_HAVE_DATA) &&
          !(genesis.status & BLOCK_STATUS_PRUNED)) {
        *height = 0;
        return ECHO_OK;
      }
    }
  }

  *height = (uint32_t)min_height;
  return ECHO_OK;
}

echo_result_t block_index_db_is_pruned(block_index_db_t *bdb,
                                       const hash256_t *hash,
                                       bool *pruned) {
  echo_result_t result;
  block_index_entry_t entry;

  if (bdb == NULL || hash == NULL || pruned == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  result = block_index_db_lookup_by_hash(bdb, hash, &entry);
  if (result != ECHO_OK) {
    return result;
  }

  /* A block is considered pruned if BLOCK_STATUS_PRUNED is set */
  *pruned = (entry.status & BLOCK_STATUS_PRUNED) != 0;
  return ECHO_OK;
}

/* ========================================================================
 * Validated Tip Persistence
 * ======================================================================== */

echo_result_t block_index_db_set_validated_tip(block_index_db_t *bdb,
                                                uint32_t height,
                                                const hash256_t *hash) {
  if (bdb == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  echo_result_t result;
  db_stmt_t stmt;

  /* Prepare insert/replace statement */
  result = db_prepare(
      &bdb->db,
      "INSERT OR REPLACE INTO metadata (key, value) VALUES (?, ?)", &stmt);
  if (result != ECHO_OK) {
    return result;
  }

  /* Store height */
  result = db_bind_text(&stmt, 1, "validated_tip_height");
  if (result != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return result;
  }
  result = db_bind_int(&stmt, 2, (int)height);
  if (result != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return result;
  }
  result = db_step(&stmt);
  /* INSERT returns ECHO_DONE on success, not ECHO_OK */
  if (result != ECHO_OK && result != ECHO_DONE && result != ECHO_ERR_NOT_FOUND) {
    db_stmt_finalize(&stmt);
    return result;
  }

  /* Store hash if provided */
  if (hash != NULL) {
    result = db_stmt_reset(&stmt);
    if (result != ECHO_OK) {
      db_stmt_finalize(&stmt);
      return result;
    }
    result = db_bind_text(&stmt, 1, "validated_tip_hash");
    if (result != ECHO_OK) {
      db_stmt_finalize(&stmt);
      return result;
    }
    result = db_bind_blob(&stmt, 2, hash->bytes, 32);
    if (result != ECHO_OK) {
      db_stmt_finalize(&stmt);
      return result;
    }
    result = db_step(&stmt);
    /* INSERT returns ECHO_DONE on success, not ECHO_OK */
    if (result != ECHO_OK && result != ECHO_DONE && result != ECHO_ERR_NOT_FOUND) {
      db_stmt_finalize(&stmt);
      return result;
    }
  }

  db_stmt_finalize(&stmt);
  return ECHO_OK;
}

echo_result_t block_index_db_get_validated_tip(block_index_db_t *bdb,
                                                uint32_t *height,
                                                hash256_t *hash) {
  if (bdb == NULL || height == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  echo_result_t result;
  db_stmt_t stmt;

  result = db_prepare(&bdb->db, "SELECT value FROM metadata WHERE key = ?",
                      &stmt);
  if (result != ECHO_OK) {
    return result;
  }

  /* Get height */
  result = db_bind_text(&stmt, 1, "validated_tip_height");
  if (result != ECHO_OK) {
    db_stmt_finalize(&stmt);
    return result;
  }
  result = db_step(&stmt);
  if (result == ECHO_OK) {
    *height = (uint32_t)db_column_int(&stmt, 0);
  } else {
    /* No validated tip stored yet */
    db_stmt_finalize(&stmt);
    *height = 0;
    return ECHO_ERR_NOT_FOUND;
  }

  /* Get hash if requested */
  if (hash != NULL) {
    result = db_stmt_reset(&stmt);
    if (result != ECHO_OK) {
      db_stmt_finalize(&stmt);
      return ECHO_OK; /* Height was found, hash is optional */
    }
    result = db_bind_text(&stmt, 1, "validated_tip_hash");
    if (result != ECHO_OK) {
      db_stmt_finalize(&stmt);
      return ECHO_OK;
    }
    result = db_step(&stmt);
    if (result == ECHO_OK) {
      const void *blob = db_column_blob(&stmt, 0);
      int blob_size = db_column_bytes(&stmt, 0);
      if (blob != NULL && blob_size == 32) {
        memcpy(hash->bytes, blob, 32);
      }
    }
  }

  db_stmt_finalize(&stmt);
  return ECHO_OK;
}

/* ========================================================================
 * Storage Cleanup
 * ======================================================================== */

echo_result_t block_index_db_get_referenced_files(block_index_db_t *bdb,
                                                   uint32_t **files_out,
                                                   size_t *count_out) {
  if (bdb == NULL || files_out == NULL || count_out == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  *files_out = NULL;
  *count_out = 0;

  echo_result_t result;
  db_stmt_t stmt;

  /* Query for distinct file indices that have block data */
  result = db_prepare(
      &bdb->db,
      "SELECT DISTINCT data_file FROM blocks WHERE data_file >= 0 ORDER BY "
      "data_file",
      &stmt);
  if (result != ECHO_OK) {
    return result;
  }

  /* First pass: count results */
  size_t count = 0;
  while ((result = db_step(&stmt)) == ECHO_OK) {
    count++;
  }

  if (count == 0) {
    db_stmt_finalize(&stmt);
    return ECHO_OK; /* No files referenced - return empty array */
  }

  /* Allocate array */
  uint32_t *files = malloc(count * sizeof(uint32_t));
  if (files == NULL) {
    db_stmt_finalize(&stmt);
    return ECHO_ERR_MEMORY;
  }

  /* Reset and second pass: collect values */
  result = db_stmt_reset(&stmt);
  if (result != ECHO_OK) {
    free(files);
    db_stmt_finalize(&stmt);
    return result;
  }

  size_t i = 0;
  while ((result = db_step(&stmt)) == ECHO_OK && i < count) {
    files[i] = (uint32_t)db_column_int(&stmt, 0);
    i++;
  }

  db_stmt_finalize(&stmt);

  *files_out = files;
  *count_out = i;
  return ECHO_OK;
}
