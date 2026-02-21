# Phase 3: P2P Block Serving - Research

**Researched:** 2026-02-21
**Domain:** Bitcoin P2P protocol — SegWit service advertisement and block serving
**Confidence:** HIGH

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| P2P-01 | Node advertises NODE_WITNESS (service bit 3) in version message to peers | Services bitmask already exists in `protocol.h`; `peer_send_version` takes `our_services` param; three call sites in `node.c` all pass a hardcoded literal instead of using the named constants |
| P2P-02 | Node serves full witness-serialized blocks to peers via getdata handler | `block_serialize` already writes witness-serialized output (`ECHO_TRUE`); blocks on disk are already witness-serialized; getdata handler in `node.c` has a `TODO: Full block serving` stub at line 2786; `block_storage_read` + `node_load_block` exist and work |
| P2P-04 | Node uses INV_WITNESS_BLOCK inventory type when requesting blocks from witness-capable peers | `INV_WITNESS_BLOCK` constant is defined in `protocol.h`; `sync_cb_send_getdata_blocks` at line 1779 has a `TODO` comment and uses `INV_BLOCK` unconditionally; `node_announce_block_to_peers` uses `INV_BLOCK` unconditionally at line 3959; peer's `services` field is already stored at handshake |
</phase_requirements>

---

## Summary

Phase 3 makes Echo a genuine Bitcoin network participant by implementing three tightly coupled behaviors: advertising SegWit capability, serving witness blocks when requested, and announcing blocks with the correct inventory type to SegWit-capable peers. The work is entirely contained in `src/app/node.c` — no new files, no new headers, no new data structures. The codebase already has every constant, data structure, and storage primitive required.

The key insight from code inspection is that **blocks on disk are already witness-serialized**. `block_serialize` always passes `ECHO_TRUE` (include witness), meaning `block_storage_read` returns wire-ready witness block data. Serving `MSG_WITNESS_BLOCK` is simply reading from disk and forwarding — no re-serialization needed. Serving `MSG_BLOCK` (legacy) requires stripping witness from the stored bytes, which means using `tx_serialize` with `with_witness = ECHO_FALSE` per transaction, or computing a stripped size and serializing fresh.

The three requirements map cleanly to three one-function-each changes: (1) add `SERVICE_NODE_WITNESS` to the services bitmask at `peer_send_version` call sites, (2) implement the getdata block handler body (the `TODO` stub), and (3) change `node_announce_block_to_peers` and `sync_cb_send_getdata_blocks` to check `peer->services & SERVICE_NODE_WITNESS` before choosing the inventory type.

**Primary recommendation:** Implement in the order P2P-01 → P2P-02 → P2P-04. NODE_WITNESS advertisement is a one-liner prerequisite; block serving is the largest functional change and the one requiring the most careful handling of the pruned-block and legacy-strip cases; block announcement inventory type change depends on having verified the peer services field is populated during handshake (confirmed: it is).

---

## Standard Stack

### Core

| Component | Version/Location | Purpose | Notes |
|-----------|-----------------|---------|-------|
| `SERVICE_NODE_WITNESS` | `include/protocol.h` line 44 | Service bit 3 (SegWit capability) | Defined as `(1 << 3)` = 8; exists, just not used yet |
| `SERVICE_NODE_NETWORK` | `include/protocol.h` line 43 | Service bit 0 (full node) | Already used in `node.c` services calculation |
| `INV_WITNESS_BLOCK` | `include/protocol.h` line 54 | `0x40000002` — SegWit block inventory type | Defined; partially handled in incoming `inv` parsing (line 2712) but not in outgoing announcements |
| `INV_BLOCK` | `include/protocol.h` line 51 | `2` — legacy block inventory type | Used everywhere; needs to become conditional |
| `block_storage_read` | `include/blocks_storage.h` | Read raw block bytes from blk*.dat | Returns witness-serialized bytes; caller must `free()` |
| `node_load_block` | `include/node.h` | Load and parse block by hash | Calls `block_storage_read` + `block_parse`; used by chasers |
| `block_index_db_lookup_by_hash` | `include/block_index_db.h` | Get `block_index_entry_t` for a hash | Entry has `data_file`, `data_pos`, and `status` with `BLOCK_STATUS_PRUNED` flag |
| `block_index_db_is_pruned` | `include/block_index_db.h` | Check if a specific block is pruned | Already called in the getdata stub |
| `tx_serialize` | `include/tx.h` | Serialize one transaction with or without witness | `with_witness = ECHO_FALSE` produces legacy format |
| `peer_queue_message` | `include/peer.h` | Queue message for sending to a peer | Used throughout; accepts `msg_t*` |
| `peer->services` | `include/peer.h` line 86 | Remote peer's service flags (populated at handshake) | Use `peer->services & SERVICE_NODE_WITNESS` to check capability |

