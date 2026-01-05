/**
 * Bitcoin Echo — PULL-Based Block Download Manager
 *
 * Sequential batch download with cooperative work distribution:
 *
 * - Work is organized as BATCHES, not individual items
 * - Peers PULL work when idle, coordinator doesn't push
 * - Starved peers WAIT for work (cooperative, not punitive)
 * - Only truly stalled peers (0 B/s) are disconnected
 * - Sequential queueing ensures blocks arrive in approximate order
 *
 * See IBD-BATCH-ARCHITECTURE.md for design details.
 *
 * Build once. Build right. Stop.
 */

#define LOG_COMPONENT LOG_COMP_SYNC

#include "download_mgr.h"
#include "log.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal Structures
 * ============================================================================
 */

/**
 * Batch queue node (doubly-linked list for efficient removal).
 */
typedef struct batch_node {
  work_batch_t batch;
  struct batch_node *next;
  struct batch_node *prev;
} batch_node_t;

/**
 * Height bitmap for O(1) lookup of tracked heights.
 * Replaces O(batches * batch_size) scan with O(1) bit check.
 */
#define HEIGHT_BITMAP_CAPACITY (1024 * 1024) /* Track up to 1M heights */

/**
 * Download manager internal state.
 *
 * Cooperative model: Batch queue + peer performance tracking.
 */
struct download_mgr {
  /* Callbacks for network operations */
  download_callbacks_t callbacks;

  /* Batch queue (doubly-linked list) */
  batch_node_t *queue_head; /* Front of queue (oldest batches) */
  batch_node_t *queue_tail; /* Back of queue (newest batches) */
  size_t queue_count;       /* Number of batches in queue */

  /* Height tracking */
  uint32_t lowest_pending_height;  /* Lowest height in queue/assigned */
  uint32_t highest_queued_height;  /* Highest height added */

  /* Height bitmap for O(1) has_height lookup.
   * Bit N is set if height (bitmap_base + N) is being tracked.
   * Cleared when batch containing that height completes. */
  uint8_t *height_bitmap;
  uint32_t bitmap_base;      /* Height represented by bit 0 */
  uint32_t bitmap_capacity;  /* Number of bits allocated */

  /* Peer performance tracking */
  peer_perf_t peers[DOWNLOAD_MAX_PEERS];
  size_t peer_count;
};

/* ============================================================================
 * Internal Helpers - Batch Queue Operations
 * ============================================================================
 */

/**
 * Allocate a new batch node.
 */
static batch_node_t *batch_node_create(void) {
  batch_node_t *node = calloc(1, sizeof(batch_node_t));
  return node;
}

/**
 * Free a batch node.
 */
static void batch_node_destroy(batch_node_t *node) { free(node); }


/**
 * Get batch size for a given block height.
 *
 * Bitcoin Core uses MAX_BLOCKS_IN_TRANSIT_PER_PEER = 16 as a fixed limit,
 * but our testing found 8 blocks optimal for minimizing head-of-line
 * blocking while avoiding excessive getdata overhead.
 */
static size_t get_batch_size_for_height(uint32_t height) {
  (void)height; /* Unused - fixed batch size */
  return DOWNLOAD_BATCH_SIZE;
}

/**
 * Add batch to end of queue.
 */
static void queue_push_back(download_mgr_t *mgr, batch_node_t *node) {
  node->next = NULL;
  node->prev = mgr->queue_tail;

  if (mgr->queue_tail != NULL) {
    mgr->queue_tail->next = node;
  } else {
    mgr->queue_head = node;
  }
  mgr->queue_tail = node;
  mgr->queue_count++;
}

/**
 * Add batch to front of queue (for returned work).
 */
static void queue_push_front(download_mgr_t *mgr, batch_node_t *node) {
  node->prev = NULL;
  node->next = mgr->queue_head;

  if (mgr->queue_head != NULL) {
    mgr->queue_head->prev = node;
  } else {
    mgr->queue_tail = node;
  }
  mgr->queue_head = node;
  mgr->queue_count++;
}

/**
 * Remove batch from queue (for assignment to peer).
 * Returns the removed node (caller takes ownership).
 */
static batch_node_t *queue_pop_front(download_mgr_t *mgr) {
  if (mgr->queue_head == NULL) {
    return NULL;
  }

  batch_node_t *node = mgr->queue_head;
  mgr->queue_head = node->next;

  if (mgr->queue_head != NULL) {
    mgr->queue_head->prev = NULL;
  } else {
    mgr->queue_tail = NULL;
  }

  node->next = NULL;
  node->prev = NULL;
  mgr->queue_count--;

  return node;
}

/* ============================================================================
 * Internal Helpers - Height Bitmap Operations
 * ============================================================================
 */

/**
 * Initialize height bitmap for tracking.
 * Always starts from height 0 to handle any height range.
 */
static bool bitmap_init(download_mgr_t *mgr) {
  if (mgr->height_bitmap != NULL) {
    return true; /* Already initialized */
  }

  size_t bytes_needed = HEIGHT_BITMAP_CAPACITY / 8;
  mgr->height_bitmap = calloc(1, bytes_needed);
  if (mgr->height_bitmap == NULL) {
    LOG_WARN("download_mgr: FAILED to allocate height bitmap (%zu bytes)", bytes_needed);
    return false;
  }

  /* Always start from height 0 - we can track heights 0 to ~1M */
  mgr->bitmap_base = 0;
  mgr->bitmap_capacity = HEIGHT_BITMAP_CAPACITY;
  LOG_INFO("download_mgr: initialized height bitmap (%zu KB)", bytes_needed / 1024);
  return true;
}

