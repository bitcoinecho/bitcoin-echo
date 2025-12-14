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
#include "block_index_db.h"
#include "blocks_storage.h"
#include "consensus.h"
#include "discovery.h"
#include "echo_config.h"
#include "echo_types.h"
#include "log.h"
#include "mempool.h"
#include "peer.h"
#include "platform.h"
#include "protocol.h"
#include "sync.h"
#include "tx.h"
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

  /* Storage layer (NULL if in observer mode) */
  utxo_db_t utxo_db;
  block_index_db_t block_index_db;
  block_file_manager_t block_storage;
  bool utxo_db_open;
  bool block_index_db_open;
  bool block_storage_init;

  /* Consensus engine (NULL if in observer mode) */
  consensus_engine_t *consensus;

  /* Mempool (NULL if in observer mode) */
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

  /* Observer mode statistics (Session 9.5) */
  observer_stats_t observer_stats;
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

  /* Default to full validation mode (not observer) */
  config->observer_mode = false;
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

  /* Step 1: Create data directory structure (always needed) */
  echo_result_t result = node_init_directories(node);
  if (result != ECHO_OK) {
    node_cleanup(node);
    free(node);
    return NULL;
  }

  /*
   * Observer mode: Skip consensus, storage, and mempool initialization.
   * Only peer discovery and networking are initialized.
   */
  if (!node->config.observer_mode) {
    /* Step 2: Open databases (full node only) */
    result = node_init_databases(node);
    if (result != ECHO_OK) {
      node_cleanup(node);
      free(node);
      return NULL;
    }

    /* Step 3: Initialize consensus engine (full node only) */
    result = node_init_consensus(node);
    if (result != ECHO_OK) {
      node_cleanup(node);
      free(node);
      return NULL;
    }

    /* Step 4: Initialize mempool (full node only) */
    result = node_init_mempool(node);
    if (result != ECHO_OK) {
      node_cleanup(node);
      free(node);
      return NULL;
    }
  }

  /* Step 5: Initialize peer discovery (both modes) */
  result = node_init_discovery(node);
  if (result != ECHO_OK) {
    node_cleanup(node);
    free(node);
    return NULL;
  }

  /* Initialize observer statistics if in observer mode */
  if (node->config.observer_mode) {
    memset(&node->observer_stats, 0, sizeof(observer_stats_t));
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
  ret = snprintf(path, sizeof(path), "%s/%s", node->config.data_dir, "blocks");
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
 * EVENT LOOP PROCESSING
 * ============================================================================
 */

/**
 * Generate random 64-bit nonce using platform random bytes.
 */
static uint64_t generate_nonce(void) {
  uint64_t nonce;
  plat_random_bytes((uint8_t *)&nonce, sizeof(nonce));
  return nonce;
}

/**
 * Handle a received message from a peer.
 *
 * Dispatches message to appropriate handler based on type.
 */
static void node_handle_peer_message(node_t *node, peer_t *peer,
                                     const msg_t *msg) {
  if (node == NULL || peer == NULL || msg == NULL) {
    return;
  }

  /* Observer mode: Track message types (Session 9.5) */
  if (node->config.observer_mode) {
    /* Get message command string for tracking */
    const char *command = NULL;
    switch (msg->type) {
    case MSG_VERSION: command = "version"; break;
    case MSG_VERACK: command = "verack"; break;
    case MSG_PING: command = "ping"; break;
    case MSG_PONG: command = "pong"; break;
    case MSG_ADDR: command = "addr"; break;
    case MSG_INV: command = "inv"; break;
    case MSG_GETDATA: command = "getdata"; break;
    case MSG_BLOCK: command = "block"; break;
    case MSG_TX: command = "tx"; break;
    case MSG_HEADERS: command = "headers"; break;
    case MSG_GETBLOCKS: command = "getblocks"; break;
    case MSG_GETHEADERS: command = "getheaders"; break;
    default: command = "other"; break;
    }
    if (command != NULL) {
      node_observe_message(node, command);
    }
  }

  switch (msg->type) {
  case MSG_VERSION:
    /* Version received - send verack to complete handshake */
    {
      msg_t verack;
      memset(&verack, 0, sizeof(verack));
      verack.type = MSG_VERACK;
      peer_queue_message(peer, &verack);
      log_info(LOG_COMP_NET, "Sent VERACK to peer %s", peer->address);
    }
    break;

  case MSG_VERACK:
    /* Verack handled during handshake in peer.c */
    /* When handshake complete, add peer to sync manager */
    if (peer_is_ready(peer) && node->sync_mgr != NULL) {
      sync_add_peer(node->sync_mgr, peer, peer->start_height);
    }
    break;

  case MSG_PING:
    /* Respond with pong */
    {
      msg_t pong;
      memset(&pong, 0, sizeof(pong));
      pong.type = MSG_PONG;
      pong.payload.pong.nonce = msg->payload.ping.nonce;
      peer_queue_message(peer, &pong);
    }
    break;

  case MSG_PONG:
    /* Pong received - peer is alive */
    break;

  case MSG_ADDR:
    /* Update address manager with new addresses */
    if (msg->payload.addr.count > 0 && msg->payload.addr.addresses != NULL) {
      discovery_add_addresses(&node->addr_manager,
                              msg->payload.addr.addresses,
                              msg->payload.addr.count);
    }
    break;

  case MSG_GETADDR:
    /* Send known addresses to peer */
    {
      /* Allocate buffer for addresses to send */
      #define MAX_ADDR_TO_SEND 1000
      net_addr_t addrs[MAX_ADDR_TO_SEND];
      size_t count = discovery_select_addresses_to_advertise(
          &node->addr_manager, addrs, MAX_ADDR_TO_SEND);

      if (count > 0) {
        msg_t addr_msg;
        memset(&addr_msg, 0, sizeof(addr_msg));
        addr_msg.type = MSG_ADDR;
        addr_msg.payload.addr.count = count;
        addr_msg.payload.addr.addresses = addrs;
        peer_queue_message(peer, &addr_msg);
      }
      #undef MAX_ADDR_TO_SEND
    }
    break;

  case MSG_HEADERS:
    /* Forward to sync manager */
    if (node->sync_mgr != NULL && msg->payload.headers.count > 0) {
      sync_handle_headers(node->sync_mgr, peer, msg->payload.headers.headers,
                          msg->payload.headers.count);
    }
    break;

  case MSG_BLOCK:
    /* Forward to sync manager */
    if (node->sync_mgr != NULL) {
      sync_handle_block(node->sync_mgr, peer, &msg->payload.block.block);
    }
    break;

  case MSG_TX:
    /* Forward to mempool */
    if (node->mempool != NULL) {
      mempool_accept_result_t result;
      mempool_add(node->mempool, &msg->payload.tx.tx, &result);
      /* Ignore result - transaction may already be in mempool */
    }
    break;

  case MSG_INV:
    /* Inventory announcement - request interesting items */
    if (msg->payload.inv.count > 0 && msg->payload.inv.inventory != NULL) {
      /* Observer mode: Track block and transaction announcements (Session 9.5) */
      if (node->config.observer_mode) {
        for (size_t i = 0; i < msg->payload.inv.count; i++) {
          const inv_vector_t *inv = &msg->payload.inv.inventory[i];
          if (inv->type == INV_BLOCK || inv->type == INV_WITNESS_BLOCK) {
            node_observe_block(node, &inv->hash);
          } else if (inv->type == INV_TX || inv->type == INV_WITNESS_TX) {
            node_observe_tx(node, &inv->hash);
          }
        }
      }

      /* Allocate buffer for getdata inventory vectors */
      #define MAX_GETDATA_ITEMS 1000
      inv_vector_t items[MAX_GETDATA_ITEMS];
      size_t item_count = 0;

      /* Request blocks and transactions we don't have */
      /* In observer mode, we can optionally skip requesting to reduce bandwidth */
      if (!node->config.observer_mode) {
        for (size_t i = 0; i < msg->payload.inv.count && item_count < MAX_GETDATA_ITEMS; i++) {
          const inv_vector_t *inv = &msg->payload.inv.inventory[i];

          /* For Session 9.2, request all announced items */
          /* Filtering logic (already have? want?) will be added in later sessions */
          memcpy(&items[item_count], inv, sizeof(inv_vector_t));
          item_count++;
        }

        if (item_count > 0) {
          msg_t getdata;
          memset(&getdata, 0, sizeof(getdata));
          getdata.type = MSG_GETDATA;
          getdata.payload.getdata.count = item_count;
          getdata.payload.getdata.inventory = items;
          peer_queue_message(peer, &getdata);
        }
      }
      #undef MAX_GETDATA_ITEMS
    }
    break;

  case MSG_GETDATA:
    /* Peer requesting data from us */
    if (msg->payload.getdata.count > 0 && msg->payload.getdata.inventory != NULL) {
      for (size_t i = 0; i < msg->payload.getdata.count; i++) {
        const inv_vector_t *inv = &msg->payload.getdata.inventory[i];

        if (inv->type == INV_BLOCK || inv->type == INV_WITNESS_BLOCK) {
          /* Serving blocks from storage - deferred to later sessions */
          /* For now, we focus on syncing, not serving */
        } else if (inv->type == INV_TX || inv->type == INV_WITNESS_TX) {
          /* Try to send transaction from mempool */
          if (node->mempool != NULL) {
            const mempool_entry_t *entry =
                mempool_lookup(node->mempool, &inv->hash);
            if (entry != NULL) {
              msg_t tx_msg;
              memset(&tx_msg, 0, sizeof(tx_msg));
              tx_msg.type = MSG_TX;
              memcpy(&tx_msg.payload.tx.tx, &entry->tx, sizeof(tx_t));
              peer_queue_message(peer, &tx_msg);
            }
          }
        }
      }
    }
    break;

  case MSG_NOTFOUND:
    /* Peer doesn't have requested data - noted but no action needed */
    break;

  case MSG_REJECT:
    /* Peer rejected something we sent - log for debugging */
    break;

  case MSG_SENDHEADERS:
  case MSG_SENDCMPCT:
  case MSG_FEEFILTER:
  case MSG_WTXIDRELAY:
    /* Feature negotiation messages - acknowledged but not implemented */
    break;

  case MSG_GETHEADERS:
  case MSG_GETBLOCKS:
    /* Peer requesting headers/blocks from us - not yet implemented */
    break;

  default:
    /* Unknown message type - ignore */
    break;
  }
}

echo_result_t node_process_peers(node_t *node) {
  if (node == NULL) {
    return ECHO_ERR_INVALID_PARAM;
  }

  if (node->state != NODE_STATE_RUNNING) {
    return ECHO_OK; /* Not running, nothing to process */
  }

  /* Step 1: Accept new inbound connections if listening */
  if (node->is_listening && node->listen_socket != NULL) {
    /* Find empty peer slot for inbound connection */
    for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
      peer_t *peer = &node->peers[i];
      if (!peer_is_connected(peer)) {
        /* Try to accept connection (non-blocking) */
        uint64_t nonce = generate_nonce();
        echo_result_t result = peer_accept(peer, node->listen_socket, nonce);
        if (result == ECHO_OK) {
          node->peer_count++;

          /* Send version message to start handshake */
          uint32_t our_height = 0;
          if (node->consensus != NULL) {
            our_height = consensus_get_height(node->consensus);
          }

          /* Service flags: NODE_NETWORK (1) */
          uint64_t services = 1;
          peer_send_version(peer, services, (int32_t)our_height, true);
        }
        break; /* Only accept one per loop iteration */
      }
    }
  }

  /* Step 2: Process all connected peers */
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    peer_t *peer = &node->peers[i];

    if (!peer_is_connected(peer)) {
      continue;
    }

    /* Step 2a: Receive and process messages */
    msg_t msg;
    echo_result_t result = peer_receive(peer, &msg);

    if (result == ECHO_OK) {
      /* Message received - handle it */
      node_handle_peer_message(node, peer, &msg);

      /* Free any allocated inventory vectors (from INV/GETDATA parsing) */
      if (msg.type == MSG_INV && msg.payload.inv.inventory != NULL) {
        free(msg.payload.inv.inventory);
      } else if (msg.type == MSG_GETDATA && msg.payload.getdata.inventory != NULL) {
        free(msg.payload.getdata.inventory);
      }
    } else if (result == ECHO_ERR_WOULD_BLOCK) {
      /* No message available - not an error */
    } else {
      /* Network or protocol error - disconnect peer */
      peer_disconnect_reason_t reason = (result == ECHO_ERR_PROTOCOL)
                                            ? PEER_DISCONNECT_PROTOCOL_ERROR
                                            : PEER_DISCONNECT_NETWORK_ERROR;
      node_disconnect_peer(node, peer, reason);
      continue;
    }

    /* Step 2b: Send queued messages */
    result = peer_send_queued(peer);
    if (result != ECHO_OK && result != ECHO_ERR_WOULD_BLOCK) {
      /* Send failed - disconnect peer */
      node_disconnect_peer(node, peer, PEER_DISCONNECT_NETWORK_ERROR);
      continue;
    }

    /* Step 2c: Check for timeout */
    uint64_t now = plat_time_ms();
    uint64_t timeout_threshold = 20 * 60 * 1000; /* 20 minutes */

    if (now - peer->last_recv > timeout_threshold) {
      node_disconnect_peer(node, peer, PEER_DISCONNECT_TIMEOUT);
    }
  }

  return ECHO_OK;
}

