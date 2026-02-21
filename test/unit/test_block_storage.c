/*
 * Bitcoin Echo — Block File Storage Tests
 *
 * Tests for append-only block file storage.
 */

#include "blocks_storage.h"
#include "echo_config.h"
#include "echo_types.h"
#include <dirent.h>
#include <pthread.h>
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
 * Separate data directory for concurrent tests to avoid interference.
 */
#define TEST_CONCURRENT_DIR "/tmp/echo_block_storage_concurrent_test"

/*
 * Number of threads and writes per thread for concurrent tests.
 */
#define CONCURRENT_WRITER_THREADS 4
#define CONCURRENT_WRITES_PER_THREAD 50
#define CONCURRENT_TOTAL_WRITES \
  (CONCURRENT_WRITER_THREADS * CONCURRENT_WRITES_PER_THREAD)

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
 * Remove concurrent test directory.
 */
static void cleanup_concurrent_dir(void) {
  remove_dir_recursive(TEST_CONCURRENT_DIR);
}

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

/*
 * Test: Initialize block storage manager.
 */
static void test_init(void) {
  cleanup_test_dir();

  block_file_manager_t mgr;
  echo_result_t result = block_storage_init(&mgr, TEST_DATA_DIR);

  ASSERT_EQ(result, ECHO_OK);
  ASSERT_EQ(mgr.current_file_index, 0);
  ASSERT_EQ(mgr.current_file_offset, 0);

  /* Verify blocks directory was created */
  char blocks_dir[512];
  snprintf(blocks_dir, sizeof(blocks_dir), "%s/%s", TEST_DATA_DIR,
           ECHO_BLOCKS_DIR);

  struct stat st;
  ASSERT(stat(blocks_dir, &st) == 0);
  ASSERT(S_ISDIR(st.st_mode));

  cleanup_test_dir();
}

/*
 * Test: Write a single block.
 */
static void test_write_single_block(void) {
  cleanup_test_dir();

  block_file_manager_t mgr;
  ASSERT_EQ(block_storage_init(&mgr, TEST_DATA_DIR), ECHO_OK);

  /* Create test block */
  uint8_t block_data[256];
  uint32_t block_size;
  create_test_block(block_data, &block_size, 1);

  /* Write block */
  block_file_pos_t pos;
  echo_result_t result =
      block_storage_write(&mgr, block_data, block_size, &pos);

  ASSERT_EQ(result, ECHO_OK);
  ASSERT_EQ(pos.file_index, 0);
  ASSERT_EQ(pos.file_offset, 0);

  /* Manager should have updated position */
  ASSERT_EQ(mgr.current_file_index, 0);
  ASSERT_EQ(mgr.current_file_offset,
            BLOCK_FILE_RECORD_HEADER_SIZE + block_size);

  cleanup_test_dir();
}

/*
 * Test: Read a block back.
 */
static void test_read_block(void) {
  cleanup_test_dir();

  block_file_manager_t mgr;
  ASSERT_EQ(block_storage_init(&mgr, TEST_DATA_DIR), ECHO_OK);

  /* Create and write test block */
  uint8_t block_data[256];
  uint32_t block_size;
  create_test_block(block_data, &block_size, 1);

  block_file_pos_t pos;
  ASSERT_EQ(block_storage_write(&mgr, block_data, block_size, &pos), ECHO_OK);

  /* Read block back */
  uint8_t *read_data = NULL;
  uint32_t read_size = 0;
  echo_result_t result = block_storage_read(&mgr, pos, &read_data, &read_size);

  ASSERT_EQ(result, ECHO_OK);
  ASSERT(read_data != NULL);
  ASSERT_EQ(read_size, block_size);

  /* Verify data matches */
  ASSERT(memcmp(read_data, block_data, block_size) == 0);

  free(read_data);
  cleanup_test_dir();
}

/*
 * Test: Write multiple blocks.
 */
static void test_write_multiple_blocks(void) {
  cleanup_test_dir();

  block_file_manager_t mgr;
  ASSERT_EQ(block_storage_init(&mgr, TEST_DATA_DIR), ECHO_OK);

  block_file_pos_t positions[10];

  /* Write 10 blocks */
  for (uint32_t i = 0; i < 10; i++) {
    uint8_t block_data[256];
    uint32_t block_size;
    create_test_block(block_data, &block_size, i);

    ASSERT_EQ(block_storage_write(&mgr, block_data, block_size, &positions[i]),
              ECHO_OK);
  }

  /* Read all blocks back and verify */
  for (uint32_t i = 0; i < 10; i++) {
    uint8_t *read_data = NULL;
    uint32_t read_size = 0;
    ASSERT_EQ(block_storage_read(&mgr, positions[i], &read_data, &read_size),
              ECHO_OK);

    /* Verify nonce in merkle root */
    ASSERT_EQ(read_data[36], (i >> 0) & 0xFF);
    ASSERT_EQ(read_data[37], (i >> 8) & 0xFF);

    free(read_data);
  }

  cleanup_test_dir();
}

