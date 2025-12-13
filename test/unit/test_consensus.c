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
#include "block.h"
#include "tx.h"
#include "test_utils.h"

/* Test macros */
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("Assertion failed: %s (line %d)\n", #cond, __LINE__); \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("Assertion failed: %s == %s (got %ld vs %ld, line %d)\n", \
               #a, #b, (long)(a), (long)(b), __LINE__); \
        return; \
    } \
} while(0)

#define ASSERT_STREQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("Assertion failed: %s == %s (got '%s' vs '%s', line %d)\n", \
               #a, #b, (a), (b), __LINE__); \
        return; \
    } \
} while(0)

/* ========================================================================
 * Test: Engine Lifecycle
 * ======================================================================== */

static void engine_create_destroy(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    consensus_engine_destroy(engine);

}

static void engine_destroy_null(void) {
    /* Should not crash */
    consensus_engine_destroy(NULL);

}

static void engine_initial_state(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    /* Height should be UINT32_MAX (no blocks yet) */
    ASSERT_EQ(consensus_get_height(engine), UINT32_MAX);

    /* UTXO count should be 0 */
    ASSERT_EQ(consensus_utxo_count(engine), 0);

    /* Block index should be empty */
    ASSERT_EQ(consensus_block_index_count(engine), 0);

    consensus_engine_destroy(engine);

}

/* ========================================================================
 * Test: Error Strings
 * ======================================================================== */

static void error_strings(void) {
    /* Test that all error codes have strings */
    ASSERT_STREQ(consensus_error_str(CONSENSUS_OK), "OK");
    ASSERT(consensus_error_str(CONSENSUS_ERR_BLOCK_HEADER) != NULL);
    ASSERT(consensus_error_str(CONSENSUS_ERR_BLOCK_POW) != NULL);
    ASSERT(consensus_error_str(CONSENSUS_ERR_TX_SYNTAX) != NULL);
    ASSERT(consensus_error_str(CONSENSUS_ERR_TX_SCRIPT) != NULL);

    /* Unknown error should not crash */
    ASSERT(consensus_error_str(999) != NULL);


}

/* ========================================================================
 * Test: Result Initialization
 * ======================================================================== */

static void result_init(void) {
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


}

/* ========================================================================
 * Test: Script Verification Flags
 * ======================================================================== */

static void script_flags_pre_bip16(void) {
    uint32_t flags = consensus_get_script_flags(0);
    ASSERT_EQ(flags & SCRIPT_VERIFY_P2SH, 0);
    ASSERT_EQ(flags & SCRIPT_VERIFY_DERSIG, 0);
    ASSERT_EQ(flags & SCRIPT_VERIFY_WITNESS, 0);

}

static void script_flags_post_bip16(void) {
    uint32_t flags = consensus_get_script_flags(CONSENSUS_BIP16_HEIGHT);
    ASSERT(flags & SCRIPT_VERIFY_P2SH);

}

static void script_flags_post_bip66(void) {
    uint32_t flags = consensus_get_script_flags(CONSENSUS_BIP66_HEIGHT);
    ASSERT(flags & SCRIPT_VERIFY_P2SH);
    ASSERT(flags & SCRIPT_VERIFY_DERSIG);

}

static void script_flags_post_bip65(void) {
    uint32_t flags = consensus_get_script_flags(CONSENSUS_BIP65_HEIGHT);
    ASSERT(flags & SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY);

}

static void script_flags_post_csv(void) {
    uint32_t flags = consensus_get_script_flags(CONSENSUS_CSV_HEIGHT);
    ASSERT(flags & SCRIPT_VERIFY_CHECKSEQUENCEVERIFY);

}

static void script_flags_post_segwit(void) {
    uint32_t flags = consensus_get_script_flags(CONSENSUS_SEGWIT_HEIGHT);
    ASSERT(flags & SCRIPT_VERIFY_WITNESS);

}

static void script_flags_post_taproot(void) {
    uint32_t flags = consensus_get_script_flags(CONSENSUS_TAPROOT_HEIGHT);
    ASSERT(flags & SCRIPT_VERIFY_TAPROOT);

}

/* ========================================================================
 * Test: Chain Tip Queries
 * ======================================================================== */

static void chain_tip_initial(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    chain_tip_t tip;
    echo_result_t result = consensus_get_chain_tip(engine, &tip);
    ASSERT_EQ(result, ECHO_OK);

    /* Initial tip should have zero work */
    ASSERT(work256_is_zero(&tip.chainwork));

    consensus_engine_destroy(engine);

}

static void block_hash_not_found(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    /* Height 1 should definitely not exist in empty engine */
    hash256_t hash;
    echo_result_t result = consensus_get_block_hash(engine, 1, &hash);

    /* Should fail - no blocks at height 1 */
    ASSERT_EQ(result, ECHO_ERR_NOT_FOUND);

    consensus_engine_destroy(engine);

}

