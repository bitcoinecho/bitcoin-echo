/**
 * Bitcoin Echo — Mempool Unit Tests
 *
 * Tests transaction memory pool functionality:
 * - Mempool creation and destruction
 * - Transaction addition and removal
 * - Duplicate detection
 * - Fee rate calculation and prioritization
 * - Size limits and eviction
 * - Conflict detection (double-spend)
 * - Ancestor/descendant tracking
 * - Transaction selection for mining
 * - Maintenance operations (expire, trim)
 * - Block connect/disconnect handling
 */

#include "../../include/block.h"
#include "../../include/echo_types.h"
#include "../../include/mempool.h"
#include "../../include/tx.h"
#include "../../include/utxo.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_utils.h"

/*
 * ============================================================================
 * TEST UTILITIES
 * ============================================================================
 */

#define ASSERT_TRUE(cond, msg)                                                 \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL: %s (line %d): %s\n", __func__, __LINE__, msg);             \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_EQ(a, b, msg) ASSERT_TRUE((a) == (b), msg)
#define ASSERT_NE(a, b, msg) ASSERT_TRUE((a) != (b), msg)
#define ASSERT_NULL(ptr, msg) ASSERT_TRUE((ptr) == NULL, msg)
#define ASSERT_NOT_NULL(ptr, msg) ASSERT_TRUE((ptr) != NULL, msg)


/*
 * ============================================================================
 * MOCK DATA
 * ============================================================================
 */

/* Mock UTXO storage */
#define MAX_MOCK_UTXOS 100
static utxo_entry_t mock_utxos[MAX_MOCK_UTXOS];
static size_t mock_utxo_count = 0;
static uint32_t mock_height = 100000;
static uint32_t mock_median_time = 1700000000;

/* Track announced transactions */
#define MAX_ANNOUNCED 100
static hash256_t announced_txs[MAX_ANNOUNCED];
static size_t announced_count = 0;

/**
 * Add mock UTXO
 */
static void add_mock_utxo(const hash256_t *txid, uint32_t vout, int64_t value,
                          bool is_coinbase) {
  if (mock_utxo_count >= MAX_MOCK_UTXOS) {
    return;
  }

  utxo_entry_t *entry = &mock_utxos[mock_utxo_count++];
  entry->outpoint.txid = *txid;
  entry->outpoint.vout = vout;
  entry->value = value;
  entry->height = mock_height - 200; /* Mature */
  entry->is_coinbase = is_coinbase;

  /* Simple P2PKH script */
  entry->script_len = 25;
  entry->script_pubkey = malloc(25);
  if (entry->script_pubkey != NULL) {
    memset(entry->script_pubkey, 0, 25);
    entry->script_pubkey[0] = 0x76; /* OP_DUP */
    entry->script_pubkey[1] = 0xa9; /* OP_HASH160 */
    entry->script_pubkey[2] = 0x14; /* Push 20 bytes */
    entry->script_pubkey[23] = 0x88; /* OP_EQUALVERIFY */
    entry->script_pubkey[24] = 0xac; /* OP_CHECKSIG */
  }
}

/**
 * Clear mock UTXOs
 */
static void clear_mock_utxos(void) {
  for (size_t i = 0; i < mock_utxo_count; i++) {
    free(mock_utxos[i].script_pubkey);
  }
  mock_utxo_count = 0;
}

/**
 * Mock callback: get UTXO
 */
static echo_result_t mock_get_utxo(const outpoint_t *outpoint,
                                   utxo_entry_t *entry, void *ctx) {
  (void)ctx;

  for (size_t i = 0; i < mock_utxo_count; i++) {
    if (outpoint_equal(&mock_utxos[i].outpoint, outpoint)) {
      entry->outpoint = mock_utxos[i].outpoint;
      entry->value = mock_utxos[i].value;
      entry->height = mock_utxos[i].height;
      entry->is_coinbase = mock_utxos[i].is_coinbase;
      entry->script_len = mock_utxos[i].script_len;

      /* Allocate and copy script */
      entry->script_pubkey = malloc(entry->script_len);
      if (entry->script_pubkey != NULL) {
        memcpy(entry->script_pubkey, mock_utxos[i].script_pubkey,
               entry->script_len);
      }

      return ECHO_OK;
    }
  }

  return ECHO_ERR_NOT_FOUND;
}

/**
 * Mock callback: get height
 */
static uint32_t mock_get_height(void *ctx) {
  (void)ctx;
  return mock_height;
}

/**
 * Mock callback: get median time
 */
static uint32_t mock_get_median_time(void *ctx) {
  (void)ctx;
  return mock_median_time;
}

/**
 * Mock callback: announce tx
 */
