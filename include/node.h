/**
 * Bitcoin Echo — Node Lifecycle Management
 *
 * This module provides the application layer entry point and orchestration
 * logic for the Bitcoin Echo node. It coordinates:
 *
 *   - Platform layer initialization
 *   - Database loading and chain state restoration
 *   - Network startup and peer management
 *   - Graceful shutdown with resource cleanup
 *
 * The node is the integration point where all layers come together:
 *   - Platform abstraction (sockets, threads, files)
 *   - Storage layer (block files, UTXO database, block index)
 *   - Consensus engine (validation, chain state)
 *   - Protocol layer (P2P messaging, sync, mempool)
 *
 * Session 9.1: Node initialization and shutdown sequences.
 *
 * Build once. Build right. Stop.
 */

#ifndef ECHO_NODE_H
#define ECHO_NODE_H

#include "echo_config.h" /* Must be first for network-specific constants */
#include "block.h"
#include "log.h"
#include "block_index_db.h"
#include "blocks_storage.h"
#include "chainstate.h"
#include "consensus.h"
#include "discovery.h"
#include "echo_types.h"
#include "mempool.h"
#include "peer.h"
#include "sync.h"
#include "utxo_db.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * ============================================================================
 * NODE CONFIGURATION
 * ============================================================================
 */

/**
 * Minimum pruning target in MB.
 * Must keep at least 550 blocks (~550 MB) for reorg safety.
 */
#define PRUNE_TARGET_MIN_MB 550

/**
 * Node configuration.
 *
 * All configuration is compile-time per project philosophy, but this struct
 * allows the node to be created with a specific data directory path.
 */
typedef struct {
  char data_dir[512];       /* Path to data directory */
  uint16_t port;            /* P2P port (default: network-specific) */
  uint16_t rpc_port;        /* RPC port (default: network-specific) */
  bool observer_mode;       /* If true, skip consensus/storage (Session 9.5) */
  uint64_t prune_target_mb; /* Pruning target in MB (0 = archival/no pruning) */
  log_level_t log_level;    /* Log verbosity (default: LOG_LEVEL_INFO) */
} node_config_t;

/**
 * Initialize node configuration with defaults.
 *
 * Parameters:
 *   config   - Configuration structure to initialize
 *   data_dir - Path to data directory (will be created if needed)
 */
void node_config_init(node_config_t *config, const char *data_dir);

/*
 * ============================================================================
 * NODE STATE
 * ============================================================================
 */

/**
 * Node state enumeration.
 */
typedef enum {
  NODE_STATE_UNINITIALIZED, /* Not yet initialized */
  NODE_STATE_INITIALIZING,  /* Initialization in progress */
  NODE_STATE_STARTING,      /* Starting network and services */
  NODE_STATE_RUNNING,       /* Fully operational */
  NODE_STATE_STOPPING,      /* Shutdown in progress */
  NODE_STATE_STOPPED,       /* Fully stopped */
  NODE_STATE_ERROR          /* Error state */
} node_state_t;

/**
 * Node statistics.
 */
typedef struct {
  /* Chain state */
  uint32_t chain_height;      /* Current chain height */
  work256_t chain_work;       /* Total accumulated work */
  size_t utxo_count;          /* Number of UTXOs */
  size_t block_index_count;   /* Number of known block headers */

  /* Network state */
  size_t peer_count;          /* Number of connected peers */
  size_t outbound_peers;      /* Number of outbound connections */
  size_t inbound_peers;       /* Number of inbound connections */

  /* Mempool state */
  size_t mempool_size;        /* Number of transactions in mempool */
  size_t mempool_bytes;       /* Total mempool size in bytes */

  /* Sync state */
  bool is_syncing;            /* Whether in initial block download */
  float sync_progress;        /* Sync progress (0.0 - 100.0) */

  /* Timing */
  uint64_t start_time;        /* Node start time (plat_time_ms) */
  uint64_t uptime_ms;         /* Uptime in milliseconds */
  uint32_t start_height;      /* Validated height when node started */
} node_stats_t;

/*
 * ============================================================================
 * NODE STRUCTURE
 * ============================================================================
 */

/**
 * The node (opaque structure).
 *
 * Contains all node state including databases, consensus engine,
 * network connections, and service threads.
 */
typedef struct node node_t;

/*
 * ============================================================================
 * NODE LIFECYCLE
 * ============================================================================
 */

