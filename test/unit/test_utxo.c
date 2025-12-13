/**
 * @file test_utxo.c
 * @brief Unit tests for UTXO set implementation
 */

#include "utxo.h"
#include "echo_types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "test_utils.h"


#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            printf("\n  Assertion failed: %s\n  at %s:%d\n", \
                   #cond, __FILE__, __LINE__); \
            return; \
        } \
    } while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_TRUE(x) ASSERT(x)
#define ASSERT_FALSE(x) ASSERT(!(x))
#define ASSERT_NULL(x) ASSERT((x) == NULL)
#define ASSERT_NOT_NULL(x) ASSERT((x) != NULL)

/* ========================================================================
 * Helper Functions
 * ======================================================================== */

/**
 * Create a test outpoint
 */
static outpoint_t make_outpoint(uint8_t seed, uint32_t vout) {
    outpoint_t op;
    memset(op.txid.bytes, seed, 32);
    op.vout = vout;
    return op;
}

/**
 * Create a test UTXO entry
 */
static utxo_entry_t *make_utxo(
    uint8_t seed,
    uint32_t vout,
    int64_t value,
    uint32_t height,
    bool is_coinbase
) {
    outpoint_t op = make_outpoint(seed, vout);

    /* Simple scriptPubKey: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG */
    uint8_t script[25];
    script[0] = 0x76;  /* OP_DUP */
    script[1] = 0xa9;  /* OP_HASH160 */
    script[2] = 0x14;  /* Push 20 bytes */
    memset(script + 3, seed, 20);
    script[23] = 0x88;  /* OP_EQUALVERIFY */
    script[24] = 0xac;  /* OP_CHECKSIG */

    return utxo_entry_create(&op, value, script, 25, height, is_coinbase);
}

/* ========================================================================
 * Outpoint Tests
 * ======================================================================== */

static void test_outpoint_equal(void) {
    outpoint_t a = make_outpoint(0x01, 0);
    outpoint_t b = make_outpoint(0x01, 0);
    outpoint_t c = make_outpoint(0x02, 0);
    outpoint_t d = make_outpoint(0x01, 1);

    ASSERT_TRUE(outpoint_equal(&a, &b));
    ASSERT_FALSE(outpoint_equal(&a, &c));
    ASSERT_FALSE(outpoint_equal(&a, &d));
}

static void test_outpoint_serialize(void) {
    outpoint_t op = make_outpoint(0xAB, 0x12345678);
    uint8_t buf[36];

    size_t written = outpoint_serialize(&op, buf);
    ASSERT_EQ(written, 36);

    /* Check txid */
    for (int i = 0; i < 32; i++) {
        ASSERT_EQ(buf[i], 0xAB);
    }

    /* Check vout (little-endian) */
    ASSERT_EQ(buf[32], 0x78);
    ASSERT_EQ(buf[33], 0x56);
    ASSERT_EQ(buf[34], 0x34);
    ASSERT_EQ(buf[35], 0x12);
}

static void test_outpoint_deserialize(void) {
    uint8_t data[36];
    memset(data, 0xCD, 32);
    data[32] = 0x11;
    data[33] = 0x22;
    data[34] = 0x33;
    data[35] = 0x44;

    outpoint_t op;
    size_t read = outpoint_deserialize(data, &op);
    ASSERT_EQ(read, 36);

    /* Check txid */
    for (int i = 0; i < 32; i++) {
        ASSERT_EQ(op.txid.bytes[i], 0xCD);
    }

    /* Check vout */
    ASSERT_EQ(op.vout, 0x44332211);
}

static void test_outpoint_roundtrip(void) {
    outpoint_t original = make_outpoint(0x42, 0xDEADBEEF);
    uint8_t buf[36];
    outpoint_t decoded;

    outpoint_serialize(&original, buf);
    outpoint_deserialize(buf, &decoded);

    ASSERT_TRUE(outpoint_equal(&original, &decoded));
}

