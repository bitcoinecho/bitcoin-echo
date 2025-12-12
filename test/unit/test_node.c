/**
 * Bitcoin Echo — Node Lifecycle Tests
 *
 * Tests for Session 9.1: Node initialization and shutdown.
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
#include "mempool.h"
#include "platform.h"
#include "utxo_db.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * ============================================================================
 * TEST UTILITIES
 * ============================================================================
 */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name)                                                         \
  do {                                                                         \
    printf("  %-50s", #name);                                                  \
    fflush(stdout);                                                            \
    test_##name();                                                             \
    printf(" PASS\n");                                                         \
    tests_passed++;                                                            \
  } while (0)

#define ASSERT(cond)                                                           \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf(" FAIL\n");                                                       \
      printf("    Assertion failed: %s\n", #cond);                             \
      printf("    File: %s, Line: %d\n", __FILE__, __LINE__);                  \
      tests_failed++;                                                          \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_EQ(a, b)                                                        \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      printf(" FAIL\n");                                                       \
      printf("    Expected: %d, Got: %d\n", (int)(b), (int)(a));               \
      printf("    File: %s, Line: %d\n", __FILE__, __LINE__);                  \
      tests_failed++;                                                          \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_NOT_NULL(ptr)                                                   \
  do {                                                                         \
    if ((ptr) == NULL) {                                                       \
      printf(" FAIL\n");                                                       \
      printf("    Expected non-NULL, got NULL\n");                             \
      printf("    File: %s, Line: %d\n", __FILE__, __LINE__);                  \
      tests_failed++;                                                          \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_NULL(ptr)                                                       \
  do {                                                                         \
    if ((ptr) != NULL) {                                                       \
      printf(" FAIL\n");                                                       \
      printf("    Expected NULL, got non-NULL\n");                             \
      printf("    File: %s, Line: %d\n", __FILE__, __LINE__);                  \
      tests_failed++;                                                          \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_STR_EQ(a, b)                                                    \
  do {                                                                         \
    if (strcmp((a), (b)) != 0) {                                               \
      printf(" FAIL\n");                                                       \
      printf("    Expected: %s, Got: %s\n", (b), (a));                         \
      printf("    File: %s, Line: %d\n", __FILE__, __LINE__);                  \
      tests_failed++;                                                          \
      return;                                                                  \
    }                                                                          \
  } while (0)

/* Test data directory - created in temp location */
static char test_data_dir[256];

/**
 * Helper to generate unique test directory for each test.
 */
static void make_test_dir(const char *suffix) {
  snprintf(test_data_dir, sizeof(test_data_dir), "/tmp/echo_test_%s_%d",
           suffix, (int)plat_time_ms() % 100000);
}

/**
 * Helper to clean up test directory recursively.
 */
static void cleanup_test_dir(const char *dir) {
  char path[512];

  /* Remove known files and subdirectories */
  snprintf(path, sizeof(path), "%s/chainstate/utxo.db", dir);
  plat_file_delete(path);
  snprintf(path, sizeof(path), "%s/chainstate/utxo.db-wal", dir);
  plat_file_delete(path);
  snprintf(path, sizeof(path), "%s/chainstate/utxo.db-shm", dir);
  plat_file_delete(path);
  snprintf(path, sizeof(path), "%s/chainstate/blocks.db", dir);
  plat_file_delete(path);
  snprintf(path, sizeof(path), "%s/chainstate/blocks.db-wal", dir);
  plat_file_delete(path);
  snprintf(path, sizeof(path), "%s/chainstate/blocks.db-shm", dir);
  plat_file_delete(path);

  /* Remove directories (will fail if not empty, that's ok) */
  snprintf(path, sizeof(path), "%s/chainstate", dir);
  rmdir(path);
  snprintf(path, sizeof(path), "%s/blocks", dir);
  rmdir(path);
  rmdir(dir);
}

/*
 * ============================================================================
 * CONFIGURATION TESTS
 * ============================================================================
 */

TEST(config_init_basic) {
  node_config_t config;
  node_config_init(&config, "/path/to/data");

  ASSERT_STR_EQ(config.data_dir, "/path/to/data");
  ASSERT_EQ(config.port, ECHO_DEFAULT_PORT);
  ASSERT_EQ(config.rpc_port, ECHO_DEFAULT_RPC_PORT);
}

TEST(config_init_null_datadir) {
  node_config_t config;
  memset(&config, 0xFF, sizeof(config)); /* Fill with garbage */

  node_config_init(&config, NULL);

  /* Data dir should be empty string */
  ASSERT_EQ(config.data_dir[0], '\0');
  ASSERT_EQ(config.port, ECHO_DEFAULT_PORT);
}

TEST(config_init_empty_datadir) {
  node_config_t config;
  node_config_init(&config, "");

  ASSERT_EQ(config.data_dir[0], '\0');
}

TEST(config_init_long_datadir) {
  node_config_t config;
  char long_path[1024];
  memset(long_path, 'x', sizeof(long_path));
  long_path[sizeof(long_path) - 1] = '\0';

  node_config_init(&config, long_path);

  /* Should be truncated to fit */
  ASSERT(strlen(config.data_dir) < sizeof(config.data_dir));
  ASSERT(config.data_dir[sizeof(config.data_dir) - 1] == '\0');
}

TEST(config_init_null_config) {
  /* Should not crash */
  node_config_init(NULL, "/path/to/data");
}

/*
 * ============================================================================
 * NODE STATE STRING TESTS
 * ============================================================================
 */

TEST(state_string_all) {
  ASSERT_STR_EQ(node_state_string(NODE_STATE_UNINITIALIZED), "UNINITIALIZED");
  ASSERT_STR_EQ(node_state_string(NODE_STATE_INITIALIZING), "INITIALIZING");
  ASSERT_STR_EQ(node_state_string(NODE_STATE_STARTING), "STARTING");
  ASSERT_STR_EQ(node_state_string(NODE_STATE_RUNNING), "RUNNING");
  ASSERT_STR_EQ(node_state_string(NODE_STATE_STOPPING), "STOPPING");
  ASSERT_STR_EQ(node_state_string(NODE_STATE_STOPPED), "STOPPED");
  ASSERT_STR_EQ(node_state_string(NODE_STATE_ERROR), "ERROR");
}

TEST(state_string_invalid) {
  /* Should return "UNKNOWN" for invalid state */
  ASSERT_STR_EQ(node_state_string((node_state_t)999), "UNKNOWN");
}

/*
 * ============================================================================
 * NODE CREATION TESTS
 * ============================================================================
 */

TEST(create_null_config) {
  node_t *node = node_create(NULL);
  ASSERT_NULL(node);
}

TEST(create_empty_datadir) {
  node_config_t config;
  node_config_init(&config, "");

  node_t *node = node_create(&config);
  ASSERT_NULL(node);
}

TEST(create_and_destroy) {
  make_test_dir("create");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  ASSERT_EQ(node_get_state(node), NODE_STATE_STOPPED);
  ASSERT(!node_is_running(node));

  node_destroy(node);

  cleanup_test_dir(test_data_dir);
}

TEST(create_twice_same_dir) {
  make_test_dir("create2");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  /* First node */
  node_t *node1 = node_create(&config);
  ASSERT_NOT_NULL(node1);
  node_destroy(node1);

  /* Second node - should work, databases are closed */
  node_t *node2 = node_create(&config);
  ASSERT_NOT_NULL(node2);
  node_destroy(node2);

  cleanup_test_dir(test_data_dir);
}

/*
 * ============================================================================
 * NODE COMPONENT ACCESS TESTS
 * ============================================================================
 */

TEST(get_consensus) {
  make_test_dir("consensus");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  consensus_engine_t *consensus = node_get_consensus(node);
  ASSERT_NOT_NULL(consensus);

  const consensus_engine_t *consensus_const = node_get_consensus_const(node);
  ASSERT_NOT_NULL(consensus_const);
  ASSERT(consensus == consensus_const);

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(get_mempool) {
  make_test_dir("mempool");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  mempool_t *mp = node_get_mempool(node);
  ASSERT_NOT_NULL(mp);

  const mempool_t *mp_const = node_get_mempool_const(node);
  ASSERT_NOT_NULL(mp_const);
  ASSERT(mp == mp_const);

  /* Mempool should be empty initially */
  ASSERT_EQ(mempool_size(mp), 0);

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(get_storage_components) {
  make_test_dir("storage");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  /* Block storage */
  block_file_manager_t *bs = node_get_block_storage(node);
  ASSERT_NOT_NULL(bs);

  /* UTXO database */
  utxo_db_t *udb = node_get_utxo_db(node);
  ASSERT_NOT_NULL(udb);

  /* Block index database */
  block_index_db_t *bdb = node_get_block_index_db(node);
  ASSERT_NOT_NULL(bdb);

  /* Address manager */
  peer_addr_manager_t *am = node_get_addr_manager(node);
  ASSERT_NOT_NULL(am);

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(get_data_dir) {
  make_test_dir("datadir");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  const char *dir = node_get_data_dir(node);
  ASSERT_NOT_NULL(dir);
  ASSERT_STR_EQ(dir, test_data_dir);

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(get_components_null) {
  /* All getters should handle NULL gracefully */
  ASSERT_NULL(node_get_consensus(NULL));
  ASSERT_NULL(node_get_consensus_const(NULL));
  ASSERT_NULL(node_get_mempool(NULL));
  ASSERT_NULL(node_get_mempool_const(NULL));
  ASSERT_NULL(node_get_block_storage(NULL));
  ASSERT_NULL(node_get_utxo_db(NULL));
  ASSERT_NULL(node_get_block_index_db(NULL));
  ASSERT_NULL(node_get_addr_manager(NULL));
  ASSERT_NULL(node_get_data_dir(NULL));
  ASSERT_NULL(node_get_sync_manager(NULL));
}

/*
 * ============================================================================
 * NODE START/STOP TESTS
 * ============================================================================
 */

TEST(start_null) {
  echo_result_t result = node_start(NULL);
  ASSERT_EQ(result, ECHO_ERR_NULL_PARAM);
}

TEST(stop_null) {
  echo_result_t result = node_stop(NULL);
  ASSERT_EQ(result, ECHO_ERR_NULL_PARAM);
}

TEST(destroy_null) {
  /* Should not crash */
  node_destroy(NULL);
}

TEST(start_stop_cycle) {
  make_test_dir("startstop");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);
  ASSERT_EQ(node_get_state(node), NODE_STATE_STOPPED);

  /* Start */
  echo_result_t result = node_start(node);
  ASSERT_EQ(result, ECHO_OK);
  ASSERT_EQ(node_get_state(node), NODE_STATE_RUNNING);
  ASSERT(node_is_running(node));

  /* Stop */
  result = node_stop(node);
  ASSERT_EQ(result, ECHO_OK);
  ASSERT_EQ(node_get_state(node), NODE_STATE_STOPPED);
  ASSERT(!node_is_running(node));

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(double_start) {
  make_test_dir("doublestart");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  echo_result_t result = node_start(node);
  ASSERT_EQ(result, ECHO_OK);

  /* Second start should fail - already running */
  result = node_start(node);
  ASSERT_EQ(result, ECHO_ERR_INVALID_STATE);

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(double_stop) {
  make_test_dir("doublestop");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  node_start(node);
  node_stop(node);

  /* Second stop should be no-op */
  echo_result_t result = node_stop(node);
  ASSERT_EQ(result, ECHO_OK);

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(destroy_running_node) {
  make_test_dir("destroyrunning");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  node_start(node);
  ASSERT(node_is_running(node));

  /* Destroy should stop the node first */
  node_destroy(node);

  cleanup_test_dir(test_data_dir);
}

/*
 * ============================================================================
 * NODE STATISTICS TESTS
 * ============================================================================
 */

TEST(stats_initial) {
  make_test_dir("stats");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  node_stats_t stats;
  node_get_stats(node, &stats);

  /* Initial state: genesis only, no peers */
  ASSERT_EQ(stats.chain_height, 0);
  ASSERT_EQ(stats.peer_count, 0);
  ASSERT_EQ(stats.mempool_size, 0);
  ASSERT(!stats.is_syncing);

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(stats_running) {
  make_test_dir("statsrun");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  node_start(node);

  node_stats_t stats;
  node_get_stats(node, &stats);

  /* Should have start time and positive uptime */
  ASSERT(stats.start_time > 0);
  /* Uptime might be 0 if checked immediately */

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(stats_null_params) {
  make_test_dir("statsnull");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  /* Should not crash with NULL params */
  node_get_stats(NULL, NULL);
  node_get_stats(node, NULL);

  node_stats_t stats;
  node_get_stats(NULL, &stats);

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

/*
 * ============================================================================
 * PEER MANAGEMENT TESTS
 * ============================================================================
 */

TEST(peer_count_initial) {
  make_test_dir("peercount");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  ASSERT_EQ(node_get_peer_count(node), 0);

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(peer_count_null) { ASSERT_EQ(node_get_peer_count(NULL), 0); }

TEST(get_peer_empty) {
  make_test_dir("getpeer");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  /* No peers, should return NULL */
  ASSERT_NULL(node_get_peer(node, 0));
  ASSERT_NULL(node_get_peer(node, 100));

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(get_peer_null) { ASSERT_NULL(node_get_peer(NULL, 0)); }

/*
 * ============================================================================
 * SHUTDOWN REQUEST TESTS
 * ============================================================================
 */

TEST(shutdown_request) {
  make_test_dir("shutdown");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  ASSERT(!node_shutdown_requested(node));

  node_request_shutdown(node);
  ASSERT(node_shutdown_requested(node));

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(shutdown_request_null) {
  /* Should not crash */
  node_request_shutdown(NULL);
  ASSERT(!node_shutdown_requested(NULL));
}

/*
 * ============================================================================
 * SYNCING STATE TESTS
 * ============================================================================
 */

TEST(is_syncing_initial) {
  make_test_dir("syncing");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  /* No sync manager yet, so not syncing */
  ASSERT(!node_is_syncing(node));

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(is_syncing_null) { ASSERT(!node_is_syncing(NULL)); }

/*
 * ============================================================================
 * STATE TRANSITIONS
 * ============================================================================
 */

TEST(state_transitions) {
  make_test_dir("transitions");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);
  ASSERT_EQ(node_get_state(node), NODE_STATE_STOPPED);

  node_start(node);
  ASSERT_EQ(node_get_state(node), NODE_STATE_RUNNING);

  node_stop(node);
  ASSERT_EQ(node_get_state(node), NODE_STATE_STOPPED);

  /* Start again */
  node_start(node);
  ASSERT_EQ(node_get_state(node), NODE_STATE_RUNNING);

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

TEST(get_state_null) { ASSERT_EQ(node_get_state(NULL), NODE_STATE_UNINITIALIZED); }

/*
 * ============================================================================
 * MAIN
 * ============================================================================
 */

int main(void) {
  printf("Bitcoin Echo — Node Lifecycle Tests (Session 9.1)\n");
  printf("================================================\n\n");

  printf("Configuration tests:\n");
  RUN_TEST(config_init_basic);
  RUN_TEST(config_init_null_datadir);
  RUN_TEST(config_init_empty_datadir);
  RUN_TEST(config_init_long_datadir);
  RUN_TEST(config_init_null_config);
  printf("\n");

  printf("State string tests:\n");
  RUN_TEST(state_string_all);
  RUN_TEST(state_string_invalid);
  printf("\n");

  printf("Node creation tests:\n");
  RUN_TEST(create_null_config);
  RUN_TEST(create_empty_datadir);
  RUN_TEST(create_and_destroy);
  RUN_TEST(create_twice_same_dir);
  printf("\n");

  printf("Component access tests:\n");
  RUN_TEST(get_consensus);
  RUN_TEST(get_mempool);
  RUN_TEST(get_storage_components);
  RUN_TEST(get_data_dir);
  RUN_TEST(get_components_null);
  printf("\n");

  printf("Start/stop tests:\n");
  RUN_TEST(start_null);
  RUN_TEST(stop_null);
  RUN_TEST(destroy_null);
  RUN_TEST(start_stop_cycle);
  RUN_TEST(double_start);
  RUN_TEST(double_stop);
  RUN_TEST(destroy_running_node);
  printf("\n");

  printf("Statistics tests:\n");
  RUN_TEST(stats_initial);
  RUN_TEST(stats_running);
  RUN_TEST(stats_null_params);
  printf("\n");

  printf("Peer management tests:\n");
  RUN_TEST(peer_count_initial);
  RUN_TEST(peer_count_null);
  RUN_TEST(get_peer_empty);
  RUN_TEST(get_peer_null);
  printf("\n");

  printf("Shutdown request tests:\n");
  RUN_TEST(shutdown_request);
  RUN_TEST(shutdown_request_null);
  printf("\n");

  printf("Syncing state tests:\n");
  RUN_TEST(is_syncing_initial);
  RUN_TEST(is_syncing_null);
  printf("\n");

  printf("State transition tests:\n");
  RUN_TEST(state_transitions);
  RUN_TEST(get_state_null);
  printf("\n");

  printf("================================================\n");
  printf("Tests passed: %d\n", tests_passed);
  printf("Tests failed: %d\n", tests_failed);

  return tests_failed > 0 ? 1 : 0;
}
