# Project Research Summary

**Project:** Bitcoin Echo v1.1 — Network Participant Milestone
**Domain:** Bitcoin full node (P2P block serving, BIP-125 RBF mempool, transaction index, RPC expansion)
**Researched:** 2026-02-21
**Confidence:** HIGH

## Executive Summary

Bitcoin Echo v1.1 transforms an IBD-complete full node into a genuine network participant. v1.0 delivered all consensus, UTXO management, chain reorg, and IBD infrastructure — 1098/1098 tests passing. What remains is three categories of work: (1) P2P block serving — the node must respond to `getdata` requests from peers or be evicted from the network; (2) mempool policy — BIP-125 full-RBF rules must be implemented or Echo's mempool diverges from the rest of the network and miners lose fee revenue; (3) RPC completeness — `getrawtransaction`, `getblock`, `getblocktemplate`, and `getblockchaininfo` all have critical stubs returning zero or empty responses, breaking all standard tooling.

The recommended approach is a strict dependency-ordered build sequence with P2P block serving first, followed by parallel-safe storage and mempool work, then the RPC layer on top. All implementation is in pure C11 against existing vendored dependencies — no new libraries, no new database files (the tx index goes in the existing `block_index.db`). The codebase already has the data structures, constants, and stub hooks; every feature is a matter of filling in documented TODOs with precise, protocol-correct implementations at known file and line references.

The dominant risk is correctness, not complexity. Each feature has a known "looks done but isn't" failure mode: wrong serialization format for block serving, absolute-fee confusion for BIP-125 Rule 3, txindex stale entries after chain reorg, and wtxid-vs-txid confusion in the witness commitment computation. These mistakes are non-obvious, silently pass basic tests, and produce hard-to-debug peer disconnections or invalid blocks in production. The mitigation is targeted test vectors for each edge case before marking any feature complete.

---

## Key Findings

### Recommended Stack

The stack is frozen at pure C11 + vendored SQLite + vendored libsecp256k1 + POSIX sockets/pthreads. No new dependencies are introduced in v1.1. Every "technology decision" in this milestone is which BIP governs the feature, how existing module boundaries connect, and what must not be done wrong.

**Core technologies:**
- **Pure C11** — frozen project requirement; all implementation language; no exceptions
- **SQLite (vendored, in-tree amalgamation)** — extended with a `tx_index` table in the existing `block_index.db`; WAL mode and mutex already in place; no second database file needed or acceptable
- **POSIX sockets / pthreads** — already used for P2P; `relay.c` rate limiting applies to block serving without replication in `node.c`
- **libsecp256k1 (vendored)** — no new use in v1.1; all signature verification paths complete from v1.0

**Protocol specifications governing new work:**
- BIP-144: NODE_WITNESS service flag (bit 3), INV_WITNESS_BLOCK inventory type (0x40000002)
- BIP-125: All 5 opt-in RBF replacement rules (signaling, no new unconfirmed inputs, absolute fee, bandwidth fee, eviction count)
- BIP-22 / BIP-145: `getblocktemplate` base and SegWit extension (witness commitment, `"rules": ["segwit"]`)
- BIP-141: Witness commitment structure (SHA256d of witness merkle root + nonce, 0xaa21a9ed prefix)
- BIP-113: Median Time Past definition (median of previous 11 block timestamps)

### Expected Features

**Must have — table stakes (P2P citizen — node is broken without these):**
- **Full block serving via getdata** — peers that cannot get blocks from Echo deprioritize and eventually drop it; `relay_handle_getdata()` stub exists, body is a TODO at `node.c:2786`
- **INV_WITNESS_BLOCK announcements** — using the correct inventory type for witness-capable peers; announcing before block serving is complete is a protocol violation causing immediate peer disconnection
- **BIP-125 full-RBF with all 5 rules** — Bitcoin Core made full-RBF the default in v28.0; nodes without it reject transactions the entire network accepts; `mempool.c` detects conflicts but rejects all of them via a TODO stub at line 797

**Should have — operator useful (this milestone):**
- **Transaction index (txindex)** — `getrawtransaction` for confirmed transactions returns NOT_FOUND without it; also blocks `getblocktemplate` from knowing the confirmed tx set
- **RPC getblock verbosity=0** — most common `getblock` call; currently returns empty string; breaks all standard tooling and block explorers
- **RPC getblockchaininfo mediantime** — consensus-critical field currently returns hardcoded 0; incorrect data for any application checking time-locked transactions
- **RPC getrawtransaction (confirmed)** — directly depends on txindex; mempool lookup path already works

