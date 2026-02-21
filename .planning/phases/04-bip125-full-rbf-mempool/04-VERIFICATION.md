---
phase: 04-bip125-full-rbf-mempool
verified: 2026-02-21T18:00:00Z
status: passed
score: 4/4 must-haves verified
re_verification: false
gaps: []
human_verification: []
---

# Phase 04: BIP-125 Full-RBF Mempool Verification Report

**Phase Goal:** Echo's mempool correctly enforces all 5 BIP-125 replacement rules, making it compatible with Bitcoin Core v28+ full-RBF default policy and enabling accurate fee-market transaction selection for getblocktemplate
**Verified:** 2026-02-21T18:00:00Z
**Status:** passed
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| #   | Truth                                                                                             | Status     | Evidence                                                                                                                  |
| --- | ------------------------------------------------------------------------------------------------- | ---------- | ------------------------------------------------------------------------------------------------------------------------- |
| 1   | A signaling transaction satisfying all 5 BIP-125 rules replaces its conflicting predecessor      | VERIFIED   | `test_rbf_success` passes: replacement accepted, original evicted, `mempool_size == 1` confirmed                         |
| 2   | A replacement violating any rule is rejected with a specific error identifying which rule failed  | VERIFIED   | Distinct reject codes: `MEMPOOL_REJECT_CONFLICT` (Rules 1-2), `MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE` (Rules 3-4), `MEMPOOL_REJECT_RBF_TOO_MANY_REPLACED` (Rule 5) |
| 3   | Rule 3 enforces absolute fee totals — 4999-sat replacement against 5000-sat original is rejected  | VERIFIED   | `test_rbf_rule3_absolute_fee_trap`: output 95001 (fee 4999) against original fee 5000 returns `ECHO_ERR_INVALID` + `MEMPOOL_REJECT_RBF_INSUFFICIENT_FEE` |
| 4   | All 5 rules exercised by tests including inherited signaling and 100-tx eviction count limit      | VERIFIED   | 8 RBF test functions in `test_section("BIP-125 RBF Replacement Rules")`, all 33/33 tests pass                            |

**Score:** 4/4 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
| -------- | -------- | ------ | ------- |
| `src/protocol/mempool.c` | `rbf_validate_replacement()` + eviction-set builder + inherited signaling check | VERIFIED | All 3 static helpers present at lines 577, 617, 715; fully substantive implementation (not stubs) |
| `test/unit/test_mempool.c` | BIP-125 RBF test section with 8 test functions | VERIFIED | Section at line 375; 8 test functions at lines 391, 445, 554, 613, 662, 722, 799, 853; wired in `main()` at lines 1831-1839 |

### Key Link Verification

| From | To | Via | Status | Details |
| ---- | -- | --- | ------ | ------- |
| `rbf_validate_replacement()` | `mempool_add()` has_conflict branch | Called at line 1112 in `if (has_conflict)` block | WIRED | Return value checked; `ECHO_OK` falls through to insertion; error propagated immediately |
| `rbf_validate_replacement()` | `mempool_remove()` | Atomic eviction at Step F (lines 814-844) | WIRED | Direct conflict txids collected before any `mempool_remove` call to prevent pointer invalidation; descendants removed recursively by `mempool_remove` |
| `entry_signals_rbf_inherited()` | `txid_table_lookup()` | Recursive ancestor walk at line 587 | WIRED | Called for each input; returns true if any unconfirmed ancestor signals RBF |
| RBF test section in `main()` | `test_rbf_*` functions | `test_section` + `test_case` + `test_pass` at lines 1831-1839 | WIRED | All 8 test functions called unconditionally in `main()` |
| `test/unit/test_mempool.c` RBF tests | `mempool_add()` RBF path | Conflicting transactions asserted accept/reject with reason codes | WIRED | All 8 tests call `mempool_add` with conflicting transactions and assert `MEMPOOL_REJECT_RBF_*` reason codes |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
| ----------- | ----------- | ----------- | ------ | -------- |
| P2P-03 | 04-01-PLAN.md | Node implements BIP-125 full-RBF with all 5 replacement rules in mempool | SATISFIED | `rbf_validate_replacement()` implements all 5 rules in order (Steps A-F); `MEMPOOL_RBF_INCREMENT = 1000`, `MEMPOOL_MAX_REPLACEMENT_COUNT = 100` constants defined in `mempool.h`; conflict branch wired and tested |
| TEST-01 | 04-02-PLAN.md | BIP-125 RBF test suite validates all 5 replacement rules including edge cases | SATISFIED | 8 test functions covering: Rule 1 (signaling + inherited), Rule 2 (new unconfirmed inputs), Rule 3 (absolute-fee trap), Rule 4 (fee-rate increment), Rule 5 (eviction count limit), successful replacement, multi-conflict eviction; 33/33 pass |

