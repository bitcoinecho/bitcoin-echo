# Roadmap: Bitcoin Echo — Peer-Compatible Node

## Milestones

- ✅ **v1.0** — Phases 1-2: Foundation Fixes + Consensus Completeness (shipped 2026-02-21)
- 🚧 **v1.1 Network Participant** — Phases 3-6: P2P Block Serving, RBF Mempool, Storage + Core RPC, Mining Integration (in progress)

## Phases

<details>
<summary>✅ v1.0 (Phases 1-2) — SHIPPED 2026-02-21</summary>

- [x] Phase 1: Foundation Fixes (7/7 plans) — completed 2026-02-20
- [x] Phase 2: Consensus Completeness (3/3 plans) — completed 2026-02-21

See: `.planning/milestones/v1.0-ROADMAP.md` for full details

</details>

### 🚧 v1.1 Network Participant (In Progress)

**Milestone Goal:** Make Echo a real network participant — serve blocks to peers, handle BIP-125 full-RBF mempool policy, and expand RPC for block queries and mining pool integration.

#### Phase 3: P2P Block Serving

- [x] **Phase 3: P2P Block Serving** — Node advertises witness capability, serves blocks to peers, and uses correct inventory types (completed 2026-02-21)

#### Phase 4: BIP-125 Full-RBF Mempool

- [x] **Phase 4: BIP-125 Full-RBF Mempool** — Mempool enforces all 5 BIP-125 replacement rules with full test coverage (completed 2026-02-21)

#### Phase 5: Storage Layer and Core RPC

- [ ] **Phase 5: Storage Layer and Core RPC** — Transaction index built, confirmed tx lookup live, getblock and mediantime return real data

#### Phase 6: getblocktemplate and submitblock

- [ ] **Phase 6: getblocktemplate and submitblock** — Complete mining workflow: generate template with correct witness commitment, submit mined block

## Phase Details

### Phase 3: P2P Block Serving
**Goal**: Echo is a genuine Bitcoin network participant — it advertises witness capability, serves witness-serialized blocks to requesting peers, and uses the correct inventory type when announcing blocks to witness-capable peers
**Depends on**: Phase 2
**Requirements**: P2P-01, P2P-02, P2P-04
**Success Criteria** (what must be TRUE):
  1. Node's version message includes the NODE_WITNESS service flag (bit 3 set) — peers that inspect the services field see it correctly and send witness-serialized inventory in response
  2. A peer that sends getdata with MSG_WITNESS_BLOCK for a known block hash receives the full witness-serialized block back — a peer that sends MSG_BLOCK receives the stripped legacy serialization, not a witness block
  3. Node announces new blocks to witness-capable peers using INV_WITNESS_BLOCK (0x40000002), not INV_BLOCK — legacy peers still receive INV_BLOCK announcements
  4. Serving a block that is no longer in the local block store returns a notfound message, not silence or a crash
**Plans**: 2 plans (complete)

Plans:
- [x] 03-01-PLAN.md — NODE_WITNESS service flag + INV_WITNESS_BLOCK inventory type for announcements and IBD requests (P2P-01, P2P-04)
- [x] 03-02-PLAN.md — getdata block handler: serve witness/legacy blocks, notfound for pruned/unknown (P2P-02)

### Phase 4: BIP-125 Full-RBF Mempool
**Goal**: Echo's mempool correctly enforces all 5 BIP-125 replacement rules, making it compatible with Bitcoin Core v28+ full-RBF default policy and enabling accurate fee-market transaction selection for getblocktemplate
**Depends on**: Phase 3
**Requirements**: P2P-03, TEST-01
**Success Criteria** (what must be TRUE):
  1. A transaction that signals RBF (nSequence < 0xFFFFFFFE on at least one input) and satisfies all 5 BIP-125 rules replaces its conflicting predecessor in the mempool
  2. A replacement that violates any of the 5 rules is rejected with a specific error identifying which rule failed — it does not silently accept or panic
  3. Rule 3 is enforced on absolute fee totals, not feerates — a replacement paying 4999 sats that evicts a 5000-sat original is rejected even if the replacement's feerate is 10x higher
  4. All 5 replacement rules are exercised by the test suite, including inherited RBF signaling propagation to descendants and the 100-transaction eviction count limit
