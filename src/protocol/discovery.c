/**
 * Bitcoin Echo — Peer Discovery Implementation
 */

#include "discovery.h"
#include "echo_types.h"
#include "log.h"
#include "platform.h"
#include "protocol.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* DNS seeds for mainnet (as of December 2025)
 * Ordered by reliability/speed based on testing. */
static const char *DNS_SEEDS_MAINNET[] = {
    "dnsseed.emzy.de",               /* Stephan Oeste - fast, reliable */
    "seed.bitcoin.wiz.biz",          /* Jason Maurice */
    "dnsseed.bluematt.me",           /* Matt Corallo */
    "seed.bitcoin.sprovoost.nl",     /* Sjors Provoost */
    "seed.btc.petertodd.net",        /* Peter Todd */
    "seed.mainnet.achownodes.xyz",   /* Ava Chow */
    "seed.bitcoin.jonasschnelli.ch", /* Jonas Schnelli */
    "seed.bitcoin.sipa.be",          /* Pieter Wuille */
    NULL};

/* DNS seeds for testnet (as of December 2025) */
static const char *DNS_SEEDS_TESTNET[] = {
    "testnet-seed.bitcoin.jonasschnelli.ch",
    "seed.tbtc.petertodd.net",
    "seed.testnet.bitcoin.sprovoost.nl",
    "testnet-seed.bluematt.me",
    "seed.testnet.achownodes.xyz",
    NULL};

/* Hardcoded seed addresses for mainnet (fallback)
 * These should be IP addresses of stable, long-running nodes.
 * Format: "ip:port"
 * Currently empty - DNS seeds are reliable enough. */
static const char *HARDCODED_SEEDS_MAINNET[] = {NULL};

/* Hardcoded seed addresses for testnet (currently empty) */
static const char *HARDCODED_SEEDS_TESTNET[] = {NULL};

/* Default port numbers */
#define DEFAULT_PORT_MAINNET 8333
#define DEFAULT_PORT_TESTNET 18333
#define DEFAULT_PORT_REGTEST 18444

/* Helper: Convert IPv4 to IPv6-mapped address */
static void ipv4_to_ipv6_mapped(const char *ipv4, uint8_t *ipv6) {
  /* IPv6-mapped IPv4: ::ffff:x.x.x.x */
  memset(ipv6, 0, 10);
  ipv6[10] = 0xff;
  ipv6[11] = 0xff;

  /* Parse IPv4 octets */
  unsigned int a = 0;
  unsigned int b = 0;
  unsigned int c = 0;
  unsigned int d = 0;
  /* Use sscanf with bounds checking - we only read 4 bounded integers */
  /* NOLINTBEGIN(cert-err34-c) - sscanf correct here: return value checked */
  int parsed = sscanf(ipv4, "%u.%u.%u.%u", &a, &b, &c, &d);
  log_info(LOG_COMP_NET, "ipv4_to_ipv6_mapped: input='%s' parsed=%d values=%u.%u.%u.%u",
           ipv4, parsed, a, b, c, d);
  if (parsed == 4) {
    /* NOLINTEND(cert-err34-c) */
    if (a <= 255 && b <= 255 && c <= 255 && d <= 255) {
      ipv6[12] = (uint8_t)a;
      ipv6[13] = (uint8_t)b;
      ipv6[14] = (uint8_t)c;
      ipv6[15] = (uint8_t)d;
      log_info(LOG_COMP_NET, "Successfully converted to IPv6-mapped: %d.%d.%d.%d",
               ipv6[12], ipv6[13], ipv6[14], ipv6[15]);
    } else {
      log_warn(LOG_COMP_NET, "IP octet out of range: %u.%u.%u.%u", a, b, c, d);
    }
  } else {
    log_warn(LOG_COMP_NET, "Failed to parse IPv4 address '%s' (parsed %d/4)", ipv4, parsed);
  }
}

/* Helper: Check if address is IPv4-mapped IPv6 */
static echo_bool_t is_ipv4_mapped(const uint8_t *ipv6) {
  return (memcmp(ipv6, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff",
                 12) == 0);
}

