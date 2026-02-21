---
phase: 02-consensus-completeness
plan: 02
subsystem: consensus/chainstate
tags: [reorg, chainwork, utxo-rollback, delta, consensus]
requirements: [CONS-02, CONS-04]

dependency_graph:
  requires: []
  provides: [CONS-02, CONS-04]
  affects: [chainstate, chaser_confirm, chain_reorganize]

tech_stack:
  added: []
  patterns:
    - Chainstate-owned delta lifetime (state->deltas[] primary owner, delta_out is borrow)
    - Reverse-order UTXO revert loop in chaser_confirm_reorganize

key_files:
  created: []
  modified:
    - include/chainstate.h
    - src/consensus/chainstate.c
    - src/consensus/consensus.c
    - src/node/chaser_confirm.c
    - test/unit/test_chainstate.c
    - test/unit/test_chase.c

decisions:
  - "Chainstate owns deltas in state->deltas[]; delta_out parameter is a borrowed (non-owning) reference — callers must not free it"
  - "chainstate_apply_block_with_txids stores delta in state->deltas[] and returns borrowed pointer via delta_out, removing need for callers to manually assign"
  - "chainstate_revert_block does NOT free the delta; caller uses chainstate_prune_delta_at() for explicit release"
  - "chaser_confirm_reorganize gracefully skips UTXO rollback when chainstate is NULL (test/early-init paths)"
  - "consensus.c apply path changed from NULL to &delta_borrow so deltas are stored for reorg rollback (IBD concern noted but correct for consensus safety)"

metrics:
  duration: ~25 min
  completed: 2026-02-21T03:49:06Z
  tasks: 2
  files_modified: 6
---

# Phase 02 Plan 02: Chain Reorganization Chainwork and UTXO Rollback Summary

Wired full reorg rollback: prev_chainwork field in block_delta_t saves/restores accumulated work, and chaser_confirm_reorganize now calls chainstate_revert_block in reverse-height order instead of a TODO stub.

## What Was Built

### Task 1: prev_chainwork field and chainstate_get_delta accessor

**`include/chainstate.h`**

Added `work256_t prev_chainwork` field to `block_delta_t` after `spent_count`:

```c
work256_t prev_chainwork; /* Chainwork before this block was applied (for reorg restoration) */
```

Added `chainstate_get_delta` public declaration with full doc comment explaining ownership (borrowed pointer, chainstate owns).

**`src/consensus/chainstate.c`**

