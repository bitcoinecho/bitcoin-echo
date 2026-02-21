---
phase: 05-storage-layer-and-core-rpc
plan: 01
subsystem: database
tags: [sqlite, txindex, block-index, reorg]

requires:
  - phase: 04-mempool-and-relay
    provides: consensus apply/revert infrastructure
provides:
  - tx_index table in block_index.db mapping txid to block file position
  - txindex_insert, txindex_lookup, txindex_delete_by_block, txindex_insert_block functions
  - automatic txindex population on block apply
  - automatic txindex cleanup on reorg
affects: [05-02-rpc-implementations, getrawtransaction]

tech-stack:
  added: []
  patterns: [prepared-statement-lifecycle, mutex-protected-db-access]

key-files:
  created: []
  modified:
    - include/block_index_db.h
    - src/storage/block_index_db.c
    - src/app/node.c
    - src/node/chaser_confirm.c

key-decisions:
  - "Store txid in internal byte order (as returned by tx_compute_txid) for consistency with rpc_parse_hash"
  - "Use INSERT OR REPLACE for txindex to handle edge cases during rapid reorg sequences"
  - "Access block_index_db from chaser_confirm via chaser->base.node (base chaser already stores node pointer)"
  - "txindex deletion is best-effort during reorg — log warning and continue if it fails"

patterns-established:
  - "txindex CRUD: same prepared-statement + mutex pattern as existing blocks table"
  - "block_hash index on tx_index for O(block_tx_count) deletes during reorg"

requirements-completed: [RPC-01]

duration: 5min
completed: 2026-02-21
---

# Plan 05-01: tx_index Table and Integration Hooks Summary

**Persistent transaction index (tx_index) in block_index.db with automatic population on block apply and cleanup on reorg**

## Performance

- **Duration:** 5 min
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- tx_index table with txid (BLOB PK), block_hash, file_index, file_pos columns and block_hash index
- Four txindex functions (insert, lookup, delete_by_block, insert_block) with full mutex protection and prepared statement lifecycle
- node_apply_block automatically indexes all transactions after consensus succeeds, looking up file position from block_index_db
- chaser_confirm_reorganize deletes txindex entries for each disconnected block before reverting in-memory UTXO state

## Task Commits

Each task was committed atomically:

1. **Task 1: Add tx_index schema and CRUD functions** - `3d941c8` (feat)
2. **Task 2: Wire txindex population into apply and deletion into reorg** - `32dcbcb` (feat)

## Files Created/Modified
- `include/block_index_db.h` - Added txindex_insert, txindex_lookup, txindex_delete_by_block, txindex_insert_block declarations and prepared statement fields
- `src/storage/block_index_db.c` - tx_index schema creation, prepared statements, all four txindex function implementations with mutex protection
- `src/app/node.c` - txindex_insert_block call in node_apply_block after consensus succeeds
- `src/node/chaser_confirm.c` - txindex_delete_by_block call in reorg loop before UTXO revert

## Decisions Made
- Store txid in internal byte order for consistency with rpc_parse_hash display-to-internal conversion
- Use INSERT OR REPLACE to handle reorg edge cases where same txid appears in multiple blocks briefly
- Access block_index_db from chaser_confirm via chaser->base.node pointer (no struct changes needed)
- txindex deletion is best-effort during reorg — failure logged as warning, does not abort reorg

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- tx_index infrastructure complete, ready for RPC implementations in Plan 05-02
- getrawtransaction can now use txindex_lookup to find confirmed transactions

---
*Phase: 05-storage-layer-and-core-rpc*
*Completed: 2026-02-21*
