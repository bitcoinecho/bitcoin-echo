# Phase 4: BIP-125 Full-RBF Mempool — Research

**Researched:** 2026-02-21
**Domain:** Bitcoin mempool policy — BIP-125 Replace-by-Fee enforcement in pure C
**Confidence:** HIGH

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| P2P-03 | Node implements BIP-125 full-RBF with all 5 replacement rules in mempool | Full rule text documented below; existing `mempool_add` has a `TODO` stub at the exact insertion point |
| TEST-01 | BIP-125 RBF test suite validates all 5 replacement rules including edge cases | Existing `test/unit/test_mempool.c` infrastructure is complete; test compilation target already defined in Makefile |
</phase_requirements>

---

## Summary

Phase 4 is a focused completion of work that was deliberately stubbed out. The `mempool_add` function in `src/protocol/mempool.c` contains an explicit `TODO` comment at line 799: "TODO: Implement full RBF replacement logic — For now, reject conflicts even with RBF signaling." That is the single insertion point for Plan 04-01. All supporting data structures, constants, and collision detection infrastructure are already present and working. The task is to replace the stub rejection with the real 5-rule BIP-125 eviction-set algorithm.

Plan 04-02 adds a new test section inside the existing `test/unit/test_mempool.c` file. The test framework (`test_utils.h`), mock UTXO infrastructure, and Makefile compilation target for `test_mempool` all exist and compile today. No new files need to be created for testing; new test functions are added to the existing file and wired into `main()`.

The key technical risk is the **absolute-fee trap in Rule 3**: the replacement must pay more absolute satoshis than the sum of all transactions it evicts, not just a higher fee rate. This is the most commonly mis-implemented rule. The existing `mempool_accept_result_t` struct only carries `first_conflict` — for multi-conflict RBF scenarios the implementation needs to collect the full eviction set before running fee checks.

**Primary recommendation:** Implement the 5-rule BIP-125 check as a single `rbf_validate_replacement()` helper inside `mempool.c`, called from the existing conflict branch in `mempool_add`. The helper builds the eviction set, checks all 5 rules, then the caller performs the atomic swap. Tests go in `test_mempool.c` as a new RBF section.

---

## Standard Stack

### Core
| Component | Version | Purpose | Why Standard |
|-----------|---------|---------|--------------|
| `src/protocol/mempool.c` | existing | RBF logic lives here | Already owns the `mempool_add` conflict path |
| `test/unit/test_mempool.c` | existing | RBF test suite | Existing mock UTXO callbacks and `create_test_tx` helpers ready |
| `test/unit/test_utils.h` | existing | Test framework | `test_case`/`test_pass`/`test_fail*` pattern used throughout |

### Supporting
| Component | Purpose | When to Use |
|-----------|---------|-------------|
| `MEMPOOL_MAX_REPLACEMENT_COUNT 100` | Already-defined eviction limit | Rule 5 constant — use directly, do not magic-number |
| `MEMPOOL_RBF_INCREMENT 1000` | Fee-rate increment constant (sat/kvB) | Used for Rule 4 — already defined in `mempool.h` |
| `TX_SEQUENCE_DISABLE_RBF 0xFFFFFFFE` | RBF signal threshold | Already defined in `tx.h`; `entry->signals_rbf` is set from this in `entry_create` |
| `mempool_remove()` | Recursive descendant removal | Use for evicting conflicts and their descendants |
| `spent_lookup()` | Conflict detection | Already called in `mempool_add` — feeds the eviction set building loop |

### No External Dependencies

This is pure C11 + stdlib. No new libraries. No changes to the Makefile `TEST_MEMPOOL` target dependencies — the target already links everything needed:

```
$(TEST_MEMPOOL): test/unit/test_mempool.c src/protocol/mempool.c \
  src/consensus/tx_validate.c src/consensus/script.c \
  src/consensus/sig_verify.c src/consensus/utxo.c src/consensus/block.c \
  src/consensus/tx.c src/consensus/serialize.c src/crypto/sha256.c \
  src/crypto/sha1.c src/crypto/ripemd160.c src/crypto/secp256k1.c \
  src/platform/posix.c src/app/log.c $(TEST_UTILS_OBJ) $(LIBSECP_OBJS)
```

