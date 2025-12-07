/*
 * Bitcoin Echo — Transaction Validation Tests
 *
 * Tests for transaction validation including:
 *   - Syntactic validation
 *   - Size and count limits
 *   - Duplicate input detection
 *   - Value range validation
 *   - Script execution
 *   - Locktime/sequence validation
 *
 * Build once. Build right. Stop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tx_validate.h"
#include "tx.h"
#include "script.h"
#include "sha256.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    printf("  %-50s ", name); \
    tests_run++; \
} while(0)

#define PASS() do { \
    printf("[PASS]\n"); \
    tests_passed++; \
} while(0)

#define FAIL(msg) do { \
    printf("[FAIL] %s\n", msg); \
} while(0)

/*
 * Helper: Create a minimal valid transaction.
 */
static void create_minimal_tx(tx_t *tx)
{
    tx_init(tx);
    tx->version = 1;
    tx->locktime = 0;

    /* One input with dummy prevout */
    tx->input_count = 1;
    tx->inputs = calloc(1, sizeof(tx_input_t));
    memset(tx->inputs[0].prevout.txid.bytes, 0x11, 32);
    tx->inputs[0].prevout.vout = 0;
    tx->inputs[0].sequence = TX_SEQUENCE_FINAL;
    tx->inputs[0].script_sig = malloc(1);
    tx->inputs[0].script_sig[0] = OP_TRUE;
    tx->inputs[0].script_sig_len = 1;

    /* One output */
    tx->output_count = 1;
    tx->outputs = calloc(1, sizeof(tx_output_t));
    tx->outputs[0].value = 50 * ECHO_SATOSHIS_PER_BTC;
    tx->outputs[0].script_pubkey = malloc(1);
    tx->outputs[0].script_pubkey[0] = OP_TRUE;
    tx->outputs[0].script_pubkey_len = 1;
}

/*
 * Helper: Create a coinbase transaction.
 */
static void create_coinbase_tx(tx_t *tx)
{
    tx_init(tx);
    tx->version = 1;
    tx->locktime = 0;

    /* Coinbase input: null prevout */
    tx->input_count = 1;
    tx->inputs = calloc(1, sizeof(tx_input_t));
    memset(tx->inputs[0].prevout.txid.bytes, 0, 32);
    tx->inputs[0].prevout.vout = TX_COINBASE_VOUT;
    tx->inputs[0].sequence = TX_SEQUENCE_FINAL;

    /* Coinbase script (arbitrary data, must be 2-100 bytes) */
    tx->inputs[0].script_sig_len = 4;
    tx->inputs[0].script_sig = malloc(4);
    tx->inputs[0].script_sig[0] = 0x03;  /* Height encoding */
    tx->inputs[0].script_sig[1] = 0x01;
    tx->inputs[0].script_sig[2] = 0x00;
    tx->inputs[0].script_sig[3] = 0x00;

    /* Output: block reward */
    tx->output_count = 1;
    tx->outputs = calloc(1, sizeof(tx_output_t));
    tx->outputs[0].value = 50 * ECHO_SATOSHIS_PER_BTC;
    tx->outputs[0].script_pubkey = malloc(1);
    tx->outputs[0].script_pubkey[0] = OP_TRUE;
    tx->outputs[0].script_pubkey_len = 1;
}

/*
 * ============================================================================
 * SYNTACTIC VALIDATION TESTS
 * ============================================================================
 */

static void test_validate_null_tx(void)
{
    TEST("NULL transaction");
    tx_validate_result_t result;
    echo_result_t res = tx_validate_syntax(NULL, &result);
    if (res != ECHO_OK && result.error == TX_VALIDATE_ERR_NULL) {
        PASS();
    } else {
        FAIL("Expected TX_VALIDATE_ERR_NULL");
    }
}

static void test_validate_empty_inputs(void)
{
    TEST("Empty inputs");
    tx_t tx;
    tx_init(&tx);
    tx.version = 1;
    tx.input_count = 0;
    tx.output_count = 1;
    tx.outputs = calloc(1, sizeof(tx_output_t));
    tx.outputs[0].value = 1000;
    tx.outputs[0].script_pubkey = malloc(1);
    tx.outputs[0].script_pubkey[0] = OP_TRUE;
    tx.outputs[0].script_pubkey_len = 1;

    tx_validate_result_t result;
    echo_result_t res = tx_validate_syntax(&tx, &result);
    tx_free(&tx);

    if (res != ECHO_OK && result.error == TX_VALIDATE_ERR_EMPTY_INPUTS) {
        PASS();
    } else {
        FAIL("Expected TX_VALIDATE_ERR_EMPTY_INPUTS");
    }
}

