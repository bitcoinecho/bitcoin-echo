# Phase 2: Consensus Completeness - Research

**Researched:** 2026-02-20
**Domain:** Bitcoin Script consensus (Tapscript BIP-342), chain reorganization with UTXO undo, chainwork restoration — pure C11, no external libraries
**Confidence:** HIGH (all findings sourced directly from codebase, BIP-342 specification, and Bitcoin Core reference)

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| CONS-01 | Node validates OP_CHECKSIGADD (BIP-342 Tapscript) correctly for all key types including unknown-key-type upgrade rule | Bug confirmed at `src/consensus/script.c:4892-4897`: pubkey_elem.len != 32 returns SCRIPT_ERR_WITNESS_PUBKEYTYPE. BIP-342 says unknown key types (len != 32, non-zero) must succeed. Fix is a 3-branch conditional before the Schnorr verify call. |
| CONS-02 | Node performs full UTXO rollback on chain reorganization using delta undo system | Bug confirmed at `src/node/chaser_confirm.c:250`: TODO comment "Actually undo chainstate changes." The `chainstate_revert_block()` function exists and is correct (removes created, restores spent), but `chaser_confirm_reorganize()` never calls it. Fix: wire the delta retrieval and revert loop. |
| CONS-04 | Node recomputes chainwork correctly during reorg (stores prev_chainwork in block_delta_t) | Bug confirmed at `src/consensus/chainstate.c:733-740`: TODO comment "leave chainwork as-is." `block_delta_t` has no `prev_chainwork` field. Fix: add `work256_t prev_chainwork` to `block_delta_t`, store current chainwork before adding block's work in `chainstate_apply_block_with_txids()`, restore in `chainstate_revert_block()`. |
| CONS-05 | Node passes all BIP-342 reference test vectors for Tapscript validation | The BIP-342 reference test vectors are embedded in Bitcoin Core's qa-assets. The project has `test/vectors/script_tests.json` with basic tapscript tests (4 entries, no CHECKSIGADD vectors). Phase 2 must add a dedicated `test/unit/test_tapscript.c` with hand-coded vectors covering OP_CHECKSIGADD, unknown key types, OP_SUCCESS, and MINIMALIF. |
| TEST-01 | Test suite covers reorg scenarios: simple fork, deep reorg (6+ blocks), same-work competing chains | No reorg tests exist. A new `test/unit/test_reorg.c` must be written using the existing `chainstate_t` API. The `chain_reorg_t` / `chain_reorganize()` path in `chainstate.c:1100-1160` is the entry point. Tests require synthetic block construction and get_block_txs callback mock. |
| TEST-04 | Test suite covers Taproot script validation using BIP-342 reference vectors | `test/unit/test_script_vectors.c` exists but is not wired into `make test` or `run_all_tests.sh`. Phase 2 either extends this file with tapscript vectors or creates `test/unit/test_tapscript.c`. The planner should decide based on test file scope. |
</phase_requirements>

---

## Summary

Phase 2 is a targeted bug-fix and test phase with zero new infrastructure. All four implementation items (CONS-01, CONS-02, CONS-04) have confirmed root causes with known fix locations. The two test items (CONS-05, TEST-01) require new test files; the project's test framework and build system are already established.

The most structurally important work is CONS-02 + CONS-04 together: reorg UTXO rollback and chainwork restoration. These must land as a unit because `chainstate_revert_block()` already handles UTXO undo correctly but cannot restore chainwork without `prev_chainwork` in the delta. Implementing CONS-02 without CONS-04 leaves chainwork stale after every reorg. The planner should sequence CONS-04 (add field) before or in the same plan as CONS-02 (wire the call).

CONS-01 is the cleanest fix: a 3-branch conditional in `script_execute_tapscript()` at the pubkey validation point. The BIP-342 rule is unambiguous: pubkey_len == 0 means fail (empty sig path handles this separately); pubkey_len == 32 means Schnorr verify; any other length means succeed immediately (unknown key type upgrade rule). The current code returns `SCRIPT_ERR_WITNESS_PUBKEYTYPE` for non-32-byte pubkeys, which is wrong.

