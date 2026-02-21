# Feature Research

**Domain:** Bitcoin full node — v1.1 Network Participant milestone
**Researched:** 2026-02-21
**Confidence:** HIGH (grounded in BIPs, Bitcoin Core source, official protocol documentation)

---

## Context: What Already Exists

v1.0 shipped with all critical consensus and IBD infrastructure complete. The
features below are the remaining v1.1 work — block serving, RBF mempool, tx
index, and RPC expansion. Each feature description notes which existing
infrastructure it builds on.

Already built and NOT re-researched here:
- NODE_WITNESS service flag — done in v1.0 (P2P-01 complete)
- Full SegWit + Taproot validation — done in v1.0
- Block storage (blk*.dat) with pruning — done in v1.0
- Chain reorganization with full UTXO rollback — done in v1.0
- Mempool structure with basic transaction acceptance — exists, needs RBF rules
- RPC server infrastructure (JSON parser, HTTP, handler dispatch) — exists

---

## Feature Landscape

### Table Stakes (Peers and Operators Expect These)

Features that peers or tool operators assume exist. Missing these causes the
node to be ignored by peers or rejected by standard tooling.

| Feature | Why Expected | Complexity | Depends On |
|---------|-------------|------------|------------|
| **Full block serving via getdata** | Peers that cannot get blocks from Echo deprioritize and eventually drop it. Any peer sending `getdata MSG_WITNESS_BLOCK` expects a serialized block with witness data in response. Not responding = peer evicts you. | HIGH | `node_load_block()` already exists. `relay_handle_getdata()` stub exists. Need: dispatch from getdata handler, serialize witness block, send `MSG_BLOCK`. Pruning check needed — respond `notfound` for pruned heights. |
| **INV_WITNESS_BLOCK in announcements** | After block serving exists, new blocks must be announced with `INV_WITNESS_BLOCK (0x40000002)` to witness-capable peers, not plain `INV_BLOCK (2)`. Using `INV_BLOCK` causes peers to request without witness flag, creating a mismatch. | LOW | Peer capability tracking (does peer have `NODE_WITNESS`?). `relay_announce_block()` must use witness INV type for capable peers. |
| **BIP-125 full-RBF with all 5 rules** | Bitcoin Core made full-RBF the default in v28.0 (late 2024) and removed the `-mempoolfullrbf` toggle entirely. A node without RBF rejects fee-bump transactions that the entire rest of the network accepts. Miners using this node's mempool miss fee revenue. | MEDIUM | `mempool_add()` already detects conflicts and signals `signals_rbf`. Need: RBF replacement logic implementing 5 rules, ancestor/descendant removal on replacement. |
| **RPC getblock (verbosity=0 hex)** | The most common `getblock` call returns the raw serialized block as hex. Used by block explorers, debugging tools, and any consumer that needs to inspect block bytes. Returning an error here breaks all standard tooling. | LOW | `node_load_block()` and `blocks_storage` already exist. Need: RPC handler that reads block from storage, hex-encodes it, returns it. `rpc_getblock()` stub exists in `rpc.h`. |
| **RPC getblockchaininfo mediantime** | Median Time Past is consensus-critical (used by CLTV/CSV locktime validation). Any application checking time-locked transactions that calls `getblockchaininfo` and gets `mediantime: 0` is receiving incorrect data. The field is already in the RPC interface spec but needs to be populated. | LOW | Block index already stores timestamps in headers. Need: query last 11 headers, compute median, populate the field. Existing `rpc_getblockchaininfo()` returns placeholder. |

### Differentiators (Enable Mining Pool Integration and Advanced Tooling)

Features that are not enforced by peers but required for the node to be useful
to miners and advanced operators.

