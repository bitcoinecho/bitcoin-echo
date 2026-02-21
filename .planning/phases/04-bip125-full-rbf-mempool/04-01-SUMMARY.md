---
phase: 04-bip125-full-rbf-mempool
plan: 01
subsystem: protocol
tags: [mempool, rbf, bip125, replace-by-fee, c11]

# Dependency graph
requires:
  - phase: 03-p2p-block-serving
    provides: stable P2P layer that mempool policy builds on top of
provides:
  - rbf_validate_replacement() implementing all 5 BIP-125 rules in mempool.c
  - entry_signals_rbf_inherited() recursive ancestor signaling check
  - rbf_collect_eviction_set() with BFS descendant expansion and Rule 5 cap
  - Conflict branch in mempool_add now accepts valid RBF replacements
affects: [04-02-bip125-rbf-tests, 06-getblocktemplate]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "RBF eviction set built as fixed-size stack array (Rule 5 caps at 100) — no heap allocation needed"
    - "NOLINT(misc-no-recursion) with justification comment for bounded recursive ancestor walks"
    - "Atomic eviction: collect direct conflict txids before mempool_remove to avoid spent-table pointer invalidation"
    - "Rule 3 before Rule 4: absolute fee check happens first so fee-rate checks can't mask the absolute-fee trap"

key-files:
  created: []
  modified:
    - src/protocol/mempool.c

key-decisions:
  - "Implement BIP-125 opt-in RBF (Rules 1-5 as written) rather than full-RBF — phase success criteria explicitly check signaling; full-RBF is a policy toggle not a correctness requirement"
  - "Rule 1 signaling check moved from conflict detection loop into rbf_validate_replacement via entry_signals_rbf_inherited — ensures inherited signaling (parent signals, child inherits) is correctly handled"
  - "first_conflict variable removed from mempool_add — rbf_validate_replacement populates result->first_conflict directly from eviction_set[0]"
  - "required_fee in result is set to total_evicted_fees + relay_fee using ceiling division to avoid rounding below 1 sat"

patterns-established:
  - "RBF 5-rule validation pattern: build eviction set, check Rule 1, check Rule 2, check Rule 3 (absolute), check Rule 4 (rate), atomic evict"
  - "Eviction set deduplication: O(n) linear scan over <=100 entries via ADD_TO_EVICTION_SET macro"

requirements-completed: [P2P-03]

# Metrics
duration: 3min
completed: 2026-02-21
---

# Phase 04 Plan 01: BIP-125 Full-RBF Mempool Summary

**BIP-125 opt-in RBF implemented via rbf_validate_replacement() in mempool.c — all 5 rules enforced with atomic eviction, inherited signaling check, and zero test regressions**

## Performance

- **Duration:** ~3 min
- **Started:** 2026-02-21T17:33:11Z
- **Completed:** 2026-02-21T17:36:11Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments

- Replaced the TODO stub in the `has_conflict` branch of `mempool_add` with a real 5-rule BIP-125 validation call
- Implemented `entry_signals_rbf_inherited()` — recursive ancestor walk bounded by MEMPOOL_MAX_ANCESTORS (25) that correctly propagates inherited RBF signaling per BIP-125 Rule 1
- Implemented `rbf_collect_eviction_set()` — BFS expansion from direct conflicts to all descendants, enforcing Rule 5 (max 100 replacements) during collection with zero heap allocation (fixed stack array)
- Implemented `rbf_validate_replacement()` checking all 5 BIP-125 rules in order with atomic eviction: collect direct conflict txids before any `mempool_remove` call to prevent spent-table pointer invalidation
- All 25 existing mempool tests pass unchanged with zero compiler warnings

## Task Commits

Each task was committed atomically:

1. **Task 1: Build eviction-set collector and inherited signaling helper** - `2b4f494` (feat)
2. **Task 2: Implement rbf_validate_replacement and wire into mempool_add** - `486954c` (feat)

**Plan metadata:** TBD (docs: complete plan)

## Files Created/Modified

- `src/protocol/mempool.c` — Added three static helpers (`entry_signals_rbf_inherited`, `rbf_collect_eviction_set`, `rbf_validate_replacement`), replaced TODO stub in `mempool_add` with validator call, removed `first_conflict` variable (now managed inside validator)

## Decisions Made

- BIP-125 opt-in RBF (Rules 1-5 as written) rather than full-RBF: phase success criteria check signaling explicitly; full-RBF (dropping Rule 1 signal requirement) is a future policy toggle
- Rule 1 signaling check moved from the conflict detection loop into `rbf_validate_replacement` via `entry_signals_rbf_inherited` — the loop-level check used only `entry->signals_rbf` (direct signaling only), which is incorrect for inherited signaling
- `first_conflict` variable removed from `mempool_add` entirely; `rbf_validate_replacement` populates `result->first_conflict` from `eviction_set[0]` directly, giving correct first-conflict even in multi-conflict scenarios
- `required_fee` uses ceiling integer division `(increment * vsize + 999) / 1000` to avoid returning a required fee below the true minimum

## Deviations from Plan

None — plan executed exactly as written.

The two intermediate compile warnings during Task 1 (`entry_signals_rbf_inherited` not needed, `rbf_collect_eviction_set` unused) were expected: both helpers were unused until Task 2 wired `rbf_validate_replacement` into `mempool_add`. Zero warnings after Task 2 completion.

## Issues Encountered

None.

## Next Phase Readiness

- `rbf_validate_replacement()` is complete and correct; Plan 04-02 can add RBF-specific test coverage against this implementation
- The `test_mempool_conflict_detection` test continues to exercise non-RBF rejection (original tx uses `sequence = 0xFFFFFFFE` which does not signal; Rule 1 fires via `entry_signals_rbf_inherited` returning false)
- No changes to `mempool.h` public API — Plan 04-02 requires no header changes

---
*Phase: 04-bip125-full-rbf-mempool*
*Completed: 2026-02-21*
