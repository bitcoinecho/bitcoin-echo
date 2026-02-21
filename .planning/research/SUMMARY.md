# Project Research Summary

**Project:** Bitcoin Echo — Peer Compatibility Milestone
**Domain:** Bitcoin full node (pure C11, frozen consensus layer)
**Researched:** 2026-02-20
**Confidence:** HIGH

## Executive Summary

Bitcoin Echo is a pure C11 Bitcoin full node with a frozen consensus layer and a rigid no-external-libraries constraint. This milestone is not a greenfield build — it is completing a partially implemented node to reach genuine peer network participation. All technology choices are already fixed (C11 + vendored libsecp256k1 + vendored SQLite); the research is a protocol specification reference, not a stack selection exercise. The node currently syncs headers and downloads blocks but cannot serve blocks to peers, cannot handle chain reorganizations, and rejects valid Taproot multisig transactions. These are not minor gaps: without them the node is a network parasite that downloads but never contributes, diverges from consensus on roughly 15-20% of modern transactions, and corrupts its own UTXO set on any reorg.

The recommended approach proceeds in four strict phases driven by dependency order. Phase 1 fixes foundational bugs that block all subsequent work: async storage races, correct block hashes in validation, chainwork endianness, and fault logging infrastructure. Phase 2 completes consensus correctness in isolation: Tapscript OP_CHECKSIGADD and full chainstate reorg with UTXO undo. Phase 3 activates network participation: NODE_WITNESS service flag, block serving, and BIP-125 RBF. Phase 4 adds operator-facing RPC capabilities: transaction index, getrawtransaction, getblock hex, mediantime, and getblocktemplate. This ordering is non-negotiable — later phases depend on Phase 1 infrastructure, and incorrect chainwork storage (a Phase 1 item) silently breaks reorg logic in Phase 2.

The primary risks are three silent correctness bugs that have no obvious failure signal during IBD on the main chain: (1) chainwork stored little-endian sorts forks incorrectly but only manifests during actual reorgs, (2) the reorg path emits events without invoking UTXO undo, leaving chainstate corrupted after any fork, and (3) OP_CHECKSIGADD currently returns SCRIPT_ERR_BAD_OPCODE, causing the node to reject every Taproot multisig transaction silently during validation. All three must be fixed before mainnet steady-state operation. The mitigation is the phase order itself — Phase 1 and 2 are pure correctness work with no user-visible features, removing the temptation to skip them.

---

## Key Findings

### Recommended Stack

The stack is frozen and non-negotiable per project manifesto: pure C11, vendored libsecp256k1, vendored SQLite, POSIX sockets and pthreads. No new external libraries are permitted under any circumstances. Every "stack decision" in this milestone is a protocol implementation decision: which BIP governs which feature, what the correct C11 approach is, and what must not be done.

**Core technologies:**
- Pure C11: All implementation — frozen, no debate, no exceptions
- libsecp256k1 (vendored): Schnorr signature verification via `secp256k1_schnorrsig_verify()` — individual calls per signature; no batch verify API exists in the public header
- SQLite 3.x (vendored): Block index, UTXO set, transaction index — WAL mode already configured; new `tx_index` table extends existing schema without introducing a second storage engine
- POSIX sockets/pthreads: TCP networking and threading — already in use, no changes needed

**Critical version requirements:**
- libsecp256k1 must expose `ENABLE_MODULE_SCHNORRSIG` (already vendored with this flag)
- Protocol version 70013+ required for NODE_WITNESS / BIP-144 compatibility — Echo already negotiates at this version

### Expected Features

The feature set divides cleanly into what the P2P network enforces (peers disconnect or ignore non-compliant nodes), what makes the node useful to operators and miners, and what belongs in later milestones.

**Must have — table stakes (node is broken without these):**
- NODE_WITNESS service flag (BIP-144) — without this, peers do not send witness data; SegWit and Taproot validation becomes impossible
- OP_CHECKSIGADD / Tapscript (BIP-342) — 15-20% of mainnet transactions use Taproot; current behavior rejects all Taproot multisig as BAD_OPCODE
- Chainstate UTXO rollback for reorgs — any reorg corrupts the UTXO set permanently without this; this is a consensus correctness requirement
- Chainwork big-endian storage — SQLite bytewise comparison of little-endian blobs produces wrong fork selection; silently follows the wrong chain
- Chainwork revert on reorg — after reorg, tip chainwork is stale, causing incorrect chain comparison
- Block serving via getdata — a node that only downloads is a leech; required for real network participation
- Download manager batch count bug fix — active production bug causing LOG_ERROR during IBD
- Duplicate peer address detection fix — active race condition in connection setup
- Real block hash in chaser_validate — submitting all-zeros hash breaks validation tracking integrity

