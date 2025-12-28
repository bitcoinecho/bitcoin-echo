/**
 * Bitcoin Echo — Headers-First Initial Block Download Implementation
 *
 * This module implements headers-first sync as specified in whitepaper §7.3.
 * Design principles:
 * - Headers-first: download and validate header chain before blocks
 * - Simple peer assessment: track delivery rate, not RTT
 * - No redundant requests: one peer per block at a time
 *
 * Build once. Build right. Stop.
 */

#include "sync.h"
#include "block.h"
#include "chainstate.h"
#include "chase.h"
#include "download_mgr.h"
#include "echo_config.h"
#include "echo_types.h"
#include "log.h"
#include "peer.h"
#include "platform.h"
#include "protocol.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal Structures
 * ============================================================================
 */

/**
 * Maximum sync peers to track
 */
#define SYNC_MAX_PEERS 128

/**
 * Number of rounds in header race before selecting winner.
 * Higher = more accurate selection, but slower to start single-peer mode.
 * 2 rounds filters lucky responses while keeping race duration short.
 */
#define HEADERS_RACE_ROUNDS 2

/**
 * Minimum peers required before declaring a race winner.
 * Matches HEADER_POOL_SIZE since we only need enough to fill the pool.
 */
#define HEADERS_RACE_MIN_PEERS 8

/**
 * Post-race pool management: top 8 peers form a pool, always use best performer.
 */
#define HEADER_POOL_SIZE 8            /* Top 8 peers form the header pool */
#define POOL_PROBE_INTERVAL 1         /* Probe non-active pool members every N responses */

/**
 * Rolling window size for blocks_per_second calculation.
 * Rate = ROLLING_WINDOW_BLOCKS / time_to_validate_those_blocks
 */
#define ROLLING_WINDOW_BLOCKS 500

/**
 * Pending header entry for deferred persistence.
 *
 * During SYNC_MODE_HEADERS, we keep headers in memory and defer database
 * writes until transitioning to SYNC_MODE_BLOCKS. This avoids ~870K
 * individual SQLite INSERTs, replacing them with a single batched write.
 */
typedef struct {
  block_header_t header; /* The full 80-byte header */
  block_index_t *index;  /* Pointer to the block_index in chainstate */
} pending_header_t;

/**
 * Initial capacity for pending headers (grows as needed)
 */
#define PENDING_HEADERS_INITIAL_CAPACITY 4096

/**
 * Sync manager implementation
 */
struct sync_manager {
  /* Chain state */
  chainstate_t *chainstate;

  /* Callbacks */
  sync_callbacks_t callbacks;

  /* Current sync mode */
  sync_mode_t mode;

  /* Peer sync states */
  peer_sync_state_t peers[SYNC_MAX_PEERS];
  size_t peer_count;

  /* Sync timing */
  uint64_t start_time;
  uint64_t block_sync_start_time; /* When block validation started (0 = not yet) */
  uint32_t block_sync_start_height; /* Height when block sync started */
  uint64_t last_progress_time;
  uint64_t last_header_refresh_time; /* For periodic header refresh in blocks mode */

  /* Stats */
  uint32_t headers_received_total;
  uint32_t blocks_received_total;
  uint32_t blocks_validated_total;

  /* Rolling window for speed calculation */
  uint32_t rolling_window_start_height;  /* Height at start of window */
  uint64_t rolling_window_start_time;    /* Time at start of window */
  uint32_t last_validated_height;        /* Last known validated height */

  /* Best known header chain */
  block_index_t *best_header;

  /* Adaptive stalling timeout (in ms) - starts at 2s, grows on stalls */
  uint64_t stalling_timeout_ms;

  /* Download window size (configured at creation based on pruning mode) */
  uint32_t download_window;

  /* Parallel request rate limiting */
  hash256_t last_parallel_request_hash;
  uint64_t last_parallel_request_time;

  /* Last time we checked for stored blocks to validate */
  uint64_t last_stored_block_check_time;

  /* IBD profiling: Rate-limited progress logging (every 30s) */
  uint64_t last_progress_log_time;

  /* Network latency baseline (kept for API compatibility, currently always 0) */
  uint64_t network_median_latency_ms;

  /* Headers race: all peers compete, fastest wins and becomes designated peer.
   * This avoids wasting bandwidth on duplicate headers from slower peers. */
  bool headers_race_complete;       /* True once we've identified the fastest peer */
  size_t fastest_header_peer_idx;   /* Index of winning peer in peers[] array */
  uint64_t headers_race_start_time; /* When the race started */

  /* Post-race pool: top 8 peers, always use best performer */
  size_t header_pool_indices[HEADER_POOL_SIZE]; /* Indices of top 8 peers */
  size_t header_pool_count;                     /* Number of peers in pool */
  size_t active_pool_peer_idx;                  /* Currently active peer index (in peers[]) */
  uint32_t active_peer_responses;               /* Responses since last pool evaluation */

  /* Deferred header persistence: queue headers during SYNC_MODE_HEADERS,
   * flush all at once when transitioning to SYNC_MODE_BLOCKS. */
  pending_header_t *pending_headers;      /* Dynamic array of pending headers */
  size_t pending_headers_count;           /* Number of queued headers */
  size_t pending_headers_capacity;        /* Allocated capacity */

  /* Performance-based download manager for peer throughput tracking */
  download_mgr_t *download_mgr;

  /* Chase event dispatcher for chaser-based validation */
  chase_dispatcher_t *dispatcher;

  /* Subscription for receiving chase events */
  chase_subscription_t *subscription;
};

/* ============================================================================
 * Download Manager Callback Wrappers
 * ============================================================================
 */

/**
 * Callback wrapper: send getdata for blocks to peer.
 * Adapts sync_manager callbacks to download_mgr callback signature.
 */
static void dm_send_getdata(peer_t *peer, const hash256_t *hashes, size_t count,
                            void *ctx) {
  sync_manager_t *mgr = (sync_manager_t *)ctx;
  if (mgr->callbacks.send_getdata_blocks) {
    mgr->callbacks.send_getdata_blocks(peer, hashes, count, mgr->callbacks.ctx);
  }
}

/**
 * Callback wrapper: disconnect a slow/misbehaving peer.
 * Routes the disconnect request to node.c via the sync callbacks.
 */
static void dm_disconnect_peer(peer_t *peer, const char *reason, void *ctx) {
  sync_manager_t *mgr = (sync_manager_t *)ctx;
  log_warn(LOG_COMP_SYNC, "Download manager requests disconnect of %s: %s",
           peer->address, reason);

  /* Actually disconnect the peer via node callback */
  if (mgr->callbacks.disconnect_peer != NULL) {
    mgr->callbacks.disconnect_peer(peer, reason, mgr->callbacks.ctx);
  }
}

/* ============================================================================
 * Chase Event Handler
 * ============================================================================
 */

/**
 * Handle chase events for download coordination.
 *
 * Responds to:
 * - CHASE_STARVED: A chaser needs more work, distribute pending blocks
 * - CHASE_SPLIT: Split work from a slow peer
 */
static bool sync_chase_handler(chase_event_t event, chase_value_t value,
                               void *context) {
  sync_manager_t *mgr = (sync_manager_t *)context;
  if (!mgr) {
    return false;
  }

  switch (event) {
  case CHASE_STARVED:
    /* A downstream chaser needs more blocks - distribute pending work */
    log_debug(LOG_COMP_SYNC, "CHASE_STARVED: distributing pending work");
    download_mgr_distribute_work(mgr->download_mgr);
    break;

  case CHASE_SPLIT: {
    /* Split work from a slow peer (peer pointer in value.object) */
    peer_t *slow_peer = (peer_t *)value.object;
    if (slow_peer) {
      log_debug(LOG_COMP_SYNC, "CHASE_SPLIT: splitting work from peer %s",
                slow_peer->address);
      size_t split = download_mgr_split_work(mgr->download_mgr, slow_peer);
      if (split > 0) {
        /* Redistribute the split work to other peers */
        download_mgr_distribute_work(mgr->download_mgr);
      }
    }
    break;
  }

  case CHASE_STOP:
    /* Stop receiving events */
    return false;

  default:
    /* Ignore other events */
    break;
  }

  return true; /* Continue receiving events */
}

/* ============================================================================
 * Rate Calculation Helper
 * ============================================================================
 */