The test work (CONS-05, TEST-01) uses the existing `test_utils.h` framework, the existing `chainstate_t` API, and the existing `script_execute_tapscript()` path. No new test infrastructure is required. BIP-342 reference vectors for unknown-key-type and OP_CHECKSIGADD must be hand-coded in C because the project does not depend on external JSON test data beyond `test/vectors/script_tests.json`.

**Primary recommendation:** Sequence: 02-01 (OP_CHECKSIGADD fix) → 02-03 (add prev_chainwork field) → 02-02 (wire UTXO rollback, uses field from 02-03) → 02-04 (tapscript test vectors) → 02-05 (reorg tests). The field addition in 02-03 is a prerequisite for 02-02 correctness.

---

## Standard Stack

### Core

This phase is pure C11 with existing codebase components. No new libraries.

| Component | Location | Purpose | Notes |
|-----------|----------|---------|-------|
| `script_execute_tapscript()` | `src/consensus/script.c:4801` | Tapscript execution engine | Contains CONS-01 bug at line 4892 |
| `sig_verify(SIG_SCHNORR, ...)` | `src/consensus/sig_verify.c:127` | Schnorr signature verification | Calls `secp256k1_schnorrsig_verify()` correctly — no changes needed |
| `chainstate_revert_block()` | `src/consensus/chainstate.c:698` | UTXO rollback for one block | Correct implementation — just not called during reorg |
| `chaser_confirm_reorganize()` | `src/node/chaser_confirm.c:236` | Reorg orchestrator | Has TODO stub — needs delta retrieval + revert loop |
| `block_delta_t` | `include/chainstate.h:92` | Undo data for one block | Missing `prev_chainwork` field (CONS-04 adds it) |
| `chainstate_apply_block_with_txids()` | `src/consensus/chainstate.c:545` | Block application with delta creation | Must save chainwork before adding block work |
| `test_utils.h` | `test/unit/test_utils.h` | Test framework | `test_case()`, `test_pass()`, `test_fail()`, `test_suite_begin()`, `test_suite_end()` |

### Key API Signatures (Verified)

```c
/* chainstate.h - block_delta_t (current) */
typedef struct {
  hash256_t block_hash;
  uint32_t height;
  outpoint_t *created;
  size_t created_count;
  utxo_entry_t **spent;
  size_t spent_count;
  /* MISSING: work256_t prev_chainwork; */
} block_delta_t;

/* After CONS-04 fix: */
typedef struct {
  hash256_t block_hash;
  uint32_t height;
  outpoint_t *created;
  size_t created_count;
  utxo_entry_t **spent;
  size_t spent_count;
  work256_t prev_chainwork; /* ADD: chainwork before this block was applied */
} block_delta_t;

/* chainstate_revert_block() signature */
echo_result_t chainstate_revert_block(chainstate_t *state,
                                      const block_delta_t *delta);

/* Delta storage in chainstate (indexed by height) */
/* state->deltas[height] contains the delta for that height */
/* chainstate_prune_delta_at(state, height) frees it */

/* sig_verify for Schnorr (already correct) */
int sig_verify(sig_type_t type,         /* SIG_SCHNORR */
               const uint8_t *sig,      /* 64 bytes */
               size_t sig_len,
               const uint8_t *hash,     /* 32-byte sighash */
               const uint8_t *pubkey,   /* 32 bytes x-only */
               size_t pubkey_len,
               uint32_t flags);         /* 0 for Schnorr */
```

---

## Architecture Patterns

### Pattern 1: BIP-342 Unknown Key Type Rule

**What:** When OP_CHECKSIGADD encounters a pubkey of length != 32 (and != 0), the key is an unknown type and verification is considered successful without cryptographic checking. This is the BIP-342 soft-fork upgrade mechanism.

**Exact BIP-342 rule (from specification):**
- If pubkey_len == 0: fail with SCRIPT_ERR_PUBKEYTYPE (empty pubkey is invalid)
- If pubkey_len == 32: x-only key, perform Schnorr verify with BIP-340
- If pubkey_len != 0 and pubkey_len != 32: unknown key type, **treat as successful** (no cryptographic check)

**Current broken code (src/consensus/script.c:4891-4897):**
```c
/* Validate pubkey length */
if (pubkey_elem.len != 32) {
    element_free(&pubkey_elem);
    element_free(&sig_elem);
    ctx->error = SCRIPT_ERR_WITNESS_PUBKEYTYPE;
    return ECHO_ERR_SCRIPT_ERROR;  /* WRONG: fails unknown key types */
}
```

