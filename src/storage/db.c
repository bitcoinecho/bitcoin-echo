/*
 * Bitcoin Echo — Database Interface Implementation
 *
 * Thin wrapper around SQLite for persistent storage.
 * Provides transaction support and a clean interface for database operations.
 *
 * Build once. Build right. Stop.
 */

#include "db.h"
#include "../../lib/sqlite/sqlite3.h"
#include "echo_assert.h"
#include "echo_types.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ========================================================================
 * Database Lifecycle
 * ======================================================================== */

echo_result_t db_open(db_t *db, const char *path) {
  ECHO_ASSERT(db != NULL);
  ECHO_ASSERT(path != NULL);

  /* Initialize structure */
  memset(db, 0, sizeof(*db));
  strncpy(db->path, path, sizeof(db->path) - 1);
  db->path[sizeof(db->path) - 1] = '\0';

  /* Open or create database */
  int rc = sqlite3_open(path, &db->handle);
  if (rc != SQLITE_OK) {
    db->handle = NULL;
    return ECHO_ERR_IO;
  }

  /* Enable WAL mode for better concurrency and crash resistance */
  rc = sqlite3_exec(db->handle, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_close(db->handle);
    db->handle = NULL;
    return ECHO_ERR_IO;
  }

  /* Set synchronous to NORMAL (good balance of safety and performance) */
  rc = sqlite3_exec(db->handle, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_close(db->handle);
    db->handle = NULL;
    return ECHO_ERR_IO;
  }

  /* Enable foreign keys for referential integrity */
  rc = sqlite3_exec(db->handle, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_close(db->handle);
    db->handle = NULL;
    return ECHO_ERR_IO;
  }

  /* Performance optimizations: larger cache and memory-mapped I/O */
  sqlite3_exec(db->handle, "PRAGMA cache_size=-65536", NULL, NULL,
               NULL); /* 64MB cache */
  sqlite3_exec(db->handle, "PRAGMA mmap_size=268435456", NULL, NULL,
               NULL); /* 256MB mmap */
  sqlite3_exec(db->handle, "PRAGMA temp_store=MEMORY", NULL, NULL,
               NULL); /* Temp tables in RAM */

  return ECHO_OK;
}

echo_result_t db_set_ibd_mode(db_t *db, bool ibd_mode) {
  if (!db || !db->handle) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (ibd_mode) {
    /* IBD mode: maximum speed, less safety (can re-sync if crash) */
    sqlite3_exec(db->handle, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
  } else {
    /* Normal mode: balance of speed and safety */
    sqlite3_exec(db->handle, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
  }

  return ECHO_OK;
}

void db_close(db_t *db) {
  if (!db || !db->handle) {
    return;
  }

  /* Commit any pending transaction */
  if (db->in_transaction) {
    db_commit(db);
  }

  /* Close database */
  sqlite3_close(db->handle);
  db->handle = NULL;
}

echo_result_t db_exec(db_t *db, const char *sql) {
  ECHO_ASSERT(db != NULL);
  ECHO_ASSERT(db->handle != NULL);
  ECHO_ASSERT(sql != NULL);

  char *errmsg = NULL;
  int rc = sqlite3_exec(db->handle, sql, NULL, NULL, &errmsg);

  if (rc != SQLITE_OK) {
    if (errmsg) {
      sqlite3_free(errmsg);
    }
    return ECHO_ERR_DB;
  }

  return ECHO_OK;
}

/* ========================================================================
 * Transactions
 * ======================================================================== */

echo_result_t db_begin(db_t *db) {
  ECHO_ASSERT(db != NULL);
  ECHO_ASSERT(db->handle != NULL);

  if (db->in_transaction) {
    /* Already in transaction */
    return ECHO_ERR_INVALID;
  }

  int rc = sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    return ECHO_ERR_DB;
  }

  db->in_transaction = 1;
  return ECHO_OK;
}

echo_result_t db_commit(db_t *db) {
  ECHO_ASSERT(db != NULL);
  ECHO_ASSERT(db->handle != NULL);

  if (!db->in_transaction) {
    /* No transaction active */
    return ECHO_OK;
  }

  int rc = sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    return ECHO_ERR_DB;
  }

  db->in_transaction = 0;
  return ECHO_OK;
}

echo_result_t db_rollback(db_t *db) {
  ECHO_ASSERT(db != NULL);
  ECHO_ASSERT(db->handle != NULL);

  if (!db->in_transaction) {
    /* No transaction active */
    return ECHO_OK;
  }

  int rc = sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    return ECHO_ERR_DB;
  }

  db->in_transaction = 0;
  return ECHO_OK;
}

/* ========================================================================
 * Prepared Statements
 * ======================================================================== */

echo_result_t db_prepare(db_t *db, const char *sql, db_stmt_t *stmt) {
  ECHO_ASSERT(db != NULL);
  ECHO_ASSERT(db->handle != NULL);
  ECHO_ASSERT(sql != NULL);
  ECHO_ASSERT(stmt != NULL);

  memset(stmt, 0, sizeof(*stmt));

  int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt->stmt, NULL);
  if (rc != SQLITE_OK) {
    stmt->stmt = NULL;
    return ECHO_ERR_DB;
  }

  return ECHO_OK;
}