**Should have — operator utility (degraded without these):**
- BIP-125 RBF — full-RBF is the 2024 Bitcoin Core default; mempool diverges from network without it
- Transaction index (txindex) + getrawtransaction — without txindex, the RPC is nearly useless for confirmed transactions
- getblock verbosity=0 (raw hex) — standard tooling expects this
- getblockchaininfo mediantime — always returns 0 currently; correctness bug for time-locked transactions
- Async storage callbacks — eliminates GAP errors and decouples validation from disk I/O latency
- Checkpoint configuration — wire `top_checkpoint` from config instead of hardcoded 0

**Defer to next milestone:**
- getblocktemplate (BIP-22/23) — highest complexity, depends on stable mempool, block serving, txindex, and RBF all being complete first; mining integration milestone
- BIP-324 v2 encrypted transport — v1 fallback is guaranteed; no peer disconnects for lacking v2; separate cryptography milestone
- BIP-152 compact block relay — optimization, not correctness; depends on block serving being stable first
- BIP-157/158 compact block filters — completely orthogonal to peer compatibility; light client milestone
- Wallet / key management — explicitly out of scope per project manifesto, permanently

### Architecture Approach

The existing four-layer architecture (App > Protocol > Consensus > Platform) is maintained unchanged. This milestone adds work within existing layers without restructuring. Three inviolable isolation rules govern all new code: (1) consensus never calls storage — OP_CHECKSIGADD is pure computation with no I/O, (2) reorg coordination lives in `chaser_confirm.c` not `chainstate.c` — the existing `chain_reorganize()` callback API enforces this boundary, and (3) block serving is an application layer concern — `node.c` reads from `blocks.c` and sends via peer infrastructure, touching no consensus code. All cross-layer communication uses the existing callback pattern (`get_block_txs_fn`, `sync_callbacks_t`, `mempool_callbacks_t`).

**Major components and their milestone responsibilities:**
1. `src/consensus/script.c` — OP_CHECKSIGADD implementation; routes to `script_execute_tapscript()` for witness v1 scriptpath spends
2. `src/consensus/chainstate.c` — Chainwork big-endian format fix; `block_delta_t` gets `prev_chainwork` field for revert
3. `src/node/chaser_confirm.c` — Full reorg orchestration: `chain_reorg_create()` → `chain_reorganize()` with `node_load_block` callback; UTXO undo per reverted block
4. `src/node/chaser_validate.c` — Real block hash retrieval from block index; checkpoint config wired from `node_config_t`
5. `src/app/node.c` — Block serving (getdata handler); NODE_WITNESS in version message; async storage callback; duplicate address check
6. `src/storage/block_index_db.c` — Big-endian chainwork storage; `tx_index` SQLite table for transaction lookup
7. `src/storage/blocks.c` — Async write completion callback; block serving read path
8. `src/protocol/mempool.c` — BIP-125 RBF: all 5 rules implemented in `mempool_accept()` when conflict detected
9. `src/app/rpc.c` — getblock hex, getrawtransaction (confirmed), mediantime, getblocktemplate

### Critical Pitfalls

1. **Reorg emits events without undoing UTXO state** — `chaser_confirm.c:250` calls `CHASE_REORGANIZED` but never invokes `chainstate_revert_block()`. UTXO set corrupts silently after any fork. Fix: wire `chainstate_revert_block(state, delta)` for each height from old tip down to fork point, in reverse order, inside the reorg loop. Wrap all reversals in a single SQLite transaction.

2. **Chainwork stored little-endian breaks fork selection** — `block_index_db.c` currently uses `ORDER BY height DESC` as a workaround that only holds during linear IBD. Under a real fork, the node follows the wrong chain. Fix: store chainwork as 32-byte big-endian blob; switch the best-chain query to `ORDER BY chainwork DESC LIMIT 1`. Must be fixed before any reorg testing or the test vectors will mask the ordering bug.