echo_result_t node_process_blocks(node_t *node) {
  if (node == NULL) {
    return ECHO_ERR_INVALID_PARAM;
  }

  if (node->state != NODE_STATE_RUNNING) {
    return ECHO_OK;
  }

  /* The sync manager handles block validation and chain updates internally
   * when sync_handle_block() is called from message processing.
   *
   * This function serves as a hook for any additional block processing
   * that needs to happen outside of direct message handling, such as:
   * - Reorganization notifications
   * - Block relay to other peers
   * - Mempool cleanup after new blocks
   *
   * For Session 9.2, the sync manager does the heavy lifting.
   * Additional logic can be added here in future sessions if needed.
   */

  return ECHO_OK;
}

echo_result_t node_maintenance(node_t *node) {
  if (node == NULL) {
    return ECHO_ERR_INVALID_PARAM;
  }

  if (node->state != NODE_STATE_RUNNING) {
    return ECHO_OK;
  }

  uint64_t now = plat_time_ms();
  static uint64_t last_log = 0;
  if (now - last_log > 5000) { /* Log every 5 seconds */
    log_info(LOG_COMP_NET, "Maintenance tick: peer_count=%zu", node->peer_count);
    last_log = now;
  }

  /* Task 1: Ping peers to keep connections alive */
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    peer_t *peer = &node->peers[i];

    if (!peer_is_ready(peer)) {
      continue;
    }

    /* Send ping every 2 minutes if we haven't sent anything recently */
    uint64_t ping_interval = 2ULL * 60 * 1000; /* 2 minutes */
    if (now - peer->last_send > ping_interval) {
      msg_t ping;
      memset(&ping, 0, sizeof(ping));
      ping.type = MSG_PING;
      ping.payload.ping.nonce = generate_nonce();
      peer_queue_message(peer, &ping);
    }
  }

  /* Task 2: Tick sync manager for timeout processing and retries */
  if (node->sync_mgr != NULL) {
    sync_tick(node->sync_mgr);
    sync_process_timeouts(node->sync_mgr);
  }

  /* Task 3: Evict stale mempool transactions (future session) */
  /* Mempool maintenance will be added when mempool_tick() is implemented */

  /* Task 4: Attempt outbound connections if below target */
  size_t outbound_count = 0;
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    if (peer_is_connected(&node->peers[i]) && !node->peers[i].inbound) {
      outbound_count++;
    }
  }

  static uint64_t last_peer_log = 0;
  if (now - last_peer_log > 5000) { /* Log every 5 seconds */
    log_info(LOG_COMP_NET, "Outbound peers: %zu/%d", outbound_count, ECHO_MAX_OUTBOUND_PEERS);
    last_peer_log = now;
  }
  if (outbound_count < ECHO_MAX_OUTBOUND_PEERS) {
    /* Try to make one new outbound connection */
    net_addr_t addr;
    echo_result_t addr_result =
        discovery_select_outbound_address(&node->addr_manager, &addr);
    if (addr_result == ECHO_OK) {
      /* Find empty slot */
      for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
        peer_t *peer = &node->peers[i];
        if (!peer_is_connected(peer)) {
          /* Convert IPv4-mapped IPv6 address to string */
          char ip_str[64];
          snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", addr.ip[12],
                   addr.ip[13], addr.ip[14], addr.ip[15]);

          log_info(LOG_COMP_NET, "Attempting outbound connection to %s:%u", ip_str, addr.port);

          /* Mark address as in-use BEFORE connecting to prevent duplicate connections */
          discovery_mark_address_in_use(&node->addr_manager, &addr);

          uint64_t nonce = generate_nonce();
          echo_result_t result = peer_connect(peer, ip_str, addr.port, nonce);
          if (result == ECHO_OK) {
            node->peer_count++;
            log_info(LOG_COMP_NET, "Connected to peer %s:%u", ip_str, addr.port);

            /* Send version message to start handshake */
            uint32_t our_height = 0;
            if (node->consensus != NULL) {
              our_height = consensus_get_height(node->consensus);
            }

            /* Service flags: NODE_NETWORK (1) */
            uint64_t services = 1;
            peer_send_version(peer, services, (int32_t)our_height, true);
          } else {
            log_warn(LOG_COMP_NET, "Failed to connect to %s:%u: error %d", ip_str, addr.port, result);
          }
          break; /* Only one connection attempt per maintenance cycle */
        }
      }
    } else {
      log_debug(LOG_COMP_NET, "No addresses available for outbound connection (have %zu peers)", outbound_count);
    }
  }

  /* Task 5: Cleanup disconnected peers */
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    peer_t *peer = &node->peers[i];
    if (peer->state == PEER_STATE_DISCONNECTED && peer->socket != NULL) {
      /* Ensure socket is fully cleaned up */
      peer_init(peer); /* Re-initialize to clean state */
    }
  }

  return ECHO_OK;
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

