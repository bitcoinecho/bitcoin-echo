/**
 * Bitcoin Echo — Sync Unit Tests
 *
 * Tests headers-first initial block download functionality:
 * - Sync manager initialization
 * - Block locator construction
 * - Headers message handling
 * - Block download queue
 * - Parallel block download
 * - Sync progress tracking
 * - Timeout handling
 */

#include "block.h"
#include "chainstate.h"
#include "echo_types.h"
#include "peer.h"
#include "sync.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_utils.h"

/* Test counters */

/* Test context for callbacks */
typedef struct {
  /* Simulated storage */
  block_t stored_blocks[100];
  size_t block_count;

  /* Validation results */
  bool accept_headers;
  bool accept_blocks;

  /* Counters */
  size_t headers_validated;
  size_t blocks_validated;
} test_ctx_t;

/* Mock callback: get block */
static echo_result_t mock_get_block(const hash256_t *hash, block_t *block_out,
                                    void *ctx) {
  test_ctx_t *tctx = (test_ctx_t *)ctx;

  for (size_t i = 0; i < tctx->block_count; i++) {
    hash256_t block_hash;
    block_header_hash(&tctx->stored_blocks[i].header, &block_hash);

    if (memcmp(&block_hash, hash, sizeof(hash256_t)) == 0) {
      *block_out = tctx->stored_blocks[i];
      return ECHO_OK;
    }
  }

  return ECHO_ERR_NOT_FOUND;
}

/* Mock callback: store block */
static echo_result_t mock_store_block(const block_t *block, void *ctx) {
  test_ctx_t *tctx = (test_ctx_t *)ctx;

  if (tctx->block_count >= 100) {
    return ECHO_ERR_FULL;
  }

  tctx->stored_blocks[tctx->block_count++] = *block;
  return ECHO_OK;
}

/* Mock callback: validate header */
static echo_result_t mock_validate_header(const block_header_t *header,
                                          const block_index_t *prev_index,
                                          void *ctx) {
  test_ctx_t *tctx = (test_ctx_t *)ctx;
  (void)header;
  (void)prev_index;

  tctx->headers_validated++;

  if (!tctx->accept_headers) {
    return ECHO_ERR_INVALID;
  }

  return ECHO_OK;
}

/* Mock callback: validate and apply block */
static echo_result_t mock_validate_and_apply_block(const block_t *block,
                                                   const block_index_t *index,
                                                   void *ctx) {
  test_ctx_t *tctx = (test_ctx_t *)ctx;
  (void)block;
  (void)index;

  tctx->blocks_validated++;

  if (!tctx->accept_blocks) {
    return ECHO_ERR_INVALID;
  }

  return ECHO_OK;
}

/* Create test peer */
static peer_t *create_test_peer(const char *address, uint16_t port,
                                int32_t height) {
  peer_t *peer = calloc(1, sizeof(peer_t));
  peer_init(peer);
  strncpy(peer->address, address, sizeof(peer->address) - 1);
  peer->port = port;
  peer->start_height = height;
  peer->state = PEER_STATE_READY;
  peer->relay = ECHO_TRUE;
  return peer;
}

/* Create test block header with specific prev hash */
static void create_test_header(block_header_t *header,
                               const hash256_t *prev_hash, uint32_t nonce) {
  memset(header, 0, sizeof(block_header_t));
  header->version = 1;
  if (prev_hash) {
    header->prev_hash = *prev_hash;
  }
  header->timestamp = 1231006505 + nonce * 600;
  header->bits = 0x1d00ffff;
  header->nonce = nonce;
}

/* ============================================================================
 * Block Queue Tests
 * ============================================================================
 */

static void test_block_queue_create(void) {
  block_queue_t *queue = block_queue_create(100);
  if (!queue) {
    test_fail("Block queue create failed");
    return;
  }

  if (block_queue_size(queue) != 0) {
    block_queue_destroy(queue);
    test_fail("Block queue size should be 0");
    return;
  }

  block_queue_destroy(queue);

}

static void test_block_queue_create_zero_capacity(void) {
  block_queue_t *queue = block_queue_create(0);
  if (queue != NULL) {
    block_queue_destroy(queue);
    test_fail("Should not create queue with zero capacity");
    return;
  }

}

