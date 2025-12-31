/**
 * Bitcoin Echo — Relay Unit Tests
 *
 * Tests inventory and data relay functionality:
 * - Relay manager initialization
 * - Peer tracking
 * - inv message handling
 * - getdata message handling
 * - Block/tx reception and relay
 * - DoS prevention (rate limiting, banning)
 */

#include "block.h"
#include "echo_types.h"
#include "peer.h"
#include "platform.h"
#include "protocol.h"
#include "relay.h"
#include "tx.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_utils.h"

/* Test context for callbacks */
typedef struct {
  /* Simulated storage */
  block_t stored_blocks[10];
  size_t block_count;

  tx_t stored_txs[10];
  size_t tx_count;

  /* Validation results */
  echo_bool_t accept_blocks;
  echo_bool_t accept_txs;

  /* Counters */
  size_t blocks_processed;
  size_t txs_processed;
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
      return ECHO_SUCCESS;
    }
  }

  return ECHO_ERR_NOT_FOUND;
}

/* Mock callback: get transaction */
static echo_result_t mock_get_tx(const hash256_t *hash, tx_t *tx_out,
                                 void *ctx) {
  test_ctx_t *tctx = (test_ctx_t *)ctx;

  for (size_t i = 0; i < tctx->tx_count; i++) {
    hash256_t tx_hash;
    tx_compute_txid(&tctx->stored_txs[i], &tx_hash);

    if (memcmp(&tx_hash, hash, sizeof(hash256_t)) == 0) {
      *tx_out = tctx->stored_txs[i];
      return ECHO_SUCCESS;
    }
  }

  return ECHO_ERR_NOT_FOUND;
}

/* Mock callback: process block */
static echo_result_t mock_process_block(const block_t *block, void *ctx) {
  test_ctx_t *tctx = (test_ctx_t *)ctx;
  tctx->blocks_processed++;

  if (!tctx->accept_blocks) {
    return ECHO_ERR_INVALID;
  }

  /* Store block */
  if (tctx->block_count < 10) {
    tctx->stored_blocks[tctx->block_count++] = *block;
  }

  return ECHO_SUCCESS;
}

/* Mock callback: process transaction */
static echo_result_t mock_process_tx(const tx_t *tx, void *ctx) {
  test_ctx_t *tctx = (test_ctx_t *)ctx;
  tctx->txs_processed++;

  if (!tctx->accept_txs) {
    return ECHO_ERR_INVALID;
  }

  /* Store transaction */
  if (tctx->tx_count < 10) {
    tctx->stored_txs[tctx->tx_count++] = *tx;
  }

  return ECHO_SUCCESS;
}

/* Create test peer */
static peer_t *create_test_peer(const char *address, uint16_t port) {
  peer_t *peer = calloc(1, sizeof(peer_t));
  peer_init(peer);
  strncpy(peer->address, address, sizeof(peer->address) - 1);
  peer->port = port;
  peer->state = PEER_STATE_READY;
  peer->relay = ECHO_TRUE;
  return peer;
}

/* Destroy test peer - must call to free any queued message allocations */
static void destroy_test_peer(peer_t *peer) {
  if (peer) {
    peer_disconnect(peer, PEER_DISCONNECT_USER, "test cleanup");
    free(peer);
  }
}

/* ========== Test Cases ========== */

static void test_relay_init(void) {
  printf("test_relay_init\n");

  test_ctx_t tctx = {0};
  tctx.accept_blocks = ECHO_TRUE;
  tctx.accept_txs = ECHO_TRUE;

  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);
  if (mgr == NULL) {
    test_fail("test_relay_init: expected non-NULL relay_manager_t*, but got NULL");
    relay_destroy(mgr);
    return;
  }

  relay_destroy(mgr);

}

static void test_relay_add_remove_peer(void) {
  printf("test_relay_add_remove_peer\n");

  test_ctx_t tctx = {0};
  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);
  peer_t *peer1 = create_test_peer("192.168.1.1", 8333);
  peer_t *peer2 = create_test_peer("192.168.1.2", 8333);

  /* Add peers */
  relay_add_peer(mgr, peer1);
  relay_add_peer(mgr, peer2);

  /* Remove peer */
  relay_remove_peer(mgr, peer1);

  relay_destroy(mgr);
  destroy_test_peer(peer1);
  destroy_test_peer(peer2);

}