static void test_validate_empty_outputs(void)
{
    TEST("Empty outputs");
    tx_t tx;
    tx_init(&tx);
    tx.version = 1;
    tx.input_count = 1;
    tx.inputs = calloc(1, sizeof(tx_input_t));
    memset(tx.inputs[0].prevout.txid.bytes, 0x11, 32);
    tx.inputs[0].prevout.vout = 0;
    tx.inputs[0].sequence = TX_SEQUENCE_FINAL;
    tx.inputs[0].script_sig = malloc(1);
    tx.inputs[0].script_sig[0] = OP_TRUE;
    tx.inputs[0].script_sig_len = 1;
    tx.output_count = 0;

    tx_validate_result_t result;
    echo_result_t res = tx_validate_syntax(&tx, &result);
    tx_free(&tx);

    if (res != ECHO_OK && result.error == TX_VALIDATE_ERR_EMPTY_OUTPUTS) {
        PASS();
    } else {
        FAIL("Expected TX_VALIDATE_ERR_EMPTY_OUTPUTS");
    }
}

static void test_validate_minimal_valid(void)
{
    TEST("Minimal valid transaction");
    tx_t tx;
    create_minimal_tx(&tx);

    tx_validate_result_t result;
    echo_result_t res = tx_validate_syntax(&tx, &result);
    tx_free(&tx);

    if (res == ECHO_OK) {
        PASS();
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Unexpected error: %s",
                 tx_validate_error_string(result.error));
        FAIL(buf);
    }
}

static void test_validate_coinbase_valid(void)
{
    TEST("Valid coinbase transaction");
    tx_t tx;
    create_coinbase_tx(&tx);

    tx_validate_result_t result;
    echo_result_t res = tx_validate_syntax(&tx, &result);
    tx_free(&tx);

    if (res == ECHO_OK) {
        PASS();
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Unexpected error: %s",
                 tx_validate_error_string(result.error));
        FAIL(buf);
    }
}

static void test_validate_coinbase_script_too_short(void)
{
    TEST("Coinbase script too short");
    tx_t tx;
    create_coinbase_tx(&tx);

    /* Make script too short (< 2 bytes) */
    free(tx.inputs[0].script_sig);
    tx.inputs[0].script_sig = malloc(1);
    tx.inputs[0].script_sig[0] = 0x01;
    tx.inputs[0].script_sig_len = 1;

    tx_validate_result_t result;
    echo_result_t res = tx_validate_syntax(&tx, &result);
    tx_free(&tx);

    if (res != ECHO_OK && result.error == TX_VALIDATE_ERR_COINBASE_SCRIPT_SIZE) {
        PASS();
    } else {
        FAIL("Expected TX_VALIDATE_ERR_COINBASE_SCRIPT_SIZE");
    }
}

static void test_validate_coinbase_script_too_long(void)
{
    TEST("Coinbase script too long");
    tx_t tx;
    create_coinbase_tx(&tx);

    /* Make script too long (> 100 bytes) */
    free(tx.inputs[0].script_sig);
    tx.inputs[0].script_sig_len = 101;
    tx.inputs[0].script_sig = calloc(101, 1);

    tx_validate_result_t result;
    echo_result_t res = tx_validate_syntax(&tx, &result);
    tx_free(&tx);

    if (res != ECHO_OK && result.error == TX_VALIDATE_ERR_COINBASE_SCRIPT_SIZE) {
        PASS();
    } else {
        FAIL("Expected TX_VALIDATE_ERR_COINBASE_SCRIPT_SIZE");
    }
}

/*
 * ============================================================================
 * DUPLICATE INPUT TESTS
 * ============================================================================
 */

