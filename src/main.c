/*
 * Bitcoin Echo — Main Entry Point
 *
 * Implements the main event loop for the Bitcoin Echo node:
 * 1. Parse command-line arguments
 * 2. Initialize node (databases, consensus, network)
 * 3. Start RPC server
 * 4. Enter main processing loop:
 *    - Process peer connections and messages
 *    - Process RPC requests
 *    - Process received blocks (full node mode)
 *    - Perform periodic maintenance
 * 5. Shut down gracefully on signal
 *
 * Build once. Build right. Stop.
 */

#include "echo_assert.h"
#include "echo_config.h"
#include "echo_types.h"
#include "log.h"
#include "node.h"
#include "platform.h"
#include "rpc.h"
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

/*
 * Compile-time verification of critical type sizes.
 * These must hold for Bitcoin protocol correctness.
 */
ECHO_STATIC_ASSERT(sizeof(hash256_t) == 32, "hash256_t must be 32 bytes");
ECHO_STATIC_ASSERT(sizeof(hash160_t) == 20, "hash160_t must be 20 bytes");
ECHO_STATIC_ASSERT(sizeof(satoshi_t) == 8, "satoshi_t must be 8 bytes");

/* Global node pointer for signal handler */
static node_t *g_node = NULL;

/*
 * ============================================================================
 * COMMAND-LINE ARGUMENT PARSING
 * ============================================================================
 */

/**
 * Print usage information and exit.
 */
static void print_usage(const char *program_name) {
  printf("Bitcoin Echo v%s - A complete Bitcoin protocol implementation in "
         "pure C\n\n",
         ECHO_VERSION_STRING);
  printf("Usage: %s [options]\n\n", program_name);
  printf("Options:\n");
  printf("  --help              Show this help message\n");
  printf("  --datadir=<path>    Data directory (default: ~/.bitcoin-echo)\n");
  printf("  --observe           Observer mode (connect without validation)\n");
  printf("  --prune=<MB>        Prune old blocks to keep disk usage under <MB>\n");
  printf("                      (minimum: %d MB for reorg safety, 0=no pruning)\n",
         PRUNE_TARGET_MIN_MB);
  printf("  --testnet           Use testnet3 network\n");
  printf("  --regtest           Use regression test network\n");
  printf(
      "  --port=<port>       P2P listening port (default: network-specific)\n");
  printf(
      "  --rpcport=<port>    RPC listening port (default: network-specific)\n");
  printf("  --loglevel=<level>  Log verbosity: error, warn, info, debug\n");
  printf("                      (default: info)\n");
  printf("\n");
  printf("Network: %s (compile-time)\n", ECHO_NETWORK_NAME);
  printf("\n");
  printf("Examples:\n");
  printf("  %s --observe --datadir=/tmp/echo-test\n", program_name);
  printf("  %s --datadir=~/.bitcoin-echo --rpcport=8332\n", program_name);
  printf("\n");
}

/**
 * Parse command-line arguments into node configuration.
 *
 * Returns:
 *   0 on success
 *   1 if help was requested (caller should exit)
 *   -1 on error
 */
