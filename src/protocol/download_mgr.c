/**
 * Bitcoin Echo — PULL-Based Block Download Manager
 *
 * Implements cooperative work distribution:
 *
 * - Work is organized as BATCHES, not individual items
 * - Peers PULL work when idle, coordinator doesn't push
 * - Starved peers WAIT for work (no sacrifice of slow peers)
 * - Sticky batches RACE blocking blocks to multiple peers
 * - Only truly stalled peers (0 B/s) are disconnected
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
  uint32_t lowest_pending_height;   /* Lowest height in queue/assigned */
  uint32_t highest_queued_height;   /* Highest height added */
  uint32_t highest_received_height; /* Highest block received (download frontier) */

  /* Peer performance tracking */
  peer_perf_t peers[DOWNLOAD_MAX_PEERS];
  size_t peer_count;

  /* Stall detection */
  uint32_t last_validated_height;  /* Last reported validated height */
  uint64_t last_progress_time;     /* When validation last progressed */

  /* Adaptive stall timeout (Bitcoin Core style backoff) */
  uint32_t stall_backoff_height;   /* Height we're stuck at */
  uint32_t stall_backoff_count;    /* Times we've stolen at this height */
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
 * Clone a batch node (for sticky batches).
 * Creates a new node with copied batch data but NOT sticky (the clone is normal).
 */
static batch_node_t *batch_node_clone(const batch_node_t *src) {
  batch_node_t *node = batch_node_create();
  if (node == NULL) {
    return NULL;
  }
  /* Copy batch data */
  memcpy(&node->batch, &src->batch, sizeof(work_batch_t));
  /* Clone is NOT sticky - only the original queue entry is sticky */
  node->batch.sticky = false;
  node->batch.sticky_height = 0;
  /* Reset link pointers */
  node->next = NULL;
  node->prev = NULL;
  return node;
}

/**
 * Get batch size for a given block height.
 *
 * Bitcoin Core uses MAX_BLOCKS_IN_TRANSIT_PER_PEER = 16 as a fixed limit.
 * We use 64 blocks (4x) to reduce getdata overhead and improve throughput,
 * trading off finer-grained batch reassignment for fewer round trips.
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
 * Add batch to front of queue, but after any sticky batch.
 * Sticky batches contain blocking blocks needed for validation progress,
 * so returned work from evicted peers should not displace them.
 */