**Corrected pattern:**
```c
/* BIP-342 key type dispatch */
if (pubkey_elem.len == 0) {
    /* Empty pubkey: consensus failure */
    element_free(&pubkey_elem);
    element_free(&sig_elem);
    ctx->error = SCRIPT_ERR_PUBKEYTYPE;
    return ECHO_ERR_SCRIPT_ERROR;
} else if (pubkey_elem.len == 32) {
    /* Known key type: Schnorr verify (code continues below) */
} else {
    /* Unknown key type: BIP-342 upgrade rule — treat as success */
    element_free(&pubkey_elem);
    element_free(&sig_elem);
    n++;  /* Unknown key type counts as valid signature */
    stack_push_num(&ctx->stack, n);
    continue;
}
```

**Note:** The same unknown-key-type rule applies to `OP_CHECKSIG` and `OP_CHECKSIGVERIFY` in Tapscript. Read the Tapscript OP_CHECKSIG path (also in `script_execute_tapscript`) to see if it has the same bug.

### Pattern 2: UTXO Rollback During Reorg

**What:** When a reorg occurs, `chaser_confirm_reorganize()` must loop from tip to fork_point, retrieving the delta for each height and calling `chainstate_revert_block()`.

**Current broken code (src/node/chaser_confirm.c:249-260):**
```c
/* Roll back to fork point */
/* TODO: Actually undo chainstate changes */
uint32_t old_height = chaser->confirmed_height;

for (uint32_t h = old_height; h > fork_point; h--) {
    chaser_notify_height(&chaser->base, CHASE_REORGANIZED, h);
}

chaser->confirmed_height = fork_point;
```

**Corrected pattern:**
```c
/* Roll back to fork point — undo UTXO changes for each block */
uint32_t old_height = chaser->confirmed_height;

for (uint32_t h = old_height; h > fork_point; h--) {
    /* Retrieve delta for this height */
    block_delta_t *delta = chainstate_get_delta(chaser->chainstate, h);
    if (delta != NULL) {
        chainstate_revert_block(chaser->chainstate, delta);
    }
    /* Delta is retained in chainstate (pruned later) */
    chaser_notify_height(&chaser->base, CHASE_REORGANIZED, h);
}

chaser->confirmed_height = fork_point;
```

**Important:** `chainstate_t->deltas` is indexed by height. Need a getter function (check if `chainstate_get_delta()` exists or needs to be added). Look at `chainstate_prune_delta_at()` for the pattern: `state->deltas[height]`. A static inline accessor or a new function `chainstate_get_delta(state, height)` is cleaner than accessing the internal array directly.

**Delta availability:** Deltas are stored during `chainstate_apply_block()` if `delta_out != NULL`. Currently chaser_confirm calls `chainstate_apply_block()` — verify that it passes `&delta` or passes `NULL`. If passing `NULL`, the delta is never stored and the reorg cannot be undone. This is likely the second part of the CONS-02 bug.

### Pattern 3: prev_chainwork in block_delta_t

**What:** `chainstate_revert_block()` currently has a TODO for chainwork restoration. Adding `prev_chainwork` to `block_delta_t` and saving it before `work256_add()` in `chainstate_apply_block_with_txids()` allows correct restoration.

**Current broken code (src/consensus/chainstate.c:670-677):**
```c
/* Update chain tip */
state->tip.hash = block_hash;
state->tip.height = new_height;

/* Calculate and add block work */
work256_t block_work;
result = work256_from_bits(header->bits, &block_work);
work256_add(&state->tip.chainwork, &block_work, &state->tip.chainwork);
```

**Fix pattern (save before adding):**
```c
/* Save previous chainwork in delta BEFORE updating */
if (delta != NULL) {
    delta->prev_chainwork = state->tip.chainwork;  /* Save old value */
}

/* Update chain tip */
state->tip.hash = block_hash;
state->tip.height = new_height;

/* Calculate and add block work */
work256_t block_work;
result = work256_from_bits(header->bits, &block_work);
work256_add(&state->tip.chainwork, &block_work, &state->tip.chainwork);
```

**Restore in chainstate_revert_block():**
```c
/* Restore chainwork */
state->tip.chainwork = delta->prev_chainwork;
```

