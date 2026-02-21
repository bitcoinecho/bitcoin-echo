# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-02-20)

**Core value:** Echo must correctly validate and serve the Bitcoin blockchain on mainnet — a node that peers trust and rely on.
**Current focus:** Phase 1 — Foundation Fixes

## Current Position

Phase: 1 of 4 (Foundation Fixes)
Plan: 4 of 7 in current phase
Status: In progress
Last activity: 2026-02-21 — Plan 01-04 complete: flush-before-index ordering in block storage

Progress: [█░░░░░░░░░] 14%

## Performance Metrics

**Velocity:**
- Total plans completed: 4
- Average duration: ~7 min
- Total execution time: ~26 min

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 01-foundation-fixes | 4 | ~26 min | ~6.5 min |

**Recent Trend:**
- Last 5 plans: 01-01 (~5 min), 01-02 (5 min), 01-03 (9 min), 01-04 (4 min)
- Trend: Stable ~5-9 min/plan

*Updated after each plan completion*
| Phase 01 P05 | 8 | 1 tasks | 2 files |

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

### Pending Todos

None yet.

### Blockers/Concerns

- [Phase 2 planning]: Verify whether script_execute() actually routes witness v1 scriptpath spends to script_execute_tapscript() or falls through to generic dispatcher — flag from research, must read script.c before planning Phase 2
- [Phase 2 planning]: Confirm vendored secp256k1 API signature for secp256k1_schnorrsig_verify before implementation
- [Phase 1 planning, RESOLVED]: Download manager synchronization model confirmed — block_storage_write() is synchronous with mutex protection; async storage path disabled (INFR-01 fixed with flush-before-index approach)

## Session Continuity

Last session: 2026-02-21
Stopped at: Completed 01-04-PLAN.md (flush-before-index block storage ordering)
Resume file: None
