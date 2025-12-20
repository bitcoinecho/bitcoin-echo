/**
 * Bitcoin Echo — Event Loop Tests
 *
 * Tests for Session 9.2: Main Event Loop
 *
 * Verifies:
 * - node_process_peers() peer message handling
 * - node_process_blocks() block processing
 * - node_maintenance() periodic tasks
 * - Signal handling and shutdown
 *
 * Build once. Build right. Stop.
 */

#include "../../src/app/node.c" /* Include implementation for testing */
#include "mempool.h"
#include "protocol.h"
#include "sync.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "test_utils.h"

/*
 * ============================================================================
 * TEST: Node State Functions
 * ============================================================================
 */

static void test_node_shutdown_signal(void) {
  test_case("Node shutdown signal");

  /* Create node */
  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_event_loop");

  node_t *node = node_create(&config);
  if (node == NULL) {
    test_fail("Node creation failed");
    return;
  }

  /* Initially no shutdown requested */
  if (node_shutdown_requested(node)) {
    test_fail("Shutdown should not be requested initially");
    node_destroy(node);
    return;
  }

  /* Request shutdown */
  node_request_shutdown(node);
  if (!node_shutdown_requested(node)) {
    test_fail("Shutdown should be requested after calling node_request_shutdown");
    node_destroy(node);
    return;
  }

  /* Cleanup */
  node_destroy(node);

  test_pass();
}

/*
 * ============================================================================
 * TEST: Event Loop Functions - Basic Operation
 * ============================================================================
 */

static void test_process_peers_null_node(void) {
  test_case("Process peers null node");

  echo_result_t result = node_process_peers(NULL);
  if (result != ECHO_ERR_INVALID_PARAM) {
    test_fail("Expected ECHO_ERR_INVALID_PARAM");
    return;
  }

  test_pass();
}

static void test_process_blocks_null_node(void) {
  test_case("Process blocks null node");

  echo_result_t result = node_process_blocks(NULL);
  if (result != ECHO_ERR_INVALID_PARAM) {
    test_fail("Expected ECHO_ERR_INVALID_PARAM");
    return;
  }

  test_pass();
}

static void test_maintenance_null_node(void) {
  test_case("Maintenance null node");

  echo_result_t result = node_maintenance(NULL);
  if (result != ECHO_ERR_INVALID_PARAM) {
    test_fail("Expected ECHO_ERR_INVALID_PARAM");
    return;
  }

  test_pass();
}

static void test_process_peers_uninitialized_node(void) {
  test_case("Process peers uninitialized node");

  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_event_loop_uninit");

  node_t *node = node_create(&config);
  if (node == NULL) {
    test_fail("Node creation failed");
    return;
  }

  /* Node is not in RUNNING state, should return OK but do nothing */
  echo_result_t result = node_process_peers(node);
  if (result != ECHO_OK) {
    test_fail("Expected ECHO_OK");
    node_destroy(node);
    return;
  }

  node_destroy(node);

  test_pass();
}

static void test_process_blocks_uninitialized_node(void) {
  test_case("Process blocks uninitialized node");

  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_event_loop_uninit2");

  node_t *node = node_create(&config);
  if (node == NULL) {
    test_fail("Node creation failed");
    return;
  }

  /* Node is not in RUNNING state, should return OK but do nothing */
  echo_result_t result = node_process_blocks(node);
  if (result != ECHO_OK) {
    test_fail("Expected ECHO_OK");
    node_destroy(node);
    return;
  }

  node_destroy(node);

  test_pass();
}

static void test_maintenance_uninitialized_node(void) {
  test_case("Maintenance uninitialized node");

  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_event_loop_uninit3");

  node_t *node = node_create(&config);
  if (node == NULL) {
    test_fail("Node creation failed");
    return;
  }

  /* Node is not in RUNNING state, should return OK but do nothing */
  echo_result_t result = node_maintenance(node);
  if (result != ECHO_OK) {
    test_fail("Expected ECHO_OK");
    node_destroy(node);
    return;
  }

  node_destroy(node);

  test_pass();
}

/*
 * ============================================================================
 * TEST: Helper Function - Random Nonce Generation
 * ============================================================================
 */

static void test_generate_nonce(void) {
  test_case("Generate nonce");

  uint64_t nonce1 = generate_nonce();
  uint64_t nonce2 = generate_nonce();
  uint64_t nonce3 = generate_nonce();

  /* Very unlikely to get same nonce twice in a row */
  /* (probability is 1 / 2^64, effectively zero) */
  int all_different = (nonce1 != nonce2) && (nonce2 != nonce3) && (nonce1 != nonce3);
  if (!all_different) {
    test_fail("Generated nonces are not unique");
    return;
  }

  test_pass();
}

