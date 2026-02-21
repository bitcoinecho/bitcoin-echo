# Architecture

**Analysis Date:** 2026-02-20

## Pattern Overview

**Overall:** Layered architecture with strict separation between consensus (frozen core), protocol/networking, and application management.

**Key Characteristics:**
- **Pure Consensus Core**: Consensus layer performs no I/O, networking, or dynamic allocation during validation—deterministic pure computation
- **Frozen Rules**: Bitcoin consensus rules (P2SH, BIP-66, SegWit, Taproot) are compile-time constants, immutable after build
- **Headers-First IBD**: Initial Block Download uses headers-first synchronization with parallel block download coordination
- **Pull-Based Work Distribution**: Block download uses cooperative batch model where peers pull work rather than being pushed assignments
- **Atomic Chain State**: Block application and reversal are atomic operations with delta tracking for reorg support
- **SQLite Storage**: UTXO set and block index stored in SQLite with WAL mode for consistency

## Layers

**Application Layer (App):**
- Purpose: Node lifecycle, RPC interface, event loop orchestration
- Location: `src/app/` (node.c, rpc.c, log.c, mining.c)
- Contains: Node initialization/startup/shutdown, RPC server, logging, signal handling
- Depends on: All other layers (protocol, storage, consensus)
- Used by: `src/main.c` entry point

**Protocol Layer (P2P & Sync):**
- Purpose: Network communication, peer management, headers-first sync, block download coordination
- Location: `src/protocol/` (peer.c, sync.c, download_mgr.c, messages.c, relay.c, mempool.c, discovery.c)
- Contains: Peer connection state machines, message serialization, sync state tracking, batch-based download work queue
- Depends on: Consensus layer (for validation), Storage layer (for block persistence)
- Used by: Node lifecycle management, event processing loop

**Node/Chaser Layer (Coordination):**
- Purpose: Event-driven coordination of parallel validators, chain confirmation, and plugin architecture
- Location: `src/node/` (chaser.c, chaser_validate.c, chaser_confirm.c, chase.c)
- Contains: Chaser base interface (virtual methods), event dispatcher, concurrent validation workers, confirmation tracking
- Depends on: Consensus engine (for block validation), Storage (for block loading)
- Used by: Main event loop for coordinating validation pipeline

**Consensus Layer (FROZEN - Core Rules):**
- Purpose: Bitcoin protocol validation—no I/O, networking, or dynamic allocation allowed
- Location: `src/consensus/` (consensus.c, block_validate.c, tx_validate.c, script.c, chainstate.c, utxo.c, merkle.c, sig_verify.c)
- Contains: Block/tx/script validation, UTXO set operations, chain state transitions, PoW verification
- Depends on: Crypto layer only (hashing, signatures)
- Used by: Protocol layer (block arrival), Storage layer (historical verification), Chasers (parallel validation)
- **IMMUTABLE**: All consensus rules hardcoded at compile-time; never loaded from external config

**Storage Layer (Persistence):**
- Purpose: Block files, UTXO database, block index—all I/O happens here
- Location: `src/storage/` (blocks.c, utxo_db.c, block_index_db.c, db.c)
- Contains: Block file format (blk*.dat), SQLite UTXO table, block metadata indexing, file position tracking
- Depends on: Consensus (for validation context), Platform (for file I/O)
- Used by: Application layer (startup/shutdown), Protocol layer (block persistence), Consensus (UTXO lookups)

**Crypto Layer (Primitives):**
- Purpose: Hash functions and signature verification
- Location: `src/crypto/` (sha256.c, ripemd160.c, sha1.c, secp256k1.c)
- Contains: SHA256d, RIPEMD160, ECDSA secp256k1, signature verification
- Depends on: Nothing (stdlib only)
- Used by: Consensus (signature verification, PoW checking), all layers (hashing)

**Platform Abstraction (System Calls):**
- Purpose: Abstract OS differences for sockets, threads, file I/O, time
- Location: `src/platform/posix.c` (POSIX implementation; Windows via build config)
- Contains: Socket operations, threading primitives, file operations, clock functions
- Depends on: Nothing (OS-level only)
- Used by: All layers

## Data Flow

**Initial Block Download (IBD) - Full Block Download Sequence:**

