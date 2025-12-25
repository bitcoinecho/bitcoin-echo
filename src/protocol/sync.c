/**
 * Bitcoin Echo — Headers-First Initial Block Download Implementation
 *
 * This module implements headers-first sync as specified in whitepaper §7.3.
 *
 * CRITICAL ZONE REDUNDANCY
 * ========================
 * Block download optimization using redundant requests for critical blocks:
 *
 * 1. CRITICAL ZONE: The next N blocks from validation tip are precious.
 *    Request each from multiple top peers simultaneously.
 *    First response wins, duplicates are discarded.
 *
 * 2. SPECULATIVE ZONE: Blocks beyond critical zone are requested normally.
 *    These fill while we wait for critical blocks.
 *
 * 3. ANTICIPATORY FILLING: Don't wait for stalls - be proactive.
 *    Request critical blocks BEFORE they become blocking.
 *
 * Build once. Build right. Stop.
 */

#include "sync.h"
#include "block.h"
#include "chainstate.h"
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
 * Block queue entry
 */
typedef struct {
  hash256_t hash;
  uint32_t height;
  peer_t *assigned_peer;
  uint64_t request_time;
  bool in_flight;
  uint32_t retry_count;
  bool valid; /* Entry is in use */
} block_queue_entry_t;

/**
 * Block download queue implementation
 */
struct block_queue {
  block_queue_entry_t *entries;
  size_t capacity;
  size_t count;
  size_t pending_count;
  size_t inflight_count;
};

/**
 * Maximum sync peers to track
 */
#define SYNC_MAX_PEERS 128

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

  /* Block download queue */
  block_queue_t *block_queue;

  /* Sync timing */
  uint64_t start_time;
  uint64_t block_sync_start_time; /* When block validation started (0 = not yet) */
  uint64_t last_progress_time;
  uint64_t last_header_refresh_time; /* For periodic header refresh in blocks mode */

  /* Stats */
  uint32_t headers_received_total;
  uint32_t blocks_received_total;
  uint32_t blocks_validated_total;

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

  /* Headers-first sync: single peer for header download (avoids 8x redundant requests) */
  size_t headers_sync_peer_idx;        /* Index into peers[] array */
  bool has_headers_sync_peer;          /* Whether we have a designated peer */

  /*
   * PEER QUALITY ASSESSMENT: Quick initial selection, then continuous rotation.
   *
   * Before IBD begins:
   * 1. Connect to peers (AUDITION_PEER_COUNT)
   * 2. Send pings to all, measure RTT
   * 3. Cull slow peers, keep top performers
   * 4. Start IBD - continuous rotation finds hidden gems
   */
#define PING_CONTEST_MIN_PEERS 24      /* Wait for this many peers before assessment */
#define PING_CONTEST_WAIT_MS 15000     /* 15s to gather more peers after min */
#define PING_CONTEST_TIMEOUT_MS 5000   /* 5s for pong responses */
  uint64_t ping_contest_trigger_time;  /* When we hit min peers (0 = not triggered) */
  uint64_t ping_contest_start_time;    /* When pings were sent (0 = not started) */
  size_t ping_contest_responses;       /* How many pongs received */
  size_t ping_contest_sent;            /* How many pings sent */
  bool skip_ping_contest;              /* Skip ping contest (for testing) */
  bool audition_complete;              /* True after culling is done */

  /* Rolling window for accurate block rate calculation (500 blocks = ~30-60s at typical rates) */
#define RATE_WINDOW_SIZE 500
  uint64_t rate_window[RATE_WINDOW_SIZE]; /* Ring buffer of validation timestamps */
  size_t rate_window_idx;                 /* Next write position */
  size_t rate_window_count;               /* Number of entries (0 to RATE_WINDOW_SIZE) */

  /*
   * Rolling window for block sizes - enables adaptive download window.
   * Like Bitcoin's difficulty adjustment, we measure actual conditions
   * rather than assuming based on height. Target a memory budget and
   * continuously adapt the download window as blocks grow/shrink.
   */
#define SIZE_WINDOW_SIZE 100              /* 100-block rolling average - responsive but smooth */
  uint32_t size_window[SIZE_WINDOW_SIZE]; /* Ring buffer of block sizes (bytes) */
  size_t size_window_idx;                 /* Next write position */
  size_t size_window_count;               /* Number of entries (0 to SIZE_WINDOW_SIZE) */
  uint64_t size_window_total;             /* Running sum for O(1) average calculation */

  /*
   * Bottleneck diagnostics: Track whether we're CPU-bound or network-bound.
   * When we finish validating a block, was the next one already available?
   */
  uint32_t blocks_ready;          /* Next block was immediately available */
  uint32_t blocks_starved;        /* Had to wait for next block */
  uint64_t total_validation_ms;   /* Cumulative time spent in validation callbacks */
  uint64_t total_starvation_ms;   /* Cumulative time spent waiting for blocks */
  uint64_t starvation_start_time; /* When we started waiting (0 = not waiting) */
  uint32_t last_validated_height; /* Height of last validated block */

  /* Deferred header persistence: queue headers during SYNC_MODE_HEADERS,
   * flush all at once when transitioning to SYNC_MODE_BLOCKS. */
  pending_header_t *pending_headers;      /* Dynamic array of pending headers */
  size_t pending_headers_count;           /* Number of queued headers */
  size_t pending_headers_capacity;        /* Allocated capacity */
};

/* ============================================================================
 * Rolling Rate Calculation Helpers
 * ============================================================================
 */

/**
 * Record a block validation timestamp in the rolling window.
 * Call this each time a block is validated.
 */
static void rate_window_record(struct sync_manager *mgr, uint64_t timestamp) {
  mgr->rate_window[mgr->rate_window_idx] = timestamp;
  mgr->rate_window_idx = (mgr->rate_window_idx + 1) % RATE_WINDOW_SIZE;
  if (mgr->rate_window_count < RATE_WINDOW_SIZE) {
    mgr->rate_window_count++;
  }
}

/**
 * Calculate blocks per second from the rolling window.
 * Returns 0.0 if insufficient data.
 */
static float rate_window_get_rate(const struct sync_manager *mgr) {
  if (mgr->rate_window_count < 2) {
    return 0.0f;
  }

  /* Find oldest and newest timestamps in the window */
  size_t oldest_idx;
  if (mgr->rate_window_count < RATE_WINDOW_SIZE) {
    oldest_idx = 0;
  } else {
    oldest_idx = mgr->rate_window_idx; /* Oldest is at current write position */
  }
  size_t newest_idx =
      (mgr->rate_window_idx + RATE_WINDOW_SIZE - 1) % RATE_WINDOW_SIZE;

  uint64_t oldest_ts = mgr->rate_window[oldest_idx];
  uint64_t newest_ts = mgr->rate_window[newest_idx];

  if (newest_ts <= oldest_ts) {
    return 0.0f;
  }

  uint64_t elapsed_ms = newest_ts - oldest_ts;
  if (elapsed_ms == 0) {
    return 0.0f;
  }

  /* Rate = (count - 1) blocks in elapsed_ms milliseconds */
  /* We have count timestamps, representing count-1 intervals */
  float blocks = (float)(mgr->rate_window_count - 1);
  float seconds = (float)elapsed_ms / 1000.0f;

  return blocks / seconds;
}

/* ============================================================================
 * Adaptive Window Sizing (Block Size Rolling Average)
 *
 * Like Bitcoin's difficulty adjustment, we measure actual block sizes rather
 * than assuming based on height. This enables continuous, smooth adaptation
 * of the download window to maintain a target memory budget.
 * ============================================================================
 */

/**
 * Target memory budget for queued blocks (64 MB).
 * This is the maximum data we want in-flight/pending at any time.
 */
#define ADAPTIVE_WINDOW_MEMORY_BUDGET_BYTES (64 * 1024 * 1024)

/**
 * Minimum and maximum window bounds.
 * Even with huge blocks, we want some pipeline depth.
 * Even with tiny blocks, cap to avoid memory explosion.
 */
#define ADAPTIVE_WINDOW_MIN 64
#define ADAPTIVE_WINDOW_MAX 2048

/**
 * Default block size assumption when we have no measurements yet.
 * ~250KB is a reasonable modern block estimate.
 */
#define ADAPTIVE_DEFAULT_BLOCK_SIZE (250 * 1024)

/**
 * Record a block size in the rolling window.
 * Uses O(1) running sum for efficient average calculation.
 */
static void size_window_record(struct sync_manager *mgr, uint32_t block_size) {
  /* Subtract old value from running sum if window is full */
  if (mgr->size_window_count == SIZE_WINDOW_SIZE) {
    mgr->size_window_total -= mgr->size_window[mgr->size_window_idx];
  }

  /* Add new value */
  mgr->size_window[mgr->size_window_idx] = block_size;
  mgr->size_window_total += block_size;

  /* Advance index and count */
  mgr->size_window_idx = (mgr->size_window_idx + 1) % SIZE_WINDOW_SIZE;
  if (mgr->size_window_count < SIZE_WINDOW_SIZE) {
    mgr->size_window_count++;
  }
}

/**
 * Get average block size from the rolling window.
 * Returns default estimate if insufficient data.
 */
static uint32_t size_window_get_average(const struct sync_manager *mgr) {
  if (mgr->size_window_count == 0) {
    return ADAPTIVE_DEFAULT_BLOCK_SIZE;
  }
  return (uint32_t)(mgr->size_window_total / mgr->size_window_count);
}

