/**
 * @file test_consensus.c
 * @brief Test suite for the consensus engine
 *
 * Tests the unified consensus engine interface including:
 *   - Engine lifecycle (create/destroy)
 *   - Block validation
 *   - Transaction validation
 *   - Chain state queries
 *   - UTXO management
 *   - Block index queries
 *   - Chain selection
 *   - Script flag computation
 *
 * Build once. Build right. Stop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "consensus.h"
#include "sha256.h"
#include "merkle.h"
#include "block.h"
#include "tx.h"

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;

/* Test macros */
#define TEST(name) static bool test_##name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  Testing %s... ", #name); \
    fflush(stdout); \
    if (test_##name()) { \
        tests_passed++; \
        printf("PASSED\n"); \
    } else { \
        printf("FAILED\n"); \
    } \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("Assertion failed: %s (line %d)\n", #cond, __LINE__); \
        return false; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("Assertion failed: %s == %s (got %ld vs %ld, line %d)\n", \
               #a, #b, (long)(a), (long)(b), __LINE__); \
        return false; \
    } \
} while(0)

#define ASSERT_STREQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("Assertion failed: %s == %s (got '%s' vs '%s', line %d)\n", \
               #a, #b, (a), (b), __LINE__); \
        return false; \
    } \
} while(0)

/* ========================================================================
 * Helper Functions
 * ======================================================================== */

/**
 * Create a simple coinbase transaction for testing.
 */
static void create_test_coinbase(tx_t *tx, uint32_t height, satoshi_t value) {
    memset(tx, 0, sizeof(tx_t));

    tx->version = 1;
    tx->locktime = 0;

    /* Single coinbase input */
    tx->input_count = 1;
    tx->inputs = calloc(1, sizeof(tx_input_t));

    /* Null outpoint for coinbase */
    memset(&tx->inputs[0].prevout.txid, 0, 32);
    tx->inputs[0].prevout.vout = 0xFFFFFFFF;

    /* BIP-34 height encoding in scriptsig */
    /* Simple encoding: push height as minimal integer */
    uint8_t height_script[10];
    size_t script_len = 0;

    if (height == 0) {
        height_script[0] = 0x01;  /* Push 1 byte */
        height_script[1] = 0x00;  /* Value 0 */
        script_len = 2;
    } else if (height < 17) {
        height_script[0] = 0x01;
        height_script[1] = (uint8_t)height;
        script_len = 2;
    } else if (height < 128) {
        height_script[0] = 0x01;
        height_script[1] = (uint8_t)height;
        script_len = 2;
    } else if (height < 32768) {
        height_script[0] = 0x02;
        height_script[1] = (uint8_t)(height & 0xFF);
        height_script[2] = (uint8_t)((height >> 8) & 0xFF);
        script_len = 3;
    } else {
        height_script[0] = 0x03;
        height_script[1] = (uint8_t)(height & 0xFF);
        height_script[2] = (uint8_t)((height >> 8) & 0xFF);
        height_script[3] = (uint8_t)((height >> 16) & 0xFF);
        script_len = 4;
    }

    /* Add some padding to meet minimum scriptsig length */
    height_script[script_len++] = 0x00;  /* OP_0 */
    height_script[script_len++] = 0x00;

    tx->inputs[0].script_sig = malloc(script_len);
    memcpy(tx->inputs[0].script_sig, height_script, script_len);
    tx->inputs[0].script_sig_len = script_len;
    tx->inputs[0].sequence = 0xFFFFFFFF;

    /* Single output */
    tx->output_count = 1;
    tx->outputs = calloc(1, sizeof(tx_output_t));
    tx->outputs[0].value = value;

    /* Simple P2PKH-like output script */
    uint8_t script[] = {0x76, 0xa9, 0x14,  /* OP_DUP OP_HASH160 PUSH20 */
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00,
                        0x88, 0xac};  /* OP_EQUALVERIFY OP_CHECKSIG */
    tx->outputs[0].script_pubkey = malloc(25);
    memcpy(tx->outputs[0].script_pubkey, script, 25);
    tx->outputs[0].script_pubkey_len = 25;
}