/*
 * Test: Resume after restart (scan existing files).
 */
static void test_resume_after_restart(void) {
  cleanup_test_dir();

  block_file_manager_t mgr;
  ASSERT_EQ(block_storage_init(&mgr, TEST_DATA_DIR), ECHO_OK);

  /* Write some blocks */
  uint8_t block_data[256];
  uint32_t block_size;
  create_test_block(block_data, &block_size, 1);

  block_file_pos_t pos1, pos2;
  ASSERT_EQ(block_storage_write(&mgr, block_data, block_size, &pos1), ECHO_OK);

  create_test_block(block_data, &block_size, 2);
  ASSERT_EQ(block_storage_write(&mgr, block_data, block_size, &pos2), ECHO_OK);

  uint32_t expected_offset = mgr.current_file_offset;

  /* Reinitialize manager (simulating restart) */
  block_file_manager_t mgr2;
  ASSERT_EQ(block_storage_init(&mgr2, TEST_DATA_DIR), ECHO_OK);

  /* Should resume at same position */
  ASSERT_EQ(mgr2.current_file_index, 0);
  ASSERT_EQ(mgr2.current_file_offset, expected_offset);

  /* Should be able to write more blocks */
  create_test_block(block_data, &block_size, 3);
  block_file_pos_t pos3;
  ASSERT_EQ(block_storage_write(&mgr2, block_data, block_size, &pos3), ECHO_OK);

  cleanup_test_dir();
}

/*
 * Test: Get block file path.
 */
static void test_get_path(void) {
  block_file_manager_t mgr;
  strcpy(mgr.data_dir, TEST_DATA_DIR);

  char path[512];

  block_storage_get_path(&mgr, 0, path);
  ASSERT(strstr(path, "blk00000.dat") != NULL);

  block_storage_get_path(&mgr, 1, path);
  ASSERT(strstr(path, "blk00001.dat") != NULL);

  block_storage_get_path(&mgr, 99999, path);
  ASSERT(strstr(path, "blk99999.dat") != NULL);
}

/*
 * Test: Read non-existent block.
 */
static void test_read_nonexistent(void) {
  cleanup_test_dir();

  block_file_manager_t mgr;
  ASSERT_EQ(block_storage_init(&mgr, TEST_DATA_DIR), ECHO_OK);

  block_file_pos_t pos;
  pos.file_index = 99;
  pos.file_offset = 0;

  uint8_t *block_data = NULL;
  uint32_t block_size = 0;

  echo_result_t result =
      block_storage_read(&mgr, pos, &block_data, &block_size);
  ASSERT_EQ(result, ECHO_ERR_NOT_FOUND);
  ASSERT(block_data == NULL);

  cleanup_test_dir();
}

/*
 * Test: NULL parameter checks.
 */
static void test_null_params(void) {
  block_file_manager_t mgr;
  uint8_t block_data[256];
  uint32_t block_size = 81;
  block_file_pos_t pos;
  uint8_t *read_data = NULL;
  uint32_t read_size = 0;

  ASSERT_EQ(block_storage_init(NULL, TEST_DATA_DIR), ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_init(&mgr, NULL), ECHO_ERR_NULL_PARAM);

  ASSERT_EQ(block_storage_write(NULL, block_data, block_size, &pos),
            ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_write(&mgr, NULL, block_size, &pos),
            ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_write(&mgr, block_data, block_size, NULL),
            ECHO_ERR_NULL_PARAM);

  ASSERT_EQ(block_storage_read(NULL, pos, &read_data, &read_size),
            ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_read(&mgr, pos, NULL, &read_size),
            ECHO_ERR_NULL_PARAM);
  ASSERT_EQ(block_storage_read(&mgr, pos, &read_data, NULL),
            ECHO_ERR_NULL_PARAM);
}

/*
 * Test: Large block (near max size).
 */