/* Helper: Check if address is loopback */
static echo_bool_t is_loopback(const uint8_t *ipv6) {
  /* IPv6 loopback: ::1 */
  static const uint8_t ipv6_loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                            0, 0, 0, 0, 0, 0, 0, 1};
  if (memcmp(ipv6, ipv6_loopback, 16) == 0) {
    return ECHO_TRUE;
  }

  /* IPv4-mapped loopback: ::ffff:127.x.x.x */
  if (is_ipv4_mapped(ipv6) && ipv6[12] == 127) {
    return ECHO_TRUE;
  }

  return ECHO_FALSE;
}

/* Helper: Check if address is unspecified (0.0.0.0 or ::) */
static echo_bool_t is_unspecified(const uint8_t *ipv6) {
  static const uint8_t zero[16] = {0};

  /* Check for all zeros (::) */
  if (memcmp(ipv6, zero, 16) == 0) {
    return ECHO_TRUE;
  }

  /* Check for IPv4-mapped 0.0.0.0 (::ffff:0.0.0.0) */
  if (is_ipv4_mapped(ipv6) && ipv6[12] == 0 && ipv6[13] == 0 && ipv6[14] == 0 &&
      ipv6[15] == 0) {
    return ECHO_TRUE;
  }

  return ECHO_FALSE;
}

/* Helper: Check if address is multicast */
static echo_bool_t is_multicast(const uint8_t *ipv6) {
  /* IPv6 multicast: ff00::/8 */
  if (ipv6[0] == 0xff) {
    return ECHO_TRUE;
  }

  /* IPv4-mapped multicast: ::ffff:224.0.0.0/4 */
  if (is_ipv4_mapped(ipv6) && (ipv6[12] & 0xf0) == 0xe0) {
    return ECHO_TRUE;
  }

  return ECHO_FALSE;
}

/* Helper: Compare two network addresses for equality (ignoring timestamp) */
static echo_bool_t addr_equal(const net_addr_t *a, const net_addr_t *b) {
  return memcmp(a->ip, b->ip, 16) == 0 && a->port == b->port;
}

/* Helper: Find address in manager */
static peer_addr_entry_t *find_address(peer_addr_manager_t *manager,
                                       const net_addr_t *addr) {
  for (size_t i = 0; i < manager->count; i++) {
    if (addr_equal(&manager->addresses[i].addr, addr)) {
      return &manager->addresses[i];
    }
  }
  return NULL;
}

/* Helper: Find address in manager (const version) */
static const peer_addr_entry_t *
find_address_const(const peer_addr_manager_t *manager, const net_addr_t *addr) {
  for (size_t i = 0; i < manager->count; i++) {
    if (addr_equal(&manager->addresses[i].addr, addr)) {
      return &manager->addresses[i];
    }
  }
  return NULL;
}

/* Helper: Get default port for network */
static uint16_t get_default_port(network_type_t network) {
  switch (network) {
  case NETWORK_MAINNET:
    return DEFAULT_PORT_MAINNET;
  case NETWORK_TESTNET:
    return DEFAULT_PORT_TESTNET;
  case NETWORK_REGTEST:
    return DEFAULT_PORT_REGTEST;
  }
  return DEFAULT_PORT_MAINNET;
}

void discovery_init(peer_addr_manager_t *manager, network_type_t network) {
  memset(manager, 0, sizeof(*manager));
  manager->network = network;
}