### Supporting

| Component | Location | Purpose | When to Use |
|-----------|----------|---------|-------------|
| `block_serialize_size` | `include/block.h` | Compute witness-serialized block size | Sizing buffer for witness block response |
| `block_serialize` | `include/block.h` | Serialize full block (always with witness) | Not needed for serving — disk bytes are already correct |
| `msg_notfound_t` | `include/protocol.h` | `notfound` message structure | Send when block is pruned or not found |
| `MSG_BLOCK` | `include/protocol.h` | Block message type enum value | Used when serving legacy (non-witness) blocks |
| `BLOCK_STATUS_PRUNED` | `include/block_index_db.h` | Status flag indicating block data deleted | Check before attempting to read from storage |
| `BLOCK_STATUS_HAVE_DATA` | `include/block_index_db.h` | Status flag indicating block data is stored | Cross-check: HAVE_DATA and not PRUNED = servable |

### Alternatives Considered

| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Reading raw bytes from disk with `block_storage_read` for witness blocks | Parse with `node_load_block` then re-serialize | Raw read is O(1) memcpy; parse+re-serialize wastes CPU and is only needed for legacy stripping |
| Checking `peer->services & SERVICE_NODE_WITNESS` at announcement time | Adding a `prefers_witness` flag to `peer_t` | The services field is already stored; adding a redundant field would violate simplicity |
| Adding `block_serialize_no_witness` helper | Inline legacy-strip loop in getdata handler | A helper is cleaner and testable; only needed for the `MSG_BLOCK` (legacy) case |

---

## Architecture Patterns

### Recommended Project Structure

No new files or directories. All changes are in `src/app/node.c`.

```
src/app/node.c              # All three plan changes land here
  sync_cb_send_getdata_blocks()  # Plan 03-03: add witness check here
  MSG_GETDATA handler           # Plan 03-02: implement block serving
  node_announce_block_to_peers()  # Plan 03-03: add witness check here
  peer_send_version() call sites  # Plan 03-01: add SERVICE_NODE_WITNESS
```

### Pattern 1: Services Flag Combination

**What:** Combine service flags with bitwise OR. The current code passes literal `1` or `0` to `peer_send_version`; it should use named constants.

**When to use:** Any place that sets `our_services` before calling `peer_send_version`.

**Current code (three identical call sites in node.c):**
```c
/* Lines 2937, 2961, 3329 — same pattern each time */
uint64_t services = node_is_pruning_enabled(node) ? 0 : 1;
peer_send_version(peer, services, (int32_t)our_height, true);
```

**Fixed code:**
```c
uint64_t services = SERVICE_NODE_WITNESS;  /* Always advertise SegWit */
if (!node_is_pruning_enabled(node)) {
  services |= SERVICE_NODE_NETWORK;        /* Full block history if not pruned */
}
peer_send_version(peer, services, (int32_t)our_height, true);
```

**Note on pruned nodes and NODE_WITNESS:** A pruned node can still advertise `NODE_WITNESS` — it means "I speak SegWit protocol", not "I have all historical blocks". Pruned nodes in Bitcoin Core advertise both `NODE_NETWORK_LIMITED` and `NODE_WITNESS`. This project uses 0 for pruned and `SERVICE_NODE_NETWORK` for non-pruned. The fix: always include `SERVICE_NODE_WITNESS`; keep the `SERVICE_NODE_NETWORK` gating on pruning unchanged.

**Confidence:** HIGH — verified against `protocol.h` constants and `peer.c` version message construction.

### Pattern 2: getdata Block Serving — Inventory Type Dispatch

**What:** When a peer sends `getdata` with `INV_BLOCK` or `INV_WITNESS_BLOCK`, check the inventory type bit 30 to decide whether to include witness data.

**Protocol spec:** `INV_WITNESS_BLOCK = 0x40000002`. Bit 30 (`0x40000000`) is the "include witness" flag. If set, serve witness-serialized. If clear (`INV_BLOCK = 2`), serve legacy-stripped.