**Mining integration (end of milestone — blocked on all the above):**
- **RPC getblocktemplate (BIP-22/BIP-145)** — highest complexity; requires stable mempool, txindex, and block serving; witness commitment computation (wtxid merkle root) is the hardest subproblem
- **RPC submitblock** — ships alongside getblocktemplate; forms the complete mining workflow

**Deferred — explicitly out of scope for v1.1:**
- BIP-324 v2 encrypted transport — v1 fallback is graceful; no peer disconnects for lacking v2; significant standalone crypto project
- BIP-152 compact block relay — optimization over full block relay; must not be built before the full block path is stable
- getblock verbosity=1/2 (decoded JSON) — verbosity=0 hex is the table-stakes form; verbose decode deferred to GUI integration milestone
- Mempool persistence across restarts — restart workflow is `rm -rf ~/.bitcoin-echo`; no persistence infrastructure exists
- BIP-157/158 compact block filters — light client feature, not peer compatibility requirement

### Architecture Approach

The four-layer architecture (App → Protocol → Consensus → Platform, with Storage cross-cutting) is unchanged. v1.1 adds no new layers and violates no existing boundaries. New features slot into specific layers without restructuring: block serving and RPC changes are App layer (`node.c`, `rpc.c`); RBF is Protocol layer (`mempool.c`); txindex and MTP query are Storage layer (`block_index_db.c`). The Consensus layer remains frozen. Every new cross-layer call is App → Storage — no new callbacks, no new intra-layer dependencies.

**Major components modified in v1.1:**
1. **`src/app/node.c`** — Add NODE_WITNESS to services flags (lines 2937, 2961, 3329-3330); implement getdata block serving body (TODO at line 2786); use INV_WITNESS_BLOCK when requesting from witness peers (TODO at line 1779)
2. **`src/app/rpc.c`** — Wire getblock verbosity=0 (line 1783 TODO), mediantime (line 1662 TODO), getrawtransaction confirmed (line 1897 TODO), getblocktemplate witness commitment and real MTP (partial implementation at lines 2111-2421)
3. **`src/protocol/mempool.c`** — Implement BIP-125 replacement logic replacing TODO stub at line 797
4. **`src/storage/block_index_db.c`** — Add `tx_index` table, `txindex_insert()`, `txindex_lookup()`, `get_median_time()` functions

**Unchanged (all of):** consensus/, crypto/, platform/, chaser layers, blocks.c, utxo_db.c, peer.c, sync.c, download_mgr.c, relay.c, discovery.c, protocol_serialize.c.

### Critical Pitfalls

1. **Wrong serialization for MSG_BLOCK vs MSG_WITNESS_BLOCK** — The bit-30 distinction in `inv->type` (0x40000002 vs 0x00000002) must be checked with `inv->type & 0x40000000`, not equality. Sending witness-serialized blocks to legacy peers (or vice versa) causes immediate disconnection. Prevention: branch on the bit in the getdata dispatch; test that witness fields are present in served SegWit blocks.

2. **BIP-125 Rule 3: absolute fee, not feerate** — The replacement's total fee must exceed the sum of all evicted transactions' absolute fees (not fee rates). Implementing this as a feerate comparison creates a free relay attack. Prevention: collect the full descendant eviction set first, sum all absolute fees, then compare totals. Test vector: original 1 tx paying 5000 sats; replacement paying 4999 sats at 10x feerate — must be rejected.

3. **txindex not invalidated on chain reorg** — The tx index is an append-on-confirm structure; without an explicit DELETE-on-disconnect pass during reorg, stale entries persist pointing to non-canonical-chain transactions. Recovery requires a full index rebuild. Prevention: DELETE tx_index rows for disconnected blocks in the same SQLite transaction as the UTXO delta rollback.

4. **Witness commitment uses txids instead of wtxids** — The witness merkle root for `getblocktemplate` must use wtxids for all non-coinbase transactions (coinbase wtxid = 32 zero bytes). Using txids produces a different hash that fails `submitblock` validation with "bad-witness-merkle-match". Prevention: confirm wtxid source from `mempool_entry_t`; acceptance test must mine a block from the template and submit it.

5. **Send queue OOM from full block copies** — `PEER_SEND_QUEUE_SIZE = 128` slots. A SegWit block can be 4 MB. Copying full block payloads into queue slots yields up to 512 MB per peer connection. Prevention: queue block handles or pointers and serialize at send time; never copy full block payloads into the message queue.

