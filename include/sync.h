/**
 * Bitcoin Echo — Headers-First Initial Block Download
 *
 * This module implements initial block download (IBD) using headers-first
 * synchronization as specified in the whitepaper §7.3:
 *
 * 1. Request headers from peers using getheaders
 * 2. Validate headers (proof-of-work, difficulty, linkage) without full blocks
 * 3. Identify the best chain by accumulated work
 * 4. Request full blocks for the best chain in parallel from multiple peers
 * 5. Validate and apply blocks to build the UTXO set
 *
 * This approach minimizes wasted bandwidth on orphan chains and allows
 * efficient parallel block downloads.
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_SYNC_H
#define ECHO_SYNC_H

#include "block.h"
#include "chainstate.h"
#include "echo_types.h"
#include "peer.h"
#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * Constants
 * ============================================================================
 */

/* Maximum headers to request in a single getheaders message */
#define SYNC_MAX_HEADERS_PER_REQUEST 2000

/* Maximum block locator hashes to include in getheaders */
#define SYNC_MAX_LOCATOR_HASHES 32

/* Maximum parallel block downloads per peer.
 * Bitcoin Core limits serving to 16 blocks at a time (MAX_BLOCKS_IN_TRANSIT_PER_PEER).
 * Requesting more than 16 just queues on the peer side, inflating our in-flight
 * count without actually downloading. Match Core's limit for accurate tracking.
 */
#define SYNC_MAX_BLOCKS_PER_PEER 16

/* Maximum total parallel block downloads.
 * 512 = 16 blocks × 32 peers for full parallelism.
 */
#define SYNC_MAX_PARALLEL_BLOCKS 512

/* Timeout for getheaders response (30 seconds) */
#define SYNC_HEADERS_TIMEOUT_MS 30000

/* Initial block stalling timeout (2 seconds) - matches Bitcoin Core */
#define SYNC_BLOCK_STALLING_TIMEOUT_MS 2000

/* Maximum block stalling timeout (64 seconds) - matches Bitcoin Core */
#define SYNC_BLOCK_STALLING_TIMEOUT_MAX_MS 64000

/* Timeout decay factor when blocks connect successfully (0.85) */
#define SYNC_STALLING_TIMEOUT_DECAY 0.85

/* Critical zone: request first N blocks from multiple peers to reduce
 * head-of-line blocking. First response wins, duplicates discarded.
 *
 * REDUNDANCY=32 means request from ALL connected peers. This maximizes
 * the chance of finding a peer with the block data, since many peers
 * are pruned or don't serve historical blocks.
 */
#define SYNC_CRITICAL_ZONE_SIZE 8
#define SYNC_CRITICAL_ZONE_REDUNDANCY 32

/* Minimum time between header sync attempts with same peer (5 seconds) */
#define SYNC_HEADER_RETRY_INTERVAL_MS 5000

/* Periodic header refresh during block sync to catch new blocks (30 seconds) */
#define SYNC_HEADER_REFRESH_INTERVAL_MS 30000

/* Block download window - how far ahead of validated tip to download.
 * Larger window = more parallelism but more memory/storage usage.
 *
 * For ARCHIVAL nodes: Use large window (16384) for maximum parallelism.
 * For PRUNED nodes: Use small window (1024) to bound storage overage.
 *   At ~1.5 MB/block post-SegWit, 1024 blocks = ~1.5 GB buffer.
 *
 * Bitcoin Core: 1024, Libbitcoin: 50000.
 */
#define SYNC_BLOCK_DOWNLOAD_WINDOW_ARCHIVAL 16384
#define SYNC_BLOCK_DOWNLOAD_WINDOW_PRUNED 1024

/* Default for backward compatibility */
#define SYNC_BLOCK_DOWNLOAD_WINDOW SYNC_BLOCK_DOWNLOAD_WINDOW_ARCHIVAL

/* Stale tip threshold - consider sync stalled if no progress in this time */
#define SYNC_STALE_TIP_THRESHOLD_MS (30ULL * 60 * 1000) /* 30 minutes */

/* ============================================================================
 * Sync State
 * ============================================================================
 */

/**
 * Sync mode
 */
typedef enum {
  SYNC_MODE_IDLE,         /* Not syncing */
  SYNC_MODE_PING_CONTEST, /* Measuring peer latency before headers sync */
  SYNC_MODE_HEADERS,      /* Downloading headers */
  SYNC_MODE_BLOCKS,       /* Downloading and validating blocks */
  SYNC_MODE_DONE,         /* Sync complete, in steady state */
  SYNC_MODE_STALLED       /* Sync stalled (no progress) */
} sync_mode_t;

