/**
 * Bitcoin Echo — Inventory and Data Relay Implementation
 */

#include "relay.h"
#include "block.h"
#include "echo_types.h"
#include "peer.h"
#include "platform.h"
#include "protocol.h"
#include "tx.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum tracked peers */
#define MAX_PEERS 125

/* Maximum banned addresses */
#define MAX_BANS 1000

/* Inventory item expiry time (10 minutes) */
#define INVENTORY_EXPIRY_MS (10ULL * 60ULL * 1000ULL)

/**
 * Banned address entry
 */
typedef struct {
  char address[64];    /* IP address */
  uint64_t ban_until;  /* Ban expires at this time (plat_time_ms) */
  ban_reason_t reason; /* Reason for ban */
  echo_bool_t active;  /* Whether this slot is in use */
} ban_entry_t;

/**
 * Peer tracking entry
 *
 * Associates a peer pointer with its inventory state.
 */
typedef struct {
  peer_t *peer;               /* Peer pointer */
  peer_inventory_t inventory; /* Inventory state */
  echo_bool_t active;         /* Whether this slot is in use */
} peer_entry_t;

/**
 * Relay manager internal structure
 */
struct relay_manager {
  /* Callbacks for storage/validation */
  relay_callbacks_t callbacks;

  /* Tracked peers */
  peer_entry_t peers[MAX_PEERS];
  size_t peer_count;

  /* Banned addresses */
  ban_entry_t bans[MAX_BANS];
  size_t ban_count;
};

/**
 * Find peer entry by peer pointer.
 *
 * Returns NULL if not found.
 */
static peer_entry_t *find_peer(relay_manager_t *mgr, peer_t *peer) {
  for (size_t i = 0; i < MAX_PEERS; i++) {
    if (mgr->peers[i].active && mgr->peers[i].peer == peer) {
      return &mgr->peers[i];
    }
  }
  return NULL;
}

/**
 * Find ban entry by address.
 *
 * Returns NULL if not found.
 */
static ban_entry_t *find_ban(relay_manager_t *mgr, const char *address) {
  for (size_t i = 0; i < MAX_BANS; i++) {
    if (mgr->bans[i].active && strcmp(mgr->bans[i].address, address) == 0) {
      return &mgr->bans[i];
    }
  }
  return NULL;
}

/**
 * Check if peer has announced this inventory item.
 */
static echo_bool_t peer_has_inventory(const peer_inventory_t *inv,
                                      const hash256_t *hash, uint32_t type) {
  for (size_t i = 0; i < inv->count; i++) {
    if (inv->items[i].type == type &&
        memcmp(&inv->items[i].hash, hash, sizeof(hash256_t)) == 0) {
      return ECHO_TRUE;
    }
  }
  return ECHO_FALSE;
}

/**
 * Add inventory item to peer's announced items.
 */
static void peer_add_inventory(peer_inventory_t *inv, const hash256_t *hash,
                               uint32_t type) {
  /* Don't add if already present */
  if (peer_has_inventory(inv, hash, type)) {
    return;
  }

  /* If at capacity, remove oldest item */
  if (inv->count >= MAX_PEER_INVENTORY) {
    /* Shift all items down */
    memmove(&inv->items[0], &inv->items[1],
            (MAX_PEER_INVENTORY - 1) * sizeof(inventory_item_t));
    inv->count = MAX_PEER_INVENTORY - 1;
  }

  /* Add new item */
  inv->items[inv->count].hash = *hash;
  inv->items[inv->count].type = type;
  inv->items[inv->count].time = plat_time_ms();
  inv->count++;
}

/**
 * Check and update rate limiting for inv messages.
 *
 * Returns true if rate limit exceeded.
 */
static echo_bool_t check_inv_rate_limit(peer_inventory_t *inv) {
  uint64_t now = plat_time_ms();

  /* Reset counter if more than 1 second since last message */
  if (now - inv->last_inv_time > 1000) {
    inv->inv_count_recent = 0;
  }

  inv->last_inv_time = now;
  inv->inv_count_recent++;

  return inv->inv_count_recent > MAX_INV_PER_SECOND ? ECHO_TRUE : ECHO_FALSE;
}

/**
 * Check and update rate limiting for getdata messages.
 *
 * Returns true if rate limit exceeded.
 */
