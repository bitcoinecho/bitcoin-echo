# Stack Research

**Domain:** Bitcoin full node peer compatibility (pure C11)
**Researched:** 2026-02-20
**Confidence:** HIGH (all claims verified against BIPs, Bitcoin Core source, or official protocol docs)

---

## Context

This is not a greenfield stack decision. Bitcoin Echo already has a defined, frozen stack:
pure C11 + vendored libsecp256k1 + vendored SQLite. No new external libraries are
permitted per the project manifesto. This document is therefore a **protocol specification
reference** — which BIPs govern each feature, what the correct approach is, and what must
NOT be done. Every "technology" choice here is "how to implement X correctly in pure C11."

---

## Recommended Stack

### Core Technologies

| Technology | Version | Purpose | Why Recommended |
|------------|---------|---------|-----------------|
| Pure C11 | ISO/IEC 9899:2011 | All implementation | Frozen requirement. No debate. |
| libsecp256k1 | Vendored (bitcoin-core/secp256k1) | ECDSA + Schnorr verification | `secp256k1_schnorrsig_verify()` is the correct entry point for BIP-340. Already vendored with `ENABLE_MODULE_SCHNORRSIG`. No batch verify API exists in the public header — individual calls per sig is the only option. |
| SQLite 3.x | Vendored amalgamation | Block index, UTXO set, tx index | Already used for chainstate/block index. Adding txindex here is a natural extension. WAL mode already configured for concurrency. |
| POSIX sockets/pthreads | System | TCP networking, threading | Already in use. No change needed. |

### Protocol Specifications (What to Implement)

| BIP | Feature | Status in Codebase | Implementation Approach |
|-----|---------|-------------------|------------------------|
| BIP-340 | Schnorr signatures (64-byte, x-only pubkey) | DONE — `sig_verify(SIG_SCHNORR, ...)` routes through libsecp256k1 | No change needed |
| BIP-341 | Taproot keypath + scriptpath spend | DONE — keypath Schnorr verify works | No change needed |
| BIP-342 | Tapscript + OP_CHECKSIGADD | PARTIAL — OP_CHECKSIGADD skeleton exists at `script.c:4842`, sighash and sig verify wired, but lives at `script.c:3330` returning `SCRIPT_ERR_BAD_OPCODE` in non-tapscript path. Need to confirm the tapscript path (line 4842) is actually reached. | Wire tapscript execution path correctly; verify `script_execute_tapscript()` is called for witness v1 scriptpath spends |
| BIP-144 | SegWit P2P messages + `NODE_WITNESS` service flag | MISSING — `node.c:1762` has TODO | Set `NODE_WITNESS = (1 << 3)` in version message services; use `MSG_WITNESS_BLOCK (0x40000002)` in `getdata` to peers that advertise `NODE_WITNESS` |
| BIP-125 | Replace-By-Fee (5 rules) | STUB — detects RBF signal, `mempool.c:799` has TODO | Implement all 5 rules: (1) RBF signal check (nSequence < 0xFFFFFFFE), (2) no new unconfirmed inputs, (3) absolute fee >= sum of replaced, (4) fee covers relay bandwidth at min relay rate, (5) replaced + descendants <= 100 |
| BIP-22 / BIP-145 | `getblocktemplate` | PARTIAL — `rpc.c:2111` has partial impl, selects mempool txs, but MTP/mintime not wired, witness commitment not constructed | Complete: MTP calculation, witness commitment hash in coinbase (OP_RETURN output), correct `default_witness_commitment` field in response |
| BIP-34 | Block height in coinbase | DONE — `coinbase_encode_height()` exists in `mining.h` | No change needed |

### Supporting Approach by Feature Area

#### Block Serving (`getdata` → `MSG_BLOCK`)

**Approach:** When a peer sends `getdata` with `INV_BLOCK` or `INV_WITNESS_BLOCK`, look up the block's position via the block index (`block_index_t.data_file` + `block_index_t.data_pos`), read it from disk via `block_storage_read()`, and send it as a `MSG_BLOCK` message.

**What NOT to do:** Do not serve blocks that are pruned. Check `data_file == BLOCK_DATA_NOT_STORED` before attempting to read. Send `notfound` if unavailable — the skeleton for this already exists at `node.c:2762`.

