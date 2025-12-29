/**
 * Bitcoin Echo — Download Manager Unit Tests
 *
 * Tests performance-based block download functionality:
 * - Manager initialization and destruction
 * - Peer management (add/remove)
 * - Work queue operations
 * - Work distribution to peers
 * - Block receipt handling
 * - Stall detection
 * - Work splitting from slow peers
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

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
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

  download_mgr_t *mgr = download_mgr_create(NULL, 1024);
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

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
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

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
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

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
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

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
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
  test_case("add work items");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);

  hash256_t hashes[10];
  uint32_t heights[10];
  for (uint32_t i = 0; i < 10; i++) {
    make_test_hash(&hashes[i], i + 100);
    heights[i] = i + 100;
  }

  size_t added = download_mgr_add_work(mgr, hashes, heights, 10);

  if (added != 10) {
    test_fail_uint("blocks added", 10, added);
    download_mgr_destroy(mgr);
    return;
  }

  if (download_mgr_pending_count(mgr) != 10) {
    test_fail_uint("pending count", 10, download_mgr_pending_count(mgr));
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_has_block(void) {
  test_case("check if block in queue");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);

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
 * Work Distribution Tests
 * ============================================================================
 */

static void test_distribute_work(void) {
  test_case("distribute work to peer");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
  mock_peer_t peer1 = {.id = 1};

  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  hash256_t hashes[3];
  uint32_t heights[3];
  for (uint32_t i = 0; i < 3; i++) {
    make_test_hash(&hashes[i], i + 100);
    heights[i] = i + 100;
  }
  download_mgr_add_work(mgr, hashes, heights, 3);

  size_t assigned = download_mgr_distribute_work(mgr);

  if (assigned != 3) {
    test_fail_uint("blocks assigned", 3, assigned);
    download_mgr_destroy(mgr);
    return;
  }

  /* With batching optimization, all 3 blocks go in ONE getdata call */
  if (ctx.getdata_calls != 1) {
    test_fail_uint("getdata calls (batched)", 1, ctx.getdata_calls);
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

static void test_distribute_round_robin(void) {
  test_case("distribute work round-robin");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
  mock_peer_t peers[3];
  for (int i = 0; i < 3; i++) {
    peers[i].id = i;
    download_mgr_add_peer(mgr, (peer_t *)&peers[i]);
  }

  /* With batching, first peer fills to capacity (16) before next peer gets
   * work. Add enough blocks to require all 3 peers: 48 blocks = 16 per peer */
  hash256_t hashes[48];
  uint32_t heights[48];
  for (uint32_t i = 0; i < 48; i++) {
    make_test_hash(&hashes[i], i + 100);
    heights[i] = i + 100;
  }
  download_mgr_add_work(mgr, hashes, heights, 48);

  download_mgr_distribute_work(mgr);

  /* All three peers should have work (16 blocks each) */
  download_metrics_t metrics;
  download_mgr_get_metrics(mgr, &metrics);

  if (metrics.active_peers != 3) {
    test_fail_uint("active peers", 3, metrics.active_peers);
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_no_work_without_peers(void) {
  test_case("no distribution without peers");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);

  hash256_t hash;
  uint32_t height = 100;
  make_test_hash(&hash, height);
  download_mgr_add_work(mgr, &hash, &height, 1);

  size_t assigned = download_mgr_distribute_work(mgr);

  if (assigned != 0) {
    test_fail_uint("assigned without peers", 0, assigned);
    download_mgr_destroy(mgr);
    return;
  }

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

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
  mock_peer_t peer1 = {.id = 1};
  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  hash256_t hash;
  uint32_t height = 100;
  make_test_hash(&hash, height);
  download_mgr_add_work(mgr, &hash, &height, 1);
  download_mgr_distribute_work(mgr);

  bool result = download_mgr_block_received(mgr, (peer_t *)&peer1, &hash, 1000);

  if (!result) {
    test_fail("should accept expected block");
    download_mgr_destroy(mgr);
    return;
  }

  if (download_mgr_inflight_count(mgr) != 0) {
    test_fail_uint("inflight after receive", 0, download_mgr_inflight_count(mgr));
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

static void test_unexpected_block(void) {
  test_case("late/unrequested block from peer (libbitcoin-style accept)");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
  mock_peer_t peer1 = {.id = 1};
  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  hash256_t hash;
  make_test_hash(&hash, 999); /* Not in queue */

  /* libbitcoin-style: accept unrequested/late blocks gracefully.
   * The block will still be stored by sync.c, just not tracked for work.
   * The peer still gets throughput credit for the bytes delivered. */
  bool result = download_mgr_block_received(mgr, (peer_t *)&peer1, &hash, 1000);

  if (!result) {
    test_fail("should accept late/unrequested block (libbitcoin-style)");
    download_mgr_destroy(mgr);
    return;
  }

  download_mgr_destroy(mgr);
  test_pass();
}

/* ============================================================================
 * Block Completion Tests
 * ============================================================================
 */

static void test_block_complete(void) {
  test_case("mark block complete");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
  mock_peer_t peer1 = {.id = 1};
  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  hash256_t hash;
  uint32_t height = 100;
  make_test_hash(&hash, height);
  download_mgr_add_work(mgr, &hash, &height, 1);
  download_mgr_distribute_work(mgr);
  download_mgr_block_received(mgr, (peer_t *)&peer1, &hash, 1000);

  download_mgr_block_complete(mgr, &hash, height);

  if (download_mgr_has_block(mgr, &hash)) {
    test_fail("block should be removed after completion");
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

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
  mock_peer_t peer1 = {.id = 1};
  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  float rate;
  uint32_t in_flight;
  bool found =
      download_mgr_get_peer_stats(mgr, (peer_t *)&peer1, &rate, &in_flight);

  if (!found) {
    test_fail("should find peer stats");
    download_mgr_destroy(mgr);
    return;
  }

  if (in_flight != 0) {
    test_fail_uint("initial in_flight", 0, in_flight);
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

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
  mock_peer_t unknown = {.id = 999};

  float rate;
  uint32_t in_flight;
  bool found =
      download_mgr_get_peer_stats(mgr, (peer_t *)&unknown, &rate, &in_flight);

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

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
  mock_peer_t peer1 = {.id = 1};
  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  hash256_t hashes[5];
  uint32_t heights[5];
  for (uint32_t i = 0; i < 5; i++) {
    make_test_hash(&hashes[i], i + 100);
    heights[i] = i + 100;
  }
  download_mgr_add_work(mgr, hashes, heights, 5);
  download_mgr_distribute_work(mgr);

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
 * Work Splitting Tests
 * ============================================================================
 */

static void test_split_work(void) {
  test_case("split work from peer");

  test_ctx_t ctx = {0};
  download_callbacks_t callbacks = {.send_getdata = mock_send_getdata,
                                    .disconnect_peer = mock_disconnect_peer,
                                    .ctx = &ctx};

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
  mock_peer_t peer1 = {.id = 1};
  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  hash256_t hashes[10];
  uint32_t heights[10];
  for (uint32_t i = 0; i < 10; i++) {
    make_test_hash(&hashes[i], i + 100);
    heights[i] = i + 100;
  }
  download_mgr_add_work(mgr, hashes, heights, 10);
  download_mgr_distribute_work(mgr);

  /* Split work from peer1 */
  size_t split = download_mgr_split_work(mgr, (peer_t *)&peer1);

  if (split != 5) { /* Half of 10 */
    test_fail_uint("blocks split", 5, split);
    download_mgr_destroy(mgr);
    return;
  }

  if (download_mgr_pending_count(mgr) != 5) {
    test_fail_uint("pending after split", 5, download_mgr_pending_count(mgr));
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

  download_mgr_t *mgr = download_mgr_create(&callbacks, 1024);
  mock_peer_t peer1 = {.id = 1};
  download_mgr_add_peer(mgr, (peer_t *)&peer1);

  hash256_t hashes[5];
  uint32_t heights[5];
  for (uint32_t i = 0; i < 5; i++) {
    make_test_hash(&hashes[i], i + 100);
    heights[i] = i + 100;
  }
  download_mgr_add_work(mgr, hashes, heights, 5);
  download_mgr_distribute_work(mgr);

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
 * Main
 * ============================================================================
 */

int main(void) {
  test_suite_begin("Download Manager tests");

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
  test_has_block();

  /* Work distribution tests */
  test_distribute_work();
  test_distribute_round_robin();
  test_no_work_without_peers();

  /* Block receipt tests */
  test_block_received();
  test_unexpected_block();

  /* Block completion tests */
  test_block_complete();

  /* Peer stats tests */
  test_get_peer_stats();
  test_get_unknown_peer_stats();

  /* Metrics tests */
  test_metrics();

  /* Work splitting tests */
  test_split_work();

  /* Peer removal with work tests */
  test_remove_peer_with_work();

  test_suite_end();
  return test_global_summary();
}
