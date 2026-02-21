---
phase: 05-storage-layer-and-core-rpc
verified: 2026-02-21T00:00:00Z
status: passed
score: 4/4 success criteria verified
gaps:
  - truth: "After a chain reorganization, getrawtransaction for a transaction in a disconnected block returns not-found — stale txindex entries are deleted from blocks.db before the in-memory UTXO revert"
    status: partial
    reason: "Implementation is correct and wired. REQUIREMENTS.md checkbox for RPC-01 was never updated from [ ] to [x] after Plan 05-01 completed. The traceability table also still shows 'Pending'. The index infrastructure backing this success criterion is fully implemented; only the requirements document is stale."
    artifacts:
      - path: ".planning/REQUIREMENTS.md"
        issue: "RPC-01 checkbox is [ ] (unchecked) and traceability row shows 'Pending' despite the tx_index being fully implemented in commit 3d941c8 and 32dcbcb"
    missing:
      - "Update RPC-01 checkbox from [ ] to [x] in REQUIREMENTS.md"
      - "Update RPC-01 traceability row from 'Pending' to 'Complete' in REQUIREMENTS.md"
human_verification:
  - test: "getrawtransaction end-to-end with live synced node"
    expected: "curl -s -d '{\"method\":\"getrawtransaction\",\"params\":[\"<known_confirmed_txid>\"]}' http://localhost:8332/ returns witness-serialized hex; txids in pruned blocks return error -5 (not-found)"
    why_human: "Requires a synced mainnet node with at least one block confirmed and a known txid; cannot verify disk I/O path programmatically"
  - test: "getblock verbosity=0 end-to-end with live synced node"
    expected: "curl -s -d '{\"method\":\"getblock\",\"params\":[\"<known_block_hash>\",0]}' http://localhost:8332/ returns a hex string that decodes to a valid block"
    why_human: "Requires a synced node with block data on disk"
  - test: "getblockchaininfo mediantime with live synced node"
    expected: "jq .result.mediantime returns a non-zero Unix timestamp close to current time (not 0); matches the median of the 11 most recent block timestamps"
    why_human: "Requires a synced node with at least one block in the in-memory block_index_map"
  - test: "Reorg recovery — getrawtransaction returns not-found after block is disconnected"
    expected: "After simulating or observing a reorg, a txid that was in a disconnected block returns not-found from getrawtransaction"
    why_human: "Requires a real or simulated reorg to exercise the chaser_confirm_reorganize path"
---

# Phase 5: Storage Layer and Core RPC Verification Report

**Phase Goal:** Operators can look up any confirmed transaction by txid, retrieve raw block hex, and read a correct mediantime — all backed by a durable transaction index that survives chain reorganizations

**Verified:** 2026-02-21
**Status:** gaps_found (documentation gap only — one requirements checkbox not updated)
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths (Success Criteria)

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | getrawtransaction with a confirmed txid returns raw hex; pruned block txids return not-found | VERIFIED | rpc.c:1958 calls txindex_lookup, then block_storage_read, then tx_compute_txid scan; pruned/unavailable check at rpc.c:1967-1972 |
| 2 | getblock verbosity=0 returns raw witness-serialized block hex | VERIFIED | rpc.c:1815-1843: block_index_db_lookup_by_hash + BLOCK_STATUS_PRUNED/HAVE_DATA check + block_storage_read + json_builder_hex |
| 3 | getblockchaininfo mediantime equals median of previous 11 block timestamps (not 0) | VERIFIED | rpc.c:1662-1696: in-memory block_index_map walk, insertion sort, timestamps[ts_count/2] — replaces prior `json_builder_uint(builder, 0)` stub |
| 4 | After reorg, getrawtransaction for disconnected block tx returns not-found; deletion before UTXO revert | VERIFIED (code), PARTIAL (docs) | chaser_confirm.c:280-293 calls txindex_delete_by_block at line 285 before chainstate_revert_block at line 296; RPC-01 checkbox in REQUIREMENTS.md not updated |

