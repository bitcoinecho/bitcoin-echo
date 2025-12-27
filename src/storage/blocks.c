/*
 * Bitcoin Echo — Block File Storage Implementation
 *
 * Append-only block files compatible with Bitcoin Core format.
 *
 * Build once. Build right. Stop.
 */

#include "blocks_storage.h"
#include "echo_config.h"
#include "echo_types.h"
#include "platform.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Scan existing block files to find the current write position.
 * This is called during initialization to resume writing where we left off.
 * Handles gaps from pruned files (e.g., blk00000-blk00005 deleted).
 */
static echo_result_t scan_block_files(block_file_manager_t *mgr) {
  uint32_t file_index = 0;
  uint32_t highest_found = UINT32_MAX; /* Track highest existing file */
  uint32_t consecutive_missing = 0;
  char path[512];

  /*
   * Find the highest numbered block file, accounting for gaps from pruning.
   * Stop scanning after 1000 consecutive missing files (arbitrary but safe).
   */
  while (consecutive_missing < 1000) {
    block_storage_get_path(mgr, file_index, path);

    if (plat_file_exists(path)) {
      highest_found = file_index;
      consecutive_missing = 0;
    } else {
      consecutive_missing++;
    }

    file_index++;

    /* Sanity check: don't scan more than 100,000 files */
    if (file_index > 100000) {
      break;
    }
  }

  /* If no files found, start fresh */
  if (highest_found == UINT32_MAX) {
    mgr->current_file_index = 0;
    mgr->current_file_offset = 0;
    return ECHO_OK;
  }

  file_index = highest_found;

  /* Open the last file and find its size */
  block_storage_get_path(mgr, file_index, path);

  FILE *f = fopen(path, "rb");
  if (!f) {
    return ECHO_ERR_PLATFORM_IO;
  }

  /* Seek to end to get file size */
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return ECHO_ERR_PLATFORM_IO;
  }

  long size = ftell(f);
  fclose(f);

  if (size < 0 || size > (long)BLOCK_FILE_MAX_SIZE) {
    return ECHO_ERR_PLATFORM_IO;
  }

  /* If the last file is at max size, start a new file */
  if (size >= (long)BLOCK_FILE_MAX_SIZE) {
    mgr->current_file_index = file_index + 1;
    mgr->current_file_offset = 0;
  } else {
    mgr->current_file_index = file_index;
    mgr->current_file_offset = (uint32_t)size;
  }

  return ECHO_OK;
}

/*
 * Initialize block file manager.
 */
echo_result_t block_storage_init(block_file_manager_t *mgr,
                                 const char *data_dir) {
  if (!mgr || !data_dir) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Copy data directory path */
  size_t len = strlen(data_dir);
  if (len >= sizeof(mgr->data_dir)) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }
  memcpy(mgr->data_dir, data_dir, len + 1);

  /* Create blocks directory */
  char blocks_dir[512];
  snprintf(blocks_dir, sizeof(blocks_dir), "%s/%s", data_dir, ECHO_BLOCKS_DIR);

  if (plat_dir_create(blocks_dir) != PLAT_OK) {
    return ECHO_ERR_PLATFORM_IO;
  }

  /* Scan existing files to find current position */
  return scan_block_files(mgr);
}

/*
 * Get path to block file.
 */
void block_storage_get_path(const block_file_manager_t *mgr,
                            uint32_t file_index, char *path_out) {
  snprintf(path_out, 512, "%s/%s/blk%05u.dat", mgr->data_dir, ECHO_BLOCKS_DIR,
           file_index);
}

/*
 * Write a block to disk.
 */