/*
 * ============================================================================
 * OBSERVER MODE FUNCTIONS (Session 9.5)
 * ============================================================================
 */

bool node_is_observer(const node_t *node) {
  if (node == NULL) {
    return false;
  }
  return node->config.observer_mode;
}

void node_get_observer_stats(const node_t *node, observer_stats_t *stats) {
  if (node == NULL || stats == NULL) {
    return;
  }

  if (!node->config.observer_mode) {
    /* Not in observer mode - return empty stats */
    memset(stats, 0, sizeof(*stats));
    return;
  }

  /* Copy observer statistics */
  memcpy(stats, &node->observer_stats, sizeof(*stats));
}

void node_observe_block(node_t *node, const hash256_t *hash) {
  if (node == NULL || hash == NULL || !node->config.observer_mode) {
    return;
  }

  observer_stats_t *obs = &node->observer_stats;
  uint64_t now = plat_time_ms();

  /* Check if block already observed */
  for (size_t i = 0; i < obs->block_count; i++) {
    if (memcmp(&obs->blocks[i].hash, hash, sizeof(hash256_t)) == 0) {
      /* Already seen - increment peer count */
      obs->blocks[i].peer_count++;
      return;
    }
  }

  /* New block - add to ring buffer */
  size_t index = obs->block_write_index;
  obs->blocks[index].hash = *hash;
  obs->blocks[index].first_seen = now;
  obs->blocks[index].peer_count = 1;

  /* Update ring buffer position */
  obs->block_write_index = (obs->block_write_index + 1) % NODE_OBSERVER_MAX_BLOCKS;

  /* Update count (saturates at max) */
  if (obs->block_count < NODE_OBSERVER_MAX_BLOCKS) {
    obs->block_count++;
  }
}

