# Feature Research

**Domain:** Bitcoin full node — peer compatibility milestone
**Researched:** 2026-02-20
**Confidence:** HIGH (grounded in BIPs, Bitcoin Core source, official developer docs)

---

## Feature Landscape

### Table Stakes (Peers Disconnect / Ignore You Without These)

These are not optional. The Bitcoin P2P network enforces these implicitly: peers that cannot serve data are deprioritized, eventually evicted, or fail to be useful to the network. A node missing any of these is technically "on the network" but not a real participant.

| Feature | Why Required | Complexity | Notes |
|---------|-------------|------------|-------|
| **Block serving (getdata/getblocks)** | Peers disconnect after repeated failed block requests; a node that only downloads but never serves is a parasite on the network | HIGH | Requires block retrieval from blk*.dat storage, dispatch via P2P getdata handler. Already stubbed at `src/app/node.c:2769`. |
| **NODE_WITNESS service flag** | Bitcoin Core refuses to download blocks from non-witness peers after SegWit activation (2017). A node without this flag will not receive witness data and cannot validate SegWit transactions. | LOW | Set `NODE_WITNESS = (1 << 3)` in version message. Use `INV_WITNESS_BLOCK` for block inventory. Located at `src/app/node.c:1762`. |
| **OP_CHECKSIGADD (BIP-342 Tapscript)** | Taproot is 15-20% of all mainnet transactions (peaked 40%+ in 2024). Without OP_CHECKSIGADD, the node rejects valid Taproot multisig transactions and diverges from consensus. | HIGH | Current behavior: returns `SCRIPT_ERR_BAD_OPCODE`. Requires Schnorr batch verification via vendored libsecp256k1. Located at `src/consensus/script.c:3331`. |
| **Chainstate UTXO rollback for reorgs** | Without undo data, the node cannot follow the longest chain when a fork occurs. The UTXO set becomes permanently inconsistent after any reorg. This is a consensus correctness issue, not a polish item. | HIGH | Undo data (rev*.dat) stores CTxOut objects that were spent, allowing UTXO set to revert. Partially designed at `src/consensus/chainstate.c:736-741`. Must complete delta system + apply on disconnect. |
| **Chainwork big-endian storage** | Fork selection compares chainwork byte-by-byte in SQLite. Little-endian values produce wrong chain selection on complex reorgs. Silently selects wrong chain tip. | MEDIUM | Fix in `src/storage/block_index_db.c:155`. Update all comparison logic. |
| **Chainwork recomputation on reorg** | Without correct chainwork after reorg, the node cannot accurately select the best chain. May follow a lower-work chain. | MEDIUM | Fix in `src/consensus/chainstate.c:739`. Either store previous chainwork snapshot or trigger full recomputation. |

---

### Differentiators (Competitive Advantage Over Other Implementations)

These features set Bitcoin Echo apart. Not enforced by peers, but required for the node to be useful to miners, developers, and power users, or to perform significantly better than minimal compliance.