**Current handler (node.c line 2757–2786):**
```c
case MSG_GETDATA:
  for (size_t i = 0; i < msg->payload.getdata.count; i++) {
    const inv_vector_t *inv = &msg->payload.getdata.inventory[i];
    if (inv->type == INV_BLOCK || inv->type == INV_WITNESS_BLOCK) {
      /* pruned check already here */
      /* TODO: Full block serving */  /* <-- plan 03-02 fills this in */
    }
  }
```

**Serving witness block (INV_WITNESS_BLOCK path):**
```c
/* Blocks on disk are already witness-serialized — raw read is correct */
uint8_t *block_data = NULL;
uint32_t block_size = 0;
echo_result_t r = block_storage_read(&node->block_storage, pos, &block_data, &block_size);
if (r != ECHO_OK) {
  /* send notfound */
} else {
  /* build MSG_BLOCK from raw bytes — need a raw-bytes msg variant */
  /* or parse + re-serialize (see anti-pattern below) */
  free(block_data);
}
```

**Design choice — raw bytes vs parsed block for serving:** The `msg_t` union stores `msg_block_t` as a parsed `block_t`, not raw bytes. To send a block message, the code must either: (a) parse from disk into `block_t` using `node_load_block`, build `msg_t`, and call `peer_queue_message` (which then re-serializes via `msg_block_serialize`), or (b) serialize directly to the wire without going through `msg_t`. Option (a) is consistent with existing patterns; `node_load_block` already exists and is used by chasers. Use option (a) — parse then queue. The double-serialize (disk→parse→re-serialize) is a small and acceptable cost for code clarity.

**Serving legacy block (INV_BLOCK path):**
Parse block with `node_load_block`, then the existing `msg_block_serialize` will call `tx_serialize` with the `with_witness` parameter derived from `msg_block_t`. Wait — `msg_block_serialize` always serializes with witness (`block_serialize` passes `ECHO_TRUE`). To strip witness, a new helper is needed or inline stripping per transaction.

**Recommended approach for legacy stripping:** Add a `block_serialize_no_witness` function (or accept that the `block_serialize` path is always witness and use `tx_serialize` with `ECHO_FALSE` manually). Actually the cleanest approach: check if any tx `has_witness = ECHO_FALSE` would be accurate after parsing. The stripping logic would be: load block, allocate buffer sized with `block_serialize_size_no_witness`, serialize each tx with `ECHO_FALSE`. However since the protocol `msg_block_serialize` → `block_serialize` path always uses witness, you need a new `block_serialize` call variant OR inline a stripped serialization. Given project philosophy (simplicity, no over-engineering), the simplest correct approach is:

1. Load the block into a `block_t` via `node_load_block`
2. For `INV_WITNESS_BLOCK`: build `msg_t`, queue via `peer_queue_message` — the existing `msg_block_serialize` will include witness (correct)
3. For `INV_BLOCK`: serialize manually into a heap buffer using `tx_serialize` with `ECHO_FALSE` per tx, then use `peer_queue_message` with the pre-serialized form OR add a small static helper

**See "Open Questions" section for the notfound edge case regarding the `msg_t` raw-byte sending pattern.**

### Pattern 3: Peer Witness Capability Check for Announcements

**What:** When announcing a new block to peers, use `INV_WITNESS_BLOCK` for witness-capable peers and `INV_BLOCK` for legacy peers.

**Current code (`node_announce_block_to_peers`, line 3943–3971):**
```c
void node_announce_block_to_peers(node_t *node, const hash256_t *block_hash) {
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    peer_t *peer = &node->peers[i];
    if (peer_is_ready(peer)) {
      inv_vector_t inv_vec;
      inv_vec.type = INV_BLOCK;    /* <-- must become conditional */
      ...
    }
  }
}
```

**Fixed pattern:**
```c
bool peer_wants_witness = (peer->services & SERVICE_NODE_WITNESS) != 0;
inv_vec.type = peer_wants_witness ? INV_WITNESS_BLOCK : INV_BLOCK;
```

**Same change applies to `sync_cb_send_getdata_blocks` (line 1763):** When requesting blocks during IBD, use `INV_WITNESS_BLOCK` if the peer we're downloading from advertised `NODE_WITNESS`. The peer pointer is passed as a parameter — `peer->services` is available.