/**
 * Calculate blocks per second using a rolling window.
 * Returns rate based on last ROLLING_WINDOW_BLOCKS (500) blocks.
 * Updates the rolling window as validated height increases.
 */
static float calc_blocks_per_second(struct sync_manager *mgr) {
  if (mgr->block_sync_start_time == 0) {
    return 0.0f;
  }

  uint64_t now = plat_time_ms();
  uint32_t current_height = chainstate_get_height(mgr->chainstate);

  /* Initialize rolling window on first call */
  if (mgr->rolling_window_start_time == 0) {
    mgr->rolling_window_start_height = current_height;
    mgr->rolling_window_start_time = now;
    mgr->last_validated_height = current_height;
    return 0.0f;
  }

  /* Slide window forward when we've validated ROLLING_WINDOW_BLOCKS */
  uint32_t blocks_in_window = current_height - mgr->rolling_window_start_height;
  if (blocks_in_window >= ROLLING_WINDOW_BLOCKS) {
    /* Move window start to current - ROLLING_WINDOW_BLOCKS */
    uint32_t new_start = current_height - ROLLING_WINDOW_BLOCKS;
    /* Estimate time at new_start using linear interpolation */
    uint32_t old_range = current_height - mgr->rolling_window_start_height;
    uint32_t shift = new_start - mgr->rolling_window_start_height;
    uint64_t elapsed = now - mgr->rolling_window_start_time;
    uint64_t time_shift = (elapsed * shift) / old_range;
    mgr->rolling_window_start_height = new_start;
    mgr->rolling_window_start_time += time_shift;
  }

  mgr->last_validated_height = current_height;

  /* Calculate rate from current window */
  blocks_in_window = current_height - mgr->rolling_window_start_height;
  if (blocks_in_window == 0) {
    return 0.0f;
  }

  uint64_t elapsed_ms = now - mgr->rolling_window_start_time;
  if (elapsed_ms == 0) {
    return 0.0f;
  }

  return (float)blocks_in_window / ((float)elapsed_ms / 1000.0f);
}

/* ============================================================================
 * Pending Header Queue Helpers
 * ============================================================================
 */

/**
 * Queue a header for deferred database persistence.
 * Used during SYNC_MODE_HEADERS to avoid per-header INSERTs.
 */
static echo_result_t pending_headers_add(sync_manager_t *mgr,
                                         const block_header_t *header,
                                         block_index_t *index) {
  /* Grow array if needed */
  if (mgr->pending_headers_count >= mgr->pending_headers_capacity) {
    size_t new_cap = mgr->pending_headers_capacity == 0
                         ? PENDING_HEADERS_INITIAL_CAPACITY
                         : mgr->pending_headers_capacity * 2;
    pending_header_t *new_arr =
        realloc(mgr->pending_headers, new_cap * sizeof(pending_header_t));
    if (!new_arr) {
      return ECHO_ERR_NOMEM;
    }
    mgr->pending_headers = new_arr;
    mgr->pending_headers_capacity = new_cap;
  }

  /* Add entry */
  pending_header_t *entry = &mgr->pending_headers[mgr->pending_headers_count++];
  entry->header = *header;
  entry->index = index;

  return ECHO_OK;
}

/**
 * Flush all pending headers to the database via store_header callback.
 * Called when transitioning from SYNC_MODE_HEADERS to SYNC_MODE_BLOCKS.
 */
static echo_result_t pending_headers_flush(sync_manager_t *mgr) {
  if (mgr->pending_headers_count == 0) {
    return ECHO_OK;
  }

  if (!mgr->callbacks.store_header) {
    /* No callback - just clear the queue */
    mgr->pending_headers_count = 0;
    return ECHO_OK;
  }

  /* Begin transaction */
  if (mgr->callbacks.begin_header_batch) {
    mgr->callbacks.begin_header_batch(mgr->callbacks.ctx);
  }

  /* Store all pending headers */
  size_t stored = 0;
  size_t errors = 0;
  for (size_t i = 0; i < mgr->pending_headers_count; i++) {
    pending_header_t *entry = &mgr->pending_headers[i];
    echo_result_t result = mgr->callbacks.store_header(
        &entry->header, entry->index, mgr->callbacks.ctx);
    if (result == ECHO_OK || result == ECHO_ERR_EXISTS) {
      stored++;
    } else {
      errors++;
      if (errors <= 3) {
        log_warn(LOG_COMP_SYNC, "Failed to store header at height %u: %d",
                 entry->index->height, result);
      }
    }
  }

  /* Commit transaction */
  if (mgr->callbacks.commit_header_batch) {
    mgr->callbacks.commit_header_batch(mgr->callbacks.ctx);
  }

  log_info(LOG_COMP_SYNC, "Flushed %zu headers to database (%zu errors)",
           stored, errors);

  /* Clear the queue */
  mgr->pending_headers_count = 0;

  return errors > 0 ? ECHO_ERR_IO : ECHO_OK;
}

/* ============================================================================
 * Block Locator
 * ============================================================================
 */

echo_result_t sync_build_locator(const chainstate_t *state, hash256_t *locator,
                                 size_t *locator_len) {
  if (!state || !locator || !locator_len) {
    return ECHO_ERR_NULL_PARAM;
  }

  block_index_t *tip = chainstate_get_tip_index(state);
  if (!tip) {
    /*
     * No blocks yet - return genesis hash as the only locator entry.
     * This tells peers we want headers starting from block 1.
     */
    block_header_t genesis;
    block_genesis_header(&genesis);
    block_header_hash(&genesis, &locator[0]);
    *locator_len = 1;
    return ECHO_OK;
  }

  /* NOLINTBEGIN(clang-analyzer-core.Cast) - chainstate_get_block_index_map
   * doesn't modify state, but API lacks const qualifier */
  block_index_map_t *index_map =
      chainstate_get_block_index_map((chainstate_t *)(uintptr_t)state);
  /* NOLINTEND(clang-analyzer-core.Cast) */
  return sync_build_locator_from(index_map, tip, locator, locator_len);
}

echo_result_t sync_build_locator_from(const block_index_map_t *index_map,
                                      const block_index_t *start,
                                      hash256_t *locator, size_t *locator_len) {
  if (!locator || !locator_len) {
    return ECHO_ERR_NULL_PARAM;
  }

  (void)index_map; /* May be used for additional lookups in future */

  if (!start) {
    *locator_len = 0;
    return ECHO_OK;
  }

  size_t count = 0;
  const block_index_t *idx = start;
  uint32_t step = 1;
  uint32_t steps_since_increase = 0;

  /* Walk back from tip, adding hashes with exponentially increasing step */
  while (idx && count < SYNC_MAX_LOCATOR_HASHES) {
    locator[count++] = idx->hash;

    /* Move back 'step' blocks */
    for (uint32_t i = 0; i < step && idx->prev; i++) {
      idx = idx->prev;
    }

    /* After first 10 entries, double step size every 2 entries */
    if (count > 10) {
      steps_since_increase++;
      if (steps_since_increase >= 2) {
        step *= 2;
        steps_since_increase = 0;
      }
    }

    /* If we've moved to genesis or same block, we're done */
    if (!idx->prev) {
      if (count > 0 && count < SYNC_MAX_LOCATOR_HASHES &&
          memcmp(&locator[count - 1], &idx->hash, sizeof(hash256_t)) != 0) {
        /* Add genesis if not already added */
        locator[count++] = idx->hash;
      }
      break;
    }
  }

  *locator_len = count;
  return ECHO_OK;
}

block_index_t *sync_find_locator_fork(const chainstate_t *state,
                                      const hash256_t *locator, size_t count) {
  if (!state || !locator || count == 0) {
    return NULL;
  }

  /* NOLINTBEGIN(clang-analyzer-core.Cast) - chainstate_get_block_index_map
   * doesn't modify state, but API lacks const qualifier */
  block_index_map_t *index_map =
      chainstate_get_block_index_map((chainstate_t *)(uintptr_t)state);
  /* NOLINTEND(clang-analyzer-core.Cast) */
  if (!index_map) {
    return NULL;
  }

  /* Search for first matching block in locator */
  for (size_t i = 0; i < count; i++) {
    block_index_t *idx = block_index_map_lookup(index_map, &locator[i]);
    if (idx && idx->on_main_chain) {
      return idx;
    }
  }

  /* No match found - return genesis or NULL */
  hash256_t genesis_hash;
  block_header_t genesis;
  block_genesis_header(&genesis);
  block_header_hash(&genesis, &genesis_hash);
  return block_index_map_lookup(index_map, &genesis_hash);
}