static void test_validate_duplicate_inputs(void)
{
    TEST("Duplicate inputs");
    tx_t tx;
    tx_init(&tx);
    tx.version = 1;

    /* Two inputs spending the same outpoint */
    tx.input_count = 2;
    tx.inputs = calloc(2, sizeof(tx_input_t));

    for (int i = 0; i < 2; i++) {
        memset(tx.inputs[i].prevout.txid.bytes, 0xAA, 32);
        tx.inputs[i].prevout.vout = 0;  /* Same outpoint */
        tx.inputs[i].sequence = TX_SEQUENCE_FINAL;
        tx.inputs[i].script_sig = malloc(1);
        tx.inputs[i].script_sig[0] = OP_TRUE;
        tx.inputs[i].script_sig_len = 1;
    }

    tx.output_count = 1;
    tx.outputs = calloc(1, sizeof(tx_output_t));
    tx.outputs[0].value = 1000;
    tx.outputs[0].script_pubkey = malloc(1);
    tx.outputs[0].script_pubkey[0] = OP_TRUE;
    tx.outputs[0].script_pubkey_len = 1;

    tx_validate_result_t result;
    echo_result_t res = tx_validate_syntax(&tx, &result);
    tx_free(&tx);

    if (res != ECHO_OK && result.error == TX_VALIDATE_ERR_DUPLICATE_INPUT) {
        PASS();
    } else {
        FAIL("Expected TX_VALIDATE_ERR_DUPLICATE_INPUT");
    }
}

static void test_validate_different_inputs(void)
{
    TEST("Different inputs (same txid, different vout)");
    tx_t tx;
    tx_init(&tx);
    tx.version = 1;

    /* Two inputs spending different outputs from same tx */
    tx.input_count = 2;
    tx.inputs = calloc(2, sizeof(tx_input_t));

    for (int i = 0; i < 2; i++) {
        memset(tx.inputs[i].prevout.txid.bytes, 0xAA, 32);
        tx.inputs[i].prevout.vout = (uint32_t)i;  /* Different vouts */
        tx.inputs[i].sequence = TX_SEQUENCE_FINAL;
        tx.inputs[i].script_sig = malloc(1);
        tx.inputs[i].script_sig[0] = OP_TRUE;
        tx.inputs[i].script_sig_len = 1;
    }

    tx.output_count = 1;
    tx.outputs = calloc(1, sizeof(tx_output_t));
    tx.outputs[0].value = 1000;
    tx.outputs[0].script_pubkey = malloc(1);
    tx.outputs[0].script_pubkey[0] = OP_TRUE;
    tx.outputs[0].script_pubkey_len = 1;

    tx_validate_result_t result;
    echo_result_t res = tx_validate_syntax(&tx, &result);
    tx_free(&tx);

    if (res == ECHO_OK) {
        PASS();
    } else {
        FAIL("Should allow different vouts from same txid");
    }
}

/*
 * ============================================================================
 * VALUE VALIDATION TESTS
 * ============================================================================
 */

static void test_validate_negative_output(void)
{
    TEST("Negative output value");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.outputs[0].value = -1;

    tx_validate_result_t result;
    echo_result_t res = tx_validate_syntax(&tx, &result);
    tx_free(&tx);

    if (res != ECHO_OK && result.error == TX_VALIDATE_ERR_NEGATIVE_VALUE) {
        PASS();
    } else {
        FAIL("Expected TX_VALIDATE_ERR_NEGATIVE_VALUE");
    }
}

static void test_validate_output_too_large(void)
{
    TEST("Output value exceeds 21M BTC");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.outputs[0].value = ECHO_MAX_SATOSHIS + 1;

    tx_validate_result_t result;
    echo_result_t res = tx_validate_syntax(&tx, &result);
    tx_free(&tx);

    if (res != ECHO_OK && result.error == TX_VALIDATE_ERR_VALUE_TOO_LARGE) {
        PASS();
    } else {
        FAIL("Expected TX_VALIDATE_ERR_VALUE_TOO_LARGE");
    }
}

static void test_validate_output_at_max(void)
{
    TEST("Output value at exactly 21M BTC");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.outputs[0].value = ECHO_MAX_SATOSHIS;

    tx_validate_result_t result;
    echo_result_t res = tx_validate_syntax(&tx, &result);
    tx_free(&tx);

    if (res == ECHO_OK) {
        PASS();
    } else {
        FAIL("Max satoshi value should be valid");
    }
}

