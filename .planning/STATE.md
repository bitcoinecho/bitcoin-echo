# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-02-21)

**Core value:** Echo must correctly validate and serve the Bitcoin blockchain on mainnet — a node that peers trust and rely on.
**Current focus:** Phase 6 — getblocktemplate and submitblock

## Current Position

Phase: 6 of 6 for v1.1 (getblocktemplate and submitblock) — IN PROGRESS
Plan: 1 of 2 in phase 06 — Plan 06-01 complete
Status: Plan 06-01 complete — production getblocktemplate with IBD guard, MTP, witness commitment
Last activity: 2026-02-21 — Plan 06-01 complete (getblocktemplate BIP-22/BIP-145 production implementation)

Progress: [████████░░] 83% (v1.0: phases 1-2 complete; v1.1: phases 3-6 in progress, phase 6 plan 1/2 complete)

## Performance Metrics

**Velocity:**
- Total plans completed: 16
- Average duration: ~7 min
- Total execution time: ~108 min

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 01-foundation-fixes | 7 | ~41 min | ~6 min |
| 02-consensus-completeness | 3 | ~41 min | ~14 min |
| 03-p2p-block-serving | 2 | ~4 min | ~2 min |
| 04-bip125-full-rbf-mempool | 2 | ~10 min | ~5 min |
| 05-storage-layer-and-core-rpc | 2 | ~16 min | ~8 min |
| 06-getblocktemplate-submitblock | 1 of 2 | ~3 min | ~3 min |

*Updated after each plan completion*

## Accumulated Context

### Decisions

v1.0 decisions archived — see `.planning/milestones/v1.0-ROADMAP.md`.
Key decisions preserved in PROJECT.md Key Decisions table.

v1.1 roadmap decision: Follow research-recommended 4-phase structure (phases 3-6) — ordering is hard implementation dependency, not preference. Phase 3 (P2P) must precede Phase 6 (getblocktemplate) because block serving must be stable before mining integration begins.

03-01 decision: Pruned nodes advertise SERVICE_NODE_WITNESS — NODE_WITNESS signals protocol capability (SegWit wire format), not block history availability. Bitcoin Core does the same.
03-01 decision: peer->services check before inventory type — BIP-144 requires INV_WITNESS_BLOCK only to peers that advertised NODE_WITNESS during handshake.

03-02 decision: Direct-send for block serving bypasses peer_queue_message — peer_send_message_internal has no MSG_BLOCK case (20KB stack), msg_block_serialize always includes witness (wrong for INV_BLOCK), and block_t ownership transfer is complex.
03-02 decision: Manual stripped serialization for INV_BLOCK — block_serialize always includes witness, so legacy path manually serializes header + varint + each tx with ECHO_FALSE.
03-02 decision: Unified availability check replaces old pruning stub — single lookup_by_hash + status flag check handles unknown, pruned, and header-only blocks.

04-01 decision: BIP-125 opt-in RBF (Rules 1-5 as written) rather than full-RBF — phase success criteria check signaling; full-RBF (dropping Rule 1) is a future policy toggle not a correctness requirement.
04-01 decision: Rule 1 signaling check moved from conflict loop into rbf_validate_replacement via entry_signals_rbf_inherited — ensures inherited signaling from unconfirmed ancestors is correctly propagated.
04-01 decision: first_conflict removed from mempool_add — rbf_validate_replacement populates result->first_conflict from eviction_set[0] giving correct multi-conflict semantics.

04-02 decision: create_test_tx() uses sequence 0xFFFFFFFE which is NOT below TX_SEQUENCE_DISABLE_RBF (0xFFFFFFFE) — does not signal RBF; new create_test_tx_rbf() uses 0xFFFFFFFD to truly signal.
04-02 decision: Inherited signaling test uses 2-input child (parent output + confirmed UTXO) so replacement conflicts on the confirmed input — avoids Rule 2 rejection that would fire if replacement spent parent's unconfirmed output.
04-02 decision: Rule 5 test uses wide tree (root with 101 outputs, 101 children) because MEMPOOL_MAX_ANCESTORS (25) caps deep chain before reaching 101 entries.

05-02 decision: txindex stores block-level file position only; tx extraction reads full block, parses, and scans — O(block_size) per getrawtransaction call, acceptable for Phase 5; per-tx offset optimization deferred.
05-02 decision: No re-serialization needed for getblock v=0 — node_store_block always calls block_serialize with ECHO_TRUE so stored bytes are already witness-serialized.
05-02 decision: consensus_get_chainstate requires non-const engine — use node_get_consensus(node) not the const consensus already in function scope for MTP walk.

06-01 decision: ECHO_ERR_INVALID_STATE convention for IBD — handlers return this code for not-ready state; rpc_execute_single maps to -28. Cleaner than writing error JSON into builder from handler.
06-01 decision: merkle_root() direct call (not merkle_root_wtxids()) — no coinbase tx_t exists at template generation time; calloc ensures coinbase wtxid slot is zero-initialized per BIP-141.
06-01 decision: consensus_build_validation_ctx + difficulty_compute_next for correct bits at retarget boundaries; falls back to tip_index->bits on failure.

### Pending Todos

None.

### Blockers/Concerns

- hash_scriptpubkeys/hash_amounts placeholders in script.c block real multi-input Taproot validation (pre-existing tech debt, not a v1.1 blocker)
- test_chase Makefile link defect (pre-existing, stubs work around it)
- [Phase 6 pre-check — RESOLVED]: default_witness_commitment confirmed as 38-byte scriptPubKey (6a24aa21a9ed + 32-byte hash), not raw hash
- [Phase 5 pre-check — RESOLVED]: Stored blocks confirmed witness-serialized; no re-serialization needed in getblock v=0
- [Phase 5 pre-check — RESOLVED]: txindex DELETE hook confirmed wired in chaser_confirm.c in Phase 05-01

## Session Continuity

Last session: 2026-02-21
Stopped at: Plan 06-01 complete — production getblocktemplate (IBD guard, MTP, witness commitment, correct bits)
Resume file: None
Next action: Execute Plan 06-02 (submitblock production implementation)
