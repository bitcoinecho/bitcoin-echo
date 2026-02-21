# Pitfalls Research

**Domain:** Bitcoin full node — adding P2P block serving, BIP-125 full-RBF, transaction index, and RPC expansion to existing C11 node
**Researched:** 2026-02-21
**Confidence:** HIGH (drawn from BIP specifications, Bitcoin Core CVE disclosures, PR review discussions, protocol documentation, and direct codebase audit)

---

## Critical Pitfalls

### Pitfall 1: Block Serving Sends Wrong Serialization for MSG_BLOCK vs MSG_WITNESS_BLOCK

**What goes wrong:**
`getdata` requests arrive with two distinct inventory type codes: `INV_BLOCK` (0x00000002) for legacy (no-witness) serialization, and `INV_WITNESS_BLOCK` (0x40000002, bit 30 set) for witness-serialized blocks. Responding to `INV_WITNESS_BLOCK` with a legacy-serialized block strips all SegWit witness data. Modern peers (everything post-0.13.1) request `INV_WITNESS_BLOCK` exclusively. Responding incorrectly means they receive structurally invalid block data, immediately disconnect, and report the node as misbehaving.

The converse also fails: responding to `INV_BLOCK` with witness serialization adds bytes peers do not expect, causing parse errors.

**Why it happens:**
The bit-30 distinction is easy to miss. Developers implement a single `serialize_block()` path and forget that the inventory type field carries this serialization-format signal. The existing relay handler in `relay_handle_getdata()` must check `inv->type & INV_WITNESS_BLOCK` (not just `inv->type == INV_BLOCK`) to dispatch correctly.

**How to avoid:**
1. `relay_handle_getdata()` must branch on the 30th bit of `inv->type`:
   - `inv->type & 0x40000000` set: serialize with witness fields (SegWit-aware serialization)
   - Otherwise: serialize without witness fields (legacy)
2. Echo already defines `INV_WITNESS_BLOCK 0x40000002` in `protocol.h` — use it for the dispatch check.
3. After serving a block, verify in tests that a Bitcoin Core peer fetching via `getdata` receives a block where `tx[i].vin[j].witness` is present for SegWit transactions.

**Warning signs:**
- Bitcoin Core peer disconnects with "bad-txns-inputs-missingorspent" after receiving served block
- Witness fields missing in blocks served to peers despite the block having SegWit transactions
- Peers immediately request the block again after receiving it (they rejected the malformed response)

**Phase to address:** P2P block serving phase (P2P-02)

---

### Pitfall 2: NODE_WITNESS Not Advertised — Peers Will Not Request Witness Blocks

**What goes wrong:**
`SERVICE_NODE_WITNESS` (bit 3, value 8) must be included in the `services` field of the version message. Without it, modern peers assume the node cannot serve witness-serialized data and will not use it as a source for SegWit blocks. The node is invisible as a block source to the current network. All Bitcoin Core nodes since 0.13.1 require `NODE_WITNESS` before requesting witness-serialized blocks from a peer.

The current `peer_send_version()` in `peer.h` accepts `our_services` as a parameter. The call site in `node.c` must pass `SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS` (value 9).

**Why it happens:**
Developers advertise `NODE_NETWORK` (1) alone, unaware that `NODE_WITNESS` (8) is a separate, independently required bit. The node connects and handshakes normally — the flag omission is silent until someone notices the node is never asked to serve blocks.

**How to avoid:**
1. In the version message construction, always include `SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS` (= 9) in `nServices` for a full node.
2. If running with `--prune`, also include `SERVICE_NODE_NETWORK_LIMITED` (bit 10) so peers know to only request recent blocks.
3. Test by checking `getpeerinfo` on a connected Bitcoin Core node — the `services` field for the echo peer must show bit 3 set.

**Warning signs:**
- Bitcoin Core's `getpeerinfo` shows `"services": "0000000000000001"` (only NODE_NETWORK) for the echo peer
- No peers ever send `getdata` with `INV_WITNESS_BLOCK` to the echo node
- Echo node's block serving code is never triggered during steady-state operation after IBD

**Phase to address:** P2P service flags phase (P2P-01)

---

### Pitfall 3: Serving Pruned Blocks Returns Garbage to Peers

**What goes wrong:**
Echo runs with `--prune=1024` by default. A peer requesting a block at height 500,000 that has been pruned from disk will receive either: (a) an incorrect block (if the file position is stale and the file slot was reused), or (b) a crash/read error. Returning garbage causes the peer to ban the echo node for sending invalid data. The block index marks pruned blocks with `BLOCK_STATUS_PRUNED` — this flag must be checked before attempting to read from disk.

