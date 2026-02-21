# Codebase Structure

**Analysis Date:** 2026-02-20

## Directory Layout

```
bitcoin-echo/
├── include/                    # Public API headers (42 files)
│   ├── echo_types.h           # Core types (satoshi_t, hash256_t, result codes)
│   ├── echo_config.h          # Compile-time config (network, version)
│   ├── echo_consensus.h       # Consensus frozen state interface
│   ├── consensus.h            # Consensus engine public API
│   ├── node.h                 # Node lifecycle and orchestration
│   ├── block.h                # Block structures and parsing
│   ├── tx.h                   # Transaction structures
│   ├── utxo.h                 # UTXO entry structures
│   ├── chainstate.h           # Chain state and deltas
│   ├── protocol.h             # P2P message structures
│   ├── peer.h                 # Peer connection state
│   ├── sync.h                 # Headers-first sync state
│   ├── download_mgr.h         # Work batch download coordination
│   ├── chaser.h               # Base chaser plugin interface
│   ├── chaser_validate.h      # Validation chaser
│   ├── chaser_confirm.h       # Confirmation chaser
│   ├── chase.h                # Event dispatcher
│   ├── blocks_storage.h       # Block file I/O (blk*.dat)
│   ├── utxo_db.h              # UTXO database interface
│   ├── block_index_db.h       # Block index database interface
│   ├── mempool.h              # Transaction memory pool
│   ├── discovery.h            # Peer address discovery
│   ├── rpc.h                  # RPC server interface
│   ├── platform.h             # Platform abstraction (sockets, threads, files)
│   ├── log.h                  # Logging interface
│   ├── sha256.h, ripemd160.h  # Hash functions
│   ├── sig_verify.h           # Signature verification
│   └── [22 more headers]       # Script, relay, mining, serialization, etc.
│
├── src/
│   ├── main.c                 # Entry point, event loop, signal handling
│   │
│   ├── app/                   # Application layer (node management)
│   │   ├── node.c             # Node lifecycle (create, start, stop, destroy)
│   │   ├── log.c              # Logging implementation
│   │   ├── rpc.c              # RPC server (getsyncstatus, getblockchaininfo)
│   │   └── mining.c           # Mining simulation and block generation
│   │
│   ├── consensus/             # Consensus layer (FROZEN)
│   │   ├── consensus.c        # Consensus engine orchestrator
│   │   ├── block_validate.c   # Block structure/PoW/timestamp validation
│   │   ├── tx_validate.c      # Transaction syntax/coinbase validation
│   │   ├── tx.c               # Transaction parsing and operations
│   │   ├── script.c           # Script VM (OpCode execution)
│   │   ├── sig_verify.c       # ECDSA signature verification
│   │   ├── chainstate.c       # Block application and reorg handling
│   │   ├── utxo.c             # UTXO set operations (create, spend, lookup)
│   │   ├── merkle.c           # Merkle root computation
│   │   ├── serialize.c        # Compact size and varint parsing
│   │   └── block.c            # Block parsing and operations
│   │
│   ├── protocol/              # Protocol layer (P2P networking)
│   │   ├── peer.c             # Peer connection state machine
│   │   ├── messages.c         # Message serialization/deserialization
│   │   ├── protocol_serialize.c # Protocol type serialization
│   │   ├── sync.c             # Headers-first IBD sync state
│   │   ├── download_mgr.c     # Pull-based batch work distribution
│   │   ├── relay.c            # Block/tx relay logic (when to announce)
│   │   ├── mempool.c          # Transaction memory pool
│   │   ├── discovery.c        # Peer address manager and discovery
│   │   └── serialize.c        # General serialization utilities
│   │
│   ├── node/                  # Node coordination layer (chasers)
│   │   ├── chaser.c           # Base chaser virtual interface
│   │   ├── chaser_validate.c  # Parallel block validator
│   │   ├── chaser_confirm.c   # Sequential confirmation and UTXO update
│   │   └── chase.c            # Event dispatcher (BlockReceived, BlockValidated, etc.)
│   │
│   ├── storage/               # Storage layer (persistence)
│   │   ├── blocks.c           # Block file I/O (blk*.dat read/write)
│   │   ├── utxo_db.c          # UTXO database (SQLite)
│   │   ├── block_index_db.c   # Block index database (SQLite)
│   │   └── db.c               # Generic SQLite wrapper
│   │
│   ├── crypto/                # Cryptography primitives
│   │   ├── sha256.c           # SHA256 implementation
│   │   ├── ripemd160.c        # RIPEMD160 implementation
│   │   ├── sha1.c             # SHA1 implementation (for BIP32, if used)
│   │   └── secp256k1.c        # ECDSA secp256k1 (calls external library)
│   │
│   └── platform/              # Platform abstraction
│       └── posix.c            # POSIX implementation (sockets, threads, files)
│
├── test/
│   └── unit/                  # Unit tests
│       ├── test_consensus.c   # Consensus engine tests
│       ├── test_block_validate.c
│       ├── test_tx_validate.c
│       ├── test_script.c
│       ├── test_peer.c
│       ├── test_download_mgr.c
│       ├── test_chase.c
│       ├── test_ibd_validator.c
│       ├── test_validation_queue.c
│       ├── test_log.c
│       └── test_utils.h
│
├── lib/                       # Third-party libraries
│   ├── sqlite3/               # SQLite (embedded)
│   └── secp256k1/             # libsecp256k1 (ECDSA)
│
├── scripts/                   # Build and utility scripts
│   ├── check_quality.sh       # Linting (clang-tidy)
│   └── generate_compile_commands.sh
│
├── docs/                      # Documentation
│   └── [Design documents]
│
├── Makefile                   # Build configuration
├── build.bat                  # Windows build script
├── .clangd                    # clangd LSP configuration
├── compile_commands.json      # clang-tidy configuration
├── .gitignore                 # Git ignore patterns
└── LICENSE                    # MIT license
```