1. **Header Sync Phase** (`sync.c`, `peer.c`):
   - Main loop calls `node_process_peers()` → processes peer messages
   - On version handshake: peer added to sync candidates
   - Sync manager sends getheaders to multiple peers in parallel
   - Peer responds with up to 2000 headers in headers message
   - Headers validated via `consensus_validate_block()` (header-only, no tx validation)
   - Validated headers added to block index, chainwork accumulated
   - Sync continues until no peer has headers ahead of us

2. **Block Download Coordination** (`download_mgr.c`):
   - Once headers synced, download manager creates work batches (64 blocks each)
   - Each outbound peer PULLS a batch when idle (requests getdata for batch blocks)
   - Peer downloads blocks and sends block messages back
   - Download manager tracks: bytes_received, time_window → calculates peer throughput
   - If peer slow (< 3 KB/s after grace period) and other peers available: disconnect
   - Sticky batches created for blocks blocking completion (held by slow peers)

3. **Block Validation & Application** (`chaser_validate.c`, `chaser_confirm.c`):
   - **Parallel Validation** (chasers):
     - Chasers subscribe to BlockDownloaded events
     - Each chaser independently validates blocks via `consensus_validate_block()`
     - No UTXO modifications during validation (read-only consensus engine)
     - Validated block posted as BlockValidated event
   - **Sequential Application** (chaser_confirm):
     - Confirmation chaser consumes BlockValidated events in height order
     - For each block: `node_apply_block()` → updates UTXO set atomically
     - Block serialized to blk*.dat file, position recorded in block index
     - Block index and UTXO database updated in single SQLite transaction
     - Applied blocks posted as BlockApplied event
     - Chain tip updated, chainwork recalculated

4. **Reorg Handling** (`chainstate.c`):
   - When new block with higher chainwork announced but doesn't extend tip
   - Consensus engine maintains block index of all known headers
   - Deltas stored in memory (550 blocks for reorg safety margin)
   - On reorg: parent block found, deltas rewound in reverse, new fork applied
   - UTXO changes atomic: all deltas applied or none

**State Management:**

```
Peer State:
  DISCONNECTED → CONNECTING → CONNECTED → HANDSHAKE_SENT → HANDSHAKE_RECV → READY → DISCONNECTING

Sync State:
  IDLE → HEADERS (download headers) → BLOCKS (download blocks) → DONE (steady-state)

Node State:
  UNINITIALIZED → INITIALIZING → STARTING → RUNNING → STOPPING → STOPPED

Chaser State (per chaser instance):
  Created → Subscribed → Processing heights → Closed
```

## Key Abstractions

**Block Index (`block_index_t`, `block_index_db.c`):**
- Purpose: Track all known block headers (on-chain and orphans) with accumulated work
- Each entry stores: hash, prev_hash, height, timestamp, bits, chainwork, file position
- Linked list of block_index_t pointers enables fork tracking
- Block index database (SQLite) persists across restarts
- On-disk position recorded: file_index (blk*.dat number) + offset

**UTXO Entry (`utxo_entry_t`, `utxo_db.c`):**
- Purpose: Single unspent output with value, scriptPubKey, creation height, coinbase flag
- Stored in SQLite table keyed by outpoint (txid || vout)
- Looked up during tx validation, deleted when spent
- Batch insert/delete within transaction boundaries

**Block Delta (`block_delta_t`, `chainstate.c`):**
- Purpose: Undo data for reverting block application during reorg
- Created array: all outputs created by this block (for undo on revert)
- Spent array: all UTXO entries spent (original state, for reapplication on revert)
- Stored in memory ring buffer of 550 blocks
- Enables atomic reorg: apply parent deltas in reverse, new fork deltas forward

**Work Batch (`work_batch_t`, `download_mgr.c`):**
- Purpose: Unit of block download work assigned to peers
- Contains: block hashes, heights, expected count, received bitmap
- Sticky flag: if set, batch stays in queue and clones to new peers (for slow blocks)
- Peer pulls batch when idle; batch complete when all blocks received

**Chaser (`chaser_t`, `chaser.h`):**
- Purpose: Plugin architecture for coordinating independent processors (validation, confirmation, etc.)
- Virtual method table: start, handle_event, stop, destroy
- Subscribes to chain state events (BlockDownloaded, BlockValidated, etc.)
- Maintains position (current height being processed) for resumption after crash

