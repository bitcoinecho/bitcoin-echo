/**
 * Bitcoin Echo — Performance-Based Block Download Manager
 *
 * Implements work distribution based on measured peer throughput.
 * See download_mgr.h for design principles.
 *
 * Build once. Build right. Stop.
 */

#define LOG_COMPONENT LOG_COMP_SYNC

#include "download_mgr.h"
#include "log.h"
#include "platform.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal Structures
 * ============================================================================
 */

/**
 * Download manager internal state.
 */
struct download_mgr {
  /* Callbacks for network operations */
  download_callbacks_t callbacks;

  /* Configuration */
  uint32_t download_window;

  /* Work queue: array of work items indexed by (height % capacity) */
  work_item_t *work_items;
  size_t work_capacity;

  /* Work tracking */
  uint32_t lowest_pending_height;   /* Lowest height still pending/assigned */
  uint32_t highest_queued_height;   /* Highest height in the queue */
  size_t pending_count;             /* Blocks waiting for assignment */
  size_t inflight_count;            /* Blocks assigned to peers */

  /* Peer performance tracking */
  peer_perf_t peers[DOWNLOAD_MAX_PEERS];
  size_t peer_count;

  /* Round-robin index for work distribution */
  size_t next_peer_index;
};

/* ============================================================================
 * Internal Helpers
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
 * Find work item by hash.
 * Returns NULL if not found.
 */
static work_item_t *find_work_by_hash(download_mgr_t *mgr,
                                      const hash256_t *hash) {
  for (uint32_t h = mgr->lowest_pending_height; h <= mgr->highest_queued_height;
       h++) {
    size_t idx = h % mgr->work_capacity;
    work_item_t *item = &mgr->work_items[idx];
    if (item->state != WORK_STATE_COMPLETE &&
        memcmp(&item->hash, hash, sizeof(hash256_t)) == 0) {
      return item;
    }
  }
  return NULL;
}

/**
 * Find work item by height.
 * Returns NULL if not found or out of range.
 */
static work_item_t *find_work_by_height(download_mgr_t *mgr, uint32_t height) {
  if (height < mgr->lowest_pending_height ||
      height > mgr->highest_queued_height) {
    return NULL;
  }
  size_t idx = height % mgr->work_capacity;
  work_item_t *item = &mgr->work_items[idx];
  if (item->height == height && item->state != WORK_STATE_COMPLETE) {
    return item;
  }
  return NULL;
}

/**
 * Find next pending (unassigned) work item starting from lowest height.
 */
static work_item_t *find_next_pending(download_mgr_t *mgr) {
  for (uint32_t h = mgr->lowest_pending_height; h <= mgr->highest_queued_height;
       h++) {
    size_t idx = h % mgr->work_capacity;
    work_item_t *item = &mgr->work_items[idx];
    if (item->state == WORK_STATE_PENDING) {
      return item;
    }
  }
  return NULL;
}

/**
 * Find a peer with capacity for more work.
 * Uses round-robin starting from next_peer_index.
 */
static peer_perf_t *find_peer_with_capacity(download_mgr_t *mgr) {
  if (mgr->peer_count == 0) {
    return NULL;
  }

  size_t start = mgr->next_peer_index % mgr->peer_count;
  size_t idx = start;

  do {
    peer_perf_t *perf = &mgr->peers[idx];
    if (perf->peer != NULL && !perf->stalled &&
        perf->blocks_in_flight < DOWNLOAD_MAX_IN_FLIGHT_PER_PEER) {
      mgr->next_peer_index = (idx + 1) % mgr->peer_count;
      return perf;
    }
    idx = (idx + 1) % mgr->peer_count;
  } while (idx != start);

  return NULL;
}

/**
 * Calculate mean of peer speeds.
 */
static float calc_speed_mean(const download_mgr_t *mgr) {
  float sum = 0.0f;
  size_t count = 0;

  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].peer != NULL && mgr->peers[i].bytes_per_second > 0.0f) {
      sum += mgr->peers[i].bytes_per_second;
      count++;
    }
  }

  return count > 0 ? sum / (float)count : 0.0f;
}

/**
 * Calculate standard deviation of peer speeds.
 */
static float calc_speed_stddev(const download_mgr_t *mgr, float mean) {
  float sum_sq = 0.0f;
  size_t count = 0;

  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].peer != NULL && mgr->peers[i].bytes_per_second > 0.0f) {
      float diff = mgr->peers[i].bytes_per_second - mean;
      sum_sq += diff * diff;
      count++;
    }
  }

  if (count < 2) {
    return 0.0f;
  }

  return sqrtf(sum_sq / (float)(count - 1));
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
 * Advance lowest_pending_height past completed blocks.
 */
