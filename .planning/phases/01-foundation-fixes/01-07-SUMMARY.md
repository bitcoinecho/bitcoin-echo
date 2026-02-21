---
phase: 01-foundation-fixes
plan: 07
subsystem: testing
tags: [download-manager, peer-eviction, unit-tests, ibd]

# Dependency graph
requires:
  - phase: 01-05
    provides: Calibrated eviction threshold (1 KB/s) and debug-level logging in check_performance and evict_slowest_percent

provides:
  - Peer eviction test suite covering 5 scenarios: slowest-above-min, no-evict-at-min, floor enforcement, equal-rate, stalled-peer
  - download_mgr_inject_peer_rate() test API that bypasses real-time performance window for deterministic testing

affects:
  - Phase 2 planning: eviction logic is now covered by tests — regression-safe for future changes

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Test injection API pattern: expose a documented test-only function rather than #ifdef guards or sleeping real time"
    - "void* intermediate cast to silence -Wcast-align when opaque peer_t* is used with mock structs in test helpers"

key-files:
  created: []
  modified:
    - include/download_mgr.h
    - src/protocol/download_mgr.c
    - test/unit/test_download_mgr.c

key-decisions:
  - "Add download_mgr_inject_peer_rate() as a proper public test API (not #ifdef-guarded) — honest about its purpose, simpler than mocking plat_time_ms(), and avoids sleeping 10+ seconds in tests"
  - "has_reported always set true in inject function even for zero-rate peers — a stalled peer (rate=0) IS a reporter that stopped delivering, distinct from a peer that has never delivered"
  - "Peer set up for eviction tests via assign-work + inject-rate pattern rather than block_received() chains — injection is the only viable path since performance windows are 10 seconds of real time"

patterns-established:
  - "Eviction test setup pattern: add_peer → add_work → peer_request_work → inject_peer_rate gives a fully-armed candidate for eviction functions"
  - "All 5 eviction scenarios (above-min, at-min, floor-enforcement, equal-rate, stall) should be present in any download manager test suite to prevent network-isolation regressions"

requirements-completed: [TEST-03]

# Metrics
duration: 15min
completed: 2026-02-20
---

# Phase 01 Plan 07: Peer Eviction Test Suite Summary

**Five peer eviction test cases covering slowest-peer selection, minimum-count floor enforcement, equal-rate stability, and stalled-peer priority — with a deterministic test injection API that bypasses the 10-second real-time performance window**

## Performance

- **Duration:** ~15 min
- **Started:** 2026-02-20T00:00:00Z
- **Completed:** 2026-02-20T00:15:00Z
- **Tasks:** 1
- **Files modified:** 3

## Accomplishments
- Added `download_mgr_inject_peer_rate()` to `include/download_mgr.h` and `src/protocol/download_mgr.c` — directly sets `bytes_per_second`, `has_reported=true`, and `first_work_time=1` to bypass the 10-second grace period without sleeping
- Added `setup_active_peer()` helper in test file — add peer, assign batch, inject rate in one call
- Added 5 eviction test cases (test_evict_slowest_above_min, test_no_evict_at_minimum, test_evict_stops_at_minimum_floor, test_evict_all_equal_rates, test_evict_stalled_peer_zero_rate)
- Grew test suite from 20 to 25 passing tests with zero new warnings

## Task Commits

1. **Task 1: Add peer eviction test suite to test_download_mgr.c** - `ac8b1e4` (test)

## Files Created/Modified
- `/Users/yayseth/Projects/echo/bitcoin-echo/include/download_mgr.h` - Added `download_mgr_inject_peer_rate()` declaration in new "Test Support" section
- `/Users/yayseth/Projects/echo/bitcoin-echo/src/protocol/download_mgr.c` - Implemented `download_mgr_inject_peer_rate()` before Query Functions section
- `/Users/yayseth/Projects/echo/bitcoin-echo/test/unit/test_download_mgr.c` - Added 5 eviction test functions, `setup_active_peer()` helper, wired all into main()

## Decisions Made
- `download_mgr_inject_peer_rate()` is a proper public API function (not `#ifdef`-guarded) with a clear "FOR UNIT TESTS ONLY" comment. This is simpler, more honest, and avoids the C preprocessor complexity of test-conditional builds.
- `has_reported` is always set `true` in the inject function regardless of rate value. A peer with rate=0 that "has_reported=true" models a stalled peer (used to deliver, now stopped) — which is distinct from a warming-up peer (never delivered). This distinction is what `evict_slowest_percent` uses to decide candidacy.
- Used `void *` intermediate cast in `setup_active_peer()` to silence `-Wcast-align` when passing `mock_peer_t *` as `peer_t *` in a helper function context. The download manager only stores and compares the pointer value, so alignment mismatch is not a real runtime issue.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed test injection setting has_reported incorrectly for zero-rate peers**
- **Found during:** Task 1 (test_evict_stalled_peer_zero_rate)
- **Issue:** Initial implementation set `has_reported = (bytes_per_sec > 0.0f)`, which excluded zero-rate stalled peers from eviction candidates. `evict_slowest_percent` requires `has_reported=true` to include a peer as a candidate. A stalled peer (rate=0) must have `has_reported=true` to model "used to deliver, now stopped" — the condition that triggers eviction.
- **Fix:** Changed to `perf->has_reported = true` unconditionally in `download_mgr_inject_peer_rate()`.
- **Files modified:** `src/protocol/download_mgr.c`
- **Verification:** `test_evict_stalled_peer_zero_rate` passes; stalled peer is confirmed as the one disconnected.
- **Committed in:** `ac8b1e4` (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (Rule 1 - bug in test injection logic)
**Impact on plan:** Required for correctness of the stalled-peer test case. No scope creep.

## Issues Encountered

The `setup_active_peer()` helper function triggered `-Wcast-align` warnings when casting `mock_peer_t *` to `peer_t *` via a function parameter (the compiler can't verify alignment of a pointer argument the way it can a stack variable). Fixed by inserting a `void *` intermediate, matching the pattern used in `download_mgr.c` itself for batch node casts.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Peer eviction logic is now covered by 5 deterministic test cases — safe for Phase 2 work that may touch the download manager
- `download_mgr_inject_peer_rate()` is available if future plans need to add more eviction test scenarios
- Phase 01 plan suite is complete (7/7 plans done)

---
*Phase: 01-foundation-fixes*
*Completed: 2026-02-20*
