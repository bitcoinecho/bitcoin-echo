# Codebase Concerns

**Analysis Date:** 2026-02-20

## Tech Debt

**Incomplete Chainstate Rollback (Reorg Handling):**
- Issue: Reorg implementation doesn't actually undo chainstate changes when a fork occurs
- Files: `src/node/chaser_confirm.c:250`
- Impact: Chain reorg logic notifies height changes but doesn't revert UTXO state or block validation data. On mainnet forks, confirmed state becomes inconsistent with stored blocks.
- Fix approach: Implement proper UTXO set undo via delta log system (already partially designed in `src/consensus/chainstate.c:736-741`). Store previous chainwork snapshots or implement delta replay mechanism.

**Incomplete Tapscript Support (OP_CHECKSIGADD):**
- Issue: OP_CHECKSIGADD (BIP-342 Tapscript opcode) not implemented, returns SCRIPT_ERR_BAD_OPCODE
- Files: `src/consensus/script.c:3331`
- Impact: Cannot validate Taproot scripts with batch signature verification. Script validation fails for Taproot transactions using this opcode.
- Fix approach: Implement Tapscript batch signature verification following BIP-342 spec. Requires secp256k1 batch verification context integration.

**Incomplete RBF (Replace-By-Fee) Logic:**
- Issue: Full RBF replacement logic not implemented; conflicts are rejected even with RBF signaling
- Files: `src/protocol/mempool.c:799`
- Impact: Mempool cannot accept legitimate RBF replacements. User transactions cannot be fee-bumped once in mempool.
- Fix approach: Implement BIP-125 RBF rules: verify fee increase, check descendant count limits, validate replacement constraints.

