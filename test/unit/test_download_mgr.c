/**
 * Bitcoin Echo — Download Manager Unit Tests (PULL Model)
 *
 * Tests PULL-based block download functionality:
 * - Manager initialization and destruction
 * - Peer management (add/remove)
 * - Work queue operations (batch-based)
 * - Peer work requests (PULL model)
 * - Block receipt handling
 * - Cooperative empty queue handling
 * - Metrics and queries
 */

#include "download_mgr.h"
#include "test_utils.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mock peer structure for testing */
typedef struct {
  int id;
  char addr[32];
} mock_peer_t;

/* Test context for callbacks */
typedef struct {
  size_t getdata_calls;
  size_t disconnect_calls;
  mock_peer_t *last_getdata_peer;
  size_t last_getdata_count;
  mock_peer_t *last_disconnect_peer;
  const char *last_disconnect_reason;
} test_ctx_t;

/* Mock callbacks */
static void mock_send_getdata(peer_t *peer, const hash256_t *hashes,
                              size_t count, void *ctx) {
  test_ctx_t *tctx = (test_ctx_t *)ctx;
  (void)hashes;
  tctx->getdata_calls++;
  tctx->last_getdata_peer = (mock_peer_t *)peer;
  tctx->last_getdata_count = count;
}

static void mock_disconnect_peer(peer_t *peer, const char *reason, void *ctx) {
  test_ctx_t *tctx = (test_ctx_t *)ctx;
  tctx->disconnect_calls++;
  tctx->last_disconnect_peer = (mock_peer_t *)peer;
  tctx->last_disconnect_reason = reason;
}

/* Helper: create test hashes */
static void make_test_hash(hash256_t *hash, uint32_t height) {
  memset(hash, 0, sizeof(hash256_t));
  memcpy(hash->bytes, &height, sizeof(height));
}

/* ============================================================================
 * Creation and Destruction Tests
 * ============================================================================
 */

