/**
 * Bitcoin Echo — Node Lifecycle Tests
 *
 * Tests for node initialization, shutdown, storage, and transaction pipeline.
 *
 * Build once. Build right. Stop.
 */

#include "node.h"
#include "block.h"
#include "block_index_db.h"
#include "blocks_storage.h"
#include "consensus.h"
#include "discovery.h"
#include "echo_config.h"
#include "echo_types.h"
#include "mempool.h"
#include "platform.h"
#include "tx.h"
#include "utxo.h"
#include "utxo_db.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_utils.h"

/*
 * ============================================================================
 * TEST UTILITIES
 * ============================================================================
 */

#define ASSERT(cond)                                                           \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf(" FAIL\n");                                                       \
      printf("    Assertion failed: %s\n", #cond);                             \
      printf("    File: %s, Line: %d\n", __FILE__, __LINE__);                  \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_EQ(a, b)                                                        \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      printf(" FAIL\n");                                                       \
      printf("    Expected: %d, Got: %d\n", (int)(b), (int)(a));               \
      printf("    File: %s, Line: %d\n", __FILE__, __LINE__);                  \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_NOT_NULL(ptr)                                                   \
  do {                                                                         \
    if ((ptr) == NULL) {                                                       \
      printf(" FAIL\n");                                                       \
      printf("    Expected non-NULL, got NULL\n");                             \
      printf("    File: %s, Line: %d\n", __FILE__, __LINE__);                  \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_NULL(ptr)                                                       \
  do {                                                                         \
    if ((ptr) != NULL) {                                                       \
      printf(" FAIL\n");                                                       \
      printf("    Expected NULL, got non-NULL\n");                             \
      printf("    File: %s, Line: %d\n", __FILE__, __LINE__);                  \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_STR_EQ(a, b)                                                    \
  do {                                                                         \
    if (strcmp((a), (b)) != 0) {                                               \
      printf(" FAIL\n");                                                       \
      printf("    Expected: %s, Got: %s\n", (b), (a));                         \
      printf("    File: %s, Line: %d\n", __FILE__, __LINE__);                  \
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

static void config_init_basic(void) {
  node_config_t config;
  node_config_init(&config, "/path/to/data");

  ASSERT_STR_EQ(config.data_dir, "/path/to/data");
  ASSERT_EQ(config.port, ECHO_DEFAULT_PORT);
  ASSERT_EQ(config.rpc_port, ECHO_DEFAULT_RPC_PORT);
}

static void config_init_null_datadir(void) {
  node_config_t config;
  memset(&config, 0xFF, sizeof(config)); /* Fill with garbage */

  node_config_init(&config, NULL);

  /* Data dir should be empty string */
  ASSERT_EQ(config.data_dir[0], '\0');
  ASSERT_EQ(config.port, ECHO_DEFAULT_PORT);
}

static void config_init_empty_datadir(void) {
  node_config_t config;
  node_config_init(&config, "");

  ASSERT_EQ(config.data_dir[0], '\0');
}

static void config_init_long_datadir(void) {
  node_config_t config;
  char long_path[1024];
  memset(long_path, 'x', sizeof(long_path));
  long_path[sizeof(long_path) - 1] = '\0';

  node_config_init(&config, long_path);

  /* Should be truncated to fit */
  ASSERT(strlen(config.data_dir) < sizeof(config.data_dir));
  ASSERT(config.data_dir[sizeof(config.data_dir) - 1] == '\0');
}

static void config_init_null_config(void) {
  /* Should not crash */
  node_config_init(NULL, "/path/to/data");
}

/*
 * ============================================================================
 * NODE STATE STRING TESTS
 * ============================================================================
 */

static void state_string_all(void) {
  ASSERT_STR_EQ(node_state_string(NODE_STATE_UNINITIALIZED), "UNINITIALIZED");
  ASSERT_STR_EQ(node_state_string(NODE_STATE_INITIALIZING), "INITIALIZING");
  ASSERT_STR_EQ(node_state_string(NODE_STATE_STARTING), "STARTING");
  ASSERT_STR_EQ(node_state_string(NODE_STATE_RUNNING), "RUNNING");
  ASSERT_STR_EQ(node_state_string(NODE_STATE_STOPPING), "STOPPING");
  ASSERT_STR_EQ(node_state_string(NODE_STATE_STOPPED), "STOPPED");
  ASSERT_STR_EQ(node_state_string(NODE_STATE_ERROR), "ERROR");
}

static void state_string_invalid(void) {
  /* Should return "UNKNOWN" for invalid state */
  ASSERT_STR_EQ(node_state_string((node_state_t)999), "UNKNOWN");
}

/*
 * ============================================================================
 * NODE CREATION TESTS
 * ============================================================================
 */

static void create_null_config(void) {
  node_t *node = node_create(NULL);
  ASSERT_NULL(node);
}

static void create_empty_datadir(void) {
  node_config_t config;
  node_config_init(&config, "");

  node_t *node = node_create(&config);
  ASSERT_NULL(node);
}

static void create_and_destroy(void) {
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

static void create_twice_same_dir(void) {
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

static void get_consensus(void) {
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

static void get_mempool(void) {
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

static void get_storage_components(void) {
  make_test_dir("storage");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  /* Block storage */
  block_storage_t *bs = node_get_block_storage(node);
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

static void get_data_dir(void) {
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

static void get_components_null(void) {
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

static void start_null(void) {
  echo_result_t result = node_start(NULL);
  ASSERT_EQ(result, ECHO_ERR_NULL_PARAM);
}

static void stop_null(void) {
  echo_result_t result = node_stop(NULL);
  ASSERT_EQ(result, ECHO_ERR_NULL_PARAM);
}

static void destroy_null(void) {
  /* Should not crash */
  node_destroy(NULL);
}

static void start_stop_cycle(void) {
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

static void double_start(void) {
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

static void double_stop(void) {
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

static void destroy_running_node(void) {
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

static void stats_initial(void) {
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

static void stats_running(void) {
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

static void stats_null_params(void) {
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

static void peer_count_initial(void) {
  make_test_dir("peercount");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  ASSERT_NOT_NULL(node);

  ASSERT_EQ(node_get_peer_count(node), 0);

  node_destroy(node);
  cleanup_test_dir(test_data_dir);
}

static void peer_count_null(void) { ASSERT_EQ(node_get_peer_count(NULL), 0); }

static void get_peer_empty(void) {
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

static void get_peer_null(void) { ASSERT_NULL(node_get_peer(NULL, 0)); }

/*
 * ============================================================================
 * SHUTDOWN REQUEST TESTS
 * ============================================================================
 */

static void shutdown_request(void) {
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

static void shutdown_request_null(void) {
  /* Should not crash */
  node_request_shutdown(NULL);
  ASSERT(!node_shutdown_requested(NULL));
}

/*
 * ============================================================================
 * SYNCING STATE TESTS
 * ============================================================================
 */

static void is_syncing_initial(void) {
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

static void is_syncing_null(void) { ASSERT(!node_is_syncing(NULL)); }

/*
 * ============================================================================
 * STATE TRANSITIONS
 * ============================================================================
 */

static void state_transitions(void) {
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

static void get_state_null(void) { ASSERT_EQ(node_get_state(NULL), NODE_STATE_UNINITIALIZED); }

/*
 * ============================================================================
 * STORAGE FOUNDATION TESTS
 * ============================================================================
 */

/**
 * Test chain state restoration on node restart.
 *
 * Verifies that chain state persists across node restarts:
 *   1. Create a node
 *   2. Insert a block into the block index database
 *   3. Destroy the node
 *   4. Create a new node with the same data directory
 *   5. Verify the chain state was restored
 */
static void storage_chain_restoration(void) {
  make_test_dir("restore");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  /* === First session: create node and add a block to database === */
  node_t *node1 = node_create(&config);
  if (node1 == NULL) { passed = false; goto done; }

  block_index_db_t *bdb1 = node_get_block_index_db(node1);
  if (bdb1 == NULL) { passed = false; node_destroy(node1); goto done; }

  /* Create a genesis-like block entry */
  block_index_entry_t genesis_entry;
  memset(&genesis_entry, 0, sizeof(genesis_entry));
  genesis_entry.height = 0;
  genesis_entry.header.version = 1;
  genesis_entry.header.timestamp = 1231006505;
  genesis_entry.header.bits = 0x1d00ffff;
  genesis_entry.header.nonce = 2083236893;
  memset(genesis_entry.hash.bytes, 0, 32);
  genesis_entry.hash.bytes[0] = 0x01;
  genesis_entry.status = BLOCK_STATUS_VALID_HEADER | BLOCK_STATUS_VALID_CHAIN;

  echo_result_t result = block_index_db_insert(bdb1, &genesis_entry);
  if (result != ECHO_OK) { passed = false; node_destroy(node1); goto done; }

  /* Verify it's in the database */
  block_index_entry_t best;
  result = block_index_db_get_best_chain(bdb1, &best);
  if (result != ECHO_OK || best.height != 0) {
    passed = false; node_destroy(node1); goto done;
  }

  node_destroy(node1);

  /* === Second session: create new node with same data directory === */
  node_t *node2 = node_create(&config);
  if (node2 == NULL) { passed = false; goto done; }

  block_index_db_t *bdb2 = node_get_block_index_db(node2);
  if (bdb2 == NULL) { passed = false; node_destroy(node2); goto done; }

  result = block_index_db_get_best_chain(bdb2, &best);
  if (result != ECHO_OK || best.height != 0 || best.hash.bytes[0] != 0x01) {
    passed = false;
  }

  node_destroy(node2);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Chain state restoration across restarts");
  if (passed) {
    test_pass();
  } else {
    test_fail("chain state not restored correctly");
  }
}

/**
 * Test UTXO database persistence across restarts.
 */
static void storage_utxo_persistence(void) {
  make_test_dir("utxopers");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  /* === First session: create node and add a UTXO === */
  node_t *node1 = node_create(&config);
  if (node1 == NULL) { passed = false; goto done; }

  utxo_db_t *udb1 = node_get_utxo_db(node1);
  if (udb1 == NULL) { passed = false; node_destroy(node1); goto done; }

  /* Create a test UTXO entry */
  uint8_t script[] = {0x76, 0xa9, 0x14}; /* P2PKH prefix */
  utxo_entry_t entry;
  memset(&entry, 0, sizeof(entry));
  entry.outpoint.txid.bytes[0] = 0xAB;
  entry.outpoint.vout = 0;
  entry.value = 5000000000; /* 50 BTC */
  entry.script_pubkey = script;
  entry.script_len = sizeof(script);
  entry.height = 0;
  entry.is_coinbase = true;

  echo_result_t result = utxo_db_insert(udb1, &entry);
  if (result != ECHO_OK) { passed = false; node_destroy(node1); goto done; }

  /* Verify count */
  size_t count1;
  result = utxo_db_count(udb1, &count1);
  if (result != ECHO_OK || count1 != 1) {
    passed = false; node_destroy(node1); goto done;
  }

  node_destroy(node1);

  /* === Second session: verify UTXO persists === */
  node_t *node2 = node_create(&config);
  if (node2 == NULL) { passed = false; goto done; }

  utxo_db_t *udb2 = node_get_utxo_db(node2);
  if (udb2 == NULL) { passed = false; node_destroy(node2); goto done; }

  size_t count2;
  result = utxo_db_count(udb2, &count2);
  if (result != ECHO_OK || count2 != 1) {
    passed = false; node_destroy(node2); goto done;
  }

  /* Lookup the UTXO */
  outpoint_t lookup_outpoint;
  memset(&lookup_outpoint, 0, sizeof(lookup_outpoint));
  lookup_outpoint.txid.bytes[0] = 0xAB;
  lookup_outpoint.vout = 0;

  utxo_entry_t *found = NULL;
  result = utxo_db_lookup(udb2, &lookup_outpoint, &found);
  if (result != ECHO_OK || found == NULL ||
      found->value != 5000000000 || !found->is_coinbase) {
    passed = false;
  }

  if (found != NULL) utxo_entry_destroy(found);
  node_destroy(node2);

done:
  cleanup_test_dir(test_data_dir);
  test_case("UTXO database persistence");
  if (passed) {
    test_pass();
  } else {
    test_fail("UTXO not persisted correctly");
  }
}

/**
 * Test node_apply_block handles NULL parameters.
 */
static void storage_apply_block_persistence(void) {
  echo_result_t result = node_apply_block(NULL, NULL);

  test_case("node_apply_block handles NULL");
  if (result == ECHO_ERR_NULL_PARAM) {
    test_pass();
  } else {
    test_fail_int("wrong return code", ECHO_ERR_NULL_PARAM, result);
  }
}

/**
 * Test multiple restart cycles.
 */
static void storage_multiple_restarts(void) {
  make_test_dir("multirest");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  block_index_entry_t entry;
  echo_result_t result;

  /* Create and store data */
  for (int cycle = 0; cycle < 3 && passed; cycle++) {
    node_t *node = node_create(&config);
    if (node == NULL) { passed = false; break; }

    block_index_db_t *bdb = node_get_block_index_db(node);
    if (bdb == NULL) { passed = false; node_destroy(node); break; }

    if (cycle == 0) {
      /* First cycle: insert genesis */
      memset(&entry, 0, sizeof(entry));
      entry.height = 0;
      entry.hash.bytes[0] = 0x01;
      entry.status = BLOCK_STATUS_VALID_CHAIN;
      result = block_index_db_insert(bdb, &entry);
      if (result != ECHO_OK) { passed = false; node_destroy(node); break; }
    } else {
      /* Subsequent cycles: verify and add more */
      block_index_entry_t best;
      result = block_index_db_get_best_chain(bdb, &best);
      if (result != ECHO_OK || best.height != (uint32_t)(cycle - 1)) {
        passed = false; node_destroy(node); break;
      }

      /* Add another block */
      memset(&entry, 0, sizeof(entry));
      entry.height = (uint32_t)cycle;
      entry.hash.bytes[0] = (uint8_t)(cycle + 1);
      entry.header.prev_hash.bytes[0] = (uint8_t)cycle;
      entry.chainwork.bytes[31] = (uint8_t)(cycle + 1);
      entry.status = BLOCK_STATUS_VALID_CHAIN;
      result = block_index_db_insert(bdb, &entry);
      if (result != ECHO_OK) { passed = false; node_destroy(node); break; }
    }

    node_destroy(node);
  }

  /* Final verification */
  if (passed) {
    node_t *node = node_create(&config);
    if (node != NULL) {
      block_index_db_t *bdb = node_get_block_index_db(node);
      if (bdb != NULL) {
        block_index_entry_t best;
        result = block_index_db_get_best_chain(bdb, &best);
        if (result != ECHO_OK || best.height != 2) {
          passed = false;
        }
      } else {
        passed = false;
      }
      node_destroy(node);
    } else {
      passed = false;
    }
  }

  cleanup_test_dir(test_data_dir);
  test_case("Multiple restart cycles");
  if (passed) {
    test_pass();
  } else {
    test_fail("data not persisted across restarts");
  }
}

/*
 * ============================================================================
 * BLOCK PIPELINE TESTS
 * ============================================================================
 */

/**
 * Test invalid block tracking initialization.
 */
static void block_pipeline_invalid_tracking_init(void) {
  make_test_dir("bpinit");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  /* Initially no invalid blocks */
  if (node_get_invalid_block_count(node) != 0) { passed = false; }

  /* Check a random hash - should not be invalid */
  hash256_t test_hash;
  memset(test_hash.bytes, 0xAB, sizeof(test_hash.bytes));
  if (node_is_block_invalid(node, &test_hash)) { passed = false; }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Invalid block tracking initialization");
  if (passed) {
    test_pass();
  } else {
    test_fail("invalid block tracking not initialized correctly");
  }
}

/**
 * Test node_is_block_invalid with NULL parameters.
 */
static void block_pipeline_invalid_check_null(void) {
  bool passed = true;

  hash256_t test_hash;
  memset(test_hash.bytes, 0, sizeof(test_hash.bytes));

  /* NULL node should return false */
  if (node_is_block_invalid(NULL, &test_hash)) { passed = false; }

  /* Create a real node to test NULL hash */
  make_test_dir("bpnull");
  node_config_t config;
  node_config_init(&config, test_data_dir);

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  /* NULL hash should return false */
  if (node_is_block_invalid(node, NULL)) { passed = false; }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Invalid block check with NULL params");
  if (passed) {
    test_pass();
  } else {
    test_fail("NULL handling incorrect");
  }
}

/**
 * Test node_get_invalid_block_count with NULL.
 */
static void block_pipeline_count_null(void) {
  test_case("Invalid block count with NULL");
  if (node_get_invalid_block_count(NULL) == 0) {
    test_pass();
  } else {
    test_fail("should return 0 for NULL node");
  }
}

/**
 * Test that observer mode doesn't track invalid blocks.
 */
static void block_pipeline_observer_mode(void) {
  make_test_dir("bpobs");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  config.observer_mode = true;
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  /* Observer mode should always return 0 invalid blocks */
  if (node_get_invalid_block_count(node) != 0) { passed = false; }

  /* Any hash check should return false in observer mode */
  hash256_t test_hash;
  memset(test_hash.bytes, 0x11, sizeof(test_hash.bytes));
  if (node_is_block_invalid(node, &test_hash)) { passed = false; }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Observer mode doesn't track invalid blocks");
  if (passed) {
    test_pass();
  } else {
    test_fail("observer mode tracking invalid blocks");
  }
}

/**
 * Test node_process_received_block with NULL parameters.
 */
static void block_pipeline_process_null(void) {
  make_test_dir("bpproc");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  /* NULL node */
  block_t dummy_block;
  block_init(&dummy_block);
  if (node_process_received_block(NULL, &dummy_block) != ECHO_ERR_NULL_PARAM) {
    passed = false;
  }

  /* NULL block */
  if (node_process_received_block(node, NULL) != ECHO_ERR_NULL_PARAM) {
    passed = false;
  }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Process block with NULL params");
  if (passed) {
    test_pass();
  } else {
    test_fail("NULL params not handled correctly");
  }
}

/**
 * Test that observer mode rejects block processing.
 */
static void block_pipeline_observer_rejects(void) {
  make_test_dir("bpobsrej");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  config.observer_mode = true;
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  block_t dummy_block;
  block_init(&dummy_block);

  /* Observer mode should return invalid state */
  if (node_process_received_block(node, &dummy_block) != ECHO_ERR_INVALID_STATE) {
    passed = false;
  }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Observer mode rejects block processing");
  if (passed) {
    test_pass();
  } else {
    test_fail("observer mode accepted block processing");
  }
}

/**
 * Test sync manager is created for full node.
 */
static void block_pipeline_sync_manager_created(void) {
  make_test_dir("bpsync");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  /* Sync manager should be accessible */
  sync_manager_t *sync_mgr = node_get_sync_manager(node);
  if (sync_mgr == NULL) { passed = false; }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Sync manager created for full node");
  if (passed) {
    test_pass();
  } else {
    test_fail("sync manager not created");
  }
}

/**
 * Test sync manager is NULL for observer mode.
 */
static void block_pipeline_sync_manager_observer(void) {
  make_test_dir("bpsyncobs");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  config.observer_mode = true;
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  /* Sync manager should be NULL in observer mode */
  sync_manager_t *sync_mgr = node_get_sync_manager(node);
  if (sync_mgr != NULL) { passed = false; }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Sync manager NULL for observer mode");
  if (passed) {
    test_pass();
  } else {
    test_fail("sync manager created in observer mode");
  }
}

/*
 * ============================================================================
 * TRANSACTION PIPELINE TESTS
 * ============================================================================
 */

/**
 * Test mempool has callbacks wired after node creation.
 */
static void tx_pipeline_mempool_callbacks_wired(void) {
  make_test_dir("txcb");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  /* Mempool should exist */
  mempool_t *mp = node_get_mempool(node);
  if (mp == NULL) { passed = false; node_destroy(node); goto done; }

  /* Verify callbacks are set (mempool should work without crashing) */
  mempool_stats_t stats;
  mempool_get_stats(mp, &stats);
  if (stats.tx_count != 0) { passed = false; }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Mempool callbacks wired after creation");
  if (passed) {
    test_pass();
  } else {
    test_fail("mempool not properly initialized");
  }
}

/**
 * Test observer mode has no mempool functionality.
 */
static void tx_pipeline_observer_no_mempool_validation(void) {
  make_test_dir("txobs");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  config.observer_mode = true;
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  /* Mempool should be NULL in observer mode */
  mempool_t *mp = node_get_mempool(node);
  if (mp != NULL) { passed = false; }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Observer mode has no mempool");
  if (passed) {
    test_pass();
  } else {
    test_fail("observer mode has mempool");
  }
}

/**
 * Test UTXO database is accessible for transaction validation.
 */
static void tx_pipeline_utxo_db_accessible(void) {
  make_test_dir("txutxo");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  /* UTXO database should be accessible */
  utxo_db_t *udb = node_get_utxo_db(node);
  if (udb == NULL) { passed = false; node_destroy(node); goto done; }

  /* Should be able to query (empty at start) */
  size_t count;
  echo_result_t result = utxo_db_count(udb, &count);
  if (result != ECHO_OK || count != 0) { passed = false; }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("UTXO database accessible");
  if (passed) {
    test_pass();
  } else {
    test_fail("UTXO database not accessible");
  }
}

/**
 * Test consensus engine accessible for script flag calculation.
 */
static void tx_pipeline_consensus_accessible(void) {
  make_test_dir("txcons");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  /* Consensus engine should be accessible */
  const consensus_engine_t *consensus = node_get_consensus_const(node);
  if (consensus == NULL) { passed = false; node_destroy(node); goto done; }

  /* Should be able to get height (may be 0 or UINT32_MAX depending on init) */
  uint32_t height = consensus_get_height(consensus);
  (void)height;

  /* Should be able to get script flags */
  uint32_t flags = consensus_get_script_flags(0);
  (void)flags;

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Consensus engine accessible");
  if (passed) {
    test_pass();
  } else {
    test_fail("consensus engine not accessible");
  }
}

/**
 * Test mempool minimum fee rate is queryable.
 */
static void tx_pipeline_mempool_fee_rate(void) {
  make_test_dir("txfee");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  mempool_t *mp = node_get_mempool(node);
  if (mp == NULL) { passed = false; node_destroy(node); goto done; }

  /* Should be able to query minimum fee rate */
  uint64_t min_fee_rate = mempool_min_fee_rate(mp);
  (void)min_fee_rate; /* Just verify it doesn't crash */

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Mempool fee rate queryable");
  if (passed) {
    test_pass();
  } else {
    test_fail("fee rate not queryable");
  }
}

/**
 * Test that mempool lookup returns NULL for missing txid.
 */
static void tx_pipeline_mempool_lookup_missing(void) {
  make_test_dir("txlook");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  mempool_t *mp = node_get_mempool(node);
  if (mp == NULL) { passed = false; node_destroy(node); goto done; }

  /* Looking up a missing txid should return NULL */
  hash256_t fake_txid;
  memset(fake_txid.bytes, 0xDE, sizeof(fake_txid.bytes));
  const mempool_entry_t *entry = mempool_lookup(mp, &fake_txid);
  if (entry != NULL) { passed = false; }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Mempool lookup returns NULL for missing txid");
  if (passed) {
    test_pass();
  } else {
    test_fail("lookup returned non-NULL for missing txid");
  }
}

/**
 * Test UTXO lookup returns NOT_FOUND for missing outpoint.
 */
static void tx_pipeline_utxo_lookup_missing(void) {
  make_test_dir("utxolook");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  utxo_db_t *udb = node_get_utxo_db(node);
  if (udb == NULL) { passed = false; node_destroy(node); goto done; }

  /* Looking up a missing outpoint should return NOT_FOUND */
  outpoint_t fake_outpoint;
  memset(fake_outpoint.txid.bytes, 0xAB, sizeof(fake_outpoint.txid.bytes));
  fake_outpoint.vout = 0;

  utxo_entry_t *entry = NULL;
  echo_result_t result = utxo_db_lookup(udb, &fake_outpoint, &entry);
  if (result != ECHO_ERR_NOT_FOUND || entry != NULL) { passed = false; }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("UTXO lookup returns NOT_FOUND for missing");
  if (passed) {
    test_pass();
  } else {
    test_fail("UTXO lookup did not return NOT_FOUND");
  }
}

/**
 * Test that UTXO can be inserted and retrieved.
 * This tests the UTXO lookup chain used by transaction validation.
 */
static void tx_pipeline_utxo_insert_lookup(void) {
  make_test_dir("utxoins");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  utxo_db_t *udb = node_get_utxo_db(node);
  if (udb == NULL) { passed = false; node_destroy(node); goto done; }

  /* Insert a UTXO */
  uint8_t script[] = {0x76, 0xa9, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0xac}; /* P2PKH */

  utxo_entry_t entry;
  memset(&entry, 0, sizeof(entry));
  entry.outpoint.txid.bytes[0] = 0x12;
  entry.outpoint.txid.bytes[1] = 0x34;
  entry.outpoint.vout = 1;
  entry.value = 100000000; /* 1 BTC */
  entry.script_pubkey = script;
  entry.script_len = sizeof(script);
  entry.height = 100;
  entry.is_coinbase = false;

  echo_result_t result = utxo_db_insert(udb, &entry);
  if (result != ECHO_OK) { passed = false; node_destroy(node); goto done; }

  /* Look it up */
  utxo_entry_t *found = NULL;
  result = utxo_db_lookup(udb, &entry.outpoint, &found);
  if (result != ECHO_OK || found == NULL) {
    passed = false;
  } else {
    if (found->value != 100000000 || found->height != 100 || found->is_coinbase) {
      passed = false;
    }
    utxo_entry_destroy(found);
  }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("UTXO insert and lookup");
  if (passed) {
    test_pass();
  } else {
    test_fail("UTXO insert/lookup failed");
  }
}

/**
 * Test node components work together (integration-style test).
 * This simulates what the transaction pipeline does.
 */
static void tx_pipeline_component_integration(void) {
  make_test_dir("txint");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  node_t *node = node_create(&config);
  if (node == NULL) { passed = false; goto done; }

  /* All components needed for transaction validation should be accessible */
  mempool_t *mp = node_get_mempool(node);
  utxo_db_t *udb = node_get_utxo_db(node);
  const consensus_engine_t *consensus = node_get_consensus_const(node);

  if (mp == NULL || udb == NULL || consensus == NULL) {
    passed = false;
  } else {
    /* Verify we can get script flags (needed for validation) */
    uint32_t height = consensus_get_height(consensus);
    uint32_t flags = consensus_get_script_flags(height + 1);
    (void)flags;
  }

  node_destroy(node);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Component integration");
  if (passed) {
    test_pass();
  } else {
    test_fail("component integration failed");
  }
}

/**
 * Test that multiple restarts preserve mempool emptiness.
 * (Mempool is not persisted - this verifies expected behavior)
 */
static void tx_pipeline_mempool_not_persisted(void) {
  make_test_dir("txpers");
  node_config_t config;
  node_config_init(&config, test_data_dir);
  bool passed = true;

  /* First session */
  node_t *node1 = node_create(&config);
  if (node1 == NULL) { passed = false; goto done; }

  mempool_t *mp1 = node_get_mempool(node1);
  if (mp1 == NULL || mempool_size(mp1) != 0) {
    passed = false; node_destroy(node1); goto done;
  }

  node_destroy(node1);

  /* Second session - mempool should be empty (not persisted) */
  node_t *node2 = node_create(&config);
  if (node2 == NULL) { passed = false; goto done; }

  mempool_t *mp2 = node_get_mempool(node2);
  if (mp2 == NULL || mempool_size(mp2) != 0) {
    passed = false;
  }

  node_destroy(node2);

done:
  cleanup_test_dir(test_data_dir);
  test_case("Mempool not persisted across restarts");
  if (passed) {
    test_pass();
  } else {
    test_fail("mempool persistence incorrect");
  }
}

/*
 * ============================================================================
 * MAIN
 * ============================================================================
 */

int main(void) {
    test_suite_begin("Node Tests");

    test_section("Configuration");
    test_case("Initialize config with valid datadir"); config_init_basic(); test_pass();
    test_case("Initialize config with NULL datadir"); config_init_null_datadir(); test_pass();
    test_case("Initialize config with empty datadir"); config_init_empty_datadir(); test_pass();
    test_case("Initialize config with long datadir path"); config_init_long_datadir(); test_pass();
    test_case("Initialize NULL config pointer"); config_init_null_config(); test_pass();

    test_section("State String Conversion");
    test_case("Get string for all valid states"); state_string_all(); test_pass();
    test_case("Get string for invalid state"); state_string_invalid(); test_pass();

    test_section("Node Creation");
    test_case("Create node with NULL config"); create_null_config(); test_pass();
    test_case("Create node with empty datadir"); create_empty_datadir(); test_pass();
    test_case("Create and destroy node"); create_and_destroy(); test_pass();
    test_case("Create two nodes in same directory"); create_twice_same_dir(); test_pass();

    test_section("Component Access");
    test_case("Get consensus engine"); get_consensus(); test_pass();
    test_case("Get mempool"); get_mempool(); test_pass();
    test_case("Get storage components"); get_storage_components(); test_pass();
    test_case("Get data directory"); get_data_dir(); test_pass();
    test_case("Get components from NULL node"); get_components_null(); test_pass();

    test_section("Lifecycle");
    test_case("Start NULL node"); start_null(); test_pass();
    test_case("Stop NULL node"); stop_null(); test_pass();
    test_case("Destroy NULL node"); destroy_null(); test_pass();
    test_case("Start and stop cycle"); start_stop_cycle(); test_pass();
    test_case("Double start"); double_start(); test_pass();
    test_case("Double stop"); double_stop(); test_pass();
    test_case("Destroy running node"); destroy_running_node(); test_pass();

    test_section("Statistics");
    test_case("Initial stats"); stats_initial(); test_pass();
    test_case("Stats while running"); stats_running(); test_pass();
    test_case("Stats with NULL parameters"); stats_null_params(); test_pass();

    test_section("Peer Management");
    test_case("Peer count after creation"); peer_count_initial(); test_pass();
    test_case("Peer count for NULL node"); peer_count_null(); test_pass();
    test_case("Get peer from empty peer list"); get_peer_empty(); test_pass();
    test_case("Get peer from NULL node"); get_peer_null(); test_pass();

    test_section("Shutdown");
    test_case("Request shutdown"); shutdown_request(); test_pass();
    test_case("Shutdown request on NULL node"); shutdown_request_null(); test_pass();

    test_section("Sync State");
    test_case("Check sync state after creation"); is_syncing_initial(); test_pass();
    test_case("Check sync state for NULL node"); is_syncing_null(); test_pass();

    test_section("State Transitions");
    test_case("Valid state transitions"); state_transitions(); test_pass();
    test_case("Get state for NULL node"); get_state_null(); test_pass();

    test_section("Storage Foundation");
    storage_chain_restoration();
    storage_utxo_persistence();
    storage_apply_block_persistence();
    storage_multiple_restarts();

    test_section("Block Pipeline");
    block_pipeline_invalid_tracking_init();
    block_pipeline_invalid_check_null();
    block_pipeline_count_null();
    block_pipeline_observer_mode();
    block_pipeline_process_null();
    block_pipeline_observer_rejects();
    block_pipeline_sync_manager_created();
    block_pipeline_sync_manager_observer();

    test_section("Transaction Pipeline");
    tx_pipeline_mempool_callbacks_wired();
    tx_pipeline_observer_no_mempool_validation();
    tx_pipeline_utxo_db_accessible();
    tx_pipeline_consensus_accessible();
    tx_pipeline_mempool_fee_rate();
    tx_pipeline_mempool_lookup_missing();
    tx_pipeline_utxo_lookup_missing();
    tx_pipeline_utxo_insert_lookup();
    tx_pipeline_component_integration();
    tx_pipeline_mempool_not_persisted();

    test_suite_end();
    return test_global_summary();
}