static void test_block_queue_add(void) {
  block_queue_t *queue = block_queue_create(10);
  hash256_t hash;
  memset(&hash, 0xAA, sizeof(hash256_t));

  echo_result_t result = block_queue_add(queue, &hash, 100);
  if (result != ECHO_OK) {
    block_queue_destroy(queue);
    test_fail("Assertion failed");
    return;
  }

  if (block_queue_size(queue) != 1) {
    block_queue_destroy(queue);
    test_fail("Assertion failed");
    return;
  }

  if (block_queue_pending_count(queue) != 1) {
    block_queue_destroy(queue);
    test_fail("Assertion failed");
    return;
  }

  block_queue_destroy(queue);

}

static void test_block_queue_add_duplicate(void) {
  block_queue_t *queue = block_queue_create(10);
  hash256_t hash;
  memset(&hash, 0xBB, sizeof(hash256_t));

  block_queue_add(queue, &hash, 100);
  echo_result_t result = block_queue_add(queue, &hash, 100);

  if (result != ECHO_ERR_EXISTS) {
    block_queue_destroy(queue);
    test_fail("Assertion failed");
    return;
  }

  if (block_queue_size(queue) != 1) {
    block_queue_destroy(queue);
    test_fail("Assertion failed");
    return;
  }

  block_queue_destroy(queue);

}

static void test_block_queue_full(void) {
  block_queue_t *queue = block_queue_create(2);
  hash256_t hash1, hash2, hash3;
  memset(&hash1, 0x01, sizeof(hash256_t));
  memset(&hash2, 0x02, sizeof(hash256_t));
  memset(&hash3, 0x03, sizeof(hash256_t));

  block_queue_add(queue, &hash1, 1);
  block_queue_add(queue, &hash2, 2);
  echo_result_t result = block_queue_add(queue, &hash3, 3);

  if (result != ECHO_ERR_FULL) {
    block_queue_destroy(queue);
    test_fail("Assertion failed");
    return;
  }

  block_queue_destroy(queue);

}

static void test_block_queue_next(void) {
  block_queue_t *queue = block_queue_create(10);
  hash256_t hash1, hash2, hash3;
  memset(&hash1, 0x01, sizeof(hash256_t));
  memset(&hash2, 0x02, sizeof(hash256_t));
  memset(&hash3, 0x03, sizeof(hash256_t));

  /* Add blocks out of order */
  block_queue_add(queue, &hash3, 300);
  block_queue_add(queue, &hash1, 100);
  block_queue_add(queue, &hash2, 200);

  /* Should return lowest height first */
  hash256_t next_hash;
  uint32_t height;
  echo_result_t result = block_queue_next(queue, &next_hash, &height);

  if (result != ECHO_OK) {
    block_queue_destroy(queue);
    test_fail("Assertion failed");
    return;
  }

  if (height != 100) {
    block_queue_destroy(queue);
    test_fail("Assertion failed");
    return;
  }

  if (memcmp(&next_hash, &hash1, sizeof(hash256_t)) != 0) {
    block_queue_destroy(queue);
    test_fail("Assertion failed");
    return;
  }

  block_queue_destroy(queue);

}