3. **OP_CHECKSIGADD rejects unknown key types** — BIP-342 requires that non-32-byte, non-empty public keys are treated as unknown key types and push `n+1` (success) rather than failing. Implementing "validate if 32-byte key, fail otherwise" causes the node to reject transactions that Bitcoin Core accepts, creating a consensus split. Fix: implement the exact BIP-342 key-size branching table; test against all BIP-342 reference vectors including unknown-key-type vectors.

4. **RBF Rule 3 requires absolute fee, not just feerate** — A higher feerate with lower absolute fee is a valid RBF pinning attack. Must compute aggregate absolute fee across all replaced transactions including their descendants. Rule 5 (eviction count <= 100) requires walking the descendant graph before accepting any replacement.

5. **Async storage marks blocks received before disk write completes** — The async path is currently disabled (`if (false && ...)`) because the download manager marks blocks as "received" on enqueue, not on disk confirmation. This causes GAP errors and IBD stalls. Fix: implement a storage completion callback; download manager consults "durably written" flag, not "enqueued" flag. Never re-enable the async path without this callback in place.

---

## Implications for Roadmap

Based on combined research, the phase structure is clearly determined by hard dependencies. The ordering is not a preference — it reflects what must be true before each subsequent phase can be correctly implemented and tested.

### Phase 1: Foundation Fixes

**Rationale:** These are prerequisite blocking bugs. No later phase can be correctly implemented or meaningfully tested without them. The chainwork endianness bug silently corrupts reorg tests. Missing block hashes in chaser_validate prevent block identity tracking. The fault logging gap means Phase 2 errors are invisible. Async storage must be fixed before block serving because both use `block_file_manager_t`.

**Delivers:** A node that correctly tracks block hashes, has working fault diagnostics, has correct fork selection infrastructure, has no active production LOG_ERRORs, and has decoupled I/O from validation latency.

**Addresses:** Download manager batch count bug, duplicate peer address race, all-zeros block hash in chaser_validate, chainwork big-endian storage fix, async storage callback (eliminating GAP errors), checkpoint config wired from `node_config_t`, chaser fault handler logging.

**Avoids:** Pitfalls 2 (chainwork endianness), 6 (async storage race), 8 (batch remaining count), 9 (duplicate address race), 11 (all-zeros block hash). These are "never acceptable on mainnet" items — the technical debt table is explicit.

**Research flag:** Standard patterns — no per-phase research needed. All root causes are known from codebase audit with file and line references.

---

### Phase 2: Consensus Completeness

**Rationale:** Consensus work is independent of networking and can be developed and tested in isolation using known test vectors. OP_CHECKSIGADD is contained entirely within `script.c` and `secp256k1.c` with no networking dependency. Reorg handling is the most complex single item and must be stable before block serving is activated — serving a block on a fork that later gets reorged with broken UTXO undo produces permanent chainstate corruption.

**Delivers:** A node that correctly validates all Taproot multisig transactions (BIP-342 compliant) and correctly follows the longest chain through reorganizations with UTXO state consistency guaranteed.

**Addresses:** OP_CHECKSIGADD / Tapscript (BIP-342), full chainstate reorg with UTXO undo, chainwork revert via `prev_chainwork` in `block_delta_t`.

**Avoids:** Pitfalls 1 (reorg without UTXO undo), 3 (OP_CHECKSIGADD unknown key type handling), 7 (chainwork revert leaves stale accumulated work). Run all BIP-342 reference vectors before marking complete. Test with a synthetic 6-block reorg before marking reorg complete.

**Research flag:** Standard patterns — BIP-342 and BIP-341 are fully specified. The `chain_reorganize()` API in `chainstate.c` is already designed and partially implemented; the callback pattern is established. No additional research needed.

---

### Phase 3: Peer Network Compatibility

**Rationale:** Network participation depends on Phase 1 (async storage correct, block hashes valid) and Phase 2 (reorg correct, consensus complete) being stable. NODE_WITNESS must precede block serving because advertising witness support causes peers to send `INV_WITNESS_BLOCK` instead of `INV_BLOCK`; the node must be able to serve witness-serialized blocks before advertising the capability. BIP-125 RBF is independent of block serving but belongs here because it aligns with the "becoming a real peer" theme.