/**
 * Peer sync state
 *
 * Tracks the sync state for each connected peer.
 */
typedef struct {
  peer_t *peer;        /* The peer */
  bool sync_candidate; /* Whether peer is suitable for syncing */

  /* Headers sync state */
  bool headers_in_flight;     /* getheaders request pending */
  uint64_t headers_sent_time; /* When getheaders was sent */
  hash256_t last_header_hash; /* Last header hash we sent getheaders for */
  uint32_t headers_received;  /* Number of headers received from this peer */

  /* Block download state.
   * Arrays sized for normal limit + 4 extra slots for critical blocks.
   * The sync manager may exceed SYNC_MAX_BLOCKS_PER_PEER for the next
   * needed block (validated_tip + 1) to prevent validation stalls.
   */
  hash256_t blocks_in_flight[SYNC_MAX_BLOCKS_PER_PEER + 4];  /* Pending blocks */
  uint64_t block_request_time[SYNC_MAX_BLOCKS_PER_PEER + 4]; /* When requested */
  size_t blocks_in_flight_count;
  uint32_t blocks_received; /* Number of blocks received from this peer */

  /* Peer chain info */
  int32_t start_height;     /* Height reported in version message */
  work256_t announced_work; /* Best work announced by peer (from headers) */

  /* Performance metrics */
  uint64_t avg_headers_latency_ms; /* Average headers response time */
  uint64_t avg_block_latency_ms;   /* Average block download time */
  uint32_t timeout_count;          /* Number of timeouts from this peer */
  uint32_t blocks_requested;       /* Blocks requested from this peer */

  /* Latency tracking for metrics */
  uint64_t first_block_time;  /* When peer started delivering blocks (ms) */
  uint64_t total_latency_ms;  /* Sum of all block download latencies */
  uint32_t latency_samples;   /* Number of latency samples collected */

  /* Session-based reputation (for racing prioritization) */
  uint64_t last_delivery_time; /* When peer last delivered a block (ms) */
} peer_sync_state_t;

/**
 * Block download entry
 *
 * Tracks a pending block download.
 */
typedef struct {
  hash256_t hash;        /* Block hash */
  uint32_t height;       /* Expected block height */
  peer_t *assigned_peer; /* Peer we requested from */
  uint64_t request_time; /* When we requested */
  bool in_flight;        /* Whether request is pending */
  uint32_t retry_count;  /* Number of retries */
} block_download_t;

/**
 * Sync manager
 *
 * Coordinates the initial block download process.
 */
typedef struct sync_manager sync_manager_t;

/**
 * Sync callbacks
 *
 * The sync manager calls these functions to interact with the rest of
 * the system (storage, validation, etc.).
 */
