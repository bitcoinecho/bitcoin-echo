# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-02-21)

**Core value:** Echo must correctly validate and serve the Bitcoin blockchain on mainnet — a node that peers trust and rely on.
**Current focus:** Phase 3 — P2P Block Serving (v1.1 Network Participant)

## Current Position

Phase: 3 of 6 for v1.1 (P2P Block Serving)
Plan: 2 of 2 in current phase (phase complete)
Status: Phase 3 complete
Last activity: 2026-02-21 — Plan 03-02 complete (getdata block serving: witness + legacy direct-send)

Progress: [█████░░░░░] 43% (v1.0: phases 1-2 complete; v1.1: phase 3 complete, phases 4-6 remaining)

## Performance Metrics

**Velocity:**
- Total plans completed: 12
- Average duration: ~8 min
- Total execution time: ~86 min

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 01-foundation-fixes | 7 | ~41 min | ~6 min |
| 02-consensus-completeness | 3 | ~41 min | ~14 min |
| 03-p2p-block-serving | 2 | ~4 min | ~2 min |

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
Stopped at: Plan 03-02 complete — getdata block serving (witness + legacy direct-send, notfound for unavailable blocks)
Resume file: None
Next action: /gsd:execute-phase 04 01
