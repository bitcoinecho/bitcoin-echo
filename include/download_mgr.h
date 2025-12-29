/**
 * Bitcoin Echo — Performance-Based Block Download Manager
 *
 * This module manages block download work distribution based on measured
 * peer performance (bytes/second), not ping RTT. Key principles:
 *
 * - Measure actual throughput, not latency
 * - Assign work to peers with capacity
 * - Detect stalls and reassign work
 * - Split work from slow peers to fast ones
 *
 * Inspired by libbitcoin-node's chaser_check architecture.
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_DOWNLOAD_MGR_H
#define ECHO_DOWNLOAD_MGR_H

#include "echo_types.h"
#include "peer.h"
#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * Constants
 * ============================================================================
 */

/* Maximum blocks in-flight per peer.
 * Bitcoin Core limits serving to 16 blocks at a time.
 */
#define DOWNLOAD_MAX_IN_FLIGHT_PER_PEER 16

/* Performance measurement window in milliseconds (10 seconds).
 * Bytes received in this window are used to calculate bytes/sec.
 */
#define DOWNLOAD_PERF_WINDOW_MS 10000

/* Stall timeout: if peer has work but delivers 0 bytes for this duration,
 * reassign their work to other peers (30 seconds).
 * libbitcoin-style: we take back work but DON'T disconnect - let peer recover.
 */
#define DOWNLOAD_STALL_TIMEOUT_MS 30000

/* Blocking timeout: if a peer is holding the next block needed for sequential
 * validation for this duration, unassign ALL their work (2 seconds).
 *
 * CRITICAL: This must be VERY SHORT! A blocking block stops ALL validation.
 * 2 seconds is enough for early tiny blocks (~200 bytes) and later larger
 * blocks will generally arrive faster due to peer performance tracking.
 *
 * If validation is blocked for 2+ seconds, we aggressively steal ALL work
 * from that peer and give them a cooldown before new assignments.
 */
#define DOWNLOAD_BLOCKING_TIMEOUT_MS 2000

/* Minimum peers required for standard deviation calculation.
 * Below this count, we don't drop "slow" peers.
 */
#define DOWNLOAD_MIN_PEERS_FOR_STDDEV 3

/* Standard deviation threshold for slow peer detection.
 * If peer.speed < mean - (allowed_deviation * stddev), peer is slow.
 */
#define DOWNLOAD_ALLOWED_DEVIATION 1.5f

/* Maximum pending work items (blocks awaiting assignment).
 * Matches SYNC_BLOCK_DOWNLOAD_WINDOW.
 */
#define DOWNLOAD_MAX_PENDING 16384

/* Maximum peers to track. */
#define DOWNLOAD_MAX_PEERS 256

/* ============================================================================
 * Work Item
 * ============================================================================
 */

/**
 * State of a work item (block download request).
 */
typedef enum {
  WORK_STATE_PENDING,   /* Waiting for assignment */
  WORK_STATE_ASSIGNED,  /* Assigned to a peer, awaiting download */
  WORK_STATE_RECEIVED,  /* Block received, awaiting validation */
  WORK_STATE_COMPLETE   /* Fully processed */
} work_state_t;

/**
 * Work item representing a single block to download.
 */
typedef struct {
  hash256_t hash;          /* Block hash */
  uint32_t height;         /* Block height */
  work_state_t state;      /* Current state */
  peer_t *assigned_peer;   /* Peer assigned to download (NULL if pending) */
  uint64_t assigned_time;  /* Time when assigned (ms since epoch) */
  uint32_t retry_count;    /* Number of reassignments */
} work_item_t;

/* ============================================================================
 * Peer Performance Tracking
 * ============================================================================
 */

/**
 * Per-peer performance tracking.
 *
 * Measures actual throughput (bytes/second) over a rolling window.
 */
typedef struct {
  peer_t *peer;               /* The peer (NULL if slot unused) */
  uint64_t bytes_this_window; /* Bytes received in current window */
  uint64_t window_start_time; /* When current window started (ms) */
  float bytes_per_second;     /* Calculated rate (updated each window) */
  uint32_t blocks_in_flight;  /* Number of blocks currently assigned */
  uint64_t last_delivery_time;/* Time of last block delivery (ms) */
  bool stalled;               /* True if peer has stalled */
  uint64_t stall_until;       /* Cooldown: no new work until this time (ms) */
} peer_perf_t;

/* ============================================================================
 * Download Manager
 * ============================================================================
 */

/**
 * Download manager state.
 *
 * Coordinates block downloads across multiple peers based on performance.
 */