**Delivers:** A node that is a genuine Bitcoin network participant: advertises witness support, serves blocks to requesting peers, handles BIP-125 RBF replacements, and maintains a mempool consistent with the modern network.

**Addresses:** NODE_WITNESS service flag (BIP-144), block serving via getdata handler (`INV_BLOCK` and `INV_WITNESS_BLOCK`), BIP-125 RBF with all 5 rules implemented.

**Avoids:** Pitfall 5 (block serving without NODE_WITNESS drops witness data), Pitfall 4 (RBF Rule 3 absolute fee check, Rule 5 descendant limit). The mutated block state isolation pitfall (CVE-2024-52921 pattern) must also be addressed: index block download state by `(peer_id, block_hash)`, not `block_hash` alone.

**Research flag:** Standard patterns — BIP-144 service flags are fully specified and already partially implemented. Block serving flow is documented in Bitcoin Core `net_processing.cpp`. RBF rules are stable and well-sourced. No additional research needed.

---

### Phase 4: RPC and Operator Capabilities

**Rationale:** RPC features depend on the storage infrastructure from Phases 1-3. Transaction index writes during block application (Phase 3 pipeline) and reads during RPC dispatch. getblock hex uses the same block read path as block serving. getblocktemplate is explicitly last — it is the most complex feature, touching mempool (Phase 3 RBF), block structure, coinbase construction, and witness commitment (BIP-141). It must be implemented only after mempool and block serving are stable.

**Delivers:** A node that operators and developers can actually use: confirmed transaction lookup, raw block retrieval, correct mediantime for time-locked transaction validation, and (at the end of the phase) block template construction for mining integration.

**Addresses:** Transaction index (new `tx_index` SQLite table), getrawtransaction for confirmed transactions, getblock verbosity=0 (raw hex), getblockchaininfo mediantime (query last 11 block timestamps, return median), getblocktemplate (BIP-22/23) with MTP, witness commitment, sigoplimit, and `"rules": ["segwit"]`.

**Avoids:** Do not implement getblocktemplate before mempool and block serving are stable — it depends on both. Do not use a separate flat-file index for txindex — SQLite is the persistence layer; a new table is correct. The pruning interaction with txindex must be handled: return not-found for pruned blocks (Bitcoin Core behavior with pruning enabled).

**Research flag:** getblocktemplate needs attention during planning — BIP-22 and BIP-145 are fully specified, but the witness commitment construction (witness merkle root with all-zero coinbase wtxid, then SHA256d with nonce) is subtle and the existing partial implementation in `rpc.c:2111` has known gaps (MTP not wired, witness commitment not constructed). Recommend treating getblocktemplate as a sub-phase within Phase 4, implemented last after all other RPC items are complete.

---

### Phase Ordering Rationale

- **Foundation before consensus:** The chainwork endianness bug would cause reorg test vectors to pass with wrong behavior (correct chain selected by height, which coincidentally works on the test main chain). Async storage must be correct before block serving for correctness, not just performance.
- **Consensus before networking:** A node that serves blocks while rejecting valid Taproot transactions is a liability on the network. Reorg correctness must precede block serving because the block serving path and reorg path share the `block_file_manager_t` — a reorg that corrupts chainstate before serving is completed produces permanently wrong state.
- **Networking before RPC:** Transaction index is populated during block application (the networking pipeline). RPC features that depend on txindex can only be tested end-to-end after the population path is in place.
- **Grouping rationale:** The groupings reflect natural isolation boundaries in the architecture. Phase 2 items (consensus) can be developed and unit tested without any network infrastructure. Phase 3 items (networking) require a running peer but no operator RPC tooling. This matches the existing test runner structure.

### Research Flags

Phases likely needing deeper research during planning:
- **Phase 4 — getblocktemplate:** The witness commitment construction requires careful sequencing (witness merkle tree with zero coinbase wtxid, then SHA256d with the nonce from the coinbase). The existing `coinbase_params_t` struct has the right fields but the population logic is unwritten. Recommend reviewing Bitcoin Core `miner.cpp` `CreateNewBlock()` and `IncrementExtraNoonce()` before implementation planning.
- **Phase 2 — Tapscript routing verification:** The critical question is whether `script_execute_tapscript()` is correctly called for witness v1 scriptpath spends, or whether witness v1 execution falls through to the generic opcode dispatcher that returns `SCRIPT_ERR_BAD_OPCODE`. This requires reading `script_execute()` routing logic before implementation, not during.

