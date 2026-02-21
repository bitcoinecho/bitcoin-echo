# Phase 5: Storage Layer and Core RPC - Research

**Researched:** 2026-02-21
**Domain:** SQLite tx indexing, block file I/O, RPC wiring in pure C
**Confidence:** HIGH

---

## Summary

Phase 5 adds a transaction index table to `block_index.db`, wires `getrawtransaction` to look up confirmed transactions, makes `getblock verbosity=0` return the raw witness-serialized block hex from disk, and computes a real median-time-past (MTP) for `getblockchaininfo`. All infrastructure — SQLite schema patterns, block file I/O, serialization, RPC dispatch — already exists and is production-quality. This phase is purely about wiring things together with minimal new code.

The four sub-plans (05-01 through 05-04) map directly to the four requirements. The only structural question is the reorg hook for txindex DELETE: the chaser_confirm reorganization loop in `chaser_confirm.c` is the correct call site. No new tables, no new databases, no new parsing infrastructure needed.

Two pre-checks flagged in STATE.md must be resolved before coding: (1) confirm the serialization format stored on disk is witness-inclusive (answer: YES — `node_store_block` calls `block_serialize()` which passes `ECHO_TRUE` to every `tx_serialize`); (2) confirm the txindex DELETE hook call site in `chaser_confirm.c` (answer: the reorg loop at lines 264–295 of `chaser_confirm.c` reverts blocks in reverse height order — txindex deletes must fire inside this loop, in the same SQLite transaction as the UTXO delta rollback).

**Primary recommendation:** Add `tx_index` table to `block_index_db.c` (same SQLite connection, no new db handle), populate during `node_apply_block`, delete during reorg in `chaser_confirm_reorganize`, and wire the two RPC stubs that have `TODO` comments already placed.

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| RPC-01 | Node maintains a transaction index mapping txid to block file position | `tx_index` table added to `block_index.db`; `txindex_insert` called from `node_apply_block`; `txindex_delete_by_block` called from reorg loop in `chaser_confirm_reorganize` |
| RPC-02 | User can query confirmed transactions by txid via `getrawtransaction` RPC | `rpc_getrawtransaction` has a `TODO: Implement transaction index` comment at line 1897; after RPC-01, this is a lookup + `block_storage_read` + `tx_serialize` hex response |
| RPC-03 | User can retrieve raw block hex via `getblock` RPC at verbosity=0 | `rpc_getblock` verbosity=0 branch has `TODO: Read block from storage` at line 1783; use `block_index_db_lookup_by_hash` for file pos, then `block_storage_read`, then `json_builder_hex` |
| RPC-04 | `getblockchaininfo` returns correct mediantime (median of previous 11 block timestamps) | `rpc_getblockchaininfo` has `TODO: implement MTP query` at line 1662; walk tip's `block_index_t` chain backwards 11 entries via `block_index_db_get_prev`, sort timestamps, return median |
</phase_requirements>

---

## Standard Stack

### Core (already present — no new dependencies)

| Component | Version | Purpose | Why Standard |
|-----------|---------|---------|--------------|
| SQLite (embedded) | vendored | tx_index table storage | Already used for `blocks` table in `block_index.db`; same `db_t` handle, same `db_exec`/`db_prepare`/`db_step` pattern |
| `block_index_db.{h,c}` | project | Schema home for tx_index | Existing prepared-statement pattern, same WAL-mode db, natural extension point |
| `blocks_storage.{h,c}` | project | Raw block file read for getblock and getrawtransaction | `block_storage_read(mgr, pos, &buf, &size)` already implemented |
| `block.{h,c}` | project | `block_serialize`/`block_parse`/`tx_serialize` | Already does witness-inclusive serialization via `ECHO_TRUE` |
| `rpc.{h,c}` | project | Method stubs already dispatched | Stubs exist with `TODO` markers; `json_builder_hex` helper already present |

### Supporting

| Component | Version | Purpose | When to Use |
|-----------|---------|---------|-------------|
| `block_index_db_lookup_by_hash` | project | Get file pos for getblock verbosity=0 | Entry has `data_file` and `data_pos`; already populated by `node_store_block` |
| `block_index_db_get_prev` | project | Walk chain backwards for MTP calculation | Returns parent entry with timestamp from stored 80-byte header |
| `consensus_lookup_block_index` | project | In-memory block index lookup | Used by getblockchaininfo for tip_index; same pattern for MTP walk |