typedef struct {
  /**
   * Get block data by hash from storage.
   *
   * Returns:
   *   ECHO_OK if block found
   *   ECHO_ERR_NOT_FOUND if block not in storage
   */
  echo_result_t (*get_block)(const hash256_t *hash, block_t *block_out,
                             void *ctx);

  /**
   * Store block data.
   *
   * Returns:
   *   ECHO_OK on success
   *   ECHO_ERR_EXISTS if already stored
   */
  echo_result_t (*store_block)(const block_t *block, void *ctx);

  /**
   * Validate a block header (contextual validation).
   *
   * This should check PoW, timestamp, prev_block reference, difficulty.
   * The pre-computed hash is provided to avoid redundant SHA256d computation.
   *
   * Returns:
   *   ECHO_OK if valid
   *   ECHO_ERR_INVALID if invalid
   */
  echo_result_t (*validate_header)(const block_header_t *header,
                                   const hash256_t *hash,
                                   const block_index_t *prev_index, void *ctx);

  /**
   * Store/persist a validated header to disk.
   *
   * Called immediately after a header is validated and added to the chain.
   * This allows headers to be persisted during sync, not just when full
   * blocks are validated.
   *
   * Parameters:
   *   header - The block header
   *   index  - The block index entry (contains hash, height, chainwork)
   *   ctx    - User context
   *
   * Returns:
   *   ECHO_OK on success
   *   ECHO_ERR_EXISTS if already stored (not an error)
   */
  echo_result_t (*store_header)(const block_header_t *header,
                                const block_index_t *index, void *ctx);

  /**
   * Validate and apply a full block.
   *
   * Returns:
   *   ECHO_OK if valid and applied
   *   ECHO_ERR_INVALID if block invalid
   */
  echo_result_t (*validate_and_apply_block)(const block_t *block,
                                            const block_index_t *index,
                                            void *ctx);

  /**
   * Send getheaders message to peer.
   *
   * Called by sync manager to request headers from a peer.
   * The locator is built by the sync manager.
   *
   * Parameters:
   *   peer         - Peer to send to
   *   locator      - Block locator hashes (most recent first)
   *   locator_len  - Number of locator hashes
   *   stop_hash    - Hash to stop at (NULL for as many as possible)
   *   ctx          - User context
   */
  void (*send_getheaders)(peer_t *peer, const hash256_t *locator,
                          size_t locator_len, const hash256_t *stop_hash,
                          void *ctx);

  /**
   * Send getdata message for blocks to peer.
   *
   * Called by sync manager to request block downloads.
   *
   * Parameters:
   *   peer   - Peer to send to
   *   hashes - Array of block hashes to request
   *   count  - Number of hashes
   *   ctx    - User context
   */
  void (*send_getdata_blocks)(peer_t *peer, const hash256_t *hashes,
                              size_t count, void *ctx);

  /**
   * Send ping to a peer for RTT measurement.
   *
   * Called during ping contest to measure peer latency.
   * The node should track the nonce and sent time in the peer.
   *
   * Parameters:
   *   peer - Peer to send ping to
   *   ctx  - User context
   */
  void (*send_ping)(peer_t *peer, void *ctx);

  /**
   * Get block hash by height from the database.
   *
   * Used for efficient block queueing - avoids walking back through
   * prev pointers when there's a large height gap.
   *
   * Parameters:
   *   height - Block height
   *   hash   - Output: block hash at that height
   *   ctx    - User context
   *
   * Returns:
   *   ECHO_OK on success
   *   ECHO_ERR_NOT_FOUND if no block at that height
   */
  echo_result_t (*get_block_hash_at_height)(uint32_t height, hash256_t *hash,
                                            void *ctx);

  /**
   * Begin a header batch transaction.
   *
   * Called before processing a batch of headers to enable
   * database transaction batching for performance.
   *
   * Parameters:
   *   ctx - User context
   */
  void (*begin_header_batch)(void *ctx);

  /**
   * Commit a header batch transaction.
   *
   * Called after processing a batch of headers to commit
   * all inserts in a single transaction.
   *
   * Parameters:
   *   ctx - User context
   */
  void (*commit_header_batch)(void *ctx);

  /**
   * Flush all in-memory headers to database.
   *
   * Called when transitioning from HEADERS to BLOCKS mode.
   * During header sync, headers are kept in memory only for speed.
   * This callback persists them all at once in a single transaction.
   *
   * Parameters:
   *   ctx - User context
   *
   * Returns:
   *   ECHO_OK on success
   */
  echo_result_t (*flush_headers)(void *ctx);

  /**
   * Cull peers after audition phase - keep only the fastest performers.
   *
   * Called after the ping contest completes. The implementation should:
   * 1. Sort connected peers by RTT (last_rtt_ms in peer_t)
   * 2. Disconnect the slowest peers
   * 3. Keep only target_count fastest peers
   *
   * Parameters:
   *   target_count - Number of peers to keep
   *   ctx          - User context
   *
   * Returns:
   *   Number of peers actually culled
   */
  size_t (*cull_slow_peers)(size_t target_count, void *ctx);

  /**
   * Evict a peer during continuous rotation.
   *
   * Called during IBD when a peer is identified as a poor performer.
   * The implementation should disconnect the peer and optionally
   * trigger connection to a new peer from the address pool.
   *
   * Parameters:
   *   peer   - Peer to evict
   *   reason - Human-readable reason for eviction (for logging)
   *   ctx    - User context
   */
  void (*evict_peer)(peer_t *peer, const char *reason, void *ctx);

  /**
   * Request new peer connections after rotation eviction.
   *
   * Called after evicting underperformers to immediately fill the slots
   * with fresh peers from the address pool. This ensures we maintain
   * peer count and continuously test new candidates.
   *
   * Parameters:
   *   count - Number of new peers to connect to
   *   ctx   - User context
   */
  void (*request_new_peers)(size_t count, void *ctx);

  /* Context pointer passed to all callbacks */
  void *ctx;
} sync_callbacks_t;

/**
 * Sync progress information
 */
