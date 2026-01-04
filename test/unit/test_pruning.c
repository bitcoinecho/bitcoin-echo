/*
 * Bitcoin Echo — Pruning Tests
 *
 * Tests for block pruning functionality including:
 * - Block status flags for pruned blocks
 * - Block storage pruning operations (height-based)
 * - Block index database pruning operations
 * - Pruning configuration
 */

#include "block_index_db.h"
#include "blocks_storage.h"
#include "echo_types.h"
#include "node.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_utils.h"

/*
 * Test data directory.
 */
#define TEST_DATA_DIR "/tmp/echo_pruning_test"

/*
 * Recursively remove a directory and all its contents.
 */
// NOLINTNEXTLINE(misc-no-recursion)
static void remove_dir_recursive(const char *path) {
  DIR *dir = opendir(path);
  if (dir == NULL) {
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

    struct stat st;
    if (stat(full_path, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        remove_dir_recursive(full_path);
      } else {
        unlink(full_path);
      }
    }
  }
  closedir(dir);
  rmdir(path);
}

static void cleanup_test_dir(void) { remove_dir_recursive(TEST_DATA_DIR); }

/*
 * Test: BLOCK_STATUS_PRUNED flag value.
 */
static void test_pruned_flag_value(void) {
  bool passed = true;

  /* Verify flag is defined and has expected value */
  if (BLOCK_STATUS_PRUNED != 0x40) {
    passed = false;
    goto done;
  }

  /* Verify flag doesn't overlap with other status flags */
  if ((BLOCK_STATUS_PRUNED & BLOCK_STATUS_VALID_HEADER) != 0) {
    passed = false;
    goto done;
  }
  if ((BLOCK_STATUS_PRUNED & BLOCK_STATUS_VALID_TREE) != 0) {
    passed = false;
    goto done;
  }
  if ((BLOCK_STATUS_PRUNED & BLOCK_STATUS_VALID_SCRIPTS) != 0) {
    passed = false;
    goto done;
  }
  if ((BLOCK_STATUS_PRUNED & BLOCK_STATUS_VALID_CHAIN) != 0) {
    passed = false;
    goto done;
  }
  if ((BLOCK_STATUS_PRUNED & BLOCK_STATUS_HAVE_DATA) != 0) {
    passed = false;
    goto done;
  }
  if ((BLOCK_STATUS_PRUNED & BLOCK_STATUS_FAILED) != 0) {
    passed = false;
    goto done;
  }

done:
  test_case("BLOCK_STATUS_PRUNED flag value");
  if (passed) {
    test_pass();
  } else {
    test_fail("flag value or overlap check failed");
  }
}

/*
 * Test: PRUNE_TARGET_MIN_MB constant.
 */
static void test_prune_target_min(void) {
  bool passed = true;

  /* Verify minimum is 128 MB (allows faster testing feedback) */
  if (PRUNE_TARGET_MIN_MB != 128) {
    passed = false;
  }

  test_case("PRUNE_TARGET_MIN_MB constant");
  if (passed) {
    test_pass();
  } else {
    test_fail("expected 128 MB minimum");
  }
}

/*
 * Test: Block storage exists check (height-based).
 */
