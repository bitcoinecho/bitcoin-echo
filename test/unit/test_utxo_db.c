/*
 * Bitcoin Echo — UTXO Database Tests
 *
 * Comprehensive test suite for the UTXO database implementation.
 * Tests all aspects of persistent UTXO storage including:
 * - Database lifecycle (open/close)
 * - UTXO insertion and lookup
 * - UTXO deletion
 * - Batch operations
 * - Atomic block application
 * - Statistics and queries
 * - Error handling
 * - Transaction atomicity
 *
 * Build once. Build right. Stop.
 */

#include "utxo_db.h"
#include "echo_assert.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "test_utils.h"

/* Test database path (will be deleted after each test) */
#define TEST_DB_PATH "/tmp/test_utxo.db"
#define TEST_DB_WAL "/tmp/test_utxo.db-wal"
#define TEST_DB_SHM "/tmp/test_utxo.db-shm"

/* Test counter */
/* Helper: Clean up test database files */
static void cleanup_test_db(void) {
    unlink(TEST_DB_PATH);
    unlink(TEST_DB_WAL);
    unlink(TEST_DB_SHM);
}

/* Helper: Create a test outpoint */
static outpoint_t make_outpoint(uint32_t n) {
    outpoint_t op;
    memset(&op, 0, sizeof(op));
    /* Use n as a simple txid pattern */
    for (int i = 0; i < 32; i++) {
        op.txid.bytes[i] = (uint8_t)((n + i) & 0xFF);
    }
    op.vout = n;
    return op;
}

/* Helper: Create a test UTXO entry */
static utxo_entry_t *make_utxo(uint32_t n, int64_t value, uint32_t height) {
    outpoint_t op = make_outpoint(n);
    uint8_t script[] = {0x76, 0xa9, 0x14};  /* OP_DUP OP_HASH160 OP_PUSH20 */
    return utxo_entry_create(&op, value, script, sizeof(script), height, false);
}

/* ========================================================================
 * Test Cases
 * ======================================================================== */

/* Test 1: Database Open and Close */
static void test_db_open_close(void) {
    cleanup_test_db();

    utxo_db_t udb;
    echo_result_t res = utxo_db_open(&udb, TEST_DB_PATH);
    ECHO_ASSERT(res == ECHO_OK);

    utxo_db_close(&udb);

    /* Verify database file was created */
    FILE *f = fopen(TEST_DB_PATH, "rb");
    ECHO_ASSERT(f != NULL);
    fclose(f);

    cleanup_test_db();

    printf("PASS\n");
}

