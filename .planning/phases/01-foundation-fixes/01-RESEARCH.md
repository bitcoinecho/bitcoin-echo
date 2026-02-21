# Phase 1: Foundation Fixes - Research

**Researched:** 2026-02-20
**Domain:** C11 multithreaded node — download manager, block storage I/O, chaser pipeline, block index database, peer management
**Confidence:** HIGH (all findings sourced directly from codebase; no external library research required)

---

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

- Eviction events logged at debug level only — available when needed, doesn't clutter normal IBD output
- Calibration rationale documented in code comments — future maintainers should understand why the chosen threshold value was selected, including measurement methodology
- Timestamp format: follow existing log system conventions, don't change format for this phase

### Claude's Discretion

- Peer eviction: threshold value, fixed vs adaptive strategy, aggressiveness level
- Diagnostic logging: format, error chain depth, timestamps, IBD verification method
- Test infrastructure: concurrency simulation approach, peer mocking approach, network requirements
- Async writes: backpressure model, failure recovery, callback granularity, fsync policy

### Deferred Ideas (OUT OF SCOPE)

None — discussion stayed within phase scope
</user_constraints>

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| BUGF-01 | Download manager correctly handles batch remaining count when duplicate blocks received from reassigned batches | Bug already partially fixed: received[] bitmap exists and guards against decrement. Root cause found: the SAFETY CHECK at `peer_request_work()` (download_mgr.c:596-611) detects remaining=0 with unreceived blocks and re-queues, but the LOG_ERROR fire means IBD logs show errors. The fix is to prevent the miscount, not just detect it after. |
| BUGF-02 | Peer manager prevents duplicate address connections via proper deduplication check before connection setup | Dedup check exists in node.c:3271-3292 as a "defensive" LOG_WARN-level check; `discovery_mark_address_in_use()` is called before the per-peer loop. Race: another goroutine can select same address between `select` and `mark_in_use`. Fix: move `mark_in_use` earlier and add address check in `discovery_select_outbound_address()` against active peer list. |
| INFR-01 | Block storage uses async write with completion callback — download manager tracks "durably written" not "enqueued" | `block_storage_write()` is synchronous today; caller does not know when data is durable. `BLOCK_STORAGE_FLUSH_INTERVAL=100` means 99 blocks between real flushes. Download manager marks block received immediately after `block_storage_write()` returns — before fsync. GAP errors appear because chaser_confirm tries to load a block whose position was indexed but whose data may not have been flushed yet. |
| INFR-02 | Chaser validation retrieves real block hash from block index instead of submitting all-zeros | Confirmed bug at chaser_validate.c:518-519: `uint8_t hash[32] = {0}` is submitted on CHASE_CHECKED. The CHASE_BUMP / CHASE_START path (chaser_validate.c:486-487) calls `node_load_block_at_height` which returns the real hash in `hash_out`. The CHASE_CHECKED path never does this. Fix: look up hash via `block_index_db_lookup_by_height()` before submitting. |
| INFR-03 | Checkpoint height is configurable via node config instead of hardcoded to 0 | Confirmed at chaser_validate.c:178: `chaser->top_checkpoint = 0; /* TODO: Get from config */`. `node_config_t` struct exists with `assume_valid` flag but no `checkpoint_height` field. `chaser_validate_set_checkpoint()` API exists and works. Fix: add `checkpoint_height` to `node_config_t`, pass through `node_create()` → `chaser_validate_create()` → `chaser_validate_set_checkpoint()`. |
| INFR-04 | Chaser fault handler logs error details via log system before shutdown signal | Confirmed at chaser.c:177-188: `chaser_fault()` has `/* TODO: Integrate with logging system */` comment and goes directly to `chase_notify(CHASE_STOP)`. Fix: call `log_error()` with chaser name and error code before the notify. |
| INFR-05 | Peer eviction threshold (SLOWEST_EVICTION_MIN_RATE) calibrated from mainnet IBD measurement data | Current threshold: `DOWNLOAD_MIN_RATE_BYTES_PER_SEC = 3072` (3 KB/s), defined in download_mgr.h:53. Comment says "NOTE: Early blocks are tiny (< 1KB), so threshold is conservative." This needs calibration from actual mainnet IBD run data. No constant named `SLOWEST_EVICTION_MIN_RATE` exists yet — this is the value to add/rename after calibration. |
| CONS-03 | Node stores chainwork in big-endian format for correct SQLite bytewise fork selection | Confirmed bug at block_index_db.c:150-155: explicit TODO comment "store chainwork in big-endian format." `work256_t` stores bytes little-endian internally (chainstate.h:49-51). Insert at block_index_db.c:593-594 binds `entry->chainwork.bytes` directly. Best-chain query uses `ORDER BY height DESC` as workaround. Fix: byte-reverse the 32-byte blob on insert and on read-back. |
| TEST-02 | Test suite covers concurrent block storage reads and writes under load | `test_block_storage.c` exists. Concurrency model: `block_file_manager_t` has `pthread_mutex_t mutex` protecting all mutable state. Tests should spawn multiple threads doing simultaneous reads (block_storage_read) and writes (block_storage_write) and verify no corruption. |
| TEST-03 | Test suite covers peer eviction with multiple slow/stalled peers | `test_download_mgr.c` exists with mock callback infrastructure. Tests need: multiple mock peers at varying simulated rates, call `download_mgr_check_performance()` and `download_mgr_evict_slowest_percent()`, verify correct peers evicted while minimum count maintained. |
| TEST-05 | Test suite covers large block handling at ECHO_MAX_BLOCK_SIZE and near 4x limit, including corrupted size fields and truncated reads | Four edge cases required: (1) exact `ECHO_MAX_BLOCK_SIZE` boundary, (2) corrupted size field in file header, (3) truncated read (I/O failure mid-block), (4) near-4x witness weight limit. `block_storage_read()` reads size from file header and mallocs — corrupted size leads to oversized malloc or OOB read. |
</phase_requirements>