static void test_validate_total_overflow(void)
{
    TEST("Total output value overflow");
    tx_t tx;
    tx_init(&tx);
    tx.version = 1;

    tx.input_count = 1;
    tx.inputs = calloc(1, sizeof(tx_input_t));
    memset(tx.inputs[0].prevout.txid.bytes, 0x11, 32);
    tx.inputs[0].prevout.vout = 0;
    tx.inputs[0].sequence = TX_SEQUENCE_FINAL;
    tx.inputs[0].script_sig = malloc(1);
    tx.inputs[0].script_sig[0] = OP_TRUE;
    tx.inputs[0].script_sig_len = 1;

    /* Two outputs that together overflow */
    tx.output_count = 2;
    tx.outputs = calloc(2, sizeof(tx_output_t));
    tx.outputs[0].value = ECHO_MAX_SATOSHIS;
    tx.outputs[0].script_pubkey = malloc(1);
    tx.outputs[0].script_pubkey[0] = OP_TRUE;
    tx.outputs[0].script_pubkey_len = 1;
    tx.outputs[1].value = 1;
    tx.outputs[1].script_pubkey = malloc(1);
    tx.outputs[1].script_pubkey[0] = OP_TRUE;
    tx.outputs[1].script_pubkey_len = 1;

    tx_validate_result_t result;
    echo_result_t res = tx_validate_syntax(&tx, &result);
    tx_free(&tx);

    if (res != ECHO_OK && result.error == TX_VALIDATE_ERR_TOTAL_OVERFLOW) {
        PASS();
    } else {
        FAIL("Expected TX_VALIDATE_ERR_TOTAL_OVERFLOW");
    }
}

/*
 * ============================================================================
 * LOCKTIME TESTS
 * ============================================================================
 */

static void test_locktime_zero_always_valid(void)
{
    TEST("Locktime 0 always valid");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.locktime = 0;
    tx.inputs[0].sequence = 0;  /* Non-final */

    echo_bool_t satisfied = tx_locktime_satisfied(&tx, 0, 0);
    tx_free(&tx);

    if (satisfied) {
        PASS();
    } else {
        FAIL("Locktime 0 should always be satisfied");
    }
}

static void test_locktime_block_height_not_reached(void)
{
    TEST("Locktime block height not reached");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.locktime = 100;
    tx.inputs[0].sequence = 0;  /* Non-final to enable locktime */

    echo_bool_t satisfied = tx_locktime_satisfied(&tx, 50, 0);
    tx_free(&tx);

    if (!satisfied) {
        PASS();
    } else {
        FAIL("Should fail - block height not reached");
    }
}

static void test_locktime_block_height_reached(void)
{
    TEST("Locktime block height reached");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.locktime = 100;
    tx.inputs[0].sequence = 0;  /* Non-final */

    echo_bool_t satisfied = tx_locktime_satisfied(&tx, 100, 0);
    tx_free(&tx);

    if (satisfied) {
        PASS();
    } else {
        FAIL("Locktime should be satisfied at exact height");
    }
}

static void test_locktime_timestamp_not_reached(void)
{
    TEST("Locktime timestamp not reached");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.locktime = 500000001;  /* Above threshold = timestamp */
    tx.inputs[0].sequence = 0;  /* Non-final */

    echo_bool_t satisfied = tx_locktime_satisfied(&tx, 0, 500000000);
    tx_free(&tx);

    if (!satisfied) {
        PASS();
    } else {
        FAIL("Should fail - timestamp not reached");
    }
}

static void test_locktime_timestamp_reached(void)
{
    TEST("Locktime timestamp reached");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.locktime = 500000001;
    tx.inputs[0].sequence = 0;  /* Non-final */

    echo_bool_t satisfied = tx_locktime_satisfied(&tx, 0, 500000001);
    tx_free(&tx);

    if (satisfied) {
        PASS();
    } else {
        FAIL("Locktime should be satisfied");
    }
}

static void test_locktime_all_final_sequences(void)
{
    TEST("All final sequences bypass locktime");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.locktime = 1000000;  /* Far future */
    tx.inputs[0].sequence = TX_SEQUENCE_FINAL;

    echo_bool_t satisfied = tx_locktime_satisfied(&tx, 0, 0);
    tx_free(&tx);

    if (satisfied) {
        PASS();
    } else {
        FAIL("Final sequences should bypass locktime");
    }
}

/*
 * ============================================================================
 * RELATIVE LOCKTIME (BIP-68) TESTS
 * ============================================================================
 */

static void test_sequence_disabled(void)
{
    TEST("Relative locktime disabled flag");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.version = 2;
    tx.inputs[0].sequence = SEQUENCE_LOCKTIME_DISABLE_FLAG | 1000;

    echo_bool_t satisfied = tx_sequence_satisfied(&tx, 0, 0, 0, 0, 0);
    tx_free(&tx);

    if (satisfied) {
        PASS();
    } else {
        FAIL("Disabled flag should always satisfy");
    }
}

