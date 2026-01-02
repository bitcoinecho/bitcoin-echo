/*
 * Bitcoin Echo — IBD Validator Tests
 *
 * Tests for the IBD chunk validator and UTXO batch tracking.
 *
 * Build once. Build right. Stop.
 */

#include "ibd_validator.h"
#include "test_utils.h"
#include "utxo.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================
 * Helper Functions
 * ======================================================================== */

/* Create a test outpoint with deterministic values */
static void make_test_outpoint(outpoint_t *out, uint32_t seed) {
  memset(out, 0, sizeof(*out));
  /* Fill txid with seed pattern */
  for (int i = 0; i < 32; i++) {
    out->txid.bytes[i] = (uint8_t)((seed + i) & 0xFF);
  }
  out->vout = seed % 10;
}

/* Create a test UTXO entry */
static utxo_entry_t *make_test_utxo(uint32_t seed, int64_t value,
                                     uint32_t height) {
  outpoint_t outpoint;
  make_test_outpoint(&outpoint, seed);

  /* Simple P2PKH-like script */
  uint8_t script[25];
  memset(script, 0x76, sizeof(script)); /* OP_DUP filler */

  return utxo_entry_create(&outpoint, value, script, sizeof(script), height,
                           false);
}

/* ========================================================================
 * Batch Lifecycle Tests
 * ======================================================================== */

