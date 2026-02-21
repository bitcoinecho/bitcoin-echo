---
phase: 01-foundation-fixes
plan: "06"
subsystem: testing
tags: [pthread, concurrency, block-storage, edge-cases, c11]

# Dependency graph
requires:
  - phase: 01-04
    provides: flush-before-index ordering in block_storage_write() making storage layer safe for concurrent test validation
provides:
  - Concurrent block storage tests validating mutex protection under 4-thread load
  - Edge case tests for max size, corrupted size fields, truncated reads, near-4x witness limit
affects:
  - Phase 2-4 work that builds on block storage layer

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "storage_writer_arg_t/storage_reader_arg_t structs for passing thread arguments and collecting results"
    - "Separate TEST_CONCURRENT_DIR for concurrent tests to avoid interference with sequential tests"
    - "Shared position array protected by count_mutex for mixed read/write concurrency pattern"

key-files:
  created: []
  modified:
    - test/unit/test_block_storage.c

key-decisions:
  - "Corrupted size field test uses 0xFFFFFFFF (4 GB) — well above ECHO_MAX_BLOCK_SIZE*4 threshold that blocks.c rejects"
  - "Near-4x witness limit test uses ECHO_MAX_BLOCK_SIZE*4-1 (3,999,999 bytes) — highest valid storage size"
  - "Mixed read/write test publishes positions via shared_count+count_mutex so readers only read committed writes"
  - "Concurrent tests use TEST_CONCURRENT_DIR (/tmp/echo_block_storage_concurrent_test) separate from sequential TEST_DATA_DIR"

patterns-established:
  - "Concurrent test pattern: spawn N threads, join all, verify per-thread results then cross-thread invariants"
  - "Edge case pattern: write valid block, corrupt on-disk bytes via fopen r+b, reinit manager, verify error returned"

requirements-completed: [TEST-02, TEST-05]

# Metrics
duration: 15min
completed: 2026-02-20
---

# Phase 01 Plan 06: Block Storage Concurrent and Edge Case Tests Summary

**7 new tests cover 4-thread concurrent writes/reads with corruption detection and max-size/truncated/witness-limit edge cases against the mutex-protected block_file_manager_t**

## Performance

- **Duration:** ~15 min
- **Completed:** 2026-02-20
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments

- 3 concurrent tests exercise block_file_manager_t mutex under real pthread load: 4-thread write (200 blocks, all positions unique), 4-thread read (100 pre-written blocks), mixed 2-writer/2-reader (no deadlock, pattern verified)
- 4 edge case tests cover all user-required scenarios: exact ECHO_MAX_BLOCK_SIZE boundary, corrupted 0xFFFFFFFF size field rejected, ftruncated file returns error, near-4x witness weight (3,999,999 bytes) roundtrip succeeds
- Total block storage tests: 16/16 passing (up from 9/9)

## Task Commits

1. **Task 1 + Task 2: Concurrent and edge case tests** - `83b27b7` (test)

**Plan metadata:** (created in this commit)

## Files Created/Modified

- `test/unit/test_block_storage.c` — Added storage_writer_arg_t, storage_reader_arg_t, mixed_writer_arg_t, mixed_reader_arg_t structs; test_concurrent_writes(), test_concurrent_reads(), test_mixed_concurrent_rw(), test_max_size_block(), test_corrupted_size_field(), test_truncated_read(), test_near_4x_witness_limit() functions; 7 new test_case() registrations in main()

## Decisions Made

- Corrupted size field test uses `0xFFFFFFFF` as the corrupt value — this is 4,294,967,295 bytes, well above the `ECHO_MAX_BLOCK_SIZE * 4` (4,000,000) threshold that `block_storage_read()` rejects in blocks.c line 432. No fix to blocks.c was needed — validation was already present.
- Mixed read/write thread coordination uses a separate `count_mutex` guarding a shared write counter so readers only read positions that writers have already committed and published.
- Near-4x witness limit uses `ECHO_MAX_BLOCK_SIZE * 4U - 1U` = 3,999,999 bytes — the highest size the storage layer accepts. This directly tests the SegWit maximum-weight storage boundary.

## Deviations from Plan

None — plan executed exactly as written. The bounds check for corrupted size fields (`block_size > ECHO_MAX_BLOCK_SIZE * 4`) was already present in blocks.c; no implementation fix was needed.

## Issues Encountered

- The `make` timestamp cache caused the rebuilt test binary to not register as newer than the previous binary, requiring a direct `cc` invocation to force recompilation and confirm all 16 tests run. This is a make dependency issue with the test_utils.o object file, not a code problem.

## Next Phase Readiness

- Block storage layer validated under concurrent load — Phase 2-4 work can build on it confidently
- All 7 user-required test scenarios covered (TEST-02, TEST-05)

---
*Phase: 01-foundation-fixes*
*Completed: 2026-02-20*