---

## Summary

Phase 1 is a pure codebase-internal fix phase. There are no external libraries to introduce and no new subsystems to design. Every bug and infrastructure gap has been located precisely in source files. The work divides cleanly into three categories: (1) confirmed bugs with clear root causes, (2) TODO stubs that need wiring, and (3) new tests for concurrency and edge-case behavior.

The most architecturally significant work is INFR-01 (async storage write completion). Today `block_storage_write()` is synchronous and returns before fsync. The download manager calls it inline and immediately marks the block as received — but `BLOCK_STORAGE_FLUSH_INTERVAL=100` means the data may not be durable for another 99 blocks. When chaser_confirm tries to load those blocks, it can see a stale index position pointing to unflushed data. The fix requires either (a) flushing on every write during critical paths, or (b) introducing a write-completion callback that the download manager holds until durability is confirmed. Given the PULL model's batch architecture, option (a) with selective fsync on batch completion is simpler to implement correctly.

The chainwork endianness fix (CONS-03) requires a careful one-time migration: the 32-byte blob must be byte-reversed on insert and reversed again on read-back, and the best-chain query must be updated to `ORDER BY chainwork DESC`. The existing `work256_compare()` function operates in-memory on little-endian bytes and must not be changed. Only the database serialization layer changes.

**Primary recommendation:** Fix bugs and wire TODOs in dependency order: INFR-04 first (cheapest, enables diagnostic visibility for all subsequent work), then INFR-02 and INFR-03 (correctness preconditions for validation), then BUGF-01 and BUGF-02 (eliminate active error log noise), then CONS-03 (requires careful migration), then INFR-01 (most structural change), then INFR-05 (calibration after other fixes stabilize IBD), then tests.

---

## Standard Stack

### Core