| Feature | Value Proposition | Complexity | Depends On |
|---------|------------------|------------|------------|
| **Transaction index (txindex)** | Without txindex, `getrawtransaction` only works for mempool transactions. Any RPC call for a confirmed transaction fails. Standard expectation for any node with an RPC interface. txindex also unblocks `getblocktemplate` needing to check if candidate transactions are already confirmed. | MEDIUM | Block storage already writes blk*.dat with block file + offset tracked in block index. Need: a separate txindex table (txid → file_num, block_offset, tx_offset_within_block). Write during `node_apply_block()`. Read during `rpc_getrawtransaction()`. |
| **RPC getblocktemplate (BIP-22/23)** | Mining pools cannot use Echo without `getblocktemplate`. This is the only standard path for a node to contribute to block production. A node that can validate and serve blocks but cannot template them is not useful to mining infrastructure. | HIGH | Requires: mempool with RBF (fee market is accurate), txindex (know which txs confirmed), coinbase construction (already in `mining.h`), witness commitment calculation (BIP-141 `hash_wtxid_root`), block weight accounting. The most complex feature in this milestone. |
| **RPC getrawtransaction (confirmed)** | Depends on txindex. Once txindex exists, querying confirmed transactions by hash should work. Without it, the RPC returns "not found" for anything not in mempool. | LOW | txindex (above). The mempool lookup path already works. Need only: on txid lookup miss in mempool, check txindex, load from blk*.dat, deserialize, return. |

### Anti-Features (Deliberately Excluded from This Milestone)

| Feature | Why Requested | Why to Exclude | Alternative |
|---------|--------------|----------------|-------------|
| **BIP-324 v2 encrypted transport** | Bitcoin Core v27+ enables v2 by default. Looks like a compatibility requirement. | v2 falls back to v1 gracefully. Non-v2 nodes receive inbound v1 connections. No peer disconnects for lacking v2. ChaCha20Poly1305 + Elligator Swift is a significant pure-crypto project orthogonal to this milestone. | Dedicated cryptography milestone after block serving is stable. |
| **BIP-152 compact block relay** | Reduces block propagation latency; used by all Bitcoin Core nodes. | Compact blocks are an optimization over full block relay, not a correctness requirement. Nodes without it fall back to full block relay (MSG_BLOCK). Compact blocks also depend on a stable block serving implementation — they must not be built before the full block path is working. | After block serving is stable. |
| **RPC getblock verbosity=1 or 2 (decoded JSON)** | Provides decoded transaction detail in JSON. Useful for block explorers. | Verbosity=0 (hex) is the table-stakes form. Verbosity=1/2 requires serializing all tx fields as JSON — significant work with no peer compatibility impact. | Defer to RPC polish milestone with bitcoinecho-gui integration. |
| **Fee estimation RPC** | Useful for wallets constructing transactions. | No peer compatibility requirement. Needs long mempool history tracking to be accurate. Entirely internal feature. | Defer to RPC polish milestone. |
| **Mempool persistence across restarts** | Avoids re-downloading mempool on restart. | Current restart workflow is `rm -rf ~/.bitcoin-echo`. No persistence infrastructure exists. Complex: need to re-validate all persisted txs against current UTXO set on load. Minimal benefit given current restart requirement. | Defer to operational improvement milestone. |
| **BIP-157/158 compact block filters** | Light wallets query chain without full IBD. | No peer compatibility requirement. Only light clients query for it. Significant scope: filter computation for every block. | Dedicated light client milestone. |
| **submitblock RPC** | Needed by miners to submit mined blocks. | Only relevant once `getblocktemplate` is complete. The mining workflow is: `getblocktemplate` → mine → `submitblock`. Without the template step, submit is useless. Include in same phase as `getblocktemplate`. | Same phase as getblocktemplate (Phase 4). |

---

## Feature Dependencies

```
[Block Serving — getdata handler]
    requires --> [node_load_block()]          (already exists)
    requires --> [blocks_storage read path]   (already exists)
    requires --> [INV_WITNESS_BLOCK announcements]  (must use correct INV type)
    future   --> [BIP-152 Compact Blocks]     (depends on block serving being stable)

[INV_WITNESS_BLOCK announcements]
    requires --> [Block Serving]               (announcing before you can serve = broken)
    requires --> [Per-peer capability tracking](NODE_WITNESS flag in peer state)

[BIP-125 Full-RBF]
    requires --> [Existing mempool conflict detection]   (already exists)
    enhances --> [getblocktemplate]            (accurate fee market in mempool)
    standalone on: [Block Serving]

[Transaction Index (txindex)]
    requires --> [node_apply_block()]          (write during block application)
    requires --> [blk*.dat storage]            (index points into existing files)
    required-by --> [RPC getrawtransaction (confirmed)]
    required-by --> [getblocktemplate]         (check confirmed status of txs)

[RPC getblock verbosity=0]
    requires --> [node_load_block()]           (already exists)
    standalone on: [txindex], [Block Serving]

[RPC getblockchaininfo mediantime]
    requires --> [block_index_db timestamps]   (headers stored with timestamps — already exists)
    standalone: minimal work, no new infrastructure

[RPC getblocktemplate]
    requires --> [BIP-125 RBF]                (mempool fee market accurate)
    requires --> [txindex]                    (know confirmed tx set)
    requires --> [coinbase_create()]           (already in mining.h)
    requires --> [mempool_select_for_block()]  (already in mempool.h)
    requires --> [Block Serving]              (node must be network participant first)
    requires --> [witness commitment hash]     (BIP-141 wtxid merkle root computation)

[RPC submitblock]
    requires --> [getblocktemplate]            (only useful after template step)
    requires --> [node_process_received_block()] (already exists)
```