/* Test 2: Insert and Lookup Single UTXO */
static void test_insert_lookup(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    /* Create test UTXO */
    utxo_entry_t *entry = make_utxo(1, 50000, 100);
    ECHO_ASSERT(entry != NULL);

    /* Insert UTXO */
    echo_result_t res = utxo_db_insert(&udb, entry);
    ECHO_ASSERT(res == ECHO_OK);

    /* Lookup UTXO */
    utxo_entry_t *found = NULL;
    res = utxo_db_lookup(&udb, &entry->outpoint, &found);
    ECHO_ASSERT(res == ECHO_OK);
    ECHO_ASSERT(found != NULL);

    /* Verify fields */
    ECHO_ASSERT(found->value == 50000);
    ECHO_ASSERT(found->height == 100);
    ECHO_ASSERT(found->is_coinbase == false);
    ECHO_ASSERT(found->script_len == 3);
    ECHO_ASSERT(outpoint_equal(&found->outpoint, &entry->outpoint));

    utxo_entry_destroy(entry);
    utxo_entry_destroy(found);
    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* Test 3: Lookup Non-Existent UTXO */
static void test_lookup_not_found(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    outpoint_t op = make_outpoint(999);
    utxo_entry_t *found = NULL;
    echo_result_t res = utxo_db_lookup(&udb, &op, &found);

    ECHO_ASSERT(res == ECHO_ERR_NOT_FOUND);
    ECHO_ASSERT(found == NULL);

    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* Test 4: UTXO Exists Check */
static void test_exists(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    utxo_entry_t *entry = make_utxo(1, 50000, 100);
    ECHO_ASSERT(utxo_db_insert(&udb, entry) == ECHO_OK);

    /* Check existing UTXO */
    bool exists = false;
    ECHO_ASSERT(utxo_db_exists(&udb, &entry->outpoint, &exists) == ECHO_OK);
    ECHO_ASSERT(exists == true);

    /* Check non-existent UTXO */
    outpoint_t op = make_outpoint(999);
    ECHO_ASSERT(utxo_db_exists(&udb, &op, &exists) == ECHO_OK);
    ECHO_ASSERT(exists == false);

    utxo_entry_destroy(entry);
    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* Test 5: Delete UTXO */
static void test_delete(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    utxo_entry_t *entry = make_utxo(1, 50000, 100);
    ECHO_ASSERT(utxo_db_insert(&udb, entry) == ECHO_OK);

    /* Delete UTXO */
    echo_result_t res = utxo_db_delete(&udb, &entry->outpoint);
    ECHO_ASSERT(res == ECHO_OK);

    /* Verify it's gone */
    utxo_entry_t *found = NULL;
    res = utxo_db_lookup(&udb, &entry->outpoint, &found);
    ECHO_ASSERT(res == ECHO_ERR_NOT_FOUND);

    utxo_entry_destroy(entry);
    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* Test 6: Delete Non-Existent UTXO */
static void test_delete_not_found(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    outpoint_t op = make_outpoint(999);
    echo_result_t res = utxo_db_delete(&udb, &op);
    ECHO_ASSERT(res == ECHO_ERR_NOT_FOUND);

    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* Test 7: Batch Insert */
static void test_batch_insert(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    /* Create multiple UTXOs */
    const int count = 100;
    utxo_entry_t *entries[count];
    for (int i = 0; i < count; i++) {
        entries[i] = make_utxo(i, 1000 + i, 100 + i);
        ECHO_ASSERT(entries[i] != NULL);
    }

    /* Batch insert */
    echo_result_t res = utxo_db_insert_batch(&udb, (const utxo_entry_t **)entries, count);
    ECHO_ASSERT(res == ECHO_OK);

    /* Verify all were inserted */
    for (int i = 0; i < count; i++) {
        bool exists = false;
        ECHO_ASSERT(utxo_db_exists(&udb, &entries[i]->outpoint, &exists) == ECHO_OK);
        ECHO_ASSERT(exists == true);
    }

    /* Cleanup */
    for (int i = 0; i < count; i++) {
        utxo_entry_destroy(entries[i]);
    }

    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* Test 8: Batch Delete */
static void test_batch_delete(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    /* Create and insert multiple UTXOs */
    const int count = 50;
    utxo_entry_t *entries[count];
    outpoint_t outpoints[count];

    for (int i = 0; i < count; i++) {
        entries[i] = make_utxo(i, 1000 + i, 100);
        outpoints[i] = entries[i]->outpoint;
        ECHO_ASSERT(utxo_db_insert(&udb, entries[i]) == ECHO_OK);
    }

    /* Batch delete */
    echo_result_t res = utxo_db_delete_batch(&udb, outpoints, count);
    ECHO_ASSERT(res == ECHO_OK);

    /* Verify all were deleted */
    for (int i = 0; i < count; i++) {
        bool exists = false;
        ECHO_ASSERT(utxo_db_exists(&udb, &outpoints[i], &exists) == ECHO_OK);
        ECHO_ASSERT(exists == false);
    }

    /* Cleanup */
    for (int i = 0; i < count; i++) {
        utxo_entry_destroy(entries[i]);
    }

    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* Test 9: Apply Block (Atomic Operation) */
static void test_apply_block(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    /* Create initial UTXOs that will be spent */
    const int initial_count = 10;
    utxo_entry_t *initial[initial_count];
    for (int i = 0; i < initial_count; i++) {
        initial[i] = make_utxo(i, 5000, 100);
        ECHO_ASSERT(utxo_db_insert(&udb, initial[i]) == ECHO_OK);
    }

    /* Create new UTXOs to add */
    const int new_count = 20;
    utxo_entry_t *new_utxos[new_count];
    for (int i = 0; i < new_count; i++) {
        new_utxos[i] = make_utxo(100 + i, 1000 + i, 200);
    }

    /* Outpoints to spend (first 5 initial UTXOs) */
    const int spent_count = 5;
    outpoint_t spent[spent_count];
    for (int i = 0; i < spent_count; i++) {
        spent[i] = initial[i]->outpoint;
    }

    /* Apply block: delete 5, add 20 */
    echo_result_t res = utxo_db_apply_block(
        &udb,
        (const utxo_entry_t **)new_utxos,
        new_count,
        spent,
        spent_count
    );
    ECHO_ASSERT(res == ECHO_OK);

    /* Verify spent UTXOs are gone */
    for (int i = 0; i < spent_count; i++) {
        bool exists = false;
        ECHO_ASSERT(utxo_db_exists(&udb, &spent[i], &exists) == ECHO_OK);
        ECHO_ASSERT(exists == false);
    }

    /* Verify unspent UTXOs still exist */
    for (int i = spent_count; i < initial_count; i++) {
        bool exists = false;
        ECHO_ASSERT(utxo_db_exists(&udb, &initial[i]->outpoint, &exists) == ECHO_OK);
        ECHO_ASSERT(exists == true);
    }

    /* Verify new UTXOs were added */
    for (int i = 0; i < new_count; i++) {
        bool exists = false;
        ECHO_ASSERT(utxo_db_exists(&udb, &new_utxos[i]->outpoint, &exists) == ECHO_OK);
        ECHO_ASSERT(exists == true);
    }

    /* Cleanup */
    for (int i = 0; i < initial_count; i++) {
        utxo_entry_destroy(initial[i]);
    }
    for (int i = 0; i < new_count; i++) {
        utxo_entry_destroy(new_utxos[i]);
    }

    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* Test 10: UTXO Count */
__attribute__((unused))
static void test_count(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    /* Initially empty */
    size_t count;
    ECHO_ASSERT(utxo_db_count(&udb, &count) == ECHO_OK);
    ECHO_ASSERT(count == 0);

    /* Add some UTXOs */
    const int num = 42;
    for (int i = 0; i < num; i++) {
        utxo_entry_t *entry = make_utxo(i, 1000, 100);
        ECHO_ASSERT(utxo_db_insert(&udb, entry) == ECHO_OK);
        utxo_entry_destroy(entry);
    }

    /* Check count */
    ECHO_ASSERT(utxo_db_count(&udb, &count) == ECHO_OK);
    ECHO_ASSERT(count == num);

    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* Test 11: Total Value */
static void test_total_value(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    /* Initially zero */
    int64_t total;
    ECHO_ASSERT(utxo_db_total_value(&udb, &total) == ECHO_OK);
    ECHO_ASSERT(total == 0);

    /* Add UTXOs with known values */
    int64_t expected_total = 0;
    for (int i = 0; i < 10; i++) {
        int64_t value = 1000 + i * 100;
        expected_total += value;
        utxo_entry_t *entry = make_utxo(i, value, 100);
        ECHO_ASSERT(utxo_db_insert(&udb, entry) == ECHO_OK);
        utxo_entry_destroy(entry);
    }

    /* Check total */
    ECHO_ASSERT(utxo_db_total_value(&udb, &total) == ECHO_OK);
    ECHO_ASSERT(total == expected_total);

    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* Test 12: Persistence Across Open/Close */
static void test_persistence(void) {
    cleanup_test_db();

    /* First session: insert UTXOs */
    {
        utxo_db_t udb;
        ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

        for (int i = 0; i < 10; i++) {
            utxo_entry_t *entry = make_utxo(i, 5000 + i, 100);
            ECHO_ASSERT(utxo_db_insert(&udb, entry) == ECHO_OK);
            utxo_entry_destroy(entry);
        }

        utxo_db_close(&udb);
    }

    /* Second session: verify UTXOs persist */
    {
        utxo_db_t udb;
        ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

        size_t count;
        ECHO_ASSERT(utxo_db_count(&udb, &count) == ECHO_OK);
        ECHO_ASSERT(count == 10);

        /* Verify each UTXO */
        for (int i = 0; i < 10; i++) {
            outpoint_t op = make_outpoint(i);
            utxo_entry_t *found = NULL;
            ECHO_ASSERT(utxo_db_lookup(&udb, &op, &found) == ECHO_OK);
            ECHO_ASSERT(found->value == 5000 + i);
            utxo_entry_destroy(found);
        }

        utxo_db_close(&udb);
    }

    cleanup_test_db();

    printf("PASS\n");
}

/* Test 13: Coinbase UTXO */
static void test_coinbase_utxo(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    /* Create coinbase UTXO */
    outpoint_t op = make_outpoint(1);
    uint8_t script[] = {0x76, 0xa9};
    utxo_entry_t *entry = utxo_entry_create(&op, 5000000000LL, script, 2, 100, true);
    ECHO_ASSERT(entry != NULL);

    /* Insert and retrieve */
    ECHO_ASSERT(utxo_db_insert(&udb, entry) == ECHO_OK);

    utxo_entry_t *found = NULL;
    ECHO_ASSERT(utxo_db_lookup(&udb, &op, &found) == ECHO_OK);
    ECHO_ASSERT(found->is_coinbase == true);
    ECHO_ASSERT(found->value == 5000000000LL);
    ECHO_ASSERT(found->height == 100);

    utxo_entry_destroy(entry);
    utxo_entry_destroy(found);
    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* Test 14: Large Script Storage */
static void test_large_script(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    /* Create UTXO with large script (e.g., 10KB) */
    const size_t script_len = 10000;
    uint8_t *script = malloc(script_len);
    ECHO_ASSERT(script != NULL);
    for (size_t i = 0; i < script_len; i++) {
        script[i] = (uint8_t)(i & 0xFF);
    }

    outpoint_t op = make_outpoint(1);
    utxo_entry_t *entry = utxo_entry_create(&op, 50000, script, script_len, 100, false);
    ECHO_ASSERT(entry != NULL);

    /* Insert and retrieve */
    ECHO_ASSERT(utxo_db_insert(&udb, entry) == ECHO_OK);

    utxo_entry_t *found = NULL;
    ECHO_ASSERT(utxo_db_lookup(&udb, &op, &found) == ECHO_OK);
    ECHO_ASSERT(found->script_len == script_len);
    ECHO_ASSERT(memcmp(found->script_pubkey, script, script_len) == 0);

    free(script);
    utxo_entry_destroy(entry);
    utxo_entry_destroy(found);
    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* Helper: Iterator callback for counting */
static bool count_callback(const utxo_entry_t *entry, void *user_data) {
    (void)entry;  /* Unused */
    int *count = (int *)user_data;
    (*count)++;
    return true;  /* Continue iteration */
}

/* Test 15: Foreach Iterator */
static void test_foreach(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    /* Insert test UTXOs */
    const int count = 20;
    for (int i = 0; i < count; i++) {
        utxo_entry_t *entry = make_utxo(i, 1000 + i, 100);
        ECHO_ASSERT(utxo_db_insert(&udb, entry) == ECHO_OK);
        utxo_entry_destroy(entry);
    }

    /* Count via iterator */
    int iter_count = 0;
    echo_result_t res = utxo_db_foreach(&udb, count_callback, &iter_count);
    ECHO_ASSERT(res == ECHO_OK);
    ECHO_ASSERT(iter_count == count);

    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* Test 16: Empty Database Operations */
static void test_empty_database(void) {
    cleanup_test_db();

    utxo_db_t udb;
    ECHO_ASSERT(utxo_db_open(&udb, TEST_DB_PATH) == ECHO_OK);

    /* Count on empty database */
    size_t count;
    ECHO_ASSERT(utxo_db_count(&udb, &count) == ECHO_OK);
    ECHO_ASSERT(count == 0);

    /* Total value on empty database */
    int64_t total;
    ECHO_ASSERT(utxo_db_total_value(&udb, &total) == ECHO_OK);
    ECHO_ASSERT(total == 0);

    /* Apply empty block */
    ECHO_ASSERT(utxo_db_apply_block(&udb, NULL, 0, NULL, 0) == ECHO_OK);

    utxo_db_close(&udb);
    cleanup_test_db();

    printf("PASS\n");
}

/* ========================================================================
 * Main Test Runner
 * ======================================================================== */

int main(void) {
    test_suite_begin("Utxo Db Tests");

    test_case("Db open close"); test_db_open_close(); test_pass();
    test_case("Insert lookup"); test_insert_lookup(); test_pass();
    test_case("Lookup not found"); test_lookup_not_found(); test_pass();
    test_case("Exists"); test_exists(); test_pass();
    test_case("Delete"); test_delete(); test_pass();
    test_case("Delete not found"); test_delete_not_found(); test_pass();
    test_case("Batch insert"); test_batch_insert(); test_pass();
    test_case("Batch delete"); test_batch_delete(); test_pass();
    test_case("Apply block"); test_apply_block(); test_pass();
    test_case("Total value"); test_total_value(); test_pass();
    test_case("Persistence"); test_persistence(); test_pass();
    test_case("Coinbase utxo"); test_coinbase_utxo(); test_pass();
    test_case("Large script"); test_large_script(); test_pass();
    test_case("Foreach"); test_foreach(); test_pass();
    test_case("Empty database"); test_empty_database(); test_pass();

    test_suite_end();
    return test_global_summary();
}
