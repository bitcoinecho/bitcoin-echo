---
phase: 01-foundation-fixes
plan: 02
subsystem: protocol
tags: [download-mgr, peer-discovery, batch-clone, duplicate-address]

requires:
  - phase: 01-01
    provides: fault logging for diagnostic visibility during IBD
provides:
  - Correct batch remaining count on clone (no false LOG_ERROR)
  - No duplicate peer address connections during IBD
affects: [01-04, 01-05]

tech-stack:
  added: []
  patterns: [bitmap-derived-counts, check-before-mark]

key-files:
  created: []
  modified:
    - src/protocol/download_mgr.c
    - src/app/node.c

key-decisions:
  - "Recalculate remaining from received[] bitmap on clone rather than resetting bitmap"
  - "Keep safety check as LOG_WARN defense-in-depth rather than removing it"
  - "Move duplicate check before mark_address_in_use to eliminate TOCTOU window"
  - "Downgrade duplicate address log to LOG_DEBUG since fix prevents normal occurrence"

patterns-established:
  - "Bitmap-derived counts: remaining must be recalculated from bitmap state, never copied"
  - "Check-before-mark: verify preconditions before claiming resources"

requirements-completed: [BUGF-01, BUGF-02]

duration: 5min
completed: 2026-02-20
---

# Plan 01-02: IBD Bug Fixes Summary

**Batch remaining count recalculated from received[] bitmap on clone; duplicate address check moved before in_use mark**

## Performance

- **Duration:** ~5 min
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- Batch clone recalculates remaining from actual received[] bitmap state, eliminating false LOG_ERROR
- Duplicate address check moved before discovery_mark_address_in_use(), eliminating TOCTOU window
- Safety check downgraded from LOG_ERROR to LOG_WARN (defense-in-depth)
- Duplicate address warning downgraded from LOG_WARN to LOG_DEBUG

## Task Commits

1. **Task 1: Fix batch remaining count mismatch** - `11f43a3` (fix)
2. **Task 2: Fix duplicate peer address race** - `a08876d` (fix)

## Files Created/Modified
- `src/protocol/download_mgr.c` - Recalculate remaining in batch_node_clone(), downgrade safety check
- `src/app/node.c` - Reorder: duplicate check → mark_in_use → connect, downgrade log

## Decisions Made
- Kept the bitmap as source of truth, derived remaining from it (not the other way around)
- No mark_address_free call needed on duplicate skip since mark hasn't been set yet

## Deviations from Plan
None - plan executed as specified.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Clean IBD logs enable meaningful error detection for subsequent plans
- Download manager batch handling is now correct for flush-before-index work (01-04)

---
*Phase: 01-foundation-fixes*
*Completed: 2026-02-20*
