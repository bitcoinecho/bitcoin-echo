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

/* Helper to remove test database */
static void cleanup_test_db(void) {
  remove(TEST_DB_PATH);
  remove(TEST_DB_PATH "-shm");
  remove(TEST_DB_PATH "-wal");
}

/* Helper to create a test block entry */
static void make_test_entry(block_index_entry_t *entry, uint32_t height,
                            uint8_t hash_seed, uint32_t status) {
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
  entry->status = status;
}

/* ========================================================================
 * Tests
 * ======================================================================== */

static void test_open_close(void) {
  block_index_db_t bdb;
  echo_result_t result;

  test_case("Open and close database");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_insert_and_lookup_by_hash(void) {
  block_index_db_t bdb;
  block_index_entry_t entry, retrieved;
  echo_result_t result;

  test_case("Insert and lookup by hash");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Create and insert entry */
  make_test_entry(&entry, 100, 42, BLOCK_STATUS_VALID_HEADER);
  result = block_index_db_insert(&bdb, &entry);
  if (result != ECHO_OK) {
    test_fail("failed to insert entry");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Lookup by hash */
  result = block_index_db_lookup_by_hash(&bdb, &entry.hash, &retrieved);
  if (result != ECHO_OK) {
    test_fail("failed to lookup by hash");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Verify fields */
  if (memcmp(retrieved.hash.bytes, entry.hash.bytes, 32) != 0) {
    test_fail("hash mismatch");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (retrieved.height != entry.height) {
    test_fail_uint("height mismatch", entry.height, retrieved.height);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (retrieved.status != entry.status) {
    test_fail_uint("status mismatch", entry.status, retrieved.status);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (memcmp(retrieved.chainwork.bytes, entry.chainwork.bytes, 32) != 0) {
    test_fail("chainwork mismatch");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_lookup_nonexistent(void) {
  block_index_db_t bdb;
  block_index_entry_t entry;
  hash256_t fake_hash;
  echo_result_t result;

  test_case("Lookup nonexistent entry");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Try to lookup non-existent hash */
  memset(fake_hash.bytes, 0xFF, 32);
  result = block_index_db_lookup_by_hash(&bdb, &fake_hash, &entry);
  if (result != ECHO_ERR_NOT_FOUND) {
    test_fail("should return NOT_FOUND");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_exists(void) {
  block_index_db_t bdb;
  block_index_entry_t entry;
  hash256_t fake_hash;
  bool exists;
  echo_result_t result;

  test_case("Check if entry exists");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Check non-existent */
  memset(fake_hash.bytes, 0xFF, 32);
  result = block_index_db_exists(&bdb, &fake_hash, &exists);
  if (result != ECHO_OK) {
    test_fail("exists check failed");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (exists) {
    test_fail("should not exist");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Insert entry */
  make_test_entry(&entry, 50, 77, BLOCK_STATUS_VALID_HEADER);
  result = block_index_db_insert(&bdb, &entry);
  if (result != ECHO_OK) {
    test_fail("failed to insert");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Check exists */
  result = block_index_db_exists(&bdb, &entry.hash, &exists);
  if (result != ECHO_OK) {
    test_fail("exists check failed");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (!exists) {
    test_fail("should exist");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_lookup_by_height(void) {
  block_index_db_t bdb;
  block_index_entry_t entry, retrieved;
  echo_result_t result;

  test_case("Lookup by height");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Insert entry at height 200 */
  make_test_entry(&entry, 200, 88, BLOCK_STATUS_VALID_HEADER);
  result = block_index_db_insert(&bdb, &entry);
  if (result != ECHO_OK) {
    test_fail("failed to insert");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Lookup by height */
  result = block_index_db_lookup_by_height(&bdb, 200, &retrieved);
  if (result != ECHO_OK) {
    test_fail("failed to lookup by height");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (retrieved.height != 200) {
    test_fail_uint("height mismatch", 200, retrieved.height);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (memcmp(retrieved.hash.bytes, entry.hash.bytes, 32) != 0) {
    test_fail("hash mismatch");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Lookup non-existent height */
  result = block_index_db_lookup_by_height(&bdb, 999, &retrieved);
  if (result != ECHO_ERR_NOT_FOUND) {
    test_fail("should return NOT_FOUND");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_update_status(void) {
  block_index_db_t bdb;
  block_index_entry_t entry, retrieved;
  hash256_t fake_hash;
  uint32_t new_status;
  echo_result_t result;

  test_case("Update block status");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Insert with initial status */
  make_test_entry(&entry, 10, 33, BLOCK_STATUS_VALID_HEADER);
  result = block_index_db_insert(&bdb, &entry);
  if (result != ECHO_OK) {
    test_fail("failed to insert");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Update status */
  new_status = BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_VALID_TREE;
  result = block_index_db_update_status(&bdb, &entry.hash, new_status);
  if (result != ECHO_OK) {
    test_fail("failed to update status");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Verify update */
  result = block_index_db_lookup_by_hash(&bdb, &entry.hash, &retrieved);
  if (result != ECHO_OK) {
    test_fail("failed to lookup");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (retrieved.status != new_status) {
    test_fail_uint("status not updated", new_status, retrieved.status);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Try to update non-existent block */
  memset(fake_hash.bytes, 0xAA, 32);
  result = block_index_db_update_status(&bdb, &fake_hash, 0);
  if (result != ECHO_ERR_NOT_FOUND) {
    test_fail("should return NOT_FOUND");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_get_best_chain(void) {
  block_index_db_t bdb;
  block_index_entry_t entry1, entry2, entry3, best;
  echo_result_t result;

  test_case("Get best chain");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Empty database should return NOT_FOUND */
  result = block_index_db_get_best_chain(&bdb, &best);
  if (result != ECHO_ERR_NOT_FOUND) {
    test_fail("empty db should return NOT_FOUND");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Insert blocks with VALID_CHAIN status and different chainwork */
  make_test_entry(&entry1, 100, 10, BLOCK_STATUS_VALID_CHAIN);
  make_test_entry(&entry2, 150, 20, BLOCK_STATUS_VALID_CHAIN);
  make_test_entry(&entry3, 200, 30, BLOCK_STATUS_VALID_CHAIN); /* highest */

  result = block_index_db_insert(&bdb, &entry1);
  if (result != ECHO_OK) {
    test_fail("failed to insert entry1");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  result = block_index_db_insert(&bdb, &entry2);
  if (result != ECHO_OK) {
    test_fail("failed to insert entry2");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  result = block_index_db_insert(&bdb, &entry3);
  if (result != ECHO_OK) {
    test_fail("failed to insert entry3");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Get best chain (should be entry3 with highest chainwork) */
  result = block_index_db_get_best_chain(&bdb, &best);
  if (result != ECHO_OK) {
    test_fail("failed to get best chain");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (best.height != 200) {
    test_fail_uint("best chain should be height 200", 200, best.height);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (memcmp(best.hash.bytes, entry3.hash.bytes, 32) != 0) {
    test_fail("wrong best block");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_get_prev(void) {
  block_index_db_t bdb;
  block_index_entry_t entry1, entry2, prev;
  echo_result_t result;

  test_case("Get previous block");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Create chain: entry1 -> entry2 */
  make_test_entry(&entry1, 100, 10, BLOCK_STATUS_VALID_HEADER);
  make_test_entry(&entry2, 101, 11, BLOCK_STATUS_VALID_HEADER);

  /* Link entry2 to entry1 */
  memcpy(entry2.header.prev_hash.bytes, entry1.hash.bytes, 32);

  result = block_index_db_insert(&bdb, &entry1);
  if (result != ECHO_OK) {
    test_fail("failed to insert entry1");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  result = block_index_db_insert(&bdb, &entry2);
  if (result != ECHO_OK) {
    test_fail("failed to insert entry2");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Get previous block of entry2 */
  result = block_index_db_get_prev(&bdb, &entry2.hash, &prev);
  if (result != ECHO_OK) {
    test_fail("failed to get prev");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (memcmp(prev.hash.bytes, entry1.hash.bytes, 32) != 0) {
    test_fail("prev hash mismatch");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (prev.height != 100) {
    test_fail_uint("prev height mismatch", 100, prev.height);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_find_common_ancestor(void) {
  block_index_db_t bdb;
  block_index_entry_t genesis, a1, a2, b1, ancestor;
  echo_result_t result;

  test_case("Find common ancestor");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /*
   * Create fork:
   *   genesis -> a1 -> a2
   *      |
   *      +----> b1
   *
   * Common ancestor of a2 and b1 should be genesis.
   */
  make_test_entry(&genesis, 0, 1, BLOCK_STATUS_VALID_HEADER);
  make_test_entry(&a1, 1, 2, BLOCK_STATUS_VALID_HEADER);
  make_test_entry(&a2, 2, 3, BLOCK_STATUS_VALID_HEADER);
  make_test_entry(&b1, 1, 4, BLOCK_STATUS_VALID_HEADER);

  /* Link chain A */
  memset(genesis.header.prev_hash.bytes, 0, 32);
  memcpy(a1.header.prev_hash.bytes, genesis.hash.bytes, 32);
  memcpy(a2.header.prev_hash.bytes, a1.hash.bytes, 32);

  /* Link chain B (forks from genesis) */
  memcpy(b1.header.prev_hash.bytes, genesis.hash.bytes, 32);

  result = block_index_db_insert(&bdb, &genesis);
  if (result != ECHO_OK) {
    test_fail("failed to insert genesis");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  result = block_index_db_insert(&bdb, &a1);
  if (result != ECHO_OK) {
    test_fail("failed to insert a1");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  result = block_index_db_insert(&bdb, &a2);
  if (result != ECHO_OK) {
    test_fail("failed to insert a2");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  result = block_index_db_insert(&bdb, &b1);
  if (result != ECHO_OK) {
    test_fail("failed to insert b1");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Find common ancestor */
  result =
      block_index_db_find_common_ancestor(&bdb, &a2.hash, &b1.hash, &ancestor);
  if (result != ECHO_OK) {
    test_fail("failed to find common ancestor");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (memcmp(ancestor.hash.bytes, genesis.hash.bytes, 32) != 0) {
    test_fail("ancestor should be genesis");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (ancestor.height != 0) {
    test_fail_uint("ancestor height should be 0", 0, ancestor.height);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_mark_best_chain(void) {
  block_index_db_t bdb;
  block_index_entry_t entry1, entry2, retrieved;
  hash256_t hashes[2];
  echo_result_t result;

  test_case("Mark blocks as best chain");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Insert blocks without best chain flag */
  make_test_entry(&entry1, 10, 50, BLOCK_STATUS_VALID_HEADER);
  make_test_entry(&entry2, 11, 51, BLOCK_STATUS_VALID_HEADER);

  result = block_index_db_insert(&bdb, &entry1);
  if (result != ECHO_OK) {
    test_fail("failed to insert entry1");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  result = block_index_db_insert(&bdb, &entry2);
  if (result != ECHO_OK) {
    test_fail("failed to insert entry2");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Mark as best chain */
  hashes[0] = entry1.hash;
  hashes[1] = entry2.hash;
  result = block_index_db_mark_best_chain(&bdb, hashes, 2);
  if (result != ECHO_OK) {
    test_fail("failed to mark best chain");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Verify flag is set */
  result = block_index_db_lookup_by_hash(&bdb, &entry1.hash, &retrieved);
  if (result != ECHO_OK) {
    test_fail("failed to lookup entry1");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if ((retrieved.status & BLOCK_STATUS_VALID_CHAIN) == 0) {
    test_fail("best chain flag not set on entry1");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  result = block_index_db_lookup_by_hash(&bdb, &entry2.hash, &retrieved);
  if (result != ECHO_OK) {
    test_fail("failed to lookup entry2");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if ((retrieved.status & BLOCK_STATUS_VALID_CHAIN) == 0) {
    test_fail("best chain flag not set on entry2");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_unmark_best_chain(void) {
  block_index_db_t bdb;
  block_index_entry_t entry, retrieved;
  hash256_t hash;
  echo_result_t result;

  test_case("Unmark best chain");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Insert block with best chain flag */
  make_test_entry(&entry, 20, 60,
                  BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_VALID_CHAIN);

  result = block_index_db_insert(&bdb, &entry);
  if (result != ECHO_OK) {
    test_fail("failed to insert");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Unmark best chain */
  hash = entry.hash;
  result = block_index_db_unmark_best_chain(&bdb, &hash, 1);
  if (result != ECHO_OK) {
    test_fail("failed to unmark");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Verify flag is cleared */
  result = block_index_db_lookup_by_hash(&bdb, &entry.hash, &retrieved);
  if (result != ECHO_OK) {
    test_fail("failed to lookup");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if ((retrieved.status & BLOCK_STATUS_VALID_CHAIN) != 0) {
    test_fail("best chain flag not cleared");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if ((retrieved.status & BLOCK_STATUS_VALID_HEADER) == 0) {
    test_fail("other flags should remain");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_count(void) {
  block_index_db_t bdb;
  block_index_entry_t entry;
  size_t count;
  echo_result_t result;
  int i;

  test_case("Count entries");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Empty database */
  result = block_index_db_count(&bdb, &count);
  if (result != ECHO_OK) {
    test_fail("failed to count");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (count != 0) {
    test_fail_uint("count should be 0", 0, (unsigned long)count);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Insert 10 blocks */
  for (i = 0; i < 10; i++) {
    make_test_entry(&entry, (uint32_t)i, (uint8_t)(i + 100),
                    BLOCK_STATUS_VALID_HEADER);
    result = block_index_db_insert(&bdb, &entry);
    if (result != ECHO_OK) {
      test_fail("failed to insert");
      block_index_db_close(&bdb);
      cleanup_test_db();
      return;
    }
  }

  /* Count should be 10 */
  result = block_index_db_count(&bdb, &count);
  if (result != ECHO_OK) {
    test_fail("failed to count");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (count != 10) {
    test_fail_uint("count should be 10", 10, (unsigned long)count);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_get_height(void) {
  block_index_db_t bdb;
  block_index_entry_t entry;
  uint32_t height;
  echo_result_t result;

  test_case("Get chain height");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Empty database */
  result = block_index_db_get_height(&bdb, &height);
  if (result != ECHO_ERR_NOT_FOUND) {
    test_fail("empty db should return NOT_FOUND");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Insert block at height 500 with VALID_CHAIN status */
  make_test_entry(&entry, 500, 99, BLOCK_STATUS_VALID_CHAIN);
  result = block_index_db_insert(&bdb, &entry);
  if (result != ECHO_OK) {
    test_fail("failed to insert");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Get height */
  result = block_index_db_get_height(&bdb, &height);
  if (result != ECHO_OK) {
    test_fail("failed to get height");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (height != 500) {
    test_fail_uint("height should be 500", 500, height);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_get_chainwork(void) {
  block_index_db_t bdb;
  block_index_entry_t entry;
  work256_t chainwork;
  echo_result_t result;

  test_case("Get chain work");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Insert block with VALID_CHAIN status */
  make_test_entry(&entry, 300, 77, BLOCK_STATUS_VALID_CHAIN);
  result = block_index_db_insert(&bdb, &entry);
  if (result != ECHO_OK) {
    test_fail("failed to insert");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Get chainwork */
  result = block_index_db_get_chainwork(&bdb, &chainwork);
  if (result != ECHO_OK) {
    test_fail("failed to get chainwork");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (memcmp(chainwork.bytes, entry.chainwork.bytes, 32) != 0) {
    test_fail("chainwork mismatch");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_get_chain_block(void) {
  block_index_db_t bdb;
  block_index_entry_t entry1, entry2, retrieved;
  echo_result_t result;

  test_case("Get block at height");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Insert blocks at same height with different status */
  make_test_entry(&entry1, 100, 40, BLOCK_STATUS_VALID_HEADER);
  make_test_entry(&entry2, 100, 41,
                  BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_VALID_CHAIN);

  result = block_index_db_insert(&bdb, &entry1);
  if (result != ECHO_OK) {
    test_fail("failed to insert entry1");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  result = block_index_db_insert(&bdb, &entry2);
  if (result != ECHO_OK) {
    test_fail("failed to insert entry2");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Get block on best chain at height 100 */
  result = block_index_db_get_chain_block(&bdb, 100, &retrieved);
  if (result != ECHO_OK) {
    test_fail("failed to get chain block");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (memcmp(retrieved.hash.bytes, entry2.hash.bytes, 32) != 0) {
    test_fail("should get entry2");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Try non-existent height */
  result = block_index_db_get_chain_block(&bdb, 999, &retrieved);
  if (result != ECHO_ERR_NOT_FOUND) {
    test_fail("should return NOT_FOUND");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_persistence(void) {
  block_index_db_t bdb;
  block_index_entry_t entry, retrieved;
  echo_result_t result;

  test_case("Data persistence across restarts");

  cleanup_test_db();

  /* Open, insert, close */
  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  make_test_entry(&entry, 42, 123, BLOCK_STATUS_VALID_HEADER);
  result = block_index_db_insert(&bdb, &entry);
  if (result != ECHO_OK) {
    test_fail("failed to insert");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);

  /* Reopen and verify data persisted */
  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to reopen database");
    cleanup_test_db();
    return;
  }

  result = block_index_db_lookup_by_hash(&bdb, &entry.hash, &retrieved);
  if (result != ECHO_OK) {
    test_fail("failed to lookup after reopen");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (retrieved.height != 42) {
    test_fail_uint("height not persisted", 42, retrieved.height);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (memcmp(retrieved.hash.bytes, entry.hash.bytes, 32) != 0) {
    test_fail("hash not persisted");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_mark_have_data(void) {
  block_index_db_t bdb;
  block_index_entry_t entry, retrieved;
  echo_result_t result;

  test_case("Mark block as having data (file-per-block mode)");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Insert block without HAVE_DATA flag */
  make_test_entry(&entry, 100, 42, BLOCK_STATUS_VALID_HEADER);
  result = block_index_db_insert(&bdb, &entry);
  if (result != ECHO_OK) {
    test_fail("failed to insert");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Mark as having data */
  result = block_index_db_mark_have_data(&bdb, &entry.hash);
  if (result != ECHO_OK) {
    test_fail_int("mark_have_data failed", ECHO_OK, result);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Verify HAVE_DATA flag is set */
  result = block_index_db_lookup_by_hash(&bdb, &entry.hash, &retrieved);
  if (result != ECHO_OK) {
    test_fail("failed to lookup");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (!(retrieved.status & BLOCK_STATUS_HAVE_DATA)) {
    test_fail("HAVE_DATA flag not set");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Try marking nonexistent block */
  hash256_t fake_hash = {0};
  fake_hash.bytes[0] = 0xFF;
  result = block_index_db_mark_have_data(&bdb, &fake_hash);
  if (result != ECHO_ERR_NOT_FOUND) {
    test_fail_int("should return NOT_FOUND for nonexistent block",
                  ECHO_ERR_NOT_FOUND, result);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

static void test_clear_have_data(void) {
  block_index_db_t bdb;
  block_index_entry_t entry, retrieved;
  echo_result_t result;

  test_case("Clear HAVE_DATA flag (pruning)");

  cleanup_test_db();

  result = block_index_db_open(&bdb, TEST_DB_PATH);
  if (result != ECHO_OK) {
    test_fail("failed to open database");
    cleanup_test_db();
    return;
  }

  /* Insert block with HAVE_DATA flag */
  make_test_entry(&entry, 100, 42, BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_HAVE_DATA);
  result = block_index_db_insert(&bdb, &entry);
  if (result != ECHO_OK) {
    test_fail("failed to insert");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Clear HAVE_DATA (simulates pruning) */
  result = block_index_db_clear_have_data(&bdb, &entry.hash);
  if (result != ECHO_OK) {
    test_fail_int("clear_have_data failed", ECHO_OK, result);
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  /* Verify HAVE_DATA cleared and PRUNED set */
  result = block_index_db_lookup_by_hash(&bdb, &entry.hash, &retrieved);
  if (result != ECHO_OK) {
    test_fail("failed to lookup");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (retrieved.status & BLOCK_STATUS_HAVE_DATA) {
    test_fail("HAVE_DATA flag still set after clear");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }
  if (!(retrieved.status & BLOCK_STATUS_PRUNED)) {
    test_fail("PRUNED flag not set after clear");
    block_index_db_close(&bdb);
    cleanup_test_db();
    return;
  }

  block_index_db_close(&bdb);
  cleanup_test_db();
  test_pass();
}

/* ========================================================================
 * Main
 * ======================================================================== */

int main(void) {
  test_suite_begin("Block Index Database Tests");

  test_open_close();
  test_insert_and_lookup_by_hash();
  test_lookup_nonexistent();
  test_exists();
  test_lookup_by_height();
  test_update_status();
  test_get_best_chain();
  test_get_prev();
  test_find_common_ancestor();
  test_mark_best_chain();
  test_unmark_best_chain();
  test_count();
  test_get_height();
  test_get_chainwork();
  test_get_chain_block();
  test_persistence();
  test_mark_have_data();
  test_clear_have_data();

  test_suite_end();
  return test_global_summary();
}