static void test_block_storage_exists_height(void) {
  cleanup_test_dir();
  bool passed = true;

  block_storage_t storage;
  if (block_storage_create(&storage, TEST_DATA_DIR) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Block at height 0 should not exist yet */
  if (block_storage_exists_height(&storage, 0) != false) {
    passed = false;
    goto done;
  }

  /* Write a block at height 0 */
  uint8_t block_data[100] = {0};
  if (block_storage_write_height(&storage, 0, block_data, 100) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Now block at height 0 should exist */
  if (block_storage_exists_height(&storage, 0) != true) {
    passed = false;
    goto done;
  }

  /* Block at height 1 should not exist */
  if (block_storage_exists_height(&storage, 1) != false) {
    passed = false;
    goto done;
  }

  block_storage_destroy(&storage);

done:
  cleanup_test_dir();
  test_case("Block storage exists height check");
  if (passed) {
    test_pass();
  } else {
    test_fail("exists height check failed");
  }
}

/*
 * Test: Block storage total size query.
 */
static void test_block_storage_get_total_size(void) {
  cleanup_test_dir();
  bool passed = true;

  block_storage_t storage;
  if (block_storage_create(&storage, TEST_DATA_DIR) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Empty storage should have size 0 */
  uint64_t total = 999;
  if (block_storage_get_total_size(&storage, &total) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (total != 0) {
    passed = false;
    goto done;
  }

  /* Write some blocks at different heights */
  uint8_t block_data[100] = {0};
  if (block_storage_write_height(&storage, 0, block_data, 100) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (block_storage_write_height(&storage, 1, block_data, 100) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (block_storage_write_height(&storage, 2, block_data, 100) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Total should be 300 bytes (100 bytes each, no header overhead) */
  if (block_storage_get_total_size(&storage, &total) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (total != 300) {
    passed = false;
    goto done;
  }

  block_storage_destroy(&storage);

done:
  cleanup_test_dir();
  test_case("Block storage get total size");
  if (passed) {
    test_pass();
  } else {
    test_fail("total size query failed");
  }
}

/*
 * Test: Block storage prune by height.
 */
static void test_block_storage_prune_height(void) {
  cleanup_test_dir();
  bool passed = true;

  block_storage_t storage;
  if (block_storage_create(&storage, TEST_DATA_DIR) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Write blocks at heights 0-4 */
  uint8_t block_data[100] = {0};
  for (uint32_t i = 0; i < 5; i++) {
    if (block_storage_write_height(&storage, i, block_data, 100) != ECHO_OK) {
      passed = false;
      goto done;
    }
  }

  /* Verify all 5 blocks exist */
  for (uint32_t i = 0; i < 5; i++) {
    if (!block_storage_exists_height(&storage, i)) {
      passed = false;
      goto done;
    }
  }

  /* Prune blocks 0-2 */
  for (uint32_t i = 0; i < 3; i++) {
    if (block_storage_prune_height(&storage, i) != ECHO_OK) {
      passed = false;
      goto done;
    }
  }

  /* Verify blocks 0-2 no longer exist */
  for (uint32_t i = 0; i < 3; i++) {
    if (block_storage_exists_height(&storage, i)) {
      passed = false;
      goto done;
    }
  }

  /* Verify blocks 3-4 still exist */
  for (uint32_t i = 3; i < 5; i++) {
    if (!block_storage_exists_height(&storage, i)) {
      passed = false;
      goto done;
    }
  }

  /* Prune non-existent block should succeed (idempotent) */
  if (block_storage_prune_height(&storage, 100) != ECHO_OK) {
    passed = false;
    goto done;
  }

  block_storage_destroy(&storage);

done:
  cleanup_test_dir();
  test_case("Block storage prune by height");
  if (passed) {
    test_pass();
  } else {
    test_fail("prune by height failed");
  }
}

/*
 * Test: Block index database mark pruned.
 */
static void test_block_index_db_mark_pruned(void) {
  cleanup_test_dir();
  mkdir(TEST_DATA_DIR, 0755);
  bool passed = true;

  char db_path[512];
  snprintf(db_path, sizeof(db_path), "%s/blocks.db", TEST_DATA_DIR);

  block_index_db_t db;
  if (block_index_db_open(&db, db_path) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Create some test block entries */
  for (uint32_t i = 0; i < 10; i++) {
    block_index_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.height = i;
    entry.hash.bytes[0] = (uint8_t)i; /* Unique hash */
    entry.status = BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_HAVE_DATA;

    if (block_index_db_insert(&db, &entry) != ECHO_OK) {
      passed = false;
      block_index_db_close(&db);
      goto done;
    }
  }

  /* Mark blocks 0-4 as pruned */
  if (block_index_db_mark_pruned(&db, 0, 5) != ECHO_OK) {
    passed = false;
    block_index_db_close(&db);
    goto done;
  }

  /* Verify blocks 0-4 are marked as pruned */
  for (uint32_t i = 0; i < 5; i++) {
    block_index_entry_t entry;
    if (block_index_db_lookup_by_height(&db, i, &entry) != ECHO_OK) {
      passed = false;
      block_index_db_close(&db);
      goto done;
    }
    if ((entry.status & BLOCK_STATUS_PRUNED) == 0) {
      passed = false;
      block_index_db_close(&db);
      goto done;
    }
    if ((entry.status & BLOCK_STATUS_HAVE_DATA) != 0) {
      passed = false;
      block_index_db_close(&db);
      goto done;
    }
  }

  /* Verify blocks 5-9 are NOT pruned */
  for (uint32_t i = 5; i < 10; i++) {
    block_index_entry_t entry;
    if (block_index_db_lookup_by_height(&db, i, &entry) != ECHO_OK) {
      passed = false;
      block_index_db_close(&db);
      goto done;
    }
    if ((entry.status & BLOCK_STATUS_PRUNED) != 0) {
      passed = false;
      block_index_db_close(&db);
      goto done;
    }
    if ((entry.status & BLOCK_STATUS_HAVE_DATA) == 0) {
      passed = false;
      block_index_db_close(&db);
      goto done;
    }
  }

  block_index_db_close(&db);

done:
  cleanup_test_dir();
  test_case("Block index DB mark pruned");
  if (passed) {
    test_pass();
  } else {
    test_fail("mark pruned operation failed");
  }
}

/*
 * Test: Block index database get pruned height.
 */
static void test_block_index_db_get_pruned_height(void) {
  cleanup_test_dir();
  mkdir(TEST_DATA_DIR, 0755);
  bool passed = true;

  char db_path[512];
  snprintf(db_path, sizeof(db_path), "%s/blocks.db", TEST_DATA_DIR);

  block_index_db_t db;
  if (block_index_db_open(&db, db_path) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Create test entries */
  for (uint32_t i = 0; i < 10; i++) {
    block_index_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.height = i;
    entry.hash.bytes[0] = (uint8_t)i;
    entry.status = BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_HAVE_DATA;

    if (block_index_db_insert(&db, &entry) != ECHO_OK) {
      passed = false;
      block_index_db_close(&db);
      goto done;
    }
  }

  /* Initially, pruned height should be 0 (genesis has data) */
  uint32_t pruned_height = 999;
  if (block_index_db_get_pruned_height(&db, &pruned_height) != ECHO_OK) {
    passed = false;
    block_index_db_close(&db);
    goto done;
  }
  if (pruned_height != 0) {
    passed = false;
    block_index_db_close(&db);
    goto done;
  }

  /* Prune blocks 0-4 */
  if (block_index_db_mark_pruned(&db, 0, 5) != ECHO_OK) {
    passed = false;
    block_index_db_close(&db);
    goto done;
  }

  /* Now pruned height should be 5 (first block with data) */
  if (block_index_db_get_pruned_height(&db, &pruned_height) != ECHO_OK) {
    passed = false;
    block_index_db_close(&db);
    goto done;
  }
  if (pruned_height != 5) {
    passed = false;
    block_index_db_close(&db);
    goto done;
  }

  block_index_db_close(&db);

done:
  cleanup_test_dir();
  test_case("Block index DB get pruned height");
  if (passed) {
    test_pass();
  } else {
    test_fail("get pruned height failed");
  }
}

/*
 * Test: Block index database is_pruned check.
 */
static void test_block_index_db_is_pruned(void) {
  cleanup_test_dir();
  mkdir(TEST_DATA_DIR, 0755);
  bool passed = true;

  char db_path[512];
  snprintf(db_path, sizeof(db_path), "%s/blocks.db", TEST_DATA_DIR);

  block_index_db_t db;
  if (block_index_db_open(&db, db_path) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Create a test entry */
  block_index_entry_t entry;
  memset(&entry, 0, sizeof(entry));
  entry.height = 0;
  entry.hash.bytes[0] = 0x42;
  entry.status = BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_HAVE_DATA;

  if (block_index_db_insert(&db, &entry) != ECHO_OK) {
    passed = false;
    block_index_db_close(&db);
    goto done;
  }

  /* Check not pruned initially */
  bool is_pruned = true;
  if (block_index_db_is_pruned(&db, &entry.hash, &is_pruned) != ECHO_OK) {
    passed = false;
    block_index_db_close(&db);
    goto done;
  }
  if (is_pruned != false) {
    passed = false;
    block_index_db_close(&db);
    goto done;
  }

  /* Mark as pruned */
  if (block_index_db_mark_pruned(&db, 0, 1) != ECHO_OK) {
    passed = false;
    block_index_db_close(&db);
    goto done;
  }

  /* Now should be pruned */
  if (block_index_db_is_pruned(&db, &entry.hash, &is_pruned) != ECHO_OK) {
    passed = false;
    block_index_db_close(&db);
    goto done;
  }
  if (is_pruned != true) {
    passed = false;
    block_index_db_close(&db);
    goto done;
  }

  block_index_db_close(&db);

done:
  cleanup_test_dir();
  test_case("Block index DB is_pruned check");
  if (passed) {
    test_pass();
  } else {
    test_fail("is_pruned check failed");
  }
}

/*
 * Test: Node config prune target initialization.
 */
static void test_node_config_prune_target(void) {
  bool passed = true;

  node_config_t config;
  node_config_init(&config, TEST_DATA_DIR);

  /* Default should be 0 (no pruning) */
  if (config.prune_target_mb != 0) {
    passed = false;
  }

  test_case("Node config prune target initialization");
  if (passed) {
    test_pass();
  } else {
    test_fail("default prune target not 0");
  }
}

/*
 * Test: NULL parameter handling for pruning functions.
 */
static void test_pruning_null_params(void) {
  bool passed = true;
  uint64_t size;

  /* block_storage_get_total_size null checks */
  if (block_storage_get_total_size(NULL, &size) != ECHO_ERR_NULL_PARAM) {
    passed = false;
    goto done;
  }

  block_storage_t storage;
  memset(&storage, 0, sizeof(storage));
  if (block_storage_get_total_size(&storage, NULL) != ECHO_ERR_NULL_PARAM) {
    passed = false;
    goto done;
  }

  /* block_storage_prune_height null check */
  if (block_storage_prune_height(NULL, 0) != ECHO_ERR_NULL_PARAM) {
    passed = false;
    goto done;
  }

done:
  test_case("Pruning NULL parameter handling");
  if (passed) {
    test_pass();
  } else {
    test_fail("NULL param check failed");
  }
}

/*
 * Test: Block storage scan heights for pruning.
 */
static void test_block_storage_scan_heights(void) {
  cleanup_test_dir();
  bool passed = true;

  block_storage_t storage;
  if (block_storage_create(&storage, TEST_DATA_DIR) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Write blocks at heights 0, 5, 10 (sparse) */
  uint8_t block_data[100] = {0};
  if (block_storage_write_height(&storage, 0, block_data, 100) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (block_storage_write_height(&storage, 5, block_data, 100) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (block_storage_write_height(&storage, 10, block_data, 100) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Scan heights */
  uint32_t *heights = NULL;
  size_t count = 0;
  if (block_storage_scan_heights(&storage, &heights, &count) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Should have 3 heights in ascending order */
  if (count != 3) {
    passed = false;
    free(heights);
    goto done;
  }
  if (heights[0] != 0 || heights[1] != 5 || heights[2] != 10) {
    passed = false;
    free(heights);
    goto done;
  }

  free(heights);
  block_storage_destroy(&storage);

done:
  cleanup_test_dir();
  test_case("Block storage scan heights");
  if (passed) {
    test_pass();
  } else {
    test_fail("scan heights failed");
  }
}

/*
 * Main test runner.
 */
int main(void) {
  test_suite_begin("Pruning Tests");

  test_pruned_flag_value();
  test_prune_target_min();
  test_block_storage_exists_height();
  test_block_storage_get_total_size();
  test_block_storage_prune_height();
  test_block_storage_scan_heights();
  test_block_index_db_mark_pruned();
  test_block_index_db_get_pruned_height();
  test_block_index_db_is_pruned();
  test_node_config_prune_target();
  test_pruning_null_params();

  test_suite_end();
  return test_global_summary();
}