/* ============================================================================
 * Sync Manager Implementation
 * ============================================================================
 */

/**
 * Find peer sync state by peer pointer.
 */
static peer_sync_state_t *find_peer_state(sync_manager_t *mgr, peer_t *peer) {
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].peer == peer) {
      return &mgr->peers[i];
    }
  }
  return NULL;
}

/**
 * Find best peer for header sync (highest start_height, not in flight).
 * Note: Will be used when implementing selective header sync peer selection.
 */
/* NOLINTNEXTLINE(misc-unused-parameters) - reserved for future use */
__attribute__((unused)) static peer_sync_state_t *
find_best_header_peer(sync_manager_t *mgr) {
  peer_sync_state_t *best = NULL;
  int32_t best_height = -1;

  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_sync_state_t *ps = &mgr->peers[i];
    if (ps->sync_candidate && !ps->headers_in_flight &&
        peer_is_ready(ps->peer)) {
      if (ps->start_height > best_height) {
        best_height = ps->start_height;
        best = ps;
      }
    }
  }
  return best;
}

/**
 * Count sync-eligible peers.
 */
static size_t count_sync_peers(const sync_manager_t *mgr) {
  size_t count = 0;
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].sync_candidate && peer_is_ready(mgr->peers[i].peer)) {
      count++;
    }
  }
  return count;
}

sync_manager_t *sync_create(chainstate_t *chainstate,
                            const sync_callbacks_t *callbacks,
                            uint32_t download_window,
                            chase_dispatcher_t *dispatcher) {
  if (!chainstate || !callbacks) {
    return NULL;
  }

  /* Use default if 0 passed */
  if (download_window == 0) {
    download_window = SYNC_BLOCK_DOWNLOAD_WINDOW;
  }

  sync_manager_t *mgr = calloc(1, sizeof(sync_manager_t));
  if (!mgr) {
    return NULL;
  }

  /* Create download manager - sole source of truth for block downloads */
  download_callbacks_t dm_callbacks = {
      .send_getdata = dm_send_getdata,
      .disconnect_peer = dm_disconnect_peer,
      .ctx = mgr};
  mgr->download_mgr = download_mgr_create(&dm_callbacks, download_window);
  if (!mgr->download_mgr) {
    free(mgr);
    return NULL;
  }

  mgr->chainstate = chainstate;
  mgr->callbacks = *callbacks;
  mgr->dispatcher = dispatcher;
  mgr->subscription = NULL;
  mgr->mode = SYNC_MODE_IDLE;

  /* Subscribe to chase events for download coordination */
  if (dispatcher != NULL) {
    mgr->subscription =
        chase_subscribe(dispatcher, sync_chase_handler, mgr);
    if (mgr->subscription != NULL) {
      log_debug(LOG_COMP_SYNC, "Subscribed to chase events");
    }
  }
  mgr->peer_count = 0;
  mgr->download_window = download_window;

  /* Initialize adaptive stalling timeout to 2 seconds */
  mgr->stalling_timeout_ms = SYNC_BLOCK_STALLING_TIMEOUT_MS;

  /* Initialize pending headers queue for deferred persistence */
  mgr->pending_headers = NULL;
  mgr->pending_headers_count = 0;
  mgr->pending_headers_capacity = 0;

  /* Initialize best header to current tip_index (which should be the best
   * header after restoration, not the validated tip) */
  mgr->best_header = chainstate_get_tip_index(chainstate);

  log_info(LOG_COMP_SYNC,
           "sync_create: best_header=%p (height=%u), chainstate_height=%u",
           (void *)mgr->best_header,
           mgr->best_header ? mgr->best_header->height : 0,
           chainstate_get_height(chainstate));

  return mgr;
}

void sync_destroy(sync_manager_t *mgr) {
  if (!mgr) {
    return;
  }

  /* Unsubscribe from chase events */
  if (mgr->subscription != NULL && mgr->dispatcher != NULL) {
    chase_unsubscribe(mgr->dispatcher, mgr->subscription);
    mgr->subscription = NULL;
  }

  download_mgr_destroy(mgr->download_mgr);
  free(mgr->pending_headers);
  free(mgr);
}

void sync_add_peer(sync_manager_t *mgr, peer_t *peer, int32_t height) {
  if (!mgr || !peer) {
    return;
  }

  /* Check if already tracked */
  if (find_peer_state(mgr, peer)) {
    return;
  }

  /* Check capacity */
  if (mgr->peer_count >= SYNC_MAX_PEERS) {
    return;
  }

  /* Add peer */
  peer_sync_state_t *ps = &mgr->peers[mgr->peer_count++];
  memset(ps, 0, sizeof(peer_sync_state_t));
  ps->peer = peer;
  ps->start_height = height;

  /* Peer is sync candidate if:
   * 1. They have blocks we need (height > our_height)
   * 2. They can serve full historical blocks (full archival node)
   *
   * Per BIP-159:
   *   - Pruned nodes: MUST NOT set NODE_NETWORK, only NODE_NETWORK_LIMITED
   *   - Full archival nodes: Set NODE_NETWORK, MAY also set NODE_NETWORK_LIMITED
   *
   * So checking NODE_NETWORK is sufficient - if set, peer can serve all blocks.
   */
  uint32_t our_height = chainstate_get_height(mgr->chainstate);
  echo_bool_t has_blocks = (height > (int32_t)our_height);
  echo_bool_t can_serve_full = (peer->services & SERVICE_NODE_NETWORK) != 0;
  ps->sync_candidate = has_blocks && can_serve_full;

  /* Add to download manager for performance tracking (if sync candidate) */
  if (ps->sync_candidate) {
    download_mgr_add_peer(mgr->download_mgr, peer);
  }

  log_info(LOG_COMP_SYNC, "Added peer %s to sync_mgr: height=%d, our_height=%u, "
           "services=0x%llx, sync_candidate=%s%s, state=%s",
           peer->address, height, our_height, (unsigned long long)peer->services,
           ps->sync_candidate ? "yes" : "no",
           (has_blocks && !can_serve_full) ? " (PRUNED - rejected)" : "",
           peer_state_string(peer->state));
}

void sync_remove_peer(sync_manager_t *mgr, peer_t *peer) {
  if (!mgr || !peer) {
    return;
  }

  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].peer == peer) {
      log_info(LOG_COMP_SYNC, "Removing peer %s from sync_mgr (state=%s)",
               peer->address, peer_state_string(peer->state));

      /* Remove from download manager (handles unassigning in-flight blocks) */
      download_mgr_remove_peer(mgr->download_mgr, peer);

      /* Remove by shifting remaining peers */
      for (size_t j = i; j < mgr->peer_count - 1; j++) {
        mgr->peers[j] = mgr->peers[j + 1];
      }
      mgr->peer_count--;
      return;
    }
  }
}

echo_result_t sync_start(sync_manager_t *mgr) {
  if (!mgr) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (mgr->mode != SYNC_MODE_IDLE && mgr->mode != SYNC_MODE_STALLED) {
    return ECHO_ERR_INVALID_STATE;
  }

  if (count_sync_peers(mgr) == 0) {
    return ECHO_ERR_INVALID_STATE;
  }

  uint64_t now = plat_time_ms();
  mgr->start_time = now;
  mgr->last_progress_time = now;

  /*
   * If we already have headers beyond the validated tip (e.g., from a previous
   * session), skip straight to BLOCKS mode. Otherwise start with HEADERS.
   */
  uint32_t validated_height = chainstate_get_height(mgr->chainstate);
  uint32_t best_header_height = mgr->best_header ? mgr->best_header->height : 0;

  log_info(LOG_COMP_SYNC,
           "sync_start: best_header=%p (height=%u), validated_height=%u",
           (void *)mgr->best_header, best_header_height, validated_height);

  bool resume_in_blocks_mode =
      (mgr->best_header != NULL && best_header_height > validated_height);

  if (resume_in_blocks_mode) {
    log_info(LOG_COMP_SYNC,
             "Starting sync in BLOCKS mode (headers=%u, validated=%u)",
             best_header_height, validated_height);
    mgr->mode = SYNC_MODE_BLOCKS;
    mgr->block_sync_start_time = now;
    mgr->block_sync_start_height = validated_height;
  } else {
    log_info(LOG_COMP_SYNC, "Starting headers-first sync");
    mgr->mode = SYNC_MODE_HEADERS;
  }

  return ECHO_OK;
}