static void test_batch_create_destroy(void) {
  ibd_utxo_batch_t *batch;

  test_case("Create and destroy UTXO batch");

  batch = ibd_utxo_batch_create(100, 199);
  if (!batch) {
    test_fail("failed to create batch");
    return;
  }

  if (batch->chunk_start_height != 100) {
    test_fail_uint("wrong start height", 100, batch->chunk_start_height);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  if (batch->chunk_end_height != 199) {
    test_fail_uint("wrong end height", 199, batch->chunk_end_height);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  ibd_utxo_batch_destroy(batch);
  test_pass();
}

static void test_batch_destroy_null(void) {
  test_case("Destroy NULL batch (no crash)");

  /* Should not crash */
  ibd_utxo_batch_destroy(NULL);

  test_pass();
}

/* ========================================================================
 * UTXO Tracking Tests
 * ======================================================================== */

static void test_batch_add_created(void) {
  ibd_utxo_batch_t *batch;
  utxo_entry_t *utxo;

  test_case("Add created UTXO to batch");

  batch = ibd_utxo_batch_create(100, 199);
  if (!batch) {
    test_fail("failed to create batch");
    return;
  }

  utxo = make_test_utxo(1, 50000, 100);
  if (!utxo) {
    test_fail("failed to create UTXO");
    ibd_utxo_batch_destroy(batch);
    return;
  }

  echo_result_t result = ibd_utxo_batch_add_created(batch, utxo);
  if (result != ECHO_OK) {
    test_fail_int("add_created failed", ECHO_OK, result);
    utxo_entry_destroy(utxo);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  /* The batch now owns the UTXO, don't destroy it separately */
  ibd_utxo_batch_destroy(batch);
  test_pass();
}

static void test_batch_add_spent(void) {
  ibd_utxo_batch_t *batch;
  outpoint_t outpoint;

  test_case("Add spent outpoint to batch");

  batch = ibd_utxo_batch_create(100, 199);
  if (!batch) {
    test_fail("failed to create batch");
    return;
  }

  make_test_outpoint(&outpoint, 42);

  echo_result_t result = ibd_utxo_batch_add_spent(batch, &outpoint);
  if (result != ECHO_OK) {
    test_fail_int("add_spent failed", ECHO_OK, result);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  if (batch->spent_count != 1) {
    test_fail_uint("wrong spent count", 1, batch->spent_count);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  ibd_utxo_batch_destroy(batch);
  test_pass();
}

static void test_batch_multiple_created(void) {
  ibd_utxo_batch_t *batch;

  test_case("Add multiple created UTXOs");

  batch = ibd_utxo_batch_create(100, 199);
  if (!batch) {
    test_fail("failed to create batch");
    return;
  }

  /* Add 100 UTXOs */
  for (uint32_t i = 0; i < 100; i++) {
    utxo_entry_t *utxo = make_test_utxo(i, 1000 + i, 100);
    if (!utxo) {
      test_fail("failed to create UTXO");
      ibd_utxo_batch_destroy(batch);
      return;
    }

    echo_result_t result = ibd_utxo_batch_add_created(batch, utxo);
    if (result != ECHO_OK) {
      test_fail_int("add_created failed", ECHO_OK, result);
      utxo_entry_destroy(utxo);
      ibd_utxo_batch_destroy(batch);
      return;
    }
  }

  ibd_utxo_batch_destroy(batch);
  test_pass();
}

static void test_batch_multiple_spent(void) {
  ibd_utxo_batch_t *batch;

  test_case("Add multiple spent outpoints");

  batch = ibd_utxo_batch_create(100, 199);
  if (!batch) {
    test_fail("failed to create batch");
    return;
  }

  /* Add 100 spent outpoints */
  for (uint32_t i = 0; i < 100; i++) {
    outpoint_t outpoint;
    make_test_outpoint(&outpoint, i);

    echo_result_t result = ibd_utxo_batch_add_spent(batch, &outpoint);
    if (result != ECHO_OK) {
      test_fail_int("add_spent failed", ECHO_OK, result);
      ibd_utxo_batch_destroy(batch);
      return;
    }
  }

  if (batch->spent_count != 100) {
    test_fail_uint("wrong spent count", 100, batch->spent_count);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  ibd_utxo_batch_destroy(batch);
  test_pass();
}

/* ========================================================================
 * Created-Then-Spent Optimization Tests
 * ======================================================================== */

static void test_batch_created_then_spent(void) {
  ibd_utxo_batch_t *batch;
  utxo_entry_t *utxo;
  outpoint_t outpoint;

  test_case("Created-then-spent optimization");

  batch = ibd_utxo_batch_create(100, 199);
  if (!batch) {
    test_fail("failed to create batch");
    return;
  }

  /* Create a UTXO */
  utxo = make_test_utxo(42, 50000, 100);
  if (!utxo) {
    test_fail("failed to create UTXO");
    ibd_utxo_batch_destroy(batch);
    return;
  }

  /* Remember the outpoint before adding (batch takes ownership) */
  memcpy(&outpoint, &utxo->outpoint, sizeof(outpoint));

  echo_result_t result = ibd_utxo_batch_add_created(batch, utxo);
  if (result != ECHO_OK) {
    test_fail_int("add_created failed", ECHO_OK, result);
    utxo_entry_destroy(utxo);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  /* Now spend the same UTXO */
  result = ibd_utxo_batch_add_spent(batch, &outpoint);
  if (result != ECHO_OK) {
    test_fail_int("add_spent failed", ECHO_OK, result);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  /* The created-then-spent counter should be 1 */
  if (batch->created_then_spent_count != 1) {
    test_fail_uint("wrong created_then_spent count", 1,
                   batch->created_then_spent_count);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  ibd_utxo_batch_destroy(batch);
  test_pass();
}

static void test_batch_spent_not_created(void) {
  ibd_utxo_batch_t *batch;
  outpoint_t outpoint;

  test_case("Spent UTXO not in created set (normal case)");

  batch = ibd_utxo_batch_create(100, 199);
  if (!batch) {
    test_fail("failed to create batch");
    return;
  }

  /* Spend a UTXO that wasn't created in this batch */
  make_test_outpoint(&outpoint, 99);
  echo_result_t result = ibd_utxo_batch_add_spent(batch, &outpoint);
  if (result != ECHO_OK) {
    test_fail_int("add_spent failed", ECHO_OK, result);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  /* The created-then-spent counter should be 0 */
  if (batch->created_then_spent_count != 0) {
    test_fail_uint("wrong created_then_spent count", 0,
                   batch->created_then_spent_count);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  /* Should be recorded as a spent outpoint */
  if (batch->spent_count != 1) {
    test_fail_uint("wrong spent count", 1, batch->spent_count);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  ibd_utxo_batch_destroy(batch);
  test_pass();
}

/* ========================================================================
 * Statistics Tests
 * ======================================================================== */

static void test_batch_get_stats(void) {
  ibd_utxo_batch_t *batch;
  size_t created_count, spent_count, cancelled_count;

  test_case("Get batch statistics");

  batch = ibd_utxo_batch_create(100, 199);
  if (!batch) {
    test_fail("failed to create batch");
    return;
  }

  /* Add some UTXOs and spends */
  for (uint32_t i = 0; i < 10; i++) {
    utxo_entry_t *utxo = make_test_utxo(i, 1000, 100);
    if (utxo) {
      ibd_utxo_batch_add_created(batch, utxo);
    }
  }

  for (uint32_t i = 0; i < 5; i++) {
    outpoint_t outpoint;
    make_test_outpoint(&outpoint, i + 100); /* Different from created */
    ibd_utxo_batch_add_spent(batch, &outpoint);
  }

  ibd_utxo_batch_get_stats(batch, &created_count, &spent_count, &cancelled_count);

  if (created_count != 10) {
    test_fail_uint("wrong created_count", 10, created_count);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  if (spent_count != 5) {
    test_fail_uint("wrong spent_count", 5, spent_count);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  if (cancelled_count != 0) {
    test_fail_uint("wrong cancelled_count", 0, cancelled_count);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  ibd_utxo_batch_destroy(batch);
  test_pass();
}

/* ========================================================================
 * Lookup Tests
 * ======================================================================== */

static void test_batch_lookup_created(void) {
  ibd_utxo_batch_t *batch;
  utxo_entry_t *utxo;
  outpoint_t outpoint;

  test_case("Lookup created UTXO in batch");

  batch = ibd_utxo_batch_create(100, 199);
  if (!batch) {
    test_fail("failed to create batch");
    return;
  }

  /* Create a UTXO */
  utxo = make_test_utxo(42, 50000, 100);
  if (!utxo) {
    test_fail("failed to create UTXO");
    ibd_utxo_batch_destroy(batch);
    return;
  }

  /* Remember the outpoint */
  memcpy(&outpoint, &utxo->outpoint, sizeof(outpoint));

  echo_result_t result = ibd_utxo_batch_add_created(batch, utxo);
  if (result != ECHO_OK) {
    test_fail_int("add_created failed", ECHO_OK, result);
    utxo_entry_destroy(utxo);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  /* Lookup should find it */
  const utxo_entry_t *found = ibd_utxo_batch_lookup(batch, &outpoint);
  if (found == NULL) {
    test_fail("lookup returned NULL for existing UTXO");
    ibd_utxo_batch_destroy(batch);
    return;
  }

  if (found->value != 50000) {
    test_fail_int("wrong value in lookup result", 50000, (long)found->value);
    ibd_utxo_batch_destroy(batch);
    return;
  }

  ibd_utxo_batch_destroy(batch);
  test_pass();
}

static void test_batch_lookup_not_found(void) {
  ibd_utxo_batch_t *batch;
  outpoint_t outpoint;

  test_case("Lookup non-existent UTXO in batch");

  batch = ibd_utxo_batch_create(100, 199);
  if (!batch) {
    test_fail("failed to create batch");
    return;
  }

  make_test_outpoint(&outpoint, 999);

  /* Lookup should return NULL */
  const utxo_entry_t *found = ibd_utxo_batch_lookup(batch, &outpoint);
  if (found != NULL) {
    test_fail("lookup should return NULL for non-existent UTXO");
    ibd_utxo_batch_destroy(batch);
    return;
  }

  ibd_utxo_batch_destroy(batch);
  test_pass();
}

/* ========================================================================
 * Main Entry Point
 * ======================================================================== */

int main(void) {
  test_suite_begin("IBD Validator");

  test_section("Batch Lifecycle");
  test_batch_create_destroy();
  test_batch_destroy_null();

  test_section("UTXO Tracking");
  test_batch_add_created();
  test_batch_add_spent();
  test_batch_multiple_created();
  test_batch_multiple_spent();

  test_section("Created-Then-Spent Optimization");
  test_batch_created_then_spent();
  test_batch_spent_not_created();

  test_section("Statistics");
  test_batch_get_stats();

  test_section("Lookup");
  test_batch_lookup_created();
  test_batch_lookup_not_found();

  test_suite_end();

  return test_global_summary();
}