---

## Architecture Patterns

### The Existing Conflict Path — Where Implementation Goes

In `mempool_add` (line 798–807 in `src/protocol/mempool.c`), the current stub is:

```c
/* Handle RBF conflicts */
if (has_conflict) {
    /* TODO: Implement full RBF replacement logic */
    /* For now, reject conflicts even with RBF signaling */
    if (result != NULL) {
        result->reason = MEMPOOL_REJECT_CONFLICT;
        result->conflicts_count = 1;
        result->first_conflict = first_conflict;
    }
    return ECHO_ERR_INVALID;
}
```

This `if (has_conflict)` block is replaced with a call to a new internal helper that implements all 5 BIP-125 rules. If the helper returns `ECHO_OK`, execution continues to the normal insertion path. If it returns an error, `mempool_add` returns the error immediately.

### The RBF Validation Algorithm (5-Rule Sequence)

The helper must:

1. **Build the complete eviction set** — walk all conflicting transactions (direct conflicts found via `spent_lookup`) and collect all their descendants via a BFS/DFS through the mempool.
2. **Check Rule 1** — every transaction in the eviction set must `signals_rbf == true`. If any conflict was not signaling (or inherited from a non-signaling ancestor), reject with `MEMPOOL_REJECT_CONFLICT`.
3. **Check Rule 2** — the replacement may only have unconfirmed inputs that were inputs of the transactions being evicted. Specifically, new inputs that spend unconfirmed mempool outputs not in the eviction set are forbidden.
4. **Check Rule 5** — `eviction_set_size <= MEMPOOL_MAX_REPLACEMENT_COUNT`. Reject with `MEMPOOL_REJECT_RBF_TOO_MANY_REPLACED` if exceeded.
5. **Check Rule 3** — `replacement_fee >= sum(evicted entry fees)`. Reject with `MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE`.
6. **Check Rule 4** — `replacement_fee_rate >= conflict_fee_rate + MEMPOOL_RBF_INCREMENT`. Reject with `MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE`.
7. **Atomically evict and insert** — call `mempool_remove()` for each direct conflict (which recursively removes their descendants), then insert the replacement via the normal insertion path.

The `mempool_remove()` function already handles recursive descendant removal and cleans up spent outpoints. This is the correct eviction mechanism.

### Eviction Set Data Structure

A fixed-size array on the stack works given the Rule 5 cap of 100. No heap allocation needed for the eviction set itself:

```c
#define MAX_EVICTION_SET MEMPOOL_MAX_REPLACEMENT_COUNT
mempool_entry_t *eviction_set[MAX_EVICTION_SET];
size_t eviction_count = 0;
```

Collect direct conflicts first, then collect each conflict's descendants using the spent-outpoint table. Mark visited entries to avoid duplicates (use a visited flag or check presence in the set before adding).

### RBF Signaling Inheritance (Rule 1 Detail)

BIP-125 Rule 1 says "explicitly or through inheritance." A transaction **inherits** RBF signaling if any of its unconfirmed ancestor transactions signals RBF. The `entry->signals_rbf` field in `mempool_entry_t` is currently set only for direct signaling:

```c
/* Check RBF signaling (any input with sequence < MAX-1) */
entry->signals_rbf = false;
for (size_t i = 0; i < tx->input_count; i++) {
    if (tx->inputs[i].sequence < TX_SEQUENCE_DISABLE_RBF) {
        entry->signals_rbf = true;
        break;
    }
}
```

For Rule 1, the check in `rbf_validate_replacement()` should be:

```c
/* A conflict signals RBF if it directly signals OR if any ancestor in
 * the mempool signals (inherited signaling per BIP-125 Rule 1). */
static bool entry_signals_rbf_inherited(const mempool_t *mp,
                                        const mempool_entry_t *entry) {
    if (entry->signals_rbf) return true;
    /* Check if any unconfirmed input's parent signals */
    for (size_t i = 0; i < entry->tx.input_count; i++) {
        const mempool_entry_t *parent = txid_table_lookup(
            mp, &entry->tx.inputs[i].prevout.txid);
        if (parent != NULL && entry_signals_rbf_inherited(mp, parent)) {
            return true;
        }
    }
    return false;
}
```

