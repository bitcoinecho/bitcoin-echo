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
#include "blocks_storage.h"
#include "chainstate.h"
#include "chase.h"
#include "chaser.h"
#include "chaser_confirm.h"
#include "chaser_validate.h"
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
  block_file_manager_t block_storage;
  bool utxo_db_open;
  bool block_index_db_open;
  bool block_storage_init;

  /* IBD optimization: defer UTXO persistence for speed */
  bool ibd_mode;                   /* Currently in initial block download */
  uint32_t last_utxo_persist_height; /* Last height we persisted UTXOs */

  /* Consensus engine (NULL if in observer mode) */
  consensus_engine_t *consensus;

  /* Mempool (NULL if in observer mode) */
  mempool_t *mempool;

  /* Sync manager */
  sync_manager_t *sync_mgr;

  /* Chase event system */
  chase_dispatcher_t *dispatcher;
  chaser_validate_t *chaser_validate;
  chaser_confirm_t *chaser_confirm;

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

  /*
   * UTXO Delta Accumulator (Session IBD optimization)
   *
   * Instead of flushing ALL UTXOs at each checkpoint, we only:
   *   - DELETE spent outpoints (accumulated here)
   *   - INSERT newly created UTXOs (collected at flush time by filtering
   *     the in-memory UTXO set by height > last_checkpoint)
   *
   * This reduces checkpoint I/O from ~6.6M to ~150k operations.
   */
  #define UTXO_ACCUM_INITIAL_CAPACITY 65536
  outpoint_t *accum_spent;           /* Array of spent outpoints */
  size_t accum_spent_count;
  size_t accum_spent_capacity;
};

/*
 * ============================================================================
 * FORWARD DECLARATIONS
 * ============================================================================
 */

static echo_result_t node_init_directories(node_t *node);
static echo_result_t node_flush_utxo_set_to_db(node_t *node, uint32_t height);
static echo_result_t node_init_databases(node_t *node);
static echo_result_t node_init_consensus(node_t *node);
static echo_result_t node_restore_chain_state(node_t *node);
static echo_result_t node_init_mempool(node_t *node);
static echo_result_t node_init_discovery(node_t *node);
static echo_result_t node_init_sync(node_t *node);
static echo_result_t node_init_chase(node_t *node);
static void node_cleanup(node_t *node);
static echo_result_t node_cleanup_orphan_block_files(node_t *node);

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

/*
 * ============================================================================
 * UTXO DELTA ACCUMULATOR
 * ============================================================================
 *
 * Functions to manage the UTXO delta accumulator for efficient checkpointing.
 */

/**
 * Initialize the spent outpoint accumulator.
 */
static echo_result_t node_accum_init(node_t *node) {
  node->accum_spent = calloc(UTXO_ACCUM_INITIAL_CAPACITY, sizeof(outpoint_t));
  if (node->accum_spent == NULL) {
    return ECHO_ERR_NOMEM;
  }
  node->accum_spent_count = 0;
  node->accum_spent_capacity = UTXO_ACCUM_INITIAL_CAPACITY;

  return ECHO_OK;
}

/**
 * Free the accumulator array entirely.
 */
static void node_accum_free(node_t *node) {
  free(node->accum_spent);
  node->accum_spent = NULL;
  node->accum_spent_count = 0;
  node->accum_spent_capacity = 0;
}

/*
 * ============================================================================
 * ORPHAN BLOCK FILE CLEANUP
 * ============================================================================
 *
 * After a restart (especially from checkpoint), there may be block files
 * on disk that are not referenced by any block in the database. This happens
 * when:
 *   - The database was reset/restored but block files weren't cleaned up
 *   - A crash occurred after writing block data but before updating the DB
 *   - Development/debugging sessions left orphaned data
 *
 * This function scans block files and deletes any not referenced by the DB,
 * ensuring we don't waste disk space on unreachable data.
 */