/* ========================================================================
 * UTXO Entry Tests
 * ======================================================================== */

static void test_utxo_entry_create(void) {
    utxo_entry_t *entry = make_utxo(0x01, 0, 5000000000, 100, false);
    ASSERT_NOT_NULL(entry);

    ASSERT_EQ(entry->value, 5000000000);
    ASSERT_EQ(entry->script_len, 25);
    ASSERT_EQ(entry->height, 100);
    ASSERT_FALSE(entry->is_coinbase);
    ASSERT_NOT_NULL(entry->script_pubkey);

    utxo_entry_destroy(entry);
}

static void test_utxo_entry_clone(void) {
    utxo_entry_t *original = make_utxo(0x02, 1, 1000000, 200, true);
    ASSERT_NOT_NULL(original);

    utxo_entry_t *clone = utxo_entry_clone(original);
    ASSERT_NOT_NULL(clone);

    /* Verify deep copy */
    ASSERT_TRUE(outpoint_equal(&original->outpoint, &clone->outpoint));
    ASSERT_EQ(original->value, clone->value);
    ASSERT_EQ(original->script_len, clone->script_len);
    ASSERT_EQ(original->height, clone->height);
    ASSERT_EQ(original->is_coinbase, clone->is_coinbase);

    /* Verify separate memory */
    ASSERT_NE(original->script_pubkey, clone->script_pubkey);
    ASSERT_EQ(memcmp(original->script_pubkey, clone->script_pubkey, 25), 0);

    utxo_entry_destroy(original);
    utxo_entry_destroy(clone);
}

static void test_utxo_entry_is_mature_coinbase(void) {
    /* Coinbase created at height 100 */
    utxo_entry_t *entry = make_utxo(0x03, 0, 5000000000, 100, true);
    ASSERT_NOT_NULL(entry);

    /* Not mature before 100 blocks */
    ASSERT_FALSE(utxo_entry_is_mature(entry, 100));
    ASSERT_FALSE(utxo_entry_is_mature(entry, 150));
    ASSERT_FALSE(utxo_entry_is_mature(entry, 199));

    /* Mature after 100 blocks */
    ASSERT_TRUE(utxo_entry_is_mature(entry, 200));
    ASSERT_TRUE(utxo_entry_is_mature(entry, 250));

    utxo_entry_destroy(entry);
}

static void test_utxo_entry_is_mature_noncoinbase(void) {
    /* Non-coinbase outputs are always mature */
    utxo_entry_t *entry = make_utxo(0x04, 0, 1000000, 100, false);
    ASSERT_NOT_NULL(entry);

    ASSERT_TRUE(utxo_entry_is_mature(entry, 100));
    ASSERT_TRUE(utxo_entry_is_mature(entry, 101));
    ASSERT_TRUE(utxo_entry_is_mature(entry, 150));

    utxo_entry_destroy(entry);
}

/* ========================================================================
 * UTXO Set Tests
 * ======================================================================== */

static void test_utxo_set_create(void) {
    utxo_set_t *set = utxo_set_create(0);
    ASSERT_NOT_NULL(set);
    ASSERT_EQ(utxo_set_size(set), 0);

    utxo_set_destroy(set);
}

static void test_utxo_set_insert_and_lookup(void) {
    utxo_set_t *set = utxo_set_create(0);
    ASSERT_NOT_NULL(set);

    utxo_entry_t *entry = make_utxo(0x05, 0, 1000000, 100, false);
    ASSERT_NOT_NULL(entry);

    /* Insert */
    echo_result_t result = utxo_set_insert(set, entry);
    ASSERT_EQ(result, ECHO_OK);
    ASSERT_EQ(utxo_set_size(set), 1);

    /* Lookup */
    const utxo_entry_t *found = utxo_set_lookup(set, &entry->outpoint);
    ASSERT_NOT_NULL(found);
    ASSERT_TRUE(outpoint_equal(&found->outpoint, &entry->outpoint));
    ASSERT_EQ(found->value, entry->value);

    utxo_entry_destroy(entry);
    utxo_set_destroy(set);
}

