# Pitfalls Research

**Domain:** Bitcoin full node peer compatibility — C11 implementation
**Researched:** 2026-02-20
**Confidence:** HIGH (pitfalls drawn from actual codebase audit, BIP specifications, Bitcoin Core disclosures, and protocol documentation)

---

## Critical Pitfalls

### Pitfall 1: Reorg Notifies Without Undoing Chainstate

**What goes wrong:**
The reorg path in `chaser_confirm.c:250` emits `CHASE_REORGANIZED` events and resets `confirmed_height` to the fork point, but never invokes the UTXO delta reversal machinery in `chainstate.c`. The in-memory UTXO set and on-disk SQLite UTXO table retain the spent/created state from the disconnected blocks. Any subsequent block application on the new fork will attempt to spend UTXOs that never existed on that fork, triggering a consensus invariant panic (or worse: silently accepting invalid state).

**Why it happens:**
The delta reversal code (`chainstate_revert_block`) is fully implemented but never called from the confirm chaser. The TODO at line 250 deferred the wiring. Without an actual mainnet reorg during development, the code path is never exercised.

**How to avoid:**
1. Call `chainstate_revert_block(state, delta)` for each height from `old_height` down to `fork_point + 1` inside the reorg loop — in reverse order.
2. The deltas ring buffer holds 550 blocks. Enforce that reorgs deeper than 550 blocks abort (they cannot be handled safely without full re-validation from genesis).
3. After reverting all UTXO deltas, verify the resulting UTXO set tip hash matches the expected fork point hash before proceeding to apply the new fork.
4. Chainwork must also be reverted. Store `prev_chainwork` in each `block_delta_t` at apply time so the revert path can restore it without recomputation.

**Warning signs:**
- Log messages showing `CHASE_REORGANIZED` but UTXO count not decreasing
- Subsequent block application fails with "UTXO already spent" or "UTXO not found" after a reorg
- Chainwork value remains unchanged after a reorg that should reduce it

**Phase to address:** Reorg handling phase (before any mainnet steady-state work)

---

### Pitfall 2: Chainwork Stored Little-Endian Breaks Fork Selection

**What goes wrong:**
SQLite's `ORDER BY chainwork DESC` on a BLOB column performs byte-by-byte lexicographic comparison. If chainwork is stored in little-endian format (least-significant byte first), then a chain with higher cumulative work may sort lower than a chain with less work, causing the node to follow the wrong fork. The code acknowledges this: `block_index_db.c:155` uses `ORDER BY height DESC` as a workaround that only holds during linear IBD.

**Why it happens:**
Bitcoin's internal chainwork representation is a 256-bit big-endian integer (matching the wire format of nBits difficulty calculation). If the implementation stores it in native machine byte order or little-endian for any reason, SQLite cannot compare it correctly. During IBD on the main chain, height and chainwork are monotonically correlated, hiding the bug. Fork selection is the only code path that exercises it.

**How to avoid:**
1. Store chainwork as a 32-byte big-endian blob unconditionally. Convert at store time if the internal representation is little-endian.
2. Switch the `best_chain_stmt` query to `ORDER BY chainwork DESC LIMIT 1`.
3. Test with a synthetic fork: inject two block index entries with the same height but different chainwork, verify the query returns the one with higher work.

**Warning signs:**
- `best_chain_stmt` query uses `ORDER BY height` instead of `ORDER BY chainwork`
- Node selects lower-work fork after a reorg
- Chainwork values look correct in logs but fork selection picks wrong tip

**Phase to address:** Chainwork / reorg phase — fix storage format before implementing reorg, otherwise the reorg test vectors will mask the ordering bug

---

### Pitfall 3: OP_CHECKSIGADD Must Not Fail on Unknown Key Types

**What goes wrong:**
BIP-342 defines a strict upgrade mechanism: public keys that are not 32 bytes (x-only) are "unknown key types" and signatures against them must be treated as valid (no script failure). Implementing OP_CHECKSIGADD as "verify sig if key is 32 bytes, fail otherwise" will cause the node to reject valid Taproot transactions that use future key types, creating a consensus split from Bitcoin Core.

The BIP-342 rules for OP_CHECKSIGADD are:
- If fewer than 3 stack elements: script fails immediately
- If `n` (the accumulator) is larger than 4 bytes: script fails immediately
- If the key is 32 bytes: verify the Schnorr signature using `secp256k1_schnorrsig_verify`
  - Empty sig: push `n`, continue
  - Valid sig: push `n + 1`, continue
  - Invalid sig: script fails immediately