**Why it happens:**
Block serving implementations look up the file position from the block index and read blindly. The pruning status check is a separate flag lookup that is easy to omit when block_index_db already provides the position.

**How to avoid:**
1. In `relay_handle_getdata()`, after looking up the block index entry, check `entry.status & BLOCK_STATUS_PRUNED`. If pruned, send `notfound` immediately.
2. Also check `entry.status & BLOCK_STATUS_HAVE_DATA` — a block may be in the index without its data being stored.
3. Send a `notfound` message (same structure as `inv`) for any requested block that cannot be served. This is protocol-correct and tells the peer to try elsewhere.
4. If running with `NODE_NETWORK_LIMITED`, only serve blocks within the last 288 blocks above the minimum chain download limit (as per BIP-159).

**Warning signs:**
- Log shows block storage read errors when serving older blocks
- Peers disconnect after requesting blocks below the pruning cutoff
- `relay_handle_getdata` never sends `notfound` messages — only `block` messages

**Phase to address:** P2P block serving phase (P2P-02)

---

### Pitfall 4: BIP-125 Rule 3 Uses Absolute Fee, Not Feerate — Implementations Routinely Get This Wrong

**What goes wrong:**
BIP-125 Rule 3 requires the replacement transaction pay an absolute fee of at least the sum paid by all original transactions being evicted (including their descendants). A replacement with higher feerate but lower absolute fee must be rejected.

This is not an obscure edge case — it is the primary anti-bandwidth-exhaustion mechanism. Implementing Rule 3 as "replacement feerate > original feerate" instead of "replacement absolute fee >= sum of all evicted fees" creates a free relay attack: an attacker builds a large low-fee transaction chain, then broadcasts high-feerate but low-absolute-fee replacements to cause repeated mempool churn at zero net cost to themselves.

Bitcoin Core's `mempool.h` already defines `MEMPOOL_RBF_INCREMENT 1000` and `MEMPOOL_MAX_REPLACEMENT_COUNT 100` — but these constants only apply if the rule check code actually computes aggregate fees across the full eviction set.

**Why it happens:**
Rule 4 (feerate >= min relay feerate) and Rule 3 (absolute fee >= evicted absolute fees) are adjacent in the spec. Developers implement Rule 4 thinking they have addressed Rule 3. The fee-vs-feerate distinction requires explicitly summing across all evicted transactions, which requires walking the descendant graph first.

**How to avoid:**
1. Before checking Rule 3, walk the full descendant tree of every conflicting transaction to compute the total eviction set and aggregate fees. Cap at `MEMPOOL_MAX_REPLACEMENT_COUNT` (100) during this walk — if the walk exceeds 100, reject under Rule 5 before reaching Rule 3.
2. Rule 3 check: `replacement.fee >= sum(evicted_tx.fee for evicted_tx in eviction_set)`.
3. Rule 4 check (separate): `replacement.fee_rate >= mempool_min_fee_rate(mp)`.
4. Test vectors: original 1 tx paying 5000 sats; replacement paying 4999 sats at 10x feerate — must be rejected (Rule 3 fails despite Rule 4 passing).

**Warning signs:**
- Mempool accepts replacements with lower absolute fee than original
- `conflicts_count` in `mempool_accept_result_t` is always 1 (descendant traversal skipped)
- No test exercises: high-feerate + low-absolute-fee replacement scenario

**Phase to address:** BIP-125 full-RBF phase (P2P-03)

---

### Pitfall 5: BIP-125 Rule 2 — Replacement Cannot Introduce New Unconfirmed Inputs

**What goes wrong:**
Rule 2 states: "The replacement transaction may only include an unconfirmed input if that input was included in one of the original transactions." A replacement that spends a new unconfirmed parent (one not in the original conflicting set) creates a new, unvalidated dependency. This allows pinning: an attacker broadcasts the victim's transaction, then submits a "replacement" that depends on an attacker-controlled unconfirmed output, anchoring the victim's replacement to the attacker's arbitrary timeline.

**Why it happens:**
When collecting conflicting transactions to build the eviction set, developers focus on outputs being double-spent. They do not check whether the replacement's *inputs* introduce new unconfirmed parents beyond those already in the original's dependency graph.