size_t discovery_query_dns_seeds(peer_addr_manager_t *manager) {
  const char **seeds = NULL;
  uint16_t default_port = get_default_port(manager->network);

  /* Select DNS seeds based on network */
  switch (manager->network) {
  case NETWORK_MAINNET:
    seeds = DNS_SEEDS_MAINNET;
    log_info(LOG_COMP_NET, "Querying mainnet DNS seeds...");
    break;
  case NETWORK_TESTNET:
    seeds = DNS_SEEDS_TESTNET;
    log_info(LOG_COMP_NET, "Querying testnet DNS seeds...");
    break;
  case NETWORK_REGTEST:
    /* Regtest doesn't use DNS seeds */
    return 0;
  }

  size_t total_added = 0;

  /* Query each DNS seed - get up to 50 addresses per seed.
   * Exit early once we have enough addresses to avoid slow seeds.
   * 128 addresses = 4x our max outbound peers (32), plenty for selection. */
  #define MAX_ADDRS_PER_SEED 50
  #define MIN_ADDRESSES_NEEDED 128
  for (size_t i = 0; seeds[i] != NULL; i++) {
    /* Early exit if we have enough addresses */
    if (total_added >= MIN_ADDRESSES_NEEDED) {
      log_info(LOG_COMP_NET, "Have %zu addresses, skipping remaining seeds",
               total_added);
      break;
    }

    log_info(LOG_COMP_NET, "Resolving DNS seed: %s", seeds[i]);

    /* Allocate buffer for multiple IP strings */
    char ip_buffers[MAX_ADDRS_PER_SEED][64];
    char *ip_ptrs[MAX_ADDRS_PER_SEED];
    for (size_t j = 0; j < MAX_ADDRS_PER_SEED; j++) {
      ip_ptrs[j] = ip_buffers[j];
    }

    size_t addr_count = 0;
    int result = plat_dns_resolve_all(seeds[i], ip_ptrs, 64,
                                      MAX_ADDRS_PER_SEED, &addr_count);

    /* Skip if DNS resolution failed */
    if (result != PLAT_OK || addr_count == 0) {
      log_warn(LOG_COMP_NET, "DNS resolution failed for %s (error %d)", seeds[i], result);
      continue;
    }

    log_info(LOG_COMP_NET, "Resolved %s -> %zu addresses", seeds[i], addr_count);

    /* Add all resolved addresses to manager */
    for (size_t j = 0; j < addr_count && manager->count < MAX_PEER_ADDRESSES; j++) {
      net_addr_t addr;
      memset(&addr, 0, sizeof(addr));

      /* Convert IP string to address structure */
      ipv4_to_ipv6_mapped(ip_buffers[j], addr.ip);
      addr.port = default_port;
      addr.services = SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS;
      addr.timestamp = (uint32_t)(plat_time_ms() / 1000);

      /* Check for duplicate before adding (same IP may appear in multiple seeds) */
      if (find_address(manager, &addr) != NULL) {
        continue;
      }

      /* Add to manager */
      peer_addr_entry_t entry;
      memset(&entry, 0, sizeof(entry));
      entry.addr = addr;
      entry.source = ADDR_SOURCE_DNS_SEED;

      manager->addresses[manager->count++] = entry;
      total_added++;

      log_debug(LOG_COMP_NET, "Added address %zu/%zu from %s: %d.%d.%d.%d:%u",
                j + 1, addr_count, seeds[i],
                addr.ip[12], addr.ip[13], addr.ip[14], addr.ip[15], addr.port);
    }
  }

  log_info(LOG_COMP_NET, "DNS seed query complete: %zu addresses discovered", total_added);
  return total_added;
}

size_t discovery_add_hardcoded_seeds(peer_addr_manager_t *manager) {
  const char **seeds = NULL;

  /* Select hardcoded seeds based on network */
  switch (manager->network) {
  case NETWORK_MAINNET:
    seeds = HARDCODED_SEEDS_MAINNET;
    break;
  case NETWORK_TESTNET:
    seeds = HARDCODED_SEEDS_TESTNET;
    break;
  case NETWORK_REGTEST:
    /* Regtest doesn't use hardcoded seeds (use manual addresses) */
    return 0;
  }

  size_t total_added = 0;

  /* Add each hardcoded seed */
  for (size_t i = 0; seeds[i] != NULL; i++) {
    /* Parse "ip:port" format */
    char ip_str[64];
    uint16_t port = get_default_port(manager->network);
    const char *colon = strchr(seeds[i], ':');

    if (colon != NULL) {
      /* Extract IP and port */
      size_t ip_len = (size_t)(colon - seeds[i]);
      if (ip_len >= sizeof(ip_str)) {
        continue; /* IP too long */
      }
      memcpy(ip_str, seeds[i], ip_len);
      ip_str[ip_len] = '\0';

      /* Parse port - limited to 0-65535 range */
      /* NOLINTBEGIN(cert-err34-c) - sscanf correct: checking return and range
       */
      unsigned int port_val = 0;
      if (sscanf(colon + 1, "%u", &port_val) == 1 && port_val <= 65535) {
        /* NOLINTEND(cert-err34-c) */
        port = (uint16_t)port_val;
      }
    } else {
      /* No port specified, use default */
      strncpy(ip_str, seeds[i], sizeof(ip_str) - 1);
      ip_str[sizeof(ip_str) - 1] = '\0';
    }

    /* Create address */
    net_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    ipv4_to_ipv6_mapped(ip_str, addr.ip);

    /* Skip if conversion failed (check if still 0.0.0.0) */
    if (addr.ip[12] == 0 && addr.ip[13] == 0 && addr.ip[14] == 0 && addr.ip[15] == 0) {
      log_warn(LOG_COMP_NET, "Skipping invalid hardcoded seed: %s", seeds[i]);
      continue;
    }

    addr.port = port;
    addr.services = SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS;
    addr.timestamp = (uint32_t)(plat_time_ms() / 1000);

    /* Add to manager */
    peer_addr_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.addr = addr;
    entry.source = ADDR_SOURCE_HARDCODED;

    if (manager->count < MAX_PEER_ADDRESSES) {
      manager->addresses[manager->count++] = entry;
      total_added++;
    }
  }

  return total_added;
}