### Pattern 4: Delta Storage Wiring in chaser_confirm

Before the reorg fix works, verify whether `chaser_confirm_block()` passes `&delta` to `chainstate_apply_block()`. Read `src/node/chaser_confirm.c` starting at the `confirm_process_blocks()` function (line ~274) to see the block application call. If `delta_out = NULL` is passed, deltas are never stored. Fix: pass `&delta`, then call `chainstate_store_delta(state, height, delta)` (or equivalent). The chainstate internal `deltas[]` array is the storage mechanism — check if a storage setter function exists or needs to be added.

### Anti-Patterns to Avoid

- **Reverting blocks in forward order:** The reorg rollback loop MUST go from tip down to fork_point+1 (not fork_point up to tip). `chainstate_revert_block()` verifies the delta matches the current tip — reverting out of order will fail the hash check.
- **Forgetting to wire delta storage in apply:** The delta is only populated if `delta_out != NULL` is passed to `chainstate_apply_block()`. Without this, no deltas are stored and rollback is impossible.
- **Calling chainstate_prune_delta_at() before rollback:** Deltas must survive until the reorg actually uses them. The `DELTA_REORG_DEPTH = 550` window means deltas should not be pruned until 550+ blocks deep.
- **Modifying work256_compare/add/sub for DB:** Per project decision [01-03], byte reversal only at DB boundary. In-memory work arithmetic remains little-endian unchanged.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Unknown key type branch | New pubkey type registry | Simple `if (len != 32)` conditional | BIP-342 explicitly defines only 3 cases: 0, 32, other |
| UTXO undo data structure | Custom undo log | Existing `block_delta_t` + `chainstate_revert_block()` | Already implemented correctly — just needs wiring |
| Chainwork math | Custom big-integer for reorg | Existing `work256_t` + store/restore pattern | Single field save/restore is sufficient |
| Test key generation | Actual secp256k1 key generation | Hard-coded 32-byte test pubkeys + valid Schnorr signatures | Tests use synthetic data; real signing not needed in unit tests |
| BIP-342 test vector parser | JSON parser | Hard-coded C test vectors | Project has no JSON parsing in test framework; script_tests.json uses custom text format |

---

## Common Pitfalls

### Pitfall 1: OP_CHECKSIG in Tapscript Has the Same Unknown-Key-Type Bug

**What goes wrong:** The fix for OP_CHECKSIGADD (CONS-01) also applies to OP_CHECKSIG and OP_CHECKSIGVERIFY in Tapscript execution. If `script_execute_tapscript()` delegates to `script_exec_op()` for CHECKSIG, and `script_exec_op()` also fails for non-32-byte pubkeys, the tests will pass for CHECKSIGADD but fail for CHECKSIG.

**Why it happens:** The developer focuses on CHECKSIGADD (the explicit TODO) but misses the same rule in CHECKSIG.

**How to avoid:** Search for all pubkey length checks in the Tapscript execution path. Read `script_exec_op()` for OP_CHECKSIG handling and verify it also implements the unknown-key-type rule.

**Warning signs:** BIP-342 test vectors for CHECKSIG with 33-byte pubkeys fail even after fixing CHECKSIGADD.

### Pitfall 2: chaser_confirm Does Not Store Deltas During Apply

**What goes wrong:** Fixing `chaser_confirm_reorganize()` to call `chainstate_revert_block()` does nothing if no deltas were ever stored. The reorg loop finds `state->deltas[h] == NULL` for every height.

**Why it happens:** `chainstate_apply_block()` only creates a delta if `delta_out != NULL`. If `chaser_confirm_block()` passes `NULL`, no deltas are created.

**How to avoid:** Read `src/node/chaser_confirm.c` around `confirm_process_blocks()` to see the exact call to `chainstate_apply_block()`. If delta is not being requested, this must be fixed before the reorg rollback can work.

**Warning signs:** Reorg test verifies UTXO state but finds orphaned outputs still present — means revert never ran.

### Pitfall 3: Delta Storage Setter May Not Exist

**What goes wrong:** After creating a delta in `chaser_confirm_block()`, the delta needs to be stored in `chainstate->deltas[height]`. There may not be a public setter function for this.