```c
/* Fixed sync_cb_send_getdata_blocks */
bool peer_has_witness = (peer->services & SERVICE_NODE_WITNESS) != 0;
for (size_t i = 0; i < count; i++) {
  inventory[i].type = peer_has_witness ? INV_WITNESS_BLOCK : INV_BLOCK;
  inventory[i].hash = hashes[i];
}
```

**Confidence:** HIGH — `peer->services` is populated during handshake (line 86 in `peer.h`), the service flag is set when version message is processed.

### Anti-Patterns to Avoid

- **Parsing block twice for witness serving:** Blocks on disk are already witness-serialized. Parsing just to re-serialize is wasteful but acceptable for code clarity. Do NOT implement a custom raw-bytes message path that bypasses `msg_t` — it would violate the protocol layer abstraction.
- **Advertising NODE_WITNESS only for non-pruned nodes:** NODE_WITNESS means "I understand the SegWit wire protocol", not "I have all historical block data". A pruned node must still advertise it. The pruned/non-pruned distinction controls `SERVICE_NODE_NETWORK`, not `SERVICE_NODE_WITNESS`.
- **Using INV_WITNESS_BLOCK in announcements without checking peer services:** Some old peers (protocol < 70013) do not understand `INV_WITNESS_BLOCK`. Always check `peer->services & SERVICE_NODE_WITNESS` before using witness inventory types. (Bitcoin Core has been witness-capable since 2016, so in practice all reachable peers support it, but the check is still protocol-correct behavior.)
- **Sending silence instead of notfound for pruned blocks:** The success criteria explicitly requires `notfound`, not silence. The pruned-check stub is already in the getdata handler and sends `notfound` — preserve this behavior and extend it to cover the "block not in index at all" case too.
- **Forgetting to free `block_storage_read` allocation:** `block_storage_read` allocates with `malloc()` and the comment in `blocks_storage.h` explicitly says "caller must free()". Always `free(block_data)` after use.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Witness vs legacy detection for inventory | Custom bit-masking logic | `inv->type == INV_WITNESS_BLOCK` or `inv->type & 0x40000000` | The distinction is already in the constant definitions |
| Block byte retrieval | Custom file I/O | `block_storage_read` | Already handles LRU cache, file handles, magic verification |
| Block loading for getdata serving | Raw offset math | `node_load_block(node, &inv->hash, &block_out)` | Handles the index lookup + storage read pipeline |
| Peer capability check | Custom protocol version logic | `peer->services & SERVICE_NODE_WITNESS` | Services are the correct signal; protocol version alone is not sufficient |
| Pruned block detection | Querying block status by height | `block_index_db_is_pruned(bdb, &hash, &pruned)` | Already called in the existing stub; hash-based lookup is correct |

**Key insight:** The codebase already has every primitive needed. This phase is about wiring existing components together, not building new components.

---

## Common Pitfalls

### Pitfall 1: Services Bitmask — Passing Literal Instead of Constants

**What goes wrong:** The current code passes literal `1` or `0` to `peer_send_version`. When adding `NODE_WITNESS`, a developer might write `services = 9` (1 | 8) instead of `SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS`. The literal `9` is fragile and means nothing to a reader.

**Why it happens:** The original services code predates the named constants being "activated" — the constants exist but aren't used.

**How to avoid:** Use `SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS` explicitly. Never use numeric literals for bitmasks.

**Warning signs:** Any `services = 1` or `services = 0` literal in `node.c` — there are exactly three call sites (lines 2937, 2961, 3329).

### Pitfall 2: Block Serialization Format on Disk vs Wire

**What goes wrong:** Developer assumes blocks on disk might be legacy-serialized (no witness), so writes code to handle both cases. Actually all blocks are stored with witness (`block_serialize` → `ECHO_TRUE` always). Code that "handles both cases" is dead code that adds confusion.

**Why it happens:** The stored format is not documented in a single obvious place — it requires tracing `node_store_block` → `block_serialize` → `tx_serialize(..., ECHO_TRUE)`.

**How to avoid:** Confirmed by code inspection: disk format = witness. Serving `INV_WITNESS_BLOCK` = read from disk, forward bytes directly (via parse→queue path). Serving `INV_BLOCK` = read from disk, then strip witness per tx.

**Warning signs:** Any code that branches on "does this block have witness data" before serving — not needed for INV_WITNESS_BLOCK path.

### Pitfall 3: notfound for Blocks Not in the Index

