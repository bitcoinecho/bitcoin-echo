---
phase: 03-p2p-block-serving
verified: 2026-02-21T17:00:00Z
status: passed
score: 12/12 must-haves verified
re_verification: false
---

# Phase 3: P2P Block Serving Verification Report

**Phase Goal:** Node advertises witness capability, serves blocks to peers, and uses correct inventory types
**Verified:** 2026-02-21
**Status:** PASSED
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| #  | Truth | Status | Evidence |
|----|-------|--------|----------|
| 1  | Node's version message includes SERVICE_NODE_WITNESS (bit 3) in services — both pruned and non-pruned | VERIFIED | 3 `peer_send_version` call sites (lines 3152, 3179, 3550) each set `uint64_t services = SERVICE_NODE_WITNESS` |
| 2  | Non-pruned node advertises SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS; pruned advertises SERVICE_NODE_WITNESS only | VERIFIED | All 3 call sites: `if (!node_is_pruning_enabled(node)) { services |= SERVICE_NODE_NETWORK; }` |
| 3  | Node announces new blocks to witness-capable peers using INV_WITNESS_BLOCK | VERIFIED | `node_announce_block_to_peers` (line 4183): `bool peer_has_witness = (peer->services & SERVICE_NODE_WITNESS) != 0; inv_vec.type = peer_has_witness ? INV_WITNESS_BLOCK : INV_BLOCK;` |
| 4  | Node announces new blocks to legacy peers using INV_BLOCK | VERIFIED | Same conditional — falls through to INV_BLOCK for non-witness peers |
| 5  | Node requests blocks from witness-capable peers during IBD using INV_WITNESS_BLOCK | VERIFIED | `sync_cb_send_getdata_blocks` (line 1781): `bool peer_has_witness = ...; uint32_t block_inv_type = peer_has_witness ? INV_WITNESS_BLOCK : INV_BLOCK;` |
| 6  | Node requests blocks from legacy peers during IBD using INV_BLOCK | VERIFIED | Same conditional — falls to INV_BLOCK for non-witness peers |
| 7  | A peer sending getdata with INV_WITNESS_BLOCK for a known block receives the full witness-serialized block | VERIFIED | Handler at line 2766: `want_witness` path calls `block_serialize`, builds 24-byte header, sends via `plat_socket_send` loop |
| 8  | A peer sending getdata with INV_BLOCK for a known block receives stripped legacy serialization without witness data | VERIFIED | Else branch (line 2879): `block_header_serialize` + `varint_write(tx_count)` + `tx_serialize(ECHO_FALSE)` per tx, then direct-send |
| 9  | A getdata for a pruned block hash returns notfound | VERIFIED | Status check at line 2792: `(idx_entry.status & BLOCK_STATUS_PRUNED)` → `goto serve_block_notfound` |
| 10 | A getdata for a block hash not in the index returns notfound | VERIFIED | `block_index_db_lookup_by_hash` returning `ECHO_ERR_NOT_FOUND` (line 2782) → `goto serve_block_notfound` |
| 11 | A getdata for a block known by header but not yet downloaded returns notfound | VERIFIED | `!(idx_entry.status & BLOCK_STATUS_HAVE_DATA)` check (line 2792) → `goto serve_block_notfound` |
| 12 | No memory leaks in the block serving path — all malloc'd buffers freed on every exit path | VERIFIED | `block_free` called immediately after serialization in both paths; heap buffers (`buf`, `stripped_buf`) freed before every `continue`/`goto`; `tx_ser_failed` bool pattern avoids mid-loop leak |

