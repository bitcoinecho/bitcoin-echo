# Architecture Research

**Domain:** Bitcoin full node — v1.1 Network Participant milestone (C11)
**Researched:** 2026-02-21
**Confidence:** HIGH (direct codebase analysis of post-v1.0 state)

---

## Context: What v1.0 Delivered

v1.0 is complete. The node IBDs mainnet correctly with:
- Chainwork stored big-endian (fork selection correct)
- Block flush-before-index (no GAP errors)
- Real block hash in chaser_validate (not all-zeros)
- BIP-342 Tapscript OP_CHECKSIGADD with unknown key type dispatch
- Full UTXO rollback on chain reorganization (delta undo system)
- Peer eviction threshold calibrated from mainnet data
- 1098/1098 tests passing

This document covers only the **NEW** work for v1.1: P2P block serving,
BIP-125 full-RBF mempool, transaction index, and RPC expansion (getblock
raw hex, mediantime, getblocktemplate).

---

## System Overview

The four-layer architecture is unchanged. New features slot into existing
layers without restructuring. Each feature maps cleanly to one or two layers.

```
┌──────────────────────────────────────────────────────────────────────┐
│                    APPLICATION LAYER  (src/app/)                      │
│                                                                        │
│  ┌──────────────────┐  ┌──────────────────────────────────────────┐   │
│  │    node.c        │  │                  rpc.c                    │   │
│  │                  │  │                                           │   │
│  │  MODIFIED:       │  │  MODIFIED:                                │   │
│  │  - Add NODE_     │  │  - getblock verbosity=0 (read blk*.dat)   │   │
│  │    WITNESS to    │  │  - getblockchaininfo mediantime (real MTP) │   │
│  │    services      │  │  - getrawtransaction confirmed (tx_index)  │   │
│  │  - Implement     │  │  - getblocktemplate witness commitment,    │   │
│  │    getdata block │  │    real MTP mintime, segwit rules field    │   │
│  │    serving       │  │                                           │   │
│  └──────────────────┘  └──────────────────────────────────────────┘   │
│                                                                        │
├────────────────────────────────────────────────────────────────────────┤
│                   PROTOCOL LAYER  (src/protocol/)                      │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────┐     │
│  │                     mempool.c                                  │     │
│  │                                                                │     │
│  │  MODIFIED:                                                     │     │
│  │  - Implement BIP-125 full-RBF (rules 1-5) in mempool_add()    │     │
│  │  - Replace TODO stub at line 797 with actual replacement logic │     │
│  │  - Conflict detection already in place; replacement path missing│     │
│  └──────────────────────────────────────────────────────────────┘     │
│                                                                        │
│  download_mgr.c  peer.c  relay.c  sync.c  — UNCHANGED                 │
│                                                                        │
├────────────────────────────────────────────────────────────────────────┤
│         NODE / CHASER COORDINATION LAYER  (src/node/)                  │
│                                                                        │
│  chaser_validate.c  chaser_confirm.c  chaser.c  — UNCHANGED            │
│                                                                        │
├────────────────────────────────────────────────────────────────────────┤
│            CONSENSUS LAYER  (src/consensus/)  *** FROZEN ***           │
│                                                                        │
│  script.c  chainstate.c  block_validate.c  tx_validate.c  utxo.c      │
│  — ALL UNCHANGED (v1.0 completed consensus work)                       │
│                                                                        │
├────────────────────────────────────────────────────────────────────────┤
│                   STORAGE LAYER  (src/storage/)                        │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────┐     │
│  │              block_index_db.c / db.h                          │     │
│  │                                                                │     │
│  │  NEW:                                                          │     │
│  │  - tx_index SQLite table (txid → file_index, file_offset,     │     │
│  │    tx_offset within block)                                     │     │
│  │  - block_index_db_txindex_insert() — called per-tx on apply   │     │
│  │  - block_index_db_txindex_lookup() — called by rpc_getrawtx   │     │
│  │  - block_index_db_get_median_time() — queries last 11 headers  │     │
│  └──────────────────────────────────────────────────────────────┘     │
│                                                                        │
│  blocks.c (block_file_manager_t)  utxo_db.c  — UNCHANGED              │
│                                                                        │
├────────────────────────────────────────────────────────────────────────┤
│   CRYPTO LAYER (src/crypto/)    PLATFORM (src/platform/)  — UNCHANGED  │
└────────────────────────────────────────────────────────────────────────┘
```

---

## Component Boundaries: New vs Modified vs Unchanged

### What is NEW (no prior implementation)

