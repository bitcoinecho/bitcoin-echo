/*
 * Bitcoin Echo — Pruning Tests
 *
 * Tests for block pruning functionality including:
 * - Block status flags for pruned blocks
 * - Block storage file operations (delete, size)
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

  /* Verify minimum is 550 MB (for reorg safety) */
  if (PRUNE_TARGET_MIN_MB != 550) {
    passed = false;
  }

  test_case("PRUNE_TARGET_MIN_MB constant");
  if (passed) {
    test_pass();
  } else {
    test_fail("expected 550 MB minimum");
  }
}

/*
 * Test: Block storage file existence check.
 */
static void test_block_storage_file_exists(void) {
  cleanup_test_dir();
  bool passed = true;

  block_file_manager_t mgr;
  if (block_storage_init(&mgr, TEST_DATA_DIR) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* File 0 should not exist yet */
  bool exists = true;
  if (block_storage_file_exists(&mgr, 0, &exists) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (exists != false) {
    passed = false;
    goto done;
  }

  /* Write a block to create the file */
  uint8_t block_data[100] = {0};
  block_file_pos_t pos;
  if (block_storage_write(&mgr, block_data, 100, &pos) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Now file 0 should exist */
  if (block_storage_file_exists(&mgr, 0, &exists) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (exists != true) {
    passed = false;
    goto done;
  }

  /* File 1 should not exist */
  if (block_storage_file_exists(&mgr, 1, &exists) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (exists != false) {
    passed = false;
    goto done;
  }

done:
  cleanup_test_dir();
  test_case("Block storage file exists check");
  if (passed) {
    test_pass();
  } else {
    test_fail("file existence check failed");
  }
}

/*
 * Test: Block storage file size query.
 */
static void test_block_storage_get_file_size(void) {
  cleanup_test_dir();
  bool passed = true;

  block_file_manager_t mgr;
  if (block_storage_init(&mgr, TEST_DATA_DIR) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Size of non-existent file should be 0 */
  uint64_t size = 999;
  if (block_storage_get_file_size(&mgr, 0, &size) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (size != 0) {
    passed = false;
    goto done;
  }

  /* Write a block */
  uint8_t block_data[100] = {0};
  block_file_pos_t pos;
  if (block_storage_write(&mgr, block_data, 100, &pos) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* File should now have size > 0 */
  if (block_storage_get_file_size(&mgr, 0, &size) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (size == 0) {
    passed = false;
    goto done;
  }
  /* Size should include record header */
  if (size < 100 + BLOCK_FILE_RECORD_HEADER_SIZE) {
    passed = false;
    goto done;
  }

done:
  cleanup_test_dir();
  test_case("Block storage get file size");
  if (passed) {
    test_pass();
  } else {
    test_fail("file size query failed");
  }
}

/*
 * Test: Block storage total size query.
 */
static void test_block_storage_get_total_size(void) {
  cleanup_test_dir();
  bool passed = true;

  block_file_manager_t mgr;
  if (block_storage_init(&mgr, TEST_DATA_DIR) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Empty storage should have size 0 */
  uint64_t total = 999;
  if (block_storage_get_total_size(&mgr, &total) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (total != 0) {
    passed = false;
    goto done;
  }

  /* Write some blocks */
  uint8_t block_data[100] = {0};
  block_file_pos_t pos;
  if (block_storage_write(&mgr, block_data, 100, &pos) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (block_storage_write(&mgr, block_data, 100, &pos) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (block_storage_write(&mgr, block_data, 100, &pos) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Total should be > 0 */
  if (block_storage_get_total_size(&mgr, &total) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (total == 0) {
    passed = false;
    goto done;
  }

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
 * Test: Block storage get current file.
 */
static void test_block_storage_get_current_file(void) {
  cleanup_test_dir();
  bool passed = true;

  block_file_manager_t mgr;
  if (block_storage_init(&mgr, TEST_DATA_DIR) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Initial file should be 0 */
  if (block_storage_get_current_file(&mgr) != 0) {
    passed = false;
    goto done;
  }

done:
  cleanup_test_dir();
  test_case("Block storage get current file");
  if (passed) {
    test_pass();
  } else {
    test_fail("current file index incorrect");
  }
}

/*
 * Test: Block storage delete file.
 */
static void test_block_storage_delete_file(void) {
  cleanup_test_dir();
  bool passed = true;

  block_file_manager_t mgr;
  if (block_storage_init(&mgr, TEST_DATA_DIR) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Write a block to create file 0 */
  uint8_t block_data[100] = {0};
  block_file_pos_t pos;
  if (block_storage_write(&mgr, block_data, 100, &pos) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Verify file exists */
  bool exists = false;
  if (block_storage_file_exists(&mgr, 0, &exists) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (exists != true) {
    passed = false;
    goto done;
  }

  /* Cannot delete current write file */
  if (block_storage_delete_file(&mgr, 0) != ECHO_ERR_INVALID_PARAM) {
    passed = false;
    goto done;
  }

  /* Simulate having moved to file 1 by modifying manager */
  mgr.current_file_index = 1;

  /* Now we should be able to delete file 0 */
  if (block_storage_delete_file(&mgr, 0) != ECHO_OK) {
    passed = false;
    goto done;
  }

  /* Verify file is gone */
  if (block_storage_file_exists(&mgr, 0, &exists) != ECHO_OK) {
    passed = false;
    goto done;
  }
  if (exists != false) {
    passed = false;
    goto done;
  }

done:
  cleanup_test_dir();
  test_case("Block storage delete file");
  if (passed) {
    test_pass();
  } else {
    test_fail("delete file operation failed");
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
  block_file_manager_t mgr;
  bool exists;
  uint64_t size;

  if (block_storage_file_exists(NULL, 0, &exists) != ECHO_ERR_NULL_PARAM) {
    passed = false;
    goto done;
  }
  if (block_storage_file_exists(&mgr, 0, NULL) != ECHO_ERR_NULL_PARAM) {
    passed = false;
    goto done;
  }

  if (block_storage_get_file_size(NULL, 0, &size) != ECHO_ERR_NULL_PARAM) {
    passed = false;
    goto done;
  }
  if (block_storage_get_file_size(&mgr, 0, NULL) != ECHO_ERR_NULL_PARAM) {
    passed = false;
    goto done;
  }

  if (block_storage_get_total_size(NULL, &size) != ECHO_ERR_NULL_PARAM) {
    passed = false;
    goto done;
  }
  if (block_storage_get_total_size(&mgr, NULL) != ECHO_ERR_NULL_PARAM) {
    passed = false;
    goto done;
  }

  if (block_storage_delete_file(NULL, 0) != ECHO_ERR_NULL_PARAM) {
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
 * Main test runner.
 */
int main(void) {
  test_suite_begin("Pruning Tests");

  test_pruned_flag_value();
  test_prune_target_min();
  test_block_storage_file_exists();
  test_block_storage_get_file_size();
  test_block_storage_get_total_size();
  test_block_storage_get_current_file();
  test_block_storage_delete_file();
  test_block_index_db_mark_pruned();
  test_block_index_db_get_pruned_height();
  test_block_index_db_is_pruned();
  test_node_config_prune_target();
  test_pruning_null_params();

  test_suite_end();
  return test_global_summary();
}
