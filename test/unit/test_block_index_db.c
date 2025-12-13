/*
 * Bitcoin Echo — Block Index Database Tests
 *
 * Comprehensive tests for the block index database implementation.
 *
 * Build once. Build right. Stop.
 */

#include "block.h"
#include "block_index_db.h"
#include "chainstate.h"
#include "echo_types.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "test_utils.h"

/* Test database path */
#define TEST_DB_PATH "test_block_index.db"

/* Test counter */
/* ANSI color codes */
#define COLOR_GREEN "\033[32m"
#define COLOR_RED "\033[31m"
#define COLOR_RESET "\033[0m"

/* Test macros */

#define PASS()                                                                 \
  do {                                                                         \
    tests_passed++;                                                            \
    printf(COLOR_GREEN "PASS" COLOR_RESET "\n");                               \
  } while (0)

#define FAIL(msg)                                                              \
  do {                                                                         \
    printf(COLOR_RED "FAIL" COLOR_RESET ": %s\n", msg);                        \
    return;                                                                    \
  } while (0)

#define ASSERT(cond, msg)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      FAIL(msg);                                                               \
    }                                                                          \
  } while (0)

/* Helper to remove test database */
static void cleanup_test_db(void) {
  remove(TEST_DB_PATH);
  remove(TEST_DB_PATH "-shm");
  remove(TEST_DB_PATH "-wal");
}

/* Helper to create a test block entry */
static void make_test_entry(block_index_entry_t *entry, uint32_t height,
                            uint8_t hash_seed) {
  size_t i;

  /* Generate deterministic hash */
  memset(entry->hash.bytes, 0, 32);
  entry->hash.bytes[0] = hash_seed;

  /* Set height */
  entry->height = height;

  /* Create header */
  memset(&entry->header, 0, sizeof(entry->header));
  entry->header.version = 1;
  entry->header.timestamp = 1231006505 + (height * 600);
  entry->header.bits = 0x1d00ffff;
  entry->header.nonce = height;

  /* Set prev_hash (link to previous block) */
  if (height > 0) {
    memset(entry->header.prev_hash.bytes, 0, 32);
    entry->header.prev_hash.bytes[0] = hash_seed - 1;
  } else {
    memset(entry->header.prev_hash.bytes, 0, 32);
  }

  /* Create deterministic chainwork (increases with height) */
  memset(entry->chainwork.bytes, 0, 32);
  for (i = 0; i < 4; i++) {
    entry->chainwork.bytes[31 - i] = (uint8_t)(height >> (i * 8));
  }

  /* Set status */
  entry->status = BLOCK_STATUS_VALID_HEADER;
}

/* ========================================================================
 * Tests
 * ======================================================================== */

static void test_open_close(void) {

  block_index_db_t bdb;
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  block_index_db_close(&bdb);

  cleanup_test_db();
}

