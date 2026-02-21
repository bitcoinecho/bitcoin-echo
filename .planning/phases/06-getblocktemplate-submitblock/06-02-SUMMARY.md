---
phase: 06-getblocktemplate-submitblock
plan: 02
subsystem: rpc
tags: [rpc, mining, submitblock, ibd-guard, peer-announcement, block-propagation]

# Dependency graph
requires:
  - phase: 06-01
    provides: [ECHO_ERR_INVALID_STATE-convention, rpc_execute_single -28 mapping, getblocktemplate-production]
provides:
  - submitblock-production
  - ibd-guard-submitblock
  - peer-announcement-on-block-acceptance
affects: [rpc.c, rpc_submitblock]

# Tech tracking
tech-stack:
  added: []
  patterns: [IBD-guard-at-function-top, announce-before-free, block_header_hash-then-announce]

key-files:
  created: []
  modified:
    - src/app/rpc.c

key-decisions:
  - "IBD guard for submitblock returns ECHO_ERR_INVALID_STATE (same -28 convention as getblocktemplate) — miners cannot build on incomplete chain during sync."
  - "block_free() moved after node_announce_block_to_peers() — announcement references block.header so free must come last to avoid use-after-free."

patterns-established:
  - "IBD guard pattern: check node_is_ibd_mode() immediately after NULL checks, return ECHO_ERR_INVALID_STATE for -28 RPC error."
  - "Post-apply announcement pattern: compute block_header_hash then call node_announce_block_to_peers before block_free."

requirements-completed: [RPC-06]

# Metrics
duration: 1min
completed: 2026-02-21
---

# Phase 06 Plan 02: submitblock Production Implementation Summary

**Production submitblock with IBD guard (ECHO_ERR_INVALID_STATE -> -28) and peer announcement via node_announce_block_to_peers after successful node_apply_block.**

## Performance

- **Duration:** ~1 min
- **Started:** 2026-02-21T20:43:12Z
- **Completed:** 2026-02-21T20:44:08Z
- **Tasks:** 1
- **Files modified:** 1

## Accomplishments
- Added IBD guard at top of rpc_submitblock — returns ECHO_ERR_INVALID_STATE which rpc_execute_single maps to RPC error -28
- Added block hash computation and node_announce_block_to_peers call after successful node_apply_block
- Moved block_free() after the announcement to avoid use-after-free (block.header referenced in hash computation)

## Task Commits

Each task was committed atomically:

1. **Task 1: Add IBD guard and peer announcement to submitblock** - `d39e504` (feat)

**Plan metadata:** (docs commit — see below)

## Files Created/Modified
- `src/app/rpc.c` - Added IBD guard and peer announcement in rpc_submitblock

## Decisions Made
- IBD guard for submitblock uses the same `ECHO_ERR_INVALID_STATE -> -28` convention established in Plan 06-01. Miners must not build on incomplete chain — consistent with getblocktemplate behavior.
- `block_free()` is called after `node_announce_block_to_peers()` because the announcement computes the block hash from `block.header`. Freeing first would be use-after-free.

## Deviations from Plan

None — plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Phase 06 is complete. Both getblocktemplate and submitblock are production-ready.
- getblocktemplate provides BIP-22/BIP-145 templates with IBD guard, MTP, witness commitment, and correct retarget-aware difficulty bits.
- submitblock provides IBD guard and peer propagation after block acceptance.
- All v1.1 mining RPC endpoints are fully implemented.

## Self-Check: PASSED

---
*Phase: 06-getblocktemplate-submitblock*
*Completed: 2026-02-21*
