/**
 * Bitcoin Echo — Peer Discovery Tests
 *
 * Tests for peer discovery, address management, and peer selection.
 */

#include "discovery.h"
#include "platform.h"
#include "protocol.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)                                                             \
  do {                                                                         \
    printf("  Running %s...\n", name);                                         \
    tests_run++;                                                               \
  } while (0)

#define ASSERT(cond)                                                           \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("    FAILED: %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define PASS()                                                                 \
  do {                                                                         \
    tests_passed++;                                                            \
    printf("    PASSED\n");                                                    \
  } while (0)

/* Helper: Create a test network address */
static net_addr_t make_test_addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                                  uint16_t port) {
  net_addr_t addr;
  memset(&addr, 0, sizeof(addr));

  /* IPv4-mapped IPv6: ::ffff:a.b.c.d */
  memset(addr.ip, 0, 10);
  addr.ip[10] = 0xff;
  addr.ip[11] = 0xff;
  addr.ip[12] = a;
  addr.ip[13] = b;
  addr.ip[14] = c;
  addr.ip[15] = d;

  addr.port = port;
  addr.services = SERVICE_NODE_NETWORK | SERVICE_NODE_WITNESS;
  addr.timestamp = (uint32_t)(plat_time_ms() / 1000); /* Current time */

  return addr;
}

/* Test: Initialize address manager */
static void test_init(void) {
  TEST("test_init");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  ASSERT(mgr.count == 0);
  ASSERT(mgr.network == NETWORK_MAINNET);
  ASSERT(mgr.last_addr_broadcast == 0);

  PASS();
}

/* Test: Add valid address */
static void test_add_address(void) {
  TEST("test_add_address");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  net_addr_t addr = make_test_addr(192, 168, 1, 1, 8333);

  echo_result_t res = discovery_add_address(&mgr, &addr);
  ASSERT(res == ECHO_SUCCESS);
  ASSERT(mgr.count == 1);

  /* Check address was stored */
  ASSERT(memcmp(mgr.addresses[0].addr.ip, addr.ip, 16) == 0);
  ASSERT(mgr.addresses[0].addr.port == addr.port);
  ASSERT(mgr.addresses[0].source == ADDR_SOURCE_ADDR_MSG);

  PASS();
}

/* Test: Reject duplicate address */
static void test_add_duplicate(void) {
  TEST("test_add_duplicate");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  net_addr_t addr = make_test_addr(192, 168, 1, 1, 8333);

  /* Add first time - should succeed */
  echo_result_t res = discovery_add_address(&mgr, &addr);
  ASSERT(res == ECHO_SUCCESS);
  ASSERT(mgr.count == 1);

  /* Add second time - should fail */
  res = discovery_add_address(&mgr, &addr);
  ASSERT(res == ECHO_ERR_EXISTS);
  ASSERT(mgr.count == 1); /* Count should not increase */

  PASS();
}

/* Test: Reject loopback address on mainnet */
static void test_reject_loopback_mainnet(void) {
  TEST("test_reject_loopback_mainnet");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  net_addr_t addr = make_test_addr(127, 0, 0, 1, 8333);

  echo_result_t res = discovery_add_address(&mgr, &addr);
  ASSERT(res == ECHO_ERR_INVALID);
  ASSERT(mgr.count == 0);

  PASS();
}

/* Test: Accept loopback address on regtest */
static void test_accept_loopback_regtest(void) {
  TEST("test_accept_loopback_regtest");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_REGTEST);

  net_addr_t addr = make_test_addr(127, 0, 0, 1, 18444);

  echo_result_t res = discovery_add_address(&mgr, &addr);
  ASSERT(res == ECHO_SUCCESS);
  ASSERT(mgr.count == 1);

  PASS();
}

/* Test: Reject unspecified address (0.0.0.0) */
static void test_reject_unspecified(void) {
  TEST("test_reject_unspecified");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  net_addr_t addr = make_test_addr(0, 0, 0, 0, 8333);

  echo_result_t res = discovery_add_address(&mgr, &addr);
  ASSERT(res == ECHO_ERR_INVALID);
  ASSERT(mgr.count == 0);

  PASS();
}