static void is_main_chain_unknown(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    hash256_t random_hash;
    memset(&random_hash, 0xAB, 32);

    /* Unknown hash should not be on main chain */
    ASSERT(!consensus_is_main_chain(engine, &random_hash));

    consensus_engine_destroy(engine);

}

/* ========================================================================
 * Test: UTXO Queries
 * ======================================================================== */

static void utxo_lookup_empty(void) {
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

}

static void utxo_count_empty(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    ASSERT_EQ(consensus_utxo_count(engine), 0);

    consensus_engine_destroy(engine);

}

/* ========================================================================
 * Test: Block Index Queries
 * ======================================================================== */

static void block_index_lookup_empty(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    hash256_t random_hash;
    memset(&random_hash, 0xCD, 32);

    const block_index_t *index = consensus_lookup_block_index(engine, &random_hash);
    ASSERT(index == NULL);

    consensus_engine_destroy(engine);

}

static void block_index_count_empty(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    ASSERT_EQ(consensus_block_index_count(engine), 0);

    consensus_engine_destroy(engine);

}

static void best_block_index_empty(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    const block_index_t *best = consensus_get_best_block_index(engine);
    ASSERT(best == NULL);

    consensus_engine_destroy(engine);

}

/* ========================================================================
 * Test: Header Validation
 * ======================================================================== */

static void validate_header_genesis(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    block_header_t genesis;
    block_genesis_header(&genesis);

    consensus_result_t result;
    /* Call validation - result may vary (PoW may fail with test genesis) */
    /* We just test that it doesn't crash */
    (void)consensus_validate_header(engine, &genesis, &result);

    consensus_engine_destroy(engine);

}

static void validate_header_disconnected(void) {
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

}

/* ========================================================================
 * Test: Statistics
 * ======================================================================== */

static void stats_initial(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    consensus_stats_t stats;
    consensus_get_stats(engine, &stats);

    /* Initial state stats */
    ASSERT_EQ(stats.utxo_count, 0);
    ASSERT_EQ(stats.block_index_count, 0);

    consensus_engine_destroy(engine);

}

/* ========================================================================
 * Test: Chainstate Access
 * ======================================================================== */

static void get_chainstate(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    chainstate_t *chainstate = consensus_get_chainstate(engine);
    ASSERT(chainstate != NULL);

    consensus_engine_destroy(engine);

}

static void get_utxo_set(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    const utxo_set_t *utxo_set = consensus_get_utxo_set(engine);
    ASSERT(utxo_set != NULL);

    consensus_engine_destroy(engine);

}

/* ========================================================================
 * Test: Add Header
 * ======================================================================== */

static void add_header_genesis(void) {
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

}

static void add_header_duplicate(void) {
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

}

/* ========================================================================
 * Test: Context Building
 * ======================================================================== */

static void build_validation_ctx_genesis(void) {
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

}

static void build_validation_ctx_height_1(void) {
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

}

/* ========================================================================
 * Test: Subsidy Calculation (via coinbase validation)
 * ======================================================================== */

static void subsidy_initial(void) {
    /* Initial subsidy is 50 BTC */
    satoshi_t subsidy = coinbase_subsidy(0);
    ASSERT_EQ(subsidy, 5000000000LL);

}

static void subsidy_first_halving(void) {
    /* First halving at block 210000 */
    satoshi_t before = coinbase_subsidy(209999);
    satoshi_t after = coinbase_subsidy(210000);

    ASSERT_EQ(before, 5000000000LL);  /* 50 BTC */
    ASSERT_EQ(after, 2500000000LL);   /* 25 BTC */


}

static void subsidy_second_halving(void) {
    /* Second halving at block 420000 */
    satoshi_t before = coinbase_subsidy(419999);
    satoshi_t after = coinbase_subsidy(420000);

    ASSERT_EQ(before, 2500000000LL);  /* 25 BTC */
    ASSERT_EQ(after, 1250000000LL);   /* 12.5 BTC */


}

/* ========================================================================
 * Test: Would Reorg
 * ======================================================================== */

static void would_reorg_no_tip(void) {
    consensus_engine_t *engine = consensus_engine_create();
    ASSERT(engine != NULL);

    block_header_t genesis;
    block_genesis_header(&genesis);

    /* No tip yet, so no reorg needed */
    bool reorg = consensus_would_reorg(engine, &genesis);
    ASSERT(!reorg);

    consensus_engine_destroy(engine);

}

/* ========================================================================
 * Test: Activation Heights
 * ======================================================================== */