void node_observe_tx(node_t *node, const hash256_t *txid) {
  if (node == NULL || txid == NULL || !node->config.observer_mode) {
    return;
  }

  observer_stats_t *obs = &node->observer_stats;
  uint64_t now = plat_time_ms();

  /* Check if transaction already observed (simple linear search for recent txs)
   * We only check the last 100 entries to avoid O(n) search on large buffer */
  size_t check_count = obs->tx_count < 100 ? obs->tx_count : 100;
  for (size_t i = 0; i < check_count; i++) {
    size_t idx = (obs->tx_write_index + NODE_OBSERVER_MAX_TXS - 1 - i) %
                 NODE_OBSERVER_MAX_TXS;
    if (memcmp(&obs->txs[idx].txid, txid, sizeof(hash256_t)) == 0) {
      /* Already seen recently - skip */
      return;
    }
  }

  /* New transaction - add to ring buffer */
  size_t index = obs->tx_write_index;
  obs->txs[index].txid = *txid;
  obs->txs[index].first_seen = now;

  /* Update ring buffer position */
  obs->tx_write_index = (obs->tx_write_index + 1) % NODE_OBSERVER_MAX_TXS;

  /* Update count (saturates at max) */
  if (obs->tx_count < NODE_OBSERVER_MAX_TXS) {
    obs->tx_count++;
  }
}

