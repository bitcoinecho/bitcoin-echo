# Architecture Research

**Domain:** Bitcoin full node peer compatibility — layered C11 implementation
**Researched:** 2026-02-20
**Confidence:** HIGH (direct codebase analysis, no external sources needed)

---

## Standard Architecture

### System Overview

The existing layered architecture with App > Protocol > Consensus (frozen) >
Platform holds unchanged. This milestone adds work within each layer without
restructuring layers. The new concerns slot into specific existing subsystems.

```
┌─────────────────────────────────────────────────────────────────────┐
│                     APPLICATION LAYER  (src/app/)                    │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────────────┐  │
│  │  node.c  │  │  rpc.c   │  │ mining.c │  │      log.c         │  │
│  │          │  │(+txindex)│  │(+block   │  │                    │  │
│  │(+block   │  │(+mediantp│  │ template)│  │(+fault handler)    │  │
│  │ serving) │  │+getrawt) │  │          │  │                    │  │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────────────────────┘  │
│       │              │              │                                  │
├───────┴──────────────┴──────────────┴──────────────────────────────── │
│              PROTOCOL LAYER  (src/protocol/)                          │
│  ┌───────────┐  ┌──────────────┐  ┌───────────┐  ┌──────────────┐   │
│  │ mempool.c │  │download_mgr.c│  │  peer.c   │  │   sync.c     │   │
│  │  (+RBF)  │  │ (+batch bug) │  │(+dedup fix│  │  (+WITNESS   │   │
│  │           │  │              │  │            │  │   flag)      │   │
│  └────┬──────┘  └──────┬───────┘  └─────┬─────┘  └──────────────┘   │
│       │                │                 │                             │
├───────┴────────────────┴─────────────────┴─────────────────────────── │
│           NODE / CHASER COORDINATION LAYER  (src/node/)               │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐   │
│  │ chaser_validate.c│  │ chaser_confirm.c │  │   chaser.c       │   │
│  │ (+real hash,     │  │ (+UTXO undo,     │  │ (+fault handler  │   │
│  │  +checkpoint cfg)│  │  +reorg execute, │  │  logging)        │   │
│  │                  │  │  +checkpoint cfg)│  │                  │   │
│  └────┬─────────────┘  └────┬─────────────┘  └──────────────────┘   │
│       │                     │                                          │
├───────┴─────────────────────┴──────────────────────────────────────── │
│            CONSENSUS LAYER  (src/consensus/)  *** FROZEN ***          │
│  ┌───────────────┐  ┌─────────────────┐  ┌──────────────────────┐   │
│  │  script.c     │  │  chainstate.c   │  │ block_validate.c     │   │
│  │(+CHECKSIGADD) │  │(+chainwork BE,  │  │                      │   │
│  │               │  │ +delta persist) │  │                      │   │
│  └───────────────┘  └─────────────────┘  └──────────────────────┘   │
│  ┌───────────────┐  ┌─────────────────┐                              │
│  │  tx_validate.c│  │   utxo.c        │                              │
│  └───────────────┘  └─────────────────┘                              │
├─────────────────────────────────────────────────────────────────────── │
│            STORAGE LAYER  (src/storage/)                              │
│  ┌──────────────┐  ┌─────────────┐  ┌──────────────────────────┐    │
│  │  blocks.c    │  │  utxo_db.c  │  │  block_index_db.c        │    │
│  │ (+async      │  │             │  │  (+big-endian chainwork,  │    │
│  │  callbacks)  │  │             │  │   +transaction index)     │    │
│  └──────────────┘  └─────────────┘  └──────────────────────────┘    │
├─────────────────────────────────────────────────────────────────────── │
│          CRYPTO LAYER  (src/crypto/)    PLATFORM (src/platform/)      │
│          Unchanged                      Unchanged                      │
└─────────────────────────────────────────────────────────────────────── │
```

### Component Responsibilities

The table below maps each concern from CONCERNS.md to the component that owns
the fix and what other components it touches.