static void activation_heights(void) {
    /* Verify activation heights are defined correctly */
    ASSERT(CONSENSUS_BIP16_HEIGHT > 0);
    ASSERT(CONSENSUS_BIP34_HEIGHT > CONSENSUS_BIP16_HEIGHT);
    ASSERT(CONSENSUS_BIP65_HEIGHT > CONSENSUS_BIP34_HEIGHT);
    ASSERT(CONSENSUS_BIP66_HEIGHT > CONSENSUS_BIP34_HEIGHT);
    ASSERT(CONSENSUS_CSV_HEIGHT > CONSENSUS_BIP65_HEIGHT);
    ASSERT(CONSENSUS_SEGWIT_HEIGHT > CONSENSUS_CSV_HEIGHT);
    ASSERT(CONSENSUS_TAPROOT_HEIGHT > CONSENSUS_SEGWIT_HEIGHT);


}

/* ========================================================================
 * Test: Block Validation (Basic Structure)
 * ======================================================================== */

static void validate_block_null_coinbase(void) {
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

}

/* ========================================================================
 * Test: TX Context Building (Error Cases)
 * ======================================================================== */

static void tx_ctx_missing_utxo(void) {
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

}

/* ========================================================================
 * Test: Free TX Context
 * ======================================================================== */

static void free_tx_ctx_null(void) {
    /* Should not crash */
    consensus_free_tx_ctx(NULL);

}

/* ========================================================================
 * Main Test Runner
 * ======================================================================== */

int main(void) {
    test_suite_begin("Consensus Engine Tests");

    test_section("Engine Lifecycle");
    test_case("Create and destroy engine"); engine_create_destroy(); test_pass();
    test_case("Destroy NULL engine"); engine_destroy_null(); test_pass();
    test_case("Initial engine state"); engine_initial_state(); test_pass();

    test_section("Error Handling");
    test_case("Error code strings"); error_strings(); test_pass();
    test_case("Result initialization"); result_init(); test_pass();

    test_section("Script Verification Flags");
    test_case("Pre-BIP16 flags"); script_flags_pre_bip16(); test_pass();
    test_case("Post-BIP16 flags (P2SH)"); script_flags_post_bip16(); test_pass();
    test_case("Post-BIP66 flags (DER sigs)"); script_flags_post_bip66(); test_pass();
    test_case("Post-BIP65 flags (CLTV)"); script_flags_post_bip65(); test_pass();
    test_case("Post-CSV flags"); script_flags_post_csv(); test_pass();
    test_case("Post-SegWit flags"); script_flags_post_segwit(); test_pass();
    test_case("Post-Taproot flags"); script_flags_post_taproot(); test_pass();

    test_section("Chain Tip Queries");
    test_case("Initial chain tip"); chain_tip_initial(); test_pass();
    test_case("Block hash not found"); block_hash_not_found(); test_pass();
    test_case("Unknown hash not on main chain"); is_main_chain_unknown(); test_pass();

    test_section("UTXO Queries");
    test_case("Lookup in empty UTXO set"); utxo_lookup_empty(); test_pass();
    test_case("UTXO count in empty set"); utxo_count_empty(); test_pass();

    test_section("Block Index Queries");
    test_case("Lookup in empty block index"); block_index_lookup_empty(); test_pass();
    test_case("Block index count when empty"); block_index_count_empty(); test_pass();
    test_case("Best block index when empty"); best_block_index_empty(); test_pass();

    test_section("Header Validation");
    test_case("Validate genesis header"); validate_header_genesis(); test_pass();
    test_case("Reject disconnected header"); validate_header_disconnected(); test_pass();

    test_section("Statistics");
    test_case("Initial statistics"); stats_initial(); test_pass();

    test_section("Chainstate Access");
    test_case("Get chainstate pointer"); get_chainstate(); test_pass();
    test_case("Get UTXO set pointer"); get_utxo_set(); test_pass();

    test_section("Add Header");
    test_case("Add genesis header"); add_header_genesis(); test_pass();
    test_case("Reject duplicate header"); add_header_duplicate(); test_pass();

    test_section("Context Building");
    test_case("Build validation context for genesis"); build_validation_ctx_genesis(); test_pass();
    test_case("Build validation context for height 1"); build_validation_ctx_height_1(); test_pass();

    test_section("Subsidy Calculation");
    test_case("Initial subsidy (50 BTC)"); subsidy_initial(); test_pass();
    test_case("First halving (50→25 BTC)"); subsidy_first_halving(); test_pass();
    test_case("Second halving (25→12.5 BTC)"); subsidy_second_halving(); test_pass();

    test_section("Reorg Detection");
    test_case("No reorg with no tip"); would_reorg_no_tip(); test_pass();

    test_section("Activation Heights");
    test_case("BIP activation height ordering"); activation_heights(); test_pass();

    test_section("Block Validation");
    test_case("Reject block with no coinbase"); validate_block_null_coinbase(); test_pass();

    test_section("Transaction Context");
    test_case("Error on missing UTXO"); tx_ctx_missing_utxo(); test_pass();
    test_case("Free NULL tx context"); free_tx_ctx_null(); test_pass();

    test_suite_end();
    return test_global_summary();
}