### Alternatives Considered

| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| tx_index in block_index.db | Separate tx.db file | Separate db means two-phase commit complexity for reorg atomicity; same db is strictly simpler and correct |
| Walking in-memory block_index_map for MTP | Querying block_index_db for timestamps | In-memory map has timestamps in `block_index_t->timestamp`; db also has them in the 80-byte header blob; in-memory is faster and simpler — no extra I/O |
| Storing absolute file offset in tx_index | Storing (file_index, file_pos) | `block_file_pos_t` stores (file_index, file_offset); tx_index should store same tuple so `block_storage_read` can be called directly |

---

## Architecture Patterns

### Recommended Project Structure

No new files required. Changes touch:

```
include/block_index_db.h    # Add txindex_insert, txindex_lookup, txindex_delete_by_block
src/storage/block_index_db.c # Implement above functions + schema migration
src/app/node.c              # Call txindex_insert in node_apply_block
src/node/chaser_confirm.c   # Call txindex_delete_by_block in reorg loop
src/app/rpc.c               # Wire getrawtransaction, getblock v=0, mediantime
```

### Pattern 1: tx_index Schema (in block_index.db)

**What:** New table added to the existing `block_index.db` SQLite database.

**Schema:**
```sql
CREATE TABLE IF NOT EXISTS tx_index (
    txid      BLOB NOT NULL,           -- 32 bytes, txid (internal byte order)
    block_hash BLOB NOT NULL,           -- 32 bytes, which block contains this tx
    file_index INTEGER NOT NULL,        -- blk*.dat file number
    file_pos   INTEGER NOT NULL,        -- byte offset of block record in file
    tx_offset  INTEGER NOT NULL,        -- byte offset of this tx within the block
    PRIMARY KEY (txid)
);
CREATE INDEX IF NOT EXISTS idx_txindex_block ON tx_index(block_hash);
```

**Notes on design:**
- `txid` is the non-witness txid (sha256d of stripped serialization) — matches Bitcoin's canonical definition
- `block_hash` foreign key enables `DELETE FROM tx_index WHERE block_hash = ?` on reorg — O(1) per disconnected block without scanning all entries
- `file_index` + `file_pos` are the block-level position (same as `block_file_pos_t`); to retrieve a single tx, read the full block and scan for `tx_offset`
- Alternative: store the serialized tx directly in tx_index (avoid block re-read); this is NOT recommended — it doubles storage and violates the single-source-of-truth principle; block files are the canonical store
- `tx_offset` field: optional optimization; can be omitted and tx found by scanning deserialized block; for clarity the initial implementation should omit it (scan block) and add it later if needed

**Simpler schema (recommended for initial implementation):**
```sql
CREATE TABLE IF NOT EXISTS tx_index (
    txid       BLOB PRIMARY KEY,   -- 32 bytes
    block_hash BLOB NOT NULL,      -- 32 bytes (for reorg delete)
    file_index INTEGER NOT NULL,   -- blk*.dat number
    file_pos   INTEGER NOT NULL    -- byte offset of block record (includes 8-byte prefix)
);
CREATE INDEX IF NOT EXISTS idx_txindex_block ON tx_index(block_hash);
```

Lookup procedure for `getrawtransaction`:
1. `txindex_lookup(bdb, &txid, &block_hash, &pos)` — gets block file position
2. `block_storage_read(mgr, pos, &buf, &size)` — reads entire block bytes
3. `block_parse(buf, size, &block, NULL)` — deserializes block
4. Scan `block.txs[i]` to find matching txid, then `tx_serialize` it
5. `json_builder_hex(builder, tx_bytes, tx_len)`

This is correct but reads an entire block (up to 4MB) to return one tx. For Phase 5 correctness this is fine; tx_offset optimization is a future improvement.

### Pattern 2: node_apply_block Txindex Insertion

**What:** After consensus_apply_block succeeds in `node_apply_block`, iterate over all transactions in the block and insert one txindex row per tx.