Depth is bounded by `MEMPOOL_MAX_ANCESTORS` (25), so recursion is safe. The NOLINT pattern for recursion is already established in `mempool_remove`.

### What Changes in `mempool_accept_result_t`

The current result struct only has `first_conflict` and `conflicts_count`. For correct error reporting (Success Criteria 2: "specific error identifying which rule failed"), the existing `mempool_reject_t` enum already has:
- `MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE`
- `MEMPOOL_REJECT_RBF_TOO_MANY_REPLACED`
- `MEMPOOL_REJECT_CONFLICT` (reused for Rule 1 failure)

No new enum values are needed. The `result->reason` field carries which rule failed.

### Full-RBF vs. Opt-In RBF

Bitcoin Core v28+ defaults to **full-RBF** (any transaction can be replaced if replacement satisfies Rules 1-5, regardless of nSequence signaling). The phase goal says "compatible with Bitcoin Core v28+ full-RBF default policy." However, the Success Criteria use the phrase "signals RBF (nSequence < 0xFFFFFFFE on at least one input)" — this is opt-in RBF (BIP-125 Rule 1 as written), not full-RBF (which would drop Rule 1 signaling requirement). The `echo_policy.h` has `POLICY_ENABLE_RBF` commented out but acknowledges both modes.

**Resolution:** Implement BIP-125 opt-in RBF (Rules 1-5 as written). The phase success criteria explicitly check for signaling. Full-RBF (ignoring the signal) is an additional policy toggle, not a correctness requirement for this phase. This matches what `POLICY_ENABLE_RBF` in `echo_policy.h` implies.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Descendant removal during eviction | Custom graph walk | `mempool_remove()` | Already handles recursive descendant removal and spent-table cleanup correctly |
| Conflict detection | New spent-outpoint scan | `spent_lookup()` (already called in `mempool_add`) | Conflicts are already collected in the `has_conflict` / `first_conflict` path; extend it to collect all conflicts |
| Eviction set deduplication | Hash table | Fixed array with size check | Rule 5 caps at 100; linear scan over 100 entries is trivial |
| Test transaction construction | New helpers | Existing `create_test_tx()` and `make_txid()` in test_mempool.c | Fully functional; create variant `create_test_tx_no_rbf()` for non-signaling originals |

**Key insight:** The mempool already has all data structures needed for RBF. `spent_lookup` finds conflicts, `mempool_remove` evicts recursively, `mempool_entry_t.signals_rbf` tracks signaling. The only missing piece is the validation logic that connects them.

---

## Common Pitfalls

### Pitfall 1: Rule 3 — Absolute Fee, Not Fee Rate
**What goes wrong:** Checking that `replacement_fee_rate > evicted_fee_rate` and calling it sufficient. A replacement paying 4999 sats at 50 sat/vB that evicts a 5000-sat original at 5 sat/vB passes the rate check but fails the absolute fee check.
**Why it happens:** Rule 4 (fee rate) is more intuitive; Rule 3 (absolute total) is easy to skip.
**How to avoid:** Compute `satoshi_t total_evicted_fees = sum of entry->fee for all eviction set members` before checking Rule 4. Rule 3 check: `replacement_fee >= total_evicted_fees`.
**Warning signs:** Test "replacement paying 4999 sats against 5000-sat original" passes when it should fail.

### Pitfall 2: Eviction Set Must Include All Descendants
**What goes wrong:** Building the eviction set with only the direct conflicts (the entries `spent_lookup` returns), missing their children.
**Why it happens:** `has_conflict` in the current `mempool_add` only collects direct conflicts.
**How to avoid:** After collecting direct conflicts, walk each conflict's outputs and call `spent_lookup` to find children, grandchildren, etc., up to the Rule 5 limit.
**Warning signs:** Rule 5 (eviction count) test passes with a single-level chain but fails with a 2-deep descendant chain.

