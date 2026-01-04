/*
 * Bitcoin Echo — Block File Storage
 *
 * File-per-block storage for batch IBD architecture.
 *
 * Each block stored as individual file in subdirectories:
 *   blocks/{height/1000}/{height}.blk
 *
 * Example:
 *   height 875432 -> blocks/875/000875432.blk
 *   height 1000   -> blocks/1/000001000.blk
 *   height 0      -> blocks/0/000000000.blk
 *
 * Benefits:
 *   - Trivial pruning via unlink()
 *   - Natural handling of out-of-order downloads
 *   - Fast directory operations (~1000 files per subdir)
 *   - Simple restart recovery (scan subdirectories)
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_BLOCKS_STORAGE_H
#define ECHO_BLOCKS_STORAGE_H

#include "echo_types.h"
#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * FILE-PER-BLOCK STORAGE API
 *
 * Each block is stored as an individual file named by height.
 * Organized into subdirectories of 1000 blocks each for filesystem efficiency.
 * ============================================================================
 */

/*
 * Block storage manager for file-per-block storage.
 * Minimal state - just tracks the data directory path.
 */
typedef struct {
  char data_dir[256]; /* Data directory path (blocks/ is appended) */
} block_storage_t;

/*
 * Initialize file-per-block storage.
 *
 * Parameters:
 *   storage  - Storage handle to initialize
 *   data_dir - Path to data directory (blocks/ will be created inside)
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Creates blocks/ directory if it doesn't exist
 */
echo_result_t block_storage_create(block_storage_t *storage,
                                   const char *data_dir);

/*
 * Write a block to storage by height.
 *
 * Parameters:
 *   storage    - Storage handle
 *   height     - Block height (determines filename and subdirectory)
 *   block_data - Raw block data (header + transactions)
 *   block_size - Size of block in bytes
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Creates subdirectory if it doesn't exist: blocks/{height/1000}/
 *   - Creates file: blocks/{height/1000}/{height:09d}.blk
 *   - Overwrites if file already exists
 *   - File contains raw block data only (no prefix)
 */
echo_result_t block_storage_write_height(block_storage_t *storage,
                                         uint32_t height,
                                         const uint8_t *block_data,
                                         uint32_t block_size);

/*
 * Read a block from storage by height.
 *
 * Parameters:
 *   storage    - Storage handle
 *   height     - Block height
 *   block_out  - Output: pointer to allocated buffer with block data
 *   size_out   - Output: size of block in bytes
 *
 * Returns:
 *   ECHO_OK on success, ECHO_ERR_NOT_FOUND if block doesn't exist
 *
 * Notes:
 *   - Allocates buffer with malloc(); caller must free()
 *   - Returns raw block data
 */
echo_result_t block_storage_read_height(block_storage_t *storage,
                                        uint32_t height, uint8_t **block_out,
                                        uint32_t *size_out);

/*
 * Prune (delete) a block from storage.
 *
 * Parameters:
 *   storage - Storage handle
 *   height  - Block height to delete
 *
 * Returns:
 *   ECHO_OK on success (including if file didn't exist - idempotent)
 *
 * Notes:
 *   - Simply unlinks the file
 *   - Does not remove empty subdirectories (minor cleanup debt)
 */
echo_result_t block_storage_prune_height(block_storage_t *storage,
                                         uint32_t height);

/*
 * Check if a block exists in storage.
 *
 * Parameters:
 *   storage - Storage handle
 *   height  - Block height to check
 *
 * Returns:
 *   true if block file exists, false otherwise
 *
 * Notes:
 *   - Uses stat() - lightweight existence check
 */
bool block_storage_exists_height(const block_storage_t *storage,
                                 uint32_t height);

/*
 * Scan storage and return all stored block heights.
 *
 * Parameters:
 *   storage     - Storage handle
 *   heights_out - Output: array of heights (caller must free)
 *   count_out   - Output: number of heights
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Allocates heights_out with malloc(); caller must free()
 *   - Returns heights in ascending order
 *   - Scans all subdirectories under blocks/
 */
echo_result_t block_storage_scan_heights(const block_storage_t *storage,
                                         uint32_t **heights_out,
                                         size_t *count_out);

/*
 * Get total disk usage of all block files.
 *
 * Parameters:
 *   storage    - Storage handle
 *   total_size - Output: total size of all block files in bytes
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 *
 * Notes:
 *   - Sums size of all .blk files across all subdirectories
 */
echo_result_t block_storage_get_total_size(const block_storage_t *storage,
                                           uint64_t *total_size);

/*
 * Prune blocks in a height range.
 *
 * Parameters:
 *   storage      - Storage handle
 *   start_height - First height to prune (inclusive)
 *   end_height   - Last height to prune (inclusive)
 *
 * Returns:
 *   Number of blocks actually deleted
 *
 * Notes:
 *   - Iterates through range, calling prune_height for each
 *   - Skips non-existent files (idempotent)
 */
uint32_t block_storage_prune_range(block_storage_t *storage,
                                   uint32_t start_height, uint32_t end_height);

/*
 * Close/destroy block storage.
 *
 * Parameters:
 *   storage - Storage handle
 *
 * Notes:
 *   - No-op for file-per-block storage (no open handles)
 *   - Clears the structure
 */
void block_storage_destroy(block_storage_t *storage);

/*
 * Get the path for a block file given its height.
 *
 * Parameters:
 *   storage  - Storage handle
 *   height   - Block height
 *   path_out - Output buffer for path (at least 512 bytes)
 *
 * Notes:
 *   - Format: {data_dir}/blocks/{height/1000}/{height:09d}.blk
 *   - Helper function exposed for testing/debugging
 */
void block_storage_get_height_path(const block_storage_t *storage,
                                   uint32_t height, char *path_out);

#endif /* ECHO_BLOCKS_STORAGE_H */