void sync_stop(sync_manager_t *mgr) {
  if (!mgr) {
    return;
  }

  mgr->mode = SYNC_MODE_IDLE;

  /* Clear header in-flight state (block download state is in download_mgr) */
  for (size_t i = 0; i < mgr->peer_count; i++) {
    mgr->peers[i].headers_in_flight = false;
  }

  /* Note: download_mgr keeps its state. In-flight work will timeout and be
   * reassigned when sync resumes. This is intentional - we don't want to
   * lose work tracking if sync is temporarily stopped. */
}

echo_result_t sync_handle_headers(sync_manager_t *mgr, peer_t *peer,
                                  const block_header_t *headers, size_t count) {
  if (!mgr || !peer || (!headers && count > 0)) {
    return ECHO_ERR_NULL_PARAM;
  }

  peer_sync_state_t *ps = find_peer_state(mgr, peer);
  if (!ps) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Clear in-flight flag */
  ps->headers_in_flight = false;

  /* Track response time for peer performance ranking.
   * Used to identify the fastest peer in header sync race. */
  uint64_t now = plat_time_ms();
  if (ps->headers_sent_time > 0 && count == SYNC_MAX_HEADERS_PER_REQUEST) {
    uint64_t response_ms = now - ps->headers_sent_time;

    /* Accumulate race statistics for best-of-N selection */
    if (mgr->mode == SYNC_MODE_HEADERS && !mgr->headers_race_complete) {
      ps->headers_race_responses++;
      ps->headers_race_total_ms += response_ms;

      /* Check if this peer has completed all race rounds */
      if (ps->headers_race_responses >= HEADERS_RACE_ROUNDS) {
        /* Count how many peers have completed the race */
        size_t completed_count = 0;
        uint64_t best_avg = UINT64_MAX;

        for (size_t i = 0; i < mgr->peer_count; i++) {
          peer_sync_state_t *candidate = &mgr->peers[i];
          if (candidate->headers_race_responses >= HEADERS_RACE_ROUNDS) {
            completed_count++;
            uint64_t avg = candidate->headers_race_total_ms /
                           candidate->headers_race_responses;
            if (avg < best_avg) {
              best_avg = avg;
            }
          }
        }

        /* Select pool once MIN_PEERS complete the race.
         * Top 8 peers form a pool - we always use the best performer. */
        if (completed_count >= HEADERS_RACE_MIN_PEERS) {
          mgr->headers_race_complete = true;
          mgr->header_pool_count = 0;
          mgr->active_peer_responses = 0;

          /* Build sorted list of top HEADER_POOL_SIZE peers by avg response time */
          for (size_t p = 0; p < HEADER_POOL_SIZE && p < completed_count; p++) {
            uint64_t pool_best_avg = UINT64_MAX;
            size_t pool_best_idx = 0;

            for (size_t i = 0; i < mgr->peer_count; i++) {
              /* Skip if already in pool */
              bool already_in_pool = false;
              for (size_t k = 0; k < mgr->header_pool_count; k++) {
                if (mgr->header_pool_indices[k] == i) {
                  already_in_pool = true;
                  break;
                }
              }
              if (already_in_pool) continue;

              peer_sync_state_t *c = &mgr->peers[i];
              if (c->headers_race_responses >= HEADERS_RACE_ROUNDS) {
                uint64_t avg = c->headers_race_total_ms / c->headers_race_responses;
                if (avg < pool_best_avg) {
                  pool_best_avg = avg;
                  pool_best_idx = i;
                }
              }
            }

            if (pool_best_avg < UINT64_MAX) {
              mgr->header_pool_indices[mgr->header_pool_count++] = pool_best_idx;
            }
          }

          /* First peer in pool is the initial active peer (best from race) */
          if (mgr->header_pool_count > 0) {
            mgr->active_pool_peer_idx = mgr->header_pool_indices[0];
            mgr->fastest_header_peer_idx = mgr->active_pool_peer_idx; /* For compatibility */
          }

          log_info(LOG_COMP_SYNC,
                   "Headers race complete: pool of %zu peers from %zu candidates (best avg=%lums)",
                   mgr->header_pool_count, completed_count, (unsigned long)best_avg);
        }
      }
    }

    /* Post-race: track performance for all pool members */
    if (mgr->headers_race_complete) {
      size_t peer_idx = (size_t)(ps - mgr->peers);

      /* Update ring buffer for this peer's response times */
      ps->recent_times[ps->recent_times_idx] = response_ms;
      ps->recent_times_idx = (ps->recent_times_idx + 1) % SYNC_HEADER_RESPONSE_WINDOW;
      if (ps->recent_times_count < SYNC_HEADER_RESPONSE_WINDOW) {
        ps->recent_times_count++;
      }

      /* If this is the active peer, increment response counter and evaluate pool */
      if (peer_idx == mgr->active_pool_peer_idx) {
        mgr->active_peer_responses++;

        if (mgr->active_peer_responses >= POOL_PROBE_INTERVAL) {
          mgr->active_peer_responses = 0;

          /* Find best performer in pool based on recent performance */
          uint64_t best_recent_avg = UINT64_MAX;
          size_t best_pool_idx = mgr->active_pool_peer_idx;

          for (size_t p = 0; p < mgr->header_pool_count; p++) {
            size_t idx = mgr->header_pool_indices[p];
            if (idx >= mgr->peer_count) continue;

            peer_sync_state_t *pool_peer = &mgr->peers[idx];
            if (!pool_peer->sync_candidate || !peer_is_ready(pool_peer->peer)) continue;

            /* Calculate average from ring buffer */
            if (pool_peer->recent_times_count > 0) {
              uint64_t sum = 0;
              for (uint32_t i = 0; i < pool_peer->recent_times_count; i++) {
                sum += pool_peer->recent_times[i];
              }
              uint64_t avg = sum / pool_peer->recent_times_count;
              if (avg < best_recent_avg) {
                best_recent_avg = avg;
                best_pool_idx = idx;
              }
            }
          }

          /* Switch if a different peer is now best */
          if (best_pool_idx != mgr->active_pool_peer_idx && best_recent_avg < UINT64_MAX) {
            peer_sync_state_t *old_active = &mgr->peers[mgr->active_pool_peer_idx];
            peer_sync_state_t *new_active = &mgr->peers[best_pool_idx];

            /* Calculate old active's average for logging */
            uint64_t old_avg = UINT64_MAX;
            if (old_active->recent_times_count > 0) {
              uint64_t sum = 0;
              for (uint32_t i = 0; i < old_active->recent_times_count; i++) {
                sum += old_active->recent_times[i];
              }
              old_avg = sum / old_active->recent_times_count;
            }

            log_info(LOG_COMP_SYNC,
                     "Pool switch: %s (avg=%lums) -> %s (avg=%lums)",
                     old_active->peer->address, (unsigned long)old_avg,
                     new_active->peer->address, (unsigned long)best_recent_avg);

            mgr->active_pool_peer_idx = best_pool_idx;
            mgr->fastest_header_peer_idx = best_pool_idx; /* For compatibility */
          }

          /* Probe non-active pool members to keep their stats fresh */
          if (mgr->header_pool_count > 1) {
            uint64_t now = plat_time_ms();

            /* Build locator once for all probes */
            hash256_t locator[SYNC_MAX_LOCATOR_HASHES];
            size_t locator_len = 0;
            block_index_t *locator_tip = mgr->best_header;
            if (locator_tip != NULL) {
              block_index_map_t *map =
                  chainstate_get_block_index_map(mgr->chainstate);
              sync_build_locator_from(map, locator_tip, locator, &locator_len);
            } else {
              sync_build_locator(mgr->chainstate, locator, &locator_len);
            }

            /* Send probe to each non-active pool member that's available */
            for (size_t p = 0; p < mgr->header_pool_count; p++) {
              size_t idx = mgr->header_pool_indices[p];
              if (idx == mgr->active_pool_peer_idx) continue; /* Skip active peer */
              if (idx >= mgr->peer_count) continue;

              peer_sync_state_t *pool_peer = &mgr->peers[idx];
              if (!pool_peer->sync_candidate || !peer_is_ready(pool_peer->peer)) continue;
              if (pool_peer->headers_in_flight) continue;

              pool_peer->headers_in_flight = true;
              pool_peer->headers_sent_time = now;

              if (mgr->callbacks.send_getheaders) {
                mgr->callbacks.send_getheaders(pool_peer->peer, locator, locator_len,
                                               NULL, mgr->callbacks.ctx);
              }
            }
          }
        }
      }
    }
  }

  /* Reset sent time to allow immediate follow-up request (fixes 5-second throttle bug).
   * The SYNC_HEADER_RETRY_INTERVAL_MS is meant for retries on timeout, not as a
   * throttle between successful responses. */
  ps->headers_sent_time = 0;

  if (count == 0) {
    /* No more headers from this peer - check if we should transition to blocks */
    log_info(LOG_COMP_SYNC,
             "0 headers: mode=%d, best_header=%p, tip_height=%u",
             mgr->mode, (void *)mgr->best_header,
             chainstate_get_height(mgr->chainstate));
    if (mgr->mode == SYNC_MODE_HEADERS && mgr->best_header) {
      uint32_t tip_height = chainstate_get_height(mgr->chainstate);
      if (mgr->best_header->height > tip_height) {
        log_info(LOG_COMP_SYNC,
                 "Transitioning to BLOCKS mode (best_header=%u, validated=%u)",
                 mgr->best_header->height, tip_height);
        mgr->mode = SYNC_MODE_BLOCKS;
        if (mgr->block_sync_start_time == 0) {
          mgr->block_sync_start_time = plat_time_ms();
          mgr->block_sync_start_height = tip_height;
        }
      }
    }
    return ECHO_OK;
  }

  /* Process each header */
  block_index_map_t *index_map =
      chainstate_get_block_index_map(mgr->chainstate);
  block_index_t *prev_index = NULL;

  /* Begin database transaction for batch insert */
  if (mgr->callbacks.begin_header_batch) {
    mgr->callbacks.begin_header_batch(mgr->callbacks.ctx);
  }

  for (size_t i = 0; i < count; i++) {
    const block_header_t *header = &headers[i];

    /* Compute header hash */
    hash256_t header_hash;
    if (block_header_hash(header, &header_hash) != ECHO_OK) {
      return ECHO_ERR_INVALID;
    }

    /* Check if we already have this header */
    block_index_t *existing = block_index_map_lookup(index_map, &header_hash);
    if (existing) {
      prev_index = existing;
      continue;
    }

    /* Find previous block */
    if (i == 0) {
      prev_index = block_index_map_lookup(index_map, &header->prev_hash);
    }

    if (!prev_index) {
      /* Orphan header - can't connect to our chain */
      return ECHO_ERR_INVALID;
    }

    /* Validate header if callback provided (pass pre-computed hash) */
    if (mgr->callbacks.validate_header) {
      echo_result_t result = mgr->callbacks.validate_header(
          header, &header_hash, prev_index, mgr->callbacks.ctx);
      if (result != ECHO_OK) {
        return ECHO_ERR_INVALID;
      }
    }

    /* Add header to chain state (pass pre-computed hash) */
    block_index_t *new_index = NULL;
    echo_result_t result = chainstate_add_header_with_hash(
        mgr->chainstate, header, &header_hash, &new_index);

    if (result == ECHO_ERR_EXISTS) {
      /* Already have it - continue with next */
      new_index = block_index_map_lookup(index_map, &header_hash);
    } else if (result != ECHO_OK) {
      return result;
    }

    /* Persist or queue header.
     * OPTIMIZATION: During SYNC_MODE_HEADERS, queue headers for deferred
     * persistence instead of writing each one immediately. This reduces
     * ~870K individual SQLite INSERTs to one batched write at mode transition,
     * cutting header sync time from ~10 min to ~1 min. */
    if (new_index) {
      if (mgr->mode == SYNC_MODE_HEADERS) {
        /* Queue for later flush */
        pending_headers_add(mgr, header, new_index);
      } else if (mgr->callbacks.store_header) {
        /* Immediate persistence */
        echo_result_t store_result =
            mgr->callbacks.store_header(header, new_index, mgr->callbacks.ctx);
        if (store_result != ECHO_OK && store_result != ECHO_ERR_EXISTS) {
          log_warn(LOG_COMP_SYNC, "Failed to persist header at height %u: %d",
                   new_index->height, store_result);
        }
      }
    }

    /* Update best header if this has more work */
    if (new_index && (!mgr->best_header ||
                      work256_compare(&new_index->chainwork,
                                      &mgr->best_header->chainwork) > 0)) {
      mgr->best_header = new_index;
    }

    prev_index = new_index;
    mgr->headers_received_total++;
    ps->headers_received++;
  }

  /* Commit database transaction for batch insert */
  if (mgr->callbacks.commit_header_batch) {
    mgr->callbacks.commit_header_batch(mgr->callbacks.ctx);
  }

  ps->last_header_hash = headers[count - 1].prev_hash;
  block_header_hash(&headers[count - 1], &ps->last_header_hash);

  /* Update per-peer header tip for parallel sync.
   * This allows each peer to advance independently instead of all
   * peers requesting from the same global best_header. */
  ps->peer_best_header = prev_index;

  mgr->last_progress_time = plat_time_ms();

  /* If we got max headers, request more */
  if (count == SYNC_MAX_HEADERS_PER_REQUEST) {
    /* Will request more on next tick */
  } else {
    /* Got fewer than max - peer has no more headers */
    /* Check if we should transition to block download */
    if (mgr->mode == SYNC_MODE_HEADERS && mgr->best_header) {
      uint32_t tip_height = chainstate_get_height(mgr->chainstate);
      if (mgr->best_header->height > tip_height) {
        /* Flush all queued headers to database before switching modes. */
        if (mgr->pending_headers_count > 0) {
          log_info(LOG_COMP_SYNC,
                   "Flushing %zu queued headers to database...",
                   mgr->pending_headers_count);
          echo_result_t flush_result = pending_headers_flush(mgr);
          if (flush_result != ECHO_OK) {
            log_error(LOG_COMP_SYNC, "Failed to flush headers: %d",
                      flush_result);
            /* Continue anyway - headers are in memory */
          }
        }

        mgr->mode = SYNC_MODE_BLOCKS;
        if (mgr->block_sync_start_time == 0) {
          mgr->block_sync_start_time = plat_time_ms();
          mgr->block_sync_start_height = tip_height;
        }
      }
    }
  }

  /* Immediate follow-up: If we got a full batch (2000 headers), there's more to fetch.
   * Send the next getheaders request immediately instead of waiting for sync_tick.
   * This dramatically improves header sync throughput. */
  if (mgr->mode == SYNC_MODE_HEADERS && count == SYNC_MAX_HEADERS_PER_REQUEST &&
      ps->sync_candidate && peer_is_ready(ps->peer) && mgr->callbacks.send_getheaders) {
    hash256_t locator[SYNC_MAX_LOCATOR_HASHES];
    size_t locator_len = 0;

    block_index_t *locator_tip = mgr->best_header;
    if (locator_tip != NULL) {
      block_index_map_t *map = chainstate_get_block_index_map(mgr->chainstate);
      sync_build_locator_from(map, locator_tip, locator, &locator_len);
    } else {
      sync_build_locator(mgr->chainstate, locator, &locator_len);
    }

    ps->headers_in_flight = true;
    ps->headers_sent_time = plat_time_ms();
    mgr->callbacks.send_getheaders(ps->peer, locator, locator_len, NULL,
                                   mgr->callbacks.ctx);
  }

  return ECHO_OK;
}

