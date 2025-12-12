/**
 * Bitcoin Echo — Inventory and Data Relay
 *
 * This module handles the inv/getdata protocol for propagating blocks and
 * transactions across the P2P network:
 *
 * - Inventory tracking (what each peer knows)
 * - Inventory announcements (inv messages)
 * - Data requests (getdata messages)
 * - Block and transaction reception
 * - Relay to connected peers
 * - DoS prevention (rate limiting, peer banning)
 *
 * Relay policy: relay everything that passes validation and basic policy checks.
 * Sophisticated relay policies are explicitly out of scope.
 */

#ifndef ECHO_RELAY_H
#define ECHO_RELAY_H

#include "block.h"
#include "echo_types.h"
#include "peer.h"
#include "protocol.h"
#include "tx.h"
#include <stdint.h>

/* Maximum inventory items to track per peer */
#define MAX_PEER_INVENTORY 50000

/* Maximum getdata requests we'll handle at once from a peer */
#define MAX_GETDATA_BATCH 1000

/* Rate limiting: max inv messages per peer per second */
#define MAX_INV_PER_SECOND 100

/* Rate limiting: max getdata requests per peer per second */
#define MAX_GETDATA_PER_SECOND 1000

/* Ban score threshold (peer gets banned at this score) */
#define BAN_THRESHOLD 100

/* Ban duration in milliseconds (24 hours) */
#define BAN_DURATION_MS (24ULL * 60ULL * 60ULL * 1000ULL)

/**
 * Ban reason
 */
typedef enum {
  BAN_REASON_NONE = 0,
  BAN_REASON_MANUAL,           /* Manually banned */
  BAN_REASON_PROTOCOL_VIOLATION, /* Protocol violation */
  BAN_REASON_EXCESSIVE_INV,    /* Too many inv messages */
  BAN_REASON_EXCESSIVE_GETDATA, /* Too many getdata requests */
  BAN_REASON_INVALID_DATA,     /* Sent invalid block/tx */
  BAN_REASON_MISBEHAVING       /* General misbehavior */
} ban_reason_t;

/**
 * Peer inventory tracking
 *
 * Tracks what inventory items (blocks/txs) each peer has announced.
 * Used to avoid requesting the same item multiple times.
 */
typedef struct {
  hash256_t hash;  /* Item hash */
  uint32_t type;   /* INV_TX, INV_BLOCK, etc. */
  uint64_t time;   /* When we learned about this (plat_time_ms) */
} inventory_item_t;

/**
 * Per-peer inventory state
 */
typedef struct {
  /* Inventory items this peer has announced */
  inventory_item_t items[MAX_PEER_INVENTORY];
  size_t count;

  /* Rate limiting */
  uint64_t last_inv_time;     /* Last time we received inv */
  size_t inv_count_recent;    /* Number of inv in last second */
  uint64_t last_getdata_time; /* Last time we received getdata */
  size_t getdata_count_recent; /* Number of getdata in last second */

  /* Misbehavior tracking */
  int ban_score; /* Accumulated ban score */
} peer_inventory_t;

/**
 * Relay manager
 *
 * Coordinates relay across all connected peers.
 */
typedef struct relay_manager relay_manager_t;

/**
 * Callbacks for relay manager
 *
 * The relay manager calls these functions when it needs to:
 * - Retrieve blocks/transactions from storage
 * - Validate received blocks/transactions
 * - Announce new blocks/transactions to the application layer
 */
typedef struct {
  /**
   * Get block by hash from storage.
   *
   * Returns:
   *   ECHO_SUCCESS if block found (*block_out is valid)
   *   ECHO_ERR_NOT_FOUND if block not found
   */
  echo_result_t (*get_block)(const hash256_t *hash, block_t *block_out,
                             void *ctx);

  /**
   * Get transaction by hash from mempool or confirmed blocks.
   *
   * Returns:
   *   ECHO_SUCCESS if transaction found (*tx_out is valid)
   *   ECHO_ERR_NOT_FOUND if transaction not found
   */
  echo_result_t (*get_tx)(const hash256_t *hash, tx_t *tx_out, void *ctx);

  /**
   * Validate and process received block.
   *
   * Returns:
   *   ECHO_SUCCESS if block is valid
   *   ECHO_ERR_INVALID if block is invalid
   */
  echo_result_t (*process_block)(const block_t *block, void *ctx);

  /**
   * Validate and add transaction to mempool.
   *
   * Returns:
   *   ECHO_SUCCESS if transaction is valid and added
   *   ECHO_ERR_INVALID if transaction is invalid
   *   ECHO_ERR_DUPLICATE if transaction already in mempool
   */
  echo_result_t (*process_tx)(const tx_t *tx, void *ctx);

  /* Context pointer passed to callbacks */
  void *ctx;
} relay_callbacks_t;

/**
 * Initialize relay manager.
 *
 * Parameters:
 *   callbacks - Callback functions for storage/validation
 *
 * Returns:
 *   Pointer to allocated relay manager, or NULL on failure.
 */
relay_manager_t *relay_init(const relay_callbacks_t *callbacks);

/**
 * Destroy relay manager and free resources.
 */
void relay_destroy(relay_manager_t *mgr);

/**
 * Add peer to relay tracking.
 *
 * Must be called when a peer completes handshake.
 *
 * Parameters:
 *   mgr  - Relay manager
 *   peer - Peer to add
 */
void relay_add_peer(relay_manager_t *mgr, peer_t *peer);