static void test_large_block(void) {
  cleanup_test_dir();

  block_file_manager_t mgr;
  ASSERT_EQ(block_storage_init(&mgr, TEST_DATA_DIR), ECHO_OK);

  /* Create a large block (1 MB) */
  uint32_t large_size = 1024 * 1024;
  uint8_t *large_block = (uint8_t *)malloc(large_size);
  ASSERT(large_block != NULL);

  /* Fill with pattern */
  for (uint32_t i = 0; i < large_size; i++) {
    large_block[i] = (uint8_t)(i & 0xFF);
  }

  /* Write it */
  block_file_pos_t pos;
  ASSERT_EQ(block_storage_write(&mgr, large_block, large_size, &pos), ECHO_OK);

  /* Read it back */
  uint8_t *read_data = NULL;
  uint32_t read_size = 0;
  ASSERT_EQ(block_storage_read(&mgr, pos, &read_data, &read_size), ECHO_OK);

  ASSERT_EQ(read_size, large_size);
  ASSERT(memcmp(read_data, large_block, large_size) == 0);

  free(large_block);
  free(read_data);
  cleanup_test_dir();
}

/*
 * ============================================================================
 * CONCURRENT STORAGE TESTS
 * ============================================================================
 *
 * These tests verify that the mutex in block_file_manager_t correctly
 * serializes concurrent writes and reads, preventing corruption and deadlock.
 */

/*
 * Thread argument for concurrent storage write tests.
 * Each writer thread uses thread_id and a write sequence counter to fill
 * its blocks with a recognizable pattern that can be verified after joining.
 */
typedef struct {
  block_file_manager_t *mgr;
  int thread_id;           /* 0-3 */
  int write_count;         /* Number of writes to perform */
  block_file_pos_t *positions; /* Output: positions for each write */
  uint8_t *patterns;       /* Output: first byte of pattern for each write */
  int result;              /* 0 = success, -1 = error */
} storage_writer_arg_t;

/*
 * Thread argument for concurrent storage read tests.
 * Each reader reads a slice of pre-written blocks and verifies content.
 */
typedef struct {
  block_file_manager_t *mgr;
  block_file_pos_t *positions;  /* Input: positions to read */
  uint8_t *expected_patterns;   /* Input: expected first byte for each pos */
  int read_start;               /* First index to read */
  int read_count;               /* Number of reads to perform */
  int result;                   /* 0 = success, -1 = error */
} storage_reader_arg_t;

/*
 * Writer thread function.
 * Writes write_count blocks of varying sizes using a thread_id-based pattern.
 * Sizes cycle through 64-4096 bytes to exercise different record sizes.
 */
static void *concurrent_writer(void *arg) {
  storage_writer_arg_t *a = (storage_writer_arg_t *)arg;
  a->result = 0;

  for (int i = 0; i < a->write_count; i++) {
    /* Vary block sizes between 64 and 4096 bytes */
    uint32_t size = (uint32_t)(64 + ((a->thread_id * 17 + i * 31) % 4033));

    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) {
      a->result = -1;
      return NULL;
    }

    /*
     * Fill with a recognizable pattern: alternating thread_id and sequence.
     * First byte encodes (thread_id << 4) | (i & 0x0F) for later verification.
     */
    uint8_t pattern_byte = (uint8_t)(((a->thread_id & 0xF) << 4) | (i & 0xF));
    memset(data, pattern_byte, size);
    a->patterns[i] = pattern_byte;

    echo_result_t r = block_storage_write(a->mgr, data, size, &a->positions[i]);
    free(data);

    if (r != ECHO_OK) {
      a->result = -1;
      return NULL;
    }
  }

  return NULL;
}

/*
 * Reader thread function.
 * Reads previously-written blocks and verifies the pattern byte matches.
 */
static void *concurrent_reader(void *arg) {
  storage_reader_arg_t *a = (storage_reader_arg_t *)arg;
  a->result = 0;

  for (int i = 0; i < a->read_count; i++) {
    int idx = a->read_start + i;
    uint8_t *data = NULL;
    uint32_t size = 0;

    echo_result_t r =
        block_storage_read(a->mgr, a->positions[idx], &data, &size);

    if (r != ECHO_OK || data == NULL) {
      a->result = -1;
      return NULL;
    }

    /* Verify pattern: every byte should match the expected pattern byte */
    uint8_t expected = a->expected_patterns[idx];
    for (uint32_t b = 0; b < size; b++) {
      if (data[b] != expected) {
        free(data);
        a->result = -1;
        return NULL;
      }
    }

    free(data);
  }

  return NULL;
}

/*
 * Test: Concurrent writes from 4 threads (TEST-02).
 *
 * 4 writer threads each write 50 blocks to the same manager simultaneously.
 * After joining, verifies:
 * - All 200 writes returned ECHO_OK
 * - No two writes share the same file_index + file_offset (positions unique)
 * - Reading back each position returns the correct pattern data
 */