echo_result_t block_storage_write(block_file_manager_t *mgr,
                                  const uint8_t *block_data,
                                  uint32_t block_size,
                                  block_file_pos_t *pos_out) {
  if (!mgr || !block_data || !pos_out) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Check if we need to start a new file */
  uint32_t record_size = BLOCK_FILE_RECORD_HEADER_SIZE + block_size;

  if (mgr->current_file_offset + record_size > BLOCK_FILE_MAX_SIZE) {
    /* Start new file */
    mgr->current_file_index++;
    mgr->current_file_offset = 0;
  }

  /* Get path to current file */
  char path[512];
  block_storage_get_path(mgr, mgr->current_file_index, path);

  /* Open file for appending */
  FILE *f = fopen(path, "ab");
  if (!f) {
    return ECHO_ERR_PLATFORM_IO;
  }

  /* Write magic bytes (little-endian) */
  uint32_t magic = ECHO_NETWORK_MAGIC;
  uint8_t magic_bytes[4] = {(magic >> 0) & 0xFF, (magic >> 8) & 0xFF,
                            (magic >> 16) & 0xFF, (magic >> 24) & 0xFF};

  if (fwrite(magic_bytes, 1, 4, f) != 4) {
    fclose(f);
    return ECHO_ERR_PLATFORM_IO;
  }

  /* Write block size (little-endian) */
  uint8_t size_bytes[4] = {(block_size >> 0) & 0xFF, (block_size >> 8) & 0xFF,
                           (block_size >> 16) & 0xFF,
                           (block_size >> 24) & 0xFF};

  if (fwrite(size_bytes, 1, 4, f) != 4) {
    fclose(f);
    return ECHO_ERR_PLATFORM_IO;
  }

  /* Write block data */
  if (fwrite(block_data, 1, block_size, f) != block_size) {
    fclose(f);
    return ECHO_ERR_PLATFORM_IO;
  }

  /* Flush to disk */
  if (fflush(f) != 0) {
    fclose(f);
    return ECHO_ERR_PLATFORM_IO;
  }

  fclose(f);

  /* Return position */
  pos_out->file_index = mgr->current_file_index;
  pos_out->file_offset = mgr->current_file_offset;

  /* Update current position */
  mgr->current_file_offset += record_size;

  return ECHO_OK;
}

/*
 * Read a block from disk.
 */