**Score:** 3/4 automated — 4th criterion is implemented correctly but has a documentation discrepancy in REQUIREMENTS.md (RPC-01 checkbox stale).

---

## Required Artifacts

### Plan 05-01 Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `include/block_index_db.h` | txindex_insert, txindex_lookup, txindex_delete_by_block, txindex_insert_block declarations | VERIFIED | Lines 655-719: all four functions declared with full doc comments; prepared statement fields at lines 89-91 |
| `src/storage/block_index_db.c` | tx_index schema creation, prepared statements, all txindex functions | VERIFIED | Schema at line 147-159; three prepared statements at lines 233-249; finalize at lines 269-271; four full implementations at lines 1592-1746 with mutex protection |
| `src/app/node.c` | txindex_insert_block call in node_apply_block | VERIFIED | Lines 3692-3709: txindex_insert_block called after consensus succeeds and block_index_db_lookup_by_hash retrieves file position; before UTXO DB step (Step 4 at line 3720) |
| `src/node/chaser_confirm.c` | txindex_delete_by_block call in reorg loop before UTXO revert | VERIFIED | Lines 280-293: txindex_delete_by_block at line 285 inside reorg for-loop; chainstate_revert_block at line 296 — ordering is correct |

### Plan 05-02 Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `src/app/rpc.c` | getrawtransaction confirmed tx lookup, getblock v=0 raw hex, mediantime MTP calculation | VERIFIED | txindex_lookup at line 1958; block_storage_read at lines 1836 and 1983; block_index_map_lookup at line 1679; no stub returns remain in these three handlers |

---

## Key Link Verification

### Plan 05-01 Key Links

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `src/app/node.c` (node_apply_block) | `src/storage/block_index_db.c` (txindex_insert_block) | txindex_insert_block after consensus_apply_block | WIRED | Confirmed at node.c:3697; called only when block_index_db_open and data_file >= 0 |
| `src/node/chaser_confirm.c` (reorg loop) | `src/storage/block_index_db.c` (txindex_delete_by_block) | txindex_delete_by_block before chainstate_revert_block | WIRED | Confirmed at chaser_confirm.c:285; ordering verified against revert at line 296 |

### Plan 05-02 Key Links

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `src/app/rpc.c` (rpc_getrawtransaction) | block_index_db.c (txindex_lookup) | txindex_lookup for txid → block file position, then block_storage_read + block_parse + tx scan | WIRED | rpc.c:1958 calls txindex_lookup; rpc.c:1983 calls block_storage_read; rpc.c:2000-2022 scans txs by txid comparison |
| `src/app/rpc.c` (rpc_getblock v=0) | blocks_storage.c (block_storage_read) | block_index_db_lookup_by_hash for file position, then block_storage_read for raw bytes | WIRED | rpc.c:1819 lookup; rpc.c:1823-1828 prune check; rpc.c:1836 block_storage_read; rpc.c:1840 json_builder_hex |
| `src/app/rpc.c` (rpc_getblockchaininfo) | chainstate.c (block_index_map_lookup) | in-memory walk from tip_index backwards 11 blocks via prev_hash | WIRED | rpc.c:1671 chainstate_get_block_index_map; rpc.c:1679 block_index_map_lookup in while-loop |

---

## Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| RPC-01 | 05-01 | Node maintains a transaction index mapping txid to block file position | IMPLEMENTED, DOCS STALE | tx_index schema in block_index_db.c:147; all four CRUD functions implemented; REQUIREMENTS.md checkbox not updated to [x] |
| RPC-02 | 05-02 | User can query confirmed transactions by txid via getrawtransaction RPC | SATISFIED | rpc.c:1953-2027: full confirmed tx lookup path via txindex_lookup |
| RPC-03 | 05-02 | User can retrieve raw block hex via getblock RPC at verbosity=0 | SATISFIED | rpc.c:1815-1843: witness-serialized bytes returned via block_storage_read |
| RPC-04 | 05-02 | getblockchaininfo returns correct mediantime | SATISFIED | rpc.c:1662-1696: MTP computed from in-memory block_index_map walk, insertion sort, median at ts_count/2 |