static void test_relay_handle_inv(void) {
  printf("test_relay_handle_inv\n");

  test_ctx_t tctx = {0};
  tctx.accept_blocks = ECHO_TRUE;

  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);
  peer_t *peer = create_test_peer("192.168.1.1", 8333);
  relay_add_peer(mgr, peer);

  /* Create inv message */
  msg_inv_t inv;
  inv.count = 1;
  inv_vector_t inv_vec;
  inv_vec.type = INV_BLOCK;
  memset(&inv_vec.hash, 0xAA, sizeof(hash256_t));
  inv.inventory = &inv_vec;

  /* Handle inv - should queue getdata */
  echo_result_t result = relay_handle_inv(mgr, peer, &inv);
  if (result != ECHO_SUCCESS) {
    test_fail_int("relay_handle_inv returned", (long)ECHO_SUCCESS, (long)result);
    relay_destroy(mgr);
    destroy_test_peer(peer);
    return;
  }

  relay_destroy(mgr);
  destroy_test_peer(peer);

}

static void test_relay_inv_rate_limit(void) {
  printf("test_relay_inv_rate_limit\n");

  test_ctx_t tctx = {0};
  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);
  peer_t *peer = create_test_peer("192.168.1.1", 8333);
  relay_add_peer(mgr, peer);

  msg_inv_t inv;
  inv.count = 1;
  inv_vector_t inv_vec;
  inv_vec.type = INV_BLOCK;
  memset(&inv_vec.hash, 0xAA, sizeof(hash256_t));
  inv.inventory = &inv_vec;

  /* Send MAX_INV_PER_SECOND messages - should succeed */
  for (size_t i = 0; i < MAX_INV_PER_SECOND; i++) {
    echo_result_t result;
    if ((result = relay_handle_inv(mgr, peer, &inv)) != ECHO_SUCCESS) {
      char message[80];
      snprintf(message, sizeof(message), "inv %zu should succeed", i);
      test_fail_int(message, (long)ECHO_SUCCESS, (long)result);
      relay_destroy(mgr);
      destroy_test_peer(peer);
      return;
    }
  }

  /* Next message should be rate limited */
  if (relay_handle_inv(mgr, peer, &inv) != ECHO_ERR_RATE_LIMIT) {
    test_fail("inv should be rate limited");
    relay_destroy(mgr);
    destroy_test_peer(peer);
    return;
  }

  relay_destroy(mgr);
  destroy_test_peer(peer);

}

static void test_relay_handle_getdata(void) {
  printf("test_relay_handle_getdata\n");

  test_ctx_t tctx = {0};

  /* Create a test block and store it */
  block_t block;
  memset(&block, 0, sizeof(block_t));
  block.header.version = 1;
  memset(&block.header.prev_hash, 0x11, sizeof(hash256_t));
  tctx.stored_blocks[0] = block;
  tctx.block_count = 1;

  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);
  peer_t *peer = create_test_peer("192.168.1.1", 8333);
  relay_add_peer(mgr, peer);

  /* Request the block */
  hash256_t block_hash;
  block_header_hash(&block.header, &block_hash);

  msg_getdata_t getdata;
  getdata.count = 1;
  inv_vector_t inv_vec;
  inv_vec.type = INV_BLOCK;
  inv_vec.hash = block_hash;
  getdata.inventory = &inv_vec;

  /* Handle getdata - should queue block message */
  echo_result_t result = relay_handle_getdata(mgr, peer, &getdata);
  if (result != ECHO_SUCCESS) {
    test_fail_int("relay_handle_getdata returned", (long)ECHO_SUCCESS, (long)result);
    relay_destroy(mgr);
    destroy_test_peer(peer);
    return;
  }

  relay_destroy(mgr);
  destroy_test_peer(peer);

}

static void test_relay_getdata_rate_limit(void) {
  printf("test_relay_getdata_rate_limit\n");

  test_ctx_t tctx = {0};
  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);
  peer_t *peer = create_test_peer("192.168.1.1", 8333);
  relay_add_peer(mgr, peer);

  msg_getdata_t getdata;
  getdata.count = 1;
  inv_vector_t inv_vec;
  inv_vec.type = INV_BLOCK;
  memset(&inv_vec.hash, 0xBB, sizeof(hash256_t));
  getdata.inventory = &inv_vec;

  /* Send MAX_GETDATA_PER_SECOND messages - should succeed */
  for (size_t i = 0; i < MAX_GETDATA_PER_SECOND; i++) {
    echo_result_t result;
    if ((result = relay_handle_getdata(mgr, peer, &getdata)) != ECHO_SUCCESS) {
      char message[80];
      snprintf(message, sizeof(message), "getdata %zu should succeed", i);
      test_fail_int(message, (long)ECHO_SUCCESS, (long)result);
      relay_destroy(mgr);
      destroy_test_peer(peer);
    }
  }

  /* Next message should be rate limited */
  if (relay_handle_getdata(mgr, peer, &getdata) != ECHO_ERR_RATE_LIMIT) {
    test_fail("getdata should be rate limited");
    relay_destroy(mgr);
    destroy_test_peer(peer);
    return;
  }

  relay_destroy(mgr);
  destroy_test_peer(peer);

}