static void test_utxo_set_exists(void) {
    utxo_set_t *set = utxo_set_create(0);
    ASSERT_NOT_NULL(set);

    utxo_entry_t *entry = make_utxo(0x06, 0, 1000000, 100, false);
    ASSERT_NOT_NULL(entry);

    outpoint_t missing = make_outpoint(0x99, 0);

    ASSERT_FALSE(utxo_set_exists(set, &entry->outpoint));
    ASSERT_FALSE(utxo_set_exists(set, &missing));

    utxo_set_insert(set, entry);

    ASSERT_TRUE(utxo_set_exists(set, &entry->outpoint));
    ASSERT_FALSE(utxo_set_exists(set, &missing));

    utxo_entry_destroy(entry);
    utxo_set_destroy(set);
}

static void test_utxo_set_insert_duplicate(void) {
    utxo_set_t *set = utxo_set_create(0);
    ASSERT_NOT_NULL(set);

    utxo_entry_t *entry = make_utxo(0x07, 0, 1000000, 100, false);
    ASSERT_NOT_NULL(entry);

    /* First insert should succeed */
    ASSERT_EQ(utxo_set_insert(set, entry), ECHO_OK);

    /* Second insert should fail */
    ASSERT_EQ(utxo_set_insert(set, entry), ECHO_ERR_EXISTS);
    ASSERT_EQ(utxo_set_size(set), 1);

    utxo_entry_destroy(entry);
    utxo_set_destroy(set);
}

static void test_utxo_set_remove(void) {
    utxo_set_t *set = utxo_set_create(0);
    ASSERT_NOT_NULL(set);

    utxo_entry_t *entry = make_utxo(0x08, 0, 1000000, 100, false);
    ASSERT_NOT_NULL(entry);

    utxo_set_insert(set, entry);
    ASSERT_EQ(utxo_set_size(set), 1);

    /* Remove */
    echo_result_t result = utxo_set_remove(set, &entry->outpoint);
    ASSERT_EQ(result, ECHO_OK);
    ASSERT_EQ(utxo_set_size(set), 0);

    /* Verify it's gone */
    ASSERT_FALSE(utxo_set_exists(set, &entry->outpoint));

    /* Removing again should fail */
    result = utxo_set_remove(set, &entry->outpoint);
    ASSERT_EQ(result, ECHO_ERR_NOT_FOUND);

    utxo_entry_destroy(entry);
    utxo_set_destroy(set);
}

static void test_utxo_set_multiple_entries(void) {
    utxo_set_t *set = utxo_set_create(0);
    ASSERT_NOT_NULL(set);

    /* Insert 10 entries */
    utxo_entry_t *entries[10];
    for (int i = 0; i < 10; i++) {
        entries[i] = make_utxo(i + 1, i, 1000000 + i, 100 + i, i % 2 == 0);
        ASSERT_NOT_NULL(entries[i]);
        ASSERT_EQ(utxo_set_insert(set, entries[i]), ECHO_OK);
    }

    ASSERT_EQ(utxo_set_size(set), 10);

    /* Verify all can be found */
    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(utxo_set_exists(set, &entries[i]->outpoint));
    }

    /* Remove every other entry */
    for (int i = 0; i < 10; i += 2) {
        ASSERT_EQ(utxo_set_remove(set, &entries[i]->outpoint), ECHO_OK);
    }

    ASSERT_EQ(utxo_set_size(set), 5);

    /* Verify correct entries remain */
    for (int i = 0; i < 10; i++) {
        if (i % 2 == 0) {
            ASSERT_FALSE(utxo_set_exists(set, &entries[i]->outpoint));
        } else {
            ASSERT_TRUE(utxo_set_exists(set, &entries[i]->outpoint));
        }
    }

    /* Cleanup */
    for (int i = 0; i < 10; i++) {
        utxo_entry_destroy(entries[i]);
    }
    utxo_set_destroy(set);
}