static void mock_announce_tx(const hash256_t *txid, void *ctx) {
  (void)ctx;
  if (announced_count < MAX_ANNOUNCED) {
    announced_txs[announced_count++] = *txid;
  }
}

/**
 * Create test callbacks
 */
static mempool_callbacks_t create_test_callbacks(void) {
  mempool_callbacks_t cb = {.get_utxo = mock_get_utxo,
                            .get_height = mock_get_height,
                            .get_median_time = mock_get_median_time,
                            .announce_tx = mock_announce_tx,
                            .ctx = NULL};
  return cb;
}

/**
 * Create a simple test transaction.
 *
 * Creates a transaction with one input spending the given UTXO
 * and one output with the specified value (remainder is fee).
 */
static void create_test_tx(tx_t *tx, const hash256_t *input_txid,
                           uint32_t input_vout, int64_t output_value) {
  tx_init(tx);

  tx->version = 2;
  tx->locktime = 0;

  /* One input */
  tx->input_count = 1;
  tx->inputs = calloc(1, sizeof(tx_input_t));
  if (tx->inputs == NULL) {
    return;
  }

  tx->inputs[0].prevout.txid = *input_txid;
  tx->inputs[0].prevout.vout = input_vout;
  tx->inputs[0].sequence = 0xFFFFFFFE; /* RBF signaling */

  /* Simple scriptSig */
  tx->inputs[0].script_sig_len = 4;
  tx->inputs[0].script_sig = malloc(4);
  if (tx->inputs[0].script_sig != NULL) {
    memset(tx->inputs[0].script_sig, 0, 4);
  }

  /* One output */
  tx->output_count = 1;
  tx->outputs = calloc(1, sizeof(tx_output_t));
  if (tx->outputs == NULL) {
    return;
  }

  tx->outputs[0].value = output_value;

  /* Simple P2PKH script */
  tx->outputs[0].script_pubkey_len = 25;
  tx->outputs[0].script_pubkey = malloc(25);
  if (tx->outputs[0].script_pubkey != NULL) {
    memset(tx->outputs[0].script_pubkey, 0, 25);
    tx->outputs[0].script_pubkey[0] = 0x76;
    tx->outputs[0].script_pubkey[1] = 0xa9;
    tx->outputs[0].script_pubkey[2] = 0x14;
    tx->outputs[0].script_pubkey[23] = 0x88;
    tx->outputs[0].script_pubkey[24] = 0xac;
  }

  tx->has_witness = ECHO_FALSE;
}

/**
 * Create a unique txid from an index.
 */
static hash256_t make_txid(uint32_t idx) {
  hash256_t txid = {{0}};
  txid.bytes[0] = (uint8_t)(idx >> 24);
  txid.bytes[1] = (uint8_t)(idx >> 16);
  txid.bytes[2] = (uint8_t)(idx >> 8);
  txid.bytes[3] = (uint8_t)idx;
  return txid;
}

/*
 * ============================================================================
 * TEST CASES: LIFECYCLE
 * ============================================================================
 */

/**
 * Test mempool creation with default config.
 */
static void test_mempool_create_default(void) {
  mempool_t *mp = mempool_create();
  ASSERT_NOT_NULL(mp, "mempool_create should succeed");

  ASSERT_EQ(mempool_size(mp), 0, "new mempool should be empty");
  ASSERT_EQ(mempool_bytes(mp), 0, "new mempool should have 0 bytes");

  mempool_destroy(mp);

}

/**
 * Test mempool creation with custom config.
 */
static void test_mempool_create_custom_config(void) {
  mempool_config_t config = {.max_size = 1024 * 1024,
                             .min_fee_rate = 2000,
                             .expiry_time = 3600,
                             .max_ancestors = 10,
                             .max_descendants = 10,
                             .max_ancestor_size = 50000,
                             .max_descendant_size = 50000};

  mempool_t *mp = mempool_create_with_config(&config);
  ASSERT_NOT_NULL(mp, "mempool_create_with_config should succeed");

  mempool_destroy(mp);

}

/**
 * Test mempool destruction handles NULL.
 */
static void test_mempool_destroy_null(void) {
  mempool_destroy(NULL); /* Should not crash */

}

/*
 * ============================================================================
 * TEST CASES: BASIC OPERATIONS
 * ============================================================================
 */

/**
 * Test adding a simple transaction.
 */
