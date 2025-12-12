/**
 * Bitcoin Echo — Node Lifecycle Implementation
 *
 * This module implements the node initialization and shutdown sequences
 * as specified in Session 9.1.
 *
 * Initialization sequence:
 *   1. Platform layer init (via plat_* functions)
 *   2. Create data directory structure
 *   3. Open databases (UTXO, block index)
 *   4. Initialize block storage
 *   5. Create and restore consensus engine
 *   6. Initialize mempool
 *   7. Initialize peer discovery
 *
 * Shutdown sequence (reverse order):
 *   1. Stop network (disconnect peers)
 *   2. Flush and close databases
 *   3. Free all allocated resources
 *
 * Build once. Build right. Stop.
 */

#include "node.h"
#include "block.h"
#include "block_index_db.h"
#include "blocks_storage.h"
#include "chainstate.h"
#include "consensus.h"
#include "discovery.h"
#include "echo_config.h"
#include "echo_types.h"
#include "mempool.h"
#include "peer.h"
#include "platform.h"
#include "sync.h"
#include "utxo_db.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ============================================================================
 * NODE INTERNAL STRUCTURE
 * ============================================================================
 */

/**
 * Internal node structure.
 *
 * Contains all state for a running node.
 */
struct node {
  /* Configuration */
  node_config_t config;

  /* State */
  node_state_t state;
  volatile bool shutdown_requested; /* Signal-safe shutdown flag */
  uint64_t start_time;              /* When node started (plat_time_ms) */

  /* Storage layer */
  utxo_db_t utxo_db;
  block_index_db_t block_index_db;
  block_file_manager_t block_storage;
  bool utxo_db_open;
  bool block_index_db_open;
  bool block_storage_init;

  /* Consensus engine */
  consensus_engine_t *consensus;

  /* Mempool */
  mempool_t *mempool;

  /* Sync manager */
  sync_manager_t *sync_mgr;

  /* Peer discovery and management */
  peer_addr_manager_t addr_manager;
  peer_t peers[NODE_MAX_PEERS];
  size_t peer_count;

  /* Listening socket */
  plat_socket_t *listen_socket;
  bool is_listening;
};

/*
 * ============================================================================
 * FORWARD DECLARATIONS
 * ============================================================================
 */

static echo_result_t node_init_directories(node_t *node);
static echo_result_t node_init_databases(node_t *node);
static echo_result_t node_init_consensus(node_t *node);
static echo_result_t node_init_mempool(node_t *node);
static echo_result_t node_init_discovery(node_t *node);
static void node_cleanup(node_t *node);

/*
 * ============================================================================
 * CONFIGURATION
 * ============================================================================
 */

void node_config_init(node_config_t *config, const char *data_dir) {
  if (config == NULL) {
    return;
  }

  memset(config, 0, sizeof(*config));

  /* Copy data directory path */
  if (data_dir != NULL && data_dir[0] != '\0') {
    size_t len = strlen(data_dir);
    if (len >= sizeof(config->data_dir)) {
      len = sizeof(config->data_dir) - 1;
    }
    memcpy(config->data_dir, data_dir, len);
    config->data_dir[len] = '\0';
  }

  /* Set default ports based on network */
  config->port = ECHO_DEFAULT_PORT;
  config->rpc_port = ECHO_DEFAULT_RPC_PORT;
}

/*
 * ============================================================================
 * NODE CREATION
 * ============================================================================
 */

node_t *node_create(const node_config_t *config) {
  if (config == NULL) {
    return NULL;
  }

  if (config->data_dir[0] == '\0') {
    return NULL;
  }

  /* Allocate node structure */
  node_t *node = calloc(1, sizeof(node_t));
  if (node == NULL) {
    return NULL;
  }

  /* Copy configuration */
  memcpy(&node->config, config, sizeof(node_config_t));
  node->state = NODE_STATE_INITIALIZING;
  node->shutdown_requested = false;

  /* Initialize all peers to disconnected state */
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    peer_init(&node->peers[i]);
  }
  node->peer_count = 0;

  /* Step 1: Create data directory structure */
  echo_result_t result = node_init_directories(node);
  if (result != ECHO_OK) {
    node_cleanup(node);
    free(node);
    return NULL;
  }

  /* Step 2: Open databases */
  result = node_init_databases(node);
  if (result != ECHO_OK) {
    node_cleanup(node);
    free(node);
    return NULL;
  }

  /* Step 3: Initialize consensus engine */
  result = node_init_consensus(node);
  if (result != ECHO_OK) {
    node_cleanup(node);
    free(node);
    return NULL;
  }

  /* Step 4: Initialize mempool */
  result = node_init_mempool(node);
  if (result != ECHO_OK) {
    node_cleanup(node);
    free(node);
    return NULL;
  }

  /* Step 5: Initialize peer discovery */
  result = node_init_discovery(node);
  if (result != ECHO_OK) {
    node_cleanup(node);
    free(node);
    return NULL;
  }

  /* Node created successfully but not yet started */
  node->state = NODE_STATE_STOPPED;
  return node;
}

