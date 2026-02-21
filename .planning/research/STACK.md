# Stack Research

**Domain:** Bitcoin full node — P2P block serving, BIP-125 full-RBF mempool, transaction index, RPC expansion
**Researched:** 2026-02-21
**Confidence:** HIGH (all claims verified against BIPs, Bitcoin Core source, or codebase audit)

---

## Context: No New Libraries

Bitcoin Echo's stack is frozen: pure C11 + vendored SQLite + vendored libsecp256k1. The project manifesto explicitly prohibits external dependencies. Every "technology" in this document is "which BIP governs this feature, how the existing modules connect, and what must NOT be done wrong." There are no `npm install` commands here.

This document supersedes the v1.0 STACK.md. It focuses exclusively on what v1.1 adds: P2P block serving, BIP-125 full-RBF mempool policy, transaction index, and RPC expansion.

---

## Recommended Stack

### Core Technologies

| Technology | Version | Purpose | Why Recommended |
|------------|---------|---------|-----------------|
| Pure C11 | ISO/IEC 9899:2011 | All implementation | Frozen project requirement. No change. |
| SQLite (vendored) | Amalgamation in-tree | Block index, UTXO, new tx_index table | Add `tx_index` table to existing block_index_db. Zero new dependencies — the storage engine is already there, already WAL-mode, already mutex-protected. |
| libsecp256k1 (vendored) | bitcoin-core/secp256k1 | ECDSA + Schnorr | No new use in v1.1. Already complete for all signature verification paths. |
| POSIX sockets/pthreads | System | TCP networking, threading | Already in use. The `relay.c` getdata handler already has rate limiting; just needs block-serving wired in. |

### Protocol Specifications — What v1.1 Implements

| BIP | Feature | Current State in Codebase | What v1.1 Must Do |
|-----|---------|--------------------------|-------------------|
| BIP-144 | NODE_WITNESS service flag, INV_WITNESS_BLOCK | `node.c:1779` has TODO: "Add NODE_WITNESS to services and use INV_WITNESS_BLOCK for SegWit". `node.c:2937-2962` sends `services=1` (NODE_NETWORK only) unconditionally. | Change `services` in `peer_send_version()` calls to include `SERVICE_NODE_WITNESS (1 << 3)`. Gate on whether node can actually serve witness blocks (IBD complete). |
| BIP-144 | Block serving via getdata handler | `node.c:2786` has `/* TODO: Full block serving */`. Prune-check and notfound path already written. | Read block via `block_storage_read()` using `data_file`/`data_pos` from `block_index_db`. Serialize to wire via `protocol_serialize.h`. Send as `MSG_BLOCK` or `MSG_WITNESS_BLOCK`. |
| BIP-125 | Opt-in RBF replacement (all 5 rules) | `mempool.c:797-807` has `/* TODO: Implement full RBF replacement logic */` and immediately rejects conflicts. RBF signaling detection (`signals_rbf`) already works. | Implement all 5 rules in `mempool_add()` when `has_conflict == true` and conflicting entry `signals_rbf`. See implementation approach below. |
| BIP-22 / BIP-145 | `getblocktemplate` completion | `rpc.c:2111-2421` has partial implementation. Transaction selection works. MTP returns 0 (`TODO`). Witness commitment not constructed. `getblockchaininfo` mediantime also returns 0. | Implement MTP query (11-block median). Compute witness merkle root (wtxids). Construct `default_witness_commitment`. Return `"rules": ["segwit"]`. |
| None (node capability) | Transaction index | `rpc.c:1897` has `/* TODO: Implement transaction index for confirmed txs */`. No `tx_index` table exists yet. | Add `tx_index` SQLite table. Populate on block confirmation. Wire into `rpc_getrawtransaction()` and `rpc_getblock()` verbosity=0. |
| BIP-144 | `getblock` verbosity=0 raw hex | `rpc.c:1783` has `/* TODO: Read block from storage and return hex */` | Read block from blk*.dat, serialize to hex string in JSON response. |

---

## Supporting Approach by Feature