| Component | Primary Concern | Communicates With |
|-----------|----------------|-------------------|
| `src/consensus/script.c` | OP_CHECKSIGADD (BIP-342 Tapscript) | `crypto/secp256k1` (Schnorr batch verify) |
| `src/consensus/chainstate.c` | Chainwork big-endian format; delta application on reorg | `storage/block_index_db.c` (persistence), `node/chaser_confirm.c` (reorg trigger) |
| `src/node/chaser_confirm.c` | UTXO undo execution via `chain_reorganize()`; checkpoint config | `consensus/chainstate.c` (apply/revert), `storage/utxo_db.c` (persistence), `node/chaser.c` (fault logging) |
| `src/node/chaser_validate.c` | Real block hash retrieval; checkpoint config from node config | `storage/block_index_db.c` (hash lookup), `node/chaser.c` (fault logging) |
| `src/node/chaser.c` | Fault handler logging integration | `app/log.c` |
| `src/protocol/mempool.c` | BIP-125 RBF (replace-by-fee) | `consensus/tx_validate.c` (validation), `consensus/chainstate.c` (UTXO lookup) |
| `src/protocol/download_mgr.c` | Batch remaining count bug | Internal (self-contained fix) |
| `src/app/node.c` | Block serving (getdata responder); NODE_WITNESS service flag; async storage callbacks; duplicate peer address detection | `storage/blocks.c` (block read), `protocol/peer.c` (send message), `protocol/sync.c` (service flags) |
| `src/app/rpc.c` | getblock verbosity=0 hex; getrawtransaction for confirmed; getblockchaininfo mediantime; getblocktemplate | `storage/block_index_db.c` (tx index lookup), `consensus/chainstate.c` (MTP), `protocol/mempool.c` (template selection) |
| `src/storage/block_index_db.c` | Chainwork big-endian storage; transaction index schema | `consensus/chainstate.c` (work format), `app/rpc.c` (tx lookup) |
| `src/storage/blocks.c` | Async write callback; block serving read path | `app/node.c` (callback notification), `protocol/peer.c` (serve on getdata) |
| `src/echo_config.h` / `src/app/node.c` | Checkpoint configuration from config (not hardcoded 0) | `node/chaser_validate.c`, `node/chaser_confirm.c` |

---

## Component Boundaries

### Four Isolation Rules That Must Hold

1. **Consensus layer never calls Storage.** Consensus (`chainstate.c`,
   `script.c`, `tx_validate.c`) may not perform any I/O. OP_CHECKSIGADD
   implementation is pure computation — secp256k1 Schnorr verify only.
   No disk reads in consensus.

2. **Reorg coordination lives in chaser_confirm, not chainstate.**
   `chain_reorganize()` in `chainstate.c` already provides the API (it accepts
   a `get_block_txs_fn` callback). `chaser_confirm.c` is responsible for
   calling it with the correct block loading callback. The split: consensus
   provides the state machine; chaser_confirm provides the orchestration.

3. **Block serving is an Application layer concern.** The getdata handler in
   `node.c` reads from `blocks.c` storage and serializes via
   `protocol_serialize.c`. It does not touch consensus. If the block is pruned,
   it returns `notfound`.

4. **Mempool is Policy, not Consensus.** RBF logic in `mempool.c` may differ
   between nodes without causing chain divergence. The fix modifies acceptance
   policy only — no consensus rule changes.

### Boundary Violations to Avoid

Do not add I/O into `chainstate.c` to support chainwork persistence. The right
pattern: `chainstate.c` computes big-endian-comparable chainwork in memory;
`block_index_db.c` stores it in the correct byte order. The format fix is in
the storage layer's serialization, not the consensus struct itself.

---

## Data Flow Changes Needed

### 1. Reorg / Chainstate Rollback

**Current (broken) flow:**
```
chaser_confirm → chainstate_apply_block() [new chain]
             → (TODO: skip undo of old chain entirely)
```

