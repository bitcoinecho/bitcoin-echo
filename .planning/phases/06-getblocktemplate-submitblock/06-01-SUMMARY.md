---
phase: 06-getblocktemplate-submitblock
plan: 01
subsystem: rpc
tags: [rpc, mining, bip22, bip145, segwit, witness-commitment, ibd-guard]
dependency_graph:
  requires: [05-02]
  provides: [getblocktemplate-production, ibd-error-passthrough]
  affects: [rpc.c, rpc_execute_single, rpc_getblocktemplate]
tech_stack:
  added: [merkle.h]
  patterns: [MTP-walk, witness-commitment-computation, ECHO_ERR_INVALID_STATE-convention]
key_files:
  modified:
    - src/app/rpc.c
decisions:
  - "ECHO_ERR_INVALID_STATE convention: handlers return this code for IBD/not-ready; rpc_execute_single maps to -28. Cleaner than writing error JSON into builder."
  - "merkle_root() direct call (not merkle_root_wtxids()) because no coinbase tx_t exists at template generation time; calloc ensures coinbase wtxid slot is zero-initialized."
  - "consensus_build_validation_ctx + difficulty_compute_next for correct bits at retarget boundaries; falls back to tip_index->bits on failure."
metrics:
  duration: "3 min"
  completed: "2026-02-21"
  tasks_completed: 2
  files_modified: 1
---

# Phase 06 Plan 01: getblocktemplate Production Implementation Summary

Production BIP-22/BIP-145 getblocktemplate with IBD guard, real MTP computation, SegWit witness commitment using calloc+merkle_root+witness_commitment, and correct retarget-aware difficulty bits.

## Tasks Completed

| Task | Name | Commit | Files |
|------|------|--------|-------|
| 1 | IBD guard: rpc_execute_single error-code passthrough | af331ec | src/app/rpc.c |
| 2 | Upgrade rpc_getblocktemplate to production BIP-22/BIP-145 | 0992da5 | src/app/rpc.c |

## What Was Built

### Task 1: IBD Guard Error Passthrough

Modified `rpc_execute_single` to add an `else if` branch after the `ECHO_OK` check. When a handler returns `ECHO_ERR_INVALID_STATE`, the dispatcher now emits `{"error":{"code":-28,"message":"Bitcoin Echo is downloading blocks..."}}` — the standard Bitcoin Core warmup error that mining clients handle gracefully. All other non-OK codes still map to `-32603 Internal error`. This convention is used by both `rpc_getblocktemplate` (this plan) and `rpc_submitblock` (plan 06-02).

### Task 2: Production getblocktemplate

Replaced the stub body with a full BIP-22/BIP-145 implementation:

**IBD guard:** `node_is_ibd_mode(node)` check at function top returns `ECHO_ERR_INVALID_STATE`, which rpc_execute_single maps to -28.

**Real MTP computation:** Walks up to 11 blocks back through `block_index_map` from `tip_index`, collects timestamps, insertion-sorts, takes median. Sets `mintime = mtp + 1` per BIP-22 (strictly greater than MTP). Pattern copied directly from `rpc_getblockchaininfo`.

**Correct difficulty bits:** Uses `consensus_build_validation_ctx(consensus, next_height, &block_ctx)` then `difficulty_compute_next(&block_ctx.difficulty_ctx, &bits)`. Handles retarget boundaries (every 2016 blocks) correctly. Falls back to `tip_index->bits` if context build fails.

**BIP-145 rules array:** Emits `"rules":["segwit"]` — Echo is mainnet-only post-SegWit activation (height 481824), so all templates require it.

**Witness commitment:** Allocates `hash256_t *wtxids = calloc(1 + selected_count, sizeof(hash256_t))` — zero-initialization ensures coinbase wtxid slot (index 0) is the required 32 zero bytes per BIP-141. Calls `tx_compute_wtxid` for each selected transaction. Calls `merkle_root(wtxids, wtxid_count, &witness_root)` directly (not `merkle_root_wtxids()` which needs a real coinbase tx_t). Calls `witness_commitment(&witness_root, &nonce, &commitment)` with zero nonce. Builds 38-byte scriptPubKey: `0x6a 0x24 0xaa 0x21 0xa9 0xed <32-byte-commitment>`. Emits as `"default_witness_commitment":"<76-hex-chars>"`.

**Per-transaction BIP-145 fields:** Added `"hash"` (wtxid from cached `entry->wtxid`), fixed `"weight"` to use `tx_weight(&entry->tx)` instead of `entry->vsize * 4`, added empty `"depends":[]` array. Existing `"data"`, `"txid"`, `"fee"` fields preserved.

**New include:** Added `#include "merkle.h"` for `merkle_root()` and `witness_commitment()`.

## Verification Results

1. `make` compiles with zero errors, zero warnings
2. `default_witness_commitment` field emitted in rpc_getblocktemplate
3. `ECHO_ERR_INVALID_STATE` mapped to `RPC_ERR_CLIENT_IN_WARMUP` (-28) in rpc_execute_single
4. `difficulty_compute_next` used for correct bits at retarget boundaries
5. `merkle_root` from merkle.h used for witness commitment tree
6. `tx_weight()` used for correct transaction weight

## Deviations from Plan

None — plan executed exactly as written.

## Self-Check: PASSED