echo_result_t discovery_add_address(peer_addr_manager_t *manager,
                                    const net_addr_t *addr) {
  /* Validate address */
  if (!discovery_is_address_valid(manager, addr)) {
    return ECHO_ERR_INVALID;
  }

  /* Check for duplicate */
  if (find_address(manager, addr) != NULL) {
    return ECHO_ERR_EXISTS;
  }

  /* Check capacity */
  if (manager->count >= MAX_PEER_ADDRESSES) {
    return ECHO_ERR_FULL;
  }

  /* Add address */
  peer_addr_entry_t entry;
  memset(&entry, 0, sizeof(entry));
  entry.addr = *addr;
  entry.source = ADDR_SOURCE_ADDR_MSG;

  manager->addresses[manager->count++] = entry;
  return ECHO_SUCCESS;
}

size_t discovery_add_addresses(peer_addr_manager_t *manager,
                               const net_addr_t *addresses, size_t count) {
  size_t added = 0;
  for (size_t i = 0; i < count; i++) {
    if (discovery_add_address(manager, &addresses[i]) == ECHO_SUCCESS) {
      added++;
    }
  }
  return added;
}

size_t
discovery_select_addresses_to_advertise(const peer_addr_manager_t *manager,
                                        net_addr_t *out_addrs,
                                        size_t max_count) {
  size_t selected = 0;
  uint64_t now_sec = plat_time_ms() / 1000;

  /* Select fresh, reachable addresses */
  for (size_t i = 0; i < manager->count && selected < max_count; i++) {
    const peer_addr_entry_t *entry = &manager->addresses[i];

    /* Only advertise fresh addresses */
    if (now_sec - entry->addr.timestamp > ADDR_FRESH_THRESHOLD_SEC) {
      continue;
    }

    /* Prefer reachable addresses */
    if (!entry->reachable) {
      continue;
    }

    out_addrs[selected++] = entry->addr;
  }

  /* If we didn't find enough fresh/reachable addresses, add some others */
  if (selected < max_count / 2) {
    for (size_t i = 0; i < manager->count && selected < max_count; i++) {
      const peer_addr_entry_t *entry = &manager->addresses[i];

      /* Skip if already selected */
      echo_bool_t already_selected = ECHO_FALSE;
      for (size_t j = 0; j < selected; j++) {
        if (addr_equal(&out_addrs[j], &entry->addr)) {
          already_selected = ECHO_TRUE;
          break;
        }
      }
      if (already_selected) {
        continue;
      }

      /* Don't advertise very old addresses */
      if (now_sec - entry->addr.timestamp > ADDR_MAX_AGE_SEC) {
        continue;
      }

      out_addrs[selected++] = entry->addr;
    }
  }

  return selected;
}

/*
 * Calculate exponential backoff cooldown based on failed attempts.
 * Base cooldown: 60 seconds, doubles with each attempt, capped at 1 hour.
 *
 * 0 attempts: 60 seconds
 * 1 attempt:  120 seconds (2 min)
 * 2 attempts: 240 seconds (4 min)
 * 3 attempts: 480 seconds (8 min)
 * 4 attempts: 960 seconds (16 min)
 * 5+ attempts: 3600 seconds (1 hour) cap
 */