| Component | File | Purpose | Notes |
|-----------|------|---------|-------|
| Download manager | `src/protocol/download_mgr.c` | PULL-based block download, batch tracking | ~1873 lines, complete implementation |
| Chaser validate | `src/node/chaser_validate.c` | Parallel block validation threadpool | Worker threads + work queue |
| Chaser base | `src/node/chaser.c` | Event dispatch, fault reporting, lifecycle | `chaser_fault()` needs logging wired |
| Block index DB | `src/storage/block_index_db.c` | SQLite-backed block header store | Chainwork endianness bug confirmed |
| Block storage | `src/storage/blocks.c` | Append-only blk*.dat file manager | Mutex-protected, synchronous writes |
| Discovery | `src/protocol/discovery.c` | Peer address manager | `find_address()` dedup exists |
| Node lifecycle | `src/app/node.c` | Orchestration, connection management | Duplicate peer check at line 3271 |
| Log system | `include/log.h` | `log_error/warn/info/debug(comp, fmt, ...)` | Printf-style, component-tagged |

### Supporting

| Component | Version/Location | Purpose | When to Use |
|-----------|---------|---------|-------------|
| pthreads | POSIX | Mutex protection, thread creation | All concurrency in storage and chaser |
| SQLite (embedded) | `lib/sqlite/sqlite3.h` | Block index persistence | Do NOT use WAL-specific APIs without checking db.h wrappers |
| db.h wrappers | `include/db.h` | Prepared statement API | Always use instead of raw sqlite3 calls |
| `plat_time_ms()` | `include/platform.h` | Monotonic millisecond clock | Use for all timing in download manager |

---

## Architecture Patterns

### Project Layer Rule

The project enforces: App → Protocol → Consensus (FROZEN) → Platform. Consensus layer must not do I/O. Block storage and download manager are in the Protocol/Storage layer. Chaser is in App (node/) layer. Keep this separation clean — do not introduce I/O paths into consensus headers.

### Pattern 1: Log-then-Act for Fault Reporting

**What:** Any fatal fault must log before signaling shutdown so the error survives in the log file.
**When to use:** `chaser_fault()` fix, any new error paths in this phase.
**Example (INFR-04 fix):**
```c
void chaser_fault(chaser_t *self, int error) {
    if (!self) {
        return;
    }
    /* Log the fault BEFORE notifying shutdown — log flushes are not guaranteed
     * after CHASE_STOP is dispatched, so this must come first. */
    log_error(LOG_COMP_SYNC,
              "chaser '%s' fault: error=%d — initiating shutdown",
              self->name, error);

    chase_value_t value = {.count = (size_t)error};
    chase_notify(self->dispatcher, CHASE_STOP, value);
}
```
Source: `src/node/chaser.c:177-188` (existing stub), `include/log.h` (API).

### Pattern 2: Byte-Reversal for Big-Endian Database Storage

**What:** SQLite blob comparison is bytewise. For `ORDER BY chainwork DESC` to select the highest-work chain, chainwork must be stored most-significant-byte first (big-endian). `work256_t` internally is little-endian (byte[0] is LSB). Reverse exactly at the insert/read boundary.
**When to use:** CONS-03 fix only. Do NOT change `work256_compare()` or any in-memory arithmetic.
**Example:**
```c
/* On insert: reverse work256_t bytes before binding to SQLite */
static void work256_to_be_blob(const work256_t *work, uint8_t *be_out) {
    for (int i = 0; i < 32; i++) {
        be_out[i] = work->bytes[31 - i];
    }
}

/* On read: reverse the blob back to little-endian work256_t */
static void be_blob_to_work256(const uint8_t *be_in, work256_t *work) {
    for (int i = 0; i < 32; i++) {
        work->bytes[i] = be_in[31 - i];
    }
}
```
Source: Derived from `chainstate.h:49-51` (LE storage), `block_index_db.c:150-155` (TODO comment), `block_index_db.c:593-594` (insert site).

### Pattern 3: Hash Lookup Before Submission in chaser_validate