static void test_sequence_blocks_not_reached(void)
{
    TEST("Relative locktime blocks not reached");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.version = 2;
    tx.inputs[0].sequence = 10;  /* 10 blocks */

    /* UTXO at height 100, current height 105 (only 5 blocks) */
    echo_bool_t satisfied = tx_sequence_satisfied(&tx, 0, 100, 0, 105, 0);
    tx_free(&tx);

    if (!satisfied) {
        PASS();
    } else {
        FAIL("Should fail - not enough blocks");
    }
}

static void test_sequence_blocks_reached(void)
{
    TEST("Relative locktime blocks reached");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.version = 2;
    tx.inputs[0].sequence = 10;  /* 10 blocks */

    /* UTXO at height 100, current height 110 */
    echo_bool_t satisfied = tx_sequence_satisfied(&tx, 0, 100, 0, 110, 0);
    tx_free(&tx);

    if (satisfied) {
        PASS();
    } else {
        FAIL("Should pass - 10 blocks elapsed");
    }
}

static void test_sequence_time_not_reached(void)
{
    TEST("Relative locktime time not reached");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.version = 2;
    /* Time-based: 10 * 512 seconds = 5120 seconds */
    tx.inputs[0].sequence = SEQUENCE_LOCKTIME_TYPE_FLAG | 10;

    /* UTXO at time 1000, current time 5000 (only 4000s elapsed) */
    echo_bool_t satisfied = tx_sequence_satisfied(&tx, 0, 0, 1000, 0, 5000);
    tx_free(&tx);

    if (!satisfied) {
        PASS();
    } else {
        FAIL("Should fail - not enough time");
    }
}

static void test_sequence_time_reached(void)
{
    TEST("Relative locktime time reached");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.version = 2;
    /* Time-based: 10 * 512 seconds = 5120 seconds */
    tx.inputs[0].sequence = SEQUENCE_LOCKTIME_TYPE_FLAG | 10;

    /* UTXO at time 1000, current time 6200 (5200s elapsed) */
    echo_bool_t satisfied = tx_sequence_satisfied(&tx, 0, 0, 1000, 0, 6200);
    tx_free(&tx);

    if (satisfied) {
        PASS();
    } else {
        FAIL("Should pass - enough time elapsed");
    }
}

static void test_sequence_version_1_ignores(void)
{
    TEST("Version 1 ignores BIP-68");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.version = 1;  /* Version 1 */
    tx.inputs[0].sequence = 1000;  /* Would require 1000 blocks */

    /* Should pass because version 1 ignores BIP-68 */
    echo_bool_t satisfied = tx_sequence_satisfied(&tx, 0, 0, 0, 0, 0);
    tx_free(&tx);

    if (satisfied) {
        PASS();
    } else {
        FAIL("Version 1 should ignore BIP-68");
    }
}

/*
 * ============================================================================
 * FEE COMPUTATION TESTS
 * ============================================================================
 */

static void test_compute_fee_valid(void)
{
    TEST("Compute fee - valid transaction");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.outputs[0].value = 40 * ECHO_SATOSHIS_PER_BTC;

    utxo_info_t utxo = {
        .value = 50 * ECHO_SATOSHIS_PER_BTC,
        .script_pubkey = tx.outputs[0].script_pubkey,
        .script_pubkey_len = tx.outputs[0].script_pubkey_len,
        .height = 100,
        .is_coinbase = ECHO_FALSE
    };

    satoshi_t fee;
    echo_result_t res = tx_compute_fee(&tx, &utxo, 1, &fee);
    tx_free(&tx);

    if (res == ECHO_OK && fee == 10 * ECHO_SATOSHIS_PER_BTC) {
        PASS();
    } else {
        FAIL("Expected 10 BTC fee");
    }
}

static void test_compute_fee_coinbase(void)
{
    TEST("Compute fee - coinbase");
    tx_t tx;
    create_coinbase_tx(&tx);

    satoshi_t fee;
    echo_result_t res = tx_compute_fee(&tx, NULL, 0, &fee);
    tx_free(&tx);

    if (res == ECHO_OK && fee == 0) {
        PASS();
    } else {
        FAIL("Coinbase fee should be 0");
    }
}

/*
 * ============================================================================
 * ERROR STRING TESTS
 * ============================================================================
 */