static void test_concurrent_writes(void) {
  cleanup_concurrent_dir();

  block_file_manager_t mgr;
  ASSERT_EQ(block_storage_init(&mgr, TEST_CONCURRENT_DIR), ECHO_OK);

  /* Allocate per-thread position and pattern arrays */
  block_file_pos_t *all_positions = (block_file_pos_t *)malloc(
      sizeof(block_file_pos_t) * (size_t)CONCURRENT_TOTAL_WRITES);
  uint8_t *all_patterns =
      (uint8_t *)malloc(sizeof(uint8_t) * (size_t)CONCURRENT_TOTAL_WRITES);
  ASSERT(all_positions != NULL);
  ASSERT(all_patterns != NULL);

  pthread_t threads[CONCURRENT_WRITER_THREADS];
  storage_writer_arg_t args[CONCURRENT_WRITER_THREADS];

  /* Launch writer threads */
  for (int t = 0; t < CONCURRENT_WRITER_THREADS; t++) {
    args[t].mgr = &mgr;
    args[t].thread_id = t;
    args[t].write_count = CONCURRENT_WRITES_PER_THREAD;
    args[t].positions = &all_positions[(size_t)t * CONCURRENT_WRITES_PER_THREAD];
    args[t].patterns = &all_patterns[(size_t)t * CONCURRENT_WRITES_PER_THREAD];
    args[t].result = 0;

    int rc = pthread_create(&threads[t], NULL, concurrent_writer, &args[t]);
    if (rc != 0) {
      free(all_positions);
      free(all_patterns);
      ASSERT(rc == 0); /* Force test failure */
    }
  }

  /* Join all threads */
  for (int t = 0; t < CONCURRENT_WRITER_THREADS; t++) {
    pthread_join(threads[t], NULL);
  }

  /* Verify all writes succeeded */
  for (int t = 0; t < CONCURRENT_WRITER_THREADS; t++) {
    if (args[t].result != 0) {
      free(all_positions);
      free(all_patterns);
      ASSERT(args[t].result == 0); /* Force test failure */
    }
  }

  /*
   * Verify all positions are unique (no two writes got same file+offset).
   * Use O(n^2) check — 200 writes is small enough.
   */
  for (int i = 0; i < CONCURRENT_TOTAL_WRITES; i++) {
    for (int j = i + 1; j < CONCURRENT_TOTAL_WRITES; j++) {
      int same_file = (all_positions[i].file_index == all_positions[j].file_index);
      int same_off = (all_positions[i].file_offset == all_positions[j].file_offset);
      if (same_file && same_off) {
        free(all_positions);
        free(all_patterns);
        ASSERT(!(same_file && same_off)); /* Duplicate position detected */
      }
    }
  }

  /* Read back every written block and verify content */
  for (int i = 0; i < CONCURRENT_TOTAL_WRITES; i++) {
    uint8_t *data = NULL;
    uint32_t size = 0;
    echo_result_t r = block_storage_read(&mgr, all_positions[i], &data, &size);
    if (r != ECHO_OK || data == NULL) {
      free(all_positions);
      free(all_patterns);
      ASSERT(r == ECHO_OK);
    }

    /* Verify every byte matches the pattern */
    uint8_t expected = all_patterns[i];
    int mismatch = 0;
    for (uint32_t b = 0; b < size; b++) {
      if (data[b] != expected) {
        mismatch = 1;
        break;
      }
    }
    free(data);

    if (mismatch) {
      free(all_positions);
      free(all_patterns);
      ASSERT(!mismatch);
    }
  }

  free(all_positions);
  free(all_patterns);
  block_storage_close(&mgr);
  cleanup_concurrent_dir();
}

/*
 * Test: Concurrent readers from 4 threads (TEST-02).
 *
 * Write 100 blocks sequentially. Then spawn 4 reader threads each reading
 * 25 blocks by position. Verify all reads return correct data.
 */