static echo_bool_t check_getdata_rate_limit(peer_inventory_t *inv) {
  uint64_t now = plat_time_ms();

  /* Reset counter if more than 1 second since last message */
  if (now - inv->last_getdata_time > 1000) {
    inv->getdata_count_recent = 0;
  }

  inv->last_getdata_time = now;
  inv->getdata_count_recent++;

  return inv->getdata_count_recent > MAX_GETDATA_PER_SECOND ? ECHO_TRUE
                                                            : ECHO_FALSE;
}

/* ========== Public API ========== */

relay_manager_t *relay_init(const relay_callbacks_t *callbacks) {
  if (callbacks == NULL) {
    return NULL;
  }

  relay_manager_t *mgr = calloc(1, sizeof(relay_manager_t));
  if (mgr == NULL) {
    return NULL;
  }

  mgr->callbacks = *callbacks;
  mgr->peer_count = 0;
  mgr->ban_count = 0;

  /* Initialize all entries as inactive */
  for (size_t i = 0; i < MAX_PEERS; i++) {
    mgr->peers[i].active = ECHO_FALSE;
  }
  for (size_t i = 0; i < MAX_BANS; i++) {
    mgr->bans[i].active = ECHO_FALSE;
  }

  return mgr;
}

void relay_destroy(relay_manager_t *mgr) {
  if (mgr == NULL) {
    return;
  }
  free(mgr);
}

void relay_add_peer(relay_manager_t *mgr, peer_t *peer) {
  if (mgr == NULL || peer == NULL) {
    return;
  }

  /* Find free slot */
  for (size_t i = 0; i < MAX_PEERS; i++) {
    if (!mgr->peers[i].active) {
      mgr->peers[i].peer = peer;
      mgr->peers[i].active = ECHO_TRUE;

      /* Initialize inventory state */
      memset(&mgr->peers[i].inventory, 0, sizeof(peer_inventory_t));

      mgr->peer_count++;
      return;
    }
  }
}

void relay_remove_peer(relay_manager_t *mgr, peer_t *peer) {
  if (mgr == NULL || peer == NULL) {
    return;
  }

  peer_entry_t *entry = find_peer(mgr, peer);
  if (entry != NULL) {
    entry->active = ECHO_FALSE;
    entry->peer = NULL;
    if (mgr->peer_count > 0) {
      mgr->peer_count--;
    }
  }
}

echo_result_t relay_handle_inv(relay_manager_t *mgr, peer_t *peer,
                               const msg_inv_t *msg) {
  if (mgr == NULL || peer == NULL || msg == NULL) {
    return ECHO_ERR_INVALID;
  }

  /* Find peer entry */
  peer_entry_t *entry = find_peer(mgr, peer);
  if (entry == NULL) {
    return ECHO_ERR_INVALID;
  }

  /* Check rate limit */
  if (check_inv_rate_limit(&entry->inventory)) {
    return ECHO_ERR_RATE_LIMIT;
  }

  /* Validate message */
  if (msg->count == 0 || msg->count > MAX_INV_ENTRIES) {
    return ECHO_ERR_INVALID;
  }

  /* Build getdata request for items we don't have */
  msg_getdata_t getdata;
  getdata.count = 0;
  getdata.inventory = calloc(
      msg->count, sizeof(inv_vector_t)); // NOLINT(bugprone-sizeof-expression)
  if (getdata.inventory == NULL) {
    return ECHO_ERR_MEMORY;
  }

  for (size_t i = 0; i < msg->count; i++) {
    const inv_vector_t *inv = &msg->inventory[i];

    /* Record that peer has this item */
    peer_add_inventory(&entry->inventory, &inv->hash, inv->type);

    /* Check if we already have this item */
    echo_bool_t have_item = ECHO_FALSE;

    if (inv->type == INV_BLOCK || inv->type == INV_WITNESS_BLOCK) {
      /* Check if we have the block */
      block_t block;
      if (mgr->callbacks.get_block(&inv->hash, &block, mgr->callbacks.ctx) ==
          ECHO_SUCCESS) {
        have_item = ECHO_TRUE;
      }
    } else if (inv->type == INV_TX || inv->type == INV_WITNESS_TX) {
      /* Check if we have the transaction */
      tx_t tx;
      if (mgr->callbacks.get_tx(&inv->hash, &tx, mgr->callbacks.ctx) ==
          ECHO_SUCCESS) {
        have_item = ECHO_TRUE;
      }
    }

    /* If we don't have it, request it */
    if (!have_item) {
      getdata.inventory[getdata.count++] = *inv;
    }
  }

  /* Queue getdata if we need anything */
  echo_result_t result = ECHO_SUCCESS;
  if (getdata.count > 0) {
    msg_t msg_out;
    msg_out.type = MSG_GETDATA;
    msg_out.payload.getdata = getdata;
    result = peer_queue_message(peer, &msg_out);
  }

  free(getdata.inventory);
  return result;
}

