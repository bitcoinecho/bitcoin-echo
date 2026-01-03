/*
 * Bitcoin Echo — Block File Storage Tests
 *
 * Tests for file-per-block storage.
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
 * Create a minimal valid block for testing.
 */
static void create_test_block(uint8_t *buf, uint32_t *size_out,
                              uint32_t nonce) {
  /* Block header (80 bytes) */
  uint8_t header[80] = {0};

  /* Version (4 bytes) */
  header[0] = 0x01;
  header[1] = 0x00;
  header[2] = 0x00;
  header[3] = 0x00;

  /* Previous block hash (32 bytes) - all zeros */

  /* Merkle root (32 bytes) - use nonce for uniqueness */
  header[36] = (nonce >> 0) & 0xFF;
  header[37] = (nonce >> 8) & 0xFF;
  header[38] = (nonce >> 16) & 0xFF;
  header[39] = (nonce >> 24) & 0xFF;

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
  header[76] = (nonce >> 0) & 0xFF;
  header[77] = (nonce >> 8) & 0xFF;
  header[78] = (nonce >> 16) & 0xFF;
  header[79] = (nonce >> 24) & 0xFF;

  /* Transaction count (varint: 0) */
  uint8_t tx_count = 0;

  /* Assemble block */
  memcpy(buf, header, 80);
  buf[80] = tx_count;

  *size_out = 81;
}

/* ============================================================================
 * FILE-PER-BLOCK STORAGE TESTS
 * ============================================================================
 */

/*
 * Test: Create file-per-block storage.
 */