### Dependency Notes

- **Block serving is the unlock for everything else.** The node cannot be a
  genuine network participant, cannot relay post-IBD blocks, and cannot test
  the complete P2P loop until it can respond to `getdata` requests. All other
  features are safe to develop in parallel but this one is the gating item.

- **INV_WITNESS_BLOCK and block serving must ship together.** Announcing a
  block you cannot serve is a protocol violation. The peer sends `getdata
  MSG_WITNESS_BLOCK` and gets nothing back, then penalizes Echo for it.

- **txindex is a write-time concern.** It must be added to `node_apply_block()`
  before it can be queried. Starting txindex mid-IBD requires either re-indexing
  existing blocks or only indexing from the start height. Simplest approach: add
  write during apply, re-index from genesis on first startup with txindex enabled.
  For this codebase, the correct approach is to add it from the start (always on).

- **getblocktemplate is the most complex feature.** It requires all other
  features to be complete and stable first. It touches mempool selection
  (`mempool_select_for_block()`), coinbase construction (`coinbase_create()`),
  witness commitment computation, block weight accounting, and BIP-22/23 JSON
  format. Build last.

- **BIP-125 full-RBF is cleanly isolated.** It modifies only `mempool_add()`
  and the conflict resolution path. It does not depend on block serving or
  txindex. Can be implemented in parallel with the P2P work.

---

## Expected Behavior Per Feature

### Block Serving (getdata handler)

A peer sends `getdata [MSG_WITNESS_BLOCK, <blockhash>]`. The node must:

1. Look up `<blockhash>` in the block index database.
2. If not found: respond with `notfound [INV_WITNESS_BLOCK, <blockhash>]`.
3. If found but pruned: respond with `notfound` (cannot serve pruned blocks).
4. If found and available: load block from `blk*.dat`, serialize with witness
   encoding, send `MSG_BLOCK` message to requesting peer.
5. Rate-limit: if peer sends excessive getdata (> `MAX_GETDATA_PER_SECOND`),
   apply ban score per `relay.h` constants.

The existing `relay_handle_getdata()` function has this call shape defined. The
implementation gap is: retrieving the actual block bytes and sending them.

### INV_WITNESS_BLOCK Announcements

When `relay_announce_block()` is called after a new block is validated, the
inventory type used depends on the receiving peer's capability:

- Peer has `SERVICE_NODE_WITNESS`: announce `INV_WITNESS_BLOCK (0x40000002)`.
- Peer lacks `SERVICE_NODE_WITNESS`: announce `INV_BLOCK (2)`.

This requires `peer_t` to carry the `services` flags from the version message
handshake. If the handshake already stores this, the change is in the announce
path only.

### BIP-125 Full-RBF (5 Rules)

When `mempool_add()` finds a conflicting transaction (one that spends the same
input), it applies these checks before replacing:

1. **Signaling** (full-RBF: skip this check — replace regardless of signal).
   With opt-in RBF: the conflicting tx must have `nSequence < 0xFFFFFFFE`.
   Echo implements full-RBF per Bitcoin Core v28+ behavior — no signaling check.

2. **No new unconfirmed inputs**: The replacement transaction may not introduce
   unconfirmed inputs that were not already in the conflicting transactions.
   Prevents using replacements to chain new unconfirmed dependencies.

3. **Absolute fee**: `replacement_fee >= sum(fees of all conflicting txs)`.
   The replacement must pay at least as much in total as everything it replaces.

4. **Incremental relay fee**: `replacement_fee - conflicting_fees >=
   replacement_vsize * MEMPOOL_RBF_INCREMENT / 1000`. The replacement must
   pay for its own bandwidth at the minimum relay rate.

