/*
 * Bitcoin Echo — Block File Storage
 *
 * Manages persistent storage of raw blocks in append-only files.
 * Format is compatible with Bitcoin Core's blk*.dat files:
 *   - Network magic (4 bytes)
 *   - Block size (4 bytes, little-endian)
 *   - Raw block data (header + transactions)
 *
 * Files are named blk00000.dat, blk00001.dat, etc., and stored in the
 * blocks/ directory. Each file grows to ~128 MB before a new file is created.
 *
 * Block positions are tracked externally (in the block index database).
 * This module only handles raw block storage and retrieval.
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_BLOCKS_STORAGE_H
#define ECHO_BLOCKS_STORAGE_H

#include "echo_types.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Block file position.
 * Identifies a block's location on disk.
 */
typedef struct {
  uint32_t file_index;  /* File number (0 = blk00000.dat, 1 = blk00001.dat) */
  uint32_t file_offset; /* Byte offset within file to start of magic bytes */
} block_file_pos_t;

/*
 * Read file cache size.
 * During IBD, blocks arrive out of order, so sequential height confirmation
 * jumps between files. Caching multiple file handles avoids open/close overhead.
 */
#define BLOCK_READ_CACHE_SIZE 8

/*
 * Block file manager.
 * Manages writing and reading blocks to/from disk.
 *
 * IBD optimization: keeps current write file open to avoid
 * per-block fopen/fclose syscall overhead. Also caches read file
 * handles for faster block retrieval during confirmation.
 */
typedef struct {
  char data_dir[256];           /* Data directory path */
  uint32_t current_file_index;  /* Current file being written */
  uint32_t current_file_offset; /* Current offset in current file */
  void *current_file;           /* Current write file handle (FILE*) */
  uint32_t blocks_since_flush;  /* Blocks written since last flush */
  uint64_t cached_total_size;   /* Cached total disk usage (updated incrementally) */

  /* Read file cache: avoids fopen/fclose per block during IBD confirmation */
  struct {
    uint32_t file_index;        /* File index, or UINT32_MAX if slot empty */
    void *file_handle;          /* FILE* handle */
    uint64_t last_access;       /* Access counter for LRU eviction */
  } read_cache[BLOCK_READ_CACHE_SIZE];
  uint64_t read_cache_access_counter; /* Monotonic counter for LRU */

  pthread_mutex_t mutex; /* Protects all mutable state for thread safety */
} block_file_manager_t;

/*
 * Number of blocks between flushes.
 * Balances durability vs. performance during IBD.
 */
#define BLOCK_STORAGE_FLUSH_INTERVAL 100

/*
 * Maximum size of a single block file (128 MB).
 * When a file reaches this size, we start a new file.
 */
#define BLOCK_FILE_MAX_SIZE (128 * 1024 * 1024)

/*
 * Block file record header size (magic + size).
 */
#define BLOCK_FILE_RECORD_HEADER_SIZE 8

/*
 * Initialize block file manager.
 *
 * Parameters:
 *   mgr      - Manager to initialize
 *   data_dir - Path to data directory (blocks/ will be created inside)
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Creates blocks/ directory if it doesn't exist
 *   - Scans existing block files to find current write position
 */
echo_result_t block_storage_init(block_file_manager_t *mgr,
                                 const char *data_dir);

/*
 * Write a block to disk.
 *
 * Parameters:
 *   mgr       - Block file manager
 *   block_data - Raw block data (header + transactions)
 *   block_size - Size of block in bytes
 *   pos_out   - Output: position where block was written
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Writes network magic + size prefix + block data
 *   - Automatically creates new file if current file would exceed max size
 *   - Position is returned for storage in block index
 */
echo_result_t block_storage_write(block_file_manager_t *mgr,
                                  const uint8_t *block_data,
                                  uint32_t block_size,
                                  block_file_pos_t *pos_out);