echo_result_t relay_handle_getdata(relay_manager_t *mgr, peer_t *peer,
                                   const msg_getdata_t *msg) {
  if (mgr == NULL || peer == NULL || msg == NULL) {
    return ECHO_ERR_INVALID;
  }

  /* Find peer entry */
  peer_entry_t *entry = find_peer(mgr, peer);
  if (entry == NULL) {
    return ECHO_ERR_INVALID;
  }

  /* Check rate limit */
  if (check_getdata_rate_limit(&entry->inventory)) {
    return ECHO_ERR_RATE_LIMIT;
  }

  /* Validate message */
  if (msg->count == 0 || msg->count > MAX_GETDATA_BATCH) {
    return ECHO_ERR_INVALID;
  }

  /* Track items we don't have (for notfound message) */
  msg_notfound_t notfound;
  notfound.count = 0;
  notfound.inventory = calloc(
      msg->count, sizeof(inv_vector_t)); // NOLINT(bugprone-sizeof-expression)
  if (notfound.inventory == NULL) {
    return ECHO_ERR_MEMORY;
  }

  /* Process each requested item */
  for (size_t i = 0; i < msg->count; i++) {
    const inv_vector_t *inv = &msg->inventory[i];
    echo_result_t result = ECHO_ERR_NOT_FOUND;

    if (inv->type == INV_BLOCK || inv->type == INV_WITNESS_BLOCK) {
      /* Retrieve and send block */
      block_t block;
      result = mgr->callbacks.get_block(&inv->hash, &block, mgr->callbacks.ctx);
      if (result == ECHO_SUCCESS) {
        msg_t msg_out;
        msg_out.type = MSG_BLOCK;
        msg_out.payload.block.block = block;
        peer_queue_message(peer, &msg_out);
      }
    } else if (inv->type == INV_TX || inv->type == INV_WITNESS_TX) {
      /* Retrieve and send transaction */
      tx_t tx;
      result = mgr->callbacks.get_tx(&inv->hash, &tx, mgr->callbacks.ctx);
      if (result == ECHO_SUCCESS) {
        msg_t msg_out;
        msg_out.type = MSG_TX;
        msg_out.payload.tx.tx = tx;
        peer_queue_message(peer, &msg_out);
      }
    }

    /* If we don't have it, add to notfound */
    if (result != ECHO_SUCCESS) {
      notfound.inventory[notfound.count++] = *inv;
    }
  }

  /* Send notfound if we're missing any items */
  if (notfound.count > 0) {
    msg_t msg_out;
    msg_out.type = MSG_NOTFOUND;
    msg_out.payload.notfound = notfound;
    peer_queue_message(peer, &msg_out);
  }

  free(notfound.inventory);
  return ECHO_SUCCESS;
}

echo_result_t relay_handle_block(relay_manager_t *mgr, peer_t *peer,
                                 const block_t *block) {
  if (mgr == NULL || peer == NULL || block == NULL) {
    return ECHO_ERR_INVALID;
  }

  /* Process block through consensus validation */
  echo_result_t result =
      mgr->callbacks.process_block(block, mgr->callbacks.ctx);

  if (result == ECHO_ERR_INVALID) {
    /* Invalid block — increase ban score */
    peer_entry_t *entry = find_peer(mgr, peer);
    if (entry != NULL) {
      entry->inventory.ban_score += 100;
    }
    return ECHO_ERR_INVALID;
  }

  if (result == ECHO_SUCCESS) {
    /* Valid new block — relay to all peers except sender */
    hash256_t block_hash;
    block_header_hash(&block->header, &block_hash);

    for (size_t i = 0; i < MAX_PEERS; i++) {
      if (mgr->peers[i].active && mgr->peers[i].peer != peer &&
          peer_is_ready(mgr->peers[i].peer)) {

        /* Don't relay if peer already has it */
        if (!peer_has_inventory(&mgr->peers[i].inventory, &block_hash,
                                INV_BLOCK)) {
          /* Send inv */
          msg_inv_t inv;
          inv.count = 1;
          inv_vector_t inv_vec;
          inv_vec.type = INV_BLOCK;
          inv_vec.hash = block_hash;
          inv.inventory = &inv_vec;

          msg_t msg_out;
          msg_out.type = MSG_INV;
          msg_out.payload.inv = inv;
          peer_queue_message(mgr->peers[i].peer, &msg_out);

          /* Mark that we've announced it to this peer */
          peer_add_inventory(&mgr->peers[i].inventory, &block_hash, INV_BLOCK);
        }
      }
    }
  }

  return result;
}