static void test_error_strings(void)
{
    TEST("Error string coverage");
    int all_valid = 1;

    /* Check a few key error strings */
    if (strcmp(tx_validate_error_string(TX_VALIDATE_OK), "OK") != 0) {
        all_valid = 0;
    }
    if (strcmp(tx_validate_error_string(TX_VALIDATE_ERR_NULL),
               "NULL transaction") != 0) {
        all_valid = 0;
    }
    if (strcmp(tx_validate_error_string(TX_VALIDATE_ERR_DUPLICATE_INPUT),
               "Duplicate input") != 0) {
        all_valid = 0;
    }

    if (all_valid) {
        PASS();
    } else {
        FAIL("Error string mismatch");
    }
}

/*
 * ============================================================================
 * FULL VALIDATION TESTS
 * ============================================================================
 */

static void test_full_validation_insufficient_funds(void)
{
    TEST("Full validation - insufficient funds");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.outputs[0].value = 60 * ECHO_SATOSHIS_PER_BTC;  /* More than input */

    uint8_t script[] = { OP_TRUE };
    utxo_info_t utxo = {
        .value = 50 * ECHO_SATOSHIS_PER_BTC,
        .script_pubkey = script,
        .script_pubkey_len = 1,
        .height = 100,
        .is_coinbase = ECHO_FALSE
    };

    tx_validate_ctx_t ctx = {
        .block_height = 200,
        .block_time = 1600000000,
        .median_time_past = 0,
        .utxos = &utxo,
        .utxo_count = 1,
        .script_flags = 0
    };

    tx_validate_result_t result;
    echo_result_t res = tx_validate(&tx, &ctx, &result);
    tx_free(&tx);

    if (res != ECHO_OK && result.error == TX_VALIDATE_ERR_INSUFFICIENT_FUNDS) {
        PASS();
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Expected INSUFFICIENT_FUNDS, got %s",
                 tx_validate_error_string(result.error));
        FAIL(buf);
    }
}

static void test_full_validation_success(void)
{
    TEST("Full validation - success");
    tx_t tx;
    create_minimal_tx(&tx);
    tx.outputs[0].value = 49 * ECHO_SATOSHIS_PER_BTC;

    uint8_t script[] = { OP_TRUE };
    utxo_info_t utxo = {
        .value = 50 * ECHO_SATOSHIS_PER_BTC,
        .script_pubkey = script,
        .script_pubkey_len = 1,
        .height = 100,
        .is_coinbase = ECHO_FALSE
    };

    tx_validate_ctx_t ctx = {
        .block_height = 200,
        .block_time = 1600000000,
        .median_time_past = 0,
        .utxos = &utxo,
        .utxo_count = 1,
        .script_flags = 0
    };

    tx_validate_result_t result;
    echo_result_t res = tx_validate(&tx, &ctx, &result);
    tx_free(&tx);

    if (res == ECHO_OK) {
        PASS();
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Unexpected error: %s",
                 tx_validate_error_string(result.error));
        FAIL(buf);
    }
}

/*
 * ============================================================================
 * MAIN
 * ============================================================================
 */

int main(void)
{
    printf("Bitcoin Echo — Transaction Validation Tests\n");
    printf("============================================\n\n");

    printf("Syntactic Validation:\n");
    test_validate_null_tx();
    test_validate_empty_inputs();
    test_validate_empty_outputs();
    test_validate_minimal_valid();
    test_validate_coinbase_valid();
    test_validate_coinbase_script_too_short();
    test_validate_coinbase_script_too_long();

    printf("\nDuplicate Input Detection:\n");
    test_validate_duplicate_inputs();
    test_validate_different_inputs();

    printf("\nValue Validation:\n");
    test_validate_negative_output();
    test_validate_output_too_large();
    test_validate_output_at_max();
    test_validate_total_overflow();

    printf("\nLocktime Validation:\n");
    test_locktime_zero_always_valid();
    test_locktime_block_height_not_reached();
    test_locktime_block_height_reached();
    test_locktime_timestamp_not_reached();
    test_locktime_timestamp_reached();
    test_locktime_all_final_sequences();

    printf("\nRelative Locktime (BIP-68):\n");
    test_sequence_disabled();
    test_sequence_blocks_not_reached();
    test_sequence_blocks_reached();
    test_sequence_time_not_reached();
    test_sequence_time_reached();
    test_sequence_version_1_ignores();

    printf("\nFee Computation:\n");
    test_compute_fee_valid();
    test_compute_fee_coinbase();

    printf("\nError Strings:\n");
    test_error_strings();

    printf("\nFull Validation:\n");
    test_full_validation_insufficient_funds();
    test_full_validation_success();

    printf("\n============================================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