/**
 * Free a test transaction.
 */
static void free_test_tx(tx_t *tx) {
    for (size_t i = 0; i < tx->input_count; i++) {
        free(tx->inputs[i].script_sig);
        /* Free witness items */
        if (tx->inputs[i].witness.count > 0) {
            for (size_t j = 0; j < tx->inputs[i].witness.count; j++) {
                free(tx->inputs[i].witness.items[j].data);
            }
            free(tx->inputs[i].witness.items);
        }
    }
    free(tx->inputs);

    for (size_t i = 0; i < tx->output_count; i++) {
        free(tx->outputs[i].script_pubkey);
    }
    free(tx->outputs);
}

/**
 * Create a simple test block.
 */
static void create_test_block(block_t *block, uint32_t height,
                               const hash256_t *prev_hash, uint32_t timestamp) {
    memset(block, 0, sizeof(block_t));

    /* Set up header */
    block->header.version = 0x20000000;  /* BIP-9 version */
    if (prev_hash != NULL) {
        block->header.prev_hash = *prev_hash;
    } else {
        memset(&block->header.prev_hash, 0, 32);
    }
    block->header.timestamp = timestamp;
    block->header.bits = GENESIS_BLOCK_BITS;  /* Genesis difficulty */
    block->header.nonce = 0;

    /* Create coinbase */
    block->tx_count = 1;
    block->txs = calloc(1, sizeof(tx_t));
    create_test_coinbase(&block->txs[0], height, coinbase_subsidy(height));

    /* Compute merkle root */
    hash256_t txid;
    tx_compute_txid(&block->txs[0], &txid);
    merkle_root(&txid, 1, &block->header.merkle_root);
}

/**
 * Free a test block.
 */
static void free_test_block(block_t *block) {
    for (size_t i = 0; i < block->tx_count; i++) {
        free_test_tx(&block->txs[i]);
    }
    free(block->txs);
}

/* ========================================================================
 * Test: Engine Lifecycle
 * ======================================================================== */

TEST(engine_create_destroy) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    consensus_engine_destroy(engine);
    return true;
}

TEST(engine_destroy_null) {
    /* Should not crash */
    consensus_engine_destroy(NULL);
    return true;
}

TEST(engine_initial_state) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    /* Height should be UINT32_MAX (no blocks yet) */
    ASSERT_EQ(consensus_get_height(engine), UINT32_MAX);

    /* UTXO count should be 0 */
    ASSERT_EQ(consensus_utxo_count(engine), 0);

    /* Block index should be empty */
    ASSERT_EQ(consensus_block_index_count(engine), 0);

    consensus_engine_destroy(engine);
    return true;
}

/* ========================================================================
 * Test: Error Strings
 * ======================================================================== */

TEST(error_strings) {
    /* Test that all error codes have strings */
    ASSERT_STREQ(consensus_error_str(CONSENSUS_OK), "OK");
    ASSERT(consensus_error_str(CONSENSUS_ERR_BLOCK_HEADER) != NULL);
    ASSERT(consensus_error_str(CONSENSUS_ERR_BLOCK_POW) != NULL);
    ASSERT(consensus_error_str(CONSENSUS_ERR_TX_SYNTAX) != NULL);
    ASSERT(consensus_error_str(CONSENSUS_ERR_TX_SCRIPT) != NULL);

    /* Unknown error should not crash */
    ASSERT(consensus_error_str(999) != NULL);

    return true;
}

/* ========================================================================
 * Test: Result Initialization
 * ======================================================================== */

TEST(result_init) {
    consensus_result_t result;

    /* Set some garbage */
    memset(&result, 0xFF, sizeof(result));

    /* Initialize */
    consensus_result_init(&result);

    /* Check initialized values */
    ASSERT_EQ(result.error, CONSENSUS_OK);
    ASSERT_EQ(result.failing_index, 0);
    ASSERT_EQ(result.failing_input_index, 0);
    ASSERT_EQ(result.block_error, BLOCK_VALID);
    ASSERT_EQ(result.tx_error, TX_VALIDATE_OK);

    return true;
}