5. **Eviction limit**: Total number of transactions to be evicted (conflicting
   tx + all their descendants) must not exceed `MEMPOOL_MAX_REPLACEMENT_COUNT`
   (100). Prevents a single replacement from evicting a large portion of the
   mempool.

If all 5 rules pass: remove conflicting transactions and their descendants,
insert the replacement. If any rule fails: reject with the appropriate
`MEMPOOL_REJECT_RBF_*` code (these codes already exist in `mempool.h`).

The data structures for this (`signals_rbf`, `MEMPOOL_MAX_REPLACEMENT_COUNT`,
`MEMPOOL_RBF_INCREMENT`, `conflicts_count`, `first_conflict` in
`mempool_accept_result_t`) are already defined in `mempool.h`. The
implementation gap is the replacement logic itself.

### Transaction Index (txindex)

The index maps `txid (32 bytes)` → `(file_num: int32, block_byte_offset:
uint32, tx_byte_offset_within_block: uint32)`. This matches Bitcoin Core's
`CDiskTxPos` structure.

Write path: During `node_apply_block()`, after the block is stored to disk and
the block index entry records `data_file` and `data_pos`, iterate all
transactions in the block and write a txindex entry for each. The tx offset
within the block is computed during serialization.

Read path: Given a txid:
1. Query txindex for `(file_num, block_offset, tx_offset)`.
2. Open the appropriate `blk*.dat` file.
3. Seek to `block_offset + tx_offset`.
4. Deserialize the transaction.
5. Return to caller.

Storage: A new SQLite table in the block index database (same file, separate
table) is the natural fit given the existing `block_index_db.h` infrastructure.
No new database files are needed.

### RPC getblock verbosity=0

Parameters: `getblock <blockhash> [verbosity=0]`.

Implementation:
1. Parse `blockhash` from hex (already have `rpc_parse_hash()`).
2. Call `node_load_block()` to get the block.
3. Serialize the block to bytes (with witness encoding — `protocol_serialize.h`).
4. Hex-encode and return as a JSON string result.

Edge cases: block not found returns `RPC_ERR_BLOCK_NOT_FOUND (-5)`. Block
pruned returns the same error (data unavailable). The `rpc_getblock()` stub
already exists in `rpc.h`.

### RPC getblockchaininfo mediantime

Median Time Past is the median of the timestamps of the last 11 blocks. It is
a consensus parameter (used by CLTV/CSV). The field is already listed in the
`rpc_getblockchaininfo()` docstring in `rpc.h` but returns 0.

Implementation:
1. Get current tip height from chainstate.
2. Query the last 11 block headers from `block_index_db` (by height, in
   descending order from tip).
3. Extract `header.timestamp` from each.
4. Sort the 11 timestamps, return the median (6th value, index 5).
5. Populate the `mediantime` field in the JSON response.

This is purely a query against existing data. No new storage needed.

### RPC getblocktemplate (BIP-22/BIP-23)

Parameters: `getblocktemplate {"rules": ["segwit"]}`. Must declare `segwit`
support to receive a SegWit-compatible template.

Response fields required for pool integration:

| Field | Source | Notes |
|-------|--------|-------|
| `version` | `CURRENT_BLOCK_VERSION` (4) | Integer |
| `previousblockhash` | Tip block hash, reversed hex | Standard block explorer format |
| `transactions` | `mempool_select_for_block()` | Array of {data, txid, fee, weight, sigops} |
| `coinbaseaux` | `{"flags": ""}` | Typically empty flags field |
| `coinbasevalue` | Block subsidy + total mempool fees | In satoshis |
| `target` | From `bits` field, expanded to 256-bit hex | For miner's hash comparison |
| `mintime` | Median Time Past + 1 | Minimum valid timestamp for new block |
| `mutable` | `["time", "transactions", "prevblock"]` | Miner can adjust these |
| `noncerange` | `"00000000ffffffff"` | Full 32-bit nonce space |
| `sigoplimit` | `CONSENSUS_MAX_BLOCK_SIGOPS` | Per consensus rules |
| `sizelimit` | `1000000` | Legacy size limit |
| `weightlimit` | `4000000` | SegWit weight limit |
| `curtime` | Current Unix timestamp | Node's current time |
| `bits` | Difficulty target, 4-byte hex | Next block's bits field |
| `height` | Tip height + 1 | Next block height |

