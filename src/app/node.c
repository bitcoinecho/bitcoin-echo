/**
 * Bitcoin Echo — Node Lifecycle Implementation
 *
 * This module implements the node initialization and shutdown sequences.
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
#include "block_tracker.h"
#include "blocks_storage.h"
#include "chainstate.h"
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
#include "tx_validate.h"
#include "utxo.h"
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
  uint32_t start_height;            /* Validated height when node started */

  /* Storage layer (NULL if in observer mode) */
  utxo_db_t utxo_db;
  block_index_db_t block_index_db;
  block_storage_t block_storage;           /* File-per-block storage */
  uint64_t cached_storage_size;            /* Cached size, updated every prune check */
  bool utxo_db_open;
  bool block_index_db_open;
  bool block_storage_init;

  /* IBD optimization: defer UTXO persistence until shutdown */
  bool ibd_mode;                   /* Currently in initial block download */

  /* Consensus engine (NULL if in observer mode) */
  consensus_engine_t *consensus;

  /* Mempool (NULL if in observer mode) */
  mempool_t *mempool;

  /* Sync manager */
  sync_manager_t *sync_mgr;

  /* Peer discovery and management */
  peer_addr_manager_t addr_manager;
  peer_t peers[NODE_MAX_PEERS];

  /* Listening socket */
  plat_socket_t *listen_socket;
  bool is_listening;

  /* Observer mode statistics */
  observer_stats_t observer_stats;

  /* Block pipeline tracking */
  #define NODE_MAX_INVALID_BLOCKS 1000
  hash256_t invalid_blocks[NODE_MAX_INVALID_BLOCKS];
  size_t invalid_block_count;
  size_t invalid_block_write_idx;  /* Ring buffer write position */

};

/*
 * ============================================================================
 * FORWARD DECLARATIONS
 * ============================================================================
 */

static echo_result_t node_init_directories(node_t *node);
static echo_result_t node_flush_utxo_shutdown(node_t *node);
static echo_result_t node_init_databases(node_t *node);
static echo_result_t node_init_consensus(node_t *node);
static echo_result_t node_restore_chain_state(node_t *node);
static echo_result_t node_init_mempool(node_t *node);
static echo_result_t node_init_discovery(node_t *node);
static echo_result_t node_init_sync(node_t *node);
static void node_cleanup(node_t *node);

/* Sync manager callbacks */
static echo_result_t sync_cb_get_block(const hash256_t *hash, block_t *block_out,
                                       void *ctx);
/* node_store_block is public, forward declaration not needed */
static echo_result_t sync_cb_validate_header(const block_header_t *header,
                                             const hash256_t *hash,
                                             const block_index_t *prev_index,
                                             void *ctx);
static echo_result_t sync_cb_store_header(const block_header_t *header,
                                          const block_index_t *index, void *ctx);
/* Helper functions */
static uint64_t generate_nonce(void);

/* Sync manager send callbacks */
static void sync_cb_send_getheaders(peer_t *peer, const hash256_t *locator,
                                    size_t locator_len,
                                    const hash256_t *stop_hash, void *ctx);
static void sync_cb_send_getdata_blocks(peer_t *peer, const hash256_t *hashes,
                                        size_t count, void *ctx);
static echo_result_t sync_cb_get_block_hash_at_height(uint32_t height,
                                                       hash256_t *hash,
                                                       void *ctx);
static void sync_cb_disconnect_peer(peer_t *peer, const char *reason,
                                    void *ctx);
static uint64_t sync_cb_get_storage_size(void *ctx);

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

  /* Default log level */
  config->log_level = LOG_LEVEL_INFO;

  /* Default to AssumeValid enabled (skip scripts for historical blocks) */
  config->assume_valid = true;
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

    /* Record starting height for "blocks this session" metric */
    uint32_t height = consensus_get_height(node->consensus);
    /* UINT32_MAX means "not initialized" - treat as 0 for fresh starts */
    node->start_height = (height == UINT32_MAX) ? 0 : height;

    /* Step 4: Initialize mempool (full node only) */
    result = node_init_mempool(node);
    if (result != ECHO_OK) {
      node_cleanup(node);
      free(node);
      return NULL;
    }

    /* Step 5: Initialize sync manager (full node only) */
    result = node_init_sync(node);
    if (result != ECHO_OK) {
      node_cleanup(node);
      free(node);
      return NULL;
    }

  }

  /* Step 6: Initialize peer discovery (both modes) */
  result = node_init_discovery(node);
  if (result != ECHO_OK) {
    node_cleanup(node);
    free(node);
    return NULL;
  }

  /* Initialize observer statistics (tracked in all modes) */
  memset(&node->observer_stats, 0, sizeof(observer_stats_t));

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

  /* Enable IBD mode for fast initial sync (will be disabled when sync complete)
   */
  node->ibd_mode = true;
  db_set_ibd_mode(&node->utxo_db.db, true);

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

  /* Enable IBD mode for block index DB too (synchronous=OFF for speed) */
  db_set_ibd_mode(&node->block_index_db.db, true);

  /* Initialize file-per-block storage */
  result = block_storage_create(&node->block_storage, node->config.data_dir);
  if (result != ECHO_OK) {
    log_error(LOG_COMP_STORE, "Failed to initialize block storage: %d", result);
    return result;
  }
  node->block_storage_init = true;

  return ECHO_OK;
}

/**
 * Context for UTXO restoration iterator callback.
 */
typedef struct {
  utxo_set_t *utxo_set;
  size_t loaded;
  echo_result_t result;
} utxo_restore_ctx_t;

/**
 * Iterator callback for loading UTXOs from database into memory.
 * Returns true to continue iteration, false to stop on error.
 */
static bool utxo_restore_callback(const utxo_entry_t *entry, void *user_data) {
  utxo_restore_ctx_t *ctx = (utxo_restore_ctx_t *)user_data;

  echo_result_t result = utxo_set_insert(ctx->utxo_set, entry);
  if (result == ECHO_OK) {
    ctx->loaded++;
  } else if (result == ECHO_ERR_EXISTS) {
    /* Duplicate - shouldn't happen but harmless, skip */
    ctx->loaded++;
  } else {
    /* Actual error - stop iteration */
    ctx->result = result;
    return false;
  }

  return true;
}

/**
 * Restore chain state from persistent storage.
 *
 * This function loads the blockchain state from the databases into the
 * consensus engine's in-memory structures. It performs:
 *
 *   1. Query block_index_db for the best chain tip
 *   2. Load all block headers from genesis to tip into the block index map
 *   3. Restore the chain tip in the consensus engine
 *   4. Restore UTXO set from database into memory
 *   5. Verify UTXO database consistency (count check)
 */