/**
 * Calculate adaptive download window based on memory budget.
 *
 * This is the heart of continuous adaptation:
 *   effective_window = memory_budget / avg_block_size
 *
 * As blocks grow larger, window shrinks proportionally.
 * As blocks shrink (empty blocks), window expands.
 */
static uint32_t get_adaptive_window(const struct sync_manager *mgr) {
  uint32_t avg_size = size_window_get_average(mgr);

  /* Prevent division by zero */
  if (avg_size == 0) {
    avg_size = ADAPTIVE_DEFAULT_BLOCK_SIZE;
  }

  /* Calculate ideal window for memory budget */
  uint32_t ideal_window = ADAPTIVE_WINDOW_MEMORY_BUDGET_BYTES / avg_size;

  /* Clamp to bounds */
  if (ideal_window < ADAPTIVE_WINDOW_MIN) {
    ideal_window = ADAPTIVE_WINDOW_MIN;
  }
  if (ideal_window > ADAPTIVE_WINDOW_MAX) {
    ideal_window = ADAPTIVE_WINDOW_MAX;
  }

  /* Never exceed configured maximum (pruned vs archival) */
  if (ideal_window > mgr->download_window) {
    ideal_window = mgr->download_window;
  }

  return ideal_window;
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
 * Block Queue Implementation
 * ============================================================================
 */

block_queue_t *block_queue_create(size_t capacity) {
  if (capacity == 0) {
    return NULL;
  }

  block_queue_t *queue = calloc(1, sizeof(block_queue_t));
  if (!queue) {
    return NULL;
  }

  queue->entries = calloc(capacity, sizeof(block_queue_entry_t));
  if (!queue->entries) {
    free(queue);
    return NULL;
  }

  queue->capacity = capacity;
  queue->count = 0;
  queue->pending_count = 0;
  queue->inflight_count = 0;

  return queue;
}

void block_queue_destroy(block_queue_t *queue) {
  if (!queue) {
    return;
  }
  free(queue->entries);
  free(queue);
}

echo_result_t block_queue_add(block_queue_t *queue, const hash256_t *hash,
                              uint32_t height) {
  if (!queue || !hash) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Check for duplicate */
  for (size_t i = 0; i < queue->capacity; i++) {
    if (queue->entries[i].valid &&
        memcmp(&queue->entries[i].hash, hash, sizeof(hash256_t)) == 0) {
      return ECHO_ERR_EXISTS;
    }
  }

  /* Check capacity */
  if (queue->count >= queue->capacity) {
    return ECHO_ERR_FULL;
  }

  /* Find empty slot */
  for (size_t i = 0; i < queue->capacity; i++) {
    if (!queue->entries[i].valid) {
      queue->entries[i].hash = *hash;
      queue->entries[i].height = height;
      queue->entries[i].assigned_peer = NULL;
      queue->entries[i].request_time = 0;
      queue->entries[i].in_flight = false;
      queue->entries[i].retry_count = 0;
      queue->entries[i].valid = true;
      queue->count++;
      queue->pending_count++;
      return ECHO_OK;
    }
  }

  return ECHO_ERR_FULL;
}

echo_result_t block_queue_next(block_queue_t *queue, hash256_t *hash,
                               uint32_t *height) {
  if (!queue || !hash || !height) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Find lowest-height unassigned block */
  uint32_t min_height = UINT32_MAX;
  size_t min_idx = SIZE_MAX;

  for (size_t i = 0; i < queue->capacity; i++) {
    if (queue->entries[i].valid && !queue->entries[i].in_flight) {
      if (queue->entries[i].height < min_height) {
        min_height = queue->entries[i].height;
        min_idx = i;
      }
    }
  }

  if (min_idx == SIZE_MAX) {
    return ECHO_ERR_NOT_FOUND;
  }

  *hash = queue->entries[min_idx].hash;
  *height = queue->entries[min_idx].height;
  return ECHO_OK;
}

echo_result_t block_queue_find_by_height(block_queue_t *queue, uint32_t height,
                                         hash256_t *hash) {
  if (!queue || !hash) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Find block at specified height (pending or in-flight) */
  for (size_t i = 0; i < queue->capacity; i++) {
    if (queue->entries[i].valid && queue->entries[i].height == height) {
      *hash = queue->entries[i].hash;
      return ECHO_OK;
    }
  }

  return ECHO_ERR_NOT_FOUND;
}

void block_queue_assign(block_queue_t *queue, const hash256_t *hash,
                        peer_t *peer) {
  if (!queue || !hash || !peer) {
    return;
  }

  for (size_t i = 0; i < queue->capacity; i++) {
    if (queue->entries[i].valid &&
        memcmp(&queue->entries[i].hash, hash, sizeof(hash256_t)) == 0) {
      if (!queue->entries[i].in_flight) {
        queue->pending_count--;
        queue->inflight_count++;
      }
      queue->entries[i].assigned_peer = peer;
      queue->entries[i].request_time = plat_time_ms();
      queue->entries[i].in_flight = true;
      return;
    }
  }
}

void block_queue_complete(block_queue_t *queue, const hash256_t *hash) {
  if (!queue || !hash) {
    return;
  }

  for (size_t i = 0; i < queue->capacity; i++) {
    if (queue->entries[i].valid &&
        memcmp(&queue->entries[i].hash, hash, sizeof(hash256_t)) == 0) {
      if (queue->entries[i].in_flight) {
        queue->inflight_count--;
      } else {
        queue->pending_count--;
      }
      queue->entries[i].valid = false;
      queue->count--;
      return;
    }
  }
}

void block_queue_unassign(block_queue_t *queue, const hash256_t *hash) {
  if (!queue || !hash) {
    return;
  }

  for (size_t i = 0; i < queue->capacity; i++) {
    if (queue->entries[i].valid &&
        memcmp(&queue->entries[i].hash, hash, sizeof(hash256_t)) == 0) {
      if (queue->entries[i].in_flight) {
        queue->entries[i].assigned_peer = NULL;
        queue->entries[i].in_flight = false;
        queue->entries[i].retry_count++;
        queue->inflight_count--;
        queue->pending_count++;
      }
      return;
    }
  }
}

void block_queue_unassign_peer(block_queue_t *queue, peer_t *peer) {
  if (!queue || !peer) {
    return;
  }

  for (size_t i = 0; i < queue->capacity; i++) {
    if (queue->entries[i].valid && queue->entries[i].in_flight &&
        queue->entries[i].assigned_peer == peer) {
      queue->entries[i].assigned_peer = NULL;
      queue->entries[i].in_flight = false;
      queue->entries[i].retry_count++;
      queue->inflight_count--;
      queue->pending_count++;
    }
  }
}

size_t block_queue_pending_count(const block_queue_t *queue) {
  return queue ? queue->pending_count : 0;
}

size_t block_queue_inflight_count(const block_queue_t *queue) {
  return queue ? queue->inflight_count : 0;
}

size_t block_queue_size(const block_queue_t *queue) {
  return queue ? queue->count : 0;
}

bool block_queue_contains(const block_queue_t *queue, const hash256_t *hash) {
  if (!queue || !hash) {
    return false;
  }

  for (size_t i = 0; i < queue->capacity; i++) {
    if (queue->entries[i].valid &&
        memcmp(&queue->entries[i].hash, hash, sizeof(hash256_t)) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * Check if a block at a given height is pending (in queue but not in-flight).
 * Returns true if the block is pending and needs to be requested.
 */
static bool block_queue_is_pending_at_height(const block_queue_t *queue,
                                             uint32_t height) {
  if (!queue) {
    return false;
  }

  for (size_t i = 0; i < queue->capacity; i++) {
    if (queue->entries[i].valid && queue->entries[i].height == height) {
      /* Block found - is it pending (not in-flight)? */
      return !queue->entries[i].in_flight;
    }
  }
  return false;
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
 * Select or validate the designated headers sync peer.
 *
 * During SYNC_MODE_HEADERS, we use a single peer for header download to avoid
 * redundant requests (previously we asked all peers for the same headers,
 * wasting 7/8 of bandwidth with 8 peers).
 *
 * Returns the peer_sync_state for the headers sync peer, or NULL if none available.
 * If the current designated peer is no longer suitable, selects a new one.
 */
static peer_sync_state_t *get_headers_sync_peer(sync_manager_t *mgr) {
  /* Check if current designated peer is still valid */
  if (mgr->has_headers_sync_peer && mgr->headers_sync_peer_idx < mgr->peer_count) {
    peer_sync_state_t *ps = &mgr->peers[mgr->headers_sync_peer_idx];
    if (ps->sync_candidate && peer_is_ready(ps->peer)) {
      return ps;
    }
    /* Current peer no longer suitable, need to pick a new one */
    log_info(LOG_COMP_SYNC, "Headers sync peer no longer suitable, selecting new peer");
    mgr->has_headers_sync_peer = false;
  }

  /* Select new headers sync peer: prefer highest start_height (has most chain) */
  peer_sync_state_t *best = NULL;
  int32_t best_height = -1;

  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_sync_state_t *ps = &mgr->peers[i];
    if (ps->sync_candidate && peer_is_ready(ps->peer)) {
      if (ps->start_height > best_height) {
        best = ps;
        best_height = ps->start_height;
        mgr->headers_sync_peer_idx = i;
      }
    }
  }

  if (best) {
    mgr->has_headers_sync_peer = true;
    log_info(LOG_COMP_SYNC, "Selected headers sync peer: %s (height=%d)",
             best->peer->address, best_height);
  }

  return best;
}

/**
 * Find best peer for block download.
 *
 * SIMPLIFIED Core-style selection:
 * 1. Must be sync_candidate (has blocks we need)
 * 2. Must be PEER_STATE_READY (handshake complete)
 * 3. Must have capacity: blocks_in_flight < 16
 * 4. Prefer peer with fewer in-flight (spread load)
 */
static peer_sync_state_t *find_best_block_peer(sync_manager_t *mgr) {
  peer_sync_state_t *best = NULL;
  size_t best_inflight = SIZE_MAX;
  size_t candidates = 0;
  size_t not_sync_candidate = 0;
  size_t not_ready = 0;
  size_t no_capacity = 0;

  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_sync_state_t *ps = &mgr->peers[i];
    bool ready = peer_is_ready(ps->peer);
    bool has_capacity = ps->blocks_in_flight_count < SYNC_MAX_BLOCKS_PER_PEER;

    if (!ps->sync_candidate) {
      not_sync_candidate++;
    } else if (!ready) {
      not_ready++;
    } else if (!has_capacity) {
      no_capacity++;
    } else {
      candidates++;

      /* Pick peer with fewest in-flight requests (spread load) */
      if (ps->blocks_in_flight_count < best_inflight) {
        best_inflight = ps->blocks_in_flight_count;
        best = ps;
      }
    }
  }

  if (candidates == 0) {
    log_info(LOG_COMP_SYNC,
             "find_best_block_peer: no candidates (peers=%zu, "
             "not_sync_candidate=%zu, not_ready=%zu, no_capacity=%zu)",
             mgr->peer_count, not_sync_candidate, not_ready, no_capacity);
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

/**
 * Force-request a critical block, bypassing capacity limits.
 *
 * Core-style: The next needed block is critical for validation progress.
 * If all peers are at capacity, we MUST still request it from someone.
 * Request from the peer with fewest in-flight blocks (lowest additional load).
 *
 * Returns true if the block was successfully requested.
 */
static bool force_request_critical_block(sync_manager_t *mgr,
                                         const hash256_t *hash,
                                         uint32_t height) {
  if (!mgr || !hash) {
    return false;
  }

  /* Find a sync candidate peer that DOESN'T already have this block in-flight.
   * The whole point of force-requesting is that the current peer(s) are slow,
   * so we must try a DIFFERENT peer to have any chance of success.
   *
   * PRIORITIZATION (per PEER_REPUTATION.md "Immediate Actions"):
   * 1. Prefer peers that delivered a block recently (last 5 seconds)
   * 2. Among equals, prefer peers with fewer in-flight blocks
   */
  peer_sync_state_t *best = NULL;
  size_t best_inflight = SIZE_MAX;
  bool best_is_recent = false;
  uint64_t now = plat_time_ms();

  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_sync_state_t *ps = &mgr->peers[i];
    if (!ps->sync_candidate || !peer_is_ready(ps->peer)) {
      continue;
    }

    /* CRITICAL: Skip peers that already have this block in-flight */
    bool already_has = false;
    for (size_t j = 0; j < ps->blocks_in_flight_count; j++) {
      if (memcmp(&ps->blocks_in_flight[j], hash, sizeof(hash256_t)) == 0) {
        already_has = true;
        break;
      }
    }
    if (already_has) {
      continue;  /* This peer is already slow on this block, try another */
    }

    /* Allow exceeding capacity by 1 for critical blocks */
    if (ps->blocks_in_flight_count >= SYNC_MAX_BLOCKS_PER_PEER + 1) {
      continue;
    }

    /* Check if this peer delivered recently (within 5 seconds) */
    bool is_recent = (ps->last_delivery_time > 0 &&
                      now - ps->last_delivery_time < 5000);

    /* Prefer recent deliverers, then by fewest in-flight */
    bool is_better = false;
    if (is_recent && !best_is_recent) {
      is_better = true;  /* Recent always beats non-recent */
    } else if (is_recent == best_is_recent) {
      is_better = (ps->blocks_in_flight_count < best_inflight);
    }

    if (is_better) {
      best = ps;
      best_inflight = ps->blocks_in_flight_count;
      best_is_recent = is_recent;
    }
  }

  if (!best) {
    /* Count how many peers already have this block */
    size_t already_have = 0;
    for (size_t i = 0; i < mgr->peer_count; i++) {
      peer_sync_state_t *ps = &mgr->peers[i];
      for (size_t j = 0; j < ps->blocks_in_flight_count; j++) {
        if (memcmp(&ps->blocks_in_flight[j], hash, sizeof(hash256_t)) == 0) {
          already_have++;
          break;
        }
      }
    }
    log_warn(LOG_COMP_SYNC,
             "force_request_critical_block: no NEW peer for height %u "
             "(already requested from %zu peers)",
             height, already_have);
    return false;
  }

  /* Assign block to peer */
  block_queue_assign(mgr->block_queue, hash, best->peer);

  /* Add to peer's in-flight list (may exceed normal limit) */
  if (best->blocks_in_flight_count < SYNC_MAX_BLOCKS_PER_PEER + 4) {
    best->blocks_in_flight[best->blocks_in_flight_count] = *hash;
    best->block_request_time[best->blocks_in_flight_count] = plat_time_ms();
    best->blocks_in_flight_count++;
    best->blocks_requested++;
  }

  /* Send immediate getdata request */
  if (mgr->callbacks.send_getdata_blocks) {
    hash256_t blocks[1] = {*hash};
    mgr->callbacks.send_getdata_blocks(best->peer, blocks, 1, mgr->callbacks.ctx);
  }

  log_info(LOG_COMP_SYNC,
           "Force-requested critical block %u from peer %s (now %zu in-flight)",
           height, best->peer->address, best->blocks_in_flight_count);

  return true;
}

sync_manager_t *sync_create(chainstate_t *chainstate,
                            const sync_callbacks_t *callbacks,
                            uint32_t download_window) {
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

  mgr->block_queue = block_queue_create(download_window);
  if (!mgr->block_queue) {
    free(mgr);
    return NULL;
  }

  mgr->chainstate = chainstate;
  mgr->callbacks = *callbacks;
  mgr->mode = SYNC_MODE_IDLE;
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
  block_queue_destroy(mgr->block_queue);
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

  log_info(LOG_COMP_SYNC, "Added peer %s to sync_mgr: height=%d, our_height=%u, "
           "services=0x%llx, sync_candidate=%s%s, state=%s",
           peer->address, height, our_height, (unsigned long long)peer->services,
           ps->sync_candidate ? "yes" : "no",
           (has_blocks && !can_serve_full) ? " (PRUNED - rejected)" : "",
           peer_state_string(peer->state));

  /* Check if we should trigger ping contest countdown.
   * Wait for 12 sync-candidate peers, then wait 12 more seconds. */
  if (mgr->mode == SYNC_MODE_IDLE && mgr->ping_contest_trigger_time == 0) {
    size_t sync_candidates = count_sync_peers(mgr);
    if (sync_candidates >= PING_CONTEST_MIN_PEERS) {
      mgr->ping_contest_trigger_time = plat_time_ms();
      log_info(LOG_COMP_SYNC,
               "Ping contest triggered: %zu sync candidates, waiting %d seconds",
               sync_candidates, PING_CONTEST_WAIT_MS / 1000);
    }
  }
}

void sync_remove_peer(sync_manager_t *mgr, peer_t *peer) {
  if (!mgr || !peer) {
    return;
  }

  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].peer == peer) {
      log_info(LOG_COMP_SYNC, "Removing peer %s from sync_mgr (state=%s)",
               peer->address, peer_state_string(peer->state));

      /* Unassign any blocks from this peer */
      block_queue_unassign_peer(mgr->block_queue, peer);

      /* Handle headers sync peer tracking */
      if (mgr->has_headers_sync_peer) {
        if (mgr->headers_sync_peer_idx == i) {
          /* This was our headers sync peer - need to pick a new one */
          log_info(LOG_COMP_SYNC, "Headers sync peer disconnected, will select new peer");
          mgr->has_headers_sync_peer = false;
        } else if (mgr->headers_sync_peer_idx > i) {
          /* Our headers sync peer is after the removed one - adjust index */
          mgr->headers_sync_peer_idx--;
        }
      }

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

  /*
   * AUDITION PHASE: Always run ping contest to select fastest peers,
   * regardless of whether we already have headers. The only exception
   * is when skip_ping_contest is explicitly set (e.g., for testing).
   *
   * After audition completes, we'll transition to BLOCKS or HEADERS mode
   * based on whether we have headers.
   */
  bool resume_in_blocks_mode =
      (mgr->best_header != NULL && best_header_height > validated_height);

  if (!mgr->skip_ping_contest && !mgr->audition_complete) {
    if (mgr->ping_contest_trigger_time == 0) {
      /* Not enough peers yet - caller should retry later */
      log_info(LOG_COMP_SYNC,
               "sync_start: waiting for %d peers (have %zu)",
               PING_CONTEST_MIN_PEERS, count_sync_peers(mgr));
      return ECHO_ERR_INVALID_STATE;
    }

    uint64_t wait_elapsed = now - mgr->ping_contest_trigger_time;
    if (wait_elapsed < PING_CONTEST_WAIT_MS) {
      /* Still in countdown - caller should retry */
      return ECHO_ERR_INVALID_STATE;
    }

    /* Countdown complete - send pings and enter PING_CONTEST mode */
    log_info(LOG_COMP_SYNC, "Starting ping contest with %zu peers",
             mgr->peer_count);

    mgr->ping_contest_start_time = now;
    mgr->ping_contest_sent = 0;
    mgr->ping_contest_responses = 0;

    /* Send ping to all sync-candidate peers */
    for (size_t i = 0; i < mgr->peer_count; i++) {
      peer_sync_state_t *ps = &mgr->peers[i];
      if (ps->sync_candidate && peer_is_ready(ps->peer)) {
        if (mgr->callbacks.send_ping) {
          mgr->callbacks.send_ping(ps->peer, mgr->callbacks.ctx);
          mgr->ping_contest_sent++;
        }
      }
    }

    log_info(LOG_COMP_SYNC, "Sent pings to %zu peers, waiting for responses",
             mgr->ping_contest_sent);

    mgr->mode = SYNC_MODE_PING_CONTEST;
  } else {
    /*
     * Audition complete or skipped - start in appropriate mode.
     * Use BLOCKS mode if we already have headers ahead of validated tip.
     */
    if (resume_in_blocks_mode) {
      log_info(LOG_COMP_SYNC,
               "Starting sync in BLOCKS mode (audition %s, headers=%u, validated=%u)",
               mgr->audition_complete ? "complete" : "skipped",
               best_header_height, validated_height);
      mgr->mode = SYNC_MODE_BLOCKS;
      mgr->block_sync_start_time = now;
    } else {
      log_info(LOG_COMP_SYNC,
               "Starting headers-first sync (audition %s)",
               mgr->audition_complete ? "complete" : "skipped");
      mgr->mode = SYNC_MODE_HEADERS;
    }
  }

  return ECHO_OK;
}

void sync_stop(sync_manager_t *mgr) {
  if (!mgr) {
    return;
  }

  mgr->mode = SYNC_MODE_IDLE;

  /* Clear all in-flight state */
  for (size_t i = 0; i < mgr->peer_count; i++) {
    mgr->peers[i].headers_in_flight = false;
    mgr->peers[i].blocks_in_flight_count = 0;
  }

  /* Clear block queue */
  while (block_queue_size(mgr->block_queue) > 0) {
    hash256_t hash;
    uint32_t height;
    if (block_queue_next(mgr->block_queue, &hash, &height) == ECHO_OK) {
      block_queue_complete(mgr->block_queue, &hash);
    } else {
      break;
    }
  }
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
        }
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

  /*
   * IBD profiling: Capture download time before clearing from in-flight.
   * We look for the earliest request time across all peers (for parallel reqs).
   */
  uint64_t download_start_time = 0;
  uint64_t now = plat_time_ms();

  /*
   * Remove this block from ALL peers' in-flight lists, not just the sender.
   * This is critical for parallel requests: we may have requested the same
   * block from multiple peers, and whichever responds first wins. We must
   * clear the request from all peers to avoid false timeout detection.
   */
  for (size_t p = 0; p < mgr->peer_count; p++) {
    peer_sync_state_t *check_ps = &mgr->peers[p];
    for (size_t i = 0; i < check_ps->blocks_in_flight_count; i++) {
      if (memcmp(&check_ps->blocks_in_flight[i], &block_hash,
                 sizeof(hash256_t)) == 0) {
        /* Capture earliest request time for download timing */
        if (download_start_time == 0 ||
            check_ps->block_request_time[i] < download_start_time) {
          download_start_time = check_ps->block_request_time[i];
        }
        /* Remove from this peer's in-flight list */
        for (size_t j = i; j < check_ps->blocks_in_flight_count - 1; j++) {
          check_ps->blocks_in_flight[j] = check_ps->blocks_in_flight[j + 1];
          check_ps->block_request_time[j] = check_ps->block_request_time[j + 1];
        }
        check_ps->blocks_in_flight_count--;
        break; /* Each peer has at most one entry for this block */
      }
    }
  }

  /* Update peer quality latency tracking for the delivering peer */
  if (download_start_time > 0 && now > download_start_time) {
    uint64_t download_ms = now - download_start_time;

    /* Record first block time if not set */
    if (ps->first_block_time == 0) {
      ps->first_block_time = now;
    }

    /* Update running average latency */
    ps->total_latency_ms += download_ms;
    ps->latency_samples++;
    ps->avg_block_latency_ms = ps->total_latency_ms / ps->latency_samples;
  }

  /* Store block if callback provided (before validation - we need the data) */
  if (mgr->callbacks.store_block) {
    mgr->callbacks.store_block(block, mgr->callbacks.ctx);
  }

  /* Find block index */
  block_index_map_t *index_map =
      chainstate_get_block_index_map(mgr->chainstate);
  block_index_t *block_index = block_index_map_lookup(index_map, &block_hash);

  /* Validate and apply if callback provided */
  if (mgr->callbacks.validate_and_apply_block && block_index) {
    /*
     * Check if this block connects to the validated tip before attempting
     * validation. This avoids expensive validation attempts and noisy error
     * logging for out-of-order blocks during parallel download.
     *
     * NOTE: We can't use on_main_chain because headers-first sync sets that
     * flag on all headers during header download. Instead, check if this
     * block's parent height equals the current validated tip height.
     */
    uint32_t validated_height = chainstate_get_height(mgr->chainstate);
    uint32_t block_height = block_index->height;
    bool can_connect =
        (block_height == 0) || /* Genesis can always connect */
        (block_height == validated_height + 1); /* Next block after tip */

    if (!can_connect) {
      /* Block doesn't connect to current tip - re-queue for later */
      block_queue_unassign(mgr->block_queue, &block_hash);
      return ECHO_ERR_WOULD_BLOCK; /* Not an error, just waiting for parent */
    }

    /* Bottleneck tracking: record starvation time if we were waiting */
    if (mgr->starvation_start_time > 0) {
      uint64_t starvation_end = plat_time_ms();
      mgr->total_starvation_ms += (starvation_end - mgr->starvation_start_time);
      mgr->starvation_start_time = 0;
    }

    /* Time the validation callback */
    uint64_t validation_start = plat_time_ms();
    echo_result_t result = mgr->callbacks.validate_and_apply_block(
        block, block_index, mgr->callbacks.ctx);
    uint64_t validation_end = plat_time_ms();
    mgr->total_validation_ms += (validation_end - validation_start);

    if (result != ECHO_OK) {
      /*
       * Block validation failed for a reason other than ordering.
       * Re-queue to try again later.
       */
      block_queue_unassign(mgr->block_queue, &block_hash);
      return ECHO_ERR_INVALID;
    }
    mgr->blocks_validated_total++;
    mgr->last_validated_height = block_height;
    rate_window_record(mgr, plat_time_ms());

    /* Core-style adaptive timeout: decay on success (faster recovery) */
    uint64_t decayed = (uint64_t)(mgr->stalling_timeout_ms * SYNC_STALLING_TIMEOUT_DECAY);
    if (decayed < SYNC_BLOCK_STALLING_TIMEOUT_MS) {
      decayed = SYNC_BLOCK_STALLING_TIMEOUT_MS;
    }
    mgr->stalling_timeout_ms = decayed;

    /*
     * After successful validation, check if we have the next block already
     * stored on disk (from out-of-order download). If so, load and validate
     * it immediately without waiting for another network round-trip.
     */
    if (mgr->callbacks.get_block) {
      uint32_t next_height = chainstate_get_height(mgr->chainstate) + 1;

      while (next_height <= mgr->best_header->height) {
        /* Find block index at next height */
        block_index_t *next_index = NULL;
        block_index_map_t *idx_map =
            chainstate_get_block_index_map(mgr->chainstate);

        /*
         * Look up block by height using callback (works for stored blocks
         * from previous sessions that aren't in the queue).
         */
        hash256_t next_hash;
        bool found_block = false;

        /* First try the height lookup callback (finds stored blocks) */
        if (mgr->callbacks.get_block_hash_at_height &&
            mgr->callbacks.get_block_hash_at_height(next_height, &next_hash,
                                                    mgr->callbacks.ctx) == ECHO_OK) {
          next_index = block_index_map_lookup(idx_map, &next_hash);
          if (next_index != NULL) {
            found_block = true;
            log_debug(LOG_COMP_SYNC,
                      "Found block at height %u via callback, data_file=%u",
                      next_height, next_index->data_file);
          }
        }

        /* Fall back to queue lookup if callback didn't find it */
        if (!found_block &&
            block_queue_find_by_height(mgr->block_queue, next_height,
                                       &next_hash) == ECHO_OK) {
          next_index = block_index_map_lookup(idx_map, &next_hash);
          if (next_index != NULL) {
            found_block = true;
            log_debug(LOG_COMP_SYNC,
                      "Found block at height %u in queue, data_file=%u",
                      next_height, next_index->data_file);
          }
        }

        if (!found_block) {
          log_debug(LOG_COMP_SYNC,
                    "Block at height %u not found", next_height);
          break;
        }

        if (next_index == NULL ||
            next_index->data_file == BLOCK_DATA_NOT_STORED) {
          log_debug(LOG_COMP_SYNC,
                    "Block at height %u not stored yet (data_file=%u)",
                    next_height,
                    next_index ? next_index->data_file : 0xFFFFFFFF);
          break; /* Next block not stored yet */
        }

        /* Load block from storage */
        block_t stored_block;
        echo_result_t load_result =
            mgr->callbacks.get_block(&next_index->hash, &stored_block,
                                     mgr->callbacks.ctx);
        if (load_result != ECHO_OK) {
          log_debug(LOG_COMP_SYNC,
                    "Failed to load stored block at height %u: %d",
                    next_height, load_result);
          /* Mark as not stored so we don't keep retrying (file may be pruned) */
          next_index->data_file = BLOCK_DATA_NOT_STORED;
          next_index->data_pos = 0;
          break;
        }

        /* Time the stored block validation too */
        uint64_t stored_val_start = plat_time_ms();
        echo_result_t val_result = mgr->callbacks.validate_and_apply_block(
            &stored_block, next_index, mgr->callbacks.ctx);
        uint64_t stored_val_end = plat_time_ms();
        mgr->total_validation_ms += (stored_val_end - stored_val_start);

        block_free(&stored_block);

        if (val_result != ECHO_OK) {
          log_warn(LOG_COMP_SYNC,
                   "Stored block at height %u failed validation: %d",
                   next_height, val_result);
          break;
        }

        /* Success! Remove from queue and continue */
        block_queue_complete(mgr->block_queue, &next_index->hash);
        mgr->blocks_validated_total++;
        mgr->last_validated_height = next_height;
        mgr->blocks_ready++; /* Had next block immediately available */
        rate_window_record(mgr, plat_time_ms());
        mgr->last_progress_time = plat_time_ms();

        log_info(LOG_COMP_SYNC,
                 "Validated stored block at height %u (from disk)",
                 next_height);

        next_height++;
      }

      /*
       * Bottleneck tracking: After processing all available stored blocks,
       * check if the next block is ready. If not, start starvation timer.
       */
      uint32_t current_height = chainstate_get_height(mgr->chainstate);
      hash256_t check_next_hash;
      if (current_height < mgr->best_header->height &&
          block_queue_find_by_height(mgr->block_queue, current_height + 1,
                                     &check_next_hash) != ECHO_OK) {
        /* Next block not in queue - we're now waiting (starved) */
        mgr->blocks_starved++;
        mgr->starvation_start_time = plat_time_ms();
      }
    }
  }

  /* Only mark complete in queue AFTER successful validation */
  block_queue_complete(mgr->block_queue, &block_hash);

  mgr->blocks_received_total++;
  ps->blocks_received++;
  ps->last_delivery_time = plat_time_ms(); /* Track for session reputation */
  mgr->last_progress_time = ps->last_delivery_time;

  /* Record block size for adaptive window calculation */
  size_t block_size = block_serialize_size(block);
  size_window_record(mgr, (uint32_t)block_size);

  /* IBD profiling: Log download timing for slow blocks or every 1000 blocks */
  if (download_start_time > 0) {
    uint64_t download_ms = now - download_start_time;
    if (download_ms > 500 || mgr->blocks_received_total % 1000 == 0) {
      log_info(LOG_COMP_SYNC,
               "Block downloaded in %lums (%zu txs, peer=%s)",
               (unsigned long)download_ms, block->tx_count,
               peer->address);
    }
  }

  /* Check if sync is complete */
  if (mgr->mode == SYNC_MODE_BLOCKS &&
      block_queue_size(mgr->block_queue) == 0) {
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
   * CORE-STYLE NEXT-BLOCK STALL DETECTION
   *
   * The most critical block is the NEXT NEEDED block (validated_tip + 1).
   * If this block is stalled or not requested, validation cannot progress
   * regardless of how many other blocks we download.
   *
   * Core uses aggressive 2-second timeout specifically for this block and
   * ensures it's always being requested from SOME peer.
   *
   * Handle three cases:
   * 1. Block in-flight and stalled -> unassign and re-request
   * 2. Block pending (not requested) -> force-request immediately
   * 3. Block not in queue -> log warning (should not happen during IBD)
   */
  if (mgr->mode == SYNC_MODE_BLOCKS && mgr->chainstate) {
    uint32_t next_needed_height = chainstate_get_height(mgr->chainstate) + 1;
    hash256_t next_needed_hash;
    bool found_in_flight = false;

    /* Find the hash of the next needed block */
    if (block_queue_find_by_height(mgr->block_queue, next_needed_height,
                                   &next_needed_hash) == ECHO_OK) {
      /* Check if this block is in-flight with any peer */
      for (size_t i = 0; i < mgr->peer_count && !found_in_flight; i++) {
        peer_sync_state_t *ps = &mgr->peers[i];
        for (size_t j = 0; j < ps->blocks_in_flight_count; j++) {
          if (memcmp(&ps->blocks_in_flight[j], &next_needed_hash,
                     sizeof(hash256_t)) == 0) {
            found_in_flight = true;
            /* Found it - check if stalled (use base 2s timeout, not adaptive) */
            uint64_t elapsed = now - ps->block_request_time[j];
            if (elapsed > SYNC_BLOCK_STALLING_TIMEOUT_MS) {
              log_info(LOG_COMP_SYNC,
                       "Next needed block %u stalled for %llums - re-requesting",
                       next_needed_height, (unsigned long long)elapsed);

              /* Unassign from this peer so it gets re-requested */
              block_queue_unassign(mgr->block_queue, &next_needed_hash);

              /* Remove from peer's in-flight list */
              for (size_t k = j; k < ps->blocks_in_flight_count - 1; k++) {
                ps->blocks_in_flight[k] = ps->blocks_in_flight[k + 1];
                ps->block_request_time[k] = ps->block_request_time[k + 1];
              }
              ps->blocks_in_flight_count--;

              /* Track starvation time for metrics */
              mgr->total_starvation_ms += elapsed;
              mgr->blocks_starved++;

              /* Force immediate re-request, bypassing capacity limits */
              force_request_critical_block(mgr, &next_needed_hash, next_needed_height);
            }
            break;
          }
        }
      }

      /*
       * CASE 2: Block is PENDING (in queue but not in-flight).
       *
       * This is the critical bug fix: if the next needed block is in the queue
       * but no peer has requested it yet, we're blocked waiting for something
       * that will never arrive. Force-request it now, bypassing capacity limits!
       */
      if (!found_in_flight &&
          block_queue_is_pending_at_height(mgr->block_queue, next_needed_height)) {
        log_info(LOG_COMP_SYNC,
                 "Next needed block %u is PENDING but not in-flight - forcing request",
                 next_needed_height);
        mgr->blocks_starved++;

        /* Force request, bypassing peer capacity limits */
        force_request_critical_block(mgr, &next_needed_hash, next_needed_height);
      }
    }
  }

  /* Process header timeouts */
  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_sync_state_t *ps = &mgr->peers[i];

    if (ps->headers_in_flight &&
        (now - ps->headers_sent_time > SYNC_HEADERS_TIMEOUT_MS)) {
      ps->headers_in_flight = false;
      ps->timeout_count++;

      /* If this was our designated headers sync peer, clear it so we pick a new one */
      if (mgr->has_headers_sync_peer && mgr->headers_sync_peer_idx == i) {
        log_info(LOG_COMP_SYNC, "Headers sync peer timed out, will select new peer");
        mgr->has_headers_sync_peer = false;
      }
    }

    /* Process block timeouts - count stalls per peer per tick, not per block.
     *
     * Core-style adaptive timeout:
     * - Starts at SYNC_BLOCK_STALLING_TIMEOUT_MS (5s)
     * - Doubles on each stall (up to SYNC_BLOCK_STALLING_TIMEOUT_MAX_MS)
     * - Decays by 0.85 on each block success
     * This allows tolerance for network variance while recovering quickly
     * when conditions improve.
     */
    size_t stalled_this_tick = 0;
    for (size_t j = 0; j < ps->blocks_in_flight_count;) {
      if (now - ps->block_request_time[j] > mgr->stalling_timeout_ms) {
        /* Timeout - unassign block and re-queue */
        hash256_t stalled_hash = ps->blocks_in_flight[j];
        block_queue_unassign(mgr->block_queue, &stalled_hash);
        stalled_this_tick++;

        /* Remove from peer's in-flight list */
        for (size_t k = j; k < ps->blocks_in_flight_count - 1; k++) {
          ps->blocks_in_flight[k] = ps->blocks_in_flight[k + 1];
          ps->block_request_time[k] = ps->block_request_time[k + 1];
        }
        ps->blocks_in_flight_count--;
      } else {
        j++;
      }
    }

    /* Only count ONE stall event per peer per tick, not one per block */
    if (stalled_this_tick > 0) {
      ps->timeout_count++;

      log_debug(LOG_COMP_SYNC,
               "Peer stall: %zu blocks timed out (stall events: %u)",
               stalled_this_tick, ps->timeout_count);

      /* After 3 stall events, disconnect slow peer.
       * Core-style: only double the global timeout when we DISCONNECT,
       * not on every stall event. This prevents timeout death spiral.
       */
      if (ps->timeout_count >= 3 && ps->peer) {
        log_info(LOG_COMP_SYNC,
                 "Disconnecting slow peer after %u stall events",
                 ps->timeout_count);

        /* Core-style: double timeout only when disconnecting a peer */
        uint64_t doubled = mgr->stalling_timeout_ms * 2;
        if (doubled > SYNC_BLOCK_STALLING_TIMEOUT_MAX_MS) {
          doubled = SYNC_BLOCK_STALLING_TIMEOUT_MAX_MS;
        }
        mgr->stalling_timeout_ms = doubled;
        log_info(LOG_COMP_SYNC, "Global timeout now %llums",
                 (unsigned long long)mgr->stalling_timeout_ms);

        peer_disconnect(ps->peer, PEER_DISCONNECT_MISBEHAVING,
                        "Too many block stalls during IBD");
        continue;
      }
    }

    /* Block delivery rate check: disconnect peers that accept requests
     * but don't deliver blocks. This catches peers with headers but no
     * block data - ping RTT doesn't measure block delivery ability.
     *
     * TIERED APPROACH (per PEER_REPUTATION.md "Immediate Actions"):
     *
     * 1. Non-responders (0% delivery): Cull aggressively after 30 requests/30s
     *    These peers are truly broken - not serving any blocks at all.
     *
     * 2. Slow responders (1-10% delivery): Only cull if we have >16 peers
     *    With 8-peer racing, even good peers only deliver ~12.5% (1/8).
     *    Don't create a "death spiral" by culling slow-but-working peers
     *    when we're already low on sync candidates.
     */
    if (ps->peer && ps->blocks_requested >= 30) {
      uint64_t connected_ms = now - ps->peer->connect_time;
      uint32_t delivery_pct = (ps->blocks_received * 100) / ps->blocks_requested;

      /* Tier 1: Zero delivery - always cull after 30s (truly broken) */
      if (delivery_pct == 0 && connected_ms >= 30000) {
        log_info(LOG_COMP_SYNC,
                 "Disconnecting non-responding peer: 0/%u blocks over %llus",
                 ps->blocks_requested, (unsigned long long)(connected_ms / 1000));
        peer_disconnect(ps->peer, PEER_DISCONNECT_MISBEHAVING,
                        "Zero block delivery during IBD");
        continue;
      }

      /* Tier 2: Low delivery - only cull if we have enough peers */
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

  /*
   * ADAPTIVE WINDOW: Calculate effective download window based on recent
   * block sizes. Like difficulty adjustment, this continuously adapts to
   * actual conditions rather than assuming based on height.
   */
  uint32_t effective_window = get_adaptive_window(mgr);
  uint32_t avg_block_size = size_window_get_average(mgr);

  /* Calculate target range using adaptive window */
  uint32_t start_height = tip_height + 1;
  uint32_t end_height = tip_height + effective_window;
  if (end_height > mgr->best_header->height) {
    end_height = mgr->best_header->height;
  }

  log_info(LOG_COMP_SYNC,
           "queue_blocks: tip=%u, start=%u, end=%u, window=%u (avg_blk=%uKB), best=%u",
           tip_height, start_height, end_height, effective_window,
           avg_block_size / 1024, mgr->best_header->height);

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
      /* Check if we already have this block in queue or storage */
      bool in_queue = block_queue_contains(mgr->block_queue, &hash);
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
          /*
           * Block is already in storage. If it's the next block to validate,
           * validate it immediately to kickstart the stored-block chain.
           * This handles the restart case where validated_height = 0 but
           * we have blocks stored from a previous run.
           */
          uint32_t validated_height = chainstate_get_height(mgr->chainstate);
          bool is_next = (h == validated_height + 1);
          bool has_callback = (mgr->callbacks.validate_and_apply_block != NULL);

          if (h == start_height) {
            log_info(LOG_COMP_SYNC,
                     "queue_blocks: kickstart check h=%u, validated=%u, "
                     "is_next=%s, has_callback=%s",
                     h, validated_height, is_next ? "YES" : "NO",
                     has_callback ? "YES" : "NO");
          }

          if (is_next && has_callback) {
            block_index_map_t *idx_map = chainstate_get_block_index_map(mgr->chainstate);
            block_index_t *block_idx = block_index_map_lookup(idx_map, &hash);

            log_info(LOG_COMP_SYNC,
                     "queue_blocks: kickstart block_idx=%p", (void *)block_idx);

            if (block_idx != NULL) {
              log_info(LOG_COMP_SYNC,
                       "Validating stored block at height %u (kickstart)", h);

              echo_result_t val_result = mgr->callbacks.validate_and_apply_block(
                  &stored, block_idx, mgr->callbacks.ctx);

              if (val_result == ECHO_OK) {
                mgr->blocks_validated_total++;
                rate_window_record(mgr, plat_time_ms());
                mgr->last_progress_time = plat_time_ms();
                log_info(LOG_COMP_SYNC,
                         "Validated stored block at height %u (kickstart success)", h);
              } else {
                log_warn(LOG_COMP_SYNC,
                         "Stored block at height %u failed validation: %d", h, val_result);
              }
            }
          } else if (h <= 5) {
            log_info(LOG_COMP_SYNC, "queue_blocks: height %u already in storage", h);
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
      if (!block_queue_contains(mgr->block_queue, &idx->hash)) {
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
  }

  /* Add to queue in height order (lowest first) */
  for (size_t i = 0; i < to_queue_count; i++) {
    block_queue_add(mgr->block_queue, &to_queue[i], heights[i]);
  }
}

/**
 * Request critical zone blocks with redundancy.
 *
 * Critical zone = first N blocks from validated tip. These are requested from
 * multiple peers simultaneously to eliminate head-of-line blocking. First
 * response wins; duplicates are discarded by block storage layer.
 *
 * Returns number of racing requests made.
 */
static size_t request_critical_zone_blocks(sync_manager_t *mgr) {
  uint32_t validated_height = chainstate_get_height(mgr->chainstate);
  size_t racing_requests = 0;

  /* Need callback to look up block hash by height */
  if (!mgr->callbacks.get_block_hash_at_height) {
    return 0;
  }

  /* Find the first SYNC_CRITICAL_ZONE_SIZE pending blocks */
  for (uint32_t h = validated_height + 1;
       h <= validated_height + SYNC_CRITICAL_ZONE_SIZE; h++) {

    /* Look up block hash at this height */
    hash256_t block_hash;
    if (mgr->callbacks.get_block_hash_at_height(h, &block_hash,
                                                 mgr->callbacks.ctx) != ECHO_OK) {
      continue;  /* No header for this height yet */
    }

    /* Check if already in-flight from enough peers */
    size_t current_inflight = 0;
    for (size_t i = 0; i < mgr->peer_count; i++) {
      peer_sync_state_t *ps = &mgr->peers[i];
      for (size_t j = 0; j < ps->blocks_in_flight_count; j++) {
        if (memcmp(&ps->blocks_in_flight[j], &block_hash, sizeof(hash256_t)) == 0) {
          current_inflight++;
          break;
        }
      }
    }

    /* Already have enough redundancy for this block */
    if (current_inflight >= SYNC_CRITICAL_ZONE_REDUNDANCY) {
      continue;
    }

    /* Request from additional peers to reach redundancy target.
     *
     * TWO-PASS APPROACH (per PEER_REPUTATION.md "Immediate Actions"):
     * 1. First pass: Prefer peers with good delivery rate (>15%)
     * 2. Second pass: Fill remaining slots from any available peer
     *
     * This ensures high-performing peers get racing priority.
     */
    size_t needed = SYNC_CRITICAL_ZONE_REDUNDANCY - current_inflight;

    /* Pass 1: High-delivery peers first (>15% delivery rate) */
    for (size_t i = 0; i < mgr->peer_count && needed > 0; i++) {
      peer_sync_state_t *ps = &mgr->peers[i];

      /* Skip if not a sync candidate (pruned peers can't serve historical blocks) */
      if (!ps->sync_candidate) continue;
      /* Skip if not ready or no capacity */
      if (!ps->peer || !peer_is_ready(ps->peer)) continue;
      if (ps->blocks_in_flight_count >= SYNC_MAX_BLOCKS_PER_PEER) continue;

      /* Pass 1: Only high-delivery peers (>15%) */
      if (ps->blocks_requested >= 10) {
        uint32_t delivery_pct = (ps->blocks_received * 100) / ps->blocks_requested;
        if (delivery_pct < 15) continue;  /* Skip low-delivery peers in pass 1 */
      }

      /* Skip if already has this block in-flight */
      bool already_has = false;
      for (size_t j = 0; j < ps->blocks_in_flight_count; j++) {
        if (memcmp(&ps->blocks_in_flight[j], &block_hash, sizeof(hash256_t)) == 0) {
          already_has = true;
          break;
        }
      }
      if (already_has) continue;

      /* Request this critical block from this peer */
      ps->blocks_in_flight[ps->blocks_in_flight_count] = block_hash;
      ps->block_request_time[ps->blocks_in_flight_count] = plat_time_ms();
      ps->blocks_in_flight_count++;
      ps->blocks_requested++;

      if (mgr->callbacks.send_getdata_blocks) {
        mgr->callbacks.send_getdata_blocks(ps->peer, &block_hash, 1,
                                           mgr->callbacks.ctx);
      }

      racing_requests++;
      needed--;
    }

    /* Pass 2: Fill remaining slots from any available peer */
    for (size_t i = 0; i < mgr->peer_count && needed > 0; i++) {
      peer_sync_state_t *ps = &mgr->peers[i];

      if (!ps->sync_candidate) continue;
      if (!ps->peer || !peer_is_ready(ps->peer)) continue;
      if (ps->blocks_in_flight_count >= SYNC_MAX_BLOCKS_PER_PEER) continue;

      /* Skip if already has this block in-flight */
      bool already_has = false;
      for (size_t j = 0; j < ps->blocks_in_flight_count; j++) {
        if (memcmp(&ps->blocks_in_flight[j], &block_hash, sizeof(hash256_t)) == 0) {
          already_has = true;
          break;
        }
      }
      if (already_has) continue;

      /* Request this critical block from this peer */
      ps->blocks_in_flight[ps->blocks_in_flight_count] = block_hash;
      ps->block_request_time[ps->blocks_in_flight_count] = plat_time_ms();
      ps->blocks_in_flight_count++;
      ps->blocks_requested++;

      if (mgr->callbacks.send_getdata_blocks) {
        mgr->callbacks.send_getdata_blocks(ps->peer, &block_hash, 1,
                                           mgr->callbacks.ctx);
      }

      racing_requests++;
      needed--;
    }
  }

  if (racing_requests > 0) {
    log_debug(LOG_COMP_SYNC, "Critical zone: %zu racing requests for blocks %u-%u",
              racing_requests, validated_height + 1,
              validated_height + SYNC_CRITICAL_ZONE_SIZE);
  } else {
    /* Debug why no racing happened - log peer capacity every 5s */
    static uint64_t last_no_race_log = 0;
    uint64_t now = plat_time_ms();
    if (now - last_no_race_log >= 5000) {
      last_no_race_log = now;

      /* Count sync-eligible peers with capacity vs full */
      size_t peers_ready = 0, peers_full = 0, peers_not_ready = 0, peers_pruned = 0;
      for (size_t i = 0; i < mgr->peer_count; i++) {
        peer_sync_state_t *ps = &mgr->peers[i];
        if (!ps->sync_candidate) {
          peers_pruned++;
        } else if (!ps->peer || !peer_is_ready(ps->peer)) {
          peers_not_ready++;
        } else if (ps->blocks_in_flight_count >= SYNC_MAX_BLOCKS_PER_PEER) {
          peers_full++;
        } else {
          peers_ready++;
        }
      }

      log_info(LOG_COMP_SYNC,
               "Critical zone: NO RACING | peers=%zu (ready=%zu full=%zu notready=%zu pruned=%zu) "
               "| validated=%u",
               mgr->peer_count, peers_ready, peers_full, peers_not_ready, peers_pruned,
               validated_height);
    }
  }

  return racing_requests;
}

/**
 * Request blocks from peers.
 */
static void request_blocks(sync_manager_t *mgr) {
  /* First: ensure critical zone blocks have redundant requests */
  request_critical_zone_blocks(mgr);

  /* Track blocks to request per peer for batched getdata */
  hash256_t blocks_for_peer[SYNC_MAX_BLOCKS_PER_PEER];
  size_t blocks_count = 0;
  peer_sync_state_t *current_peer = NULL;

  uint32_t validated_height = chainstate_get_height(mgr->chainstate);

  /* Log entry state */
  size_t pending = block_queue_pending_count(mgr->block_queue);
  size_t inflight = block_queue_inflight_count(mgr->block_queue);
  if (pending > 0 && inflight < SYNC_MAX_PARALLEL_BLOCKS) {
    log_info(LOG_COMP_SYNC,
             "request_blocks entry: peer_count=%zu, pending=%zu, inflight=%zu",
             mgr->peer_count, pending, inflight);
  }

  /*
   * Request blocks from peers with capacity.
   * Core approach: simple parallel download, no racing, no priority pools.
   * Window adapts based on recent block sizes.
   */
  uint32_t effective_window = get_adaptive_window(mgr);
  uint32_t max_request_height = validated_height + effective_window;

  /* Request blocks from queue */
  size_t iteration = 0;
  while (block_queue_pending_count(mgr->block_queue) > 0 &&
         block_queue_inflight_count(mgr->block_queue) <
             SYNC_MAX_PARALLEL_BLOCKS) {
    iteration++;
    size_t cur_pending = block_queue_pending_count(mgr->block_queue);
    size_t cur_inflight = block_queue_inflight_count(mgr->block_queue);
    log_debug(LOG_COMP_SYNC,
              "request_blocks iter=%zu: pending=%zu, inflight=%zu, blocks_count=%zu",
              iteration, cur_pending, cur_inflight, blocks_count);

    /* Find peer with capacity */
    peer_sync_state_t *ps = find_best_block_peer(mgr);
    if (!ps) {
      log_info(LOG_COMP_SYNC,
               "request_blocks: no peer with capacity at iter=%zu", iteration);
      break;
    }

    /* If we're switching peers, send accumulated requests to previous peer */
    if (current_peer != NULL && current_peer != ps && blocks_count > 0) {
      if (mgr->callbacks.send_getdata_blocks) {
        mgr->callbacks.send_getdata_blocks(current_peer->peer, blocks_for_peer,
                                           blocks_count, mgr->callbacks.ctx);
      }
      blocks_count = 0;
    }
    current_peer = ps;

    /* Get next block to download */
    hash256_t hash;
    uint32_t height;
    if (block_queue_next(mgr->block_queue, &hash, &height) != ECHO_OK) {
      break;
    }

    /* Don't request blocks too far ahead of validated tip */
    if (height > max_request_height) {
      log_debug(LOG_COMP_SYNC,
                "request_blocks: block at height %u exceeds window (tip=%u)",
                height, validated_height);
      break;
    }

    /* Assign to peer */
    block_queue_assign(mgr->block_queue, &hash, ps->peer);

    /* Add to peer's in-flight list */
    if (ps->blocks_in_flight_count < SYNC_MAX_BLOCKS_PER_PEER) {
      ps->blocks_in_flight[ps->blocks_in_flight_count] = hash;
      ps->block_request_time[ps->blocks_in_flight_count] = plat_time_ms();
      ps->blocks_in_flight_count++;
      ps->blocks_requested++;
    }

    /* Collect for batched getdata */
    if (blocks_count < SYNC_MAX_BLOCKS_PER_PEER) {
      blocks_for_peer[blocks_count++] = hash;
    }
  }

  /* Send any remaining accumulated requests */
  if (current_peer != NULL && blocks_count > 0) {
    if (mgr->callbacks.send_getdata_blocks) {
      log_info(LOG_COMP_SYNC, "Sending getdata for %zu blocks", blocks_count);
      mgr->callbacks.send_getdata_blocks(current_peer->peer, blocks_for_peer,
                                         blocks_count, mgr->callbacks.ctx);
    }
  }
}

void sync_report_pong(sync_manager_t *mgr, peer_t *peer) {
  if (!mgr || !peer) {
    return;
  }

  /* Only count during ping contest */
  if (mgr->mode == SYNC_MODE_PING_CONTEST) {
    mgr->ping_contest_responses++;
    log_debug(LOG_COMP_SYNC, "Pong from %s: RTT=%llu ms (%zu/%zu responses)",
              peer->address, (unsigned long long)peer->last_rtt_ms,
              mgr->ping_contest_responses, mgr->ping_contest_sent);
  }
}

void sync_tick(sync_manager_t *mgr) {
  if (!mgr) {
    return;
  }

  sync_process_timeouts(mgr);

  switch (mgr->mode) {
  case SYNC_MODE_PING_CONTEST: {
    /* Wait for ping responses or timeout, then pick fastest peer */
    uint64_t now = plat_time_ms();
    uint64_t elapsed = now - mgr->ping_contest_start_time;

    bool all_responded = (mgr->ping_contest_responses >= mgr->ping_contest_sent);
    bool timed_out = (elapsed >= PING_CONTEST_TIMEOUT_MS);

    if (all_responded || timed_out) {
      /* Contest complete - find fastest peer */
      peer_sync_state_t *fastest = NULL;
      uint64_t fastest_rtt = UINT64_MAX;

      for (size_t i = 0; i < mgr->peer_count; i++) {
        peer_sync_state_t *ps = &mgr->peers[i];
        if (ps->sync_candidate && peer_is_ready(ps->peer) &&
            ps->peer->last_rtt_ms > 0 && ps->peer->last_rtt_ms < fastest_rtt) {
          fastest = ps;
          fastest_rtt = ps->peer->last_rtt_ms;
          mgr->headers_sync_peer_idx = i;
        }
      }

      if (fastest) {
        log_info(LOG_COMP_SYNC,
                 "Ping contest complete: winner=%s (RTT=%llu ms), "
                 "responses=%zu/%zu",
                 fastest->peer->address, (unsigned long long)fastest_rtt,
                 mgr->ping_contest_responses, mgr->ping_contest_sent);
        mgr->has_headers_sync_peer = true;
      } else {
        log_warn(LOG_COMP_SYNC,
                 "Ping contest: no responses, falling back to height-based selection");
        mgr->has_headers_sync_peer = false;
      }

      /*
       * AUDITION CULLING: Now that we know everyone's RTT, dismiss the
       * slow performers and keep only the elite orchestra.
       */
      if (mgr->callbacks.cull_slow_peers && !mgr->audition_complete) {
        size_t target = ECHO_MAX_OUTBOUND_PEERS;
        log_info(LOG_COMP_SYNC,
                 "Audition complete - culling to top %zu fastest peers",
                 target);
        size_t culled = mgr->callbacks.cull_slow_peers(target, mgr->callbacks.ctx);
        log_info(LOG_COMP_SYNC,
                 "Audition: dismissed %zu slow peers, keeping fastest %zu",
                 culled, target);
        mgr->audition_complete = true;
      }

      /*
       * Transition to next mode: BLOCKS if we already have headers
       * ahead of validated tip, otherwise HEADERS to fetch headers first.
       */
      uint32_t validated_height = chainstate_get_height(mgr->chainstate);
      uint32_t best_header_height = mgr->best_header ? mgr->best_header->height : 0;

      if (mgr->best_header != NULL && best_header_height > validated_height) {
        log_info(LOG_COMP_SYNC,
                 "Audition complete - resuming BLOCKS mode (headers=%u, validated=%u)",
                 best_header_height, validated_height);
        mgr->mode = SYNC_MODE_BLOCKS;
        mgr->block_sync_start_time = plat_time_ms();
      } else {
        log_info(LOG_COMP_SYNC, "Audition complete - starting headers-first sync");
        mgr->mode = SYNC_MODE_HEADERS;
      }
    }
    break;
  }

  case SYNC_MODE_HEADERS: {
    /* Single-peer header sync: use one designated peer to avoid redundant requests.
     * Previously we asked ALL peers for the same headers, wasting 7/8 bandwidth. */
    peer_sync_state_t *ps = get_headers_sync_peer(mgr);
    if (!ps) {
      /* No suitable peer available yet */
      break;
    }

    /* Only send if no request in flight and retry interval passed (for timeouts) */
    if (!ps->headers_in_flight) {
      uint64_t now = plat_time_ms();
      if (now - ps->headers_sent_time >= SYNC_HEADER_RETRY_INTERVAL_MS) {
        ps->headers_in_flight = true;
        ps->headers_sent_time = now;

        /* Build block locator and send getheaders */
        if (mgr->callbacks.send_getheaders) {
          hash256_t locator[SYNC_MAX_LOCATOR_HASHES];
          size_t locator_len = 0;

          /* For headers-first sync, use best_header if we have received any
           * headers. Otherwise fall back to chainstate tip (genesis). */
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

      /* Compute bottleneck analysis */
      uint32_t total_blocks = mgr->blocks_ready + mgr->blocks_starved;
      float ready_pct = total_blocks > 0
                            ? (100.0f * mgr->blocks_ready / total_blocks)
                            : 0.0f;
      float starved_pct = total_blocks > 0
                              ? (100.0f * mgr->blocks_starved / total_blocks)
                              : 0.0f;

      log_info(LOG_COMP_SYNC,
               "[IBD] height=%u/%u (%.1f%%) | %.1f blk/s | "
               "pending=%zu inflight=%zu | ETA=%uh%02um",
               progress.tip_height, progress.best_header_height,
               progress.sync_percentage, blocks_per_sec,
               (size_t)progress.blocks_pending,
               (size_t)progress.blocks_in_flight, eta_hours, eta_mins);

      log_info(LOG_COMP_SYNC,
               "[BOTTLENECK] ready=%u(%.0f%%) starved=%u(%.0f%%) | "
               "validation=%llums starvation=%llums | peers=%zu",
               mgr->blocks_ready, ready_pct,
               mgr->blocks_starved, starved_pct,
               (unsigned long long)mgr->total_validation_ms,
               (unsigned long long)mgr->total_starvation_ms,
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
     * PERIODIC STORED BLOCK VALIDATION: Check if next-needed blocks are
     * already stored on disk and validate them. This handles the case where
     * blocks arrive out of order, get stored, and the "next" block never
     * arrives from the network because we already have it.
     *
     * Only check every 100ms to avoid excessive disk reads.
     */
    if (now - mgr->last_stored_block_check_time >= 100) {
      mgr->last_stored_block_check_time = now;

      uint32_t validated_height = chainstate_get_height(mgr->chainstate);
      uint32_t blocks_validated_this_tick = 0;
      const uint32_t max_per_tick = 100; /* Limit to avoid blocking */

      while (blocks_validated_this_tick < max_per_tick) {
        uint32_t next_height = validated_height + 1;
        hash256_t next_hash;
        bool found_block = false;

        block_index_map_t *idx_map =
            chainstate_get_block_index_map(mgr->chainstate);
        block_index_t *next_index = NULL;

        /* First try height callback (finds stored blocks from prev session) */
        if (mgr->callbacks.get_block_hash_at_height &&
            mgr->callbacks.get_block_hash_at_height(next_height, &next_hash,
                                                    mgr->callbacks.ctx) == ECHO_OK) {
          next_index = block_index_map_lookup(idx_map, &next_hash);
          if (next_index != NULL) {
            found_block = true;
          }
        }

        /* Fall back to queue lookup */
        if (!found_block &&
            block_queue_find_by_height(mgr->block_queue, next_height,
                                       &next_hash) == ECHO_OK) {
          next_index = block_index_map_lookup(idx_map, &next_hash);
          if (next_index != NULL) {
            found_block = true;
          }
        }

        if (!found_block) {
          break; /* Block not found */
        }

        if (next_index == NULL ||
            next_index->data_file == BLOCK_DATA_NOT_STORED) {
          break; /* Not stored yet */
        }

        /* Load and validate */
        block_t stored_block;
        block_init(&stored_block);
        echo_result_t load_result = mgr->callbacks.get_block(
            &next_hash, &stored_block, mgr->callbacks.ctx);

        if (load_result != ECHO_OK) {
          block_free(&stored_block);
          /* Mark as not stored so we don't keep retrying (file may be pruned) */
          if (next_index != NULL) {
            next_index->data_file = BLOCK_DATA_NOT_STORED;
            next_index->data_pos = 0;
          }
          break;
        }

        echo_result_t val_result = mgr->callbacks.validate_and_apply_block(
            &stored_block, next_index, mgr->callbacks.ctx);

        block_free(&stored_block);

        if (val_result != ECHO_OK) {
          log_warn(LOG_COMP_SYNC,
                   "Stored block at height %u failed validation: %d",
                   next_height, val_result);
          break;
        }

        /* Success! */
        block_queue_complete(mgr->block_queue, &next_hash);
        mgr->blocks_validated_total++;
        rate_window_record(mgr, now);
        mgr->last_progress_time = now;
        validated_height = next_height;
        blocks_validated_this_tick++;

        if (blocks_validated_this_tick == 1 ||
            blocks_validated_this_tick % 10 == 0) {
          log_info(LOG_COMP_SYNC,
                   "Validated stored block at height %u (periodic check)",
                   next_height);
        }
      }
    }

    /* Parallel Block Racing: Maximum redundancy.
     * Critical blocks race across all peers simultaneously.
     * E[min of 32 exponentials] = mean/32 ≈ 31ms per block.
     * request_racing_critical_blocks() is called inside request_blocks().
     */
    request_blocks(mgr);
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
  progress->blocks_validated = mgr->blocks_validated_total;
  progress->blocks_pending =
      (uint32_t)block_queue_pending_count(mgr->block_queue);
  progress->blocks_in_flight = block_queue_inflight_count(mgr->block_queue);

  /* Network state */
  progress->sync_peers = count_sync_peers(mgr);

  /* Chain info */
  chain_tip_t tip;
  if (chainstate_get_tip(mgr->chainstate, &tip) == ECHO_OK) {
    progress->tip_height = tip.height;
    progress->tip_work = tip.chainwork;
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
  return mgr && (mgr->mode == SYNC_MODE_PING_CONTEST ||
                 mgr->mode == SYNC_MODE_HEADERS ||
                 mgr->mode == SYNC_MODE_BLOCKS);
}

void sync_skip_ping_contest(sync_manager_t *mgr) {
  if (mgr) {
    mgr->skip_ping_contest = true;
  }
}

bool sync_is_audition_complete(const sync_manager_t *mgr) {
  return mgr && mgr->audition_complete;
}

/* ============================================================================
 * Helper Functions
 * ============================================================================
 */

const char *sync_mode_string(sync_mode_t mode) {
  switch (mode) {
  case SYNC_MODE_IDLE:
    return "IDLE";
  case SYNC_MODE_PING_CONTEST:
    return "PING_CONTEST";
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

void sync_get_metrics(const sync_manager_t *mgr, sync_metrics_t *metrics) {
  if (!metrics) {
    return;
  }

  /* Initialize with defaults */
  metrics->blocks_per_second = 0.0f;
  metrics->eta_seconds = 0;
  metrics->network_median_latency = 0;
  metrics->active_sync_peers = 0;
  metrics->mode_string = "idle";
  metrics->blocks_ready = 0;
  metrics->blocks_starved = 0;
  metrics->total_validation_ms = 0;
  metrics->total_starvation_ms = 0;

  if (!mgr) {
    return;
  }

  /* Get current progress for calculations */
  sync_progress_t progress;
  sync_get_progress(mgr, &progress);

  /* Mode string */
  metrics->mode_string = sync_mode_string(progress.mode);

  /* Calculate blocks per second using rolling window for accurate real-time rate */
  metrics->blocks_per_second = rate_window_get_rate(mgr);

  /* ETA in seconds - use rolling window rate for consistency with displayed speed.
   * Previously used overall average which was inflated by fast early blocks. */
  if (metrics->blocks_per_second > 0 && progress.best_header_height > progress.tip_height) {
    uint32_t remaining = progress.best_header_height - progress.tip_height;
    metrics->eta_seconds = (uint64_t)(remaining / metrics->blocks_per_second);
  }

  /* Network median latency from peer quality system */
  metrics->network_median_latency = mgr->network_median_latency_ms;

  /* Count active sync peers (those with blocks received) */
  uint32_t active = 0;
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].blocks_received > 0 || mgr->peers[i].latency_samples > 0) {
      active++;
    }
  }
  metrics->active_sync_peers = active;

  /* Bottleneck diagnostics */
  metrics->blocks_ready = mgr->blocks_ready;
  metrics->blocks_starved = mgr->blocks_starved;
  metrics->total_validation_ms = mgr->total_validation_ms;
  metrics->total_starvation_ms = mgr->total_starvation_ms;
}