**Call site:** `src/app/node.c` in `node_apply_block()` — after Step 1 (consensus) succeeds, before or alongside Step 2 (status update). The `txindex_insert_block` helper takes the entire block and file position, iterates internally, and executes in the same outer transaction if one is active (or opens its own).

**Pattern:**
```c
/* After consensus_apply_block succeeds */
if (node->block_index_db_open) {
    block_file_pos_t pos = {
        .file_index = stored_file_index,  /* from node_store_block result */
        .file_offset = stored_file_pos,
    };
    echo_result_t idx_result = txindex_insert_block(
        &node->block_index_db, &block_hash, block, &pos);
    if (idx_result != ECHO_OK) {
        /* Log warning but do not abort — txindex is best-effort on apply */
        log_warn(LOG_COMP_DB, "txindex insert failed for block %u: %d", height, idx_result);
    }
}
```

**Problem:** `node_store_block` (which records file position) is called separately from `node_apply_block`. The block's `data_file` and `data_pos` must be retrieved from `block_index_entry_t` after storage, not from the block struct itself. Look up via `block_index_db_lookup_by_hash` to get the stored position.

### Pattern 3: Reorg Txindex Deletion

**What:** In `chaser_confirm_reorganize`, inside the loop that reverts blocks (lines 264–295 of `chaser_confirm.c`), delete txindex entries for each disconnected block hash BEFORE reverting the UTXO delta. Both operations should be in the same SQLite transaction for atomicity.

**Pattern in chaser_confirm_reorganize:**
```c
for (uint32_t h = old_height; h > fork_point; h--) {
    /* Get the block hash at this height for txindex cleanup */
    hash256_t block_hash_at_h;
    /* ... look up hash for height h ... */

    /* Delete txindex entries for this disconnected block */
    txindex_delete_by_block(node_get_block_index_db(node), &block_hash_at_h);

    /* Revert UTXO delta (existing code) */
    const block_delta_t *delta = chainstate_get_delta(chaser->chainstate, h);
    /* ... existing revert logic ... */
}
```

**Atomicity requirement (from success criteria 4):** The txindex DELETE and UTXO delta rollback must be atomic. The `block_index_db` uses WAL mode. The txindex and blocks tables are in the same database. A `BEGIN`/`COMMIT` wrapping the entire reorg loop makes both the txindex deletes and any UTXO db writes atomic. Check whether `chaser_confirm_reorganize` already uses a transaction scope — if not, add one.

**Getting the hash at height h during reorg:** Use `block_index_db_get_chain_block(bdb, h, &entry)` which returns the best-chain block at that height. During a reorg this should still return the old chain block since `BLOCK_STATUS_VALID_CHAIN` has not yet been updated.

### Pattern 4: getblock verbosity=0

**What:** The `TODO` at rpc.c line 1783 needs to be replaced with actual block data retrieval.

**Pattern:**
```c
if (verbosity == 0) {
    /* Retrieve raw block bytes from storage */
    block_index_entry_t entry;
    block_index_db_t *bdb = node_get_block_index_db(node);
    echo_result_t res2 = block_index_db_lookup_by_hash(bdb, &block_hash, &entry);
    if (res2 != ECHO_OK) {
        return ECHO_ERR_NOT_FOUND;
    }
    if (entry.status & BLOCK_STATUS_PRUNED) {
        /* Pruned — return not-found per RPC-01 behavior */
        return ECHO_ERR_NOT_FOUND;
    }
    if (!(entry.status & BLOCK_STATUS_HAVE_DATA)) {
        return ECHO_ERR_NOT_FOUND;
    }
    block_file_pos_t pos = {
        .file_index = (uint32_t)entry.data_file,
        .file_offset = entry.data_pos,
    };
    uint8_t *block_bytes = NULL;
    uint32_t block_size = 0;
    block_file_manager_t *mgr = node_get_block_storage(node);
    res2 = block_storage_read(mgr, pos, &block_bytes, &block_size);
    if (res2 != ECHO_OK) {
        return res2;
    }
    json_builder_hex(builder, block_bytes, block_size);
    free(block_bytes);
    return ECHO_OK;
}
```