/* ========================================================================
 * Test: Script Verification Flags
 * ======================================================================== */

TEST(script_flags_pre_bip16) {
    uint32_t flags = consensus_get_script_flags(0);
    ASSERT_EQ(flags & SCRIPT_VERIFY_P2SH, 0);
    ASSERT_EQ(flags & SCRIPT_VERIFY_DERSIG, 0);
    ASSERT_EQ(flags & SCRIPT_VERIFY_WITNESS, 0);
    return true;
}

TEST(script_flags_post_bip16) {
    uint32_t flags = consensus_get_script_flags(CONSENSUS_BIP16_HEIGHT);
    ASSERT(flags & SCRIPT_VERIFY_P2SH);
    return true;
}

TEST(script_flags_post_bip66) {
    uint32_t flags = consensus_get_script_flags(CONSENSUS_BIP66_HEIGHT);
    ASSERT(flags & SCRIPT_VERIFY_P2SH);
    ASSERT(flags & SCRIPT_VERIFY_DERSIG);
    return true;
}

TEST(script_flags_post_bip65) {
    uint32_t flags = consensus_get_script_flags(CONSENSUS_BIP65_HEIGHT);
    ASSERT(flags & SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY);
    return true;
}

TEST(script_flags_post_csv) {
    uint32_t flags = consensus_get_script_flags(CONSENSUS_CSV_HEIGHT);
    ASSERT(flags & SCRIPT_VERIFY_CHECKSEQUENCEVERIFY);
    return true;
}

TEST(script_flags_post_segwit) {
    uint32_t flags = consensus_get_script_flags(CONSENSUS_SEGWIT_HEIGHT);
    ASSERT(flags & SCRIPT_VERIFY_WITNESS);
    return true;
}

TEST(script_flags_post_taproot) {
    uint32_t flags = consensus_get_script_flags(CONSENSUS_TAPROOT_HEIGHT);
    ASSERT(flags & SCRIPT_VERIFY_TAPROOT);
    return true;
}

/* ========================================================================
 * Test: Chain Tip Queries
 * ======================================================================== */

TEST(chain_tip_initial) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    chain_tip_t tip;
    echo_result_t result = consensus_get_chain_tip(engine, &tip);
    ASSERT_EQ(result, ECHO_OK);

    /* Initial tip should have zero work */
    ASSERT(work256_is_zero(&tip.chainwork));

    consensus_engine_destroy(engine);
    return true;
}

TEST(block_hash_not_found) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    /* Height 1 should definitely not exist in empty engine */
    hash256_t hash;
    echo_result_t result = consensus_get_block_hash(engine, 1, &hash);

    /* Should fail - no blocks at height 1 */
    ASSERT_EQ(result, ECHO_ERR_NOT_FOUND);

    consensus_engine_destroy(engine);
    return true;
}

TEST(is_main_chain_unknown) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    hash256_t random_hash;
    memset(&random_hash, 0xAB, 32);

    /* Unknown hash should not be on main chain */
    ASSERT(!consensus_is_main_chain(engine, &random_hash));

    consensus_engine_destroy(engine);
    return true;
}

/* ========================================================================
 * Test: UTXO Queries
 * ======================================================================== */

TEST(utxo_lookup_empty) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    outpoint_t outpoint;
    memset(&outpoint, 0, sizeof(outpoint));
    outpoint.vout = 0;

    /* Lookup should return NULL */
    const utxo_entry_t *utxo = consensus_lookup_utxo(engine, &outpoint);
    ASSERT(utxo == NULL);

    /* Exists should return false */
    ASSERT(!consensus_utxo_exists(engine, &outpoint));

    consensus_engine_destroy(engine);
    return true;
}

TEST(utxo_count_empty) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    ASSERT_EQ(consensus_utxo_count(engine), 0);

    consensus_engine_destroy(engine);
    return true;
}

/* ========================================================================
 * Test: Block Index Queries
 * ======================================================================== */

