/*
 * Bitcoin Echo — Block File Storage Implementation
 *
 * File-per-block storage for IBD decoupled architecture.
 *
 * Build once. Build right. Stop.
 */

#include "blocks_storage.h"
#include "echo_config.h"
#include "echo_types.h"
#include "log.h"
#include "platform.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ============================================================================
 * FILE-PER-BLOCK STORAGE IMPLEMENTATION
 * ============================================================================
 */

/*
 * Get the path for a block file given its height.
 * Format: {data_dir}/blocks/{height}.blk
 */
void block_storage_get_height_path(const block_storage_t *storage,
                                   uint32_t height, char *path_out) {
  snprintf(path_out, 512, "%s/%s/%u.blk", storage->data_dir, ECHO_BLOCKS_DIR,
           height);
}

/*
 * Initialize file-per-block storage.
 */
echo_result_t block_storage_create(block_storage_t *storage,
                                   const char *data_dir) {
  if (storage == NULL || data_dir == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Copy data directory path */
  size_t len = strlen(data_dir);
  if (len >= sizeof(storage->data_dir)) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }
  memcpy(storage->data_dir, data_dir, len + 1);

  /* Create blocks directory */
  char blocks_dir[512];
  snprintf(blocks_dir, sizeof(blocks_dir), "%s/%s", data_dir, ECHO_BLOCKS_DIR);

  if (plat_dir_create(blocks_dir) != PLAT_OK) {
    log_error(LOG_COMP_STORE, "Failed to create blocks directory: %s",
              blocks_dir);
    return ECHO_ERR_PLATFORM_IO;
  }

  log_debug(LOG_COMP_STORE, "Initialized file-per-block storage at %s",
            blocks_dir);
  return ECHO_OK;
}

/*
 * Write a block to storage by height.
 */
echo_result_t block_storage_write_height(block_storage_t *storage,
                                         uint32_t height,
                                         const uint8_t *block_data,
                                         uint32_t block_size) {
  if (storage == NULL || block_data == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (block_size == 0) {
    return ECHO_ERR_INVALID_PARAM;
  }

  /* Get file path */
  char path[512];
  block_storage_get_height_path(storage, height, path);

  /* Open file for writing (creates if doesn't exist, truncates if exists) */
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    log_error(LOG_COMP_STORE, "Failed to open block file for writing: %s",
              path);
    return ECHO_ERR_PLATFORM_IO;
  }

  /* Write raw block data */
  size_t written = fwrite(block_data, 1, block_size, f);
  if (written != block_size) {
    log_error(LOG_COMP_STORE,
              "Failed to write block %u: wrote %zu of %u bytes", height,
              written, block_size);
    fclose(f);
    /* Try to remove the partial file */
    unlink(path);
    return ECHO_ERR_PLATFORM_IO;
  }

  /* Flush and close */
  if (fflush(f) != 0) {
    log_error(LOG_COMP_STORE, "Failed to flush block %u", height);
    fclose(f);
    return ECHO_ERR_PLATFORM_IO;
  }

  fclose(f);
  return ECHO_OK;
}

/*
 * Read a block from storage by height.
 */
echo_result_t block_storage_read_height(block_storage_t *storage,
                                        uint32_t height, uint8_t **block_out,
                                        uint32_t *size_out) {
  if (storage == NULL || block_out == NULL || size_out == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Initialize outputs */
  *block_out = NULL;
  *size_out = 0;

  /* Get file path */
  char path[512];
  block_storage_get_height_path(storage, height, path);

  /* Open file for reading */
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Get file size */
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return ECHO_ERR_PLATFORM_IO;
  }

  long file_size = ftell(f);
  if (file_size <= 0 || file_size > (long)ECHO_MAX_BLOCK_SIZE * 4) {
    fclose(f);
    return ECHO_ERR_INVALID_FORMAT;
  }

  /* Seek back to start */
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return ECHO_ERR_PLATFORM_IO;
  }

  /* Allocate buffer */
  uint8_t *block = (uint8_t *)malloc((size_t)file_size);
  if (block == NULL) {
    fclose(f);
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /* Read entire file */
  size_t bytes_read = fread(block, 1, (size_t)file_size, f);
  fclose(f);

  if (bytes_read != (size_t)file_size) {
    free(block);
    return ECHO_ERR_TRUNCATED;
  }

  *block_out = block;
  *size_out = (uint32_t)file_size;
  return ECHO_OK;
}

/*
 * Prune (delete) a block from storage.
 */