static echo_result_t node_restore_chain_state(node_t *node) {
  echo_result_t result;

  /* Query the best chain tip from the block index database */
  block_index_entry_t best_entry;
  result = block_index_db_get_best_chain(&node->block_index_db, &best_entry);

  if (result == ECHO_ERR_NOT_FOUND) {
    /* Empty database - fresh start, add genesis to block index */
    log_info(LOG_COMP_MAIN, "No existing chain state found, starting fresh");

    /*
     * Add genesis block header to the block_index_map.
     * This is required for sync_handle_headers to connect incoming headers
     * (block 1's prev_hash is the genesis hash).
     */
    chainstate_t *chainstate = consensus_get_chainstate(node->consensus);
    if (chainstate != NULL) {
      block_header_t genesis;
      block_genesis_header(&genesis);

      block_index_t *genesis_index = NULL;
      echo_result_t add_result =
          consensus_add_header(node->consensus, &genesis, &genesis_index);

      if (add_result == ECHO_OK || add_result == ECHO_ERR_EXISTS) {
        /* If ECHO_ERR_EXISTS, look up the existing genesis index */
        if (genesis_index == NULL && add_result == ECHO_ERR_EXISTS) {
          hash256_t genesis_hash;
          block_header_hash(&genesis, &genesis_hash);
          block_index_map_t *map = chainstate_get_block_index_map(chainstate);
          genesis_index = block_index_map_lookup(map, &genesis_hash);
        }

        /* Set genesis as the chain tip so sync_add_peer calculates
         * our_height correctly and peers become sync candidates */
        if (genesis_index != NULL) {
          chainstate_set_tip_index(chainstate, genesis_index);
          log_info(LOG_COMP_MAIN, "Genesis block set as chain tip (height 0)");

          /* Persist genesis to database so it can be restored on restart */
          if (node->block_index_db_open) {
            block_index_entry_t genesis_entry = {
                .hash = genesis_index->hash,
                .height = 0,
                .header = genesis,
                .chainwork = genesis_index->chainwork,
                .status = BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_VALID_CHAIN,
                .data_file = -1,
                .data_pos = 0};
            echo_result_t db_result =
                block_index_db_insert(&node->block_index_db, &genesis_entry);
            if (db_result != ECHO_OK && db_result != ECHO_ERR_EXISTS) {
              log_warn(LOG_COMP_MAIN, "Failed to persist genesis: %d", db_result);
            }
          }
        }
      } else {
        log_error(LOG_COMP_MAIN, "Failed to add genesis block: %d", add_result);
        return add_result;
      }
    }

    return ECHO_OK;
  }

  if (result != ECHO_OK) {
    log_error(LOG_COMP_MAIN, "Failed to query best chain: %d", result);
    return result;
  }

  log_info(LOG_COMP_MAIN,
           "Restoring chain state: height=%u, blocks to load",
           best_entry.height);

  /*
   * Load all block headers from genesis (height 0) to tip.
   * We iterate in order to build the correct prev pointers in the block index.
   */
  chainstate_t *chainstate = consensus_get_chainstate(node->consensus);
  if (chainstate == NULL) {
    log_error(LOG_COMP_MAIN, "Failed to get chainstate from consensus engine");
    return ECHO_ERR_INVALID;
  }

  uint32_t loaded_count = 0;
  for (uint32_t height = 0; height <= best_entry.height; height++) {
    block_index_entry_t entry;
    result = block_index_db_get_chain_block(&node->block_index_db, height, &entry);

    if (result != ECHO_OK) {
      log_error(LOG_COMP_MAIN,
                "Failed to load block at height %u: %d", height, result);
      return result;
    }

    /* Add header to consensus engine's block index map */
    block_index_t *index = NULL;
    result = consensus_add_header(node->consensus, &entry.header, &index);

    if (result != ECHO_OK && result != ECHO_ERR_EXISTS) {
      log_error(LOG_COMP_MAIN,
                "Failed to add header at height %u: %d", height, result);
      return result;
    }

    /* Update chainwork from database (may differ from calculated if we have
     * persisted state from previous sessions) */
    if (index != NULL) {
      index->chainwork = entry.chainwork;
      index->on_main_chain = (entry.status & BLOCK_STATUS_VALID_CHAIN) != 0;
    }

    loaded_count++;

    /* Log progress every 10000 blocks */
    if (height > 0 && height % 10000 == 0) {
      log_info(LOG_COMP_MAIN, "Loaded %u block headers...", height);
    }
  }

  /* Set the best header index for sync locator building.
   *
   * IMPORTANT: We use chainstate_set_best_header_index() rather than
   * chainstate_set_tip_index() because we're loading HEADERS, not validated
   * blocks. The chainstate tip (state->tip) should remain at the last
   * validated block (genesis if blocks=0), while tip_index points to
   * the best header for building getheaders locators.
   *
   * chainstate_get_height() returns state->tip.height (validated blocks).
   * chainstate_get_tip_index() returns state->tip_index (best header).
   */
  block_index_map_t *map = chainstate_get_block_index_map(chainstate);
  block_index_t *tip_index = block_index_map_lookup(map, &best_entry.hash);

  if (tip_index != NULL) {
    chainstate_set_best_header_index(chainstate, tip_index);
    log_info(LOG_COMP_MAIN, "Best header restored: height=%u", tip_index->height);
  }

  /*
   * Restore validated tip from database.
   * This tells us how far we've actually validated blocks (with UTXO updates),
   * not just how many headers we have.
   *
   * The validated_tip is saved at fixed 5000-block checkpoints during IBD.
   * We do NOT save on graceful shutdown - this keeps the system simple and
   * avoids UTXO/stored-block state mismatches.
   *
   * NOTE: chainstate_set_tip_index() modifies tip_index, which would overwrite
   * the best header we just set. We re-set the best header after this.
   */
  uint32_t validated_height = 0;
  result = block_index_db_get_validated_tip(&node->block_index_db,
                                            &validated_height, NULL);
  log_info(LOG_COMP_MAIN, "Checkpoint restored: validated_height=%u", validated_height);

  /*
   * OPTIMIZATION: Keep stored blocks above the checkpoint for revalidation.
   *
   * Previously, we invalidated stored blocks above the checkpoint, forcing
   * re-download. But the blocks themselves are valid Bitcoin blocks - only
   * the UTXO set needs rebuilding. The sync code already handles loading
   * stored blocks from disk and revalidating them (see sync.c lines 1615-1643).
   *
   * By keeping stored blocks, we can validate from disk at ~50+ blk/s instead
   * of waiting for network downloads at ~5 blk/s.
   */
  if (validated_height > 0 && node->block_index_db_open) {
    /* Count stored blocks above checkpoint for logging */
    db_stmt_t count_stmt;
    echo_result_t count_result = db_prepare(
        &node->block_index_db.db,
        "SELECT COUNT(*) FROM blocks WHERE height > ? AND (status & 16) != 0",
        &count_stmt);
    if (count_result == ECHO_OK) {
      db_bind_int(&count_stmt, 1, (int)validated_height);
      if (db_step(&count_stmt) == ECHO_OK) {
        int64_t stored_count = db_column_int(&count_stmt, 0);
        if (stored_count > 0) {
          log_info(LOG_COMP_MAIN,
                   "Keeping %lld stored blocks above checkpoint for revalidation",
                   (long long)stored_count);
        }
      }
      db_stmt_finalize(&count_stmt);
    }
  }

  if (result == ECHO_OK && validated_height > 0) {
    /* Find the block index for the validated tip */
    block_index_entry_t validated_entry;
    result = block_index_db_get_chain_block(&node->block_index_db,
                                            validated_height, &validated_entry);
    log_info(LOG_COMP_MAIN, "get_chain_block(height=%u) result=%d",
             validated_height, result);

    if (result == ECHO_OK) {
      block_index_t *validated_index =
          block_index_map_lookup(map, &validated_entry.hash);
      log_info(LOG_COMP_MAIN, "block_index_map_lookup: %s (entry.height=%u)",
               validated_index ? "FOUND" : "NOT FOUND", validated_entry.height);

      if (validated_index != NULL) {
        log_info(LOG_COMP_MAIN,
                 "validated_index->height=%u before chainstate_set_tip_index",
                 validated_index->height);

        /* Set the chainstate validated tip (also modifies tip_index) */
        chainstate_set_tip_index(chainstate, validated_index);

        /* Verify the tip was set correctly */
        uint32_t check_height = chainstate_get_height(chainstate);
        log_info(LOG_COMP_MAIN,
                 "After chainstate_set_tip_index: chainstate_get_height=%u",
                 check_height);

        /* Mark consensus engine as initialized so consensus_get_height works */
        consensus_mark_initialized(node->consensus);

        log_info(LOG_COMP_MAIN, "Validated tip restored: height=%u",
                 validated_height);
      } else {
        log_error(LOG_COMP_MAIN,
                  "Failed to find validated block in index map at height %u",
                  validated_height);
      }
    } else {
      log_error(LOG_COMP_MAIN,
                "Failed to get chain block at height %u: result=%d",
                validated_height, result);
    }
  } else {
    log_info(LOG_COMP_MAIN,
             "No validated tip found (result=%d, height=%u), "
             "will start validation from genesis",
             result, validated_height);
  }

  /*
   * Re-set best header index AFTER validated tip restoration.
   * This ensures tip_index points to our best known header (for sync locator
   * building) rather than the validated tip.
   */
  if (tip_index != NULL) {
    chainstate_set_best_header_index(chainstate, tip_index);
  }

  /*
   * Restore UTXO set from database into memory.
   *
   * During IBD, UTXOs are flushed to the database on graceful shutdown.
   * We need to restore them so validation can continue from the validated tip.
   * Without this, validation would fail with "Missing input UTXO" errors
   * because the in-memory UTXO set would be empty.
   */
  if (node->utxo_db_open && validated_height > 0) {
    size_t utxo_count = 0;
    result = utxo_db_count(&node->utxo_db, &utxo_count);

    if (result == ECHO_OK && utxo_count > 0) {
      log_info(LOG_COMP_MAIN,
               "Restoring %zu UTXOs from database into memory...", utxo_count);

      /* Get the mutable UTXO set from chainstate */
      utxo_set_t *utxo_set = chainstate_get_utxo_set_mutable(chainstate);
      if (utxo_set != NULL) {
        utxo_restore_ctx_t ctx = {
            .utxo_set = utxo_set,
            .loaded = 0,
            .result = ECHO_OK,
        };

        result = utxo_db_foreach(&node->utxo_db, utxo_restore_callback, &ctx);

        if (result == ECHO_OK && ctx.result == ECHO_OK) {
          log_info(LOG_COMP_MAIN, "UTXO restoration complete: %zu entries loaded",
                   ctx.loaded);
        } else {
          log_error(LOG_COMP_MAIN,
                    "UTXO restoration failed: db_result=%d, ctx_result=%d",
                    result, ctx.result);
          /* Non-fatal: validation will re-build UTXO set from blocks */
        }
      } else {
        log_warn(LOG_COMP_MAIN, "Could not get mutable UTXO set for restoration");
      }
    } else if (result == ECHO_OK) {
      log_info(LOG_COMP_MAIN, "UTXO database empty, nothing to restore");
    } else {
      log_warn(LOG_COMP_MAIN, "Failed to count UTXOs in database: %d", result);
    }
  } else if (validated_height == 0) {
    log_info(LOG_COMP_MAIN,
             "No validated tip, skipping UTXO restoration (will build from genesis)");
  }

  log_info(LOG_COMP_MAIN,
           "Chain state restoration complete: %u headers loaded, validated=%u",
           loaded_count, validated_height);

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

  /* Configure AssumeValid based on node config */
  if (!node->config.assume_valid) {
    consensus_set_assume_valid_height(node->consensus, 0);
  }

  /*
   * Restore chain state from databases.
   * This loads block headers from block_index_db into the consensus engine
   * and sets up the chain tip. The UTXO set will be queried from utxo_db
   * during validation.
   */
  echo_result_t result = node_restore_chain_state(node);
  if (result != ECHO_OK) {
    log_error(LOG_COMP_MAIN, "Failed to restore chain state: %d", result);
    return result;
  }

  return ECHO_OK;
}

/*
 * ============================================================================
 * MEMPOOL CALLBACKS
 * ============================================================================
 *
 * These callbacks connect the mempool to the node's UTXO database,
 * consensus engine, and P2P layer for transaction validation and relay.
 */

/**
 * Look up a UTXO for mempool validation.
 *
 * Checks both the UTXO database (confirmed outputs) and the mempool
 * (unconfirmed outputs from ancestor transactions).
 */
static echo_result_t mempool_cb_get_utxo(const outpoint_t *outpoint,
                                          utxo_entry_t *entry, void *ctx) {
  node_t *node = (node_t *)ctx;
  if (node == NULL || outpoint == NULL || entry == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* First check the UTXO database for confirmed outputs */
  if (node->utxo_db_open) {
    utxo_entry_t *db_entry = NULL;
    echo_result_t result = utxo_db_lookup(&node->utxo_db, outpoint, &db_entry);
    if (result == ECHO_OK && db_entry != NULL) {
      /* Found in database - copy to caller's entry */
      entry->outpoint = db_entry->outpoint;
      entry->value = db_entry->value;
      entry->height = db_entry->height;
      entry->is_coinbase = db_entry->is_coinbase;
      entry->script_len = db_entry->script_len;
      /* Copy script if present */
      if (db_entry->script_len > 0 && db_entry->script_pubkey != NULL) {
        entry->script_pubkey = malloc(db_entry->script_len);
        if (entry->script_pubkey != NULL) {
          memcpy(entry->script_pubkey, db_entry->script_pubkey,
                 db_entry->script_len);
        }
      } else {
        entry->script_pubkey = NULL;
      }
      utxo_entry_destroy(db_entry);
      return ECHO_OK;
    }
  }

  /* Not in database - check mempool for unconfirmed ancestor outputs */
  if (node->mempool != NULL) {
    const mempool_entry_t *mem_entry = mempool_lookup(node->mempool,
                                                       &outpoint->txid);
    if (mem_entry != NULL && outpoint->vout < mem_entry->tx.output_count) {
      /* Found in mempool - build entry from transaction output */
      const tx_output_t *txout = &mem_entry->tx.outputs[outpoint->vout];
      entry->outpoint = *outpoint;
      entry->value = txout->value;
      entry->height = 0;       /* Unconfirmed - use height 0 */
      entry->is_coinbase = 0;  /* Mempool txs can't be coinbase */
      entry->script_len = txout->script_pubkey_len;
      if (txout->script_pubkey_len > 0 && txout->script_pubkey != NULL) {
        entry->script_pubkey = malloc(txout->script_pubkey_len);
        if (entry->script_pubkey != NULL) {
          memcpy(entry->script_pubkey, txout->script_pubkey,
                 txout->script_pubkey_len);
        }
      } else {
        entry->script_pubkey = NULL;
      }
      return ECHO_OK;
    }
  }

  return ECHO_ERR_NOT_FOUND;
}

/**
 * Get current block height for mempool operations.
 */
static uint32_t mempool_cb_get_height(void *ctx) {
  node_t *node = (node_t *)ctx;
  if (node == NULL || node->consensus == NULL) {
    return 0;
  }
  return consensus_get_height(node->consensus);
}

/**
 * Get median time past for locktime validation.
 *
 * Computes the median of the timestamps of the previous 11 blocks.
 */
static uint32_t mempool_cb_get_median_time(void *ctx) {
  node_t *node = (node_t *)ctx;
  if (node == NULL || node->consensus == NULL) {
    return 0;
  }

  /* Get the chain tip index */
  const block_index_t *tip = consensus_get_best_block_index(node->consensus);
  if (tip == NULL) {
    return 0;
  }

  /* Collect timestamps of the last 11 blocks (or fewer if chain is short) */
  uint32_t timestamps[11];
  size_t count = 0;
  const block_index_t *block = tip;

  while (block != NULL && count < 11) {
    timestamps[count] = block->timestamp;
    count++;
    block = block->prev;
  }

  if (count == 0) {
    return 0;
  }

  /* Sort timestamps to find median */
  for (size_t i = 0; i < count - 1; i++) {
    for (size_t j = i + 1; j < count; j++) {
      if (timestamps[j] < timestamps[i]) {
        uint32_t tmp = timestamps[i];
        timestamps[i] = timestamps[j];
        timestamps[j] = tmp;
      }
    }
  }

  /* Return median (middle element) */
  return timestamps[count / 2];
}

/**
 * Announce a new transaction to connected peers.
 *
 * Called by mempool when a transaction is accepted.
 * Sends INV message to all connected, ready peers.
 */
static void mempool_cb_announce_tx(const hash256_t *txid, void *ctx) {
  node_t *node = (node_t *)ctx;
  if (node == NULL || txid == NULL) {
    return;
  }

  /*
   * During initial block download, don't announce transactions to peers.
   * We shouldn't have any in our mempool anyway (we're dropping incoming
   * txs), but this is a safety check.
   */
  if (node->ibd_mode) {
    return;
  }

  /* Send INV to all connected peers */
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    peer_t *peer = &node->peers[i];
    if (peer_is_ready(peer)) {
      /* Build and send INV message */
      inv_vector_t inv_vec;
      inv_vec.type = INV_WITNESS_TX;  /* Use witness type for modern peers */
      inv_vec.hash = *txid;

      msg_t inv_msg;
      memset(&inv_msg, 0, sizeof(inv_msg));
      inv_msg.type = MSG_INV;
      inv_msg.payload.inv.count = 1;
      inv_msg.payload.inv.inventory = &inv_vec;

      peer_queue_message(peer, &inv_msg);
    }
  }

  log_debug(LOG_COMP_POOL, "Announced transaction to peers");
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
   * Set up mempool callbacks to connect to UTXO database and P2P layer.
   */
  mempool_callbacks_t callbacks = {
      .get_utxo = mempool_cb_get_utxo,
      .get_height = mempool_cb_get_height,
      .get_median_time = mempool_cb_get_median_time,
      .announce_tx = mempool_cb_announce_tx,
      .ctx = node};

  mempool_set_callbacks(node->mempool, &callbacks);

  log_info(LOG_COMP_POOL, "Mempool initialized with UTXO and relay callbacks");

  return ECHO_OK;
}