/**
 * Set bit for a height (mark as tracked).
 */
static void bitmap_set(download_mgr_t *mgr, uint32_t height) {
  if (mgr->height_bitmap == NULL) {
    return;
  }

  if (height < mgr->bitmap_base) {
    return; /* Below range */
  }

  uint32_t offset = height - mgr->bitmap_base;
  if (offset >= mgr->bitmap_capacity) {
    return; /* Above range */
  }

  mgr->height_bitmap[offset / 8] |= (1 << (offset % 8));
}

/**
 * Clear bit for a height (mark as no longer tracked).
 */
static void bitmap_clear(download_mgr_t *mgr, uint32_t height) {
  if (mgr->height_bitmap == NULL) {
    return;
  }

  if (height < mgr->bitmap_base) {
    return;
  }

  uint32_t offset = height - mgr->bitmap_base;
  if (offset >= mgr->bitmap_capacity) {
    return;
  }

  mgr->height_bitmap[offset / 8] &= ~(1 << (offset % 8));
}

/**
 * Check if height is tracked (O(1) lookup).
 */
static bool bitmap_has(const download_mgr_t *mgr, uint32_t height) {
  static uint64_t call_count = 0;
  static uint64_t null_bitmap = 0;
  static uint64_t found_count = 0;

  call_count++;

  if (mgr->height_bitmap == NULL) {
    null_bitmap++;
    if (null_bitmap % 10000 == 1) {
      LOG_WARN("bitmap_has: NULL bitmap! calls=%llu, null=%llu", (unsigned long long)call_count, (unsigned long long)null_bitmap);
    }
    return false;
  }

  if (height < mgr->bitmap_base) {
    return false;
  }

  uint32_t offset = height - mgr->bitmap_base;
  if (offset >= mgr->bitmap_capacity) {
    return false;
  }

  bool found = (mgr->height_bitmap[offset / 8] & (1 << (offset % 8))) != 0;
  if (found) {
    found_count++;
  }

  /* Log stats periodically */
  if (call_count % 100000 == 0) {
    LOG_INFO("bitmap_has stats: calls=%llu, found=%llu (%.1f%%)",
             (unsigned long long)call_count, (unsigned long long)found_count,
             100.0 * (double)found_count / (double)call_count);
  }

  return found;
}

/**
 * Clear all bits for heights in a batch.
 * Called when batch is completed and being freed.
 */
static void bitmap_clear_batch(download_mgr_t *mgr, const work_batch_t *batch) {
  if (batch == NULL) {
    return;
  }
  for (size_t i = 0; i < batch->count; i++) {
    bitmap_clear(mgr, batch->heights[i]);
  }
}

/* ============================================================================
 * Internal Helpers - Peer Operations
 * ============================================================================
 */

/**
 * Find peer_perf slot by peer pointer.
 * Returns NULL if not found.
 */
static peer_perf_t *find_peer_perf(download_mgr_t *mgr, const peer_t *peer) {
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].peer == peer) {
      return &mgr->peers[i];
    }
  }
  return NULL;
}

/**
 * Find peer_perf slot by peer pointer (const version).
 * Returns NULL if not found.
 */
static const peer_perf_t *find_peer_perf_const(const download_mgr_t *mgr,
                                               const peer_t *peer) {
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].peer == peer) {
      return &mgr->peers[i];
    }
  }
  return NULL;
}

/**
 * Compact the peers array by removing NULL entries.
 */
static void compact_peers(download_mgr_t *mgr) {
  size_t write = 0;
  for (size_t read = 0; read < mgr->peer_count; read++) {
    if (mgr->peers[read].peer != NULL) {
      if (write != read) {
        mgr->peers[write] = mgr->peers[read];
      }
      write++;
    }
  }
  mgr->peer_count = write;
}

/**
 * Update performance window for a peer.
 * Called when recording bytes or on timer.
 *
 * Once a peer has delivered bytes (rate > 0), they're marked as "has_reported"
 * and become subject to statistical checks. Peers who never delivered aren't
 * in the performance pool yet.
 */
static void update_peer_window(peer_perf_t *perf, uint64_t now) {
  uint64_t elapsed = now - perf->window_start_time;

  if (elapsed >= DOWNLOAD_PERF_WINDOW_MS) {
    /* Calculate bytes/second for the completed window */
    perf->bytes_per_second =
        (float)perf->bytes_this_window / ((float)elapsed / 1000.0f);

    /* Mark as "reported" once we've proven we can deliver */
    if (perf->bytes_per_second > 0.0f) {
      perf->has_reported = true;
    }

    /* Reset window */
    perf->bytes_this_window = 0;
    perf->window_start_time = now;
  }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================
 */

download_mgr_t *download_mgr_create(const download_callbacks_t *callbacks) {
  if (callbacks == NULL) {
    return NULL;
  }

  download_mgr_t *mgr = calloc(1, sizeof(download_mgr_t));
  if (mgr == NULL) {
    return NULL;
  }

  mgr->callbacks = *callbacks;
  return mgr;
}

void download_mgr_destroy(download_mgr_t *mgr) {
  if (mgr == NULL) {
    return;
  }

  /* Free all queued batches */
  while (mgr->queue_head != NULL) {
    batch_node_t *node = queue_pop_front(mgr);
    batch_node_destroy(node);
  }

  /* Free all assigned batches (in peer slots) */
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].batch != NULL) {
      /* batch is first field in batch_node_t, so cast is direct */
      batch_node_t *node = (batch_node_t *)(void *)mgr->peers[i].batch;
      batch_node_destroy(node);
      mgr->peers[i].batch = NULL;
    }
  }

  /* Free height bitmap */
  free(mgr->height_bitmap);

  free(mgr);
}