/**
 * Remove peer from relay tracking.
 *
 * Must be called when a peer disconnects.
 *
 * Parameters:
 *   mgr  - Relay manager
 *   peer - Peer to remove
 */
void relay_remove_peer(relay_manager_t *mgr, peer_t *peer);

/**
 * Handle received inv message.
 *
 * Processes inventory announcement from peer:
 * - Rate limit check
 * - Filter already-known items
 * - Queue getdata for interesting items
 *
 * Parameters:
 *   mgr  - Relay manager
 *   peer - Peer that sent the inv
 *   msg  - The inv message
 *
 * Returns:
 *   ECHO_SUCCESS if processed
 *   ECHO_ERR_RATE_LIMIT if peer exceeded rate limit
 *   ECHO_ERR_INVALID if message malformed
 */
echo_result_t relay_handle_inv(relay_manager_t *mgr, peer_t *peer,
                               const msg_inv_t *msg);

/**
 * Handle received getdata message.
 *
 * Processes data request from peer:
 * - Rate limit check
 * - Retrieve requested items from storage
 * - Queue block/tx messages to peer
 * - Send notfound for missing items
 *
 * Parameters:
 *   mgr  - Relay manager
 *   peer - Peer that sent the getdata
 *   msg  - The getdata message
 *
 * Returns:
 *   ECHO_SUCCESS if processed
 *   ECHO_ERR_RATE_LIMIT if peer exceeded rate limit
 *   ECHO_ERR_INVALID if message malformed
 */
echo_result_t relay_handle_getdata(relay_manager_t *mgr, peer_t *peer,
                                   const msg_getdata_t *msg);

/**
 * Handle received block message.
 *
 * Processes received block:
 * - Validate block
 * - Add to blockchain if valid
 * - Relay to other peers if new
 *
 * Parameters:
 *   mgr   - Relay manager
 *   peer  - Peer that sent the block
 *   block - The received block
 *
 * Returns:
 *   ECHO_SUCCESS if block processed successfully
 *   ECHO_ERR_INVALID if block is invalid (peer may be penalized)
 *   ECHO_ERR_DUPLICATE if block already known
 */
echo_result_t relay_handle_block(relay_manager_t *mgr, peer_t *peer,
                                 const block_t *block);

/**
 * Handle received tx message.
 *
 * Processes received transaction:
 * - Validate transaction
 * - Add to mempool if valid
 * - Relay to other peers if new
 *
 * Parameters:
 *   mgr  - Relay manager
 *   peer - Peer that sent the tx
 *   tx   - The received transaction
 *
 * Returns:
 *   ECHO_SUCCESS if transaction processed successfully
 *   ECHO_ERR_INVALID if transaction is invalid (peer may be penalized)
 *   ECHO_ERR_DUPLICATE if transaction already in mempool
 */
echo_result_t relay_handle_tx(relay_manager_t *mgr, peer_t *peer,
                              const tx_t *tx);

/**
 * Announce new block to all peers.
 *
 * Sends inv message with block hash to all connected, ready peers.
 *
 * Parameters:
 *   mgr       - Relay manager
 *   block_hash - Hash of the new block
 */
void relay_announce_block(relay_manager_t *mgr, const hash256_t *block_hash);

/**
 * Announce new transaction to all peers.
 *
 * Sends inv message with transaction hash to all connected, ready peers.
 *
 * Parameters:
 *   mgr    - Relay manager
 *   tx_hash - Hash of the new transaction
 */
void relay_announce_tx(relay_manager_t *mgr, const hash256_t *tx_hash);

/**
 * Increase peer's ban score.
 *
 * Adds to the peer's accumulated misbehavior score.
 * If score exceeds threshold, peer is banned.
 *
 * Parameters:
 *   mgr    - Relay manager
 *   peer   - Peer to penalize
 *   amount - Ban score to add
 *   reason - Reason for penalty
 *
 * Returns:
 *   true if peer should be banned (score exceeded threshold)
 */
echo_bool_t relay_increase_ban_score(relay_manager_t *mgr, peer_t *peer,
                                     int amount, ban_reason_t reason);

/**
 * Check if address is banned.
 *
 * Parameters:
 *   mgr     - Relay manager
 *   address - IP address to check
 *
 * Returns:
 *   true if address is currently banned
 */
echo_bool_t relay_is_banned(relay_manager_t *mgr, const char *address);

/**
 * Ban an address.
 *
 * Parameters:
 *   mgr      - Relay manager
 *   address  - IP address to ban
 *   duration_ms - Ban duration in milliseconds (0 for BAN_DURATION_MS)
 *   reason   - Ban reason
 */
void relay_ban_address(relay_manager_t *mgr, const char *address,
                       uint64_t duration_ms, ban_reason_t reason);

/**
 * Unban an address.
 *
 * Parameters:
 *   mgr     - Relay manager
 *   address - IP address to unban
 */
void relay_unban_address(relay_manager_t *mgr, const char *address);

/**
 * Clean up expired bans and stale inventory.
 *
 * Should be called periodically (e.g., once per minute).
 *
 * Parameters:
 *   mgr - Relay manager
 */
void relay_cleanup(relay_manager_t *mgr);

/**
 * Get human-readable string for ban reason.
 *
 * Returns:
 *   String representation of reason (e.g., "EXCESSIVE_INV")
 */
const char *relay_ban_reason_string(ban_reason_t reason);

#endif /* ECHO_RELAY_H */