- If the key is empty (0 bytes): script fails immediately
- If the key is any other size (not 0, not 32): treat as unknown, push `n + 1`, continue

**Why it happens:**
Developers conflate "unknown key type" with "invalid key." BIP-341 defines this softfork-compatibility rule precisely to allow future key type upgrades without hardforks. Missing it means the node fails on transactions that Bitcoin Core accepts.

**How to avoid:**
1. The vendored secp256k1 (`secp256k1_schnorrsig.h`) already provides `secp256k1_schnorrsig_verify` — the infrastructure exists.
2. Implement OP_CHECKSIGADD in `script.c:3331` following the exact BIP-342 opcode table:
   - Empty sig handling: do not call verify, just push `n`
   - Validate `n` fits in CScriptNum (4 bytes)
   - Non-32-byte, non-empty key: push `n + 1` (success — unknown key type)
3. Test against BIP-342 reference test vectors before considering it complete.
4. Track sigops budget: each non-empty signature verification decrements the per-script budget by 50 weight units.

**Warning signs:**
- Taproot script-path spends that use OP_CHECKSIGADD all fail validation
- Test vectors from BIP-342 Appendix fail
- Node diverges from Bitcoin Core when processing Taproot blocks (blocks 709,632+)

**Phase to address:** Tapscript validation phase

---

### Pitfall 4: BIP-125 RBF Rule #3 Requires Absolute Fee, Not Just Feerate

**What goes wrong:**
A common RBF implementation mistake is checking only that the replacement has a higher feerate (sats/vbyte) than the original. BIP-125 Rule #3 requires the replacement pay a higher **absolute fee** than the sum of all original transactions being replaced, including their descendants. A transaction with a higher feerate but lower absolute fee is a valid RBF attack vector that enables free bandwidth exhaustion.

The second common mistake is Rule #5: the total count of evicted transactions (originals + all their descendants) must not exceed 100. Failing to walk the descendant graph before accepting the replacement allows attackers to pin large transaction packages by pre-loading many descendants.

**Why it happens:**
Rule #3 and Rule #4 (minimum relay feerate) are easy to conflate. Rule #5 requires a descendant traversal that feels like unnecessary overhead. Both are explicitly anti-DoS protections; skipping them creates exploitable attack surfaces.

**How to avoid:**
1. Implement all 5 BIP-125 rules in order at `mempool.c:799`:
   - Rule 1: At least one input signals replaceability (nSequence < 0xFFFFFFFE)
   - Rule 2: Replacement may not include new unconfirmed inputs
   - Rule 3: Replacement absolute fee >= sum of replaced transaction fees
   - Rule 4: Replacement feerate >= node's minimum relay feerate setting
   - Rule 5: Eviction count (originals + descendants) <= 100
2. Walk the descendant graph to compute Rule 3 aggregate fee and Rule 5 eviction count before accepting.
3. Test with an attacker scenario: original tx pays 1000 sats on 500 vbytes (2 sat/vb), replacement pays 3000 sats on 2000 vbytes (1.5 sat/vb) — higher absolute fee, lower rate. Rule 3 passes, Rule 4 fails: this is the correct outcome.

**Warning signs:**
- Replacement accepted with lower absolute fee than original
- Mempool descendant count not checked before eviction
- `conflicts_count` always 1 regardless of descendant tree size
- Rule 2 not implemented (replacement introduces new unconfirmed inputs)

**Phase to address:** RBF / mempool phase

---

### Pitfall 5: Block Serving Without NODE_WITNESS Drops Witness Data

**What goes wrong:**
If a node serves blocks in response to `getdata` with `MSG_BLOCK` (not `MSG_WITNESS_BLOCK`) to a peer that requested witness data, or if the node does not advertise `NODE_WITNESS` in its version message, connected peers will not request witness blocks from it. Peers that need to validate Taproot and SegWit transactions will receive stripped blocks (without witness fields), which they must reject under their own validation rules. The serving node appears broken to the network.

The specific failure: `INV_BLOCK` (0x00000002) requests a non-witness block. `INV_WITNESS_BLOCK` (0x40000002, 30th bit set) requests the witness-serialized block. A node that only responds to `INV_BLOCK` cannot serve SegWit blocks correctly to modern peers.