The coinbase transaction is NOT included in `transactions` — the pool
constructs it using `coinbasevalue` and `coinbaseaux`. The witness commitment
(`OP_RETURN` in coinbase output 1) is computed from the `wtxid` merkle root of
all selected transactions (BIP-141). This is the most complex field: it
requires computing `hash_wtxid_root` from selected transaction wtxids.

`submitblock` accepts the fully-assembled hex block from the miner, calls
`node_process_received_block()`, and returns null on success.

---

## MVP Definition

### Peer Compatible (This Milestone — Core Features)

The node cannot be a genuine network participant without all of these.

- [ ] **Full block serving (getdata handler)** — Responding to peer block
  requests. Without this, Echo is a leech. The relay_handle_getdata() dispatch
  stub exists; the implementation of actually loading and sending the block
  does not.

- [ ] **INV_WITNESS_BLOCK announcements** — Using the correct INV type when
  announcing new blocks to witness-capable peers. Low effort, correct behavior.

- [ ] **BIP-125 full-RBF (5 rules)** — Implementing replacement logic in
  mempool_add(). The data structures exist; the replacement algorithm does not.

### Operator Useful (This Milestone — After Core)

Makes the node usable for tooling and operators.

- [ ] **Transaction index (txindex)** — Write during apply, read during RPC.
  Unlocks confirmed tx queries.

- [ ] **RPC getblock verbosity=0** — Serialize stored block to hex. Low effort,
  high tooling impact.

- [ ] **RPC getblockchaininfo mediantime** — Query 11 headers, compute median.
  Correctness fix for time-lock applications.

- [ ] **RPC getrawtransaction (confirmed)** — Depends on txindex. Once txindex
  exists, this is a query path addition.

### Mining Integration (End of This Milestone)

Only after mempool and block serving are stable and tested.

- [ ] **RPC getblocktemplate (BIP-22/23)** — Full template construction.
  Highest complexity feature. Depends on RBF, txindex, and block serving all
  being stable first.

- [ ] **RPC submitblock** — Submit mined block to the network. Ships alongside
  getblocktemplate since they form a single mining workflow.

---

## Feature Prioritization Matrix

| Feature | User Value | Implementation Cost | Priority |
|---------|-----------|---------------------|----------|
| Block serving (getdata handler) | HIGH — peer compatibility | MEDIUM | P1 |
| INV_WITNESS_BLOCK announcements | HIGH — protocol correctness | LOW | P1 |
| BIP-125 full-RBF (5 rules) | HIGH — mempool diverges without it | MEDIUM | P1 |
| RPC getblock verbosity=0 | HIGH — standard tooling expects it | LOW | P2 |
| RPC getblockchaininfo mediantime | MEDIUM — correctness for time-locks | LOW | P2 |
| Transaction index (txindex) | HIGH — RPC usability for confirmed txs | MEDIUM | P2 |
| RPC getrawtransaction (confirmed) | HIGH — standard RPC expectation | LOW (once txindex exists) | P2 |
| RPC getblocktemplate | HIGH — mining integration | HIGH | P3 |
| RPC submitblock | HIGH — mining workflow completion | LOW (once getblocktemplate exists) | P3 |

**Priority key:**
- P1: Must have — node is a bad P2P citizen without these
- P2: Should have this milestone — node is degraded for operators without these
- P3: Important capability — blocked on P1/P2 being complete and stable

---

## Implementation Notes Per Feature

### Block Serving: The Critical Path

The existing `relay_handle_getdata()` in `relay.h` has a callback interface:
`relay_callbacks_t.get_block()`. The implementation gap is that this callback
needs to be wired to `node_load_block()`. The serialization path must use the
witness-aware serializer (not just the block header). The INV type in the
response is `MSG_BLOCK` regardless of how the peer requested it — the witness
encoding is determined by the request type (`MSG_WITNESS_BLOCK` → include
witness data in serialization).

Pruning interaction: check `node_is_block_pruned()` before attempting to load.
If pruned, respond with `notfound`. Document that pruned nodes cannot fully
serve blocks — `SERVICE_NODE_NETWORK_LIMITED` service flag should be set if
pruning is enabled (BIP-159).

### BIP-125 RBF: Ancestor Tracking Required

Rules 2 and 4 require knowing the ancestor set of the replacement transaction.
`mempool.h` already tracks `ancestor_count`, `ancestor_fees`, and
`ancestor_size` per entry. The replacement check requires computing these for
the *incoming* transaction before adding it, then comparing against the
conflicting transaction's values. The `mempool_select_for_block()` function
already iterates by fee rate — the same ancestor-ordering logic applies to
replacement evaluation.