**Key fact confirmed:** `block_serialize` (called by `node_store_block`) passes `ECHO_TRUE` to every `tx_serialize` call, meaning stored blocks include witness data. So `block_storage_read` returns witness-serialized bytes — the bytes can be passed directly to `json_builder_hex` without re-serialization. This satisfies RPC-03 ("raw witness-serialized hex").

### Pattern 5: mediantime (MTP) Calculation

**What:** Bitcoin's Median Time Past (MTP) is the median of the timestamps of the 11 blocks before the current tip (heights `tip_height - 10` through `tip_height`). For `getblockchaininfo`, MTP at the tip means the median of the 11 most recent confirmed block timestamps.

**Standard algorithm:**
1. Collect up to 11 timestamps: for `i = 0..10`, look up block at `tip_height - i`
2. Sort the 11 timestamps
3. Return `timestamps[5]` (the middle value; index 5 in 0-based sorted array of 11)

**Data source options:**
- **In-memory `block_index_map_t`**: `consensus_lookup_block_index` returns `block_index_t*` which has `.timestamp`. Walk backwards via `.prev` pointer. Fastest, no I/O. Correct as long as the tip's 11 ancestors are in the in-memory map (they always will be during normal operation).
- **`block_index_db`**: Query `SELECT header FROM blocks WHERE height >= ? ORDER BY height DESC LIMIT 11` — but timestamp is inside the 80-byte header blob, requiring deserialization. Slower, but more correct for edge cases.

**Recommended:** Use the in-memory block_index_map traversal via `consensus_lookup_block_index`. It's already used in `rpc_getblockchaininfo` for `tip_index`. Walk backwards 11 steps using `block_index_map_lookup(map, &entry->prev_hash)`.

**Pattern:**
```c
/* Compute MTP at tip */
uint32_t timestamps[11];
int ts_count = 0;
const block_index_t *cur = tip_index;  /* already looked up above */
while (cur != NULL && ts_count < 11) {
    timestamps[ts_count++] = cur->timestamp;
    if (ts_count < 11 && cur->height > 0) {
        block_index_map_t *bim = chainstate_get_block_index_map(
            consensus_get_chainstate(consensus));
        cur = block_index_map_lookup(bim, &cur->prev_hash);
    } else {
        break;
    }
}
/* Sort ascending */
/* simple insertion sort for 11 elements */
uint32_t median = (ts_count > 0) ? timestamps[ts_count / 2] : 0;
json_builder_uint(builder, median);
```

**Note:** `consensus_get_chainstate` may not exist as a public API. Check `consensus.h`. Alternative: expose a helper `rpc_compute_median_time(node, &tip_hash, &mtp_out)` that accesses the block index map through `node_get_consensus` + existing lookup pattern.

### Anti-Patterns to Avoid

- **Opening a second SQLite connection** for tx_index: use the existing `block_index_db_t` handle on `node->block_index_db`. Separate connections to the same WAL-mode file are permitted but unnecessarily complex.
- **Storing tx bytes in tx_index table**: wastes disk (duplicates data already in blk*.dat), violates single-source-of-truth. Look up block, parse, find tx.
- **Reading block from disk to extract a single field during getblockchaininfo**: MTP uses in-memory block_index_t timestamps — zero I/O.
- **Skipping the `block_hash` index column on tx_index**: makes reorg cleanup an O(total_txs) full scan instead of O(block_tx_count). Always index by block_hash for DELETE.
- **Attempting atomicity with two separate SQLite databases**: tx_index and blocks are in the same `block_index.db` file — a single `BEGIN`/`COMMIT` covers both.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Hex encoding of block bytes | Custom hex encoder | `json_builder_hex(builder, data, len)` | Already implemented in rpc.c; handles arbitrary byte arrays |
| Block deserialization for tx scan | Custom parser | `block_parse(buf, size, &block, NULL)` | Already handles both legacy and SegWit wire format |
| TX serialization for getrawtransaction response | Custom serializer | `tx_serialize(&tx, ECHO_TRUE, buf, size, &written)` | Already witness-aware |
| Transaction ID computation | Hand-roll SHA256d | `tx_compute_txid(&tx, &txid)` | Already exists in tx.h; computes non-witness txid correctly |
| Sorting 11 timestamps | stdlib qsort | Simple 11-element insertion sort in-place | 11 elements: insertion sort is cleaner than setting up qsort comparator |
| SQLite schema creation | Direct sqlite3 calls | `db_exec(db, "CREATE TABLE IF NOT EXISTS ...")` | Existing pattern in block_index_db.c create_schema() |
| Prepared statements | Direct sqlite3 calls | `db_prepare`, `db_bind_blob`, `db_step` | Existing pattern; avoid raw sqlite3_* calls outside db.c wrapper |