**Correct flow after fix:**
```
chaser_confirm detects fork (new block doesn't extend tip)
    ↓
chain_reorg_create(current_tip, new_tip) → chain_reorg_t plan
    ↓
For each block in disconnect list:
    chaser_confirm → load block from storage (node_load_block)
    → chainstate_revert_block(state, stored_delta)
    → mempool_readd_for_disconnect(mempool, block)
    ↓
For each block in connect list:
    chaser_confirm → node_load_block(hash)
    → consensus_validate_block()   [read-only]
    → chainstate_apply_block()     [UTXO update, creates delta]
    → node_store_block()           [persist]
    ↓
Update block index on-chain markers
Emit CHASE_ORGANIZED event
```

Critical data flow: the `chain_reorganize()` function signature already
accepts a `get_block_txs_fn` callback. `chaser_confirm.c` must provide that
callback backed by `node_load_block()`. The function prototype is defined;
the implementation is the missing piece.

### 2. Async Storage / GAP Errors

**Current (forced synchronous) flow:**
```
chaser_confirm → node_store_block() [blocks until disk write complete]
             → marks block received
             → signals download_mgr
```

**Correct async flow after fix:**
```
chaser_confirm → node_store_block_async(block, callback, ctx)
    ↓
Storage thread writes to disk
    ↓
Callback fires on completion: marks block received → signals download_mgr
```

The callback signature needed: `void (*on_stored)(echo_result_t result, void *ctx)`.
The `block_file_manager_t` already has a mutex; the callback fires after
`fflush()` from the storage thread. This decouples validation latency from
disk I/O latency. Impact: download_mgr no longer sees GAP errors from async
race; `node.c` gets a new `node_store_block_async()` entry point.

### 3. Block Serving (getdata responder)

**Current (stub) flow:**
```
Peer sends getdata(INV_BLOCK, hash)
    ↓
node.c → TODO comment → returns without serving
```

**Correct flow after fix:**
```
Peer sends getdata(INV_BLOCK or INV_WITNESS_BLOCK, hash)
    ↓
node.c:
    1. block_index_map_lookup(hash) → check if known
    2. Check if block is pruned (block_index.data_file == BLOCK_DATA_NOT_STORED)
       → if pruned: send notfound message, done
    3. node_load_block(node, hash, &block) → block_storage_read()
    4. If INV_WITNESS_BLOCK: serialize with witness data (full block)
       If INV_BLOCK: serialize stripping witness (compat mode)
    5. peer_send_block(peer, serialized_data, len)
```

Data direction: Storage → App Layer → Network. No consensus involvement.

### 4. Tapscript OP_CHECKSIGADD (BIP-342)

**Current (failing) flow:**
```
script_execute_tapscript() encounters OP_CHECKSIGADD (0xba)
    ↓
script_exec_op() → SCRIPT_ERR_BAD_OPCODE (rejects any Taproot multisig tx)
```

**Correct flow after fix:**
```
script_execute_tapscript() encounters OP_CHECKSIGADD
    ↓
Stack: [sig, num, pubkey] (3 inputs)
    ↓
If sig is empty: pop sig, push (num) unchanged — BIP-342 NULLFAIL semantics
If sig is present:
    → secp256k1_schnorrsig_verify(sig, sighash, pubkey) via sighash_taproot()
    → if valid: push (num + 1)
    → if invalid: push (num + 0)   [unlike CHECKSIG which fails on bad sig]
```

Key difference from OP_CHECKSIG: OP_CHECKSIGADD does not fail on bad
signatures in Tapscript; it pushes num+0. This enables threshold multisig
patterns without CHECKMULTISIG. Implementation is contained entirely within
`src/consensus/script.c` and `src/crypto/secp256k1.c`.

### 5. BIP-125 RBF in Mempool

**Current (blocking) flow:**
```
mempool_add() detects conflicting tx (spends same outpoint)
    ↓
Returns MEMPOOL_REJECT_CONFLICT unconditionally
```

