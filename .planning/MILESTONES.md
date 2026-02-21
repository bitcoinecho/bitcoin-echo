# Milestones

## v1.0 Bitcoin Echo Peer-Compatible Node (Shipped: 2026-02-21)

**Phases completed:** 2 phases, 10 plans
**Requirements satisfied:** 17/17
**Tests passing:** 1098/1098
**Files modified:** 14 | Lines: +719 / -146
**Timeline:** 78 days (2025-12-05 → 2026-02-20)
**Git range:** `ac25f35` → `1118891`

**Key accomplishments:**
- Fixed 5 IBD bugs: batch remaining count, duplicate address connections, chainwork big-endian storage, flush-before-index ordering, eviction threshold calibration
- Implemented BIP-342 Tapscript three-branch key type dispatch for OP_CHECKSIGADD and OP_CHECKSIG with unknown-key-type upgrade rule
- Wired full UTXO rollback via prev_chainwork in block_delta_t with reverse-height-order revert loop in chaser_confirm_reorganize
- Added 4 test suites: concurrent block storage (7 tests), peer eviction (5 tests), BIP-342 Tapscript vectors (10 tests), chain reorganization (3 scenarios)
- Discovered and fixed multi-block connect hash mismatch bug in chain_reorganize during reorg test development

**Tech debt carried forward:**
- hash_scriptpubkeys/hash_amounts placeholders in script.c (pre-existing, blocks real multi-input Taproot validation)
- test_chase Makefile link defect (stubs work around it)
- Phase 1 SUMMARY files use provides/affects instead of requirements-completed frontmatter

**Archive:** `.planning/milestones/v1.0-ROADMAP.md`, `v1.0-REQUIREMENTS.md`, `v1.0-MILESTONE-AUDIT.md`

---