void node_observe_message(node_t *node, const char *command) {
  if (node == NULL || command == NULL || !node->config.observer_mode) {
    return;
  }

  observer_stats_t *obs = &node->observer_stats;

  /* Update message counters based on command */
  if (strcmp(command, "version") == 0) {
    obs->msg_version++;
  } else if (strcmp(command, "verack") == 0) {
    obs->msg_verack++;
  } else if (strcmp(command, "ping") == 0) {
    obs->msg_ping++;
  } else if (strcmp(command, "pong") == 0) {
    obs->msg_pong++;
  } else if (strcmp(command, "addr") == 0) {
    obs->msg_addr++;
  } else if (strcmp(command, "inv") == 0) {
    obs->msg_inv++;
  } else if (strcmp(command, "getdata") == 0) {
    obs->msg_getdata++;
  } else if (strcmp(command, "block") == 0) {
    obs->msg_block++;
  } else if (strcmp(command, "tx") == 0) {
    obs->msg_tx++;
  } else if (strcmp(command, "headers") == 0) {
    obs->msg_headers++;
  } else if (strcmp(command, "getblocks") == 0) {
    obs->msg_getblocks++;
  } else if (strcmp(command, "getheaders") == 0) {
    obs->msg_getheaders++;
  } else {
    obs->msg_other++;
  }
}