**Why it happens:**
NODE_WITNESS (service bit 3) and INV_WITNESS_BLOCK inventory type are two separate mechanisms that must both be implemented. Implementing block serving without both leaves the node unable to participate in modern block relay.

**How to avoid:**
1. Add `NODE_WITNESS` (bit 3, value 8) to the `nServices` field in the version message.
2. When handling `getdata`, check the inventory type:
   - `INV_WITNESS_BLOCK`: serialize with witness fields (`serialize_block_witness()`)
   - `INV_BLOCK`: serialize without witness fields (`serialize_block_legacy()`)
3. When announcing blocks via `inv`, use `INV_WITNESS_BLOCK` when the connection has negotiated witness support.
4. Test by connecting to Bitcoin Core and verifying it fetches blocks from the echo node with witness data intact.

**Warning signs:**
- Bitcoin Core logs "peer does not have NODE_WITNESS" when connecting to echo
- Peers disconnect after receiving blocks without witness data
- SegWit transactions appear in blocks but witness fields are stripped
- Node does not appear in Bitcoin Core's `getpeerinfo` service flags as supporting witness

**Phase to address:** Block serving phase

---

### Pitfall 6: Async Storage Race: Download Manager Marks Block Received Before Disk Write

**What goes wrong:**
The current async storage code is disabled (`if (false && ...`) precisely because of this race: the download manager marks a block slot as "received" when the storage queue enqueues it, not when the storage thread writes it to disk. If the download manager completes a batch and signals the chaser before all blocks in the batch are durably written, the chaser tries to load and apply blocks that haven't landed on disk yet — producing "GAP" errors and stalling IBD indefinitely.

**Why it happens:**
The natural implementation has the event loop enqueue blocks for async I/O and immediately mark them as done to keep the pipeline moving. But the completion signal (block available for validation) must be gated on the storage thread's write confirmation, not on enqueue. Without a callback mechanism, the two systems are decoupled in the wrong direction.

**How to avoid:**
1. Implement a storage completion callback: when the storage thread finishes a write, it invokes a callback that marks the block slot as durably written in the download manager.
2. The callback must be safe to call from the storage thread — use a mutex-protected counter or a lock-free flag per block slot.
3. The download manager batch completion check must consult the "durably written" flags, not just the "received from peer" flags.
4. Never re-enable the `if (false && ...)` async path without this callback in place.

**Warning signs:**
- Log shows "GAP: block at height X not yet stored" during IBD
- Batch completion fires but chaser cannot load blocks
- Storage queue depth grows without bound under I/O pressure
- Removing the `if (false && ...)` guard causes IBD to stall

**Phase to address:** Async I/O / storage phase — implement callback first, then enable async path

---

### Pitfall 7: Chainwork Revert Leaves Tip With Stale Accumulated Work

**What goes wrong:**
`chainstate_revert_block()` in `chainstate.c:739` has a TODO that explicitly leaves chainwork unchanged after reverting a block. The comment acknowledges two possible fixes: store `prev_chainwork` in the delta or recompute from the height index. Neither is implemented. After any reorg, `state->tip.chainwork` reflects the chainwork of the old tip, not the reverted fork point. Fork selection will then incorrectly compare the stale (too-high) chainwork against incoming blocks, potentially:
- Ignoring a valid longer chain as "not better"
- Accepting a shorter chain as the new tip if work comparison succeeds only due to the stale value

**Why it happens:**
Chainwork is accumulated (added) when applying blocks and must be subtracted (or reset to a snapshot) when reverting. Without per-block work stored in the delta, recomputing requires iterating the height index, which was deemed complex enough to defer.

**How to avoid:**
1. The simplest fix: add a `work256_t prev_chainwork` field to `block_delta_t` and populate it when calling `chainstate_apply_block()`. During revert, restore it directly.
2. This field is 32 bytes per delta entry × 550 deltas = 17.6 KB — negligible.
3. Update `work256_zero()`, `work256_add()`, and the delta allocation code to include this field.
4. After revert, assert that `state->tip.chainwork` equals the stored `prev_chainwork` from the delta that was just reverted.

**Warning signs:**
- After a reorg, chainwork in logs is higher than expected for the fork point height
- `chainstate_get_tip()` returns correct height but implausibly high chainwork
- Node accepts a lower-work chain as the new best tip after a reorg

**Phase to address:** Chainwork / reorg phase — fix simultaneously with Pitfall 1 and Pitfall 2

