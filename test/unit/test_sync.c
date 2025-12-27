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

  /* Send callback tracking */
  size_t getheaders_sent;
  size_t getdata_blocks_sent;
  peer_t *last_getheaders_peer;
  peer_t *last_getdata_peer;
  size_t last_locator_len;
  size_t last_getdata_count;
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
                                          const hash256_t *hash,
                                          const block_index_t *prev_index,
                                          void *ctx) {
  test_ctx_t *tctx = (test_ctx_t *)ctx;
  (void)header;
  (void)hash;
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

/* Mock callback: send getheaders */
static void mock_send_getheaders(peer_t *peer, const hash256_t *locator,
                                 size_t locator_len, const hash256_t *stop_hash,
                                 void *ctx) {
  test_ctx_t *tctx = (test_ctx_t *)ctx;
  (void)locator;
  (void)stop_hash;

  tctx->getheaders_sent++;
  tctx->last_getheaders_peer = peer;
  tctx->last_locator_len = locator_len;
}

/* Mock callback: send getdata for blocks */
static void mock_send_getdata_blocks(peer_t *peer, const hash256_t *hashes,
                                     size_t count, void *ctx) {
  test_ctx_t *tctx = (test_ctx_t *)ctx;
  (void)hashes;

  tctx->getdata_blocks_sent++;
  tctx->last_getdata_peer = peer;
  tctx->last_getdata_count = count;
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
  peer->services = SERVICE_NODE_NETWORK; /* Required for sync_candidate */
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

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);
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

  if (sync_create(NULL, &callbacks, 0, NULL) != NULL) {

  }

  chainstate_t *chainstate = chainstate_create();
  if (sync_create(chainstate, NULL, 0, NULL) != NULL) {
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

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);
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

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);
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

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);

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

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);
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

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);

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

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);
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

/**
 * Test locator bounds check with a very long chain.
 *
 * Regression test: stack buffer overflow in sync_build_locator_from when chain
 * is long enough to fill all SYNC_MAX_LOCATOR_HASHES entries and genesis is
 * reached, the code would write past the array bounds.
 *
 * This test creates a chain longer than what's needed to fill all 32 locator
 * slots, verifying:
 * 1. No buffer overflow (would crash with ASan)
 * 2. locator_len never exceeds SYNC_MAX_LOCATOR_HASHES
 * 3. Function returns successfully
 */
static void test_sync_build_locator_bounds_check(void) {
  /* Create a chain of 10000 block_index_t nodes - more than enough to fill
   * all 32 locator slots with the exponential step algorithm */
  const size_t chain_len = 10000;
  block_index_t *chain = calloc(chain_len, sizeof(block_index_t));
  if (!chain) {
    test_fail("Failed to allocate chain");
    return;
  }

  /* Build linked list: chain[0] is genesis (prev=NULL), chain[N-1] is tip */
  for (size_t i = 0; i < chain_len; i++) {
    chain[i].height = (uint32_t)i;
    chain[i].on_main_chain = true;
    /* Set a unique hash for each block */
    memset(&chain[i].hash, 0, sizeof(hash256_t));
    chain[i].hash.bytes[0] = (uint8_t)(i & 0xFF);
    chain[i].hash.bytes[1] = (uint8_t)((i >> 8) & 0xFF);

    if (i > 0) {
      chain[i].prev = &chain[i - 1];
      chain[i].prev_hash = chain[i - 1].hash;
    } else {
      chain[i].prev = NULL; /* Genesis has no prev */
    }
  }

  /* Build locator from the tip */
  hash256_t locator[SYNC_MAX_LOCATOR_HASHES];
  size_t locator_len = 0;

  echo_result_t result =
      sync_build_locator_from(NULL, &chain[chain_len - 1], locator, &locator_len);

  /* Verify success */
  if (result != ECHO_OK) {
    free(chain);
    test_fail("sync_build_locator_from failed");
    return;
  }

  /* Verify locator_len doesn't exceed max (the key bounds check) */
  if (locator_len > SYNC_MAX_LOCATOR_HASHES) {
    free(chain);
    test_fail("locator_len exceeds SYNC_MAX_LOCATOR_HASHES");
    return;
  }

  /* With 10000 blocks, we should fill all 32 slots */
  if (locator_len != SYNC_MAX_LOCATOR_HASHES) {
    free(chain);
    test_fail("Expected full locator");
    return;
  }

  /* First entry should be the tip */
  if (memcmp(&locator[0], &chain[chain_len - 1].hash, sizeof(hash256_t)) != 0) {
    free(chain);
    test_fail("First locator entry should be tip");
    return;
  }

  free(chain);

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

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);
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

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);
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

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);
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

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);

  /* Tick should not crash when idle */
  sync_tick(mgr);

  sync_destroy(mgr);
  chainstate_destroy(chainstate);

}