echo_result_t relay_handle_tx(relay_manager_t *mgr, peer_t *peer,
                              const tx_t *tx) {
  if (mgr == NULL || peer == NULL || tx == NULL) {
    return ECHO_ERR_INVALID;
  }

  /* Process transaction through validation and mempool */
  echo_result_t result = mgr->callbacks.process_tx(tx, mgr->callbacks.ctx);

  if (result == ECHO_ERR_INVALID) {
    /* Invalid transaction — increase ban score */
    peer_entry_t *entry = find_peer(mgr, peer);
    if (entry != NULL) {
      entry->inventory.ban_score += 10;
    }
    return ECHO_ERR_INVALID;
  }

  if (result == ECHO_SUCCESS) {
    /* Valid new transaction — relay to all peers except sender */
    hash256_t tx_hash;
    tx_compute_txid(tx, &tx_hash);

    for (size_t i = 0; i < MAX_PEERS; i++) {
      if (mgr->peers[i].active && mgr->peers[i].peer != peer &&
          peer_is_ready(mgr->peers[i].peer)) {

        /* Check if peer wants tx relay and doesn't already have it */
        if (mgr->peers[i].peer->relay &&
            !peer_has_inventory(&mgr->peers[i].inventory, &tx_hash, INV_TX)) {
          /* Send inv */
          msg_inv_t inv;
          inv.count = 1;
          inv_vector_t inv_vec;
          inv_vec.type = INV_TX;
          inv_vec.hash = tx_hash;
          inv.inventory = &inv_vec;

          msg_t msg_out;
          msg_out.type = MSG_INV;
          msg_out.payload.inv = inv;
          peer_queue_message(mgr->peers[i].peer, &msg_out);

          /* Mark that we've announced it to this peer */
          peer_add_inventory(&mgr->peers[i].inventory, &tx_hash, INV_TX);
        }
      }
    }
  }

  return result;
}

void relay_announce_block(relay_manager_t *mgr, const hash256_t *block_hash) {
  if (mgr == NULL || block_hash == NULL) {
    return;
  }

  /* Announce to all ready peers */
  for (size_t i = 0; i < MAX_PEERS; i++) {
    if (mgr->peers[i].active && peer_is_ready(mgr->peers[i].peer)) {

      /* Don't announce if peer already knows */
      if (!peer_has_inventory(&mgr->peers[i].inventory, block_hash,
                              INV_BLOCK)) {
        /* Send inv */
        msg_inv_t inv;
        inv.count = 1;
        inv_vector_t inv_vec;
        inv_vec.type = INV_BLOCK;
        inv_vec.hash = *block_hash;
        inv.inventory = &inv_vec;

        msg_t msg_out;
        msg_out.type = MSG_INV;
        msg_out.payload.inv = inv;
        peer_queue_message(mgr->peers[i].peer, &msg_out);

        /* Mark that we've announced it */
        peer_add_inventory(&mgr->peers[i].inventory, block_hash, INV_BLOCK);
      }
    }
  }
}

void relay_announce_tx(relay_manager_t *mgr, const hash256_t *tx_hash) {
  if (mgr == NULL || tx_hash == NULL) {
    return;
  }

  /* Announce to all ready peers that want tx relay */
  for (size_t i = 0; i < MAX_PEERS; i++) {
    if (mgr->peers[i].active && peer_is_ready(mgr->peers[i].peer) &&
        mgr->peers[i].peer->relay) {

      /* Don't announce if peer already knows */
      if (!peer_has_inventory(&mgr->peers[i].inventory, tx_hash, INV_TX)) {
        /* Send inv */
        msg_inv_t inv;
        inv.count = 1;
        inv_vector_t inv_vec;
        inv_vec.type = INV_TX;
        inv_vec.hash = *tx_hash;
        inv.inventory = &inv_vec;

        msg_t msg_out;
        msg_out.type = MSG_INV;
        msg_out.payload.inv = inv;
        peer_queue_message(mgr->peers[i].peer, &msg_out);

        /* Mark that we've announced it */
        peer_add_inventory(&mgr->peers[i].inventory, tx_hash, INV_TX);
      }
    }
  }
}