void db_stmt_finalize(db_stmt_t *stmt) {
  if (!stmt || !stmt->stmt) {
    return;
  }

  sqlite3_finalize(stmt->stmt);
  stmt->stmt = NULL;
}

echo_result_t db_stmt_reset(db_stmt_t *stmt) {
  ECHO_ASSERT(stmt != NULL);
  ECHO_ASSERT(stmt->stmt != NULL);

  int rc = sqlite3_reset(stmt->stmt);
  if (rc != SQLITE_OK) {
    return ECHO_ERR_DB;
  }

  /* Also clear bindings */
  sqlite3_clear_bindings(stmt->stmt);

  return ECHO_OK;
}

/* ========================================================================
 * Parameter Binding
 * ======================================================================== */

echo_result_t db_bind_int(db_stmt_t *stmt, int index, int value) {
  ECHO_ASSERT(stmt != NULL);
  ECHO_ASSERT(stmt->stmt != NULL);

  int rc = sqlite3_bind_int(stmt->stmt, index, value);
  if (rc != SQLITE_OK) {
    return ECHO_ERR_DB;
  }

  return ECHO_OK;
}

echo_result_t db_bind_int64(db_stmt_t *stmt, int index, int64_t value) {
  ECHO_ASSERT(stmt != NULL);
  ECHO_ASSERT(stmt->stmt != NULL);

  int rc = sqlite3_bind_int64(stmt->stmt, index, value);
  if (rc != SQLITE_OK) {
    return ECHO_ERR_DB;
  }

  return ECHO_OK;
}

echo_result_t db_bind_blob(db_stmt_t *stmt, int index, const void *data,
                           size_t size) {
  ECHO_ASSERT(stmt != NULL);
  ECHO_ASSERT(stmt->stmt != NULL);

  /* SQLITE_TRANSIENT means SQLite will make a copy of the data */
  int rc =
      sqlite3_bind_blob(stmt->stmt, index, data, (int)size, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return ECHO_ERR_DB;
  }

  return ECHO_OK;
}

echo_result_t db_bind_text(db_stmt_t *stmt, int index, const char *text) {
  ECHO_ASSERT(stmt != NULL);
  ECHO_ASSERT(stmt->stmt != NULL);

  /* SQLITE_TRANSIENT means SQLite will make a copy of the text */
  int rc = sqlite3_bind_text(stmt->stmt, index, text, -1, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return ECHO_ERR_DB;
  }

  return ECHO_OK;
}

/* ========================================================================
 * Statement Execution and Result Retrieval
 * ======================================================================== */

echo_result_t db_step(db_stmt_t *stmt) {
  ECHO_ASSERT(stmt != NULL);
  ECHO_ASSERT(stmt->stmt != NULL);

  int rc = sqlite3_step(stmt->stmt);

  if (rc == SQLITE_ROW) {
    return ECHO_OK; /* Row available */
  } else if (rc == SQLITE_DONE) {
    return ECHO_DONE; /* No more rows */
  } else if ((rc & 0xFF) == SQLITE_CONSTRAINT) {
    /* SQLite returns extended error codes like SQLITE_CONSTRAINT_PRIMARYKEY.
     * Mask with 0xFF to get the primary error code. */
    return ECHO_ERR_EXISTS; /* Constraint violation (e.g., duplicate key) */
  } else {
    return ECHO_ERR_DB; /* Other database error */
  }
}

int db_column_int(db_stmt_t *stmt, int index) {
  ECHO_ASSERT(stmt != NULL);
  ECHO_ASSERT(stmt->stmt != NULL);

  return sqlite3_column_int(stmt->stmt, index);
}

int64_t db_column_int64(db_stmt_t *stmt, int index) {
  ECHO_ASSERT(stmt != NULL);
  ECHO_ASSERT(stmt->stmt != NULL);

  return sqlite3_column_int64(stmt->stmt, index);
}

const void *db_column_blob(db_stmt_t *stmt, int index) {
  ECHO_ASSERT(stmt != NULL);
  ECHO_ASSERT(stmt->stmt != NULL);

  return sqlite3_column_blob(stmt->stmt, index);
}

const char *db_column_text(db_stmt_t *stmt, int index) {
  ECHO_ASSERT(stmt != NULL);
  ECHO_ASSERT(stmt->stmt != NULL);

  return (const char *)sqlite3_column_text(stmt->stmt, index);
}

int db_column_bytes(db_stmt_t *stmt, int index) {
  ECHO_ASSERT(stmt != NULL);
  ECHO_ASSERT(stmt->stmt != NULL);

  return sqlite3_column_bytes(stmt->stmt, index);
}

/* ========================================================================
 * Utility Functions
 * ======================================================================== */

int64_t db_last_insert_rowid(db_t *db) {
  ECHO_ASSERT(db != NULL);
  ECHO_ASSERT(db->handle != NULL);

  return sqlite3_last_insert_rowid(db->handle);
}

int db_changes(db_t *db) {
  ECHO_ASSERT(db != NULL);
  ECHO_ASSERT(db->handle != NULL);

  return sqlite3_changes(db->handle);
}

const char *db_errmsg(db_t *db) {
  ECHO_ASSERT(db != NULL);
  ECHO_ASSERT(db->handle != NULL);

  return sqlite3_errmsg(db->handle);
}