### Pitfall 3: Rule 2 — New Unconfirmed Inputs
**What goes wrong:** Not checking whether the replacement introduces new unconfirmed inputs that were not in the transactions being replaced.
**Why it happens:** Rule 2 is less commonly tested. It prevents "laundering" — using RBF to link new unconfirmed parents into the transaction.
**How to avoid:** For each input of the replacement transaction, if `txid_table_lookup(mp, &prevout->txid) != NULL` (unconfirmed parent), verify that parent is in the eviction set.
**Warning signs:** A replacement that adds a new mempool-spending input is accepted when it should be rejected.

### Pitfall 4: Atomic Eviction — Don't Remove Before Validating All 5 Rules
**What goes wrong:** Removing conflicting transactions before completing all 5 rule checks. If Rule 4 fails after Rule 3 passed, the mempool is now in a torn state.
**How to avoid:** Build the eviction set and check all 5 rules before calling `mempool_remove()` on any conflict. Only evict after all rules pass.
**Warning signs:** Mempool size is wrong after a failed replacement attempt.

### Pitfall 5: Inherited Signaling — Descendants Must Also Be Checked
**What goes wrong:** Checking only whether the direct conflict signals RBF, but not its ancestors. A transaction that itself doesn't signal but spends an output from an RBF-signaling transaction inherits the signal.
**Why it happens:** `entry->signals_rbf` only reflects direct signaling, not inherited.
**How to avoid:** Use `entry_signals_rbf_inherited()` helper as shown above — check ancestors recursively.
**Warning signs:** Test for inherited signaling propagation fails.

### Pitfall 6: `mempool_remove` Modifies the Spent Table — Call Order Matters
**What goes wrong:** Calling `mempool_remove(mp, &conflict->txid)` for multiple conflicts in sequence, where removing the first conflict causes the second conflict's spent-entry to disappear (because a common ancestor was shared).
**How to avoid:** Collect all direct conflict txids first, then call `mempool_remove` for each. The recursive descent handles their children. Alternatively, verify each `txid_table_lookup` is still valid before removing.

---

## Code Examples

### Pattern 1: Existing Conflict Collection (extend this)

```c
/* Source: src/protocol/mempool.c mempool_add, line 688-707 */
for (size_t i = 0; i < tx->input_count; i++) {
    const outpoint_t *prevout = &tx->inputs[i].prevout;

    /* Check if spent by mempool tx (conflict detection) */
    mempool_entry_t *spender = spent_lookup(mp, prevout);
    if (spender != NULL) {
        if (!has_conflict) {
            has_conflict = true;
            first_conflict = spender->txid;
        }
        /* BIP-125 Rule 1: original must signal (or inherit) RBF */
        if (!spender->signals_rbf) {
            /* ORIGINAL doesn't signal — reject */
            ...
        }
    }
}
```

For Phase 04-01, the conflict collection loop should populate an eviction array instead of just setting `has_conflict`. The `if (has_conflict)` block that follows then calls the 5-rule validator with that array.

### Pattern 2: NOLINT Annotation for Recursion

```c
/* Source: src/protocol/mempool.c line 901-903 */
/* NOLINTBEGIN(misc-no-recursion) - Recursion is intentional: removing a
 * transaction must also remove all descendants (transactions spending its
 * outputs). Depth is bounded by MEMPOOL_MAX_DESCENDANTS (25). */
echo_result_t mempool_remove(mempool_t *mp, const hash256_t *txid) {
```

Any new recursive helper (e.g., `entry_signals_rbf_inherited`) must use the same NOLINT annotation pattern with a justification comment.

### Pattern 3: Test Function Structure