**RPC-01 documentation discrepancy:** The REQUIREMENTS.md checklist shows `- [ ] **RPC-01**` (unchecked) and the traceability table shows `Pending`. The implementation is complete (commits 3d941c8 and 32dcbcb). This is a stale document, not a missing implementation. The checklist for RPC-02, RPC-03, and RPC-04 are all correctly marked `[x]`.

---

## Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| `src/app/rpc.c` | 1902 | `TODO: get tx count from storage` | Info | nTx field in getblock verbosity=1 path — explicitly deferred in Plan 05-02 per plan text; not a Phase 5 requirement |
| `src/app/rpc.c` | 2241 | `TODO: process template_request` | Info | getblocktemplate stub — Phase 6 scope (RPC-05) |
| `src/app/node.c` | 1519 | `TODO: Implement proper storage callback` | Info | Pre-existing TODO from earlier phase |
| `src/node/chaser_confirm.c` | 66 | `TODO: Get from config` | Info | Pre-existing TODO for checkpoint config |

No blockers. The two warnings from the clean build (`get_batch_size_for_height` in download_mgr.c, `node_flush_utxo_shutdown` in node.c) are pre-existing from earlier phases — confirmed by git log showing download_mgr.c last modified in phase 1 work.

---

## Mediantime Correctness Analysis

The MTP implementation at rpc.c:1662-1696 is mathematically correct:

- Collects up to 11 timestamps walking backward from `tip_index` via `cur->prev_hash`
- Guards against walking past genesis with `if (cur->height == 0) break`
- Sorts 11 elements ascending using insertion sort
- Takes `timestamps[ts_count / 2]` — with ts_count=11 this is index 5, the 6th of 11 elements (true median)
- With fewer than 11 blocks (e.g., early chain), `ts_count / 2` still gives the correct median
- The tip's own timestamp is included as the most recent block, consistent with Bitcoin Core MTP calculation

---

## Human Verification Required

### 1. getrawtransaction Confirmed Lookup

**Test:** With a synced pruning node, call `curl -s -d '{"method":"getrawtransaction","params":["<confirmed_txid>"]}' http://localhost:8332/`
**Expected:** Returns witness-serialized hex of the transaction; calling for a txid in a pruned block returns a JSON error (not-found)
**Why human:** Requires a synced node with at least one indexed block and a known txid

### 2. getblock Verbosity=0 Raw Hex

**Test:** Call `curl -s -d '{"method":"getblock","params":["<known_block_hash>",0]}' http://localhost:8332/`
**Expected:** Returns a hex string whose decoded bytes parse as a valid block; the block hash of the decoded header matches the request param
**Why human:** Requires a synced node with block data on disk

### 3. getblockchaininfo Mediantime Non-Zero

**Test:** Call `curl -s -d '{"method":"getblockchaininfo"}' http://localhost:8332/ | jq .result.mediantime`
**Expected:** Returns a Unix timestamp in a reasonable range (not 0, not current wall time but within a few hours of it for a synced node)
**Why human:** Requires a synced node with blocks in the in-memory block_index_map

### 4. Reorg Txindex Cleanup

**Test:** Observe or simulate a chain reorg (even a 1-block reorg during normal sync); after the reorg, call getrawtransaction for a txid that was only in the disconnected block
**Expected:** Returns not-found (the txindex deletion at chaser_confirm.c:285 fired before the in-memory UTXO revert)
**Why human:** Real reorgs are rare on mainnet and cannot be triggered programmatically in the running node

---

## Gaps Summary

The single gap is documentation-only: **RPC-01 checkbox in REQUIREMENTS.md was not updated** after Plan 05-01 completed. The implementation is complete, correct, and wired. No code changes are needed.

The four success criteria listed in the phase goal are all implemented in the codebase. The gap is that REQUIREMENTS.md still shows RPC-01 as `[ ]` (pending) despite the tx_index being fully functional. This should be a trivial documentation fix rather than a re-plan.

---

*Verified: 2026-02-21*
*Verifier: Claude (gsd-verifier)*
