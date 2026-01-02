/*
 * Bitcoin Echo — Block Tracker Tests
 *
 * Tests for the block availability tracker (bitmap-based).
 *
 * Build once. Build right. Stop.
 */

#include "block_tracker.h"
#include "test_utils.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ========================================================================
 * Lifecycle Tests
 * ======================================================================== */

static void test_create_destroy(void) {
  block_tracker_t *tracker;

  test_case("Create and destroy tracker");

  tracker = block_tracker_create(0);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  if (block_tracker_get_validated_tip(tracker) != 0) {
    test_fail_uint("wrong validated_tip", 0,
                   block_tracker_get_validated_tip(tracker));
    block_tracker_destroy(tracker);
    return;
  }

  if (block_tracker_get_highest_stored(tracker) != 0) {
    test_fail_uint("wrong highest_stored", 0,
                   block_tracker_get_highest_stored(tracker));
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

static void test_create_with_tip(void) {
  block_tracker_t *tracker;

  test_case("Create tracker with non-zero validated tip");

  tracker = block_tracker_create(10000);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  if (block_tracker_get_validated_tip(tracker) != 10000) {
    test_fail_uint("wrong validated_tip", 10000,
                   block_tracker_get_validated_tip(tracker));
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

/* ========================================================================
 * Availability Tests
 * ======================================================================== */

static void test_mark_single_block(void) {
  block_tracker_t *tracker;
  echo_result_t result;

  test_case("Mark single block as available");

  tracker = block_tracker_create(0);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  /* Mark block 100 as available */
  result = block_tracker_mark_available(tracker, 100);
  if (result != ECHO_OK) {
    test_fail("failed to mark block available");
    block_tracker_destroy(tracker);
    return;
  }

  /* Check it's available */
  if (!block_tracker_has_block(tracker, 100)) {
    test_fail("block 100 should be available");
    block_tracker_destroy(tracker);
    return;
  }

  /* Check other blocks are not */
  if (block_tracker_has_block(tracker, 99)) {
    test_fail("block 99 should not be available");
    block_tracker_destroy(tracker);
    return;
  }

  if (block_tracker_has_block(tracker, 101)) {
    test_fail("block 101 should not be available");
    block_tracker_destroy(tracker);
    return;
  }

  /* Check highest_stored updated */
  if (block_tracker_get_highest_stored(tracker) != 100) {
    test_fail_uint("wrong highest_stored", 100,
                   block_tracker_get_highest_stored(tracker));
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

static void test_mark_below_validated_tip(void) {
  block_tracker_t *tracker;
  echo_result_t result;

  test_case("Marking block below validated tip is ignored");

  tracker = block_tracker_create(1000);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  /* Try to mark block 500 (below validated tip) */
  result = block_tracker_mark_available(tracker, 500);
  if (result != ECHO_OK) {
    test_fail("mark_available should succeed (no-op)");
    block_tracker_destroy(tracker);
    return;
  }

  /* Highest stored should still be 1000 (validated tip) */
  if (block_tracker_get_highest_stored(tracker) != 1000) {
    test_fail_uint("highest_stored changed unexpectedly", 1000,
                   block_tracker_get_highest_stored(tracker));
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

static void test_has_block_at_validated_tip(void) {
  block_tracker_t *tracker;

  test_case("Blocks at or below validated tip are 'available'");

  tracker = block_tracker_create(1000);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  /* Block at validated tip should return true */
  if (!block_tracker_has_block(tracker, 1000)) {
    test_fail("block at validated_tip should be available");
    block_tracker_destroy(tracker);
    return;
  }

  /* Blocks below validated tip should return true */
  if (!block_tracker_has_block(tracker, 500)) {
    test_fail("block below validated_tip should be available");
    block_tracker_destroy(tracker);
    return;
  }

  /* Block above validated tip should return false (not stored) */
  if (block_tracker_has_block(tracker, 1001)) {
    test_fail("block above validated_tip should not be available");
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

static void test_mark_idempotent(void) {
  block_tracker_t *tracker;

  test_case("Marking same block twice is idempotent");

  tracker = block_tracker_create(0);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  block_tracker_mark_available(tracker, 100);
  block_tracker_mark_available(tracker, 100);

  if (!block_tracker_has_block(tracker, 100)) {
    test_fail("block should be available");
    block_tracker_destroy(tracker);
    return;
  }

  if (block_tracker_available_count(tracker) != 1) {
    test_fail_uint("available count wrong", 1,
                   block_tracker_available_count(tracker));
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

/* ========================================================================
 * Consecutive Range Tests
 * ======================================================================== */

static void test_consecutive_range_simple(void) {
  block_tracker_t *tracker;
  block_range_t range;

  test_case("Find consecutive range - simple case");

  tracker = block_tracker_create(0);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  /* Mark blocks 1-10 as available */
  for (uint32_t h = 1; h <= 10; h++) {
    block_tracker_mark_available(tracker, h);
  }

  if (!block_tracker_find_consecutive_range(tracker, &range)) {
    test_fail("should find consecutive range");
    block_tracker_destroy(tracker);
    return;
  }

  if (range.start_height != 1) {
    test_fail_uint("wrong start_height", 1, range.start_height);
    block_tracker_destroy(tracker);
    return;
  }

  if (range.end_height != 10) {
    test_fail_uint("wrong end_height", 10, range.end_height);
    block_tracker_destroy(tracker);
    return;
  }

  if (range.count != 10) {
    test_fail_uint("wrong count", 10, range.count);
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

static void test_consecutive_range_with_gap(void) {
  block_tracker_t *tracker;
  block_range_t range;

  test_case("Find consecutive range stops at gap");

  tracker = block_tracker_create(1000);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  /* Mark blocks 1001-1050, then skip 1051, then 1052-1100 */
  for (uint32_t h = 1001; h <= 1050; h++) {
    block_tracker_mark_available(tracker, h);
  }
  /* Skip 1051 */
  for (uint32_t h = 1052; h <= 1100; h++) {
    block_tracker_mark_available(tracker, h);
  }

  if (!block_tracker_find_consecutive_range(tracker, &range)) {
    test_fail("should find consecutive range");
    block_tracker_destroy(tracker);
    return;
  }

  /* Should stop at 1050 (before the gap at 1051) */
  if (range.start_height != 1001) {
    test_fail_uint("wrong start_height", 1001, range.start_height);
    block_tracker_destroy(tracker);
    return;
  }

  if (range.end_height != 1050) {
    test_fail_uint("wrong end_height", 1050, range.end_height);
    block_tracker_destroy(tracker);
    return;
  }

  if (range.count != 50) {
    test_fail_uint("wrong count", 50, range.count);
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

static void test_consecutive_range_first_missing(void) {
  block_tracker_t *tracker;
  block_range_t range;

  test_case("No consecutive range when first block missing");

  tracker = block_tracker_create(1000);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  /* Skip 1001, mark 1002-1010 */
  for (uint32_t h = 1002; h <= 1010; h++) {
    block_tracker_mark_available(tracker, h);
  }

  if (block_tracker_find_consecutive_range(tracker, &range)) {
    test_fail("should not find range when first block missing");
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

static void test_consecutive_range_empty(void) {
  block_tracker_t *tracker;
  block_range_t range;

  test_case("No consecutive range when no blocks stored");

  tracker = block_tracker_create(1000);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  if (block_tracker_find_consecutive_range(tracker, &range)) {
    test_fail("should not find range when no blocks stored");
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

/* ========================================================================
 * Blocking Block Tests
 * ======================================================================== */

static void test_find_blocking_block(void) {
  block_tracker_t *tracker;
  uint32_t blocking;

  test_case("Find blocking block in gap");

  tracker = block_tracker_create(1000);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  /* Mark 1001-1050, skip 1051, mark 1052-1100 */
  for (uint32_t h = 1001; h <= 1050; h++) {
    block_tracker_mark_available(tracker, h);
  }
  for (uint32_t h = 1052; h <= 1100; h++) {
    block_tracker_mark_available(tracker, h);
  }

  if (!block_tracker_find_blocking_block(tracker, &blocking)) {
    test_fail("should find blocking block");
    block_tracker_destroy(tracker);
    return;
  }

  if (blocking != 1051) {
    test_fail_uint("wrong blocking block", 1051, blocking);
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

/* ========================================================================
 * Validated Tip Update Tests
 * ======================================================================== */

static void test_mark_validated(void) {
  block_tracker_t *tracker;
  block_range_t range;

  test_case("Mark validated advances tip and clears bits");

  tracker = block_tracker_create(1000);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  /* Mark blocks 1001-1100 as available */
  for (uint32_t h = 1001; h <= 1100; h++) {
    block_tracker_mark_available(tracker, h);
  }

  /* Validate up to 1050 */
  block_tracker_mark_validated(tracker, 1050);

  if (block_tracker_get_validated_tip(tracker) != 1050) {
    test_fail_uint("wrong validated_tip", 1050,
                   block_tracker_get_validated_tip(tracker));
    block_tracker_destroy(tracker);
    return;
  }

  /* New consecutive range should start at 1051 */
  if (!block_tracker_find_consecutive_range(tracker, &range)) {
    test_fail("should find consecutive range");
    block_tracker_destroy(tracker);
    return;
  }

  if (range.start_height != 1051) {
    test_fail_uint("wrong range start", 1051, range.start_height);
    block_tracker_destroy(tracker);
    return;
  }

  if (range.end_height != 1100) {
    test_fail_uint("wrong range end", 1100, range.end_height);
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

/* ========================================================================
 * Statistics Tests
 * ======================================================================== */

static void test_available_count(void) {
  block_tracker_t *tracker;

  test_case("Available count tracks stored blocks");

  tracker = block_tracker_create(0);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  /* Mark every other block from 1-20 */
  for (uint32_t h = 1; h <= 20; h += 2) {
    block_tracker_mark_available(tracker, h);
  }

  /* Should have 10 blocks: 1, 3, 5, 7, 9, 11, 13, 15, 17, 19 */
  if (block_tracker_available_count(tracker) != 10) {
    test_fail_uint("wrong available count", 10,
                   block_tracker_available_count(tracker));
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

static void test_missing_count(void) {
  block_tracker_t *tracker;

  test_case("Missing count tracks gaps");

  tracker = block_tracker_create(0);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  /* Mark blocks 1-5, skip 6, mark 7-10 */
  for (uint32_t h = 1; h <= 5; h++) {
    block_tracker_mark_available(tracker, h);
  }
  for (uint32_t h = 7; h <= 10; h++) {
    block_tracker_mark_available(tracker, h);
  }

  /* Total range is 1-10 (10 blocks), 9 available, 1 missing (block 6) */
  if (block_tracker_missing_count(tracker) != 1) {
    test_fail_uint("wrong missing count", 1,
                   block_tracker_missing_count(tracker));
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

/* ========================================================================
 * Reset Tests
 * ======================================================================== */

static void test_reset(void) {
  block_tracker_t *tracker;

  test_case("Reset clears state");

  tracker = block_tracker_create(0);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  /* Mark some blocks */
  for (uint32_t h = 1; h <= 100; h++) {
    block_tracker_mark_available(tracker, h);
  }

  /* Reset to validated tip 5000 */
  block_tracker_reset(tracker, 5000);

  if (block_tracker_get_validated_tip(tracker) != 5000) {
    test_fail_uint("wrong validated_tip after reset", 5000,
                   block_tracker_get_validated_tip(tracker));
    block_tracker_destroy(tracker);
    return;
  }

  if (block_tracker_get_highest_stored(tracker) != 5000) {
    test_fail_uint("wrong highest_stored after reset", 5000,
                   block_tracker_get_highest_stored(tracker));
    block_tracker_destroy(tracker);
    return;
  }

  /* Old blocks below validated tip ARE considered available (already validated) */
  if (!block_tracker_has_block(tracker, 50)) {
    test_fail("block 50 (below validated_tip) should be available");
    block_tracker_destroy(tracker);
    return;
  }

  /* But blocks above the new validated tip are not available */
  if (block_tracker_has_block(tracker, 5001)) {
    test_fail("block 5001 (above validated_tip) should not be available");
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

/* ========================================================================
 * Large Height Tests
 * ======================================================================== */

static void test_large_height(void) {
  block_tracker_t *tracker;

  test_case("Handle heights beyond initial capacity");

  tracker = block_tracker_create(0);
  if (!tracker) {
    test_fail("failed to create tracker");
    return;
  }

  /* Mark a block beyond initial capacity (1M heights) */
  uint32_t height = 2000000;
  echo_result_t result = block_tracker_mark_available(tracker, height);
  if (result != ECHO_OK) {
    test_fail("failed to mark high height");
    block_tracker_destroy(tracker);
    return;
  }

  if (!block_tracker_has_block(tracker, height)) {
    test_fail("high height block should be available");
    block_tracker_destroy(tracker);
    return;
  }

  if (block_tracker_get_highest_stored(tracker) != height) {
    test_fail_uint("wrong highest_stored", height,
                   block_tracker_get_highest_stored(tracker));
    block_tracker_destroy(tracker);
    return;
  }

  block_tracker_destroy(tracker);
  test_pass();
}

/* ========================================================================
 * Main
 * ======================================================================== */

int main(void) {
  test_suite_begin("Block Tracker");

  test_section("Lifecycle");
  test_create_destroy();
  test_create_with_tip();

  test_section("Availability Tracking");
  test_mark_single_block();
  test_mark_below_validated_tip();
  test_has_block_at_validated_tip();
  test_mark_idempotent();

  test_section("Consecutive Range Finding");
  test_consecutive_range_simple();
  test_consecutive_range_with_gap();
  test_consecutive_range_first_missing();
  test_consecutive_range_empty();

  test_section("Blocking Block Detection");
  test_find_blocking_block();

  test_section("Validation Advancement");
  test_mark_validated();

  test_section("Statistics");
  test_available_count();
  test_missing_count();

  test_section("Reset");
  test_reset();

  test_section("Capacity Handling");
  test_large_height();

  test_suite_end();
  return test_global_summary();
}