static void test_concurrent_reads(void) {
  cleanup_concurrent_dir();

  block_file_manager_t mgr;
  ASSERT_EQ(block_storage_init(&mgr, TEST_CONCURRENT_DIR), ECHO_OK);

  /* Write 100 blocks sequentially to establish known positions */
  int total = 100;
  block_file_pos_t *positions =
      (block_file_pos_t *)malloc(sizeof(block_file_pos_t) * (size_t)total);
  uint8_t *patterns = (uint8_t *)malloc(sizeof(uint8_t) * (size_t)total);
  ASSERT(positions != NULL);
  ASSERT(patterns != NULL);

  for (int i = 0; i < total; i++) {
    uint32_t size = (uint32_t)(64 + (i * 13 % 512)); /* Vary sizes */
    uint8_t *data = (uint8_t *)malloc(size);
    ASSERT(data != NULL);

    uint8_t pat = (uint8_t)(0xA0 | (i & 0x1F));
    memset(data, pat, size);
    patterns[i] = pat;

    echo_result_t r = block_storage_write(&mgr, data, size, &positions[i]);
    free(data);
    ASSERT_EQ(r, ECHO_OK);
  }

  /* Each of 4 reader threads reads 25 blocks */
  int readers = 4;
  int per_reader = total / readers;

  pthread_t threads[4];
  storage_reader_arg_t args[4];

  for (int t = 0; t < readers; t++) {
    args[t].mgr = &mgr;
    args[t].positions = positions;
    args[t].expected_patterns = patterns;
    args[t].read_start = t * per_reader;
    args[t].read_count = per_reader;
    args[t].result = 0;

    int rc = pthread_create(&threads[t], NULL, concurrent_reader, &args[t]);
    if (rc != 0) {
      free(positions);
      free(patterns);
      ASSERT(rc == 0);
    }
  }

  for (int t = 0; t < readers; t++) {
    pthread_join(threads[t], NULL);
  }

  for (int t = 0; t < readers; t++) {
    if (args[t].result != 0) {
      free(positions);
      free(patterns);
      ASSERT(args[t].result == 0);
    }
  }

  free(positions);
  free(patterns);
  block_storage_close(&mgr);
  cleanup_concurrent_dir();
}

/*
 * Mixed writer thread: continuously writes new blocks.
 * Stores positions in a shared array protected by a mutex.
 */
typedef struct {
  block_file_manager_t *mgr;
  block_file_pos_t *shared_positions; /* Shared output array */
  uint8_t *shared_patterns;           /* Pattern byte for each write */
  int *shared_count;                  /* Number of positions written so far */
  pthread_mutex_t *count_mutex;       /* Protects shared_count */
  int target_writes;                  /* How many writes to perform */
  int result;
} mixed_writer_arg_t;

/*
 * Mixed reader thread: reads blocks whose positions have been committed
 * by writers. Uses the shared_count to only read committed positions.
 */
typedef struct {
  block_file_manager_t *mgr;
  block_file_pos_t *shared_positions; /* Input: shared positions array */
  uint8_t *shared_patterns;           /* Expected pattern for each */
  int *shared_count;                  /* Number of available positions */
  pthread_mutex_t *count_mutex;       /* Protects shared_count */
  int target_reads;                   /* How many successful reads to do */
  int result;
} mixed_reader_arg_t;

static void *mixed_writer_thread(void *arg) {
  mixed_writer_arg_t *a = (mixed_writer_arg_t *)arg;
  a->result = 0;

  for (int i = 0; i < a->target_writes; i++) {
    uint32_t size = (uint32_t)(128 + (i * 37 % 512));
    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) {
      a->result = -1;
      return NULL;
    }

    uint8_t pat = (uint8_t)(0xC0 | (i & 0x3F));
    memset(data, pat, size);

    block_file_pos_t pos;
    echo_result_t r = block_storage_write(a->mgr, data, size, &pos);
    free(data);

    if (r != ECHO_OK) {
      a->result = -1;
      return NULL;
    }

    /* Publish the new position to readers */
    pthread_mutex_lock(a->count_mutex);
    int idx = *a->shared_count;
    a->shared_positions[idx] = pos;
    a->shared_patterns[idx] = pat;
    (*a->shared_count)++;
    pthread_mutex_unlock(a->count_mutex);
  }

  return NULL;
}

static void *mixed_reader_thread(void *arg) {
  mixed_reader_arg_t *a = (mixed_reader_arg_t *)arg;
  a->result = 0;

  int reads_done = 0;
  int spin_limit = 100000; /* Prevent infinite loop if writers fail */
  int spin = 0;

  while (reads_done < a->target_reads && spin < spin_limit) {
    /* Snapshot current write count */
    pthread_mutex_lock(a->count_mutex);
    int available = *a->shared_count;
    pthread_mutex_unlock(a->count_mutex);

    if (available == 0) {
      spin++;
      /* Yield to let writers make progress */
      sched_yield();
      continue;
    }

    /* Read from the middle of available positions */
    int idx = reads_done % available;

    uint8_t *data = NULL;
    uint32_t size = 0;
    echo_result_t r =
        block_storage_read(a->mgr, a->shared_positions[idx], &data, &size);

    if (r != ECHO_OK || data == NULL) {
      a->result = -1;
      return NULL;
    }

    /* Verify pattern */
    uint8_t expected = a->shared_patterns[idx];
    int mismatch = 0;
    for (uint32_t b = 0; b < size; b++) {
      if (data[b] != expected) {
        mismatch = 1;
        break;
      }
    }
    free(data);

    if (mismatch) {
      a->result = -1;
      return NULL;
    }

    reads_done++;
    spin = 0;
  }

  return NULL;
}

