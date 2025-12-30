/**
 * Bitcoin Echo — Peer Discovery
 *
 * This module implements peer discovery mechanisms:
 * - DNS seed queries for bootstrap
 * - Hardcoded seed addresses as fallback
 * - addr/getaddr message handling for peer address relay
 * - Peer address storage and selection
 *
 * Peer discovery is essential for joining the Bitcoin network. A new node
 * uses DNS seeds or hardcoded addresses to find initial peers, then learns
 * about additional peers through addr messages.
 */

#ifndef ECHO_DISCOVERY_H
#define ECHO_DISCOVERY_H

#include "echo_types.h"
#include "protocol.h"
#include <stdint.h>

/* Maximum peer addresses to store (matches Bitcoin Core's addrman capacity) */
#define MAX_PEER_ADDRESSES 65536

/* Maximum addresses per DNS seed query */
#define MAX_DNS_RESULTS 128

/* Minimum time between addr broadcasts (24 hours in milliseconds) */
#define ADDR_BROADCAST_INTERVAL_MS (24ULL * 60 * 60 * 1000)

/* Maximum age for peer address to be considered fresh (3 hours in seconds) */
#define ADDR_FRESH_THRESHOLD_SEC (3ULL * 60 * 60)

/* Maximum age for peer address to be considered valid (7 days in seconds) */
#define ADDR_MAX_AGE_SEC (7ULL * 24 * 60 * 60)

/* IBD ban duration - addresses that self-close during IBD are banned for 1 hour
 * This prevents wasting connection slots on pruned nodes that can't serve
 * historical blocks */
#define IBD_BAN_DURATION_MS (60ULL * 60 * 1000)

/* Minimum connection duration to avoid IBD ban (2 minutes)
 * Peers that disconnect themselves within this window get banned */
#define IBD_BAN_MIN_DURATION_MS (2ULL * 60 * 1000)

/**
 * Network type for seed selection
 */
typedef enum {
  NETWORK_MAINNET,
  NETWORK_TESTNET,
  NETWORK_REGTEST
} network_type_t;

/**
 * Peer address entry
 *
 * Stores information about a known peer address.
 */
typedef struct {
  net_addr_t addr;       /* Network address with timestamp */
  uint64_t last_try;     /* Last connection attempt (plat_time_ms) */
  uint64_t last_success; /* Last successful connection (plat_time_ms) */
  uint64_t ibd_ban_until; /* IBD ban expiry time (plat_time_ms), 0 = not banned */
  uint32_t attempts;     /* Number of connection attempts */
  echo_bool_t in_use;    /* Whether address is currently being used */
  echo_bool_t reachable; /* Whether we've successfully connected before */
  uint8_t source;        /* How we learned about this address */
} peer_addr_entry_t;

/* Address source flags */
#define ADDR_SOURCE_DNS_SEED 0x01
#define ADDR_SOURCE_HARDCODED 0x02
#define ADDR_SOURCE_ADDR_MSG 0x04
#define ADDR_SOURCE_MANUAL 0x08

/**
 * Peer address manager
 *
 * Manages known peer addresses and selection.
 */
typedef struct {
  peer_addr_entry_t addresses[MAX_PEER_ADDRESSES];
  size_t count;                 /* Number of known addresses */
  uint64_t last_addr_broadcast; /* Last time we sent addr to peers */
  network_type_t network;       /* Which network we're on */
} peer_addr_manager_t;

/**
 * Initialize peer address manager.
 *
 * Parameters:
 *   manager - Address manager to initialize
 *   network - Network type (mainnet/testnet/regtest)
 */
void discovery_init(peer_addr_manager_t *manager, network_type_t network);

/**
 * Query DNS seeds for peer addresses.
 *
 * Queries all DNS seeds for the configured network and adds results to
 * address manager.
 *
 * This function uses the platform DNS resolution API (plat_dns_resolve).
 * DNS seeds are only available for mainnet and testnet, not regtest.
 *
 * Parameters:
 *   manager - Address manager to populate
 *
 * Returns:
 *   Number of addresses discovered from DNS seeds
 *   0 if DNS resolution fails or network doesn't have DNS seeds
 */
size_t discovery_query_dns_seeds(peer_addr_manager_t *manager);

/**
 * Add hardcoded seed addresses.
 *
 * Adds hardcoded IP addresses as fallback when DNS seeds are unavailable.
 * These addresses are embedded in the binary and represent known stable nodes.
 *
 * Parameters:
 *   manager - Address manager to populate
 *
 * Returns:
 *   Number of addresses added
 */
size_t discovery_add_hardcoded_seeds(peer_addr_manager_t *manager);

/**
 * Add peer address from addr message.
 *
 * Validates and adds address to manager. Rejects addresses that are:
 * - Too old (timestamp more than 10 minutes in future or very old)
 * - Invalid (0.0.0.0, loopback, etc.)
 * - Duplicate
 *
 * Parameters:
 *   manager - Address manager
 *   addr    - Network address to add
 *
 * Returns:
 *   ECHO_SUCCESS if added
 *   ECHO_ERR_INVALID if address is invalid
 *   ECHO_ERR_FULL if address manager is full
 *   ECHO_ERR_EXISTS if address already known
 */
echo_result_t discovery_add_address(peer_addr_manager_t *manager,
                                    const net_addr_t *addr);