```c
/* Source: test/unit/test_mempool.c (all existing test functions) */
static void test_rbf_rule3_absolute_fee_trap(void) {
    mempool_t *mp = mempool_create();
    mempool_callbacks_t cb = create_test_callbacks();
    mempool_set_callbacks(mp, &cb);

    /* Original: 5000 sat fee, ~100 vB, fee_rate = ~50000 sat/kvB */
    hash256_t input1 = make_txid(200);
    add_mock_utxo(&input1, 0, 100000, false);
    tx_t original;
    create_test_tx(&original, &input1, 0, 95000); /* 5000 sat fee */
    mempool_accept_result_t result;
    mempool_add(mp, &original, &result);
    ASSERT_EQ(result.reason, MEMPOOL_ACCEPT_OK, "original accepted");

    /* Replacement: 4999 sat fee at much higher fee rate — must FAIL Rule 3 */
    /* (same input, different output value, still signals RBF) */
    tx_t replacement;
    create_test_tx(&replacement, &input1, 0, 95001); /* only 4999 sat fee */
    echo_result_t err = mempool_add(mp, &replacement, &result);
    ASSERT_EQ(err, ECHO_ERR_INVALID, "Rule 3 violation must be rejected");
    ASSERT_EQ(result.reason, MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE,
              "reason must be insufficient fee");

    tx_free(&original);
    tx_free(&replacement);
    clear_mock_utxos();
    mempool_destroy(mp);
}
```

Test functions are `static void`, call `test_case()`/`test_pass()` inline in `main()`, and use the `ASSERT_*` macros defined at the top of `test_mempool.c`.

### Pattern 4: Main Loop Wiring

```c
/* Source: test/unit/test_mempool.c main(), lines 1130-1156 */
/* New RBF tests are added as additional lines in the same style: */
test_section("BIP-125 RBF Replacement Rules");
test_case("RBF Rule 1: signaling required"); test_rbf_rule1_signaling(); test_pass();
test_case("RBF Rule 1: inherited signaling"); test_rbf_rule1_inherited(); test_pass();
test_case("RBF Rule 2: no new unconfirmed inputs"); test_rbf_rule2_no_new_unconfirmed(); test_pass();
test_case("RBF Rule 3: absolute fee trap"); test_rbf_rule3_absolute_fee_trap(); test_pass();
test_case("RBF Rule 4: fee rate increment"); test_rbf_rule4_fee_rate(); test_pass();
test_case("RBF Rule 5: eviction count limit"); test_rbf_rule5_eviction_limit(); test_pass();
test_case("RBF: successful replacement"); test_rbf_success(); test_pass();
test_case("RBF: multi-conflict eviction"); test_rbf_multi_conflict(); test_pass();
```

---

## The 5 BIP-125 Rules — Precise Implementation Spec

Source: https://github.com/bitcoin/bips/blob/master/bip-0125.mediawiki (HIGH confidence)

| Rule | Check | Reject Code |
|------|-------|-------------|
| 1 | Every transaction in eviction set must signal RBF (directly or inherited via unconfirmed ancestors) | `MEMPOOL_REJECT_CONFLICT` |
| 2 | Replacement may only have unconfirmed inputs that appeared in the transactions being replaced (no new unconfirmed parents from outside eviction set) | `MEMPOOL_REJECT_CONFLICT` |
| 3 | `replacement_fee >= sum(eviction_set[i]->fee)` — absolute satoshis, not rate | `MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE` |
| 4 | `replacement_fee_rate >= mempool_min_fee_rate(mp) + MEMPOOL_RBF_INCREMENT` — replacement must pay above the relay minimum at its own size | `MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE` |
| 5 | `eviction_set_count <= MEMPOOL_MAX_REPLACEMENT_COUNT (100)` | `MEMPOOL_REJECT_RBF_TOO_MANY_REPLACED` |

**Rule 4 precise computation:** Bitcoin Core adds the incremental relay fee (1 sat/vB by default = `MEMPOOL_RBF_INCREMENT` sat/kvB) times the replacement's vsize to the total required fee. The replacement must cover the combined fee of all evicted transactions PLUS pay for its own relay bandwidth at the minimum rate. This is already encoded in `MEMPOOL_RBF_INCREMENT`.

Formula: `replacement_fee >= total_evicted_fees + (MEMPOOL_RBF_INCREMENT * replacement_vsize / 1000)`

---

## Current State of the Codebase