**What goes wrong:** The existing stub sends `notfound` for pruned blocks. But a peer can request a block hash that Echo has never seen at all (not in block index). The handler currently falls through silently for that case.

**Why it happens:** The existing stub only handles the pruned case; it never checks if the block is in the index at all.

**How to avoid:** When implementing the getdata handler, call `block_index_db_lookup_by_hash` first. If `ECHO_ERR_NOT_FOUND`, send `notfound`. If found but pruned, send `notfound`. If found and not pruned, serve the block.

**Warning signs:** Handler that calls `block_index_db_is_pruned` without first checking existence — `is_pruned` returns `ECHO_ERR_NOT_FOUND` for unknown hashes; ensure the code distinguishes "pruned" from "unknown".

### Pitfall 4: sync_cb_send_getdata_blocks and Service Flags During IBD

**What goes wrong:** During IBD, Echo downloads blocks using `INV_BLOCK`. After adding `NODE_WITNESS` advertisement, Echo will receive blocks from peers who think Echo supports witness. If Echo sends `INV_BLOCK` getdata but peer sends `MSG_BLOCK` with witness data (because peer noticed Echo advertised NODE_WITNESS), the deserialization still works — but Echo is implicitly requesting non-witness blocks while advertising witness support, which is confusing.

**Why it happens:** `sync_cb_send_getdata_blocks` has a hardcoded `INV_BLOCK` with a `TODO` comment acknowledging the problem.

**How to avoid:** Fix `sync_cb_send_getdata_blocks` to check `peer->services & SERVICE_NODE_WITNESS` and use `INV_WITNESS_BLOCK` when true. This is P2P-04 requirement. Do this in plan 03-03 (same plan as announcement type fix).

**Important nuance:** Changing getdata requests during IBD from `INV_BLOCK` to `INV_WITNESS_BLOCK` means peers will send full witness-serialized blocks. Echo already parses them correctly (tx_parse handles the SegWit marker). The stored blocks will then be witness-serialized — which is what we want and what already happens via `block_serialize(..., ECHO_TRUE)`.

### Pitfall 5: Memory Safety in getdata Handler

**What goes wrong:** `block_storage_read` allocates a buffer with `malloc`. If any early return path (error, block not found, pruned) is added after the allocation, the buffer leaks.

**Why it happens:** The C idiom for cleanup requires explicit `free` before every return path.

**How to avoid:** Use the standard goto-cleanup pattern or simply free before each error return. Since `node_load_block` wraps `block_storage_read` and handles this internally (returning a parsed `block_t`), using `node_load_block` is safer — the allocation is hidden and `block_free` handles cleanup.

**Warning signs:** Any path through the getdata handler that returns without calling `free(block_data)`.

---

## Code Examples

Verified patterns from code inspection:

### Plan 03-01: Fixed services flags (peer_send_version call sites)

```c
/* Source: src/app/node.c lines 2935-2938 (inbound), 2960-2962 (outbound async),
 *         3328-3330 (outbound initial) — same fix at all three sites */

/* Before: */
uint64_t services = node_is_pruning_enabled(node) ? 0 : 1;

/* After: */
uint64_t services = SERVICE_NODE_WITNESS;  /* Always: we speak SegWit protocol */
if (!node_is_pruning_enabled(node)) {
  services |= SERVICE_NODE_NETWORK;        /* Full block history if not pruned */
}
```

### Plan 03-02: Block serving in getdata handler

