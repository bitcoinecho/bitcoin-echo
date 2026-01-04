/*
 * Bitcoin Echo — Block File Storage Implementation
 *
 * File-per-block storage with subdirectory organization for batch IBD.
 *
 * Directory structure:
 *   blocks/0/000000000.blk ... blocks/0/000000999.blk   (heights 0-999)
 *   blocks/1/000001000.blk ... blocks/1/000001999.blk   (heights 1000-1999)
 *   ...
 *   blocks/930/000930000.blk ...                         (heights 930000+)
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
 * PATH UTILITIES
 * ============================================================================
 */

/*
 * Get the subdirectory number for a given height.
 * Each subdirectory holds 1000 blocks.
 */
static uint32_t get_subdir(uint32_t height) { return height / 1000; }

/*
 * Get the path for a block file given its height.
 * Format: {data_dir}/blocks/{subdir}/{height:09d}.blk
 */
void block_storage_get_height_path(const block_storage_t *storage,
                                   uint32_t height, char *path_out) {
  uint32_t subdir = get_subdir(height);
  snprintf(path_out, 512, "%s/%s/%u/%09u.blk", storage->data_dir,
           ECHO_BLOCKS_DIR, subdir, height);
}

/*
 * Get the subdirectory path for a given height.
 */
static void get_subdir_path(const block_storage_t *storage, uint32_t height,
                            char *path_out) {
  uint32_t subdir = get_subdir(height);
  snprintf(path_out, 512, "%s/%s/%u", storage->data_dir, ECHO_BLOCKS_DIR,
           subdir);
}

/*
 * Ensure subdirectory exists for a given height.
 */
static echo_result_t ensure_subdir(const block_storage_t *storage,
                                   uint32_t height) {
  char subdir_path[512];
  get_subdir_path(storage, height, subdir_path);

  if (plat_dir_create(subdir_path) != PLAT_OK) {
    log_error(LOG_COMP_STORE, "Failed to create subdirectory: %s", subdir_path);
    return ECHO_ERR_PLATFORM_IO;
  }

  return ECHO_OK;
}

/* ============================================================================
 * STORAGE LIFECYCLE
 * ============================================================================
 */

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

  log_debug(LOG_COMP_STORE,
            "Initialized file-per-block storage with subdirs at %s",
            blocks_dir);
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

/* ============================================================================
 * BLOCK I/O
 * ============================================================================
 */

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

  /* Ensure subdirectory exists */
  echo_result_t result = ensure_subdir(storage, height);
  if (result != ECHO_OK) {
    return result;
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

  return ECHO_OK;
}

/*
 * Prune blocks in a height range.
 */