| Component | What Is New | Location |
|-----------|-------------|----------|
| Transaction index (txindex) | SQLite table `tx_index` and its two functions | `src/storage/block_index_db.c` |
| txindex population | Write one row per confirmed tx in `node_apply_block()` | `src/app/node.c` |
| txindex read | `txindex_lookup()` called from `rpc_getrawtransaction` | `src/storage/block_index_db.c` |
| MTP query | `block_index_db_get_median_time()` walking last 11 headers | `src/storage/block_index_db.c` |
| BIP-125 replacement logic | Replacement decision + eviction in `mempool_add()` | `src/protocol/mempool.c` |

### What is MODIFIED (stub or incorrect implementation exists)

| Component | Current State | What Changes |
|-----------|--------------|--------------|
| `node.c` services flags | `services = node_is_pruning_enabled(node) ? 0 : 1` (no NODE_WITNESS) | Add `SERVICE_NODE_WITNESS` (bit 3) to services |
| `node.c` getdata handler | Pruning check present, then `/* TODO: Full block serving */` | Implement block read + serialize + send |
| `rpc.c` `rpc_getblock` verbosity=0 | Returns empty string `""` | Read block from blk*.dat, hex-encode, return |
| `rpc.c` `rpc_getblockchaininfo` mediantime | Returns hardcoded `0` | Call `block_index_db_get_median_time()` |
| `rpc.c` `rpc_getblock` mediantime field | Returns `index->timestamp` (wrong, should be MTP) | Call `block_index_db_get_median_time()` |
| `rpc.c` `rpc_getblocktemplate` | Functional scaffold, missing: witness commitment, real MTP for mintime, `"rules": ["segwit"]` | Add witness commitment output, wire real MTP, add segwit rules field |
| `rpc.c` `rpc_getrawtransaction` | Checks mempool only, returns NOT_FOUND for confirmed | Search txindex after mempool miss |
| `mempool.c` RBF | Detects signaling correctly, rejects all conflicts with TODO | Implement all 5 BIP-125 replacement rules |

### What is UNCHANGED

Everything in: consensus/, crypto/, platform/, chaser_validate.c,
chaser_confirm.c, chaser.c, blocks.c, utxo_db.c, peer.c, sync.c,
download_mgr.c, relay.c, discovery.c, protocol_serialize.c.

---

## Data Flow Changes

### 1. P2P Block Serving

**Current flow (node.c getdata handler):**
```
Peer sends: getdata(INV_BLOCK | INV_WITNESS_BLOCK, hash)
    |
    v
node.c checks if pruned → sends notfound if pruned
    |
    v
/* TODO: Full block serving */   ← STOPS HERE
```

**Correct flow after fix:**
```
Peer sends: getdata(INV_BLOCK | INV_WITNESS_BLOCK, hash)
    |
    v
node.c:
  1. block_index_db_lookup_by_hash(&node->block_index_db, hash, &entry)
     → if not found: send notfound, done
  2. If entry.status & BLOCK_STATUS_PRUNED: send notfound, done
  3. block_storage_read(&node->block_file_mgr, entry.data_file,
                        entry.data_pos, &raw_bytes, &raw_size)
     → if error: send notfound, done
  4. Determine serialization:
     - INV_WITNESS_BLOCK: send raw_bytes as-is (already witness-serialized)
     - INV_BLOCK: strip witness data (re-parse block, re-serialize without)
  5. Build msg_t with MSG_BLOCK payload from raw bytes
  6. peer_queue_message(peer, &block_msg)
```

Key implementation note: Bitcoin Core stores blocks in witness-serialized form
in blk*.dat (the canonical on-disk format includes witness). For INV_BLOCK
(pre-segwit peers), the node must strip witness data before sending. For
INV_WITNESS_BLOCK, the raw bytes can be sent directly.

**Layer touch:** App only. No consensus, no storage schema change.

### 2. NODE_WITNESS Service Flag

**Current flow:**
```
node.c handshake: services = node_is_pruning_enabled(node) ? 0 : 1
   ↓ (SERVICE_NODE_NETWORK only — no witness flag)
peer_send_version(peer, services, height, relay)
   ↓ (peers use INV_BLOCK for downloads, not INV_WITNESS_BLOCK)
```

**Correct flow after fix:**
```
services = node_is_pruning_enabled(node) ? 0 : SERVICE_NODE_NETWORK;
services |= SERVICE_NODE_WITNESS;  /* BIP-144: bit 3 */
peer_send_version(peer, services, height, relay)
   ↓ (peers now advertise INV_WITNESS_BLOCK in their inv messages)
```

