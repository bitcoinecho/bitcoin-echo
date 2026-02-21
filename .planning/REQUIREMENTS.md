# Requirements: Bitcoin Echo v1.1 Network Participant

**Defined:** 2026-02-21
**Core Value:** Echo must correctly validate and serve the Bitcoin blockchain on mainnet — a node that peers trust and rely on.

## v1.1 Requirements

Requirements for the Network Participant milestone. Each maps to roadmap phases.

### P2P Network

- [x] **P2P-01**: Node advertises NODE_WITNESS (service bit 3) in version message to peers
- [x] **P2P-02**: Node serves full witness-serialized blocks to peers via getdata handler
- [ ] **P2P-03**: Node implements BIP-125 full-RBF with all 5 replacement rules in mempool
- [x] **P2P-04**: Node uses INV_WITNESS_BLOCK inventory type when requesting blocks from witness-capable peers

### RPC

- [ ] **RPC-01**: Node maintains a transaction index mapping txid to block file position
- [ ] **RPC-02**: User can query confirmed transactions by txid via getrawtransaction RPC
- [ ] **RPC-03**: User can retrieve raw block hex via getblock RPC at verbosity=0
- [ ] **RPC-04**: getblockchaininfo returns correct mediantime (median of previous 11 block timestamps)
- [ ] **RPC-05**: User can request block template for mining via getblocktemplate RPC (BIP-22/BIP-145 compliant with witness commitment)
- [ ] **RPC-06**: User can submit mined block via submitblock RPC

### Testing

- [ ] **TEST-01**: BIP-125 RBF test suite validates all 5 replacement rules including edge cases

## Future Requirements

Deferred to later milestones. Tracked but not in current roadmap.

### Transport

- **TRANS-01**: BIP-324 v2 encrypted P2P transport
- **TRANS-02**: BIP-152 compact block relay

### RPC Polish

- **RPCP-01**: getblock verbosity=1 (decoded JSON with transaction details)
- **RPCP-02**: getblock verbosity=2 (decoded JSON with full transaction data)
- **RPCP-03**: Fee estimation RPC (estimatesmartfee)
- **RPCP-04**: Mempool persistence across restarts

### Light Client

- **LITE-01**: BIP-157/158 compact block filters

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| Wallet/key management/signing | Per project manifesto — users bring external signer |
| Mobile platform support | POSIX desktop focus |
| GUI/frontend work | Separate bitcoinecho-gui repo, separate milestone |
| External dependency additions | Pure C11 + vendored libs only |
| getblock verbose decode (v=1/2) | Deferred to RPC polish milestone with GUI integration |
| BIP-324 encrypted transport | v1 fallback sufficient; no peer disconnects for lacking v2 |
| BIP-152 compact block relay | Optimization; depends on full block serving being stable first |
| Mempool persistence | Restart workflow is rm -rf; no persistence infrastructure |
| BIP-157/158 block filters | Light client feature, not peer compatibility requirement |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| P2P-01 | Phase 3 | Complete |
| P2P-02 | Phase 3 | Complete |
| P2P-04 | Phase 3 | Complete |
| P2P-03 | Phase 4 | Pending |
| TEST-01 | Phase 4 | Pending |
| RPC-01 | Phase 5 | Pending |
| RPC-02 | Phase 5 | Pending |
| RPC-03 | Phase 5 | Pending |
| RPC-04 | Phase 5 | Pending |
| RPC-05 | Phase 6 | Pending |
| RPC-06 | Phase 6 | Pending |

**Coverage:**
- v1.1 requirements: 11 total
- Mapped to phases: 11
- Unmapped: 0

---
*Requirements defined: 2026-02-21*
*Last updated: 2026-02-21 — traceability filled after roadmap creation (phases 3-6)*