/*
 * Test: Mixed concurrent read/write (2 writer threads + 2 reader threads).
 *
 * Writer threads write new blocks while reader threads read previously-written
 * blocks. Verifies no corruption or deadlock under concurrent mixed access.
 */
static void test_mixed_concurrent_rw(void) {
  cleanup_concurrent_dir();

  block_file_manager_t mgr;
  ASSERT_EQ(block_storage_init(&mgr, TEST_CONCURRENT_DIR), ECHO_OK);

  int total_slots = 200; /* Writers will write up to 200 blocks total */

  block_file_pos_t *shared_positions = (block_file_pos_t *)malloc(
      sizeof(block_file_pos_t) * (size_t)total_slots);
  uint8_t *shared_patterns =
      (uint8_t *)malloc(sizeof(uint8_t) * (size_t)total_slots);
  ASSERT(shared_positions != NULL);
  ASSERT(shared_patterns != NULL);

  int shared_count = 0;
  pthread_mutex_t count_mutex;
  pthread_mutex_init(&count_mutex, NULL);

  /* 2 writer threads writing 100 blocks each */
  pthread_t wthreads[2];
  mixed_writer_arg_t wargs[2];

  for (int t = 0; t < 2; t++) {
    wargs[t].mgr = &mgr;
    wargs[t].shared_positions = shared_positions;
    wargs[t].shared_patterns = shared_patterns;
    wargs[t].shared_count = &shared_count;
    wargs[t].count_mutex = &count_mutex;
    wargs[t].target_writes = 100;
    wargs[t].result = 0;

    int rc = pthread_create(&wthreads[t], NULL, mixed_writer_thread, &wargs[t]);
    if (rc != 0) {
      free(shared_positions);
      free(shared_patterns);
      pthread_mutex_destroy(&count_mutex);
      ASSERT(rc == 0);
    }
  }

  /* 2 reader threads each doing 50 reads */
  pthread_t rthreads[2];
  mixed_reader_arg_t rargs[2];

  for (int t = 0; t < 2; t++) {
    rargs[t].mgr = &mgr;
    rargs[t].shared_positions = shared_positions;
    rargs[t].shared_patterns = shared_patterns;
    rargs[t].shared_count = &shared_count;
    rargs[t].count_mutex = &count_mutex;
    rargs[t].target_reads = 50;
    rargs[t].result = 0;

    int rc = pthread_create(&rthreads[t], NULL, mixed_reader_thread, &rargs[t]);
    if (rc != 0) {
      free(shared_positions);
      free(shared_patterns);
      pthread_mutex_destroy(&count_mutex);
      ASSERT(rc == 0);
    }
  }

  /* Join all threads */
  for (int t = 0; t < 2; t++) {
    pthread_join(wthreads[t], NULL);
  }
  for (int t = 0; t < 2; t++) {
    pthread_join(rthreads[t], NULL);
  }

  /* Verify all threads succeeded */
  for (int t = 0; t < 2; t++) {
    if (wargs[t].result != 0) {
      free(shared_positions);
      free(shared_patterns);
      pthread_mutex_destroy(&count_mutex);
      ASSERT(wargs[t].result == 0);
    }
    if (rargs[t].result != 0) {
      free(shared_positions);
      free(shared_patterns);
      pthread_mutex_destroy(&count_mutex);
      ASSERT(rargs[t].result == 0);
    }
  }

  free(shared_positions);
  free(shared_patterns);
  pthread_mutex_destroy(&count_mutex);
  block_storage_close(&mgr);
  cleanup_concurrent_dir();
}

/*
 * ============================================================================
 * LARGE BLOCK AND EDGE CASE TESTS
 * ============================================================================
 *
 * These tests exercise size boundary conditions and error detection for
 * corrupted or truncated block files.
 */

/*
 * Test: Block at exact ECHO_MAX_BLOCK_SIZE boundary (TEST-05).
 *
 * Writes a block of exactly ECHO_MAX_BLOCK_SIZE bytes and reads it back.
 * Verifies the storage layer handles the maximum legacy block size without
 * off-by-one errors.
 */