Phases with standard patterns (skip research-phase):
- **Phase 1 — Foundation fixes:** All root causes are known with exact file and line numbers from the codebase audit in CONCERNS.md. Implementation is mechanical.
- **Phase 3 — NODE_WITNESS + block serving:** Fully specified in BIP-144 and Bitcoin Core `net_processing.cpp`. The stub at `node.c:2769` already has the correct shape.
- **Phase 3 — BIP-125 RBF:** All 5 rules are stable, well-documented, and the data structures (`mempool_entry_t.signals_rbf`, `MEMPOOL_RBF_INCREMENT`) are already in place.

---

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | No decisions to make — frozen stack with explicit manifesto constraints. All protocol references (BIPs, Bitcoin Core source) are authoritative and current. |
| Features | HIGH | Feature set derived from BIPs (HIGH confidence primary sources) and Bitcoin Core behavior (HIGH confidence source code). Taproot adoption statistics sourced from secondary/tertiary sources but the correctness requirement is BIP-derived, not statistics-dependent. |
| Architecture | HIGH | Based on direct codebase audit — the existing layer boundaries, component responsibilities, and callback patterns are read from actual source headers. No inference required. |
| Pitfalls | HIGH | All critical pitfalls are grounded in actual code bugs (specific file and line references), BIP specification requirements, or documented Bitcoin Core CVEs. Not speculative. |

**Overall confidence:** HIGH

### Gaps to Address

- **Tapscript execution routing:** Whether `script_execute()` actually routes witness v1 scriptpath spends to `script_execute_tapscript()` vs the generic dispatcher is unconfirmed without reading `script.c` in detail. The research identifies this as "the critical gap" but cannot resolve it without a direct code read during implementation planning. Flag for Phase 2 planning.
- **Async storage callback thread safety:** The specific callback invocation model (called from storage thread, notifying download manager) requires verifying that the download manager's block slot flags are mutex-protected or lock-free. The architecture research recommends a mutex-protected counter or per-slot lock-free flag but the correct choice depends on the download manager's existing synchronization model. Flag for Phase 1 planning.
- **Vendored secp256k1 API surface:** The research confirms `secp256k1_schnorrsig_verify` is in the public header but the exact call signature and context initialization for the vendored version should be verified against the vendored header at `lib/secp256k1/include/secp256k1_schnorrsig.h` before Phase 2 implementation begins.

---

## Sources

### Primary (HIGH confidence)
- BIP-340, BIP-341, BIP-342 — Schnorr, Taproot, Tapscript specification — canonical implementation rules
- BIP-144 — NODE_WITNESS, MSG_WITNESS_BLOCK, service flag values
- BIP-125 — RBF signaling and all 5 replacement rules
- BIP-22, BIP-145 — getblocktemplate core and SegWit extension
- Bitcoin Core `net_processing.cpp` — block serving flow (`ProcessGetBlockData`)
- Bitcoin Core `interpreter.cpp` — OP_CHECKSIGADD reference implementation
- Bitcoin Core `txindex.cpp` — CDiskTxPos structure, index write pattern
- Bitcoin Core `miner.cpp` — block template construction algorithm
- Bitcoin Core `protocol.h` — ServiceFlags and InvType enum values
- Bitcoin Core `mempool-replacements.md` — current RBF policy documentation
- `lib/secp256k1/include/secp256k1_schnorrsig.h` (vendored) — API surface confirmed
- `.planning/codebase/CONCERNS.md` — codebase audit with file/line references for all pitfalls
- `.planning/codebase/ARCHITECTURE.md` — existing layer documentation

### Secondary (MEDIUM confidence)
- developer.bitcoin.org P2P reference — service flags, message types (official but not always current)
- Bitcoin Wiki Data Storage (0.11) — undo file format for reorg (structurally accurate, older doc)
- Bitcoin Optech RBF topic — full-RBF default in Bitcoin Core PR #30493 (2024)
- CVE-2024-52921 disclosure — mutated block download state isolation requirement

### Tertiary (LOW confidence)
- Binance News Taproot adoption statistics (2024-2025) — 15-20% mainnet Taproot usage (secondary source; the correctness requirement is BIP-derived regardless of adoption percentage)

---

*Research completed: 2026-02-20*
*Ready for roadmap: yes*