---

## Implications for Roadmap

Based on the dependency graph in FEATURES.md and the build order in ARCHITECTURE.md, 4 phases are recommended. The ordering is non-negotiable: it reflects hard implementation dependencies, not preferences.

### Phase 1: P2P Block Serving
**Rationale:** Block serving is the gating item for the entire milestone. Without it, Echo is a network leech — it cannot be a genuine participant and will be deprioritized by peers. INV_WITNESS_BLOCK and block serving must ship together; advertising NODE_WITNESS before the getdata handler is complete is a protocol violation (peers send `getdata MSG_WITNESS_BLOCK` and receive nothing, then penalize the node). No later phase depends on this being done first, but `getblocktemplate` explicitly requires block serving to be stable before mining integration begins.
**Delivers:** A node that peers want to stay connected to; complete INV → GETDATA → BLOCK round trip; NODE_WITNESS correctly advertised; INV_WITNESS_BLOCK used when announcing to capable peers
**Addresses features:** Full block serving (P2P-02), INV_WITNESS_BLOCK announcements (P2P-04), NODE_WITNESS service flag (P2P-01)
**Avoids pitfalls:** Wrong MSG_BLOCK vs MSG_WITNESS_BLOCK serialization; NODE_WITNESS advertised before serving is ready; serving pruned blocks without notfound; send queue OOM from full block copies
**Files touched:** `src/app/node.c` only

### Phase 2: BIP-125 Full-RBF Mempool
**Rationale:** The RBF implementation in `mempool.c` is completely isolated from P2P and storage concerns — it touches only `mempool_add()`. It can be developed in parallel with Phase 1 but is sequenced as Phase 2 because its unit tests are entirely offline and fast (no network infrastructure required), and a stable, correct mempool is prerequisite for `getblocktemplate` in Phase 4. Full-RBF is also required before `getblocktemplate` can produce accurate fee-market transaction selections.
**Delivers:** BIP-125-compliant mempool replacement; accurate fee market for mining integration; no mempool divergence from Bitcoin Core v28+ default policy
**Addresses features:** BIP-125 full-RBF with all 5 rules (P2P-03)
**Avoids pitfalls:** Rule 3 absolute-fee confusion; Rule 2 new unconfirmed inputs; inherited signaling not propagated to descendants; Rule 5 descendant count exceeding 100
**Files touched:** `src/protocol/mempool.c` only

### Phase 3: Storage Layer and Core RPC
**Rationale:** The txindex and MTP query function are prerequisite for all RPC completeness work. Building storage infrastructure first lets RPC handlers be wired cleanly in a single pass without stubs. `getblock` verbosity=0 reuses the block read path established in Phase 1. `getrawtransaction` for confirmed transactions directly depends on txindex being populated. This phase can begin as soon as Phase 1 has established the block storage read path.
**Delivers:** Transaction index (confirmed tx lookup); MTP query function reused by multiple RPC handlers; `getblock` returning real witness-serialized hex; `getblockchaininfo` returning real mediantime; `getrawtransaction` for confirmed transactions
**Addresses features:** txindex (RPC-01), getrawtransaction confirmed (RPC-02), getblock verbosity=0 (RPC-03), getblockchaininfo mediantime (RPC-04)
**Avoids pitfalls:** txindex not invalidated on reorg (DELETE on disconnect in same SQLite tx as UTXO rollback); txindex in a separate database file (keep in existing block_index.db for atomicity); mediantime off-by-one (median of previous 11 blocks, not including the current block, using sorted index 5 not 0 or 10)
**Files touched:** `src/storage/block_index_db.c`, `include/block_index_db.h`, `src/app/node.c` (txindex population in `node_apply_block`), `src/app/rpc.c`