typedef struct {
  sync_mode_t mode; /* Current sync mode */

  /* Header sync progress */
  uint32_t headers_total;     /* Total headers known */
  uint32_t headers_validated; /* Headers with PoW validated */

  /* Block sync progress */
  uint32_t blocks_downloaded; /* Full blocks downloaded */
  uint32_t blocks_validated;  /* Blocks fully validated and applied */
  uint32_t blocks_pending;    /* Blocks awaiting download */

  /* Network state */
  size_t sync_peers;       /* Number of peers syncing with */
  size_t blocks_in_flight; /* Currently downloading */

  /* Chain info */
  uint32_t tip_height;         /* Current validated tip height */
  uint32_t best_header_height; /* Best header chain height */
  work256_t tip_work;          /* Work at current tip */
  work256_t best_header_work;  /* Work at best header */

  /* Performance */
  uint64_t start_time;         /* When sync started */
  uint64_t last_progress_time; /* Last time we made progress */
  float sync_percentage;       /* Percentage complete (0.0-100.0) */
} sync_progress_t;

/* ============================================================================
 * Sync Manager API
 * ============================================================================
 */

/**
 * Create a sync manager.
 *
 * Parameters:
 *   chainstate      - The chain state to sync
 *   callbacks       - Callback functions for storage/validation
 *   download_window - How far ahead of validated tip to download blocks.
 *                     Use SYNC_BLOCK_DOWNLOAD_WINDOW_PRUNED for pruned nodes,
 *                     SYNC_BLOCK_DOWNLOAD_WINDOW_ARCHIVAL for archival nodes,
 *                     or 0 for default (archival).
 *
 * Returns:
 *   Newly allocated sync manager, or NULL on failure
 */
sync_manager_t *sync_create(chainstate_t *chainstate,
                            const sync_callbacks_t *callbacks,
                            uint32_t download_window);

/**
 * Destroy sync manager and free resources.
 */
void sync_destroy(sync_manager_t *mgr);

/**
 * Add a peer to sync tracking.
 *
 * Should be called when a peer completes handshake.
 *
 * Parameters:
 *   mgr   - Sync manager
 *   peer  - Peer to add
 *   height - Height from peer's version message
 */
void sync_add_peer(sync_manager_t *mgr, peer_t *peer, int32_t height);

/**
 * Remove a peer from sync tracking.
 *
 * Should be called when a peer disconnects.
 */
void sync_remove_peer(sync_manager_t *mgr, peer_t *peer);

/**
 * Start initial block download.
 *
 * Begins the sync process by requesting headers from peers.
 *
 * Returns:
 *   ECHO_OK if sync started
 *   ECHO_ERR_INVALID_STATE if already syncing or no peers
 */
echo_result_t sync_start(sync_manager_t *mgr);

/**
 * Stop syncing.
 *
 * Cancels any in-flight requests and returns to idle state.
 */
void sync_stop(sync_manager_t *mgr);

/**
 * Handle received headers message.
 *
 * Processes headers from a peer:
 * - Validates headers (PoW, linkage)
 * - Adds to header chain
 * - Requests more headers if needed
 * - Transitions to block download when headers complete
 *
 * Parameters:
 *   mgr     - Sync manager
 *   peer    - Peer that sent the message
 *   headers - Array of block headers
 *   count   - Number of headers
 *
 * Returns:
 *   ECHO_OK if processed successfully
 *   ECHO_ERR_INVALID if headers invalid (peer may be penalized)
 */
echo_result_t sync_handle_headers(sync_manager_t *mgr, peer_t *peer,
                                  const block_header_t *headers, size_t count);

/**
 * Handle received block message.
 *
 * Processes a block received during sync:
 * - Validates block connects to chain
 * - Validates and applies to chain state
 * - Updates download queue
 *
 * Parameters:
 *   mgr   - Sync manager
 *   peer  - Peer that sent the block
 *   block - The received block
 *
 * Returns:
 *   ECHO_OK if processed successfully
 *   ECHO_ERR_INVALID if block invalid
 *   ECHO_ERR_NOT_FOUND if block wasn't requested
 */
echo_result_t sync_handle_block(sync_manager_t *mgr, peer_t *peer,
                                const block_t *block);

/**
 * Process sync timeouts and request retries.
 *
 * Should be called periodically (e.g., every second).
 * Handles:
 * - Retry timed-out header requests
 * - Retry timed-out block downloads
 * - Reassign blocks from slow peers
 * - Detect stalled sync
 */
void sync_process_timeouts(sync_manager_t *mgr);