static int parse_arguments(int argc, char *argv[], node_config_t *config) {
  /* Default data directory */
  const char *default_datadir = getenv("HOME");
  char datadir[512];
  if (default_datadir != NULL) {
    snprintf(datadir, sizeof(datadir), "%s/.bitcoin-echo", default_datadir);
  } else {
    snprintf(datadir, sizeof(datadir), ".bitcoin-echo");
  }

  /* Initialize configuration with defaults */
  node_config_init(config, datadir);

  /* Parse arguments */
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];

    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(argv[0]);
      return 1; /* Help requested */
    } else if (strncmp(arg, "--datadir=", 10) == 0) {
      const char *path = arg + 10;
      if (path[0] == '\0') {
        fprintf(stderr, "Error: --datadir requires a path\n");
        return -1;
      }
      /* Expand ~ to home directory */
      if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home != NULL) {
          snprintf(config->data_dir, sizeof(config->data_dir), "%s%s", home,
                   path + 1);
        } else {
          fprintf(stderr, "Error: Cannot expand ~ (HOME not set)\n");
          return -1;
        }
      } else {
        size_t len = strlen(path);
        if (len >= sizeof(config->data_dir)) {
          fprintf(stderr, "Error: datadir path too long\n");
          return -1;
        }
        memcpy(config->data_dir, path, len + 1);
      }
    } else if (strcmp(arg, "--observe") == 0) {
      config->observer_mode = true;
    } else if (strncmp(arg, "--prune=", 8) == 0) {
      const char *prune_str = arg + 8;
      if (prune_str[0] == '-') {
        fprintf(stderr, "Error: Invalid prune value: %s\n", prune_str);
        return -1;
      }
      char *endptr = NULL;
      errno = 0;
      unsigned long prune_mb = strtoul(prune_str, &endptr, 10);
      // Check if conversion was valid
      if (endptr == prune_str || *endptr != '\0') {
        fprintf(stderr, "Error: Invalid prune value: %s\n", prune_str);
        return -1;
      }
      // Check for overflow or underflow
      if (errno == ERANGE || prune_mb >= ULONG_MAX) {
        fprintf(stderr, "Error: Prune value out of range: %s\n", prune_str);
        return -1;
      }
      if (prune_mb < PRUNE_TARGET_MIN_MB) {
        fprintf(stderr, "Error: Prune target must be at least %d MB\n",
                PRUNE_TARGET_MIN_MB);
        return -1;
      }
      config->prune_target_mb = (uint64_t)prune_mb;
    } else if (strcmp(arg, "--testnet") == 0) {
#if !defined(ECHO_NETWORK_TESTNET)
      fprintf(stderr,
              "Error: --testnet specified but not compiled for testnet\n");
      fprintf(stderr, "Recompile with -DECHO_NETWORK_TESTNET\n");
      return -1;
#endif
    } else if (strcmp(arg, "--regtest") == 0) {
#if !defined(ECHO_NETWORK_REGTEST)
      fprintf(stderr,
              "Error: --regtest specified but not compiled for regtest\n");
      fprintf(stderr, "Recompile with -DECHO_NETWORK_REGTEST\n");
      return -1;
#endif
    } else if (strncmp(arg, "--port=", 7) == 0) {
      const char *port_str = arg + 7;
      int port = atoi(port_str);
      if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: Invalid port number: %s\n", port_str);
        return -1;
      }
      config->port = (uint16_t)port;
    } else if (strncmp(arg, "--rpcport=", 10) == 0) {
      const char *port_str = arg + 10;
      int port = atoi(port_str);
      if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: Invalid RPC port number: %s\n", port_str);
        return -1;
      }
      config->rpc_port = (uint16_t)port;
    } else if (strncmp(arg, "--loglevel=", 11) == 0) {
      const char *level = arg + 11;
      if (strcmp(level, "error") == 0) {
        config->log_level = LOG_LEVEL_ERROR;
      } else if (strcmp(level, "warn") == 0) {
        config->log_level = LOG_LEVEL_WARN;
      } else if (strcmp(level, "info") == 0) {
        config->log_level = LOG_LEVEL_INFO;
      } else if (strcmp(level, "debug") == 0) {
        config->log_level = LOG_LEVEL_DEBUG;
      } else {
        fprintf(stderr, "Error: Unknown log level '%s'\n", level);
        fprintf(stderr, "Valid levels: error, warn, info, debug\n");
        return -1;
      }
    } else {
      fprintf(stderr, "Error: Unknown option: %s\n", arg);
      fprintf(stderr, "Use --help for usage information\n");
      return -1;
    }
  }

  return 0; /* Success */
}

/*
 * ============================================================================
 * SIGNAL HANDLING
 * ============================================================================
 */

/**
 * Signal handler for graceful shutdown.
 *
 * Handles SIGINT (Ctrl+C) and SIGTERM for clean node shutdown.
 */
static void signal_handler(int sig) {
  (void)sig; /* Unused parameter */

  if (g_node != NULL) {
    node_request_shutdown(g_node);
  }
}

/*
 * ============================================================================
 * MAIN EVENT LOOP
 * ============================================================================
 */

/**
 * Main event loop.
 *
 * Processes peer messages, RPC requests, blocks, and performs maintenance
 * until shutdown is requested.
 */
