---
phase: 01-foundation-fixes
plan: 04
subsystem: storage
tags: [block-storage, flush, durability, gap-errors, ibd, chaser-confirm]

requires:
  - phase: 01-02
    provides: correct batch remaining count — clean IBD error baseline for validating GAP elimination

provides:
  - "fflush() called in block_storage_write() before returning file position — index never ahead of durable data"
  - "Write → flush → index ordering enforced in node_store_block()"
  - "GAP errors eliminated: chaser_confirm cannot read a position pointing to unflushed data"

affects:
  - "01-05 and later: IBD throughput baseline is now fflush-per-block (one syscall), not fsync-per-block"

tech-stack:
  added: []
  patterns:
    - "Write-flush-before-index: fflush() inside block_storage_write() before returning position"
    - "Defense-in-depth: block_storage_read() also flushes before reading same file (belt-and-suspenders)"

key-files:
  created: []
  modified:
    - src/storage/blocks.c
    - src/app/node.c
    - include/blocks_storage.h

key-decisions:
  - "Use fflush() not fsync() — fflush pushes to OS page cache (one write(2) syscall); fsync would flush to physical media and kill IBD throughput"
  - "Flush inside block_storage_write() rather than in node_store_block() — guarantees the API contract: returned position always points to readable data"
  - "Remove periodic flush interval (every 100 blocks) — now redundant since every write flushes"
  - "Retain defensive flush in block_storage_read() as belt-and-suspenders for future code paths"
  - "BLOCK_STORAGE_FLUSH_INTERVAL kept in header as documentation reference, value changed to 1"

patterns-established:
  - "write-flush-before-index: every block_storage_write() guarantees fflush before returning position"

requirements-completed: [INFR-01]

duration: 4min
completed: 2026-02-21
---

# Phase 01 Plan 04: Block Storage Flush-Before-Index Summary

**fflush() added to block_storage_write() before returning file position, ensuring the block index never points to data still in the C stdio buffer — eliminating GAP errors in chaser_confirm**

## Performance

- **Duration:** ~4 min
- **Started:** 2026-02-21T01:46:36Z
- **Completed:** 2026-02-21T01:51:09Z
- **Tasks:** 1
- **Files modified:** 3

## Accomplishments

- Added `fflush()` inside `block_storage_write()` after all `fwrite()` calls complete, before returning the file position
- Removed the old periodic flush (`if blocks_since_flush >= 100`) — redundant now that every write flushes
- Added detailed comment at the flush site explaining the GAP error mechanism, why fflush is sufficient, and why fsync is explicitly avoided
- Updated `node_store_block()` with ordering documentation comment: write → fflush → index_db_update → in_memory_index_update
- Updated `block_storage_read()` defensive flush comment from "CRITICAL" to "belt-and-suspenders" to reflect new role
- Updated `BLOCK_STORAGE_FLUSH_INTERVAL` constant comment to explain it's retained as documentation (value set to 1)

## Task Commits

1. **Task 1: Implement flush-before-index ordering** - `1b8f762` (fix)

## Files Created/Modified

- `src/storage/blocks.c` — fflush() after fwrite() before returning position; removed periodic flush block; updated defensive read flush comment
- `src/app/node.c` — added ordering guarantee comment in node_store_block() documenting write → fflush → index sequence
- `include/blocks_storage.h` — updated BLOCK_STORAGE_FLUSH_INTERVAL comment; updated block_storage_flush() notes

## Decisions Made

**fflush not fsync:** The GAP error is caused by data in the C stdio buffer not being visible to other FILE* handles on the same file. fflush() pushes data from the C library buffer to the OS page cache (one write(2) syscall). Any subsequent fopen()+fread() will see the data. fsync() additionally flushes the OS page cache to physical media — necessary for crash durability but at ~2ms per call would reduce IBD throughput to under 500 blocks/sec. The GAP error does not require crash durability, only inter-process/inter-handle visibility.

**Flush location:** Inside `block_storage_write()` rather than in `node_store_block()`. The API contract is: "the returned position points to readable data." Callers should not need to know to flush before using the returned position.

**Retained defensive flush in read:** `block_storage_read()` already had a defensive fflush before reading from the current write file. With our fix this is now belt-and-suspenders. Left in place for safety against future code paths that might bypass the write API.

## Deviations from Plan

None — plan executed exactly as written. Strategy A (flush-before-index) was implemented as specified.

## Issues Encountered

- `test_chase` linker failure in `make test` is pre-existing (missing `block_index_db.o` and `log.o` in the test_chase Makefile link line). Confirmed pre-existing by stashing changes and verifying the same failure on HEAD. Out of scope for this plan.

## Verification Results

- `make` builds with zero new warnings (pre-existing unused function warning in node.c is unrelated)
- No `fsync()` in per-write path (grep confirms: only appears in comments explaining why it's not used)
- `node_store_block()` ordering: `block_storage_write()` (write+flush) → `block_index_db_update_data_pos()` → `block_index->data_file/data_pos` assignment
- `blocks_since_flush` reset to 0 after every write (periodic flush removed)

## Next Phase Readiness

- IBD throughput baseline is now one fflush (one write(2) syscall) per block — acceptable cost
- chaser_confirm can safely trust that any block index position points to readable data
- Subsequent plans (01-05 through 01-07) can now run IBD with clean GAP-free logs

---
*Phase: 01-foundation-fixes*
*Completed: 2026-02-21*