static void test_create_destroy(void) {
  test_case("create and destroy manager");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  if (mgr == NULL) {
    test_fail("failed to create manager");
    return;
  }

  /* Verify initial state */
  if (download_mgr_pending_count(mgr) != 0) {
    test_fail_uint("pending count", 0, download_mgr_pending_count(mgr));
    download_mgr_destroy(mgr);
    return;
  }

  if (download_mgr_inflight_count(mgr) != 0) {
    test_fail_uint("inflight count", 0, download_mgr_inflight_count(mgr));
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_create_null_callbacks(void) {
  test_case("create with NULL callbacks");

  download_mgr_t *mgr = download_mgr_create(NULL);
  if (mgr != NULL) {
    test_fail("should fail with NULL callbacks");
    download_mgr_destroy(mgr);
    return;
  }

  test_pass();
}

/* ============================================================================
 * Peer Management Tests
 * ============================================================================
 */

static void test_add_peer(void) {
  test_case("add peer");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peer1 = {.id = 1};

  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  download_metrics_t metrics;
  download_mgr_get_metrics(mgr, &metrics);

  if (metrics.total_peers != 1) {
    test_fail_uint("total peers", 1, metrics.total_peers);
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_add_multiple_peers(void) {
  test_case("add multiple peers");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peers[5];

  for (int i = 0; i < 5; i++) {
    peers[i].id = i;
    download_mgr_add_peer(mgr, (peer_t *)&peers[i]);
  }

  download_metrics_t metrics;
  download_mgr_get_metrics(mgr, &metrics);

  if (metrics.total_peers != 5) {
    test_fail_uint("total peers", 5, metrics.total_peers);
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_remove_peer(void) {
  test_case("remove peer");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peer1 = {.id = 1};
  mock_peer_t peer2 = {.id = 2};

  download_mgr_add_peer(mgr, (peer_t *)&peer1);
  download_mgr_add_peer(mgr, (peer_t *)&peer2);
  download_mgr_remove_peer(mgr, (peer_t *)&peer1);

  download_metrics_t metrics;
  download_mgr_get_metrics(mgr, &metrics);

  if (metrics.total_peers != 1) {
    test_fail_uint("total peers after remove", 1, metrics.total_peers);
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_add_duplicate_peer(void) {
  test_case("add duplicate peer");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peer1 = {.id = 1};

  download_mgr_add_peer(mgr, (peer_t *)&peer1);
  download_mgr_add_peer(mgr, (peer_t *)&peer1); /* Duplicate */

  download_metrics_t metrics;
  download_mgr_get_metrics(mgr, &metrics);

  if (metrics.total_peers != 1) {
    test_fail_uint("total peers (no duplicate)", 1, metrics.total_peers);
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

/* ============================================================================
 * Work Queue Tests
 * ============================================================================
 */

static void test_add_work(void) {
  test_case("add work items (creates batches)");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);

  hash256_t hashes[5];
  uint32_t heights[5];
  for (uint32_t i = 0; i < 5; i++) {
    make_test_hash(&hashes[i], i + 100);
    heights[i] = i + 100;
  }

  size_t added = download_mgr_add_work(mgr, hashes, heights, 5);

  if (added != 5) {
    test_fail_uint("blocks added", 5, added);
    download_mgr_destroy(mgr);
    return;
  }

  if (download_mgr_pending_count(mgr) != 5) {
    test_fail_uint("pending count", 5, download_mgr_pending_count(mgr));
    download_mgr_destroy(mgr);
    return;
  }

  /* Should have created 1 batch (5 < DOWNLOAD_BATCH_SIZE blocks per batch) */
  if (download_mgr_queue_count(mgr) != 1) {
    test_fail_uint("queue count", 1, download_mgr_queue_count(mgr));
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_add_work_multiple_batches(void) {
  test_case("add work creates multiple batches");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);

  /* Add enough blocks for 3 batches. */
  size_t num_blocks = (size_t)DOWNLOAD_BATCH_SIZE * 2 + 1;
  hash256_t *hashes = malloc(num_blocks * sizeof(hash256_t));
  uint32_t *heights = malloc(num_blocks * sizeof(uint32_t));
  for (size_t i = 0; i < num_blocks; i++) {
    make_test_hash(&hashes[i], (uint32_t)(i + 100));
    heights[i] = (uint32_t)(i + 100);
  }

  size_t added = download_mgr_add_work(mgr, hashes, heights, num_blocks);

  if (added != num_blocks) {
    test_fail_uint("blocks added", num_blocks, added);
    free(hashes);
    free(heights);
    download_mgr_destroy(mgr);
    return;
  }

  /* Should have created 3 batches */
  if (download_mgr_queue_count(mgr) != 3) {
    test_fail_uint("queue count", 3, download_mgr_queue_count(mgr));
    free(hashes);
    free(heights);
    download_mgr_destroy(mgr);
    return;
  }

  free(hashes);
  free(heights);
  download_mgr_destroy(mgr);
  test_pass();
}

static void test_has_block(void) {
  test_case("check if block in queue");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);

  hash256_t hash1, hash2;
  uint32_t height1 = 100;
  make_test_hash(&hash1, height1);
  make_test_hash(&hash2, 999); /* Not in queue */

  download_mgr_add_work(mgr, &hash1, &height1, 1);

  if (!download_mgr_has_block(mgr, &hash1)) {
    test_fail("should find block in queue");
    download_mgr_destroy(mgr);
    return;
  }

  if (download_mgr_has_block(mgr, &hash2)) {
    test_fail("should not find unknown block");
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

/* ============================================================================
 * PULL Model Work Assignment Tests
 * ============================================================================
 */

static void test_peer_request_work(void) {
  test_case("peer requests work (PULL model)");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peer1 = {.id = 1};

  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  hash256_t hashes[3];
  uint32_t heights[3];
  for (uint32_t i = 0; i < 3; i++) {
    make_test_hash(&hashes[i], i + 100);
    heights[i] = i + 100;
  }
  download_mgr_add_work(mgr, hashes, heights, 3);

  /* Peer requests work - PULL model */
  bool got_work = download_mgr_peer_request_work(mgr, (peer_t *)&peer1);

  if (!got_work) {
    test_fail("peer should get work");
    download_mgr_destroy(mgr);
    return;
  }

  /* Should have sent one getdata for the batch */
  if (ctx.getdata_calls != 1) {
    test_fail_uint("getdata calls", 1, ctx.getdata_calls);
    download_mgr_destroy(mgr);
    return;
  }

  /* All 3 blocks in the batch */
  if (ctx.last_getdata_count != 3) {
    test_fail_uint("getdata count", 3, ctx.last_getdata_count);
    download_mgr_destroy(mgr);
    return;
  }

  if (download_mgr_inflight_count(mgr) != 3) {
    test_fail_uint("inflight count", 3, download_mgr_inflight_count(mgr));
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_peer_request_no_work(void) {
  test_case("peer request with empty queue");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peer1 = {.id = 1};

  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  /* No work added - peer requests work */
  bool got_work = download_mgr_peer_request_work(mgr, (peer_t *)&peer1);

  if (got_work) {
    test_fail("peer should not get work from empty queue");
    download_mgr_destroy(mgr);
    return;
  }

  /* No getdata should be sent */
  if (ctx.getdata_calls != 0) {
    test_fail_uint("getdata calls", 0, ctx.getdata_calls);
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_peer_is_idle(void) {
  test_case("peer idle check");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peer1 = {.id = 1};

  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  /* Peer should be idle initially */
  if (!download_mgr_peer_is_idle(mgr, (peer_t *)&peer1)) {
    test_fail("peer should be idle initially");
    download_mgr_destroy(mgr);
    return;
  }

  /* Add work and have peer request it */
  hash256_t hash;
  uint32_t height = 100;
  make_test_hash(&hash, height);
  download_mgr_add_work(mgr, &hash, &height, 1);
  download_mgr_peer_request_work(mgr, (peer_t *)&peer1);

  /* Peer should NOT be idle now */
  if (download_mgr_peer_is_idle(mgr, (peer_t *)&peer1)) {
    test_fail("peer should not be idle with work");
    download_mgr_destroy(mgr);
    return;
  }

  /* Receive the block - peer should be idle again */
  download_mgr_block_received(mgr, (peer_t *)&peer1, &hash, 1000);

  if (!download_mgr_peer_is_idle(mgr, (peer_t *)&peer1)) {
    test_fail("peer should be idle after batch complete");
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_multiple_peers_pull(void) {
  test_case("multiple peers pull work");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peers[3];
  for (int i = 0; i < 3; i++) {
    peers[i].id = i;
    download_mgr_add_peer(mgr, (peer_t *)&peers[i]);
  }

  /* Add exactly 3 batches worth of blocks. */
  size_t num_blocks = (size_t)DOWNLOAD_BATCH_SIZE * 3;
  hash256_t *hashes = malloc(num_blocks * sizeof(hash256_t));
  uint32_t *heights = malloc(num_blocks * sizeof(uint32_t));
  for (size_t i = 0; i < num_blocks; i++) {
    make_test_hash(&hashes[i], (uint32_t)(i + 100));
    heights[i] = (uint32_t)(i + 100);
  }
  download_mgr_add_work(mgr, hashes, heights, num_blocks);

  /* Each peer requests work */
  for (int i = 0; i < 3; i++) {
    download_mgr_peer_request_work(mgr, (peer_t *)&peers[i]);
  }

  /* All three peers should have work */
  download_metrics_t metrics;
  download_mgr_get_metrics(mgr, &metrics);

  if (metrics.active_peers != 3) {
    test_fail_uint("active peers", 3, metrics.active_peers);
    free(hashes);
    free(heights);
    download_mgr_destroy(mgr);
    return;
  }

  /* Queue should be empty (all batches assigned) */
  if (download_mgr_queue_count(mgr) != 0) {
    test_fail_uint("queue count", 0, download_mgr_queue_count(mgr));
    free(hashes);
    free(heights);
    download_mgr_destroy(mgr);
    return;
  }

  free(hashes);
  free(heights);
  download_mgr_destroy(mgr);
  test_pass();
}

/* ============================================================================
 * Block Receipt Tests
 * ============================================================================
 */

static void test_block_received(void) {
  test_case("block received from peer");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peer1 = {.id = 1};
  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  hash256_t hash;
  uint32_t height = 100;
  make_test_hash(&hash, height);
  download_mgr_add_work(mgr, &hash, &height, 1);
  download_mgr_peer_request_work(mgr, (peer_t *)&peer1);

  bool result = download_mgr_block_received(mgr, (peer_t *)&peer1, &hash, 1000);

  if (!result) {
    test_fail("should accept expected block");
    download_mgr_destroy(mgr);
    return;
  }

  /* Batch complete - inflight should be 0 */
  if (download_mgr_inflight_count(mgr) != 0) {
    test_fail_uint("inflight after receive", 0, download_mgr_inflight_count(mgr));
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_unexpected_block(void) {
  test_case("late/unrequested block from peer (graceful accept)");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peer1 = {.id = 1};
  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  hash256_t hash;
  make_test_hash(&hash, 999); /* Not in queue */

  /* Accept unrequested/late blocks gracefully. The block will still be stored
   * by sync.c, just not tracked for work. The peer still gets throughput
   * credit for the bytes delivered. */
  bool result = download_mgr_block_received(mgr, (peer_t *)&peer1, &hash, 1000);

  if (!result) {
    test_fail("should accept late/unrequested block gracefully");
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

/* ============================================================================
 * Empty Queue Tests (Cooperative Model)
 * ============================================================================
 */

static void test_starved_peer_waits(void) {
  test_case("starved peer waits (cooperative model)");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peer1 = {.id = 1};
  mock_peer_t peer2 = {.id = 2};
  download_mgr_add_peer(mgr, (peer_t *)&peer1);
  download_mgr_add_peer(mgr, (peer_t *)&peer2);

  /* Give peer1 some work (exactly 1 batch worth) */
  hash256_t hashes[DOWNLOAD_BATCH_SIZE];
  uint32_t heights[DOWNLOAD_BATCH_SIZE];
  for (uint32_t i = 0; i < DOWNLOAD_BATCH_SIZE; i++) {
    make_test_hash(&hashes[i], i + 100);
    heights[i] = i + 100;
  }
  download_mgr_add_work(mgr, hashes, heights, DOWNLOAD_BATCH_SIZE);
  download_mgr_peer_request_work(mgr, (peer_t *)&peer1);

  /* peer2 tries to request work but queue is empty - just returns false */
  bool got_work = download_mgr_peer_request_work(mgr, (peer_t *)&peer2);
  if (got_work) {
    test_fail("peer2 should not get work (queue empty)");
    download_mgr_destroy(mgr);
    return;
  }

  /* No one should be disconnected - peer2 just waits */
  if (ctx.disconnect_calls != 0) {
    test_fail_uint("disconnect calls", 0, ctx.disconnect_calls);
    download_mgr_destroy(mgr);
    return;
  }

  /* peer1 should still have their work */
  if (download_mgr_peer_is_idle(mgr, (peer_t *)&peer1)) {
    test_fail("peer1 should still have work");
    download_mgr_destroy(mgr);
    return;
  }

  /* Queue should still be empty */
  if (download_mgr_pending_count(mgr) != 0) {
    test_fail_uint("pending count", 0, download_mgr_pending_count(mgr));
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

/* ============================================================================
 * Peer Stats Tests
 * ============================================================================
 */

static void test_get_peer_stats(void) {
  test_case("get peer stats");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peer1 = {.id = 1};
  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  float rate;
  uint32_t remaining;
  bool found =
      download_mgr_get_peer_stats(mgr, (peer_t *)&peer1, &rate, &remaining);

  if (!found) {
    test_fail("should find peer stats");
    download_mgr_destroy(mgr);
    return;
  }

  if (remaining != 0) {
    test_fail_uint("initial remaining", 0, remaining);
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_get_unknown_peer_stats(void) {
  test_case("get unknown peer stats");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t unknown = {.id = 999};

  float rate;
  uint32_t remaining;
  bool found =
      download_mgr_get_peer_stats(mgr, (peer_t *)&unknown, &rate, &remaining);

  if (found) {
    test_fail("should not find unknown peer");
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

/* ============================================================================
 * Metrics Tests
 * ============================================================================
 */

static void test_metrics(void) {
  test_case("get download metrics");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peer1 = {.id = 1};
  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  hash256_t hashes[5];
  uint32_t heights[5];
  for (uint32_t i = 0; i < 5; i++) {
    make_test_hash(&hashes[i], i + 100);
    heights[i] = i + 100;
  }
  download_mgr_add_work(mgr, hashes, heights, 5);
  download_mgr_peer_request_work(mgr, (peer_t *)&peer1);

  download_metrics_t metrics;
  download_mgr_get_metrics(mgr, &metrics);

  if (metrics.total_peers != 1) {
    test_fail_uint("total_peers", 1, metrics.total_peers);
    download_mgr_destroy(mgr);
    return;
  }

  if (metrics.inflight_count != 5) {
    test_fail_uint("inflight_count", 5, metrics.inflight_count);
    download_mgr_destroy(mgr);
    return;
  }

  if (metrics.lowest_pending != 100) {
    test_fail_uint("lowest_pending", 100, metrics.lowest_pending);
    download_mgr_destroy(mgr);
    return;
  }

  if (metrics.highest_assigned != 104) {
    test_fail_uint("highest_assigned", 104, metrics.highest_assigned);
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

/* ============================================================================
 * Remove Peer with Work Tests
 * ============================================================================
 */

static void test_remove_peer_with_work(void) {
  test_case("remove peer with assigned work");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);
  mock_peer_t peer1 = {.id = 1};
  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  hash256_t hashes[5];
  uint32_t heights[5];
  for (uint32_t i = 0; i < 5; i++) {
    make_test_hash(&hashes[i], i + 100);
    heights[i] = i + 100;
  }
  download_mgr_add_work(mgr, hashes, heights, 5);
  download_mgr_peer_request_work(mgr, (peer_t *)&peer1);

  /* Remove peer - work should return to pending */
  download_mgr_remove_peer(mgr, (peer_t *)&peer1);

  if (download_mgr_pending_count(mgr) != 5) {
    test_fail_uint("pending after peer removal", 5,
                   download_mgr_pending_count(mgr));
    download_mgr_destroy(mgr);
    return;
  }

  if (download_mgr_inflight_count(mgr) != 0) {
    test_fail_uint("inflight after peer removal", 0,
                   download_mgr_inflight_count(mgr));
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

/* ============================================================================
 * Peer Eviction Tests
 *
 * These tests exercise download_mgr_evict_slowest_percent() and
 * download_mgr_check_performance(). Both functions require peers to have
 * has_reported=true and be past the grace period — state that normally takes
 * 10+ seconds of real delivery to accumulate. We use the test-only
 * download_mgr_inject_peer_rate() to set rates directly.
 * ============================================================================
 */

/**
 * Helper: add a peer, give it work, and inject a simulated download rate.
 * Returns true if setup succeeded.
 *
 * After calling this the peer appears "active and past grace period" from
 * the eviction functions' perspective, with the rate you specify.
 */
static bool setup_active_peer(download_mgr_t *mgr, mock_peer_t *peer,
                              uint32_t height_start, float bytes_per_sec) {
  /* Cast through void* to silence -Wcast-align: mock_peer_t is opaque to the
   * download manager (it only stores and compares the pointer), so alignment
   * is not an issue at the usage site. This matches the pattern used elsewhere
   * in the codebase (e.g., batch_node_t cast in download_mgr.c). */
  peer_t *p = (peer_t *)(void *)peer;

  download_mgr_add_peer(mgr, p);

  /* Give the peer a batch of work so it counts as "actively downloading" */
  hash256_t hash;
  uint32_t height = height_start;
  make_test_hash(&hash, height);
  download_mgr_add_work(mgr, &hash, &height, 1);
  if (!download_mgr_peer_request_work(mgr, p)) {
    return false; /* Could not assign work */
  }

  /* Inject rate — bypasses 10-second real-time window */
  return download_mgr_inject_peer_rate(mgr, p, bytes_per_sec);
}

static void test_evict_slowest_above_min(void) {
  test_case("evict slowest peers when above minimum count");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);

  /* 8 peers total: 4 fast (100 KB/s) and 4 slow (100 B/s).
   * Use distinct height_start values so each peer gets a unique batch. */
  mock_peer_t fast_peers[4];
  mock_peer_t slow_peers[4];

  for (int i = 0; i < 4; i++) {
    fast_peers[i].id = 10 + i;
    if (!setup_active_peer(mgr, &fast_peers[i], (uint32_t)(1000 + i * 2),
                           100000.0f /* 100 KB/s */)) {
      test_fail("failed to set up fast peer");
      download_mgr_destroy(mgr);
      return;
    }
  }

  for (int i = 0; i < 4; i++) {
    slow_peers[i].id = 20 + i;
    if (!setup_active_peer(mgr, &slow_peers[i], (uint32_t)(2000 + i * 2),
                           100.0f /* 100 B/s — below 1 KB/s threshold */)) {
      test_fail("failed to set up slow peer");
      download_mgr_destroy(mgr);
      return;
    }
  }

  /* Trigger percentile eviction: evict bottom 50%, respecting min_rate_to_keep=0
   * (no rate floor — evict purely by rank). With 8 candidates, 50% = 4 peers. */
  size_t evicted = download_mgr_evict_slowest_percent(mgr, 50.0f, 0.0f);

  /* Should have evicted exactly the 4 slow peers, leaving 4 fast ones.
   * Min peers to keep = DOWNLOAD_MIN_PEERS_TO_KEEP = 3, so 8-4=4 >= 3. */
  if (evicted != 4) {
    test_fail_uint("evicted count", 4, evicted);
    download_mgr_destroy(mgr);
    return;
  }

  /* Disconnect callback must have fired exactly 4 times */
  if (ctx.disconnect_calls != 4) {
    test_fail_uint("disconnect calls", 4, ctx.disconnect_calls);
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_no_evict_at_minimum(void) {
  test_case("no eviction when at minimum peer count");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);

  /* Set up exactly DOWNLOAD_MIN_PEERS_TO_KEEP slow peers.
   * evict_slowest_percent requires candidate_count > DOWNLOAD_MIN_PEERS_TO_KEEP,
   * so with exactly MIN peers it must refuse to evict any. */
  mock_peer_t peers[DOWNLOAD_MIN_PEERS_TO_KEEP];

  for (int i = 0; i < DOWNLOAD_MIN_PEERS_TO_KEEP; i++) {
    peers[i].id = 30 + i;
    if (!setup_active_peer(mgr, &peers[i], (uint32_t)(3000 + i * 2),
                           50.0f /* 50 B/s — very slow */)) {
      test_fail("failed to set up peer");
      download_mgr_destroy(mgr);
      return;
    }
  }

  /* Attempt eviction — should be blocked because candidate_count == MIN */
  size_t evicted = download_mgr_evict_slowest_percent(mgr, 50.0f, 0.0f);

  if (evicted != 0) {
    test_fail_uint("evicted count (should be zero at minimum)", 0, evicted);
    download_mgr_destroy(mgr);
    return;
  }

  /* No disconnects */
  if (ctx.disconnect_calls != 0) {
    test_fail_uint("disconnect calls (should be zero)", 0,
                   ctx.disconnect_calls);
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_evict_stops_at_minimum_floor(void) {
  test_case("eviction stops at minimum peer count floor");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);

  /* 5 peers: 4 slow, 1 fast. If DOWNLOAD_MIN_PEERS_TO_KEEP=3:
   *   max_evict = 5 - 3 = 2 (can only evict 2 to stay at floor)
   * The 1 fast peer must survive; up to 2 slow peers are removed. */
  mock_peer_t fast_peer = {.id = 40};
  if (!setup_active_peer(mgr, &fast_peer, 4000, 100000.0f /* 100 KB/s */)) {
    test_fail("failed to set up fast peer");
    download_mgr_destroy(mgr);
    return;
  }

  mock_peer_t slow_peers[4];
  for (int i = 0; i < 4; i++) {
    slow_peers[i].id = 41 + i;
    if (!setup_active_peer(mgr, &slow_peers[i], (uint32_t)(4100 + i * 2),
                           50.0f /* 50 B/s */)) {
      test_fail("failed to set up slow peer");
      download_mgr_destroy(mgr);
      return;
    }
  }

  /* Evict bottom 50% (no rate floor) — 50% of 5 = 2, capped at max_evict=2 */
  size_t evicted = download_mgr_evict_slowest_percent(mgr, 50.0f, 0.0f);

  /* Exactly 2 slow peers evicted (the floor prevents evicting all 4 slow) */
  if (evicted != 2) {
    test_fail_uint("evicted count", 2, evicted);
    download_mgr_destroy(mgr);
    return;
  }

  /* At least DOWNLOAD_MIN_PEERS_TO_KEEP peers must still be tracked */
  download_metrics_t metrics;
  download_mgr_get_metrics(mgr, &metrics);
  if (metrics.total_peers < DOWNLOAD_MIN_PEERS_TO_KEEP) {
    test_fail_uint("remaining peers below minimum", DOWNLOAD_MIN_PEERS_TO_KEEP,
                   metrics.total_peers);
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_evict_all_equal_rates(void) {
  test_case("no crash with equal-rate peers");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);

  /* 6 peers all at the same slow rate (100 B/s — below 1 KB/s threshold).
   * Eviction should still proceed without crashing: the sort is stable
   * for equal rates, so the first N peers in insertion order get evicted. */
  mock_peer_t peers[6];
  for (int i = 0; i < 6; i++) {
    peers[i].id = 50 + i;
    if (!setup_active_peer(mgr, &peers[i], (uint32_t)(5000 + i * 2),
                           100.0f /* identical rate */)) {
      test_fail("failed to set up peer");
      download_mgr_destroy(mgr);
      return;
    }
  }

  /* evict_slowest_percent must not crash and must respect min peers floor */
  size_t evicted = download_mgr_evict_slowest_percent(mgr, 10.0f, 0.0f);

  /* 10% of 6 = 0, rounds up to 1; max_evict = 6-3 = 3; so evicted == 1 */
  if (evicted != 1) {
    test_fail_uint("evicted count (equal-rate, 10%)", 1, evicted);
    download_mgr_destroy(mgr);
    return;
  }

  /* At least DOWNLOAD_MIN_PEERS_TO_KEEP must survive */
  download_metrics_t metrics;
  download_mgr_get_metrics(mgr, &metrics);
  if (metrics.total_peers < DOWNLOAD_MIN_PEERS_TO_KEEP) {
    test_fail_uint("remaining peers below minimum after equal-rate eviction",
                   DOWNLOAD_MIN_PEERS_TO_KEEP, metrics.total_peers);
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_evict_stalled_peer_zero_rate(void) {
  test_case("stalled peer (zero rate) is evicted first");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks);

  /* 6 peers: 5 at moderate speed, 1 stalled (zero bytes/sec).
   * A stalled peer has rate=0 and has_reported=true — it USED to deliver
   * but stopped. The zero rate sits at the bottom of any sorted list.
   *
   * Note: check_performance (not evict_slowest_percent) handles the
   * stall-timeout disconnect path.  For evict_slowest_percent a zero-rate
   * peer is simply the slowest and will be selected first. */
  mock_peer_t stalled = {.id = 60};
  if (!setup_active_peer(mgr, &stalled, 6000, 0.0f /* stalled */)) {
    test_fail("failed to set up stalled peer");
    download_mgr_destroy(mgr);
    return;
  }

  mock_peer_t normal_peers[5];
  for (int i = 0; i < 5; i++) {
    normal_peers[i].id = 61 + i;
    if (!setup_active_peer(mgr, &normal_peers[i], (uint32_t)(6100 + i * 2),
                           50000.0f /* 50 KB/s */)) {
      test_fail("failed to set up normal peer");
      download_mgr_destroy(mgr);
      return;
    }
  }

  /* Evict bottom 10% (at least 1 peer, which should be the stalled one) */
  ctx.disconnect_calls = 0;
  size_t evicted = download_mgr_evict_slowest_percent(mgr, 10.0f, 0.0f);

  /* Must have evicted exactly 1 (the stalled peer is the slowest) */
  if (evicted != 1) {
    test_fail_uint("evicted count", 1, evicted);
    download_mgr_destroy(mgr);
    return;
  }

  /* Disconnect callback must have fired exactly once */
  if (ctx.disconnect_calls != 1) {
    test_fail_uint("disconnect calls", 1, ctx.disconnect_calls);
    download_mgr_destroy(mgr);
    return;
  }

  /* The peer that was disconnected must be the stalled one */
  if (ctx.last_disconnect_peer != (mock_peer_t *)&stalled) {
    test_fail("evicted wrong peer — stalled peer should be evicted first");
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

/* ============================================================================
 * Main
 * ============================================================================
 */

int main(void) {
  test_suite_begin("Download Manager tests (PULL model)");

  /* Creation tests */
  test_create_destroy();
  test_create_null_callbacks();

  /* Peer management tests */
  test_add_peer();
  test_add_multiple_peers();
  test_remove_peer();
  test_add_duplicate_peer();

  /* Work queue tests */
  test_add_work();
  test_add_work_multiple_batches();
  test_has_block();

  /* PULL model work assignment tests */
  test_peer_request_work();
  test_peer_request_no_work();
  test_peer_is_idle();
  test_multiple_peers_pull();

  /* Block receipt tests */
  test_block_received();
  test_unexpected_block();

  /* Empty queue tests (cooperative model) */
  test_starved_peer_waits();

  /* Peer stats tests */
  test_get_peer_stats();
  test_get_unknown_peer_stats();

  /* Metrics tests */
  test_metrics();

  /* Peer removal with work tests */
  test_remove_peer_with_work();

  /* Peer eviction tests */
  test_evict_slowest_above_min();
  test_no_evict_at_minimum();
  test_evict_stops_at_minimum_floor();
  test_evict_all_equal_rates();
  test_evict_stalled_peer_zero_rate();

  test_suite_end();
  return test_global_summary();
}
