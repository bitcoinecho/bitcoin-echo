# Phase 6: getblocktemplate and submitblock - Research

**Researched:** 2026-02-21
**Domain:** Bitcoin mining RPC (BIP-22, BIP-145), witness commitment computation, block submission pipeline
**Confidence:** HIGH

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| RPC-05 | User can request block template for mining via getblocktemplate RPC (BIP-22/BIP-145 compliant with witness commitment) | Witness commitment computation path fully mapped; BIP-22/145 field requirements documented; IBD guard using node_is_ibd_mode() identified |
| RPC-06 | User can submit mined block via submitblock RPC | Existing submitblock stub already parses, validates, and applies; gaps are -28 guard and block announcement wiring |
</phase_requirements>

---

## Summary

Phase 6 upgrades two already-stubbed RPC handlers (`rpc_getblocktemplate` and `rpc_submitblock`) from placeholder implementations to production-quality implementations that satisfy BIP-22 and BIP-145 requirements.

The current `rpc_getblocktemplate` stub (lines 2239–2388 of `src/app/rpc.c`) is structurally correct — it calls `mempool_select_for_block`, computes fees, emits `bits`, `target`, `height`, `curtime` — but is missing three critical correctness requirements: (1) the `mintime` field uses a rough estimate instead of the real MTP, (2) there is no `default_witness_commitment` field at all, and (3) there is no `-28` guard for nodes still in IBD. The existing `rpc_submitblock` stub is almost complete — it parses, validates, and applies the block — but is also missing the `-28` guard and does not call `node_announce_block_to_peers` after acceptance.

The primary new work in this phase is the **witness commitment computation**. For `getblocktemplate`, the `default_witness_commitment` field is a hex-encoded scriptPubKey (not a raw hash) of the form `6a24aa21a9ed<32-byte-commitment>`. The 32-byte commitment is `SHA256d(witness_merkle_root || 32-zero-byte-nonce)`, where the witness merkle root is built from wtxids of all selected transactions, with the coinbase's wtxid replaced by 32 zero bytes. All the cryptographic primitives for this are already present in the codebase (`merkle_root_wtxids`, `witness_commitment`, `tx_compute_wtxid`).

**Primary recommendation:** The two stubs need surgical additions — MTP computation (copy pattern from `rpc_getblockchaininfo`), witness commitment computation (using existing merkle.h APIs), `-28` guard using `node_is_ibd_mode()`, and peer announcement in submitblock. No new modules, no new headers.

---

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| merkle.h (project) | internal | `merkle_root_wtxids()` computes witness merkle root; `witness_commitment()` computes SHA256d(root || nonce) | Already implemented; handles coinbase-wtxid-as-zeros by protocol |
| mining.h (project) | internal | `coinbase_subsidy()` for coinbase value; `coinbase_create()` for coinbase tx construction | Already implemented and tested |
| mempool.h (project) | internal | `mempool_select_for_block()`, `mempool_iter_by_fee()` for tx selection | Already wired in existing stub |
| block_validate.h (project) | internal | `block_validate_mtp()`, `WITNESS_COMMITMENT_PREFIX` for format verification | Already used in consensus engine |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| consensus.h (project) | internal | `consensus_validate_block()`, `consensus_get_height()` | submitblock validation path |
| node.h (project) | internal | `node_is_ibd_mode()`, `node_apply_block()`, `node_announce_block_to_peers()` | IBD guard and post-submit announcement |
| tx.h (project) | internal | `tx_compute_wtxid()`, `tx_weight()`, `tx_vsize()` | wtxid for witness merkle; weight for template fields |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| `merkle_root_wtxids()` (handles zeros) | Custom loop | No reason to hand-roll; existing function already handles coinbase = 32 zero bytes |
| `node_is_ibd_mode()` | `node_is_syncing()` | `node_is_ibd_mode()` is the canonical IBD flag (set false when sync completes); `node_is_syncing()` queries sync_mgr mode and may differ in edge cases |

