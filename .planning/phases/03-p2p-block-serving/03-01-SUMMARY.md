---
phase: 03-p2p-block-serving
plan: 01
subsystem: p2p
tags: [bitcoin, p2p, segwit, bip-144, inventory, services]

# Dependency graph
requires:
  - phase: 02-consensus-completeness
    provides: SegWit consensus validation (script.c block_validate.c)
provides:
  - NODE_WITNESS advertised in version message at all three call sites
  - INV_WITNESS_BLOCK used for witness-capable peers in block announcements
  - INV_WITNESS_BLOCK used for witness-capable peers in IBD block requests
  - peer->services checked before choosing inventory type
affects:
  - 03-02 (block serving — needs correct INV types established here)
  - 03-03 (headers-first sync — relies on witness-aware peer capability)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Check peer->services & SERVICE_NODE_WITNESS before choosing inventory type"
    - "Always advertise SERVICE_NODE_WITNESS; add SERVICE_NODE_NETWORK only for non-pruned"

key-files:
  created: []
  modified:
    - src/app/node.c

key-decisions:
  - "Pruned nodes advertise SERVICE_NODE_WITNESS — NODE_WITNESS signals protocol capability (SegWit wire format), not block history availability. Bitcoin Core does the same."
  - "peer->services check before inventory type — BIP-144 requires sending INV_WITNESS_BLOCK only to peers that advertised NODE_WITNESS during handshake"

patterns-established:
  - "services bitmask pattern: start with SERVICE_NODE_WITNESS, OR in SERVICE_NODE_NETWORK if not pruned"
  - "peer_has_witness bool: (peer->services & SERVICE_NODE_WITNESS) != 0 before INV type selection"

requirements-completed: [P2P-01, P2P-04]

# Metrics
duration: 1min
completed: 2026-02-21
---

# Phase 3 Plan 01: NODE_WITNESS Services Flag and Inventory Types Summary

**BIP-144 compliance: Echo now advertises SERVICE_NODE_WITNESS and uses INV_WITNESS_BLOCK for witness-capable peers in both block announcements and IBD requests**

## Performance

- **Duration:** 1 min
- **Started:** 2026-02-21T16:34:05Z
- **Completed:** 2026-02-21T16:35:05Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments

- All three `peer_send_version` call sites updated to always include `SERVICE_NODE_WITNESS`; non-pruned nodes additionally include `SERVICE_NODE_NETWORK`
- `node_announce_block_to_peers` now checks `peer->services` and sends `INV_WITNESS_BLOCK` to witness-capable peers, `INV_BLOCK` to legacy peers
- `sync_cb_send_getdata_blocks` now uses `INV_WITNESS_BLOCK` when the sync peer advertised `SERVICE_NODE_WITNESS`; resolved TODO comment removed

## Task Commits

Each task was committed atomically:

1. **Task 1: Add SERVICE_NODE_WITNESS to version message services bitmask** - `05738e3` (feat)
2. **Task 2: Use INV_WITNESS_BLOCK for witness-capable peers in announcements and IBD requests** - `8e49894` (feat)

**Plan metadata:** (see final docs commit)

## Files Created/Modified

- `src/app/node.c` - Three peer_send_version call sites, node_announce_block_to_peers, sync_cb_send_getdata_blocks updated

## Decisions Made

- Pruned nodes advertise `SERVICE_NODE_WITNESS`: NODE_WITNESS signals "I understand the SegWit wire protocol", not "I have all historical blocks". A pruned node can and should advertise it. Bitcoin Core does the same. Only `SERVICE_NODE_NETWORK` is gated on non-pruned status.
- `peer->services` check before choosing inventory type: Per BIP-144, `INV_WITNESS_BLOCK` should only be sent to peers that advertised `SERVICE_NODE_WITNESS`. The `peer->services` field is populated during handshake.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None. Pre-existing warnings (`node_flush_utxo_shutdown` unused function, `get_batch_size_for_height` unused function) are unrelated to this task and out of scope.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Plan 03-02 (block serving) can now proceed: the correct inventory types are established
- All three `peer_send_version` sites consistently advertise `SERVICE_NODE_WITNESS`
- Block announcements and IBD requests are protocol-correct per BIP-144
- No blockers introduced

## Self-Check: PASSED

- FOUND: .planning/phases/03-p2p-block-serving/03-01-SUMMARY.md
- FOUND: 05738e3 (feat(03-01): advertise SERVICE_NODE_WITNESS in version message)
- FOUND: 8e49894 (feat(03-01): use INV_WITNESS_BLOCK for witness-capable peers)

---
*Phase: 03-p2p-block-serving*
*Completed: 2026-02-21*