echo_bool_t relay_increase_ban_score(relay_manager_t *mgr, peer_t *peer,
                                     int amount, ban_reason_t reason) {
  if (mgr == NULL || peer == NULL) {
    return ECHO_FALSE;
  }

  peer_entry_t *entry = find_peer(mgr, peer);
  if (entry == NULL) {
    return ECHO_FALSE;
  }

  entry->inventory.ban_score += amount;

  if (entry->inventory.ban_score >= BAN_THRESHOLD) {
    /* Ban this peer's address */
    relay_ban_address(mgr, peer->address, BAN_DURATION_MS, reason);
    return ECHO_TRUE;
  }

  return ECHO_FALSE;
}

echo_bool_t relay_is_banned(relay_manager_t *mgr, const char *address) {
  if (mgr == NULL || address == NULL) {
    return ECHO_FALSE;
  }

  ban_entry_t *ban = find_ban(mgr, address);
  if (ban == NULL) {
    return ECHO_FALSE;
  }

  uint64_t now = plat_time_ms();
  if (now >= ban->ban_until) {
    /* Ban expired */
    ban->active = ECHO_FALSE;
    if (mgr->ban_count > 0) {
      mgr->ban_count--;
    }
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}

void relay_ban_address(relay_manager_t *mgr, const char *address,
                       uint64_t duration_ms, ban_reason_t reason) {
  if (mgr == NULL || address == NULL) {
    return;
  }

  /* Check if already banned */
  ban_entry_t *ban = find_ban(mgr, address);
  if (ban != NULL) {
    /* Update existing ban */
    uint64_t duration = duration_ms > 0 ? duration_ms : BAN_DURATION_MS;
    ban->ban_until = plat_time_ms() + duration;
    ban->reason = reason;
    return;
  }

  /* Find free slot */
  for (size_t i = 0; i < MAX_BANS; i++) {
    if (!mgr->bans[i].active) {
      strncpy(mgr->bans[i].address, address, sizeof(mgr->bans[i].address) - 1);
      mgr->bans[i].address[sizeof(mgr->bans[i].address) - 1] = '\0';

      uint64_t duration = duration_ms > 0 ? duration_ms : BAN_DURATION_MS;
      mgr->bans[i].ban_until = plat_time_ms() + duration;
      mgr->bans[i].reason = reason;
      mgr->bans[i].active = ECHO_TRUE;

      mgr->ban_count++;
      return;
    }
  }
}

void relay_unban_address(relay_manager_t *mgr, const char *address) {
  if (mgr == NULL || address == NULL) {
    return;
  }

  ban_entry_t *ban = find_ban(mgr, address);
  if (ban != NULL) {
    ban->active = ECHO_FALSE;
    if (mgr->ban_count > 0) {
      mgr->ban_count--;
    }
  }
}

void relay_cleanup(relay_manager_t *mgr) {
  if (mgr == NULL) {
    return;
  }

  uint64_t now = plat_time_ms();

  /* Clean up expired bans */
  for (size_t i = 0; i < MAX_BANS; i++) {
    if (mgr->bans[i].active && now >= mgr->bans[i].ban_until) {
      mgr->bans[i].active = ECHO_FALSE;
      if (mgr->ban_count > 0) {
        mgr->ban_count--;
      }
    }
  }

  /* Clean up stale inventory items */
  for (size_t i = 0; i < MAX_PEERS; i++) {
    if (mgr->peers[i].active) {
      peer_inventory_t *inv = &mgr->peers[i].inventory;

      /* Remove items older than INVENTORY_EXPIRY_MS */
      size_t write_idx = 0;
      for (size_t read_idx = 0; read_idx < inv->count; read_idx++) {
        if (now - inv->items[read_idx].time < INVENTORY_EXPIRY_MS) {
          if (write_idx != read_idx) {
            inv->items[write_idx] = inv->items[read_idx];
          }
          write_idx++;
        }
      }
      inv->count = write_idx;
    }
  }
}

const char *relay_ban_reason_string(ban_reason_t reason) {
  switch (reason) {
  case BAN_REASON_NONE:
    return "NONE";
  case BAN_REASON_MANUAL:
    return "MANUAL";
  case BAN_REASON_PROTOCOL_VIOLATION:
    return "PROTOCOL_VIOLATION";
  case BAN_REASON_EXCESSIVE_INV:
    return "EXCESSIVE_INV";
  case BAN_REASON_EXCESSIVE_GETDATA:
    return "EXCESSIVE_GETDATA";
  case BAN_REASON_INVALID_DATA:
    return "INVALID_DATA";
  case BAN_REASON_MISBEHAVING:
    return "MISBEHAVING";
  default:
    return "UNKNOWN";
  }
}