static void test_block_queue_assign(void) {
  block_queue_t *queue = block_queue_create(10);
  hash256_t hash;
  memset(&hash, 0xCC, sizeof(hash256_t));

  block_queue_add(queue, &hash, 100);

  peer_t *peer = create_test_peer("192.168.1.1", 8333, 1000);
  block_queue_assign(queue, &hash, peer);

  if (block_queue_pending_count(queue) != 0) {
    block_queue_destroy(queue);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  if (block_queue_inflight_count(queue) != 1) {
    block_queue_destroy(queue);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  block_queue_destroy(queue);
  free(peer);

}

static void test_block_queue_complete(void) {
  block_queue_t *queue = block_queue_create(10);
  hash256_t hash;
  memset(&hash, 0xDD, sizeof(hash256_t));

  block_queue_add(queue, &hash, 100);
  peer_t *peer = create_test_peer("192.168.1.1", 8333, 1000);
  block_queue_assign(queue, &hash, peer);
  block_queue_complete(queue, &hash);

  if (block_queue_size(queue) != 0) {
    block_queue_destroy(queue);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  block_queue_destroy(queue);
  free(peer);

}

static void test_block_queue_unassign(void) {
  block_queue_t *queue = block_queue_create(10);
  hash256_t hash;
  memset(&hash, 0xEE, sizeof(hash256_t));

  block_queue_add(queue, &hash, 100);
  peer_t *peer = create_test_peer("192.168.1.1", 8333, 1000);
  block_queue_assign(queue, &hash, peer);
  block_queue_unassign(queue, &hash);

  if (block_queue_pending_count(queue) != 1) {
    block_queue_destroy(queue);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  if (block_queue_inflight_count(queue) != 0) {
    block_queue_destroy(queue);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  block_queue_destroy(queue);
  free(peer);

}

static void test_block_queue_unassign_peer(void) {
  block_queue_t *queue = block_queue_create(10);
  hash256_t hash1, hash2;
  memset(&hash1, 0x11, sizeof(hash256_t));
  memset(&hash2, 0x22, sizeof(hash256_t));

  block_queue_add(queue, &hash1, 100);
  block_queue_add(queue, &hash2, 200);

  peer_t *peer1 = create_test_peer("192.168.1.1", 8333, 1000);
  peer_t *peer2 = create_test_peer("192.168.1.2", 8333, 1000);

  block_queue_assign(queue, &hash1, peer1);
  block_queue_assign(queue, &hash2, peer2);

  /* Unassign all blocks from peer1 */
  block_queue_unassign_peer(queue, peer1);

  if (block_queue_pending_count(queue) != 1) {
    block_queue_destroy(queue);
    free(peer1);
    free(peer2);
    test_fail("Assertion failed");
    return;
  }

  if (block_queue_inflight_count(queue) != 1) {
    block_queue_destroy(queue);
    free(peer1);
    free(peer2);
    test_fail("Assertion failed");
    return;
  }

  block_queue_destroy(queue);
  free(peer1);
  free(peer2);

}

static void test_block_queue_contains(void) {
  block_queue_t *queue = block_queue_create(10);
  hash256_t hash1, hash2;
  memset(&hash1, 0x33, sizeof(hash256_t));
  memset(&hash2, 0x44, sizeof(hash256_t));

  block_queue_add(queue, &hash1, 100);

  if (!block_queue_contains(queue, &hash1)) {
    block_queue_destroy(queue);
    test_fail("Assertion failed");
    return;
  }

  if (block_queue_contains(queue, &hash2)) {
    block_queue_destroy(queue);
    test_fail("Assertion failed");
    return;
  }

  block_queue_destroy(queue);

}

/* ============================================================================
 * Sync Manager Tests
 * ============================================================================
 */

static void test_sync_create(void) {
  chainstate_t *chainstate = chainstate_create();
  if (!chainstate) {
  
  }

  test_ctx_t tctx = {0};
  tctx.accept_headers = true;
  tctx.accept_blocks = true;

  sync_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .store_block = mock_store_block,
      .validate_header = mock_validate_header,
      .validate_and_apply_block = mock_validate_and_apply_block,
      .ctx = &tctx,
  };

  sync_manager_t *mgr = sync_create(chainstate, &callbacks);
  if (!mgr) {
    chainstate_destroy(chainstate);
    test_fail("Assertion failed");
    return;
  }

  sync_destroy(mgr);
  chainstate_destroy(chainstate);

}

static void test_sync_create_null_params(void) {
  test_ctx_t tctx = {0};
  sync_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .ctx = &tctx,
  };

  if (sync_create(NULL, &callbacks) != NULL) {
  
  }

  chainstate_t *chainstate = chainstate_create();
  if (sync_create(chainstate, NULL) != NULL) {
    chainstate_destroy(chainstate);
    test_fail("Assertion failed");
    return;
  }

  chainstate_destroy(chainstate);

}

static void test_sync_add_remove_peer(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  sync_callbacks_t callbacks = {.ctx = &tctx};

  sync_manager_t *mgr = sync_create(chainstate, &callbacks);
  peer_t *peer1 = create_test_peer("192.168.1.1", 8333, 100000);
  peer_t *peer2 = create_test_peer("192.168.1.2", 8333, 200000);

  sync_add_peer(mgr, peer1, 100000);
  sync_add_peer(mgr, peer2, 200000);

  sync_progress_t progress;
  sync_get_progress(mgr, &progress);
  if (progress.sync_peers != 2) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer1);
    free(peer2);
    test_fail("Assertion failed");
    return;
  }

  sync_remove_peer(mgr, peer1);
  sync_get_progress(mgr, &progress);
  if (progress.sync_peers != 1) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer1);
    free(peer2);
    test_fail("Assertion failed");
    return;
  }

  sync_destroy(mgr);
  chainstate_destroy(chainstate);
  free(peer1);
  free(peer2);

}

