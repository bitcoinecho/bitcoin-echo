# Roadmap: Bitcoin Echo — Peer-Compatible Node

## Milestones

- ✅ **v1.0** — Phases 1-2: Foundation Fixes + Consensus Completeness (shipped 2026-02-21)
- 📋 **Next** — Phases 3-4: Peer Network + RPC (planned)

## Phases

<details>
<summary>✅ v1.0 (Phases 1-2) — SHIPPED 2026-02-21</summary>

- [x] Phase 1: Foundation Fixes (7/7 plans) — completed 2026-02-20
- [x] Phase 2: Consensus Completeness (3/3 plans) — completed 2026-02-21

See: `.planning/milestones/v1.0-ROADMAP.md` for full details

</details>

### Phase 3: Peer Network Compatibility
**Goal**: The node is a genuine Bitcoin network participant: it advertises witness capability, serves blocks to requesting peers, and maintains a mempool consistent with full-RBF nodes
**Depends on**: Phase 2
**Requirements**: P2P-01, P2P-02, P2P-03, P2P-04, TEST-06
**Success Criteria** (what must be TRUE):
  1. Node's version message includes the NODE_WITNESS service flag — peers that inspect the service flags field see BIT-3 set, and they send witness-serialized inventory
  2. Peers that send a getdata request for a block hash receive a valid serialized block response — for witness-capable peers the response uses witness serialization (INV_WITNESS_BLOCK), for legacy peers it uses the stripped format (INV_BLOCK)
  3. A transaction that signals RBF and offers a higher absolute fee replaces its predecessor in the mempool — a replacement that violates any of the 5 BIP-125 rules is rejected with a specific error, not accepted
  4. The node advertises block inventory to witness-capable peers using INV_WITNESS_BLOCK type, not INV_BLOCK
**Plans**: TBD

Plans:
- [ ] 03-01: Add NODE_WITNESS service flag to version message (P2P-01)
- [ ] 03-02: Implement getdata handler for INV_BLOCK and INV_WITNESS_BLOCK (P2P-02)
- [ ] 03-03: Implement BIP-125 RBF with all 5 replacement rules in mempool (P2P-03)
- [ ] 03-04: Use INV_WITNESS_BLOCK for block inventory to witness-capable peers (P2P-04)
- [ ] 03-05: Test BIP-125 RBF replacement validation with all 5 rules (TEST-06)

### Phase 4: RPC and Operator Capabilities
**Goal**: Operators and developers can look up confirmed transactions, retrieve raw blocks, read correct mediantime for time-locked transaction analysis, and generate block templates for mining pool integration
**Depends on**: Phase 3
**Requirements**: RPC-01, RPC-02, RPC-03, RPC-04, RPC-05
**Success Criteria** (what must be TRUE):
  1. getrawtransaction called with a confirmed txid returns the raw hex-encoded transaction bytes — calls for transactions in pruned blocks return not-found, matching Bitcoin Core pruning behavior
  2. getblock called with a block hash and verbosity=0 returns the raw hex-encoded block bytes for that block
  3. getblockchaininfo returns a mediantime value that equals the median of the last 11 block timestamps at the current tip — it does not return 0
  4. getblocktemplate returns a valid block template including a correct witness commitment in the coinbase, correct MTP-derived time floor, and the "segwit" entry in the rules array, suitable for submission by a mining pool
**Plans**: TBD

Plans:
- [ ] 04-01: Create tx_index SQLite table and populate during block application (RPC-02)
- [ ] 04-02: Implement getrawtransaction using tx_index lookup (RPC-01)
- [ ] 04-03: Implement getblock verbosity=0 returning raw hex (RPC-03)
- [ ] 04-04: Implement mediantime calculation for getblockchaininfo (RPC-04)
- [ ] 04-05: Implement getblocktemplate with MTP, witness commitment, and sigoplimit (RPC-05)

## Progress

**Execution Order:**
Phases execute in numeric order: 1 → 2 → 3 → 4

| Phase | Milestone | Plans Complete | Status | Completed |
|-------|-----------|----------------|--------|-----------|
| 1. Foundation Fixes | v1.0 | 7/7 | Complete | 2026-02-20 |
| 2. Consensus Completeness | v1.0 | 3/3 | Complete | 2026-02-21 |
| 3. Peer Network Compatibility | Next | 0/5 | Not started | - |
| 4. RPC and Operator Capabilities | Next | 0/5 | Not started | - |