## Directory Purposes

**`include/`:**
- Purpose: Public API headers—consumed by all layers
- Contains: Type definitions, struct layouts, function prototypes
- Key files: `echo_types.h` (fundamental types), `node.h` (node API), `consensus.h` (validation engine)

**`src/app/`:**
- Purpose: Application lifecycle, node management, RPC, logging
- Contains: Node creation/startup/shutdown, RPC server endpoints, log formatting
- Key responsibility: Integrate all lower layers; manage initialization sequence

**`src/consensus/`:**
- Purpose: Bitcoin protocol rules (IMMUTABLE after build)
- Contains: Block/tx/script validation, UTXO operations, chain state, proof-of-work
- Key responsibility: Pure computation—no I/O, networking, or external state
- **Compile-time consensus rules**: All BIP heights hardcoded (P2SH, SegWit, Taproot, etc.)

**`src/protocol/`:**
- Purpose: P2P networking, peer management, sync state, block download
- Contains: Peer state machine, message serialization, headers-first sync, batch work distribution
- Key responsibility: Network communication and download coordination

**`src/node/`:**
- Purpose: Event-driven coordination and chaser plugin system
- Contains: Chaser base interface, parallel validator, sequential confirmer, event dispatcher
- Key responsibility: Orchestrate independent validation workers

**`src/storage/`:**
- Purpose: Persistent storage (block files, UTXO database, block index)
- Contains: blk*.dat file I/O, SQLite schema and operations
- Key responsibility: All disk I/O happens here; maintain consistency across restarts

**`src/crypto/`:**
- Purpose: Cryptographic primitives
- Contains: Hash functions (SHA256, RIPEMD160), ECDSA signature verification
- Key responsibility: Provide pure crypto operations for validation

**`src/platform/`:**
- Purpose: OS abstraction
- Contains: Socket operations, file I/O, threading, time functions
- Key responsibility: Hide platform differences (POSIX vs. Windows)

**`test/unit/`:**
- Purpose: Unit tests for isolated components
- Contains: Test files for consensus, validation, protocol components
- Key files: `test_consensus.c`, `test_download_mgr.c`

**`lib/`:**
- Purpose: Embedded third-party code
- Contains: SQLite (UTXO/block index storage), libsecp256k1 (ECDSA)
- Note: Minimal external dependencies by design

## Key File Locations

**Entry Points:**
- `src/main.c`: Program entry point, command-line parsing, event loop, signal handling

**Configuration:**
- `include/echo_config.h`: Network selection, consensus heights, version strings
- `include/echo_platform_config.h`: Platform-specific constants

