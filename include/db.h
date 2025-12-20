/*
 * Bitcoin Echo — Database Interface
 *
 * Thin wrapper around SQLite for persistent storage of UTXO set and block
 * index. Provides a clean interface for database operations with transaction
 * support.
 *
 * This module integrates the SQLite amalgamation embedded in lib/sqlite/.
 * SQLite is public domain and aligns with the project's philosophy of minimal
 * external dependencies with frozen, stable code.
 *
 * All database operations are wrapped in transactions for atomicity. The
 * interface is designed to be simple and focused on the two databases we need:
 * - UTXO database (chainstate)
 * - Block index database
 *
 * WAL (Write-Ahead Logging) mode is enabled for better concurrency and crash
 * resistance.
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_DB_H
#define ECHO_DB_H

#include "echo_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration of SQLite database handle (opaque to users) */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

/*
 * Database handle.
 * Represents an open SQLite database connection.
 */
typedef struct {
  sqlite3 *handle;    /* SQLite database handle */
  char path[512];     /* Path to database file */
  int in_transaction; /* Whether a transaction is active */
} db_t;

/*
 * Prepared statement handle.
 * Represents a compiled SQL statement ready for execution.
 */
typedef struct {
  sqlite3_stmt *stmt; /* SQLite statement handle */
} db_stmt_t;

/* ========================================================================
 * Database Lifecycle
 * ======================================================================== */

/**
 * Open or create a database.
 *
 * Parameters:
 *   db   - Database handle to initialize
 *   path - Path to database file (will be created if doesn't exist)
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Creates the database file and parent directories if needed
 *   - Enables WAL mode for better concurrency
 *   - Sets synchronous=NORMAL for good balance of safety and performance
 *   - Sets foreign_keys=ON for referential integrity
 */
echo_result_t db_open(db_t *db, const char *path);

/**
 * Close a database.
 *
 * Parameters:
 *   db - Database handle to close
 *
 * Notes:
 *   - Commits any pending transaction
 *   - Releases all resources
 *   - Safe to call on already-closed database
 */
void db_close(db_t *db);

/**
 * Execute a SQL statement that doesn't return results.
 *
 * Parameters:
 *   db  - Database handle
 *   sql - SQL statement to execute
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Use for CREATE TABLE, INSERT, UPDATE, DELETE, etc.
 *   - For queries that return results, use prepared statements
 */
echo_result_t db_exec(db_t *db, const char *sql);

/* ========================================================================
 * Transactions
 * ======================================================================== */

/**
 * Begin a transaction.
 *
 * Parameters:
 *   db - Database handle
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Nested transactions are not supported
 *   - Call db_commit() or db_rollback() to end the transaction
 */
echo_result_t db_begin(db_t *db);

/**
 * Commit the current transaction.
 *
 * Parameters:
 *   db - Database handle
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - All changes since db_begin() are made permanent
 *   - No-op if no transaction is active
 */
echo_result_t db_commit(db_t *db);

/**
 * Rollback the current transaction.
 *
 * Parameters:
 *   db - Database handle
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - All changes since db_begin() are discarded
 *   - No-op if no transaction is active
 */
echo_result_t db_rollback(db_t *db);

/* ========================================================================
 * Prepared Statements
 * ======================================================================== */

/**
 * Prepare a SQL statement for execution.
 *
 * Parameters:
 *   db   - Database handle
 *   sql  - SQL statement with optional ? placeholders
 *   stmt - Output: prepared statement
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Use ? as placeholders for bound parameters
 *   - Must call db_stmt_finalize() when done
 *   - Example: "SELECT * FROM utxo WHERE outpoint = ?"
 */
echo_result_t db_prepare(db_t *db, const char *sql, db_stmt_t *stmt);

/**
 * Finalize a prepared statement and release resources.
 *
 * Parameters:
 *   stmt - Prepared statement to finalize
 *
 * Notes:
 *   - Safe to call on already-finalized statement
 *   - Always call this when done with a statement
 */
void db_stmt_finalize(db_stmt_t *stmt);

/**
 * Reset a prepared statement for re-execution.
 *
 * Parameters:
 *   stmt - Prepared statement to reset
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Clears bindings and result set
 *   - Allows re-executing the same statement with different parameters
 */
echo_result_t db_stmt_reset(db_stmt_t *stmt);

/* ========================================================================
 * Parameter Binding
 * ======================================================================== */

/**
 * Bind an integer parameter to a prepared statement.
 *
 * Parameters:
 *   stmt  - Prepared statement
 *   index - Parameter index (1-based)
 *   value - Integer value to bind
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t db_bind_int(db_stmt_t *stmt, int index, int value);

/**
 * Bind a 64-bit integer parameter to a prepared statement.
 *
 * Parameters:
 *   stmt  - Prepared statement
 *   index - Parameter index (1-based)
 *   value - 64-bit integer value to bind
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t db_bind_int64(db_stmt_t *stmt, int index, int64_t value);

/**
 * Bind a blob (binary data) parameter to a prepared statement.
 *
 * Parameters:
 *   stmt  - Prepared statement
 *   index - Parameter index (1-based)
 *   data  - Pointer to binary data
 *   size  - Size of data in bytes
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Makes a copy of the data; caller retains ownership
 */