/**
 * Create and initialize a node.
 *
 * This performs the complete initialization sequence:
 *   1. Create data directory structure if needed
 *   2. Open or create databases (UTXO, block index)
 *   3. Initialize block storage manager
 *   4. Create and restore consensus engine state
 *   5. Initialize mempool
 *   6. Initialize peer discovery
 *
 * The node is created but NOT started. Network connections are not
 * established until node_start() is called.
 *
 * Parameters:
 *   config - Node configuration
 *
 * Returns:
 *   Newly created node, or NULL on failure.
 *   On failure, call node_get_error() for details (if applicable).
 */
node_t *node_create(const node_config_t *config);

/**
 * Start the node.
 *
 * This starts all services:
 *   1. Start listening for inbound connections
 *   2. Connect to outbound peers
 *   3. Begin initial block download if needed
 *
 * The node runs in the background using platform threads.
 * Use node_is_running() to check status and node_get_stats() for progress.
 *
 * Parameters:
 *   node - The node to start
 *
 * Returns:
 *   ECHO_OK on success, error code on failure.
 */
echo_result_t node_start(node_t *node);

/**
 * Stop the node gracefully.
 *
 * This performs graceful shutdown:
 *   1. Stop accepting new connections
 *   2. Disconnect all peers with proper goodbye
 *   3. Flush pending database writes
 *   4. Close databases
 *   5. Release all resources
 *
 * This function blocks until shutdown is complete.
 *
 * Parameters:
 *   node - The node to stop
 *
 * Returns:
 *   ECHO_OK on success, error code on failure.
 */
echo_result_t node_stop(node_t *node);

/**
 * Destroy a node and free all resources.
 *
 * If the node is running, this calls node_stop() first.
 *
 * Parameters:
 *   node - The node to destroy (may be NULL)
 */
void node_destroy(node_t *node);

/*
 * ============================================================================
 * NODE QUERIES
 * ============================================================================
 */

/**
 * Get current node state.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Current node state
 */
node_state_t node_get_state(const node_t *node);

/**
 * Check if node is running.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   true if node is in NODE_STATE_RUNNING
 */
bool node_is_running(const node_t *node);

/**
 * Check if node is in initial block download.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   true if node is syncing the blockchain
 */
bool node_is_syncing(const node_t *node);

/**
 * Get node statistics.
 *
 * Parameters:
 *   node  - The node
 *   stats - Output: statistics structure
 */
void node_get_stats(const node_t *node, node_stats_t *stats);

/**
 * Get human-readable string for node state.
 *
 * Parameters:
 *   state - Node state
 *
 * Returns:
 *   Static string describing the state
 */
const char *node_state_string(node_state_t state);

/*
 * ============================================================================
 * NODE COMPONENT ACCESS
 * ============================================================================
 *
 * These functions provide access to node subsystems for the event loop
 * and RPC interface (implemented in later sessions).
 */

/**
 * Get the consensus engine.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Pointer to consensus engine
 */
consensus_engine_t *node_get_consensus(node_t *node);

/**
 * Get the consensus engine (const version).
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Const pointer to consensus engine
 */
const consensus_engine_t *node_get_consensus_const(const node_t *node);

/**
 * Get the mempool.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Pointer to mempool
 */
mempool_t *node_get_mempool(node_t *node);

/**
 * Get the mempool (const version).
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Const pointer to mempool
 */
const mempool_t *node_get_mempool_const(const node_t *node);

/**
 * Get the sync manager.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Pointer to sync manager
 */
sync_manager_t *node_get_sync_manager(node_t *node);

/**
 * Get the peer address manager.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Pointer to peer address manager
 */
peer_addr_manager_t *node_get_addr_manager(node_t *node);

/**
 * Get the block storage manager.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Pointer to block storage manager
 */
block_file_manager_t *node_get_block_storage(node_t *node);

/**
 * Get the UTXO database.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Pointer to UTXO database
 */
utxo_db_t *node_get_utxo_db(node_t *node);

/**
 * Get the block index database.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Pointer to block index database
 */
block_index_db_t *node_get_block_index_db(node_t *node);

/**
 * Get the node's data directory path.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Path to data directory
 */
const char *node_get_data_dir(const node_t *node);

/*
 * ============================================================================
 * PEER MANAGEMENT
 * ============================================================================
 */

/**
 * Maximum number of peers the node can track.
 */
#define NODE_MAX_PEERS ECHO_MAX_TOTAL_PEERS

/**
 * Get the number of connected peers.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Number of connected peers
 */
size_t node_get_peer_count(const node_t *node);

/**
 * Get a connected peer by index.
 *
 * Parameters:
 *   node  - The node
 *   index - Peer index (0 to node_get_peer_count() - 1)
 *
 * Returns:
 *   Pointer to peer, or NULL if index out of range
 */