### txindex: Schema Addition

Add a new table to the block index SQLite database:

```sql
CREATE TABLE txindex (
    txid    BLOB PRIMARY KEY,   -- 32 bytes, transaction hash
    file    INTEGER NOT NULL,   -- blk*.dat file number
    bpos    INTEGER NOT NULL,   -- byte offset of block in file
    txpos   INTEGER NOT NULL    -- byte offset of tx from block start
);
CREATE INDEX IF NOT EXISTS idx_txindex_txid ON txindex(txid);
```

This lives in the same database file as `block_index_db.h` (different table).
The write happens in `node_apply_block()` after `block_index_db_update_data_pos()`
confirms the file/offset. The read happens in `rpc_getrawtransaction()`.

Coinbase transactions are also indexed (they are confirmed transactions). Their
txid uniqueness is guaranteed by BIP-34 height encoding.

### getblocktemplate: Witness Commitment

The witness commitment is the most subtle part of `getblocktemplate`. Per BIP-141:

```
witness_commitment = SHA256d(witness_root_hash || coinbase_witness_reserved_value)
```

Where:
- `witness_root_hash` = wtxid merkle root of all transactions (coinbase
  wtxid = all zeros for this computation).
- `coinbase_witness_reserved_value` = 32 zero bytes (the standard reserved value).

The commitment is stored in the coinbase transaction as an `OP_RETURN` output:
`OP_RETURN <commitment_header_4_bytes> <commitment_32_bytes>`. The header is
`0xaa21a9ed` (BIP-141 magic).

Since the pool constructs the coinbase (not Echo), `getblocktemplate` must
provide the `default_witness_commitment` field so the pool can include it. The
commitment is computed from the selected transaction wtxids — so it must be
recomputed each time the transaction set changes (i.e., each template request).

The `coinbase_params_t.witness_commitment` field in `mining.h` already has the
right type. The gap is computing the actual wtxid merkle root from selected
mempool entries.

---

## Sources

- [BIP-125: Opt-in Full Replace-by-Fee — bips.dev](https://bips.dev/125/) —
  The 5 replacement rules, signaling via nSequence (HIGH confidence, final BIP)
- [Bitcoin Core mempool-replacements.md — GitHub](https://github.com/bitcoin/bitcoin/blob/0de63b8b46eff5cda85b4950062703324ba65a80/doc/policy/mempool-replacements.md) —
  Current Bitcoin Core policy (rules 1 "signaling" removed in full-RBF,
  feerate diagram improvement added) (HIGH confidence, Bitcoin Core source)
- [Bitcoin Core PR #30493: Enable full-RBF by default](https://github.com/bitcoin/bitcoin/pull/30493) —
  Full-RBF default confirmed for Bitcoin Core v28 (HIGH confidence, merged PR)
- [BIP-22: getblocktemplate — bips.dev](https://bips.dev/22/) —
  Template fields, pool integration protocol (HIGH confidence, final BIP)
- [BIP-23: getblocktemplate — Pooled Mining — bips.dev](https://bips.dev/23/) —
  `capabilities` negotiation (HIGH confidence, final BIP)
- [BIP-141: Segregated Witness — bips.dev](https://bips.dev/141/) —
  Witness commitment construction, coinbase output format (HIGH confidence, final BIP)
- [Bitcoin Core TxIndex source — GitHub](https://github.com/bitcoin/bitcoin/blob/master/src/index/txindex.cpp) —
  CDiskTxPos structure: file_num, block_offset, tx_offset (HIGH confidence, Bitcoin Core source)
- [Bitcoin P2P Protocol: MSG_WITNESS_BLOCK — developer.bitcoin.org](https://developer.bitcoin.org/reference/p2p_networking.html) —
  INV type 0x40000002, SERVICE_NODE_WITNESS flag value 8 (MEDIUM confidence, official)
- [Bitcoin Core getblockchaininfo RPC — bitcoincore.org](https://bitcoincore.org/en/doc/25.0.0/rpc/blockchain/getblockchaininfo/) —
  mediantime field is median UNIX epoch of last 11 block timestamps (HIGH confidence, official)

---

*Feature research for: Bitcoin full node — v1.1 Network Participant*
*Researched: 2026-02-21*