### Phase 4: getblocktemplate and submitblock
**Rationale:** The most complex feature in this milestone. Requires stable mempool (Phase 2), real MTP (Phase 3), txindex (Phase 3), and stable block serving (Phase 1) — it is genuinely last by hard dependency. The witness commitment computation (wtxid merkle root + SHA256d with the 32-byte zero nonce) is the hardest subproblem and must be tested by actually mining a block from the template and submitting it via `submitblock`. `submitblock` ships in the same phase because it is useless without the template step.
**Delivers:** Complete mining pool integration; `getblocktemplate` fully BIP-22/BIP-145 compliant with real MTP mintime, correct witness commitment, `"rules": ["segwit"]` field; `submitblock`; Echo is usable by mining infrastructure
**Addresses features:** getblocktemplate BIP-22/145 (RPC-05), submitblock
**Avoids pitfalls:** Witness commitment uses txids not wtxids; commitment pre-inserted into coinbasetxn instead of returned as `default_witness_commitment` script; getblocktemplate without IBD-complete guard (must return RPC error -28 while syncing); `mintime` using wall clock approximation instead of real MTP + 1
**Files touched:** `src/app/rpc.c`, `src/app/mining.c`

### Phase Ordering Rationale

- **Phase 1 must be first:** The node cannot test block serving, mempool, or RPC features in a realistic way while it is being deprioritized by peers. Block serving is also the only phase where the files touched (`node.c`) involve complex threading and peer state concerns.
- **Phase 2 before Phase 4, independent of Phase 1:** RBF is isolated in `mempool.c` with no P2P or storage dependency. It is sequenced after Phase 1 to give Phase 1 time for integration testing before RBF unit tests begin in parallel. Phase 4 requires a correct mempool, so Phase 2 must complete first.
- **Phase 3 between P2P and template:** Storage primitives (txindex, MTP) are prerequisites for every RPC handler in Phase 4. Building them before wiring the RPC handlers eliminates stubs and avoids two-pass implementation.
- **Phase 4 is last by hard dependency:** getblocktemplate touches mempool selection (Phase 2 complete), MTP (Phase 3 complete), txindex (Phase 3 complete), and witness serialization (Phase 1 stable). There is no valid earlier position for this phase.

### Research Flags

Phases requiring no additional research (standard patterns, well-documented, integration points identified):
- **Phase 1 (P2P block serving):** All integration points identified in direct codebase audit. BIP-144, developer.bitcoin.org, and Bitcoin Core `net_processing.cpp` agree on the 6-step getdata handler algorithm. Both changes to `node.c` are documented TODOs with known context.
- **Phase 2 (RBF):** BIP-125 is a final specification. All 5 rules are verbatim in the BIP. Bitcoin Core's v28+ behavior is documented. The data structures in `mempool.h` (signals_rbf, MEMPOOL_RBF_INCREMENT, MEMPOOL_MAX_REPLACEMENT_COUNT) are already correct. Implementation is algorithmic, not research-dependent.
- **Phase 3 (Storage + Core RPC):** SQLite schema pattern matches the existing prepared-statement infrastructure in block_index_db. MTP definition is BIP-113. The block read path is the same one used by Phase 1. No ambiguity in any integration point.

Phases needing targeted pre-implementation verification:
- **Phase 4 (getblocktemplate):** The witness commitment field semantics (returned as a hex script in `default_witness_commitment`, NOT pre-inserted into `coinbasetxn`) and the wtxid merkle root construction (coinbase wtxid = 32 zero bytes; other txs use wtxid not txid) should be verified against BIP-145 and BIP-141 side by side before coding. The spec language is precise but easy to misread; this is the single known area where the research recommends a careful re-read before starting. Acceptance criterion: mine a real block from the template and submit it via `submitblock`.