**How to avoid:**
1. For each input in the replacement transaction, if the input is unconfirmed (mempool UTXO, not confirmed UTXO), verify that the spending outpoint's transaction ID appears in the set of original conflicting transactions.
2. If any replacement input refers to an unconfirmed parent NOT present in the originals, reject with `MEMPOOL_REJECT_CONFLICT`.
3. Test: original tx spends confirmed UTXO A; replacement spends confirmed UTXO A + unconfirmed UTXO B (not in original) — must be rejected.

**Warning signs:**
- Replacement accepted when it has more inputs than the original
- No check performed against mempool parent set of replacement inputs
- `mempool_is_spent()` checked but `mempool_lookup()` not used to verify parent is in eviction set

**Phase to address:** BIP-125 full-RBF phase (P2P-03)

---

### Pitfall 6: Full-RBF Inherited Signaling Not Propagated to Descendants

**What goes wrong:**
BIP-125 states: "Transactions that don't explicitly signal replaceability are replaceable for as long as any one of their ancestors signals replaceability." Bitcoin Core's implementation historically did NOT implement inherited signaling for descendants (CVE-2021-31876 / PR #21946). A child transaction of an RBF-signaling parent would be reported as non-replaceable, even though it inherits replaceability.

For full-RBF (where *all* transactions are replaceable regardless of signaling), inherited signaling is moot — every transaction is replaceable. But if Echo implements opt-in RBF as a stepping stone before full-RBF, inherited signaling must be tracked in the `signals_rbf` field of `mempool_entry_t`. Failure means: a child of a replaceable parent incorrectly appears locked in, causing tooling built on `mempool_lookup()` to misreport replaceability.

**Why it happens:**
The `signals_rbf` field is set at transaction acceptance time based on the transaction's own inputs. Ancestor state is not consulted. Tracking inherited replaceability requires checking parent entries in the mempool at add time.

**How to avoid:**
For full-RBF: treat every mempool transaction as replaceable unconditionally. This sidesteps inherited signaling entirely and matches Bitcoin Core's `memempoolrequirestandard=0` / `-mempoolfullrbf` behavior.

For opt-in RBF: when setting `entry.signals_rbf`, also check `mempool_lookup(mp, parent_txid)->signals_rbf` for each unconfirmed parent input. If any parent signals RBF, the child inherits it.

**Warning signs:**
- `entry.signals_rbf` is always set from `tx.vin[i].sequence` alone with no ancestor check
- Child transactions of RBF-signaling parents cannot be replaced despite BIP-125 permitting it
- `mempool_reject_string(MEMPOOL_REJECT_CONFLICT)` returned for replacements of inherited-RBF descendants

**Phase to address:** BIP-125 full-RBF phase (P2P-03)

---

### Pitfall 7: Transaction Index Not Invalidated on Chain Reorganization

**What goes wrong:**
A transaction index maps txid → (block_file, block_offset) for confirmed transactions. When a chain reorganization disconnects a block, any transactions confirmed in that block are no longer confirmed — they move back to the mempool (if valid on the new tip) or are orphaned. If the tx index is not updated during reorg, it retains stale entries pointing to blocks that are no longer on the main chain. `getrawtransaction` will return transaction data for "confirmed" transactions that are actually unconfirmed or invalid on the current tip, and worse: may return the wrong transaction if a txid appears in both the old and new chain at different positions.

**Why it happens:**
The UTXO rollback system (`chainstate_revert_block`) handles the UTXO set. The transaction index is a separate data structure that requires its own rollback pass. Developers implementing the tx index as an append-only write (insert on confirm) without a delete-on-disconnect path leave it inconsistent after reorg.

**How to avoid:**
1. The tx index must be updated in the same reorg handling path as the UTXO delta reversal.
2. When disconnecting a block: DELETE FROM tx_index WHERE block_hash = {disconnected_block_hash}.
3. When connecting a block on the new fork: INSERT INTO tx_index for each transaction in the new block.
4. Both operations must be in the same SQLite transaction as the UTXO delta writes.
5. Test: confirm 3 blocks, reorg back 2 blocks, re-confirm different transactions. Verify tx index contains only current-chain transactions.

**Warning signs:**
- `rpc_getrawtransaction` returns data for transactions in disconnected blocks
- No DELETE path in the tx index update code path
- Tx index updates happen in a different code path than `chainstate_revert_block` calls
- Tx index update is not wrapped in a SQLite transaction with UTXO updates

**Phase to address:** Transaction index phase (RPC-01), must respect existing reorg handling

---

### Pitfall 8: Transaction Index Schema Collides With Block Index Existing SQLite Handles