No orphaned requirements: REQUIREMENTS.md shows both P2P-03 and TEST-01 mapped to Phase 4, and both are claimed by plans 04-01 and 04-02 respectively.

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
| ---- | ---- | ------- | -------- | ------ |
| — | — | None | — | — |

No TODO/FIXME/placeholder comments found. No stub return patterns. No empty implementations. The `if (has_conflict)` branch that previously contained a TODO stub is fully replaced with `rbf_validate_replacement()`.

### Human Verification Required

None. All goal behaviors are programmatically verifiable and confirmed:

- Replacement acceptance/rejection is a deterministic return code check (verified by test suite)
- Atomic eviction correctness is verified by `mempool_size` assertions post-replacement
- Rule 3 absolute-fee enforcement is verified by the exact 4999-vs-5000 sat edge case test

### Test Execution Results

```
Results: 33/33 tests passed
  - 25 pre-existing tests: all pass (no regressions)
  - 8 new RBF tests: all pass

BIP-125 RBF Replacement Rules:
  [PASS] RBF Rule 1: signaling required
  [PASS] RBF Rule 1: inherited signaling
  [PASS] RBF Rule 2: no new unconfirmed inputs
  [PASS] RBF Rule 3: absolute fee trap
  [PASS] RBF Rule 4: fee rate increment
  [PASS] RBF Rule 5: eviction count limit
  [PASS] RBF: successful replacement
  [PASS] RBF: multi-conflict eviction
```

Build: zero warnings, zero errors under clang-tidy.

### Commit Verification

All 4 commits documented in SUMMARY files are confirmed present in git history:

| Commit | Plan | Description |
| ------ | ---- | ----------- |
| `2b4f494` | 04-01 Task 1 | feat(04-01): add RBF eviction-set collector and inherited signaling helper |
| `486954c` | 04-01 Task 2 | feat(04-01): implement rbf_validate_replacement and wire into mempool_add |
| `a3c0aa6` | 04-02 Task 1 | feat(04-02): add RBF helpers and test functions for Rules 1-3 |
| `b88a13a` | 04-02 Task 2 | feat(04-02): add Rules 4-5 and success case RBF tests, wire into main |

### Notable Implementation Details

- `entry_signals_rbf_inherited()` uses NOLINT(misc-no-recursion) with justification; bounded by MEMPOOL_MAX_ANCESTORS (25)
- Eviction set built as fixed-size stack array (MAX_EVICTION_SET = MEMPOOL_MAX_REPLACEMENT_COUNT = 100) — no heap allocation
- Rule 3 checked before Rule 4: absolute fee trap cannot be masked by fee-rate check
- Atomic eviction: direct conflict txids stored before any `mempool_remove` call to prevent spent-table pointer invalidation
- `create_test_tx()` uses sequence `0xFFFFFFFE` which does NOT signal RBF (equal to, not less than, `TX_SEQUENCE_DISABLE_RBF`); new `create_test_tx_rbf()` correctly uses `0xFFFFFFFD`
- Rule 5 test uses wide-tree approach (root with 101 outputs + 101 children = 102-entry eviction set) because MEMPOOL_MAX_ANCESTORS (25) prevents deep chains from reaching 101 entries

---

_Verified: 2026-02-21T18:00:00Z_
_Verifier: Claude (gsd-verifier)_
