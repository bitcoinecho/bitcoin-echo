/*
 * Bitcoin Echo — Database Integration Tests
 *
 * Comprehensive tests for the SQLite wrapper layer.
 * Tests cover database lifecycle, transactions, prepared statements,
 * parameter binding, and result retrieval.
 *
 * Build once. Build right. Stop.
 */

#include "../../include/db.h"
#include "../../include/echo_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_utils.h"


#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("\n    Assertion failed: %s\n", #cond); \
        return; \
    } \
} while (0)

/* Test database path */
#define TEST_DB_PATH "/tmp/test_echo_db.sqlite"

/* Helper: Remove test database file */
static void cleanup_test_db(void)
{
    unlink(TEST_DB_PATH);
    unlink("/tmp/test_echo_db.sqlite-shm");
    unlink("/tmp/test_echo_db.sqlite-wal");
}

/* ========================================================================
 * Database Lifecycle Tests
 * ======================================================================== */

static void test_db_open_close(void)
{
    db_t db;

    cleanup_test_db();

    /* Open database */
    echo_result_t res = db_open(&db, TEST_DB_PATH);
    ASSERT(res == ECHO_OK);
    ASSERT(db.handle != NULL);
    ASSERT(strcmp(db.path, TEST_DB_PATH) == 0);
    ASSERT(db.in_transaction == 0);

    /* Close database */
    db_close(&db);
    ASSERT(db.handle == NULL);

    /* Double close should be safe */
    db_close(&db);

    cleanup_test_db();
}

static void test_db_exec_create_table(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);

    /* Create a simple table */
    const char *sql = "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)";
    echo_result_t res = db_exec(&db, sql);
    ASSERT(res == ECHO_OK);

    /* Verify table exists by trying to create it again (should fail) */
    res = db_exec(&db, sql);
    ASSERT(res == ECHO_ERR_DB);

    db_close(&db);
    cleanup_test_db();
}

static void test_db_exec_insert(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);

    /* Create table */
    ASSERT(db_exec(&db, "CREATE TABLE test (id INTEGER PRIMARY KEY, value INTEGER)") == ECHO_OK);

    /* Insert some rows */
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (42)") == ECHO_OK);
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (100)") == ECHO_OK);

    /* Verify last insert rowid */
    int64_t rowid = db_last_insert_rowid(&db);
    ASSERT(rowid == 2);

    /* Verify changes */
    int changes = db_changes(&db);
    ASSERT(changes == 1);  /* Last INSERT changed 1 row */

    db_close(&db);
    cleanup_test_db();
}

/* ========================================================================
 * Transaction Tests
 * ======================================================================== */

static void test_db_transaction_commit(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (value INTEGER)") == ECHO_OK);

    /* Begin transaction */
    ASSERT(db_begin(&db) == ECHO_OK);
    ASSERT(db.in_transaction == 1);

    /* Nested begin should fail */
    ASSERT(db_begin(&db) == ECHO_ERR_INVALID);

    /* Insert data */
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (123)") == ECHO_OK);

    /* Commit transaction */
    ASSERT(db_commit(&db) == ECHO_OK);
    ASSERT(db.in_transaction == 0);

    /* Verify data persisted */
    db_stmt_t stmt;
    ASSERT(db_prepare(&db, "SELECT value FROM test", &stmt) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_OK);
    ASSERT(db_column_int(&stmt, 0) == 123);
    db_stmt_finalize(&stmt);

    db_close(&db);
    cleanup_test_db();
}

static void test_db_transaction_rollback(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (value INTEGER)") == ECHO_OK);

    /* Begin transaction */
    ASSERT(db_begin(&db) == ECHO_OK);

    /* Insert data */
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (456)") == ECHO_OK);

    /* Rollback transaction */
    ASSERT(db_rollback(&db) == ECHO_OK);
    ASSERT(db.in_transaction == 0);

    /* Verify data was not persisted */
    db_stmt_t stmt;
    ASSERT(db_prepare(&db, "SELECT COUNT(*) FROM test", &stmt) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_OK);
    ASSERT(db_column_int(&stmt, 0) == 0);
    db_stmt_finalize(&stmt);

    db_close(&db);
    cleanup_test_db();
}

static void test_db_close_commits_transaction(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (value INTEGER)") == ECHO_OK);

    /* Begin transaction and insert data */
    ASSERT(db_begin(&db) == ECHO_OK);
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (789)") == ECHO_OK);

    /* Close without explicit commit */
    db_close(&db);

    /* Reopen and verify data was committed */
    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    db_stmt_t stmt;
    ASSERT(db_prepare(&db, "SELECT value FROM test", &stmt) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_OK);
    ASSERT(db_column_int(&stmt, 0) == 789);
    db_stmt_finalize(&stmt);

    db_close(&db);
    cleanup_test_db();
}

