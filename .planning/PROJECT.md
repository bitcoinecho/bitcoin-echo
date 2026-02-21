# Bitcoin Echo — Peer-Compatible Node

## What This Is

A full Bitcoin node implementation in pure C11 that can complete initial block download on mainnet, validate all consensus rules including SegWit and Taproot, serve blocks to peers, and participate as a real network participant. This milestone addresses all known issues from the codebase audit to bring echo from "downloads and validates" to "fully peer-compatible."

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

### Active

- [ ] Complete chainstate rollback for reorg handling (UTXO undo via delta system)
- [ ] Implement OP_CHECKSIGADD (BIP-342 Tapscript batch verification)
- [ ] Implement BIP-125 RBF (replace-by-fee) in mempool
- [ ] Transaction index for confirmed tx lookups
- [ ] Full block serving to peers (getdata/getblocks)
- [ ] NODE_WITNESS service flag and INV_WITNESS_BLOCK
- [ ] Fix chainwork big-endian storage for fork selection
- [ ] Fix chainwork recomputation on reorg
- [ ] Async storage callbacks (decouple validation from I/O)
- [ ] Fix download manager batch remaining count bug
- [ ] Fix duplicate address detection in peer manager
- [ ] RPC getblock raw hex (verbosity=0)
- [ ] RPC getrawtransaction for confirmed transactions
- [ ] RPC getblockchaininfo mediantime (MTP calculation)
- [ ] RPC getblocktemplate for mining pool integration
- [ ] Checkpoint configuration (read from config, not hardcoded 0)
- [ ] Block hash retrieval during chaser validation
- [ ] Logging system integration for chaser fault handler
- [ ] Peer eviction threshold calibration (SLOWEST_EVICTION_MIN_RATE)
- [ ] Test: reorg scenarios (deep, complex, same-work chains)
- [ ] Test: concurrent block storage read/write
- [ ] Test: peer eviction under load
- [ ] Test: Taproot script validation vectors (BIP-342)
- [ ] Test: large block handling (max size, corrupted, truncated)
- [ ] Test: BIP-125 RBF validation

### Out of Scope

- Wallet/key management/signing — per project manifesto, users bring external signer
- Mobile platform support — POSIX desktop focus
- GUI/frontend work — separate bitcoinecho-gui repo, separate milestone
- RPC polish for GUI integration — deferred to dedicated RPC milestone
- External dependency additions — pure C11 + vendored libs only

## Context

Bitcoin Echo is a from-scratch Bitcoin full node in pure C11 with no external dependencies beyond vendored SQLite and libsecp256k1. The codebase is ~15-25k lines with heavy comments and a frozen consensus layer that performs no I/O.

A comprehensive codebase audit (`.planning/codebase/CONCERNS.md`) identified tech debt, known bugs, security considerations, performance bottlenecks, fragile areas, and test coverage gaps. This milestone addresses all of them to produce a peer-compatible mainnet node.

The node currently completes IBD (headers sync + block download + validation) but cannot serve blocks, has incomplete Taproot support, and has several known bugs and missing features that prevent it from being a reliable network participant.

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
| Address all CONCERNS.md items | Comprehensive sweep before adding new features | — Pending |
| Include getblocktemplate | Enables mining pool integration as part of peer compatibility | — Pending |
| Skip wallet/signing | Per project manifesto — users bring external signer | — Pending |
| Defer RPC GUI polish | Separate milestone with bitcoinecho-gui repo | — Pending |
| Peer-compatible node as target | IBD + serve blocks + validate SegWit/Taproot = real participant | — Pending |

---
*Last updated: 2026-02-20 after initialization*