/*
 * Read a block from disk.
 *
 * Parameters:
 *   mgr        - Block file manager
 *   pos        - Position of block (from block index)
 *   block_out  - Output: pointer to allocated buffer with block data
 *   size_out   - Output: size of block in bytes
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Allocates buffer with malloc(); caller must free()
 *   - Verifies magic bytes match network
 *   - Returns raw block data (without magic/size prefix)
 */
echo_result_t block_storage_read(block_file_manager_t *mgr,
                                 block_file_pos_t pos, uint8_t **block_out,
                                 uint32_t *size_out);

/*
 * Get path to block file.
 *
 * Parameters:
 *   mgr        - Block file manager
 *   file_index - File index (0 = blk00000.dat, etc.)
 *   path_out   - Output buffer for path (at least 512 bytes)
 *
 * Notes:
 *   - Helper function to construct block file paths
 */
void block_storage_get_path(const block_file_manager_t *mgr,
                            uint32_t file_index, char *path_out);

/*
 * ============================================================================
 * PRUNING OPERATIONS
 * ============================================================================
 */

/*
 * Delete a block file.
 *
 * Parameters:
 *   mgr        - Block file manager
 *   file_index - Index of file to delete
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - File must exist
 *   - Used during pruning to reclaim disk space
 *   - Cannot delete the current write file
 */
echo_result_t block_storage_delete_file(block_file_manager_t *mgr,
                                        uint32_t file_index);

/*
 * Check if a block file exists.
 *
 * Parameters:
 *   mgr        - Block file manager
 *   file_index - Index of file to check
 *   exists     - Output: true if file exists, false otherwise
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t block_storage_file_exists(const block_file_manager_t *mgr,
                                        uint32_t file_index, bool *exists);

/*
 * Get the size of a block file.
 *
 * Parameters:
 *   mgr        - Block file manager
 *   file_index - Index of file to query
 *   size       - Output: file size in bytes (0 if file doesn't exist)
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t block_storage_get_file_size(const block_file_manager_t *mgr,
                                          uint32_t file_index, uint64_t *size);

/*
 * Get total disk usage of all block files.
 *
 * Parameters:
 *   mgr        - Block file manager
 *   total_size - Output: total size of all block files in bytes
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Sums size of all blk*.dat files
 *   - Used to check if pruning is needed
 */
echo_result_t block_storage_get_total_size(const block_file_manager_t *mgr,
                                           uint64_t *total_size);

/*
 * Get the current write file index.
 *
 * Parameters:
 *   mgr - Block file manager
 *
 * Returns:
 *   Index of the current file being written to
 */
uint32_t block_storage_get_current_file(const block_file_manager_t *mgr);

/*
 * Get the lowest file index (for pruning scan).
 *
 * Parameters:
 *   mgr        - Block file manager
 *   file_index - Output: lowest file index with existing file
 *
 * Returns:
 *   ECHO_OK if found, ECHO_ERR_NOT_FOUND if no files exist
 */
echo_result_t block_storage_get_lowest_file(const block_file_manager_t *mgr,
                                            uint32_t *file_index);

/*
 * ============================================================================
 * BATCHING OPERATIONS (IBD optimization)
 * ============================================================================
 */

/*
 * Flush buffered writes to disk.
 *
 * Parameters:
 *   mgr - Block file manager
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Called automatically every BLOCK_STORAGE_FLUSH_INTERVAL blocks
 *   - Can be called manually for durability at checkpoints
 */
echo_result_t block_storage_flush(block_file_manager_t *mgr);

/*
 * Close the block storage manager.
 *
 * Parameters:
 *   mgr - Block file manager
 *
 * Notes:
 *   - Flushes any pending writes
 *   - Closes the current write file
 *   - Must be called during shutdown for clean state
 */
void block_storage_close(block_file_manager_t *mgr);

#endif /* ECHO_BLOCKS_STORAGE_H */
