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
 * Maximum sync peers to track.
 * During IBD we only have outbound peers, so use outbound limit.
 */
#define SYNC_MAX_PEERS ECHO_MAX_OUTBOUND_PEERS

/**
 * Simple header sync: one peer at a time, probe occasionally for faster peers.
 *
 * HEADER_PROBE_INTERVAL: Every N successful batches, try one random peer.
 * HEADER_SLOW_THRESHOLD_MS: If active peer takes longer than this, try another.
 */
#define HEADER_PROBE_INTERVAL 3       /* Probe a random peer every 3 batches (6k headers) */
#define HEADER_SLOW_THRESHOLD_MS 2000 /* Consider peer slow if batch takes >2s */

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
  uint64_t last_performance_check_time; /* For statistical deviation checks */
  uint64_t last_slowest_eviction_time;  /* For periodic slowest 10% eviction */

  /* Stats */
  uint32_t headers_received_total;
  uint32_t blocks_received_total;
  uint32_t blocks_validated_total;

  /* Best known header chain */
  block_index_t *best_header;

  /* Adaptive stalling timeout (in ms) - starts at 2s, grows on stalls */
  uint64_t stalling_timeout_ms;

  /* Parallel request rate limiting */
  hash256_t last_parallel_request_hash;
  uint64_t last_parallel_request_time;

  /* Last time we checked for stored blocks to validate */
  uint64_t last_stored_block_check_time;

  /* IBD profiling: Rate-limited progress logging (every 30s) */
  uint64_t last_progress_log_time;

  /* Network latency baseline (kept for API compatibility, currently always 0) */
  uint64_t network_median_latency_ms;

  /* Simple header sync: one peer at a time, probe occasionally for faster peers */
  bool have_header_peer;            /* True once we have an active header peer */
  size_t active_header_peer_idx;    /* Index of current header peer in peers[] */
  uint32_t header_batch_count;      /* Batches received from active peer */
  uint64_t last_header_response_ms; /* Response time of last batch (for performance) */
  size_t probe_peer_idx;            /* Peer being probed (SIZE_MAX if none) */
  uint64_t probe_sent_time;         /* When probe was sent */

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

  /* Reusable work queue arrays (allocated once, reused forever).
   * Sized to download_mgr's full capacity (DOWNLOAD_MAX_BATCHES * DOWNLOAD_BATCH_SIZE).
   * Eliminates 288 KB stack allocation on every queue_blocks_from_headers() call. */
  hash256_t *queue_work_hashes;   /* Block hashes to queue */
  uint32_t *queue_work_heights;   /* Corresponding heights */
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
 * Cooperative model: Idle peers request work when available.
 * Starved peers wait - we never steal work from active peers.
 */