static void test_create(void) {
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
 * Test: Write and read block by height.
 */
static void test_write_read(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  /* Create test block */
  uint8_t block_data[256];
  uint32_t block_size;
  create_test_block(block_data, &block_size, 42);

  /* Write at height 100 */
  ASSERT_EQ(block_storage_write_height(&storage, 100, block_data, block_size),
            ECHO_OK);

  /* Read back */
  uint8_t *read_data = NULL;
  uint32_t read_size = 0;
  ASSERT_EQ(block_storage_read_height(&storage, 100, &read_data, &read_size),
            ECHO_OK);

  ASSERT(read_data != NULL);
  ASSERT_EQ(read_size, block_size);
  ASSERT(memcmp(read_data, block_data, block_size) == 0);

  free(read_data);
  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Check block existence.
 */
static void test_exists(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  /* Should not exist initially */
  ASSERT(!block_storage_exists_height(&storage, 100));

  /* Create test block */
  uint8_t block_data[256];
  uint32_t block_size;
  create_test_block(block_data, &block_size, 42);

  /* Write it */
  ASSERT_EQ(block_storage_write_height(&storage, 100, block_data, block_size),
            ECHO_OK);

  /* Now should exist */
  ASSERT(block_storage_exists_height(&storage, 100));

  /* Other heights should not exist */
  ASSERT(!block_storage_exists_height(&storage, 99));
  ASSERT(!block_storage_exists_height(&storage, 101));

  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Prune block.
 */
static void test_prune(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  /* Create test block */
  uint8_t block_data[256];
  uint32_t block_size;
  create_test_block(block_data, &block_size, 42);

  /* Write it */
  ASSERT_EQ(block_storage_write_height(&storage, 100, block_data, block_size),
            ECHO_OK);
  ASSERT(block_storage_exists_height(&storage, 100));

  /* Prune it */
  ASSERT_EQ(block_storage_prune_height(&storage, 100), ECHO_OK);

  /* Should no longer exist */
  ASSERT(!block_storage_exists_height(&storage, 100));

  /* Pruning non-existent block should be OK (idempotent) */
  ASSERT_EQ(block_storage_prune_height(&storage, 100), ECHO_OK);

  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Scan heights.
 */
static void test_scan_heights(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  /* Create test block */
  uint8_t block_data[256];
  uint32_t block_size;
  create_test_block(block_data, &block_size, 1);

  /* Write blocks at various heights (out of order) */
  ASSERT_EQ(block_storage_write_height(&storage, 100, block_data, block_size),
            ECHO_OK);
  ASSERT_EQ(block_storage_write_height(&storage, 50, block_data, block_size),
            ECHO_OK);
  ASSERT_EQ(block_storage_write_height(&storage, 200, block_data, block_size),
            ECHO_OK);
  ASSERT_EQ(block_storage_write_height(&storage, 1, block_data, block_size),
            ECHO_OK);

  /* Scan heights */
  uint32_t *heights = NULL;
  size_t count = 0;
  ASSERT_EQ(block_storage_scan_heights(&storage, &heights, &count), ECHO_OK);

  ASSERT_EQ(count, 4);
  ASSERT(heights != NULL);

  /* Should be sorted */
  ASSERT_EQ(heights[0], 1);
  ASSERT_EQ(heights[1], 50);
  ASSERT_EQ(heights[2], 100);
  ASSERT_EQ(heights[3], 200);

  free(heights);
  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Get total size.
 */
static void test_total_size(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  /* Initially empty */
  uint64_t size = 0;
  ASSERT_EQ(block_storage_get_total_size(&storage, &size), ECHO_OK);
  ASSERT_EQ(size, 0);

  /* Create test block */
  uint8_t block_data[256];
  uint32_t block_size;
  create_test_block(block_data, &block_size, 1);

  /* Write two blocks */
  ASSERT_EQ(block_storage_write_height(&storage, 1, block_data, block_size),
            ECHO_OK);
  ASSERT_EQ(block_storage_write_height(&storage, 2, block_data, block_size),
            ECHO_OK);

  /* Check size */
  ASSERT_EQ(block_storage_get_total_size(&storage, &size), ECHO_OK);
  ASSERT_EQ(size, (uint64_t)block_size * 2);

  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Height path generation.
 */
static void test_height_path(void) {
  block_storage_t storage;
  strcpy(storage.data_dir, TEST_DATA_DIR);

  char path[512];

  block_storage_get_height_path(&storage, 0, path);
  ASSERT(strstr(path, "/blocks/0.blk") != NULL);

  block_storage_get_height_path(&storage, 1, path);
  ASSERT(strstr(path, "/blocks/1.blk") != NULL);

  block_storage_get_height_path(&storage, 500000, path);
  ASSERT(strstr(path, "/blocks/500000.blk") != NULL);
}

/*
 * Test: Read non-existent height.
 */
static void test_read_nonexistent(void) {
  cleanup_test_dir();

  block_storage_t storage;
  ASSERT_EQ(block_storage_create(&storage, TEST_DATA_DIR), ECHO_OK);

  uint8_t *read_data = NULL;
  uint32_t read_size = 0;

  echo_result_t result =
      block_storage_read_height(&storage, 999, &read_data, &read_size);
  ASSERT_EQ(result, ECHO_ERR_NOT_FOUND);
  ASSERT(read_data == NULL);

  block_storage_destroy(&storage);
  cleanup_test_dir();
}

/*
 * Test: Null parameters for new API.
 */
static void test_null_params(void) {
  block_storage_t storage;
  uint8_t block_data[256];
  uint32_t block_size = 81;
  uint8_t *read_data = NULL;
  uint32_t read_size = 0;
  uint32_t *heights = NULL;
  size_t count = 0;
  uint64_t total_size = 0;

  ASSERT_EQ(block_storage_create(NULL, TEST_DATA_DIR), ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_create(&storage, NULL), ECHO_ERR_NULL_PARAM);

  ASSERT_EQ(block_storage_write_height(NULL, 1, block_data, block_size),
            ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_write_height(&storage, 1, NULL, block_size),
            ECHO_ERR_NULL_PARAM);

  ASSERT_EQ(block_storage_read_height(NULL, 1, &read_data, &read_size),
            ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_read_height(&storage, 1, NULL, &read_size),
            ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_read_height(&storage, 1, &read_data, NULL),
            ECHO_ERR_NULL_PARAM);

  ASSERT_EQ(block_storage_prune_height(NULL, 1), ECHO_ERR_NULL_PARAM);

  ASSERT_EQ(block_storage_scan_heights(NULL, &heights, &count),
            ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_scan_heights(&storage, NULL, &count),
            ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_scan_heights(&storage, &heights, NULL),
            ECHO_ERR_NULL_PARAM);

  ASSERT_EQ(block_storage_get_total_size(NULL, &total_size),
            ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_get_total_size(&storage, NULL),
            ECHO_ERR_NULL_PARAM);
}

/*
 * Main test runner.
 */
int main(void) {
  test_suite_begin("Block Storage Tests");

  test_case("Create storage");
  test_create();
  test_pass();
  test_case("Write and read by height");
  test_write_read();
  test_pass();
  test_case("Block existence check");
  test_exists();
  test_pass();
  test_case("Prune block");
  test_prune();
  test_pass();
  test_case("Scan heights");
  test_scan_heights();
  test_pass();
  test_case("Get total size");
  test_total_size();
  test_pass();
  test_case("Height path generation");
  test_height_path();
  test_pass();
  test_case("Read nonexistent height");
  test_read_nonexistent();
  test_pass();
  test_case("Null parameters");
  test_null_params();
  test_pass();

  test_suite_end();
  return test_global_summary();
}