/*
 * ============================================================================
 * INITIALIZATION HELPERS
 * ============================================================================
 */

/**
 * Create data directory structure.
 *
 * Creates:
 *   data_dir/
 *   data_dir/blocks/
 *   data_dir/chainstate/
 */
static echo_result_t node_init_directories(node_t *node) {
  char path[600];
  int ret;

  /* Create main data directory */
  ret = plat_dir_create(node->config.data_dir);
  if (ret != PLAT_OK) {
    return ECHO_ERR_IO;
  }

  /* Create blocks directory */
  ret =
      snprintf(path, sizeof(path), "%s/%s", node->config.data_dir, "blocks");
  if (ret < 0 || (size_t)ret >= sizeof(path)) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }
  ret = plat_dir_create(path);
  if (ret != PLAT_OK) {
    return ECHO_ERR_IO;
  }

  /* Create chainstate directory */
  ret = snprintf(path, sizeof(path), "%s/%s", node->config.data_dir,
                 "chainstate");
  if (ret < 0 || (size_t)ret >= sizeof(path)) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }
  ret = plat_dir_create(path);
  if (ret != PLAT_OK) {
    return ECHO_ERR_IO;
  }

  return ECHO_OK;
}

/**
 * Open or create databases.
 */
static echo_result_t node_init_databases(node_t *node) {
  char path[600];
  echo_result_t result;
  int ret;

  /* Open UTXO database */
  ret = snprintf(path, sizeof(path), "%s/chainstate/utxo.db",
                 node->config.data_dir);
  if (ret < 0 || (size_t)ret >= sizeof(path)) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }

  result = utxo_db_open(&node->utxo_db, path);
  if (result != ECHO_OK) {
    return result;
  }
  node->utxo_db_open = true;

  /* Open block index database */
  ret = snprintf(path, sizeof(path), "%s/chainstate/blocks.db",
                 node->config.data_dir);
  if (ret < 0 || (size_t)ret >= sizeof(path)) {
    return ECHO_ERR_BUFFER_TOO_SMALL;
  }

  result = block_index_db_open(&node->block_index_db, path);
  if (result != ECHO_OK) {
    return result;
  }
  node->block_index_db_open = true;

  /* Initialize block file storage */
  result = block_storage_init(&node->block_storage, node->config.data_dir);
  if (result != ECHO_OK) {
    return result;
  }
  node->block_storage_init = true;

  return ECHO_OK;
}

/**
 * Initialize consensus engine and restore chain state.
 */