/**
 * Add multiple addresses from addr message.
 *
 * Bulk version of discovery_add_address().
 *
 * Parameters:
 *   manager   - Address manager
 *   addresses - Array of network addresses
 *   count     - Number of addresses in array
 *
 * Returns:
 *   Number of addresses successfully added
 */
size_t discovery_add_addresses(peer_addr_manager_t *manager,
                               const net_addr_t *addresses, size_t count);

/**
 * Select addresses to send in addr message.
 *
 * Selects up to max_count fresh addresses to advertise to a peer.
 * Prioritizes recently seen, reachable addresses.
 *
 * Parameters:
 *   manager   - Address manager
 *   out_addrs - Output array (must hold max_count entries)
 *   max_count - Maximum number of addresses to return
 *
 * Returns:
 *   Number of addresses written to out_addrs
 */
size_t
discovery_select_addresses_to_advertise(const peer_addr_manager_t *manager,
                                        net_addr_t *out_addrs,
                                        size_t max_count);

/**
 * Select address for outbound connection.
 *
 * Chooses the best address for a new outbound connection.
 * Prioritizes:
 * - Addresses we haven't tried recently
 * - Addresses that worked before
 * - Fresh addresses
 *
 * Parameters:
 *   manager - Address manager
 *   out_addr - Output: selected address
 *
 * Returns:
 *   ECHO_SUCCESS if address selected
 *   ECHO_ERR_NOT_FOUND if no suitable addresses available
 */
echo_result_t discovery_select_outbound_address(peer_addr_manager_t *manager,
                                                net_addr_t *out_addr);

/**
 * Mark address as currently in use.
 *
 * Prevents selecting the same address for multiple simultaneous connections.
 *
 * Parameters:
 *   manager - Address manager
 *   addr    - Address to mark
 */
void discovery_mark_address_in_use(peer_addr_manager_t *manager,
                                   const net_addr_t *addr);

/**
 * Mark address as no longer in use.
 *
 * Parameters:
 *   manager - Address manager
 *   addr    - Address to unmark
 *   success - Whether connection was successful
 */
void discovery_mark_address_free(peer_addr_manager_t *manager,
                                 const net_addr_t *addr, echo_bool_t success);

/**
 * Mark connection attempt to address.
 *
 * Updates last_try timestamp and increments attempt counter.
 *
 * Parameters:
 *   manager - Address manager
 *   addr    - Address that was attempted
 */
void discovery_mark_attempt(peer_addr_manager_t *manager,
                            const net_addr_t *addr);

/**
 * Mark successful connection to address.
 *
 * Updates last_success timestamp and sets reachable flag.
 *
 * Parameters:
 *   manager - Address manager
 *   addr    - Address that succeeded
 */
void discovery_mark_success(peer_addr_manager_t *manager,
                            const net_addr_t *addr);

/**
 * Mark address as IBD-banned.
 *
 * Called when a peer self-closes (PEER_CLOSED) during IBD after a short
 * connection duration. These are typically pruned nodes that can't serve
 * historical blocks. The ban expires after IBD_BAN_DURATION_MS.
 *
 * Parameters:
 *   manager - Address manager
 *   addr    - Address to ban
 */
void discovery_mark_ibd_banned(peer_addr_manager_t *manager,
                               const net_addr_t *addr);

/**
 * Check if address is IBD-banned.
 *
 * Returns true if the address has an active IBD ban (ban_until > now).
 *
 * Parameters:
 *   manager - Address manager
 *   addr    - Address to check
 *
 * Returns:
 *   true if address is currently IBD-banned
 */
echo_bool_t discovery_is_ibd_banned(const peer_addr_manager_t *manager,
                                    const net_addr_t *addr);

/**
 * Check if address is valid for connection.
 *
 * Validates that address is:
 * - Not loopback (unless regtest)
 * - Not unspecified (0.0.0.0 or ::)
 * - Not multicast
 * - Has valid port
 *
 * Parameters:
 *   manager - Address manager (for network type)
 *   addr    - Address to validate
 *
 * Returns:
 *   true if address is valid for outbound connection
 */
echo_bool_t discovery_is_address_valid(const peer_addr_manager_t *manager,
                                       const net_addr_t *addr);

/**
 * Get count of known addresses.
 *
 * Returns:
 *   Number of addresses in manager
 */
size_t discovery_get_address_count(const peer_addr_manager_t *manager);

/**
 * Get count of reachable addresses.
 *
 * Returns:
 *   Number of addresses we've successfully connected to before
 */
size_t discovery_get_reachable_count(const peer_addr_manager_t *manager);

/**
 * Parse IPv4 address string to net_addr_t.
 *
 * Converts an IPv4 address string (e.g., "192.168.1.1") and port
 * to a net_addr_t structure with IPv6-mapped IPv4 format.
 *
 * Parameters:
 *   ip_str   - IPv4 address as string (e.g., "192.168.1.1")
 *   port     - Port number
 *   out_addr - Output: parsed address
 *
 * Returns:
 *   ECHO_SUCCESS if parsed successfully
 *   ECHO_ERR_INVALID if address is malformed
 */
echo_result_t discovery_parse_address(const char *ip_str, uint16_t port,
                                      net_addr_t *out_addr);

#endif /* ECHO_DISCOVERY_H */