In `chainstate_apply_block_with_txids`:
- Save `state->tip.chainwork` into `delta->prev_chainwork` before `work256_add` (so the delta records the value before the block's contribution is added)
- Store the created delta in `state->deltas[new_height]` with capacity growth, taking ownership
- Return borrowed pointer via `delta_out` — callers must NOT free it

In `chainstate_revert_block`:
- Replaced the TODO stub with `state->tip.chainwork = delta->prev_chainwork` for exact chainwork restoration
- Added `height_index[delta->height]` zero-clear to prevent stale orphan entries

Added `chainstate_get_delta` implementation with bounds checking: rejects heights above tip, heights outside `DELTA_REORG_DEPTH` window, and heights beyond `deltas_capacity`.

Updated `chain_reorganize` disconnect path to use `chainstate_prune_delta_at` instead of raw `block_delta_destroy` (matches new ownership model).

**`src/consensus/consensus.c`**

Changed `apply_block_internal` to pass `&delta_borrow` instead of `NULL` for `delta_out`. This triggers delta creation and storage in `state->deltas[]` during all block application, enabling reorg rollback even for blocks applied through the normal confirm path.

**`test/unit/test_chainstate.c`**

Removed `block_delta_destroy(delta)` calls for deltas returned by `chainstate_apply_block` — chainstate now owns them and `chainstate_destroy` frees them. Added clarifying comments.

### Task 2: Wire UTXO rollback loop in chaser_confirm_reorganize

**`src/node/chaser_confirm.c`**

Replaced the TODO stub with a complete revert loop:

```c
if (chaser->chainstate != NULL) {
    for (uint32_t h = old_height; h > fork_point; h--) {
        const block_delta_t *delta = chainstate_get_delta(chaser->chainstate, h);
        if (delta == NULL) {
            log_error(..., "missing delta at height %u during reorg", h);
            chaser_unlock(&chaser->base);
            return false;
        }
        echo_result_t result = chainstate_revert_block(chaser->chainstate, delta);
        if (result != ECHO_OK) {
            log_error(..., "failed to revert block at height %u", h);
            chaser_unlock(&chaser->base);
            return false;
        }
        chainstate_prune_delta_at(chaser->chainstate, h);
        chaser_notify_height(&chaser->base, CHASE_REORGANIZED, h);
    }
} else {
    /* NULL chainstate: height-only tracking (tests / early init) */
    for (uint32_t h = old_height; h > fork_point; h--) {
        chaser_notify_height(&chaser->base, CHASE_REORGANIZED, h);
    }
}
```

**`test/unit/test_chase.c`**

Added stubs for `chainstate_get_delta`, `chainstate_revert_block`, `chainstate_prune_delta_at` matching the existing stub pattern (void* types, no-op bodies). The NULL-returning `chainstate_get_delta` stub is harmless because the test passes NULL chainstate — the `chaser->chainstate != NULL` guard in the new code ensures the rollback loop is skipped entirely.

## Verification Results

```
grep -n "prev_chainwork" include/chainstate.h src/consensus/chainstate.c
  include/chainstate.h:104:  work256_t prev_chainwork; (field)
  chainstate.c:689:  delta->prev_chainwork = state->tip.chainwork; (save in apply)
  chainstate.c:786:  state->tip.chainwork = delta->prev_chainwork; (restore in revert)

grep -n "chainstate_revert_block" src/node/chaser_confirm.c
  line 278: chainstate_revert_block(chaser->chainstate, delta);

grep -n "chainstate_get_delta" include/chainstate.h src/consensus/chainstate.c src/node/chaser_confirm.c
  chainstate.h:355: declaration
  chainstate.c:828: implementation
  chaser_confirm.c:266: usage in reorg loop

grep -n "TODO" src/node/chaser_confirm.c | grep -v top_checkpoint
  (no output — TODO stub removed)

make test: ALL 1095 TESTS PASSED
```

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Delta ownership semantics were undefined**
- **Found during:** Task 1
- **Issue:** `chainstate_apply_block_with_txids` returned a delta to the caller but didn't store it internally, making reorg rollback impossible via `chainstate_get_delta`. `chain_reorganize` worked around this by manually assigning `state->deltas[h] = delta`, but the normal confirm path (consensus.c) passed NULL.
- **Fix:** Changed `chainstate_apply_block_with_txids` to store the delta in `state->deltas[]` and take ownership. `delta_out` becomes a borrowed reference. Updated all callers accordingly (test cleanup + chain_reorganize simplification).
- **Files modified:** `src/consensus/chainstate.c`, `test/unit/test_chainstate.c`
- **Commit:** bbcff9f

**2. [Rule 2 - Missing critical functionality] test_chase stubs needed for new chainstate calls**
- **Found during:** Task 2
- **Issue:** `test_chase` links without `chainstate.c` and uses stub functions. New calls to `chainstate_get_delta`, `chainstate_revert_block`, `chainstate_prune_delta_at` in `chaser_confirm.c` caused linker errors.
- **Fix:** Added matching stubs to `test_chase.c`. NULL-returning `chainstate_get_delta` stub is harmless because the test creates chasers with NULL chainstate, triggering the graceful-skip path.
- **Files modified:** `test/unit/test_chase.c`
- **Commit:** 055d445

**3. [Rule 1 - Bug] Existing test_chase reorg tests broke due to new delta-required logic**
- **Found during:** Task 2
- **Issue:** `test_chaser_confirm_reorg` passes NULL for chainstate. The new reorg loop tried to call `chainstate_get_delta(NULL, h)` which returned NULL, causing the function to return false (error) instead of succeeding.
- **Fix:** Added `if (chaser->chainstate != NULL)` guard. When NULL, fallback to height-only notification (original behavior). When non-NULL, require deltas for correct rollback.
- **Files modified:** `src/node/chaser_confirm.c`
- **Commit:** 055d445

## Self-Check: PASSED

All key files exist and both commits (bbcff9f, 055d445) are present in git history.