---

## Moderate Pitfalls

### Pitfall 8: Download Manager Batch Remaining Count Can Undercount

**What goes wrong:**
`download_mgr.c:597-601` documents an active bug: when duplicate blocks arrive from reassigned batches or batch theft, the `remaining` counter can reach 0 while the block bitmap still shows unreceived slots. The code detects and corrects this at runtime, logging `LOG_ERROR`. The risk is that the correction may not fire in all paths, or future modifications break the correction logic, causing batches to complete prematurely and leaving gaps in the chain.

**Prevention:**
1. Fix the root cause: clear the duplicate-tracking bitmap when a batch is stolen or reassigned. Never allow a peer to "count" a block from a batch it no longer owns.
2. Add an assertion before marking a batch complete: `assert(all_bits_set(batch->received_bitmap))`. This catches the inconsistency immediately rather than through a LOG_ERROR correction.
3. Test the batch theft path explicitly: assign batch to peer A, steal and reassign to peer B, send blocks from both peers in random order, verify no gaps result.

---

### Pitfall 9: Peer Duplicate Address Race in Connection Setup

**What goes wrong:**
`node.c:3280` logs "BUG: Duplicate address..." when the connection loop adds a peer before verifying the address is not already connected. On mainnet, well-connected peers can advertise many addresses overlapping with existing connections, triggering this bug at scale.

**Prevention:**
1. Check for duplicate address in the outbound connection logic *before* calling `connect()`, not after.
2. Hold the peer list lock during the check-and-add to prevent TOCTOU races in any future multi-threaded connection path.
3. Add a hash set of connected addresses (IP:port) for O(1) duplicate detection rather than the current O(n) scan.

---

### Pitfall 10: Checkpoint Value Hardcoded to 0 Disables Validation Bypass

**What goes wrong:**
`chaser_validate.c:178` and `chaser_confirm.c:66` initialize `top_checkpoint` to 0. The checkpoint bypass is intended to skip full script validation for historically confirmed blocks below a trusted height, dramatically speeding up IBD. With 0 as the checkpoint, every block from genesis is fully script-validated — correct but unnecessarily slow.

**Prevention:**
Read `top_checkpoint` from the config file at startup. Use Bitcoin Core's current checkpoint set as the default: the highest checkpoint is block 295000 (hash known). Add a config key `top_checkpoint_height` with sane default.

---

### Pitfall 11: Block Hash All-Zeros During Validation Corrupts Tracking

**What goes wrong:**
`chaser_validate.c:517` submits an all-zero hash because the block index query is not performed. The validated block's hash cannot be correlated with network peer inventory, block index entries, or block storage positions. This means the validation pipeline cannot definitively link a validated block back to its known header, preventing correct chain tip updates and block serving lookups.

**Prevention:**
Perform a block index lookup by height to retrieve the expected hash before submitting to validation. Pass the retrieved hash through the validation pipeline. Add an assertion that the hash of the validated block matches the index entry.

---

### Pitfall 12: Mutated Blocks Can Poison Download State of Other Peers

**What goes wrong:**
CVE-2024-52921 (Bitcoin Core versions before v25.0): a peer sending a mutated (invalid but parseable) block could clear the compact block download state of other peers who had announced the same block. A single adversarial peer could stall block propagation for all connections. The root cause: block download state was shared globally rather than scoped per-connection.

**Prevention:**
When implementing block serving, ensure that a peer's `getdata` response (valid or invalid) can only affect the download state of that specific peer's connection, never the state of other connections downloading the same block. Index download state by (peer_id, block_hash), not by block_hash alone.

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Synchronous block writes (current) | Correct behavior, no GAP errors | I/O blocks validation thread; IBD is slower | Acceptable until callback system is built |
| Hardcoded `top_checkpoint = 0` | Always correct (full validation) | IBD 3-5x slower than with checkpoints | Acceptable in early milestone; fix before release |
| `ORDER BY height` instead of `ORDER BY chainwork` | Works on linear IBD | Wrong fork selected under complex reorg | Never acceptable on mainnet; fix before reorg work |
| Rejecting all RBF conflicts | Prevents invalid replacements | Legitimate fee bumps rejected; mempool unusable for LN users | Acceptable until BIP-125 fully implemented |
| `if (false && ...)` disabling async path | Correctness preserved | Performance left on table | Acceptable until storage callback is implemented |
| Empty block hash in validation | Compiles and runs | Cannot correlate validated blocks with index entries | Never acceptable once block serving is active |