uint32_t block_storage_prune_range(block_storage_t *storage,
                                   uint32_t start_height, uint32_t end_height) {
  if (storage == NULL || start_height > end_height) {
    return 0;
  }

  uint32_t deleted = 0;

  for (uint32_t height = start_height; height <= end_height; height++) {
    if (block_storage_exists_height(storage, height)) {
      if (block_storage_prune_height(storage, height) == ECHO_OK) {
        deleted++;
      }
    }
  }

  if (deleted > 0) {
    log_debug(LOG_COMP_STORE, "Pruned %u blocks in range %u-%u", deleted,
              start_height, end_height);
  }

  return deleted;
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

/* ============================================================================
 * DIRECTORY SCANNING
 * ============================================================================
 */

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

  /* Must end with ".blk" */
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
 * Scans all subdirectories under blocks/.
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

  /* Open main blocks directory */
  DIR *main_dir = opendir(blocks_dir);
  if (main_dir == NULL) {
    /* Directory doesn't exist = no blocks, return empty */
    return ECHO_OK;
  }

  /* First pass: count all block files across all subdirs */
  size_t count = 0;
  struct dirent *subdir_entry;

  while ((subdir_entry = readdir(main_dir)) != NULL) {
    /* Skip . and .. */
    if (subdir_entry->d_name[0] == '.') {
      continue;
    }

    /* Build subdirectory path */
    char subdir_path[512];
    snprintf(subdir_path, sizeof(subdir_path), "%s/%s", blocks_dir,
             subdir_entry->d_name);

    /* Check if it's a directory */
    struct stat st;
    if (stat(subdir_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
      continue;
    }

    /* Open subdirectory */
    DIR *sub_dir = opendir(subdir_path);
    if (sub_dir == NULL) {
      continue;
    }

    /* Count matching files */
    struct dirent *file_entry;
    while ((file_entry = readdir(sub_dir)) != NULL) {
      if (parse_block_filename(file_entry->d_name) != UINT32_MAX) {
        count++;
      }
    }

    closedir(sub_dir);
  }

  /* Empty directory tree */
  if (count == 0) {
    closedir(main_dir);
    return ECHO_OK;
  }

  /* Allocate array */
  uint32_t *heights = (uint32_t *)malloc(count * sizeof(uint32_t));
  if (heights == NULL) {
    closedir(main_dir);
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /* Second pass: collect heights */
  rewinddir(main_dir);
  size_t idx = 0;

  while ((subdir_entry = readdir(main_dir)) != NULL && idx < count) {
    /* Skip . and .. */
    if (subdir_entry->d_name[0] == '.') {
      continue;
    }

    /* Build subdirectory path */
    char subdir_path[512];
    snprintf(subdir_path, sizeof(subdir_path), "%s/%s", blocks_dir,
             subdir_entry->d_name);

    /* Check if it's a directory */
    struct stat st;
    if (stat(subdir_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
      continue;
    }

    /* Open subdirectory */
    DIR *sub_dir = opendir(subdir_path);
    if (sub_dir == NULL) {
      continue;
    }

    /* Collect heights */
    struct dirent *file_entry;
    while ((file_entry = readdir(sub_dir)) != NULL && idx < count) {
      uint32_t height = parse_block_filename(file_entry->d_name);
      if (height != UINT32_MAX) {
        heights[idx++] = height;
      }
    }

    closedir(sub_dir);
  }

  closedir(main_dir);

  /* Sort heights in ascending order */
  qsort(heights, idx, sizeof(uint32_t), compare_heights);

  *heights_out = heights;
  *count_out = idx;

  log_debug(LOG_COMP_STORE, "Scanned %zu block files across subdirectories",
            idx);
  return ECHO_OK;
}

/*
 * Get total disk usage of all block files.
 * Scans all subdirectories and sums file sizes.
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

  /* Open main blocks directory */
  DIR *main_dir = opendir(blocks_dir);
  if (main_dir == NULL) {
    return ECHO_OK; /* No directory = 0 bytes */
  }

  struct dirent *subdir_entry;
  uint64_t total = 0;

  while ((subdir_entry = readdir(main_dir)) != NULL) {
    /* Skip . and .. */
    if (subdir_entry->d_name[0] == '.') {
      continue;
    }

    /* Build subdirectory path */
    char subdir_path[512];
    snprintf(subdir_path, sizeof(subdir_path), "%s/%s", blocks_dir,
             subdir_entry->d_name);

    /* Check if it's a directory */
    struct stat st;
    if (stat(subdir_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
      continue;
    }

    /* Open subdirectory */
    DIR *sub_dir = opendir(subdir_path);
    if (sub_dir == NULL) {
      continue;
    }

    /* Sum file sizes */
    struct dirent *file_entry;
    while ((file_entry = readdir(sub_dir)) != NULL) {
      /* Skip non-block files */
      if (parse_block_filename(file_entry->d_name) == UINT32_MAX) {
        continue;
      }

      /* Build full path and get file size */
      char file_path[512];
      snprintf(file_path, sizeof(file_path), "%s/%s", subdir_path,
               file_entry->d_name);

      struct stat file_st;
      if (stat(file_path, &file_st) == 0 && S_ISREG(file_st.st_mode)) {
        total += (uint64_t)file_st.st_size;
      }
    }

    closedir(sub_dir);
  }

  closedir(main_dir);
  *total_size = total;
  return ECHO_OK;
}