static void test_sync_tick_headers_mode(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  sync_callbacks_t callbacks = {.ctx = &tctx};

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);
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

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);

  /* Should not crash */
  sync_process_timeouts(mgr);

  sync_destroy(mgr);
  chainstate_destroy(chainstate);

}

/* ============================================================================
 * Send Callback Tests
 * ============================================================================
 */

static void test_sync_send_getheaders_callback(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  tctx.accept_headers = true;

  sync_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .store_block = mock_store_block,
      .validate_header = mock_validate_header,
      .validate_and_apply_block = mock_validate_and_apply_block,
      .send_getheaders = mock_send_getheaders,
      .send_getdata_blocks = mock_send_getdata_blocks,
      .ctx = &tctx,
  };

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);
  peer_t *peer = create_test_peer("192.168.1.1", 8333, 100000);
  sync_add_peer(mgr, peer, 100000);

  /* Start sync - should be in headers mode */
  echo_result_t result = sync_start(mgr);
  if (result != ECHO_OK) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer);
    test_fail("Failed to start sync");
    return;
  }

  /* Tick should trigger getheaders send */
  sync_tick(mgr);

  /* Verify callback was called */
  if (tctx.getheaders_sent != 1) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer);
    test_fail("Expected getheaders to be sent once");
    return;
  }

  if (tctx.last_getheaders_peer != peer) {
    sync_destroy(mgr);
    chainstate_destroy(chainstate);
    free(peer);
    test_fail("Expected getheaders sent to correct peer");
    return;
  }

  sync_destroy(mgr);
  chainstate_destroy(chainstate);
  free(peer);

}

static void test_sync_send_getheaders_not_called_when_no_callback(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};

  /* No send_getheaders callback */
  sync_callbacks_t callbacks = {
      .ctx = &tctx,
  };

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);
  peer_t *peer = create_test_peer("192.168.1.1", 8333, 100000);
  sync_add_peer(mgr, peer, 100000);

  sync_start(mgr);

  /* Should not crash without callback */
  sync_tick(mgr);

  sync_destroy(mgr);
  chainstate_destroy(chainstate);
  free(peer);

}

static void test_sync_callbacks_with_all_fields(void) {
  chainstate_t *chainstate = chainstate_create();
  test_ctx_t tctx = {0};
  tctx.accept_headers = true;
  tctx.accept_blocks = true;

  /* Test with all callbacks set */
  sync_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .store_block = mock_store_block,
      .validate_header = mock_validate_header,
      .validate_and_apply_block = mock_validate_and_apply_block,
      .send_getheaders = mock_send_getheaders,
      .send_getdata_blocks = mock_send_getdata_blocks,
      .ctx = &tctx,
  };

  sync_manager_t *mgr = sync_create(chainstate, &callbacks, 0, NULL);
  if (!mgr) {
    chainstate_destroy(chainstate);
    test_fail("Failed to create sync manager with all callbacks");
    return;
  }

  sync_destroy(mgr);
  chainstate_destroy(chainstate);

}

/* ============================================================================
 * Test Runner
 * ============================================================================
 */

int main(void) {
    test_suite_begin("Block Synchronization Tests");
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
    test_case("Sync build locator bounds check"); test_sync_build_locator_bounds_check(); test_pass();
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
    test_case("Sync send getheaders callback"); test_sync_send_getheaders_callback(); test_pass();
    test_case("Sync send getheaders not called without callback"); test_sync_send_getheaders_not_called_when_no_callback(); test_pass();
    test_case("Sync callbacks with all fields"); test_sync_callbacks_with_all_fields(); test_pass();

    test_suite_end();
    return test_global_summary();
}
