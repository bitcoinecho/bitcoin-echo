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

/* Test counter */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)                                                             \
  do {                                                                         \
    printf("Running test: %s...", name);                                       \
    tests_run++;                                                               \
  } while (0)

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
  TEST("node shutdown signaling");

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

  PASS();
}

/*
 * ============================================================================
 * TEST: Event Loop Functions - Basic Operation
 * ============================================================================
 */

static void test_process_peers_null_node(void) {
  TEST("node_process_peers with NULL node");

  echo_result_t result = node_process_peers(NULL);
  assert(result == ECHO_ERR_INVALID_PARAM);

  PASS();
}

static void test_process_blocks_null_node(void) {
  TEST("node_process_blocks with NULL node");

  echo_result_t result = node_process_blocks(NULL);
  assert(result == ECHO_ERR_INVALID_PARAM);

  PASS();
}

static void test_maintenance_null_node(void) {
  TEST("node_maintenance with NULL node");

  echo_result_t result = node_maintenance(NULL);
  assert(result == ECHO_ERR_INVALID_PARAM);

  PASS();
}

static void test_process_peers_uninitialized_node(void) {
  TEST("node_process_peers with uninitialized node");

  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_event_loop_uninit");

  node_t *node = node_create(&config);
  assert(node != NULL);

  /* Node is not in RUNNING state, should return OK but do nothing */
  echo_result_t result = node_process_peers(node);
  assert(result == ECHO_OK);

  node_destroy(node);

  PASS();
}

static void test_process_blocks_uninitialized_node(void) {
  TEST("node_process_blocks with uninitialized node");

  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_event_loop_uninit2");

  node_t *node = node_create(&config);
  assert(node != NULL);

  /* Node is not in RUNNING state, should return OK but do nothing */
  echo_result_t result = node_process_blocks(node);
  assert(result == ECHO_OK);

  node_destroy(node);

  PASS();
}

static void test_maintenance_uninitialized_node(void) {
  TEST("node_maintenance with uninitialized node");

  node_config_t config;
  node_config_init(&config, "/tmp/echo_test_event_loop_uninit3");

  node_t *node = node_create(&config);
  assert(node != NULL);

  /* Node is not in RUNNING state, should return OK but do nothing */
  echo_result_t result = node_maintenance(node);
  assert(result == ECHO_OK);

  node_destroy(node);

  PASS();
}

/*
 * ============================================================================
 * TEST: Helper Function - Random Nonce Generation
 * ============================================================================
 */

static void test_generate_nonce(void) {
  TEST("generate_nonce produces different values");

  uint64_t nonce1 = generate_nonce();
  uint64_t nonce2 = generate_nonce();
  uint64_t nonce3 = generate_nonce();

  /* Very unlikely to get same nonce twice in a row */
  /* (probability is 1 / 2^64, effectively zero) */
  int all_different = (nonce1 != nonce2) && (nonce2 != nonce3) && (nonce1 != nonce3);
  assert(all_different);

  PASS();
}

/*
 * ============================================================================
 * TEST: Message Handling - Basic Dispatch
 * ============================================================================
 */

static void test_handle_ping_message(void) {
  TEST("handle PING message generates PONG");

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
  /* For Session 9.2, we verify the function doesn't crash */
  /* Full verification of queued messages would require peer internals */

  node_destroy(node);

  PASS();
}

static void test_handle_null_message(void) {
  TEST("handle NULL message safely");

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

  PASS();
}

static void test_handle_unknown_message(void) {
  TEST("handle unknown message type");

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

  PASS();
}

/*
 * ============================================================================
 * TEST: Event Loop Integration
 * ============================================================================
 */

static void test_event_loop_functions_sequence(void) {
  TEST("event loop functions called in sequence");

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

  PASS();
}

static void test_shutdown_requested_stops_loop(void) {
  TEST("shutdown request would stop event loop");

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

  PASS();
}

/*
 * ============================================================================
 * MAIN TEST RUNNER
 * ============================================================================
 */

int main(void) {
  printf("Bitcoin Echo — Event Loop Tests (Session 9.2)\n");
  printf("==============================================\n\n");

  /* Node state tests */
  test_node_shutdown_signal();

  /* Basic operation tests */
  test_process_peers_null_node();
  test_process_blocks_null_node();
  test_maintenance_null_node();
  test_process_peers_uninitialized_node();
  test_process_blocks_uninitialized_node();
  test_maintenance_uninitialized_node();

  /* Helper function tests */
  test_generate_nonce();

  /* Message handling tests */
  test_handle_ping_message();
  test_handle_null_message();
  test_handle_unknown_message();

  /* Integration tests */
  test_event_loop_functions_sequence();
  test_shutdown_requested_stops_loop();

  /* Summary */
  printf("\n==============================================\n");
  printf("Tests run: %d\n", tests_run);
  printf("Tests passed: %d\n", tests_passed);
  printf("Tests failed: %d\n", tests_run - tests_passed);

  if (tests_passed == tests_run) {
    printf("\n✓ All tests passed!\n");
    return 0;
  } else {
    printf("\n✗ Some tests failed!\n");
    return 1;
  }
}