**Protocol detail:** For peers that advertised `NODE_WITNESS`, respond to `INV_WITNESS_BLOCK` with witness-serialized block data. The serialization difference: witness transactions include witness stack data after inputs/outputs. Echo already serializes witness data (`tx.has_witness` path in `tx_serialize()`).

**Rate limiting:** Bitcoin Core enforces `MAX_BLOCKS_IN_TRANSIT_PER_PEER = 16` concurrent block requests per peer. Echo does not need to replicate this exactly, but should not serve unlimited concurrent requests to avoid DoS.

**Source:** [developer.bitcoin.org P2P reference](https://developer.bitcoin.org/reference/p2p_networking.html) — HIGH confidence

---

#### NODE_WITNESS Service Flag (BIP-144)

**Approach:** Add `NODE_WITNESS (1 << 3) = 8` to the service flags bitmask in the `version` message. This advertises that the node can provide witness-serialized blocks and transactions.

**Consequence:** After advertising `NODE_WITNESS`, peers will send `INV_WITNESS_BLOCK` instead of `INV_BLOCK` for new blocks. The node must be prepared to handle and serve witness data in both directions.

**Inventory types defined by BIP-144:**
- `MSG_WITNESS_TX = 0x40000001` — already referenced in `node.c:1194`
- `MSG_WITNESS_BLOCK = 0x40000002` — referenced in `node.c:2695`

**What NOT to do:** Do not advertise `NODE_WITNESS` until the node can actually serve witness blocks. Advertising prematurely causes peers to send witness-formatted `getdata` requests that the node cannot fulfill.

**Source:** [BIP-144](https://github.com/bitcoin/bips/blob/master/bip-0144.mediawiki) — HIGH confidence

---

#### Tapscript / OP_CHECKSIGADD (BIP-342)

**Approach:** The implementation at `src/consensus/script.c:4842` has the correct skeleton — it pops `(sig, n, pubkey)`, handles empty sig (push n unchanged), validates 64/65-byte sig length and 32-byte pubkey length, computes `sighash_taproot()` with `ext_flag=1` for script path, and calls `sig_verify(SIG_SCHNORR, ...)`. The logic appears complete in `script_execute_tapscript()`.

**The critical gap:** The stub at line 3330 returns `SCRIPT_ERR_BAD_OPCODE` — this is in the non-tapscript execution path. The question is whether `script_execute_tapscript()` is correctly invoked for witness v1 scriptpath spends. Verify that `script_execute()` routes witness v1 scriptpath spends to `script_execute_tapscript()` rather than the generic opcode dispatcher.

**OP_SUCCESS opcodes (BIP-342):** Opcodes 80, 98, 126-129, 131-134, 137-138, 141-142, 149-153, 187-254 cause immediate script success. These must be checked at parse time, before any execution — if any OP_SUCCESSx appears anywhere in the script, validation passes immediately. The existing `script_execute_tapscript()` has this check at line 4822.

**CHECKMULTISIG disabled:** `OP_CHECKMULTISIG` and `OP_CHECKMULTISIGVERIFY` must fail immediately in Tapscript context (not succeed silently). Line 4837 handles this.

**Annex handling (BIP-341/342):** If the last witness stack element starts with `0x50`, it is the annex and must be stripped before script execution. Must not fail validation because of the annex's presence, but its contents are currently undefined.

**Source:** [BIP-342](https://github.com/bitcoin/bips/blob/master/bip-0342.mediawiki), [Bitcoin Core interpreter.cpp](https://github.com/bitcoin/bitcoin/blob/master/src/script/interpreter.cpp) — HIGH confidence

---

#### BIP-125 RBF (Replace-By-Fee)

**Approach:** Implement all 5 rules inside `mempool_accept()` when `has_conflict == true` and the conflicting transaction signals RBF (`signals_rbf == true`).

The 5 rules (from BIP-125):
1. **Signaling:** All transactions being replaced must signal RBF via `nSequence < 0xFFFFFFFE`. Already detected as `entry->signals_rbf`.
2. **No new unconfirmed inputs:** The replacement transaction must not introduce unconfirmed inputs that were not in the original transaction(s). Check that all inputs either come from confirmed UTXOs or from the inputs of transactions being replaced.
3. **Absolute fee:** Replacement total fee >= sum of fees of all transactions being evicted (including descendants).
4. **Bandwidth fee:** Replacement fee >= (its own vsize) × min_relay_fee_rate. Must pay for the relay bandwidth consumed.
5. **Descendant limit:** Total number of transactions to be evicted (the conflicting tx and all its descendants) must be <= 100.

**Implementation order:** Rules 1 and 5 are cheapest. Check rule 1 first (already done). Check rule 5 by counting descendants of the conflicting transaction. Then check rules 3 and 4 (requires knowing the total fee of evicted transactions). Finally remove the evicted transactions atomically before accepting the replacement.

**Data structures:** The mempool already has `ancestor_fee` tracking. Need to add descendant tracking (or walk the mempool graph on demand). For a mempool under 5000 entries, walking on demand is fine.

**What NOT to do:** Do not implement full-RBF (replacing without the `nSequence` signal) — Bitcoin Core has debated this at length and it is not required for BIP-125 compatibility. Stick to BIP-125 signaled RBF only.

**Source:** [BIP-125](https://github.com/bitcoin/bips/blob/master/bip-0125.mediawiki) — HIGH confidence

---

#### Chainstate Rollback / Reorg Handling

**Approach:** The delta system is designed but incomplete. `block_delta_t` records both created UTXOs (to delete) and spent UTXOs (to restore). The `chainstate_apply_delta_reverse()` function exists at `chainstate.c:718` and has the reversal logic, but the chainwork recalculation is stubbed.

**What must be implemented:**
1. **UTXO undo:** When rolling back, for each entry in `delta->created`, remove the UTXO from the UTXO set. For each entry in `delta->spent`, restore the UTXO. This is the "undo" of `utxo_apply_block()`.
2. **Chainwork on rollback:** Store the previous chainwork in the delta at apply time. On rollback, restore it from the delta. This is simpler than recomputation and avoids requiring a full chain walk.
3. **Height index rollback:** Update `state->height_index` to remove the rolled-back block hash.
4. **Chaser integration:** `chaser_confirm.c:250` has the TODO — it calls the rollback notification but doesn't invoke `chainstate_apply_delta_reverse()`. Wire this.

**Storage format — chainwork endianness:** The `work256_t` struct stores chainwork little-endian (`uint8_t bytes[32]`). For SQLite comparison using `ORDER BY chainwork DESC`, this requires big-endian storage so that SQLite's bytewise comparison produces the correct ordering. Fix: store chainwork bytes reversed (big-endian) in SQLite, convert on read.

**Bitcoin Core's approach (reference):** Bitcoin Core stores undo data in `rev*.dat` files alongside block files. Each block's undo data contains the spent outputs (CTxOut = amount + scriptPubKey) needed to reconstruct the UTXO set state before that block. Echo's delta system achieves the same goal in-memory/SQLite.

**Source:** [Bitcoin Wiki Data Storage](https://en.bitcoin.it/wiki/Bitcoin_Core_0.11_(ch_2):_Data_Storage), codebase audit — HIGH confidence

---

#### Transaction Index (for `getrawtransaction`)

**Approach:** Store a mapping from txid → `(file_index, file_offset, tx_offset_within_block)` in SQLite. Add a new table `tx_index` to the existing block index database. Populate it during block confirmation (when blocks are written to disk). The `block_file_pos_t` struct already provides `file_index` and `file_offset` for the block; add a per-transaction byte offset within the block.

**Schema:**
```sql
CREATE TABLE tx_index (
  txid BLOB NOT NULL PRIMARY KEY,  -- 32 bytes, big-endian txid
  file_index INTEGER NOT NULL,     -- blk*.dat file number
  file_offset INTEGER NOT NULL,    -- byte offset of block start in file
  tx_offset INTEGER NOT NULL       -- byte offset of tx within block data
);
```

**Lookup:** `getrawtransaction` → query `tx_index` → read block from `block_storage_read()` → deserialize block → extract transaction at `tx_offset`.

**What NOT to do:** Do not use a separate flat-file index like Bitcoin Core's LevelDB approach — SQLite is already the persistence layer and adding a new table is simpler than introducing a second storage engine.

**Pruning interaction:** If the block containing the transaction has been pruned, the txindex entry will point to a deleted file. Either tombstone these on prune or accept that `getrawtransaction` returns not-found for pruned blocks (Bitcoin Core behavior with pruning enabled).

**Source:** [Bitcoin Core txindex.cpp](https://github.com/bitcoin/bitcoin/blob/master/src/index/txindex.cpp), [Bitcoin Core PR Review Club](https://bitcoincore.reviews/22383) — HIGH confidence

---

#### `getblocktemplate` Completion (BIP-22 + BIP-145)

**What's already done:** Transaction selection from mempool (`mempool_select_for_block()`), basic block header fields, version bits.

**What's missing:**
1. **MTP (Median Time Past):** `mintime` field must be `MTP + 1`. Query the last 11 blocks' timestamps from the block index, sort them, return the median. Already identified as a bug in `getblockchaininfo` — fix both together.
2. **Witness commitment:** For SegWit blocks (height >= 481824 on mainnet), the coinbase must include an `OP_RETURN` output with the witness commitment: `SHA256d(witness_merkle_root || coinbase_witness_nonce)`. BIP-145 requires `getblocktemplate` to return a `default_witness_commitment` field. The `coinbase_params_t` struct already has `include_witness_commitment` and `witness_commitment` fields.
3. **Witness merkle root:** Compute the witness merkle tree (using wtxids instead of txids, with the coinbase wtxid = all zeros). This is required to populate the witness commitment.
4. **`sigoplimit` and `sizelimit`:** Return the maximum signature operations and block weight limits so miners know the constraints.

**BIP-145 requirement:** Return `"rules": ["segwit"]` in the template to indicate SegWit support. Miners that don't support SegWit will ignore this field; miners that do will use the witness commitment.

**Source:** [BIP-22](https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki), [BIP-145](https://bips.dev/145/) — HIGH confidence

---

### Development Tools

| Tool | Purpose | Notes |
|------|---------|-------|
| clang-tidy (cert-*, bugprone-*, misc-*, portability-*, readability-*) | Static analysis | Already configured. Must pass zero warnings. |
| AddressSanitizer + LeakSanitizer | Memory safety | Already in CI. Run on all new code. |
| Custom test runner (`test/run_all_tests.sh`) | Unit test execution | Already 40+ test executables. Add Taproot vectors. |
| BIP-342 test vectors | Tapscript validation correctness | Available in the BIP repository and in Bitcoin Core's test suite (`src/test/data/script_tests.json`). Must pass all. |

---

## Alternatives Considered

| Recommended | Alternative | When to Use Alternative |
|-------------|-------------|-------------------------|
| SQLite for txindex | Separate flat-file hash → offset map | If SQLite proves too slow for txindex lookups (unlikely with indexed column). Benchmark first. |
| Individual Schnorr verify per OP_CHECKSIGADD call | Batch verify all sigs in one call | Batch verify does NOT exist in the vendored libsecp256k1 public API (`secp256k1_schnorrsig.h`). Would require using internal APIs or vendoring a different library. Not permitted. Individual verify is correct per BIP-342. |
| In-memory delta for reorg undo | rev*.dat files like Bitcoin Core | Echo's delta system is already designed and partially implemented. Completing it is less work than implementing a separate undo file format. Rev*.dat is only needed if reorg depth exceeds `DELTA_REORG_DEPTH = 550`, which is not a realistic concern. |
| RBF-only replacement (BIP-125) | Full-RBF (replace any transaction regardless of signal) | Full-RBF is controversial and not required for P2P compatibility. Stick to BIP-125 signaled RBF. |

---

## What NOT to Use

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| Any new external library | Violates project manifesto; breaks pure C11 + stdlib constraint | Implement in pure C11 or use existing vendored libs |
| `secp256k1_schnorrsig_verify` batch variant | Does not exist in the public libsecp256k1 API as of 2026 | `secp256k1_schnorrsig_verify()` called once per signature in OP_CHECKSIGADD loop |
| LevelDB for txindex | Would be a new external dependency | SQLite `tx_index` table in existing database |
| BIP-152 compact blocks | Optional protocol feature; adds significant complexity for block relay; not required for peer compatibility | Standard `getdata` → `block` message flow is sufficient |
| Bloom filters (BIP-37) | Deprecated privacy-leak vector; modern nodes disable by default | Not needed for peer compatibility; can return "not supported" |
| `getaddr` spam | Some implementations flood peers with addr messages | Send `addr` only with own address, rate-limit peer addr relay |

---

## Stack Patterns by Feature

**When implementing block serving:**
- Use `block_index_t.data_file` and `block_index_t.data_pos` to locate the block
- Read via `block_storage_read()` (already has mutex protection)
- Check `BLOCK_DATA_NOT_STORED` before attempting read
- For `INV_WITNESS_BLOCK` requests, the raw block data already contains witness transactions if it was stored with witness data

**When implementing RBF:**
- Walk the mempool conflict graph only once per accept attempt
- Keep a running total of evicted fees during the conflict walk (needed for rule 3)
- Atomic: either accept the replacement and evict all conflicting descendants, or reject. No partial state.

**When implementing reorg:**
- Delta apply and reverse must be transactional with respect to the UTXO SQLite database
- Use SQLite `BEGIN TRANSACTION` / `COMMIT` / `ROLLBACK` to ensure UTXO changes are atomic
- Test with a 6-block reorg scenario on regtest before marking complete

**When implementing chainwork storage:**
- Store as big-endian in SQLite for correct bytewise sort ordering
- Convert to little-endian `work256_t` when loading into memory (reverse the 32 bytes)
- All in-memory arithmetic uses little-endian; only the SQLite stored value is big-endian

---

## Version Compatibility

| Protocol Feature | Required Protocol Version | Notes |
|------------------|--------------------------|-------|
| NODE_WITNESS service flag | 70013+ | Introduced with BIP-144 / SegWit activation (2016) |
| INV_WITNESS_BLOCK inventory type | 70013+ | Same |
| Compact blocks (BIP-152) | 70014+ | NOT implementing; listed for awareness |
| `sendcmpct` message | 70014+ | NOT implementing |
| Taproot consensus rules | Any (soft fork at height 709632) | No protocol version change; validated by all nodes |
| BIP-125 RBF | Policy only — not a protocol version | No version message signaling required |

---

## Sources

- [BIP-340: Schnorr Signatures](https://github.com/bitcoin/bips/blob/master/bip-0340.mediawiki) — Tagged hash construction, 64-byte sig format — HIGH confidence
- [BIP-341: Taproot](https://github.com/bitcoin/bips/blob/master/bip-0341.mediawiki) — TapTweak formula, keypath vs scriptpath — HIGH confidence
- [BIP-342: Tapscript](https://github.com/bitcoin/bips/blob/master/bip-0342.mediawiki) — OP_CHECKSIGADD semantics, OP_SUCCESS opcodes, annex — HIGH confidence
- [BIP-144: SegWit P2P](https://github.com/bitcoin/bips/blob/master/bip-0144.mediawiki) — NODE_WITNESS = (1<<3), MSG_WITNESS_BLOCK = 0x40000002 — HIGH confidence
- [BIP-125: RBF](https://github.com/bitcoin/bips/blob/master/bip-0125.mediawiki) — All 5 replacement rules — HIGH confidence
- [BIP-22: getblocktemplate](https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki) — Template fields, coinbase structure — HIGH confidence
- [BIP-145: getblocktemplate SegWit](https://bips.dev/145/) — Witness commitment requirement — HIGH confidence
- [developer.bitcoin.org P2P reference](https://developer.bitcoin.org/reference/p2p_networking.html) — Service flags, message types — HIGH confidence
- [Bitcoin Core net_processing.cpp](https://github.com/bitcoin/bitcoin/blob/master/src/net_processing.cpp) — Block serving flow (ProcessGetBlockData) — HIGH confidence
- [Bitcoin Core interpreter.cpp](https://github.com/bitcoin/bitcoin/blob/master/src/script/interpreter.cpp) — OP_CHECKSIGADD implementation — HIGH confidence
- [Bitcoin Core txindex.cpp](https://github.com/bitcoin/bitcoin/blob/master/src/index/txindex.cpp) — CDiskTxPos structure, index write pattern — HIGH confidence
- [Bitcoin Core miner.cpp](https://github.com/bitcoin/bitcoin/blob/master/src/node/miner.cpp) — Block template construction algorithm — HIGH confidence
- [secp256k1_schnorrsig.h](https://github.com/bitcoin-core/secp256k1/blob/master/include/secp256k1_schnorrsig.h) — No batch verify in public API confirmed — HIGH confidence
- [Bitcoin Wiki Data Storage](https://en.bitcoin.it/wiki/Bitcoin_Core_0.11_(ch_2):_Data_Storage) — Undo data format (rev*.dat) — MEDIUM confidence (older doc, structure unchanged)
- [Bitcoin Optech: Compact Blocks](https://bitcoinops.org/en/topics/compact-block-relay/) — Protocol version requirements for BIP-152 — MEDIUM confidence

---
*Stack research for: Bitcoin full node peer compatibility*
*Researched: 2026-02-20*