**Missing Transaction Indexing for RPC:**
- Issue: RPC `getrawtransaction` for confirmed txs not implemented; only searches mempool
- Files: `src/app/rpc.c:1897`
- Impact: Users cannot query historical transactions. `getrawtransaction` only works for unconfirmed transactions.
- Fix approach: Implement transaction index in block storage layer (similar to Bitcoin Core's txindex). Store transaction hash -> block position mapping.

**Incomplete RPC `getblock` Implementation:**
- Issue: Raw hex block serialization for `getblock` (verbosity=0) not implemented
- Files: `src/app/rpc.c:1783`
- Impact: RPC clients cannot retrieve raw block hex format. Only parsed block info available.
- Fix approach: Implement block serialization from storage, return hex-encoded bytes.

**Missing Median Time Past (MTP) Query:**
- Issue: `getblockchaininfo` mediantime always returns 0
- Files: `src/app/rpc.c:1662`
- Impact: RPC clients cannot determine network time consensus. May affect application-level time-dependent logic.
- Fix approach: Query block index for last 11 blocks, calculate median timestamp.

**Tuning Constants Not Calibrated:**
- Issue: Download manager peer eviction threshold `SLOWEST_EVICTION_MIN_RATE` hardcoded to 0.0 KB/s with TODO to tune based on IBD monitoring
- Files: `src/protocol/sync.c:1654`
- Impact: Peer eviction during IBD may be too aggressive or too lenient; lacking production monitoring data.
- Fix approach: Collect IBD statistics on peer download rates, analyze distribution, set threshold at appropriate percentile.

**Checkpoint Configuration Not Used:**
- Issue: `top_checkpoint` hardcoded to 0 in chaser initialization; should be configurable
- Files: `src/node/chaser_validate.c:178`, `src/node/chaser_confirm.c:66`
- Impact: Cannot use Bitcoin Core checkpoints for faster validation or consensus safety during IBD.
- Fix approach: Read checkpoint configuration from config file; integrate with validation bypass logic.

**Block Hash Not Retrieved During Validation:**
- Issue: Chaser validation submits empty hash (all zeros) due to missing database query
- Files: `src/node/chaser_validate.c:517`
- Impact: Validation tracking doesn't use actual block hashes; cannot correlate with network peers or verify integrity.
- Fix approach: Query block index database for hash at given height.

**Logging System Integration Incomplete:**
- Issue: Chaser fault handler TODO comment indicates logging system integration not finished
- Files: `src/node/chaser.c:183`
- Impact: Critical failures may not be properly logged with context before shutdown.
- Fix approach: Integrate fault handler with log system to emit error details before shutdown signal.

**Storage Callback Not Implemented for GAP Detection:**
- Issue: Comment in `src/app/node.c:1509` indicates proper storage callback for marking blocks received (not just queued) is not yet implemented
- Files: `src/app/node.c:1509`
- Impact: Synchronous writes required as workaround, hurts performance. Async storage will cause GAP errors due to race between queue completion and disk write.
- Fix approach: Implement callback-based completion tracking that fires after storage thread confirms disk write.

**Block Serving Not Implemented:**
- Issue: Full block serving disabled; node cannot provide blocks to other peers during IBD
- Files: `src/app/node.c:2769`
- Impact: Node cannot bootstrap other peers during initial block download. Network efficiency reduced.
- Fix approach: Implement block retrieval from storage and serve via getblocks/getdata protocol.

**Missing NODE_WITNESS Service Flag:**
- Issue: SegWit witness blocks not advertised; INV_WITNESS_BLOCK not used
- Files: `src/app/node.c:1762`
- Impact: Peers may not send SegWit witness data in blocks. May receive full blocks instead of compact SegWit format.
- Fix approach: Add NODE_WITNESS to service flags, use INV_WITNESS_BLOCK for block inventory during SegWit relay.

**Chainwork Storage Format (Big-Endian Issue):**
- Issue: Chainwork stored in block index but TODO comment warns proper fork handling requires big-endian format for byte-by-byte comparison
- Files: `src/storage/block_index_db.c:155`
- Impact: Fork chain selection may use SQLite's byte-comparison on little-endian values, potentially selecting wrong chain during complex reorgs.
- Fix approach: Store chainwork in big-endian format; update comparison logic in block index queries.

**Chainwork Recomputation on Reorg:**
- Issue: Chainwork not properly updated during reorg; stored as-is without delta application
- Files: `src/consensus/chainstate.c:739`
- Impact: After reorg, chainwork may not reflect actual chain difficulty history. Chain tip selection logic may be incorrect.
- Fix approach: Either store previous chainwork in delta or trigger recomputation of full chain work on reorg.

---

## Known Bugs

**BUG: Download Manager Batch Remaining Count Mismatch:**
- Symptoms: Batch marked complete (remaining=0) but unreceived blocks still in array. Batch must be returned to queue and count corrected.
- Files: `src/protocol/download_mgr.c:597-601`
- Trigger: Occurs when duplicate blocks are received from reassigned batch or batch is stolen from one peer and assigned to another before completion
- Workaround: Code detects and corrects count at runtime with LOG_ERROR. Batch continues as if it were incomplete.
- Root cause: Race condition in batch assignment/theft logic when multiple peers work on same batch or batch reassignment doesn't clear duplicate tracking

**BUG: Duplicate Address Detection in Peer Manager:**
- Symptoms: Log warning "BUG: Duplicate address..." when connecting to same address twice
- Files: `src/app/node.c:3280`
- Trigger: Connection logic adds peer before checking if address already connected
- Workaround: Logs warning but allows duplicate to proceed
- Root cause: Address deduplication check runs during connection but duplicate may still be added by race condition

---

## Security Considerations

**Memory-Safety During Block Validation:**
- Risk: Large block processing (up to 4x ECHO_MAX_BLOCK_SIZE per TODO at `src/protocol/download_mgr.c:405`) requires malloc/free. No hardened allocator.
- Files: `src/storage/blocks.c`, `src/protocol/serialize.c`, `src/consensus/block_validate.c`
- Current mitigation: Bounds checks on block size; malloc failures checked
- Recommendations:
  - Consider fixed-size allocation pools for consensus-critical blocks
  - Add integer overflow checks on size calculations
  - Test with maximum block sizes and corrupted size fields

**Taproot Script Validation Incomplete:**
- Risk: Missing OP_CHECKSIGADD means Taproot multisig cannot be validated. Node would accept invalid Taproot scripts.
- Files: `src/consensus/script.c:3331`
- Current mitigation: Returns SCRIPT_ERR_BAD_OPCODE (rejects transaction), blocking validation entirely
- Recommendations: Prioritize OP_CHECKSIGADD implementation; fuzz test with Taproot vectors from BIP-342

**Mempool Replacement Vulnerability:**
- Risk: RBF not implemented means node may evict fee-bumping transactions from mempool, silently losing user intent
- Files: `src/protocol/mempool.c:799-802`
- Current mitigation: Rejects all conflicting txs (safe but restricts functionality)
- Recommendations: Implement full BIP-125 RBF validation with ancestor fee tracking

**Thread Safety in Block File Manager:**
- Risk: Multiple FILE* handles for read/write on same file (write in `mgr->current_file`, reads via cache)
- Files: `src/storage/blocks.c:351-353` (fflush during read from same file)
- Current mitigation: Mutex protects all file operations; explicit flush before reads from write file
- Recommendations:
  - Verify no readers access write file without mutex lock
  - Consider separating read-cache updates under mutex
  - Test concurrent read/write scenarios under load

**Integer Overflow in Chainwork Arithmetic:**
- Risk: 256-bit chain work calculated in multiple places; no checked arithmetic on accumulation
- Files: `src/consensus/consensus.c`, `src/consensus/chainstate.c`
- Current mitigation: Chainwork is bounded by difficulty; overflow unlikely in practice
- Recommendations: Add WARNLINT for arithmetic operations or implement safe 256-bit add with overflow detection

---

## Performance Bottlenecks

**Synchronous Block Storage During IBD:**
- Problem: Block writing blocks validation during initial block download (GAP errors mentioned in code)
- Files: `src/app/node.c:1512`
- Cause: Async storage + callback not implemented; synchronous write required for correctness
- Improvement path: Implement storage callback system to mark blocks as durably written; decouple validation from I/O

**Download Manager Sticky Batch Racing Inefficiency:**
- Problem: When a block gets stuck, multiple peers race to download same block; wastes bandwidth
- Files: `src/protocol/download_mgr.c` (sticky batch logic)
- Cause: Necessary trade-off for robustness, but could be optimized with faster retransmit logic
- Improvement path: Implement exponential backoff + predictive peer rotation instead of racing all peers

**Block Index Lookups by Height:**
- Problem: Height-to-hash lookups use array linear search for forks
- Files: `src/consensus/chainstate.c:49-50` (height_index is simple array)
- Cause: Design prioritizes main chain (array); fork blocks require hash table lookup
- Improvement path: Benchmark fork chain lookups; consider B-tree or cache-friendly structure if fork lookups are frequent

**SQLite Block Index Queries During IBD:**
- Problem: Block index stored in SQLite (via `src/storage/block_index_db.c`); synchronous queries slow down IBD
- Files: `src/storage/block_index_db.c`
- Cause: Consensus state must be durable; SQLite provides ACID guarantees
- Improvement path: Batch block index writes; use WAL mode; profile actual query latency during IBD

---

## Fragile Areas

**Chaser Validation Worker Thread Pool:**
- Files: `src/node/chaser_validate.c`
- Why fragile:
  - Backlog queue may overflow if validators can't keep up with incoming blocks
  - Checkpoint bypass logic hardcoded to 0
  - Worker thread fault propagation via chase value (count field overloaded for error codes)
- Safe modification:
  - Ensure backlog is sized for expected worst-case latency
  - Test with stalled validators; verify backpressure propagates correctly
  - Add explicit error field to chase_value_t instead of using count
- Test coverage: `test/unit/test_chase.c` tests basic threading but not overload scenarios

**Download Manager Peer Eviction Logic:**
- Files: `src/protocol/download_mgr.c:1440-1629`
- Why fragile:
  - Multiple thresholds (SLOWEST_EVICTION_MIN_RATE, percentile checks, stall timeouts) interact
  - Minimum peer count (DOWNLOAD_MIN_PEERS_TO_KEEP=3) is critical; too low causes network isolation
  - Stall timeout escalation (backoff_count) may disconnect too many peers in cascade
- Safe modification:
  - Log all eviction decisions with peer rates and justification
  - Test with adversarial peer mix (slow + fast, stalled peers, flakey connections)
  - Verify minimum peer count empirically during IBD
- Test coverage: `test/unit/test_relay.c` tests peer interactions but not under load

**Block File Manager Mutex + File Handle Cache:**
- Files: `src/storage/blocks.c`
- Why fragile:
  - Read cache has fixed size (BLOCK_READ_CACHE_SIZE); overflow causes handle close
  - Cast-away-const pattern for mutable access in const functions (NOLINTNEXTLINE pragmas)
  - Write file handle held open across operations; flush logic critical before reads
- Safe modification:
  - Bounds-check all read cache access
  - Never modify const references without explicit cast review
  - Verify fflush() called before every cross-file read
  - Test concurrent read/write stress test
- Test coverage: `test/unit/test_block_index_db.c` has basic storage tests but not concurrent I/O

**Chainstate Reorg Delta Application:**
- Files: `src/consensus/chainstate.c:730-770`
- Why fragile:
  - Delta chain store not fully implemented (TODO at line 739)
  - Reorg requires reverting UTXO set; currently skipped (TODO at `src/node/chaser_confirm.c:250`)
  - Fork chain selection uses chainwork comparison; format issue may cause wrong chain selection
- Safe modification:
  - Never apply block without verifying previous block height/hash
  - Test reorg scenarios: simple fork, deep reorg, same-work chains
  - Verify UTXO revert correctness by comparing state before/after reorg
- Test coverage: `test/unit/test_chainstate.c` and `test/unit/test_consensus.c` test basic logic but not complex reorg scenarios

---

## Scaling Limits

**Block File Size Cap:**
- Current capacity: 128 MB per file (BLOCK_FILE_MAX_SIZE in `src/storage/blocks.c`)
- Limit: Bitcoin mainnet has ~800k blocks = ~520 GB total; would need ~4,160 files
- Scaling path: Files are append-only, OK for mainnet. For testnet with reorgs, prune strategy must handle sparse file indices.

**Height Index Array:**
- Current capacity: MAX_HEIGHT_INDEX = 1,000,000 blocks in `src/consensus/chainstate.c:26`
- Limit: Bitcoin mainnet at ~870k blocks; array sized for future growth
- Scaling path: Array allocation on startup is acceptable; sparse array for long reorganizations would waste memory. Consider dynamic growth if needed.

**Block Index Map Hash Table:**
- Current capacity: BLOCK_INDEX_MAP_DEFAULT_SIZE = 1,048,576 (1M) buckets in `src/consensus/chainstate.c:30`
- Limit: Hash table load factor not checked; collision chains may degrade lookup
- Scaling path: Implement dynamic resizing (currently fixed size). Monitor collision rate during IBD.

**Download Manager Peer List:**
- Current capacity: Fixed array in `src/protocol/download_mgr.c` (typical Bitcoin client: 8-125 connections)
- Limit: Hardcoded limits (DOWNLOAD_MIN_PEERS_TO_KEEP=3, DOWNLOAD_MAX_PEERS not found - check if unbounded)
- Scaling path: For large peer sets, consider heap-allocated dynamic array instead of fixed struct.

---

## Dependencies at Risk

**libsecp256k1 (Embedded, Vendored):**
- Risk: Library vendored in `lib/secp256k1/`. OP_CHECKSIGADD not available without custom build
- Impact: Cannot validate Taproot batch signatures without implementing custom integration
- Migration plan:
  - Check if vendored version supports batch verification context (secp256k1-zkp needed)
  - Or implement OP_CHECKSIGADD signature parsing separately, call secp256k1 for each sig

**SQLite (Embedded, Vendored):**
- Risk: Block index durability depends on SQLite ACID guarantees; version frozen
- Impact: SQLite bugs in WAL mode or concurrent access could corrupt block index
- Migration plan:
  - Monitor SQLite releases for security fixes
  - Consider custom LMDB integration if SQLite performance insufficient
  - Implement checksum validation for block index entries

---

## Missing Critical Features

**Transaction Index:**
- Problem: Cannot query confirmed transactions by hash via RPC
- Blocks: RPC `getrawtransaction` for any tx not in mempool
- Fix path: Implement txindex during block storage (hash -> file/offset); query during RPC

**Full Block Serving:**
- Problem: Node cannot bootstrap peers; blocks only downloaded, never served
- Blocks: P2P block synchronization between nodes
- Fix path: Implement block retrieval from storage; respond to `getdata` with stored blocks

**Wallet/Signing:**
- Problem: No key management, no transaction creation, no signing
- Blocks: All wallet operations (user-facing)
- Fix path: Out of scope per CLAUDE.md ("Don't: wallet code"); users bring external signer

**Mining Template Generation:**
- Problem: `getblocktemplate` RPC stubbed out; params ignored
- Blocks: Mining pool integration
- Fix path: Implement block template creation from mempool transactions

---

## Test Coverage Gaps

**Reorg Scenarios (Deep and Complex):**
- What's not tested: Multi-block reorgs, same-work chains, reorg during IBD, reorg during active mining
- Files: `src/consensus/chainstate.c`, `src/node/chaser_confirm.c`, `src/consensus/consensus.c`
- Risk: Reorg logic incomplete (delta not applied); easy to regress with untested edge cases
- Priority: **High** - Consensus-critical path

**Concurrent Block Storage and Reads:**
- What's not tested: Simultaneous block writes and reads from different threads under load
- Files: `src/storage/blocks.c`, `src/protocol/download_mgr.c`
- Risk: Race condition or mutex deadlock could cause data corruption or hang
- Priority: **High** - Can cause silent corruption

**Peer Eviction Under Load:**
- What's not tested: Download manager with 50+ peers, network latency, peer churn, adversarial slow peers
- Files: `src/protocol/download_mgr.c`
- Risk: Eviction thresholds may disconnect too many peers (network isolation) or not enough (stuck on slow peers)
- Priority: **Medium** - Affects IBD efficiency but not correctness

**Taproot Script Validation:**
- What's not tested: OP_CHECKSIGADD not implemented (errors on validation)
- Files: `src/consensus/script.c`
- Risk: Cannot validate Taproot transactions; will be common on mainnet
- Priority: **High** - Network will diverge on Taproot blocks if not implemented

**Large Block Handling:**
- What's not tested: Blocks at ECHO_MAX_BLOCK_SIZE and near 4x limit, corrupted size fields, truncated reads
- Files: `src/protocol/download_mgr.c`, `src/storage/blocks.c`
- Risk: Buffer overflow or DoS from malformed blocks
- Priority: **High** - Security-critical

**BIP-125 RBF Validation:**
- What's not tested: Replace-by-fee logic; currently disabled
- Files: `src/protocol/mempool.c`
- Risk: RBF implementation may have bugs when eventually added (unsafe for production mempool)
- Priority: **Medium** - Needed for mainnet but not critical for IBD

---

*Concerns audit: 2026-02-20*