**Correct flow after fix:**
```
mempool_add() detects conflicting tx
    ↓
Check: does any conflicting tx signal RBF? (signals_rbf = true on entry)
If no: MEMPOOL_REJECT_CONFLICT (unchanged)
If yes (BIP-125 applies):
    1. New tx fee >= sum(conflicting fees) + MEMPOOL_RBF_INCREMENT
    2. Replacement count <= MEMPOOL_MAX_REPLACEMENT_COUNT
    3. No new unconfirmed inputs (no chain of new ancestors)
    4. All conflicting txs pay lower fee rate than replacement
    If all checks pass:
        → Remove conflicting txs (and descendants)
        → Insert replacement
        → MEMPOOL_ACCEPT_OK
    Else: MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE or MEMPOOL_REJECT_RBF_TOO_MANY_REPLACED
```

Data flow: entirely within `mempool.c`. The `mempool_entry_t` struct already
has `signals_rbf`, `fee`, `fee_rate`, `ancestor_count` fields. The rejection
enum already has `MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE` and
`MEMPOOL_REJECT_RBF_TOO_MANY_REPLACED`. The data structures are complete;
the acceptance logic is the missing implementation.

### 6. Transaction Index for RPC

**Current flow:**
```
rpc_getrawtransaction(txid) → mempool_lookup(txid) → found or NULL
                                                    → (TODO for confirmed)
```

**Correct flow after fix:**
```
rpc_getrawtransaction(txid, verbose, blockhash)
    ↓
1. mempool_lookup(txid) → if found: return mempool tx
2. txindex_lookup(txid) → block_file_pos_t + offset within block
3. node_load_block_at_pos(pos) → parse block
4. Find tx with matching txid within block
5. Return raw hex or JSON depending on verbose flag
```

New storage component needed: `txindex` (txid → block position + tx offset).
This can live in `block_index_db.c` as an additional SQLite table (txid BLOB
PRIMARY KEY, file_index INTEGER, file_offset INTEGER, tx_offset INTEGER). It
is populated during `node_apply_block()` for each transaction in the block.

---

## Suggested Build Order

The concerns have dependencies. Building in the wrong order creates integration
pain. The correct sequence:

### Phase 1: Foundation Fixes (no new features, unblock everything else)

These are prerequisite for correctness. Do these first.

1. **Chainwork big-endian storage** (`block_index_db.c`) — fixes fork selection
   logic. No downstream dependencies except correctness. Requires migration of
   existing stored chainwork if any is present (but since node starts fresh
   `rm -rf ~/.bitcoin-echo`, migration is not needed).

2. **Async storage callbacks** (`blocks.c`, `node.c`) — eliminates GAP errors
   and the current synchronous write workaround. Unblocks accurate block marking.
   This is a performance and correctness fix. Required before any block serving
   work because serving also reads from the same `block_file_manager_t`.

3. **Download manager batch remaining bug** (`download_mgr.c`) — currently
   self-detected and patched at runtime with LOG_ERROR. Should be eliminated at
   root cause before adding more batch complexity.

4. **Duplicate peer address detection** (`node.c`) — race condition fix, low
   complexity.

5. **Chaser fault handler + logging integration** (`chaser.c`, `log.c`) — must
   exist before adding more complex chaser work (reorg) so errors surface
   correctly.

6. **Block hash retrieval in chaser_validate** (`chaser_validate.c`,
   `block_index_db.c`) — currently submits all-zeros hash. Fixes validation
   tracking integrity. Required before reorg testing because hashes identify
   blocks in the reorg plan.

7. **Checkpoint configuration** (`chaser_validate.c`, `chaser_confirm.c`,
   `node_config_t`) — wire `node_config.checkpoint_height` through to both
   chasers instead of hardcoded 0. Enables faster IBD testing.

### Phase 2: Consensus Completeness (can run in parallel after Phase 1)

