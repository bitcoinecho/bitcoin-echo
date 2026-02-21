---
phase: 03-p2p-block-serving
plan: 02
subsystem: p2p
tags: [bitcoin, p2p, getdata, block-serving, witness, legacy, direct-send, BIP-144]

# Dependency graph
requires:
  - phase: 03-p2p-block-serving
    plan: 01
    provides: SERVICE_NODE_WITNESS advertised + INV_WITNESS_BLOCK inventory types
provides:
  - Peers requesting INV_WITNESS_BLOCK receive full witness-serialized block via direct-send
  - Peers requesting INV_BLOCK receive stripped legacy serialization (no witness) via direct-send
  - Unknown, pruned, and header-only blocks return notfound message
affects:
  - 03-03 (headers-first sync — block serving must be stable before optimizing sync)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Direct-send for large messages: serialize to heap buffer, build 24-byte header, plat_socket_send in a loop — bypasses peer_queue_message 20KB stack limit"
    - "Stripped legacy serialization: block_header_serialize + varint(tx_count) + tx_serialize(ECHO_FALSE) per tx"
    - "Block availability check: block_index_db_lookup_by_hash then BLOCK_STATUS_HAVE_DATA && !BLOCK_STATUS_PRUNED"

key-files:
  created: []
  modified:
    - src/app/node.c

key-decisions:
  - "Direct-send bypasses peer_queue_message — peer_send_message_internal has no MSG_BLOCK case (20KB stack buffer), msg_block_serialize always includes witness (wrong for INV_BLOCK), and block_t ownership transfer to queue is complex. Direct-send avoids all three problems."
  - "Manual stripped serialization for INV_BLOCK — block_serialize always includes witness data. Legacy path manually serializes header + varint + each tx with ECHO_FALSE to produce a correct stripped block."
  - "Unified availability check replaces old pruning stub — single block_index_db_lookup_by_hash + status flag check handles all three notfound cases (unknown, pruned, header-only) instead of the separate pruning-only check."

patterns-established:
  - "Direct-send pattern: malloc heap buffer, serialize, build msg_header_t, plat_socket_send header then payload in loop, free buffer"
  - "Block availability gate: lookup_by_hash + (HAVE_DATA && !PRUNED) before loading"

requirements-completed: [P2P-02]

# Metrics
duration: 3min
completed: 2026-02-21
---

# Phase 3 Plan 02: Block Serving via getdata Handler Summary

**getdata block handler serves witness blocks (INV_WITNESS_BLOCK) and stripped legacy blocks (INV_BLOCK) via direct-send, with notfound for unknown/pruned/header-only blocks**

## Performance

- **Duration:** 3 min
- **Started:** 2026-02-21T16:38:32Z
- **Completed:** 2026-02-21T16:41:31Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments

- Replaced the `TODO: Full block serving` stub with a complete getdata block handler covering both witness and legacy serialization
- INV_WITNESS_BLOCK requests served via `block_serialize` to heap buffer then `plat_socket_send` in a loop (handles blocks up to 4MB)
- INV_BLOCK requests served via manual stripped serialization: `block_header_serialize` + `varint_write(tx_count)` + `tx_serialize(ECHO_FALSE)` per transaction
- Three notfound cases handled: unknown hash, pruned block, header-only block (known but not yet downloaded)
- Old separate pruning stub replaced by unified availability check via `block_index_db_lookup_by_hash` + status flags

## Task Commits

Each task was committed atomically:

1. **Task 1: Implement getdata block handler with direct-send for witness blocks** - `3b49eea` (feat)
2. **Task 2: Implement legacy block stripping for INV_BLOCK direct-send** - `0122819` (feat)

**Plan metadata:** (see final docs commit)

## Files Created/Modified

- `src/app/node.c` - MSG_GETDATA case: replaced pruning stub + TODO with full block serving handler (both witness and legacy paths), added `protocol_serialize.h` and `serialize.h` includes

## Decisions Made

- **Direct-send over peer_queue_message:** `peer_send_message_internal` has no MSG_BLOCK case and uses a 20KB stack buffer (blocks can be 4MB). `msg_block_serialize` always includes witness (wrong for INV_BLOCK). Block_t ownership transfer to the message queue would require solving shallow copy of owned pointers. Direct-send avoids all three issues -- the handler owns the buffer from malloc to free.
- **Manual stripped serialization for INV_BLOCK:** Since `block_serialize` always includes witness data, the legacy path manually serializes: 80-byte header + varint tx_count + each tx via `tx_serialize` with `ECHO_FALSE`. This satisfies the ROADMAP success criterion that INV_BLOCK receives stripped legacy, not a witness block.
- **Unified availability check:** The old code had a separate pruning-specific check (`node_is_pruning_enabled` + `block_index_db_is_pruned`). The new code uses a single `block_index_db_lookup_by_hash` + status flag check (`BLOCK_STATUS_HAVE_DATA` and `BLOCK_STATUS_PRUNED`), which correctly handles all three notfound scenarios in one code path.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None. Pre-existing warning (`node_flush_utxo_shutdown` unused function) is unrelated to this task and out of scope.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Plan 03-03 (headers-first sync improvements) can proceed: block serving is now functional
- Echo can serve both witness and legacy blocks to requesting peers
- All notfound cases handled correctly (unknown, pruned, header-only)
- No blockers introduced

## Self-Check: PASSED

- FOUND: .planning/phases/03-p2p-block-serving/03-02-SUMMARY.md
- FOUND: 3b49eea (feat(03-02): implement getdata witness block serving with direct-send)
- FOUND: 0122819 (feat(03-02): implement legacy block stripping for INV_BLOCK direct-send)

---
*Phase: 03-p2p-block-serving*
*Completed: 2026-02-21*