Advertising NODE_WITNESS causes peers to use `INV_WITNESS_BLOCK` (type
`0x40000002`) in subsequent inv messages instead of `INV_BLOCK` (type 2).
This must be set before block serving is tested because the peer's inv type
determines which getdata requests we receive. The SERVICE_NODE_WITNESS constant
(`1 << 3 = 8`) is already defined in `include/protocol.h:45`.

Additionally, `sync_cb_send_getdata_blocks()` in node.c must use
`INV_WITNESS_BLOCK` when requesting blocks from peers that advertise
NODE_WITNESS (currently hardcoded as INV_BLOCK per the TODO at line 1779).

**Layer touch:** App (node.c) only. One-line constants change + one-line
getdata type selection in sync callback.

### 3. BIP-125 Full-RBF

**Current flow (mempool.c, starting at line 797):**
```
mempool_add() has_conflict = true, at least one spender signals RBF
    |
    v
/* TODO: Implement full RBF replacement logic */
/* For now, reject conflicts even with RBF signaling */
result->reason = MEMPOOL_REJECT_CONFLICT;
return ECHO_ERR_INVALID;
```

**Correct flow after fix:**
```
has_conflict = true, at least one spender signals RBF
    |
    v
1. Collect all directly conflicting entries (those spending same outpoints)
2. Walk descendants of each conflicting entry to build full replacement set
3. BIP-125 Rule 1: replacement tx must signal RBF (check new tx inputs)
4. BIP-125 Rule 2: replacement count = |direct_conflicts| +
                   |all_descendants| <= MEMPOOL_MAX_REPLACEMENT_COUNT (100)
   → if exceeded: MEMPOOL_REJECT_RBF_TOO_MANY_REPLACED
5. BIP-125 Rule 3: replacement absolute fee > sum(all_replaced_fees)
   → NOT just fee rate — absolute total fee; MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE
6. BIP-125 Rule 4: replacement fee rate > ALL conflicting tx fee rates
7. BIP-125 Rule 5: replacement fee - sum(replaced fees) >= MEMPOOL_RBF_INCREMENT
   (the "relay fee increment" — ensures relay is economically rational)
    |
    v
All rules pass:
  - Remove each conflicting entry and its descendants (mempool_remove_chain)
  - Insert replacement tx normally
  - MEMPOOL_ACCEPT_OK
```

Existing data structures (`mempool_entry_t.signals_rbf`, `mempool_entry_t.fee`,
`mempool_entry_t.ancestor_fees`, `MEMPOOL_RBF_INCREMENT`, `MEMPOOL_MAX_REPLACEMENT_COUNT`)
are already in place. The `has_conflict` detection (lines 698-716) already
identifies which existing entries conflict. The missing piece is the replacement
decision tree and eviction of the displaced transactions + their descendants.

The descendant graph walk is the hard part: the mempool uses a `next/prev`
linked list (`struct mempool_entry *next`) but no explicit ancestor/descendant
pointer chain. Walking descendants requires scanning all entries to find those
spending outputs of the conflict set. With a 300 MB mempool cap and 25-deep
descendant limit, this is bounded but not trivial.

**Layer touch:** Protocol layer (mempool.c) only. No cross-layer changes.

### 4. Transaction Index (txindex)

**Current state:** `getrawtransaction` for confirmed transactions returns
`ECHO_ERR_NOT_FOUND` (line 1899 in rpc.c: `/* TODO: Implement transaction
index for confirmed txs */`).

**New storage schema:**
```sql
CREATE TABLE IF NOT EXISTS tx_index (
  txid        BLOB NOT NULL PRIMARY KEY,  /* 32 bytes */
  file_index  INTEGER NOT NULL,           /* blk*.dat file number */
  file_offset INTEGER NOT NULL,           /* byte offset within file to block start */
  tx_offset   INTEGER NOT NULL            /* byte offset within block to tx start */
);
CREATE INDEX IF NOT EXISTS idx_txindex_txid ON tx_index(txid);
```

The table lives in `block_index.db` (same SQLite database as blocks table),
opened by `block_index_db_open()`. Adding it there avoids a second database
file and reuses the existing WAL handle and mutex.

**Population flow:**
```
node_apply_block(node, block)
    |
    v
(existing: consensus_apply_block, node_store_block, utxo_db updates)
    |
    v
NEW: for each tx in block:
  block_index_db_txindex_insert(&node->block_index_db,
                                &tx_hash,
                                stored_pos.file_index,
                                stored_pos.file_offset,
                                tx_byte_offset_within_block)
```

The `tx_byte_offset_within_block` must be computed during block serialization
in `node_store_block()`. The block is serialized to raw bytes; each
transaction's offset within that byte array is known during serialization.
Alternatively: store `file_offset` as the offset to the block start, and
`tx_offset` as the byte position of the tx start relative to the block's
transaction section. On lookup, read the full block from `file_offset`,
then skip to `tx_offset` bytes into the transaction section.