**Score:** 12/12 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `src/app/node.c` | Services flags, inventory type awareness, getdata block handler | VERIFIED | Contains `SERVICE_NODE_WITNESS` (5 occurrences), `peer_has_witness` (2 occurrences), complete getdata handler with witness and legacy paths, notfound label and 3 gotos |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `peer_send_version` call sites (lines 3152, 3179, 3550) | `protocol.h SERVICE_NODE_WITNESS` constant | bitwise OR in services calculation | WIRED | All 3 sites: `uint64_t services = SERVICE_NODE_WITNESS; if (!pruned) services |= SERVICE_NODE_NETWORK;` |
| `node_announce_block_to_peers` (line 4183) | `peer->services` check | conditional `INV_WITNESS_BLOCK` vs `INV_BLOCK` | WIRED | `bool peer_has_witness = (peer->services & SERVICE_NODE_WITNESS) != 0; inv_vec.type = peer_has_witness ? INV_WITNESS_BLOCK : INV_BLOCK;` |
| `sync_cb_send_getdata_blocks` (line 1781) | `peer->services` check | conditional `INV_WITNESS_BLOCK` vs `INV_BLOCK` | WIRED | `bool peer_has_witness = ...; uint32_t block_inv_type = peer_has_witness ? INV_WITNESS_BLOCK : INV_BLOCK;` |
| getdata handler (line 2766) | `node_load_block` + `block_index_db_lookup_by_hash` | block existence check then load-and-serve | WIRED | `block_index_db_lookup_by_hash` at line 2778, `node_load_block(node, &inv->hash, &block)` at line 2802 |
| getdata handler witness path | `plat_socket_send` with witness-serialized buffer | `block_serialize` to heap buffer, build 24-byte header, send directly | WIRED | `block_serialize(&block, buf, buf_size, &written)` then `plat_socket_send` header + payload loop (lines 2833–2876) |
| getdata handler legacy path (INV_BLOCK) | `plat_socket_send` with stripped buffer | `tx_serialize(ECHO_FALSE)` per tx, build 24-byte header, send directly | WIRED | `tx_serialize(&block.txs[t], ECHO_FALSE, ...)` at line 2930; `plat_socket_send` at lines 2965–2981 |
| getdata handler | notfound for missing/pruned blocks | `block_index_db_lookup_by_hash` error or BLOCK_STATUS checks | WIRED | `serve_block_notfound` label at line 2991; `MSG_NOTFOUND` message sent via `peer_queue_message` |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| P2P-01 | 03-01-PLAN.md | Node advertises NODE_WITNESS (service bit 3) in version message to peers | SATISFIED | 3 `peer_send_version` call sites verified in `src/app/node.c` — `SERVICE_NODE_WITNESS` at lines 3152, 3179, 3550 |
| P2P-02 | 03-02-PLAN.md | Node serves full witness-serialized blocks to peers via getdata handler | SATISFIED | Complete getdata handler at lines 2766–3001: witness path (`block_serialize`) and legacy path (`tx_serialize(ECHO_FALSE)`) both via direct-send |
| P2P-04 | 03-01-PLAN.md | Node uses INV_WITNESS_BLOCK inventory type when requesting blocks from witness-capable peers | SATISFIED | `sync_cb_send_getdata_blocks` at line 1781 and `node_announce_block_to_peers` at line 4183 both check `peer->services & SERVICE_NODE_WITNESS` before choosing inventory type |

All three requirements declared in PLAN frontmatter are accounted for and satisfied. REQUIREMENTS.md Traceability table confirms P2P-01, P2P-02, P2P-04 are all mapped to Phase 3. No orphaned requirements detected.

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| `src/protocol/download_mgr.c` | 203 | Unused function `get_batch_size_for_height` | Info | Pre-existing, unrelated to this phase. Documented in both SUMMARY files as out of scope. |
| `src/app/node.c` | 3845 | Unused function `node_flush_utxo_shutdown` | Info | Pre-existing, unrelated to this phase. Documented in both SUMMARY files as out of scope. |

No blockers. No warnings introduced by this phase. Build is clean with respect to phase changes.

### Human Verification Required

None. All goal truths are verifiable through static code inspection and build verification. The protocol behavior (whether peers accept and correctly parse the served blocks) would require live network testing, but correctness of the serialization paths (witness vs. stripped legacy, notfound signaling) is fully evident in code.

### Commits Verified

All four commits from the SUMMARY files are confirmed present in git history:

- `05738e3` — feat(03-01): advertise SERVICE_NODE_WITNESS in version message
- `8e49894` — feat(03-01): use INV_WITNESS_BLOCK for witness-capable peers
- `3b49eea` — feat(03-02): implement getdata witness block serving with direct-send
- `0122819` — feat(03-02): implement legacy block stripping for INV_BLOCK direct-send

### Summary

Phase 3 achieved its goal completely. All six truths from plan 03-01 and all six truths from plan 03-02 are verified in the actual codebase:

- **Services advertisement (P2P-01):** Three `peer_send_version` call sites all use `SERVICE_NODE_WITNESS` as the base, with `SERVICE_NODE_NETWORK` added only for non-pruned nodes. No magic number literals remain.
- **Inventory types (P2P-04):** Both announcement (`node_announce_block_to_peers`) and IBD request (`sync_cb_send_getdata_blocks`) sites check `peer->services & SERVICE_NODE_WITNESS` before selecting `INV_WITNESS_BLOCK` vs `INV_BLOCK`. The old TODO comment is gone.
- **Block serving (P2P-02):** The getdata handler is fully implemented — not a stub. The witness path uses `block_serialize` to a heap buffer sent via `plat_socket_send`; the legacy path manually constructs stripped serialization with `tx_serialize(ECHO_FALSE)` per transaction. Three notfound cases (unknown hash, pruned, header-only) all correctly send `MSG_NOTFOUND`. Memory management is correct on every exit path.

---
_Verified: 2026-02-21_
_Verifier: Claude (gsd-verifier)_