8. **OP_CHECKSIGADD / Tapscript** (`script.c`, `secp256k1.c`) — self-contained
   within consensus layer. No other phase depends on this. Required before
   mainnet validation of modern Taproot transactions is possible. Test with
   BIP-342 vectors immediately after.

9. **Chainstate reorg + UTXO undo** (`chaser_confirm.c`, `chainstate.c`) —
   the most complex single item. Depends on: Phase 1 items (correct hashes,
   working fault logging), and the existing `chain_reorganize()` API in
   `chainstate.c` which is already implemented. `chaser_confirm_reorganize()`
   stub exists; it needs to call `chain_reorg_create()` then
   `chain_reorganize()` with `node_load_block` as the block fetch callback.
   Also fix `chainwork` not updating on reorg (chainstate.c line 739).

### Phase 3: Peer Network Compatibility (depends on Phase 1 + 2)

10. **NODE_WITNESS service flag + INV_WITNESS_BLOCK** (`node.c`, peer
    handshake) — wire the SegWit service flag into the version message.
    Peers will then send witness block data in INV messages. Required before
    block serving because we need to know which inventory type to advertise.

11. **Block serving (getdata responder)** (`node.c`) — depends on: async
    storage (Phase 1), correct block loading, NODE_WITNESS flag (step 10).
    Implement full `getdata` handler: lookup hash in block index, read from
    `block_storage_read()`, serialize, send. Distinguish
    `INV_BLOCK` (strip witness) vs `INV_WITNESS_BLOCK` (full with witness).

12. **BIP-125 RBF in mempool** (`mempool.c`) — self-contained within Protocol
    layer. Can be done any time after Phase 1. Test with BIP-125 acceptance
    rules from the BIP specification.

### Phase 4: RPC + Index (depends on Phase 3)

13. **Transaction index** (`block_index_db.c` + `node.c` population) — new
    SQLite table. Populated during `node_apply_block()`. Required before
    `getrawtransaction` for confirmed transactions works.

14. **getblockchaininfo mediantime** (`rpc.c`, `chainstate.c`) — query last
    11 block timestamps, compute median. Requires `chainstate_get_block_at_height()`
    which already exists. Simple to implement.

15. **getblock verbosity=0** (`rpc.c`, `blocks.c`) — load block from storage,
    serialize to hex. Depends on block serving read path (step 11).

16. **getrawtransaction for confirmed** (`rpc.c`) — depends on transaction
    index (step 13).

17. **getblocktemplate** (`rpc.c`, `mining.c`) — uses existing
    `mempool_select_for_block()` API. Constructs a coinbase template, selects
    transactions, returns JSON template per BIP-22/23. Medium complexity.

18. **Peer eviction threshold calibration** (`sync.c`) — tune
    `SLOWEST_EVICTION_MIN_RATE` from 0.0 to a real value based on IBD
    observation. Requires running a full IBD and logging peer rates.

---

## Architectural Patterns

### Pattern 1: Callback-Based Cross-Layer Communication

**What:** Higher layers pass function pointers to lower-level operations.
Used throughout: `get_block_txs_fn` in `chain_reorganize()`, `sync_callbacks_t`
in sync manager, `mempool_callbacks_t` in mempool.

**When to use:** When a lower layer (consensus, storage) needs to ask for data
that requires knowledge only an upper layer has (e.g., loading a block by hash
requires knowing the storage path, which is App layer knowledge).

**Application to new work:** The reorg implementation uses this pattern
correctly. `chaser_confirm.c` wraps `node_load_block()` as a `get_block_txs_fn`
callback and passes it to `chain_reorganize()`. Consensus does not reach up;
App provides the loader down.