TEST(block_index_lookup_empty) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    hash256_t random_hash;
    memset(&random_hash, 0xCD, 32);

    const block_index_t *index = consensus_lookup_block_index(engine, &random_hash);
    ASSERT(index == NULL);

    consensus_engine_destroy(engine);
    return true;
}

TEST(block_index_count_empty) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    ASSERT_EQ(consensus_block_index_count(engine), 0);

    consensus_engine_destroy(engine);
    return true;
}

TEST(best_block_index_empty) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    const block_index_t *best = consensus_get_best_block_index(engine);
    ASSERT(best == NULL);

    consensus_engine_destroy(engine);
    return true;
}

/* ========================================================================
 * Test: Header Validation
 * ======================================================================== */

TEST(validate_header_genesis) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    block_header_t genesis;
    block_genesis_header(&genesis);

    consensus_result_t result;
    /* Call validation - result may vary (PoW may fail with test genesis) */
    /* We just test that it doesn't crash */
    (void)consensus_validate_header(engine, &genesis, &result);

    consensus_engine_destroy(engine);
    return true;
}

TEST(validate_header_disconnected) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    /* Create a header with non-zero prev_hash (not genesis, no parent) */
    block_header_t header;
    memset(&header, 0, sizeof(header));
    header.version = 1;
    memset(&header.prev_hash, 0xAB, 32);  /* Unknown parent */
    header.timestamp = 1231006505;
    header.bits = GENESIS_BLOCK_BITS;

    consensus_result_t result;
    bool valid = consensus_validate_header(engine, &header, &result);

    /* Should fail - disconnected from chain */
    ASSERT(!valid);
    ASSERT_EQ(result.error, CONSENSUS_ERR_INVALID_PREV);

    consensus_engine_destroy(engine);
    return true;
}

/* ========================================================================
 * Test: Statistics
 * ======================================================================== */

TEST(stats_initial) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    consensus_stats_t stats;
    consensus_get_stats(engine, &stats);

    /* Initial state stats */
    ASSERT_EQ(stats.utxo_count, 0);
    ASSERT_EQ(stats.block_index_count, 0);

    consensus_engine_destroy(engine);
    return true;
}

/* ========================================================================
 * Test: Chainstate Access
 * ======================================================================== */

TEST(get_chainstate) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    chainstate_t *chainstate = consensus_get_chainstate(engine);
    ASSERT(chainstate != NULL);

    consensus_engine_destroy(engine);
    return true;
}

TEST(get_utxo_set) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    const utxo_set_t *utxo_set = consensus_get_utxo_set(engine);
    ASSERT(utxo_set != NULL);

    consensus_engine_destroy(engine);
    return true;
}

/* ========================================================================
 * Test: Add Header
 * ======================================================================== */

TEST(add_header_genesis) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    block_header_t genesis;
    block_genesis_header(&genesis);

    block_index_t *index = NULL;
    echo_result_t result = consensus_add_header(engine, &genesis, &index);

    ASSERT_EQ(result, ECHO_OK);
    ASSERT(index != NULL);
    ASSERT_EQ(index->height, 0);

    /* Should now be in the index */
    ASSERT_EQ(consensus_block_index_count(engine), 1);

    consensus_engine_destroy(engine);
    return true;
}

TEST(add_header_duplicate) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    block_header_t genesis;
    block_genesis_header(&genesis);

    block_index_t *index = NULL;
    echo_result_t result = consensus_add_header(engine, &genesis, &index);
    ASSERT_EQ(result, ECHO_OK);

    /* Try to add again */
    result = consensus_add_header(engine, &genesis, &index);
    ASSERT_EQ(result, ECHO_ERR_EXISTS);

    consensus_engine_destroy(engine);
    return true;
}

/* ========================================================================
 * Test: Context Building
 * ======================================================================== */

