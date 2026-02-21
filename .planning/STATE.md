# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-02-20)

**Core value:** Echo must correctly validate and serve the Bitcoin blockchain on mainnet — a node that peers trust and rely on.
**Current focus:** Phase 1 — Foundation Fixes

## Current Position

Phase: 1 of 4 (Foundation Fixes)
Plan: 7 of 7 in current phase (COMPLETE)
Status: Phase 1 complete — ready for Phase 2
Last activity: 2026-02-20 — Plan 01-07 complete: peer eviction test suite

Progress: [██░░░░░░░░] 25%

## Performance Metrics

**Velocity:**
- Total plans completed: 7
- Average duration: ~9 min
- Total execution time: ~41 min

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 01-foundation-fixes | 7 | ~41 min | ~6 min |

**Recent Trend:**
- Last 5 plans: 01-03 (~9 min), 01-04 (4 min), 01-05 (8 min), 01-06 (6 min), 01-07 (15 min)
- Trend: Stable ~5-15 min/plan

*Updated after each plan completion*
| Phase 01 P05 | 8 | 1 tasks | 2 files |
| Phase 01 P07 | 15 | 1 tasks | 3 files |

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

### Pending Todos

None yet.

### Blockers/Concerns

- [Phase 2 planning]: Verify whether script_execute() actually routes witness v1 scriptpath spends to script_execute_tapscript() or falls through to generic dispatcher — flag from research, must read script.c before planning Phase 2
- [Phase 2 planning]: Confirm vendored secp256k1 API signature for secp256k1_schnorrsig_verify before implementation
- [Phase 1 planning, RESOLVED]: Download manager synchronization model confirmed — block_storage_write() is synchronous with mutex protection; async storage path disabled (INFR-01 fixed with flush-before-index approach)

## Session Continuity

Last session: 2026-02-20
Stopped at: Completed 01-07-PLAN.md (peer eviction test suite — Phase 1 complete)
Resume file: None