**What goes wrong:**
The existing codebase uses separate SQLite database files for the block index (`block_index_db_t`) and UTXO set (`utxo_db_t`). Adding a transaction index as a third table in one of these existing databases causes:
1. Lock contention: WAL mode allows one writer at a time per database. IBD writes to the block index at high frequency. Tx index writes during block confirmation would contend with block index writes.
2. Schema coupling: if the tx index is added to `block_index_db.h`, all callers of block index functions now depend on tx index tables existing.

**Why it happens:**
Adding a new table to an existing database feels simpler than creating a new database file. But SQLite WAL contention is real, and schema coupling makes the tx index impossible to disable cleanly.

**How to avoid:**
1. Create a separate SQLite database file: `tx_index.db` in the data directory.
2. Use the existing `db_t` infrastructure from `db.h` to manage the new database.
3. Enable WAL mode separately on the tx index database.
4. The tx index schema is simple: `CREATE TABLE tx_index (txid BLOB PRIMARY KEY, file_index INTEGER, file_offset INTEGER, block_height INTEGER)`.
5. Make the tx index optional: controlled by a config flag, not always present.

**Warning signs:**
- Tx index tables added to `block_index_db.h` schema
- Block index writes and tx index writes share the same `db_t` handle
- No separate SQLite file for the tx index

**Phase to address:** Transaction index phase (RPC-01)

---

### Pitfall 9: getblocktemplate Omits or Misplaces the SegWit Witness Commitment

**What goes wrong:**
BIP-145 requires `getblocktemplate` to include a `default_witness_commitment` field when SegWit transactions are in the template. The commitment is a 38-byte `OP_RETURN` output that encodes `SHA256d(witness_merkle_root || coinbase_witness_nonce)`. Two common implementation mistakes:

1. **Commitment in coinbasetxn**: BIP-145 explicitly prohibits the server from including the commitment in `coinbasetxn`. The miner/client must insert it. If the server pre-inserts it, the client inserts it again, producing an invalid coinbase.
2. **Commitment not at end of coinbase outputs**: The commitment must be the final output of the coinbase transaction. If placed elsewhere, Bitcoin Core's `submitblock` validator rejects the block.

A third mistake: computing the witness merkle root using txids instead of wtxids. The witness commitment merkle root uses wtxids for all non-coinbase transactions; the coinbase wtxid is defined as all-zeros.

**Why it happens:**
BIP-141 (witness commitment) and BIP-145 (getblocktemplate witness update) must be read together. The `default_witness_commitment` field name sounds like "we provide it complete" but actually means "here is the pre-built commitment output script, client inserts it." The wtxid vs txid distinction requires reading BIP-141 §6.2 carefully.

**How to avoid:**
1. `rpc_getblocktemplate` returns `default_witness_commitment` as a hex-encoded output script (the `OP_RETURN 0xaa21a9ed...` script), not a complete output or a complete transaction.
2. The witness merkle root is computed using wtxids: `coinbase_wtxid = 0x00...00` (32 zero bytes); `other_tx_wtxid = double_sha256(witness_serialization)`.
3. `mining.h` already has `coinbase_params_t.include_witness_commitment` and `witness_commitment` fields. Populate them correctly.
4. Test: generate a block template, mine it with a Python script, submit via `submitblock`, verify Bitcoin Core accepts it.

**Warning signs:**
- Witness commitment inserted into `coinbasetxn` in the RPC response
- Witness merkle root computed using txids instead of wtxids
- `submitblock` rejects blocks generated from the template with "bad-witness-merkle-match"
- Commitment output is not the last output in the coinbase

**Phase to address:** RPC getblocktemplate phase (RPC-05)

---

### Pitfall 10: getblock Raw Hex Omits the 4-Byte Segwit Marker and Flag Bytes

**What goes wrong:**
SegWit (BIP-141) extended the serialization format: a valid SegWit block includes a 2-byte marker (`0x00 0x01`) after the transaction count when at least one transaction has witnesses. `getblock` verbosity=0 must return the full witness serialization, not the stripped (legacy) serialization, or downstream tools and wallets that parse the raw hex will fail to find witness data.

The converse is also a pitfall: if the node always appends the segwit marker even for pre-SegWit blocks (pre-481,824), the marker bytes make the block appear malformed to legacy parsers.

**Why it happens:**
Block serialization has two modes. The simpler "write all transactions" path omits the marker/flag. The witness serialization path requires: checking if any transaction has witness data, conditionally adding the 2-byte marker, serializing transactions with witness fields, and serializing the witness data per-input. Implementing only the simple path and never the witness path produces truncated data.