/* ========================================================================
 * Prepared Statement Tests
 * ======================================================================== */

static void test_db_prepare_finalize(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (id INTEGER PRIMARY KEY)") == ECHO_OK);

    /* Prepare statement */
    db_stmt_t stmt;
    echo_result_t res = db_prepare(&db, "SELECT * FROM test", &stmt);
    ASSERT(res == ECHO_OK);
    ASSERT(stmt.stmt != NULL);

    /* Finalize statement */
    db_stmt_finalize(&stmt);
    ASSERT(stmt.stmt == NULL);

    /* Double finalize should be safe */
    db_stmt_finalize(&stmt);

    db_close(&db);
    cleanup_test_db();
}

static void test_db_prepare_invalid_sql(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);

    /* Prepare invalid SQL */
    db_stmt_t stmt;
    echo_result_t res = db_prepare(&db, "INVALID SQL", &stmt);
    ASSERT(res == ECHO_ERR_DB);

    db_close(&db);
    cleanup_test_db();
}

static void test_db_stmt_reset(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (value INTEGER)") == ECHO_OK);
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (1)") == ECHO_OK);
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (2)") == ECHO_OK);

    /* Prepare statement */
    db_stmt_t stmt;
    ASSERT(db_prepare(&db, "SELECT value FROM test", &stmt) == ECHO_OK);

    /* Execute first time */
    ASSERT(db_step(&stmt) == ECHO_OK);
    ASSERT(db_column_int(&stmt, 0) == 1);
    ASSERT(db_step(&stmt) == ECHO_OK);
    ASSERT(db_column_int(&stmt, 0) == 2);
    ASSERT(db_step(&stmt) == ECHO_DONE);

    /* Reset and execute again */
    ASSERT(db_stmt_reset(&stmt) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_OK);
    ASSERT(db_column_int(&stmt, 0) == 1);

    db_stmt_finalize(&stmt);
    db_close(&db);
    cleanup_test_db();
}

/* ========================================================================
 * Parameter Binding Tests
 * ======================================================================== */

static void test_db_bind_int(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (value INTEGER)") == ECHO_OK);

    /* Prepare insert statement */
    db_stmt_t stmt;
    ASSERT(db_prepare(&db, "INSERT INTO test (value) VALUES (?)", &stmt) == ECHO_OK);

    /* Bind and execute */
    ASSERT(db_bind_int(&stmt, 1, 42) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_DONE);

    db_stmt_finalize(&stmt);

    /* Verify */
    ASSERT(db_prepare(&db, "SELECT value FROM test", &stmt) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_OK);
    ASSERT(db_column_int(&stmt, 0) == 42);

    db_stmt_finalize(&stmt);
    db_close(&db);
    cleanup_test_db();
}

static void test_db_bind_int64(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (value INTEGER)") == ECHO_OK);

    /* Prepare insert statement */
    db_stmt_t stmt;
    ASSERT(db_prepare(&db, "INSERT INTO test (value) VALUES (?)", &stmt) == ECHO_OK);

    /* Bind large value */
    int64_t large_value = 9223372036854775807LL;  /* INT64_MAX */
    ASSERT(db_bind_int64(&stmt, 1, large_value) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_DONE);

    db_stmt_finalize(&stmt);

    /* Verify */
    ASSERT(db_prepare(&db, "SELECT value FROM test", &stmt) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_OK);
    ASSERT(db_column_int64(&stmt, 0) == large_value);

    db_stmt_finalize(&stmt);
    db_close(&db);
    cleanup_test_db();
}

static void test_db_bind_blob(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (data BLOB)") == ECHO_OK);

    /* Prepare insert statement */
    db_stmt_t stmt;
    ASSERT(db_prepare(&db, "INSERT INTO test (data) VALUES (?)", &stmt) == ECHO_OK);

    /* Bind blob */
    uint8_t blob_data[32];
    for (int i = 0; i < 32; i++) {
        blob_data[i] = (uint8_t)i;
    }
    ASSERT(db_bind_blob(&stmt, 1, blob_data, 32) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_DONE);

    db_stmt_finalize(&stmt);

    /* Verify */
    ASSERT(db_prepare(&db, "SELECT data FROM test", &stmt) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_OK);

    const void *retrieved_blob = db_column_blob(&stmt, 0);
    int blob_size = db_column_bytes(&stmt, 0);
    ASSERT(blob_size == 32);
    ASSERT(memcmp(retrieved_blob, blob_data, 32) == 0);

    db_stmt_finalize(&stmt);
    db_close(&db);
    cleanup_test_db();
}

