# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-02-20)

**Core value:** Echo must correctly validate and serve the Bitcoin blockchain on mainnet — a node that peers trust and rely on.
**Current focus:** Phase 2 — Consensus Completeness

## Current Position

Phase: 2 of 4 (Consensus Completeness)
Plan: 3 of 3 in current phase (PHASE COMPLETE)
Status: Phase 2 complete — all 3 plans done
Last activity: 2026-02-21 — Plan 02-03 complete: chain reorg test suite (3 scenarios, 1098 tests pass)

Progress: [█████░░░░░] 50%

## Performance Metrics

**Velocity:**
- Total plans completed: 9
- Average duration: ~10 min
- Total execution time: ~66 min

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 01-foundation-fixes | 7 | ~41 min | ~6 min |
| 02-consensus-completeness | 3 | ~41 min | ~14 min |

**Recent Trend:**
- Last 5 plans: 01-07 (15 min), 02-01 (~0 min research), 02-02 (25 min), 02-03 (16 min)
- Trend: Stable ~5-25 min/plan

*Updated after each plan completion*
| Phase 01 P05 | 8 | 1 tasks | 2 files |
| Phase 01 P07 | 15 | 1 tasks | 3 files |
| Phase 02 P02 | 25 | 2 tasks | 6 files |
| Phase 02 P03 | 16 | 1 tasks | 4 files |

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Recent decisions affecting current work:

- [Roadmap]: 4-phase structure driven by hard dependency order, not depth template — research confirms ordering is non-negotiable
- [Roadmap]: CONS-03 (chainwork endianness) assigned to Phase 1, not Phase 2 — it silently corrupts reorg test results if fixed concurrently with reorg work
- [Roadmap]: getblocktemplate included in Phase 4 as last item — highest complexity, depends on mempool (Phase 3 RBF) and block serving both stable
- [01-03]: Byte reversal at DB boundary only — work256_compare/add/sub unchanged; ORDER BY chainwork DESC is semantically correct Nakamoto criterion
- [01-04]: fflush() not fsync() — fflush pushes to OS page cache (one write(2) syscall); fsync would flush to physical media and kill IBD throughput
- [Phase 01]: DOWNLOAD_MIN_RATE_BYTES_PER_SEC set to 1024 (1 KB/s) — conservative threshold based on Bitcoin Core nMinExpectedRate behavior; 3072 was arbitrary
- [Phase 01]: All routine eviction events use LOG_DEBUG only — invisible during normal IBD, available for diagnostics
- [01-07]: download_mgr_inject_peer_rate() always sets has_reported=true even for zero-rate stalled peers — stalled peer IS a reporter (used to deliver, stopped), distinct from warming-up peer (never delivered)
- [01-07]: Test injection API is a proper public function (not #ifdef-guarded) with "FOR UNIT TESTS ONLY" doc comment — simpler and more honest than preprocessor-conditional builds
- [Phase 01]: Corrupted size field test uses 0xFFFFFFFF — validation already present in blocks.c (ECHO_MAX_BLOCK_SIZE*4 check), no fix needed
- [Phase 01]: Near-4x witness limit test uses ECHO_MAX_BLOCK_SIZE*4-1 (3,999,999 bytes) — highest valid storage size per current blocks.c bounds
- [Phase 02]: Chainstate owns deltas in state->deltas[]; delta_out parameter is a borrowed (non-owning) reference — callers must not free it
- [Phase 02]: [02-02]: consensus.c apply path now requests deltas (changed NULL to &delta_borrow) enabling reorg rollback for blocks confirmed via normal path
- [Phase 02]: [02-02]: chaser_confirm_reorganize gracefully skips UTXO rollback when chainstate is NULL (test/early-init paths)
- [Phase 02]: [02-03]: chain_reorganize used nonce=0 in minimal connect-phase headers, causing hash mismatch for multi-block connects — fixed by patching state->tip.hash, height_index, and delta->block_hash from to_connect->hash after each apply
- [Phase 02]: [02-03]: block_index_t does not store nonce — authoritative block hash must come from block_index_t->hash, not from re-hashing a reconstructed header

### Pending Todos

None yet.

### Blockers/Concerns

- [Phase 2 planning]: Verify whether script_execute() actually routes witness v1 scriptpath spends to script_execute_tapscript() or falls through to generic dispatcher — flag from research, must read script.c before planning Phase 2
- [Phase 2 planning]: Confirm vendored secp256k1 API signature for secp256k1_schnorrsig_verify before implementation
- [Phase 1 planning, RESOLVED]: Download manager synchronization model confirmed — block_storage_write() is synchronous with mutex protection; async storage path disabled (INFR-01 fixed with flush-before-index approach)

## Session Continuity

Last session: 2026-02-21
Stopped at: Completed 02-03-PLAN.md (chain reorg test suite — 3 scenarios, chain_reorganize bug fix)
Resume file: None
