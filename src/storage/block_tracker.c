/*
 * Bitcoin Echo — Block Availability Tracker Implementation
 *
 * Bitmap-based tracking of which block heights are stored on disk.
 *
 * Build once. Build right. Stop.
 */

#include "block_tracker.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Bitmap Helpers
 * ============================================================================
 */

/* Get byte index for a height */
static inline size_t height_to_byte(uint32_t height) { return height / 8; }

/* Get bit mask for a height within its byte */
static inline uint8_t height_to_mask(uint32_t height) {
  return (uint8_t)(1 << (height % 8));
}

/* Ensure bitmap can hold the given height, growing if needed */
static echo_result_t ensure_capacity(block_tracker_t *tracker,
                                     uint32_t height) {
  size_t needed_bytes = height_to_byte(height) + 1;

  if (needed_bytes <= tracker->map_capacity / 8) {
    return ECHO_OK; /* Already have capacity */
  }

  /* Calculate new capacity */
  size_t new_capacity = tracker->map_capacity;
  while (new_capacity / 8 < needed_bytes) {
    new_capacity *= BLOCK_TRACKER_GROWTH_FACTOR;
  }

  /* Allocate new bitmap */
  size_t new_bytes = new_capacity / 8;
  uint8_t *new_map = calloc(new_bytes, 1);
  if (!new_map) {
    log_error(LOG_COMP_STORE, "block_tracker: failed to grow bitmap to %zu bytes", new_bytes);
    return ECHO_ERR_MEMORY;
  }

  /* Copy existing data */
  size_t old_bytes = tracker->map_capacity / 8;
  if (tracker->availability_map) {
    memcpy(new_map, tracker->availability_map, old_bytes);
    free(tracker->availability_map);
  }

  tracker->availability_map = new_map;
  tracker->map_capacity = new_capacity;

  log_debug(LOG_COMP_STORE, "block_tracker: grew bitmap to %zu heights (%zu KB)",
            new_capacity, new_bytes / 1024);

  return ECHO_OK;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

block_tracker_t *block_tracker_create(uint32_t validated_tip) {
  block_tracker_t *tracker = calloc(1, sizeof(block_tracker_t));
  if (!tracker) {
    log_error(LOG_COMP_STORE, "block_tracker: failed to allocate tracker");
    return NULL;
  }

  tracker->validated_tip = validated_tip;
  tracker->highest_stored = validated_tip;
  tracker->map_capacity = BLOCK_TRACKER_INITIAL_CAPACITY;

  size_t bytes = tracker->map_capacity / 8;
  tracker->availability_map = calloc(bytes, 1);
  if (!tracker->availability_map) {
    log_error(LOG_COMP_STORE, "block_tracker: failed to allocate bitmap (%zu bytes)", bytes);
    free(tracker);
    return NULL;
  }

  log_info(LOG_COMP_STORE, "block_tracker: created with validated_tip=%u, capacity=%zu heights",
           validated_tip, tracker->map_capacity);

  return tracker;
}

void block_tracker_destroy(block_tracker_t *tracker) {
  if (!tracker) {
    return;
  }

  free(tracker->availability_map);
  free(tracker);
}

void block_tracker_reset(block_tracker_t *tracker, uint32_t validated_tip) {
  if (!tracker) {
    return;
  }

  tracker->validated_tip = validated_tip;
  tracker->highest_stored = validated_tip;

  /* Clear the bitmap */
  size_t bytes = tracker->map_capacity / 8;
  memset(tracker->availability_map, 0, bytes);

  log_info(LOG_COMP_STORE, "block_tracker: reset to validated_tip=%u", validated_tip);
}

/* ============================================================================
 * Availability Tracking
 * ============================================================================
 */

echo_result_t block_tracker_mark_available(block_tracker_t *tracker,
                                           uint32_t height) {
  if (!tracker) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Don't track heights at or below validated tip */
  if (height <= tracker->validated_tip) {
    return ECHO_OK;
  }

  /* Ensure we have capacity for this height */
  echo_result_t result = ensure_capacity(tracker, height);
  if (result != ECHO_OK) {
    return result;
  }

  /* Set the bit */
  size_t byte_idx = height_to_byte(height);
  uint8_t mask = height_to_mask(height);
  tracker->availability_map[byte_idx] |= mask;

  /* Update highest stored */
  if (height > tracker->highest_stored) {
    tracker->highest_stored = height;
  }

  return ECHO_OK;
}

bool block_tracker_has_block(const block_tracker_t *tracker, uint32_t height) {
  if (!tracker) {
    return false;
  }

  /* Heights at or below validated tip are considered "available" */
  if (height <= tracker->validated_tip) {
    return true;
  }

  /* Heights beyond our capacity are not available */
  if (height >= tracker->map_capacity) {
    return false;
  }

  size_t byte_idx = height_to_byte(height);
  uint8_t mask = height_to_mask(height);
  return (tracker->availability_map[byte_idx] & mask) != 0;
}

void block_tracker_mark_validated(block_tracker_t *tracker,
                                  uint32_t new_validated_tip) {
  if (!tracker) {
    return;
  }

  if (new_validated_tip <= tracker->validated_tip) {
    return; /* No change or going backwards */
  }

  /*
   * Clear bits from old validated_tip + 1 to new_validated_tip.
   * This saves memory by not tracking heights we've already processed.
   * Since we only ever validate consecutive ranges, all these heights
   * must have been available.
   */
  for (uint32_t h = tracker->validated_tip + 1; h <= new_validated_tip; h++) {
    if (h < tracker->map_capacity) {
      size_t byte_idx = height_to_byte(h);
      uint8_t mask = height_to_mask(h);
      tracker->availability_map[byte_idx] &= ~mask;
    }
  }

  uint32_t old_tip = tracker->validated_tip;
  tracker->validated_tip = new_validated_tip;

  log_debug(LOG_COMP_STORE, "block_tracker: validated %u -> %u (%u blocks)",
            old_tip, new_validated_tip, new_validated_tip - old_tip);
}

/* ============================================================================
 * Range Finding
 * ============================================================================
 */

bool block_tracker_find_consecutive_range(const block_tracker_t *tracker,
                                          block_range_t *range_out) {
  if (!tracker || !range_out) {
    return false;
  }

  /* Start from the first block after validated tip */
  uint32_t start = tracker->validated_tip + 1;

  /* If nothing stored above validated tip, no range */
  if (start > tracker->highest_stored) {
    return false;
  }

  /* Check if start block is available */
  if (!block_tracker_has_block(tracker, start)) {
    return false; /* First block missing, no consecutive range */
  }

  /* Walk forward until we hit a gap or reach highest_stored */
  uint32_t end = start;
  while (end < tracker->highest_stored &&
         block_tracker_has_block(tracker, end + 1)) {
    end++;
  }

  range_out->start_height = start;
  range_out->end_height = end;
  range_out->count = end - start + 1;

  return true;
}

bool block_tracker_find_blocking_block(const block_tracker_t *tracker,
                                       uint32_t *height_out) {
  if (!tracker || !height_out) {
    return false;
  }

  /* Start from the first block after validated tip */
  uint32_t start = tracker->validated_tip + 1;

  /* If nothing stored above validated tip, first missing is start */
  if (start > tracker->highest_stored) {
    *height_out = start;
    return true;
  }

  /* Walk forward to find first missing block */
  for (uint32_t h = start; h <= tracker->highest_stored; h++) {
    if (!block_tracker_has_block(tracker, h)) {
      *height_out = h;
      return true;
    }
  }

  /* All blocks up to highest_stored are available */
  if (tracker->highest_stored < UINT32_MAX) {
    *height_out = tracker->highest_stored + 1;
    return true; /* Next block to download */
  }

  return false; /* No blocking block (shouldn't happen in practice) */
}

/* ============================================================================
 * Statistics
 * ============================================================================
 */

uint32_t block_tracker_available_count(const block_tracker_t *tracker) {
  if (!tracker) {
    return 0;
  }

  uint32_t count = 0;
  for (uint32_t h = tracker->validated_tip + 1; h <= tracker->highest_stored;
       h++) {
    if (block_tracker_has_block(tracker, h)) {
      count++;
    }
  }

  return count;
}

uint32_t block_tracker_missing_count(const block_tracker_t *tracker) {
  if (!tracker) {
    return 0;
  }

  if (tracker->highest_stored <= tracker->validated_tip) {
    return 0;
  }

  uint32_t total = tracker->highest_stored - tracker->validated_tip;
  uint32_t available = block_tracker_available_count(tracker);

  return total - available;
}