| Feature | Value Proposition | Complexity | Notes |
|---------|------------------|------------|-------|
| **BIP-125 RBF (full-RBF)** | Bitcoin Core made full-RBF the default in 2024 (PR #30493) and removed the opt-in toggle (PR #30592). A node without RBF cannot accept legitimate fee-bump transactions, making its mempool diverge from the network. Miners using this node's mempool miss fee-paying replacements. | MEDIUM | BIP-125 rules: check signaling (nSequence < 0xFFFFFFFE), verify absolute fee increase, check descendant limit (100 txns), verify feerate superiority. Located at `src/protocol/mempool.c:799`. |
| **getblocktemplate (BIP-22/23)** | Mining pools cannot use this node without getblocktemplate. This is the only path for a node to contribute to block production. Stubbed out currently. | HIGH | Requires: assembled block from mempool ordered by feerate, correct coinbase tx structure, witness commitment, proper sigops accounting. High value for mining integration. |
| **Transaction index (txindex)** | Without txindex, `getrawtransaction` only works for mempool transactions. Any RPC client querying historical transactions fails. Standard expectation for any node that exposes an RPC. | MEDIUM | Hash → (block file, offset) map. Write during block storage. Query during RPC dispatch. Located at `src/app/rpc.c:1897`. |
| **RPC getblock verbosity=0 (raw hex)** | Standard RPC method. Any tooling that calls `getblock` with verbosity=0 (the default for many libraries) gets an error. Required for block explorers and debugging workflows. | LOW | Serialize stored block bytes as hex. Located at `src/app/rpc.c:1783`. |
| **RPC getblockchaininfo mediantime** | Median Time Past (MTP) is consensus-critical for CLTV/CSV time locks. Always returning 0 is a correctness bug for any application checking time-locked transactions. | LOW | Query last 11 block timestamps from block index, return median. Located at `src/app/rpc.c:1662`. |
| **Async storage callbacks (decouple I/O from validation)** | Synchronous block writes during IBD artificially throttle validation throughput. The GAP error workaround burns CPU polling. Proper async callbacks are the correct architecture. | MEDIUM | Implement completion callback that fires after storage thread confirms disk write. Located at `src/app/node.c:1509`. Critical for IBD performance. |
| **Checkpoint configuration** | Hardcoded `top_checkpoint = 0` means all blocks are fully validated during IBD including pre-checkpoint blocks. Configurable checkpoints allow safe IBD speedup for known-good history. | LOW | Read from config file. Integrate with `chaser_validate.c:178` and `chaser_confirm.c:66`. |

---

### Anti-Features (Deliberately Excluded from This Milestone)

These features are commonly requested or might seem like natural inclusions, but they belong in later milestones or are permanently out of scope.

| Feature | Why Requested | Why to Exclude | Alternative |
|---------|--------------|----------------|-------------|
| **BIP-324 v2 encrypted transport** | Bitcoin Core v27+ enables v2 by default; looks like a peer compatibility requirement | v2 falls back to v1 gracefully. Non-v2 nodes receive inbound v1 connections normally. No peer disconnects for lacking v2. Implementing ChaCha20Poly1305 + Elligator Swift is a significant pure-crypto project with no peer compatibility consequence right now. | Implement in a dedicated cryptography milestone after peer compat is solid. |
| **BIP-152 compact block relay** | Reduces block propagation latency; used by all Bitcoin Core nodes | Compact blocks are an optimization, not a correctness requirement. Nodes without it fall back to full block relay (MSG_BLOCK instead of MSG_CMPCT_BLOCK). It also requires a working mempool and block serving first — which are table stakes items being done now. | Implement after block serving is stable. Compact blocks depend on block serving. |
| **BIP-157/158 compact block filters (NODE_COMPACT_FILTERS)** | Light wallets use this to query chain without full IBD | Completely orthogonal to peer compatibility. No peers expect it. Only light clients query for it. Significant implementation scope (filter computation for every block). | Defer to a dedicated light client support milestone. |
| **Wallet / key management** | Users want to send transactions | Explicitly out of scope per project manifesto. "Users bring external signer." Adding wallet code couples consensus logic to key material — exactly what the manifesto opposes. | Document external signer workflow (e.g., HWI, coldcard). |
| **P2P fee estimation** | Useful for users constructing transactions | Not a peer compatibility requirement. No peer disconnects a node for lacking it. Complex to implement correctly (requires long mempool history tracking). | Defer to RPC polish milestone. |
| **Mempool eviction tuning (SLOWEST_EVICTION_MIN_RATE)** | Affects IBD peer efficiency | The threshold is a tuning parameter, not a feature. Setting it requires real IBD measurement data, not code changes. | Collect IBD statistics during this milestone; tune in the next. |
| **GUI / frontend work** | Better UX for node operators | Separate repo (bitcoinecho-gui), separate milestone, separate concern. P2P compatibility is independent of GUI. | Track in bitcoinecho-gui milestone. |

---

## Feature Dependencies

```
[Chainstate UTXO Rollback]
    requires --> [Chainwork Big-Endian Storage]
    requires --> [Chainwork Recomputation on Reorg]

[Block Serving (getdata/getblocks)]
    required-by --> [BIP-152 Compact Blocks]  (future milestone)
    required-by --> [getblocktemplate]         (needs blocks in storage)

[NODE_WITNESS service flag]
    enables --> [OP_CHECKSIGADD / Tapscript]  (peers send witness data only to witness nodes)

[Transaction Index (txindex)]
    required-by --> [RPC getrawtransaction (confirmed)]
    required-by --> [getblocktemplate]         (needs to know which txs are already confirmed)

[Async Storage Callbacks]
    enhances --> [Block Serving]               (blocks must be durably written before serving)
    enhances --> [IBD throughput]              (decouples pipeline stages)

[OP_CHECKSIGADD]
    requires --> [NODE_WITNESS]                (witness data must arrive to validate Taproot)
    depends-on --> [libsecp256k1 batch verify] (vendored; check API support first)

[BIP-125 RBF]
    standalone, but enhances --> [getblocktemplate]  (mempool with RBF reflects real fee market)

[RPC getblock verbosity=0]
    requires --> [Block Serving infrastructure] (same storage read path)

[RPC getblockchaininfo mediantime]
    requires --> [Block index with timestamps]  (already exists; just needs query)
```

### Dependency Notes

- **Block serving requires undo data infrastructure to be correct:** If a node serves a block on a fork that later gets reorged, the node's chainstate may be incorrect. Reorg correctness and block serving should be completed in tandem.
- **NODE_WITNESS must precede Tapscript validation:** Peers will not send witness data to nodes without NODE_WITNESS. OP_CHECKSIGADD cannot be tested end-to-end without witness data arriving.
- **txindex is independent of block serving:** The index writes during ingestion (validation pipeline) and reads during RPC. Block serving reads the same blk*.dat files but via a different path.
- **Async storage callbacks enhance block serving:** Blocks must be durably written to disk before being served. Without confirmed write callbacks, serving freshly validated blocks risks serving unwritten data.
- **getblocktemplate is the most complex feature:** It touches mempool (RBF), block structure, coinbase construction, witness commitment (BIP-141), and the block index. It should be implemented last in this milestone, after mempool and block serving are stable.

---

## MVP Definition

### Peer Compatible (v1 — This Milestone)

Minimum required to be a genuine network participant that peers trust.

- [ ] **NODE_WITNESS service flag** — Peers stop sending witness data without this. Prerequisite for Taproot validation.
- [ ] **OP_CHECKSIGADD (BIP-342)** — ~15-20% of mainnet transactions use Taproot. Without this, the node diverges from consensus on valid blocks.
- [ ] **Chainstate UTXO rollback** — Consensus correctness. Any reorg (which mainnet has regularly) corrupts chainstate without this.
- [ ] **Chainwork big-endian + recomputation** — Required for correct fork selection. Silent bug without this.
- [ ] **Block serving (getdata/getblocks)** — A node that only downloads is not a peer; it is a leech. Required to be a real network participant.
- [ ] **Download manager batch count bug fix** — Active bug causing LOG_ERROR in production during IBD.
- [ ] **Duplicate address detection fix** — Active bug causing LOG_ERROR warnings in production.
- [ ] **Block hash in chaser validation** — Submitting all-zeros hash breaks validation correlation and integrity checking.
- [ ] **Logging integration in chaser fault handler** — Critical failures must be logged before shutdown.

### Useful to Operators (v1.x — Same Milestone, After Core)

Features that make the node usable beyond "runs on mainnet."

- [ ] **BIP-125 RBF** — Full-RBF is the 2024 Bitcoin Core default. Mempool diverges from network without it.
- [ ] **Transaction index + getrawtransaction** — Without this, the RPC is nearly useless for any confirmed transaction query.
- [ ] **RPC getblock verbosity=0** — Standard tooling expects this.
- [ ] **RPC getblockchaininfo mediantime** — Correctness fix; currently always returns 0.
- [ ] **Async storage callbacks** — IBD performance improvement; correctness fix for GAP errors.
- [ ] **Checkpoint configuration** — Correctness/configurability improvement for validation.

### Mining Integration (v2 — Next Milestone or End of This)

Only after mempool and block serving are stable.

- [ ] **getblocktemplate (BIP-22/23)** — Enables mining pool integration. Depends on stable mempool, block serving, txindex, and RBF all being complete first.

---

## Feature Prioritization Matrix

| Feature | User Value | Implementation Cost | Priority |
|---------|-----------|---------------------|----------|
| NODE_WITNESS service flag | HIGH (blocks witness data without it) | LOW | P1 |
| OP_CHECKSIGADD (Tapscript) | HIGH (consensus divergence without it) | HIGH | P1 |
| Chainstate UTXO rollback | HIGH (consensus correctness) | HIGH | P1 |
| Chainwork big-endian fix | HIGH (silent wrong chain selection) | MEDIUM | P1 |
| Block serving (getdata) | HIGH (network participant requirement) | HIGH | P1 |
| BIP-125 RBF | HIGH (mempool diverges from network) | MEDIUM | P1 |
| Batch count bug fix | HIGH (active production bug) | LOW | P1 |
| Duplicate address fix | MEDIUM (warning, not corruption) | LOW | P1 |
| Chainwork recomputation | HIGH (reorg correctness) | MEDIUM | P1 |
| Block hash in chaser | MEDIUM (integrity tracking broken) | LOW | P2 |
| Transaction index (txindex) | HIGH (RPC usability) | MEDIUM | P2 |
| RPC getblock verbosity=0 | HIGH (standard tooling expects it) | LOW | P2 |
| RPC mediantime | MEDIUM (correctness for time-locks) | LOW | P2 |
| Async storage callbacks | MEDIUM (IBD performance) | MEDIUM | P2 |
| Checkpoint configuration | LOW (IBD safety, speedup) | LOW | P2 |
| Logging chaser fault handler | LOW (diagnostics) | LOW | P2 |
| getblocktemplate | HIGH (mining integration) | HIGH | P3 |

**Priority key:**
- P1: Must have for peer compatibility — node is broken without these
- P2: Should have this milestone — node is useful without them but degraded
- P3: Important future capability — blocked on P1/P2 being complete

---

## Implementation Notes Per Feature

### OP_CHECKSIGADD (BIP-342)

The vendored libsecp256k1 (at `lib/secp256k1/`) must be checked for whether it exposes `secp256k1_schnorrsig_verify`. If not available in the vendored version, two implementation paths exist:

1. Call secp256k1 for each signature individually (no batch context needed). This is simpler and sufficient for correctness.
2. Use secp256k1-zkp batch verification for performance. Requires vendoring the extended library.

**Recommendation (MEDIUM confidence):** Implement per-signature Schnorr verification first. Batch verification is an optimization for a second pass. The per-signature approach integrates cleanly with the existing script VM opcode dispatch.

Sigops budget per BIP-342: `50 + witness_byte_size`. Each OP_CHECKSIGADD with non-empty signature costs 50 budget units. Budget exhaustion = `SCRIPT_ERR_TAPSCRIPT_VALIDATION_WEIGHT`.

### BIP-125 RBF

Bitcoin Core made full-RBF the default in 2024 and removed the configuration toggle entirely. Echo's implementation should implement the 6 BIP-125 replacement rules:

1. Conflicting transactions explicitly signal RBF (`nSequence < 0xFFFFFFFE`)
2. Replacement only includes unconfirmed inputs already in conflicting transactions
3. Absolute fee of replacement >= sum of replaced transactions
4. Additional fees cover replacement bandwidth at incremental relay feerate
5. Number of replaced transactions + descendants <= 100
6. Replacement feerate > feerate of all directly conflicting transactions

**Note:** With full-RBF now being the Bitcoin Core default, rule 1 (signaling check) can be made configurable or bypassed entirely. The other 5 rules remain regardless.

### Chainstate UTXO Rollback

Bitcoin Core stores undo data in `rev*.dat` files as CTxOut objects (amount + script) for each input spent by a block. On reorg:

1. For each block being disconnected (from tip backward): read undo data, restore UTXOs
2. For each block being connected (on new chain): apply normally

The delta system at `src/consensus/chainstate.c:736-741` is the right design. Each block application should write a delta record before applying. On reorg, replay deltas in reverse. The implementation must:

- Store deltas during `block_connect` (existing validation path)
- Read deltas in reverse during `block_disconnect` (new reorg path)
- Update chainwork after each disconnect/connect step
- Verify UTXO set integrity after reorg completes

---

## Sources

- [Bitcoin P2P Network Reference — developer.bitcoin.org](https://developer.bitcoin.org/reference/p2p_networking.html) — service flags, block serving, message types (MEDIUM confidence, official but not always current)
- [bitcoin/src/protocol.h — GitHub](https://github.com/bitcoin/bitcoin/blob/master/src/protocol.h) — ServiceFlags and InvType enum values (HIGH confidence, Bitcoin Core source)
- [BIP-342 Tapscript — bips.dev](https://bips.dev/342/) — OP_CHECKSIGADD specification, sigops budget (HIGH confidence, final BIP)
- [BIP-125 Replace-by-Fee — bips.dev](https://bips.dev/125/) — RBF signaling and replacement rules (HIGH confidence, final BIP)
- [Bitcoin Optech: Replace-by-fee](https://bitcoinops.org/en/topics/replace-by-fee/) — Full-RBF default in Bitcoin Core #30493 (2024), toggle removed in #30592 (HIGH confidence, well-sourced)
- [bitcoin/doc/policy/mempool-replacements.md](https://github.com/bitcoin/bitcoin/blob/0de63b8b46eff5cda85b4950062703324ba65a80/doc/policy/mempool-replacements.md) — Current Bitcoin Core mempool replacement rules (HIGH confidence, Bitcoin Core source)
- [BIP-324 v2 P2P Transport — bips.dev](https://bips.dev/324/) — v2 optional, v1 fallback guaranteed (HIGH confidence, final BIP)
- [Bitcoin Core Compact Blocks FAQ](https://bitcoincore.org/en/2016/06/07/compact-blocks-faq/) — Compact blocks optional optimization (HIGH confidence, official)
- [Block Relay — Bitcoin Core Academy](https://bitcoincore.academy/block-relay.html) — Relay modes and requirements (MEDIUM confidence, educational)
- [Bitcoin Core 0.11 Data Storage — Bitcoin Wiki](https://en.bitcoin.it/wiki/Bitcoin_Core_0.11_(ch_2):_Data_Storage) — Undo data structure for reorgs (MEDIUM confidence, older but structurally accurate)
- [Taproot adoption statistics 2024-2025 — Binance News](https://www.binance.com/en/square/post/2024-01-30-bitcoin-network-taproot-adoption-rate-increases-from-1-to-39-in-a-year-3427543682289) — 15-20% of mainnet txs use Taproot (LOW-MEDIUM confidence, secondary source)

---

*Feature research for: Bitcoin full node peer compatibility*
*Researched: 2026-02-20*