peer_t *node_get_peer(node_t *node, size_t index);

/**
 * Disconnect a peer.
 *
 * Parameters:
 *   node   - The node
 *   peer   - Peer to disconnect
 *   reason - Disconnect reason
 */
void node_disconnect_peer(node_t *node, peer_t *peer,
                          peer_disconnect_reason_t reason);

/*
 * ============================================================================
 * EVENT LOOP PROCESSING
 * ============================================================================
 */

/**
 * Process peer connections and messages.
 *
 * This is the main peer processing routine for the event loop:
 *   1. Accept new inbound connections (if listening)
 *   2. Check outbound connection attempts
 *   3. Receive and process messages from all connected peers
 *   4. Send queued messages to peers
 *   5. Disconnect unresponsive or misbehaving peers
 *
 * This function should be called regularly from the main event loop.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   ECHO_OK on success
 */
echo_result_t node_process_peers(node_t *node);

/**
 * Process received blocks.
 *
 * This validates and applies received blocks to the chain:
 *   1. Validate block headers and transactions
 *   2. Update chain state and UTXO set
 *   3. Handle reorganizations
 *   4. Update sync progress
 *   5. Relay new blocks to peers
 *
 * This function should be called regularly from the main event loop.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   ECHO_OK on success
 */
echo_result_t node_process_blocks(node_t *node);

/**
 * Apply a validated block to the node.
 *
 * This function performs full block application with persistence:
 *   1. Apply block to consensus engine (UTXO set, chain tip)
 *   2. Store block in block files (blk*.dat)
 *   3. Update block index database
 *   4. Update UTXO database atomically
 *
 * The block must have been validated first via consensus_validate_block().
 *
 * Session 9.6.0: Storage Foundation & Chain Restoration
 *
 * Parameters:
 *   node  - The node
 *   block - The validated block to apply
 *
 * Returns:
 *   ECHO_OK on success, error code on failure
 */
echo_result_t node_apply_block(node_t *node, const block_t *block);

/**
 * Perform periodic maintenance tasks.
 *
 * This handles timer-based operations:
 *   1. Ping peers to keep connections alive
 *   2. Request new blocks/headers if syncing stalled
 *   3. Evict stale mempool transactions
 *   4. Update sync progress metrics
 *   5. Cleanup disconnected peers
 *
 * This function should be called at regular intervals (e.g., every second).
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   ECHO_OK on success
 */
echo_result_t node_maintenance(node_t *node);

/*
 * ============================================================================
 * SIGNAL HANDLING
 * ============================================================================
 */

/**
 * Request node shutdown.
 *
 * This is signal-safe and can be called from a signal handler.
 * The node will begin graceful shutdown at the next opportunity.
 *
 * Parameters:
 *   node - The node
 */
void node_request_shutdown(node_t *node);

/**
 * Check if shutdown has been requested.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   true if shutdown has been requested
 */
bool node_shutdown_requested(const node_t *node);

/*
 * ============================================================================
 * OBSERVER MODE (Session 9.5)
 * ============================================================================
 */

/**
 * Maximum number of observed blocks/transactions to track.
 */
#define NODE_OBSERVER_MAX_BLOCKS 100
#define NODE_OBSERVER_MAX_TXS 1000

/**
 * Observed block information.
 */
typedef struct {
  hash256_t hash;       /* Block hash */
  uint64_t first_seen;  /* Timestamp (plat_time_ms) */
  uint32_t peer_count;  /* Number of peers that announced it */
} observer_block_t;

/**
 * Observed transaction information.
 */
typedef struct {
  hash256_t txid;       /* Transaction ID */
  uint64_t first_seen;  /* Timestamp (plat_time_ms) */
} observer_tx_t;

/**
 * Observer mode statistics.
 */
typedef struct {
  /* Message counts by type */
  uint64_t msg_version;
  uint64_t msg_verack;
  uint64_t msg_ping;
  uint64_t msg_pong;
  uint64_t msg_addr;
  uint64_t msg_inv;
  uint64_t msg_getdata;
  uint64_t msg_block;
  uint64_t msg_tx;
  uint64_t msg_headers;
  uint64_t msg_getblocks;
  uint64_t msg_getheaders;
  uint64_t msg_other;

  /* Recent observations */
  observer_block_t blocks[NODE_OBSERVER_MAX_BLOCKS];
  size_t block_count;
  size_t block_write_index; /* Ring buffer write position */

  observer_tx_t txs[NODE_OBSERVER_MAX_TXS];
  size_t tx_count;
  size_t tx_write_index; /* Ring buffer write position */
} observer_stats_t;

