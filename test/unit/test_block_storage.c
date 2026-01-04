/*
 * Bitcoin Echo — Block Storage Tests
 *
 * Tests for file-per-block storage with height-based indexing.
 */

#include "blocks_storage.h"
#include "echo_config.h"
#include "echo_types.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_utils.h"


#define ASSERT(cond)                                                           \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("\n%s:%d: Assertion failed: %s\n", __FILE__, __LINE__, #cond);    \
      return;                                                                 \
    }                                                                          \
  } while (0)

#define ASSERT_EQ(a, b)                                                        \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      printf("\n%s:%d: Expected %d, got %d\n", __FILE__, __LINE__, (int)(b),   \
             (int)(a));                                                        \
      return;                                                                 \
    }                                                                          \
  } while (0)

/*
 * Test data directory.
 */
#define TEST_DATA_DIR "/tmp/echo_block_storage_test"

/*
 * Recursively remove a directory and all its contents.
 * Intentional recursion, bounded by directory depth
 */
// NOLINTNEXTLINE(misc-no-recursion)
static void remove_dir_recursive(const char *path) {
  DIR *dir = opendir(path);
  if (dir == NULL) {
    return; /* Directory doesn't exist or can't be opened */
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

/*
 * Remove test directory and all contents.
 */
static void cleanup_test_dir(void) { remove_dir_recursive(TEST_DATA_DIR); }

/*
 * Create test block data with a unique pattern based on height.
 */
static void create_test_block(uint8_t *buf, uint32_t *size_out,
                              uint32_t height) {
  /* Block header (80 bytes) */
  uint8_t header[80] = {0};

  /* Version (4 bytes) */
  header[0] = 0x01;
  header[1] = 0x00;
  header[2] = 0x00;
  header[3] = 0x00;

  /* Previous block hash (32 bytes) - all zeros */

  /* Merkle root (32 bytes) - use height for uniqueness */
  header[36] = (height >> 0) & 0xFF;
  header[37] = (height >> 8) & 0xFF;
  header[38] = (height >> 16) & 0xFF;
  header[39] = (height >> 24) & 0xFF;

  /* Timestamp (4 bytes) */
  header[68] = 0x29;
  header[69] = 0xAB;
  header[70] = 0x5F;
  header[71] = 0x49;

  /* Bits (4 bytes) - max difficulty */
  header[72] = 0xFF;
  header[73] = 0xFF;
  header[74] = 0x00;
  header[75] = 0x1D;

  /* Nonce (4 bytes) */
  header[76] = (height >> 0) & 0xFF;
  header[77] = (height >> 8) & 0xFF;
  header[78] = (height >> 16) & 0xFF;
  header[79] = (height >> 24) & 0xFF;

  /* Transaction count (varint: 0) */
  uint8_t tx_count = 0;

  /* Assemble block */
  memcpy(buf, header, 80);
  buf[80] = tx_count;

  *size_out = 81;
}

/*
 * Test: Initialize block storage.
 */
static void test_init(void) {
  cleanup_test_dir();

  block_storage_t storage;
  echo_result_t result = block_storage_create(&storage, TEST_DATA_DIR);

  ASSERT_EQ(result, ECHO_OK);

  /* Verify blocks directory was created */
  char blocks_dir[512];
  snprintf(blocks_dir, sizeof(blocks_dir), "%s/%s", TEST_DATA_DIR,
           ECHO_BLOCKS_DIR);

  struct stat st;
  ASSERT(stat(blocks_dir, &st) == 0);
  ASSERT(S_ISDIR(st.st_mode));

  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Write a single block by height.
 */
static void test_write_single_block(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  /* Create test block */
  uint8_t block_data[256];
  uint32_t block_size;
  create_test_block(block_data, &block_size, 100);

  /* Write block at height 100 */
  echo_result_t result =
      block_storage_write_height(&storage, 100, block_data, block_size);

  ASSERT_EQ(result, ECHO_OK);

  /* Verify block exists */
  ASSERT(block_storage_exists_height(&storage, 100));
  ASSERT(!block_storage_exists_height(&storage, 99));
  ASSERT(!block_storage_exists_height(&storage, 101));

  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Read a block back by height.
 */
static void test_read_block(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  /* Create and write test block */
  uint8_t block_data[256];
  uint32_t block_size;
  create_test_block(block_data, &block_size, 42);

  ASSERT_EQ(block_storage_write_height(&storage, 42, block_data, block_size),
            ECHO_OK);

  /* Read block back */
  uint8_t *read_data = NULL;
  uint32_t read_size = 0;
  echo_result_t result =
      block_storage_read_height(&storage, 42, &read_data, &read_size);

  ASSERT_EQ(result, ECHO_OK);
  ASSERT(read_data != NULL);
  ASSERT_EQ(read_size, block_size);

  /* Verify data matches */
  ASSERT(memcmp(read_data, block_data, block_size) == 0);

  free(read_data);
  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Write multiple blocks at different heights.
 */
static void test_write_multiple_blocks(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  /* Write 10 blocks at heights 0, 100, 200, ... */
  for (uint32_t i = 0; i < 10; i++) {
    uint8_t block_data[256];
    uint32_t block_size;
    uint32_t height = i * 100;
    create_test_block(block_data, &block_size, height);

    ASSERT_EQ(block_storage_write_height(&storage, height, block_data, block_size),
              ECHO_OK);
  }

  /* Read all blocks back and verify */
  for (uint32_t i = 0; i < 10; i++) {
    uint32_t height = i * 100;
    uint8_t *read_data = NULL;
    uint32_t read_size = 0;
    ASSERT_EQ(block_storage_read_height(&storage, height, &read_data, &read_size),
              ECHO_OK);

    /* Verify height encoding in merkle root */
    ASSERT_EQ(read_data[36], (height >> 0) & 0xFF);
    ASSERT_EQ(read_data[37], (height >> 8) & 0xFF);

    free(read_data);
  }

  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Blocks persist after storage restart.
 */
static void test_resume_after_restart(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  /* Write some blocks */
  uint8_t block_data[256];
  uint32_t block_size;

  create_test_block(block_data, &block_size, 1);
  ASSERT_EQ(block_storage_write_height(&storage, 1, block_data, block_size), ECHO_OK);

  create_test_block(block_data, &block_size, 2);
  ASSERT_EQ(block_storage_write_height(&storage, 2, block_data, block_size), ECHO_OK);

  block_storage_destroy(&storage);

  /* Reinitialize storage (simulating restart) */
  block_storage_t storage2;
  ASSERT_EQ(block_storage_create(&storage2, TEST_DATA_DIR), ECHO_OK);

  /* Blocks should still exist */
  ASSERT(block_storage_exists_height(&storage2, 1));
  ASSERT(block_storage_exists_height(&storage2, 2));

  /* Should be able to read them */
  uint8_t *read_data = NULL;
  uint32_t read_size = 0;
  ASSERT_EQ(block_storage_read_height(&storage2, 1, &read_data, &read_size), ECHO_OK);
  ASSERT_EQ(read_data[36], 1); /* Height encoded in merkle root */
  free(read_data);

  /* Should be able to write more blocks */
  create_test_block(block_data, &block_size, 3);
  ASSERT_EQ(block_storage_write_height(&storage2, 3, block_data, block_size), ECHO_OK);

  block_storage_destroy(&storage2);
  cleanup_test_dir();
}

/*
 * Test: Get block file path.
 */
static void test_get_path(void) {
  block_storage_t storage;
  strcpy(storage.data_dir, TEST_DATA_DIR);

  char path[512];

  /* Height 0 -> subdirectory 0 */
  block_storage_get_height_path(&storage, 0, path);
  ASSERT(strstr(path, "/0/") != NULL);
  ASSERT(strstr(path, "000000000.blk") != NULL);

  /* Height 999 -> subdirectory 0 */
  block_storage_get_height_path(&storage, 999, path);
  ASSERT(strstr(path, "/0/") != NULL);
  ASSERT(strstr(path, "000000999.blk") != NULL);

  /* Height 1000 -> subdirectory 1 */
  block_storage_get_height_path(&storage, 1000, path);
  ASSERT(strstr(path, "/1/") != NULL);
  ASSERT(strstr(path, "000001000.blk") != NULL);

  /* Height 500000 -> subdirectory 500 */
  block_storage_get_height_path(&storage, 500000, path);
  ASSERT(strstr(path, "/500/") != NULL);
  ASSERT(strstr(path, "000500000.blk") != NULL);
}

/*
 * Test: Read non-existent block.
 */
static void test_read_nonexistent(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  uint8_t *block_data = NULL;
  uint32_t block_size = 0;

  echo_result_t result =
      block_storage_read_height(&storage, 99999, &block_data, &block_size);
  ASSERT_EQ(result, ECHO_ERR_NOT_FOUND);
  ASSERT(block_data == NULL);

  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: NULL parameter checks.
 */
static void test_null_params(void) {
  block_storage_t storage;
  uint8_t block_data[256];
  uint32_t block_size = 81;
  uint8_t *read_data = NULL;
  uint32_t read_size = 0;

  ASSERT_EQ(block_storage_create(NULL, TEST_DATA_DIR), ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_create(&storage, NULL), ECHO_ERR_NULL_PARAM);

  /* Initialize for further tests */
  cleanup_test_dir();
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  ASSERT_EQ(block_storage_write_height(NULL, 0, block_data, block_size),
            ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_write_height(&storage, 0, NULL, block_size),
            ECHO_ERR_NULL_PARAM);

  ASSERT_EQ(block_storage_read_height(NULL, 0, &read_data, &read_size),
            ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_read_height(&storage, 0, NULL, &read_size),
            ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_read_height(&storage, 0, &read_data, NULL),
            ECHO_ERR_NULL_PARAM);

  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Large block (near max size).
 */
static void test_large_block(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  /* Create a large block (1 MB) */
  uint32_t large_size = 1024 * 1024;
  uint8_t *large_block = (uint8_t *)malloc(large_size);
  ASSERT(large_block != NULL);

  /* Fill with pattern */
  for (uint32_t i = 0; i < large_size; i++) {
    large_block[i] = (uint8_t)(i & 0xFF);
  }

  /* Write it at height 12345 */
  ASSERT_EQ(block_storage_write_height(&storage, 12345, large_block, large_size),
            ECHO_OK);

  /* Read it back */
  uint8_t *read_data = NULL;
  uint32_t read_size = 0;
  ASSERT_EQ(block_storage_read_height(&storage, 12345, &read_data, &read_size),
            ECHO_OK);

  ASSERT_EQ(read_size, large_size);
  ASSERT(memcmp(read_data, large_block, large_size) == 0);

  free(large_block);
  free(read_data);
  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Prune blocks by height.
 */
static void test_prune_height(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  /* Write some blocks */
  uint8_t block_data[256];
  uint32_t block_size;

  for (uint32_t h = 0; h < 5; h++) {
    create_test_block(block_data, &block_size, h);
    ASSERT_EQ(block_storage_write_height(&storage, h, block_data, block_size), ECHO_OK);
  }

  /* Verify all exist */
  for (uint32_t h = 0; h < 5; h++) {
    ASSERT(block_storage_exists_height(&storage, h));
  }

  /* Prune block at height 2 */
  ASSERT_EQ(block_storage_prune_height(&storage, 2), ECHO_OK);

  /* Verify block 2 is gone but others remain */
  ASSERT(block_storage_exists_height(&storage, 0));
  ASSERT(block_storage_exists_height(&storage, 1));
  ASSERT(!block_storage_exists_height(&storage, 2));
  ASSERT(block_storage_exists_height(&storage, 3));
  ASSERT(block_storage_exists_height(&storage, 4));

  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Scan stored heights.
 */
static void test_scan_heights(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  /* Write blocks at heights 10, 100, 1000, 2500 (spans multiple subdirs) */
  uint8_t block_data[256];
  uint32_t block_size;
  uint32_t heights[] = {10, 100, 1000, 2500};
  size_t num_heights = sizeof(heights) / sizeof(heights[0]);

  for (size_t i = 0; i < num_heights; i++) {
    create_test_block(block_data, &block_size, heights[i]);
    ASSERT_EQ(block_storage_write_height(&storage, heights[i], block_data, block_size),
              ECHO_OK);
  }

  /* Scan heights */
  uint32_t *scanned_heights = NULL;
  size_t scanned_count = 0;
  ASSERT_EQ(block_storage_scan_heights(&storage, &scanned_heights, &scanned_count),
            ECHO_OK);

  ASSERT_EQ(scanned_count, num_heights);

  /* Verify heights are returned (should be sorted ascending) */
  for (size_t i = 0; i < num_heights; i++) {
    ASSERT_EQ(scanned_heights[i], heights[i]);
  }

  free(scanned_heights);
  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Get total storage size.
 */
static void test_total_size(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  /* Initially empty */
  uint64_t total_size = 0;
  ASSERT_EQ(block_storage_get_total_size(&storage, &total_size), ECHO_OK);
  ASSERT_EQ(total_size, 0);

  /* Write some blocks */
  uint8_t block_data[256];
  uint32_t block_size;

  create_test_block(block_data, &block_size, 0);
  ASSERT_EQ(block_storage_write_height(&storage, 0, block_data, block_size), ECHO_OK);

  create_test_block(block_data, &block_size, 1);
  ASSERT_EQ(block_storage_write_height(&storage, 1, block_data, block_size), ECHO_OK);

  /* Should have non-zero size now */
  ASSERT_EQ(block_storage_get_total_size(&storage, &total_size), ECHO_OK);
  ASSERT(total_size > 0);
  ASSERT(total_size >= 2 * block_size); /* At least 2 blocks worth */

  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Main test runner.
 */
int main(void) {
    test_suite_begin("Block Storage Tests");

    test_case("Initialize block storage"); test_init(); test_pass();
    test_case("Write single block"); test_write_single_block(); test_pass();
    test_case("Read block"); test_read_block(); test_pass();
    test_case("Write multiple blocks"); test_write_multiple_blocks(); test_pass();
    test_case("Resume after restart"); test_resume_after_restart(); test_pass();
    test_case("Get file path"); test_get_path(); test_pass();
    test_case("Read nonexistent block"); test_read_nonexistent(); test_pass();
    test_case("Null parameters"); test_null_params(); test_pass();
    test_case("Large block handling"); test_large_block(); test_pass();
    test_case("Prune block by height"); test_prune_height(); test_pass();
    test_case("Scan stored heights"); test_scan_heights(); test_pass();
    test_case("Get total storage size"); test_total_size(); test_pass();

    test_suite_end();
    return test_global_summary();
}