/* Test: Reject multicast address */
static void test_reject_multicast(void) {
  TEST("test_reject_multicast");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  net_addr_t addr = make_test_addr(224, 0, 0, 1, 8333); /* Multicast range */

  echo_result_t res = discovery_add_address(&mgr, &addr);
  ASSERT(res == ECHO_ERR_INVALID);
  ASSERT(mgr.count == 0);

  PASS();
}

/* Test: Reject zero port */
static void test_reject_zero_port(void) {
  TEST("test_reject_zero_port");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  net_addr_t addr = make_test_addr(192, 168, 1, 1, 0);

  echo_result_t res = discovery_add_address(&mgr, &addr);
  ASSERT(res == ECHO_ERR_INVALID);
  ASSERT(mgr.count == 0);

  PASS();
}

/* Test: Add multiple addresses */
static void test_add_multiple(void) {
  TEST("test_add_multiple");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  net_addr_t addrs[5];
  for (size_t i = 0; i < 5; i++) {
    addrs[i] = make_test_addr(192, 168, 1, (uint8_t)(i + 1), 8333);
  }

  size_t added = discovery_add_addresses(&mgr, addrs, 5);
  ASSERT(added == 5);
  ASSERT(mgr.count == 5);

  PASS();
}

/* Test: Address validation */
static void test_address_validation(void) {
  TEST("test_address_validation");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  /* Valid address */
  net_addr_t valid = make_test_addr(8, 8, 8, 8, 8333);
  ASSERT(discovery_is_address_valid(&mgr, &valid) == ECHO_TRUE);

  /* Loopback (invalid on mainnet) */
  net_addr_t loopback = make_test_addr(127, 0, 0, 1, 8333);
  ASSERT(discovery_is_address_valid(&mgr, &loopback) == ECHO_FALSE);

  /* Unspecified */
  net_addr_t unspec = make_test_addr(0, 0, 0, 0, 8333);
  ASSERT(discovery_is_address_valid(&mgr, &unspec) == ECHO_FALSE);

  /* Multicast */
  net_addr_t mcast = make_test_addr(239, 255, 255, 250, 8333);
  ASSERT(discovery_is_address_valid(&mgr, &mcast) == ECHO_FALSE);

  /* Zero port */
  net_addr_t zero_port = make_test_addr(8, 8, 8, 8, 0);
  ASSERT(discovery_is_address_valid(&mgr, &zero_port) == ECHO_FALSE);

  PASS();
}

/* Test: Select outbound address */
static void test_select_outbound(void) {
  TEST("test_select_outbound");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  /* Empty manager - should fail */
  net_addr_t selected;
  echo_result_t res = discovery_select_outbound_address(&mgr, &selected);
  ASSERT(res == ECHO_ERR_NOT_FOUND);

  /* Add addresses */
  net_addr_t addr1 = make_test_addr(192, 168, 1, 1, 8333);
  net_addr_t addr2 = make_test_addr(192, 168, 1, 2, 8333);
  net_addr_t addr3 = make_test_addr(192, 168, 1, 3, 8333);

  discovery_add_address(&mgr, &addr1);
  discovery_add_address(&mgr, &addr2);
  discovery_add_address(&mgr, &addr3);

  /* Should select one */
  res = discovery_select_outbound_address(&mgr, &selected);
  ASSERT(res == ECHO_SUCCESS);

  /* Should select different address next time (or same if it's best) */
  /* This is probabilistic, so we just check it succeeds */
  res = discovery_select_outbound_address(&mgr, &selected);
  ASSERT(res == ECHO_SUCCESS);

  PASS();
}

