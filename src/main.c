/*
 * Bitcoin Echo — Main Entry Point
 *
 * Implements the main event loop for the Bitcoin Echo node:
 * 1. Initialize node (databases, consensus, network)
 * 2. Enter main processing loop:
 *    - Process peer connections and messages
 *    - Process received blocks
 *    - Perform periodic maintenance
 * 3. Shut down gracefully on signal
 *
 * Session 9.2: Main event loop implementation.
 *
 * Build once. Build right. Stop.
 */

#include "echo_assert.h"
#include "echo_config.h"
#include "echo_types.h"
#include "node.h"
#include "platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Compile-time verification of critical type sizes.
 * These must hold for Bitcoin protocol correctness.
 */
ECHO_STATIC_ASSERT(sizeof(hash256_t) == 32, "hash256_t must be 32 bytes");
ECHO_STATIC_ASSERT(sizeof(hash160_t) == 20, "hash160_t must be 20 bytes");
ECHO_STATIC_ASSERT(sizeof(satoshi_t) == 8, "satoshi_t must be 8 bytes");

/* Global node pointer for signal handler */
static node_t *g_node = NULL;

/**
 * Signal handler for graceful shutdown.
 *
 * Handles SIGINT (Ctrl+C) and SIGTERM for clean node shutdown.
 *
 * NOTE: This function will be used in future sessions when full node
 * startup is implemented. Marked as unused for Session 9.2.
 */
__attribute__((unused)) static void signal_handler(int sig) {
  (void)sig; /* Unused parameter */

  if (g_node != NULL) {
    node_request_shutdown(g_node);
  }
}

/**
 * Main event loop.
 *
 * Processes peer messages, blocks, and performs maintenance until
 * shutdown is requested.
 *
 * NOTE: This function will be used in future sessions when full node
 * startup is implemented. Marked as unused for Session 9.2.
 */
__attribute__((unused)) static void run_event_loop(node_t *node) {
  uint64_t last_maintenance = plat_time_ms();
  const uint64_t maintenance_interval = 1000; /* 1 second */

  printf("Event loop started. Press Ctrl+C to stop.\n");

  while (!node_shutdown_requested(node)) {
    /* Process peer connections and messages */
    node_process_peers(node);

    /* Process received blocks (validation and chain updates) */
    node_process_blocks(node);

    /* Perform periodic maintenance (ping, timeout handling, etc.) */
    uint64_t now = plat_time_ms();
    if (now - last_maintenance >= maintenance_interval) {
      node_maintenance(node);
      last_maintenance = now;
    }

    /* Small sleep to avoid busy-waiting */
    plat_sleep_ms(10);
  }

  printf("Shutdown requested. Stopping node...\n");
}

int main(int argc, char *argv[]) {
  (void)argc; /* Unused for Session 9.2 */
  (void)argv; /* Unused for Session 9.2 */

  printf("Bitcoin Echo v%s (%s)\n", ECHO_VERSION_STRING, ECHO_NETWORK_NAME);
  printf("A complete Bitcoin protocol implementation in pure C.\n\n");

  /* For Session 9.2, demonstrate event loop structure without full node startup.
   * Full node initialization will be integrated in Session 9.3 (RPC) and beyond.
   *
   * The event loop functions are now implemented and can be tested:
   * - node_process_peers() - peer message handling
   * - node_process_blocks() - block validation and chain updates
   * - node_maintenance() - periodic tasks (ping, timeouts, etc.)
   *
   * A complete example of running the node would look like:
   *
   *   node_config_t config;
   *   node_config_init(&config, "/path/to/data");
   *   node_t *node = node_create(&config);
   *   if (node == NULL) {
   *     fprintf(stderr, "Failed to create node\n");
   *     return 1;
   *   }
   *
   *   g_node = node;
   *   signal(SIGINT, signal_handler);
   *   signal(SIGTERM, signal_handler);
   *
   *   if (node_start(node) != ECHO_OK) {
   *     fprintf(stderr, "Failed to start node\n");
   *     node_destroy(node);
   *     return 1;
   *   }
   *
   *   run_event_loop(node);
   *
   *   node_stop(node);
   *   node_destroy(node);
   */

  printf("Event loop implementation complete.\n");
  printf("Run unit tests to verify event loop functions.\n");

  return ECHO_OK;
}
