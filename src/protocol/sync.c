/**
 * Bitcoin Echo — Headers-First Initial Block Download Implementation
 *
 * This module implements headers-first sync as specified in whitepaper §7.3.
 *
 * Build once. Build right. Stop.
 */

#include "sync.h"
#include "block.h"
#include "chainstate.h"
#include "echo_types.h"
#include "log.h"
#include "peer.h"
#include "platform.h"
#include <stdbool.h>
#include <stdint.h>
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
};

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
 * Find best peer for block download (fewest blocks in flight).
 */
static peer_sync_state_t *find_best_block_peer(sync_manager_t *mgr) {
  peer_sync_state_t *best = NULL;
  size_t min_inflight = SIZE_MAX;
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
      /* Debug: log why peer is not ready */
      log_debug(LOG_COMP_SYNC, "Peer %s not_ready: state=%s",
                ps->peer->address, peer_state_string(ps->peer->state));
    } else if (!has_capacity) {
      no_capacity++;
    } else {
      candidates++;
      if (ps->blocks_in_flight_count < min_inflight) {
        min_inflight = ps->blocks_in_flight_count;
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

sync_manager_t *sync_create(chainstate_t *chainstate,
                            const sync_callbacks_t *callbacks) {
  if (!chainstate || !callbacks) {
    return NULL;
  }

  sync_manager_t *mgr = calloc(1, sizeof(sync_manager_t));
  if (!mgr) {
    return NULL;
  }

  mgr->block_queue = block_queue_create(SYNC_BLOCK_DOWNLOAD_WINDOW);
  if (!mgr->block_queue) {
    free(mgr);
    return NULL;
  }

  mgr->chainstate = chainstate;
  mgr->callbacks = *callbacks;
  mgr->mode = SYNC_MODE_IDLE;
  mgr->peer_count = 0;

  /* Initialize adaptive stalling timeout to 2 seconds */
  mgr->stalling_timeout_ms = SYNC_BLOCK_STALLING_TIMEOUT_MS;

  /* Initialize best header to current tip */
  mgr->best_header = chainstate_get_tip_index(chainstate);

  return mgr;
}

void sync_destroy(sync_manager_t *mgr) {
  if (!mgr) {
    return;
  }
  block_queue_destroy(mgr->block_queue);
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

  /* Peer is sync candidate if they have blocks we need */
  uint32_t our_height = chainstate_get_height(mgr->chainstate);
  ps->sync_candidate = (height > (int32_t)our_height);

  log_info(LOG_COMP_SYNC, "Added peer %s to sync_mgr: height=%d, our_height=%u, "
           "sync_candidate=%s, state=%s",
           peer->address, height, our_height,
           ps->sync_candidate ? "yes" : "no",
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

      /* Unassign any blocks from this peer */
      block_queue_unassign_peer(mgr->block_queue, peer);

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

  mgr->mode = SYNC_MODE_HEADERS;
  mgr->start_time = plat_time_ms();
  mgr->last_progress_time = mgr->start_time;

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

    /* Persist header to disk if callback provided */
    if (new_index && mgr->callbacks.store_header) {
      echo_result_t store_result =
          mgr->callbacks.store_header(header, new_index, mgr->callbacks.ctx);
      if (store_result != ECHO_OK && store_result != ECHO_ERR_EXISTS) {
        /* Log but don't fail - header is in memory which is what matters for
         * sync */
        log_warn(LOG_COMP_SYNC, "Failed to persist header at height %u: %d",
                 new_index->height, store_result);
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
        mgr->mode = SYNC_MODE_BLOCKS;
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

  /* Compute block hash */
  hash256_t block_hash;
  if (block_header_hash(&block->header, &block_hash) != ECHO_OK) {
    return ECHO_ERR_INVALID;
  }

  /* Check if this block was requested */
  bool found = false;
  for (size_t i = 0; i < ps->blocks_in_flight_count; i++) {
    if (memcmp(&ps->blocks_in_flight[i], &block_hash, sizeof(hash256_t)) == 0) {
      /* Remove from in-flight list */
      for (size_t j = i; j < ps->blocks_in_flight_count - 1; j++) {
        ps->blocks_in_flight[j] = ps->blocks_in_flight[j + 1];
        ps->block_request_time[j] = ps->block_request_time[j + 1];
      }
      ps->blocks_in_flight_count--;
      found = true;
      break;
    }
  }

  if (!found) {
    /* Unsolicited block - might still be useful */
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

    echo_result_t result = mgr->callbacks.validate_and_apply_block(
        block, block_index, mgr->callbacks.ctx);
    if (result != ECHO_OK) {
      /*
       * Block validation failed for a reason other than ordering.
       * Re-queue to try again later.
       */
      block_queue_unassign(mgr->block_queue, &block_hash);
      return ECHO_ERR_INVALID;
    }
    mgr->blocks_validated_total++;

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

        /* Search for a block at next_height with stored data */
        /* Note: We need to find BY HEIGHT which requires scanning */
        /* For now, check if we have any pending blocks at this height */
        hash256_t next_hash;
        if (block_queue_find_by_height(mgr->block_queue, next_height,
                                       &next_hash) == ECHO_OK) {
          next_index = block_index_map_lookup(idx_map, &next_hash);
          log_debug(LOG_COMP_SYNC,
                    "Found block at height %u in queue, index=%p, data_file=%u",
                    next_height, (void *)next_index,
                    next_index ? next_index->data_file : 0xFFFFFFFF);
        } else {
          /* Block not in queue - might already be validated or not known */
          log_debug(LOG_COMP_SYNC,
                    "Block at height %u not found in queue", next_height);
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
          break;
        }

        /* Validate and apply the stored block */
        echo_result_t val_result = mgr->callbacks.validate_and_apply_block(
            &stored_block, next_index, mgr->callbacks.ctx);

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
        mgr->last_progress_time = plat_time_ms();

        log_info(LOG_COMP_SYNC,
                 "Validated stored block at height %u (from disk)",
                 next_height);

        next_height++;
      }
    }
  }

  /* Only mark complete in queue AFTER successful validation */
  block_queue_complete(mgr->block_queue, &block_hash);

  mgr->blocks_received_total++;
  ps->blocks_received++;
  mgr->last_progress_time = plat_time_ms();

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

  /* Process header timeouts */
  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_sync_state_t *ps = &mgr->peers[i];

    if (ps->headers_in_flight &&
        (now - ps->headers_sent_time > SYNC_HEADERS_TIMEOUT_MS)) {
      ps->headers_in_flight = false;
      ps->timeout_count++;
    }

    /* Process block timeouts - fixed timeout, track per-peer stalls */
    for (size_t j = 0; j < ps->blocks_in_flight_count;) {
      if (now - ps->block_request_time[j] > SYNC_BLOCK_STALLING_TIMEOUT_MS) {
        /* Timeout - unassign block and re-queue */
        hash256_t stalled_hash = ps->blocks_in_flight[j];
        block_queue_unassign(mgr->block_queue, &stalled_hash);

        ps->timeout_count++;
        log_info(LOG_COMP_SYNC,
                 "Block stalled from peer after %llu ms (peer stalls: %u)",
                 (unsigned long long)(now - ps->block_request_time[j]),
                 ps->timeout_count);

        /* Remove from peer's in-flight list */
        for (size_t k = j; k < ps->blocks_in_flight_count - 1; k++) {
          ps->blocks_in_flight[k] = ps->blocks_in_flight[k + 1];
          ps->block_request_time[k] = ps->block_request_time[k + 1];
        }
        ps->blocks_in_flight_count--;

        /* After 3 stalls, disconnect this slow peer - we have thousands more!
         * Don't waste time with unresponsive peers during IBD.
         */
        if (ps->timeout_count >= 3 && ps->peer) {
          log_info(LOG_COMP_SYNC,
                   "Disconnecting slow peer after %u stalls - plenty more available",
                   ps->timeout_count);
          peer_disconnect(ps->peer, PEER_DISCONNECT_MISBEHAVING,
                          "Too many block stalls during IBD");
        }
      } else {
        j++;
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

  /* Calculate target range */
  uint32_t start_height = tip_height + 1;
  uint32_t end_height = tip_height + SYNC_BLOCK_DOWNLOAD_WINDOW;
  if (end_height > mgr->best_header->height) {
    end_height = mgr->best_header->height;
  }

  if (start_height > end_height) {
    /* Already fully synced */
    return;
  }

  /*
   * Use direct height lookup via callback if available (much faster for
   * large height gaps). Falls back to walking prev pointers if not.
   */
  hash256_t to_queue[SYNC_BLOCK_DOWNLOAD_WINDOW];
  uint32_t heights[SYNC_BLOCK_DOWNLOAD_WINDOW];
  size_t to_queue_count = 0;

  if (mgr->callbacks.get_block_hash_at_height) {
    /* Fast path: query database by height directly */
    for (uint32_t h = start_height;
         h <= end_height && to_queue_count < SYNC_BLOCK_DOWNLOAD_WINDOW; h++) {
      hash256_t hash;
      echo_result_t cb_result = mgr->callbacks.get_block_hash_at_height(
          h, &hash, mgr->callbacks.ctx);
      if (cb_result != ECHO_OK) {
        if (h <= 5) {
          log_warn(LOG_COMP_SYNC, "queue_blocks: height %u hash lookup failed: %d",
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
        if (!in_storage) {
          to_queue[to_queue_count] = hash;
          heights[to_queue_count] = h;
          to_queue_count++;
          if (h <= 5) {
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
          if (h == validated_height + 1 && mgr->callbacks.validate_and_apply_block) {
            block_index_map_t *idx_map = chainstate_get_block_index_map(mgr->chainstate);
            block_index_t *block_idx = block_index_map_lookup(idx_map, &hash);

            if (block_idx != NULL) {
              log_info(LOG_COMP_SYNC,
                       "Validating stored block at height %u (kickstart)", h);

              echo_result_t val_result = mgr->callbacks.validate_and_apply_block(
                  &stored, block_idx, mgr->callbacks.ctx);

              if (val_result == ECHO_OK) {
                mgr->blocks_validated_total++;
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
  } else {
    /* Slow path: walk back from best_header (for very old code) */
    block_index_t *idx = mgr->best_header;

    /* First, walk back to reach our target range (skip higher blocks) */
    while (idx && idx->height > end_height) {
      idx = idx->prev;
    }

    /* Collect blocks to queue (walking backward, we'll reverse later) */
    while (idx && idx->height >= start_height &&
           to_queue_count < SYNC_BLOCK_DOWNLOAD_WINDOW) {
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
 * Request blocks from peers.
 */
static void request_blocks(sync_manager_t *mgr) {
  /* Track blocks to request per peer for batched getdata */
  hash256_t blocks_for_peer[SYNC_MAX_BLOCKS_PER_PEER];
  size_t blocks_count = 0;
  peer_sync_state_t *current_peer = NULL;

  uint32_t validated_height = chainstate_get_height(mgr->chainstate);

  /*
   * CRITICAL PATH OPTIMIZATION: If the next needed block (validated_height+1)
   * is in-flight and taking >1 second, request it from ALL other peers too.
   * First response wins. This dramatically reduces stall time.
   */
  uint32_t next_needed = validated_height + 1;
  hash256_t next_hash;
  if (block_queue_find_by_height(mgr->block_queue, next_needed, &next_hash) ==
      ECHO_OK) {
    uint64_t now = plat_time_ms();

    /* Find if this block is in-flight and stalling */
    for (size_t i = 0; i < mgr->peer_count; i++) {
      peer_sync_state_t *ps = &mgr->peers[i];
      for (size_t j = 0; j < ps->blocks_in_flight_count; j++) {
        if (memcmp(&ps->blocks_in_flight[j], &next_hash, sizeof(hash256_t)) ==
            0) {
          /* Found it - check if stalling (>500ms) */
          if (now - ps->block_request_time[j] > 500) {
            /* Request from ALL other peers immediately */
            for (size_t k = 0; k < mgr->peer_count; k++) {
              if (k == i)
                continue; /* Skip the original peer */
              peer_sync_state_t *other = &mgr->peers[k];
              if (other->peer && other->peer->state == PEER_STATE_READY &&
                  other->blocks_in_flight_count < SYNC_MAX_BLOCKS_PER_PEER) {
                /* Request the blocking block from this peer too */
                if (mgr->callbacks.send_getdata_blocks) {
                  log_info(
                      LOG_COMP_SYNC,
                      "Parallel request for blocking block %u from extra peer",
                      next_needed);
                  mgr->callbacks.send_getdata_blocks(other->peer, &next_hash, 1,
                                                     mgr->callbacks.ctx);
                }
              }
            }
          }
          goto done_parallel_check;
        }
      }
    }
  }
done_parallel_check:;  /* Empty statement after label */

  /* Log entry state */
  size_t pending = block_queue_pending_count(mgr->block_queue);
  size_t inflight = block_queue_inflight_count(mgr->block_queue);
  if (pending > 0 && inflight < SYNC_MAX_PARALLEL_BLOCKS) {
    log_info(LOG_COMP_SYNC,
             "request_blocks entry: peer_count=%zu, pending=%zu, inflight=%zu",
             mgr->peer_count, pending, inflight);
  }

  /*
   * Request blocks within the download window of the validated tip.
   * SYNC_BLOCK_DOWNLOAD_WINDOW (1024) matches Bitcoin Core.
   */
  uint32_t max_request_height = validated_height + SYNC_BLOCK_DOWNLOAD_WINDOW;

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

void sync_tick(sync_manager_t *mgr) {
  if (!mgr) {
    return;
  }

  sync_process_timeouts(mgr);

  switch (mgr->mode) {
  case SYNC_MODE_HEADERS: {
    /* Request headers from peers that aren't already syncing */
    for (size_t i = 0; i < mgr->peer_count; i++) {
      peer_sync_state_t *ps = &mgr->peers[i];
      if (ps->sync_candidate && !ps->headers_in_flight &&
          peer_is_ready(ps->peer)) {
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
    }
    break;
  }

  case SYNC_MODE_BLOCKS: {
    log_info(LOG_COMP_SYNC,
             "SYNC_MODE_BLOCKS: best_header=%p, pending=%zu, inflight=%zu",
             (void *)mgr->best_header,
             block_queue_pending_count(mgr->block_queue),
             block_queue_inflight_count(mgr->block_queue));

    uint64_t now = plat_time_ms();
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

    /* Request blocks from peers */
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
  return mgr &&
         (mgr->mode == SYNC_MODE_HEADERS || mgr->mode == SYNC_MODE_BLOCKS);
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