void download_mgr_add_peer(download_mgr_t *mgr, peer_t *peer) {
  if (mgr == NULL || peer == NULL) {
    return;
  }

  /* Check if already tracked */
  if (find_peer_perf(mgr, peer) != NULL) {
    return;
  }

  /* Find empty slot */
  if (mgr->peer_count >= DOWNLOAD_MAX_PEERS) {
    LOG_WARN("download_mgr: max peers reached, cannot add peer");
    return;
  }

  peer_perf_t *perf = &mgr->peers[mgr->peer_count++];
  memset(perf, 0, sizeof(peer_perf_t));
  perf->peer = peer;
  perf->batch = NULL; /* Idle - will pull work */
  perf->window_start_time = plat_time_ms();
  perf->last_delivery_time = plat_time_ms();

  LOG_DEBUG("download_mgr: added peer, total=%zu", mgr->peer_count);
}

void download_mgr_remove_peer(download_mgr_t *mgr, peer_t *peer) {
  if (mgr == NULL || peer == NULL) {
    return;
  }

  peer_perf_t *perf = find_peer_perf(mgr, peer);
  if (perf == NULL) {
    return;
  }

  /* Return any assigned batch to the queue */
  if (perf->batch != NULL) {
    /* batch is first field in batch_node_t, so cast is direct */
    batch_node_t *node = (batch_node_t *)(void *)perf->batch;
    uint32_t batch_start = node->batch.heights[0];
    uint32_t batch_end = batch_start + (uint32_t)node->batch.count - 1;
    node->batch.assigned_time = 0; /* Mark as unassigned */
    queue_push_front(mgr, node);   /* Return to front of queue */
    perf->batch = NULL;
    LOG_INFO("download_mgr: returned batch [%u-%u] to queue from removed peer",
             batch_start, batch_end);
  }

  /* Mark slot as empty */
  perf->peer = NULL;
  compact_peers(mgr);

  LOG_DEBUG("download_mgr: removed peer, total=%zu", mgr->peer_count);
}

size_t download_mgr_add_work(download_mgr_t *mgr, const hash256_t *hashes,
                             const uint32_t *heights, size_t count) {
  if (mgr == NULL || hashes == NULL || heights == NULL || count == 0) {
    return 0;
  }

  /* Check queue capacity */
  if (mgr->queue_count >= DOWNLOAD_MAX_BATCHES) {
    LOG_WARN("download_mgr: batch queue full (%zu batches)", mgr->queue_count);
    return 0;
  }

  /* Initialize height bitmap on first add (lazy init with first height as base) */
  if (mgr->height_bitmap == NULL) {
    bitmap_init(mgr);
  }

  size_t added = 0;
  size_t i = 0;

  while (i < count && mgr->queue_count < DOWNLOAD_MAX_BATCHES) {
    /* Create a new batch */
    batch_node_t *node = batch_node_create();
    if (node == NULL) {
      LOG_WARN("download_mgr: failed to allocate batch node");
      break;
    }

    /* Determine batch size based on starting height */
    size_t target_batch_size = get_batch_size_for_height(heights[i]);

    /* Fill the batch with up to target_batch_size blocks */
    size_t batch_count = 0;
    while (batch_count < target_batch_size && i < count) {
      memcpy(&node->batch.hashes[batch_count], &hashes[i], sizeof(hash256_t));
      node->batch.heights[batch_count] = heights[i];
      /* Mark height as tracked in bitmap for O(1) lookup */
      bitmap_set(mgr, heights[i]);
      batch_count++;
      i++;
      added++;
    }

    node->batch.count = batch_count;
    node->batch.remaining = batch_count;
    node->batch.assigned_time = 0; /* Not assigned yet */

    /* Update height tracking */
    if (mgr->lowest_pending_height == 0 ||
        node->batch.heights[0] < mgr->lowest_pending_height) {
      mgr->lowest_pending_height = node->batch.heights[0];
    }
    if (node->batch.heights[batch_count - 1] > mgr->highest_queued_height) {
      mgr->highest_queued_height = node->batch.heights[batch_count - 1];
    }

    /* Add to queue */
    queue_push_back(mgr, node);
  }

  if (added > 0) {
    LOG_DEBUG("download_mgr: added %zu blocks, queue now has %zu batches", added,
              mgr->queue_count);
  }

  return added;
}

/* ============================================================================
 * PULL Model API Implementation
 * ============================================================================
 */