static void advance_lowest_pending(download_mgr_t *mgr) {
  while (mgr->lowest_pending_height <= mgr->highest_queued_height) {
    size_t idx = mgr->lowest_pending_height % mgr->work_capacity;
    if (mgr->work_items[idx].state == WORK_STATE_COMPLETE) {
      mgr->lowest_pending_height++;
    } else {
      break;
    }
  }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================
 */

download_mgr_t *download_mgr_create(const download_callbacks_t *callbacks,
                                    uint32_t window) {
  if (callbacks == NULL) {
    return NULL;
  }

  download_mgr_t *mgr = calloc(1, sizeof(download_mgr_t));
  if (mgr == NULL) {
    return NULL;
  }

  mgr->callbacks = *callbacks;
  mgr->download_window = window > 0 ? window : DOWNLOAD_MAX_PENDING;
  mgr->work_capacity = mgr->download_window;

  mgr->work_items = calloc(mgr->work_capacity, sizeof(work_item_t));
  if (mgr->work_items == NULL) {
    free(mgr);
    return NULL;
  }

  /* Initialize all work items as complete (empty slots) */
  for (size_t i = 0; i < mgr->work_capacity; i++) {
    mgr->work_items[i].state = WORK_STATE_COMPLETE;
  }

  return mgr;
}

void download_mgr_destroy(download_mgr_t *mgr) {
  if (mgr == NULL) {
    return;
  }
  free(mgr->work_items);
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
  perf->window_start_time = plat_time_ms();

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

  /* Unassign all work from this peer */
  for (uint32_t h = mgr->lowest_pending_height; h <= mgr->highest_queued_height;
       h++) {
    size_t idx = h % mgr->work_capacity;
    work_item_t *item = &mgr->work_items[idx];
    if (item->assigned_peer == peer && item->state == WORK_STATE_ASSIGNED) {
      item->state = WORK_STATE_PENDING;
      item->assigned_peer = NULL;
      item->assigned_time = 0;
      mgr->pending_count++;
      if (mgr->inflight_count > 0) {
        mgr->inflight_count--;
      }
    }
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

  size_t added = 0;

  for (size_t i = 0; i < count; i++) {
    uint32_t height = heights[i];

    /* Check capacity */
    if (mgr->highest_queued_height > 0 &&
        height > mgr->lowest_pending_height + mgr->work_capacity - 1) {
      /* Would overflow the ring buffer */
      break;
    }

    /* Check for duplicate */
    size_t idx = height % mgr->work_capacity;
    work_item_t *item = &mgr->work_items[idx];

    if (item->state != WORK_STATE_COMPLETE && item->height == height) {
      /* Already have this height */
      continue;
    }

    /* Initialize new work item */
    memcpy(&item->hash, &hashes[i], sizeof(hash256_t));
    item->height = height;
    item->state = WORK_STATE_PENDING;
    item->assigned_peer = NULL;
    item->assigned_time = 0;
    item->retry_count = 0;

    /* Update tracking */
    if (mgr->highest_queued_height == 0 ||
        height > mgr->highest_queued_height) {
      mgr->highest_queued_height = height;
    }
    if (mgr->lowest_pending_height == 0 ||
        height < mgr->lowest_pending_height) {
      mgr->lowest_pending_height = height;
    }

    mgr->pending_count++;
    added++;
  }

  if (added > 0) {
    LOG_DEBUG("download_mgr: added %zu blocks, pending=%zu", added,
                   mgr->pending_count);
  }

  return added;
}

size_t download_mgr_distribute_work(download_mgr_t *mgr) {
  if (mgr == NULL || mgr->pending_count == 0) {
    return 0;
  }

  size_t assigned = 0;

  while (mgr->pending_count > 0) {
    /* Find a peer with capacity */
    peer_perf_t *perf = find_peer_with_capacity(mgr);
    if (perf == NULL) {
      break; /* No peers available */
    }

    /* Find next pending block */
    work_item_t *item = find_next_pending(mgr);
    if (item == NULL) {
      break; /* No pending work */
    }

    /* Assign to peer */
    item->state = WORK_STATE_ASSIGNED;
    item->assigned_peer = perf->peer;
    item->assigned_time = plat_time_ms();
    perf->blocks_in_flight++;
    mgr->pending_count--;
    mgr->inflight_count++;
    assigned++;

    /* Send getdata request (batch of 1 for now, could batch multiple) */
    if (mgr->callbacks.send_getdata != NULL) {
      mgr->callbacks.send_getdata(perf->peer, &item->hash, 1,
                                  mgr->callbacks.ctx);
    }
  }

  return assigned;
}

bool download_mgr_block_received(download_mgr_t *mgr, peer_t *peer,
                                 const hash256_t *hash, size_t block_size) {
  if (mgr == NULL || peer == NULL || hash == NULL) {
    return false;
  }

  /* Find the work item */
  work_item_t *item = find_work_by_hash(mgr, hash);
  if (item == NULL) {
    LOG_WARN("download_mgr: received unexpected block");
    return false;
  }

  /* Verify it was assigned to this peer */
  if (item->assigned_peer != peer) {
    /* Could be a late delivery from a previous assignment - accept anyway */
    LOG_DEBUG("download_mgr: block from different peer than assigned");
  }

  /* Update work item state */
  item->state = WORK_STATE_RECEIVED;
  if (item->assigned_peer != NULL) {
    peer_perf_t *perf = find_peer_perf(mgr, item->assigned_peer);
    if (perf != NULL && perf->blocks_in_flight > 0) {
      perf->blocks_in_flight--;
    }
  }
  if (mgr->inflight_count > 0) {
    mgr->inflight_count--;
  }

  /* Update peer performance tracking */
  peer_perf_t *perf = find_peer_perf(mgr, peer);
  if (perf != NULL) {
    perf->bytes_this_window += block_size;
    perf->last_delivery_time = plat_time_ms();
    perf->stalled = false;
  }

  return true;
}

void download_mgr_block_complete(download_mgr_t *mgr, const hash256_t *hash,
                                 uint32_t height) {
  if (mgr == NULL || hash == NULL) {
    return;
  }

  work_item_t *item = find_work_by_height(mgr, height);
  if (item == NULL) {
    return;
  }

  if (memcmp(&item->hash, hash, sizeof(hash256_t)) != 0) {
    LOG_WARN("download_mgr: hash mismatch at height %u", height);
    return;
  }

  item->state = WORK_STATE_COMPLETE;
  advance_lowest_pending(mgr);
}

void download_mgr_check_performance(download_mgr_t *mgr) {
  if (mgr == NULL) {
    return;
  }

  uint64_t now = plat_time_ms();
  size_t active_peers = 0;

  /* Update per-peer performance metrics */
  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_perf_t *perf = &mgr->peers[i];
    if (perf->peer == NULL) {
      continue;
    }

    /* Check if window has elapsed */
    uint64_t elapsed = now - perf->window_start_time;
    if (elapsed >= DOWNLOAD_PERF_WINDOW_MS) {
      /* Calculate bytes/second */
      perf->bytes_per_second =
          (float)perf->bytes_this_window / ((float)elapsed / 1000.0f);

      /* Reset window */
      perf->bytes_this_window = 0;
      perf->window_start_time = now;

      LOG_DEBUG("download_mgr: peer %p rate=%.0f B/s in_flight=%u",
                     (void *)perf->peer, (double)perf->bytes_per_second,
                     perf->blocks_in_flight);
    }

    /* Check for stall: has work but no delivery */
    if (perf->blocks_in_flight > 0 && !perf->stalled) {
      uint64_t since_delivery = now - perf->last_delivery_time;
      if (since_delivery > DOWNLOAD_STALL_TIMEOUT_MS) {
        LOG_INFO("download_mgr: peer stalled, reassigning %u blocks",
                      perf->blocks_in_flight);
        perf->stalled = true;

        /* Unassign all work from this peer */
        for (uint32_t h = mgr->lowest_pending_height;
             h <= mgr->highest_queued_height; h++) {
          size_t idx = h % mgr->work_capacity;
          work_item_t *item = &mgr->work_items[idx];
          if (item->assigned_peer == perf->peer &&
              item->state == WORK_STATE_ASSIGNED) {
            item->state = WORK_STATE_PENDING;
            item->assigned_peer = NULL;
            item->retry_count++;
            mgr->pending_count++;
            if (mgr->inflight_count > 0) {
              mgr->inflight_count--;
            }
          }
        }
        perf->blocks_in_flight = 0;

        /* Optionally disconnect stalled peer */
        if (mgr->callbacks.disconnect_peer != NULL) {
          mgr->callbacks.disconnect_peer(perf->peer, "stalled",
                                         mgr->callbacks.ctx);
        }
      }
    }

    if (perf->blocks_in_flight > 0) {
      active_peers++;
    }
  }

  /* Skip slow peer detection if not enough peers */
  if (active_peers < DOWNLOAD_MIN_PEERS_FOR_STDDEV) {
    return;
  }

  /* Calculate mean and stddev for slow peer detection */
  float mean = calc_speed_mean(mgr);
  float stddev = calc_speed_stddev(mgr, mean);

  if (stddev < 1.0f) {
    return; /* Not enough variance to detect outliers */
  }

  float threshold = mean - (DOWNLOAD_ALLOWED_DEVIATION * stddev);

  /* Check each peer against threshold */
  for (size_t i = 0; i < mgr->peer_count; i++) {
    peer_perf_t *perf = &mgr->peers[i];
    if (perf->peer == NULL || perf->stalled) {
      continue;
    }

    /* Only check peers that have been measured */
    if (perf->bytes_per_second > 0.0f && perf->bytes_per_second < threshold) {
      LOG_INFO("download_mgr: slow peer (%.0f B/s < %.0f threshold), "
                    "splitting work",
                    (double)perf->bytes_per_second, (double)threshold);

      /* Split work from this peer (take half) */
      download_mgr_split_work(mgr, perf->peer);
    }
  }
}

size_t download_mgr_split_work(download_mgr_t *mgr, peer_t *slow_peer) {
  if (mgr == NULL || slow_peer == NULL) {
    return 0;
  }

  peer_perf_t *perf = find_peer_perf(mgr, slow_peer);
  if (perf == NULL || perf->blocks_in_flight == 0) {
    return 0;
  }

  /* Take half of the slow peer's work */
  size_t to_unassign = perf->blocks_in_flight / 2;
  if (to_unassign == 0) {
    to_unassign = 1; /* At least one block */
  }

  size_t unassigned = 0;

  /* Unassign from the end (highest heights) first */
  for (uint32_t h = mgr->highest_queued_height;
       h >= mgr->lowest_pending_height && unassigned < to_unassign; h--) {
    size_t idx = h % mgr->work_capacity;
    work_item_t *item = &mgr->work_items[idx];
    if (item->assigned_peer == slow_peer && item->state == WORK_STATE_ASSIGNED) {
      item->state = WORK_STATE_PENDING;
      item->assigned_peer = NULL;
      item->retry_count++;
      if (perf->blocks_in_flight > 0) {
        perf->blocks_in_flight--;
      }
      mgr->pending_count++;
      if (mgr->inflight_count > 0) {
        mgr->inflight_count--;
      }
      unassigned++;
    }
  }

  LOG_DEBUG("download_mgr: split %zu blocks from slow peer", unassigned);
  return unassigned;
}

/* ============================================================================
 * Query Functions
 * ============================================================================
 */

size_t download_mgr_pending_count(const download_mgr_t *mgr) {
  return mgr != NULL ? mgr->pending_count : 0;
}

size_t download_mgr_inflight_count(const download_mgr_t *mgr) {
  return mgr != NULL ? mgr->inflight_count : 0;
}

size_t download_mgr_active_peer_count(const download_mgr_t *mgr) {
  if (mgr == NULL) {
    return 0;
  }

  size_t count = 0;
  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].peer != NULL && mgr->peers[i].blocks_in_flight > 0) {
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
  return find_work_by_hash((download_mgr_t *)mgr, hash) != NULL;
}