static void run_event_loop(node_t *node, rpc_server_t *rpc) {
  uint64_t last_maintenance = plat_time_ms();
  const uint64_t maintenance_interval = 1000; /* 1 second */

  log_info(LOG_COMP_MAIN, "Event loop started. Press Ctrl+C to stop.");

  while (!node_shutdown_requested(node)) {
    /* Process peer connections and messages */
    node_process_peers(node);

    /* Process RPC requests */
    if (rpc != NULL) {
      rpc_server_process(rpc);
    }

    /* Process received blocks (full node mode only) */
    if (!node_is_observer(node)) {
      node_process_blocks(node);
    }

    /* Perform periodic maintenance (ping, timeout handling, etc.) */
    uint64_t now = plat_time_ms();
    if (now - last_maintenance >= maintenance_interval) {
      node_maintenance(node);
      last_maintenance = now;
    }

    /* Small sleep to avoid busy-waiting */
    plat_sleep_ms(10);
  }

  log_info(LOG_COMP_MAIN, "Shutdown requested. Stopping node...");
}

/*
 * ============================================================================
 * MAIN
 * ============================================================================
 */

int main(int argc, char *argv[]) {
  int exit_code = 0;

  /* Parse command-line arguments */
  node_config_t config;
  int parse_result = parse_arguments(argc, argv, &config);
  if (parse_result != 0) {
    return (parse_result > 0) ? 0 : 1; /* 0 for help, 1 for error */
  }

  /* Print banner */
  printf("Bitcoin Echo v%s (%s)\n", ECHO_VERSION_STRING, ECHO_NETWORK_NAME);
  printf("A complete Bitcoin protocol implementation in pure C.\n\n");

  if (config.observer_mode) {
    printf("Mode: Observer (watch-only, no validation)\n");
  } else if (config.prune_target_mb > 0) {
    printf("Mode: Full validating node (pruned, target %llu MB)\n",
           (unsigned long long)config.prune_target_mb);
  } else {
    printf("Mode: Full validating node (archival)\n");
  }
  printf("Data directory: %s\n", config.data_dir);
  printf("P2P port: %u\n", config.port);
  printf("RPC port: %u\n", config.rpc_port);
  printf("Log level: %s\n\n", log_level_string(config.log_level));

  /* Initialize logging */
  log_init();
  log_set_level(config.log_level);

  /* Create node */
  log_info(LOG_COMP_MAIN, "Creating node...");
  node_t *node = node_create(&config);
  if (node == NULL) {
    log_error(LOG_COMP_MAIN, "Failed to create node");
    log_shutdown();
    return 1;
  }

  /* Set global node pointer for signal handler */
  g_node = node;

  /* Register signal handlers */
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  /*
   * Ignore SIGPIPE - this signal is sent when we write to a socket
   * that has been closed by the remote peer. Without this, the default
   * action is to terminate the process immediately with no error message.
   * By ignoring it, send() returns -1 with errno=EPIPE instead, which
   * we handle as a normal network error.
   */
  signal(SIGPIPE, SIG_IGN);

  /* Create RPC server */
  log_info(LOG_COMP_MAIN, "Starting RPC server on port %u...", config.rpc_port);
  rpc_config_t rpc_config;
  rpc_config_init(&rpc_config);
  rpc_config.port = config.rpc_port;
  rpc_config.bind_addr = "127.0.0.1";

  rpc_server_t *rpc = rpc_server_create(&rpc_config, node);
  if (rpc == NULL) {
    log_error(LOG_COMP_MAIN, "Failed to create RPC server");
    node_destroy(node);
    log_shutdown();
    return 1;
  }

  if (rpc_server_start(rpc) != ECHO_OK) {
    log_error(LOG_COMP_MAIN, "Failed to start RPC server");
    rpc_server_destroy(rpc);
    node_destroy(node);
    log_shutdown();
    return 1;
  }

  log_info(LOG_COMP_MAIN, "RPC server listening on http://127.0.0.1:%u",
           config.rpc_port);

  /* Start node */
  log_info(LOG_COMP_MAIN, "Starting node...");
  if (node_start(node) != ECHO_OK) {
    log_error(LOG_COMP_MAIN, "Failed to start node");
    rpc_server_stop(rpc);
    rpc_server_destroy(rpc);
    node_destroy(node);
    log_shutdown();
    return 1;
  }

  log_info(LOG_COMP_MAIN, "Node started successfully");

  if (config.observer_mode) {
    log_info(LOG_COMP_MAIN,
             "Observer mode active - connecting to Bitcoin network...");
  }

  /* Run main event loop */
  run_event_loop(node, rpc);

  /* Cleanup */
  log_info(LOG_COMP_MAIN, "Shutting down...");

  rpc_server_stop(rpc);
  rpc_server_destroy(rpc);

  node_stop(node);
  node_destroy(node);

  log_info(LOG_COMP_MAIN, "Shutdown complete");
  log_shutdown();

  return exit_code;
}