### Feature 1: NODE_WITNESS + INV_WITNESS_BLOCK (P2P-01, P2P-04)

**Integration point:** `src/app/node.c` — three call sites at lines 2937, 2961, 3329-3330 all pass `services=1`.

**Correct value:**
```c
#define SERVICE_NODE_WITNESS (1 << 3)  /* Already defined in protocol.h */

/* When node can serve full blocks (IBD complete, not pruned): */
uint64_t services = SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS;

/* When pruning enabled (can only serve recent blocks): */
uint64_t services = SERVICE_NODE_NETWORK_LIMITED | SERVICE_NODE_WITNESS;
```

**Gate condition:** Only advertise `NODE_WITNESS` after IBD is complete and the node can actually respond to `INV_WITNESS_BLOCK` getdata with a real block. Advertising during IBD is a protocol violation — peers will immediately send getdata requests the node cannot fulfill.

**Consequence of enabling:** Modern peers (protocol version >= 70013) will send `INV_WITNESS_BLOCK (0x40000002)` instead of `INV_BLOCK (0x2)` in getdata. The `node.c:2763` handler already handles both types with `if (inv->type == INV_BLOCK || inv->type == INV_WITNESS_BLOCK)` — but the block serving body is a TODO.

**What NOT to do:** Do not send `INV_WITNESS_BLOCK` in the download manager's own getdata calls (`node.c:1779`) until block serving is complete. The two directions are independent: serving blocks to peers requires `NODE_WITNESS` in our version; requesting witness blocks from peers requires checking `peer->services & SERVICE_NODE_WITNESS`.