echo_result_t sync_handle_block(sync_manager_t *mgr, peer_t *peer,
                                const block_t *block) {
  if (!mgr || !peer || !block) {
    return ECHO_ERR_NULL_PARAM;
  }

  peer_sync_state_t *ps = find_peer_state(mgr, peer);
  if (!ps) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Successful block delivery - decay timeout counter to reward good behavior.
   * This prevents peers from being disconnected if they're usually responsive
   * but occasionally slow. Combined with the 6-event threshold, gives peers
   * multiple chances before disconnection.
   */
  if (ps->timeout_count > 0) {
    ps->timeout_count--;
  }

  /* Compute block hash */
  hash256_t block_hash;
  if (block_header_hash(&block->header, &block_hash) != ECHO_OK) {
    return ECHO_ERR_INVALID;
  }

  uint64_t now = plat_time_ms();

  /* Track delivery times for this peer */
  if (ps->first_block_time == 0) {
    ps->first_block_time = now;
  }
  ps->last_delivery_time = now;

  /* Notify download manager of block receipt for performance tracking.
   * Calculate approximate block size: header + tx count varint + txs.
   * This doesn't need to be exact - it's for relative peer comparison.
   */
  size_t block_bytes = 80 + block->tx_count * 250; /* Rough estimate: 250 bytes/tx avg */
  download_mgr_block_received(mgr->download_mgr, peer, &block_hash, block_bytes);

  /* Store block if callback provided (before validation - we need the data) */
  if (mgr->callbacks.store_block) {
    mgr->callbacks.store_block(block, mgr->callbacks.ctx);
  }

  /* Find block index and mark complete */
  block_index_map_t *index_map =
      chainstate_get_block_index_map(mgr->chainstate);
  block_index_t *block_index = block_index_map_lookup(index_map, &block_hash);

  if (block_index) {
    download_mgr_block_complete(mgr->download_mgr, &block_hash,
                                block_index->height);

    /* Notify chaser pipeline that block is ready for validation */
    if (mgr->dispatcher != NULL) {
      chase_notify_height(mgr->dispatcher, CHASE_CHECKED, block_index->height);
    }
  }

  mgr->blocks_received_total++;
  ps->blocks_received++;
  ps->last_delivery_time = plat_time_ms(); /* Track for session reputation */
  mgr->last_progress_time = ps->last_delivery_time;

  /* Check if sync is complete */
  if (mgr->mode == SYNC_MODE_BLOCKS &&
      download_mgr_pending_count(mgr->download_mgr) == 0 &&
      download_mgr_inflight_count(mgr->download_mgr) == 0) {
    uint32_t tip_height = chainstate_get_height(mgr->chainstate);
    if (!mgr->best_header || tip_height >= mgr->best_header->height) {
      mgr->mode = SYNC_MODE_DONE;
    }
  }

  return ECHO_OK;
}