/*
 * ============================================================================
 * TEST: Message Handling - Basic Dispatch
 * ============================================================================
 */

static void test_handle_ping_message(void) {
  test_case("Handle ping message");

  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_ping");

  node_t *node = node_create(&config);
  if (node == NULL) {
    test_fail("Node creation failed");
    return;
  }

  /* Create a mock peer */
  peer_t peer;
  peer_init(&peer);
  peer.state = PEER_STATE_READY; /* Simulate ready peer */

  /* Create PING message */
  msg_t ping;
  memset(&ping, 0, sizeof(ping));
  ping.type = MSG_PING;
  ping.payload.ping.nonce = 0x1234567890ABCDEFULL;

  /* Handle the message */
  node_handle_peer_message(node, &peer, &ping);

  /* Verify PONG was queued (check peer send queue) */
  /* For Session 9.2, we verify the function doesn't crash */
  /* Full verification of queued messages would require peer internals */

  node_destroy(node);

  test_pass();
}

static void test_handle_null_message(void) {
  test_case("Handle null message");

  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_null_msg");

  node_t *node = node_create(&config);
  if (node == NULL) {
    test_fail("Node creation failed");
    return;
  }

  peer_t peer;
  peer_init(&peer);

  /* Should handle NULL gracefully */
  node_handle_peer_message(node, &peer, NULL);
  node_handle_peer_message(node, NULL, NULL);
  node_handle_peer_message(NULL, &peer, NULL);

  node_destroy(node);

  test_pass();
}

static void test_handle_unknown_message(void) {
  test_case("Handle unknown message");

  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_unknown_msg");

  node_t *node = node_create(&config);
  if (node == NULL) {
    test_fail("Node creation failed");
    return;
  }

  peer_t peer;
  peer_init(&peer);
  peer.state = PEER_STATE_READY;

  /* Create message with unknown type */
  msg_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = MSG_UNKNOWN;

  /* Should handle gracefully (default case in switch) */
  node_handle_peer_message(node, &peer, &msg);

  node_destroy(node);

  test_pass();
}

/*
 * ============================================================================
 * TEST: Event Loop Integration
 * ============================================================================
 */

static void test_event_loop_functions_sequence(void) {
  test_case("Event loop functions sequence");

  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_sequence");

  node_t *node = node_create(&config);
  if (node == NULL) {
    test_fail("Node creation failed");
    return;
  }

  /* Simulate one iteration of event loop */
  /* (Node is not running, so functions should no-op gracefully) */

  echo_result_t result;

  result = node_process_peers(node);
  if (result != ECHO_OK) {
    test_fail("node_process_peers failed");
    node_destroy(node);
    return;
  }

  result = node_process_blocks(node);
  if (result != ECHO_OK) {
    test_fail("node_process_blocks failed");
    node_destroy(node);
    return;
  }

  result = node_maintenance(node);
  if (result != ECHO_OK) {
    test_fail("node_maintenance failed");
    node_destroy(node);
    return;
  }

  node_destroy(node);

  test_pass();
}

static void test_shutdown_requested_stops_loop(void) {
  test_case("Shutdown requested stops loop");

  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_shutdown_loop");

  node_t *node = node_create(&config);
  if (node == NULL) {
    test_fail("Node creation failed");
    return;
  }

  /* Simulate event loop condition */
  int loop_count = 0;
  const int max_iterations = 10;
  const int expected_loop_count = 5;

  while (!node_shutdown_requested(node) && loop_count < max_iterations) {
    loop_count++;

    /* Simulate some iterations before shutdown */
    if (loop_count == expected_loop_count) {
      node_request_shutdown(node);
    }
  }

  /* Should have stopped at iteration 5 due to shutdown request */
  if (loop_count != expected_loop_count) {
    test_fail_int("Unexpected loop count", expected_loop_count, loop_count);
    node_destroy(node);
    return;
  }

  if (!node_shutdown_requested(node)) {
    test_fail("Shutdown was not requested");
    node_destroy(node);
    return;
  }

  node_destroy(node);

  test_pass();
}

/*
 * ============================================================================
 * MAIN TEST RUNNER
 * ============================================================================
 */

int main(void) {
    test_suite_begin("Event Loop Tests");

    test_node_shutdown_signal();
    test_process_peers_null_node();
    test_process_blocks_null_node();
    test_maintenance_null_node();
    test_process_peers_uninitialized_node();
    test_process_blocks_uninitialized_node();
    test_maintenance_uninitialized_node();
    test_generate_nonce();
    test_handle_ping_message();
    test_handle_null_message();
    test_handle_unknown_message();
    test_event_loop_functions_sequence();
    test_shutdown_requested_stops_loop();

    test_suite_end();
    return test_global_summary();
}