static void test_max_size_block(void) {
  cleanup_test_dir();

  block_file_manager_t mgr;
  ASSERT_EQ(block_storage_init(&mgr, TEST_DATA_DIR), ECHO_OK);

  uint32_t size = ECHO_MAX_BLOCK_SIZE;
  uint8_t *data = (uint8_t *)malloc(size);
  ASSERT(data != NULL);

  /* Fill with recognizable pattern */
  memset(data, 0xAB, size);

  block_file_pos_t pos;
  echo_result_t r = block_storage_write(&mgr, data, size, &pos);
  ASSERT_EQ(r, ECHO_OK);

  /* Read it back */
  uint8_t *read_data = NULL;
  uint32_t read_size = 0;
  r = block_storage_read(&mgr, pos, &read_data, &read_size);
  ASSERT_EQ(r, ECHO_OK);
  ASSERT(read_data != NULL);
  ASSERT_EQ(read_size, size);

  /* Verify all bytes match */
  ASSERT(memcmp(read_data, data, size) == 0);

  free(data);
  free(read_data);
  block_storage_close(&mgr);
  cleanup_test_dir();
}

/*
 * Test: Corrupted size field is detected and returns error (TEST-05).
 *
 * Writes a normal block, then directly corrupts the 4-byte size field in the
 * blk*.dat file with 0xFFFFFFFF (4 GB). Verifies block_storage_read() returns
 * an error instead of attempting a 4 GB malloc.
 *
 * The size check in block_storage_read() rejects values > ECHO_MAX_BLOCK_SIZE*4,
 * so 0xFFFFFFFF (4,294,967,295) is well above the threshold and must be rejected.
 */