```c
/* Source: src/app/node.c case MSG_GETDATA (line 2757) — filling in the TODO */

if (inv->type == INV_BLOCK || inv->type == INV_WITNESS_BLOCK) {
  bool is_pruned = false;
  bool want_witness = (inv->type == INV_WITNESS_BLOCK);

  /* Check existence and pruning state */
  block_index_entry_t entry;
  echo_result_t r = block_index_db_lookup_by_hash(
      &node->block_index_db, &inv->hash, &entry);

  if (r == ECHO_ERR_NOT_FOUND) {
    /* Unknown block — send notfound */
    goto send_notfound;
  }
  if (r != ECHO_OK) {
    break; /* Database error — skip silently */
  }

  if (entry.status & BLOCK_STATUS_PRUNED) {
    /* Block was pruned — send notfound */
    goto send_notfound;
  }

  /* Load and serve the block */
  block_t block;
  block_init(&block);
  r = node_load_block(node, &inv->hash, &block);
  if (r != ECHO_OK) {
    block_free(&block);
    goto send_notfound;
  }

  if (want_witness) {
    /* Serve witness-serialized block */
    msg_t block_msg;
    memset(&block_msg, 0, sizeof(block_msg));
    block_msg.type = MSG_BLOCK;
    memcpy(&block_msg.payload.block.block, &block, sizeof(block_t));
    peer_queue_message(peer, &block_msg);
    /* Note: block_t ownership transferred; do NOT call block_free after queue */
  } else {
    /* Serve legacy (no-witness) block — requires stripped serialization */
    /* [see Open Questions #1 for the cleanest approach] */
  }
  continue;

send_notfound:
  {
    inv_vector_t nf_inv = *inv;
    msg_t nf_msg;
    memset(&nf_msg, 0, sizeof(nf_msg));
    nf_msg.type = MSG_NOTFOUND;
    nf_msg.payload.notfound.count = 1;
    nf_msg.payload.notfound.inventory = &nf_inv;
    peer_queue_message(peer, &nf_msg);
  }
}
```

**Note on block_t ownership after peer_queue_message:** `peer_queue_message` copies the `msg_t` struct into the queue entry. Since `msg_t` contains `msg_block_t` which contains `block_t` by value, and `block_t` contains pointer fields (`txs`, scripts, witnesses), the copy is a shallow copy. This is a problem — the queue entry will have dangling pointers after `block_free`. Check how the send queue handles this. Looking at `peer.h` line 61-63: `allocated` flag on queue entries. This needs investigation — see Open Questions #2.

### Plan 03-03: Witness-aware inventory type for announcements

```c
/* Source: node_announce_block_to_peers (line 3943) */

/* Before: */
inv_vec.type = INV_BLOCK;

/* After: */
bool peer_has_witness = (peer->services & SERVICE_NODE_WITNESS) != 0;
inv_vec.type = peer_has_witness ? INV_WITNESS_BLOCK : INV_BLOCK;
```

```c
/* Source: sync_cb_send_getdata_blocks (line 1778) */

/* Before: */
inventory[i].type = INV_BLOCK;

/* After: */
bool peer_has_witness = (peer->services & SERVICE_NODE_WITNESS) != 0;
inventory[i].type = peer_has_witness ? INV_WITNESS_BLOCK : INV_BLOCK;
```

---

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| All peers receive `INV_BLOCK` announcements | Witness-capable peers receive `INV_WITNESS_BLOCK` | BIP-144 (SegWit activation, Aug 2017) | Peers that requested witness blocks via services get correct inventory type |
| `INV_BLOCK` getdata fetches legacy blocks | `INV_WITNESS_BLOCK` fetches witness-serialized blocks | BIP-144 | Enables correct witness data delivery during IBD |
| NODE_NETWORK only service flag | NODE_NETWORK + NODE_WITNESS | SegWit activation | Peers use service flags to decide what inventory to send |

**Note on bit 30 (0x40000000):** Bitcoin Core uses bit 30 of the inventory type as a generic "include witness" modifier. `MSG_BLOCK | MSG_WITNESS_FLAG = INV_WITNESS_BLOCK`. The same pattern applies to transactions: `INV_TX | MSG_WITNESS_FLAG = INV_WITNESS_TX`. The codebase already defines `INV_WITNESS_TX` and `INV_WITNESS_BLOCK` correctly.

**Deprecated:**
- Serving `INV_BLOCK` to all peers regardless of capability: Still valid for truly legacy peers (pre-70013), but all reachable mainnet peers today are SegWit-capable. The check is still protocol-correct to include.

---

## Open Questions

1. **Legacy block stripping for `INV_BLOCK` responses**
   - What we know: `block_serialize` always includes witness. To serve `MSG_BLOCK` (non-witness), must strip witness from each tx. `tx_serialize` supports `with_witness = ECHO_FALSE`.
   - What's unclear: Should plan 03-02 add a `block_serialize_no_witness` helper in `block.h`/`block.c`, or inline the stripped serialization in the getdata handler? The project philosophy prefers obvious purpose and heavy comments; a named helper is clearer.
   - Recommendation: Add `block_serialize_no_witness` as a small static helper inside the getdata handler (local scope, not exported), or if it's needed elsewhere, add to `block.h`. Since Phase 5 (getblock RPC) also needs raw witness-serialized bytes, the pattern is consistent. For Phase 3, the planner should decide: (a) inline stripped serialization with clear comments, or (b) add a new `block_serialize` overload. Either is acceptable; clarity wins over DRY.