---

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | Frozen stack; all findings are "which BIP governs which feature" and "which existing module connects where". Zero ambiguity about technology choices. |
| Features | HIGH | Grounded in final BIPs, Bitcoin Core source (merged PRs including PR #30493 for full-RBF default), and direct codebase audit with line numbers. Anti-feature list is well-reasoned with explicit deferral rationale. |
| Architecture | HIGH | Based on direct line-level codebase audit of TODOs and stubs in node.c, mempool.c, and rpc.c. Build order derived from actual code dependency graph, not speculation. Every integration point is named. |
| Pitfalls | HIGH | Sourced from BIP specifications, Bitcoin Core CVE disclosures (CVE-2024-52920, CVE-2021-31876), PR review discussions (PR #21946, #22698), and direct codebase audit. The "looks done but isn't" checklist has 11 specific, verifiable items. |

**Overall confidence:** HIGH

### Gaps to Address

- **Witness commitment field format in getblocktemplate:** The distinction between `default_witness_commitment` as a hex output script (correct, BIP-145) vs as a pre-built coinbase output (wrong) should be empirically verified against a live Bitcoin Core node's `getblocktemplate` response before Phase 4 begins. Known confusion point per PITFALLS.md Pitfall 9; worth 5 minutes of empirical verification before writing any code.

- **Stored block serialization format on disk:** ARCHITECTURE.md states that `block_storage_read()` returns bytes already in witness format if the block was stored post-SegWit. This should be confirmed empirically by inspecting a stored block's raw bytes before Phase 3 wires `getblock` verbosity=0. If stored blocks are legacy-serialized (possible if IBD used INV_BLOCK), Phase 3 will require an explicit re-serialization step rather than pass-through.

- **txindex DELETE integration point during reorg:** The correct call site for DELETE-on-disconnect is the block disconnection path in `chaser_confirm.c`. The exact function name and SQLite transaction scope must be verified in the source before implementing the reorg integration. The architecture is unambiguous; the hook needs one read of `chaser_confirm.c` before Phase 3 begins.

- **Taproot hash_scriptpubkeys / hash_amounts placeholder (v1.0 tech debt):** PITFALLS.md notes this as existing tech debt that blocks full multi-input Taproot coverage on mainnet. Not a v1.1 blocker, but should be tracked and addressed in a post-v1.1 consensus audit before Echo is run against mainnet Taproot-heavy traffic.

---

## Sources

### Primary (HIGH confidence)
- [BIP-144: SegWit P2P](https://github.com/bitcoin/bips/blob/master/bip-0144.mediawiki) — NODE_WITNESS flag (bit 3), INV_WITNESS_BLOCK (0x40000002), wire serialization
- [BIP-125: Opt-in RBF](https://github.com/bitcoin/bips/blob/master/bip-0125.mediawiki) — all 5 replacement rules verbatim
- [BIP-22: getblocktemplate](https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki) — template fields, mintime = MTP+1, sigoplimit
- [BIP-145: getblocktemplate SegWit update](https://bips.dev/145/) — default_witness_commitment, rules:["segwit"]
- [BIP-141: Witness commitment structure](https://github.com/bitcoin/bips/blob/master/bip-0141.mediawiki) — SHA256d(witness_merkle_root || nonce), 0xaa21a9ed prefix
- [BIP-113: Median time-past](https://github.com/bitcoin/bips/blob/master/bip-0113.mediawiki) — MTP definition, median of previous 11 blocks
- [Bitcoin Core txindex.cpp CDiskTxPos](https://github.com/bitcoin/bitcoin/blob/master/src/index/txindex.cpp) — file_index + file_offset + tx_pos_in_block pattern
- [Bitcoin Core net_processing.cpp ProcessGetBlockData](https://github.com/bitcoin/bitcoin/blob/master/src/net_processing.cpp) — getdata block handler reference, CanServeBlocks(), CanServeWitnesses()
- [Bitcoin Core CVE-2024-52920](https://bitcoincore.org/en/2024/07/03/disclose-getdata-cpu/) — getdata count validation, DoS pattern to avoid
- [Bitcoin Core mempool-replacements.md](https://github.com/bitcoin/bitcoin/blob/master/doc/policy/mempool-replacements.md) — current policy, full-RBF default since v28
- [Bitcoin Core PR #30493: Enable full-RBF by default](https://github.com/bitcoin/bitcoin/pull/30493) — full-RBF default confirmed for Bitcoin Core v28
- Direct codebase audit (2026-02-21) — line-level TODOs in node.c (lines 1779, 2786, 2937), mempool.c (line 797), rpc.c (lines 1662, 1783, 1814, 1897, 2111-2421); existing constants, structs, and stubs verified in headers

### Secondary (MEDIUM confidence)
- [developer.bitcoin.org P2P reference](https://developer.bitcoin.org/reference/p2p_networking.html) — getdata/block message flow, notfound behavior
- [Bitcoin Core PR #22698: RBF inherited signaling](https://github.com/bitcoin/bitcoin/pull/22698) — correct fix for inherited signaling; confirms Core divergence from BIP-125 spec
- [Bitcoin Core PR Review Club #22665](https://bitcoincore.reviews/22665) — BIP-125 inherited signaling implementation gap
- [Transaction Pinning: Bitcoin Optech](https://bitcoinops.org/en/topics/transaction-pinning/) — Rule 3 and Rule 5 pinning attack vectors
- [Bitcoin Core PR #27050: Witness blocks in prune mode](https://github.com/bitcoin/bitcoin/pull/27050) — witness serialization complexity when serving pruned-range blocks

---
*Research completed: 2026-02-21*
*Ready for roadmap: yes*