/**
 * Tick the sync manager.
 *
 * Main sync loop - should be called frequently.
 * Handles:
 * - Sending getheaders requests
 * - Queuing block downloads
 * - Processing timeouts
 *
 * Returns messages that need to be sent (caller owns the memory).
 */
void sync_tick(sync_manager_t *mgr);

/**
 * Report a pong received from a peer.
 *
 * Called by the node when a pong is received during ping contest.
 * The peer's last_rtt_ms should already be updated before calling this.
 *
 * Parameters:
 *   mgr  - Sync manager
 *   peer - Peer that sent the pong
 */
void sync_report_pong(sync_manager_t *mgr, peer_t *peer);

/**
 * Get current sync progress.
 */
void sync_get_progress(const sync_manager_t *mgr, sync_progress_t *progress);

/**
 * Check if sync is complete.
 *
 * Returns:
 *   true if fully synced to best known chain
 */
bool sync_is_complete(const sync_manager_t *mgr);

/**
 * Check if we're in initial block download.
 *
 * Returns:
 *   true if actively downloading headers or blocks
 */
bool sync_is_ibd(const sync_manager_t *mgr);

/**
 * Skip ping contest for testing purposes.
 *
 * When set, sync_start will go directly to HEADERS mode without
 * waiting for peers or running a ping contest.
 */
void sync_skip_ping_contest(sync_manager_t *mgr);

/**
 * Check if the audition phase is complete.
 *
 * During the audition phase, the node connects to many peers
 * (ECHO_AUDITION_PEER_COUNT) and measures their RTT via ping contest.
 * After culling, only the fastest ECHO_MAX_OUTBOUND_PEERS are kept.
 *
 * Use this to determine the current peer limit:
 * - During audition: allow up to ECHO_AUDITION_PEER_COUNT
 * - After audition: maintain only ECHO_MAX_OUTBOUND_PEERS
 *
 * Returns:
 *   true if audition is complete (culling has happened)
 */
bool sync_is_audition_complete(const sync_manager_t *mgr);

/* ============================================================================
 * Block Locator
 * ============================================================================
 */

/**
 * Build a block locator for getheaders/getblocks.
 *
 * A block locator is a list of block hashes used to find the common point
 * between our chain and a peer's chain. It contains:
 * - Recent blocks (last 10)
 * - Exponentially spaced blocks (at heights tip-12, tip-14, tip-18, ...)
 * - Genesis block
 *
 * Parameters:
 *   state       - Chain state to build locator from
 *   locator     - Output array (must hold SYNC_MAX_LOCATOR_HASHES entries)
 *   locator_len - Output: number of hashes written
 *
 * Returns:
 *   ECHO_OK on success
 */
echo_result_t sync_build_locator(const chainstate_t *state, hash256_t *locator,
                                 size_t *locator_len);

/**
 * Build a block locator starting from a specific block.
 *
 * Parameters:
 *   index_map   - Block index map to traverse
 *   start       - Block to start from
 *   locator     - Output array
 *   locator_len - Output: number of hashes written
 *
 * Returns:
 *   ECHO_OK on success
 */
echo_result_t sync_build_locator_from(const block_index_map_t *index_map,
                                      const block_index_t *start,
                                      hash256_t *locator, size_t *locator_len);

/**
 * Find the common block from a locator.
 *
 * Searches our chain for the first block matching a hash in the locator.
 *
 * Parameters:
 *   state   - Our chain state
 *   locator - Locator from peer
 *   count   - Number of hashes in locator
 *
 * Returns:
 *   Block index of common block, or NULL if no common block found
 */
block_index_t *sync_find_locator_fork(const chainstate_t *state,
                                      const hash256_t *locator, size_t count);

/* ============================================================================
 * Block Download Queue
 * ============================================================================
 */

/**
 * Block download queue
 *
 * Manages the queue of blocks to download during sync.
 */
typedef struct block_queue block_queue_t;

/**
 * Create a block download queue.
 *
 * Parameters:
 *   capacity - Maximum blocks in queue
 *
 * Returns:
 *   Newly allocated queue, or NULL on failure
 */
block_queue_t *block_queue_create(size_t capacity);

/**
 * Destroy block queue.
 */
void block_queue_destroy(block_queue_t *queue);

/**
 * Add block to download queue.
 *
 * Parameters:
 *   queue  - The queue
 *   hash   - Block hash to download
 *   height - Expected block height (for ordering)
 *
 * Returns:
 *   ECHO_OK if added
 *   ECHO_ERR_FULL if queue is full
 *   ECHO_ERR_EXISTS if already in queue
 */