static bool sync_chase_handler(chase_event_t event, chase_value_t value,
                               void *context) {
  (void)value;

  sync_manager_t *mgr = (sync_manager_t *)context;
  if (!mgr) {
    return false;
  }

  switch (event) {
  case CHASE_STARVED: {
    /* A downstream chaser needs more blocks.
     * Find any idle peer and have them request work. If no work is
     * available, they wait - we never steal from active peers. */
    for (size_t i = 0; i < mgr->peer_count; i++) {
      peer_sync_state_t *ps = &mgr->peers[i];
      if (ps->sync_candidate && peer_is_ready(ps->peer)) {
        if (download_mgr_peer_is_idle(mgr->download_mgr, ps->peer)) {
          bool got_work = download_mgr_peer_request_work(mgr->download_mgr, ps->peer);
          log_debug(LOG_COMP_SYNC, "CHASE_STARVED: peer %s requested work (got=%s)",
                    ps->peer->address, got_work ? "yes" : "no");
          break; /* One peer at a time */
        }
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
 * Calculate blocks per second using lifetime average.
 * Simple: validated_blocks / uptime_seconds.
 * Gives smooth, predictable ETA that matches user intuition.
 */
static float calc_blocks_per_second(struct sync_manager *mgr) {
  if (mgr->block_sync_start_time == 0) {
    return 0.0f;
  }

  uint64_t now = plat_time_ms();
  uint64_t elapsed_ms = now - mgr->block_sync_start_time;
  if (elapsed_ms == 0) {
    return 0.0f;
  }

  uint32_t current_height = chainstate_get_height(mgr->chainstate);
  uint32_t validated_this_session = 0;
  if (current_height > mgr->block_sync_start_height) {
    validated_this_session = current_height - mgr->block_sync_start_height;
  }

  return (float)validated_this_session / ((float)elapsed_ms / 1000.0f);
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
                            chase_dispatcher_t *dispatcher) {
  if (!chainstate || !callbacks) {
    return NULL;
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
  mgr->download_mgr = download_mgr_create(&dm_callbacks);
  if (!mgr->download_mgr) {
    free(mgr);
    return NULL;
  }

  mgr->chainstate = chainstate;
  mgr->callbacks = *callbacks;
  mgr->dispatcher = dispatcher;
  mgr->subscription = NULL;
  mgr->mode = SYNC_MODE_IDLE;

  /* Allocate reusable work queue arrays (eliminates per-call stack allocation) */
  size_t queue_capacity = (size_t)DOWNLOAD_MAX_BATCHES * DOWNLOAD_BATCH_SIZE;
  mgr->queue_work_hashes = malloc(queue_capacity * sizeof(hash256_t));
  mgr->queue_work_heights = malloc(queue_capacity * sizeof(uint32_t));
  if (!mgr->queue_work_hashes || !mgr->queue_work_heights) {
    free(mgr->queue_work_hashes);
    free(mgr->queue_work_heights);
    download_mgr_destroy(mgr->download_mgr);
    free(mgr);
    return NULL;
  }

  /* Subscribe to chase events for download coordination */
  if (dispatcher != NULL) {
    mgr->subscription =
        chase_subscribe(dispatcher, sync_chase_handler, mgr);
    if (mgr->subscription != NULL) {
      log_debug(LOG_COMP_SYNC, "Subscribed to chase events");
    }
  }
  mgr->peer_count = 0;

  /* Initialize adaptive stalling timeout to 2 seconds */
  mgr->stalling_timeout_ms = SYNC_BLOCK_STALLING_TIMEOUT_MS;

  /* Initialize pending headers queue for deferred persistence */
  mgr->pending_headers = NULL;
  mgr->pending_headers_count = 0;
  mgr->pending_headers_capacity = 0;

  /* Initialize simple header sync state */
  mgr->have_header_peer = false;
  mgr->active_header_peer_idx = SIZE_MAX;
  mgr->header_batch_count = 0;
  mgr->last_header_response_ms = 0;
  mgr->probe_peer_idx = SIZE_MAX;
  mgr->probe_sent_time = 0;

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
  free(mgr->queue_work_hashes);
  free(mgr->queue_work_heights);
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

    /* PULL model: Immediately give work to new peer if in BLOCKS mode.
     * Don't make them wait for sync_tick - put them to work now! */
    if (mgr->mode == SYNC_MODE_BLOCKS && peer_is_ready(peer)) {
      bool got_work = download_mgr_peer_request_work(mgr->download_mgr, peer);
      if (got_work) {
        log_debug(LOG_COMP_SYNC, "Immediately assigned work to new peer %s",
                  peer->address);
      }
    }
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

  /* Simple header sync: one peer at a time, probe occasionally for faster peers.
   *
   * Strategy:
   * 1. First peer to respond with 2000 headers becomes active peer
   * 2. Track response time for each batch
   * 3. Every HEADER_PROBE_INTERVAL batches, send probe to ONE random peer
   * 4. BOTH responses are useful (headers processed either way)
   * 5. Whichever responds faster becomes/stays active (race to win)
   *
   * This is efficient: probes are never "wasted" since both responses
   * contain headers we need. We just naturally select the faster peer.
   */
  uint64_t now = plat_time_ms();
  size_t peer_idx = (size_t)(ps - mgr->peers);

  if (ps->headers_sent_time > 0 && count == SYNC_MAX_HEADERS_PER_REQUEST) {
    uint64_t response_ms = now - ps->headers_sent_time;

    /* Is there a race in progress? (probe was sent, waiting for responses) */
    bool is_probe_peer = (mgr->probe_peer_idx == peer_idx);
    bool is_active_peer = (mgr->have_header_peer && peer_idx == mgr->active_header_peer_idx);
    bool race_in_progress = (mgr->probe_sent_time > 0);

    if (!mgr->have_header_peer) {
      /* First peer to respond with a full batch becomes active */
      log_info(LOG_COMP_SYNC, "Header peer selected: %s (%lums response)",
               ps->peer->address, (unsigned long)response_ms);

      mgr->have_header_peer = true;
      mgr->active_header_peer_idx = peer_idx;
      mgr->last_header_response_ms = response_ms;
      mgr->header_batch_count = 1;
      mgr->probe_peer_idx = SIZE_MAX;
      mgr->probe_sent_time = 0;
    } else if (race_in_progress && (is_probe_peer || is_active_peer)) {
      /* Race in progress - first responder wins! */
      uint64_t race_time = now - mgr->probe_sent_time;

      if (is_probe_peer) {
        /* Probe peer responded first - they win! */
        peer_sync_state_t *old_active = &mgr->peers[mgr->active_header_peer_idx];
        log_info(LOG_COMP_SYNC,
                 "Probe won race (%lums): switching %s -> %s",
                 (unsigned long)race_time,
                 old_active->peer->address, ps->peer->address);

        mgr->active_header_peer_idx = peer_idx;
        mgr->last_header_response_ms = response_ms;
        mgr->header_batch_count = 1;
      } else {
        /* Active peer responded first - they keep their spot */
        log_debug(LOG_COMP_SYNC, "Active peer won race (%lums)",
                  (unsigned long)race_time);
        mgr->last_header_response_ms = response_ms;
        mgr->header_batch_count++;
      }

      /* Race complete */
      mgr->probe_peer_idx = SIZE_MAX;
      mgr->probe_sent_time = 0;
    } else if (is_active_peer) {
      /* Normal response from active peer (no race) */
      mgr->last_header_response_ms = response_ms;
      mgr->header_batch_count++;
      /* Race starting is handled in immediate follow-up section below,
       * so both requests go out at the same time */
    }
    /* Note: responses from non-active, non-probe peers are ignored for
     * performance tracking but their headers are still processed below. */
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
        /* Flush all queued headers to database before switching modes.
         * Bug fixed 2025-01-01: This path was missing the flush call,
         * causing block downloads to fail with "hash lookup failed". */
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
   * This dramatically improves header sync throughput.
   *
   * PARALLEL RACING: Every HEADER_PROBE_INTERVAL batches, we send requests to BOTH
   * the active peer AND a random challenger at the same time. Whichever responds
   * first becomes/stays active. Both responses' headers are used - no waste!
   */
  if (mgr->mode == SYNC_MODE_HEADERS && count == SYNC_MAX_HEADERS_PER_REQUEST &&
      mgr->have_header_peer && peer_idx == mgr->active_header_peer_idx &&
      ps->sync_candidate && peer_is_ready(ps->peer) && mgr->callbacks.send_getheaders) {

    /* Build locator once - used for both active and challenger */
    hash256_t locator[SYNC_MAX_LOCATOR_HASHES];
    size_t locator_len = 0;
    block_index_t *locator_tip = mgr->best_header;
    if (locator_tip != NULL) {
      block_index_map_t *map = chainstate_get_block_index_map(mgr->chainstate);
      sync_build_locator_from(map, locator_tip, locator, &locator_len);
    } else {
      sync_build_locator(mgr->chainstate, locator, &locator_len);
    }

    uint64_t send_time = plat_time_ms();

    /* Always send to active peer */
    ps->headers_in_flight = true;
    ps->headers_sent_time = send_time;
    mgr->callbacks.send_getheaders(ps->peer, locator, locator_len, NULL,
                                   mgr->callbacks.ctx);

    /* Every N batches, OR if active peer is slow, send to a random challenger */
    bool slow_response = mgr->last_header_response_ms > HEADER_SLOW_THRESHOLD_MS;
    bool probe_interval = (mgr->header_batch_count % HEADER_PROBE_INTERVAL == 0);
    if (probe_interval || slow_response) {
      /* Find available challenger peers */
      size_t candidates[SYNC_MAX_PEERS];
      size_t candidate_count = 0;

      for (size_t i = 0; i < mgr->peer_count && candidate_count < SYNC_MAX_PEERS; i++) {
        if (i == mgr->active_header_peer_idx) continue;
        peer_sync_state_t *candidate = &mgr->peers[i];
        if (candidate->sync_candidate && peer_is_ready(candidate->peer) &&
            !candidate->headers_in_flight) {
          candidates[candidate_count++] = i;
        }
      }

      if (candidate_count > 0) {
        /* Pick random challenger */
        size_t pick = (size_t)(send_time % candidate_count);
        size_t challenger_idx = candidates[pick];
        peer_sync_state_t *challenger = &mgr->peers[challenger_idx];

        /* Send to challenger IN PARALLEL with active peer */
        mgr->probe_peer_idx = challenger_idx;
        mgr->probe_sent_time = send_time;
        challenger->headers_in_flight = true;
        challenger->headers_sent_time = send_time;
        mgr->callbacks.send_getheaders(challenger->peer, locator, locator_len,
                                       NULL, mgr->callbacks.ctx);

        log_info(LOG_COMP_SYNC, "Header race started: %s vs %s (batch %u%s)",
                 ps->peer->address, challenger->peer->address,
                 mgr->header_batch_count,
                 slow_response ? ", slow response triggered" : "");
      }
    }
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

  /* Pre-check: is block already stored? (for duplicate detection) */
  block_index_map_t *index_map =
      chainstate_get_block_index_map(mgr->chainstate);
  block_index_t *block_index = block_index_map_lookup(index_map, &block_hash);
  bool was_already_stored = (block_index && block_index->data_file != BLOCK_DATA_NOT_STORED);

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

  /* Re-fetch block index after store_block (may have been created/updated) */
  block_index = block_index_map_lookup(index_map, &block_hash);

  /* Only mark block complete if it wasn't already stored (prevents infinite loop
   * when sticky batches send duplicates). Duplicates are already complete. */
  if (block_index && !was_already_stored) {
    download_mgr_block_complete(mgr->download_mgr, &block_hash,
                                block_index->height);

    /* NOTE: CHASE_CHECKED is fired by the storage thread after async write
     * completes, not here. This ensures the block is on disk before we try
     * to validate it. */

    /* Check if peer is now idle (batch complete) and have them request more
     * work immediately. If no work available, they wait. */
    if (download_mgr_peer_is_idle(mgr->download_mgr, peer)) {
      download_mgr_peer_request_work(mgr->download_mgr, peer);
    }
  }

  /* Only count unique blocks (not duplicates from sticky batches) */
  if (!was_already_stored) {
    mgr->blocks_received_total++;
    ps->blocks_received++;
  }
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

    /*
     * IBD peer eviction: disconnect non-sync-candidates to free slots
     *
     * During IBD, every peer slot is valuable. Peers that can't help with
     * sync (pruned nodes missing blocks we need, nodes behind our tip) take
     * slots that could be used by peers who CAN help.
     *
     * We give peers a 30-second grace period after connection for:
     * - Address relay (they may share good peer addresses)
     * - State to settle (version exchange, sync_candidate determination)
     *
     * After grace period, evict unconditionally. No dead weight during IBD.
     */
    if (ps->peer && !ps->sync_candidate &&
        (mgr->mode == SYNC_MODE_HEADERS || mgr->mode == SYNC_MODE_BLOCKS)) {
      uint64_t connected_ms = now - ps->peer->connect_time;

      /* 30 second grace period, then evict */
      if (connected_ms >= 30000) {
        log_info(LOG_COMP_SYNC,
                 "Evicting non-sync peer after %llus",
                 (unsigned long long)(connected_ms / 1000));
        peer_disconnect(ps->peer, PEER_DISCONNECT_RESOURCE_LIMIT,
                        "Non-sync peer evicted during IBD");
        continue;
      }
    }
  }
}

/**
 * Queue blocks for download from headers.
 *
 * Optimized with batch database query and increased rate limit:
 * - Single SQL query fetches up to 4096 block hashes at once (instead of 4096 individual queries)
 * - No per-block storage check (rely on periodic CHASE_BUMP to handle stored blocks)
 * - Increased rate limit to 4096 blocks per tick (still spreads load across ticks for very large gaps)
 *
 * Result: Near-instant queuing with no micro-hangs, even for large height gaps.
 */
static void queue_blocks_from_headers(sync_manager_t *mgr) {
  if (!mgr->best_header) {
    log_debug(LOG_COMP_SYNC, "queue_blocks: no best_header");
    return;
  }

  uint32_t tip_height = chainstate_get_height(mgr->chainstate);

  /* Calculate target range: from validated tip to best header */
  uint32_t start_height = tip_height + 1;
  uint32_t end_height = mgr->best_header->height;

  log_info(LOG_COMP_SYNC,
           "queue_blocks: tip=%u, start=%u, end=%u, best=%u",
           tip_height, start_height, end_height, mgr->best_header->height);

  if (start_height > end_height) {
    /* Already fully synced */
    log_info(LOG_COMP_SYNC, "queue_blocks: already synced (start > end)");
    return;
  }

  /*
   * Use reusable arrays from sync_manager (allocated once at creation).
   * Batch query with composite index (status, height) makes this fast - single SQL query.
   * No rate limit needed - composite index keeps queries fast even for large gaps.
   */
  #define QUEUE_BATCH_SIZE ((size_t)DOWNLOAD_MAX_BATCHES * DOWNLOAD_BATCH_SIZE)
  hash256_t *to_queue = mgr->queue_work_hashes;
  uint32_t *heights = mgr->queue_work_heights;
  size_t to_queue_count = 0;
  size_t batch_limit = QUEUE_BATCH_SIZE;  /* Query full range (up to download queue capacity) */

  /* Calculate query range (limited only by array capacity) */
  uint32_t query_end = start_height + (uint32_t)batch_limit - 1;
  if (query_end > end_height) {
    query_end = end_height;
  }

  if (mgr->callbacks.get_block_hashes_in_range) {
    /* Optimized path: single batch query for entire range */
    size_t fetched_count = 0;
    echo_result_t result = mgr->callbacks.get_block_hashes_in_range(
        start_height, query_end, to_queue, heights, batch_limit,
        &fetched_count, mgr->callbacks.ctx);

    if (result != ECHO_OK) {
      log_warn(LOG_COMP_SYNC,
               "queue_blocks: batch query failed (heights %u-%u): %d",
               start_height, query_end, result);
      return;
    }

    /* Filter out blocks already in download queue */
    for (size_t i = 0; i < fetched_count; i++) {
      if (!download_mgr_has_block(mgr->download_mgr, &to_queue[i])) {
        /* Keep this block - move it to to_queue_count position if needed */
        if (i != to_queue_count) {
          to_queue[to_queue_count] = to_queue[i];
          heights[to_queue_count] = heights[i];
        }
        to_queue_count++;
      }
    }

    log_info(LOG_COMP_SYNC,
             "queue_blocks: batch query fetched=%zu, queuing=%zu (heights %u-%u)",
             fetched_count, to_queue_count, start_height, query_end);
  } else if (mgr->callbacks.get_block_hash_at_height) {
    /* Fallback: per-height queries (old slow path, kept for compatibility) */
    uint32_t lookup_failures = 0;
    for (uint32_t h = start_height; h <= query_end && to_queue_count < batch_limit; h++) {
      hash256_t hash;
      echo_result_t cb_result = mgr->callbacks.get_block_hash_at_height(
          h, &hash, mgr->callbacks.ctx);
      if (cb_result != ECHO_OK) {
        lookup_failures++;
        if (lookup_failures <= 3) {
          log_warn(LOG_COMP_SYNC,
                   "queue_blocks: height %u hash lookup failed: %d",
                   h, cb_result);
        }
        continue;
      }

      if (!download_mgr_has_block(mgr->download_mgr, &hash)) {
        to_queue[to_queue_count] = hash;
        heights[to_queue_count] = h;
        to_queue_count++;
      }
    }
    log_info(LOG_COMP_SYNC,
             "queue_blocks: fallback path queued=%zu, failures=%u",
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
           to_queue_count < batch_limit) {
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
     * Simple header sync: one peer at a time, probe occasionally.
     *
     * - First peer to respond with 2000 headers becomes active
     * - Only request from active peer (no parallel requests = no duplication)
     * - Every HEADER_PROBE_INTERVAL batches, probe one random peer
     * - If probe is faster, switch to them
     */
    uint64_t now = plat_time_ms();

    /* Check if active header peer is still valid */
    if (mgr->have_header_peer) {
      if (mgr->active_header_peer_idx >= mgr->peer_count) {
        /* Active peer index is out of bounds (peers removed) - reset */
        log_info(LOG_COMP_SYNC, "Header peer gone (index OOB), finding new peer");
        mgr->have_header_peer = false;
      } else {
        peer_sync_state_t *active = &mgr->peers[mgr->active_header_peer_idx];
        if (!active->sync_candidate || !peer_is_ready(active->peer)) {
          /* Active peer is no longer usable - reset */
          log_info(LOG_COMP_SYNC, "Header peer disconnected, finding new peer");
          mgr->have_header_peer = false;
        }
      }
    }

    if (!mgr->have_header_peer) {
      /* No active peer yet - send to a few peers to find the fastest.
       * Keep it small (4 peers max) to avoid flooding. First to respond wins. */
      size_t discovery_limit = mgr->peer_count < 4 ? mgr->peer_count : 4;
      size_t sent = 0;

      for (size_t i = 0; i < mgr->peer_count && sent < discovery_limit; i++) {
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
            sent++;
          }
        }
      }
    } else {
      /* Have active peer - only send to them (no duplication!) */
      peer_sync_state_t *ps = &mgr->peers[mgr->active_header_peer_idx];

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

      /* Probing is handled in sync_handle_headers when active peer responds */
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

    /* Statistical deviation check: every DOWNLOAD_PERF_WINDOW_MS (10 seconds).
     * Disconnects stalled peers (0 B/s) and slow peers (> 1.5 stddev below mean).
     * This cooperative model rewards fast peers with more work. */
    if (now - mgr->last_performance_check_time >= DOWNLOAD_PERF_WINDOW_MS) {
      mgr->last_performance_check_time = now;
      size_t dropped = download_mgr_check_performance(mgr->download_mgr);
      if (dropped > 0) {
        log_info(LOG_COMP_SYNC,
                 "Performance check dropped %zu slow/stalled peers", dropped);
      }
    }

    /* Aggressive percentile eviction: every 30 seconds, kick bottom 10%.
     * This ensures we're constantly cycling through peers to find faster ones.
     * Complements the absolute threshold check above with relative ranking.
     *
     * min_rate_to_keep: Peers above this rate are never evicted (they're "fast
     * enough"). Set to 0.0 to disable and evict purely by percentile. We'll
     * tune this threshold based on IBD testing - monitoring peer rate
     * distributions will reveal the right cutoff. */
#define SLOWEST_EVICTION_INTERVAL_MS 30000
#define SLOWEST_EVICTION_MIN_RATE 0.0f  /* TODO: tune based on IBD monitoring */
    if (now - mgr->last_slowest_eviction_time >= SLOWEST_EVICTION_INTERVAL_MS) {
      mgr->last_slowest_eviction_time = now;
      size_t evicted = download_mgr_evict_slowest_percent(
          mgr->download_mgr, 10.0f, SLOWEST_EVICTION_MIN_RATE);
      if (evicted > 0) {
        log_info(LOG_COMP_SYNC,
                 "Percentile eviction kicked %zu slowest peers", evicted);
      }
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

    /* Queue blocks from headers - but only if queue has capacity.
     * Flow control: don't queue more work until current work is being
     * consumed. This prevents excessive memory usage and reduces per-tick
     * overhead from repeated storage lookups. */
    size_t pending = download_mgr_pending_count(mgr->download_mgr);
    size_t inflight = download_mgr_inflight_count(mgr->download_mgr);
    size_t total_queued = pending + inflight;
    size_t queue_capacity = (size_t)DOWNLOAD_MAX_BATCHES * DOWNLOAD_BATCH_SIZE;
    if (total_queued < queue_capacity / 2) {
      /* Below 50% capacity - queue more work */
      queue_blocks_from_headers(mgr);
    }

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

    /* Stall detection: Check if validation is stuck waiting for a slow peer.
     * If so, steal their batch and give another peer a chance. */
    {
      uint32_t validated_height = chainstate_get_height(mgr->chainstate);
      if (download_mgr_check_stall(mgr->download_mgr, validated_height)) {
        /* Batch was stolen - bump chasers to retry validation */
        if (mgr->dispatcher != NULL) {
          chase_notify_height(mgr->dispatcher, CHASE_BUMP, 0);
        }
      }
    }

    /* PULL model: Have idle peers request work.
     * This replaces the old distribute_work() PUSH approach.
     * Peers pull work when their batch is complete (idle).
     */
    for (size_t i = 0; i < mgr->peer_count; i++) {
      peer_sync_state_t *ps = &mgr->peers[i];
      if (!ps->sync_candidate || !peer_is_ready(ps->peer)) {
        continue;
      }

      if (download_mgr_peer_is_idle(mgr->download_mgr, ps->peer)) {
        download_mgr_peer_request_work(mgr->download_mgr, ps->peer);
      }
    }

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
  metrics->download_rate = 0.0f;
  metrics->validation_rate = 0.0f;
  metrics->pending_validation = 0;
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

  /* VALIDATION RATE: Blocks added to chain per second (strict order)
   * This is the rate we can advance the chainstate tip. */
  metrics->validation_rate = calc_blocks_per_second(mgr);

  /* DOWNLOAD RATE: Blocks received from network per second (any order)
   * This shows how fast we're getting data, regardless of ordering. */
  if (mgr->block_sync_start_time > 0) {
    uint64_t now = plat_time_ms();
    uint64_t elapsed_ms = now - mgr->block_sync_start_time;
    if (elapsed_ms > 0) {
      metrics->download_rate =
          (float)mgr->blocks_received_total * 1000.0f / (float)elapsed_ms;
    }
  }

  /* PENDING VALIDATION: Downloaded blocks waiting for their turn.
   * This is the gap between download and validation.
   * High value = head-of-line blocking (we have blocks but can't use them).
   * This is blocks_received_total minus blocks_validated. */
  uint32_t validated_height = chainstate_get_height(mgr->chainstate);
  uint32_t validated_this_session = 0;
  if (validated_height > mgr->block_sync_start_height) {
    validated_this_session = validated_height - mgr->block_sync_start_height;
  }
  if (mgr->blocks_received_total > validated_this_session) {
    metrics->pending_validation =
        mgr->blocks_received_total - validated_this_session;
  }

  /* ETA in seconds (based on validation rate - the true sync bottleneck).
   * Downloads may race ahead, but validation determines actual sync progress. */
  if (metrics->validation_rate > 0 &&
      progress.best_header_height > progress.tip_height) {
    uint32_t remaining = progress.best_header_height - progress.tip_height;
    metrics->eta_seconds = (uint64_t)(remaining / metrics->validation_rate);
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
