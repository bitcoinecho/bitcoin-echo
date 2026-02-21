---
phase: 05-storage-layer-and-core-rpc
plan: 02
subsystem: rpc
tags: [rpc, getrawtransaction, getblock, getblockchaininfo, txindex, block_storage, mtp, mediantime]

# Dependency graph
requires:
  - phase: 05-01
    provides: "tx_index table + txindex_lookup/insert/delete CRUD in block_index_db"
provides:
  - "getrawtransaction confirmed tx lookup via txindex_lookup + block_storage_read + block_parse"
  - "getblock verbosity=0 raw witness-serialized block hex from disk"
  - "getblockchaininfo mediantime as true MTP from in-memory block_index_map walk"
affects: [phase-06-getblocktemplate]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Prune-check before disk read: check BLOCK_STATUS_PRUNED + BLOCK_STATUS_HAVE_DATA before block_storage_read to return NOT_FOUND cleanly"
    - "Tx retrieval from confirmed block: txindex_lookup -> block_index_db_lookup_by_hash (prune check) -> block_storage_read -> block_parse -> tx scan by txid"
    - "MTP walk: iterate in-memory block_index_map up to 11 blocks via cur->prev_hash; insertion sort; median at ts_count/2"

key-files:
  created: []
  modified:
    - "src/app/rpc.c - Three TODO stubs replaced: getrawtransaction confirmed path, getblock v=0 raw hex, getblockchaininfo mediantime MTP"

key-decisions:
  - "txindex stores block-level file position only; tx extraction reads full block, parses, and scans — acceptable O(block) per RPC call for Phase 5; tx_offset optimization deferred"
  - "No re-serialization needed for getblock v=0: block_storage_read returns witness-serialized bytes (node_store_block always calls block_serialize with ECHO_TRUE)"
  - "cons_mut (non-const consensus_engine_t*) required for consensus_get_chainstate — use node_get_consensus(node) not the const consensus already in scope"

patterns-established:
  - "Prune-check pattern: always check (BLOCK_STATUS_PRUNED || !BLOCK_STATUS_HAVE_DATA || data_file < 0) before block_storage_read in RPC handlers"
  - "MTP pattern: block_index_map in-memory walk + insertion sort for 11-element timestamp median, guarded by height==0 to avoid walking past genesis"

requirements-completed: [RPC-02, RPC-03, RPC-04]

# Metrics
duration: 9min
completed: 2026-02-21
---

# Phase 5 Plan 02: RPC Stub Wire-Up Summary

**getrawtransaction via tx_index, getblock v=0 raw hex, and mediantime MTP — three TODO stubs replaced with working implementations, zero new files**

## Performance

- **Duration:** ~9 min
- **Started:** 2026-02-21T00:00:00Z
- **Completed:** 2026-02-21T00:09:00Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments
- getrawtransaction now looks up confirmed transactions via txindex_lookup, reads the containing block, parses it, finds the tx by txid comparison, and returns witness-inclusive hex
- getblock verbosity=0 reads raw block bytes from disk via block_index_db_lookup_by_hash + block_storage_read and returns witness-serialized hex; pruned/header-only blocks return not-found
- getblockchaininfo mediantime computes real MTP (Median Time Past) by walking up to 11 blocks back through the in-memory block_index_map, sorting timestamps, and taking the median — no longer returns 0

## Task Commits

Each task was committed atomically:

1. **Task 1: Wire getblock verbosity=0 and mediantime MTP** - `ea12340` (feat)
2. **Task 2: Wire getrawtransaction for confirmed transactions via tx_index** - `78cca15` (feat)

**Plan metadata:** (docs commit, see below)

## Files Created/Modified
- `/Users/yayseth/Projects/echo/bitcoin-echo/src/app/rpc.c` - Three TODO stubs replaced; 135 lines added, 7 removed

## Decisions Made
- txindex stores only block-level file position (not per-tx offset): extracting a tx requires reading the full block and scanning — O(block_size) per call, acceptable for Phase 5; per-tx offset optimization is a future improvement
- No re-serialization needed for getblock v=0: confirmed by research that node_store_block always calls block_serialize with ECHO_TRUE, so block_storage_read bytes are already witness-serialized
- consensus_get_chainstate requires non-const engine: used node_get_consensus(node) in a new cons_mut local variable rather than the const consensus already in function scope

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 5 RPC surface is complete: operators can now use getrawtransaction, getblock v=0, and getblockchaininfo with correct mediantime
- Phase 6 (getblocktemplate) can proceed; the pre-check concern about witness commitment format should be verified against a live Bitcoin Core node before implementation

---
*Phase: 05-storage-layer-and-core-rpc*
*Completed: 2026-02-21*
