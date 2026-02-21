# Phase 1: Foundation Fixes - Context

**Gathered:** 2026-02-20
**Status:** Ready for planning

<domain>
## Phase Boundary

Eliminate active bugs and infrastructure gaps that block all subsequent work. The node must run IBD on mainnet with zero LOG_ERRORs, correct block identity tracking (real hashes, not all-zeros), correct fork selection infrastructure (big-endian chainwork), decoupled I/O (async storage writes confirmed before marking complete), and all known race conditions eliminated (duplicate peer addresses, batch remaining counts).

</domain>

<decisions>
## Implementation Decisions

### Peer eviction tuning
- Eviction aggressiveness and threshold strategy (fixed vs adaptive): Claude's discretion based on mainnet data and Bitcoin Core behavior
- Eviction events logged at debug level only — available when needed, doesn't clutter normal IBD output
- Calibration rationale documented in code comments — future maintainers should understand why the chosen threshold value was selected, including measurement methodology

### Diagnostic logging
- Log format (structured vs prose) and error chain depth: Claude's discretion based on existing log patterns in the codebase — stay consistent with current style
- IBD success verification method (grep vs exit code): Claude's discretion — pick simplest approach that works
- Timestamp format: follow existing log system conventions, don't change format for this phase

### Test scenario design
- Concurrent storage tests (TEST-02): Claude's discretion on threading vs sequential approach, based on actual concurrency model in codebase
- Large block tests (TEST-05): cover ALL four edge cases — max size blocks at ECHO_MAX_BLOCK_SIZE boundary, corrupted size fields (malicious input), truncated reads (I/O failure), and near-4x witness weight limit (SegWit boundary)
- Peer eviction tests (TEST-03): Claude's discretion on mock peers vs rate-limited connections — favor what's easiest to maintain and most reliable in CI
- Network access: Claude's discretion on offline-only vs optional network tests — favor CI reliability

### Async write behavior
- Backpressure strategy (immediate vs buffered): Claude's discretion based on existing download/storage architecture
- Write failure handling (immediate shutdown vs retry-then-shutdown): Claude's discretion — prioritize data integrity
- Completion granularity (per-block vs per-batch callbacks): Claude's discretion based on how download manager currently batches work
- fsync behavior: Claude's discretion based on Bitcoin Core's approach and data integrity requirements

### Claude's Discretion
- Peer eviction: threshold value, fixed vs adaptive strategy, aggressiveness level
- Diagnostic logging: format, error chain depth, timestamps, IBD verification method
- Test infrastructure: concurrency simulation approach, peer mocking approach, network requirements
- Async writes: backpressure model, failure recovery, callback granularity, fsync policy

</decisions>

<specifics>
## Specific Ideas

- Large block tests should hit all four boundaries: exact max size, corrupted size fields, truncated reads, and near-4x witness limit — user wants comprehensive edge case coverage here
- Eviction calibration should be documented with methodology in code comments — not just the value, but how it was derived
- Debug-level logging for eviction events specifically — operator shouldn't see eviction churn in normal output

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 01-foundation-fixes*
*Context gathered: 2026-02-20*