echo_result_t db_bind_blob(db_stmt_t *stmt, int index, const void *data,
                           size_t size);

/**
 * Bind a text (string) parameter to a prepared statement.
 *
 * Parameters:
 *   stmt  - Prepared statement
 *   index - Parameter index (1-based)
 *   text  - Null-terminated string
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Makes a copy of the string; caller retains ownership
 */
echo_result_t db_bind_text(db_stmt_t *stmt, int index, const char *text);

/* ========================================================================
 * Statement Execution and Result Retrieval
 * ======================================================================== */

/**
 * Execute a prepared statement and advance to next row.
 *
 * Parameters:
 *   stmt - Prepared statement
 *
 * Returns:
 *   ECHO_OK if a row is available
 *   ECHO_DONE if no more rows
 *   error code on failure
 *
 * Notes:
 *   - Call this once per result row
 *   - Use db_column_* functions to retrieve column values
 *   - Returns ECHO_DONE when no more rows are available
 */
echo_result_t db_step(db_stmt_t *stmt);

/**
 * Get integer value from current row.
 *
 * Parameters:
 *   stmt  - Prepared statement
 *   index - Column index (0-based)
 *
 * Returns:
 *   Integer value from column
 *
 * Notes:
 *   - Only valid after db_step() returns ECHO_OK
 */
int db_column_int(db_stmt_t *stmt, int index);

/**
 * Get 64-bit integer value from current row.
 *
 * Parameters:
 *   stmt  - Prepared statement
 *   index - Column index (0-based)
 *
 * Returns:
 *   64-bit integer value from column
 *
 * Notes:
 *   - Only valid after db_step() returns ECHO_OK
 */
int64_t db_column_int64(db_stmt_t *stmt, int index);

/**
 * Get blob (binary data) value from current row.
 *
 * Parameters:
 *   stmt  - Prepared statement
 *   index - Column index (0-based)
 *
 * Returns:
 *   Pointer to blob data (owned by statement, do not free)
 *
 * Notes:
 *   - Only valid after db_step() returns ECHO_OK
 *   - Pointer is valid until next db_step() or db_stmt_reset()
 *   - Use db_column_bytes() to get size
 */
const void *db_column_blob(db_stmt_t *stmt, int index);

/**
 * Get text (string) value from current row.
 *
 * Parameters:
 *   stmt  - Prepared statement
 *   index - Column index (0-based)
 *
 * Returns:
 *   Pointer to null-terminated string (owned by statement, do not free)
 *
 * Notes:
 *   - Only valid after db_step() returns ECHO_OK
 *   - Pointer is valid until next db_step() or db_stmt_reset()
 */
const char *db_column_text(db_stmt_t *stmt, int index);

/**
 * Get size of blob or text column in bytes.
 *
 * Parameters:
 *   stmt  - Prepared statement
 *   index - Column index (0-based)
 *
 * Returns:
 *   Size of column data in bytes
 *
 * Notes:
 *   - Only valid after db_step() returns ECHO_OK
 *   - For text, this is strlen() (excludes null terminator)
 */
int db_column_bytes(db_stmt_t *stmt, int index);

/* ========================================================================
 * Utility Functions
 * ======================================================================== */

/**
 * Get the row ID of the last inserted row.
 *
 * Parameters:
 *   db - Database handle
 *
 * Returns:
 *   Row ID of last INSERT, or 0 if no inserts
 */
int64_t db_last_insert_rowid(db_t *db);

/**
 * Get number of rows modified by last statement.
 *
 * Parameters:
 *   db - Database handle
 *
 * Returns:
 *   Number of rows changed by last INSERT, UPDATE, or DELETE
 */
int db_changes(db_t *db);

/**
 * Get last error message from database.
 *
 * Parameters:
 *   db - Database handle
 *
 * Returns:
 *   Human-readable error message (owned by db, do not free)
 *
 * Notes:
 *   - Valid until next database operation
 */
const char *db_errmsg(db_t *db);

/**
 * Set IBD (Initial Block Download) mode for performance optimization.
 *
 * Parameters:
 *   db       - Database handle
 *   ibd_mode - true to enable IBD mode (fast, less safe),
 *              false for normal mode (balanced safety/speed)
 *
 * Returns:
 *   ECHO_OK on success
 *
 * Notes:
 *   - IBD mode uses synchronous=OFF for maximum write speed
 *   - Normal mode uses synchronous=NORMAL
 *   - Only use IBD mode during initial sync (can re-sync if crash)
 */
echo_result_t db_set_ibd_mode(db_t *db, bool ibd_mode);

#endif /* ECHO_DB_H */