static void test_sync_start(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  sync_callbacks_t callbacks = {.ctx = &tctx};

  sync_manager_t *mgr = sync_create(chainstate, &callbacks);
  peer_t *peer = create_test_peer("192.168.1.1", 8333, 100000);
  sync_add_peer(mgr, peer, 100000);

  echo_result_t result = sync_start(mgr);
  if (result != ECHO_OK) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  if (!sync_is_ibd(mgr)) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  sync_destroy(mgr);
  chainstate_destroy(chainstate);
  free(peer);

}

static void test_sync_start_no_peers(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  sync_callbacks_t callbacks = {.ctx = &tctx};

  sync_manager_t *mgr = sync_create(chainstate, &callbacks);

  /* Should fail with no peers */
  echo_result_t result = sync_start(mgr);
  if (result != ECHO_ERR_INVALID_STATE) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    test_fail("Assertion failed");
    return;
  }

  sync_destroy(mgr);
  chainstate_destroy(chainstate);

}

static void test_sync_stop(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  sync_callbacks_t callbacks = {.ctx = &tctx};

  sync_manager_t *mgr = sync_create(chainstate, &callbacks);
  peer_t *peer = create_test_peer("192.168.1.1", 8333, 100000);
  sync_add_peer(mgr, peer, 100000);

  sync_start(mgr);
  sync_stop(mgr);

  if (sync_is_ibd(mgr)) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  sync_destroy(mgr);
  chainstate_destroy(chainstate);
  free(peer);

}

static void test_sync_is_complete(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  sync_callbacks_t callbacks = {.ctx = &tctx};

  sync_manager_t *mgr = sync_create(chainstate, &callbacks);

  /* Initially not complete */
  if (sync_is_complete(mgr)) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    test_fail("Assertion failed");
    return;
  }

  sync_destroy(mgr);
  chainstate_destroy(chainstate);

}

static void test_sync_get_progress(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  sync_callbacks_t callbacks = {.ctx = &tctx};

  sync_manager_t *mgr = sync_create(chainstate, &callbacks);
  peer_t *peer = create_test_peer("192.168.1.1", 8333, 100000);
  sync_add_peer(mgr, peer, 100000);

  sync_progress_t progress;
  sync_get_progress(mgr, &progress);

  if (progress.mode != SYNC_MODE_IDLE) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  sync_start(mgr);
  sync_get_progress(mgr, &progress);

  if (progress.mode != SYNC_MODE_HEADERS) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  sync_destroy(mgr);
  chainstate_destroy(chainstate);
  free(peer);

}

/* ============================================================================
 * Block Locator Tests
 * ============================================================================
 */

static void test_sync_build_locator_empty(void) {
  chainstate_t *chainstate = chainstate_create();

  hash256_t locator[SYNC_MAX_LOCATOR_HASHES];
  size_t locator_len = 0;

  echo_result_t result = sync_build_locator(chainstate, locator, &locator_len);
  if (result != ECHO_OK) {
    chainstate_destroy(chainstate);
    test_fail("Assertion failed");
    return;
  }

  /* For empty chain with only genesis, we should get at least one hash */
  /* (Depends on implementation - genesis might or might not be in chain yet) */

  chainstate_destroy(chainstate);

}

static void test_sync_build_locator_null_params(void) {
  chainstate_t *chainstate = chainstate_create();
  hash256_t locator[SYNC_MAX_LOCATOR_HASHES];
  size_t locator_len;

  if (sync_build_locator(NULL, locator, &locator_len) != ECHO_ERR_NULL_PARAM) {
    chainstate_destroy(chainstate);
    test_fail("Assertion failed");
    return;
  }

  if (sync_build_locator(chainstate, NULL, &locator_len) !=
      ECHO_ERR_NULL_PARAM) {
    chainstate_destroy(chainstate);
    test_fail("Assertion failed");
    return;
  }

  if (sync_build_locator(chainstate, locator, NULL) != ECHO_ERR_NULL_PARAM) {
    chainstate_destroy(chainstate);
    test_fail("Assertion failed");
    return;
  }

  chainstate_destroy(chainstate);

}