**Why it happens:** The delta storage is an implementation detail of chainstate.c. The public API is `chainstate_prune_delta_at()` (deletes) but there may be no `chainstate_store_delta()`.

**How to avoid:** Check chainstate.h for a delta setter. If it doesn't exist, either (a) pass `&delta_out` directly to `chainstate_apply_block()` which has internal logic for this, or (b) add a `chainstate_store_delta(state, delta)` function. Look at how `chain_reorganize()` (chainstate.c:1100+) handles deltas — it may show the intended pattern.

**Warning signs:** Compilation error because no setter exists; or deltas silently discarded.

### Pitfall 4: Reorg Leaves height_index Stale

**What goes wrong:** `chainstate_revert_block()` restores UTXO set and chainwork but does it also update `state->height_index[]`? If not, `chainstate_get_block_at_height()` returns wrong hashes after reorg.

**Why it happens:** `chainstate_apply_block()` sets `state->height_index[new_height] = block_hash`. Revert should clear or update this.

**How to avoid:** Read `chainstate_revert_block()` (chainstate.c:698-743) carefully — specifically whether it touches `height_index`. If not, add the cleanup. The test for success criterion 4 (chainwork in block index) will catch this.

**Warning signs:** Post-reorg state passes UTXO check but `chainstate_get_block_at_height()` returns orphaned block hash.

### Pitfall 5: OP_SUCCESS Range in script_execute_tapscript

**What goes wrong:** The OP_SUCCESS range check may not match BIP-342 exactly, causing legitimate OP_SUCCESS opcodes to be executed rather than triggering immediate success.

**Why it happens:** The range `(op >= 0xbb && op <= 0xfe)` starts at 0xbb, but OP_CHECKSIGADD is 0xba. If the check was written as `>= 0xba`, it would treat CHECKSIGADD as an OP_SUCCESS, which is wrong. Conversely, if the range misses some success opcodes (e.g., gaps in the BIP-342 list), valid scripts fail.

**How to avoid:** The current code at script.c:4827-4830 looks correct — 0xbb starts after 0xba (OP_CHECKSIGADD). Verify against BIP-342 Appendix A success opcode list. The existing basic tapscript tests in `test/vectors/script_tests.json` don't cover OP_SUCCESS, so add explicit test vectors.

**Warning signs:** Tapscript script with OP_SUCCESS opcode fails validation instead of succeeding immediately.

---

## Code Examples

### OP_CHECKSIGADD Unknown Key Type Fix

```c
/* src/consensus/script.c, inside OP_CHECKSIGADD handler in script_execute_tapscript() */
/* After popping sig, n, and pubkey from stack: */

/* BIP-342 key type dispatch — REPLACE current pubkey length check */
if (pubkey_elem.len == 0) {
    /* Empty pubkey: invalid per BIP-342 */
    element_free(&pubkey_elem);
    element_free(&sig_elem);
    ctx->error = SCRIPT_ERR_PUBKEYTYPE;
    return ECHO_ERR_SCRIPT_ERROR;
} else if (pubkey_elem.len != 32) {
    /* Unknown key type: BIP-342 upgrade rule */
    /* No signature verification — treat as success */
    element_free(&pubkey_elem);
    element_free(&sig_elem);
    /* Signature must be empty or we'd have failed earlier on non-empty sig check */
    /* Actually per BIP-342: unknown key + non-empty sig still succeeds (no verification) */
    if (sig_elem.len > 0) {
        n++;  /* Unknown key type counts as valid */
    }
    /* sig_elem.len == 0 was handled earlier (push n unchanged) — but we're past that */
    stack_push_num(&ctx->stack, n);
    continue;
}
/* pubkey_elem.len == 32: x-only key, continue with Schnorr verify */
```

**Note:** Re-read BIP-342 carefully for the unknown-key-type + non-empty-sig interaction. The spec says "signature validation is considered to be successful" for unknown key types regardless of sig content. The empty-sig case (push n unchanged) is handled before reaching this point (line 4876-4880 in current code).

### block_delta_t Extension (chainstate.h)

```c
/* include/chainstate.h */
typedef struct {
  hash256_t block_hash; /* Hash of the applied block */
  uint32_t height;      /* Height of the applied block */

  /* UTXO changes */
  outpoint_t *created;
  size_t created_count;
  utxo_entry_t **spent;
  size_t spent_count;

  /* Chainwork undo (CONS-04 addition) */
  work256_t prev_chainwork; /* Chainwork before this block was applied */
} block_delta_t;
```