static void test_corrupted_size_field(void) {
  cleanup_test_dir();

  block_file_manager_t mgr;
  ASSERT_EQ(block_storage_init(&mgr, TEST_DATA_DIR), ECHO_OK);

  /* Write a small valid block to get a real position */
  uint8_t block_data[256];
  uint32_t block_size;
  create_test_block(block_data, &block_size, 42);

  block_file_pos_t pos;
  ASSERT_EQ(block_storage_write(&mgr, block_data, block_size, &pos), ECHO_OK);

  /* Close write handle so we can reopen the file for corruption */
  block_storage_close(&mgr);

  /* Get the path to the block file */
  block_file_manager_t tmp_mgr;
  strcpy(tmp_mgr.data_dir, TEST_DATA_DIR);
  char path[512];
  block_storage_get_path(&tmp_mgr, pos.file_index, path);

  /*
   * Corrupt the size field: it is 4 bytes at pos.file_offset + 4
   * (after the 4-byte magic). Write 0xFFFFFFFF (4 GB) as little-endian.
   */
  FILE *f = fopen(path, "r+b");
  ASSERT(f != NULL);

  long size_field_offset = (long)pos.file_offset + 4; /* Skip magic bytes */
  ASSERT(fseek(f, size_field_offset, SEEK_SET) == 0);

  uint8_t corrupt_size[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  ASSERT(fwrite(corrupt_size, 1, 4, f) == 4);
  fclose(f);

  /* Reinitialize manager to get fresh file handles */
  ASSERT_EQ(block_storage_init(&mgr, TEST_DATA_DIR), ECHO_OK);

  /* Attempt to read — must return an error, not crash or malloc 4 GB */
  uint8_t *read_data = NULL;
  uint32_t read_size = 0;
  echo_result_t r = block_storage_read(&mgr, pos, &read_data, &read_size);

  /* Any error code is acceptable; success is not */
  ASSERT(r != ECHO_OK);
  ASSERT(read_data == NULL);

  block_storage_close(&mgr);
  cleanup_test_dir();
}

/*
 * Test: Truncated read — file shorter than size field claims (TEST-05).
 *
 * Writes a block, then truncates the blk*.dat file so it is shorter than
 * what the size field promises. Verifies block_storage_read() returns
 * ECHO_ERR_TRUNCATED, not garbage data or a crash.
 */
static void test_truncated_read(void) {
  cleanup_test_dir();

  block_file_manager_t mgr;
  ASSERT_EQ(block_storage_init(&mgr, TEST_DATA_DIR), ECHO_OK);

  /* Write a moderately sized block (4096 bytes) */
  uint32_t size = 4096;
  uint8_t *data = (uint8_t *)malloc(size);
  ASSERT(data != NULL);
  memset(data, 0x55, size);

  block_file_pos_t pos;
  echo_result_t r = block_storage_write(&mgr, data, size, &pos);
  free(data);
  ASSERT_EQ(r, ECHO_OK);

  block_storage_close(&mgr);

  /* Get block file path */
  block_file_manager_t tmp_mgr;
  strcpy(tmp_mgr.data_dir, TEST_DATA_DIR);
  char path[512];
  block_storage_get_path(&tmp_mgr, pos.file_index, path);

  /*
   * Truncate the file to only the header (magic + size) but not the data.
   * The file offset of the record is pos.file_offset.
   * Record header is 8 bytes (magic + size).
   * Truncate to pos.file_offset + 8 + 512 (partial data — shorter than 4096).
   */
  off_t trunc_len = (off_t)pos.file_offset + 8 + 512;
  ASSERT(truncate(path, trunc_len) == 0);

  /* Reinitialize and attempt to read */
  ASSERT_EQ(block_storage_init(&mgr, TEST_DATA_DIR), ECHO_OK);

  uint8_t *read_data = NULL;
  uint32_t read_size = 0;
  r = block_storage_read(&mgr, pos, &read_data, &read_size);

  /* Must return an error (truncated data) — not success with garbage content */
  ASSERT(r != ECHO_OK);
  ASSERT(read_data == NULL);

  block_storage_close(&mgr);
  cleanup_test_dir();
}

/*
 * Test: Block near 4x witness weight limit (TEST-05).
 *
 * Bitcoin SegWit blocks can be up to ECHO_MAX_BLOCK_WEIGHT = 4,000,000 weight
 * units. In the worst case (all witness data), this approaches 4 MB of raw
 * byte storage. The storage layer permits sizes up to ECHO_MAX_BLOCK_SIZE * 4
 * bytes. This test writes and reads a block just under that limit to verify
 * near-boundary handling.
 *
 * Note: ECHO_MAX_BLOCK_SIZE is the legacy 1 MB limit; *4 gives the SegWit
 * weight limit expressed as raw bytes (4,000,000 bytes for max-weight blocks).
 */
static void test_near_4x_witness_limit(void) {
  cleanup_test_dir();

  block_file_manager_t mgr;
  ASSERT_EQ(block_storage_init(&mgr, TEST_DATA_DIR), ECHO_OK);

  /*
   * Use a size just under ECHO_MAX_BLOCK_SIZE * 4 (the upper bound that
   * block_storage_read() allows). We subtract 1 to stay within bounds.
   * This is 3,999,999 bytes — the maximum SegWit-weight storage scenario.
   */
  uint32_t near_max = (uint32_t)(ECHO_MAX_BLOCK_SIZE) * 4U - 1U;

  uint8_t *data = (uint8_t *)malloc(near_max);
  ASSERT(data != NULL);

  memset(data, 0xCD, near_max);

  block_file_pos_t pos;
  echo_result_t r = block_storage_write(&mgr, data, near_max, &pos);
  ASSERT_EQ(r, ECHO_OK);

  /* Read it back */
  uint8_t *read_data = NULL;
  uint32_t read_size = 0;
  r = block_storage_read(&mgr, pos, &read_data, &read_size);
  ASSERT_EQ(r, ECHO_OK);
  ASSERT(read_data != NULL);
  ASSERT_EQ(read_size, near_max);

  /* Spot-check: first, last, and middle bytes */
  ASSERT(read_data[0] == 0xCD);
  ASSERT(read_data[near_max / 2] == 0xCD);
  ASSERT(read_data[near_max - 1] == 0xCD);

  free(data);
  free(read_data);
  block_storage_close(&mgr);
  cleanup_test_dir();
}

/*
 * Main test runner.
 */
int main(void) {
    test_suite_begin("Block Storage Tests");

    test_case("Initialize block storage manager"); test_init(); test_pass();
    test_case("Write single block"); test_write_single_block(); test_pass();
    test_case("Read block"); test_read_block(); test_pass();
    test_case("Write multiple blocks"); test_write_multiple_blocks(); test_pass();
    test_case("Resume after restart"); test_resume_after_restart(); test_pass();
    test_case("Get file path"); test_get_path(); test_pass();
    test_case("Read nonexistent block"); test_read_nonexistent(); test_pass();
    test_case("Null parameters"); test_null_params(); test_pass();
    test_case("Large block handling"); test_large_block(); test_pass();

    test_section("Concurrent Storage Tests");
    test_case("4-thread concurrent writes no corruption"); test_concurrent_writes(); test_pass();
    test_case("4-thread concurrent reads correct data"); test_concurrent_reads(); test_pass();
    test_case("Mixed concurrent read/write no deadlock"); test_mixed_concurrent_rw(); test_pass();

    test_section("Large Block and Edge Case Tests");
    test_case("Block at exact ECHO_MAX_BLOCK_SIZE"); test_max_size_block(); test_pass();
    test_case("Corrupted size field detected"); test_corrupted_size_field(); test_pass();
    test_case("Truncated read returns error"); test_truncated_read(); test_pass();
    test_case("Near 4x witness weight limit handled"); test_near_4x_witness_limit(); test_pass();

    test_suite_end();
    return test_global_summary();
}