bool download_mgr_peer_request_work(download_mgr_t *mgr, peer_t *peer) {
  if (mgr == NULL || peer == NULL) {
    return false;
  }

  peer_perf_t *perf = find_peer_perf(mgr, peer);
  if (perf == NULL) {
    LOG_WARN("download_mgr: unknown peer requesting work");
    return false;
  }

  /* Peer should be idle when requesting work */
  if (perf->batch != NULL && perf->batch->remaining > 0) {
    LOG_DEBUG("download_mgr: peer still has work, ignoring request");
    return false;
  }

  /* If peer had a completed batch, free it */
  if (perf->batch != NULL) {
    batch_node_t *old_node = (batch_node_t *)(void *)perf->batch;
    uint32_t old_start = old_node->batch.heights[0];
    uint32_t old_end = old_start + (uint32_t)old_node->batch.count - 1;
    LOG_INFO("download_mgr: freeing completed batch [%u-%u]", old_start, old_end);
    /* Clear bitmap bits for completed batch heights */
    bitmap_clear_batch(mgr, &old_node->batch);
    batch_node_destroy(old_node);
    perf->batch = NULL;
  }

  /* Try to get a batch from the queue */
  if (mgr->queue_head == NULL) {
    /* Queue empty - peer is starved */
    LOG_DEBUG("download_mgr: no work available, peer starved");
    return false;
  }

  /* Pop batch from queue and assign to peer */
  batch_node_t *node = queue_pop_front(mgr);

  /* Assign batch to peer */
  uint64_t now = plat_time_ms();
  node->batch.assigned_time = now;
  perf->batch = &node->batch;
  perf->last_delivery_time = now;

  /* Track first work assignment for grace period (set once, never reset) */
  if (perf->first_work_time == 0) {
    perf->first_work_time = now;
  }

  /* NOTE: We do NOT reset remaining on reassignment.
   *
   * When a batch is reassigned (after peer disconnect), the received[] bitmap
   * preserves which blocks we already have. We request ALL blocks again
   * (storage layer deduplicates), but block_received() only decrements
   * remaining for blocks not already marked received.
   */

  /* Send getdata for all blocks in batch */
  if (mgr->callbacks.send_getdata != NULL) {
    mgr->callbacks.send_getdata(peer, node->batch.hashes, node->batch.count,
                                mgr->callbacks.ctx);
  }

  uint32_t batch_start = node->batch.heights[0];
  uint32_t batch_end = batch_start + (uint32_t)node->batch.count - 1;
  LOG_INFO("download_mgr: assigned batch [%u-%u] (%zu blocks) to peer",
           batch_start, batch_end, node->batch.count);
  return true;
}

bool download_mgr_block_received(download_mgr_t *mgr, peer_t *peer,
                                 const hash256_t *hash, size_t block_size) {
  if (mgr == NULL || peer == NULL || hash == NULL) {
    return false;
  }

  peer_perf_t *perf = find_peer_perf(mgr, peer);
  if (perf == NULL) {
    /* Unknown peer - accept block but can't track */
    LOG_DEBUG("download_mgr: block from unknown peer");
    return true;
  }

  /* Update performance tracking */
  uint64_t now = plat_time_ms();
  perf->bytes_this_window += block_size;
  perf->last_delivery_time = now;
  update_peer_window(perf, now);

  /* First, try to find block in the delivering peer's batch (most common case) */
  if (perf->batch != NULL) {
    for (size_t i = 0; i < perf->batch->count; i++) {
      if (memcmp(&perf->batch->hashes[i], hash, sizeof(hash256_t)) == 0) {
        /* Found it - check if already received (duplicate) */
        if (perf->batch->received[i]) {
          LOG_DEBUG("download_mgr: duplicate block at index %zu (already received), "
                    "remaining=%zu unchanged",
                    i, perf->batch->remaining);
          return false; /* Duplicate - don't count */
        }

        /* First time receiving this block - mark received and decrement */
        perf->batch->received[i] = true;
        if (perf->batch->remaining > 0) {
          perf->batch->remaining--;
        }
        /* Clear bitmap bit immediately so collect_gaps won't see this as tracked */
        bitmap_clear(mgr, perf->batch->heights[i]);
        LOG_DEBUG("download_mgr: block received at index %zu, batch remaining=%zu",
                  i, perf->batch->remaining);
        return true;
      }
    }
  }

  /* Block not in delivering peer's batch (or peer has no batch).
   * This happens during DRAIN mode when idle peers fulfill redundant requests.
   * Search ALL assigned batches to find and mark the block as received. */
  for (size_t p = 0; p < mgr->peer_count; p++) {
    peer_perf_t *other = &mgr->peers[p];
    if (other->peer == NULL || other->batch == NULL) {
      continue;
    }
    /* Don't re-check the delivering peer's batch */
    if (other == perf) {
      continue;
    }

    for (size_t i = 0; i < other->batch->count; i++) {
      if (memcmp(&other->batch->hashes[i], hash, sizeof(hash256_t)) == 0) {
        /* Found it in another peer's batch */
        if (other->batch->received[i]) {
          LOG_DEBUG("download_mgr: duplicate block (already in peer %p batch)",
                    (void *)other->peer);
          return false; /* Duplicate - don't count */
        }

        /* Mark as received in the owning batch */
        other->batch->received[i] = true;
        if (other->batch->remaining > 0) {
          other->batch->remaining--;
        }
        /* Clear bitmap bit immediately */
        bitmap_clear(mgr, other->batch->heights[i]);
        LOG_DEBUG("download_mgr: DRAIN block received via redundant request, "
                  "owning batch remaining=%zu",
                  other->batch->remaining);
        return true;
      }
    }
  }

  /* Block not in any batch - truly late delivery or unrequested */
  LOG_DEBUG("download_mgr: block not in any batch (late delivery)");
  return true;
}