static void test_mempool_add_simple(void) {
  mempool_t *mp = mempool_create();
  ASSERT_NOT_NULL(mp, "mempool_create should succeed");

  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  /* Create UTXO */
  hash256_t input_txid = make_txid(1);
  add_mock_utxo(&input_txid, 0, 100000, false);

  /* Create transaction (10000 sat fee) */
  tx_t tx;
  create_test_tx(&tx, &input_txid, 0, 90000);

  /* Add to mempool */
  mempool_accept_result_t result;
  echo_result_t err = mempool_add(mp, &tx, &result);

  ASSERT_EQ(err, ECHO_OK, "mempool_add should succeed");
  ASSERT_EQ(result.reason, MEMPOOL_ACCEPT_OK, "should be accepted");
  ASSERT_EQ(mempool_size(mp), 1, "mempool should have 1 tx");

  /* Verify lookup works */
  hash256_t txid;
  tx_compute_txid(&tx, &txid);

  const mempool_entry_t *entry = mempool_lookup(mp, &txid);
  ASSERT_NOT_NULL(entry, "should find tx in mempool");
  ASSERT_EQ(entry->fee, 10000, "fee should be 10000");

  tx_free(&tx);
  clear_mock_utxos();
  mempool_destroy(mp);

}

/**
 * Test duplicate rejection.
 */
static void test_mempool_reject_duplicate(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  hash256_t input_txid = make_txid(2);
  add_mock_utxo(&input_txid, 0, 100000, false);

  tx_t tx;
  create_test_tx(&tx, &input_txid, 0, 90000);

  /* Add first time */
  mempool_accept_result_t result;
  mempool_add(mp, &tx, &result);
  ASSERT_EQ(result.reason, MEMPOOL_ACCEPT_OK, "first add should succeed");

  /* Add second time */
  echo_result_t err = mempool_add(mp, &tx, &result);
  ASSERT_EQ(err, ECHO_ERR_DUPLICATE, "duplicate should be rejected");
  ASSERT_EQ(result.reason, MEMPOOL_REJECT_DUPLICATE, "reason should be duplicate");

  tx_free(&tx);
  clear_mock_utxos();
  mempool_destroy(mp);

}

/**
 * Test removing a transaction.
 */
static void test_mempool_remove(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  hash256_t input_txid = make_txid(3);
  add_mock_utxo(&input_txid, 0, 100000, false);

  tx_t tx;
  create_test_tx(&tx, &input_txid, 0, 90000);

  mempool_accept_result_t result;
  mempool_add(mp, &tx, &result);
  ASSERT_EQ(mempool_size(mp), 1, "should have 1 tx");

  hash256_t txid;
  tx_compute_txid(&tx, &txid);

  echo_result_t err = mempool_remove(mp, &txid);
  ASSERT_EQ(err, ECHO_OK, "remove should succeed");
  ASSERT_EQ(mempool_size(mp), 0, "should have 0 txs");

  /* Verify lookup fails */
  ASSERT_NULL(mempool_lookup(mp, &txid), "tx should not be found");

  tx_free(&tx);
  clear_mock_utxos();
  mempool_destroy(mp);

}

/**
 * Test removing non-existent transaction.
 */
static void test_mempool_remove_not_found(void) {
  mempool_t *mp = mempool_create();

  hash256_t txid = make_txid(999);
  echo_result_t err = mempool_remove(mp, &txid);
  ASSERT_EQ(err, ECHO_ERR_NOT_FOUND, "should return not found");

  mempool_destroy(mp);

}

/**
 * Test mempool_exists function.
 */
static void test_mempool_exists(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  hash256_t input_txid = make_txid(4);
  add_mock_utxo(&input_txid, 0, 100000, false);

  tx_t tx;
  create_test_tx(&tx, &input_txid, 0, 90000);

  hash256_t txid;
  tx_compute_txid(&tx, &txid);

  ASSERT_TRUE(!mempool_exists(mp, &txid), "should not exist before add");

  mempool_accept_result_t result;
  mempool_add(mp, &tx, &result);

  ASSERT_TRUE(mempool_exists(mp, &txid), "should exist after add");

  tx_free(&tx);
  clear_mock_utxos();
  mempool_destroy(mp);

}

/*
 * ============================================================================
 * TEST CASES: FEE VALIDATION
 * ============================================================================
 */

/**
 * Test fee rate calculation.
 */
static void test_fee_rate_calculation(void) {
  /* 1000 sat fee, 100 vbyte tx = 10000 sat/kvB */
  uint64_t rate = mempool_calc_fee_rate(1000, 100);
  ASSERT_EQ(rate, 10000, "fee rate should be 10000 sat/kvB");

  /* 500 sat fee, 250 vbyte tx = 2000 sat/kvB */
  rate = mempool_calc_fee_rate(500, 250);
  ASSERT_EQ(rate, 2000, "fee rate should be 2000 sat/kvB");

  /* Edge case: zero vsize */
  rate = mempool_calc_fee_rate(1000, 0);
  ASSERT_EQ(rate, 0, "zero vsize should give zero rate");


}