typedef struct download_mgr download_mgr_t;

/**
 * Callbacks for download manager operations.
 */
typedef struct {
  /**
   * Send getdata request to peer for blocks.
   *
   * Parameters:
   *   peer   - Peer to send to
   *   hashes - Array of block hashes to request
   *   count  - Number of hashes
   *   ctx    - User context
   */
  void (*send_getdata)(peer_t *peer, const hash256_t *hashes, size_t count,
                       void *ctx);

  /**
   * Notify that a peer should be disconnected (too slow or misbehaving).
   *
   * Parameters:
   *   peer   - Peer to disconnect
   *   reason - Human-readable reason
   *   ctx    - User context
   */
  void (*disconnect_peer)(peer_t *peer, const char *reason, void *ctx);

  /* User context passed to all callbacks */
  void *ctx;
} download_callbacks_t;

/* ============================================================================
 * Download Manager API
 * ============================================================================
 */

/**
 * Create a download manager.
 *
 * Parameters:
 *   callbacks - Callback functions for network operations
 *   window    - Maximum blocks ahead of validated tip to download
 *
 * Returns:
 *   Newly allocated download manager, or NULL on failure
 */
download_mgr_t *download_mgr_create(const download_callbacks_t *callbacks,
                                    uint32_t window);

/**
 * Destroy download manager and free resources.
 */
void download_mgr_destroy(download_mgr_t *mgr);

/**
 * Add a peer to the download manager.
 *
 * Call when a peer completes handshake and is ready for block downloads.
 *
 * Parameters:
 *   mgr  - Download manager
 *   peer - Peer to add
 */
void download_mgr_add_peer(download_mgr_t *mgr, peer_t *peer);

/**
 * Remove a peer from the download manager.
 *
 * Reassigns all in-flight work from this peer.
 *
 * Parameters:
 *   mgr  - Download manager
 *   peer - Peer to remove
 */
void download_mgr_remove_peer(download_mgr_t *mgr, peer_t *peer);

/**
 * Add blocks to the pending work queue.
 *
 * Called when new headers are received and we have blocks to download.
 *
 * Parameters:
 *   mgr     - Download manager
 *   hashes  - Array of block hashes
 *   heights - Array of corresponding heights
 *   count   - Number of blocks to add
 *
 * Returns:
 *   Number of blocks actually added (may be less if queue full or duplicates)
 */
size_t download_mgr_add_work(download_mgr_t *mgr, const hash256_t *hashes,
                             const uint32_t *heights, size_t count);

/**
 * Distribute pending work to peers with capacity.
 *
 * Assigns blocks to peers using round-robin distribution.
 * Should be called periodically (e.g., every tick).
 *
 * Parameters:
 *   mgr - Download manager
 *
 * Returns:
 *   Number of blocks assigned this call
 */
size_t download_mgr_distribute_work(download_mgr_t *mgr);

/**
 * Record block receipt from a peer.
 *
 * Updates performance tracking and marks work as received.
 *
 * Parameters:
 *   mgr        - Download manager
 *   peer       - Peer that delivered the block
 *   hash       - Block hash
 *   block_size - Size of block in bytes (for throughput calculation)
 *
 * Returns:
 *   true if block was expected from this peer, false otherwise
 */
bool download_mgr_block_received(download_mgr_t *mgr, peer_t *peer,
                                 const hash256_t *hash, size_t block_size);

/**
 * Mark a block as fully validated and processed.
 *
 * Removes the work item from tracking.
 *
 * Parameters:
 *   mgr    - Download manager
 *   hash   - Block hash
 *   height - Block height
 */
void download_mgr_block_complete(download_mgr_t *mgr, const hash256_t *hash,
                                 uint32_t height);

/**
 * Check for stalled peers and performance issues.
 *
 * Should be called periodically (e.g., every second). Handles:
 * - Updating per-peer bytes/second calculations
 * - Detecting stalled peers (no delivery for STALL_TIMEOUT)
 * - Detecting slow peers (below mean - deviation * stddev)
 * - Reassigning work from stalled/slow peers
 *
 * Parameters:
 *   mgr - Download manager
 */
void download_mgr_check_performance(download_mgr_t *mgr);

/**
 * Split work from a slow peer.
 *
 * Takes half of the slow peer's assigned blocks and makes them available
 * for reassignment to other peers. Used when a peer is detected as slow
 * but not completely stalled.
 *
 * Parameters:
 *   mgr       - Download manager
 *   slow_peer - Peer to take work from
 *
 * Returns:
 *   Number of blocks unassigned
 */