### chainstate_revert_block() Chainwork Restore (chainstate.c)

```c
/* Inside chainstate_revert_block(), after UTXO undo: */

/* Update chain tip - REPLACE the current TODO */
if (delta->height == 0) {
    memset(&state->tip.hash, 0, 32);
    state->tip.height = 0;
    work256_zero(&state->tip.chainwork);
} else {
    state->tip.hash = state->height_index[delta->height - 1];
    state->tip.height = delta->height - 1;
    state->tip.chainwork = delta->prev_chainwork;  /* Restore saved value */
}
```

### Test Pattern for Reorg (test_chainstate.c extension or new test_reorg.c)

```c
/* Synthetic reorg test — no real blocks needed */
static void test_simple_reorg(void) {
    test_case("simple 2-block reorg restores UTXO state");

    chainstate_t *cs = chainstate_create();
    /* Apply 3 blocks on chain A (heights 0-2) */
    /* Fork: apply 3 blocks on chain B from same ancestor */
    /* Verify: reorg from A tip to B tip */
    /* Assert: UTXOs from A blocks are gone, UTXOs from B blocks present */
    /* Assert: chainwork at tip reflects B's accumulated work */
    chainstate_destroy(cs);
    test_pass();
}
```

### Test Pattern for OP_CHECKSIGADD (test_tapscript.c or test_script.c extension)

```c
/* Hard-coded BIP-342 test: unknown key type succeeds */
static void test_checksigadd_unknown_key_type(void) {
    test_case("OP_CHECKSIGADD with unknown key type (33 bytes) succeeds");

    script_context_t ctx;
    script_context_init(&ctx, SCRIPT_VERIFY_TAPROOT);
    ctx.is_tapscript = ECHO_TRUE;

    /* Stack: [sig] [n=0] [pubkey (33 bytes, unknown type)] */
    uint8_t pubkey_33[33] = {0x02, /* compressed EC point prefix */ ...};
    uint8_t sig_64[64] = {/* any 64 bytes — crypto not checked for unknown type */};
    uint8_t n_zero[1] = {0};

    stack_push(&ctx.stack, sig_64, 64);
    stack_push(&ctx.stack, n_zero, 0);  /* n=0 as empty (script number encoding) */
    stack_push(&ctx.stack, pubkey_33, 33);

    uint8_t checksigadd_script[1] = {OP_CHECKSIGADD};
    echo_result_t res = script_execute_tapscript(&ctx, checksigadd_script, 1);

    /* Should succeed with n=1 on stack */
    /* ...assertions... */
    script_context_free(&ctx);
    test_pass();
}
```

---

## State of the Art

| Old Approach | Current State | Fix Required | Impact |
|--------------|---------------|--------------|--------|
| OP_CHECKSIGADD stub in script_exec_op() | Returns SCRIPT_ERR_BAD_OPCODE from legacy path; Tapscript path has real implementation but wrong pubkey length check | Fix unknown-key-type branch in tapscript path | Taproot multisig transactions fail |
| UTXO rollback not wired | TODO comment in chaser_confirm_reorganize() | Wire delta retrieval and revert loop | Reorg leaves orphaned UTXOs |
| Chainwork not restored | TODO in chainstate_revert_block() | Add prev_chainwork to delta, save/restore | Block index shows wrong accumulated work |
| No OP_CHECKSIGADD test vectors | script_tests.json has 4 basic tapscript tests, no CHECKSIGADD | Add test_tapscript.c with BIP-342 cases | Gap in test coverage |
| No reorg tests | No test_reorg.c | Create reorg tests using chainstate_t API | Reorg correctness unverified |

---

## Open Questions

1. **Does chaser_confirm_block() currently pass delta_out to chainstate_apply_block()?**
   - What we know: `chaser_confirm_reorganize()` has a TODO and does not call `chainstate_revert_block()`
   - What's unclear: Whether the block application path stores deltas in the chainstate. Must read `confirm_process_blocks()` (~line 274) fully before planning 02-02.
   - Recommendation: Planner must read `src/node/chaser_confirm.c:274-380` as first step of 02-02 plan before editing.

