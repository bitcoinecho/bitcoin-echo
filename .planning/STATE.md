# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-02-21)

**Core value:** Echo must correctly validate and serve the Bitcoin blockchain on mainnet — a node that peers trust and rely on.
**Current focus:** Phase 4 — BIP-125 Full-RBF Mempool

## Current Position

Phase: 4 of 6 for v1.1 (BIP-125 Full-RBF Mempool)
Plan: 1 of 2 in current phase (plan 04-01 complete)
Status: Phase 4 in progress
Last activity: 2026-02-21 — Plan 04-01 complete (BIP-125 RBF validation: all 5 rules, atomic eviction, inherited signaling)

Progress: [█████░░░░░] 43% (v1.0: phases 1-2 complete; v1.1: phases 3-4 in progress, phases 5-6 remaining)

## Performance Metrics

**Velocity:**
- Total plans completed: 13
- Average duration: ~7 min
- Total execution time: ~89 min

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 01-foundation-fixes | 7 | ~41 min | ~6 min |
| 02-consensus-completeness | 3 | ~41 min | ~14 min |
| 03-p2p-block-serving | 2 | ~4 min | ~2 min |
| 04-bip125-full-rbf-mempool | 1 | ~3 min | ~3 min |

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

### Pending Todos

None.

### Blockers/Concerns

- hash_scriptpubkeys/hash_amounts placeholders in script.c block real multi-input Taproot validation (pre-existing tech debt, not a v1.1 blocker)
- test_chase Makefile link defect (pre-existing, stubs work around it)
- [Phase 6 pre-check]: Verify witness commitment field format in getblocktemplate against live Bitcoin Core node before coding — default_witness_commitment is a hex output script, NOT a pre-built coinbase output
- [Phase 5 pre-check]: Confirm stored block serialization format — if IBD used INV_BLOCK, blocks may be legacy-serialized and Phase 5 getblock will need re-serialization, not pass-through
- [Phase 5 pre-check]: Verify txindex DELETE hook call site in chaser_confirm.c before implementing reorg integration

## Session Continuity

Last session: 2026-02-21
Stopped at: Plan 04-01 complete — BIP-125 RBF validation (rbf_validate_replacement, all 5 rules, atomic eviction)
Resume file: None
Next action: /gsd:execute-phase 04 02
