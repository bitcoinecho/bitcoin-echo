# Requirements: Bitcoin Echo — Peer-Compatible Node

**Defined:** 2026-02-20
**Core Value:** Echo must correctly validate and serve the Bitcoin blockchain on mainnet — a node that peers trust and rely on.

## v1 Requirements

Requirements for peer-compatible mainnet node. Each maps to roadmap phases.

### Consensus

- [ ] **CONS-01**: Node validates OP_CHECKSIGADD (BIP-342 Tapscript) correctly for all key types including unknown-key-type upgrade rule
- [ ] **CONS-02**: Node performs full UTXO rollback on chain reorganization using delta undo system
- [x] **CONS-03**: Node stores chainwork in big-endian format for correct SQLite bytewise fork selection
- [ ] **CONS-04**: Node recomputes chainwork correctly during reorg (stores prev_chainwork in block_delta_t)
- [ ] **CONS-05**: Node passes all BIP-342 reference test vectors for Tapscript validation

### P2P Network

- [ ] **P2P-01**: Node advertises NODE_WITNESS service flag (BIP-144) in version message
- [ ] **P2P-02**: Node serves blocks to peers via getdata handler for both INV_BLOCK and INV_WITNESS_BLOCK
- [ ] **P2P-03**: Node implements BIP-125 full-RBF with all 5 replacement rules (signaling, no new unconfirmed inputs, absolute fee >= evicted sum, bandwidth fee, <= 100 descendants evicted)
- [ ] **P2P-04**: Node uses INV_WITNESS_BLOCK for block inventory when advertising to witness-capable peers

### Bug Fixes

- [ ] **BUGF-01**: Download manager correctly handles batch remaining count when duplicate blocks received from reassigned batches
- [ ] **BUGF-02**: Peer manager prevents duplicate address connections via proper deduplication check before connection setup

### Infrastructure

- [x] **INFR-01**: Block storage uses async write with completion callback — download manager tracks "durably written" not "enqueued"
- [ ] **INFR-02**: Chaser validation retrieves real block hash from block index instead of submitting all-zeros
- [ ] **INFR-03**: Checkpoint height is configurable via node config instead of hardcoded to 0
- [ ] **INFR-04**: Chaser fault handler logs error details via log system before shutdown signal
- [x] **INFR-05**: Peer eviction threshold (SLOWEST_EVICTION_MIN_RATE) calibrated from mainnet IBD measurement data

### RPC

- [ ] **RPC-01**: getrawtransaction returns confirmed transactions via transaction index lookup
- [ ] **RPC-02**: Transaction index (txid -> file_index, file_offset, tx_offset) populated during block application
- [ ] **RPC-03**: getblock with verbosity=0 returns raw hex-encoded block bytes
- [ ] **RPC-04**: getblockchaininfo returns correct mediantime (median of last 11 block timestamps)
- [ ] **RPC-05**: getblocktemplate returns valid block template with correct coinbase, witness commitment, MTP, and sigoplimit per BIP-22/BIP-145

### Testing

- [ ] **TEST-01**: Test suite covers reorg scenarios: simple fork, deep reorg (6+ blocks), same-work competing chains
- [x] **TEST-02**: Test suite covers concurrent block storage reads and writes under load
- [x] **TEST-03**: Test suite covers peer eviction with multiple slow/stalled peers
- [ ] **TEST-04**: Test suite covers Taproot script validation using BIP-342 reference vectors
- [x] **TEST-05**: Test suite covers large block handling at ECHO_MAX_BLOCK_SIZE and near 4x limit, including corrupted size fields and truncated reads
- [ ] **TEST-06**: Test suite covers BIP-125 RBF replacement validation with all 5 rules

## v2 Requirements

Deferred to future milestones. Tracked but not in current roadmap.

### Crypto/Transport

- **CRYPT-01**: BIP-324 v2 encrypted P2P transport with ChaCha20Poly1305
- **CRYPT-02**: Elligator Swift key exchange for connection setup

### Block Relay Optimization

- **RELAY-01**: BIP-152 compact block relay for reduced propagation latency
- **RELAY-02**: Compact block reconstruction from mempool

### Light Client Support

- **LIGHT-01**: BIP-157/158 compact block filters (NODE_COMPACT_FILTERS)
- **LIGHT-02**: Filter header chain serving

### GUI Integration

- **GUI-01**: RPC response format polish for bitcoinecho-gui compatibility
- **GUI-02**: WebSocket push notifications for block/tx events

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| Wallet / key management / signing | Per project manifesto — users bring external signer |
| Mobile platform support | POSIX desktop focus; Windows via build config later |
| GUI / frontend work | Separate bitcoinecho-gui repo, separate milestone |
| P2P fee estimation | Not a peer compatibility requirement; complex mempool history |
| BIP-324 v2 transport | v1 fallback guaranteed; no peer disconnects for lacking v2 |
| BIP-152 compact block relay | Optimization; depends on block serving being stable first |
| External dependencies | Pure C11 + vendored libs only per manifesto |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| CONS-01 | Phase 2 | Pending |
| CONS-02 | Phase 2 | Pending |
| CONS-03 | Phase 1 | Complete |
| CONS-04 | Phase 2 | Pending |
| CONS-05 | Phase 2 | Pending |
| P2P-01 | Phase 3 | Pending |
| P2P-02 | Phase 3 | Pending |
| P2P-03 | Phase 3 | Pending |
| P2P-04 | Phase 3 | Pending |
| BUGF-01 | Phase 1 | Pending |
| BUGF-02 | Phase 1 | Pending |
| INFR-01 | Phase 1 | Complete |
| INFR-02 | Phase 1 | Pending |
| INFR-03 | Phase 1 | Pending |
| INFR-04 | Phase 1 | Pending |
| INFR-05 | Phase 1 | Complete |
| RPC-01 | Phase 4 | Pending |
| RPC-02 | Phase 4 | Pending |
| RPC-03 | Phase 4 | Pending |
| RPC-04 | Phase 4 | Pending |
| RPC-05 | Phase 4 | Pending |
| TEST-01 | Phase 2 | Pending |
| TEST-02 | Phase 1 | Complete |
| TEST-03 | Phase 1 | Complete |
| TEST-04 | Phase 2 | Pending |
| TEST-05 | Phase 1 | Complete |
| TEST-06 | Phase 3 | Pending |

**Coverage:**
- v1 requirements: 27 total
- Mapped to phases: 27
- Unmapped: 0

---
*Requirements defined: 2026-02-20*
*Last updated: 2026-02-20 after roadmap creation — all 27 requirements mapped*