/**
 * Test low fee rejection.
 */
static void test_mempool_reject_low_fee(void) {
  /* Create mempool with high minimum fee */
  mempool_config_t config = {.max_size = MEMPOOL_DEFAULT_MAX_SIZE,
                             .min_fee_rate = 50000, /* 50 sat/vB */
                             .expiry_time = MEMPOOL_DEFAULT_EXPIRY_TIME,
                             .max_ancestors = MEMPOOL_MAX_ANCESTORS,
                             .max_descendants = MEMPOOL_MAX_DESCENDANTS,
                             .max_ancestor_size = MEMPOOL_MAX_ANCESTOR_SIZE,
                             .max_descendant_size = MEMPOOL_MAX_DESCENDANT_SIZE};

  mempool_t *mp = mempool_create_with_config(&config);
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  hash256_t input_txid = make_txid(5);
  add_mock_utxo(&input_txid, 0, 100000, false);

  /* Create transaction with low fee (1000 sat, ~100 vB = 10000 sat/kvB) */
  tx_t tx;
  create_test_tx(&tx, &input_txid, 0, 99000); /* Only 1000 sat fee */

  mempool_accept_result_t result;
  echo_result_t err = mempool_add(mp, &tx, &result);

  ASSERT_EQ(err, ECHO_ERR_INVALID, "low fee should be rejected");
  ASSERT_EQ(result.reason, MEMPOOL_REJECT_FEE_TOO_LOW, "reason should be fee too low");
  ASSERT_TRUE(result.required_fee > 0, "should indicate required fee");

  tx_free(&tx);
  clear_mock_utxos();
  mempool_destroy(mp);

}

/*
 * ============================================================================
 * TEST CASES: CONFLICT DETECTION
 * ============================================================================
 */

/**
 * Test double-spend conflict detection.
 */
static void test_mempool_conflict_detection(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  hash256_t input_txid = make_txid(6);
  add_mock_utxo(&input_txid, 0, 100000, false);

  /* Add first transaction */
  tx_t tx1;
  create_test_tx(&tx1, &input_txid, 0, 90000);

  mempool_accept_result_t result;
  mempool_add(mp, &tx1, &result);
  ASSERT_EQ(result.reason, MEMPOOL_ACCEPT_OK, "first tx should be accepted");

  /* Try to add conflicting transaction (same input) */
  tx_t tx2;
  create_test_tx(&tx2, &input_txid, 0, 89000); /* Different output value */

  echo_result_t err = mempool_add(mp, &tx2, &result);
  ASSERT_EQ(err, ECHO_ERR_INVALID, "conflict should be rejected");
  ASSERT_EQ(result.reason, MEMPOOL_REJECT_CONFLICT, "reason should be conflict");

  tx_free(&tx1);
  tx_free(&tx2);
  clear_mock_utxos();
  mempool_destroy(mp);

}

/**
 * Test mempool_is_spent function.
 */
static void test_mempool_is_spent(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  hash256_t input_txid = make_txid(7);
  add_mock_utxo(&input_txid, 0, 100000, false);

  outpoint_t outpoint = {.txid = input_txid, .vout = 0};

  ASSERT_TRUE(!mempool_is_spent(mp, &outpoint), "should not be spent before add");

  tx_t tx;
  create_test_tx(&tx, &input_txid, 0, 90000);

  mempool_accept_result_t result;
  mempool_add(mp, &tx, &result);

  ASSERT_TRUE(mempool_is_spent(mp, &outpoint), "should be spent after add");

  tx_free(&tx);
  clear_mock_utxos();
  mempool_destroy(mp);

}

/*
 * ============================================================================
 * TEST CASES: FEE ORDERING
 * ============================================================================
 */

/**
 * Test fee-rate ordering in iterator.
 */