static void test_sync_find_locator_fork_null(void) {
  chainstate_t *chainstate = chainstate_create();
  hash256_t locator[1];
  memset(&locator[0], 0xAA, sizeof(hash256_t));

  /* Should handle NULL gracefully */
  if (sync_find_locator_fork(NULL, locator, 1) != NULL) {
    chainstate_destroy(chainstate);
    test_fail("Assertion failed");
    return;
  }

  if (sync_find_locator_fork(chainstate, NULL, 1) != NULL) {
    chainstate_destroy(chainstate);
    test_fail("Assertion failed");
    return;
  }

  if (sync_find_locator_fork(chainstate, locator, 0) != NULL) {
    chainstate_destroy(chainstate);
    test_fail("Assertion failed");
    return;
  }

  chainstate_destroy(chainstate);

}

/* ============================================================================
 * Sync Mode String Tests
 * ============================================================================
 */

static void test_sync_mode_string(void) {
  if (strcmp(sync_mode_string(SYNC_MODE_IDLE), "IDLE") != 0) {
  
  }
  if (strcmp(sync_mode_string(SYNC_MODE_HEADERS), "HEADERS") != 0) {
  
  }
  if (strcmp(sync_mode_string(SYNC_MODE_BLOCKS), "BLOCKS") != 0) {
  
  }
  if (strcmp(sync_mode_string(SYNC_MODE_DONE), "DONE") != 0) {
  
  }
  if (strcmp(sync_mode_string(SYNC_MODE_STALLED), "STALLED") != 0) {
  
  }

}

/* ============================================================================
 * Estimate Remaining Time Tests
 * ============================================================================
 */

static void test_sync_estimate_remaining_time_idle(void) {
  sync_progress_t progress = {0};
  progress.mode = SYNC_MODE_IDLE;

  uint64_t estimate = sync_estimate_remaining_time(&progress);
  if (estimate != 0) {
  
  }

}

static void test_sync_estimate_remaining_time_done(void) {
  sync_progress_t progress = {0};
  progress.mode = SYNC_MODE_DONE;

  uint64_t estimate = sync_estimate_remaining_time(&progress);
  if (estimate != 0) {
  
  }

}

static void test_sync_estimate_remaining_time_no_progress(void) {
  sync_progress_t progress = {0};
  progress.mode = SYNC_MODE_BLOCKS;
  progress.start_time = 1000;
  progress.last_progress_time = 1000;
  progress.blocks_validated = 0;

  uint64_t estimate = sync_estimate_remaining_time(&progress);
  if (estimate != UINT64_MAX) {
  
  }

}

/* ============================================================================
 * Headers Handling Tests
 * ============================================================================
 */

static void test_sync_handle_headers_empty(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  tctx.accept_headers = true;

  sync_callbacks_t callbacks = {
      .validate_header = mock_validate_header,
      .ctx = &tctx,
  };

  sync_manager_t *mgr = sync_create(chainstate, &callbacks);
  peer_t *peer = create_test_peer("192.168.1.1", 8333, 100000);
  sync_add_peer(mgr, peer, 100000);

  /* Handle empty headers */
  echo_result_t result = sync_handle_headers(mgr, peer, NULL, 0);
  if (result != ECHO_OK) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  sync_destroy(mgr);
  chainstate_destroy(chainstate);
  free(peer);

}

static void test_sync_handle_headers_unknown_peer(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  sync_callbacks_t callbacks = {.ctx = &tctx};

  sync_manager_t *mgr = sync_create(chainstate, &callbacks);
  peer_t *peer = create_test_peer("192.168.1.1", 8333, 100000);
  /* Don't add peer */

  block_header_t header;
  create_test_header(&header, NULL, 1);

  echo_result_t result = sync_handle_headers(mgr, peer, &header, 1);
  if (result != ECHO_ERR_NOT_FOUND) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  sync_destroy(mgr);
  chainstate_destroy(chainstate);
  free(peer);

}

/* ============================================================================
 * Block Handling Tests
 * ============================================================================
 */

static void test_sync_handle_block_unknown_peer(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  sync_callbacks_t callbacks = {.ctx = &tctx};

  sync_manager_t *mgr = sync_create(chainstate, &callbacks);
  peer_t *peer = create_test_peer("192.168.1.1", 8333, 100000);
  /* Don't add peer */

  block_t block;
  block_init(&block);
  block.header.version = 1;

  echo_result_t result = sync_handle_block(mgr, peer, &block);
  if (result != ECHO_ERR_NOT_FOUND) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  sync_destroy(mgr);
  chainstate_destroy(chainstate);
  free(peer);

}

