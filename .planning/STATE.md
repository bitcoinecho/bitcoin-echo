# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-02-21)

**Core value:** Echo must correctly validate and serve the Bitcoin blockchain on mainnet — a node that peers trust and rely on.
**Current focus:** Phase 3 — P2P Block Serving (v1.1 Network Participant)

## Current Position

Phase: 3 of 6 for v1.1 (P2P Block Serving)
Plan: 1 of 3 in current phase
Status: In progress
Last activity: 2026-02-21 — Plan 03-01 complete (SERVICE_NODE_WITNESS + INV_WITNESS_BLOCK)

Progress: [████░░░░░░] 37% (v1.0: phases 1-2 complete; v1.1: phase 3 in progress 1/3 plans done)

## Performance Metrics

**Velocity:**
- Total plans completed: 11
- Average duration: ~9 min
- Total execution time: ~83 min

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 01-foundation-fixes | 7 | ~41 min | ~6 min |
| 02-consensus-completeness | 3 | ~41 min | ~14 min |
| 03-p2p-block-serving | 1 | ~1 min | ~1 min |

*Updated after each plan completion*

## Accumulated Context

### Decisions

v1.0 decisions archived — see `.planning/milestones/v1.0-ROADMAP.md`.
Key decisions preserved in PROJECT.md Key Decisions table.

v1.1 roadmap decision: Follow research-recommended 4-phase structure (phases 3-6) — ordering is hard implementation dependency, not preference. Phase 3 (P2P) must precede Phase 6 (getblocktemplate) because block serving must be stable before mining integration begins.

03-01 decision: Pruned nodes advertise SERVICE_NODE_WITNESS — NODE_WITNESS signals protocol capability (SegWit wire format), not block history availability. Bitcoin Core does the same.
03-01 decision: peer->services check before inventory type — BIP-144 requires INV_WITNESS_BLOCK only to peers that advertised NODE_WITNESS during handshake.

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
Stopped at: Plan 03-01 complete — SERVICE_NODE_WITNESS in version message + INV_WITNESS_BLOCK for witness-capable peers
Resume file: None
Next action: /gsd:execute-phase 03 02