static void test_db_bind_text(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (name TEXT)") == ECHO_OK);

    /* Prepare insert statement */
    db_stmt_t stmt;
    ASSERT(db_prepare(&db, "INSERT INTO test (name) VALUES (?)", &stmt) == ECHO_OK);

    /* Bind text */
    const char *text = "Bitcoin Echo";
    ASSERT(db_bind_text(&stmt, 1, text) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_DONE);

    db_stmt_finalize(&stmt);

    /* Verify */
    ASSERT(db_prepare(&db, "SELECT name FROM test", &stmt) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_OK);

    const char *retrieved_text = db_column_text(&stmt, 0);
    int text_size = db_column_bytes(&stmt, 0);
    ASSERT(strcmp(retrieved_text, text) == 0);
    ASSERT(text_size == (int)strlen(text));

    db_stmt_finalize(&stmt);
    db_close(&db);
    cleanup_test_db();
}

static void test_db_bind_multiple_parameters(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (id INTEGER, name TEXT, value INTEGER)") == ECHO_OK);

    /* Prepare insert statement with multiple parameters */
    db_stmt_t stmt;
    ASSERT(db_prepare(&db, "INSERT INTO test (id, name, value) VALUES (?, ?, ?)", &stmt) == ECHO_OK);

    /* Bind parameters */
    ASSERT(db_bind_int(&stmt, 1, 1) == ECHO_OK);
    ASSERT(db_bind_text(&stmt, 2, "Test") == ECHO_OK);
    ASSERT(db_bind_int64(&stmt, 3, 100000) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_DONE);

    db_stmt_finalize(&stmt);

    /* Verify */
    ASSERT(db_prepare(&db, "SELECT id, name, value FROM test", &stmt) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_OK);
    ASSERT(db_column_int(&stmt, 0) == 1);
    ASSERT(strcmp(db_column_text(&stmt, 1), "Test") == 0);
    ASSERT(db_column_int64(&stmt, 2) == 100000);

    db_stmt_finalize(&stmt);
    db_close(&db);
    cleanup_test_db();
}

/* ========================================================================
 * Result Retrieval Tests
 * ======================================================================== */

static void test_db_step_multiple_rows(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (value INTEGER)") == ECHO_OK);
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (1)") == ECHO_OK);
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (2)") == ECHO_OK);
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (3)") == ECHO_OK);

    /* Query all rows */
    db_stmt_t stmt;
    ASSERT(db_prepare(&db, "SELECT value FROM test ORDER BY value", &stmt) == ECHO_OK);

    /* Row 1 */
    ASSERT(db_step(&stmt) == ECHO_OK);
    ASSERT(db_column_int(&stmt, 0) == 1);

    /* Row 2 */
    ASSERT(db_step(&stmt) == ECHO_OK);
    ASSERT(db_column_int(&stmt, 0) == 2);

    /* Row 3 */
    ASSERT(db_step(&stmt) == ECHO_OK);
    ASSERT(db_column_int(&stmt, 0) == 3);

    /* No more rows */
    ASSERT(db_step(&stmt) == ECHO_DONE);

    db_stmt_finalize(&stmt);
    db_close(&db);
    cleanup_test_db();
}