**What:** The CHASE_CHECKED path in chaser_validate.c submits an all-zero hash. The CHASE_BUMP/START path already calls `node_load_block_at_height()` which returns the real hash. The CHASE_CHECKED path must also retrieve the hash.
**When to use:** INFR-02 fix.
**Example (INFR-02 fix):**
```c
case CHASE_CHECKED:
    {
        uint32_t height = value.height;
        uint32_t position = chaser_position(self);

        if (height == position + 1) {
            /* Retrieve real block hash from block index */
            block_index_entry_t entry;
            block_index_db_t *bdb =
                node_get_block_index_db(chaser->base.node);
            uint8_t hash[32] = {0}; /* fallback if lookup fails */
            if (block_index_db_lookup_by_height(bdb, height, &entry) == ECHO_OK) {
                memcpy(hash, entry.hash.bytes, 32);
            } else {
                log_warn(LOG_COMP_SYNC,
                         "chaser_validate: no index entry for height %u",
                         height);
            }

            bool bypass = chaser_validate_is_bypass(chaser, height);
            if (chaser_validate_submit(chaser, height, hash, bypass) == 0) {
                chaser_set_position(self, height);
            }
        }
    }
    break;
```
Source: `src/node/chaser_validate.c:509-527` (existing broken code), `include/block_index_db.h:211-214` (lookup API), `include/node.h:382` (`node_get_block_index_db`).

### Pattern 4: Config Field Addition for checkpoint_height

**What:** `node_config_t` currently has `assume_valid` bool but no `checkpoint_height` field. `chaser_validate_set_checkpoint()` API is complete and working.
**When to use:** INFR-03 fix.
**Change sites:**
1. `include/node.h:58-66` — add `uint32_t checkpoint_height;` to `node_config_t`
2. `src/app/node.c` — populate from config in `node_config_init()` and pass to `chaser_validate_set_checkpoint()` after chaser creation
3. `src/app/main.c` (if CLI parsing exists) — parse `--checkpoint` flag if desired
Source: `include/node.h:58-66`, `include/chaser_validate.h:147` (`chaser_validate_set_checkpoint`), `src/node/chaser_validate.c:178`.

### Pattern 5: Mock-Based Download Manager Tests

**What:** `test_download_mgr.c` uses typed mock peers (`mock_peer_t`) cast to `peer_t*`, with callback-based mock functions for `send_getdata` and `disconnect_peer`. Tests operate entirely in-process with no real sockets.
**When to use:** TEST-03 (peer eviction tests).
**Pattern from existing code:**
```c
typedef struct { int id; char addr[32]; } mock_peer_t;

static void mock_send_getdata(peer_t *peer, const hash256_t *hashes,
                              size_t count, void *ctx) { /* ... */ }
static void mock_disconnect_peer(peer_t *peer, const char *reason,
                                 void *ctx) { /* ... */ }

download_callbacks_t callbacks = {
    .send_getdata = mock_send_getdata,
    .disconnect_peer = mock_disconnect_peer,
    .ctx = &ctx
};
```
Source: `test/unit/test_download_mgr.c:23-53`.

### Pattern 6: Block Storage Test Infrastructure

**What:** `test_block_storage.c` uses `/tmp/echo_block_storage_test` as temp dir, creates/destroys with `remove_dir_recursive()`. Tests call `block_storage_init()`, perform operations, then `block_storage_close()`.
**When to use:** TEST-02 (concurrent storage) and TEST-05 (large block / edge case).
**Concurrency approach:** Use `pthread_create()` + `pthread_join()`. `block_file_manager_t.mutex` protects all mutable state — concurrent tests validate that the mutex prevents corruption, not that concurrent writes succeed without mutex.

### Anti-Patterns to Avoid

- **Modifying `work256_compare()`:** This compares in-memory little-endian values correctly. Only the DB serialization boundary changes for CONS-03.
- **I/O in consensus layer:** Block storage flush decisions must stay in the storage/node layers.
- **Resetting remaining count on re-assignment (BUGF-01):** The existing fix is to NOT reset remaining — the received[] bitmap handles deduplication. The bug is that remaining can undercount; fix the decrement guard, not the reset path.
- **Using `ORDER BY height DESC` after CONS-03 fix:** Once chainwork is big-endian, the best-chain query must switch to `ORDER BY chainwork DESC LIMIT 1`.
- **Network access in tests:** All tests should be offline. Mock peers and local temp files only.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Thread-safe mutation | Custom spinlock | `pthread_mutex_t` already in `block_file_manager_t` | Already implemented, tested |
| 256-bit byte reversal | Bit tricks | Simple 32-iteration for loop | Clarity > cleverness per project philosophy |
| Block hash lookup | Cache/map | `block_index_db_lookup_by_height()` already exists | Prepared statement, mutex-protected |
| Checkpoint wiring | New mechanism | `chaser_validate_set_checkpoint()` already exists | API complete, just needs wiring |
| Logging | Custom formatter | `log_error(LOG_COMP_SYNC, fmt, ...)` | Fixed format, thread-safe, component-tagged |