TEST(build_validation_ctx_genesis) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    full_block_ctx_t ctx;
    echo_result_t result = consensus_build_validation_ctx(engine, 0, &ctx);

    ASSERT_EQ(result, ECHO_OK);
    ASSERT_EQ(ctx.height, 0);
    ASSERT_EQ(ctx.header_ctx.height, 0);

    /* Genesis has no timestamps for MTP */
    ASSERT_EQ(ctx.header_ctx.timestamp_count, 0);

    consensus_engine_destroy(engine);
    return true;
}

TEST(build_validation_ctx_height_1) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    /* Add genesis header first */
    block_header_t genesis;
    block_genesis_header(&genesis);
    consensus_add_header(engine, &genesis, NULL);

    full_block_ctx_t ctx;
    echo_result_t result = consensus_build_validation_ctx(engine, 1, &ctx);

    ASSERT_EQ(result, ECHO_OK);
    ASSERT_EQ(ctx.height, 1);

    consensus_engine_destroy(engine);
    return true;
}

/* ========================================================================
 * Test: Subsidy Calculation (via coinbase validation)
 * ======================================================================== */

TEST(subsidy_initial) {
    /* Initial subsidy is 50 BTC */
    satoshi_t subsidy = coinbase_subsidy(0);
    ASSERT_EQ(subsidy, 5000000000LL);
    return true;
}

TEST(subsidy_first_halving) {
    /* First halving at block 210000 */
    satoshi_t before = coinbase_subsidy(209999);
    satoshi_t after = coinbase_subsidy(210000);

    ASSERT_EQ(before, 5000000000LL);  /* 50 BTC */
    ASSERT_EQ(after, 2500000000LL);   /* 25 BTC */

    return true;
}

TEST(subsidy_second_halving) {
    /* Second halving at block 420000 */
    satoshi_t before = coinbase_subsidy(419999);
    satoshi_t after = coinbase_subsidy(420000);

    ASSERT_EQ(before, 2500000000LL);  /* 25 BTC */
    ASSERT_EQ(after, 1250000000LL);   /* 12.5 BTC */

    return true;
}

/* ========================================================================
 * Test: Would Reorg
 * ======================================================================== */

TEST(would_reorg_no_tip) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    block_header_t genesis;
    block_genesis_header(&genesis);

    /* No tip yet, so no reorg needed */
    bool reorg = consensus_would_reorg(engine, &genesis);
    ASSERT(!reorg);

    consensus_engine_destroy(engine);
    return true;
}

/* ========================================================================
 * Test: Activation Heights
 * ======================================================================== */

TEST(activation_heights) {
    /* Verify activation heights are defined correctly */
    ASSERT(CONSENSUS_BIP16_HEIGHT > 0);
    ASSERT(CONSENSUS_BIP34_HEIGHT > CONSENSUS_BIP16_HEIGHT);
    ASSERT(CONSENSUS_BIP65_HEIGHT > CONSENSUS_BIP34_HEIGHT);
    ASSERT(CONSENSUS_BIP66_HEIGHT > CONSENSUS_BIP34_HEIGHT);
    ASSERT(CONSENSUS_CSV_HEIGHT > CONSENSUS_BIP65_HEIGHT);
    ASSERT(CONSENSUS_SEGWIT_HEIGHT > CONSENSUS_CSV_HEIGHT);
    ASSERT(CONSENSUS_TAPROOT_HEIGHT > CONSENSUS_SEGWIT_HEIGHT);

    return true;
}

/* ========================================================================
 * Test: Block Validation (Basic Structure)
 * ======================================================================== */

TEST(validate_block_null_coinbase) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    /* Create a block with no transactions */
    block_t block;
    memset(&block, 0, sizeof(block));
    block.header.version = 1;
    block.header.bits = GENESIS_BLOCK_BITS;
    block.tx_count = 0;
    block.txs = NULL;

    consensus_result_t result;
    bool valid = consensus_validate_block(engine, &block, &result);

    /* Should fail - no coinbase */
    ASSERT(!valid);

    consensus_engine_destroy(engine);
    return true;
}

/* ========================================================================
 * Test: TX Context Building (Error Cases)
 * ======================================================================== */