static void test_db_column_types(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (i INTEGER, i64 INTEGER, b BLOB, t TEXT)") == ECHO_OK);

    /* Insert mixed types */
    db_stmt_t insert_stmt;
    ASSERT(db_prepare(&db, "INSERT INTO test VALUES (?, ?, ?, ?)", &insert_stmt) == ECHO_OK);

    uint8_t blob[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT(db_bind_int(&insert_stmt, 1, 42) == ECHO_OK);
    ASSERT(db_bind_int64(&insert_stmt, 2, 9999999999LL) == ECHO_OK);
    ASSERT(db_bind_blob(&insert_stmt, 3, blob, 4) == ECHO_OK);
    ASSERT(db_bind_text(&insert_stmt, 4, "Hello") == ECHO_OK);
    ASSERT(db_step(&insert_stmt) == ECHO_DONE);
    db_stmt_finalize(&insert_stmt);

    /* Query and verify all types */
    db_stmt_t select_stmt;
    ASSERT(db_prepare(&db, "SELECT i, i64, b, t FROM test", &select_stmt) == ECHO_OK);
    ASSERT(db_step(&select_stmt) == ECHO_OK);

    ASSERT(db_column_int(&select_stmt, 0) == 42);
    ASSERT(db_column_int64(&select_stmt, 1) == 9999999999LL);

    const void *retrieved_blob = db_column_blob(&select_stmt, 2);
    ASSERT(db_column_bytes(&select_stmt, 2) == 4);
    ASSERT(memcmp(retrieved_blob, blob, 4) == 0);

    const char *retrieved_text = db_column_text(&select_stmt, 3);
    ASSERT(strcmp(retrieved_text, "Hello") == 0);
    ASSERT(db_column_bytes(&select_stmt, 3) == 5);

    db_stmt_finalize(&select_stmt);
    db_close(&db);
    cleanup_test_db();
}

/* ========================================================================
 * Utility Function Tests
 * ======================================================================== */

static void test_db_last_insert_rowid(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (id INTEGER PRIMARY KEY, value INTEGER)") == ECHO_OK);

    /* Insert rows and check rowid */
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (10)") == ECHO_OK);
    ASSERT(db_last_insert_rowid(&db) == 1);

    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (20)") == ECHO_OK);
    ASSERT(db_last_insert_rowid(&db) == 2);

    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (30)") == ECHO_OK);
    ASSERT(db_last_insert_rowid(&db) == 3);

    db_close(&db);
    cleanup_test_db();
}

static void test_db_changes(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);
    ASSERT(db_exec(&db, "CREATE TABLE test (value INTEGER)") == ECHO_OK);
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (1)") == ECHO_OK);
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (2)") == ECHO_OK);
    ASSERT(db_exec(&db, "INSERT INTO test (value) VALUES (3)") == ECHO_OK);

    /* Update multiple rows */
    ASSERT(db_exec(&db, "UPDATE test SET value = 100") == ECHO_OK);
    ASSERT(db_changes(&db) == 3);

    /* Delete rows */
    ASSERT(db_exec(&db, "DELETE FROM test WHERE value = 100") == ECHO_OK);
    ASSERT(db_changes(&db) == 3);

    db_close(&db);
    cleanup_test_db();
}

static void test_db_errmsg(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);

    /* Trigger an error */
    echo_result_t res = db_exec(&db, "INVALID SQL");
    ASSERT(res == ECHO_ERR_DB);

    /* Get error message */
    const char *errmsg = db_errmsg(&db);
    ASSERT(errmsg != NULL);
    ASSERT(strlen(errmsg) > 0);

    db_close(&db);
    cleanup_test_db();
}

/* ========================================================================
 * WAL Mode Tests
 * ======================================================================== */

static void test_db_wal_mode_enabled(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);

    /* Query journal mode */
    db_stmt_t stmt;
    ASSERT(db_prepare(&db, "PRAGMA journal_mode", &stmt) == ECHO_OK);
    ASSERT(db_step(&stmt) == ECHO_OK);

    const char *journal_mode = db_column_text(&stmt, 0);
    ASSERT(strcmp(journal_mode, "wal") == 0);

    db_stmt_finalize(&stmt);
    db_close(&db);
    cleanup_test_db();
}

/* ========================================================================
 * Integration Test: UTXO-like Table
 * ======================================================================== */