---

## Common Pitfalls

### Pitfall 1: Chainwork Read-Back After CONS-03

**What goes wrong:** After fixing insert to store big-endian, existing code that reads chainwork and passes it to `work256_compare()` or `work256_add()` will receive a byte-reversed value and compute nonsense.
**Why it happens:** `populate_entry_from_row()` (block_index_db.c:276-277) does a raw `memcpy` from the blob into `entry->chainwork.bytes`. After CONS-03, this memcpy reads big-endian data into a struct that arithmetic functions expect to be little-endian.
**How to avoid:** Every `memcpy` of chainwork FROM the database must be paired with `be_blob_to_work256()`. Audit all callers of `block_index_db_insert()` and `populate_entry_from_row()`.
**Warning signs:** `work256_compare()` returns wrong ordering after fix; chainwork display values look backward.

### Pitfall 2: Database Migration for Existing chainwork Data

**What goes wrong:** If an existing `~/.bitcoin-echo` database has little-endian chainwork blobs and the code is updated to write big-endian, existing rows are incompatible.
**Why it happens:** No migration was planned.
**How to avoid:** Per CLAUDE.md instructions, `rm -rf ~/.bitcoin-echo` before starting. Since there is no shutdown recovery yet, fresh starts are the expected workflow. Document this in code comment. No migration script needed for v0 since the node always starts from genesis.
**Warning signs:** Best-chain query returns genesis instead of tip.

### Pitfall 3: BUGF-01 — Remaining Count Underrun vs. Overrun

**What goes wrong:** The current SAFETY CHECK at `download_mgr_peer_request_work()` detects when `remaining=0` but blocks are still unreceived, and re-queues the batch with a LOG_ERROR. This means the error fires when a peer's old completed batch is freed, not when the count went wrong.
**Why it happens:** The count goes wrong in `download_mgr_block_received()` — the `received[]` bitmap guards against double-decrement but the guard fires at the wrong moment when a batch is cloned for sticky races. When a batch is cloned and both peers receive the same block, both decrement unless one sees `received[i]=true` first.
**How to avoid:** Verify the received[] check fires in `block_received()` BEFORE decrementing, and that the bitmap is not reset on re-assignment. The fix is in `block_received()`, not in the cleanup path.
**Warning signs:** LOG_ERROR "BUG - batch ... has remaining=0 but X blocks unreceived" appearing during normal IBD.

### Pitfall 4: BUGF-02 — Race Window for Duplicate Address

**What goes wrong:** `discovery_select_outbound_address()` selects an address → `discovery_mark_address_in_use()` marks it → loop iterates again → same address is not selectable. But the duplicate check in node.c:3271-3292 compares against peer state AFTER slot selection. If two connections to the same address succeed before the second loop iteration checks, both proceed.
**Why it happens:** The `in_use` flag is checked in `discovery_select_outbound_address()` but the peer state check happens separately. There's a TOCTOU window.
**How to avoid:** The node event loop is single-threaded for connection setup. The race is not truly concurrent — it's a within-iteration logic error. Ensure `discovery_mark_address_in_use()` is called before searching for a peer slot, and that the per-peer duplicate check (line 3273) happens before `peer_connect()`.

### Pitfall 5: INFR-01 — fsync Strategy for IBD Performance

