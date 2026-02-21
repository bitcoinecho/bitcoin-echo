# Bitcoin Echo — Peer-Compatible Node

## What This Is

A full Bitcoin node implementation in pure C11 that completes initial block download on mainnet, validates all consensus rules including SegWit and Taproot (with BIP-342 Tapscript key type dispatch), handles chain reorganizations with full UTXO rollback, and is being extended toward full peer compatibility with block serving and RPC capabilities.

## Core Value

Echo must correctly validate and serve the Bitcoin blockchain on mainnet — a node that peers trust and rely on.

## Requirements

### Validated

- ✓ Headers-first IBD with parallel block download — existing
- ✓ Block validation (PoW, merkle, coinbase, structure) — existing
- ✓ Transaction validation (UTXO, scripts, value conservation) — existing
- ✓ Script VM with P2SH, BIP-66, CLTV, CSV, SegWit — existing
- ✓ UTXO management in SQLite with WAL mode — existing
- ✓ Block storage (blk*.dat) with pruning — existing
- ✓ Block index persistence in SQLite — existing
- ✓ Peer discovery and connection management — existing
- ✓ Download manager with batch-based work distribution — existing
- ✓ Mempool with basic transaction acceptance — existing
- ✓ Basic RPC (getsyncstatus, getblockchaininfo) — existing
- ✓ Chaser-based parallel validation pipeline — existing
- ✓ Platform abstraction (POSIX) — existing
- ✓ Chainwork big-endian storage for correct SQLite fork selection — v1.0
- ✓ Block storage flush-before-index for durable writes — v1.0
- ✓ Download manager batch remaining count fix — v1.0
- ✓ Duplicate address connection prevention — v1.0
- ✓ Chaser fault logging before shutdown — v1.0
- ✓ Real block hash in chaser validation — v1.0
- ✓ Configurable checkpoint height — v1.0
- ✓ Peer eviction threshold calibrated from mainnet IBD data — v1.0
- ✓ OP_CHECKSIGADD BIP-342 Tapscript with unknown key type upgrade rule — v1.0
- ✓ Full UTXO rollback on chain reorganization via delta undo system — v1.0
- ✓ Chainwork recomputation on reorg (prev_chainwork in block_delta_t) — v1.0
- ✓ Test: concurrent block storage (7 tests) — v1.0
- ✓ Test: peer eviction under load (5 tests) — v1.0
- ✓ Test: BIP-342 Tapscript reference vectors (10 tests) — v1.0
- ✓ Test: large block edge cases (corrupted, near-4x witness limit) — v1.0
- ✓ Test: chain reorganization (simple fork, deep reorg, same-work) — v1.0

### Active

- [ ] NODE_WITNESS service flag and INV_WITNESS_BLOCK (P2P-01, P2P-04)
- [ ] Full block serving to peers via getdata handler (P2P-02)
- [ ] BIP-125 full-RBF with all 5 replacement rules (P2P-03)
- [ ] Transaction index for confirmed tx lookups (RPC-01, RPC-02)
- [ ] RPC getblock raw hex verbosity=0 (RPC-03)
- [ ] RPC getblockchaininfo mediantime (RPC-04)
- [ ] RPC getblocktemplate for mining pool integration (RPC-05)
- [ ] Test: BIP-125 RBF validation (TEST-06)

### Out of Scope

- Wallet/key management/signing — per project manifesto, users bring external signer
- Mobile platform support — POSIX desktop focus
- GUI/frontend work — separate bitcoinecho-gui repo, separate milestone
- RPC polish for GUI integration — deferred to dedicated RPC milestone
- External dependency additions — pure C11 + vendored libs only
- BIP-324 v2 encrypted transport — v1 fallback sufficient, no peer disconnects
- BIP-152 compact block relay — optimization, depends on block serving being stable

## Current Milestone: v1.1 Network Participant

**Goal:** Make Echo a real network participant — serve blocks to peers, handle BIP-125 full-RBF mempool policy, and expand RPC for block queries and mining pool integration.

**Target features:**
- NODE_WITNESS service flag and INV_WITNESS_BLOCK support
- Full block serving to peers via getdata handler
- BIP-125 full-RBF with all 5 replacement rules
- Transaction index for confirmed tx lookups
- RPC getblock, mediantime, getblocktemplate
- BIP-125 RBF test suite

## Context

Bitcoin Echo is a from-scratch Bitcoin full node in pure C11 with no external dependencies beyond vendored SQLite and libsecp256k1. The codebase is 15,648 LOC source + 31,692 LOC tests with heavy comments and a frozen consensus layer that performs no I/O.

v1.0 shipped with all known IBD bugs fixed, BIP-342 Tapscript validation complete, and full UTXO rollback for chain reorganizations. The node completes IBD on mainnet with 1098/1098 tests passing. Next milestone targets peer network compatibility (block serving, RBF mempool) and RPC capabilities.

**Known tech debt:** hash_scriptpubkeys/hash_amounts placeholders in script.c will block real multi-input Taproot transaction validation on mainnet. test_chase Makefile has a link defect worked around by stubs.

Testing is against Bitcoin mainnet. The start sequence is: `rm -rf ~/.bitcoin-echo && ./echo --prune=1024`.

## Constraints

- **Tech stack**: Pure C11, stdlib + embedded SQLite/libsecp256k1 only — no new external libs
- **Consensus immutability**: Consensus layer must remain I/O-free and deterministic
- **Architecture**: Maintain existing layered architecture (App > Protocol > Consensus > Platform)
- **Style**: Obvious purpose, bounds-check all, constants not magic numbers, heavy comments
- **Build**: Must pass clang-tidy (cert-*, bugprone-*, misc-*, portability-*, readability-*)
- **Testing**: Mainnet validation — node must IBD to tip without errors

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Address all CONCERNS.md items | Comprehensive sweep before adding new features | ✓ Good — all 17 items resolved in v1.0 |
| Include getblocktemplate | Enables mining pool integration as part of peer compatibility | — Pending (Phase 4) |
| Skip wallet/signing | Per project manifesto — users bring external signer | ✓ Good — scope stayed focused |
| Defer RPC GUI polish | Separate milestone with bitcoinecho-gui repo | ✓ Good — no scope creep |
| Peer-compatible node as target | IBD + serve blocks + validate SegWit/Taproot = real participant | — In progress (Phases 3-4 remaining) |
| fflush() not fsync() for block storage | Pushes to OS page cache without killing IBD throughput | ✓ Good — GAP errors eliminated |
| Byte reversal at DB boundary only for chainwork | work256 arithmetic unchanged, ORDER BY DESC works correctly | ✓ Good — clean separation |
| 1 KB/s eviction threshold | Conservative, matches Bitcoin Core nMinExpectedRate behavior | ✓ Good — no false evictions during IBD |
| Chainstate owns deltas, callers get borrowed refs | Clear ownership semantics, no double-free risk | ✓ Good — reorg rollback works cleanly |

---
*Last updated: 2026-02-21 after v1.1 milestone start*