static void test_relay_handle_block(void) {
  printf("test_relay_handle_block\n");

  test_ctx_t tctx = {0};
  tctx.accept_blocks = ECHO_TRUE;

  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);
  peer_t *peer1 = create_test_peer("192.168.1.1", 8333);
  peer_t *peer2 = create_test_peer("192.168.1.2", 8333);
  relay_add_peer(mgr, peer1);
  relay_add_peer(mgr, peer2);

  /* Create and process block */
  block_t block;
  memset(&block, 0, sizeof(block_t));
  block.header.version = 1;

  echo_result_t result = relay_handle_block(mgr, peer1, &block);
  if (result != ECHO_SUCCESS) {
    test_fail_int("relay_handle_block returned", (long)ECHO_SUCCESS, (long)result);
    relay_destroy(mgr);
    destroy_test_peer(peer1);
    destroy_test_peer(peer2);
    return;
  }

  /* Verify block was processed */
  if (tctx.blocks_processed != 1) {
    test_fail_int("expected blocks processed", 1, tctx.blocks_processed);
    relay_destroy(mgr);
    destroy_test_peer(peer1);
    destroy_test_peer(peer2);
    return;
  }

  relay_destroy(mgr);
  destroy_test_peer(peer1);
  destroy_test_peer(peer2);

}

static void test_relay_handle_tx(void) {
  printf("test_relay_handle_tx\n");

  test_ctx_t tctx = {0};
  tctx.accept_txs = ECHO_TRUE;

  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);
  peer_t *peer1 = create_test_peer("192.168.1.1", 8333);
  peer_t *peer2 = create_test_peer("192.168.1.2", 8333);
  relay_add_peer(mgr, peer1);
  relay_add_peer(mgr, peer2);

  /* Create and process transaction */
  tx_t tx;
  memset(&tx, 0, sizeof(tx_t));
  tx.version = 1;

  echo_result_t result = relay_handle_tx(mgr, peer1, &tx);
  if (result != ECHO_SUCCESS) {
    test_fail_int("relay_handle_tx returned", (long)ECHO_SUCCESS, (long)result);
    relay_destroy(mgr);
    destroy_test_peer(peer1);
    destroy_test_peer(peer2);
    return;
  }

  /* Verify tx was processed */
  if (tctx.txs_processed != 1) {
    test_fail_int("expected tx processed", 1L, (long)tctx.blocks_processed);
    relay_destroy(mgr);
    destroy_test_peer(peer1);
    destroy_test_peer(peer2);
    return;
  }

  relay_destroy(mgr);
  destroy_test_peer(peer1);
  destroy_test_peer(peer2);

}

static void test_relay_announce_block(void) {
  printf("test_relay_announce_block\n");

  test_ctx_t tctx = {0};
  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);
  peer_t *peer1 = create_test_peer("192.168.1.1", 8333);
  peer_t *peer2 = create_test_peer("192.168.1.2", 8333);
  relay_add_peer(mgr, peer1);
  relay_add_peer(mgr, peer2);

  /* Announce block */
  hash256_t block_hash;
  memset(&block_hash, 0xCC, sizeof(hash256_t));
  relay_announce_block(mgr, &block_hash);

  /* Both peers should have received inv (via send queue) */

  relay_destroy(mgr);
  destroy_test_peer(peer1);
  destroy_test_peer(peer2);

}

static void test_relay_announce_tx(void) {
  printf("test_relay_announce_tx\n");

  test_ctx_t tctx = {0};
  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);
  peer_t *peer1 = create_test_peer("192.168.1.1", 8333);
  peer_t *peer2 = create_test_peer("192.168.1.2", 8333);
  peer1->relay = ECHO_TRUE;
  peer2->relay = ECHO_FALSE; /* Should not receive tx announcements */
  relay_add_peer(mgr, peer1);
  relay_add_peer(mgr, peer2);

  /* Announce transaction */
  hash256_t tx_hash;
  memset(&tx_hash, 0xDD, sizeof(hash256_t));
  relay_announce_tx(mgr, &tx_hash);

  /* Only peer1 should receive inv */

  relay_destroy(mgr);
  destroy_test_peer(peer1);
  destroy_test_peer(peer2);

}

static void test_relay_ban_score(void) {
  printf("test_relay_ban_score\n");

  test_ctx_t tctx = {0};
  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);
  peer_t *peer = create_test_peer("192.168.1.1", 8333);
  relay_add_peer(mgr, peer);

  /* Increase ban score below threshold */
  echo_bool_t should_ban = relay_increase_ban_score(
      mgr, peer, 50, BAN_REASON_PROTOCOL_VIOLATION);
  if (should_ban) {
    test_fail("should not ban yet");
    relay_destroy(mgr);
    destroy_test_peer(peer);
    return;
  }

  /* Increase to threshold */
  should_ban = relay_increase_ban_score(mgr, peer, 50, BAN_REASON_MISBEHAVING);
  if (!should_ban) {
    test_fail("should ban now");
    relay_destroy(mgr);
    destroy_test_peer(peer);
    return;
  }

  /* Check if address is banned */
  if (!relay_is_banned(mgr, "192.168.1.1")) {
    test_fail("address should be banned");
    relay_destroy(mgr);
    destroy_test_peer(peer);
    return;
  }

  relay_destroy(mgr);
  destroy_test_peer(peer);

}