static uint64_t get_retry_cooldown_ms(uint32_t attempts,
                                      echo_bool_t previously_reachable) {
  /* Previously reachable addresses get shorter cooldowns */
  uint64_t base_ms = previously_reachable ? 30000 : 60000;
  uint64_t max_ms = 3600000; /* 1 hour */

  if (attempts == 0) {
    return base_ms;
  }

  /* Calculate 2^attempts, capped to prevent overflow */
  uint32_t shift = (attempts > 6) ? 6 : attempts;
  uint64_t cooldown_ms = base_ms << shift;

  return (cooldown_ms > max_ms) ? max_ms : cooldown_ms;
}

echo_result_t discovery_select_outbound_address(peer_addr_manager_t *manager,
                                                net_addr_t *out_addr) {
  uint64_t now_ms = plat_time_ms();
  peer_addr_entry_t *best = NULL;
  uint64_t best_score = 0;
  size_t skipped_cooldown = 0;
  size_t skipped_ibd_banned = 0;

  /* Find best address based on:
   * - Not currently in use
   * - Not IBD-banned
   * - Exponential backoff since last attempt
   * - Previous success
   * - Freshness
   */
  for (size_t i = 0; i < manager->count; i++) {
    peer_addr_entry_t *entry = &manager->addresses[i];

    /* Skip if in use */
    if (entry->in_use) {
      continue;
    }

    /* Skip if IBD-banned (self-closed during IBD) */
    if (entry->ibd_ban_until != 0 && now_ms < entry->ibd_ban_until) {
      skipped_ibd_banned++;
      continue;
    }

    /* Calculate cooldown based on failed attempts (exponential backoff) */
    if (entry->last_try != 0) {
      uint64_t cooldown_ms =
          get_retry_cooldown_ms(entry->attempts, entry->reachable);
      uint64_t time_since_try = now_ms - entry->last_try;

      if (time_since_try < cooldown_ms) {
        skipped_cooldown++;
        continue;
      }
    }

    /* Calculate score (higher is better) */
    uint64_t score = 0;

    /* Bonus for never tried */
    if (entry->last_try == 0) {
      score += 1000000;
    }

    /* Bonus for previous success */
    if (entry->reachable) {
      score += 500000;
    }

    /* Bonus for time since last attempt */
    if (entry->last_try != 0) {
      uint64_t time_since_try = now_ms - entry->last_try;
      score += time_since_try / 1000; /* Convert to seconds */
    }

    /* Penalty for failed attempts (saturating subtraction to avoid underflow) */
    uint64_t penalty = 0;
    if (entry->attempts > 0 && !entry->reachable) {
      /* Non-reachable addresses with many attempts get heavy penalty */
      penalty = (uint64_t)entry->attempts * 50000;
    } else if (entry->attempts > 0) {
      /* Previously reachable addresses get lighter penalty */
      penalty = (uint64_t)entry->attempts * 10000;
    }
    score = (score > penalty) ? (score - penalty) : 0;

    /* Consider this address if it's the best so far */
    if (best == NULL || score > best_score) {
      best = entry;
      best_score = score;
    }
  }

  if (best == NULL) {
    log_warn(LOG_COMP_NET,
             "No suitable outbound address found (checked %zu, %zu in "
             "cooldown, %zu IBD-banned)",
             manager->count, skipped_cooldown, skipped_ibd_banned);
    return ECHO_ERR_NOT_FOUND;
  }

  *out_addr = best->addr;
  log_info(LOG_COMP_NET,
           "Selected address: %d.%d.%d.%d:%u (score=%llu, attempts=%u)",
           best->addr.ip[12], best->addr.ip[13], best->addr.ip[14],
           best->addr.ip[15], best->addr.port, (unsigned long long)best_score,
           best->attempts);
  return ECHO_SUCCESS;
}

void discovery_mark_address_in_use(peer_addr_manager_t *manager,
                                   const net_addr_t *addr) {
  peer_addr_entry_t *entry = find_address(manager, addr);
  if (entry != NULL) {
    entry->in_use = ECHO_TRUE;
  }
}