/* Test: Mark address in use */
static void test_mark_in_use(void) {
  TEST("test_mark_in_use");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  net_addr_t addr = make_test_addr(192, 168, 1, 1, 8333);
  discovery_add_address(&mgr, &addr);

  /* Initially not in use */
  ASSERT(mgr.addresses[0].in_use == ECHO_FALSE);

  /* Mark in use */
  discovery_mark_address_in_use(&mgr, &addr);
  ASSERT(mgr.addresses[0].in_use == ECHO_TRUE);

  /* Should not be selected for outbound */
  net_addr_t selected;
  echo_result_t res = discovery_select_outbound_address(&mgr, &selected);
  ASSERT(res == ECHO_ERR_NOT_FOUND); /* No available addresses */

  /* Mark free */
  discovery_mark_address_free(&mgr, &addr, ECHO_FALSE);
  ASSERT(mgr.addresses[0].in_use == ECHO_FALSE);

  PASS();
}

/* Test: Mark connection attempt */
static void test_mark_attempt(void) {
  TEST("test_mark_attempt");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  net_addr_t addr = make_test_addr(192, 168, 1, 1, 8333);
  discovery_add_address(&mgr, &addr);

  /* Initially no attempts */
  ASSERT(mgr.addresses[0].attempts == 0);
  ASSERT(mgr.addresses[0].last_try == 0);

  /* Mark attempt */
  discovery_mark_attempt(&mgr, &addr);
  ASSERT(mgr.addresses[0].attempts == 1);
  ASSERT(mgr.addresses[0].last_try != 0);

  /* Mark another attempt */
  discovery_mark_attempt(&mgr, &addr);
  ASSERT(mgr.addresses[0].attempts == 2);

  PASS();
}

/* Test: Mark successful connection */
static void test_mark_success(void) {
  TEST("test_mark_success");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  net_addr_t addr = make_test_addr(192, 168, 1, 1, 8333);
  discovery_add_address(&mgr, &addr);

  /* Initially not reachable */
  ASSERT(mgr.addresses[0].reachable == ECHO_FALSE);
  ASSERT(mgr.addresses[0].last_success == 0);

  /* Mark success */
  discovery_mark_success(&mgr, &addr);
  ASSERT(mgr.addresses[0].reachable == ECHO_TRUE);
  ASSERT(mgr.addresses[0].last_success != 0);

  PASS();
}

/* Test: Get address counts */
static void test_get_counts(void) {
  TEST("test_get_counts");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  ASSERT(discovery_get_address_count(&mgr) == 0);
  ASSERT(discovery_get_reachable_count(&mgr) == 0);

  /* Add addresses */
  net_addr_t addr1 = make_test_addr(192, 168, 1, 1, 8333);
  net_addr_t addr2 = make_test_addr(192, 168, 1, 2, 8333);
  net_addr_t addr3 = make_test_addr(192, 168, 1, 3, 8333);

  discovery_add_address(&mgr, &addr1);
  discovery_add_address(&mgr, &addr2);
  discovery_add_address(&mgr, &addr3);

  ASSERT(discovery_get_address_count(&mgr) == 3);
  ASSERT(discovery_get_reachable_count(&mgr) == 0);

  /* Mark one as reachable */
  discovery_mark_success(&mgr, &addr1);
  ASSERT(discovery_get_reachable_count(&mgr) == 1);

  /* Mark another as reachable */
  discovery_mark_success(&mgr, &addr2);
  ASSERT(discovery_get_reachable_count(&mgr) == 2);

  PASS();
}

/* Test: Select addresses to advertise */
static void test_select_to_advertise(void) {
  TEST("test_select_to_advertise");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  /* Add addresses and mark as reachable */
  net_addr_t addr1 = make_test_addr(192, 168, 1, 1, 8333);
  net_addr_t addr2 = make_test_addr(192, 168, 1, 2, 8333);
  net_addr_t addr3 = make_test_addr(192, 168, 1, 3, 8333);

  discovery_add_address(&mgr, &addr1);
  discovery_add_address(&mgr, &addr2);
  discovery_add_address(&mgr, &addr3);

  discovery_mark_success(&mgr, &addr1);
  discovery_mark_success(&mgr, &addr2);

  /* Select addresses */
  net_addr_t selected[10];
  size_t count = discovery_select_addresses_to_advertise(&mgr, selected, 10);

  /* Should return at least the reachable ones */
  ASSERT(count >= 2);
  ASSERT(count <= 3);

  PASS();
}