---

## Architecture Patterns

### Recommended Project Structure

Both handlers live in the single `src/app/rpc.c` file. No new files needed.

```
src/app/
└── rpc.c    # Both rpc_getblocktemplate and rpc_submitblock are here already
```

### Pattern 1: MTP Computation (copy from getblockchaininfo)

**What:** Walk the block_index_map backwards from tip, collect up to 11 timestamps, insertion-sort, take the median.

**When to use:** Whenever MTP is needed in RPC context.

**Example (from existing `rpc_getblockchaininfo`, lines 1662–1695 of rpc.c):**
```c
uint32_t mtp = 0;
if (tip_index != NULL) {
  uint32_t timestamps[11];
  int ts_count = 0;
  consensus_engine_t *cons_mut = node_get_consensus(node);
  chainstate_t *cs = consensus_get_chainstate(cons_mut);
  block_index_map_t *bim = chainstate_get_block_index_map(cs);
  const block_index_t *cur = tip_index;

  while (cur != NULL && ts_count < 11) {
    timestamps[ts_count++] = cur->timestamp;
    if (cur->height == 0) break;
    cur = block_index_map_lookup(bim, &cur->prev_hash);
  }
  /* insertion sort */
  for (int i = 1; i < ts_count; i++) {
    uint32_t key = timestamps[i];
    int j = i - 1;
    while (j >= 0 && timestamps[j] > key) { timestamps[j+1] = timestamps[j]; j--; }
    timestamps[j+1] = key;
  }
  mtp = (ts_count > 0) ? timestamps[ts_count / 2] : 0;
}
/* mintime = MTP + 1 (must be strictly greater than MTP) */
uint32_t mintime = mtp + 1;
```

### Pattern 2: Witness Commitment Computation for getblocktemplate

**What:** Build a fake coinbase with zero wtxid, compute witness merkle root across selected txs, compute `SHA256d(root || nonce)`, format as scriptPubKey hex.

**When to use:** When `"segwit"` rule is active (which it always is post-SegWit activation on mainnet, height 481824).

**Protocol specifics (from BIP-141):**
- Coinbase wtxid = 32 zero bytes (not computed from the actual coinbase)
- Witness nonce = 32 zero bytes (default nonce; miner can use any 32-byte value but zero is standard for GBT)
- Witness commitment = SHA256d(witness_merkle_root || witness_nonce)
- Output scriptPubKey = `0x6a 0x24 0xaa 0x21 0xa9 0xed <32-byte-commitment>`
- This is emitted as `default_witness_commitment` in the template as a hex string

**Example pattern:**
```c
/* Build wtxid array: coinbase is 32 zeros, rest are real wtxids */
hash256_t *wtxids = calloc(1 + selected_count, sizeof(hash256_t));
/* wtxids[0] already zero (coinbase) */
for (size_t i = 0; i < selected_count; i++) {
  tx_compute_wtxid(&selected[i]->tx, &wtxids[i + 1]);
}

hash256_t witness_root;
merkle_root(wtxids, 1 + selected_count, &witness_root);
free(wtxids);

hash256_t nonce; /* 32 zero bytes - standard witness nonce */
memset(&nonce, 0, sizeof(hash256_t));

hash256_t commitment;
witness_commitment(&witness_root, &nonce, &commitment);

/* Build scriptPubKey: OP_RETURN(0x6a) + push36(0x24) + prefix(aa21a9ed) + commitment(32 bytes) */
uint8_t script[38];
script[0] = 0x6a; /* OP_RETURN */
script[1] = 0x24; /* push 36 bytes */
script[2] = 0xaa; script[3] = 0x21; script[4] = 0xa9; script[5] = 0xed;
memcpy(script + 6, commitment.bytes, 32);

json_builder_append(builder, ",\"default_witness_commitment\":");
json_builder_hex(builder, script, 38);
```