2. **block_t ownership when copied into peer send queue**
   - What we know: `peer_msg_queue_entry_t` in `peer.h` has an `allocated` bool. `peer_queue_message` copies the `msg_t`. A `block_t` inside `msg_block_t` has owned pointers (`txs`, scripts, witness items).
   - What's unclear: Does the current code handle deep-copy of `block_t` in the send queue, or does it rely on the caller keeping the block alive until the queue flushes? This matters significantly for the getdata handler — if the queue is a shallow copy, the block must stay allocated until after `peer_send_queued` runs.
   - Recommendation: Before writing the getdata block-serving code, the planner should read `peer.c`'s `peer_queue_message` implementation to understand the ownership model. The `allocated` flag in `peer_msg_queue_entry_t` likely controls whether the queue entry owns heap data. The plan should explicitly address this with either a deep-copy or a different serving strategy (e.g., serialize to a heap buffer and send as raw bytes without going through `msg_t`).

3. **notfound for blocks during IBD when block index has header but no data**
   - What we know: The block index stores all headers (from headers-first sync) before blocks are downloaded. A block can be in the index with `BLOCK_STATUS_HAVE_DATA` clear and `BLOCK_STATUS_PRUNED` clear — it's known but not yet stored.
   - What's unclear: Should a getdata for a block we know of but haven't downloaded yet return `notfound`, or should it drop the request? The success criteria says "no longer in the local block store returns a notfound message", which covers the pruned case. For "never downloaded", the intent is the same: we don't have it.
   - Recommendation: Send `notfound` for any block not in storage (pruned OR never downloaded). Check: `!(entry.status & BLOCK_STATUS_HAVE_DATA)` OR `(entry.status & BLOCK_STATUS_PRUNED)` → notfound.

---

## Sources

### Primary (HIGH confidence)

All findings are from direct code inspection of the Bitcoin Echo codebase:

- `include/protocol.h` — SERVICE_NODE_NETWORK, SERVICE_NODE_WITNESS, INV_BLOCK, INV_WITNESS_BLOCK constants; all confirmed present and correctly valued
- `include/peer.h` — peer_t structure with services field (line 86), peer_queue_message, peer_send_version signatures
- `include/blocks_storage.h` — block_storage_read API; malloc-allocated return; mutex-protected
- `include/block_index_db.h` — block_index_db_lookup_by_hash, block_index_db_is_pruned, BLOCK_STATUS_* flags
- `include/block.h` — block_serialize (always witness), block_serialize_size, block_init, block_free
- `include/tx.h` — tx_serialize with_witness parameter; tx_serialize_size
- `include/node.h` — node_load_block, node_announce_block_to_peers, node_store_block
- `src/app/node.c` — actual call sites for peer_send_version (lines 2937, 2961, 3329); getdata handler TODO stub (line 2786); sync_cb_send_getdata_blocks TODO (line 1779); node_announce_block_to_peers with hardcoded INV_BLOCK (line 3959)
- `src/consensus/block.c` — block_serialize implementation confirming `ECHO_TRUE` (witness always included)

### Secondary (MEDIUM confidence)

- Bitcoin protocol documentation (BIP-144): "Transaction Signature Verification for Version 0 Witness Program" — defines the `MSG_WITNESS_FLAG (0x40000000)` modifier for inventory types. Consistent with what's implemented in `protocol.h`.
- Bitcoin protocol documentation (BIP-37/BIP-339): Service flag semantics — NODE_WITNESS means "I speak SegWit wire protocol", independent of whether the node has full block history. Pruned nodes can and should advertise it.

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — all constants, functions, and data structures verified by direct header inspection
- Architecture: HIGH — getdata handler stub, services call sites, and announcement function are all directly located in node.c with line numbers
- Pitfalls: HIGH for pitfalls 1-3 (directly observed in code); MEDIUM for pitfalls 4-5 (reasoning from patterns, not confirmed by running the code)
- Open questions: MEDIUM — the block_t ownership question requires reading peer.c implementation; the legacy stripping question is a design decision not a factual question

**Research date:** 2026-02-21
**Valid until:** Stable protocol area — valid indefinitely unless node.c is significantly restructured. The specific line numbers will drift as code changes.