static echo_result_t node_cleanup_orphan_block_files(node_t *node) {
  if (node == NULL || !node->block_storage_init || !node->block_index_db_open) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Get list of file indices referenced by blocks in the DB */
  uint32_t *referenced_files = NULL;
  size_t referenced_count = 0;
  echo_result_t result = block_index_db_get_referenced_files(
      &node->block_index_db, &referenced_files, &referenced_count);
  if (result != ECHO_OK) {
    log_warn(LOG_COMP_STORE, "Failed to query referenced block files: %d",
             result);
    return result;
  }

  /* Scan all block files on disk */
  uint32_t files_deleted = 0;
  uint64_t bytes_freed = 0;
  uint32_t current_file = block_storage_get_current_file(&node->block_storage);

  for (uint32_t file_idx = 0; file_idx < current_file; file_idx++) {
    bool exists = false;
    result = block_storage_file_exists(&node->block_storage, file_idx, &exists);
    if (result != ECHO_OK || !exists) {
      continue;
    }

    /* Check if this file is referenced */
    bool is_referenced = false;
    for (size_t i = 0; i < referenced_count; i++) {
      if (referenced_files[i] == file_idx) {
        is_referenced = true;
        break;
      }
    }

    if (!is_referenced) {
      /* Get file size before deleting (for logging) */
      uint64_t file_size = 0;
      block_storage_get_file_size(&node->block_storage, file_idx, &file_size);

      /* Delete orphan file */
      result = block_storage_delete_file(&node->block_storage, file_idx);
      if (result == ECHO_OK) {
        files_deleted++;
        bytes_freed += file_size;
      } else {
        log_warn(LOG_COMP_STORE, "Failed to delete orphan block file %u: %d",
                 file_idx, result);
      }
    }
  }

  free(referenced_files);

  if (files_deleted > 0) {
    log_info(LOG_COMP_STORE,
             "Cleaned up %u orphan block files, freed %llu MB",
             files_deleted, (unsigned long long)(bytes_freed / (1024 * 1024)));
  }

  return ECHO_OK;
}

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

    /* Step 3b: Initialize chase event system (full node only) */
    result = node_init_chase(node);
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

    /* Step 5b: Initialize UTXO delta accumulator for efficient checkpointing */
    result = node_accum_init(node);
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
  node->last_utxo_persist_height = 0;
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

  /* Initialize block file storage */
  result = block_storage_init(&node->block_storage, node->config.data_dir);
  if (result != ECHO_OK) {
    return result;
  }
  node->block_storage_init = true;

  /*
   * Clean up orphan block files that may have been left behind from
   * previous runs (e.g., after a crash or checkpoint restore where
   * the database was reset but block files weren't deleted).
   */
  result = node_cleanup_orphan_block_files(node);
  if (result != ECHO_OK) {
    log_warn(LOG_COMP_STORE, "Orphan block file cleanup failed: %d", result);
    /* Non-fatal - continue anyway */
  }

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

    /* Update chainwork and data position from database (may differ from
     * calculated if we have persisted state from previous sessions) */
    if (index != NULL) {
      index->chainwork = entry.chainwork;
      index->on_main_chain = (entry.status & BLOCK_STATUS_VALID_CHAIN) != 0;
      /* Restore block data file position (for stored blocks).
       * Skip if block is pruned - the file no longer exists. */
      if (entry.data_file >= 0 && !(entry.status & BLOCK_STATUS_PRUNED)) {
        index->data_file = (uint32_t)entry.data_file;
        index->data_pos = entry.data_pos;
      }
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
        "SELECT COUNT(*) FROM blocks WHERE height > ? AND data_file >= 0",
        &count_stmt);
    if (count_result == ECHO_OK) {
      db_bind_int(&count_stmt, 1, (int64_t)validated_height);
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

/**
 * Initialize chase event system and chasers.
 *
 * Creates the event dispatcher and validation/confirmation chasers.
 * These components handle parallel block validation and sequential
 * confirmation.
 */
static echo_result_t node_init_chase(node_t *node) {
  /* Create chase event dispatcher */
  node->dispatcher = chase_dispatcher_create();
  if (node->dispatcher == NULL) {
    log_error(LOG_COMP_MAIN, "Failed to create chase dispatcher");
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /* Get chainstate for chasers */
  chainstate_t *chainstate = consensus_get_chainstate(node->consensus);
  if (chainstate == NULL) {
    log_error(LOG_COMP_MAIN, "Failed to get chainstate for chasers");
    return ECHO_ERR_INVALID;
  }

  /*
   * Create validation chaser with threadpool.
   * Worker count 0 = auto-detect CPU count.
   * Max backlog 0 = default (50 concurrent validations).
   */
  node->chaser_validate = chaser_validate_create(
      node, node->dispatcher, chainstate, 0, 0);
  if (node->chaser_validate == NULL) {
    log_error(LOG_COMP_MAIN, "Failed to create validation chaser");
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /* Create confirmation chaser (single-threaded, sequential) */
  node->chaser_confirm = chaser_confirm_create(
      node, node->dispatcher, chainstate);
  if (node->chaser_confirm == NULL) {
    log_error(LOG_COMP_MAIN, "Failed to create confirmation chaser");
    return ECHO_ERR_OUT_OF_MEMORY;
  }

  /* Start chasers (subscribes them to events) */
  if (chaser_start(&node->chaser_validate->base) != 0) {
    log_error(LOG_COMP_MAIN, "Failed to start validation chaser");
    return ECHO_ERR_INVALID;
  }

  if (chaser_start(&node->chaser_confirm->base) != 0) {
    log_error(LOG_COMP_MAIN, "Failed to start confirmation chaser");
    return ECHO_ERR_INVALID;
  }

  /* Fire CHASE_START to begin chaser operations */
  chase_notify_default(node->dispatcher, CHASE_START);

  log_info(LOG_COMP_MAIN, "Chase event system initialized and started");
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

  /* Look up block position in database */
  block_index_entry_t entry;
  echo_result_t result =
      block_index_db_lookup_by_hash(&node->block_index_db, hash, &entry);
  if (result != ECHO_OK) {
    return result;
  }

  /* Check if block data is stored (data_file != -1) */
  if (entry.data_file < 0) {
    return ECHO_ERR_NOT_FOUND; /* Header only, no block data */
  }

  /* Load block data from storage */
  block_file_pos_t pos;
  pos.file_index = (uint32_t)entry.data_file;
  pos.file_offset = entry.data_pos;

  uint8_t *block_data = NULL;
  uint32_t block_size = 0;
  result = block_storage_read(&node->block_storage, pos, &block_data, &block_size);
  if (result != ECHO_OK) {
    log_warn(LOG_COMP_STORE, "Failed to read block from file %u offset %u: %d",
             pos.file_index, pos.file_offset, result);
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
 * Stores the block to disk immediately so out-of-order blocks don't need
 * to be re-downloaded. Records the file position in the database and
 * in-memory block index for later retrieval.
 */
echo_result_t node_store_block(node_t *node, const block_t *block) {
  if (node == NULL || block == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* Skip if block storage not initialized */
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

  /* Write to block storage */
  block_file_pos_t pos;
  result = block_storage_write(&node->block_storage, block_data,
                               (uint32_t)written, &pos);
  free(block_data);

  if (result != ECHO_OK) {
    log_error(LOG_COMP_STORE, "Failed to write block to storage: %d", result);
    return result;
  }

  /* Update block index database with file position */
  if (node->block_index_db_open) {
    result = block_index_db_update_data_pos(&node->block_index_db, &block_hash,
                                            pos.file_index, pos.file_offset);
    if (result != ECHO_OK && result != ECHO_ERR_NOT_FOUND) {
      log_warn(LOG_COMP_STORE, "Failed to update block index DB: %d", result);
      /* Continue anyway - block is stored on disk */
    }
  }

  /* Update in-memory block index if available */
  chainstate_t *chainstate = consensus_get_chainstate(node->consensus);
  if (chainstate != NULL) {
    block_index_map_t *index_map = chainstate_get_block_index_map(chainstate);
    block_index_t *block_index = block_index_map_lookup(index_map, &block_hash);
    if (block_index != NULL) {
      block_index->data_file = pos.file_index;
      block_index->data_pos = pos.file_offset;
      log_info(LOG_COMP_STORE, "Block stored: height=%u, file=%u, offset=%u",
               block_index->height, pos.file_index, pos.file_offset);
    } else {
      log_warn(LOG_COMP_STORE, "Block stored but no block_index found");
    }
  }

  log_debug(LOG_COMP_STORE, "Block stored at file %u offset %u (height lookup pending)",
            pos.file_index, pos.file_offset);

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
      /* NOTE: validate_and_apply_block removed - validation is now chase-driven */
      .send_getheaders = sync_cb_send_getheaders,
      .send_getdata_blocks = sync_cb_send_getdata_blocks,
      .get_block_hash_at_height = sync_cb_get_block_hash_at_height,
      .begin_header_batch = sync_cb_begin_header_batch,
      .commit_header_batch = sync_cb_commit_header_batch,
      .flush_headers = NULL,
      .ctx = node};

  /* Create sync manager with appropriate download window for mode */
  uint32_t download_window = node->config.prune_target_mb > 0
                                 ? SYNC_BLOCK_DOWNLOAD_WINDOW_PRUNED
                                 : SYNC_BLOCK_DOWNLOAD_WINDOW_ARCHIVAL;
  node->sync_mgr = sync_create(chainstate, &callbacks, download_window,
                               node->dispatcher);
  if (node->sync_mgr == NULL) {
    log_error(LOG_COMP_MAIN, "Failed to create sync manager");
    return ECHO_ERR_OUT_OF_MEMORY;
  }
  log_info(LOG_COMP_MAIN, "Download window: %u blocks (%s mode)", download_window,
           node->config.prune_target_mb > 0 ? "pruned" : "archival");

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

  /*
   * Shutdown sequence:
   * 1. Stop accepting new connections
   * 2. Disconnect all peers
   * 3. Stop sync manager
   * 4. Databases will be closed in node_destroy()
   *
   * NOTE: We do NOT save validated_tip or flush UTXOs on shutdown.
   * State is only persisted at fixed 5000-block checkpoints during IBD.
   * This is intentional: it's simpler and avoids UTXO/stored-block mismatches.
   * On restart, we always resume from the last 5000-block checkpoint.
   * Max 4999 blocks of progress may be lost, but the state is always consistent.
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
  /* Free UTXO delta accumulator */
  node_accum_free(node);

  /*
   * Destroy chase event system (reverse order of creation).
   * Signal CHASE_STOP first to allow chasers to drain their queues.
   */
  if (node->dispatcher != NULL) {
    chase_notify_default(node->dispatcher, CHASE_STOP);
  }

  if (node->chaser_confirm != NULL) {
    chaser_confirm_destroy(node->chaser_confirm);
    node->chaser_confirm = NULL;
  }

  if (node->chaser_validate != NULL) {
    chaser_validate_destroy(node->chaser_validate);
    node->chaser_validate = NULL;
  }

  if (node->dispatcher != NULL) {
    chase_dispatcher_destroy(node->dispatcher);
    node->dispatcher = NULL;
  }

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
       * During IBD, disconnect pruned peers immediately to free connection
       * slots for full archival nodes that can serve historical blocks.
       *
       * Per BIP-159:
       *   - Pruned nodes: MUST NOT set NODE_NETWORK, only NODE_NETWORK_LIMITED
       *   - Full archival nodes: Set NODE_NETWORK, MAY also set NODE_NETWORK_LIMITED
       *
       * So checking NODE_NETWORK is sufficient - if set, peer can serve all blocks.
       * Full nodes often signal BOTH bits, so we must NOT reject based on LIMITED.
       */
      bool is_full_node = (peer->services & SERVICE_NODE_NETWORK) != 0;
      bool is_ibd = node->ibd_mode;

      if (is_ibd && !is_full_node) {
        log_info(LOG_COMP_NET,
                 "Disconnecting pruned peer %s (services=0x%llx) - need full "
                 "nodes for IBD",
                 peer->address, (unsigned long long)peer->services);
        peer_disconnect(peer, PEER_DISCONNECT_USER, "Pruned peer during IBD");
        break;
      }

      /* Add full nodes to sync manager */
      if (node->sync_mgr != NULL) {
        sync_add_peer(node->sync_mgr, peer, peer->start_height);
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
    break;

  case MSG_TX:
    /*
     * IBD Optimization: During initial block download, drop all incoming
     * transactions. They waste CPU (validation), memory (mempool), and
     * will be in the blocks we download anyway.
     */
    if (node->ibd_mode) {
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
      if (!peer_is_connected(peer)) {
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
           * - USER disconnect or long-lived connection = success
           * - MISBEHAVING, PROTOCOL_ERROR, immediate disconnect = failure
           */
          echo_bool_t was_success = ECHO_FALSE;
          uint64_t connection_duration_ms =
              plat_time_ms() - peer->connect_time;

          /* Consider successful if connection lasted > 5 minutes */
          if (connection_duration_ms > 300000) {
            was_success = ECHO_TRUE;
          } else if (peer->disconnect_reason == PEER_DISCONNECT_USER) {
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
   * - Below 50%: try up to 8 connections per cycle
   * - Below 75%: try up to 4 connections per cycle
   * - Below target: try up to 2 connections per cycle
   */
  size_t max_attempts = 2; /* Always try at least 2 when below target */
  if (outbound_count < target_peers / 2) {
    max_attempts = 8;
  } else if (outbound_count < (target_peers * 3) / 4) {
    max_attempts = 4;
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

    /* Find empty slot */
    bool connected = false;
    for (size_t i = 0; i < NODE_MAX_PEERS; i++) {
      peer_t *peer = &node->peers[i];
      if (!peer_is_connected(peer)) {
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
          log_info(LOG_COMP_NET, "Connected to peer %s:%u", ip_str, addr.port);

          /* Send version message to start handshake */
          uint32_t our_height = 0;
          if (node->consensus != NULL) {
            our_height = consensus_get_height(node->consensus);
          }

          /* Service flags: NODE_NETWORK (1) only if we're not pruned */
          uint64_t services = node_is_pruning_enabled(node) ? 0 : 1;
          peer_send_version(peer, services, (int32_t)our_height, true);
          connected = true;
        } else {
          log_warn(LOG_COMP_NET, "Failed to connect to %s:%u: error %d",
                   ip_str, addr.port, result);
          /* Release address back to pool as failed attempt */
          discovery_mark_address_free(&node->addr_manager, &addr, ECHO_FALSE);
        }
        break;
      }
    }
    attempts++;
    if (!connected) {
      /* Release address - we marked it in_use but couldn't use it */
      discovery_mark_address_free(&node->addr_manager, &addr, ECHO_FALSE);
      break; /* No empty slots available */
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

  /*
   * Step 1: Apply block to consensus engine (in-memory state).
   * This updates the UTXO set and chain tip in memory.
   */
  consensus_result_t validation_result;
  consensus_result_init(&validation_result);
  result = consensus_apply_block(node->consensus, block, &validation_result);
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
     * IBD OPTIMIZATION: Skip per-block UTXO persistence during initial sync.
     * The in-memory UTXO set is sufficient for validation.
     *
     * At checkpoint intervals (every 5,000 blocks), we flush the ENTIRE
     * in-memory UTXO set to the database. This ensures all accumulated
     * UTXOs are persisted, not just the current block's changes.
     *
     * This gives ~10x speedup during IBD by eliminating per-block SQLite writes.
     */
#define UTXO_PERSIST_INTERVAL 5000

    if (!node->ibd_mode) {
      /* Normal operation: persist every block */
      result = utxo_db_apply_block(&node->utxo_db, new_utxos, new_count,
                                   spent_outpoints, spent_count);
      if (result != ECHO_OK) {
        log_error(LOG_COMP_DB, "Failed to apply block to UTXO database: %d",
                  result);
      }
    } else if (height % UTXO_PERSIST_INTERVAL == 0) {
      /* IBD checkpoint: flush entire UTXO set to database */
      result = node_flush_utxo_set_to_db(node, height);

      if (result == ECHO_OK) {
        node->last_utxo_persist_height = height;

        /* Also persist validated tip */
        if (node->block_index_db_open) {
          block_index_db_set_validated_tip(&node->block_index_db, height, NULL);
        }

        log_info(LOG_COMP_DB, "Checkpoint at height %u (UTXO + validated tip)",
                 height);
      } else {
        log_error(LOG_COMP_DB, "Failed to flush UTXO set at checkpoint: %d",
                  result);
      }
    }

    free(new_utxos);
    free(new_entries);
    free(spent_outpoints);
  }

  log_info(LOG_COMP_CONS, "Block applied: height=%u txs=%zu", height,
           block->tx_count);

  return ECHO_OK;
}

/*
 * ============================================================================
 * UTXO FLUSH
 * ============================================================================
 */

/* Context for UTXO flush callback */
typedef struct {
  utxo_db_t *udb;
  size_t inserted;
  size_t skipped;
  echo_result_t result;
} utxo_flush_ctx_t;

/* Callback to insert each UTXO into the database */
static bool utxo_flush_callback(const utxo_entry_t *entry, void *user_data) {
  utxo_flush_ctx_t *ctx = (utxo_flush_ctx_t *)user_data;
  if (ctx == NULL || entry == NULL) {
    return false;
  }

  /* Try to insert (INSERT OR IGNORE handles duplicates) */
  echo_result_t result = utxo_db_insert(ctx->udb, entry);
  if (result == ECHO_OK) {
    ctx->inserted++;
  } else if (result == ECHO_ERR_EXISTS) {
    ctx->skipped++;
  } else {
    ctx->result = result;
    return false; /* Stop iteration on error */
  }

  return true; /* Continue iteration */
}

/**
 * Flush entire in-memory UTXO set to database.
 *
 * This iterates over all UTXOs in memory and inserts them into the database.
 * Used at checkpoints to persist the complete UTXO state.
 */
static echo_result_t node_flush_utxo_set_to_db(node_t *node, uint32_t height) {
  if (node == NULL || node->consensus == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  /* IBD profiling - track flush timing */
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
  log_info(LOG_COMP_DB, "Flushing %zu in-memory UTXOs to database (height=%u)",
           utxo_count, height);

  if (utxo_count == 0) {
    /* Nothing to flush */
    return ECHO_OK;
  }

  /* Begin transaction for bulk insert */
  echo_result_t result = db_begin(&node->utxo_db.db);
  if (result != ECHO_OK) {
    log_error(LOG_COMP_DB, "Failed to begin UTXO flush transaction: %d",
              result);
    return result;
  }

  /* Iterate and insert all UTXOs */
  utxo_flush_ctx_t ctx = {
      .udb = &node->utxo_db,
      .inserted = 0,
      .skipped = 0,
      .result = ECHO_OK,
  };

  utxo_set_foreach(utxo_set, utxo_flush_callback, &ctx);

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

  /* IBD profiling - log flush timing */
  uint64_t flush_elapsed = plat_monotonic_ms() - flush_start;
  log_info(LOG_COMP_DB,
           "UTXO flush complete in %lums: %zu inserted, %zu skipped "
           "(rate=%.1f/ms)",
           (unsigned long)flush_elapsed, ctx.inserted, ctx.skipped,
           flush_elapsed > 0 ? (double)ctx.inserted / flush_elapsed : 0.0);

  return ECHO_OK;
}

/*
 * ============================================================================
 * DELTA-BASED UTXO FLUSH (IBD Optimization)
 * ============================================================================
 *
 * Instead of flushing all ~6.6M UTXOs at each checkpoint, we:
 *   1. INSERT only UTXOs created since last checkpoint (height > threshold)
 *   2. DELETE only UTXOs that were spent since last checkpoint
 *
 * This reduces checkpoint I/O from ~6.6M to ~150k operations (5000 blocks).
 */

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
   * Unified validation path: Store block and fire CHASE_CHECKED.
   * This routes ALL blocks (IBD + relay) through the chase event system:
   *   CHASE_CHECKED → chaser_validate → CHASE_VALID → chaser_confirm
   *
   * This matches libbitcoin-node's unified organize() approach.
   */

  /* Determine block height from parent */
  uint32_t height = 0;
  if (existing != NULL) {
    /* Header already known - use its height */
    height = existing->height;
  } else {
    /* Look up parent to compute height */
    const block_index_t *parent = consensus_lookup_block_index(
        node->consensus, &block->header.prev_hash);
    if (parent == NULL) {
      /* Parent unknown - orphan block, can't process yet */
      log_debug(LOG_COMP_CONS, "Orphan block received (parent unknown)");
      return ECHO_ERR_NOT_FOUND;
    }
    height = parent->height + 1;
  }

  /* Store block to disk */
  result = node_store_block(node, block);
  if (result != ECHO_OK && result != ECHO_ERR_EXISTS) {
    log_error(LOG_COMP_STORE, "Failed to store received block: %d", result);
    return result;
  }

  /* Fire CHASE_CHECKED to trigger validation pipeline */
  if (node->dispatcher != NULL) {
    chase_notify_height(node->dispatcher, CHASE_CHECKED, height);
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

uint64_t node_get_block_storage_size(const node_t *node) {
  if (node == NULL || !node->block_storage_init) {
    return 0;
  }

  uint64_t total_size = 0;
  echo_result_t result = block_storage_get_total_size(
      &node->block_storage, &total_size);

  if (result != ECHO_OK) {
    return 0;
  }

  return total_size;
}

echo_result_t node_load_block(node_t *node, const hash256_t *hash,
                              block_t *block_out) {
  if (node == NULL || hash == NULL || block_out == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }

  if (!node->block_storage_init || !node->block_index_db_open) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Look up block position in database */
  block_index_entry_t entry;
  echo_result_t result =
      block_index_db_lookup_by_hash(&node->block_index_db, hash, &entry);
  if (result != ECHO_OK) {
    return result;
  }

  /* Check if block data is stored */
  if (entry.data_file < 0) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Load block data from storage */
  block_file_pos_t pos;
  pos.file_index = (uint32_t)entry.data_file;
  pos.file_offset = entry.data_pos;

  uint8_t *block_data = NULL;
  uint32_t block_size = 0;
  result = block_storage_read(&node->block_storage, pos, &block_data, &block_size);
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

  if (!node->block_index_db_open) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Look up block entry at height (includes hash) */
  block_index_entry_t entry;
  echo_result_t result =
      block_index_db_lookup_by_height(&node->block_index_db, height, &entry);
  if (result != ECHO_OK) {
    return ECHO_ERR_NOT_FOUND;
  }

  /* Load block by hash */
  result = node_load_block(node, &entry.hash, block_out);
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

uint32_t node_prune_blocks(node_t *node, uint32_t target_height) {
  if (node == NULL || !node->block_storage_init || !node->block_index_db_open) {
    return 0;
  }

  /* Cannot prune if not in pruning mode */
  if (!node_is_pruning_enabled(node)) {
    return 0;
  }

  /* Get current chain height */
  uint32_t chain_height = 0;
  if (node->consensus != NULL) {
    chain_height = consensus_get_height(node->consensus);
  }

  /* Maintain safety margin: keep at least 550 blocks for reorg safety */
  uint32_t min_keep_height = 0;
  if (chain_height > 550) {
    min_keep_height = chain_height - 550;
  }

  /* Don't prune below the safety margin */
  if (target_height > min_keep_height) {
    target_height = min_keep_height;
  }

  /* Get current pruned height */
  uint32_t current_pruned_height = node_get_pruned_height(node);

  /* Nothing to do if already pruned past target */
  if (current_pruned_height >= target_height) {
    return current_pruned_height;
  }

  log_info(LOG_COMP_STORE, "Pruning blocks from height %u to %u",
           current_pruned_height, target_height);

  /*
   * Strategy: Delete old block files that contain only prunable blocks.
   *
   * For simplicity, we delete entire block files. A more sophisticated
   * approach would track exactly which blocks are in each file, but
   * this is sufficient for the initial implementation.
   *
   * Block files are ~128 MB each, containing roughly 1000 blocks at
   * current sizes.
   */

  /* Get the lowest file index */
  uint32_t lowest_file = 0;
  echo_result_t result = block_storage_get_lowest_file(&node->block_storage,
                                                        &lowest_file);
  if (result != ECHO_OK) {
    log_warn(LOG_COMP_STORE, "No block files found for pruning");
    return current_pruned_height;
  }

  /* Get current write file (don't delete it) */
  uint32_t current_file = block_storage_get_current_file(&node->block_storage);

  /* Calculate how many bytes we need to free */
  uint64_t current_size = node_get_block_storage_size(node);
  uint64_t target_size = node->config.prune_target_mb * 1024ULL * 1024ULL;
  uint64_t bytes_to_free = (current_size > target_size) ? (current_size - target_size) : 0;

  /* Delete old block files until we've freed enough space */
  uint32_t files_deleted = 0;
  uint64_t bytes_freed = 0;
  for (uint32_t file_idx = lowest_file; file_idx < current_file; file_idx++) {
    /* Check if file exists */
    bool exists = false;
    result = block_storage_file_exists(&node->block_storage, file_idx, &exists);
    if (result != ECHO_OK || !exists) {
      continue;
    }

    /* Get file size before deleting */
    uint64_t file_size = 0;
    block_storage_get_file_size(&node->block_storage, file_idx, &file_size);

    /* Delete the file */
    result = block_storage_delete_file(
        (block_file_manager_t *)&node->block_storage, file_idx);
    if (result == ECHO_OK) {
      files_deleted++;
      bytes_freed += file_size;
      log_info(LOG_COMP_STORE, "Deleted block file blk%05u.dat (%llu MB)",
               file_idx, (unsigned long long)(file_size / (1024 * 1024)));
    } else {
      log_warn(LOG_COMP_STORE, "Failed to delete blk%05u.dat: %d",
               file_idx, result);
    }

    /* Stop if we've freed enough bytes */
    if (bytes_freed >= bytes_to_free) {
      break;
    }
  }

  /* Mark blocks as pruned in the database */
  if (files_deleted > 0) {
    result = block_index_db_mark_pruned(
        (block_index_db_t *)&node->block_index_db,
        current_pruned_height, target_height);
    if (result != ECHO_OK) {
      log_warn(LOG_COMP_DB, "Failed to mark blocks as pruned: %d", result);
    }
  }

  /* Get and return the new pruned height */
  uint32_t new_pruned_height = node_get_pruned_height(node);

  if (files_deleted > 0) {
    log_info(LOG_COMP_STORE,
             "Pruning complete: deleted %u files, freed %llu MB",
             files_deleted, (unsigned long long)(bytes_freed / (1024 * 1024)));
  }

  return new_pruned_height;
}

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
   * prune blocks needed for reorg. Since consensus_get_height() returns
   * the VALIDATED height (not downloaded height), we never prune blocks
   * that are downloaded but not yet validated.
   *
   * This keeps disk usage bounded during IBD rather than accumulating
   * potentially 100GB+ before pruning begins.
   */

  /* Get current storage size */
  uint64_t current_size_bytes = node_get_block_storage_size(node);
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

  /* Calculate how much to prune */
  uint64_t excess_bytes = current_size_bytes - target_size_bytes;

  /* Estimate blocks to prune (assuming ~1 MB per block on average) */
  uint32_t blocks_to_prune = (uint32_t)(excess_bytes / (1024ULL * 1024ULL)) + 100;

  /* Get current pruned height and calculate target */
  uint32_t current_pruned_height = node_get_pruned_height(node);
  uint32_t target_height = current_pruned_height + blocks_to_prune;

  /* Perform pruning */
  node_prune_blocks(node, target_height);

  return ECHO_OK;
}