2. **Does chainstate_t expose delta retrieval publicly or is state->deltas[] private?**
   - What we know: `chainstate_prune_delta_at(state, height)` is the only public delta API. `state->deltas[]` is internal.
   - What's unclear: Whether 02-02 needs to add a public getter or can be done differently.
   - Recommendation: Two options: (a) add `chainstate_get_delta(state, height)` to chainstate.h/chainstate.c, or (b) pass delta out of `chainstate_apply_block()` and store it externally in chaser_confirm. Option (a) is cleaner given the existing `prune_delta_at` precedent.

3. **Do OP_CHECKSIG and OP_CHECKSIGVERIFY in script_execute_tapscript() also have the unknown-key-type bug?**
   - What we know: `script_execute_tapscript()` delegates non-CHECKSIGADD opcodes to `script_execute()` → `script_exec_op()`.
   - What's unclear: Whether `script_exec_op()`'s OP_CHECKSIG handler checks pubkey length and rejects non-32-byte keys in Tapscript context.
   - Recommendation: Read `script_exec_op()` OP_CHECKSIG handler before finalizing 02-01 plan. Fix all affected opcodes in the same plan.

4. **Are BIP-342 reference vectors for unknown-key-type available in a standard location?**
   - What we know: Bitcoin Core's qa-assets repository contains tapscript test vectors but is not bundled with this project. The existing `test/vectors/script_tests.json` has no CHECKSIGADD or unknown-key-type vectors.
   - What's unclear: Whether the planner should (a) fetch and embed specific vectors from BIP-341/342 or (b) hand-code representative vectors in C.
   - Recommendation: Hand-code vectors in C. Fetching external JSON at test time violates the project's "no external dependencies" principle, and the test framework has no JSON parser. The BIP-342 specification defines the rules clearly enough to write synthetic test vectors.

---

## Sources

### Primary (HIGH confidence)

- `src/consensus/script.c` — OP_CHECKSIGADD implementation at line 4842, pubkey length check at 4892, Tapscript executor at 4801; OP_CHECKSIGADD stub in legacy path at 3329
- `src/node/chaser_confirm.c` — `chaser_confirm_reorganize()` at 236 with TODO at 250
- `src/consensus/chainstate.c` — `chainstate_revert_block()` at 698 with chainwork TODO at 733; `chainstate_apply_block_with_txids()` at 545; delta storage via `state->deltas[]`
- `include/chainstate.h` — `block_delta_t` struct definition, missing `prev_chainwork`; `chainstate_revert_block()` signature; `chainstate_prune_delta_at()` API
- `include/script.h` — `script_execute_tapscript()` declaration, `TAPROOT_LEAF_VERSION_TAPSCRIPT`, `script_context_t` fields for tapscript
- `src/consensus/sig_verify.c` — Schnorr verification via `secp256k1_schnorrsig_verify()` at line 153; API confirmed correct
- `test/unit/test_utils.h` — Test framework: `test_case()`, `test_pass()`, `test_fail()`, `test_suite_begin()`, `test_suite_end()`
- `Makefile` — Build and test wiring pattern; new tests need: entry in `test:` target, `run_test` in `run_all_tests.sh`
- BIP-342 specification (bips.dev/342/) — Unknown key type rule: pubkey_len != 0 and != 32 means success without verification

### Secondary (MEDIUM confidence)

- Bitcoin Core GitHub issue #32012 — Confirms tapscript test coverage lives in qa-assets, not in the base repo's script_tests.json; tapscript unit test gaps are a known issue in Bitcoin Core itself

### Tertiary (LOW confidence)

- GitHub PR #19953 (Bitcoin Core BIP-340-342 implementation) — Reference for how Bitcoin Core structures the unknown-key-type check; not directly verified

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — all from direct codebase read
- Architecture (bug locations): HIGH — bugs confirmed with exact line numbers
- Architecture (fix patterns): HIGH — based on existing correct code in the same codebase
- Pitfalls: HIGH — based on reading the actual code paths and delta wiring gaps
- BIP-342 spec rules: HIGH — read from bips.dev/342/ specification directly
- Test vector approach: MEDIUM — hand-coded vectors are the right approach but exact vector content needs BIP-342 spec derivation during plan execution

**Research date:** 2026-02-20
**Valid until:** N/A — pure codebase research, valid until source files change