void discovery_mark_address_free(peer_addr_manager_t *manager,
                                 const net_addr_t *addr, echo_bool_t success) {
  peer_addr_entry_t *entry = find_address(manager, addr);
  if (entry != NULL) {
    entry->in_use = ECHO_FALSE;
    if (success) {
      discovery_mark_success(manager, addr);
    } else {
      /* Mark as unreachable on failure - this removes the +500000 reachable
       * bonus and applies the heavy -50000/attempt penalty, preventing
       * immediate reconnection to misbehaving peers */
      entry->reachable = ECHO_FALSE;
    }
  }
}

void discovery_mark_attempt(peer_addr_manager_t *manager,
                            const net_addr_t *addr) {
  peer_addr_entry_t *entry = find_address(manager, addr);
  if (entry != NULL) {
    entry->last_try = plat_time_ms();
    entry->attempts++;
  }
}

void discovery_mark_success(peer_addr_manager_t *manager,
                            const net_addr_t *addr) {
  peer_addr_entry_t *entry = find_address(manager, addr);
  if (entry != NULL) {
    entry->last_success = plat_time_ms();
    entry->reachable = ECHO_TRUE;
    /* Reset attempts on success to prevent cooldown from growing
     * indefinitely across successful connection cycles */
    entry->attempts = 0;
  }
}

void discovery_mark_ibd_banned(peer_addr_manager_t *manager,
                               const net_addr_t *addr) {
  peer_addr_entry_t *entry = find_address(manager, addr);
  if (entry != NULL) {
    uint64_t now = plat_time_ms();
    entry->ibd_ban_until = now + IBD_BAN_DURATION_MS;
    log_info(LOG_COMP_NET,
             "IBD-banned %d.%d.%d.%d:%u for 1 hour (self-closed during IBD)",
             addr->ip[12], addr->ip[13], addr->ip[14], addr->ip[15], addr->port);
  }
}

echo_bool_t discovery_is_ibd_banned(const peer_addr_manager_t *manager,
                                    const net_addr_t *addr) {
  const peer_addr_entry_t *entry = find_address_const(manager, addr);
  if (entry == NULL || entry->ibd_ban_until == 0) {
    return ECHO_FALSE;
  }
  return plat_time_ms() < entry->ibd_ban_until;
}

echo_bool_t discovery_is_address_valid(const peer_addr_manager_t *manager,
                                       const net_addr_t *addr) {
  /* Check for unspecified address */
  if (is_unspecified(addr->ip)) {
    return ECHO_FALSE;
  }

  /* Check for multicast */
  if (is_multicast(addr->ip)) {
    return ECHO_FALSE;
  }

  /* Check for loopback (except in regtest) */
  if (manager->network != NETWORK_REGTEST && is_loopback(addr->ip)) {
    return ECHO_FALSE;
  }

  /* Check for valid port */
  if (addr->port == 0) {
    return ECHO_FALSE;
  }

  return ECHO_TRUE;
}

size_t discovery_get_address_count(const peer_addr_manager_t *manager) {
  return manager->count;
}

size_t discovery_get_reachable_count(const peer_addr_manager_t *manager) {
  size_t count = 0;
  for (size_t i = 0; i < manager->count; i++) {
    if (manager->addresses[i].reachable) {
      count++;
    }
  }
  return count;
}

echo_result_t discovery_parse_address(const char *ip_str, uint16_t port,
                                      net_addr_t *out_addr) {
  if (ip_str == NULL || out_addr == NULL) {
    return ECHO_ERR_INVALID;
  }

  memset(out_addr, 0, sizeof(*out_addr));

  /* Convert IPv4 string to IPv6-mapped address */
  ipv4_to_ipv6_mapped(ip_str, out_addr->ip);

  /* Check if conversion succeeded (not all zeros) */
  if (out_addr->ip[12] == 0 && out_addr->ip[13] == 0 && out_addr->ip[14] == 0 &&
      out_addr->ip[15] == 0) {
    return ECHO_ERR_INVALID;
  }

  out_addr->port = port;
  out_addr->services = SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS;
  out_addr->timestamp = (uint32_t)(plat_time_ms() / 1000);

  return ECHO_SUCCESS;
}