void sync_process_timeouts(sync_manager_t *mgr) {
  if (!mgr) {
    return;
  }

  uint64_t now = plat_time_ms();

  /* Check for stalled sync */
  if (mgr->mode != SYNC_MODE_IDLE && mgr->mode != SYNC_MODE_DONE) {
    if (now - mgr->last_progress_time > SYNC_STALE_TIP_THRESHOLD_MS) {
      mgr->mode = SYNC_MODE_STALLED;
    }
  }

  /*
   * Block download performance checking is now handled by download_mgr.
   * It tracks per-peer throughput and detects stalls internally.
   * We call download_mgr_check_performance() in sync_tick.
   */

  /* Process header timeouts */
  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_sync_state_t *ps = &mgr->peers[i];

    if (ps->headers_in_flight &&
        (now - ps->headers_sent_time > SYNC_HEADERS_TIMEOUT_MS)) {
      ps->headers_in_flight = false;
      ps->timeout_count++;
    }

    /* Block stall detection is now handled by download_mgr_check_performance() */

    /* Block delivery rate check: disconnect peers that accept requests
     * but don't deliver blocks. Ping RTT is irrelevant - only actual
     * block delivery matters.
     *
     * Non-responders (0% delivery) are culled after 30s/30 requests.
     * Low-delivery peers (<10%) are only culled if we have >16 peers,
     * to avoid a death spiral when we're already short on peers.
     */
    if (ps->peer && ps->blocks_requested >= 30) {
      uint64_t connected_ms = now - ps->peer->connect_time;
      uint32_t delivery_pct = (ps->blocks_received * 100) / ps->blocks_requested;

      /* Zero delivery - always cull after 30s */
      if (delivery_pct == 0 && connected_ms >= 30000) {
        log_info(LOG_COMP_SYNC,
                 "Disconnecting non-responding peer: 0/%u blocks over %llus",
                 ps->blocks_requested, (unsigned long long)(connected_ms / 1000));
        peer_disconnect(ps->peer, PEER_DISCONNECT_MISBEHAVING,
                        "Zero block delivery during IBD");
        continue;
      }

      /* Low delivery - only cull if we have enough peers */
      if (delivery_pct < 10 && connected_ms >= 60000 &&
          ps->blocks_requested >= 50) {
        size_t sync_peers = count_sync_peers(mgr);
        if (sync_peers > 16) {
          log_info(LOG_COMP_SYNC,
                   "Disconnecting poor-delivery peer: %u/%u blocks (%u%%) "
                   "over %llus (have %zu sync peers)",
                   ps->blocks_received, ps->blocks_requested, delivery_pct,
                   (unsigned long long)(connected_ms / 1000), sync_peers);
          peer_disconnect(ps->peer, PEER_DISCONNECT_MISBEHAVING,
                          "Poor block delivery rate during IBD");
          continue;
        }
      }
    }
  }
}

/**
 * Queue blocks for download from headers.
 */
static void queue_blocks_from_headers(sync_manager_t *mgr) {
  if (!mgr->best_header) {
    log_debug(LOG_COMP_SYNC, "queue_blocks: no best_header");
    return;
  }

  uint32_t tip_height = chainstate_get_height(mgr->chainstate);

  /* Fixed download window (simplified from adaptive) */
  uint32_t effective_window = mgr->download_window;

  /* Calculate target range */
  uint32_t start_height = tip_height + 1;
  uint32_t end_height = tip_height + effective_window;
  if (end_height > mgr->best_header->height) {
    end_height = mgr->best_header->height;
  }

  log_info(LOG_COMP_SYNC,
           "queue_blocks: tip=%u, start=%u, end=%u, window=%u, best=%u",
           tip_height, start_height, end_height, effective_window,
           mgr->best_header->height);

  if (start_height > end_height) {
    /* Already fully synced */
    log_info(LOG_COMP_SYNC, "queue_blocks: already synced (start > end)");
    return;
  }

  /*
   * Use direct height lookup via callback if available (much faster for
   * large height gaps). Falls back to walking prev pointers if not.
   *
   * Array sized for max possible window (archival), but iteration limited
   * to configured window.
   */
  hash256_t to_queue[SYNC_BLOCK_DOWNLOAD_WINDOW_ARCHIVAL];
  uint32_t heights[SYNC_BLOCK_DOWNLOAD_WINDOW_ARCHIVAL];
  size_t to_queue_count = 0;

  if (mgr->callbacks.get_block_hash_at_height) {
    /* Fast path: query database by height directly */
    uint32_t lookup_failures = 0;
    for (uint32_t h = start_height;
         h <= end_height && to_queue_count < effective_window; h++) {
      hash256_t hash;
      echo_result_t cb_result = mgr->callbacks.get_block_hash_at_height(
          h, &hash, mgr->callbacks.ctx);
      if (cb_result != ECHO_OK) {
        lookup_failures++;
        /* Log first few failures to help debug */
        if (lookup_failures <= 3) {
          log_warn(LOG_COMP_SYNC,
                   "queue_blocks: height %u hash lookup failed: %d",
                   h, cb_result);
        }
        continue;
      }
      /* Check if we already have this block in download manager or storage */
      bool in_queue = download_mgr_has_block(mgr->download_mgr, &hash);
      if (!in_queue) {
        block_t stored;
        block_init(&stored);
        bool in_storage = mgr->callbacks.get_block &&
                          mgr->callbacks.get_block(&hash, &stored,
                                                   mgr->callbacks.ctx) ==
                              ECHO_OK;
        /* Log first iteration to debug */
        if (h == start_height) {
          log_info(LOG_COMP_SYNC,
                   "queue_blocks: first block h=%u in_storage=%s",
                   h, in_storage ? "YES" : "NO");
        }
        if (!in_storage) {
          to_queue[to_queue_count] = hash;
          heights[to_queue_count] = h;
          to_queue_count++;
          if (h == start_height) {
            log_info(LOG_COMP_SYNC, "queue_blocks: queuing height %u", h);
          }
        } else {
          /* Block already in storage - notify chaser to validate it */
          if (mgr->dispatcher != NULL) {
            chase_notify_height(mgr->dispatcher, CHASE_CHECKED, h);
          }
        }
        block_free(&stored);
      } else if (h <= 5) {
        log_info(LOG_COMP_SYNC, "queue_blocks: height %u already in queue", h);
      }
    }
    log_info(LOG_COMP_SYNC,
             "queue_blocks: fast path done - queued=%zu, failures=%u",
             to_queue_count, lookup_failures);
  } else {
    /* Slow path: walk back from best_header (for very old code) */
    block_index_t *idx = mgr->best_header;

    /* First, walk back to reach our target range (skip higher blocks) */
    while (idx && idx->height > end_height) {
      idx = idx->prev;
    }

    /* Collect blocks to queue (walking backward, we'll reverse later) */
    while (idx && idx->height >= start_height &&
           to_queue_count < effective_window) {
      if (!download_mgr_has_block(mgr->download_mgr, &idx->hash)) {
        block_t stored;
        block_init(&stored);
        if (!mgr->callbacks.get_block ||
            mgr->callbacks.get_block(&idx->hash, &stored,
                                      mgr->callbacks.ctx) != ECHO_OK) {
          to_queue[to_queue_count] = idx->hash;
          heights[to_queue_count] = idx->height;
          to_queue_count++;
        }
        block_free(&stored);
      }
      idx = idx->prev;
    }

    /* Reverse the order (slow path collects in descending height order) */
    for (size_t i = 0; i < to_queue_count / 2; i++) {
      hash256_t tmp_hash = to_queue[i];
      uint32_t tmp_height = heights[i];
      to_queue[i] = to_queue[to_queue_count - 1 - i];
      heights[i] = heights[to_queue_count - 1 - i];
      to_queue[to_queue_count - 1 - i] = tmp_hash;
      heights[to_queue_count - 1 - i] = tmp_height;
    }
  }

  if (to_queue_count > 0) {
    log_info(LOG_COMP_SYNC, "Queueing %zu blocks (heights %u-%u)",
             to_queue_count, heights[0], heights[to_queue_count - 1]);

    /* Add to download manager (handles deduplication internally) */
    download_mgr_add_work(mgr->download_mgr, to_queue, heights, to_queue_count);
  }
}