echo_result_t block_storage_prune_height(block_storage_t *storage,
                                         uint32_t height) {
  if (storage == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Get file path */
  char path[512];
  block_storage_get_height_path(storage, height, path);

  /* Delete the file (ignore ENOENT - file doesn't exist is OK) */
  if (unlink(path) != 0) {
    /* Check if file didn't exist (that's fine, idempotent) */
    if (!plat_file_exists(path)) {
      return ECHO_OK;
    }
    log_error(LOG_COMP_STORE, "Failed to prune block %u", height);
    return ECHO_ERR_PLATFORM_IO;
  }

  log_debug(LOG_COMP_STORE, "Pruned block %u", height);
  return ECHO_OK;
}

/*
 * Check if a block exists in storage.
 */
bool block_storage_exists_height(const block_storage_t *storage,
                                 uint32_t height) {
  if (storage == NULL) {
    return false;
  }

  char path[512];
  block_storage_get_height_path(storage, height, path);
  return plat_file_exists(path);
}

/*
 * Comparison function for qsort (ascending order).
 */
static int compare_heights(const void *a, const void *b) {
  uint32_t ha = *(const uint32_t *)a;
  uint32_t hb = *(const uint32_t *)b;
  if (ha < hb)
    return -1;
  if (ha > hb)
    return 1;
  return 0;
}

/*
 * Check if filename matches {digits}.blk pattern.
 * Returns the parsed height on success, UINT32_MAX on failure.
 */
static uint32_t parse_block_filename(const char *name) {
  size_t len = strlen(name);

  /* Must be at least "0.blk" (5 chars) and end with ".blk" */
  if (len < 5 || strcmp(name + len - 4, ".blk") != 0) {
    return UINT32_MAX;
  }

  /* Check all chars before .blk are digits */
  size_t digit_len = len - 4;
  for (size_t i = 0; i < digit_len; i++) {
    if (name[i] < '0' || name[i] > '9') {
      return UINT32_MAX;
    }
  }

  /* Parse height */
  char *endptr;
  unsigned long height = strtoul(name, &endptr, 10);

  /* Ensure parsing stopped at .blk */
  if (endptr != name + digit_len) {
    return UINT32_MAX;
  }

  /* Check for overflow */
  if (height > UINT32_MAX) {
    return UINT32_MAX;
  }

  return (uint32_t)height;
}

/*
 * Scan storage and return all stored block heights.
 */
echo_result_t block_storage_scan_heights(const block_storage_t *storage,
                                         uint32_t **heights_out,
                                         size_t *count_out) {
  if (storage == NULL || heights_out == NULL || count_out == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  *heights_out = NULL;
  *count_out = 0;

  /* Build blocks directory path */
  char blocks_dir[512];
  snprintf(blocks_dir, sizeof(blocks_dir), "%s/%s", storage->data_dir,
           ECHO_BLOCKS_DIR);

  /* Open directory */
  DIR *dir = opendir(blocks_dir);
  if (dir == NULL) {
    /* Directory doesn't exist = no blocks, return empty */
    return ECHO_OK;
  }

  /* First pass: count matching files */
  size_t count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (parse_block_filename(entry->d_name) != UINT32_MAX) {
      count++;
    }
  }

  /* Empty directory */
  if (count == 0) {
    closedir(dir);
    return ECHO_OK;
  }

  /* Allocate array */
  uint32_t *heights = (uint32_t *)malloc(count * sizeof(uint32_t));
  if (heights == NULL) {
    closedir(dir);
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /* Second pass: collect heights */
  rewinddir(dir);
  size_t idx = 0;
  while ((entry = readdir(dir)) != NULL && idx < count) {
    uint32_t height = parse_block_filename(entry->d_name);
    if (height != UINT32_MAX) {
      heights[idx++] = height;
    }
  }

  closedir(dir);

  /* Sort heights in ascending order */
  qsort(heights, idx, sizeof(uint32_t), compare_heights);

  *heights_out = heights;
  *count_out = idx;

  log_debug(LOG_COMP_STORE, "Scanned %zu block files in storage", idx);
  return ECHO_OK;
}

/*
 * Get total disk usage of all block files.
 */
echo_result_t block_storage_get_total_size(const block_storage_t *storage,
                                              uint64_t *total_size) {
  if (storage == NULL || total_size == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  *total_size = 0;

  /* Build blocks directory path */
  char blocks_dir[512];
  snprintf(blocks_dir, sizeof(blocks_dir), "%s/%s", storage->data_dir,
           ECHO_BLOCKS_DIR);

  /* Open directory */
  DIR *dir = opendir(blocks_dir);
  if (dir == NULL) {
    return ECHO_OK; /* No directory = 0 bytes */
  }

  struct dirent *entry;
  uint64_t total = 0;

  while ((entry = readdir(dir)) != NULL) {
    /* Skip non-block files */
    if (parse_block_filename(entry->d_name) == UINT32_MAX) {
      continue;
    }

    /* Build full path and get file size */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", blocks_dir, entry->d_name);

    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
      total += (uint64_t)st.st_size;
    }
  }

  closedir(dir);
  *total_size = total;
  return ECHO_OK;
}

/*
 * Close/destroy block storage.
 */
void block_storage_destroy(block_storage_t *storage) {
  if (storage == NULL) {
    return;
  }
  /* No-op for file-per-block storage - no open handles to close */
  memset(storage, 0, sizeof(*storage));
}