static void test_insert_and_lookup_by_hash(void) {

  block_index_db_t bdb;
  block_index_entry_t entry, retrieved;
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /* Create and insert entry */
  make_test_entry(&entry, 100, 42);
  result = block_index_db_insert(&bdb, &entry);
  ASSERT(result == ECHO_OK, "failed to insert entry");

  /* Lookup by hash */
  result = block_index_db_lookup_by_hash(&bdb, &entry.hash, &retrieved);
  ASSERT(result == ECHO_OK, "failed to lookup by hash");

  /* Verify fields */
  ASSERT(memcmp(retrieved.hash.bytes, entry.hash.bytes, 32) == 0,
         "hash mismatch");
  ASSERT(retrieved.height == entry.height, "height mismatch");
  ASSERT(retrieved.status == entry.status, "status mismatch");
  ASSERT(memcmp(retrieved.chainwork.bytes, entry.chainwork.bytes, 32) == 0,
         "chainwork mismatch");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_lookup_nonexistent(void) {

  block_index_db_t bdb;
  block_index_entry_t entry;
  hash256_t fake_hash;
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /* Try to lookup non-existent hash */
  memset(fake_hash.bytes, 0xFF, 32);
  result = block_index_db_lookup_by_hash(&bdb, &fake_hash, &entry);
  ASSERT(result == ECHO_ERR_NOT_FOUND, "should return NOT_FOUND");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_exists(void) {

  block_index_db_t bdb;
  block_index_entry_t entry;
  hash256_t fake_hash;
  bool exists;
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /* Check non-existent */
  memset(fake_hash.bytes, 0xFF, 32);
  result = block_index_db_exists(&bdb, &fake_hash, &exists);
  ASSERT(result == ECHO_OK, "exists check failed");
  ASSERT(!exists, "should not exist");

  /* Insert entry */
  make_test_entry(&entry, 50, 77);
  result = block_index_db_insert(&bdb, &entry);
  ASSERT(result == ECHO_OK, "failed to insert");

  /* Check exists */
  result = block_index_db_exists(&bdb, &entry.hash, &exists);
  ASSERT(result == ECHO_OK, "exists check failed");
  ASSERT(exists, "should exist");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_lookup_by_height(void) {

  block_index_db_t bdb;
  block_index_entry_t entry, retrieved;
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /* Insert entry at height 200 */
  make_test_entry(&entry, 200, 88);
  result = block_index_db_insert(&bdb, &entry);
  ASSERT(result == ECHO_OK, "failed to insert");

  /* Lookup by height */
  result = block_index_db_lookup_by_height(&bdb, 200, &retrieved);
  ASSERT(result == ECHO_OK, "failed to lookup by height");
  ASSERT(retrieved.height == 200, "height mismatch");
  ASSERT(memcmp(retrieved.hash.bytes, entry.hash.bytes, 32) == 0,
         "hash mismatch");

  /* Lookup non-existent height */
  result = block_index_db_lookup_by_height(&bdb, 999, &retrieved);
  ASSERT(result == ECHO_ERR_NOT_FOUND, "should return NOT_FOUND");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_update_status(void) {

  block_index_db_t bdb;
  block_index_entry_t entry, retrieved;
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /* Insert with initial status */
  make_test_entry(&entry, 10, 33);
  entry.status = BLOCK_STATUS_VALID_HEADER;
  result = block_index_db_insert(&bdb, &entry);
  ASSERT(result == ECHO_OK, "failed to insert");

  /* Update status */
  uint32_t new_status = BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_VALID_TREE;
  result = block_index_db_update_status(&bdb, &entry.hash, new_status);
  ASSERT(result == ECHO_OK, "failed to update status");

  /* Verify update */
  result = block_index_db_lookup_by_hash(&bdb, &entry.hash, &retrieved);
  ASSERT(result == ECHO_OK, "failed to lookup");
  ASSERT(retrieved.status == new_status, "status not updated");

  /* Try to update non-existent block */
  hash256_t fake_hash;
  memset(fake_hash.bytes, 0xAA, 32);
  result = block_index_db_update_status(&bdb, &fake_hash, 0);
  ASSERT(result == ECHO_ERR_NOT_FOUND, "should return NOT_FOUND");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_get_best_chain(void) {

  block_index_db_t bdb;
  block_index_entry_t entry1, entry2, entry3, best;
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /* Empty database should return NOT_FOUND */
  result = block_index_db_get_best_chain(&bdb, &best);
  ASSERT(result == ECHO_ERR_NOT_FOUND, "empty db should return NOT_FOUND");

  /* Insert blocks with different chainwork */
  make_test_entry(&entry1, 100, 10); /* chainwork = 100 */
  make_test_entry(&entry2, 150, 20); /* chainwork = 150 */
  make_test_entry(&entry3, 200, 30); /* chainwork = 200 (highest) */

  result = block_index_db_insert(&bdb, &entry1);
  ASSERT(result == ECHO_OK, "failed to insert entry1");

  result = block_index_db_insert(&bdb, &entry2);
  ASSERT(result == ECHO_OK, "failed to insert entry2");

  result = block_index_db_insert(&bdb, &entry3);
  ASSERT(result == ECHO_OK, "failed to insert entry3");

  /* Get best chain (should be entry3 with highest chainwork) */
  result = block_index_db_get_best_chain(&bdb, &best);
  ASSERT(result == ECHO_OK, "failed to get best chain");
  ASSERT(best.height == 200, "best chain should be height 200");
  ASSERT(memcmp(best.hash.bytes, entry3.hash.bytes, 32) == 0,
         "wrong best block");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_get_prev(void) {

  block_index_db_t bdb;
  block_index_entry_t entry1, entry2, prev;
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /* Create chain: entry1 -> entry2 */
  make_test_entry(&entry1, 100, 10);
  make_test_entry(&entry2, 101, 11);

  /* Link entry2 to entry1 */
  memcpy(entry2.header.prev_hash.bytes, entry1.hash.bytes, 32);

  result = block_index_db_insert(&bdb, &entry1);
  ASSERT(result == ECHO_OK, "failed to insert entry1");

  result = block_index_db_insert(&bdb, &entry2);
  ASSERT(result == ECHO_OK, "failed to insert entry2");

  /* Get previous block of entry2 */
  result = block_index_db_get_prev(&bdb, &entry2.hash, &prev);
  ASSERT(result == ECHO_OK, "failed to get prev");
  ASSERT(memcmp(prev.hash.bytes, entry1.hash.bytes, 32) == 0,
         "prev hash mismatch");
  ASSERT(prev.height == 100, "prev height mismatch");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_find_common_ancestor(void) {

  block_index_db_t bdb;
  block_index_entry_t genesis, a1, a2, b1, ancestor;
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /*
   * Create fork:
   *   genesis -> a1 -> a2
   *      |
   *      +----> b1
   *
   * Common ancestor of a2 and b1 should be genesis.
   */
  make_test_entry(&genesis, 0, 1);
  make_test_entry(&a1, 1, 2);
  make_test_entry(&a2, 2, 3);
  make_test_entry(&b1, 1, 4);

  /* Link chain A */
  memset(genesis.header.prev_hash.bytes, 0, 32);
  memcpy(a1.header.prev_hash.bytes, genesis.hash.bytes, 32);
  memcpy(a2.header.prev_hash.bytes, a1.hash.bytes, 32);

  /* Link chain B (forks from genesis) */
  memcpy(b1.header.prev_hash.bytes, genesis.hash.bytes, 32);

  result = block_index_db_insert(&bdb, &genesis);
  ASSERT(result == ECHO_OK, "failed to insert genesis");

  result = block_index_db_insert(&bdb, &a1);
  ASSERT(result == ECHO_OK, "failed to insert a1");

  result = block_index_db_insert(&bdb, &a2);
  ASSERT(result == ECHO_OK, "failed to insert a2");

  result = block_index_db_insert(&bdb, &b1);
  ASSERT(result == ECHO_OK, "failed to insert b1");

  /* Find common ancestor */
  result =
      block_index_db_find_common_ancestor(&bdb, &a2.hash, &b1.hash, &ancestor);
  ASSERT(result == ECHO_OK, "failed to find common ancestor");
  ASSERT(memcmp(ancestor.hash.bytes, genesis.hash.bytes, 32) == 0,
         "ancestor should be genesis");
  ASSERT(ancestor.height == 0, "ancestor height should be 0");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_mark_best_chain(void) {

  block_index_db_t bdb;
  block_index_entry_t entry1, entry2, retrieved;
  hash256_t hashes[2];
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /* Insert blocks without best chain flag */
  make_test_entry(&entry1, 10, 50);
  make_test_entry(&entry2, 11, 51);
  entry1.status = BLOCK_STATUS_VALID_HEADER;
  entry2.status = BLOCK_STATUS_VALID_HEADER;

  result = block_index_db_insert(&bdb, &entry1);
  ASSERT(result == ECHO_OK, "failed to insert entry1");

  result = block_index_db_insert(&bdb, &entry2);
  ASSERT(result == ECHO_OK, "failed to insert entry2");

  /* Mark as best chain */
  hashes[0] = entry1.hash;
  hashes[1] = entry2.hash;
  result = block_index_db_mark_best_chain(&bdb, hashes, 2);
  ASSERT(result == ECHO_OK, "failed to mark best chain");

  /* Verify flag is set */
  result = block_index_db_lookup_by_hash(&bdb, &entry1.hash, &retrieved);
  ASSERT(result == ECHO_OK, "failed to lookup entry1");
  ASSERT((retrieved.status & BLOCK_STATUS_VALID_CHAIN) != 0,
         "best chain flag not set");

  result = block_index_db_lookup_by_hash(&bdb, &entry2.hash, &retrieved);
  ASSERT(result == ECHO_OK, "failed to lookup entry2");
  ASSERT((retrieved.status & BLOCK_STATUS_VALID_CHAIN) != 0,
         "best chain flag not set");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_unmark_best_chain(void) {

  block_index_db_t bdb;
  block_index_entry_t entry, retrieved;
  hash256_t hash;
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /* Insert block with best chain flag */
  make_test_entry(&entry, 20, 60);
  entry.status = BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_VALID_CHAIN;

  result = block_index_db_insert(&bdb, &entry);
  ASSERT(result == ECHO_OK, "failed to insert");

  /* Unmark best chain */
  hash = entry.hash;
  result = block_index_db_unmark_best_chain(&bdb, &hash, 1);
  ASSERT(result == ECHO_OK, "failed to unmark");

  /* Verify flag is cleared */
  result = block_index_db_lookup_by_hash(&bdb, &entry.hash, &retrieved);
  ASSERT(result == ECHO_OK, "failed to lookup");
  ASSERT((retrieved.status & BLOCK_STATUS_VALID_CHAIN) == 0,
         "best chain flag not cleared");
  ASSERT((retrieved.status & BLOCK_STATUS_VALID_HEADER) != 0,
         "other flags should remain");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_count(void) {

  block_index_db_t bdb;
  block_index_entry_t entry;
  size_t count;
  echo_result_t result;
  int i;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /* Empty database */
  result = block_index_db_count(&bdb, &count);
  ASSERT(result == ECHO_OK, "failed to count");
  ASSERT(count == 0, "count should be 0");

  /* Insert 10 blocks */
  for (i = 0; i < 10; i++) {
    make_test_entry(&entry, (uint32_t)i, (uint8_t)(i + 100));
    result = block_index_db_insert(&bdb, &entry);
    ASSERT(result == ECHO_OK, "failed to insert");
  }

  /* Count should be 10 */
  result = block_index_db_count(&bdb, &count);
  ASSERT(result == ECHO_OK, "failed to count");
  ASSERT(count == 10, "count should be 10");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_get_height(void) {

  block_index_db_t bdb;
  block_index_entry_t entry;
  uint32_t height;
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /* Empty database */
  result = block_index_db_get_height(&bdb, &height);
  ASSERT(result == ECHO_ERR_NOT_FOUND, "empty db should return NOT_FOUND");

  /* Insert block at height 500 with highest chainwork */
  make_test_entry(&entry, 500, 99);
  result = block_index_db_insert(&bdb, &entry);
  ASSERT(result == ECHO_OK, "failed to insert");

  /* Get height */
  result = block_index_db_get_height(&bdb, &height);
  ASSERT(result == ECHO_OK, "failed to get height");
  ASSERT(height == 500, "height should be 500");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_get_chainwork(void) {

  block_index_db_t bdb;
  block_index_entry_t entry;
  work256_t chainwork;
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /* Insert block */
  make_test_entry(&entry, 300, 77);
  result = block_index_db_insert(&bdb, &entry);
  ASSERT(result == ECHO_OK, "failed to insert");

  /* Get chainwork */
  result = block_index_db_get_chainwork(&bdb, &chainwork);
  ASSERT(result == ECHO_OK, "failed to get chainwork");
  ASSERT(memcmp(chainwork.bytes, entry.chainwork.bytes, 32) == 0,
         "chainwork mismatch");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_get_chain_block(void) {

  block_index_db_t bdb;
  block_index_entry_t entry1, entry2, retrieved;
  echo_result_t result;

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  /* Insert blocks at same height with different status */
  make_test_entry(&entry1, 100, 40);
  make_test_entry(&entry2, 100, 41);

  entry1.status = BLOCK_STATUS_VALID_HEADER; /* Not on best chain */
  entry2.status =
      BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_VALID_CHAIN; /* On best chain */

  result = block_index_db_insert(&bdb, &entry1);
  ASSERT(result == ECHO_OK, "failed to insert entry1");

  result = block_index_db_insert(&bdb, &entry2);
  ASSERT(result == ECHO_OK, "failed to insert entry2");

  /* Get block on best chain at height 100 */
  result = block_index_db_get_chain_block(&bdb, 100, &retrieved);
  ASSERT(result == ECHO_OK, "failed to get chain block");
  ASSERT(memcmp(retrieved.hash.bytes, entry2.hash.bytes, 32) == 0,
         "should get entry2");

  /* Try non-existent height */
  result = block_index_db_get_chain_block(&bdb, 999, &retrieved);
  ASSERT(result == ECHO_ERR_NOT_FOUND, "should return NOT_FOUND");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

static void test_persistence(void) {

  block_index_db_t bdb;
  block_index_entry_t entry, retrieved;
  echo_result_t result;

  cleanup_test_db();

  /* Open, insert, close */
  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to open database");

  make_test_entry(&entry, 42, 123);
  result = block_index_db_insert(&bdb, &entry);
  ASSERT(result == ECHO_OK, "failed to insert");

  block_index_db_close(&bdb);

  /* Reopen and verify data persisted */
  result = block_index_db_open(&bdb, TEST_DB_PATH);
  ASSERT(result == ECHO_OK, "failed to reopen database");

  result = block_index_db_lookup_by_hash(&bdb, &entry.hash, &retrieved);
  ASSERT(result == ECHO_OK, "failed to lookup after reopen");
  ASSERT(retrieved.height == 42, "height not persisted");
  ASSERT(memcmp(retrieved.hash.bytes, entry.hash.bytes, 32) == 0,
         "hash not persisted");

  block_index_db_close(&bdb);
  cleanup_test_db();
}

/* ========================================================================
 * Main
 * ======================================================================== */

int main(void) {
    test_suite_begin("Block Index Database Tests");

    test_case("Open and close database"); test_open_close(); test_pass();
    test_case("Insert and lookup by hash"); test_insert_and_lookup_by_hash(); test_pass();
    test_case("Lookup nonexistent entry"); test_lookup_nonexistent(); test_pass();
    test_case("Check if entry exists"); test_exists(); test_pass();
    test_case("Lookup by height"); test_lookup_by_height(); test_pass();
    test_case("Update block status"); test_update_status(); test_pass();
    test_case("Get best chain"); test_get_best_chain(); test_pass();
    test_case("Get previous block"); test_get_prev(); test_pass();
    test_case("Find common ancestor"); test_find_common_ancestor(); test_pass();
    test_case("Mark blocks as best chain"); test_mark_best_chain(); test_pass();
    test_case("Unmark best chain"); test_unmark_best_chain(); test_pass();
    test_case("Count entries"); test_count(); test_pass();
    test_case("Get chain height"); test_get_height(); test_pass();
    test_case("Get chain work"); test_get_chainwork(); test_pass();
    test_case("Get block at height"); test_get_chain_block(); test_pass();
    test_case("Data persistence across restarts"); test_persistence(); test_pass();

    test_suite_end();
    return test_global_summary();
}