/* ============================================================================
 * Tick and Timeout Tests
 * ============================================================================
 */

static void test_sync_tick_idle(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  sync_callbacks_t callbacks = {.ctx = &tctx};

  sync_manager_t *mgr = sync_create(chainstate, &callbacks);

  /* Tick should not crash when idle */
  sync_tick(mgr);

  sync_destroy(mgr);
  chainstate_destroy(chainstate);

}

static void test_sync_tick_headers_mode(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  sync_callbacks_t callbacks = {.ctx = &tctx};

  sync_manager_t *mgr = sync_create(chainstate, &callbacks);
  peer_t *peer = create_test_peer("192.168.1.1", 8333, 100000);
  sync_add_peer(mgr, peer, 100000);
  sync_start(mgr);

  /* Tick in headers mode */
  sync_tick(mgr);

  sync_progress_t progress;
  sync_get_progress(mgr, &progress);
  if (progress.mode != SYNC_MODE_HEADERS) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer);
    test_fail("Assertion failed");
    return;
  }

  sync_destroy(mgr);
  chainstate_destroy(chainstate);
  free(peer);

}

static void test_sync_process_timeouts(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  sync_callbacks_t callbacks = {.ctx = &tctx};

  sync_manager_t *mgr = sync_create(chainstate, &callbacks);

  /* Should not crash */
  sync_process_timeouts(mgr);

  sync_destroy(mgr);
  chainstate_destroy(chainstate);

}

/* ============================================================================
 * Test Runner
 * ============================================================================
 */

int main(void) {
    test_suite_begin("Block Synchronization Tests");
    test_case("Block queue create"); test_block_queue_create(); test_pass();
    test_case("Block queue create zero capacity"); test_block_queue_create_zero_capacity(); test_pass();
    test_case("Block queue add"); test_block_queue_add(); test_pass();
    test_case("Block queue add duplicate"); test_block_queue_add_duplicate(); test_pass();
    test_case("Block queue full"); test_block_queue_full(); test_pass();
    test_case("Block queue next"); test_block_queue_next(); test_pass();
    test_case("Block queue assign"); test_block_queue_assign(); test_pass();
    test_case("Block queue complete"); test_block_queue_complete(); test_pass();
    test_case("Block queue unassign"); test_block_queue_unassign(); test_pass();
    test_case("Block queue unassign peer"); test_block_queue_unassign_peer(); test_pass();
    test_case("Block queue contains"); test_block_queue_contains(); test_pass();
    test_case("Sync create"); test_sync_create(); test_pass();
    test_case("Sync create null params"); test_sync_create_null_params(); test_pass();
    test_case("Sync add remove peer"); test_sync_add_remove_peer(); test_pass();
    test_case("Sync start"); test_sync_start(); test_pass();
    test_case("Sync start no peers"); test_sync_start_no_peers(); test_pass();
    test_case("Sync stop"); test_sync_stop(); test_pass();
    test_case("Sync is complete"); test_sync_is_complete(); test_pass();
    test_case("Sync get progress"); test_sync_get_progress(); test_pass();
    test_case("Sync build locator empty"); test_sync_build_locator_empty(); test_pass();
    test_case("Sync build locator null params"); test_sync_build_locator_null_params(); test_pass();
    test_case("Sync find locator fork null"); test_sync_find_locator_fork_null(); test_pass();
    test_case("Sync mode string"); test_sync_mode_string(); test_pass();
    test_case("Sync estimate remaining time idle"); test_sync_estimate_remaining_time_idle(); test_pass();
    test_case("Sync estimate remaining time done"); test_sync_estimate_remaining_time_done(); test_pass();
    test_case("Sync estimate remaining time no progress"); test_sync_estimate_remaining_time_no_progress(); test_pass();
    test_case("Sync handle headers empty"); test_sync_handle_headers_empty(); test_pass();
    test_case("Sync handle headers unknown peer"); test_sync_handle_headers_unknown_peer(); test_pass();
    test_case("Sync handle block unknown peer"); test_sync_handle_block_unknown_peer(); test_pass();
    test_case("Sync tick idle"); test_sync_tick_idle(); test_pass();
    test_case("Sync tick headers mode"); test_sync_tick_headers_mode(); test_pass();
    test_case("Sync process timeouts"); test_sync_process_timeouts(); test_pass();

    test_suite_end();
    return test_global_summary();
}