**Source:** [BIP-144](https://github.com/bitcoin/bips/blob/master/bip-0144.mediawiki) — HIGH confidence

---

### Feature 2: Full Block Serving via getdata (P2P-02)

**Integration point:** `src/app/node.c:2763-2786` — the `MSG_GETDATA` handler. Structure already exists, block fetch and send is the `/* TODO */`.

**Required path:**
1. Look up `block_index_entry_t` by hash via `block_index_db_lookup_by_hash()`.
2. Check `entry.status & BLOCK_STATUS_HAVE_DATA`. If not set, send `notfound`.
3. Check `entry.status & BLOCK_STATUS_PRUNED`. If pruned, send `notfound` (already implemented for this case).
4. Read block from disk: `block_storage_read(entry.data_file, entry.data_pos, &block)`.
5. Serialize block to wire format via `protocol_serialize.h` → `msg_serialize_block()`.
6. Send as `MSG_BLOCK` message.

**Witness serialization:** The `block` struct from `block_storage_read()` already contains witness data if the block was stored post-SegWit activation. When peer requested `INV_WITNESS_BLOCK`, include witness in serialization (which is already the default in `tx_serialize()` when `tx->has_witness`).

**Memory:** A SegWit block can be up to 4MB (weight units). Do not allocate block data on the stack. Use heap allocation, serialize directly to a send buffer, then free.

**Rate limiting:** `relay.c` already enforces `MAX_GETDATA_PER_SECOND = 1000` per peer. Do not replicate this in node.c — trust the relay layer.

**Send queue concern:** `peer.h` defines `PEER_SEND_QUEUE_SIZE = 128` messages. Serving large blocks to many peers simultaneously can exhaust the send queue. Limit concurrent block serves per peer. If send queue is full, return `ECHO_ERR_FULL` and send `notfound` — do not block.

**Source:** [developer.bitcoin.org P2P reference](https://developer.bitcoin.org/reference/p2p_networking.html), codebase audit — HIGH confidence

---

### Feature 3: BIP-125 Full-RBF Mempool Policy (P2P-03)

**Clarification on "full-RBF" in milestone context:** The milestone says "BIP-125 full-RBF" — this means implementing all 5 replacement rules from BIP-125, not Bitcoin Core's `-mempoolfullrbf` flag (which allows replacing non-signaling transactions). Echo implements BIP-125 opt-in RBF only: replacement requires the conflicting transaction to signal via `nSequence < 0xFFFFFFFE`.

**Integration point:** `src/protocol/mempool.c:797-807` — the `/* TODO: Implement full RBF replacement logic */` block inside `mempool_add()`.

**The 5 rules (BIP-125):**

**Rule 1 — Signaling:** Conflicting transaction must have `signals_rbf == true`. Already checked at `mempool.c:699`. No additional work.

**Rule 2 — No new unconfirmed inputs:** The replacement transaction's inputs must not reference any unconfirmed transaction outputs that are not already inputs of the transaction(s) being replaced. Implementation: for each input of the new transaction, check if it comes from a mempool entry. If yes, verify that mempool entry is also one of the conflicting transactions. If it introduces a new mempool dependency not present in the conflicting set, reject.

**Rule 3 — Absolute fee:** Sum of replacement fees >= sum of all evicted transaction fees (conflicting tx + all its descendants). Implementation: walk the conflict set using the existing descendant tracking. For each conflicting tx, call `mempool_remove_entry()` would handle descendants — but first collect their total fees before removal.

**Rule 4 — Bandwidth fee:** Replacement fee >= (replacement vsize × min_relay_fee_rate). The `vsize` is already computed as `tx_vsize(tx)`. `min_relay_fee_rate` is available as `MEMPOOL_DEFAULT_MIN_FEE_RATE` or `mp->config.min_fee_rate`.

**Rule 5 — Descendant limit:** Total transactions to evict (conflicting + all their descendants) must not exceed `MEMPOOL_MAX_REPLACEMENT_COUNT = 100`. Already defined in `mempool.h`. Implementation: walk the `next` pointers in the `spent_lookup` table to collect all descendants.

**Implementation order:** 1 (free, already done) → 5 (count descendants: cheap walk) → 4 (arithmetic: cheapest validation) → 2 (unconfirmed input check: lookup per input) → 3 (fee sum: requires knowing all evicted entries) → atomic evict-and-insert.

**Atomicity:** Collect all entries to evict first, verify all rules pass, then call `mempool_remove_entry()` for each, then insert the replacement. Never partially apply.

**What NOT to do:** Do not implement `-mempoolfullrbf` (replacing non-signaling transactions). That is a separate Bitcoin Core configuration flag added in v24.0 with ongoing controversy. BIP-125 requires the signal. Implementing full-RBF would break compatibility with wallets relying on first-seen guarantees.

**Source:** [BIP-125](https://github.com/bitcoin/bips/blob/master/bip-0125.mediawiki) — HIGH confidence

---

### Feature 4: Transaction Index (RPC-01)

**Integration point:** New SQLite table in `src/storage/block_index_db.c`. Populated from the block confirmation path in `src/node/chaser_confirm.c`.

**Schema — add to existing block_index_db:**
```sql
CREATE TABLE IF NOT EXISTS tx_index (
    txid        BLOB NOT NULL PRIMARY KEY,  -- 32 bytes, txid (internal byte order)
    file_index  INTEGER NOT NULL,           -- blk*.dat file number
    file_offset INTEGER NOT NULL,           -- byte offset of block in file
    tx_offset   INTEGER NOT NULL            -- byte offset of tx within block data
);
```

**Why tx_offset:** A block can contain thousands of transactions. Rather than deserializing the whole block and scanning by txid, store the byte offset of the transaction within the block's serialized form. `getrawtransaction` can then: read the block header + seek to `tx_offset` and deserialize just that transaction's bytes. This matches Bitcoin Core's `CDiskTxPos` approach (file_index + file_offset + tx_pos_in_block).

**Population timing:** Write tx_index entries inside the block confirmation path, after `utxo_db_apply_block()` succeeds but before the SQLite transaction commits. This ensures atomicity: if the transaction commits, both UTXO state and txindex are updated together; if it rolls back, neither is updated.

**Lookup in getrawtransaction:** Query `tx_index` by txid → get `(file_index, file_offset, tx_offset)` → call `block_storage_read_tx(file_index, file_offset, tx_offset, &tx)` → serialize to hex for RPC response.

**Pruning interaction:** If a block is pruned (`BLOCK_STATUS_PRUNED` set), the tx_index entry for its transactions points to a deleted blk*.dat file. Bitcoin Core behavior: `getrawtransaction` returns "No such mempool or blockchain transaction" for pruned transactions even if the txindex entry exists. Echo should do the same: check `BLOCK_STATUS_PRUNED` before serving.

**What NOT to do:** Do not create a separate SQLite database file for the tx_index. Adding a table to the existing `block_index.db` is simpler and keeps the transaction scope the same for the confirmation path. Do not use a flat-file hash map — SQLite with a `PRIMARY KEY` index is fast enough and already present.

**Source:** [Bitcoin Core txindex.cpp (CDiskTxPos)](https://github.com/bitcoin/bitcoin/blob/master/src/index/txindex.cpp) — HIGH confidence

---

### Feature 5: RPC getblock verbosity=0 (RPC-03)

**Integration point:** `src/app/rpc.c:1781-1785` — the `if (verbosity == 0)` branch has a TODO.

**What to do:**
1. Look up `block_index_entry_t` as already done above the branch.
2. Call `block_storage_read()` with `entry.data_file`, `entry.data_pos`.
3. Serialize block to bytes via the protocol serialization path.
4. Hex-encode bytes into the JSON builder with `json_builder_hex()`.

**Buffer size:** Max block size is 4MB (1MB base + witness scaling). Allocate on heap, not stack. Use `RPC_MAX_RESPONSE_SIZE = 16MB` as the ceiling — a hex-encoded 4MB block is 8MB, well within limit.

**mediantime field:** Also in `getblock` verbosity=1 response at `rpc.c:1814` — currently returns `index->timestamp` (incorrect, just the block time). Must be replaced with MTP calculation (see Feature 6 below).

---

### Feature 6: Median Time Past (MTP) for mediantime and getblocktemplate (RPC-04)

**What MTP is:** The median of the last 11 block timestamps on the best chain (not the 11 most recent block times — the 11 blocks preceding the current block). Used as `mintime` in `getblocktemplate` (miners cannot produce a block with `nTime <= MTP`) and returned as `mediantime` in `getblockchaininfo` and `getblock`.

**Integration point:** `src/app/rpc.c:1661-1662` (`getblockchaininfo`) and `rpc.c:1814-1815` (`getblock`) both have `/* TODO: implement MTP query */`.

**Implementation:**
```c
/* Collect timestamps of last min(11, height+1) blocks */
uint32_t timestamps[11];
size_t count = 0;
for (uint32_t h = height; h > 0 && count < 11; h--, count++) {
    block_index_entry_t entry;
    block_index_db_get_chain_block(bdb, h, &entry);
    timestamps[count] = entry.header.timestamp;
}
/* Sort and return median */
qsort(timestamps, count, sizeof(uint32_t), cmp_uint32);
uint32_t mtp = timestamps[count / 2];
```

**Where to put it:** Add `block_index_db_get_mtp(block_index_db_t *bdb, uint32_t height, uint32_t *mtp_out)` to `block_index_db.h`/`.c`. Call from both RPC handlers and from `getblocktemplate`. The query is 11 SQLite lookups — cheap.

**getblocktemplate `mintime`:** Must be `MTP + 1`. This is a consensus rule enforced by Bitcoin Core — a block with `nTime <= MTP` is invalid. The `block_template_t` struct in `mining.h` already has a `mintime` field.

**Source:** [BIP-22 getblocktemplate](https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki), [BIP-145](https://bips.dev/145/), Bitcoin Core `GetMedianTimePast()` — HIGH confidence

---

### Feature 7: getblocktemplate Witness Commitment (RPC-05)

**What's missing:** `getblocktemplate` currently selects transactions from the mempool (working) but does not compute the witness commitment (required for the coinbase OP_RETURN output) or return `"default_witness_commitment"` in the JSON.

**Witness merkle root computation:**
1. Assign coinbase a wtxid of `0x00...00` (32 zero bytes) — defined by BIP-141.
2. For each non-coinbase transaction, use its wtxid (`mempool_entry_t.wtxid`).
3. Build a merkle tree over these wtxids using the same double-SHA256 algorithm as `merkle_compute_root()`.
4. The result is the witness merkle root.

**Witness commitment:**
```c
/* coinbase_witness_nonce is 32 zero bytes (default) */
uint8_t nonce[32] = {0};
/* witness_commitment = SHA256d(witness_merkle_root || nonce) */
uint8_t commitment_preimage[64];
memcpy(commitment_preimage, witness_merkle_root.bytes, 32);
memcpy(commitment_preimage + 32, nonce, 32);
sha256d(commitment_preimage, 64, commitment.bytes);
```

**Coinbase OP_RETURN output:** The coinbase transaction (constructed by the miner) must include:
```
OP_RETURN OP_PUSH36 0xaa21a9ed <32-byte commitment>
```
The header `0xaa21a9ed` is the witness commitment magic (BIP-141). `coinbase_params_t` already has `include_witness_commitment` and `witness_commitment` fields — wire them.

**JSON response field:** Return as `"default_witness_commitment": "<hex>"` in the getblocktemplate JSON. Also return `"rules": ["segwit"]` to declare SegWit capability.

**Source:** [BIP-141](https://github.com/bitcoin/bips/blob/master/bip-0141.mediawiki#commitment-structure), [BIP-145](https://bips.dev/145/) — HIGH confidence

---

## Alternatives Considered

| Recommended | Alternative | Why Not |
|-------------|-------------|---------|
| SQLite `tx_index` table in existing block_index.db | Separate `txindex.db` file | Two databases require coordinated transactions across files. SQLite cannot atomically commit across separate database files without WAL tricks. Single database keeps confirmation atomicity clean. |
| tx_offset within block for tx lookup | Re-scan block by txid on every getrawtransaction | Scanning a 1MB+ block for one transaction is wasteful, especially post-SegWit where blocks can be 4MB. Offset lookup is O(1) disk seek. |
| Opt-in RBF only (BIP-125 signal required) | Full-RBF (allow replacing non-signaling txs) | Full-RBF (`-mempoolfullrbf`) is a Bitcoin Core v24.0+ option with ongoing ecosystem debate. BIP-125 opt-in is the safe, compatible baseline. The milestone says "BIP-125 full-RBF" meaning all 5 rules, not unrestricted replacement. |
| MTP via 11-block SQLite lookups | Caching MTP in node state | Cache adds complexity and invalidation risk. 11 lookups is negligible overhead. Add cache later if profiling shows it matters. |
| Read block from blk*.dat for getblock verbosity=0 | Re-serialize block from memory | Echo's in-memory chainstate does not keep full block data; it only keeps the index. The blk*.dat files are the source of truth. |

---

## What NOT to Use

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| Any new external library | Violates project manifesto | Pure C11 + existing vendored libs |
| BIP-152 compact blocks | Optional protocol feature requiring significant extra state (mempool coordination, short ID mapping). Not required for peer compatibility. Out of scope per PROJECT.md. | Standard `getdata` → `block` message flow |
| `secp256k1_schnorrsig_verify` batch API | Does not exist in the vendored libsecp256k1 public headers. v1.1 does not add new script validation anyway. | N/A for v1.1 |
| `-mempoolfullrbf` semantics | Not what BIP-125 requires; breaks first-seen guarantees for wallets not opting in | BIP-125 signal-required replacement |
| Bloom filter (BIP-37) `filterload`/`filteradd` | Privacy-leaking, deprecated, no modern nodes use it | Not needed for peer compatibility |
| Separate block serialization buffer on stack | SegWit blocks up to 4MB; stack overflow risk | Heap allocation, explicit free |
| LevelDB | Would be a new external dependency | SQLite already present |

---

## Stack Patterns by Feature

**When advertising NODE_WITNESS:**
- Gate on `!node->ibd_mode` — do not advertise during IBD.
- Use `SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS` for non-pruned nodes.
- Use `SERVICE_NODE_NETWORK_LIMITED | SERVICE_NODE_WITNESS` for pruned nodes that keep recent blocks.

**When serving blocks (getdata handler):**
- Always check `BLOCK_STATUS_HAVE_DATA` and `BLOCK_STATUS_PRUNED` before reading disk.
- Allocate receive buffer on heap, not stack (`uint8_t *buf = malloc(MAX_BLOCK_SIZE)`).
- If `peer_queue_message()` returns `ECHO_ERR_FULL`, send `notfound` instead of blocking.

**When implementing RBF:**
- Collect the full eviction set (conflict + all descendants) before checking any fee rules.
- Apply rules in cheapest-first order: signal check → descendant count → bandwidth fee → absolute fee → new unconfirmed inputs.
- Never mutate mempool state until all checks pass.

**When computing witness commitment:**
- Coinbase wtxid is always 32 zero bytes — do NOT hash the coinbase transaction itself.
- The witness nonce in the coinbase witness stack is also 32 zero bytes for simple implementations.
- `SHA256d(witness_merkle_root || witness_nonce)` = the commitment. The coinbase OP_RETURN embeds this with the `0xaa21a9ed` prefix.

**When writing tx_index entries:**
- Write inside the same SQLite transaction as UTXO updates for the block.
- Record the tx's byte offset from the start of the block's transaction data (after the 80-byte header and tx count varint).
- On reorg (block disconnect), delete tx_index entries for disconnected block's transactions within the rollback transaction.

---

## Version Compatibility

| Protocol Feature | Required Peer Version | Notes |
|-----------------|----------------------|-------|
| NODE_WITNESS | >= 70013 | Introduced with BIP-144 / SegWit (2016). All active mainnet peers are >= 70013. |
| INV_WITNESS_BLOCK (0x40000002) | >= 70013 | Same. |
| Compact blocks (BIP-152) | >= 70014 | NOT implementing in v1.1. |
| getblocktemplate segwit rules | >= 70013 | "rules": ["segwit"] field per BIP-145. |
| BIP-125 RBF | Policy only | No protocol version signaling. Any peer can send replacement transactions; mempool policy decides acceptance. |
| tx_index | Internal only | No P2P protocol version dependency. |

---

## Sources

- [BIP-144: SegWit P2P](https://github.com/bitcoin/bips/blob/master/bip-0144.mediawiki) — NODE_WITNESS=(1<<3), MSG_WITNESS_BLOCK=0x40000002, wire serialization with witness — HIGH confidence
- [BIP-125: Opt-in RBF](https://github.com/bitcoin/bips/blob/master/bip-0125.mediawiki) — All 5 replacement rules, nSequence < 0xFFFFFFFE for signaling — HIGH confidence
- [BIP-22: getblocktemplate](https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki) — Template fields, mintime = MTP+1, sigoplimit, sizelimit — HIGH confidence
- [BIP-145: getblocktemplate SegWit update](https://bips.dev/145/) — default_witness_commitment, rules:["segwit"] — HIGH confidence
- [BIP-141: Witness commitment structure](https://github.com/bitcoin/bips/blob/master/bip-0141.mediawiki#commitment-structure) — 0xaa21a9ed prefix, SHA256d(witness_merkle_root||nonce) — HIGH confidence
- [developer.bitcoin.org P2P reference](https://developer.bitcoin.org/reference/p2p_networking.html) — getdata/block message flow, notfound — HIGH confidence
- [Bitcoin Core net_processing.cpp ProcessGetBlockData()](https://github.com/bitcoin/bitcoin/blob/master/src/net_processing.cpp) — CanServeBlocks(), CanServeWitnesses(), block-serving rate limits — HIGH confidence
- [Bitcoin Core txindex.cpp (CDiskTxPos)](https://github.com/bitcoin/bitcoin/blob/master/src/index/txindex.cpp) — file_index + file_offset + tx_pos pattern — HIGH confidence
- Codebase audit (2026-02-21) — node.c:2786 TODO, mempool.c:799 TODO, rpc.c:1783/1662/1814/1897 TODOs, node.c:2937-2962 services=1 — HIGH confidence

---

*Stack research for: Bitcoin Echo v1.1 — P2P block serving, BIP-125 RBF, tx index, RPC expansion*
*Researched: 2026-02-21*