Note: `merkle_root_wtxids()` exists in merkle.h but requires a `tx_t` array with the coinbase as the first element — it internally sets coinbase wtxid to zeros. Since we do NOT have a coinbase tx object at template generation time (it hasn't been built yet), we should use `merkle_root()` directly with a manually constructed wtxid array where `wtxids[0]` is 32 zero bytes.

### Pattern 3: IBD Guard (Error -28)

**What:** Return `RPC_ERR_CLIENT_IN_WARMUP (-28)` when node has not finished initial sync.

**When to use:** At the top of both `rpc_getblocktemplate` and `rpc_submitblock`, before any other processing.

```c
if (node_is_ibd_mode(node)) {
  /* Use rpc_response_error directly or set builder to error */
  /* Convention in rpc.c: write error into builder, return specific echo code */
  /* BUT looking at existing handlers: they return ECHO_OK and write to builder */
  /* For RPC error -28, write it into the builder result directly */
  json_builder_append(builder, "null"); /* placeholder; actual error sent via rpc_response_error */
  /* Pattern: return a specific error result code and let caller wrap in error response */
  /* See existing pattern: the handler fills builder with result, server wraps */
  /* For error responses: use the pattern established in existing code */
  return ECHO_ERR_INVALID_STATE; /* Caller will wrap with "Internal error" */
}
```

**CRITICAL NUANCE:** The existing `rpc_execute_single` in rpc.c wraps non-ECHO_OK returns as `RPC_ERR_INTERNAL_ERROR (-32603)`, not as `-28`. To emit a proper `-28` response, the handler must write the error JSON *into the builder* and return ECHO_OK. Looking at the handler signature: handlers write result into builder and return ECHO_OK for success; they do NOT write error responses themselves.

**Correct approach:** The IBD check should write an error object into builder and the caller must be refactored to detect it, OR a dedicated error-return mechanism is needed. Looking at the existing code flow: `rpc_execute_single` checks `res == ECHO_OK` and calls `rpc_response_success`, else calls `rpc_response_error` with `-32603`.

The simplest correct fix: handlers that want to return a specific RPC error code should write the error object into `builder` as a special sentinel. However this breaks the interface contract. The cleaner approach: modify `rpc_execute_single` to accept a custom error code from handlers, or implement a dedicated error builder mechanism.

**Recommended approach for this phase:** Add a small convention: handlers can write a specific magic prefix to signal an RPC error code. The simplest option that works within the current architecture is to add a new helper `rpc_error_response_into_builder()` and detect a special return code `ECHO_ERR_RPC_ERROR` (-28 specifically). But this adds complexity.

**Simplest viable approach:** Inspect what `-28` the success criteria requires. The phase success criteria says "getblocktemplate returns RPC error -28 while the node is still syncing". This requires the response JSON to have `{"result":null,"error":{"code":-28,"message":"..."},"id":...}`. The current architecture requires a mechanism change in `rpc_execute_single` or the handler writes the full response. Since other handlers don't do this, a targeted fix to `rpc_execute_single` to pass through a special code is cleanest.

**Recommendation:** Introduce a small internal convention: handlers return `ECHO_ERR_INVALID_STATE` with a specific error code stored in a thread-local or builder sentinel. Simpler: modify `rpc_execute_single` to check if `res == ECHO_ERR_INVALID_STATE` and the builder has something recognizable.

**Actual simplest pattern:** Have `rpc_getblocktemplate` and `rpc_submitblock` construct their own error JSON inside the builder, then return a special result code that tells `rpc_execute_single` to use the builder content as-is instead of wrapping it. This is architecturally clean and requires minimal change to the dispatcher.

### Pattern 4: Block Announcement After submitblock

**What:** After a block is accepted by `node_apply_block()`, announce it to peers.

**When to use:** submitblock success path.

```c
/* After successful node_apply_block() */
hash256_t block_hash;
block_header_hash(&block.header, &block_hash);
node_announce_block_to_peers(node, &block_hash);
```

Note: `node_announce_block_to_peers` is already defined in `node.h` and handles IBD mode guard internally (it skips announcement during IBD). For submitblock, calling it unconditionally after acceptance is correct.

### Pattern 5: BIP-22/BIP-145 Required Fields

The template must include:

| Field | Required By | Current Stub | Status |
|-------|-------------|--------------|--------|
| `version` | BIP-22 | YES (0x20000000) | Complete |
| `previousblockhash` | BIP-22 | YES | Complete |
| `transactions` | BIP-22 | YES (array) | Complete |
| `coinbasevalue` | BIP-22 | YES | Complete |
| `target` | BIP-22 | YES | Complete |
| `mintime` | BIP-22 | BROKEN (rough estimate) | Must fix: MTP+1 |
| `curtime` | BIP-22 | YES | Complete |
| `bits` | BIP-22 | YES | Complete |
| `height` | BIP-22 | YES | Complete |
| `sigoplimit` | BIP-22 | YES | Complete |
| `weightlimit` | BIP-145 | YES | Complete |
| `rules` | BIP-145 | MISSING | Must add: `["segwit"]` |
| `default_witness_commitment` | BIP-145 | MISSING | Must add |
| `txid` per tx | BIP-145 | MISSING in tx objects | Must add |
| `hash` (wtxid) per tx | BIP-145 | MISSING | Must add |
| `weight` per tx | BIP-145 | Stored as vsize*4 | Verify correct |
| `depends` per tx | BIP-22 | MISSING | Should add for correctness |

### Anti-Patterns to Avoid

- **Computing wtxid for coinbase:** The coinbase wtxid is by protocol definition 32 zero bytes. Never call `tx_compute_wtxid` on the coinbase. Doing so produces the wrong witness merkle root.
- **Using `merkle_root_wtxids()` without a coinbase tx:** This function expects the coinbase as `txs[0]` and handles the zero-replacement internally. Without a real coinbase object at GBT time, use `merkle_root()` directly with a manually-zeroed first entry.
- **Returning MTP as mintime:** BIP-22 requires `mintime` = MTP + 1 (the minimum valid timestamp for the next block, i.e., strictly greater than MTP). MTP itself is not a valid timestamp.
- **Serving a template while in IBD:** Miners connecting to a syncing node would build invalid blocks on an incomplete chain. Return -28.
- **Not announcing after submitblock:** The block needs to propagate to the network; `node_announce_block_to_peers` does this and skips if in IBD mode.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Witness merkle tree | Custom SHA256d loop | `merkle_root()` from merkle.h | Handles odd-leaf duplication correctly |
| SHA256d(root || nonce) | Raw SHA256 calls | `witness_commitment()` from merkle.h | Correct two-round SHA256 already tested |
| wtxid computation | Custom serialization | `tx_compute_wtxid()` from tx.h | Handles segwit marker/flag in serialization |
| Block subsidy | Custom halving formula | `coinbase_subsidy()` from block_validate.h | Already correct and tested |
| MTP computation | Custom sort | Copy from `rpc_getblockchaininfo` | Pattern already exists in codebase |

**Key insight:** Every cryptographic primitive needed for this phase is already implemented and tested. This phase is integration, not implementation.

---

## Common Pitfalls

### Pitfall 1: Wrong coinbase wtxid in witness merkle
**What goes wrong:** `tx_compute_wtxid` is called on the (yet-to-be-built) coinbase, or coinbase wtxid slot is left as garbage bytes, producing a wrong witness commitment.
**Why it happens:** BIP-141 specifies coinbase wtxid = 32 zero bytes as a protocol rule, not derived from transaction content.
**How to avoid:** Allocate the wtxid array with `calloc` (zero-initializes); index 0 stays all zeros representing the coinbase.
**Warning signs:** Commitment value changes run-to-run without tx changes.

### Pitfall 2: mintime = MTP instead of MTP + 1
**What goes wrong:** Miners use the raw MTP as their block timestamp, which fails consensus validation (timestamp must be strictly greater than MTP).
**Why it happens:** The field name "mintime" suggests the minimum, but miners may set their timestamp = mintime. If mintime = MTP, valid timestamps start at MTP+1. The field should encode MTP+1 to guide miners correctly.
**How to avoid:** `mintime = mtp + 1` always.
**Warning signs:** Blocks submitted with `timestamp == mintime` fail timestamp validation.

### Pitfall 3: Serving template without `"rules": ["segwit"]`
**What goes wrong:** Mining clients that support SegWit will not include witness data in their coinbase (and may not submit valid SegWit blocks) if "segwit" is not in the rules array.
**Why it happens:** BIP-145 specifies that "segwit" MUST appear in "rules" for templates containing witness transactions.
**How to avoid:** Always include `"rules": ["segwit"]` — Echo is mainnet-only post-SegWit, so all blocks require it.
**Warning signs:** Mining clients build coinbase without witness commitment output.

### Pitfall 4: `rpc_execute_single` wraps handler's internal error as -32603 instead of -28
**What goes wrong:** `node_is_ibd_mode()` fires, handler returns a non-ECHO_OK code, and `rpc_execute_single` emits `{"error":{"code":-32603,"message":"Internal error"}}` instead of `{"error":{"code":-28}}`.
**Why it happens:** The handler dispatch in `rpc_execute_single` maps any non-ECHO_OK as -32603.
**How to avoid:** Need a mechanism for handlers to emit specific RPC error codes. Options: (a) handler writes the full error response to builder and returns ECHO_OK (breaks contract but works), (b) introduce an `rpc_error_t` out-param, (c) use a sentinel return code with associated state. Option (a) is simplest given the codebase.
**Warning signs:** Test checking for -28 response code sees -32603 instead.

### Pitfall 5: `default_witness_commitment` is scriptPubKey, not raw commitment hash
**What goes wrong:** The 32-byte commitment hash is emitted directly as hex, missing the OP_RETURN prefix and 4-byte magic header.
**Why it happens:** The "commitment" conceptually is the 32-byte SHA256d output, but the GBT field is the full output script.
**How to avoid:** Always format as `6a24aa21a9ed<32-bytes>` (38 bytes total, 76 hex chars).
**Warning signs:** Mining clients fail to construct valid coinbase (malformed witness commitment script).

### Pitfall 6: submitblock missing peer announcement
**What goes wrong:** Block is accepted and applied to local chain but never propagated to the network.
**Why it happens:** The existing stub calls `node_apply_block` but not `node_announce_block_to_peers`.
**How to avoid:** After successful `node_apply_block`, compute block hash and call `node_announce_block_to_peers`.
**Warning signs:** Block appears in local `getblockchaininfo` but network peers don't see it.

### Pitfall 7: `weight` field for transactions in template
**What goes wrong:** Emitting `vsize * 4` as weight is incorrect for SegWit transactions. Weight = base_size * 3 + total_size (or equivalently, non-witness * 3 + witness).
**Why it happens:** The existing stub uses `entry->vsize * 4` as an approximation.
**How to avoid:** Use `tx_weight()` directly, which is already implemented correctly in tx.h.
**Warning signs:** Template weight calculation doesn't match what miners expect; blocks near the weight limit get rejected.

---

## Code Examples

Verified patterns from the codebase:

### Existing MTP Computation (rpc.c:1662-1695)
```c
/* Source: src/app/rpc.c, rpc_getblockchaininfo, reuse this pattern */
uint32_t timestamps[11];
int ts_count = 0;
consensus_engine_t *cons_mut = node_get_consensus(node);
chainstate_t *cs = consensus_get_chainstate(cons_mut);
block_index_map_t *bim = chainstate_get_block_index_map(cs);
const block_index_t *cur = tip_index; /* tip_index from consensus_lookup_block_index */

while (cur != NULL && ts_count < 11) {
  timestamps[ts_count++] = cur->timestamp;
  if (cur->height == 0) break;
  cur = block_index_map_lookup(bim, &cur->prev_hash);
}
/* insertion sort */
for (int i = 1; i < ts_count; i++) {
  uint32_t key = timestamps[i];
  int j = i - 1;
  while (j >= 0 && timestamps[j] > key) { timestamps[j+1] = timestamps[j]; j--; }
  timestamps[j+1] = key;
}
uint32_t mtp = (ts_count > 0) ? timestamps[ts_count / 2] : 0;
```

### Witness Commitment API (include/merkle.h)
```c
/* Source: include/merkle.h - already implemented */

/* Compute witness merkle root from array of hash256_t:
 * hashes[0] = 32 zero bytes (coinbase wtxid by protocol)
 * hashes[1..N] = wtxid of each selected tx */
echo_result_t merkle_root(const hash256_t *hashes, size_t count, hash256_t *root);

/* Compute SHA256d(witness_root || witness_nonce) */
echo_result_t witness_commitment(const hash256_t *witness_root,
                                 const hash256_t *witness_nonce,
                                 hash256_t *commitment);

/* Compute wtxid of a transaction (includes witness data) */
echo_result_t tx_compute_wtxid(const tx_t *tx, hash256_t *wtxid);
/* Source: include/tx.h */
```

### submitblock Current Stub (src/app/rpc.c:2391-2456)
```c
/* Already wired correctly for parsing, validating, applying.
 * Missing: -28 guard, peer announcement */
echo_result_t rpc_submitblock(node_t *node, const json_value_t *params,
                              json_builder_t *builder) {
  /* ... parse hex, block_parse ... */
  bool valid = consensus_validate_block(consensus, &block, &validation_result);
  if (!valid) {
    block_free(&block);
    json_builder_string(builder, "invalid");
    return ECHO_OK;
  }
  res = node_apply_block(node, &block);
  block_free(&block);
  if (res != ECHO_OK) {
    json_builder_string(builder, "rejected");
    return ECHO_OK;
  }
  json_builder_null(builder); /* null = accepted */
  return ECHO_OK;
}
```

### node_announce_block_to_peers (include/node.h)
```c
/* Source: include/node.h - already exists */
void node_announce_block_to_peers(node_t *node, const hash256_t *block_hash);
/* Skips announcement during IBD (node_is_ibd_mode) automatically */
```

### Difficulty bits for mainnet in getblocktemplate
```c
/* Source: consensus.h - build validation context */
echo_result_t consensus_build_validation_ctx(const consensus_engine_t *engine,
                                             uint32_t height,
                                             full_block_ctx_t *ctx);
/* full_block_ctx_t.difficulty_ctx.prev_bits is the correct bits value */
/* Alternatively, read bits from tip_index->bits for next-block bits */
```

Note: The correct `bits` for the next block is NOT necessarily `tip_index->bits` — it may differ at retarget boundaries (every 2016 blocks). The existing stub uses a compile-time constant which is wrong for mainnet at retarget. To get the correct bits: call `consensus_build_validation_ctx()` which populates `difficulty_ctx`, then call `difficulty_compute_next()`.

---

## State of the Art

| Old Approach | Current Approach | Impact |
|--------------|------------------|--------|
| Template without witness commitment | BIP-145 `default_witness_commitment` | Mining pools need this to build valid SegWit blocks |
| Rough MTP estimate | Real MTP from block index walk | Miners build blocks with invalid timestamps without real MTP |
| No IBD guard | -28 error when syncing | Prevents miners from building on incomplete chain |

---

## Open Questions

1. **IBD guard response mechanism**
   - What we know: `rpc_execute_single` maps non-ECHO_OK to -32603, but spec requires -28.
   - What's unclear: Which pattern to use — write full error into builder and return ECHO_OK, or add a mechanism to `rpc_execute_single`.
   - Recommendation: Handler writes full error JSON into builder (including `"error":{"code":-28,...}`), marks builder with a sentinel, returns ECHO_OK. `rpc_execute_single` already wraps the builder content in a success response, so this won't work cleanly. **Best approach:** Modify `rpc_execute_single` to accept an out-param for RPC error code, or add a thin wrapper that the handler sets. The cleanest C11 approach: return `ECHO_ERR_INVALID_STATE` and have the caller check a separate context. OR, simpler: add an `rpc_error_t` thread-local or use the builder with a special prefix. Given the codebase philosophy (no over-engineering), the right fix is to modify `rpc_execute_single` to map `ECHO_ERR_INVALID_STATE` to `-28`.

2. **Correct `bits` for next block**
   - What we know: The current stub uses `DIFFICULTY_POWLIMIT_BITS` as a fallback for mainnet, which is wrong at retarget boundaries.
   - What's unclear: Does Phase 6 need to get this right, or is it acceptable to always use `tip_index->bits` (correct for non-retarget blocks, ~99.9% of blocks)?
   - Recommendation: Use `tip_index->bits` for non-retarget heights; call `difficulty_compute_next()` at retarget heights. The planner should check if the success criteria requires a specific test at a retarget boundary.

3. **`depends` field in transaction array**
   - What we know: BIP-22 requires `depends` to list prerequisite transactions by 1-based index.
   - What's unclear: The mempool tracks ancestor relationships but the selected array order may not map 1:1 to ancestor indices.
   - Recommendation: For Phase 6 baseline, emit an empty `depends` array for all transactions (conservative: mining software should handle missing depends gracefully). Full depends computation is an enhancement.

---

## Sources

### Primary (HIGH confidence)
- `include/merkle.h` — `merkle_root()`, `merkle_root_wtxids()`, `witness_commitment()` APIs verified in codebase
- `include/mining.h` — `coinbase_params_t.include_witness_commitment`, `coinbase_params_t.witness_commitment` fields confirmed
- `include/tx.h` — `tx_compute_wtxid()`, `tx_weight()`, `tx_vsize()` confirmed present
- `include/node.h` — `node_is_ibd_mode()`, `node_announce_block_to_peers()` confirmed present
- `src/app/rpc.c` — Full stub implementations read; dispatch table, MTP pattern, and existing structure confirmed
- `src/app/mining.c` — `coinbase_create()` implementation with witness commitment output confirmed
- `include/block_validate.h` — `WITNESS_COMMITMENT_PREFIX[4]` = `{0xaa, 0x21, 0xa9, 0xed}` confirmed
- BIP-22 (fetched) — Required getblocktemplate response fields documented
- BIP-145 (fetched) — SegWit extensions: `rules`, `weightlimit`, `txid`, `hash`, `weight` per tx, `default_witness_commitment`
- Bitcoin Core mining.cpp (fetched) — `default_witness_commitment` is a scriptPubKey hex; `-28` is `RPC_CLIENT_IN_INITIAL_DOWNLOAD`

### Secondary (MEDIUM confidence)
- Bitcoin Core miner.cpp (fetched) — Witness commitment delegated to `GenerateCoinbaseCommitment()`; coinbase witness nonce is 32 zero bytes by convention

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — all APIs verified present in include/*.h
- Architecture: HIGH — existing stub provides the scaffolding; additions are targeted
- Pitfalls: HIGH — most derived from reading actual code and protocol specs
- IBD error mechanism: MEDIUM — mechanism not yet decided; options documented

**Research date:** 2026-02-21
**Valid until:** Stable (Bitcoin protocol frozen; code structure changes only if major refactor)