**How to avoid:**
1. When building the raw hex for `getblock` verbosity=0, check if the block contains any transaction with non-empty witness data.
2. If yes: use witness serialization. Specifically: version (4 bytes) | marker (0x00) | flag (0x01) | tx_count (varint) | transactions with witness | locktime (4 bytes).
3. The existing `blocks_storage.h` stores the raw bytes as written by the serializer — verify the stored bytes already include witness data (they should, given that the network sends witness-serialized blocks when `INV_WITNESS_BLOCK` is requested).
4. Test: call `getblock` on a known SegWit block, decode the hex, verify `tx[0].vin[0].witness` (coinbase witness nonce) is present.

**Warning signs:**
- Raw hex from `getblock` has no `0x00 0x01` marker for post-SegWit blocks
- `getblock` hex is shorter than the on-wire size of the block
- Bitcoin Core's `decoderawtransaction` fails to parse the returned hex

**Phase to address:** RPC getblock phase (RPC-03)

---

### Pitfall 11: mediantime Computation Uses Wrong Block Ancestor Set

**What goes wrong:**
Median time past (MTP) is defined as the median timestamp of the previous 11 blocks (heights `current-1` through `current-11`). Two implementation mistakes:

1. **Using `current` block's timestamp in the median**: MTP is computed from the *previous* 11 blocks, not including the current block. For block N, MTP = median(blocks N-1, N-2, ..., N-11).
2. **Sorting incorrectly**: MTP requires sorting the 11 timestamps and returning the 6th element (index 5, the middle of 11). If the sort is ascending but the return is the first or last element, the wrong value is returned.

For `getblockchaininfo`, `mediantime` must return the MTP of the current best tip — i.e., median of the tip's previous 11 blocks. This is used by transaction locktime validation and displayed for diagnostic purposes.

**Why it happens:**
The spec says "median of previous 11 blocks." "Previous" is ambiguous: does it include the current block (making it 11 of the last 12)? It does not. Off-by-one errors are common in window calculations.

**How to avoid:**
1. To compute MTP for block at height H: fetch blocks at heights H-1, H-2, ..., H-11 (capped at genesis for early blocks).
2. Extract `header.timestamp` from each. Sort the 11 (or fewer) values.
3. Return the middle value: `sorted[count / 2]` (rounds down — for 11 values, index 5).
4. For `getblockchaininfo`, fetch the best tip from the block index, then compute MTP for that tip.
5. Test: Bitcoin mainnet block 481,824 (SegWit activation) has a known MTP. Verify the implementation matches.

**Warning signs:**
- `mediantime` in `getblockchaininfo` equals `current block timestamp` (off-by-one: included current block)
- MTP never changes between consecutive blocks (returning the same element always)
- `getblockchaininfo.mediantime` does not match Bitcoin Core's value for the same tip

**Phase to address:** RPC mediantime phase (RPC-04)

---

### Pitfall 12: Peer Send Queue Overflow When Serving Large Blocks

**What goes wrong:**
Echo's peer send queue (`PEER_SEND_QUEUE_SIZE 128` entries) holds `msg_t` values. A `msg_t` contains a `msg_block_t` which contains a `block_t` — a stack-allocated structure. A SegWit block can reach 4 MB. If `peer_queue_message()` copies the entire block into the queue, and the queue has 128 slots, the send queue can consume up to 512 MB of stack or heap memory per peer connection. With multiple concurrent peers requesting large blocks, the node runs out of memory.

The current `msg_t` union stores `msg_block_t block` by value. When block serving is added, this becomes a memory pressure hotspot.

**Why it happens:**
During IBD (download direction), the node only receives blocks — it never places them in the send queue. Block serving (upload direction) is new. The send queue design that worked for small control messages is insufficient for 4 MB payloads.

**How to avoid:**
1. For block responses, do not queue the full `msg_t`. Instead, queue a handle (block hash + inv type) and serialize directly to the socket at send time.
2. Alternatively: allocate block response buffers on the heap and store a pointer in the queue entry, using the `allocated` flag in `peer_msg_queue_entry_t` to trigger free on send.
3. Rate-limit block serving per peer (e.g., max 16 blocks per second per peer) to bound memory consumption from queued block responses.
4. Check `PEER_RECV_BUFFER_SIZE (1 MB)` and `PEER_SEND_QUEUE_SIZE (128)` constants. With 128 queued messages each containing a 4 MB block, this is 512 MB per peer — multiply by max 125 peers = 64 GB. This is never acceptable.