**Key insight:** Every primitive needed for Phase 5 already exists. The work is integration, not invention.

---

## Common Pitfalls

### Pitfall 1: Block File Position Mismatch Between node_store_block and node_apply_block

**What goes wrong:** `node_apply_block` calls `consensus_apply_block` but does not store the block — `node_store_block` is a separate call made by the sync layer. By the time `node_apply_block` runs, the block may or may not have its file position in the db yet.

**Why it happens:** The two operations (store + apply) are intentionally decoupled. `node_store_block` fires during download/validation; `node_apply_block` fires during confirmation.

**How to avoid:** In `node_apply_block`, after step 1 (consensus), look up the block's `data_file`/`data_pos` from `block_index_db_lookup_by_hash`. If `data_file == -1` (not stored), the block data is not on disk yet — skip txindex insertion with a warning. This situation should be rare (store always precedes apply) but must be handled.

**Warning signs:** txindex entries exist in db but `block_storage_read` fails with NOT_FOUND.

### Pitfall 2: Reorg Atomicity — txindex DELETE Outside the UTXO Transaction

**What goes wrong:** If txindex deletes are committed separately from the UTXO delta rollback, a crash between the two leaves the node in an inconsistent state (txindex says tx not confirmed; UTXO set says it is, or vice versa).

**Why it happens:** `chaser_confirm_reorganize` currently does not wrap the reorg loop in an explicit `BEGIN`/`COMMIT`. Each `chainstate_revert_block` may or may not internally use a transaction.

**How to avoid:** Wrap the entire reorg loop in `block_index_db_begin(bdb)` / `block_index_db_commit(bdb)`. Issue `txindex_delete_by_block` inside this transaction for each disconnected block hash before calling `chainstate_revert_block`.

