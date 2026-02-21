---
phase: 02-consensus-completeness
plan: 01
subsystem: consensus/script
tags: [bip-342, tapscript, checksigadd, checksig, unknown-key-type, minimalif]
requirements: [CONS-01, CONS-05, TEST-04]

dependency_graph:
  requires: []
  provides: [CONS-01, CONS-05, TEST-04]
  affects: [script_execute_tapscript]

tech_stack:
  added: []
  patterns:
    - BIP-342 three-branch key type dispatch (empty/32-byte/unknown)
    - Tapscript-specific opcode handlers before delegation to standard executor

key_files:
  created:
    - test/unit/test_tapscript.c
  modified:
    - src/consensus/script.c
    - Makefile
    - test/run_all_tests.sh

decisions:
  - "BIP-342 unknown key type dispatch applies to both OP_CHECKSIGADD and OP_CHECKSIG/OP_CHECKSIGVERIFY in Tapscript context"
  - "OP_CHECKSIG/CHECKSIGVERIFY handled in script_execute_tapscript() before delegation to avoid ECDSA path"
  - "MINIMALIF enforcement in Tapscript checks top-of-stack before delegating OP_IF/OP_NOTIF to standard executor"
  - "SCRIPT_ERR_PUBKEYTYPE used for empty pubkey (not SCRIPT_ERR_WITNESS_PUBKEYTYPE) per BIP-342 semantics"

metrics:
  duration: ~20 min
  completed: 2026-02-21T03:35:00Z
  tasks: 1
  files_modified: 4
---

# Phase 02 Plan 01: BIP-342 Unknown Key Type Fix + Tapscript Test Suite Summary

Fixed OP_CHECKSIGADD and OP_CHECKSIG to implement BIP-342's three-branch key type dispatch, and created a comprehensive Tapscript test suite with 10 test vectors.

## What Was Built

### RED Phase: Tapscript Test Suite

**`test/unit/test_tapscript.c`** — New test file with 10 BIP-342 test vectors:
1. OP_CHECKSIGADD with 32-byte key + empty sig → n unchanged
2. OP_CHECKSIGADD with 33-byte key (unknown type) + non-empty sig → succeeds, n incremented
3. OP_CHECKSIGADD with 33-byte key + empty sig → succeeds, n unchanged
4. OP_CHECKSIGADD with 0-byte key → fails SCRIPT_ERR_PUBKEYTYPE
5. OP_CHECKSIG in Tapscript with 33-byte key + non-empty sig → succeeds (true)
6. OP_CHECKSIG in Tapscript with 0-byte key → fails SCRIPT_ERR_PUBKEYTYPE
7. OP_SUCCESS opcode (0xbb) → immediate success
8. MINIMALIF enforcement → OP_IF with non-{0x00,0x01} fails
9-10. Additional edge cases

Wired into **Makefile** (test_tapscript target) and **test/run_all_tests.sh**.

### GREEN Phase: BIP-342 Key Type Dispatch

**`src/consensus/script.c`** — Three fixes in `script_execute_tapscript()`:

**Fix 1: OP_CHECKSIGADD dispatch** — Replaced the single `if (pubkey_elem.len != 32)` rejection with:
```c
if (pubkey_elem.len == 0) {
    /* Empty pubkey: fail SCRIPT_ERR_PUBKEYTYPE */
} else if (pubkey_elem.len != 32) {
    /* Unknown key type: succeed without crypto (BIP-342 upgrade rule) */
    n++;
    stack_push_num(&ctx->stack, n);
    continue;
}
/* pubkey_elem.len == 32: x-only Schnorr, fall through to verify */
```

**Fix 2: OP_CHECKSIG/CHECKSIGVERIFY Tapscript handler** — Added dedicated handler before delegation to standard executor. Implements same three-branch dispatch: empty key fails, 32-byte does Schnorr BIP-340 verify, other lengths succeed for non-empty sig.

**Fix 3: MINIMALIF enforcement** — Added OP_IF/OP_NOTIF handler that checks top-of-stack is exactly empty or {0x01} before delegating to standard execution. Fails with SCRIPT_ERR_TAPSCRIPT_MINIMALIF otherwise.

## Verification Results

```
make test: ALL 1095 TESTS PASSED (39/39 suites)
BIP-342 Tapscript tests: 10/10 passed
```

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Scope expansion] OP_CHECKSIG needed dedicated Tapscript handler**
- **Found during:** GREEN phase
- **Issue:** The legacy OP_CHECKSIG path uses ECDSA, not Schnorr. Simply fixing the key type check wasn't enough — Tapscript CHECKSIG needs a complete handler with BIP-340 verification.
- **Fix:** Added full OP_CHECKSIG/CHECKSIGVERIFY handler in script_execute_tapscript() with Schnorr sig validation and BIP-342 key type dispatch.
- **Files modified:** `src/consensus/script.c`
- **Commit:** 644f88f

## Self-Check: PASSED

All key files exist and commits (1c6c1cb RED, 644f88f GREEN) are present in git history.