**Event Dispatcher (`chase_dispatcher_t`, `chase.c`):**
- Purpose: Central event bus for chaser coordination
- Events: BlockReceived, BlockValidated, BlockApplied, BlockFinal
- Chasers subscribe to event types; dispatcher delivers events
- All synchronous (no queuing between chasers on main event loop thread)

## Entry Points

**`src/main.c`:**
- Location: `src/main.c`
- Triggers: Process startup
- Responsibilities:
  - Parse command-line arguments (datadir, prune, network, log level)
  - Create node via `node_create()`
  - Start node via `node_start()`
  - Enter main event loop: repeatedly call `node_process_peers()`, `node_process_blocks()`, `node_maintenance()`
  - Handle SIGINT/SIGTERM for graceful shutdown
  - Call `node_stop()` and `node_destroy()`

**Node Initialization (`node_create`, `src/app/node.c`):**
- Location: `src/app/node.c`
- Triggers: Called from `main()` after config parsing
- Responsibilities:
  - Create data directory if needed
  - Open/create UTXO database (SQLite)
  - Open/create block index database
  - Initialize block file manager (scan blk*.dat for write position)
  - Create consensus engine and restore tip from block index
  - Initialize mempool
  - Initialize peer discovery

**Event Loop (`node_process_peers`, `node_process_blocks`, `src/app/node.c`):**
- Location: `src/app/node.c`
- Triggers: Called repeatedly from `main()`
- Responsibilities (peers): Accept inbound, check outbound, receive messages, send queued messages
- Responsibilities (blocks): Dequeue received blocks, validate via chasers, confirm in order
- Returns: ECHO_OK always (errors logged, peer disconnected on error)

## Error Handling

**Strategy:** Specific error codes returned; consensus validation produces detailed result struct with failing index and error type.

**Patterns:**

1. **Validation Errors** (`consensus_result_t`):
   - `consensus_error_t`: enum of all possible consensus failures (block PoW, tx script, etc.)
   - `failing_index`: which transaction (or input within tx) failed
   - `block_error`, `tx_error`, `script_error`: nested detail structs
   - Consumers can determine: "block failed: tx 3 failed: input 1 script verification failed"

2. **I/O Errors** (storage, network):
   - Specific codes: ECHO_ERR_PLATFORM_IO, ECHO_ERR_PLATFORM_NET
   - Logged and peer disconnected on network error
   - Block storage error triggers node shutdown (invariant: if we received block, it must persist)

3. **Consensus Invariant Violations**:
   - If block passes validation but application fails (UTXO missing), node panics
   - This indicates corrupt storage or logic bug—crash is correct response

4. **Mempool/Peer Errors**:
   - Unrelayed tx: ignored (will download when block arrives)
   - Invalid tx: peer disconnected (DoS protection)
   - Unknown UTXO in mempool tx: stored anyway (may arrive in future block)

## Cross-Cutting Concerns

**Logging:**
- Framework: Custom `log_*()` functions in `src/app/log.c`
- Approach: printf-style with log levels (ERROR, WARN, INFO, DEBUG)
- Configuration: Compile-time network selection (mainnet/testnet/regtest); runtime log level flag

**Validation:**
- Consensus engine is the single source of truth
- Block validation is header-only (structure check, PoW, timestamp, merkle root, coinbase)
- Transaction validation is context-aware (UTXO existence, script execution, value conservation)
- Script validation via VM in `script.c`

**Authentication:**
- No authentication mechanism—Bitcoin P2P is permissionless
- DoS protection via: rate limiting peer messages, disconnecting slow peers, banning misbehaving peers

**Pruning:**
- Automatic pruning triggered after each block applied if disk usage exceeds target
- Deletes entire blk*.dat files, updates block index to mark blocks as pruned
- Safety margin: never prunes last 550 blocks (reorg safety)
- Manual control: `node_prune_blocks()` for explicit pruning

**Thread Safety:**
- Main event loop is single-threaded
- Platform layer provides pthread mutexes for critical sections (block file manager, UTXO DB transactions)
- Chasers execute on main thread during events (no background threads currently)
- Future: could spawn chaser worker threads with queue per chaser

---

*Architecture analysis: 2026-02-20*