**Warning signs:** After a crash during reorg, `getrawtransaction` returns a transaction that is actually in the UTXO set as unspent (double-spend risk if the node restarts and the reorg-rolled-back block's tx could be replayed).

### Pitfall 3: Serialization Format Stored on Disk vs. Wire Format

**What goes wrong:** A developer assumes blk*.dat files contain legacy (non-witness) serialized blocks and tries to re-serialize with witness before returning from getblock.

**Why it happens:** The STATE.md pre-check item said "Confirm stored block serialization format — if IBD used INV_BLOCK, blocks may be legacy-serialized."

**Resolution (CONFIRMED):** `node_store_block` calls `block_serialize()`, which calls `tx_serialize(..., ECHO_TRUE)` — witness-inclusive. The blk*.dat files always contain witness-serialized blocks. `block_storage_read` returns witness bytes directly. No re-serialization needed for getblock verbosity=0.

**Warning signs:** If you ever call `block_serialize` on a parsed block before returning it via RPC, you'd double-witness-serialize (no-op functionally, but wasteful).

### Pitfall 4: txid Byte Order in tx_index Table

**What goes wrong:** Storing txid in display byte order (reversed, as shown to users) instead of internal byte order. Then lookups fail because the RPC layer calls `rpc_parse_hash` which reverses the hex string into internal order.

**Why it happens:** Bitcoin's display convention reverses the 32 bytes of a hash for human readability. The internal `hash256_t` is little-endian (first byte = LSB in display).

**How to avoid:** Store `hash256_t.bytes` directly (as returned by `tx_compute_txid`). The lookup key passed from `rpc_getrawtransaction` is already in internal order after `rpc_parse_hash`. Consistent: always store and query in internal byte order.

**Warning signs:** `SELECT * FROM tx_index WHERE txid = ?` returns zero rows for known transactions.

### Pitfall 5: MTP Walk Hitting NULL Block Index Before 11 Blocks

**What goes wrong:** At low chain heights (height < 10), there are fewer than 11 ancestors. Walking unconditionally to 11 ancestors dereferences NULL.

**Why it happens:** Genesis block has no parent; the loop must check for height == 0 and stop.

**How to avoid:** In the MTP walk loop, check `cur->height == 0` before following `cur->prev_hash`. Cap the count at `ts_count < 11 && ts_count <= cur->height`.

### Pitfall 6: Pruned Block Returns Not-Found vs. Internal Error

**What goes wrong:** For a pruned block, `block_storage_read` may fail with a file-not-found error, which gets propagated as `RPC_ERR_INTERNAL_ERROR` instead of the correct `RPC_ERR_BLOCK_NOT_FOUND` / `RPC_ERR_TX_NOT_FOUND`.

**Why it happens:** The storage layer returns a raw I/O error; the RPC layer must interpret it correctly.

**How to avoid:** Before calling `block_storage_read`, check `entry.status & BLOCK_STATUS_PRUNED`. If set, return `ECHO_ERR_NOT_FOUND` directly — don't attempt the disk read.

---

## Code Examples

### Example 1: txindex_insert (one tx row)

```c
/* Source: db.h / block_index_db.c pattern */
echo_result_t txindex_insert(block_index_db_t *bdb,
                             const hash256_t *txid,
                             const hash256_t *block_hash,
                             uint32_t file_index,
                             uint32_t file_pos) {
    pthread_mutex_lock(&bdb->mutex);

    echo_result_t r = db_stmt_reset(&bdb->txindex_insert_stmt);
    if (r != ECHO_OK) { pthread_mutex_unlock(&bdb->mutex); return r; }

    r = db_bind_blob(&bdb->txindex_insert_stmt, 1, txid->bytes, 32);
    if (r != ECHO_OK) { pthread_mutex_unlock(&bdb->mutex); return r; }

    r = db_bind_blob(&bdb->txindex_insert_stmt, 2, block_hash->bytes, 32);
    if (r != ECHO_OK) { pthread_mutex_unlock(&bdb->mutex); return r; }

    r = db_bind_int64(&bdb->txindex_insert_stmt, 3, (int64_t)file_index);
    if (r != ECHO_OK) { pthread_mutex_unlock(&bdb->mutex); return r; }

    r = db_bind_int64(&bdb->txindex_insert_stmt, 4, (int64_t)file_pos);
    if (r != ECHO_OK) { pthread_mutex_unlock(&bdb->mutex); return r; }

    echo_result_t step = db_step(&bdb->txindex_insert_stmt);
    pthread_mutex_unlock(&bdb->mutex);

    return (step == ECHO_DONE) ? ECHO_OK : step;
}
```

### Example 2: txindex_lookup

```c
echo_result_t txindex_lookup(block_index_db_t *bdb,
                             const hash256_t *txid,
                             hash256_t *block_hash_out,
                             uint32_t *file_index_out,
                             uint32_t *file_pos_out) {
    pthread_mutex_lock(&bdb->mutex);

    db_stmt_reset(&bdb->txindex_lookup_stmt);
    db_bind_blob(&bdb->txindex_lookup_stmt, 1, txid->bytes, 32);

    echo_result_t step = db_step(&bdb->txindex_lookup_stmt);
    if (step != ECHO_OK) {
        pthread_mutex_unlock(&bdb->mutex);
        return (step == ECHO_DONE) ? ECHO_ERR_NOT_FOUND : step;
    }

    /* column 0: block_hash, column 1: file_index, column 2: file_pos */
    const void *bhash = db_column_blob(&bdb->txindex_lookup_stmt, 0);
    memcpy(block_hash_out->bytes, bhash, 32);
    *file_index_out = (uint32_t)db_column_int64(&bdb->txindex_lookup_stmt, 1);
    *file_pos_out   = (uint32_t)db_column_int64(&bdb->txindex_lookup_stmt, 2);

    pthread_mutex_unlock(&bdb->mutex);
    return ECHO_OK;
}
```

### Example 3: txindex_delete_by_block

```c
echo_result_t txindex_delete_by_block(block_index_db_t *bdb,
                                      const hash256_t *block_hash) {
    pthread_mutex_lock(&bdb->mutex);

    db_stmt_reset(&bdb->txindex_delete_block_stmt);
    db_bind_blob(&bdb->txindex_delete_block_stmt, 1, block_hash->bytes, 32);

    echo_result_t step = db_step(&bdb->txindex_delete_block_stmt);
    pthread_mutex_unlock(&bdb->mutex);

    /* DONE is success (even if zero rows deleted) */
    return (step == ECHO_DONE || step == ECHO_OK) ? ECHO_OK : step;
}
```

### Example 4: MTP in-memory walk

```c
/* In rpc_getblockchaininfo, after tip_index is already looked up */
uint32_t mtp = 0;
if (tip_index != NULL) {
    uint32_t timestamps[11];
    int ts_count = 0;
    const block_index_t *cur = tip_index;
    block_index_map_t *bim =
        chainstate_get_block_index_map(consensus_get_chainstate(consensus));

    while (cur != NULL && ts_count < 11) {
        timestamps[ts_count++] = cur->timestamp;
        if (cur->height == 0) break;
        cur = block_index_map_lookup(bim, &cur->prev_hash);
    }

    /* Insertion sort (max 11 elements) */
    for (int i = 1; i < ts_count; i++) {
        uint32_t key = timestamps[i];
        int j = i - 1;
        while (j >= 0 && timestamps[j] > key) {
            timestamps[j + 1] = timestamps[j];
            j--;
        }
        timestamps[j + 1] = key;
    }

    mtp = (ts_count > 0) ? timestamps[ts_count / 2] : 0;
}
json_builder_uint(builder, mtp);
```

### Example 5: getblock verbosity=0 pattern

```c
if (verbosity == 0) {
    block_index_entry_t entry;
    block_index_db_t *bdb = node_get_block_index_db(node);
    echo_result_t res2 = block_index_db_lookup_by_hash(bdb, &block_hash, &entry);
    if (res2 != ECHO_OK) {
        return rpc_response_error(req->id, RPC_ERR_BLOCK_NOT_FOUND,
                                  "Block not found", builder);
    }
    if ((entry.status & BLOCK_STATUS_PRUNED) ||
        !(entry.status & BLOCK_STATUS_HAVE_DATA) ||
        entry.data_file < 0) {
        return rpc_response_error(req->id, RPC_ERR_BLOCK_NOT_FOUND,
                                  "Block not available (pruned)", builder);
    }
    block_file_pos_t pos = {
        .file_index  = (uint32_t)entry.data_file,
        .file_offset = entry.data_pos,
    };
    uint8_t *block_bytes = NULL;
    uint32_t block_sz = 0;
    block_file_manager_t *mgr = node_get_block_storage(node);
    res2 = block_storage_read(mgr, pos, &block_bytes, &block_sz);
    if (res2 != ECHO_OK) {
        return res2;
    }
    json_builder_hex(builder, block_bytes, block_sz);
    free(block_bytes);
    return ECHO_OK;
}
```

---

## State of the Art

| Old Approach | Current Approach | Notes |
|--------------|------------------|-------|
| mediantime = 0 (stub) | MTP from in-memory block_index_map walk | Zero I/O, 11-element sort |
| getrawtransaction = mempool only | mempool + txindex db lookup | txindex populated at apply-time |
| getblock v=0 = empty string | block_storage_read + hex | witness-serialized passthrough |
| txindex missing entirely | tx_index table in block_index.db | Same db, reorg-safe via block_hash index |

**Deprecated/not applicable:**
- Bitcoin Core stores txindex in a separate LevelDB; Echo uses SQLite in the same file — simpler and sufficient.

---

## Open Questions

1. **Does `consensus_get_chainstate` exist as a public API?**
   - What we know: `chainstate_get_block_index_map` exists; `node_get_consensus` exists
   - What's unclear: Whether the consensus engine exposes chainstate directly for in-memory block_index_map access in the MTP walk, or whether a different path is needed
   - Recommendation: Check `consensus.h` for an accessor. If absent, either add one or use `block_index_db_lookup_by_height` for the 11-block walk instead. The db approach adds ~11 SQLite queries per RPC call — acceptable for MTP on getblockchaininfo which is low-frequency.

2. **Should txindex_insert happen in node_apply_block or node_store_block?**
   - What we know: `node_store_block` writes the block to disk and has the exact file position. `node_apply_block` does consensus + UTXO update.
   - What's unclear: The exact sequencing — which runs first in the confirmation path?
   - Recommendation: Insert txindex in `node_apply_block` (after consensus succeeds), retrieving the file position from `block_index_db_lookup_by_hash`. This keeps txindex consistent with UTXO state: a tx is indexable only after its block is confirmed. If block not yet stored (edge case), log and skip — the index will be incomplete but safe.

3. **Is there a transaction boundary wrapping the reorg loop in chaser_confirm_reorganize?**
   - What we know: The loop calls `chainstate_revert_block` which operates on the in-memory UTXO set; the UTXO db is flushed separately
   - What's unclear: Whether there is already a BEGIN/COMMIT around the block_index_db operations during reorg
   - Recommendation: Add explicit `block_index_db_begin(bdb)` / `block_index_db_commit(bdb)` in `chaser_confirm_reorganize` to wrap all txindex deletes atomically. Rollback on any failure.

4. **Does the existing getrawtransaction need to check for pruned txs via block_hash and then BLOCK_STATUS_PRUNED?**
   - What we know: RPC-02 says pruned block txids return not-found matching Bitcoin Core behavior
   - Recommendation: After txindex lookup succeeds (has block_hash + file_pos), check `block_index_db_lookup_by_hash` for BLOCK_STATUS_PRUNED. If pruned, return `RPC_ERR_TX_NOT_FOUND`. This is required for success criteria 1.

---

## Sources

### Primary (HIGH confidence)

- `/Users/yayseth/Projects/echo/bitcoin-echo/src/app/rpc.c` — Current stub implementations with explicit `TODO` markers; lines 1783 (getblock v=0), 1897 (getrawtransaction), 1662 (mediantime)
- `/Users/yayseth/Projects/echo/bitcoin-echo/src/storage/block_index_db.c` — Schema creation pattern, prepared statement registration pattern, mutex usage pattern
- `/Users/yayseth/Projects/echo/bitcoin-echo/include/block_index_db.h` — Full API surface; `block_index_entry_t` has `data_file`/`data_pos` fields
- `/Users/yayseth/Projects/echo/bitcoin-echo/include/blocks_storage.h` — `block_storage_read` API; `block_file_pos_t` type definition
- `/Users/yayseth/Projects/echo/bitcoin-echo/src/consensus/block.c` lines 206–279 — Confirms `block_serialize` uses `ECHO_TRUE` (witness-inclusive) for all txs
- `/Users/yayseth/Projects/echo/bitcoin-echo/src/node/chaser_confirm.c` lines 236–312 — Reorg loop structure; correct call site for txindex DELETE
- `/Users/yayseth/Projects/echo/bitcoin-echo/src/app/node.c` lines 1480–1513 — `node_store_block` calls `block_serialize` (witness-inclusive); lines 3618–3676 — `node_apply_block` structure

### Secondary (MEDIUM confidence)

- Bitcoin Core source precedent: txindex is a separate index from the UTXO set, updated atomically with block connect/disconnect — same model implemented here via block_index.db
- BIP-141 (SegWit): witness serialization format is what `block_serialize` produces via `ECHO_TRUE`; returned verbatim as getblock v=0 hex

### Tertiary (LOW confidence, needs validation)

- Whether `consensus_get_chainstate` is exposed as a public function — not verified in `consensus.h` scan; check before relying on in-memory MTP walk

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — all libraries already exist and in use
- Architecture: HIGH — call sites, schemas, and patterns derived from reading actual source; two pre-checks from STATE.md resolved by reading source
- Pitfalls: HIGH — derived from actual code logic, not speculation
- Open questions: MEDIUM — three small API surface questions that resolve during plan-writing

**Research date:** 2026-02-21
**Valid until:** Indefinitely (pure C project; no external library churn)