**Plans**: 2 plans

Plans:
- [x] 04-01-PLAN.md — Implement BIP-125 full-RBF replacement logic: eviction-set builder, inherited signaling check, 5-rule validator, atomic eviction (P2P-03)
- [ ] 04-02-PLAN.md — BIP-125 RBF test suite: all 5 rules with edge cases including absolute-fee trap, inherited signaling, and eviction count limit (TEST-01)

### Phase 5: Storage Layer and Core RPC
**Goal**: Operators can look up any confirmed transaction by txid, retrieve raw block hex, and read a correct mediantime — all backed by a durable transaction index that survives chain reorganizations
**Depends on**: Phase 3
**Requirements**: RPC-01, RPC-02, RPC-03, RPC-04
**Success Criteria** (what must be TRUE):
  1. getrawtransaction called with a confirmed txid returns the raw hex-encoded transaction — calls for txids in pruned blocks return not-found, matching Bitcoin Core pruning behavior
  2. getblock called with a block hash and verbosity=0 returns the raw witness-serialized hex for that block
  3. getblockchaininfo returns a mediantime that equals the median of the previous 11 block timestamps at the current chain tip — it does not return 0
  4. After a chain reorganization, getrawtransaction for a transaction in a disconnected block returns not-found — stale txindex entries from the reorg are removed in the same SQLite transaction as the UTXO delta rollback
**Plans**: 2 plans

Plans:
- [ ] 05-01-PLAN.md — tx_index table + CRUD functions + population in node_apply_block + deletion in chaser_confirm reorg (RPC-01)
- [ ] 05-02-PLAN.md — Wire getblock v=0 raw hex, mediantime MTP calculation, getrawtransaction confirmed tx lookup (RPC-02, RPC-03, RPC-04)

### Phase 6: getblocktemplate and submitblock
**Goal**: Mining pools and operators can request a valid block template with a correct SegWit witness commitment and submit a mined block — Echo is usable as mining pool infrastructure
**Depends on**: Phase 5
**Requirements**: RPC-05, RPC-06
**Success Criteria** (what must be TRUE):
  1. getblocktemplate returns a template with correct BIP-22/BIP-145 fields: real MTP-derived mintime, "rules": ["segwit"], and a default_witness_commitment hex script (not a pre-built coinbase output)
  2. The witness commitment in the template is computed from the wtxid merkle root (coinbase wtxid = 32 zero bytes; all other transactions use wtxid, not txid)
  3. getblocktemplate returns RPC error -28 while the node is still syncing — it does not serve a partial template
  4. A block constructed from the template and submitted via submitblock is accepted by Echo and results in that block appearing at the chain tip
**Plans**: TBD

Plans:
- [ ] 06-01: Implement getblocktemplate with real MTP mintime, mempool transaction selection, witness commitment computation using wtxid merkle root (RPC-05)
- [ ] 06-02: Implement submitblock — deserialize, validate, and apply the submitted block (RPC-06)

## Progress

**Execution Order:**
Phases execute in numeric order: 1 → 2 → 3 → 4 → 5 → 6

| Phase | Milestone | Plans Complete | Status | Completed |
|-------|-----------|----------------|--------|-----------|
| 1. Foundation Fixes | v1.0 | 7/7 | Complete | 2026-02-20 |
| 2. Consensus Completeness | v1.0 | 3/3 | Complete | 2026-02-21 |
| 3. P2P Block Serving | v1.1 | 2/2 | Complete | 2026-02-21 |
| 4. BIP-125 Full-RBF Mempool | 2/2 | Complete   | 2026-02-21 | - |
| 5. Storage Layer and Core RPC | v1.1 | 0/2 | Not started | - |
| 6. getblocktemplate and submitblock | v1.1 | 0/2 | Not started | - |