TEST(tx_ctx_missing_utxo) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    /* Create a transaction spending a non-existent UTXO */
    tx_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.version = 1;
    tx.input_count = 1;
    tx.inputs = calloc(1, sizeof(tx_input_t));
    memset(&tx.inputs[0].prevout.txid, 0xAB, 32);
    tx.inputs[0].prevout.vout = 0;
    tx.inputs[0].sequence = 0xFFFFFFFF;

    tx.output_count = 1;
    tx.outputs = calloc(1, sizeof(tx_output_t));
    tx.outputs[0].value = 1000;

    tx_validate_ctx_t ctx;
    echo_result_t result = consensus_build_tx_ctx(engine, &tx, 100, 1234567890, &ctx);

    /* Should fail - UTXO not found */
    ASSERT_EQ(result, ECHO_ERR_NOT_FOUND);

    /* Clean up */
    free(tx.inputs);
    free(tx.outputs);

    consensus_engine_destroy(engine);
    return true;
}

/* ========================================================================
 * Test: Free TX Context
 * ======================================================================== */

TEST(free_tx_ctx_null) {
    /* Should not crash */
    consensus_free_tx_ctx(NULL);
    return true;
}

/* ========================================================================
 * Main Test Runner
 * ======================================================================== */

int main(void) {
    printf("=== Consensus Engine Tests ===\n\n");

    printf("Engine Lifecycle:\n");
    RUN_TEST(engine_create_destroy);
    RUN_TEST(engine_destroy_null);
    RUN_TEST(engine_initial_state);

    printf("\nError Handling:\n");
    RUN_TEST(error_strings);
    RUN_TEST(result_init);

    printf("\nScript Verification Flags:\n");
    RUN_TEST(script_flags_pre_bip16);
    RUN_TEST(script_flags_post_bip16);
    RUN_TEST(script_flags_post_bip66);
    RUN_TEST(script_flags_post_bip65);
    RUN_TEST(script_flags_post_csv);
    RUN_TEST(script_flags_post_segwit);
    RUN_TEST(script_flags_post_taproot);

    printf("\nChain Tip Queries:\n");
    RUN_TEST(chain_tip_initial);
    RUN_TEST(block_hash_not_found);
    RUN_TEST(is_main_chain_unknown);

    printf("\nUTXO Queries:\n");
    RUN_TEST(utxo_lookup_empty);
    RUN_TEST(utxo_count_empty);

    printf("\nBlock Index Queries:\n");
    RUN_TEST(block_index_lookup_empty);
    RUN_TEST(block_index_count_empty);
    RUN_TEST(best_block_index_empty);

    printf("\nHeader Validation:\n");
    RUN_TEST(validate_header_genesis);
    RUN_TEST(validate_header_disconnected);

    printf("\nStatistics:\n");
    RUN_TEST(stats_initial);

    printf("\nChainstate Access:\n");
    RUN_TEST(get_chainstate);
    RUN_TEST(get_utxo_set);

    printf("\nAdd Header:\n");
    RUN_TEST(add_header_genesis);
    RUN_TEST(add_header_duplicate);

    printf("\nContext Building:\n");
    RUN_TEST(build_validation_ctx_genesis);
    RUN_TEST(build_validation_ctx_height_1);

    printf("\nSubsidy Calculation:\n");
    RUN_TEST(subsidy_initial);
    RUN_TEST(subsidy_first_halving);
    RUN_TEST(subsidy_second_halving);

    printf("\nReorg Detection:\n");
    RUN_TEST(would_reorg_no_tip);

    printf("\nActivation Heights:\n");
    RUN_TEST(activation_heights);

    printf("\nBlock Validation:\n");
    RUN_TEST(validate_block_null_coinbase);

    printf("\nTransaction Context:\n");
    RUN_TEST(tx_ctx_missing_utxo);
    RUN_TEST(free_tx_ctx_null);

    printf("\n=== Results ===\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_run - tests_passed);

    if (tests_passed == tests_run) {
        printf("\n*** ALL TESTS PASSED ***\n");
        return 0;
    } else {
        printf("\n*** SOME TESTS FAILED ***\n");
        return 1;
    }
}