static void test_db_utxo_like_table(void)
{
    db_t db;

    cleanup_test_db();

    ASSERT(db_open(&db, TEST_DB_PATH) == ECHO_OK);

    /* Create UTXO-like table (simplified version) */
    const char *create_sql =
        "CREATE TABLE utxo ("
        "  outpoint BLOB PRIMARY KEY,"
        "  value INTEGER NOT NULL,"
        "  script BLOB NOT NULL,"
        "  height INTEGER NOT NULL,"
        "  coinbase INTEGER NOT NULL"
        ")";
    ASSERT(db_exec(&db, create_sql) == ECHO_OK);

    /* Insert a UTXO */
    db_stmt_t insert_stmt;
    ASSERT(db_prepare(&db, "INSERT INTO utxo VALUES (?, ?, ?, ?, ?)", &insert_stmt) == ECHO_OK);

    uint8_t outpoint[36];
    memset(outpoint, 0, 32);  /* txid */
    outpoint[32] = 0;         /* vout (little-endian) */
    outpoint[33] = 0;
    outpoint[34] = 0;
    outpoint[35] = 0;

    uint8_t script[] = {0x76, 0xa9, 0x14};  /* Simplified scriptPubKey */

    ASSERT(db_bind_blob(&insert_stmt, 1, outpoint, 36) == ECHO_OK);
    ASSERT(db_bind_int64(&insert_stmt, 2, 5000000000LL) == ECHO_OK);  /* 50 BTC */
    ASSERT(db_bind_blob(&insert_stmt, 3, script, 3) == ECHO_OK);
    ASSERT(db_bind_int(&insert_stmt, 4, 100) == ECHO_OK);
    ASSERT(db_bind_int(&insert_stmt, 5, 1) == ECHO_OK);  /* is_coinbase */
    ASSERT(db_step(&insert_stmt) == ECHO_DONE);
    db_stmt_finalize(&insert_stmt);

    /* Query the UTXO */
    db_stmt_t select_stmt;
    ASSERT(db_prepare(&db, "SELECT value, height, coinbase FROM utxo WHERE outpoint = ?", &select_stmt) == ECHO_OK);
    ASSERT(db_bind_blob(&select_stmt, 1, outpoint, 36) == ECHO_OK);
    ASSERT(db_step(&select_stmt) == ECHO_OK);

    ASSERT(db_column_int64(&select_stmt, 0) == 5000000000LL);
    ASSERT(db_column_int(&select_stmt, 1) == 100);
    ASSERT(db_column_int(&select_stmt, 2) == 1);

    db_stmt_finalize(&select_stmt);

    /* Delete the UTXO (spending) */
    db_stmt_t delete_stmt;
    ASSERT(db_prepare(&db, "DELETE FROM utxo WHERE outpoint = ?", &delete_stmt) == ECHO_OK);
    ASSERT(db_bind_blob(&delete_stmt, 1, outpoint, 36) == ECHO_OK);
    ASSERT(db_step(&delete_stmt) == ECHO_DONE);
    ASSERT(db_changes(&db) == 1);
    db_stmt_finalize(&delete_stmt);

    /* Verify deletion */
    ASSERT(db_prepare(&db, "SELECT COUNT(*) FROM utxo", &select_stmt) == ECHO_OK);
    ASSERT(db_step(&select_stmt) == ECHO_OK);
    ASSERT(db_column_int(&select_stmt, 0) == 0);
    db_stmt_finalize(&select_stmt);

    db_close(&db);
    cleanup_test_db();
}

/* ========================================================================
 * Main
 * ======================================================================== */

int main(void) {
    test_suite_begin("Database Integration Tests");

    test_section("Database Lifecycle");
    test_case("Open and close database"); test_db_open_close(); test_pass();
    test_case("Execute CREATE TABLE"); test_db_exec_create_table(); test_pass();
    test_case("Execute INSERT"); test_db_exec_insert(); test_pass();

    test_section("Transactions");
    test_case("Commit transaction"); test_db_transaction_commit(); test_pass();
    test_case("Rollback transaction"); test_db_transaction_rollback(); test_pass();
    test_case("Close commits transaction"); test_db_close_commits_transaction(); test_pass();

    test_section("Prepared Statements");
    test_case("Prepare and finalize statement"); test_db_prepare_finalize(); test_pass();
    test_case("Reject invalid SQL"); test_db_prepare_invalid_sql(); test_pass();
    test_case("Reset statement"); test_db_stmt_reset(); test_pass();

    test_section("Parameter Binding");
    test_case("Bind integer"); test_db_bind_int(); test_pass();
    test_case("Bind int64"); test_db_bind_int64(); test_pass();
    test_case("Bind blob"); test_db_bind_blob(); test_pass();
    test_case("Bind text"); test_db_bind_text(); test_pass();
    test_case("Bind multiple parameters"); test_db_bind_multiple_parameters(); test_pass();

    test_section("Result Retrieval");
    test_case("Step through multiple rows"); test_db_step_multiple_rows(); test_pass();
    test_case("Retrieve different column types"); test_db_column_types(); test_pass();

    test_section("Utility Functions");
    test_case("Get last insert rowid"); test_db_last_insert_rowid(); test_pass();
    test_case("Get change count"); test_db_changes(); test_pass();
    test_case("Get error message"); test_db_errmsg(); test_pass();

    test_section("WAL Mode");
    test_case("WAL mode enabled by default"); test_db_wal_mode_enabled(); test_pass();

    test_section("Integration");
    test_case("UTXO-like table operations"); test_db_utxo_like_table(); test_pass();

    test_suite_end();
    return test_global_summary();
}
