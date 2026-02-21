# Deferred Items — Phase 01: Foundation Fixes

Items discovered during execution that are out of scope for the current task.

---

## From Plan 03 (Chainwork BE Storage)

**test_chase linker failure**
- **Symptom:** `make test` fails at `test/unit/test_chase` with undefined symbols: `_block_index_db_lookup_by_height`, `_log_warn`
- **Root cause:** The Makefile's `test_chase` link line does not include `block_index_db.o` or `log.o`, but `chaser_validate.c` (linked into the test) calls both. This is a Makefile omission, not a code bug.
- **Pre-existing:** Confirmed by running `make test` on the stashed (pre-change) tree — same failure occurs.
- **Impact:** `test_block_index_db` tests all pass (16/16). Only `test_chase` binary fails to link.
- **Fix needed:** Add `src/storage/block_index_db.c src/storage/db.c lib/sqlite/sqlite3.c src/app/log.c` to the `test_chase` link line in the Makefile.