void sync_tick(sync_manager_t *mgr) {
  if (!mgr) {
    return;
  }

  sync_process_timeouts(mgr);

  switch (mgr->mode) {
  case SYNC_MODE_HEADERS: {
    /*
     * Race-to-win header sync: all peers compete, fastest wins.
     *
     * Phase 1 (race): Send getheaders to ALL sync-candidate peers.
     * Phase 2 (winner): First peer to return 2000 headers wins.
     *                   Only use that peer for remaining headers.
     *
     * This combines parallel discovery (find fastest) with single-peer
     * efficiency (no duplicate processing). Much faster than pure parallel
     * which wastes 90%+ bandwidth on duplicates.
     */
    uint64_t now = plat_time_ms();

    /* Check if race winner is still valid */
    if (mgr->headers_race_complete) {
      if (mgr->fastest_header_peer_idx >= mgr->peer_count) {
        /* Winner index is out of bounds (peers removed) - reset race */
        log_info(LOG_COMP_SYNC, "Headers race winner gone (index OOB), restarting race");
        mgr->headers_race_complete = false;
      } else {
        peer_sync_state_t *winner = &mgr->peers[mgr->fastest_header_peer_idx];
        if (!winner->sync_candidate || !peer_is_ready(winner->peer)) {
          /* Winner is no longer usable - reset race */
          log_info(LOG_COMP_SYNC, "Headers race winner disconnected, restarting race");
          mgr->headers_race_complete = false;
        }
      }
    }

    if (!mgr->headers_race_complete) {
      /* RACE MODE: Send to ALL peers to find the fastest */
      if (mgr->headers_race_start_time == 0) {
        mgr->headers_race_start_time = now;
      }

      for (size_t i = 0; i < mgr->peer_count; i++) {
        peer_sync_state_t *ps = &mgr->peers[i];

        if (!ps->sync_candidate || !peer_is_ready(ps->peer)) {
          continue;
        }

        if (!ps->headers_in_flight &&
            now - ps->headers_sent_time >= SYNC_HEADER_RETRY_INTERVAL_MS) {
          ps->headers_in_flight = true;
          ps->headers_sent_time = now;

          if (mgr->callbacks.send_getheaders) {
            hash256_t locator[SYNC_MAX_LOCATOR_HASHES];
            size_t locator_len = 0;

            block_index_t *locator_tip = mgr->best_header;
            if (locator_tip != NULL) {
              block_index_map_t *map =
                  chainstate_get_block_index_map(mgr->chainstate);
              sync_build_locator_from(map, locator_tip, locator, &locator_len);
            } else {
              sync_build_locator(mgr->chainstate, locator, &locator_len);
            }

            mgr->callbacks.send_getheaders(ps->peer, locator, locator_len, NULL,
                                           mgr->callbacks.ctx);
          }
        }
      }
    } else {
      /* WINNER MODE: Only send to the fastest peer */
      peer_sync_state_t *ps = &mgr->peers[mgr->fastest_header_peer_idx];

      if (!ps->headers_in_flight &&
          now - ps->headers_sent_time >= SYNC_HEADER_RETRY_INTERVAL_MS) {
        ps->headers_in_flight = true;
        ps->headers_sent_time = now;

        if (mgr->callbacks.send_getheaders) {
          hash256_t locator[SYNC_MAX_LOCATOR_HASHES];
          size_t locator_len = 0;

          block_index_t *locator_tip = mgr->best_header;
          if (locator_tip != NULL) {
            block_index_map_t *map =
                chainstate_get_block_index_map(mgr->chainstate);
            sync_build_locator_from(map, locator_tip, locator, &locator_len);
          } else {
            sync_build_locator(mgr->chainstate, locator, &locator_len);
          }

          mgr->callbacks.send_getheaders(ps->peer, locator, locator_len, NULL,
                                         mgr->callbacks.ctx);
        }
      }

      /* Note: Probing of non-active pool members is now done in sync_handle_headers
       * when the active peer responds, ensuring probes actually execute. */
    }
    break;
  }

  case SYNC_MODE_BLOCKS: {
    uint64_t now = plat_time_ms();

    /* IBD profiling: Rate-limited progress log every 30 seconds */
    if (now - mgr->last_progress_log_time >= 30000) {
      mgr->last_progress_log_time = now;

      sync_progress_t progress;
      sync_get_progress(mgr, &progress);

      uint64_t elapsed_sec = (now - mgr->start_time) / 1000;
      float blocks_per_sec = elapsed_sec > 0
                                 ? (float)progress.blocks_validated / elapsed_sec
                                 : 0.0f;

      uint64_t eta_sec = sync_estimate_remaining_time(&progress) / 1000;
      uint32_t eta_hours = (uint32_t)(eta_sec / 3600);
      uint32_t eta_mins = (uint32_t)((eta_sec % 3600) / 60);

      log_info(LOG_COMP_SYNC,
               "[IBD] height=%u/%u (%.1f%%) | %.1f blk/s | "
               "pending=%zu inflight=%zu | ETA=%uh%02um | peers=%zu",
               progress.tip_height, progress.best_header_height,
               progress.sync_percentage, blocks_per_sec,
               (size_t)progress.blocks_pending,
               (size_t)progress.blocks_in_flight, eta_hours, eta_mins,
               mgr->peer_count);
    }
    uint32_t our_best_height =
        mgr->best_header ? mgr->best_header->height : 0;

    /*
     * Continue requesting headers from peers that may have more.
     *
     * A peer may have responded with fewer than 2000 headers (causing us to
     * transition to BLOCKS mode) but be behind the network. We should request
     * headers from any peer whose advertised start_height exceeds our
     * best_header height. This handles:
     * - The original peer being behind the network tip
     * - New peers connecting with higher heights
     * - Peers that caught up since initial connection
     */
    for (size_t i = 0; i < mgr->peer_count; i++) {
      peer_sync_state_t *ps = &mgr->peers[i];
      if (ps->sync_candidate && !ps->headers_in_flight &&
          peer_is_ready(ps->peer) &&
          ps->start_height > (int32_t)our_best_height) {
        if (now - ps->headers_sent_time >= SYNC_HEADER_RETRY_INTERVAL_MS) {
          log_info(LOG_COMP_SYNC,
                   "Requesting more headers: our_best=%u, peer_height=%d",
                   our_best_height, ps->start_height);
          ps->headers_in_flight = true;
          ps->headers_sent_time = now;

          /* Build block locator and send getheaders */
          if (mgr->callbacks.send_getheaders) {
            hash256_t locator[SYNC_MAX_LOCATOR_HASHES];
            size_t locator_len = 0;

            if (mgr->best_header != NULL) {
              block_index_map_t *map =
                  chainstate_get_block_index_map(mgr->chainstate);
              sync_build_locator_from(map, mgr->best_header, locator,
                                      &locator_len);
            } else {
              sync_build_locator(mgr->chainstate, locator, &locator_len);
            }

            mgr->callbacks.send_getheaders(ps->peer, locator, locator_len, NULL,
                                           mgr->callbacks.ctx);
          }
        }
      }
    }

    /*
     * Periodic header refresh: Request headers from any available peer
     * every SYNC_HEADER_REFRESH_INTERVAL_MS to catch newly mined blocks.
     * This handles the case where all connected peers had the same height
     * as us when they connected, but the network has since advanced.
     */
    if (now - mgr->last_header_refresh_time >= SYNC_HEADER_REFRESH_INTERVAL_MS) {
      /* Find a sync candidate peer that's not currently fetching headers */
      for (size_t i = 0; i < mgr->peer_count; i++) {
        peer_sync_state_t *ps = &mgr->peers[i];
        if (ps->sync_candidate && !ps->headers_in_flight &&
            peer_is_ready(ps->peer)) {
          log_info(LOG_COMP_SYNC,
                   "Periodic header refresh: requesting from peer (our_best=%u)",
                   our_best_height);
          ps->headers_in_flight = true;
          ps->headers_sent_time = now;
          mgr->last_header_refresh_time = now;

          /* Build block locator and send getheaders */
          if (mgr->callbacks.send_getheaders) {
            hash256_t locator[SYNC_MAX_LOCATOR_HASHES];
            size_t locator_len = 0;

            if (mgr->best_header != NULL) {
              block_index_map_t *map =
                  chainstate_get_block_index_map(mgr->chainstate);
              sync_build_locator_from(map, mgr->best_header, locator,
                                      &locator_len);
            } else {
              sync_build_locator(mgr->chainstate, locator, &locator_len);
            }

            mgr->callbacks.send_getheaders(ps->peer, locator, locator_len, NULL,
                                           mgr->callbacks.ctx);
          }
          break; /* Only request from one peer per tick */
        }
      }
    }

    /* Queue blocks from headers */
    queue_blocks_from_headers(mgr);

    /*
     * Periodically notify chasers about stored blocks that need validation.
     * This handles blocks stored from previous sessions.
     */
    if (now - mgr->last_stored_block_check_time >= 100) {
      mgr->last_stored_block_check_time = now;

      if (mgr->dispatcher != NULL) {
        /* Fire BUMP to trigger chasers to check for work */
        chase_notify_height(mgr->dispatcher, CHASE_BUMP, 0);
      }
    }

    /* Distribute pending work to peers with capacity */
    download_mgr_distribute_work(mgr->download_mgr);

    /* Check for stalls and reassign work from slow/stalled peers */
    download_mgr_check_performance(mgr->download_mgr);
    break;
  }

  case SYNC_MODE_IDLE:
  case SYNC_MODE_DONE:
  case SYNC_MODE_STALLED:
    break;
  }
}