static void test_mempool_fee_ordering(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  /* Add three transactions with different fee rates */

  /* Low fee: 1000 sat on 100000 input = ~10000 sat/kvB */
  hash256_t input1 = make_txid(10);
  add_mock_utxo(&input1, 0, 100000, false);
  tx_t tx1;
  create_test_tx(&tx1, &input1, 0, 99000);

  /* Medium fee: 5000 sat = ~50000 sat/kvB */
  hash256_t input2 = make_txid(11);
  add_mock_utxo(&input2, 0, 100000, false);
  tx_t tx2;
  create_test_tx(&tx2, &input2, 0, 95000);

  /* High fee: 10000 sat = ~100000 sat/kvB */
  hash256_t input3 = make_txid(12);
  add_mock_utxo(&input3, 0, 100000, false);
  tx_t tx3;
  create_test_tx(&tx3, &input3, 0, 90000);

  mempool_accept_result_t result;
  mempool_add(mp, &tx1, &result);
  mempool_add(mp, &tx2, &result);
  mempool_add(mp, &tx3, &result);

  ASSERT_EQ(mempool_size(mp), 3, "should have 3 txs");

  /* Iterate and verify order (highest fee first) */
  mempool_iter_t *iter = mempool_iter_by_fee(mp);
  ASSERT_NOT_NULL(iter, "iterator should be created");

  const mempool_entry_t *e1 = mempool_iter_next(iter);
  ASSERT_NOT_NULL(e1, "should get first entry");
  ASSERT_EQ(e1->fee, 10000, "first should be highest fee");

  const mempool_entry_t *e2 = mempool_iter_next(iter);
  ASSERT_NOT_NULL(e2, "should get second entry");
  ASSERT_EQ(e2->fee, 5000, "second should be medium fee");

  const mempool_entry_t *e3 = mempool_iter_next(iter);
  ASSERT_NOT_NULL(e3, "should get third entry");
  ASSERT_EQ(e3->fee, 1000, "third should be lowest fee");

  const mempool_entry_t *e4 = mempool_iter_next(iter);
  ASSERT_NULL(e4, "should have no more entries");

  mempool_iter_destroy(iter);

  tx_free(&tx1);
  tx_free(&tx2);
  tx_free(&tx3);
  clear_mock_utxos();
  mempool_destroy(mp);

}

/*
 * ============================================================================
 * TEST CASES: SIZE LIMITS
 * ============================================================================
 */

/**
 * Test mempool size limits and eviction.
 */
static void test_mempool_size_limit(void) {
  /* Create small mempool (only enough for ~2 transactions) */
  mempool_config_t config = {
      .max_size = 300, /* Very small */
      .min_fee_rate = 1000,
      .expiry_time = MEMPOOL_DEFAULT_EXPIRY_TIME,
      .max_ancestors = MEMPOOL_MAX_ANCESTORS,
      .max_descendants = MEMPOOL_MAX_DESCENDANTS,
      .max_ancestor_size = MEMPOOL_MAX_ANCESTOR_SIZE,
      .max_descendant_size = MEMPOOL_MAX_DESCENDANT_SIZE};

  mempool_t *mp = mempool_create_with_config(&config);
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  /* Add low-fee transaction first */
  hash256_t input1 = make_txid(20);
  add_mock_utxo(&input1, 0, 100000, false);
  tx_t tx1;
  create_test_tx(&tx1, &input1, 0, 99000); /* 1000 sat fee */

  mempool_accept_result_t result;
  mempool_add(mp, &tx1, &result);

  /* Add high-fee transaction that should cause eviction */
  hash256_t input2 = make_txid(21);
  add_mock_utxo(&input2, 0, 100000, false);
  tx_t tx2;
  create_test_tx(&tx2, &input2, 0, 90000); /* 10000 sat fee */

  echo_result_t err = mempool_add(mp, &tx2, &result);

  /* High-fee tx should succeed (evicting low-fee tx if needed) */
  ASSERT_EQ(err, ECHO_OK, "high-fee tx should be accepted");

  /* Verify high-fee tx is in mempool */
  hash256_t txid2;
  tx_compute_txid(&tx2, &txid2);
  ASSERT_TRUE(mempool_exists(mp, &txid2), "high-fee tx should exist");

  tx_free(&tx1);
  tx_free(&tx2);
  clear_mock_utxos();
  mempool_destroy(mp);

}

/**
 * Test mempool_trim function.
 */
static void test_mempool_trim(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  /* Add several transactions */
  for (int i = 0; i < 5; i++) {
    hash256_t input = make_txid((uint32_t)(30 + i));
    add_mock_utxo(&input, 0, 100000, false);
    tx_t tx;
    create_test_tx(&tx, &input, 0, (int64_t)(99000 - i * 1000));
    mempool_accept_result_t result;
    mempool_add(mp, &tx, &result);
    tx_free(&tx);
  }

  ASSERT_EQ(mempool_size(mp), 5, "should have 5 txs");

  /* Trim doesn't do anything if under limit */
  size_t evicted = mempool_trim(mp);
  ASSERT_EQ(evicted, 0, "should not evict anything");

  clear_mock_utxos();
  mempool_destroy(mp);

}

/*
 * ============================================================================
 * TEST CASES: UNCONFIRMED CHAINS
 * ============================================================================
 */

/**
 * Test spending unconfirmed output (parent in mempool).
 */
