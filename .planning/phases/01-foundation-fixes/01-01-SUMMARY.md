---
phase: 01-foundation-fixes
plan: 01
subsystem: infra
tags: [chaser, logging, block-index, checkpoint, validation]

requires:
  - phase: none
    provides: n/a
provides:
  - Fault logging in chaser_fault() with error detail
  - Real block hash retrieval in CHASE_CHECKED validation path
  - checkpoint_height config field wired to chaser_validate
affects: [01-02, 01-04, 01-05]

tech-stack:
  added: []
  patterns: [log-before-shutdown, config-driven-checkpoint]

key-files:
  created: []
  modified:
    - src/node/chaser.c
    - src/node/chaser_validate.c
    - include/node.h
    - src/app/node.c

key-decisions:
  - "Used log_error before CHASE_STOP because log flush is not guaranteed after stop signal"
  - "checkpoint_height=0 means use PLATFORM_ASSUMEVALID_HEIGHT (backward compatible default)"
  - "Checkpoint wired post-creation via chaser_validate_set_checkpoint() rather than at init — matches existing node.c pattern"

patterns-established:
  - "Log before signal: diagnostic logging must precede shutdown signals"
  - "Config-driven bypass: validation bypass heights come from node_config_t, not hardcoded"

requirements-completed: [INFR-04, INFR-02, INFR-03]

duration: 5min
completed: 2026-02-20
---

# Plan 01-01: Infrastructure Stubs Summary

**Chaser fault logging, real block hash retrieval in CHASE_CHECKED, and checkpoint_height config wiring**

## Performance

- **Duration:** ~5 min
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- chaser_fault() now logs error detail (chaser name + error code) before dispatching CHASE_STOP
- CHASE_CHECKED path retrieves real block hash via block_index_db_lookup_by_height() instead of submitting zero-hash
- node_config_t has checkpoint_height field, defaulting to 0, wired through node_start() to chaser_validate_set_checkpoint()

## Task Commits

1. **Task 1: Wire fault logging and block hash retrieval** - `fbd0f97` (fix)
2. **Task 2: Add checkpoint_height to node config and wire to chaser** - `97d4acf` (fix)

## Files Created/Modified
- `src/node/chaser.c` - Added log_error() in chaser_fault() before CHASE_STOP
- `src/node/chaser_validate.c` - Added block_index_db_lookup_by_height() in CHASE_CHECKED path; updated TODO comment
- `include/node.h` - Added checkpoint_height field to node_config_t
- `src/app/node.c` - Initialized checkpoint_height=0; wired config value into checkpoint selection logic

## Decisions Made
- Used log_error (not log_warn) for faults — faults are always errors that trigger shutdown
- Kept zero-hash as fallback when block_index_db_lookup fails (log_warn instead of error)
- Config checkpoint_height overrides PLATFORM_ASSUMEVALID_HEIGHT when non-zero

## Deviations from Plan
None - plan executed as specified.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Fault logging provides diagnostic visibility for debugging IBD issues in subsequent plans
- Real block hashes enable correct block identity tracking in validation pipeline
- Checkpoint config field ready for CLI argument wiring in future phases

---
*Phase: 01-foundation-fixes*
*Completed: 2026-02-20*