static echo_result_t node_init_consensus(node_t *node) {
  /* Create consensus engine */
  node->consensus = consensus_engine_create();
  if (node->consensus == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /*
   * TODO: Restore chain state from database.
   *
   * In a full implementation, we would:
   * 1. Query block_index_db for the best chain tip
   * 2. Load block headers into the consensus engine's block index
   * 3. Verify the UTXO database matches the chain tip
   *
   * For now, the consensus engine starts at genesis.
   * Chain restoration will be implemented in later sessions.
   */

  return ECHO_OK;
}

/**
 * Initialize mempool with callbacks.
 */
static echo_result_t node_init_mempool(node_t *node) {
  /* Create mempool with default configuration */
  node->mempool = mempool_create();
  if (node->mempool == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /*
   * Mempool callbacks will be set up in the event loop (Session 9.2).
   * For now, the mempool is created but not connected to UTXO lookups.
   */

  return ECHO_OK;
}

/**
 * Initialize peer discovery.
 */
static echo_result_t node_init_discovery(node_t *node) {
  /* Determine network type from compile-time configuration */
  network_type_t network_type;

#if defined(ECHO_NETWORK_MAINNET)
  network_type = NETWORK_MAINNET;
#elif defined(ECHO_NETWORK_TESTNET)
  network_type = NETWORK_TESTNET;
#else
  network_type = NETWORK_REGTEST;
#endif

  /* Initialize address manager */
  discovery_init(&node->addr_manager, network_type);

  /* Add hardcoded seeds (DNS seeds will be queried when starting) */
  discovery_add_hardcoded_seeds(&node->addr_manager);

  return ECHO_OK;
}

/*
 * ============================================================================
 * NODE START
 * ============================================================================
 */

echo_result_t node_start(node_t *node) {
  if (node == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (node->state != NODE_STATE_STOPPED) {
    return ECHO_ERR_INVALID_STATE;
  }

  node->state = NODE_STATE_STARTING;
  node->start_time = plat_time_ms();

  /*
   * Query DNS seeds for peer addresses.
   * This happens synchronously at startup.
   */
  discovery_query_dns_seeds(&node->addr_manager);

  /*
   * TODO: The full start sequence will be implemented in Session 9.2:
   *
   * 1. Create and start listening socket on configured port
   * 2. Start connection manager thread
   * 3. Connect to outbound peers
   * 4. Create and start sync manager for initial block download
   *
   * For now, we just transition to running state.
   */

  node->state = NODE_STATE_RUNNING;
  return ECHO_OK;
}

/*
 * ============================================================================
 * NODE STOP
 * ============================================================================
 */

echo_result_t node_stop(node_t *node) {
  if (node == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (node->state != NODE_STATE_RUNNING) {
    /* Already stopped or never started */
    return ECHO_OK;
  }

  node->state = NODE_STATE_STOPPING;

  /*
   * Shutdown sequence:
   * 1. Stop accepting new connections
   * 2. Disconnect all peers
   * 3. Stop sync manager
   * 4. Databases will be closed in node_destroy()
   */

  /* Stop listening socket */
  if (node->is_listening && node->listen_socket != NULL) {
    plat_socket_close(node->listen_socket);
    plat_socket_free(node->listen_socket);
    node->listen_socket = NULL;
    node->is_listening = false;
  }

  /* Disconnect all peers */
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    if (peer_is_connected(&node->peers[i])) {
      peer_disconnect(&node->peers[i], PEER_DISCONNECT_USER, "Node shutdown");
    }
  }
  node->peer_count = 0;

  /* Destroy sync manager */
  if (node->sync_mgr != NULL) {
    sync_destroy(node->sync_mgr);
    node->sync_mgr = NULL;
  }

  node->state = NODE_STATE_STOPPED;
  return ECHO_OK;
}

/*
 * ============================================================================
 * NODE DESTROY
 * ============================================================================
 */

void node_destroy(node_t *node) {
  if (node == NULL) {
    return;
  }

  /* Stop if running */
  if (node->state == NODE_STATE_RUNNING) {
    node_stop(node);
  }

  /* Cleanup all resources */
  node_cleanup(node);

  /* Free node structure */
  free(node);
}

/**
 * Cleanup all node resources.
 */
static void node_cleanup(node_t *node) {
  /* Destroy mempool */
  if (node->mempool != NULL) {
    mempool_destroy(node->mempool);
    node->mempool = NULL;
  }

  /* Destroy consensus engine */
  if (node->consensus != NULL) {
    consensus_engine_destroy(node->consensus);
    node->consensus = NULL;
  }

  /* Close block index database */
  if (node->block_index_db_open) {
    block_index_db_close(&node->block_index_db);
    node->block_index_db_open = false;
  }

  /* Close UTXO database */
  if (node->utxo_db_open) {
    utxo_db_close(&node->utxo_db);
    node->utxo_db_open = false;
  }

  /*
   * Block storage doesn't need explicit close - it's stateless
   * (just holds paths and current file positions).
   */
  node->block_storage_init = false;

  /* Free listening socket if allocated */
  if (node->listen_socket != NULL) {
    plat_socket_free(node->listen_socket);
    node->listen_socket = NULL;
  }

  node->state = NODE_STATE_STOPPED;
}

/*
 * ============================================================================
 * NODE QUERIES
 * ============================================================================
 */

node_state_t node_get_state(const node_t *node) {
  if (node == NULL) {
    return NODE_STATE_UNINITIALIZED;
  }
  return node->state;
}

bool node_is_running(const node_t *node) {
  if (node == NULL) {
    return false;
  }
  return node->state == NODE_STATE_RUNNING;
}

bool node_is_syncing(const node_t *node) {
  if (node == NULL || node->sync_mgr == NULL) {
    return false;
  }
  return sync_is_ibd(node->sync_mgr);
}

void node_get_stats(const node_t *node, node_stats_t *stats) {
  if (node == NULL || stats == NULL) {
    return;
  }

  memset(stats, 0, sizeof(*stats));

  /* Chain state */
  if (node->consensus != NULL) {
    consensus_stats_t cs;
    consensus_get_stats(node->consensus, &cs);
    stats->chain_height = cs.height;
    stats->chain_work = cs.total_work;
    stats->utxo_count = cs.utxo_count;
    stats->block_index_count = cs.block_index_count;
  }

  /* Network state */
  stats->peer_count = node->peer_count;

  /* Count inbound vs outbound */
  size_t outbound = 0;
  size_t inbound = 0;
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    if (peer_is_connected(&node->peers[i])) {
      if (node->peers[i].inbound) {
        inbound++;
      } else {
        outbound++;
      }
    }
  }
  stats->outbound_peers = outbound;
  stats->inbound_peers = inbound;

  /* Mempool state */
  if (node->mempool != NULL) {
    stats->mempool_size = mempool_size(node->mempool);
    stats->mempool_bytes = mempool_bytes(node->mempool);
  }

  /* Sync state */
  if (node->sync_mgr != NULL) {
    sync_progress_t progress;
    sync_get_progress(node->sync_mgr, &progress);
    stats->is_syncing = (progress.mode == SYNC_MODE_HEADERS ||
                         progress.mode == SYNC_MODE_BLOCKS);
    stats->sync_progress = progress.sync_percentage;
  }

  /* Timing */
  stats->start_time = node->start_time;
  if (node->state == NODE_STATE_RUNNING) {
    stats->uptime_ms = plat_time_ms() - node->start_time;
  }
}

const char *node_state_string(node_state_t state) {
  switch (state) {
  case NODE_STATE_UNINITIALIZED:
    return "UNINITIALIZED";
  case NODE_STATE_INITIALIZING:
    return "INITIALIZING";
  case NODE_STATE_STARTING:
    return "STARTING";
  case NODE_STATE_RUNNING:
    return "RUNNING";
  case NODE_STATE_STOPPING:
    return "STOPPING";
  case NODE_STATE_STOPPED:
    return "STOPPED";
  case NODE_STATE_ERROR:
    return "ERROR";
  default:
    return "UNKNOWN";
  }
}

/*
 * ============================================================================
 * COMPONENT ACCESS
 * ============================================================================
 */

consensus_engine_t *node_get_consensus(node_t *node) {
  if (node == NULL) {
    return NULL;
  }
  return node->consensus;
}

const consensus_engine_t *node_get_consensus_const(const node_t *node) {
  if (node == NULL) {
    return NULL;
  }
  return node->consensus;
}

mempool_t *node_get_mempool(node_t *node) {
  if (node == NULL) {
    return NULL;
  }
  return node->mempool;
}

const mempool_t *node_get_mempool_const(const node_t *node) {
  if (node == NULL) {
    return NULL;
  }
  return node->mempool;
}

sync_manager_t *node_get_sync_manager(node_t *node) {
  if (node == NULL) {
    return NULL;
  }
  return node->sync_mgr;
}

peer_addr_manager_t *node_get_addr_manager(node_t *node) {
  if (node == NULL) {
    return NULL;
  }
  return &node->addr_manager;
}

block_file_manager_t *node_get_block_storage(node_t *node) {
  if (node == NULL) {
    return NULL;
  }
  return &node->block_storage;
}

utxo_db_t *node_get_utxo_db(node_t *node) {
  if (node == NULL) {
    return NULL;
  }
  return &node->utxo_db;
}

block_index_db_t *node_get_block_index_db(node_t *node) {
  if (node == NULL) {
    return NULL;
  }
  return &node->block_index_db;
}

const char *node_get_data_dir(const node_t *node) {
  if (node == NULL) {
    return NULL;
  }
  return node->config.data_dir;
}

/*
 * ============================================================================
 * PEER MANAGEMENT
 * ============================================================================
 */

size_t node_get_peer_count(const node_t *node) {
  if (node == NULL) {
    return 0;
  }
  return node->peer_count;
}

peer_t *node_get_peer(node_t *node, size_t index) {
  if (node == NULL) {
    return NULL;
  }

  /* Find the nth connected peer */
  size_t count = 0;
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    if (peer_is_connected(&node->peers[i])) {
      if (count == index) {
        return &node->peers[i];
      }
      count++;
    }
  }
  return NULL;
}

void node_disconnect_peer(node_t *node, peer_t *peer,
                          peer_disconnect_reason_t reason) {
  if (node == NULL || peer == NULL) {
    return;
  }

  if (peer_is_connected(peer)) {
    peer_disconnect(peer, reason, NULL);
    node->peer_count--;
  }
}

/*
 * ============================================================================
 * SIGNAL HANDLING
 * ============================================================================
 */

void node_request_shutdown(node_t *node) {
  if (node != NULL) {
    node->shutdown_requested = true;
  }
}

bool node_shutdown_requested(const node_t *node) {
  if (node == NULL) {
    return false;
  }
  return node->shutdown_requested;
}
