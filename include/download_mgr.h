/**
 * Bitcoin Echo — PULL-Based Block Download Manager
 *
 * Cooperative work distribution:
 *
 * - Work is organized as BATCHES, not individual items
 * - Peers PULL work when idle, coordinator doesn't push
 * - Starved peers WAIT for work (cooperative, not punitive)
 * - Only truly stalled peers (0 B/s) are disconnected
 *
 * See IBD-PULL-MODEL-REWRITE.md for architectural details.
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_DOWNLOAD_MGR_H
#define ECHO_DOWNLOAD_MGR_H

#include "echo_config.h"
#include "echo_types.h"
#include "peer.h"
#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * Constants
 * ============================================================================
 */

/* Batch size: 8 blocks per peer. */
#define DOWNLOAD_BATCH_SIZE 8

/* Maximum batch size (for array allocation) */
#define DOWNLOAD_BATCH_SIZE_MAX 8

/* Maximum batches in the queue. */
#define DOWNLOAD_MAX_BATCHES 200

/* Maximum peers to track (matches sync manager's outbound peer limit). */
#define DOWNLOAD_MAX_PEERS ECHO_MAX_OUTBOUND_PEERS

/* Performance measurement window in milliseconds (10 seconds).
 * Bytes received in this window are used to calculate bytes/sec.
 */
#define DOWNLOAD_PERF_WINDOW_MS 10000

/* Minimum peers to keep in the sync pool.
 * We won't disconnect stalled peers if it would drop us below this count.
 * This ensures we always have some peers to work with during recovery.
 *
 * Note: Speed-based peer eviction (DOWNLOAD_ALLOWED_DEVIATION, etc.) was
 * removed on 2025-12-31. See download_mgr.c for rationale. We now only
 * disconnect truly stalled peers (0 B/s), not "slow" peers.
 */
#define DOWNLOAD_MIN_PEERS_TO_KEEP 3

/* ============================================================================
 * Work Batch
 * ============================================================================
 */

/**
 * Work batch representing a group of blocks to download.
 *
 * Cooperative model: Each peer gets a batch. When batch is complete (all blocks
 * received), peer requests another batch. If no batches available, peer waits.
 *
 * The received[] bitmap tracks which specific blocks have been received.
 * This prevents duplicate blocks from decrementing remaining.
 */
typedef struct work_batch {
  hash256_t hashes[DOWNLOAD_BATCH_SIZE_MAX]; /* Block hashes in this batch */
  uint32_t heights[DOWNLOAD_BATCH_SIZE_MAX]; /* Corresponding heights */
  size_t count;                              /* Number of blocks in batch (variable) */
  size_t remaining;                          /* Blocks not yet received */
  uint64_t assigned_time;                    /* When assigned to peer (0 if queued) */
  bool received[DOWNLOAD_BATCH_SIZE_MAX];    /* Bitmap: true if block already received */
} work_batch_t;

/* ============================================================================
 * Peer Performance Tracking
 * ============================================================================
 */

/**
 * Per-peer state for download tracking.
 *
 * Each peer owns ONE batch at a time. When batch completes, peer pulls another.
 * Performance is tracked using statistical deviation - peers significantly
 * slower than the mean are disconnected.
 */
typedef struct {
  peer_t *peer;                 /* The peer (NULL if slot unused) */
  work_batch_t *batch;          /* Current batch assigned to this peer (NULL if idle) */
  uint64_t bytes_this_window;   /* Bytes received in current window */
  uint64_t window_start_time;   /* When current window started (ms) */
  float bytes_per_second;       /* Calculated rate (updated each window) */
  uint64_t last_delivery_time;  /* Time of last block delivery (ms) */
  uint64_t first_work_time;     /* When first assigned work (grace period start) */
  bool has_reported;            /* True if ever had rate > 0 */
} peer_perf_t;

/* ============================================================================
 * Download Manager
 * ============================================================================
 */

/**
 * Download manager state.
 *
 * Coordinates block downloads using PULL model:
 * - Maintains queue of work batches
 * - Peers request work when idle
 * - Starved peers wait (cooperative model)
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
   * Disconnect a peer (stalled - 0 B/s for extended period).
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
 *
 * Returns:
 *   Newly allocated download manager, or NULL on failure
 */
download_mgr_t *download_mgr_create(const download_callbacks_t *callbacks);

/**
 * Destroy download manager and free resources.
 */
void download_mgr_destroy(download_mgr_t *mgr);

/**
 * Add a peer to the download manager.
 *
 * Call when a peer completes handshake and is ready for block downloads.
 * The peer will be idle until it calls peer_request_work().
 */
void download_mgr_add_peer(download_mgr_t *mgr, peer_t *peer);

/**
 * Remove a peer from the download manager.
 *
 * Returns any assigned batch to the queue.
 */
void download_mgr_remove_peer(download_mgr_t *mgr, peer_t *peer);

/**
 * Add blocks to the work queue.
 *
 * Creates batches of DOWNLOAD_BATCH_SIZE blocks and adds them to the queue.
 * Called when new headers are received.
 *
 * Parameters:
 *   mgr     - Download manager
 *   hashes  - Array of block hashes
 *   heights - Array of corresponding heights
 *   count   - Number of blocks to add
 *
 * Returns:
 *   Number of blocks actually added (may be less if queue full)
 */