```c
/* Correct pattern for reorg in chaser_confirm.c */
static echo_result_t confirm_get_block_txs(const hash256_t *hash,
                                           const tx_t **txs_out,
                                           size_t *tx_count_out,
                                           void *user_data) {
    node_t *node = (node_t *)user_data;
    block_t block;
    echo_result_t r = node_load_block(node, hash, &block);
    if (r != ECHO_OK) return r;
    *txs_out = block.txs;
    *tx_count_out = block.tx_count;
    return ECHO_OK;
}
/* Then: chain_reorganize(state, reorg, confirm_get_block_txs, node) */
```

### Pattern 2: Opaque Struct + Operation Functions

**What:** All major subsystems expose an opaque `typedef struct X X_t;` and
only operate through named functions. No direct field access from outside the
module.

**Application to new work:** Transaction index follows this pattern. Add a
`txindex_t` opaque struct in `block_index_db.h` with functions:
`txindex_insert()`, `txindex_lookup()`. Do not expose the SQLite table
directly to `rpc.c`.

### Pattern 3: Event Bus for Chaser Coordination

**What:** The chase dispatcher fires typed events (CHASE_CHECKED,
CHASE_ORGANIZED, etc.). Chasers subscribe and react. No direct calls between
chasers.

**Application to new work:** After reorg executes in `chaser_confirm.c`, it
emits appropriate events. `mempool.c` does not directly subscribe to these
(the node orchestrates the `mempool_readd_for_disconnect()` call from the
block application path). The event pattern does not need extension for this
milestone.

---

## Anti-Patterns to Avoid

### Anti-Pattern 1: I/O in Consensus for Reorg

**What people do:** Add a `block_storage_read()` call directly inside
`chainstate.c` to load block transactions during reorg.

**Why it's wrong:** Violates the core design principle. Consensus must be
deterministic pure computation with no I/O. Testing becomes impossible without
a real storage backend. The consensus layer becomes untestable in isolation.

**Do this instead:** Use the `get_block_txs_fn` callback already built into
`chain_reorganize()`. The caller (`chaser_confirm.c`) provides the I/O; the
consensus function receives data it needs as parameters.

### Anti-Pattern 2: New Global State for Transaction Index

**What people do:** Add a global `txindex_t *g_txindex` to avoid plumbing.

**Why it's wrong:** All state in this codebase flows through `node_t`. Global
state breaks the observer mode, makes testing harder, and violates the
single-node invariant.

**Do this instead:** Store `txindex_t` as a field in `node_t` (or embed it in
`block_index_db_t`). Pass `node_t *node` to RPC handlers (already done).

### Anti-Pattern 3: Synchronous Block Serving That Blocks the Event Loop

**What people do:** Call `block_storage_read()` synchronously in the getdata
handler on the main event loop thread.

**Why it's wrong:** A large block (4 MB) takes 50-100ms to read from disk. The
main event loop processes all peer messages. Blocking for disk reads drops
other peers' messages and stalls IBD progress.

**Do this instead:** Schedule block reads on a worker thread or use the
existing async storage infrastructure. For initial implementation, a simple
approach is acceptable: read synchronously but only serve one block per event
loop tick per peer, with the block capped to already-validated stored blocks.

### Anti-Pattern 4: Modifying the Consensus Struct Layout for Chainwork Endianness

**What people do:** Change `work256_t.bytes` from little-endian to big-endian
throughout `chainstate.c` to fix the comparison issue.

**Why it's wrong:** `work256_t` is used in arithmetic (`work256_add`,
`work256_compare`) throughout the consensus engine. Changing the endianness
assumption there requires auditing all arithmetic code.

**Do this instead:** Keep `work256_t` in little-endian everywhere internally.
Only `block_index_db.c` performs byte-swapping when serializing to / deserializing
from SQLite. The comparison for fork selection happens via `work256_compare()`
(which is endian-aware), not raw SQLite byte comparison. Fix the SQLite storage
format only.

---

## Integration Points

### External Libraries