### What Already Exists (Do Not Rebuild)
- `mempool_entry_t.signals_rbf` field (set in `entry_create`)
- `MEMPOOL_MAX_REPLACEMENT_COUNT 100` and `MEMPOOL_RBF_INCREMENT 1000` constants
- `MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE` and `MEMPOOL_REJECT_RBF_TOO_MANY_REPLACED` enum values
- `mempool_reject_string()` already has strings for both RBF rejection codes
- `mempool_remove()` with recursive descendant removal
- `spent_lookup()`, `txid_table_lookup()` internal helpers
- Complete test infrastructure in `test_mempool.c` with mock callbacks
- `TEST_MEMPOOL` Makefile target with correct link dependencies

### What Is Stubbed / Missing
- Line 799–807 of `mempool.c`: the `has_conflict` branch rejects all RBF; replace with real logic
- No test coverage for any RBF acceptance or multi-rule validation (existing `test_mempool_conflict_detection` tests rejection of non-RBF conflict, not RBF replacement)

### What Must NOT Change
- `mempool.h` public API — no new public functions needed; the 5-rule logic is internal
- `mempool_accept_result_t` struct — existing fields (`reason`, `conflicts_count`, `first_conflict`) are sufficient
- `mempool_reject_t` enum — existing codes cover all RBF failure modes

---

## State of the Art

| Old Approach | Current Approach | Notes |
|--------------|------------------|-------|
| Opt-in RBF only (BIP-125, ~2015) | Full-RBF default (Bitcoin Core v28+, Oct 2024) | Echo implements BIP-125 opt-in rules — compatible with Core v28+ since Core accepts both signaling and non-signaling replacement under full-RBF; Echo's stricter (requires signal) is a valid policy subset |
| `mempoolfullrbf=0` default (Core pre-v28) | `mempoolfullrbf=1` default (Core v28+) | Echo implements the signaling-required variant (opt-in), which is correct for BIP-125 compliance even if Core's default relaxed it |

---

## Open Questions

1. **Rule 4 exact formula**
   - What we know: `MEMPOOL_RBF_INCREMENT 1000` is 1 sat/vB minimum relay fee
   - What's unclear: Bitcoin Core's implementation adds `incrementalRelayFee.GetFee(replacement.GetVirtualSize())` to the total evicted fees. Is the constant `MEMPOOL_RBF_INCREMENT` already calibrated for this? Answer: yes, `MEMPOOL_RBF_INCREMENT` is sat/kvB so the formula is `total_evicted_fees + (MEMPOOL_RBF_INCREMENT * vsize / 1000)`.
   - Recommendation: Use that formula; document the derivation in a comment.

2. **`mempool_accept_result_t.conflicts_count` field**
   - Currently set to `1` in the stub. For multi-conflict RBF, should this reflect the full eviction set size or just direct conflicts?
   - Recommendation: Set to `eviction_count` (full eviction set including descendants) — this is most informative for callers and matches intent of the field.

---

## Sources

### Primary (HIGH confidence)
- BIP-125 specification: https://github.com/bitcoin/bips/blob/master/bip-0125.mediawiki — 5 rules text directly verified
- `src/protocol/mempool.c` — complete implementation read; stub at line 799 confirmed
- `include/mempool.h` — all constants, structs, and enum values verified
- `include/tx.h` — `TX_SEQUENCE_DISABLE_RBF 0xFFFFFFFE` confirmed
- `test/unit/test_mempool.c` — complete test file read; infrastructure confirmed working
- `Makefile` lines 92, 233 — `TEST_MEMPOOL` target and link dependencies confirmed

### Secondary (MEDIUM confidence)
- Bitcoin Core v28 release notes: full-RBF default change verified via BIP-125 cross-reference

---

## Metadata

**Confidence breakdown:**
- BIP-125 rule text: HIGH — read directly from specification
- Existing code state: HIGH — read actual source files
- Implementation approach: HIGH — matches existing patterns in the codebase
- Test patterns: HIGH — existing test file fully read and patterns extracted
- Rule 4 exact formula: HIGH — derivable from `MEMPOOL_RBF_INCREMENT` constant definition

**Research date:** 2026-02-21
**Valid until:** Stable — BIP-125 is finalized; mempool.c internal structure won't change within this phase