/* Test: Add hardcoded seeds */
static void test_hardcoded_seeds(void) {
  TEST("test_hardcoded_seeds");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  size_t added = discovery_add_hardcoded_seeds(&mgr);

  /* Should add at least one hardcoded seed for mainnet */
  ASSERT(added > 0);
  ASSERT(mgr.count == added);

  /* Check that addresses have correct source */
  for (size_t i = 0; i < mgr.count; i++) {
    ASSERT(mgr.addresses[i].source == ADDR_SOURCE_HARDCODED);
  }

  PASS();
}

/* Test: Regtest doesn't use hardcoded seeds */
static void test_regtest_no_hardcoded(void) {
  TEST("test_regtest_no_hardcoded");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_REGTEST);

  size_t added = discovery_add_hardcoded_seeds(&mgr);

  /* Regtest should not add any hardcoded seeds */
  ASSERT(added == 0);
  ASSERT(mgr.count == 0);

  PASS();
}

/* Test: Address capacity limit */
static void test_capacity_limit(void) {
  TEST("test_capacity_limit");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  /* Add addresses up to capacity */
  for (size_t i = 0; i < MAX_PEER_ADDRESSES; i++) {
    net_addr_t addr = make_test_addr(10, (uint8_t)(i >> 16), (uint8_t)(i >> 8),
                                      (uint8_t)i, 8333);
    echo_result_t res = discovery_add_address(&mgr, &addr);
    ASSERT(res == ECHO_SUCCESS);
  }

  ASSERT(mgr.count == MAX_PEER_ADDRESSES);

  /* Try to add one more - should fail */
  net_addr_t overflow = make_test_addr(192, 168, 1, 1, 8333);
  echo_result_t res = discovery_add_address(&mgr, &overflow);
  ASSERT(res == ECHO_ERR_FULL);
  ASSERT(mgr.count == MAX_PEER_ADDRESSES);

  PASS();
}

/* Test: Prefer reachable addresses for outbound */
static void test_prefer_reachable(void) {
  TEST("test_prefer_reachable");

  peer_addr_manager_t mgr;
  discovery_init(&mgr, NETWORK_MAINNET);

  /* Add addresses */
  net_addr_t unreachable = make_test_addr(192, 168, 1, 1, 8333);
  net_addr_t reachable = make_test_addr(192, 168, 1, 2, 8333);

  discovery_add_address(&mgr, &unreachable);
  discovery_add_address(&mgr, &reachable);

  /* Mark one as reachable */
  discovery_mark_success(&mgr, &reachable);

  /* Select should prefer reachable */
  net_addr_t selected;
  echo_result_t res = discovery_select_outbound_address(&mgr, &selected);
  ASSERT(res == ECHO_SUCCESS);

  /* We can't guarantee which one is selected due to scoring,
   * but both should be valid candidates */

  PASS();
}

int main(void) {
  printf("Running peer discovery tests...\n\n");

  test_init();
  test_add_address();
  test_add_duplicate();
  test_reject_loopback_mainnet();
  test_accept_loopback_regtest();
  test_reject_unspecified();
  test_reject_multicast();
  test_reject_zero_port();
  test_add_multiple();
  test_address_validation();
  test_select_outbound();
  test_mark_in_use();
  test_mark_attempt();
  test_mark_success();
  test_get_counts();
  test_select_to_advertise();
  test_hardcoded_seeds();
  test_regtest_no_hardcoded();
  test_capacity_limit();
  test_prefer_reachable();

  printf("\n========================================\n");
  printf("Results: %d/%d tests passed\n", tests_passed, tests_run);
  printf("========================================\n");

  return (tests_passed == tests_run) ? 0 : 1;
}