bool download_mgr_peer_is_idle(const download_mgr_t *mgr, const peer_t *peer) {
  if (mgr == NULL || peer == NULL) {
    return true;
  }

  const peer_perf_t *perf = find_peer_perf_const(mgr, peer);
  if (perf == NULL) {
    return true; /* Unknown peer considered idle */
  }

  /* Idle if no batch OR batch is complete (all blocks received) */
  return perf->batch == NULL || perf->batch->remaining == 0;
}

size_t download_mgr_check_performance(download_mgr_t *mgr) {
  if (mgr == NULL) {
    return 0;
  }

  uint64_t now = plat_time_ms();
  size_t dropped = 0;

  /* Phase 1: Update windows for all peers with work */
  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_perf_t *perf = &mgr->peers[i];
    if (perf->peer != NULL && perf->batch != NULL) {
      update_peer_window(perf, now);
    }
  }

  /* Phase 2: Collect rates using self-selection model.
   *
   * Only peers who have successfully reported a positive rate are subject
   * to statistical checks. Peers who never delivered are not yet in the
   * performance pool.
   *
   * A peer with has_reported=true but current rate=0 is STALLED (used to
   * deliver but stopped). A peer with has_reported=false is still warming
   * up and we don't penalize them. */
  float rates[DOWNLOAD_MAX_PEERS];
  peer_perf_t *peers_with_rates[DOWNLOAD_MAX_PEERS];
  peer_perf_t *stalled_peers[DOWNLOAD_MAX_PEERS];
  size_t rate_count = 0;
  size_t stalled_count = 0;
  size_t reporters = 0; /* Peers who have ever reported (in the speeds_ pool) */

  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_perf_t *perf = &mgr->peers[i];
    if (perf->peer == NULL || perf->batch == NULL) {
      continue;
    }

    /* Skip peers who completed their batch - they're idle waiting for new work,
     * not stalled. Their batch->remaining == 0 but batch != NULL until we
     * call download_mgr_get_batch() to assign them new work. */
    if (perf->batch->remaining == 0) {
      continue; /* Completed batch, idle not stalled */
    }

    /* Only peers who have proven they can deliver are in the performance
     * pool. Peers who never delivered any bytes are still warming up and
     * aren't penalized. */
    if (!perf->has_reported) {
      continue; /* Not in the speeds_ pool yet */
    }

    reporters++;

    if (perf->bytes_per_second == 0.0f) {
      /* Was delivering, now stopped - stalled */
      stalled_peers[stalled_count++] = perf;
    } else {
      /* Still delivering - add to rates for statistical check */
      rates[rate_count] = perf->bytes_per_second;
      peers_with_rates[rate_count] = perf;
      rate_count++;
    }
  }

  /* Need minimum peers in the pool to function.
   * If reporters <= 3, don't drop anyone - preserve what we have. */
  if (reporters <= DOWNLOAD_MIN_PEERS_TO_KEEP) {
    LOG_DEBUG("download_mgr: only %zu reporters, skipping performance check",
              reporters);
    return 0;
  }

  /* Phase 3: Disconnect stalled peers (had rate > 0, now rate = 0).
   * These are peers who WERE delivering but stopped.
   *
   * IMPORTANT: Check last_delivery_time, not just bytes_per_second.
   * A peer with rate=0 might have just finished their batch and be
   * waiting for new blocks - they're not truly stalled. Only disconnect
   * if they haven't delivered for 2x the window (20 seconds). */
  for (size_t i = 0; i < stalled_count; i++) {
    if (reporters - dropped <= DOWNLOAD_MIN_PEERS_TO_KEEP) {
      LOG_DEBUG("download_mgr: keeping stalled peer to maintain minimum");
      break;
    }

    peer_perf_t *perf = stalled_peers[i];

    /* Check if peer recently delivered - if so, they're just between batches */
    uint64_t since_last_delivery = now - perf->last_delivery_time;
    if (perf->last_delivery_time > 0 &&
        since_last_delivery < (uint64_t)(DOWNLOAD_PERF_WINDOW_MS * 2)) {
      LOG_INFO("download_mgr: peer shows 0 B/s but delivered %llu ms ago, "
               "keeping (between batches)",
               (unsigned long long)since_last_delivery);
      continue; /* Not truly stalled, just between batches */
    }

    batch_node_t *node = (batch_node_t *)(void *)perf->batch;
    uint32_t batch_start = node->batch.heights[0];
    uint32_t batch_end = batch_start + (uint32_t)node->batch.count - 1;

    LOG_INFO("download_mgr: peer truly stalled (0 B/s, last delivery %llu ms ago), "
             "returning batch [%u-%u] to queue",
             (unsigned long long)since_last_delivery, batch_start, batch_end);

    node->batch.assigned_time = 0;
    queue_push_front(mgr, node);
    perf->batch = NULL;

    if (mgr->callbacks.disconnect_peer != NULL) {
      mgr->callbacks.disconnect_peer(perf->peer, "stalled (0 B/s)",
                                     mgr->callbacks.ctx);
    }
    dropped++;
  }

  /* Speed-based eviction removed (2025-12-31).
   *
   * Previously we calculated mean/stddev and kicked peers below a deviation
   * threshold. This was counterproductive because:
   *
   * 1. Our download manager already handles slow peers gracefully - it gives
   *    them smaller batches, steals work if they stall, and races critical
   *    batches. Slow peers still contribute blocks.
   *
   * 2. When peers have similar speeds (low stddev), the threshold approaches
   *    the mean, causing peers at 99% of average to be evicted. We observed
   *    peers kicked for being 0.2% below threshold!
   *
   * 3. More slow peers beats fewer fast peers for parallelism:
   *    80 peers × 50% speed = 4000 throughput units
   *    5 peers × 100% speed = 500 throughput units
   *
   * 4. Active peers discovered during IBD are scarce and valuable. We should
   *    only disconnect truly stalled (0 B/s) or malicious peers, which is
   *    handled by Phase 3 above and protocol error detection.
   *
   * The stalled peer detection (Phase 3) remains - peers delivering 0 bytes
   * over an extended period are genuinely stuck and should be replaced.
   */

  if (dropped > 0) {
    LOG_INFO("download_mgr: performance check dropped %zu stalled peers", dropped);
  }

  return dropped;
}