static void test_utxo_set_clear(void) {
    utxo_set_t *set = utxo_set_create(0);
    ASSERT_NOT_NULL(set);

    /* Insert several entries */
    for (int i = 0; i < 5; i++) {
        utxo_entry_t *entry = make_utxo(i + 1, 0, 1000000, 100, false);
        utxo_set_insert(set, entry);
        utxo_entry_destroy(entry);
    }

    ASSERT_EQ(utxo_set_size(set), 5);

    /* Clear */
    utxo_set_clear(set);
    ASSERT_EQ(utxo_set_size(set), 0);

    utxo_set_destroy(set);
}

static void test_utxo_set_stress(void) {
    /* Test with many entries to verify hash table resize */
    utxo_set_t *set = utxo_set_create(16);
    ASSERT_NOT_NULL(set);

    /* Insert 1000 entries */
    for (int i = 0; i < 1000; i++) {
        utxo_entry_t *entry = make_utxo(i & 0xFF, i, 1000000 + i, 100, false);
        ASSERT_NOT_NULL(entry);
        ASSERT_EQ(utxo_set_insert(set, entry), ECHO_OK);
        utxo_entry_destroy(entry);
    }

    ASSERT_EQ(utxo_set_size(set), 1000);

    /* Verify random lookups */
    for (int i = 0; i < 1000; i += 100) {
        outpoint_t op = make_outpoint(i & 0xFF, i);
        ASSERT_TRUE(utxo_set_exists(set, &op));
    }

    utxo_set_destroy(set);
}

/* ========================================================================
 * Batch Operations Tests
 * ======================================================================== */

static void test_utxo_batch_create(void) {
    utxo_batch_t *batch = utxo_batch_create();
    ASSERT_NOT_NULL(batch);

    utxo_batch_destroy(batch);
}

static void test_utxo_batch_insert(void) {
    utxo_batch_t *batch = utxo_batch_create();
    ASSERT_NOT_NULL(batch);

    utxo_entry_t *entry = make_utxo(0x10, 0, 1000000, 100, false);
    ASSERT_NOT_NULL(entry);

    echo_result_t result = utxo_batch_insert(batch, entry);
    ASSERT_EQ(result, ECHO_OK);

    utxo_entry_destroy(entry);
    utxo_batch_destroy(batch);
}

static void test_utxo_batch_remove(void) {
    utxo_batch_t *batch = utxo_batch_create();
    ASSERT_NOT_NULL(batch);

    utxo_entry_t *entry = make_utxo(0x11, 0, 1000000, 100, false);
    ASSERT_NOT_NULL(entry);

    outpoint_t op = entry->outpoint;
    echo_result_t result = utxo_batch_remove(batch, &op, entry);
    ASSERT_EQ(result, ECHO_OK);

    utxo_entry_destroy(entry);
    utxo_batch_destroy(batch);
}

static void test_utxo_batch_multiple_changes(void) {
    utxo_batch_t *batch = utxo_batch_create();
    ASSERT_NOT_NULL(batch);

    /* Add multiple changes */
    for (int i = 0; i < 10; i++) {
        utxo_entry_t *entry = make_utxo(i + 1, i, 1000000 + i, 100, false);
        if (i % 2 == 0) {
            utxo_batch_insert(batch, entry);
        } else {
            utxo_batch_remove(batch, &entry->outpoint, entry);
        }
        utxo_entry_destroy(entry);
    }

    utxo_batch_destroy(batch);
}

/* ========================================================================
 * Iteration Tests
 * ======================================================================== */

static int iteration_count;
static int64_t total_value;