**Core Logic:**
- `include/consensus.h`, `src/consensus/consensus.c`: Validation orchestrator
- `include/chainstate.h`, `src/consensus/chainstate.c`: Block application and reorg
- `include/node.h`, `src/app/node.c`: Node orchestration

**Testing:**
- `test/unit/test_consensus.c`: Consensus validation tests
- `test/unit/test_download_mgr.c`: Batch download coordination tests
- `test/unit/test_utils.h`: Common test utilities

## Naming Conventions

**Files:**
- Source files: snake_case.c, headers: snake_case.h
- Examples: `block_validate.c`, `download_mgr.h`, `tx_validate.c`
- Test files: `test_<module>.c`

**Directories:**
- All lowercase, underscore-separated
- Examples: `src/consensus/`, `src/protocol/`, `src/node/`

**Functions:**
- Module prefix + snake_case: `utxo_db_open()`, `block_validate_pow()`, `peer_disconnect()`
- Consistency: All UTXO functions start with `utxo_`; all block storage functions start with `block_file_`

**Types:**
- Suffixed with `_t`: `hash256_t`, `block_t`, `peer_t`, `consensus_engine_t`
- Structs: lowercase with `_t` suffix
- Enums: `_t` suffix, all-caps values: `ECHO_OK`, `PEER_STATE_READY`

**Constants:**
- All-caps, underscore-separated
- Prefixed by module: `BLOCK_MAX_SIZE`, `SYNC_HEADERS_TIMEOUT_MS`, `DOWNLOAD_BATCH_SIZE`
- No magic numbers in code—constants with clear names

**Macros:**
- All-caps for clarity
- Examples: `ECHO_MAX_SATOSHIS`, `CONSENSUS_SEGWIT_HEIGHT`, `BLOCK_FILE_MAX_SIZE`

## Where to Add New Code

**New Feature (e.g., new P2P message type):**
- Message definition: `include/protocol.h`
- Serialization: `src/protocol/protocol_serialize.c`
- Handling: `src/protocol/peer.c` or appropriate protocol module
- Tests: `test/unit/test_messages.c` or protocol-focused test file

**New Consensus Rule (BIP):**
- **DO NOT ADD** to consensus layer if it changes validation rules post-activation
- Frozen consensus must be compile-time; new rules require new build
- Store historical activation heights in `include/consensus.h` as `#define CONSENSUS_BIP*_HEIGHT`
- Conditional validation: check block height against activation height in validation code

**New Database Table (UTXO-related data):**
- Schema: `src/storage/utxo_db.c` in `utxo_db_open()` CREATE TABLE statements
- Access functions: `src/storage/utxo_db.c` (lookup, insert, delete)
- Header: declare in `include/utxo_db.h`

**New Storage Format (blocks, index):**
- File format: `src/storage/blocks.c` (block file layout)
- Index format: `src/storage/block_index_db.c` (schema in CREATE TABLE)
- Header definitions: `include/blocks_storage.h`, `include/block_index_db.h`

**New Validator/Chaser (parallel processor):**
- Definition: `include/chaser_<name>.h`
- Implementation: `src/node/chaser_<name>.c`
- Registration: Add to node chaser initialization in `src/app/node.c`
- Events: Subscribe to events via dispatcher in chaser `start()` method

**Utilities (shared helpers):**
- String utilities: `src/consensus/serialize.c` or new `src/util/` directory
- Memory helpers: Avoid—prefer stack allocation and RAII via caller ownership
- Validation helpers: Keep in `src/consensus/` (part of frozen core)

## Special Directories

**`.planning/`:**
- Purpose: GSD mapping and planning documents
- Generated: Yes (by GSD orchestrator)
- Committed: Yes

**`lib/`:**
- Purpose: Vendored third-party code
- Generated: No (checked in, built statically)
- Committed: Yes (sqlite3, libsecp256k1)

**Block Storage (`~/.bitcoin-echo/blocks/`):**
- Purpose: Persistent block data (blk00000.dat, blk00001.dat, etc.)
- Generated: Yes (created at runtime)
- Committed: No (user data)

**Database Files (`~/.bitcoin-echo/`):**
- Purpose: UTXO and block index databases
- Generated: Yes (created at runtime from blockchain)
- Committed: No (user data)

---

*Structure analysis: 2026-02-20*