/* ============================================================================
 * Query Functions
 * ============================================================================
 */

size_t download_mgr_queue_count(const download_mgr_t *mgr) {
  return mgr != NULL ? mgr->queue_count : 0;
}

size_t download_mgr_assigned_count(const download_mgr_t *mgr) {
  if (mgr == NULL) {
    return 0;
  }

  size_t count = 0;
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].peer != NULL && mgr->peers[i].batch != NULL) {
      count++;
    }
  }
  return count;
}

size_t download_mgr_pending_blocks(const download_mgr_t *mgr) {
  if (mgr == NULL) {
    return 0;
  }

  size_t total = 0;

  /* Count blocks in queue */
  for (batch_node_t *node = mgr->queue_head; node != NULL; node = node->next) {
    total += node->batch.remaining;
  }

  /* Count blocks assigned to peers */
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].batch != NULL) {
      total += mgr->peers[i].batch->remaining;
    }
  }

  return total;
}

size_t download_mgr_active_peer_count(const download_mgr_t *mgr) {
  if (mgr == NULL) {
    return 0;
  }

  size_t count = 0;
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].peer != NULL && mgr->peers[i].batch != NULL &&
        mgr->peers[i].batch->remaining > 0) {
      count++;
    }
  }
  return count;
}

float download_mgr_aggregate_rate(const download_mgr_t *mgr) {
  if (mgr == NULL) {
    return 0.0f;
  }

  float total = 0.0f;
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].peer != NULL) {
      total += mgr->peers[i].bytes_per_second;
    }
  }
  return total;
}

bool download_mgr_has_block(const download_mgr_t *mgr, const hash256_t *hash) {
  if (mgr == NULL || hash == NULL) {
    return false;
  }

  /* Check queued batches */
  for (batch_node_t *node = mgr->queue_head; node != NULL; node = node->next) {
    for (size_t i = 0; i < node->batch.count; i++) {
      if (memcmp(&node->batch.hashes[i], hash, sizeof(hash256_t)) == 0) {
        return true;
      }
    }
  }

  /* Check assigned batches */
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].batch != NULL) {
      for (size_t j = 0; j < mgr->peers[i].batch->count; j++) {
        if (memcmp(&mgr->peers[i].batch->hashes[j], hash, sizeof(hash256_t)) ==
            0) {
          return true;
        }
      }
    }
  }

  return false;
}

bool download_mgr_has_height(const download_mgr_t *mgr, uint32_t height) {
  if (mgr == NULL) {
    return false;
  }

  /* O(1) bitmap lookup - no more O(n) scanning */
  return bitmap_has(mgr, height);
}

bool download_mgr_get_peer_stats(const download_mgr_t *mgr, const peer_t *peer,
                                 float *bytes_per_second,
                                 uint32_t *blocks_remaining) {
  if (mgr == NULL || peer == NULL) {
    return false;
  }

  const peer_perf_t *perf = find_peer_perf_const(mgr, peer);
  if (perf == NULL) {
    return false;
  }

  if (bytes_per_second != NULL) {
    *bytes_per_second = perf->bytes_per_second;
  }
  if (blocks_remaining != NULL) {
    *blocks_remaining =
        (perf->batch != NULL) ? (uint32_t)perf->batch->remaining : 0;
  }
  return true;
}

/* ============================================================================
 * Legacy API Compatibility
 * ============================================================================
 */

size_t download_mgr_pending_count(const download_mgr_t *mgr) {
  if (mgr == NULL) {
    return 0;
  }

  /* Pending = blocks in queue (not yet assigned) */
  size_t total = 0;
  for (batch_node_t *node = mgr->queue_head; node != NULL; node = node->next) {
    total += node->batch.remaining;
  }
  return total;
}

