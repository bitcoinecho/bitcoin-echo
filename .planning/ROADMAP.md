# Roadmap: Bitcoin Echo — Peer-Compatible Node

## Overview

Four phases ordered by hard dependency. Phase 1 eliminates active bugs and infrastructure gaps that would silently corrupt results in every later phase. Phase 2 completes consensus correctness in isolation — Tapscript and reorg handling — before the node touches the network. Phase 3 activates genuine network participation: NODE_WITNESS, block serving, and full-RBF mempool. Phase 4 equips operators and miners with the RPC surface they need. Nothing in Phases 2-4 can be correctly implemented or meaningfully tested without the preceding phases stable.

## Phases

**Phase Numbering:**
- Integer phases (1, 2, 3): Planned milestone work
- Decimal phases (2.1, 2.2): Urgent insertions (marked with INSERTED)

Decimal phases appear between their surrounding integers in numeric order.

- [ ] **Phase 1: Foundation Fixes** - Eliminate active bugs and infrastructure gaps that block all subsequent work
- [ ] **Phase 2: Consensus Completeness** - Implement OP_CHECKSIGADD and full UTXO rollback for correct chainstate
- [ ] **Phase 3: Peer Network Compatibility** - Advertise witness support, serve blocks, and enforce full-RBF
- [ ] **Phase 4: RPC and Operator Capabilities** - Transaction index, block retrieval, mediantime, and block templates

## Phase Details

### Phase 1: Foundation Fixes
**Goal**: The node runs IBD on mainnet with no active LOG_ERRORs, correct block identity tracking, correct fork selection infrastructure, decoupled I/O, and all known race conditions eliminated
**Depends on**: Nothing (first phase)
**Requirements**: BUGF-01, BUGF-02, INFR-01, INFR-02, INFR-03, INFR-04, INFR-05, CONS-03, TEST-02, TEST-03, TEST-05
**Success Criteria** (what must be TRUE):
  1. Node completes IBD from genesis to tip with zero LOG_ERROR messages in the log — no batch remaining count errors, no duplicate address errors
  2. Block hashes visible in validation logs are real block hashes, not all-zeros, for every block processed by the chaser
  3. Chainwork values stored in the block index database are big-endian blobs that sort correctly under SQLite bytewise comparison, confirmed by direct database inspection
  4. Block storage write completion is acknowledged by the download manager only after the block is confirmed durably on disk — GAP errors no longer appear during IBD
  5. Chaser fault events log the error detail to the log system before initiating shutdown — crash diagnostics are readable in the node log
**Plans**: TBD

Plans:
- [ ] 01-01: Fix download manager batch remaining count bug (BUGF-01)
- [ ] 01-02: Fix duplicate peer address detection race (BUGF-02)
- [ ] 01-03: Implement async storage write completion callback (INFR-01)
- [ ] 01-04: Wire real block hash retrieval in chaser_validate (INFR-02)
- [ ] 01-05: Wire checkpoint height from node config (INFR-03)
- [ ] 01-06: Add error logging to chaser fault handler (INFR-04)
- [ ] 01-07: Calibrate peer eviction threshold from mainnet IBD data (INFR-05)
- [ ] 01-08: Fix chainwork big-endian storage in block_index_db (CONS-03)
- [ ] 01-09: Test concurrent block storage reads and writes (TEST-02)
- [ ] 01-10: Test peer eviction under load (TEST-03)
- [ ] 01-11: Test large block handling at max size and near-limit (TEST-05)

### Phase 2: Consensus Completeness
**Goal**: The node correctly validates all Taproot multisig transactions and correctly follows the longest chain through reorganizations with UTXO state consistency guaranteed
**Depends on**: Phase 1
**Requirements**: CONS-01, CONS-02, CONS-04, CONS-05, TEST-01, TEST-04
**Success Criteria** (what must be TRUE):
  1. Node accepts and validates Taproot multisig transactions (OP_CHECKSIGADD) that Bitcoin Core accepts, including transactions using unknown key types that must succeed per BIP-342 upgrade rule — currently these return SCRIPT_ERR_BAD_OPCODE
  2. Node passes all BIP-342 Tapscript reference test vectors without failures, including the unknown-key-type vectors
  3. After a synthetic 6-block chain reorganization, the node's UTXO set matches the expected state for the new chain — no outputs from the orphaned chain remain, all outputs from the winning chain are present
  4. After a reorg, the tip's chainwork value in the block index reflects the winning chain's accumulated work, not the orphaned chain's stale value
**Plans**: TBD

Plans:
- [ ] 02-01: Implement OP_CHECKSIGADD in script.c with BIP-342 key-type branching (CONS-01)
- [ ] 02-02: Implement full UTXO rollback via delta undo in chaser_confirm (CONS-02)
- [ ] 02-03: Add prev_chainwork to block_delta_t and revert on reorg (CONS-04)
- [ ] 02-04: Test Taproot script validation with BIP-342 reference vectors (CONS-05, TEST-04)
- [ ] 02-05: Test reorg scenarios: simple fork, deep reorg (6+ blocks), same-work chains (TEST-01)

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

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 1. Foundation Fixes | 0/11 | Not started | - |
| 2. Consensus Completeness | 0/5 | Not started | - |
| 3. Peer Network Compatibility | 0/5 | Not started | - |
| 4. RPC and Operator Capabilities | 0/5 | Not started | - |