---

## Integration Gotchas

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| secp256k1 Schnorr verify | Passing ECDSA hash to Schnorr verifier | Tapscript uses tagged hash: `BIP0340/challenge` tag via `secp256k1_tagged_sha256` |
| secp256k1 x-only pubkey | Passing compressed 33-byte key to `secp256k1_xonly_pubkey_parse` | Must pass 32-byte x-coordinate only; strip the parity prefix byte |
| SQLite WAL mode + concurrent reads | Readers block on checkpoint flush | Set `PRAGMA wal_autocheckpoint = 1000`; never hold write transaction across reads |
| Block file mutex + fflush | Reading write file without flushing first | Always call `fflush(mgr->current_file)` before any read from the same file number |
| Bitcoin P2P `version` message | Sending wrong service flags | `nServices` must include `NODE_NETWORK` (1) + `NODE_WITNESS` (8) = 9 for full witness node |
| `getdata` MSG_BLOCK vs MSG_WITNESS_BLOCK | Serving legacy block to witness-capable peer | Check `inv->type & (1 << 30)` to distinguish; serve witness serialization for witness requests |

---

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| Synchronous block writes | IBD speed limited by disk I/O, not network | Implement async storage with completion callbacks | At any block rate exceeding disk write throughput |
| Linear search in `chainstate_is_on_main_chain` | Reorg detection slows quadratically | Use the hash table (`height_index`) for O(1) lookups | Above 50k blocks on slow hardware |
| SQLite UTXO writes not batched | Each block application issues many individual SQL statements | Use explicit `BEGIN TRANSACTION ... COMMIT` per block | At block throughput > 10 blocks/sec |
| Block file read cache overflow | LRU eviction causes handle close/reopen on hot blocks | Size `BLOCK_READ_CACHE_SIZE` to cover working set | During block serving with many concurrent requesters |
| Mempool descendant graph walk (RBF) | O(n) per replacement attempt where n = descendants | Cap maximum ancestor/descendant depth before walking | With deep mempool chains (e.g., chains of 25 txs) |

---

## Security Mistakes

| Mistake | Risk | Prevention |
|---------|------|------------|
| Not validating `n` size in OP_CHECKSIGADD | Script can push 5-byte CScriptNum, bypassing arithmetic bounds | Reject immediately if `n` > 4 bytes per BIP-342 |
| Serving blocks without checking pruning status | Serving pruned block data (garbage bytes) to peer | Check block index `status` flag for `BLOCK_HAVE_DATA` before serving |
| Not rate-limiting block `getdata` responses | Peer requests 10,000 blocks; I/O exhaustion and connection stall | Implement per-peer block serving rate limit (e.g., 16 blocks/second) |
| UTXO rollback without atomicity | Partial reorg leaves UTXO set in inconsistent state | Wrap all delta reversals in a single SQLite transaction; rollback on any error |
| Accepting replacement with new unconfirmed inputs (RBF Rule 2) | Allows pinning attacks via unconfirmed dependency chains | Check that all inputs of the replacement were present in the original transaction |
| Segfault on malformed large block | 4x max block size allocation without bounds | Assert allocation size <= `4 * ECHO_MAX_BLOCK_SIZE`; return parse error on overflow |

---

## "Looks Done But Isn't" Checklist

- [ ] **Reorg handling:** `chaser_confirm.c` emits `CHASE_REORGANIZED` — verify `chainstate_revert_block` is called for each reverted height, not just height notification
- [ ] **Chainwork storage:** Block index query `ORDER BY chainwork DESC` works — verify chainwork bytes are stored big-endian, not little-endian
- [ ] **OP_CHECKSIGADD:** Returns non-error — verify unknown key types (non-32-byte) push `n+1` and continue rather than failing
- [ ] **RBF Rule #3:** Replacement accepted — verify absolute fee sum was computed across all replaced transactions including descendants, not just feerate comparison
- [ ] **Block serving:** `getdata` handler responds — verify `NODE_WITNESS` is in service flags and `INV_WITNESS_BLOCK` is used for witness serialization
- [ ] **Async storage:** Storage callback fires — verify download manager consults "durably written" flag, not "enqueued" flag
- [ ] **Block hash in validation:** Validation succeeds — verify submitted hash is the actual block hash from index, not all-zeros

