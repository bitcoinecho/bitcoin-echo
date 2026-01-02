/*
 * Bitcoin Echo — Block Availability Tracker
 *
 * Tracks which block heights have been downloaded to disk using a bitmap.
 * This is a core component of the decoupled IBD architecture:
 *
 * Key insight: Downloads and validation are DECOUPLED. Blocks arrive out of
 * order from the network, are stored immediately, and validation runs
 * independently on consecutive ranges.
 *
 * The tracker provides:
 *   - O(1) check if a height is available
 *   - O(n) scan to find consecutive ranges for validation
 *   - Minimal memory footprint (1 bit per block height)
 *
 * See IBD-DECOUPLED-ARCHITECTURE.md for the full design.
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_BLOCK_TRACKER_H
#define ECHO_BLOCK_TRACKER_H

#include "echo_types.h"
#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * Constants
 * ============================================================================
 */

/*
 * Initial bitmap capacity in heights.
 * 1 million heights = 125 KB of memory.
 * Will grow automatically if needed.
 */
#define BLOCK_TRACKER_INITIAL_CAPACITY (1024 * 1024)

/*
 * Growth factor when resizing bitmap.
 */
#define BLOCK_TRACKER_GROWTH_FACTOR 2

/* ============================================================================
 * Block Tracker
 * ============================================================================
 */

/*
 * Block availability tracker.
 *
 * Tracks which block heights have blocks stored on disk.
 * Uses a bitmap for compact O(1) lookups.
 *
 * Thread safety: Not thread-safe. Caller must synchronize if used
 * from multiple threads.
 */
typedef struct block_tracker {
  uint32_t validated_tip;    /* Last validated block height */
  uint32_t highest_stored;   /* Highest block height stored to disk */
  uint8_t *availability_map; /* Bitmap: 1 = have block, 0 = missing */
  size_t map_capacity;       /* Capacity in heights (bits) */
} block_tracker_t;

/*
 * Consecutive range of available blocks.
 *
 * Returned by block_tracker_find_consecutive_range().
 */
typedef struct {
  uint32_t start_height; /* First height in range (inclusive) */
  uint32_t end_height;   /* Last height in range (inclusive) */
  uint32_t count;        /* Number of blocks in range */
} block_range_t;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/*
 * Create a block tracker.
 *
 * Parameters:
 *   validated_tip - Initial validated tip height (usually loaded from DB)
 *
 * Returns:
 *   Newly allocated tracker, or NULL on failure
 */
block_tracker_t *block_tracker_create(uint32_t validated_tip);

/*
 * Destroy a block tracker and free resources.
 */
void block_tracker_destroy(block_tracker_t *tracker);

/*
 * Reset tracker state.
 *
 * Clears the availability bitmap and resets to initial state.
 * Used when starting a fresh sync or after major reorganization.
 *
 * Parameters:
 *   tracker       - The tracker
 *   validated_tip - New validated tip height
 */
void block_tracker_reset(block_tracker_t *tracker, uint32_t validated_tip);

/* ============================================================================
 * Availability Tracking
 * ============================================================================
 */

/*
 * Mark a block height as available (downloaded to disk).
 *
 * Parameters:
 *   tracker - The tracker
 *   height  - Block height that was stored
 *
 * Returns:
 *   ECHO_OK on success
 *   ECHO_ERR_NO_MEMORY if bitmap couldn't grow
 *
 * Notes:
 *   - Automatically grows bitmap if height exceeds capacity
 *   - Updates highest_stored if height is higher
 *   - Idempotent: marking same height twice is safe
 */
echo_result_t block_tracker_mark_available(block_tracker_t *tracker,
                                           uint32_t height);

/*
 * Check if a block height is available.
 *
 * Parameters:
 *   tracker - The tracker
 *   height  - Block height to check
 *
 * Returns:
 *   true if block at height is available, false otherwise
 */
bool block_tracker_has_block(const block_tracker_t *tracker, uint32_t height);

/*
 * Mark a range of heights as validated.
 *
 * Called after successfully validating a chunk of blocks.
 * Clears availability bits for heights <= new_validated_tip
 * (we no longer need to track them individually).
 *
 * Parameters:
 *   tracker           - The tracker
 *   new_validated_tip - New validated tip after chunk validation
 */
void block_tracker_mark_validated(block_tracker_t *tracker,
                                  uint32_t new_validated_tip);

/* ============================================================================
 * Range Finding
 * ============================================================================
 */

/*
 * Find the consecutive range of available blocks above validated tip.
 *
 * This is the key operation for the decoupled validation model.
 * Scans from validated_tip + 1 forward to find the largest consecutive
 * run of available blocks.
 *
 * Parameters:
 *   tracker   - The tracker
 *   range_out - Output: the consecutive range found
 *
 * Returns:
 *   true if a non-empty range was found, false if no blocks available
 *
 * Example:
 *   validated_tip = 10000
 *   available: 10001-18000 (consecutive), 18002-20000 (gap at 18001)
 *   Result: range = {10001, 18000, 8000}
 *
 * The "blocking block" is 18001 - the first missing block that stops
 * the consecutive range.
 */
bool block_tracker_find_consecutive_range(const block_tracker_t *tracker,
                                          block_range_t *range_out);

/*
 * Get the first missing block height above validated tip.
 *
 * Useful for identifying the "blocking block" that's preventing
 * validation from progressing.
 *
 * Parameters:
 *   tracker    - The tracker
 *   height_out - Output: first missing height, or 0 if all available
 *
 * Returns:
 *   true if there's a gap (blocking block found), false if no gaps
 */
bool block_tracker_find_blocking_block(const block_tracker_t *tracker,
                                       uint32_t *height_out);

/* ============================================================================
 * Statistics
 * ============================================================================
 */

/*
 * Get the number of blocks available between validated_tip and highest_stored.
 *
 * Parameters:
 *   tracker - The tracker
 *
 * Returns:
 *   Count of available blocks in the pending range
 */
uint32_t block_tracker_available_count(const block_tracker_t *tracker);

/*
 * Get the number of blocks missing between validated_tip and highest_stored.
 *
 * Parameters:
 *   tracker - The tracker
 *
 * Returns:
 *   Count of missing blocks (gaps) in the pending range
 */
uint32_t block_tracker_missing_count(const block_tracker_t *tracker);

/*
 * Get the validated tip height.
 */
static inline uint32_t
block_tracker_get_validated_tip(const block_tracker_t *tracker) {
  return tracker->validated_tip;
}

/*
 * Get the highest stored height.
 */
static inline uint32_t
block_tracker_get_highest_stored(const block_tracker_t *tracker) {
  return tracker->highest_stored;
}

#endif /* ECHO_BLOCK_TRACKER_H */