size_t download_mgr_add_work(download_mgr_t *mgr, const hash256_t *hashes,
                             const uint32_t *heights, size_t count);

/* ============================================================================
 * PULL Model API
 * ============================================================================
 */

/**
 * Peer requests work (PULL model).
 *
 * Called when peer becomes idle (batch complete or just connected).
 * If work is available, assigns a batch to the peer and sends getdata.
 *
 * Parameters:
 *   mgr  - Download manager
 *   peer - Peer requesting work
 *
 * Returns:
 *   true if work was assigned, false if queue is empty (peer waits)
 */
bool download_mgr_peer_request_work(download_mgr_t *mgr, peer_t *peer);

/**
 * Record block receipt from a peer.
 *
 * Updates performance tracking and decrements batch remaining count.
 * When batch becomes empty, caller should call peer_request_work().
 *
 * Parameters:
 *   mgr        - Download manager
 *   peer       - Peer that delivered the block
 *   hash       - Block hash
 *   block_size - Size of block in bytes (for throughput calculation)
 *
 * Returns:
 *   true if block was in peer's batch, false if unexpected
 */
bool download_mgr_block_received(download_mgr_t *mgr, peer_t *peer,
                                 const hash256_t *hash, size_t block_size);

/**
 * Check if peer's batch is complete (all blocks received).
 *
 * Parameters:
 *   mgr  - Download manager
 *   peer - Peer to check
 *
 * Returns:
 *   true if peer has no remaining blocks in their batch (or no batch)
 */
bool download_mgr_peer_is_idle(const download_mgr_t *mgr, const peer_t *peer);

/**
 * Check peer performance using statistical deviation model.
 *
 * Called every DOWNLOAD_PERF_WINDOW_MS (10 seconds):
 * - Updates all peer windows and calculates bytes/second
 * - Calculates mean and standard deviation of active peer rates
 * - Disconnects peers with rate = 0 (stalled)
 * - Disconnects peers > DOWNLOAD_ALLOWED_DEVIATION stddev below mean (slow)
 *
 * Parameters:
 *   mgr - Download manager
 *
 * Returns:
 *   Number of peers dropped due to poor performance
 */
size_t download_mgr_check_performance(download_mgr_t *mgr);

/* ============================================================================
 * Query Functions
 * ============================================================================
 */

/**
 * Get number of batches in queue (not yet assigned).
 */
size_t download_mgr_queue_count(const download_mgr_t *mgr);

/**
 * Get number of batches assigned to peers.
 */
size_t download_mgr_assigned_count(const download_mgr_t *mgr);

/**
 * Get total blocks pending (queued + assigned but not received).
 */
size_t download_mgr_pending_blocks(const download_mgr_t *mgr);

/**
 * Get number of active peers (peers with assigned batches).
 */
size_t download_mgr_active_peer_count(const download_mgr_t *mgr);

/**
 * Get aggregate download rate across all peers (bytes/second).
 */
float download_mgr_aggregate_rate(const download_mgr_t *mgr);

/**
 * Check if a block hash is being tracked (in any batch).
 */
bool download_mgr_has_block(const download_mgr_t *mgr, const hash256_t *hash);

/**
 * Get performance stats for a specific peer.
 */
bool download_mgr_get_peer_stats(const download_mgr_t *mgr, const peer_t *peer,
                                 float *bytes_per_second,
                                 uint32_t *blocks_remaining);

/* ============================================================================
 * Legacy API Compatibility (for gradual migration)
 * ============================================================================
 */

/**
 * Get pending count (blocks in queue, not yet assigned).
 * Maps to queued batches * remaining blocks.
 */
size_t download_mgr_pending_count(const download_mgr_t *mgr);

/**
 * Get inflight count (blocks assigned but not received).
 * Maps to assigned batches * remaining blocks.
 */
size_t download_mgr_inflight_count(const download_mgr_t *mgr);

/**
 * Mark block as complete (validated).
 * NOTE: With batch model, blocks are implicitly complete when received.
 * This is kept for compatibility but is a no-op.
 */
void download_mgr_block_complete(download_mgr_t *mgr, const hash256_t *hash,
                                 uint32_t height);

/* ============================================================================
 * Debug/Metrics
 * ============================================================================
 */

/**
 * Download manager metrics for RPC/logging.
 */
typedef struct {
  size_t pending_count;       /* Blocks waiting in queue (queued batches) */
  size_t inflight_count;      /* Blocks assigned (assigned batches) */
  size_t active_peers;        /* Peers with assigned work */
  size_t total_peers;         /* Total tracked peers */
  float aggregate_rate;       /* Total bytes/second across all peers */
  uint32_t lowest_pending;    /* Lowest height in queue/assigned */
  uint32_t highest_assigned;  /* Highest height in queue/assigned */
  uint32_t stalled_peers;     /* Number of stalled peers (always 0 now) */
} download_metrics_t;

/**
 * Get download manager metrics.
 */
void download_mgr_get_metrics(const download_mgr_t *mgr,
                              download_metrics_t *metrics);

#endif /* ECHO_DOWNLOAD_MGR_H */