/**
 * Check if node is in observer mode.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   true if node is in observer mode
 */
bool node_is_observer(const node_t *node);

/**
 * Get observer statistics.
 *
 * Observer statistics are tracked in all modes. In observer-only mode
 * (--observe), the node only observes. In full mode, the node observes
 * AND validates.
 *
 * Parameters:
 *   node  - The node
 *   stats - Output: observer statistics
 */
void node_get_observer_stats(const node_t *node, observer_stats_t *stats);

/**
 * Record an observed block announcement.
 *
 * Parameters:
 *   node  - The node
 *   hash  - Block hash
 */
void node_observe_block(node_t *node, const hash256_t *hash);

/**
 * Record an observed transaction announcement.
 *
 * Parameters:
 *   node - The node
 *   txid - Transaction ID
 */
void node_observe_tx(node_t *node, const hash256_t *txid);

/**
 * Record a received protocol message (for statistics).
 *
 * Parameters:
 *   node    - The node
 *   command - Message command string (e.g., "version", "inv")
 */
void node_observe_message(node_t *node, const char *command);

/*
 * ============================================================================
 * BLOCK PIPELINE (Session 9.6.1)
 * ============================================================================
 */

/**
 * Check if a block hash is known to be invalid.
 *
 * The node maintains a ring buffer of recently rejected block hashes
 * to avoid re-downloading and re-validating blocks that have already
 * failed validation.
 *
 * Parameters:
 *   node - The node
 *   hash - Block hash to check
 *
 * Returns:
 *   true if block is known invalid, false otherwise
 */
bool node_is_block_invalid(const node_t *node, const hash256_t *hash);

/**
 * Get the count of known invalid blocks.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Number of blocks in the invalid blocks list
 */
size_t node_get_invalid_block_count(const node_t *node);

/**
 * Process a received block through the validation pipeline.
 *
 * This is the main entry point for block processing:
 *   1. Check if block is known invalid (reject if so)
 *   2. Validate via consensus engine
 *   3. Apply to chain state and storage
 *   4. Announce to peers if valid
 *
 * Parameters:
 *   node  - The node
 *   block - The block to process
 *
 * Returns:
 *   ECHO_OK if block was valid and applied
 *   ECHO_ERR_INVALID if block failed validation
 *   ECHO_ERR_EXISTS if block was already known
 */
echo_result_t node_process_received_block(node_t *node, const block_t *block);

/*
 * ============================================================================
 * PRUNING (Session 9.6.2)
 * ============================================================================
 */

/**
 * Check if pruning is enabled.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   true if prune_target_mb > 0 in config
 */
bool node_is_pruning_enabled(const node_t *node);

/**
 * Get the configured pruning target in MB.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Pruning target in MB (0 = archival/no pruning)
 */
uint64_t node_get_prune_target(const node_t *node);

/**
 * Get the current pruned height.
 *
 * This is the lowest block height for which we have block data.
 * Blocks below this height have been pruned.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Lowest block height with data (0 if unpruned or genesis)
 */
uint32_t node_get_pruned_height(const node_t *node);

/**
 * Check if a block has been pruned.
 *
 * Parameters:
 *   node   - The node
 *   height - Block height to check
 *
 * Returns:
 *   true if block data has been pruned and is unavailable
 */
bool node_is_block_pruned(const node_t *node, uint32_t height);

/**
 * Prune blocks up to the specified height.
 *
 * Deletes block files containing blocks up to (but not including) height.
 * Updates the block index to mark blocks as pruned.
 *
 * Parameters:
 *   node           - The node
 *   target_height  - Prune blocks below this height
 *
 * Returns:
 *   The actual height pruned to (may be less than requested)
 */
uint32_t node_prune_blocks(node_t *node, uint32_t target_height);

/**
 * Perform automatic pruning if needed.
 *
 * Called after applying a new block to check if disk usage exceeds
 * the pruning target. If so, prunes the oldest blocks to bring
 * usage under target while maintaining reorg safety margin.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   ECHO_OK if pruning succeeded or not needed
 */
echo_result_t node_maybe_prune(node_t *node);

/**
 * Get current block storage disk usage in bytes.
 *
 * Parameters:
 *   node - The node
 *
 * Returns:
 *   Total bytes used by block files
 */
uint64_t node_get_block_storage_size(const node_t *node);

#endif /* ECHO_NODE_H */
