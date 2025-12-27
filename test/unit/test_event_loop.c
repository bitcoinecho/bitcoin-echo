/**
 * Bitcoin Echo — Event Loop Tests
 *
 * Main Event Loop Tests
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

/* Test counter */

#define PASS()                                                                 \
  do {                                                                         \
    printf(" PASS\n");                                                         \
    tests_passed++;                                                            \
  } while (0)

/*
 * ============================================================================
 * TEST: Node State Functions
 * ============================================================================
 */

static void test_node_shutdown_signal(void) {
  /* Create node */
  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_event_loop");

  node_t *node = node_create(&config);
  assert(node != NULL);

  /* Initially no shutdown requested */
  assert(!node_shutdown_requested(node));

  /* Request shutdown */
  node_request_shutdown(node);
  assert(node_shutdown_requested(node));

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
  echo_result_t result = node_process_peers(NULL);
  assert(result == ECHO_ERR_INVALID_PARAM);

  test_pass();
}

static void test_process_blocks_null_node(void) {
  echo_result_t result = node_process_blocks(NULL);
  assert(result == ECHO_ERR_INVALID_PARAM);

  test_pass();
}

static void test_maintenance_null_node(void) {
  echo_result_t result = node_maintenance(NULL);
  assert(result == ECHO_ERR_INVALID_PARAM);

  test_pass();
}

static void test_process_peers_uninitialized_node(void) {
  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_event_loop_uninit");

  node_t *node = node_create(&config);
  assert(node != NULL);

  /* Node is not in RUNNING state, should return OK but do nothing */
  echo_result_t result = node_process_peers(node);
  assert(result == ECHO_OK);

  node_destroy(node);

  test_pass();
}

static void test_process_blocks_uninitialized_node(void) {
  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_event_loop_uninit2");

  node_t *node = node_create(&config);
  assert(node != NULL);

  /* Node is not in RUNNING state, should return OK but do nothing */
  echo_result_t result = node_process_blocks(node);
  assert(result == ECHO_OK);

  node_destroy(node);

  test_pass();
}

static void test_maintenance_uninitialized_node(void) {
  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_event_loop_uninit3");

  node_t *node = node_create(&config);
  assert(node != NULL);

  /* Node is not in RUNNING state, should return OK but do nothing */
  echo_result_t result = node_maintenance(node);
  assert(result == ECHO_OK);

  node_destroy(node);

  test_pass();
}

/*
 * ============================================================================
 * TEST: Helper Function - Random Nonce Generation
 * ============================================================================
 */

static void test_generate_nonce(void) {
  uint64_t nonce1 = generate_nonce();
  uint64_t nonce2 = generate_nonce();
  uint64_t nonce3 = generate_nonce();

  /* Very unlikely to get same nonce twice in a row */
  /* (probability is 1 / 2^64, effectively zero) */
  int all_different = (nonce1 != nonce2) && (nonce2 != nonce3) && (nonce1 != nonce3);
  assert(all_different);

  test_pass();
}

/*
 * ============================================================================
 * TEST: Message Handling - Basic Dispatch
 * ============================================================================
 */

static void test_handle_ping_message(void) {
  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_ping");

  node_t *node = node_create(&config);
  assert(node != NULL);

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
  /* We verify the function doesn't crash */
  /* Full verification of queued messages would require peer internals */

  node_destroy(node);

  test_pass();
}

static void test_handle_null_message(void) {
  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_null_msg");

  node_t *node = node_create(&config);
  assert(node != NULL);

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
  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_unknown_msg");

  node_t *node = node_create(&config);
  assert(node != NULL);

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
  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_sequence");

  node_t *node = node_create(&config);
  assert(node != NULL);

  /* Simulate one iteration of event loop */
  /* (Node is not running, so functions should no-op gracefully) */

  echo_result_t result;

  result = node_process_peers(node);
  assert(result == ECHO_OK);

  result = node_process_blocks(node);
  assert(result == ECHO_OK);

  result = node_maintenance(node);
  assert(result == ECHO_OK);

  node_destroy(node);

  test_pass();
}

static void test_shutdown_requested_stops_loop(void) {
  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_shutdown_loop");

  node_t *node = node_create(&config);
  assert(node != NULL);

  /* Simulate event loop condition */
  int loop_count = 0;
  const int max_iterations = 10;

  while (!node_shutdown_requested(node) && loop_count < max_iterations) {
    loop_count++;

    /* Simulate some iterations before shutdown */
    if (loop_count == 5) {
      node_request_shutdown(node);
    }
  }

  /* Should have stopped at iteration 5 due to shutdown request */
  assert(loop_count == 5);
  assert(node_shutdown_requested(node));

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

    test_case("Node shutdown signal"); test_node_shutdown_signal(); test_pass();
    test_case("Process peers null node"); test_process_peers_null_node(); test_pass();
    test_case("Process blocks null node"); test_process_blocks_null_node(); test_pass();
    test_case("Maintenance null node"); test_maintenance_null_node(); test_pass();
    test_case("Process peers uninitialized node"); test_process_peers_uninitialized_node(); test_pass();
    test_case("Process blocks uninitialized node"); test_process_blocks_uninitialized_node(); test_pass();
    test_case("Maintenance uninitialized node"); test_maintenance_uninitialized_node(); test_pass();
    test_case("Generate nonce"); test_generate_nonce(); test_pass();
    test_case("Handle ping message"); test_handle_ping_message(); test_pass();
    test_case("Handle null message"); test_handle_null_message(); test_pass();
    test_case("Handle unknown message"); test_handle_unknown_message(); test_pass();
    test_case("Event loop functions sequence"); test_event_loop_functions_sequence(); test_pass();
    test_case("Shutdown requested stops loop"); test_shutdown_requested_stops_loop(); test_pass();

    test_suite_end();
    return test_global_summary();
}