echo_result_t block_queue_add(block_queue_t *queue, const hash256_t *hash,
                              uint32_t height);

/**
 * Get next block to download.
 *
 * Returns the lowest-height unassigned block.
 *
 * Parameters:
 *   queue  - The queue
 *   hash   - Output: block hash
 *   height - Output: block height
 *
 * Returns:
 *   ECHO_OK if block available
 *   ECHO_ERR_NOT_FOUND if no blocks available
 */
echo_result_t block_queue_next(block_queue_t *queue, hash256_t *hash,
                               uint32_t *height);

/**
 * Find block in queue by height.
 *
 * Searches for a block at the specified height (pending or in-flight).
 * Used to check if we already have a stored block that can be validated.
 *
 * Parameters:
 *   queue  - The queue
 *   height - Block height to find
 *   hash   - Output: hash of block at this height
 *
 * Returns:
 *   ECHO_OK if found, ECHO_ERR_NOT_FOUND otherwise
 */
echo_result_t block_queue_find_by_height(block_queue_t *queue, uint32_t height,
                                         hash256_t *hash);

/**
 * Mark block as assigned to peer for download.
 *
 * Parameters:
 *   queue - The queue
 *   hash  - Block hash
 *   peer  - Peer assigned to download
 */
void block_queue_assign(block_queue_t *queue, const hash256_t *hash,
                        peer_t *peer);

/**
 * Mark block as downloaded and remove from queue.
 *
 * Parameters:
 *   queue - The queue
 *   hash  - Block hash
 */
void block_queue_complete(block_queue_t *queue, const hash256_t *hash);

/**
 * Unassign block (return to pending state).
 *
 * Used when peer disconnects or times out.
 *
 * Parameters:
 *   queue - The queue
 *   hash  - Block hash
 */
void block_queue_unassign(block_queue_t *queue, const hash256_t *hash);

/**
 * Unassign all blocks from a peer.
 */
void block_queue_unassign_peer(block_queue_t *queue, peer_t *peer);

/**
 * Get number of pending blocks (not yet assigned).
 */
size_t block_queue_pending_count(const block_queue_t *queue);

/**
 * Get number of in-flight blocks (assigned, awaiting download).
 */
size_t block_queue_inflight_count(const block_queue_t *queue);

/**
 * Get total number of blocks in queue.
 */
size_t block_queue_size(const block_queue_t *queue);

/**
 * Check if a block is in the queue.
 */
bool block_queue_contains(const block_queue_t *queue, const hash256_t *hash);

/* ============================================================================
 * Helper Functions
 * ============================================================================
 */

/**
 * Get human-readable string for sync mode.
 */
const char *sync_mode_string(sync_mode_t mode);

/**
 * Estimate remaining sync time.
 *
 * Parameters:
 *   progress - Current sync progress
 *
 * Returns:
 *   Estimated remaining time in milliseconds, or UINT64_MAX if unknown
 */
uint64_t sync_estimate_remaining_time(const sync_progress_t *progress);

/**
 * Sync metrics for RPC exposure
 *
 * Contains derived performance metrics calculated from internal sync state.
 * These are the "source of truth" metrics the GUI should display.
 */
typedef struct {
  float blocks_per_second;          /* Current sync rate (blk/s) */
  uint64_t eta_seconds;             /* Estimated time remaining */
  uint64_t network_median_latency;  /* Network baseline latency (ms) */
  uint32_t active_sync_peers;       /* Peers actively contributing blocks */
  const char *mode_string;          /* Human-readable sync mode */

  /* Bottleneck diagnostics: helps identify network vs CPU bound sync */
  uint32_t blocks_ready;            /* Blocks where next was immediately ready */
  uint32_t blocks_starved;          /* Blocks where we had to wait for next */
  uint64_t total_validation_ms;     /* Cumulative time in consensus validation */
  uint64_t total_starvation_ms;     /* Cumulative time waiting for blocks */
} sync_metrics_t;

/**
 * Get derived sync metrics for RPC.
 *
 * This provides calculated metrics (rate, ETA) that the GUI should use
 * instead of calculating them client-side.
 *
 * Parameters:
 *   mgr     - The sync manager
 *   metrics - Output metrics structure
 */
void sync_get_metrics(const sync_manager_t *mgr, sync_metrics_t *metrics);

#endif /* ECHO_SYNC_H */