void sync_get_progress(const sync_manager_t *mgr, sync_progress_t *progress) {
  if (!mgr || !progress) {
    return;
  }

  memset(progress, 0, sizeof(sync_progress_t));

  progress->mode = mgr->mode;
  progress->start_time = mgr->start_time;
  progress->last_progress_time = mgr->last_progress_time;

  /* Header progress */
  block_index_map_t *index_map =
      chainstate_get_block_index_map(mgr->chainstate);
  progress->headers_total = (uint32_t)block_index_map_size(index_map);
  progress->headers_validated = progress->headers_total;

  /* Block progress */
  progress->blocks_downloaded = mgr->blocks_received_total;
  progress->blocks_pending =
      (uint32_t)download_mgr_pending_count(mgr->download_mgr);
  progress->blocks_in_flight =
      (uint32_t)download_mgr_inflight_count(mgr->download_mgr);

  /* Network state */
  progress->sync_peers = count_sync_peers(mgr);

  /* Chain info */
  chain_tip_t tip;
  if (chainstate_get_tip(mgr->chainstate, &tip) == ECHO_OK) {
    progress->tip_height = tip.height;
    progress->tip_work = tip.chainwork;
  }

  /* Calculate blocks validated this session (tip - start height) */
  if (progress->tip_height > mgr->block_sync_start_height) {
    progress->blocks_validated = progress->tip_height - mgr->block_sync_start_height;
  } else {
    progress->blocks_validated = 0;
  }

  if (mgr->best_header) {
    progress->best_header_height = mgr->best_header->height;
    progress->best_header_work = mgr->best_header->chainwork;
  }

  /* Calculate percentage */
  if (progress->best_header_height > 0) {
    progress->sync_percentage = (float)progress->tip_height /
                                (float)progress->best_header_height * 100.0f;
  }
}

bool sync_is_complete(const sync_manager_t *mgr) {
  return mgr && mgr->mode == SYNC_MODE_DONE;
}

bool sync_is_ibd(const sync_manager_t *mgr) {
  return mgr && (mgr->mode == SYNC_MODE_HEADERS ||
                 mgr->mode == SYNC_MODE_BLOCKS);
}

/* ============================================================================
 * Helper Functions
 * ============================================================================
 */

const char *sync_mode_string(sync_mode_t mode) {
  switch (mode) {
  case SYNC_MODE_IDLE:
    return "IDLE";
  case SYNC_MODE_HEADERS:
    return "HEADERS";
  case SYNC_MODE_BLOCKS:
    return "BLOCKS";
  case SYNC_MODE_DONE:
    return "DONE";
  case SYNC_MODE_STALLED:
    return "STALLED";
  default:
    return "UNKNOWN";
  }
}

uint64_t sync_estimate_remaining_time(const sync_progress_t *progress) {
  if (!progress || progress->mode == SYNC_MODE_IDLE ||
      progress->mode == SYNC_MODE_DONE) {
    return 0;
  }

  uint64_t elapsed = progress->last_progress_time - progress->start_time;
  if (elapsed == 0 || progress->tip_height == 0) {
    return UINT64_MAX;
  }

  /* Estimate based on blocks validated per unit time */
  float blocks_per_ms = (float)progress->blocks_validated / (float)elapsed;
  if (blocks_per_ms <= 0) {
    return UINT64_MAX;
  }

  uint32_t remaining = 0;
  if (progress->best_header_height > progress->tip_height) {
    remaining = progress->best_header_height - progress->tip_height;
  }

  return (uint64_t)((float)remaining / blocks_per_ms);
}

void sync_get_metrics(sync_manager_t *mgr, sync_metrics_t *metrics) {
  if (!metrics) {
    return;
  }

  /* Initialize with defaults */
  metrics->blocks_per_second = 0.0f;
  metrics->eta_seconds = 0;
  metrics->network_median_latency = 0;
  metrics->active_sync_peers = 0;
  metrics->mode_string = "idle";

  if (!mgr) {
    return;
  }

  /* Get current progress for calculations */
  sync_progress_t progress;
  sync_get_progress(mgr, &progress);

  /* Mode string */
  metrics->mode_string = sync_mode_string(progress.mode);

  /* Calculate blocks per second from overall sync progress */
  metrics->blocks_per_second = calc_blocks_per_second(mgr);

  /* ETA in seconds */
  if (metrics->blocks_per_second > 0 && progress.best_header_height > progress.tip_height) {
    uint32_t remaining = progress.best_header_height - progress.tip_height;
    metrics->eta_seconds = (uint64_t)(remaining / metrics->blocks_per_second);
  }

  /* Network median latency from peer quality system */
  metrics->network_median_latency = mgr->network_median_latency_ms;

  /* Count active sync peers (those with blocks received) */
  uint32_t active = 0;
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].blocks_received > 0) {
      active++;
    }
  }
  metrics->active_sync_peers = active;
}