static void test_relay_ban_address(void) {
  printf("test_relay_ban_address\n");

  test_ctx_t tctx = {0};
  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);

  /* Ban address */
  relay_ban_address(mgr, "192.168.1.100", 1000, BAN_REASON_MANUAL);

  /* Check if banned */
  if (!relay_is_banned(mgr, "192.168.1.100")) {
    test_fail("address should be banned");
    relay_destroy(mgr);
    return;
  }

  /* Unban */
  relay_unban_address(mgr, "192.168.1.100");

  /* Check if unbanned */
  if (relay_is_banned(mgr, "192.168.1.100")) {
    test_fail("address should be unbanned");
    relay_destroy(mgr);
    return;
  }

  relay_destroy(mgr);

}

static void test_relay_cleanup(void) {
  printf("test_relay_cleanup\n");

  test_ctx_t tctx = {0};
  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);

  /* Ban address with short duration (1ms) */
  relay_ban_address(mgr, "192.168.1.200", 1, BAN_REASON_MANUAL);

  /* Wait for ban to expire */
  plat_sleep_ms(10);

  /* Cleanup should remove expired ban */
  relay_cleanup(mgr);

  /* Check if unbanned */
  if (relay_is_banned(mgr, "192.168.1.200")) {
    test_fail("expired ban should be removed");
    relay_destroy(mgr);
    return;
  }

  relay_destroy(mgr);

}

static void test_relay_invalid_block(void) {
  printf("test_relay_invalid_block\n");

  test_ctx_t tctx = {0};
  tctx.accept_blocks = ECHO_FALSE; /* Reject blocks */

  relay_callbacks_t callbacks = {
      .get_block = mock_get_block,
      .get_tx = mock_get_tx,
      .process_block = mock_process_block,
      .process_tx = mock_process_tx,
      .ctx = &tctx,
  };

  relay_manager_t *mgr = relay_init(&callbacks);
  peer_t *peer = create_test_peer("192.168.1.1", 8333);
  relay_add_peer(mgr, peer);

  /* Send invalid block */
  block_t block;
  memset(&block, 0, sizeof(block_t));

  echo_result_t result = relay_handle_block(mgr, peer, &block);
  if (result != ECHO_ERR_INVALID) {
    test_fail_int("relay_handle_block should return", (long)ECHO_ERR_INVALID, (long)result);
    relay_destroy(mgr);
    destroy_test_peer(peer);
    return;
  }

  /* Peer's ban score should be increased */

  relay_destroy(mgr);
  destroy_test_peer(peer);

}

static void test_relay_ban_reason_string(void) {
  printf("test_relay_ban_reason_string\n");

  const char *str = relay_ban_reason_string(BAN_REASON_EXCESSIVE_INV);
  if (strcmp(str, "EXCESSIVE_INV") != 0) {
    test_fail_str("expected", "EXCESSIVE_INV", str);
    return;
  }


}

/* ========== Test Runner ========== */

int main(void) {
    test_suite_begin("Block Relay Tests");

    test_case("Relay init"); test_relay_init(); test_pass();
    test_case("Relay add remove peer"); test_relay_add_remove_peer(); test_pass();
    test_case("Relay handle inv"); test_relay_handle_inv(); test_pass();
    test_case("Relay inv rate limit"); test_relay_inv_rate_limit(); test_pass();
    test_case("Relay handle getdata"); test_relay_handle_getdata(); test_pass();
    test_case("Relay getdata rate limit"); test_relay_getdata_rate_limit(); test_pass();
    test_case("Relay handle block"); test_relay_handle_block(); test_pass();
    test_case("Relay handle tx"); test_relay_handle_tx(); test_pass();
    test_case("Relay announce block"); test_relay_announce_block(); test_pass();
    test_case("Relay announce tx"); test_relay_announce_tx(); test_pass();
    test_case("Relay ban score"); test_relay_ban_score(); test_pass();
    test_case("Relay ban address"); test_relay_ban_address(); test_pass();
    test_case("Relay cleanup"); test_relay_cleanup(); test_pass();
    test_case("Relay invalid block"); test_relay_invalid_block(); test_pass();
    test_case("Relay ban reason string"); test_relay_ban_reason_string(); test_pass();

    test_suite_end();
    return test_global_summary();
}