size_t download_mgr_split_work(download_mgr_t *mgr, peer_t *slow_peer);

/**
 * Handle starved condition by stealing work from the slowest peer.
 *
 * libbitcoin-style work stealing: When no pending work exists but peers are
 * still downloading, find the slowest peer and take half their work. This
 * naturally rebalances work toward faster peers over time.
 *
 * Called when:
 * - pending_count == 0 (no work available to assign)
 * - inflight_count > 0 (some peers still have work)
 * - At least one peer has capacity for more work
 *
 * Parameters:
 *   mgr - Download manager
 *
 * Returns:
 *   Number of blocks stolen and made pending (0 if no action taken)
 */
size_t download_mgr_steal_from_slowest(download_mgr_t *mgr);

/**
 * Steal work from the peer blocking validation.
 *
 * Unlike steal_from_slowest (which steals from lowest throughput), this
 * function identifies which peer holds the block at validated_height+1
 * and unassigns ALL their work if they've been holding it too long.
 *
 * This is critical for IBD performance: validation must be sequential,
 * so a single slow peer holding the next-needed block blocks ALL progress.
 *
 * Parameters:
 *   mgr              - Download manager
 *   validated_height - Current validated block height
 *   max_wait_ms      - Maximum time to wait for blocking block (e.g., 3000ms)
 *
 * Returns:
 *   Number of blocks unassigned (0 if no action taken)
 */
size_t download_mgr_steal_blocking_work(download_mgr_t *mgr,
                                        uint32_t validated_height,
                                        uint64_t max_wait_ms);

/* NOTE: download_mgr_request_blocking_parallel() was REMOVED.
 *
 * Sending duplicate block requests to multiple peers is fundamentally wrong
 * and network-hostile. The libbitcoin model is: each block is only ever
 * assigned to ONE peer at a time.
 *
 * When blocking, use steal_blocking_work() to UNASSIGN the blocking block
 * and redistribute it to a single different peer. No duplicate requests.
 */

/**
 * Check if starved condition exists.
 *
 * Starved means: no pending work, but work is in-flight with some peers,
 * and at least one peer has capacity for more work (isn't at max in-flight).
 *
 * Parameters:
 *   mgr - Download manager
 *
 * Returns:
 *   true if starved condition exists, false otherwise
 */
bool download_mgr_is_starved(const download_mgr_t *mgr);

/* ============================================================================
 * Query Functions
 * ============================================================================
 */

/**
 * Get number of pending blocks (not yet assigned).
 */
size_t download_mgr_pending_count(const download_mgr_t *mgr);

/**
 * Get number of in-flight blocks (assigned, awaiting download).
 */
size_t download_mgr_inflight_count(const download_mgr_t *mgr);

/**
 * Get number of active peers (peers with work assigned).
 */
size_t download_mgr_active_peer_count(const download_mgr_t *mgr);

/**
 * Get aggregate download rate across all peers (bytes/second).
 */
float download_mgr_aggregate_rate(const download_mgr_t *mgr);

/**
 * Check if a block hash is in the work queue (any state).
 */
bool download_mgr_has_block(const download_mgr_t *mgr, const hash256_t *hash);

/**
 * Get performance stats for a specific peer.
 *
 * Parameters:
 *   mgr              - Download manager
 *   peer             - Peer to query
 *   bytes_per_second - Output: bytes/second rate
 *   blocks_in_flight - Output: number of assigned blocks
 *
 * Returns:
 *   true if peer is tracked, false otherwise
 */
bool download_mgr_get_peer_stats(const download_mgr_t *mgr, const peer_t *peer,
                                 float *bytes_per_second,
                                 uint32_t *blocks_in_flight);

/* ============================================================================
 * Debug/Metrics
 * ============================================================================
 */

/**
 * Download manager metrics for RPC/logging.
 */
typedef struct {
  size_t pending_count;       /* Blocks waiting for assignment */
  size_t inflight_count;      /* Blocks being downloaded */
  size_t active_peers;        /* Peers with assigned work */
  size_t total_peers;         /* Total tracked peers */
  float aggregate_rate;       /* Total bytes/second across all peers */
  uint32_t lowest_pending;    /* Lowest height in pending queue */
  uint32_t highest_assigned;  /* Highest height assigned */
  uint32_t stalled_peers;     /* Number of stalled peers */
} download_metrics_t;

/**
 * Get download manager metrics.
 */
void download_mgr_get_metrics(const download_mgr_t *mgr,
                              download_metrics_t *metrics);

#endif /* ECHO_DOWNLOAD_MGR_H */
