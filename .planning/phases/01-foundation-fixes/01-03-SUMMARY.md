---
phase: 01-foundation-fixes
plan: 03
subsystem: database
tags: [sqlite, chainwork, big-endian, chain-selection, block-index]

# Dependency graph
requires: []
provides:
  - "Correct chainwork byte order in block index DB — big-endian blob for SQLite bytewise sorting"
  - "ORDER BY chainwork DESC for best-chain selection"
  - "Symmetric BE/LE conversion helpers at DB boundary only"
affects:
  - "02-reorg: fork selection now uses correct chainwork ordering from DB"
  - "Any phase reading block_index_db best-chain queries"

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "DB boundary byte-reversal: in-memory LE <-> big-endian blob at insert/read only"
    - "SQLite BLOB ordering: store big-endian to get correct numeric ORDER BY"

key-files:
  created: []
  modified:
    - src/storage/block_index_db.c

key-decisions:
  - "Byte reversal happens only at the DB boundary (insert + populate_entry_from_row) — in-memory work256_t, work256_compare, work256_add are untouched"
  - "ORDER BY chainwork DESC is semantically correct — highest accumulated work wins, not highest height"
  - "test_chase linker failure is pre-existing (missing block_index_db link dep in Makefile) — out of scope, deferred"

patterns-established:
  - "work256_to_be_blob / be_blob_to_work256: static file-scoped helpers at DB boundary — never export"

requirements-completed: [CONS-03]

# Metrics
duration: 9min
completed: 2026-02-21
---

# Phase 01 Plan 03: Chainwork Big-Endian Storage Summary

**Chainwork stored big-endian in SQLite block index so ORDER BY chainwork DESC correctly selects the highest-work chain for Nakamoto fork selection**

## Performance

- **Duration:** 9 min
- **Started:** 2026-02-21T01:26:49Z
- **Completed:** 2026-02-21T01:35:44Z
- **Tasks:** 1
- **Files modified:** 1

## Accomplishments

- Added `work256_to_be_blob` and `be_blob_to_work256` static helpers at the top of `block_index_db.c` — byte-reversal happens only at the DB boundary, leaving all in-memory arithmetic unchanged
- Fixed insert path: `work256_to_be_blob` converts chainwork to big-endian before `db_bind_blob`, ensuring SQLite stores the blob in a format where bytewise comparison yields correct numeric ordering
- Fixed read path: `be_blob_to_work256` in `populate_entry_from_row` reverses the stored big-endian blob back to little-endian `work256_t` on every read — roundtrip is symmetric
- Fixed best-chain query: replaced `ORDER BY height DESC` (and its TODO comment) with `ORDER BY chainwork DESC LIMIT 1` — now uses true Nakamoto consensus criterion instead of height as a proxy

## Task Commits

Each task was committed atomically:

1. **Task 1: Implement big-endian chainwork serialization at DB boundary** - `ac25f35` (fix)

**Plan metadata:** [to be recorded after final commit]

## Files Created/Modified

- `/Users/yayseth/Projects/echo/bitcoin-echo/src/storage/block_index_db.c` — Added BE/LE helpers, fixed insert binding, fixed read memcpy, fixed ORDER BY in best-chain prepared statement

## Decisions Made

- Byte reversal is strictly at the DB boundary. `work256_compare`, `work256_add`, `work256_sub`, `work256_from_bits` and all in-memory callers are untouched. This is the minimal correct fix.
- `ORDER BY chainwork DESC` is the semantically correct criterion for Nakamoto consensus. Height is only a correct proxy when the chain never forks; chainwork is always correct.
- No database migration needed: CLAUDE.md specifies `rm -rf ~/.bitcoin-echo` before starting — the node always starts from genesis.

## Deviations from Plan

None — plan executed exactly as written.

## Issues Encountered

- `test_chase` in `make test` fails with an unresolved linker symbol (`_block_index_db_lookup_by_height`, `_log_warn`) when building the test binary. This is a pre-existing Makefile gap in the `test_chase` link line — `block_index_db.o` and `log.o` are not listed as dependencies. This is out of scope (not caused by this change) and was verified as pre-existing. Deferred to `.planning/phases/01-foundation-fixes/deferred-items.md`.

## Next Phase Readiness

- Block index DB now stores and queries chainwork correctly — `block_index_db_get_best_chain` will return the genuine highest-work tip even in the presence of forks
- Phase 02 reorg work can rely on the DB selecting the correct fork tip via `ORDER BY chainwork DESC`
- No downstream callers need changes — the interface (`block_index_entry_t.chainwork`) remains little-endian as before

---
*Phase: 01-foundation-fixes*
*Completed: 2026-02-21*