**Warning signs:**
- `peer_queue_message` copies full block data (4 MB+) into queue slot
- Node RSS grows rapidly when multiple peers request large blocks simultaneously
- OOM kill or allocation failure during block serving under load

**Phase to address:** P2P block serving phase (P2P-02), before any load testing

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Tx index only updated on block connect, not disconnect | Simple implementation | Stale entries after reorg; wrong data from RPC | Never acceptable on mainnet |
| Witness commitment computed using txids not wtxids | Simpler merkle code | `submitblock` rejections; invalid blocks | Never acceptable for `getblocktemplate` |
| Block serving from queue with full block copy | Simpler send path | OOM under concurrent peer load | Never for production; pointer/handle approach required |
| `mediantime` field hardcoded to zero or tip timestamp | No block index walks | Wrong locktime validation displayed; violates BIP-113 | Never acceptable |
| Skipping RBF Rule 2 (no new unconfirmed inputs check) | Simpler replacement logic | Enables mempool pinning attacks | Never acceptable |
| `getblock` returns legacy serialization always | Simpler serializer | No witness data in returned hex; breaks SegWit parsers | Never acceptable post-SegWit activation |
| `hash_scriptpubkeys`/`hash_amounts` placeholders in `script.c` | Compiles and passes single-input Taproot | Fails real multi-input Taproot txs on mainnet | Acceptable as v1.0 tech debt, blocks mainnet Taproot coverage |

---

## Integration Gotchas

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| Block index + tx index SQLite | Sharing a single database file for all indexes | Separate `tx_index.db` file to avoid WAL writer contention with block index writes |
| BIP-145 `default_witness_commitment` | Including pre-built commitment in `coinbasetxn` | Return hex script in `default_witness_commitment`; miner appends it as last coinbase output |
| Witness merkle root for `getblocktemplate` | Using txids for all transactions | Coinbase wtxid = 32 zero bytes; all other txs use wtxid (witness serialization hash) |
| `getblock` response serialization | Using stored raw bytes without checking if they include witness marker | Stored bytes from `block_storage_read()` include witness if the network sent `INV_WITNESS_BLOCK` during IBD — verify storage format first |
| Tx index vs mempool lookup | `getrawtransaction` looks only at tx index (confirmed) | Must check mempool first, then tx index; many transactions are unconfirmed |
| `INV_WITNESS_BLOCK` bit check | `inv->type == INV_WITNESS_BLOCK` (exact match) | `inv->type & 0x40000000` (bit check) — the lower bits carry the object type |
| RBF eviction count (Rule 5) | Counting only direct conflicts | Must count originals + all their mempool descendants; use `descendant_count` from `mempool_entry_t` |
| Pruned block serving | Attempting `block_storage_read()` without status check | Check `entry.status & BLOCK_STATUS_PRUNED` before read; send `notfound` if pruned |

---

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| Full block copy into peer send queue | RSS grows proportional to (peers × max_block_size) | Queue block handles/pointers, serialize at send time | First concurrent multi-peer block serving test |
| Tx index lookup on every `getrawtransaction` without mempool shortcut | Slow response for unconfirmed txs (disk read unnecessary) | Check mempool first, only hit tx index for confirmed txs | Mempool has 1000+ transactions and RPC is polled frequently |
| Descendant graph traversal on every RBF replacement attempt | O(n) per replacement where n = descendant count | Keep descendant count cached in `mempool_entry_t` (already designed this way) | With 25 descendants per transaction (max ancestors/descendants = 25) |
| MTP computation walking 11 blocks from disk on every `getblockchaininfo` call | Slow RPC response; disk reads per RPC call | Cache MTP at the node level, update on new block; invalidate on reorg | When `getblockchaininfo` is polled frequently (e.g., GUI refresh) |
| Block index lookup by height (ambiguous during reorg) | Returns wrong block during active reorg | Use `block_index_db_get_chain_block()` which filters by `BLOCK_STATUS_VALID_CHAIN` | During any reorg, even shallow ones |

---

## Security Mistakes