---

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| Reorg without UTXO undo (corrupted chainstate) | HIGH | Full resync from genesis: `rm -rf ~/.bitcoin-echo && ./echo --prune=1024` |
| Wrong chainwork storage format (wrong fork selected) | HIGH | Migrate block index: re-scan all headers, rewrite chainwork column in big-endian |
| OP_CHECKSIGADD missing unknown key handling | MEDIUM | Implement correction, resync from block 709,632 (Taproot activation) |
| RBF missing Rule #3 (DoS exposure) | LOW | Patch and restart; mempool is ephemeral, no persistent state corruption |
| Block serving without NODE_WITNESS | LOW | Add service flag, restart; peers reconnect and renegotiate |
| Async storage race (GAP errors, IBD stall) | MEDIUM | Disable async path, restart with synchronous writes as current fallback |

---

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| Reorg without UTXO undo | Reorg + chainstate rollback phase | Test deep reorg (10+ blocks): UTXO count matches expected before/after |
| Chainwork little-endian storage | Chainwork fix phase (prerequisite to reorg) | Inject two index entries with same height, different work; verify query selects higher-work entry |
| Chainwork not reverted on reorg | Reorg + chainstate rollback phase | After reorg, assert tip chainwork matches expected for fork point height |
| OP_CHECKSIGADD incomplete | Tapscript validation phase | Run all BIP-342 test vectors; include unknown-key-type vectors |
| BIP-125 RBF incomplete | Mempool / RBF phase | Test Rule #3 (higher absolute fee required), Rule #5 (100 tx eviction limit) |
| Block serving without NODE_WITNESS | Block serving phase | Bitcoin Core connects, requests block, receives witness-serialized data |
| Async storage race (GAP errors) | Async I/O phase | Run IBD with async path enabled; zero GAP errors over 100k blocks |
| Batch remaining count mismatch | Download manager bug fix phase | Simulate batch theft under load; assert no premature batch completion |
| Duplicate peer address race | Peer management bug fix phase | Connect 50+ peers simultaneously; assert no duplicate address log errors |
| Mutated block poisoning download state | Block serving phase | Verify block download state is indexed per-peer, not globally |

---

## Sources

- Bitcoin Echo `CONCERNS.md` audit (2026-02-20) — primary source for all pitfall root causes
- [BIP-342: Validation of Taproot Scripts](https://bips.dev/342/) — OP_CHECKSIGADD exact rules (HIGH confidence)
- [BIP-125: Opt-in Full Replace-by-Fee Signaling](https://bips.dev/125/) — 5 RBF rules (HIGH confidence)
- [BIP-340: Schnorr Signatures for secp256k1](https://bips.dev/340/) — Schnorr implementation pitfalls (HIGH confidence)
- [CVE-2024-52921: Mutated blocks hindering propagation](https://bitcoincore.org/en/2024/10/08/disclose-mutated-blocks-hindering-propagation/) — block download state isolation (HIGH confidence)
- [Bitcoin Core PR Review Club #21090: Default to NODE_WITNESS](https://bitcoincore.reviews/21090) — NODE_WITNESS requirements (MEDIUM confidence)
- [Bitcoin P2P Network Reference](https://developer.bitcoin.org/reference/p2p_networking.html) — MSG_WITNESS_BLOCK vs MSG_BLOCK (MEDIUM confidence)
- [Bitcoin Core 0.11 Data Storage](https://en.bitcoin.it/wiki/Bitcoin_Core_0.11_(ch_2):_Data_Storage) — undo file pattern for reorg (MEDIUM confidence)
- [Transaction Pinning: Bitcoin Optech](https://bitcoinops.org/en/topics/transaction-pinning/) — RBF Rule #3 and #5 pinning attacks (MEDIUM confidence)
- [One-Shot Replace-by-Fee-Rate: Peter Todd 2024](https://petertodd.org/2024/one-shot-replace-by-fee-rate) — current RBF ecosystem state (MEDIUM confidence)
- `lib/secp256k1/include/secp256k1_schnorrsig.h` (vendored) — `secp256k1_schnorrsig_verify` API confirmation (HIGH confidence)
- `src/consensus/sig_verify.c` — existing Schnorr verification infrastructure (HIGH confidence, direct codebase read)

---
*Pitfalls research for: Bitcoin full node peer compatibility (C11)*
*Researched: 2026-02-20*
