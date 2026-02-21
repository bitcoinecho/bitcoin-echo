---
phase: 01-foundation-fixes
plan: 05
subsystem: protocol
tags: [download-manager, peer-eviction, ibd, logging, calibration]

# Dependency graph
requires:
  - phase: 01-02
    provides: Download manager IBD bug fixes that established the eviction framework

provides:
  - Calibrated DOWNLOAD_MIN_RATE_BYTES_PER_SEC constant (1 KB/s) with documented rationale
  - Debug-level eviction logging in check_performance and evict_slowest_percent
  - Calibration comment referencing Bitcoin Core nMinExpectedRate and mainnet block size distribution

affects:
  - Phase 2 planning: eviction threshold is now conservative and documented — no need to revisit
  - Future IBD operators: eviction events are debug-only, invisible in normal output

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Eviction constants documented with calibration rationale and re-evaluation guidance"
    - "Routine operational events at LOG_DEBUG, exceptional conditions at LOG_WARN/LOG_INFO"

key-files:
  created: []
  modified:
    - include/download_mgr.h
    - src/protocol/download_mgr.c

key-decisions:
  - "DOWNLOAD_MIN_RATE_BYTES_PER_SEC set to 1024 (1 KB/s) — conservative threshold based on Bitcoin Core nMinExpectedRate behavior and mainnet block size distribution; 3072 was arbitrary"
  - "All routine eviction events use LOG_DEBUG only — invisible during normal IBD, available for diagnostics"
  - "Stall detection events (check_stall) remain at LOG_INFO/LOG_WARN — these are exceptional, not routine"

patterns-established:
  - "Calibration pattern: constants that encode mainnet behavior decisions must have multi-line comment with measurement methodology, reference source, and re-evaluation trigger"
  - "Log level discipline: eviction = debug, validation stall = info/warn, internal errors = warn/error"

requirements-completed: [INFR-05]

# Metrics
duration: 8min
completed: 2026-02-20
---

# Phase 01 Plan 05: Eviction Threshold Calibration Summary

**Calibrated peer eviction threshold to 1 KB/s with documented Bitcoin Core rationale, and wired debug-level logging for all routine eviction events**

## Performance

- **Duration:** ~8 min
- **Started:** 2026-02-20T00:00:00Z
- **Completed:** 2026-02-20T00:08:00Z
- **Tasks:** 1
- **Files modified:** 2

## Accomplishments
- Replaced arbitrary 3072 B/s (3 KB/s) threshold with calibrated 1024 B/s (1 KB/s) backed by Bitcoin Core nMinExpectedRate analysis and mainnet block size distribution rationale
- Added multi-line calibration comment documenting measurement methodology, block size range (250 bytes early to 4 MB modern), and explicit note to re-evaluate after first full mainnet IBD
- Downgraded all routine eviction LOG_INFO/LOG_WARN calls to LOG_DEBUG — stalled peer eviction, slow peer eviction, and evict_slowest_percent summary
- Added per-eviction LOG_DEBUG with rate context (actual rate, threshold, reporter count, min_keep)
- Added eviction-skip LOG_DEBUG when minimum peer count constraint prevents eviction
- Updated Phase 4 inline comment to reference 1 KB/s and point to header for full rationale

## Task Commits

1. **Task 1: Calibrate eviction threshold and add debug-level eviction logging** - `ec4248a` (fix)

## Files Created/Modified
- `/Users/yayseth/Projects/echo/bitcoin-echo/include/download_mgr.h` - DOWNLOAD_MIN_RATE_BYTES_PER_SEC updated from 3072 to 1024 with 17-line calibration rationale comment
- `/Users/yayseth/Projects/echo/bitcoin-echo/src/protocol/download_mgr.c` - All routine eviction events downgraded to LOG_DEBUG; per-eviction debug logs added with rate and peer-count context

## Decisions Made
- 1 KB/s chosen over 2 KB/s: Bitcoin Core's minimum rate is effectively around 1 KB/s; lower is safer for conservative IBD behavior. The stall timeout mechanism handles peers that are slow but non-zero.
- Stall detection events in `check_stall()` intentionally left at LOG_INFO/LOG_WARN — those represent exceptional conditions (validation pipeline stuck), not routine peer-selection events.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

The `test_chase` linker failure (`_block_index_db_lookup_by_height` undefined) is pre-existing and unrelated to this plan's changes. All other tests compile and link correctly.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Eviction threshold is now calibrated and documented — no further work needed on this constant until post-IBD measurement
- Eviction logging is debug-only, confirming IBD output will be clean
- Plans 01-06 and 01-07 can proceed independently

---
*Phase: 01-foundation-fixes*
*Completed: 2026-02-20*