| Mistake | Risk | Prevention |
|---------|------|------------|
| Serving blocks without rate limit | Any peer can exhaust I/O and block serving capacity (DoS) | Rate-limit block responses per peer; track in-flight responses |
| CVE-2024-52920 pattern: malformed `getdata` with huge count causes loop | CPU DoS on block serving thread | Validate `inv->count <= MAX_INV_ENTRIES` (50,000) before processing; return error on malformed count |
| Block download state indexed globally not per-peer | Adversarial peer cancels other peers' block downloads (CVE-2024-52921 pattern) | Index all pending block request state by (peer_id, block_hash), never by block_hash alone |
| RBF Rule 3 missing (absolute fee check) | Free relay attack: attacker causes repeated mempool churn at zero fee cost | Compute aggregate eviction fee before accepting any replacement |
| Tx index accepts double-write on reorg without DELETE | Old chain transactions remain "confirmed" after reorg | DELETE tx_index entries for disconnected blocks in same SQLite tx as UTXO rollback |
| `getblocktemplate` without IBD-complete guard | Mining pool receives template from syncing node, mines invalid blocks | Return RPC error code `RPC_ERR_CLIENT_IN_WARMUP` (-28) while `getsyncstatus` shows incomplete |

---

## "Looks Done But Isn't" Checklist