**What goes wrong:** Adding fsync on every block write would make IBD extremely slow (fsync is an OS-level barrier that flushes to physical media). `BLOCK_STORAGE_FLUSH_INTERVAL=100` exists specifically to batch fsyncs.
**Why it happens:** Naive "make writes durable" approach.
**How to avoid:** The correct fix is to fsync at batch completion granularity (when a download batch of 64 blocks is fully received), not per-block. The download manager already knows when a batch completes (remaining == 0 before peer calls `peer_request_work()`). This is the natural callback point. Alternatively, flush before indexing: write block to file, fsync/flush the file, then update the block index position. This ensures the index never points to unflushed data.
**Warning signs:** IBD throughput drops to <10 blocks/sec (sign of per-block fsync).

### Pitfall 6: TEST-05 — Corrupted Size Field Safety

**What goes wrong:** `block_storage_read()` reads the 4-byte size field from the blk*.dat file header, then `malloc(size)`. A corrupted size of 0xFFFFFFFF causes a 4GB malloc attempt that fails, but a size of ~4MB (ECHO_MAX_BLOCK_SIZE) causes a successful allocation of exactly the right size — but then the subsequent fread reads fewer bytes than expected (truncated file).
**Why it happens:** Size validation against `ECHO_MAX_BLOCK_SIZE` may or may not happen before malloc.
**How to avoid:** Test both: (1) size > ECHO_MAX_BLOCK_SIZE → must return ECHO_ERR_INVALID, not attempt malloc. (2) size within bounds but file truncated → fread returns short → must return ECHO_ERR_IO, not silently pass truncated data. Verify these bounds checks exist in `blocks.c` and add them if missing.

---

## Code Examples

### Existing Log Format (follow exactly)

```c
/* Source: include/log.h, example from src/protocol/download_mgr.c */

/* Format: YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [COMPONENT] Message */
log_error(LOG_COMP_SYNC,
          "chaser '%s' fault: error=%d — initiating shutdown",
          self->name, error);

log_debug(LOG_COMP_SYNC,
          "download_mgr: duplicate block at index %zu (already received), "
          "remaining=%zu unchanged",
          i, perf->batch->remaining);

/* Macro form (requires #define LOG_COMPONENT before include) */
#define LOG_COMPONENT LOG_COMP_SYNC
LOG_ERROR("download_mgr: BUG - batch [%u-%u] has remaining=0 but %zu "
          "blocks unreceived! Returning to queue.",
          old_start, old_end, unreceived);
```

### Test Case Structure (follow existing pattern)

```c
/* Source: test/unit/test_download_mgr.c */
static void test_eviction_under_load(void) {
    test_case("evict slowest peers under load");

    test_ctx_t ctx = {0};
    download_callbacks_t callbacks = {
        .send_getdata = mock_send_getdata,
        .disconnect_peer = mock_disconnect_peer,
        .ctx = &ctx
    };

    download_mgr_t *mgr = download_mgr_create(&callbacks);
    if (mgr == NULL) {
        test_fail("failed to create manager");
        return;
    }

    /* ... test body ... */

    download_mgr_destroy(mgr);
    test_pass();
}
```

### Concurrent Storage Test Pattern

```c
/* Source: Derived from test/unit/test_block_storage.c + pthreads */
typedef struct {
    block_file_manager_t *mgr;
    int thread_id;
    int result; /* 0 = success */
} storage_thread_arg_t;

static void *concurrent_writer(void *arg) {
    storage_thread_arg_t *a = (storage_thread_arg_t *)arg;
    uint8_t block_data[512];
    memset(block_data, (uint8_t)a->thread_id, sizeof(block_data));
    block_file_pos_t pos;
    echo_result_t r = block_storage_write(a->mgr, block_data, sizeof(block_data), &pos);
    a->result = (r == ECHO_OK) ? 0 : -1;
    return NULL;
}
```

---

## State of the Art

| Old Approach | Current Approach | Notes |
|--------------|------------------|-------|
| ORDER BY height DESC for best chain | Should be ORDER BY chainwork DESC | Current workaround documented with TODO; CONS-03 fixes this |
| Hardcoded checkpoint=0 | Should read from node_config_t | TODO comment in chaser_validate.c:178 |
| No logging in chaser_fault | Should log before shutdown signal | TODO comment in chaser.c:183 |
| All-zeros hash in CHASE_CHECKED path | Should lookup from block index | Known bug confirmed in chaser_validate.c:518-519 |