echo_result_t block_storage_read(block_file_manager_t *mgr,
                                 block_file_pos_t pos, uint8_t **block_out,
                                 uint32_t *size_out) {
  if (!mgr || !block_out || !size_out) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Initialize outputs */
  *block_out = NULL;
  *size_out = 0;

  /* Get path to file */
  char path[512];
  block_storage_get_path(mgr, pos.file_index, path);

  /* Open file for reading */
  FILE *f = fopen(path, "rb");
  if (!f) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Seek to position */
  if (fseek(f, pos.file_offset, SEEK_SET) != 0) {
    fclose(f);
    return ECHO_ERR_PLATFORM_IO;
  }

  /* Read and verify magic bytes */
  uint8_t magic_bytes[4];
  if (fread(magic_bytes, 1, 4, f) != 4) {
    fclose(f);
    return ECHO_ERR_TRUNCATED;
  }

  uint32_t magic =
      ((uint32_t)magic_bytes[0] << 0) | ((uint32_t)magic_bytes[1] << 8) |
      ((uint32_t)magic_bytes[2] << 16) | ((uint32_t)magic_bytes[3] << 24);

  if (magic != ECHO_NETWORK_MAGIC) {
    fclose(f);
    return ECHO_ERR_INVALID_FORMAT;
  }

  /* Read block size */
  uint8_t size_bytes[4];
  if (fread(size_bytes, 1, 4, f) != 4) {
    fclose(f);
    return ECHO_ERR_TRUNCATED;
  }

  uint32_t block_size =
      ((uint32_t)size_bytes[0] << 0) | ((uint32_t)size_bytes[1] << 8) |
      ((uint32_t)size_bytes[2] << 16) | ((uint32_t)size_bytes[3] << 24);

  /* Sanity check block size */
  if (block_size == 0 || block_size > ECHO_MAX_BLOCK_SIZE * 4) {
    fclose(f);
    return ECHO_ERR_INVALID_FORMAT;
  }

  /* Allocate buffer for block */
  uint8_t *block = (uint8_t *)malloc(block_size);
  if (!block) {
    fclose(f);
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /* Read block data */
  if (fread(block, 1, block_size, f) != block_size) {
    free(block);
    fclose(f);
    return ECHO_ERR_TRUNCATED;
  }

  fclose(f);

  /* Success */
  *block_out = block;
  *size_out = block_size;
  return ECHO_OK;
}

/*
 * ============================================================================
 * PRUNING OPERATIONS
 * ============================================================================
 */

/*
 * Delete a block file.
 */
echo_result_t block_storage_delete_file(block_file_manager_t *mgr,
                                        uint32_t file_index) {
  if (mgr == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Cannot delete the current write file */
  if (file_index >= mgr->current_file_index) {
    return ECHO_ERR_INVALID_PARAM;
  }

  char path[512];
  block_storage_get_path(mgr, file_index, path);

  /* Check if file exists */
  if (!plat_file_exists(path)) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Delete the file */
  if (remove(path) != 0) {
    return ECHO_ERR_PLATFORM_IO;
  }

  return ECHO_OK;
}

/*
 * Check if a block file exists.
 */
echo_result_t block_storage_file_exists(const block_file_manager_t *mgr,
                                        uint32_t file_index, bool *exists) {
  if (mgr == NULL || exists == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  char path[512];
  block_storage_get_path(mgr, file_index, path);

  *exists = plat_file_exists(path);
  return ECHO_OK;
}

/*
 * Get the size of a block file.
 */
echo_result_t block_storage_get_file_size(const block_file_manager_t *mgr,
                                          uint32_t file_index, uint64_t *size) {
  if (mgr == NULL || size == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  char path[512];
  block_storage_get_path(mgr, file_index, path);

  /* Check if file exists */
  if (!plat_file_exists(path)) {
    *size = 0;
    return ECHO_OK;
  }

  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    *size = 0;
    return ECHO_OK;
  }

  /* Seek to end to get file size */
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return ECHO_ERR_PLATFORM_IO;
  }

  long file_size = ftell(f);
  fclose(f);

  if (file_size < 0) {
    return ECHO_ERR_PLATFORM_IO;
  }

  *size = (uint64_t)file_size;
  return ECHO_OK;
}

/*
 * Get total disk usage of all block files.
 * Handles gaps from pruning (e.g., blk00000 deleted but blk00001+ exist).
 */
echo_result_t block_storage_get_total_size(const block_file_manager_t *mgr,
                                           uint64_t *total_size) {
  if (mgr == NULL || total_size == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  uint64_t total = 0;
  char path[512];

  /* Iterate from 0 to current file, summing existing files */
  for (uint32_t file_index = 0; file_index <= mgr->current_file_index;
       file_index++) {
    block_storage_get_path(mgr, file_index, path);

    if (!plat_file_exists(path)) {
      continue; /* File was pruned, skip it */
    }

    uint64_t file_size = 0;
    echo_result_t result =
        block_storage_get_file_size(mgr, file_index, &file_size);
    if (result != ECHO_OK) {
      continue; /* Skip files we can't read */
    }

    total += file_size;
  }

  *total_size = total;
  return ECHO_OK;
}

/*
 * Get the current write file index.
 */
uint32_t block_storage_get_current_file(const block_file_manager_t *mgr) {
  if (mgr == NULL) {
    return 0;
  }
  return mgr->current_file_index;
}

/*
 * Get the lowest file index (for pruning scan).
 */
echo_result_t block_storage_get_lowest_file(const block_file_manager_t *mgr,
                                            uint32_t *file_index) {
  if (mgr == NULL || file_index == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  char path[512];

  /* Scan from 0 to find the first existing file */
  for (uint32_t i = 0; i <= mgr->current_file_index; i++) {
    block_storage_get_path(mgr, i, path);

    if (plat_file_exists(path)) {
      *file_index = i;
      return ECHO_OK;
    }
  }

  return ECHO_ERR_NOT_FOUND;
}