static void test_mempool_unconfirmed_chain(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  /* Create parent transaction */
  hash256_t input1 = make_txid(40);
  add_mock_utxo(&input1, 0, 100000, false);

  tx_t parent;
  create_test_tx(&parent, &input1, 0, 90000);

  mempool_accept_result_t result;
  mempool_add(mp, &parent, &result);
  ASSERT_EQ(result.reason, MEMPOOL_ACCEPT_OK, "parent should be accepted");

  /* Create child transaction spending parent's output */
  hash256_t parent_txid;
  tx_compute_txid(&parent, &parent_txid);

  tx_t child;
  create_test_tx(&child, &parent_txid, 0, 80000);

  echo_result_t err = mempool_add(mp, &child, &result);
  ASSERT_EQ(err, ECHO_OK, "child should be accepted");
  ASSERT_EQ(mempool_size(mp), 2, "should have 2 txs");

  /* Verify child has correct ancestor count */
  hash256_t child_txid;
  tx_compute_txid(&child, &child_txid);

  const mempool_entry_t *child_entry = mempool_lookup(mp, &child_txid);
  ASSERT_NOT_NULL(child_entry, "should find child");
  ASSERT_EQ(child_entry->ancestor_count, 2, "child should have 2 ancestors");

  tx_free(&parent);
  tx_free(&child);
  clear_mock_utxos();
  mempool_destroy(mp);

}

/**
 * Test removing parent removes descendants.
 */
static void test_mempool_remove_with_descendants(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  /* Create parent */
  hash256_t input1 = make_txid(50);
  add_mock_utxo(&input1, 0, 100000, false);

  tx_t parent;
  create_test_tx(&parent, &input1, 0, 90000);

  mempool_accept_result_t result;
  mempool_add(mp, &parent, &result);

  /* Create child */
  hash256_t parent_txid;
  tx_compute_txid(&parent, &parent_txid);

  tx_t child;
  create_test_tx(&child, &parent_txid, 0, 80000);
  mempool_add(mp, &child, &result);

  ASSERT_EQ(mempool_size(mp), 2, "should have 2 txs");

  /* Remove parent - should also remove child */
  mempool_remove(mp, &parent_txid);

  ASSERT_EQ(mempool_size(mp), 0, "should have 0 txs after removing parent");

  tx_free(&parent);
  tx_free(&child);
  clear_mock_utxos();
  mempool_destroy(mp);

}

/*
 * ============================================================================
 * TEST CASES: BLOCK HANDLING
 * ============================================================================
 */

/**
 * Test mempool_remove_for_block.
 */
static void test_mempool_remove_for_block(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  /* Add transaction to mempool */
  hash256_t input1 = make_txid(60);
  add_mock_utxo(&input1, 0, 100000, false);

  tx_t tx;
  create_test_tx(&tx, &input1, 0, 90000);

  mempool_accept_result_t result;
  mempool_add(mp, &tx, &result);
  ASSERT_EQ(mempool_size(mp), 1, "should have 1 tx");

  /* Create a mock block containing the transaction */
  block_t block;
  memset(&block, 0, sizeof(block));
  block.tx_count = 2;
  block.txs = calloc(2, sizeof(tx_t));

  /* Coinbase (empty) */
  tx_init(&block.txs[0]);
  block.txs[0].version = 1;
  block.txs[0].input_count = 1;
  block.txs[0].inputs = calloc(1, sizeof(tx_input_t));
  block.txs[0].inputs[0].prevout.vout = 0xFFFFFFFF;
  block.txs[0].output_count = 1;
  block.txs[0].outputs = calloc(1, sizeof(tx_output_t));
  block.txs[0].outputs[0].value = 50 * 100000000LL;
  block.txs[0].outputs[0].script_pubkey = malloc(1);
  block.txs[0].outputs[0].script_pubkey_len = 1;

  /* Copy our transaction */
  block.txs[1] = tx;

  /* Remove transactions for block */
  mempool_remove_for_block(mp, &block);

  ASSERT_EQ(mempool_size(mp), 0, "mempool should be empty after block");

  /* Cleanup */
  free(block.txs[0].inputs);
  free(block.txs[0].outputs[0].script_pubkey);
  free(block.txs[0].outputs);
  free(block.txs);

  tx_free(&tx);
  clear_mock_utxos();
  mempool_destroy(mp);

}

/*
 * ============================================================================
 * TEST CASES: STATISTICS
 * ============================================================================
 */

/**
 * Test mempool statistics.
 */