void download_mgr_clear_pending(download_mgr_t *mgr) {
  if (mgr == NULL) {
    return;
  }

  /* Pop and destroy all queued (pending) batches.
   * Does NOT affect assigned batches - those will complete normally.
   * IMPORTANT: Must clear bitmap bits before destroying, otherwise
   * collect_gaps will think these heights are still being tracked. */
  size_t cleared = 0;
  batch_node_t *node;
  while ((node = queue_pop_front(mgr)) != NULL) {
    cleared += node->batch.remaining;
    bitmap_clear_batch(mgr, &node->batch);
    batch_node_destroy(node);
  }

  if (cleared > 0) {
    log_info(LOG_COMP_SYNC, "download_mgr: cleared %zu pending blocks (bitmap updated)", cleared);
  }
}

size_t download_mgr_drain_accelerate(download_mgr_t *mgr, uint64_t stall_timeout_ms) {
  if (mgr == NULL || mgr->callbacks.send_getdata == NULL) {
    return 0;
  }

  uint64_t now = plat_time_ms();
  size_t requests_sent = 0;

  /* Collect idle peers (no work or completed their batch) */
  peer_t *idle_peers[DOWNLOAD_MAX_PEERS];
  size_t idle_count = 0;

  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_perf_t *perf = &mgr->peers[i];
    if (perf->peer == NULL) {
      continue;
    }
    /* Idle = no batch, or batch is complete */
    if (perf->batch == NULL || perf->batch->remaining == 0) {
      idle_peers[idle_count++] = perf->peer;
    }
  }

  if (idle_count == 0) {
    return 0; /* No idle peers to help */
  }

  /* Collect ALL outstanding blocks from ALL peers with in-flight work.
   * Aggressive strategy: every idle peer requests ALL outstanding blocks.
   * First peer to deliver wins, duplicates are ignored.
   * This maximizes parallelism during DRAIN's final blocks. */
  hash256_t blocks_to_request[DOWNLOAD_BATCH_SIZE * DOWNLOAD_MAX_PEERS];
  size_t blocks_count = 0;

  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_perf_t *perf = &mgr->peers[i];
    if (perf->peer == NULL || perf->batch == NULL) {
      continue;
    }
    if (perf->batch->remaining == 0) {
      continue; /* Already complete */
    }

    /* For stall_timeout_ms > 0, only collect from stalled peers.
     * For stall_timeout_ms == 0, collect from ALL peers (super aggressive). */
    if (stall_timeout_ms > 0) {
      uint64_t since_last = now - perf->last_delivery_time;
      if (since_last < stall_timeout_ms) {
        continue; /* Still delivering, not stalled */
      }
    }

    /* Collect unreceived blocks from this peer's batch */
    work_batch_t *batch = perf->batch;
    for (size_t j = 0; j < batch->count && blocks_count < sizeof(blocks_to_request)/sizeof(blocks_to_request[0]); j++) {
      if (!batch->received[j]) {
        blocks_to_request[blocks_count++] = batch->hashes[j];
      }
    }
  }

  if (blocks_count == 0) {
    return 0; /* No blocks to request */
  }

  /* Distribute outstanding blocks across idle peers with 3x redundancy.
   * Each block goes to 3 different peers (or fewer if we don't have 3 idle).
   * Limit 64 blocks per getdata (Bitcoin Core's limit).
   *
   * With 5000 blocks, 100 peers, 3x redundancy: each peer gets ~150 blocks.
   * This provides fast gap-filling without overwhelming any single peer. */
  #define ACCELERATE_BLOCKS_PER_GETDATA 64
  #define ACCELERATE_REDUNDANCY 3

  /* Calculate how many blocks each peer should get for even distribution */
  size_t total_requests_needed = blocks_count * ACCELERATE_REDUNDANCY;
  size_t blocks_per_peer = (total_requests_needed + idle_count - 1) / idle_count;
  if (blocks_per_peer > blocks_count) {
    blocks_per_peer = blocks_count; /* Can't give more than we have */
  }

  /* Assign blocks to peers in round-robin with redundancy.
   * Peer 0 gets blocks [0, blocks_per_peer)
   * Peer 1 gets blocks [offset, offset + blocks_per_peer) where offset staggers
   * This ensures each block is requested by ~3 peers. */
  for (size_t i = 0; i < idle_count; i++) {
    /* Stagger starting point so different peers start at different blocks */
    size_t start_offset = (i * blocks_count / idle_count) % blocks_count;
    size_t blocks_assigned = 0;
    size_t pos = start_offset;

    /* Send up to blocks_per_peer blocks to this peer, chunked by 64 */
    while (blocks_assigned < blocks_per_peer && blocks_assigned < blocks_count) {
      size_t chunk_size = blocks_per_peer - blocks_assigned;
      if (chunk_size > ACCELERATE_BLOCKS_PER_GETDATA) {
        chunk_size = ACCELERATE_BLOCKS_PER_GETDATA;
      }
      /* Handle wrap-around */
      if (pos + chunk_size > blocks_count) {
        chunk_size = blocks_count - pos;
      }
      if (chunk_size > 0) {
        mgr->callbacks.send_getdata(idle_peers[i], blocks_to_request + pos, chunk_size,
                                    mgr->callbacks.ctx);
        blocks_assigned += chunk_size;
        pos = (pos + chunk_size) % blocks_count;
      }
    }
    requests_sent++;
  }

  if (requests_sent > 0) {
    LOG_INFO("download_mgr: DRAIN accelerate - requested %zu blocks from %zu idle peers",
             blocks_count, requests_sent);
  }

  return requests_sent;
}