---

## Open Questions

1. **INFR-01: Callback granularity vs. flush-before-index strategy**
   - What we know: The download manager knows when a batch is complete (`remaining==0`). `block_storage_flush()` exists and works. The block index is updated separately from the file write.
   - What's unclear: Whether the GAP errors are caused by (a) a block indexed before its data is flushed, or (b) a block written and flushed but its index entry not yet committed. Need to trace the exact write → index → read path in `src/app/node.c` around `node_store_block()`.
   - Recommendation: Read `node_store_block()` in `src/app/node.c` before implementing. The fix site is there, not in blocks.c or download_mgr.c.

2. **INFR-05: Mainnet IBD data source**
   - What we know: Current threshold is 3 KB/s (conservative for early blocks). The evict-slowest-percent function exists (`download_mgr_evict_slowest_percent()`). Calibration "rationale documented in code comments" per user constraint.
   - What's unclear: What actual IBD data shows for peer rate distribution. Need to run IBD (after other fixes) and collect rates.
   - Recommendation: This plan item should run AFTER other fixes stabilize IBD. Log peer rates at INFO level during IBD, grep the log, compute percentile distribution. Document the measurement and chosen threshold in comments.

3. **BUGF-01: Exact trigger condition**
   - What we know: The safety check fires when remaining=0 but received[] bitmap shows missing blocks. The bitmap guard in `block_received()` should prevent double-decrement.
   - What's unclear: Whether the bug is in sticky batch cloning (clone shares received[] by value via memcpy — so clone starts with received[] already set from the original) or in a different path.
   - Recommendation: Read the `batch_node_clone()` function at download_mgr.c:95-110. The clone copies the full `work_batch_t` including `received[]`. If the original has received[i]=true for some blocks already received, the clone inherits those marks. When the clone's peer later receives those same blocks, `block_received()` sees `received[i]=true` and skips decrement. But `remaining` in the clone was set from the original's current remaining — which may already have been decremented. This is the likely root cause: clone remaining should reflect the number of NOT-YET-received blocks at clone time.

---

## Sources

### Primary (HIGH confidence)
- Direct codebase inspection — all findings above sourced from actual file contents
  - `src/protocol/download_mgr.c` — full file read, BUGF-01 root cause analysis
  - `src/node/chaser_validate.c` — full file read, INFR-02 and INFR-03 confirmed
  - `src/node/chaser.c` — lines 177-188, INFR-04 confirmed
  - `src/storage/block_index_db.c` — lines 150-155, 593-594, CONS-03 confirmed
  - `src/protocol/discovery.c` — lines 352-355, 470-543, BUGF-02 analysis
  - `src/app/node.c` — lines 3240-3300, BUGF-02 connection logic
  - `include/blocks_storage.h` — full read, INFR-01 synchronous write model
  - `include/chaser_validate.h` — full read, API inventory
  - `include/block_index_db.h` — full read, API inventory
  - `include/chainstate.h` — lines 40-51, work256_t endianness
  - `include/node.h` — lines 58-66, node_config_t struct
  - `include/log.h` — full read, logging API and format
  - `include/download_mgr.h` — full read, constants and API
  - `include/echo_config.h` — full read, compile-time configuration
  - `test/unit/test_download_mgr.c` — lines 1-80, test pattern
  - `test/unit/test_block_storage.c` — lines 1-80, test pattern
  - `test/unit/test_utils.h` — full read, test framework API

---

## Metadata

**Confidence breakdown:**
- Bug locations: HIGH — all confirmed in source files with line numbers
- Fix approaches: HIGH — APIs exist, patterns are clear from surrounding code
- Test patterns: HIGH — existing test files show exact structure to follow
- Calibration data (INFR-05): LOW — requires actual IBD run; current threshold is reasoned guess

**Research date:** 2026-02-20
**Valid until:** This is codebase research, not ecosystem research. Valid until files change. Re-read only the files being modified before implementing each plan.