bool download_mgr_get_peer_stats(const download_mgr_t *mgr, const peer_t *peer,
                                 float *bytes_per_second,
                                 uint32_t *blocks_in_flight) {
  if (mgr == NULL || peer == NULL) {
    return false;
  }

  peer_perf_t *perf = find_peer_perf((download_mgr_t *)mgr, peer);
  if (perf == NULL) {
    return false;
  }

  if (bytes_per_second != NULL) {
    *bytes_per_second = perf->bytes_per_second;
  }
  if (blocks_in_flight != NULL) {
    *blocks_in_flight = perf->blocks_in_flight;
  }
  return true;
}

void download_mgr_get_metrics(const download_mgr_t *mgr,
                              download_metrics_t *metrics) {
  if (mgr == NULL || metrics == NULL) {
    return;
  }

  memset(metrics, 0, sizeof(download_metrics_t));

  metrics->pending_count = mgr->pending_count;
  metrics->inflight_count = mgr->inflight_count;
  metrics->total_peers = mgr->peer_count;
  metrics->lowest_pending = mgr->lowest_pending_height;
  metrics->highest_assigned = mgr->highest_queued_height;
  metrics->aggregate_rate = download_mgr_aggregate_rate(mgr);

  for (size_t i = 0; i < mgr->peer_count; i++) {
    if (mgr->peers[i].peer != NULL) {
      if (mgr->peers[i].blocks_in_flight > 0) {
        metrics->active_peers++;
      }
      if (mgr->peers[i].stalled) {
        metrics->stalled_peers++;
      }
    }
  }
}
