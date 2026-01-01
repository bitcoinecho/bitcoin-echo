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

/* Forward declaration for chase event system */
typedef struct chase_dispatcher chase_dispatcher_t;

/* ============================================================================
 * Constants
 * ============================================================================
 */

/* Maximum headers to request in a single getheaders message */
#define SYNC_MAX_HEADERS_PER_REQUEST 2000

/* Maximum block locator hashes to include in getheaders */
#define SYNC_MAX_LOCATOR_HASHES 32

/* Timeout for getheaders response (30 seconds) */
#define SYNC_HEADERS_TIMEOUT_MS 30000

/* Initial block stalling timeout (2 seconds) - matches Bitcoin Core */
#define SYNC_BLOCK_STALLING_TIMEOUT_MS 2000

/* Maximum block stalling timeout (64 seconds) - matches Bitcoin Core */
#define SYNC_BLOCK_STALLING_TIMEOUT_MAX_MS 64000

/* Timeout decay factor when blocks connect successfully (0.85) */
#define SYNC_STALLING_TIMEOUT_DECAY 0.85

/* Minimum time between header sync attempts with same peer (5 seconds) */
#define SYNC_HEADER_RETRY_INTERVAL_MS 5000

/* Periodic header refresh during block sync to catch new blocks (30 seconds) */
#define SYNC_HEADER_REFRESH_INTERVAL_MS 30000

/* Stale tip threshold - consider sync stalled if no progress in this time */
#define SYNC_STALE_TIP_THRESHOLD_MS (30ULL * 60 * 1000) /* 30 minutes */

/* Rolling window size for header peer response time tracking */
#define SYNC_HEADER_RESPONSE_WINDOW 3

/* ============================================================================
 * Sync State
 * ============================================================================
 */

/**
 * Sync mode (simplified for IBD rewrite - no ping contest)
 */
typedef enum {
  SYNC_MODE_IDLE,    /* Not syncing */
  SYNC_MODE_HEADERS, /* Downloading headers */
  SYNC_MODE_BLOCKS,  /* Downloading and validating blocks */
  SYNC_MODE_DONE,    /* Sync complete, in steady state */
  SYNC_MODE_STALLED  /* Sync stalled (no progress) */
} sync_mode_t;

/**
 * Peer sync state
 *
 * Tracks the sync state for each connected peer.
 * Note: Block download tracking is now handled by download_mgr.
 */
typedef struct {
  peer_t *peer;        /* The peer */
  bool sync_candidate; /* Whether peer is suitable for syncing */

  /* Headers sync state */
  bool headers_in_flight;     /* getheaders request pending */
  uint64_t headers_sent_time; /* When getheaders was sent */
  hash256_t last_header_hash; /* Last header hash we sent getheaders for */
  uint32_t headers_received;  /* Number of headers received from this peer */
  struct block_index *peer_best_header; /* Per-peer header tip for parallel sync */

  /* Block download state - tracking only, download_mgr manages assignments */
  uint32_t blocks_received; /* Number of blocks received from this peer */

  /* Peer chain info */
  int32_t start_height;     /* Height reported in version message */
  work256_t announced_work; /* Best work announced by peer (from headers) */

  /* Performance metrics */
  uint32_t timeout_count;     /* Number of timeouts from this peer */
  uint32_t blocks_requested;  /* Blocks requested from this peer */
  uint64_t first_block_time;  /* When peer started delivering blocks (ms) */
  uint64_t last_delivery_time; /* When peer last delivered a block (ms) */

  /* Header race metrics (best-of-N selection) */
  uint32_t headers_race_responses;    /* Number of full header batches during race */
  uint64_t headers_race_total_ms;     /* Total response time during race */

  /* Post-race monitoring: ring buffer for accurate rolling window of response times */
  uint64_t recent_times[SYNC_HEADER_RESPONSE_WINDOW]; /* Ring buffer of response times (ms) */
  uint32_t recent_times_idx;    /* Next write position in ring buffer */
  uint32_t recent_times_count;  /* Number of valid entries (0 to SYNC_HEADER_RESPONSE_WINDOW) */
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

  /* NOTE: validate_and_apply_block callback removed in Phase 3 IBD rewrite.
   * Validation is now event-driven via chase system:
   *   sync.c fires CHASE_CHECKED → chaser_validate → chaser_confirm
   * Direct block processing uses node_validate_and_apply_block() instead.
   */

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
   * Disconnect a misbehaving or stalled peer.
   *
   * Called when the sync/download manager determines a peer should be
   * disconnected (e.g., stalled, slow, or misbehaving).
   *
   * Parameters:
   *   peer   - Peer to disconnect
   *   reason - Human-readable reason for disconnection
   *   ctx    - User context
   */
  void (*disconnect_peer)(peer_t *peer, const char *reason, void *ctx);

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
 *   chainstate - The chain state to sync
 *   callbacks  - Callback functions for storage/validation
 *   dispatcher - Chase event dispatcher for validation pipeline.
 *                Pass NULL to use legacy callback-based validation.
 *
 * Returns:
 *   Newly allocated sync manager, or NULL on failure
 */
sync_manager_t *sync_create(chainstate_t *chainstate,
                            const sync_callbacks_t *callbacks,
                            chase_dispatcher_t *dispatcher);

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
 *
 * KEY INSIGHT: Download and validation rates are DIFFERENT:
 * - download_rate: Blocks received from network (any order)
 * - validation_rate: Blocks added to chain (strict order)
 *
 * When validation_rate << download_rate, we have head-of-line blocking.
 * pending_validation shows the gap (blocks downloaded but waiting).
 */
typedef struct {
  float download_rate;              /* Blocks downloaded per second */
  float validation_rate;            /* Blocks validated per second */
  uint32_t pending_validation;      /* Downloaded but not yet validated */
  uint64_t eta_seconds;             /* Estimated time remaining */
  uint64_t network_median_latency;  /* Network baseline latency (ms) */
  uint32_t active_sync_peers;       /* Peers actively contributing blocks */
  const char *mode_string;          /* Human-readable sync mode */
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
void sync_get_metrics(sync_manager_t *mgr, sync_metrics_t *metrics);

#endif /* ECHO_SYNC_H */