| Library | Integration | Notes |
|---------|-------------|-------|
| libsecp256k1 (vendored `lib/secp256k1/`) | `secp256k1_schnorrsig_verify()` for OP_CHECKSIGADD | Check whether vendored version exposes `secp256k1_schnorrsig_verify`. If not, verify the individual scalar/point operations needed. Do not require secp256k1-zkp (not vendored). |
| SQLite (vendored) | New table for transaction index; chainwork format change | Use existing `db.h` helper wrappers. Transaction index table: `CREATE TABLE IF NOT EXISTS txindex (txid BLOB PRIMARY KEY, file_index INTEGER, file_offset INTEGER, tx_pos INTEGER)` |

### Internal Boundaries

| Boundary | Communication | Notes |
|----------|---------------|-------|
| `chaser_confirm` ↔ `chainstate` | Direct function calls (same address space) | `chaser_confirm.c` calls `chain_reorg_create()`, `chain_reorganize()`, `chainstate_revert_block()` |
| `chaser_confirm` ↔ `node` | Via `get_block_txs_fn` callback | node provides block loading without consensus knowing about node |
| `node` ↔ `blocks.c` | Direct function calls + new async callback | `block_storage_write()` gets optional completion callback; `block_storage_read()` unchanged |
| `rpc` ↔ `block_index_db` | Direct function calls via `node_get_block_index_db()` | txindex_lookup wraps SQLite query |
| `mempool` ↔ `chainstate` | Via `mempool_callbacks_t.get_utxo` | Already in place; RBF adds no new cross-boundary calls |
| `node` ↔ `peer` | `peer_send_*()` functions | Block serving uses existing peer send infrastructure |

---

## Build Order Summary for Roadmap

This maps to suggested milestone phases:

| Phase | Items | Rationale |
|-------|-------|-----------|
| Phase 1: Foundation | Async storage, download mgr bug, duplicate peer, fault logging, real block hash in chaser_validate, checkpoint config, chainwork endian | All blocking bugs; no new features; highest risk to skip |
| Phase 2: Consensus | OP_CHECKSIGADD, chainstate reorg + UTXO undo | Consensus correctness; independent of networking; must test with known vectors |
| Phase 3: Network | NODE_WITNESS flag, block serving (getdata), BIP-125 RBF | Network participation; requires Phase 1 (async storage) and Phase 2 (reorg) to be stable first |
| Phase 4: RPC | txindex, getrawtransaction, getblock hex, mediantime, getblocktemplate, eviction tuning | User-facing capabilities; requires storage infrastructure from Phase 1+2 |

---

## Sources

- `include/chainstate.h` — `chain_reorganize()`, `block_delta_t`, `chain_reorg_t` APIs (HIGH confidence — direct code)
- `include/chaser_confirm.h` — `chaser_confirm_reorganize()` stub and worker thread design (HIGH confidence — direct code)
- `include/blocks_storage.h` — `block_file_manager_t`, `block_storage_write()` / `block_storage_read()` (HIGH confidence — direct code)
- `include/mempool.h` — `mempool_entry_t.signals_rbf`, `MEMPOOL_RBF_INCREMENT`, `MEMPOOL_MAX_REPLACEMENT_COUNT` (HIGH confidence — direct code)
- `include/script.h` — `OP_CHECKSIGADD`, `script_execute_tapscript()`, Taproot context fields (HIGH confidence — direct code)
- `include/node.h` — `node_load_block()`, `node_store_block()`, `node_announce_block_to_peers()` (HIGH confidence — direct code)
- `.planning/codebase/CONCERNS.md` — Canonical list of all issues with file/line references (HIGH confidence — codebase audit)
- `.planning/codebase/ARCHITECTURE.md` — Existing layer documentation (HIGH confidence — codebase audit)
- BIP-342 (Tapscript, OP_CHECKSIGADD semantics) — MEDIUM confidence from training; verify against spec before implementation
- BIP-125 (RBF rules) — MEDIUM confidence from training; rule 1-5 are stable and well-documented

---

*Architecture research for: Bitcoin Echo peer-compatible node milestone*
*Researched: 2026-02-20*