static bool count_callback(const utxo_entry_t *entry, void *user_data) {
    (void)user_data;
    iteration_count++;
    total_value += entry->value;
    return true;
}

static void test_utxo_set_foreach(void) {
    utxo_set_t *set = utxo_set_create(0);
    ASSERT_NOT_NULL(set);

    /* Insert 5 entries */
    for (int i = 0; i < 5; i++) {
        utxo_entry_t *entry = make_utxo(i + 1, 0, (i + 1) * 1000000, 100, false);
        utxo_set_insert(set, entry);
        utxo_entry_destroy(entry);
    }

    /* Iterate and count */
    iteration_count = 0;
    total_value = 0;
    utxo_set_foreach(set, count_callback, NULL);

    ASSERT_EQ(iteration_count, 5);
    ASSERT_EQ(total_value, 15000000);  /* 1+2+3+4+5 million */

    utxo_set_destroy(set);
}

static bool stop_early_callback(const utxo_entry_t *entry, void *user_data) {
    (void)entry;
    (void)user_data;
    iteration_count++;
    return iteration_count < 3;  /* Stop after 3 */
}

static void test_utxo_set_foreach_early_stop(void) {
    utxo_set_t *set = utxo_set_create(0);
    ASSERT_NOT_NULL(set);

    /* Insert 10 entries */
    for (int i = 0; i < 10; i++) {
        utxo_entry_t *entry = make_utxo(i + 1, 0, 1000000, 100, false);
        utxo_set_insert(set, entry);
        utxo_entry_destroy(entry);
    }

    /* Iterate but stop early */
    iteration_count = 0;
    utxo_set_foreach(set, stop_early_callback, NULL);

    ASSERT_EQ(iteration_count, 3);

    utxo_set_destroy(set);
}

/* ========================================================================
 * Main Test Runner
 * ======================================================================== */

int main(void) {
    test_suite_begin("Utxo Tests");

    test_case("Outpoint equal"); test_outpoint_equal(); test_pass();
    test_case("Outpoint serialize"); test_outpoint_serialize(); test_pass();
    test_case("Outpoint deserialize"); test_outpoint_deserialize(); test_pass();
    test_case("Outpoint roundtrip"); test_outpoint_roundtrip(); test_pass();
    test_case("Utxo entry create"); test_utxo_entry_create(); test_pass();
    test_case("Utxo entry clone"); test_utxo_entry_clone(); test_pass();
    test_case("Utxo entry is mature coinbase"); test_utxo_entry_is_mature_coinbase(); test_pass();
    test_case("Utxo entry is mature noncoinbase"); test_utxo_entry_is_mature_noncoinbase(); test_pass();
    test_case("Utxo set create"); test_utxo_set_create(); test_pass();
    test_case("Utxo set insert and lookup"); test_utxo_set_insert_and_lookup(); test_pass();
    test_case("Utxo set exists"); test_utxo_set_exists(); test_pass();
    test_case("Utxo set insert duplicate"); test_utxo_set_insert_duplicate(); test_pass();
    test_case("Utxo set remove"); test_utxo_set_remove(); test_pass();
    test_case("Utxo set multiple entries"); test_utxo_set_multiple_entries(); test_pass();
    test_case("Utxo set clear"); test_utxo_set_clear(); test_pass();
    test_case("Utxo set stress"); test_utxo_set_stress(); test_pass();
    test_case("Utxo batch create"); test_utxo_batch_create(); test_pass();
    test_case("Utxo batch insert"); test_utxo_batch_insert(); test_pass();
    test_case("Utxo batch remove"); test_utxo_batch_remove(); test_pass();
    test_case("Utxo batch multiple changes"); test_utxo_batch_multiple_changes(); test_pass();
    test_case("Utxo set foreach"); test_utxo_set_foreach(); test_pass();
    test_case("Utxo set foreach early stop"); test_utxo_set_foreach_early_stop(); test_pass();

    test_suite_end();
    return test_global_summary();
}