static void test_mempool_stats(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  /* Add a few transactions */
  for (int i = 0; i < 3; i++) {
    hash256_t input = make_txid((uint32_t)(70 + i));
    add_mock_utxo(&input, 0, 100000, false);
    tx_t tx;
    create_test_tx(&tx, &input, 0, (int64_t)(90000 - i * 1000));
    mempool_accept_result_t result;
    mempool_add(mp, &tx, &result);
    tx_free(&tx);
  }

  mempool_stats_t stats;
  mempool_get_stats(mp, &stats);

  ASSERT_EQ(stats.tx_count, 3, "should have 3 txs");
  ASSERT_TRUE(stats.total_bytes > 0, "should have positive bytes");
  ASSERT_TRUE(stats.total_vsize > 0, "should have positive vsize");
  ASSERT_TRUE(stats.total_fees > 0, "should have positive fees");
  ASSERT_TRUE(stats.min_fee_rate > 0, "should have positive min fee rate");

  clear_mock_utxos();
  mempool_destroy(mp);

}

/*
 * ============================================================================
 * TEST CASES: TRANSACTION SELECTION
 * ============================================================================
 */

/**
 * Test transaction selection for block building.
 */
static void test_mempool_select_for_block(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  /* Add several transactions */
  for (int i = 0; i < 5; i++) {
    hash256_t input = make_txid((uint32_t)(80 + i));
    add_mock_utxo(&input, 0, 100000, false);
    tx_t tx;
    create_test_tx(&tx, &input, 0, (int64_t)(90000 - i * 1000));
    mempool_accept_result_t result;
    mempool_add(mp, &tx, &result);
    tx_free(&tx);
  }

  /* Select transactions for block */
  const mempool_entry_t *selected[10];
  size_t count = 0;

  echo_result_t err =
      mempool_select_for_block(mp, selected, 10, 4000000, &count);

  ASSERT_EQ(err, ECHO_OK, "selection should succeed");
  ASSERT_EQ(count, 5, "should select all 5 txs");

  /* Verify they are in fee order */
  for (size_t i = 1; i < count; i++) {
    ASSERT_TRUE(selected[i - 1]->fee_rate >= selected[i]->fee_rate,
                "should be in descending fee order");
  }

  clear_mock_utxos();
  mempool_destroy(mp);

}

/*
 * ============================================================================
 * TEST CASES: MAINTENANCE
 * ============================================================================
 */

/**
 * Test mempool_clear function.
 */
static void test_mempool_clear(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  /* Add some transactions */
  for (int i = 0; i < 3; i++) {
    hash256_t input = make_txid((uint32_t)(90 + i));
    add_mock_utxo(&input, 0, 100000, false);
    tx_t tx;
    create_test_tx(&tx, &input, 0, 90000);
    mempool_accept_result_t result;
    mempool_add(mp, &tx, &result);
    tx_free(&tx);
  }

  ASSERT_EQ(mempool_size(mp), 3, "should have 3 txs");

  mempool_clear(mp);

  ASSERT_EQ(mempool_size(mp), 0, "should be empty after clear");
  ASSERT_EQ(mempool_bytes(mp), 0, "should have 0 bytes after clear");

  clear_mock_utxos();
  mempool_destroy(mp);

}

/*
 * ============================================================================
 * TEST CASES: UTILITY FUNCTIONS
 * ============================================================================
 */

/**
 * Test rejection reason strings.
 */
static void test_reject_strings(void) {
  const char *s;

  s = mempool_reject_string(MEMPOOL_ACCEPT_OK);
  ASSERT_NOT_NULL(s, "should have string for ACCEPT_OK");

  s = mempool_reject_string(MEMPOOL_REJECT_FEE_TOO_LOW);
  ASSERT_NOT_NULL(s, "should have string for FEE_TOO_LOW");

  s = mempool_reject_string(MEMPOOL_REJECT_DUPLICATE);
  ASSERT_NOT_NULL(s, "should have string for DUPLICATE");

  s = mempool_reject_string(MEMPOOL_REJECT_CONFLICT);
  ASSERT_NOT_NULL(s, "should have string for CONFLICT");


}

/**
 * Test mempool_accept_result_init.
 */
static void test_accept_result_init(void) {
  mempool_accept_result_t result;
  result.reason = MEMPOOL_REJECT_INVALID;
  result.required_fee = 12345;

  mempool_accept_result_init(&result);

  ASSERT_EQ(result.reason, MEMPOOL_ACCEPT_OK, "reason should be reset");
  ASSERT_EQ(result.required_fee, 0, "required_fee should be reset");
  ASSERT_EQ(result.conflicts_count, 0, "conflicts_count should be reset");


}

/*
 * ============================================================================
 * TEST CASES: EDGE CASES
 * ============================================================================
 */

/**
 * Test NULL parameter handling.
 */