size_t download_mgr_fill_gaps_staggered(download_mgr_t *mgr,
                                         const hash256_t *gap_hashes,
                                         size_t gap_count,
                                         size_t max_peers_to_use) {
  if (mgr == NULL || gap_hashes == NULL || gap_count == 0 ||
      mgr->callbacks.send_getdata == NULL) {
    return 0;
  }

  /*
   * Staggered gap-filling strategy:
   *
   * Instead of waiting for stall timeouts, immediately request all gaps
   * from multiple peers with staggered ordering. Each peer starts at a
   * different position in the gap list, reducing contention and maximizing
   * the chance that gaps are filled quickly.
   *
   * Example with 100 gaps and 4 peers:
   *   Peer 0: gaps 0, 1, 2, ... 99 (starts at 0)
   *   Peer 1: gaps 25, 26, ... 99, 0, 1, ... 24 (starts at 25)
   *   Peer 2: gaps 50, 51, ... 99, 0, 1, ... 49 (starts at 50)
   *   Peer 3: gaps 75, 76, ... 99, 0, 1, ... 74 (starts at 75)
   *
   * Redundancy is bounded: each gap is requested from all participating peers,
   * but we limit max_peers_to_use to control bandwidth (e.g., 4-8 peers).
   * First response wins; duplicates are discarded by existing logic.
   */

  /* Collect available peers (all tracked peers) */
  peer_t *peers[DOWNLOAD_MAX_PEERS];
  size_t peer_count = 0;

  for (size_t i = 0; i < mgr->peer_count && peer_count < max_peers_to_use; i++) {
    if (mgr->peers[i].peer != NULL) {
      peers[peer_count++] = mgr->peers[i].peer;
    }
  }

  if (peer_count == 0) {
    return 0;
  }

  /* Limit to actual available peers */
  if (peer_count > max_peers_to_use) {
    peer_count = max_peers_to_use;
  }

  /* For each peer, send staggered gap requests */
  size_t requests_sent = 0;

  /* Limit per-peer request size to avoid overwhelming */
  #define STAGGER_MAX_PER_REQUEST 128
  size_t per_peer_count = gap_count;
  if (per_peer_count > STAGGER_MAX_PER_REQUEST) {
    per_peer_count = STAGGER_MAX_PER_REQUEST;
  }

  /* Temporary buffer for rotated gap list */
  hash256_t rotated[STAGGER_MAX_PER_REQUEST];

  for (size_t p = 0; p < peer_count; p++) {
    /* Calculate starting position for this peer (staggered) */
    size_t start_pos = (p * gap_count) / peer_count;

    /* Build rotated list starting at start_pos */
    for (size_t i = 0; i < per_peer_count; i++) {
      size_t src_idx = (start_pos + i) % gap_count;
      rotated[i] = gap_hashes[src_idx];
    }

    /* Send getdata with rotated list */
    mgr->callbacks.send_getdata(peers[p], rotated, per_peer_count,
                                mgr->callbacks.ctx);
    requests_sent++;
  }

  if (requests_sent > 0) {
    LOG_INFO("download_mgr: staggered gap-fill - %zu gaps to %zu peers "
             "(%.1fx redundancy)",
             gap_count, requests_sent,
             (float)(requests_sent * per_peer_count) / (float)gap_count);
  }

  return requests_sent;
}

size_t download_mgr_inflight_count(const download_mgr_t *mgr) {
  if (mgr == NULL) {
    return 0;
  }

  /* Inflight = blocks assigned to peers but not yet received */
  size_t total = 0;
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].batch != NULL) {
      total += mgr->peers[i].batch->remaining;
    }
  }
  return total;
}

void download_mgr_block_complete(download_mgr_t *mgr, const hash256_t *hash,
                                 uint32_t height) {
  /* No-op with batch model - blocks are implicitly complete when received
   * and the batch is freed when peer requests new work.
   */
  (void)mgr;
  (void)hash;
  (void)height;
}

/* ============================================================================
 * Debug/Metrics
 * ============================================================================
 */

void download_mgr_get_metrics(const download_mgr_t *mgr,
                              download_metrics_t *metrics) {
  if (mgr == NULL || metrics == NULL) {
    return;
  }

  memset(metrics, 0, sizeof(download_metrics_t));

  metrics->pending_count = download_mgr_pending_count(mgr);
  metrics->inflight_count = download_mgr_inflight_count(mgr);
  metrics->total_peers = mgr->peer_count;
  metrics->lowest_pending = mgr->lowest_pending_height;
  metrics->highest_assigned = mgr->highest_queued_height;
  metrics->aggregate_rate = download_mgr_aggregate_rate(mgr);
  metrics->active_peers = download_mgr_active_peer_count(mgr);
  metrics->stalled_peers = 0; /* No stall tracking in PULL model */
}