**Lookup flow:**
```
rpc_getrawtransaction(node, txid)
    |
    v
1. mempool_lookup(mp, txid) → if found: return serialized mempool tx
2. block_index_db_txindex_lookup(&node->block_index_db, txid, &file_idx,
                                  &file_off, &tx_off)
   → if not found: ECHO_ERR_NOT_FOUND
3. block_storage_read(&node->block_file_mgr, {file_idx, file_off},
                       &raw_block_bytes, &raw_block_size)
4. Parse block from raw_block_bytes
5. Find and return the tx at position tx_off (or by hash match)
```

**Layer touch:** Storage (block_index_db.c schema + two functions), App (node.c
population call in `node_apply_block()`, rpc.c lookup call).

### 5. Mediantime Past (MTP)

**Current state:** Both `rpc_getblockchaininfo` (returns hardcoded `0`) and
`rpc_getblock` (returns `index->timestamp`, the block's own timestamp) and
`rpc_getblocktemplate` (uses `plat_time_ms() / 1000 - 600` as approximation)
are wrong.

MTP is defined as the median of the timestamps of the 11 blocks preceding the
current tip. It is used as a locktime floor for time-locked transactions.

**Correct computation:**
```
block_index_db_get_median_time(bdb, tip_height, &mtp):
  1. Collect timestamps of up to 11 blocks ending at tip_height
     (use block_index_db_lookup_by_height in a loop or single SQL query)
  2. Sort the 11 timestamps
  3. Return the median (element at index 5)
```

This requires walking back through block headers, which `block_index_db_t`
already supports via `block_index_db_lookup_by_height()`. A dedicated SQL
query is more efficient: `SELECT header FROM blocks WHERE height >= ? AND
height < ? AND (status & 8) ORDER BY height` then extract timestamps from
the 80-byte header blobs.

**Layer touch:** Storage (block_index_db.c new function), App (rpc.c three
call sites).

### 6. getblock verbosity=0 (raw hex)

**Current state:** Returns empty string `""` (rpc.c line 1790-1792).

**Correct flow:**
```
rpc_getblock(node, hash, verbosity=0)
    |
    v
1. block_index_db_lookup_by_hash(&node->block_index_db, hash, &entry)
   → not found: RPC_ERR_BLOCK_NOT_FOUND
   → pruned: RPC_ERR_BLOCK_NOT_FOUND (consistent with Bitcoin Core behavior)
2. block_storage_read(&node->block_file_mgr,
                      {entry.data_file, entry.data_pos},
                      &raw_bytes, &raw_size)
3. json_builder_hex(builder, raw_bytes, raw_size)
4. free(raw_bytes)
```

This is the same read path as block serving. The block_index_db already has
`entry.data_file` and `entry.data_pos` populated during `node_store_block()`.

**Layer touch:** App (rpc.c) only. Reads from existing storage path.

### 7. getblocktemplate Gaps

The existing `rpc_getblocktemplate` is a functional scaffold but has three
gaps that make it non-conformant with BIP-145 (SegWit extension to BIP-22):

**Gap 1: Witness commitment missing.**
The coinbase transaction must include a second output: `OP_RETURN
<witness_commitment>` where witness commitment = SHA256d(witness_merkle_root
|| coinbase_witness_nonce). This is required for SegWit miners. Without it,
mining pools cannot construct valid SegWit blocks.

The `coinbase_params_t` struct already has `include_witness_commitment` and
`witness_commitment` fields (`include/mining.h:103-107`). The template
builder must compute the witness merkle root from selected transactions,
populate these fields, and include them in the returned JSON under a
`coinbaseaux` or `default_witness_commitment` field.

**Gap 2: `"rules": ["segwit"]` missing.**
BIP-145 requires the template response to include `"rules": ["segwit"]`
to indicate SegWit support. Mining pool software (cgminer, etc.) uses this
field to determine whether to construct segwit coinbase transactions.

**Gap 3: mintime uses wall clock approximation, not real MTP.**
Current code: `mintime = plat_time_ms() / 1000 - 600`. Correct: query MTP
via the new `block_index_db_get_median_time()`, then `mintime = mtp + 1`.

**Layer touch:** App (rpc.c, mining.c) only. Uses existing coinbase_params_t
and mempool APIs.

---

## New Components Required

Only one genuinely new component exists in this milestone. The others are
modifications to existing files.

### New: Transaction Index Functions in block_index_db.c

Two new functions added to the existing `block_index_db_t`:

```c
/* Insert a transaction into the index.
 * Called once per tx during node_apply_block(). */
echo_result_t block_index_db_txindex_insert(
    block_index_db_t *bdb,
    const hash256_t *txid,
    uint32_t file_index,
    uint32_t file_offset,      /* offset to block start in blk*.dat */
    uint32_t tx_offset);       /* offset to tx within block body */

/* Look up a transaction by txid.
 * Called by rpc_getrawtransaction for confirmed txs. */
echo_result_t block_index_db_txindex_lookup(
    block_index_db_t *bdb,
    const hash256_t *txid,
    uint32_t *file_index_out,
    uint32_t *file_offset_out,
    uint32_t *tx_offset_out);

/* Get MTP for block at given height.
 * Queries last 11 headers, returns median timestamp. */
echo_result_t block_index_db_get_median_time(
    block_index_db_t *bdb,
    uint32_t height,
    uint32_t *mtp_out);
```

These extend the existing prepared-statement pattern in `block_index_db_t`.
Add two more `db_stmt_t` fields to the struct for `txindex_insert_stmt` and
`txindex_lookup_stmt`. The MTP query can use an ad-hoc prepared statement or
an inline SQL execution.

---

## Integration Points

### Cross-Layer Boundaries Touched by New Features

| Boundary | What Crosses It | Direction |
|----------|----------------|-----------|
| App → Storage | `node_apply_block()` calls `block_index_db_txindex_insert()` | App → Storage |
| App → Storage | `rpc_getrawtransaction()` calls `block_index_db_txindex_lookup()` | App → Storage |
| App → Storage | `rpc_getblockchaininfo()` calls `block_index_db_get_median_time()` | App → Storage |
| App → Storage | `rpc_getblock(verbosity=0)` calls `block_storage_read()` | App → Storage |
| App → Network | `node.c` getdata handler calls `peer_queue_message()` with block | App → Protocol |
| Protocol → Consensus (NO CROSS) | RBF logic reads `mempool_entry_t.signals_rbf` within mempool.c | None (intra-layer) |

No new consensus-layer calls. No I/O added to the consensus or chaser layers.
The isolation rules from v1.0 hold unchanged.

### Internal Boundary Violations to Avoid

- Do not add txindex writes to `consensus/chainstate.c`. The index is an
  application-layer concern. The correct caller is `node_apply_block()` in
  `node.c`, after `chainstate_apply_block()` succeeds.
- Do not read block storage from within the RPC handler's dispatch thread
  for blocks > ~1 MB without a timeout or size guard. The RPC timeout is
  30 seconds (`RPC_TIMEOUT_MS`), which is generous, but a 4 MB block read
  under I/O pressure could approach it. For v1.1, synchronous reads are
  acceptable; flag for future async treatment.
- Do not add a second database file for txindex. Keep it in the existing
  `block_index.db` to avoid cross-database transaction complexity with the
  UTXO set updates.

---

## Suggested Build Order

Dependencies flow in one direction. Build in this order to avoid circular
testing dependencies and minimize rework.

### Step 1: NODE_WITNESS + INV_WITNESS_BLOCK request (P2P-01, P2P-04)

**Why first:** All P2P block serving testing depends on peers sending
`INV_WITNESS_BLOCK` inventory items. Without NODE_WITNESS advertised, peers
send `INV_BLOCK` (no witness data), and the block serving path for witness
blocks cannot be exercised.

**Changes:**
- `node.c`: Add `SERVICE_NODE_WITNESS` to services in two places (inbound
  accept and outbound connect handshake)
- `node.c` `sync_cb_send_getdata_blocks()`: Use `INV_WITNESS_BLOCK` when
  the peer's services field has `SERVICE_NODE_WITNESS` set

**Files touched:** `src/app/node.c` only.
**Test:** Connect to mainnet, observe peers sending `inv(INV_WITNESS_BLOCK)`.

### Step 2: Block Serving via getdata (P2P-02)

**Why second:** Depends on Step 1 (witness type selection). Block serving
completes the full INV → GETDATA → BLOCK flow that makes Echo a real
network participant.

**Changes:**
- `node.c` getdata handler: Replace `/* TODO: Full block serving */` with
  actual implementation:
  - `block_index_db_lookup_by_hash()` to get file position
  - `block_storage_read()` to load raw bytes
  - For `INV_WITNESS_BLOCK`: send raw bytes directly (already witness format)
  - For `INV_BLOCK`: parse block, re-serialize without witness, send
  - Send `notfound` for unknown or pruned blocks

**Files touched:** `src/app/node.c` only.
**Test:** Request a known block hash from a local echo node with a peer tool
(e.g., `getdata`-capable script). Verify block bytes received match block
stored on disk.

### Step 3: BIP-125 Full-RBF Mempool (P2P-03)

**Why third:** Independent of Steps 1-2. Can technically be built in parallel.
Sequenced here because it has no dependencies and the mempool tests (TEST-06)
can be written and run without network infrastructure.

**Changes:**
- `mempool.c` `mempool_add()`: Replace TODO stub at line 797 with:
  - Collect all conflicting entries and their full descendant sets
  - Apply BIP-125 rules 1-5
  - If pass: evict conflict set, insert replacement
  - If fail: return appropriate rejection code

**Files touched:** `src/protocol/mempool.c` only.
**Test:** Unit tests for all 5 rules including the absolute-fee rule (Rule 3)
and the descendant count limit (Rule 5). TEST-06 in requirements.

### Step 4: Transaction Index (RPC-01)

**Why fourth:** Required before getrawtransaction for confirmed transactions
(RPC-02) can work. Does not depend on Steps 1-3 but is ordered here to group
storage work before RPC work.

**Changes:**
- `block_index_db.c`: Add `tx_index` table creation in `block_index_db_open()`
- `block_index_db.c`: Add `txindex_insert_stmt` and `txindex_lookup_stmt`
  prepared statements to `block_index_db_t` struct
- `block_index_db.c`: Implement `block_index_db_txindex_insert()`
- `block_index_db.c`: Implement `block_index_db_txindex_lookup()`
- `block_index_db.h`: Declare the two new functions
- `node.c` `node_apply_block()`: After `node_store_block()` succeeds, call
  `block_index_db_txindex_insert()` for each tx in block

**Files touched:** `src/storage/block_index_db.c`, `include/block_index_db.h`,
`src/app/node.c`.
**Test:** Apply a known block, query txindex for a tx in that block, verify
file position is correct.

### Step 5: MTP Query (RPC-04 dependency)

**Why fifth:** Single storage function, small, needed by multiple RPC methods.
Build it once here, wire it into all callers.

**Changes:**
- `block_index_db.c`: Implement `block_index_db_get_median_time()`
- `block_index_db.h`: Declare the function

**Files touched:** `src/storage/block_index_db.c`, `include/block_index_db.h`.
**Test:** Call with a known height, verify result matches Bitcoin Core's
reported mediantime for that height.

### Step 6: getblock verbosity=0 (RPC-03)

**Why sixth:** Block serving (Step 2) established the block read path. Wire it
into the RPC handler. Fast to implement after Step 2.

**Changes:**
- `rpc.c` `rpc_getblock()`: Replace empty string return at verbosity=0 with
  `block_storage_read()` + `json_builder_hex()`
- `rpc.c` `rpc_getblock()`: Fix verbosity=1 mediantime field to use MTP
  (from Step 5) instead of block timestamp

**Files touched:** `src/app/rpc.c`.
**Test:** Call `getblock <hash> 0`, compare hex to `bitcoin-cli getblock <hash> 0`.

### Step 7: getblockchaininfo mediantime (RPC-04)

**Why seventh:** Uses the MTP function from Step 5. Simple wire-up.

**Changes:**
- `rpc.c` `rpc_getblockchaininfo()`: Replace `json_builder_uint(builder, 0)`
  at line 1662 with a call to `block_index_db_get_median_time()`

**Files touched:** `src/app/rpc.c`.
**Test:** Call `getblockchaininfo`, compare `mediantime` to `bitcoin-cli`.

### Step 8: getrawtransaction for confirmed transactions (RPC-02)

**Why eighth:** Requires txindex (Step 4) to be populated and queryable.

**Changes:**
- `rpc.c` `rpc_getrawtransaction()`: After mempool miss, call
  `block_index_db_txindex_lookup()`, then `block_storage_read()` to load
  block, parse and find the tx, serialize to hex

**Files touched:** `src/app/rpc.c`.
**Test:** Submit a known txid for a confirmed transaction; verify hex output
matches `bitcoin-cli getrawtransaction`.

### Step 9: getblocktemplate BIP-145 compliance (RPC-05)

**Why last:** Most complex. Depends on: stable mempool (Step 3 for accurate
fee selection), real MTP (Step 5 for correct `mintime`), and witness commitment
construction which requires its own implementation.

**Changes:**
- `rpc.c` `rpc_getblocktemplate()`: Wire `block_index_db_get_median_time()`
  for `mintime` field
- `rpc.c`: Add `"rules": ["segwit"]` field to response
- `rpc.c` + `mining.c`: Implement witness commitment calculation:
  1. Collect wtxids of all selected transactions (coinbase wtxid = all-zeros)
  2. Build witness merkle tree from wtxids
  3. Compute `SHA256d(witness_merkle_root || coinbase_witness_nonce)`
  4. Encode as `OP_RETURN <commitment>` output in coinbase template
  5. Return commitment value in `"default_witness_commitment"` field

**Files touched:** `src/app/rpc.c`, `src/app/mining.c`.
**Test:** Feed template to cgminer or a test harness; verify it accepts the
template and that a block constructed from it would validate.

---

## Architectural Patterns Applied

### Pattern 1: Callback-Based Cross-Layer Communication (unchanged)

All existing callback patterns remain. No new cross-layer callbacks are added
in this milestone. The txindex write happens in `node_apply_block()` which
already has access to both the block and the storage layer — no callback needed.

### Pattern 2: Opaque Struct + Named Functions (existing, extended)

The txindex functions extend `block_index_db_t` following the existing pattern:
add prepared statements to the struct, initialize in `block_index_db_open()`,
finalize in `block_index_db_close()`, expose via named functions only. No
direct SQLite handle exposure to callers.

```c
/* Pattern for txindex — matches existing block_index_db pattern */
typedef struct {
  db_t db;
  db_stmt_t lookup_hash_stmt;
  db_stmt_t lookup_height_stmt;
  db_stmt_t insert_stmt;
  db_stmt_t update_status_stmt;
  db_stmt_t update_data_pos_stmt;
  db_stmt_t best_chain_stmt;
  /* NEW in v1.1: */
  db_stmt_t txindex_insert_stmt;
  db_stmt_t txindex_lookup_stmt;
  bool stmts_prepared;
  pthread_mutex_t mutex;
} block_index_db_t;
```

### Pattern 3: Synchronous I/O in RPC (acceptable for v1.1)

Block reads for RPC are synchronous. The RPC thread is separate from the
main event loop (started in `rpc_server_start()`), so blocking disk reads
do not affect peer processing. The 30-second `RPC_TIMEOUT_MS` is sufficient.
Do not introduce async complexity for RPC reads in this milestone.

---

## Anti-Patterns to Avoid

### Anti-Pattern 1: Advertising NODE_WITNESS Before Block Serving Is Ready

**What people do:** Set the SERVICE_NODE_WITNESS flag at startup regardless of
whether the getdata handler can serve blocks.

**Why it's wrong:** Once NODE_WITNESS is advertised, peers will send
`INV_WITNESS_BLOCK` items expecting block service. If the node sends `notfound`
for every request, peers will disconnect or penalize the node.

**Do this instead:** Implement Steps 1 and 2 together in the same phase.
NODE_WITNESS advertised only after the getdata handler is complete. In practice
this is a single development session — the flag and the handler are both
one-file changes in node.c.

### Anti-Pattern 2: Writing txindex for Pruned Blocks

**What people do:** Write txindex rows for blocks that will later be pruned,
causing stale index entries that point to deleted file positions.

**Why it's wrong:** After pruning, `block_storage_read()` at the indexed
position fails. The RPC would return an I/O error instead of a clean
not-found.

**Do this instead:** On `getrawtransaction` for a confirmed tx, after txindex
lookup, check `block_index_db_is_pruned()` for the block containing the tx.
If pruned, return `RPC_ERR_BLOCK_NOT_FOUND` with message "Block not available
(pruned)". This matches Bitcoin Core's behavior when `txindex=1` and
`prune=1` conflict.

### Anti-Pattern 3: Full RBF Rule 3 as Fee Rate Instead of Absolute Fee

**What people do:** Check that the replacement's fee rate exceeds the
conflicting transaction's fee rate. This passes rate but misses the absolute
fee requirement.

**Why it's wrong:** BIP-125 Rule 3 requires the replacement's absolute fee to
exceed the sum of all replaced transactions' absolute fees (not rates). A
tiny high-rate transaction cannot replace a large low-rate transaction with
the same or lower absolute fee. This is the "RBF pinning" attack vector.

**Do this instead:**
```c
satoshi_t total_replaced_fees = 0;
for (each tx in replacement_set) {
    total_replaced_fees += entry->fee;
}
/* Rule 3: absolute fee check */
if (new_tx_fee <= total_replaced_fees) {
    result->reason = MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE;
    return ECHO_ERR_INVALID;
}
/* Rule 5: must also exceed by at least relay increment */
if (new_tx_fee < total_replaced_fees + MEMPOOL_RBF_INCREMENT) {
    result->reason = MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE;
    return ECHO_ERR_INVALID;
}
```

### Anti-Pattern 4: Putting txindex in a Separate Database File

**What people do:** Create a separate `txindex.db` SQLite file to avoid
modifying the existing block index schema.

**Why it's wrong:** The txindex must be updated atomically with block index
updates — both happen during `node_apply_block()`. With separate files,
a crash between the two writes leaves the index inconsistent.

**Do this instead:** Add `tx_index` as a table in the existing `block_index.db`.
Both tables are in the same WAL-mode SQLite connection. The existing mutex in
`block_index_db_t` protects both tables. Atomicity is guaranteed by SQLite's
transaction boundaries.

### Anti-Pattern 5: Witness Commitment Without Coinbase Witness Nonce

**What people do:** Construct the witness commitment as SHA256d of just the
witness merkle root.

**Why it's wrong:** BIP-141 defines the commitment as:
`SHA256d(witness_root_hash || witness_reserved_value)` where
`witness_reserved_value` is the 32-byte nonce from the coinbase input's
witness field (all-zeros by convention). Omitting the nonce produces a
different hash that fails validation.

**Do this instead:** In the coinbase, include a witness input with a 32-byte
all-zeros witness data item. Compute:
```
witness_commitment = SHA256d(witness_merkle_root || all_zeros_32_bytes)
```
Then encode the commitment as an OP_RETURN output in the coinbase. The
`coinbase_params_t.witness_commitment` field already holds the right shape;
the witness nonce is implicit as all-zeros in the reference implementation.

---

## Build Order Summary

| Step | Feature | Files Changed | Depends On |
|------|---------|---------------|------------|
| 1 | NODE_WITNESS flag + INV_WITNESS_BLOCK requests | node.c | — |
| 2 | Block serving (getdata handler) | node.c | Step 1 |
| 3 | BIP-125 full-RBF (5 rules) | mempool.c | — |
| 4 | Transaction index schema + population | block_index_db.c, block_index_db.h, node.c | — |
| 5 | MTP query function | block_index_db.c, block_index_db.h | — |
| 6 | getblock verbosity=0 + mediantime fix | rpc.c | Steps 2, 5 |
| 7 | getblockchaininfo mediantime | rpc.c | Step 5 |
| 8 | getrawtransaction confirmed | rpc.c | Steps 4, 2 |
| 9 | getblocktemplate BIP-145 | rpc.c, mining.c | Steps 3, 5 |

Steps 1-2 must be sequential (NODE_WITNESS before block serving test).
Steps 3, 4, and 5 are independent of each other and of Steps 1-2 — they
can be developed in parallel if needed.
Steps 6, 7, 8 all depend on Step 5 (MTP). Steps 6 and 8 also depend on
Step 2 (block storage read path established).
Step 9 is last because it touches the most pieces.

---

## Sources

- `include/node.h` — `node_apply_block()`, `node_load_block()`, `node_get_block_index_db()` (HIGH — direct code)
- `include/block_index_db.h` — `block_index_entry_t`, existing prepared statement pattern, pruning functions (HIGH — direct code)
- `include/blocks_storage.h` — `block_storage_read()`, `block_file_pos_t` (HIGH — direct code)
- `include/mempool.h` — `mempool_entry_t.signals_rbf`, `MEMPOOL_RBF_INCREMENT`, `MEMPOOL_MAX_REPLACEMENT_COUNT`, rejection codes (HIGH — direct code)
- `include/protocol.h` — `SERVICE_NODE_WITNESS`, `INV_WITNESS_BLOCK` constants already defined (HIGH — direct code)
- `include/mining.h` — `coinbase_params_t.include_witness_commitment`, `block_template_t` (HIGH — direct code)
- `src/app/node.c` — Current services flag (line 2937), getdata stub (line 2786), TODO comment (line 1779) (HIGH — direct code)
- `src/app/rpc.c` — mediantime=0 stub (line 1662), verbosity=0 stub (lines 1790-92), getrawtransaction TODO (line 1899), getblocktemplate MTP approximation (line 2232) (HIGH — direct code)
- `src/protocol/mempool.c` — RBF TODO stub (line 797), conflict detection code (lines 698-716) (HIGH — direct code)
- BIP-125 — All 5 replacement rules (MEDIUM — training data; stable specification, well-sourced)
- BIP-141 §Commitment structure — Witness commitment = SHA256d(witness_root || witness_reserved_value) (MEDIUM — training data; verify formula against spec)
- BIP-145 — getblocktemplate SegWit extension, `"rules": ["segwit"]` field (MEDIUM — training data; stable specification)

---

*Architecture research for: Bitcoin Echo v1.1 Network Participant milestone*
*Researched: 2026-02-21*