static void test_null_params(void) {
  mempool_t *mp = mempool_create();

  /* mempool_add with NULL */
  ASSERT_EQ(mempool_add(NULL, NULL, NULL), ECHO_ERR_NULL_PARAM,
            "NULL mempool should fail");

  tx_t tx;
  tx_init(&tx);
  ASSERT_EQ(mempool_add(mp, NULL, NULL), ECHO_ERR_NULL_PARAM,
            "NULL tx should fail");

  /* mempool_remove with NULL */
  ASSERT_EQ(mempool_remove(NULL, NULL), ECHO_ERR_NULL_PARAM,
            "NULL params should fail");

  /* mempool_lookup with NULL */
  ASSERT_NULL(mempool_lookup(NULL, NULL), "NULL should return NULL");

  /* mempool_size with NULL */
  ASSERT_EQ(mempool_size(NULL), 0, "NULL should return 0");

  /* mempool_bytes with NULL */
  ASSERT_EQ(mempool_bytes(NULL), 0, "NULL should return 0");

  mempool_destroy(mp);

}

/**
 * Test coinbase rejection.
 */
static void test_reject_coinbase(void) {
  mempool_t *mp = mempool_create();
  mempool_callbacks_t cb = create_test_callbacks();
  mempool_set_callbacks(mp, &cb);

  /* Create coinbase transaction */
  tx_t coinbase;
  tx_init(&coinbase);
  coinbase.version = 1;
  coinbase.input_count = 1;
  coinbase.inputs = calloc(1, sizeof(tx_input_t));
  /* Null prevout (coinbase marker) */
  memset(&coinbase.inputs[0].prevout.txid, 0, 32);
  coinbase.inputs[0].prevout.vout = 0xFFFFFFFF;
  coinbase.inputs[0].sequence = 0xFFFFFFFF;
  coinbase.inputs[0].script_sig_len = 4;
  coinbase.inputs[0].script_sig = calloc(4, 1);

  coinbase.output_count = 1;
  coinbase.outputs = calloc(1, sizeof(tx_output_t));
  coinbase.outputs[0].value = 50 * 100000000LL;
  coinbase.outputs[0].script_pubkey_len = 25;
  coinbase.outputs[0].script_pubkey = calloc(25, 1);

  mempool_accept_result_t result;
  echo_result_t err = mempool_add(mp, &coinbase, &result);

  ASSERT_EQ(err, ECHO_ERR_INVALID, "coinbase should be rejected");
  ASSERT_EQ(result.reason, MEMPOOL_REJECT_INVALID, "reason should be invalid");

  tx_free(&coinbase);
  mempool_destroy(mp);

}

/*
 * ============================================================================
 * MAIN
 * ============================================================================
 */

int main(void) {
    test_suite_begin("Mempool Tests");
    test_case("Mempool create default"); test_mempool_create_default(); test_pass();
    test_case("Mempool create custom config"); test_mempool_create_custom_config(); test_pass();
    test_case("Mempool destroy null"); test_mempool_destroy_null(); test_pass();
    test_case("Mempool add simple"); test_mempool_add_simple(); test_pass();
    test_case("Mempool reject duplicate"); test_mempool_reject_duplicate(); test_pass();
    test_case("Mempool remove"); test_mempool_remove(); test_pass();
    test_case("Mempool remove not found"); test_mempool_remove_not_found(); test_pass();
    test_case("Mempool exists"); test_mempool_exists(); test_pass();
    test_case("Fee rate calculation"); test_fee_rate_calculation(); test_pass();
    test_case("Mempool reject low fee"); test_mempool_reject_low_fee(); test_pass();
    test_case("Mempool conflict detection"); test_mempool_conflict_detection(); test_pass();
    test_case("Mempool is spent"); test_mempool_is_spent(); test_pass();
    test_case("Mempool fee ordering"); test_mempool_fee_ordering(); test_pass();
    test_case("Mempool size limit"); test_mempool_size_limit(); test_pass();
    test_case("Mempool trim"); test_mempool_trim(); test_pass();
    test_case("Mempool unconfirmed chain"); test_mempool_unconfirmed_chain(); test_pass();
    test_case("Mempool remove with descendants"); test_mempool_remove_with_descendants(); test_pass();
    test_case("Mempool remove for block"); test_mempool_remove_for_block(); test_pass();
    test_case("Mempool stats"); test_mempool_stats(); test_pass();
    test_case("Mempool select for block"); test_mempool_select_for_block(); test_pass();
    test_case("Mempool clear"); test_mempool_clear(); test_pass();
    test_case("Reject strings"); test_reject_strings(); test_pass();
    test_case("Accept result init"); test_accept_result_init(); test_pass();
    test_case("Null params"); test_null_params(); test_pass();
    test_case("Reject coinbase"); test_reject_coinbase(); test_pass();

    test_suite_end();
    return test_global_summary();
}