- [ ] **Block serving:** `relay_handle_getdata` sends blocks — verify it checks `BLOCK_STATUS_PRUNED` and sends `notfound` for pruned blocks rather than reading stale file positions
- [ ] **Block serving:** Blocks are sent — verify `INV_WITNESS_BLOCK` triggers witness-serialized response and `INV_BLOCK` triggers legacy response; not the same serialization for both
- [ ] **NODE_WITNESS:** Version message sent — verify `nServices` field includes bit 3 (value 8) in addition to `NODE_NETWORK` (bit 0)
- [ ] **BIP-125 Rule 3:** Replacement accepted — verify the absolute fee was summed across all evicted transactions (originals + descendants), not just the feerate compared
- [ ] **BIP-125 Rule 2:** Replacement accepted — verify all unconfirmed inputs in the replacement were present in the original transaction set
- [ ] **BIP-125 Rule 5:** Replacement accepted — verify the total eviction set (originals + descendants) does not exceed 100 transactions
- [ ] **Tx index reorg:** Block disconnected — verify tx index rows for that block were deleted in the same SQLite transaction as UTXO delta reversal
- [ ] **getblocktemplate:** Template returned — verify `default_witness_commitment` is the commitment script, not pre-inserted into `coinbasetxn`; verify witness merkle root uses wtxids
- [ ] **getblocktemplate IBD guard:** Template returned — verify node returns `RPC_ERR_CLIENT_IN_WARMUP` when not fully synced
- [ ] **getblock verbosity=0:** Hex returned — verify SegWit blocks include the `0x00 0x01` marker bytes and witness data; not stripped to legacy format
- [ ] **mediantime:** Value returned — verify it is median of previous 11 blocks (not the current block's timestamp), and sorted correctly (median index = count/2)

---

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| Block serving wrong serialization format | LOW | Fix dispatch logic, restart; no persistent state impact |
| NODE_WITNESS not advertised | LOW | Add service flag bit, restart; peers reconnect and renegotiate |
| Pruned block served (garbage data) | LOW | Add status check, restart; peer ban expires after 24 hours |
| RBF Rule 3 missing (relay attack exposure) | LOW-MEDIUM | Patch and restart; mempool is ephemeral, no persistent state corruption |
| Tx index not rolled back on reorg (stale entries) | HIGH | Drop and rebuild tx index from scratch: `DROP TABLE tx_index; re-scan all blocks` |
| getblocktemplate witness commitment wrong | MEDIUM | Fix computation, restart; no chain state impact but invalid blocks may have been mined |
| getblock omitting witness data | LOW | Fix serialization, restart; no persistent state impact |
| Send queue OOM from full block copies | HIGH | Node crash; requires architectural fix to queue handling before block serving can be re-enabled |

---

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| Wrong MSG_BLOCK vs MSG_WITNESS_BLOCK serialization | P2P block serving (P2P-02) | Bitcoin Core peer fetches block with witnesses intact |
| NODE_WITNESS not advertised | P2P service flags (P2P-01) | `getpeerinfo` on connected Bitcoin Core shows bit 3 in services for echo peer |
| Serving pruned blocks without notfound | P2P block serving (P2P-02) | Request blocks below prune depth; verify `notfound` response and no crash |
| BIP-125 Rule 3 (absolute fee, not feerate) | Full-RBF phase (P2P-03) | Test: higher-feerate but lower-absolute-fee replacement rejected |
| BIP-125 Rule 2 (no new unconfirmed inputs) | Full-RBF phase (P2P-03) | Test: replacement with new unconfirmed parent rejected |
| Full-RBF inherited signaling | Full-RBF phase (P2P-03) | Child of RBF-signaling parent can be replaced |
| Tx index not rolled back on reorg | Tx index phase (RPC-01) | Reorg 2 blocks; getrawtransaction for rolled-back tx returns not-found |
| Tx index in shared SQLite database | Tx index phase (RPC-01) | Separate `tx_index.db` file exists; no WAL contention with block index writes |
| getblocktemplate witness commitment error | RPC getblocktemplate (RPC-05) | Mine a block from template; submitblock accepted by Bitcoin Core |
| getblocktemplate IBD guard missing | RPC getblocktemplate (RPC-05) | Call getblocktemplate while IBD in progress; verify RPC error -28 returned |
| getblock missing witness marker bytes | RPC getblock (RPC-03) | Decode returned hex; SegWit blocks contain witness fields |
| mediantime off-by-one | RPC mediantime (RPC-04) | Compare mediantime with Bitcoin Core for same tip hash |
| Send queue OOM from block copies | P2P block serving (P2P-02) | Serve blocks to 10 concurrent peers; RSS stays bounded |

---

## Sources

- [BIP-125: Opt-in Full Replace-by-Fee Signaling](https://bips.dev/125/) — all 5 rules verbatim (HIGH confidence)
- [BIP-145: getblocktemplate Updates for SegWit](https://bips.dev/145/) — witness commitment requirements (HIGH confidence)
- [BIP-141: Segregated Witness](https://github.com/bitcoin/bips/blob/master/bip-0141.mediawiki) — witness commitment merkle root construction (HIGH confidence)
- [BIP-113: Median time-past](https://github.com/bitcoin/bips/blob/master/bip-0113.mediawiki) — MTP definition (HIGH confidence)
- [Bitcoin P2P Network Reference](https://developer.bitcoin.org/reference/p2p_networking.html) — MSG_BLOCK vs MSG_WITNESS_BLOCK, notfound behavior (HIGH confidence)
- [Bitcoin Core CVE-2024-52920: DoS via huge GETDATA](https://bitcoincore.org/en/2024/07/03/disclose-getdata-cpu/) — getdata count validation required (HIGH confidence)
- [Bitcoin Core PR Review Club #22665: RBF replaceability status bug](https://bitcoincore.reviews/22665) — BIP-125 inherited signaling implementation gap (HIGH confidence)
- [Bitcoin Core PR #21946: Document lack of inherited signaling](https://github.com/bitcoin/bitcoin/pull/21946) — confirms Core divergence from BIP-125 spec (HIGH confidence)
- [Bitcoin Core PR #22698: Implement RBF inherited signaling](https://github.com/bitcoin/bitcoin/pull/22698) — correct fix for inherited signaling (HIGH confidence)
- [Transaction Pinning: Bitcoin Optech](https://bitcoinops.org/en/topics/transaction-pinning/) — Rule 3 and Rule 5 pinning attack vectors (MEDIUM confidence)
- [Replace-by-fee: Bitcoin Optech](https://bitcoinops.org/en/topics/replace-by-fee/) — full-RBF vs opt-in RBF ecosystem overview (MEDIUM confidence)
- [Bitcoin Core PR #27050: Witness blocks in prune mode](https://github.com/bitcoin/bitcoin/pull/27050) — witness serialization complexity when serving blocks (MEDIUM confidence)
- [Bitcoin Core Issue #28730: Empty witness data](https://github.com/bitcoin/bitcoin/issues/28730) — witness serialization must be checked in stored block data (MEDIUM confidence)
- [Bitcoin Core getblocktemplate RPC docs](https://bitcoincore.org/en/doc/23.0.0/rpc/mining/getblocktemplate/) — authoritative field list and behavior (HIGH confidence)
- [Bitcoin Core net_processing.cpp](https://github.com/bitcoin/bitcoin/blob/master/src/net_processing.cpp) — reference implementation for getdata block handler (MEDIUM confidence)
- [Prefer txindex for GetTransaction: PR Review Club #22383](https://bitcoincore.reviews/22383) — tx index mempool vs confirmed lookup ordering (MEDIUM confidence)
- Direct codebase read: `include/mempool.h`, `include/relay.h`, `include/peer.h`, `include/blocks_storage.h`, `include/block_index_db.h`, `include/rpc.h`, `include/protocol.h`, `include/mining.h` — existing architecture constraints (HIGH confidence)

---
*Pitfalls research for: Bitcoin full node peer compatibility v1.1 — P2P block serving, BIP-125 full-RBF, transaction index, RPC expansion (C11)*
*Researched: 2026-02-21*