static void queue_push_after_sticky(download_mgr_t *mgr, batch_node_t *node) {
  /* If head is sticky, insert after it */
  if (mgr->queue_head != NULL && mgr->queue_head->batch.sticky) {
    node->prev = mgr->queue_head;
    node->next = mgr->queue_head->next;
    if (mgr->queue_head->next != NULL) {
      mgr->queue_head->next->prev = node;
    } else {
      mgr->queue_tail = node;
    }
    mgr->queue_head->next = node;
    mgr->queue_count++;
    return;
  }

  /* No sticky batch at front - standard push front */
  queue_push_front(mgr, node);
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
    node->batch.assigned_time = 0;        /* Mark as unassigned */
    queue_push_after_sticky(mgr, node);   /* Return to front, after any sticky */
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

  size_t added = 0;
  size_t i = 0;

  while (i < count && mgr->queue_count < DOWNLOAD_MAX_BATCHES) {
    /* Create a new batch */
    batch_node_t *node = batch_node_create();
    if (node == NULL) {
      LOG_WARN("download_mgr: failed to allocate batch node");
      break;
    }

    /* Fill the batch with up to DOWNLOAD_BATCH_SIZE blocks */
    size_t batch_count = 0;
    while (batch_count < DOWNLOAD_BATCH_SIZE && i < count) {
      memcpy(&node->batch.hashes[batch_count], &hashes[i], sizeof(hash256_t));
      node->batch.heights[batch_count] = heights[i];
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
    batch_node_destroy(old_node);
    perf->batch = NULL;
  }

  /* Try to get a batch from the queue */
  if (mgr->queue_head == NULL) {
    /* Queue empty - peer is starved */
    LOG_DEBUG("download_mgr: no work available, peer starved");
    return false;
  }

  batch_node_t *node;
  bool is_sticky_clone = false;

  if (mgr->queue_head->batch.sticky) {
    /* Sticky batch: clone it, leave original in queue for other peers */
    node = batch_node_clone(mgr->queue_head);
    if (node == NULL) {
      LOG_WARN("download_mgr: failed to clone sticky batch");
      return false;
    }
    is_sticky_clone = true;
  } else {
    /* Normal batch: pop from queue */
    node = queue_pop_front(mgr);
  }

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
   * When a batch is stolen and reassigned, the received[] bitmap preserves
   * which blocks we already have. We request ALL blocks again (storage layer
   * deduplicates), but block_received() only decrements remaining for blocks
   * not already marked received. This fixes the "duplicate block counting" bug
   * where remaining would hit 0 even though some blocks were never delivered.
   */

  /* Send getdata for all blocks in batch */
  if (mgr->callbacks.send_getdata != NULL) {
    mgr->callbacks.send_getdata(peer, node->batch.hashes, node->batch.count,
                                mgr->callbacks.ctx);
  }

  uint32_t batch_start = node->batch.heights[0];
  uint32_t batch_end = batch_start + (uint32_t)node->batch.count - 1;
  LOG_INFO("download_mgr: assigned %sbatch [%u-%u] (%zu blocks) to peer",
           is_sticky_clone ? "PRIORITY " : "",
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

  /* Check if block is in peer's batch */
  if (perf->batch == NULL) {
    /* Late delivery - peer has no batch (was split or completed) */
    LOG_DEBUG("download_mgr: late block delivery from idle peer");
    return true;
  }

  /* Find the block in the batch */
  for (size_t i = 0; i < perf->batch->count; i++) {
    if (memcmp(&perf->batch->hashes[i], hash, sizeof(hash256_t)) == 0) {
      /* Found it - check if already received (duplicate) */
      if (perf->batch->received[i]) {
        /* Already received this block - don't decrement remaining.
         * This happens when a batch is stolen and reassigned: we request
         * all blocks again, and the new peer may send duplicates. */
        LOG_DEBUG("download_mgr: duplicate block at index %zu (already received), "
                  "remaining=%zu unchanged",
                  i, perf->batch->remaining);
        return true;
      }

      /* First time receiving this block - mark received and decrement */
      perf->batch->received[i] = true;
      if (perf->batch->remaining > 0) {
        perf->batch->remaining--;
      }

      /* Track download frontier for gap detection */
      uint32_t block_height = perf->batch->heights[i];
      if (block_height > mgr->highest_received_height) {
        mgr->highest_received_height = block_height;
      }

      LOG_DEBUG("download_mgr: block received at index %zu, batch remaining=%zu",
                i, perf->batch->remaining);
      return true;
    }
  }

  /* Block not in batch - late delivery or unrequested */
  LOG_DEBUG("download_mgr: block not in peer's batch (late delivery)");
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

/**
 * Find peer with the lowest-height batch (blocking validation).
 */
static peer_perf_t *find_peer_with_lowest_batch(download_mgr_t *mgr) {
  peer_perf_t *lowest = NULL;
  uint32_t lowest_height = UINT32_MAX;

  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_perf_t *perf = &mgr->peers[i];
    if (perf->peer == NULL || perf->batch == NULL ||
        perf->batch->remaining == 0) {
      continue;
    }

    /* Check the first (lowest) height in this batch */
    uint32_t batch_height = perf->batch->heights[0];
    if (batch_height < lowest_height) {
      lowest_height = batch_height;
      lowest = perf;
    }
  }

  return lowest;
}

bool download_mgr_check_stall(download_mgr_t *mgr, uint32_t validated_height) {
  if (mgr == NULL) {
    return false;
  }

  uint64_t now = plat_time_ms();

  /* Check if validation has made progress */
  if (validated_height > mgr->last_validated_height) {
    /* Progress! Reset stall timer and backoff */
    mgr->last_validated_height = validated_height;
    mgr->last_progress_time = now;
    mgr->stall_backoff_count = 0;  /* Reset backoff on progress */

    /* Remove any sticky batch whose blocking height has been validated.
     * The sticky batch served its purpose - the blocking block arrived. */
    if (mgr->queue_head != NULL && mgr->queue_head->batch.sticky &&
        mgr->queue_head->batch.sticky_height <= validated_height) {
      batch_node_t *resolved = queue_pop_front(mgr);
      LOG_INFO("download_mgr: removed resolved sticky batch (blocking height %u "
               "validated, current height %u)",
               resolved->batch.sticky_height, validated_height);
      batch_node_destroy(resolved);
    }

    return false;
  }

  /* No progress - check how long we've been stalled */
  if (mgr->last_progress_time == 0) {
    /* First call - initialize */
    mgr->last_progress_time = now;
    mgr->last_validated_height = validated_height;
    return false;
  }

  uint64_t stall_duration = now - mgr->last_progress_time;

  /* Adaptive stall timeout based on halving epoch.
   *
   * Early blocks are tiny and should arrive quickly, so use shorter timeouts.
   * Later blocks are larger and need more time. Scale with halving epoch:
   *   Epoch 0 (blocks 0-209,999):     0.5 second base
   *   Epoch 1 (blocks 210,000-419,999): 1.0 seconds base
   *   Epoch 2 (blocks 420,000-629,999): 1.5 seconds base
   *   etc.
   *
   * This keeps momentum early in the chain while being patient later.
   * The timeout doubles on each steal attempt (up to 64 seconds max).
   * Timeout resets when validation makes progress.
   */
#define STALL_EPOCH_BLOCKS 210000    /* Blocks per halving epoch */
#define STALL_MS_PER_EPOCH 500       /* 0.5 seconds per epoch */
#define STALL_MAX_TIMEOUT_MS 64000   /* 64 second max (matches Bitcoin Core) */

  uint32_t epoch = validated_height / STALL_EPOCH_BLOCKS;
  uint64_t base_timeout = (uint64_t)(epoch + 1) * STALL_MS_PER_EPOCH;
  uint64_t stall_timeout = base_timeout;
  for (uint32_t i = 0; i < mgr->stall_backoff_count && stall_timeout < STALL_MAX_TIMEOUT_MS; i++) {
    stall_timeout *= 2;
  }
  if (stall_timeout > STALL_MAX_TIMEOUT_MS) {
    stall_timeout = STALL_MAX_TIMEOUT_MS;
  }

  if (stall_duration < stall_timeout) {
    /* Not stalled long enough yet */
    return false;
  }

  /* We're stalled! Find the peer blocking validation (lowest batch) */
  peer_perf_t *blocker = find_peer_with_lowest_batch(mgr);

  if (blocker == NULL) {
    /* No peer with work - nothing to steal */
    LOG_INFO("download_mgr: stalled but no peers have work to steal");
    return false;
  }

  uint32_t blocker_height = blocker->batch->heights[0];
  uint32_t blocker_end = blocker_height + (uint32_t)blocker->batch->count - 1;
  uint32_t next_height = validated_height + 1;

  /* Check if this batch contains the block we need for validation.
   * A batch is relevant if: start <= next_height <= end */
  if (next_height < blocker_height) {
    /* Lowest ASSIGNED batch is ahead of what we need. But the needed block
     * might be in the QUEUE waiting to be assigned. Check the queue first
     * before declaring a GAP. */
    uint32_t queue_lowest = UINT32_MAX;
    for (batch_node_t *qnode = mgr->queue_head; qnode != NULL; qnode = qnode->next) {
      if (qnode->batch.heights[0] < queue_lowest) {
        queue_lowest = qnode->batch.heights[0];
      }
    }

    if (queue_lowest <= next_height) {
      /* Found the needed block in the queue but not being worked on.
       * Don't wait for "idle peer" - move this batch to the front so the
       * next peer to finish gets it immediately. */
      for (batch_node_t *qnode = mgr->queue_head; qnode != NULL; qnode = qnode->next) {
        uint32_t qstart = qnode->batch.heights[0];
        uint32_t qend = qstart + (uint32_t)qnode->batch.count - 1;
        if (next_height >= qstart && next_height <= qend) {
          /* Found the batch containing our needed block */
          if (qnode != mgr->queue_head) {
            /* Remove from current position */
            if (qnode->prev) qnode->prev->next = qnode->next;
            if (qnode->next) qnode->next->prev = qnode->prev;
            if (qnode == mgr->queue_tail) mgr->queue_tail = qnode->prev;
            /* Insert at front (after any sticky batch) */
            if (mgr->queue_head && mgr->queue_head->batch.sticky) {
              /* Insert after sticky */
              batch_node_t *sticky = mgr->queue_head;
              qnode->next = sticky->next;
              qnode->prev = sticky;
              if (sticky->next) sticky->next->prev = qnode;
              sticky->next = qnode;
              if (mgr->queue_tail == sticky) mgr->queue_tail = qnode;
            } else {
              /* Insert at head */
              qnode->next = mgr->queue_head;
              qnode->prev = NULL;
              if (mgr->queue_head) mgr->queue_head->prev = qnode;
              mgr->queue_head = qnode;
              if (mgr->queue_tail == NULL) mgr->queue_tail = qnode;
            }
            LOG_INFO("download_mgr: moved queued batch [%u-%u] to front "
                     "(contains needed block %u)",
                     qstart, qend, next_height);
          }
          break;
        }
      }
      mgr->last_progress_time = now;
      return false;
    }

    /* Neither assigned nor queued - this is a real GAP */
    LOG_WARN("download_mgr: stalled at %u, need block %u, but lowest assigned "
             "batch starts at %u, lowest queued at %u - GAP! Block may be lost.",
             validated_height, next_height, blocker_height, queue_lowest);
    /* Don't free this batch - it's valid future work. Just reset stall timer. */
    mgr->last_progress_time = now;
    return false;
  }

  if (next_height > blocker_end) {
    /* Batch is BEHIND what we need - it's stale (we've already validated these).
     * Free this stale batch so we don't keep finding it. */
    LOG_INFO("download_mgr: stalled at %u but lowest batch [%u-%u] is stale "
             "(we already validated past it) - freeing",
             validated_height, blocker_height, blocker_end);

    batch_node_t *stale = (batch_node_t *)(void *)blocker->batch;
    batch_node_destroy(stale);
    blocker->batch = NULL;

    /* Reset stall timer and try again next tick */
    mgr->last_progress_time = now;
    return false;
  }

  /* Block race strategy: Instead of stealing the whole batch and disconnecting
   * the peer, we create a redundant request for just the blocking block.
   * This way:
   *   - The original peer keeps working and might still deliver
   *   - The next peer to request work gets the blocking block as priority
   *   - Whoever delivers first wins - we get redundancy without burning peers
   */

  /* Find the blocking block in the blocker's batch.
   * We must SEARCH for it - can't assume consecutive heights due to gaps
   * from already-downloaded blocks. */
  batch_node_t *blocker_node = (batch_node_t *)(void *)blocker->batch;
  size_t block_idx = SIZE_MAX;
  for (size_t i = 0; i < blocker_node->batch.count; i++) {
    if (blocker_node->batch.heights[i] == next_height) {
      block_idx = i;
      break;
    }
  }
  if (block_idx == SIZE_MAX) {
    /* Block not in batch - shouldn't happen given earlier range check,
     * but could if batch has gaps. Log and reset stall timer. */
    LOG_WARN("download_mgr: stalled at %u but block not found in blocker's batch "
             "[%u-%u] (gaps?) - resetting stall timer",
             validated_height, blocker_height, blocker_end);
    mgr->last_progress_time = now;
    return false;
  }

  /* Check if blocker is actively delivering blocks.
   * Use DOWNLOAD_PERF_WINDOW_MS (10 sec) as threshold, not stall_timeout.
   * stall_timeout starts at 2 sec which is too aggressive - a peer could
   * just be waiting for network latency. 10 sec gives them time to deliver. */
  uint64_t since_last_delivery = now - blocker->last_delivery_time;
  bool blocker_is_active = (blocker->last_delivery_time > 0 &&
                            since_last_delivery < DOWNLOAD_PERF_WINDOW_MS);

  LOG_INFO("download_mgr: validation stalled at height %u for %llu ms "
           "(timeout=%llu ms, backoff=%u) - blocker has batch [%u-%u], "
           "last delivery %llu ms ago, %s",
           validated_height, (unsigned long long)stall_duration,
           (unsigned long long)stall_timeout, mgr->stall_backoff_count,
           blocker_height, blocker_end,
           (unsigned long long)since_last_delivery,
           blocker_is_active ? "ACTIVE (racing)" : "STALLED (stealing)");

  /* Check if we already have a sticky batch for this height */
  if (mgr->queue_head != NULL && mgr->queue_head->batch.sticky &&
      mgr->queue_head->batch.sticky_height == next_height) {
    LOG_DEBUG("download_mgr: sticky batch for height %u already exists",
              next_height);
    mgr->last_progress_time = now;
    return false;
  }

  /* Build a sticky batch with:
   *   1. The blocking block (next_height) - MUST be first
   *   2. Remaining blocks from blocker's batch
   *   3. Additional gap blocks from lagging peers
   *
   * Sticky batches are 2x normal size to maximize gap-filling opportunity.
   * Gap blocks are unreceived blocks that are:
   *   - Ahead of validated_height (validation will need them)
   *   - Significantly behind highest_received_height (other peers raced past)
   *
   * This proactively races blocks that would cause future stalls. */

#define STICKY_BATCH_SIZE DOWNLOAD_BATCH_SIZE_MAX  /* 2x normal (128): blocker + next gaps */

  typedef struct {
    hash256_t hash;
    uint32_t height;
  } gap_block_t;

  gap_block_t candidates[(size_t)STICKY_BATCH_SIZE * 2]; /* Extra space for sorting */
  size_t candidate_count = 0;

  /* Always add the blocking block first */
  memcpy(&candidates[0].hash, &blocker_node->batch.hashes[block_idx], sizeof(hash256_t));
  candidates[0].height = next_height;
  candidate_count = 1;

  /* Add remaining blocks from blocker's batch */
  for (size_t i = block_idx + 1; i < blocker_node->batch.count && candidate_count < (size_t)STICKY_BATCH_SIZE * 2; i++) {
    if (!blocker_node->batch.received[i]) {
      memcpy(&candidates[candidate_count].hash, &blocker_node->batch.hashes[i], sizeof(hash256_t));
      candidates[candidate_count].height = blocker_node->batch.heights[i];
      candidate_count++;
    }
  }

  /* Scan for NEXT consecutive gap after the blocker's batch.
   *
   * Walk batches in HEIGHT ORDER to find the first incomplete batch after
   * the blocker. This is the next gap that will block validation once we
   * get past the current blocker. Prioritizing this over scattered gaps
   * ensures we're always racing the blocks that matter most.
   */

  /* Collect incomplete batches that are AFTER the blocker's batch */
  typedef struct {
    work_batch_t *batch;
    uint32_t lowest_height;
  } batch_ref_t;

  batch_ref_t incomplete_batches[DOWNLOAD_MAX_PEERS];
  size_t incomplete_count = 0;

  for (size_t p = 0; p < mgr->peer_count; p++) {
    peer_perf_t *perf = &mgr->peers[p];
    if (perf->peer == NULL || perf->batch == NULL || perf->batch->remaining == 0) {
      continue;
    }
    /* Skip the blocker (already added its blocks) */
    if (perf->batch == blocker->batch) {
      continue;
    }
    /* Skip sticky batches (already racing) */
    if (perf->batch->sticky) {
      continue;
    }

    uint32_t batch_lowest = perf->batch->heights[0];

    /* Only include batches that start AFTER the blocker's batch ends */
    if (batch_lowest <= blocker_end) {
      continue;
    }

    incomplete_batches[incomplete_count].batch = perf->batch;
    incomplete_batches[incomplete_count].lowest_height = batch_lowest;
    incomplete_count++;
  }

  /* Sort by lowest height (find the NEXT gap, not random gaps) */
  for (size_t i = 0; i < incomplete_count; i++) {
    for (size_t j = i + 1; j < incomplete_count; j++) {
      if (incomplete_batches[j].lowest_height < incomplete_batches[i].lowest_height) {
        batch_ref_t tmp = incomplete_batches[i];
        incomplete_batches[i] = incomplete_batches[j];
        incomplete_batches[j] = tmp;
      }
    }
  }

  /* Walk incomplete batches in HEIGHT ORDER, filling up to 128 blocks.
   * We want blocks that are either in the blocker's batch OR upcoming gaps
   * that risk becoming blockers. By processing in height order, we always
   * grab the soonest gaps first - block 111111 before 123456. */
  for (size_t b = 0; b < incomplete_count && candidate_count < (size_t)STICKY_BATCH_SIZE * 2; b++) {
    work_batch_t *batch = incomplete_batches[b].batch;

    for (size_t j = 0; j < batch->count && candidate_count < (size_t)STICKY_BATCH_SIZE * 2; j++) {
      if (!batch->received[j]) {
        memcpy(&candidates[candidate_count].hash, &batch->hashes[j], sizeof(hash256_t));
        candidates[candidate_count].height = batch->heights[j];
        candidate_count++;
      }
    }
  }

  /* Sort by height (lowest first, but blocking block stays first) */
  for (size_t i = 1; i < candidate_count - 1; i++) {
    for (size_t j = i + 1; j < candidate_count; j++) {
      if (candidates[j].height < candidates[i].height) {
        gap_block_t tmp = candidates[i];
        candidates[i] = candidates[j];
        candidates[j] = tmp;
      }
    }
  }

  /* Remove duplicates (same height) */
  size_t unique_count = 1; /* Keep first (blocking block) */
  for (size_t i = 1; i < candidate_count; i++) {
    bool is_dup = false;
    for (size_t j = 0; j < unique_count; j++) {
      if (candidates[j].height == candidates[i].height) {
        is_dup = true;
        break;
      }
    }
    if (!is_dup) {
      if (unique_count != i) {
        candidates[unique_count] = candidates[i];
      }
      unique_count++;
    }
  }

  /* Take up to STICKY_BATCH_SIZE blocks (2x normal) */
  size_t fill_count = unique_count;
  if (fill_count > (size_t)STICKY_BATCH_SIZE) {
    fill_count = (size_t)STICKY_BATCH_SIZE;
  }

  /* Create the sticky batch */
  batch_node_t *race_batch = batch_node_create();
  if (race_batch != NULL) {
    for (size_t i = 0; i < fill_count; i++) {
      memcpy(&race_batch->batch.hashes[i], &candidates[i].hash, sizeof(hash256_t));
      race_batch->batch.heights[i] = candidates[i].height;
      race_batch->batch.received[i] = false;
    }
    race_batch->batch.count = fill_count;
    race_batch->batch.remaining = fill_count;
    race_batch->batch.assigned_time = 0;
    race_batch->batch.sticky = true;
    race_batch->batch.sticky_height = next_height;

    queue_push_front(mgr, race_batch);

    if (!blocker_is_active) {
      mgr->stall_backoff_count++;
    }

    /* Count how many are gap blocks vs blocker blocks */
    size_t gap_count = 0;
    for (size_t i = 0; i < fill_count; i++) {
      if (race_batch->batch.heights[i] > blocker_end) {
        gap_count++;
      }
    }

    LOG_INFO("download_mgr: queued STICKY batch [%u...%u] (%zu blocks, %zu gap fills) - "
             "blocker %s, frontier at %u",
             candidates[0].height, candidates[fill_count - 1].height,
             fill_count, gap_count,
             blocker_is_active ? "active" : "inactive",
             mgr->highest_received_height);
  }

  /* Reset stall timer so we don't immediately trigger again */
  mgr->last_progress_time = now;

  return true;
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
    queue_push_after_sticky(mgr, node);
    perf->batch = NULL;

    if (mgr->callbacks.disconnect_peer != NULL) {
      mgr->callbacks.disconnect_peer(perf->peer, "stalled (0 B/s)",
                                     mgr->callbacks.ctx);
    }
    dropped++;
  }

  /* Phase 4: Disconnect slow peers (below absolute minimum rate).
   *
   * Unlike the old stddev-based eviction (removed 2025-12-31), this uses an
   * absolute minimum threshold. Peers below 3 KB/s after the grace period
   * are disconnected. This avoids the problem where similar-speed peers
   * triggered eviction at 99% of average.
   *
   * 3 KB/s is very conservative - even a slow peer should manage this.
   * Peers below this threshold are likely on congested/poor connections
   * and replacing them with fresh peers should improve throughput.
   */
  for (size_t i = 0; i < rate_count; i++) {
    if (reporters - dropped <= DOWNLOAD_MIN_PEERS_TO_KEEP) {
      LOG_DEBUG("download_mgr: keeping slow peer to maintain minimum");
      break;
    }

    peer_perf_t *perf = peers_with_rates[i];

    /* Check if peer is past the grace period */
    if (perf->first_work_time == 0) {
      continue; /* Never assigned work, skip */
    }
    uint64_t time_since_first_work = now - perf->first_work_time;
    if (time_since_first_work < DOWNLOAD_SLOW_GRACE_PERIOD_MS) {
      continue; /* Still in grace period */
    }

    /* Check if peer is below minimum rate */
    if (perf->bytes_per_second >= (float)DOWNLOAD_MIN_RATE_BYTES_PER_SEC) {
      continue; /* Fast enough, keep */
    }

    batch_node_t *node = (batch_node_t *)(void *)perf->batch;
    uint32_t batch_start = node->batch.heights[0];
    uint32_t batch_end = batch_start + (uint32_t)node->batch.count - 1;

    LOG_INFO("download_mgr: peer too slow (%.1f KB/s < %.1f KB/s minimum), "
             "returning batch [%u-%u] to queue",
             perf->bytes_per_second / 1024.0f,
             (float)DOWNLOAD_MIN_RATE_BYTES_PER_SEC / 1024.0f,
             batch_start, batch_end);

    node->batch.assigned_time = 0;
    queue_push_after_sticky(mgr, node);
    perf->batch = NULL;

    if (mgr->callbacks.disconnect_peer != NULL) {
      mgr->callbacks.disconnect_peer(perf->peer, "too slow",
                                     mgr->callbacks.ctx);
    }
    dropped++;
  }

  if (dropped > 0) {
    LOG_INFO("download_mgr: performance check dropped %zu slow/stalled peers",
             dropped);
  }

  return dropped;
}

size_t download_mgr_evict_slowest_percent(download_mgr_t *mgr, float percent,
                                          float min_rate_to_keep) {
  if (mgr == NULL || percent <= 0.0f || percent > 100.0f) {
    return 0;
  }

  uint64_t now = plat_time_ms();

  /* Phase 1: Collect all active peers with valid rates.
   * Only consider peers who:
   * - Have a batch assigned (actively downloading)
   * - Have reported a positive rate (proven they can deliver)
   * - Are past the grace period (fair chance to warm up)
   */
  typedef struct {
    peer_perf_t *perf;
    float rate;
  } peer_rate_t;

  peer_rate_t candidates[DOWNLOAD_MAX_PEERS];
  size_t candidate_count = 0;

  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_perf_t *perf = &mgr->peers[i];
    if (perf->peer == NULL || perf->batch == NULL) {
      continue;
    }

    /* Skip peers still in grace period */
    if (perf->first_work_time == 0) {
      continue;
    }
    uint64_t time_since_first_work = now - perf->first_work_time;
    if (time_since_first_work < DOWNLOAD_SLOW_GRACE_PERIOD_MS) {
      continue;
    }

    /* Skip peers who haven't proven they can deliver */
    if (!perf->has_reported) {
      continue;
    }

    /* Update window to get fresh rate */
    update_peer_window(perf, now);

    candidates[candidate_count].perf = perf;
    candidates[candidate_count].rate = perf->bytes_per_second;
    candidate_count++;
  }

  /* Need enough peers to make percentile meaningful */
  if (candidate_count <= DOWNLOAD_MIN_PEERS_TO_KEEP) {
    LOG_DEBUG("download_mgr: only %zu candidates, skipping percentile eviction",
              candidate_count);
    return 0;
  }

  /* Phase 2: Sort by rate (ascending - slowest first) */
  for (size_t i = 0; i < candidate_count - 1; i++) {
    for (size_t j = i + 1; j < candidate_count; j++) {
      if (candidates[j].rate < candidates[i].rate) {
        peer_rate_t tmp = candidates[i];
        candidates[i] = candidates[j];
        candidates[j] = tmp;
      }
    }
  }

  /* Phase 3: Calculate how many to evict (bottom N percent, at least 1) */
  size_t evict_count = (size_t)((float)candidate_count * percent / 100.0f);
  if (evict_count == 0) {
    evict_count = 1; /* Always evict at least 1 if we have enough peers */
  }

  /* Cap eviction to maintain minimum peer count */
  size_t max_evict = candidate_count - DOWNLOAD_MIN_PEERS_TO_KEEP;
  if (evict_count > max_evict) {
    evict_count = max_evict;
  }

  if (evict_count == 0) {
    return 0;
  }

  /* Phase 4: Evict the slowest peers */
  size_t evicted = 0;

  for (size_t i = 0; i < evict_count; i++) {
    peer_perf_t *perf = candidates[i].perf;
    float rate = candidates[i].rate;

    /* Skip peers above the minimum rate threshold - they're "fast enough" even
     * if they're in the bottom percentile. Use 0.0 to disable this check. */
    if (min_rate_to_keep > 0.0f && rate >= min_rate_to_keep) {
      LOG_DEBUG("download_mgr: skipping peer (%.1f KB/s >= %.1f KB/s threshold)",
                rate / 1024.0f, min_rate_to_keep / 1024.0f);
      continue;
    }

    /* Return batch to queue before disconnecting */
    if (perf->batch != NULL) {
      batch_node_t *node = (batch_node_t *)(void *)perf->batch;
      uint32_t batch_start = node->batch.heights[0];
      uint32_t batch_end = batch_start + (uint32_t)node->batch.count - 1;

      LOG_INFO("download_mgr: evicting bottom %zu%% peer (%.1f KB/s, rank %zu/%zu), "
               "returning batch [%u-%u]",
               (size_t)percent, rate / 1024.0f, i + 1, candidate_count,
               batch_start, batch_end);

      node->batch.assigned_time = 0;
      queue_push_after_sticky(mgr, node);
      perf->batch = NULL;
    }

    /* Disconnect the peer */
    if (mgr->callbacks.disconnect_peer != NULL) {
      mgr->callbacks.disconnect_peer(perf->peer, "bottom 10% eviction",
                                     mgr->callbacks.ctx);
    }
    evicted++;
  }

  if (evicted > 0) {
    /* Log the rate distribution for diagnostics */
    float slowest = candidates[0].rate;
    float fastest = candidates[candidate_count - 1].rate;
    float median = candidates[candidate_count / 2].rate;
    LOG_INFO("download_mgr: evicted %zu slowest peers (%.1f%%). "
             "Rates: slowest=%.1f KB/s, median=%.1f KB/s, fastest=%.1f KB/s",
             evicted, percent, slowest / 1024.0f, median / 1024.0f,
             fastest / 1024.0f);
  }

  return evicted;
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