/*
 * ============================================================================
 * TRANSACTION ACCEPTANCE
 * ============================================================================
 *
 * Helper function for validating and accepting transactions into the mempool.
 * Used by both P2P transaction relay and RPC sendrawtransaction.
 */

/**
 * Validate and accept a transaction into the mempool.
 *
 * Performs full validation:
 *   1. Syntactic validation (structure checks)
 *   2. UTXO lookup for all inputs
 *   3. Script execution for all inputs
 *   4. Fee rate check against mempool minimum
 *   5. Add to mempool
 *
 * Parameters:
 *   node   - The node
 *   tx     - Transaction to accept (will be copied if accepted)
 *   result - Output: mempool accept result
 *
 * Returns:
 *   ECHO_OK if transaction accepted
 *   ECHO_ERR_* on validation failure
 */
static echo_result_t node_accept_transaction(node_t *node, const tx_t *tx,
                                              mempool_accept_result_t *result) {
  if (node == NULL || tx == NULL || result == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  mempool_accept_result_init(result);

  /* Skip validation in observer mode */
  if (node->config.observer_mode) {
    result->reason = MEMPOOL_REJECT_INVALID;
    return ECHO_ERR_INVALID;
  }

  /* Step 1: Syntactic validation */
  tx_validate_result_t validate_result;
  tx_validate_result_init(&validate_result);
  echo_result_t res = tx_validate_syntax(tx, &validate_result);
  if (res != ECHO_OK) {
    log_debug(LOG_COMP_POOL, "Transaction syntax validation failed: %s",
              tx_validate_error_string(validate_result.error));
    result->reason = MEMPOOL_REJECT_INVALID;
    return ECHO_ERR_INVALID;
  }

  /* Step 2: Look up UTXOs for all inputs */
  utxo_info_t *utxo_infos = calloc(tx->input_count, sizeof(utxo_info_t));
  if (utxo_infos == NULL) {
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  for (size_t i = 0; i < tx->input_count; i++) {
    const tx_input_t *input = &tx->inputs[i];
    utxo_entry_t *entry = NULL;

    /* Check UTXO database first */
    if (node->utxo_db_open) {
      res = utxo_db_lookup(&node->utxo_db, &input->prevout, &entry);
    } else {
      res = ECHO_ERR_NOT_FOUND;
    }

    /* Check mempool for unconfirmed outputs */
    if (res == ECHO_ERR_NOT_FOUND && node->mempool != NULL) {
      const mempool_entry_t *mem_entry =
          mempool_lookup(node->mempool, &input->prevout.txid);
      if (mem_entry != NULL &&
          input->prevout.vout < mem_entry->tx.output_count) {
        const tx_output_t *txout =
            &mem_entry->tx.outputs[input->prevout.vout];
        utxo_infos[i].value = txout->value;
        utxo_infos[i].script_pubkey = txout->script_pubkey;
        utxo_infos[i].script_pubkey_len = txout->script_pubkey_len;
        utxo_infos[i].height = 0;
        utxo_infos[i].is_coinbase = ECHO_FALSE;
        continue;
      }
    }

    if (res != ECHO_OK || entry == NULL) {
      log_debug(LOG_COMP_POOL, "Missing input UTXO at index %zu", i);
      free(utxo_infos);
      result->reason = MEMPOOL_REJECT_MISSING_INPUTS;
      return ECHO_ERR_NOT_FOUND;
    }

    utxo_infos[i].value = entry->value;
    utxo_infos[i].script_pubkey = entry->script_pubkey;
    utxo_infos[i].script_pubkey_len = entry->script_len;
    utxo_infos[i].height = entry->height;
    utxo_infos[i].is_coinbase = entry->is_coinbase ? ECHO_TRUE : ECHO_FALSE;
    /* Note: entry leaked intentionally - script pointer still needed */
  }

  /* Step 3: Build validation context */
  uint32_t current_height = 0;
  if (node->consensus != NULL) {
    current_height = consensus_get_height(node->consensus);
  }

  tx_validate_ctx_t ctx = {
      .block_height = current_height + 1,
      .block_time = (uint32_t)(plat_time_ms() / 1000),
      .median_time_past = (uint32_t)(plat_time_ms() / 1000),
      .utxos = utxo_infos,
      .utxo_count = tx->input_count,
      .script_flags = consensus_get_script_flags(current_height + 1)};

  /* Step 4: Full validation */
  tx_validate_result_init(&validate_result);
  res = tx_validate(tx, &ctx, &validate_result);
  if (res != ECHO_OK) {
    log_debug(LOG_COMP_POOL, "Transaction validation failed: %s",
              tx_validate_error_string(validate_result.error));
    free(utxo_infos);
    result->reason = MEMPOOL_REJECT_INVALID;
    return ECHO_ERR_INVALID;
  }

  /* Step 5: Check fee rate */
  satoshi_t fee = 0;
  res = tx_compute_fee(tx, utxo_infos, tx->input_count, &fee);
  free(utxo_infos);

  if (res != ECHO_OK) {
    result->reason = MEMPOOL_REJECT_INVALID;
    return ECHO_ERR_INVALID;
  }

  size_t vsize = tx_vsize(tx);
  uint64_t fee_rate = (vsize > 0) ? ((uint64_t)fee * 1000 / vsize) : 0;
  uint64_t min_fee_rate = mempool_min_fee_rate(node->mempool);

  if (fee_rate < min_fee_rate) {
    log_debug(LOG_COMP_POOL, "Fee rate too low: %llu < %llu sat/kvB",
              (unsigned long long)fee_rate, (unsigned long long)min_fee_rate);
    result->reason = MEMPOOL_REJECT_FEE_TOO_LOW;
    result->required_fee = (satoshi_t)(min_fee_rate * vsize / 1000);
    return ECHO_ERR_INVALID;
  }

  /* Step 6: Add to mempool */
  return mempool_add(node->mempool, tx, result);
}

/*
 * ============================================================================
 * SYNC MANAGER CALLBACKS
 * ============================================================================
 *
 * These callbacks connect the sync manager to the node's storage and
 * validation infrastructure.
 */

/**
 * Get block from storage.
 *
 * Called by sync manager to retrieve a previously stored block.
 * Used to process out-of-order blocks that were stored before their
 * parent was validated.
 */
static echo_result_t sync_cb_get_block(const hash256_t *hash, block_t *block_out,
                                       void *ctx) {
  node_t *node = (node_t *)ctx;
  if (node == NULL || hash == NULL || block_out == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (!node->block_storage_init || !node->block_index_db_open) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Look up block entry in database */
  block_index_entry_t entry;
  echo_result_t result =
      block_index_db_lookup_by_hash(&node->block_index_db, hash, &entry);
  if (result != ECHO_OK) {
    return result;
  }

  /* Check if block data is stored (HAVE_DATA flag set) */
  if (!(entry.status & BLOCK_STATUS_HAVE_DATA)) {
    return ECHO_ERR_NOT_FOUND; /* Header only, no block data */
  }

  /* Load block data from file-per-block storage */
  if (!node->block_storage_init) {
    return ECHO_ERR_NOT_FOUND;
  }

  uint8_t *block_data = NULL;
  uint32_t block_size = 0;
  result = block_storage_read_height(&node->block_storage, entry.height,
                                     &block_data, &block_size);
  if (result != ECHO_OK) {
    log_warn(LOG_COMP_STORE, "Failed to read block at height %u: %d",
             entry.height, result);
    return result;
  }

  /* Parse block data */
  size_t consumed;
  result = block_parse(block_data, block_size, block_out, &consumed);
  free(block_data);

  if (result != ECHO_OK) {
    log_warn(LOG_COMP_STORE, "Failed to parse stored block: %d", result);
    return result;
  }

  return ECHO_OK;
}

/**
 * Store block data.
 *
 * Called by sync manager to persist a block after download.
 * Uses file-per-block storage: each block stored as blocks/{height}.blk.
 * Enables out-of-order downloads and trivial pruning via unlink().
 * Sets BLOCK_STATUS_HAVE_DATA flag in the database.
 */
echo_result_t node_store_block(node_t *node, const block_t *block) {
  if (node == NULL || block == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Skip if file-per-block storage not initialized */
  if (!node->block_storage_init) {
    return ECHO_OK;
  }

  /* Compute block hash */
  hash256_t block_hash;
  echo_result_t result = block_header_hash(&block->header, &block_hash);
  if (result != ECHO_OK) {
    log_error(LOG_COMP_STORE, "Failed to compute block hash for storage");
    return result;
  }

  /* Serialize block to bytes */
  size_t block_size = block_serialize_size(block);
  uint8_t *block_data = malloc(block_size);
  if (block_data == NULL) {
    log_error(LOG_COMP_STORE, "Failed to allocate %zu bytes for block", block_size);
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  size_t written;
  result = block_serialize(block, block_data, block_size, &written);
  if (result != ECHO_OK) {
    free(block_data);
    log_error(LOG_COMP_STORE, "Failed to serialize block");
    return result;
  }

  /* Get block height from in-memory block index */
  uint32_t height = 0;
  chainstate_t *chainstate = consensus_get_chainstate(node->consensus);
  if (chainstate != NULL) {
    block_index_map_t *index_map = chainstate_get_block_index_map(chainstate);
    block_index_t *block_index = block_index_map_lookup(index_map, &block_hash);
    if (block_index != NULL) {
      height = block_index->height;
    } else {
      /* Try database lookup as fallback */
      block_index_entry_t entry;
      result = block_index_db_lookup_by_hash(&node->block_index_db, &block_hash,
                                             &entry);
      if (result == ECHO_OK) {
        height = entry.height;
      } else {
        free(block_data);
        log_error(LOG_COMP_STORE, "Cannot store block: height unknown");
        return ECHO_ERR_NOT_FOUND;
      }
    }
  }

  /* Write to file-per-block storage */
  result = block_storage_write_height(&node->block_storage, height,
                                      block_data, (uint32_t)written);
  free(block_data);

  if (result != ECHO_OK) {
    log_error(LOG_COMP_STORE, "Failed to write block %u to storage: %d", height,
              result);
    return result;
  }

  /* Update block index database with HAVE_DATA flag */
  if (node->block_index_db_open) {
    result = block_index_db_mark_have_data(&node->block_index_db, &block_hash);
    if (result != ECHO_OK && result != ECHO_ERR_NOT_FOUND) {
      log_warn(LOG_COMP_STORE, "Failed to update block index DB: %d", result);
      /* Continue anyway - block is stored on disk */
    }
  }

  log_debug(LOG_COMP_STORE, "Block stored: height=%u", height);

  return ECHO_OK;
}

/**
 * Sync callback wrapper for node_store_block.
 */
static echo_result_t sync_cb_store_block(const block_t *block, void *ctx) {
  return node_store_block((node_t *)ctx, block);
}

/**
 * Validate a block header (contextual validation).
 *
 * Called by sync manager during headers-first sync.
 */
static echo_result_t sync_cb_validate_header(const block_header_t *header,
                                             const hash256_t *hash,
                                             const block_index_t *prev_index,
                                             void *ctx) {
  node_t *node = (node_t *)ctx;
  if (node == NULL || header == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Use consensus engine header validation with pre-computed hash */
  consensus_result_t result;
  consensus_result_init(&result);

  bool valid =
      consensus_validate_header_with_hash(node->consensus, header, hash, &result);
  if (!valid) {
    log_warn(LOG_COMP_CONS, "Header validation failed: %s",
             consensus_error_str(result.error));
    return ECHO_ERR_INVALID;
  }

  /* If prev_index provided, check it matches header.prev_hash */
  if (prev_index != NULL) {
    if (memcmp(&header->prev_hash, &prev_index->hash, sizeof(hash256_t)) != 0) {
      log_warn(LOG_COMP_CONS, "Header prev_hash mismatch");
      return ECHO_ERR_INVALID;
    }
  }

  return ECHO_OK;
}

/**
 * Store/persist a validated header to disk.
 *
 * Called by sync manager immediately after header validation.
 * This ensures headers are persisted during sync, not just when
 * full blocks are validated.
 */
static echo_result_t sync_cb_store_header(const block_header_t *header,
                                          const block_index_t *index,
                                          void *ctx) {
  node_t *node = (node_t *)ctx;
  if (node == NULL || header == NULL || index == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Only persist if database is open */
  if (!node->block_index_db_open) {
    return ECHO_OK;
  }

  /* Build database entry
   * For headers-first sync, headers form a chain but blocks aren't validated yet.
   * Set VALID_HEADER (PoW validated) and VALID_CHAIN (on main chain) so they
   * can be loaded on restart. Full block validation will add VALID_SCRIPTS etc.
   */
  block_index_entry_t entry = {
      .hash = index->hash,
      .height = index->height,
      .header = *header,
      .chainwork = index->chainwork,
      .status = BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_VALID_CHAIN,
      .data_file = -1,  /* Not stored yet */
      .data_pos = 0};

  /* Insert into database */
  echo_result_t result = block_index_db_insert(&node->block_index_db, &entry);

  if (result == ECHO_ERR_EXISTS) {
    /* Already stored - not an error */
    return ECHO_OK;
  }

  return result;
}

/**
 * Mark a block as invalid (add to invalid blocks list).
 *
 * NOTE: Currently unused but will be needed when handling CHASE_UNVALID events
 * in the unified validation path. Kept for future use.
 */
__attribute__((unused))
static void node_mark_block_invalid(node_t *node, const hash256_t *hash) {
  /* Check if already marked - inline check to avoid forward declaration */
  for (size_t i = 0; i < node->invalid_block_count; i++) {
    if (memcmp(&node->invalid_blocks[i], hash, sizeof(hash256_t)) == 0) {
      return; /* Already marked */
    }
  }

  /* Add to ring buffer */
  node->invalid_blocks[node->invalid_block_write_idx] = *hash;
  node->invalid_block_write_idx =
      (node->invalid_block_write_idx + 1) % NODE_MAX_INVALID_BLOCKS;

  if (node->invalid_block_count < NODE_MAX_INVALID_BLOCKS) {
    node->invalid_block_count++;
  }
}

/**
 * Send getheaders message to peer.
 */
static void sync_cb_send_getheaders(peer_t *peer, const hash256_t *locator,
                                    size_t locator_len,
                                    const hash256_t *stop_hash, void *ctx) {
  (void)ctx; /* Node context not needed for simple message send */

  if (peer == NULL || !peer_is_ready(peer)) {
    return;
  }

  /* Build getheaders message */
  msg_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = MSG_GETHEADERS;
  msg.payload.getheaders.version = ECHO_PROTOCOL_VERSION;
  msg.payload.getheaders.hash_count = locator_len;

  /* Allocate and copy locator - will be freed by peer after sending */
  if (locator_len > 0 && locator != NULL) {
    msg.payload.getheaders.block_locator =
        malloc(locator_len * sizeof(hash256_t));
    if (msg.payload.getheaders.block_locator == NULL) {
      log_error(LOG_COMP_SYNC, "Failed to allocate block locator");
      return;
    }
    memcpy(msg.payload.getheaders.block_locator, locator,
           locator_len * sizeof(hash256_t));
  } else {
    msg.payload.getheaders.block_locator = NULL;
  }

  /* Set stop hash (all zeros means "give me as many as you can") */
  if (stop_hash != NULL) {
    msg.payload.getheaders.hash_stop = *stop_hash;
  } else {
    memset(&msg.payload.getheaders.hash_stop, 0, sizeof(hash256_t));
  }

  peer_queue_message(peer, &msg);

  log_info(LOG_COMP_SYNC, "Sent getheaders with %zu locator hashes to peer",
           locator_len);
}

/**
 * Send getdata message for blocks to peer.
 */
static void sync_cb_send_getdata_blocks(peer_t *peer, const hash256_t *hashes,
                                        size_t count, void *ctx) {
  (void)ctx; /* Node context not needed for simple message send */

  if (peer == NULL || !peer_is_ready(peer) || hashes == NULL || count == 0) {
    return;
  }

  /* Allocate inventory vectors */
  inv_vector_t *inventory = malloc(count * sizeof(inv_vector_t));
  if (inventory == NULL) {
    log_error(LOG_COMP_SYNC, "Failed to allocate getdata inventory");
    return;
  }

  /* Build inventory - request regular blocks
   * TODO: Add NODE_WITNESS to services and use INV_WITNESS_BLOCK for SegWit */
  for (size_t i = 0; i < count; i++) {
    inventory[i].type = INV_BLOCK;
    inventory[i].hash = hashes[i];
  }

  /* Build getdata message */
  msg_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = MSG_GETDATA;
  msg.payload.getdata.count = count;
  msg.payload.getdata.inventory = inventory;

  peer_queue_message(peer, &msg);

  log_debug(LOG_COMP_SYNC, "Sent getdata for %zu blocks to peer", count);
}

/**
 * Get block hash at height from the database.
 *
 * Used for efficient block queueing - avoids walking back through
 * prev pointers when there's a large height gap between tip and target.
 */
static echo_result_t sync_cb_get_block_hash_at_height(uint32_t height,
                                                       hash256_t *hash,
                                                       void *ctx) {
  node_t *node = (node_t *)ctx;
  if (node == NULL || hash == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  block_index_entry_t entry;
  /* Use lookup_by_height (not get_chain_block) because we need to query
   * headers that haven't been validated yet. get_chain_block only returns
   * blocks with BLOCK_STATUS_VALID_CHAIN, which is 0 during IBD. */
  echo_result_t result =
      block_index_db_lookup_by_height(&node->block_index_db, height, &entry);
  if (result != ECHO_OK) {
    return result;
  }

  *hash = entry.hash;
  return ECHO_OK;
}

/**
 * Begin header batch transaction for performance.
 *
 * Batching header inserts in a single transaction is ~100x faster
 * than individual auto-commit inserts.
 */
static void sync_cb_begin_header_batch(void *ctx) {
  node_t *node = (node_t *)ctx;
  if (node == NULL || !node->block_index_db_open) {
    return;
  }
  block_index_db_begin(&node->block_index_db);
}

/**
 * Commit header batch transaction.
 */
static void sync_cb_commit_header_batch(void *ctx) {
  node_t *node = (node_t *)ctx;
  if (node == NULL || !node->block_index_db_open) {
    return;
  }
  block_index_db_commit(&node->block_index_db);
}

/**
 * Disconnect a stalled/misbehaving peer.
 * Uses PEER_DISCONNECT_STALLED to ensure proper cooldown.
 */
static void sync_cb_disconnect_peer(peer_t *peer, const char *reason,
                                    void *ctx) {
  node_t *node = (node_t *)ctx;
  if (node == NULL || peer == NULL) {
    return;
  }

  log_info(LOG_COMP_NET, "Disconnecting stalled peer %s: %s", peer->address,
           reason);
  node_disconnect_peer(node, peer, PEER_DISCONNECT_STALLED);
}

/**
 * Get current block storage size in bytes.
 * Used by sync manager for pruning pressure detection.
 */
static uint64_t sync_cb_get_storage_size(void *ctx) {
  node_t *node = (node_t *)ctx;
  return node_get_block_storage_size(node);
}

/**
 * Initialize sync manager with callbacks.
 */
static echo_result_t node_init_sync(node_t *node) {
  if (node == NULL || node->consensus == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Get chainstate from consensus engine */
  chainstate_t *chainstate = consensus_get_chainstate(node->consensus);
  if (chainstate == NULL) {
    log_error(LOG_COMP_MAIN, "Failed to get chainstate from consensus engine");
    return ECHO_ERR_INVALID;
  }

  /* Set up sync callbacks.
   * Note: flush_headers is NULL because the sync manager now handles
   * deferred header persistence internally using a pending headers queue. */
  sync_callbacks_t callbacks = {
      .get_block = sync_cb_get_block,
      .store_block = sync_cb_store_block,
      .validate_header = sync_cb_validate_header,
      .store_header = sync_cb_store_header,
      .send_getheaders = sync_cb_send_getheaders,
      .send_getdata_blocks = sync_cb_send_getdata_blocks,
      .get_block_hash_at_height = sync_cb_get_block_hash_at_height,
      .begin_header_batch = sync_cb_begin_header_batch,
      .commit_header_batch = sync_cb_commit_header_batch,
      .flush_headers = NULL,
      .disconnect_peer = sync_cb_disconnect_peer,
      .get_storage_size = sync_cb_get_storage_size,
      .ctx = node,
      .node = node};

  /* Create sync manager */
  node->sync_mgr = sync_create(chainstate, &callbacks);
  if (node->sync_mgr == NULL) {
    log_error(LOG_COMP_MAIN, "Failed to create sync manager");
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /* Configure prune target for decoupled IBD storage pressure detection */
  if (node->config.prune_target_mb > 0) {
    uint64_t prune_target_bytes = node->config.prune_target_mb * 1024ULL * 1024ULL;
    sync_set_prune_target(node->sync_mgr, prune_target_bytes);
  }

  /* Initialize invalid block tracking */
  node->invalid_block_count = 0;
  node->invalid_block_write_idx = 0;
  memset(node->invalid_blocks, 0, sizeof(node->invalid_blocks));

  log_info(LOG_COMP_MAIN, "Sync manager initialized with block pipeline callbacks");

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
   * Start sequence:
   * 1. Create and start listening socket on configured port
   * 2. Start connection manager thread
   * 3. Connect to outbound peers
   * 4. Create and start sync manager for initial block download
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
  log_info(LOG_COMP_MAIN, "Node shutdown initiated...");

  /*
   * Shutdown sequence:
   * 1. Flush UTXO set to database (IBD mode only)
   * 2. Persist validated tip height
   * 3. Stop accepting new connections
   * 4. Disconnect all peers
   * 5. Databases will be closed in node_destroy()
   */

  /* Step 1: Flush UTXO set during IBD mode */
  if (node->ibd_mode && node->consensus != NULL) {
    log_info(LOG_COMP_MAIN, "Flushing UTXO set to database...");
    echo_result_t flush_result = node_flush_utxo_shutdown(node);
    if (flush_result != ECHO_OK) {
      log_error(LOG_COMP_MAIN, "Failed to flush UTXO set: %d", flush_result);
      /* Continue shutdown even if flush fails */
    }
  }

  /* Step 2: Persist validated tip */
  if (node->block_index_db_open && node->consensus != NULL) {
    chainstate_t *cs = consensus_get_chainstate(node->consensus);
    if (cs != NULL) {
      uint32_t validated_height = chainstate_get_height(cs);
      block_index_db_set_validated_tip(&node->block_index_db, validated_height,
                                       NULL);
      log_info(LOG_COMP_MAIN, "Persisted validated tip: height=%u",
               validated_height);
    }
  }

  /* Step 3: Stop listening socket */
  if (node->is_listening && node->listen_socket != NULL) {
    plat_socket_close(node->listen_socket);
    plat_socket_free(node->listen_socket);
    node->listen_socket = NULL;
    node->is_listening = false;
  }

  /* Step 4: Disconnect all peers */
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    if (peer_is_connected(&node->peers[i])) {
      peer_disconnect(&node->peers[i], PEER_DISCONNECT_USER, "Node shutdown");
    }
  }

  log_info(LOG_COMP_MAIN, "Node shutdown complete");
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
  /* Destroy sync manager */
  if (node->sync_mgr != NULL) {
    sync_destroy(node->sync_mgr);
    node->sync_mgr = NULL;
  }

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

  /* Close block storage */
  if (node->block_storage_init) {
    block_storage_destroy(&node->block_storage);
    node->block_storage_init = false;
  }

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
  stats->peer_count = node_get_peer_count(node);

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
  stats->start_height = node->start_height;
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

block_storage_t *node_get_block_storage(node_t *node) {
  if (node == NULL || !node->block_storage_init) {
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

  /* Count actual connected peers instead of using cached value
   * This ensures accuracy even if peers disconnect asynchronously */
  size_t count = 0;
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    if (peer_is_connected(&node->peers[i])) {
      count++;
    }
  }
  return count;
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

  /* Track message types (all modes) */
  {
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
    if (peer_is_ready(peer)) {
      /* Request addresses from peer FIRST - even pruned peers can give us
       * addresses. This is critical for maintaining peer diversity. */
      msg_t getaddr;
      memset(&getaddr, 0, sizeof(getaddr));
      getaddr.type = MSG_GETADDR;
      peer_queue_message(peer, &getaddr);
      log_debug(LOG_COMP_NET, "Sent GETADDR to peer %s", peer->address);

      /*
       * Keep pruned peers connected but don't use them for block downloads
       * during IBD. Pruned peers are still useful for:
       *   - Headers sync (they have headers)
       *   - Transaction relay (after IBD)
       *   - Address discovery (getaddr)
       *
       * Per BIP-159:
       *   - Pruned nodes: MUST NOT set NODE_NETWORK, only NODE_NETWORK_LIMITED
       *   - Full archival nodes: Set NODE_NETWORK, MAY also set NODE_NETWORK_LIMITED
       *
       * Only add NODE_NETWORK peers to sync manager for block downloads.
       */
      bool is_full_node = (peer->services & SERVICE_NODE_NETWORK) != 0;

      /* Add only full nodes to sync manager for block downloads */
      if (node->sync_mgr != NULL && is_full_node) {
        sync_add_peer(node->sync_mgr, peer, peer->start_height);
      } else if (!is_full_node) {
        log_debug(LOG_COMP_NET,
                  "Keeping pruned peer %s connected (services=0x%llx) - "
                  "useful for headers/relay, just not for IBD blocks",
                  peer->address, (unsigned long long)peer->services);
      }
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
    /* Calculate RTT if this pong matches our outstanding ping */
    if (peer->ping_nonce != 0 &&
        msg->payload.pong.nonce == peer->ping_nonce) {
      uint64_t now = plat_time_ms();
      peer->last_rtt_ms = now - peer->ping_sent_time;
      peer->ping_nonce = 0; /* Clear - ping answered */
      log_debug(LOG_COMP_NET, "Peer %s RTT: %llu ms",
                peer->address, (unsigned long long)peer->last_rtt_ms);
    }
    break;

  case MSG_ADDR:
    /* Update address manager with new addresses */
    if (msg->payload.addr.count > 0 && msg->payload.addr.addresses != NULL) {
      size_t before = discovery_get_address_count(&node->addr_manager);
      size_t added = discovery_add_addresses(&node->addr_manager,
                              msg->payload.addr.addresses,
                              msg->payload.addr.count);
      log_info(LOG_COMP_NET, "Got %zu addr from %s, added %zu (total: %zu)",
               (size_t)msg->payload.addr.count, peer->address, added,
               discovery_get_address_count(&node->addr_manager));
      (void)before; /* Suppress unused warning */
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
    /* Forward to sync manager (even with 0 headers to trigger mode transition) */
    log_info(LOG_COMP_SYNC, "Received MSG_HEADERS with %zu headers",
             msg->payload.headers.count);
    if (node->sync_mgr != NULL) {
      echo_result_t hdr_result = sync_handle_headers(
          node->sync_mgr, peer, msg->payload.headers.headers,
          msg->payload.headers.count);
      if (hdr_result != ECHO_OK) {
        log_warn(LOG_COMP_SYNC, "sync_handle_headers returned: %d", hdr_result);
      }
    }
    /* Free the allocated headers array */
    if (msg->payload.headers.headers != NULL) {
      free(msg->payload.headers.headers);
    }
    break;

  case MSG_BLOCK:
    /* Forward to sync manager */
    if (node->sync_mgr != NULL) {
      sync_handle_block(node->sync_mgr, peer, &msg->payload.block.block);
    }
    /* Free block data to prevent memory leak - each block contains
     * dynamically allocated transactions with inputs/outputs/scripts.
     * Cast away const: we own this memory (parser allocated it) and are done. */
    block_free((block_t *)&msg->payload.block.block);
    break;

  case MSG_TX:
    /*
     * IBD Optimization: During initial block download, drop all incoming
     * transactions. They waste CPU (validation), memory (mempool), and
     * will be in the blocks we download anyway.
     */
    if (node->ibd_mode) {
      tx_free((tx_t *)&msg->payload.tx.tx);
      break; /* Silently drop transactions during IBD */
    }

    /* Validate and accept transaction into mempool */
    if (node->mempool != NULL && !node->config.observer_mode) {
      mempool_accept_result_t result;
      echo_result_t tx_res =
          node_accept_transaction(node, &msg->payload.tx.tx, &result);
      if (tx_res != ECHO_OK && result.reason != MEMPOOL_REJECT_DUPLICATE) {
        /* Transaction rejected - may want to penalize peer for bad tx */
        log_debug(LOG_COMP_POOL, "Rejected transaction from peer: %s",
                  mempool_reject_string(result.reason));
      }
    }
    /* Free transaction data - mempool clones if accepted.
     * Cast away const: we own this memory (parser allocated it) and are done. */
    tx_free((tx_t *)&msg->payload.tx.tx);
    break;

  case MSG_INV:
    /* Inventory announcement - request interesting items */
    if (msg->payload.inv.count > 0 && msg->payload.inv.inventory != NULL) {
      /* Track block and transaction announcements (all modes) */
      for (size_t i = 0; i < msg->payload.inv.count; i++) {
        const inv_vector_t *inv = &msg->payload.inv.inventory[i];
        if (inv->type == INV_BLOCK || inv->type == INV_WITNESS_BLOCK) {
          node_observe_block(node, &inv->hash);
        } else if (inv->type == INV_TX || inv->type == INV_WITNESS_TX) {
          node_observe_tx(node, &inv->hash);
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

          /*
           * IBD Optimization (Phase 1): During initial block download, skip
           * transaction requests entirely. Every byte of bandwidth should go
           * to block downloads. Transactions will be in the blocks we download.
           */
          if (node->ibd_mode &&
              (inv->type == INV_TX || inv->type == INV_WITNESS_TX)) {
            continue; /* Skip transactions during IBD */
          }

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
          /*
           * Block request handling (Pruning): If pruning is enabled and
           * we've pruned this block, send NOTFOUND.
           */
          if (node_is_pruning_enabled(node) && node->block_index_db_open) {
            bool is_pruned = false;
            echo_result_t prune_result = block_index_db_is_pruned(
                &node->block_index_db, &inv->hash, &is_pruned);

            if (prune_result == ECHO_OK && is_pruned) {
              /* Block is pruned - send NOTFOUND */
              log_debug(LOG_COMP_NET, "Requested block is pruned, sending notfound");
              inv_vector_t notfound_inv = *inv; /* Copy to avoid const issues */
              msg_t notfound_msg;
              memset(&notfound_msg, 0, sizeof(notfound_msg));
              notfound_msg.type = MSG_NOTFOUND;
              notfound_msg.payload.notfound.count = 1;
              notfound_msg.payload.notfound.inventory = &notfound_inv;
              peer_queue_message(peer, &notfound_msg);
            }
            /* If block is not pruned, we could serve it */
          }
          /* TODO: Full block serving */
        } else if (inv->type == INV_TX || inv->type == INV_WITNESS_TX) {
          /*
           * IBD Optimization: During initial block download, don't serve
           * transactions to peers. Our mempool is empty anyway (we're
           * dropping incoming txs), and serving wastes bandwidth.
           */
          if (node->ibd_mode) {
            continue; /* Skip transaction requests during IBD */
          }

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

  case MSG_GETHEADERS: {
    /* Peer requesting headers from us */
    if (node->consensus == NULL || !node->block_index_db_open) {
      break;
    }

    /* Find common point using block locator */
    chainstate_t *chainstate = consensus_get_chainstate(node->consensus);
    const msg_getheaders_t *req = &msg->payload.getheaders;
    uint32_t start_height = 0;

    if (req->hash_count > 0 && req->block_locator != NULL) {
      block_index_t *fork_point =
          sync_find_locator_fork(chainstate, req->block_locator, req->hash_count);
      if (fork_point != NULL) {
        start_height = fork_point->height;
      }
    }

    /* Get our current chain tip height */
    uint32_t tip_height = consensus_get_height(node->consensus);
    if (start_height >= tip_height) {
      break; /* No headers to send */
    }

    /* Collect headers to send (up to 2000) */
    #define MAX_HEADERS_TO_SEND 2000
    block_header_t *headers = malloc(MAX_HEADERS_TO_SEND * sizeof(block_header_t));
    if (headers == NULL) {
      break;
    }

    size_t header_count = 0;
    static const hash256_t zero_hash = {{0}};
    block_index_db_t *bdb = &node->block_index_db;

    /* Query headers by height from database */
    for (uint32_t h = start_height + 1;
         h <= tip_height && header_count < MAX_HEADERS_TO_SEND; h++) {
      block_index_entry_t entry;
      if (block_index_db_get_chain_block(bdb, h, &entry) != ECHO_OK) {
        break;
      }

      /* Stop if we've reached the stop hash (if specified) */
      if (memcmp(&req->hash_stop, &zero_hash, sizeof(hash256_t)) != 0 &&
          memcmp(&entry.hash, &req->hash_stop, sizeof(hash256_t)) == 0) {
        break;
      }

      headers[header_count++] = entry.header;
    }

    /* Send headers response */
    if (header_count > 0) {
      msg_t response;
      memset(&response, 0, sizeof(response));
      response.type = MSG_HEADERS;
      response.payload.headers.count = header_count;
      response.payload.headers.headers = headers;
      peer_queue_message(peer, &response);

      log_debug(LOG_COMP_SYNC, "Sent %zu headers to peer (heights %u-%u)",
                header_count, start_height + 1, start_height + (uint32_t)header_count);
    } else {
      free(headers);
    }
    #undef MAX_HEADERS_TO_SEND
    break;
  }

  case MSG_GETBLOCKS:
    /* Peer requesting block inventory - not yet implemented */
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
      if (peer->state == PEER_STATE_DISCONNECTED) {
        /* Try to accept connection (non-blocking) */
        uint64_t nonce = generate_nonce();
        echo_result_t result = peer_accept(peer, node->listen_socket, nonce);
        if (result == ECHO_OK) {
          /* Send version message to start handshake */
          uint32_t our_height = 0;
          if (node->consensus != NULL) {
            our_height = consensus_get_height(node->consensus);
          }

          /* Service flags: NODE_NETWORK (1) only if we're not pruned
           * Pruned nodes cannot serve historical blocks */
          uint64_t services = node_is_pruning_enabled(node) ? 0 : 1;
          peer_send_version(peer, services, (int32_t)our_height, true);
        }
        break; /* Only accept one per loop iteration */
      }
    }
  }

  /* Step 1.5: Check pending async connections */
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    peer_t *peer = &node->peers[i];
    if (peer->state == PEER_STATE_CONNECTING) {
      echo_result_t result = peer_check_connect(peer);
      if (result == ECHO_OK) {
        /* Connection completed! Send version to start handshake */
        log_info(LOG_COMP_NET, "Async connect completed to %s:%u",
                 peer->address, peer->port);

        uint32_t our_height = 0;
        if (node->consensus != NULL) {
          our_height = consensus_get_height(node->consensus);
        }
        uint64_t services = node_is_pruning_enabled(node) ? 0 : 1;
        peer_send_version(peer, services, (int32_t)our_height, true);
      } else if (result != ECHO_SUCCESS) {
        /* Connection failed - release address back to pool */
        log_debug(LOG_COMP_NET, "Async connect failed to %s:%u: %d",
                  peer->address, peer->port, result);
        net_addr_t peer_addr;
        if (discovery_parse_address(peer->address, peer->port, &peer_addr) ==
            ECHO_OK) {
          discovery_mark_address_free(&node->addr_manager, &peer_addr,
                                      ECHO_FALSE);
        }
        /* peer_check_connect already cleaned up the peer struct */
      }
      /* result == ECHO_SUCCESS means still connecting, check next time */
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
   * The sync manager does the heavy lifting.
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
    log_info(LOG_COMP_NET, "Maintenance tick: peer_count=%zu", node_get_peer_count(node));
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
      /* Track ping for RTT measurement */
      peer->ping_nonce = ping.payload.ping.nonce;
      peer->ping_sent_time = now;
      peer_queue_message(peer, &ping);
    }
  }

  /* Task 2: Tick sync manager for timeout processing and retries */
  if (node->sync_mgr != NULL && !node->config.observer_mode) {
    /* Start sync if not already syncing and not fully synced */
    if (!sync_is_ibd(node->sync_mgr) && !sync_is_complete(node->sync_mgr)) {
      echo_result_t start_result = sync_start(node->sync_mgr);
      if (start_result == ECHO_OK) {
        log_info(LOG_COMP_SYNC, "Starting headers-first sync");
      } else {
        static uint64_t last_sync_fail_log = 0;
        if (now - last_sync_fail_log > 10000) {
          sync_progress_t prog;
          sync_get_progress(node->sync_mgr, &prog);
          log_info(LOG_COMP_SYNC,
                   "sync_start failed: %d (mode=%s, sync_peers=%zu)",
                   start_result, sync_mode_string(prog.mode), prog.sync_peers);
          last_sync_fail_log = now;
        }
      }
    }
    sync_tick(node->sync_mgr);

    /*
     * IBD Completion Detection: When sync transitions from HEADERS/BLOCKS
     * mode to DONE, we set ibd_mode = false to enable transaction
     * processing and switch database to normal mode (safer, slightly
     * slower). After this, the node participates in mempool traffic
     * normally. UTXO persistence continues with the existing batching
     * logic, just with synchronous writes for safety.
     */
    if (node->ibd_mode && sync_is_complete(node->sync_mgr)) {
      node->ibd_mode = false;

      log_info(LOG_COMP_SYNC,
               "IBD complete! Transitioning to normal operation. "
               "Mempool and transaction relay now active.");

      /* Switch databases to normal mode (sync writes for safety) */
      if (node->utxo_db_open) {
        db_set_ibd_mode(&node->utxo_db.db, false);
      }
      if (node->block_index_db_open) {
        db_set_ibd_mode(&node->block_index_db.db, false);
      }
      log_info(LOG_COMP_SYNC,
               "Databases switched to NORMAL mode (synchronous writes)");
    }
  }

  /* Task 3: Evict stale mempool transactions (future session) */
  /* Mempool maintenance will be added when mempool_tick() is implemented */

  /*
   * Task 4: Cleanup disconnected peers
   *
   * IMPORTANT: This must run BEFORE attempting new connections to avoid
   * a race condition where:
   * 1. Peer A disconnects, slot marked DISCONNECTED
   * 2. Task 5 (old order) would run new connection, reusing slot for Peer B
   * 3. Sync manager still has stale reference to Peer A via this slot
   * 4. Sync manager accesses Peer B's data thinking it's Peer A = crash
   *
   * By cleaning up first, we ensure sync_remove_peer is called before
   * the slot can be reused.
   */
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    peer_t *peer = &node->peers[i];
    /*
     * Check for disconnected peers that need cleanup.
     * peer->address[0] != '\0' indicates this slot was previously used.
     * The socket may already be NULL (cleaned up by peer_disconnect).
     */
    if (peer->state == PEER_STATE_DISCONNECTED && peer->address[0] != '\0') {
      /* Remove from sync manager before cleaning up */
      if (node->sync_mgr != NULL) {
        sync_remove_peer(node->sync_mgr, peer);
      }

      /* Release address back to discovery pool (for outbound peers only) */
      if (!peer->inbound) {
        net_addr_t peer_addr;
        if (discovery_parse_address(peer->address, peer->port, &peer_addr) ==
            ECHO_OK) {
          /*
           * Determine success based on disconnect reason:
           * - STALLED, MISBEHAVING, PROTOCOL_ERROR = always failure (ignore duration)
           * - USER disconnect or long-lived connection = success
           * - Short-lived connections with other reasons = failure
           */
          echo_bool_t was_success = ECHO_FALSE;
          uint64_t connection_duration_ms =
              plat_time_ms() - peer->connect_time;

          /* Explicit failures - never mark as success regardless of duration */
          if (peer->disconnect_reason == PEER_DISCONNECT_STALLED ||
              peer->disconnect_reason == PEER_DISCONNECT_MISBEHAVING ||
              peer->disconnect_reason == PEER_DISCONNECT_PROTOCOL_ERROR) {
            was_success = ECHO_FALSE;
          } else if (peer->disconnect_reason == PEER_DISCONNECT_USER) {
            was_success = ECHO_TRUE;
          } else if (connection_duration_ms > 300000) {
            /* Long-lived connection with normal disconnect = success */
            was_success = ECHO_TRUE;
          }

          discovery_mark_address_free(&node->addr_manager, &peer_addr,
                                      was_success);

          if (!was_success) {
            log_debug(LOG_COMP_NET,
                      "Released failed address %s:%u (reason=%d, duration=%llu "
                      "ms)",
                      peer->address, peer->port, peer->disconnect_reason,
                      (unsigned long long)connection_duration_ms);
          }
        }
      }

      /* Re-initialize to clean state (clears address, making slot available) */
      peer_init(peer);
    }
  }

  /* Task 5: Attempt outbound connections if below target */
  size_t outbound_count = 0;
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    if (peer_is_connected(&node->peers[i]) && !node->peers[i].inbound) {
      outbound_count++;
    }
  }

  /* Target outbound peer count */
  size_t target_peers = (size_t)ECHO_MAX_OUTBOUND_PEERS;

  static uint64_t last_peer_log = 0;
  if (now - last_peer_log > 5000) { /* Log every 5 seconds */
    log_info(LOG_COMP_NET, "Outbound peers: %zu/%zu", outbound_count,
             target_peers);
    last_peer_log = now;
  }

  /*
   * Try multiple connections per cycle when below target.
   * During IBD, bandwidth is critical - fill peer slots aggressively.
   * Many peers are pruned nodes that get disconnected after handshake,
   * so we need to attempt many more connections than we need.
   *
   * With 2-second connection timeout (reduced from 5), we can try more
   * connections per cycle without blocking too long:
   * - Below 25%: try up to 40 connections (80 sec worst case)
   * - Below 50%: try up to 25 connections (50 sec worst case)
   * - Below 75%: try up to 15 connections (30 sec worst case)
   * - Below target: try up to 8 connections (16 sec worst case)
   */
  size_t max_attempts = 8; /* Default when close to target */
  if (outbound_count < target_peers / 4) {
    max_attempts = 40; /* Aggressive: only ~30-40% of network is full nodes */
  } else if (outbound_count < target_peers / 2) {
    max_attempts = 25;
  } else if (outbound_count < (target_peers * 3) / 4) {
    max_attempts = 15;
  }

  size_t attempts = 0;
  while (outbound_count + attempts < target_peers &&
         attempts < max_attempts) {
    net_addr_t addr;
    echo_result_t addr_result =
        discovery_select_outbound_address(&node->addr_manager, &addr);
    if (addr_result != ECHO_OK) {
      if (attempts == 0) {
        log_debug(LOG_COMP_NET,
                  "No addresses available for outbound connection (have %zu "
                  "peers)",
                  outbound_count);
      }
      break;
    }

    /* Mark in-use IMMEDIATELY to prevent selecting same address in next loop iteration */
    discovery_mark_address_in_use(&node->addr_manager, &addr);

    /* Find empty slot (must be DISCONNECTED, not CONNECTING) */
    bool found_slot = false;
    for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
      peer_t *peer = &node->peers[i];
      if (peer->state == PEER_STATE_DISCONNECTED) {
        found_slot = true;

        /* Convert IPv4-mapped IPv6 address to string */
        char ip_str[64];
        snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", addr.ip[12],
                 addr.ip[13], addr.ip[14], addr.ip[15]);

        log_info(LOG_COMP_NET, "Attempting outbound connection to %s:%u",
                 ip_str, addr.port);

        /* Record attempt (in_use already marked after select) */
        discovery_mark_attempt(&node->addr_manager, &addr);

        uint64_t nonce = generate_nonce();
        echo_result_t result = peer_connect(peer, ip_str, addr.port, nonce);
        if (result == ECHO_OK) {
          /* Immediate connection (rare) - send version now */
          log_info(LOG_COMP_NET, "Connected to peer %s:%u", ip_str, addr.port);

          uint32_t our_height = 0;
          if (node->consensus != NULL) {
            our_height = consensus_get_height(node->consensus);
          }
          uint64_t services = node_is_pruning_enabled(node) ? 0 : 1;
          peer_send_version(peer, services, (int32_t)our_height, true);
        } else if (result == ECHO_SUCCESS) {
          /* Async connection started - peer is in CONNECTING state.
           * node_process_peers will check completion and send version. */
          log_debug(LOG_COMP_NET, "Async connect started to %s:%u",
                    ip_str, addr.port);
        } else {
          log_warn(LOG_COMP_NET, "Failed to connect to %s:%u: error %d",
                   ip_str, addr.port, result);
          /* Release address back to pool as failed attempt */
          discovery_mark_address_free(&node->addr_manager, &addr, ECHO_FALSE);
          /* Continue trying - connection failed but slot exists */
        }
        break;
      }
    }
    attempts++;
    if (!found_slot) {
      /* No empty peer slots - stop trying until one frees up */
      discovery_mark_address_free(&node->addr_manager, &addr, ECHO_FALSE);
      break;
    }
  }

  /* Task 6: Pruning - check if we need to prune old blocks */
  if (node_is_pruning_enabled(node)) {
    static uint64_t last_prune_check = 0;
    /* Check every 10 seconds during IBD, every 60 seconds after */
    uint64_t prune_interval = node->ibd_mode ? 10000 : 60000;
    if (now - last_prune_check > prune_interval) {
      node_maybe_prune(node);
      last_prune_check = now;
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
 * BLOCK APPLICATION WITH PERSISTENCE
 * ============================================================================
 */

echo_result_t node_apply_block(node_t *node, const block_t *block) {
  if (node == NULL || block == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (node->consensus == NULL) {
    return ECHO_ERR_INVALID_STATE;
  }

  echo_result_t result;

  /* Compute block hash */
  hash256_t block_hash;
  result = block_header_hash(&block->header, &block_hash);
  if (result != ECHO_OK) {
    log_error(LOG_COMP_CONS, "Failed to compute block hash");
    return result;
  }

  /* Get block height from parent */
  uint32_t height = 0;
  const block_index_t *parent = consensus_lookup_block_index(
      node->consensus, &block->header.prev_hash);
  if (parent != NULL) {
    height = parent->height + 1;
  }

  /* Timing instrumentation for validation performance analysis */
  uint64_t t_start = plat_time_ms();

  /*
   * Step 1: Apply block to consensus engine (in-memory state).
   * This updates the UTXO set and chain tip in memory.
   */
  consensus_result_t validation_result;
  consensus_result_init(&validation_result);
  result = consensus_apply_block(node->consensus, block, &validation_result);
  uint64_t t_consensus = plat_time_ms();
  if (result != ECHO_OK) {
    log_error(LOG_COMP_CONS, "Failed to apply block to consensus engine: %d",
              result);
    return result;
  }

  /*
   * Step 2: Update block index status (block already stored by node_store_block).
   * Just update the validation status flags - no duplicate storage.
   */
  if (node->block_index_db_open) {
    uint32_t new_status = BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_VALID_TREE |
                          BLOCK_STATUS_VALID_SCRIPTS | BLOCK_STATUS_VALID_CHAIN |
                          BLOCK_STATUS_HAVE_DATA;
    result = block_index_db_update_status(&node->block_index_db, &block_hash,
                                          new_status);
    if (result != ECHO_OK && result != ECHO_ERR_NOT_FOUND) {
      log_debug(LOG_COMP_DB, "Failed to update block status: %d", result);
      /* Continue anyway - block is validated in memory */
    }
  }

  /*
   * Step 4: Update UTXO database atomically.
   * Collect new UTXOs (outputs) and spent UTXOs (inputs).
   */
  if (node->utxo_db_open) {
    /* Count new outputs and spent inputs */
    size_t new_count = 0;
    size_t spent_count = 0;

    for (size_t i = 0; i < block->tx_count; i++) {
      new_count += block->txs[i].output_count;
      if (!tx_is_coinbase(&block->txs[i])) {
        spent_count += block->txs[i].input_count;
      }
    }

    /* Allocate arrays for batch operations */
    const utxo_entry_t **new_utxos = NULL;
    utxo_entry_t *new_entries = NULL;
    outpoint_t *spent_outpoints = NULL;

    if (new_count > 0) {
      new_utxos = malloc(new_count * sizeof(utxo_entry_t *));
      new_entries = malloc(new_count * sizeof(utxo_entry_t));
      if (new_utxos == NULL || new_entries == NULL) {
        free(new_utxos);
        free(new_entries);
        log_error(LOG_COMP_DB, "Failed to allocate UTXO arrays");
        return ECHO_ERR_OUT_OF_MEMORY;
      }
    }

    if (spent_count > 0) {
      spent_outpoints = malloc(spent_count * sizeof(outpoint_t));
      if (spent_outpoints == NULL) {
        free(new_utxos);
        free(new_entries);
        log_error(LOG_COMP_DB, "Failed to allocate spent outpoints array");
        return ECHO_ERR_OUT_OF_MEMORY;
      }
    }

    /* Populate new UTXOs from transaction outputs */
    size_t new_idx = 0;
    for (size_t i = 0; i < block->tx_count; i++) {
      const tx_t *tx = &block->txs[i];
      hash256_t txid;
      tx_compute_txid(tx, &txid);

      for (size_t j = 0; j < tx->output_count; j++) {
        utxo_entry_t *entry = &new_entries[new_idx];

        entry->outpoint.txid = txid;
        entry->outpoint.vout = (uint32_t)j;
        entry->value = tx->outputs[j].value;
        entry->script_pubkey = tx->outputs[j].script_pubkey;
        entry->script_len = tx->outputs[j].script_pubkey_len;
        entry->height = height;
        entry->is_coinbase = tx_is_coinbase(tx);

        new_utxos[new_idx] = entry;
        new_idx++;
      }
    }

    /* Populate spent outpoints from transaction inputs */
    size_t spent_idx = 0;
    for (size_t i = 0; i < block->tx_count; i++) {
      const tx_t *tx = &block->txs[i];
      if (tx_is_coinbase(tx)) {
        continue;
      }
      for (size_t j = 0; j < tx->input_count; j++) {
        spent_outpoints[spent_idx] = tx->inputs[j].prevout;
        spent_idx++;
      }
    }

    /*
     * UTXO Persistence Strategy:
     *
     * NORMAL MODE: Persist every block to SQLite (real-time durability)
     *
     * IBD MODE: Skip all per-block persistence. The in-memory UTXO set is
     * the source of truth. Persistence only happens at clean shutdown.
     * This eliminates ALL SQLite I/O during IBD for maximum sync speed.
     */
    if (!node->ibd_mode) {
      /* Normal operation: persist every block */
      result = utxo_db_apply_block(&node->utxo_db, new_utxos, new_count,
                                   spent_outpoints, spent_count);
      if (result != ECHO_OK) {
        log_error(LOG_COMP_DB, "Failed to apply block to UTXO database: %d",
                  result);
      }
    }
    /* IBD mode: no persistence - flush happens at shutdown only */

    free(new_utxos);
    free(new_entries);
    free(spent_outpoints);
  }

  uint64_t t_end = plat_time_ms();
  uint64_t consensus_ms = t_consensus - t_start;
  uint64_t total_ms = t_end - t_start;

  /* Log timing every 1000 blocks or if validation took >100ms */
  if (height % 1000 == 0 || total_ms > 100) {
    log_info(LOG_COMP_CONS,
             "Block applied: height=%u txs=%zu time=%llums (consensus=%llums)",
             height, block->tx_count, (unsigned long long)total_ms,
             (unsigned long long)consensus_ms);
  }

  return ECHO_OK;
}

/*
 * ============================================================================
 * UTXO SHUTDOWN FLUSH
 * ============================================================================
 *
 * At clean shutdown, rebuild the UTXO database from the in-memory set.
 * This is the ONLY time we persist UTXOs during IBD - no periodic checkpoints.
 *
 * Strategy: DELETE all from SQLite, then INSERT all from memory.
 * This is simpler and more reliable than delta-tracking.
 */

/* Context for UTXO flush callback */
typedef struct {
  utxo_db_t *udb;
  size_t inserted;
  size_t total;
  size_t last_logged;
  echo_result_t result;
} utxo_flush_ctx_t;

/* Progress logging interval: every 5 million UTXOs */
#define UTXO_FLUSH_LOG_INTERVAL 5000000

/* Callback to insert each UTXO into the database */
static bool utxo_flush_callback(const utxo_entry_t *entry, void *user_data) {
  utxo_flush_ctx_t *ctx = (utxo_flush_ctx_t *)user_data;
  if (ctx == NULL || entry == NULL) {
    return false;
  }

  echo_result_t result = utxo_db_insert(ctx->udb, entry);
  if (result == ECHO_OK || result == ECHO_ERR_EXISTS) {
    ctx->inserted++;

    /* Log progress at regular intervals */
    if (ctx->inserted - ctx->last_logged >= UTXO_FLUSH_LOG_INTERVAL) {
      double pct = ctx->total > 0 ? (100.0 * (double)ctx->inserted / (double)ctx->total) : 0.0;
      log_info(LOG_COMP_DB, "UTXO flush progress: %zu/%zu (%.1f%%)",
               ctx->inserted, ctx->total, pct);
      ctx->last_logged = ctx->inserted;
    }

    return true;
  }

  ctx->result = result;
  return false; /* Stop iteration on error */
}

/**
 * Flush entire in-memory UTXO set to database (shutdown only).
 *
 * Called from node_stop() during clean shutdown to persist all UTXOs.
 * Replaces the entire database contents with the current in-memory state.
 */
static echo_result_t node_flush_utxo_shutdown(node_t *node) {
  if (node == NULL || node->consensus == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Track flush timing */
  uint64_t flush_start = plat_monotonic_ms();

  /* Get the chainstate UTXO set */
  chainstate_t *chainstate = consensus_get_chainstate(node->consensus);
  if (chainstate == NULL) {
    return ECHO_ERR_INVALID;
  }

  const utxo_set_t *utxo_set = chainstate_get_utxo_set(chainstate);
  if (utxo_set == NULL) {
    return ECHO_ERR_INVALID;
  }

  size_t utxo_count = utxo_set_size(utxo_set);
  log_info(LOG_COMP_DB, "Shutdown UTXO flush: persisting %zu UTXOs...",
           utxo_count);

  /* Begin transaction for atomic flush */
  echo_result_t result = db_begin(&node->utxo_db.db);
  if (result != ECHO_OK) {
    log_error(LOG_COMP_DB, "Failed to begin UTXO flush transaction: %d",
              result);
    return result;
  }

  /* Step 1: Clear existing UTXOs (fresh start) */
  result = utxo_db_clear(&node->utxo_db);
  if (result != ECHO_OK) {
    log_error(LOG_COMP_DB, "Failed to clear UTXO database: %d", result);
    (void)db_rollback(&node->utxo_db.db);
    return result;
  }

  /* Step 2: Insert all UTXOs from memory */
  utxo_flush_ctx_t ctx = {
      .udb = &node->utxo_db,
      .inserted = 0,
      .total = utxo_count,
      .last_logged = 0,
      .result = ECHO_OK,
  };

  if (utxo_count > 0) {
    utxo_set_foreach(utxo_set, utxo_flush_callback, &ctx);
  }

  if (ctx.result != ECHO_OK) {
    log_error(LOG_COMP_DB, "UTXO flush failed: %d", ctx.result);
    (void)db_rollback(&node->utxo_db.db);
    return ctx.result;
  }

  /* Commit transaction */
  result = db_commit(&node->utxo_db.db);
  if (result != ECHO_OK) {
    log_error(LOG_COMP_DB, "Failed to commit UTXO flush transaction: %d",
              result);
    return result;
  }

  /* Log flush timing */
  uint64_t flush_elapsed = plat_monotonic_ms() - flush_start;
  double rate = flush_elapsed > 0 ? (double)ctx.inserted / flush_elapsed : 0.0;
  log_info(LOG_COMP_DB,
           "Shutdown UTXO flush complete in %lums: %zu inserted (%.1f/ms)",
           (unsigned long)flush_elapsed, ctx.inserted, rate);

  return ECHO_OK;
}

/*
 * ============================================================================
 * OBSERVER MODE FUNCTIONS
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

  /* Observer stats are tracked in all modes */
  memcpy(stats, &node->observer_stats, sizeof(*stats));
}

void node_observe_block(node_t *node, const hash256_t *hash) {
  if (node == NULL || hash == NULL) {
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
  if (node == NULL || txid == NULL) {
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
  if (node == NULL || command == NULL) {
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

/*
 * ============================================================================
 * BLOCK PIPELINE PUBLIC API
 * ============================================================================
 */

bool node_is_block_invalid(const node_t *node, const hash256_t *hash) {
  if (node == NULL || hash == NULL) {
    return false;
  }

  /* In observer mode, no blocks are tracked as invalid */
  if (node->config.observer_mode) {
    return false;
  }

  /* Search the invalid blocks ring buffer */
  for (size_t i = 0; i < node->invalid_block_count; i++) {
    if (memcmp(&node->invalid_blocks[i], hash, sizeof(hash256_t)) == 0) {
      return true;
    }
  }
  return false;
}

size_t node_get_invalid_block_count(const node_t *node) {
  if (node == NULL || node->config.observer_mode) {
    return 0;
  }
  return node->invalid_block_count;
}

echo_result_t node_process_received_block(node_t *node, const block_t *block) {
  if (node == NULL || block == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Observer mode doesn't process blocks */
  if (node->config.observer_mode) {
    return ECHO_ERR_INVALID_STATE;
  }

  /* Compute block hash */
  hash256_t block_hash;
  echo_result_t result = block_header_hash(&block->header, &block_hash);
  if (result != ECHO_OK) {
    return result;
  }

  /* Check if already known (on main chain) */
  const block_index_t *existing = consensus_lookup_block_index(node->consensus,
                                                                &block_hash);
  if (existing != NULL && existing->on_main_chain) {
    return ECHO_ERR_EXISTS;
  }

  /* Check if known invalid */
  if (node_is_block_invalid(node, &block_hash)) {
    return ECHO_ERR_INVALID;
  }

  /*
   * Check for orphan blocks if header not already known.
   * Blocks are stored and validated later by the sync pipeline.
   */
  if (existing == NULL) {
    /* Look up parent to verify block is not orphan */
    const block_index_t *parent = consensus_lookup_block_index(
        node->consensus, &block->header.prev_hash);
    if (parent == NULL) {
      /* Parent unknown - orphan block, can't process yet */
      log_debug(LOG_COMP_CONS, "Orphan block received (parent unknown)");
      return ECHO_ERR_NOT_FOUND;
    }
  }

  /* Store block to disk */
  result = node_store_block(node, block);
  if (result != ECHO_OK && result != ECHO_ERR_EXISTS) {
    log_error(LOG_COMP_STORE, "Failed to store received block: %d", result);
    return result;
  }

  return ECHO_OK;
}

/*
 * ============================================================================
 * PRUNING SUPPORT
 * ============================================================================
 */

bool node_is_pruning_enabled(const node_t *node) {
  if (node == NULL) {
    return false;
  }
  return node->config.prune_target_mb > 0;
}

bool node_is_ibd_mode(const node_t *node) {
  if (node == NULL) {
    return false;
  }
  return node->ibd_mode;
}

void node_announce_block_to_peers(node_t *node, const hash256_t *block_hash) {
  if (node == NULL || block_hash == NULL) {
    return;
  }

  /* Skip announcements during IBD - Core behavior */
  if (node->ibd_mode) {
    return;
  }

  /* Announce to all ready peers */
  for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
    peer_t *peer = &node->peers[i];
    if (peer_is_ready(peer)) {
      /* Build and send INV message */
      inv_vector_t inv_vec;
      inv_vec.type = INV_BLOCK;
      inv_vec.hash = *block_hash;

      msg_t inv_msg;
      memset(&inv_msg, 0, sizeof(inv_msg));
      inv_msg.type = MSG_INV;
      inv_msg.payload.inv.count = 1;
      inv_msg.payload.inv.inventory = &inv_vec;

      peer_queue_message(peer, &inv_msg);
    }
  }
}

uint64_t node_get_prune_target(const node_t *node) {
  if (node == NULL) {
    return 0;
  }
  return node->config.prune_target_mb;
}

uint32_t node_get_pruned_height(const node_t *node) {
  if (node == NULL || !node->block_index_db_open) {
    return 0;
  }

  /* Cast away const - block_index_db_get_pruned_height doesn't modify state
   * but the interface doesn't use const. This is safe. */
  block_index_db_t *bdb = (block_index_db_t *)(uintptr_t)&node->block_index_db;

  uint32_t height = 0;
  echo_result_t result = block_index_db_get_pruned_height(bdb, &height);

  if (result != ECHO_OK) {
    return 0; /* No pruning or error - return 0 */
  }

  return height;
}

bool node_is_block_pruned(const node_t *node, uint32_t height) {
  if (node == NULL) {
    return false;
  }

  /* If not pruning, no blocks are pruned */
  if (!node_is_pruning_enabled(node)) {
    return false;
  }

  /* Get the pruned height (lowest height with data) */
  uint32_t pruned_height = node_get_pruned_height(node);

  /* Block is pruned if its height is below the pruned height */
  return height < pruned_height;
}

bool node_is_block_stored(const node_t *node, uint32_t height) {
  if (node == NULL || !node->block_storage_init) {
    return false;
  }
  return block_storage_exists_height(&node->block_storage, height);
}

uint64_t node_get_block_storage_size(const node_t *node) {
  if (node == NULL || !node->block_storage_init) {
    return 0;
  }
  /* Return cached value (updated every 10s by node_maybe_prune) */
  return node->cached_storage_size;
}

echo_result_t node_load_block(node_t *node, const hash256_t *hash,
                              block_t *block_out) {
  if (node == NULL || hash == NULL || block_out == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (!node->block_storage_init || !node->block_index_db_open) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Look up block entry in database */
  block_index_entry_t entry;
  echo_result_t result =
      block_index_db_lookup_by_hash(&node->block_index_db, hash, &entry);
  if (result != ECHO_OK) {
    return result;
  }

  /* Check if block data is stored (HAVE_DATA flag set) */
  if (!(entry.status & BLOCK_STATUS_HAVE_DATA)) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Load block data from file-per-block storage */
  uint8_t *block_data = NULL;
  uint32_t block_size = 0;
  result = block_storage_read_height(&node->block_storage, entry.height,
                                     &block_data, &block_size);
  if (result != ECHO_OK) {
    return result;
  }

  /* Parse block data */
  size_t consumed;
  result = block_parse(block_data, block_size, block_out, &consumed);
  free(block_data);

  return result;
}

echo_result_t node_load_block_at_height(node_t *node, uint32_t height,
                                        block_t *block_out, hash256_t *hash_out) {
  if (node == NULL || block_out == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (!node->block_storage_init || !node->block_index_db_open) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Look up block entry at height (includes hash) */
  block_index_entry_t entry;
  echo_result_t result =
      block_index_db_lookup_by_height(&node->block_index_db, height, &entry);
  if (result != ECHO_OK) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Check if block data is stored (HAVE_DATA flag set) */
  if (!(entry.status & BLOCK_STATUS_HAVE_DATA)) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Load block data directly by height - file-per-block optimization */
  uint8_t *block_data = NULL;
  uint32_t block_size = 0;
  result = block_storage_read_height(&node->block_storage, height,
                                     &block_data, &block_size);
  if (result != ECHO_OK) {
    return result;
  }

  /* Parse block data */
  size_t consumed;
  result = block_parse(block_data, block_size, block_out, &consumed);
  free(block_data);
  if (result != ECHO_OK) {
    return result;
  }

  /* Return hash if requested */
  if (hash_out != NULL) {
    memcpy(hash_out->bytes, entry.hash.bytes, 32);
  }

  return ECHO_OK;
}

bool node_validate_block(node_t *node, const block_t *block) {
  if (node == NULL || block == NULL || node->consensus == NULL) {
    return false;
  }

  consensus_result_t result;
  consensus_result_init(&result);
  return consensus_validate_block(node->consensus, block, &result);
}

uint32_t node_get_validated_height(node_t *node) {
  if (node == NULL || node->consensus == NULL) {
    return 0;
  }
  return consensus_get_height(node->consensus);
}

/* Forward declaration for pruning helper */
static uint32_t prune_blocks_to_target(node_t *node, uint32_t start_height,
                                        uint32_t end_height);

echo_result_t node_maybe_prune(node_t *node) {
  if (node == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Nothing to do if pruning not enabled */
  if (!node_is_pruning_enabled(node)) {
    return ECHO_OK;
  }

  /*
   * Progressive IBD Pruning: We prune validated blocks even during IBD.
   *
   * The 550-block safety margin in node_prune_blocks() ensures we never
   * prune blocks needed for reorg. Additionally, node_prune_blocks() checks
   * each file's max height against the validated height to ensure we never
   * delete files containing downloaded-but-not-validated blocks.
   *
   * This keeps disk usage bounded during IBD rather than accumulating
   * potentially 100GB+ before pruning begins.
   */

  /* Get current storage size (expensive scan - update cache) */
  uint64_t current_size_bytes = 0;
  if (node->block_storage_init) {
    block_storage_get_total_size(&node->block_storage, &current_size_bytes);
    node->cached_storage_size = current_size_bytes;
  }
  uint64_t target_size_bytes = node->config.prune_target_mb * 1024 * 1024;

  /* Check if we're over target */
  if (current_size_bytes <= target_size_bytes) {
    return ECHO_OK; /* Under target, nothing to do */
  }

  /* Log - less frequently during IBD to avoid spam */
  if (node->ibd_mode) {
    static uint32_t last_ibd_prune_height = 0;
    uint32_t current_height = node->consensus != NULL
                                  ? consensus_get_height(node->consensus)
                                  : 0;
    if (current_height >= last_ibd_prune_height + 10000) {
      log_info(LOG_COMP_STORE,
               "Storage %llu MB exceeds target %llu MB, progressive IBD "
               "pruning...",
               (unsigned long long)(current_size_bytes / (1024ULL * 1024ULL)),
               (unsigned long long)node->config.prune_target_mb);
      last_ibd_prune_height = current_height;
    }
  } else {
    log_info(LOG_COMP_STORE,
             "Storage size %llu MB exceeds target %llu MB, pruning...",
             (unsigned long long)(current_size_bytes / (1024ULL * 1024ULL)),
             (unsigned long long)node->config.prune_target_mb);
  }

  /* Get validated height to calculate max prune height.
   * We can safely prune all blocks up to (validated_height - 550). */
  uint32_t validated_height = 0;
  if (node->sync_mgr != NULL) {
    const block_tracker_t *tracker = sync_get_block_tracker(node->sync_mgr);
    if (tracker != NULL) {
      validated_height = block_tracker_get_validated_tip(tracker);
    }
  }

  /* Cannot prune until validated > 550 */
  if (validated_height <= 550) {
    log_debug(LOG_COMP_STORE, "Cannot prune: validated_height=%u (need >550)",
              validated_height);
    return ECHO_OK;
  }

  /* Prune everything from 0 to (validated_height - 550) */
  uint32_t target_height = validated_height - 550;

  if (node->block_storage_init) {
    prune_blocks_to_target(node, 0, target_height);
  }

  return ECHO_OK;
}

/*
 * Prune blocks in the specified height range.
 *
 * Each block is stored as an individual file (blocks/{height}.blk).
 * Pruning is trivial: just unlink() each file.
 *
 * Parameters:
 *   node          - Node handle
 *   start_height  - First block to prune (inclusive)
 *   end_height    - Last block to prune (exclusive)
 *
 * Returns:
 *   Number of blocks successfully pruned
 */
static uint32_t prune_blocks_to_target(node_t *node, uint32_t start_height,
                                        uint32_t end_height) {
  if (node == NULL || !node->block_storage_init || !node->block_index_db_open) {
    return 0;
  }

  /* Cannot prune if not in pruning mode */
  if (!node_is_pruning_enabled(node)) {
    return 0;
  }

  /*
   * Get validated height for safety margin.
   * During decoupled IBD, consensus height stays at 0 until validation runs.
   * We must use the block_tracker's validated_tip to know what's actually safe to prune.
   *
   * CRITICAL: We can ONLY prune blocks that have been validated. If no validation
   * has occurred yet, we must NOT prune anything, even if storage is full.
   */
  uint32_t validated_height = 0;
  if (node->sync_mgr != NULL) {
    const block_tracker_t *tracker = sync_get_block_tracker(node->sync_mgr);
    if (tracker != NULL) {
      validated_height = block_tracker_get_validated_tip(tracker);
    }
  }
  /*
   * NOTE: We intentionally do NOT fall back to consensus_get_height() here.
   * During decoupled IBD, block_tracker is the source of truth for validated_tip.
   * consensus_get_height() returns UINT32_MAX when uninitialized, which would
   * bypass the safety check below.
   */

  /* Cannot prune until validation has made sufficient progress */
  if (validated_height <= 550) {
    log_debug(LOG_COMP_STORE,
              "Cannot prune: validated_height=%u (need >550)", validated_height);
    return 0;
  }

  /* Maintain safety margin: keep at least 550 blocks for reorg safety */
  uint32_t min_keep_height = 0;
  if (validated_height > 550) {
    min_keep_height = validated_height - 550;
  }

  /* Don't prune into the safety margin */
  if (end_height > min_keep_height) {
    end_height = min_keep_height;
  }

  if (start_height >= end_height) {
    return 0;
  }

  log_info(LOG_COMP_STORE, "Pruning blocks %u to %u (file-per-block)",
           start_height, end_height - 1);

  uint32_t blocks_pruned = 0;

  for (uint32_t height = start_height; height < end_height; height++) {
    /* Check if block file exists */
    if (!block_storage_exists_height(&node->block_storage, height)) {
      continue;
    }

    /* Get block hash for database update */
    block_index_entry_t entry;
    echo_result_t result = block_index_db_lookup_by_height(
        &node->block_index_db, height, &entry);
    if (result != ECHO_OK) {
      log_warn(LOG_COMP_STORE, "Block %u not in index, skipping prune", height);
      continue;
    }

    /* Prune the block file */
    result = block_storage_prune_height(&node->block_storage, height);
    if (result == ECHO_OK) {
      blocks_pruned++;

      /* Update database to mark block as pruned */
      result = block_index_db_clear_have_data(&node->block_index_db, &entry.hash);
      if (result != ECHO_OK) {
        log_warn(LOG_COMP_DB, "Failed to mark block %u as pruned: %d",
                 height, result);
      }
    } else {
      log_warn(LOG_COMP_STORE, "Failed to prune block %u: %d", height, result);
    }
  }

  if (blocks_pruned > 0) {
    /* Get total size after pruning */
    uint64_t new_size = 0;
    block_storage_get_total_size(&node->block_storage, &new_size);

    log_info(LOG_COMP_STORE,
             "Pruning complete: %u blocks pruned, storage now %llu MB",
             blocks_pruned, (unsigned long long)(new_size / (1024ULL * 1024ULL)));
  }

  return blocks_pruned;
}

/*
 * Prune blocks up to the specified target height.
 *
 * Public API for manual pruning (e.g., via RPC).
 *
 * Parameters:
 *   node          - Node handle
 *   target_height - Prune blocks below this height
 *
 * Returns:
 *   The new pruned height (first block with data after pruning)
 */
uint32_t node_prune_blocks(node_t *node, uint32_t target_height) {
  if (node == NULL || !node->block_index_db_open) {
    return 0;
  }

  /* Get current pruned height */
  uint32_t current_pruned_height = 0;
  block_index_db_get_pruned_height(&node->block_index_db, &current_pruned_height);

  /* Nothing to do if already at or beyond target */
  if (current_pruned_height >= target_height) {
    return current_pruned_height;
  }

  /* Perform pruning */
  prune_blocks_to_target(node, current_pruned_height, target_height);

  /* Get and return the new pruned height */
  uint32_t new_pruned_height = 0;
  block_index_db_get_pruned_height(&node->block_index_db, &new_pruned_height);
  return new_pruned_height;
}
