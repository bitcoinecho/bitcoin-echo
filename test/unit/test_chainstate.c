/**
 * @file test_chainstate.c
 * @brief Unit tests for chain state implementation
 */

#include "chainstate.h"
#include "block.h"
#include "tx.h"
#include "sha256.h"
#include "echo_types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    static void name(void); \
    static void run_##name(void) { \
        tests_run++; \
        printf("Running %s...", #name); \
        name(); \
        tests_passed++; \
        printf(" PASS\n"); \
    } \
    static void name(void)

#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            printf("\n  Assertion failed: %s\n  at %s:%d\n", \
                   #cond, __FILE__, __LINE__); \
            exit(1); \
        } \
    } while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_TRUE(x) ASSERT(x)
#define ASSERT_FALSE(x) ASSERT(!(x))
#define ASSERT_NULL(x) ASSERT((x) == NULL)
#define ASSERT_NOT_NULL(x) ASSERT((x) != NULL)

/* ========================================================================
 * Helper Functions
 * ======================================================================== */

/**
 * Create a simple test transaction with one output
 */
static void make_simple_tx(tx_t *tx, uint8_t seed, int64_t value, bool is_coinbase) {
    tx_init(tx);
    tx->version = 1;
    tx->locktime = 0;

    /* One input */
    tx->input_count = 1;
    tx->inputs = malloc(sizeof(tx_input_t));
    ASSERT_NOT_NULL(tx->inputs);

    if (is_coinbase) {
        /* Coinbase: null outpoint */
        memset(tx->inputs[0].prevout.txid.bytes, 0, 32);
        tx->inputs[0].prevout.vout = 0xFFFFFFFF;
    } else {
        /* Regular: reference some previous tx */
        memset(tx->inputs[0].prevout.txid.bytes, seed, 32);
        tx->inputs[0].prevout.vout = 0;
    }

    tx->inputs[0].sequence = 0xFFFFFFFF;
    tx->inputs[0].script_sig_len = 4;
    tx->inputs[0].script_sig = malloc(4);
    ASSERT_NOT_NULL(tx->inputs[0].script_sig);
    memset(tx->inputs[0].script_sig, seed, 4);
    tx->inputs[0].witness.count = 0;
    tx->inputs[0].witness.items = NULL;

    /* One output */
    tx->output_count = 1;
    tx->outputs = malloc(sizeof(tx_output_t));
    ASSERT_NOT_NULL(tx->outputs);

    tx->outputs[0].value = value;
    tx->outputs[0].script_pubkey_len = 25;  /* P2PKH */
    tx->outputs[0].script_pubkey = malloc(25);
    ASSERT_NOT_NULL(tx->outputs[0].script_pubkey);

    /* P2PKH script */
    tx->outputs[0].script_pubkey[0] = 0x76;  /* OP_DUP */
    tx->outputs[0].script_pubkey[1] = 0xa9;  /* OP_HASH160 */
    tx->outputs[0].script_pubkey[2] = 0x14;  /* Push 20 bytes */
    memset(tx->outputs[0].script_pubkey + 3, seed, 20);
    tx->outputs[0].script_pubkey[23] = 0x88;  /* OP_EQUALVERIFY */
    tx->outputs[0].script_pubkey[24] = 0xac;  /* OP_CHECKSIG */
}

/**
 * Create a test block header
 */
static void make_block_header(block_header_t *header, const hash256_t *prev_hash,
                               uint32_t timestamp, uint32_t bits, uint32_t nonce) {
    header->version = 1;
    if (prev_hash != NULL) {
        header->prev_hash = *prev_hash;
    } else {
        memset(&header->prev_hash, 0, 32);
    }
    memset(&header->merkle_root, 0, 32);  /* Simplified */
    header->timestamp = timestamp;
    header->bits = bits;
    header->nonce = nonce;
}

/* ========================================================================
 * Work256 Tests
 * ======================================================================== */

TEST(test_work256_zero) {
    work256_t work;
    work256_zero(&work);

    ASSERT_TRUE(work256_is_zero(&work));

    for (int i = 0; i < 32; i++) {
        ASSERT_EQ(work.bytes[i], 0);
    }
}

TEST(test_work256_is_zero_nonzero) {
    work256_t work;
    work256_zero(&work);
    ASSERT_TRUE(work256_is_zero(&work));

    work.bytes[0] = 1;
    ASSERT_FALSE(work256_is_zero(&work));

    work256_zero(&work);
    work.bytes[31] = 1;
    ASSERT_FALSE(work256_is_zero(&work));
}

TEST(test_work256_compare_equal) {
    work256_t a, b;
    work256_zero(&a);
    work256_zero(&b);

    ASSERT_EQ(work256_compare(&a, &b), 0);

    a.bytes[0] = 0x42;
    b.bytes[0] = 0x42;
    ASSERT_EQ(work256_compare(&a, &b), 0);
}

TEST(test_work256_compare_less) {
    work256_t a, b;
    work256_zero(&a);
    work256_zero(&b);

    a.bytes[0] = 1;
    b.bytes[0] = 2;
    ASSERT_EQ(work256_compare(&a, &b), -1);

    /* Compare at higher byte positions */
    work256_zero(&a);
    work256_zero(&b);
    b.bytes[31] = 1;  /* b is much larger */
    ASSERT_EQ(work256_compare(&a, &b), -1);
}

TEST(test_work256_compare_greater) {
    work256_t a, b;
    work256_zero(&a);
    work256_zero(&b);

    a.bytes[0] = 2;
    b.bytes[0] = 1;
    ASSERT_EQ(work256_compare(&a, &b), 1);

    /* Compare at higher byte positions */
    work256_zero(&a);
    work256_zero(&b);
    a.bytes[16] = 1;  /* a is larger */
    ASSERT_EQ(work256_compare(&a, &b), 1);
}

TEST(test_work256_add_simple) {
    work256_t a, b, result;
    work256_zero(&a);
    work256_zero(&b);

    a.bytes[0] = 1;
    b.bytes[0] = 2;

    work256_add(&a, &b, &result);
    ASSERT_EQ(result.bytes[0], 3);

    for (int i = 1; i < 32; i++) {
        ASSERT_EQ(result.bytes[i], 0);
    }
}

TEST(test_work256_add_carry) {
    work256_t a, b, result;
    work256_zero(&a);
    work256_zero(&b);

    a.bytes[0] = 0xFF;
    b.bytes[0] = 0x02;

    work256_add(&a, &b, &result);
    ASSERT_EQ(result.bytes[0], 0x01);
    ASSERT_EQ(result.bytes[1], 0x01);

    for (int i = 2; i < 32; i++) {
        ASSERT_EQ(result.bytes[i], 0);
    }
}

TEST(test_work256_add_large) {
    work256_t a, b, result;

    /* Set a = 0x00...00FF (at byte 15) */
    work256_zero(&a);
    a.bytes[15] = 0xFF;

    /* Set b = 0x00...0001 (at byte 15) */
    work256_zero(&b);
    b.bytes[15] = 0x01;

    work256_add(&a, &b, &result);

    ASSERT_EQ(result.bytes[15], 0x00);
    ASSERT_EQ(result.bytes[16], 0x01);
}

TEST(test_work256_sub_simple) {
    work256_t a, b, result;
    work256_zero(&a);
    work256_zero(&b);

    a.bytes[0] = 5;
    b.bytes[0] = 3;

    echo_result_t r = work256_sub(&a, &b, &result);
    ASSERT_EQ(r, ECHO_OK);
    ASSERT_EQ(result.bytes[0], 2);
}

TEST(test_work256_sub_borrow) {
    work256_t a, b, result;
    work256_zero(&a);
    work256_zero(&b);

    a.bytes[0] = 0x00;
    a.bytes[1] = 0x01;  /* a = 256 */
    b.bytes[0] = 0x01;  /* b = 1 */

    echo_result_t r = work256_sub(&a, &b, &result);
    ASSERT_EQ(r, ECHO_OK);
    ASSERT_EQ(result.bytes[0], 0xFF);
    ASSERT_EQ(result.bytes[1], 0x00);
}

TEST(test_work256_sub_underflow) {
    work256_t a, b, result;
    work256_zero(&a);
    work256_zero(&b);

    a.bytes[0] = 1;
    b.bytes[0] = 2;

    echo_result_t r = work256_sub(&a, &b, &result);
    ASSERT_EQ(r, ECHO_ERR_UNDERFLOW);
}

TEST(test_work256_from_bits_mainnet) {
    /* Test with mainnet genesis difficulty */
    work256_t work;
    uint32_t bits = 0x1d00ffff;  /* Genesis block difficulty */

    echo_result_t result = work256_from_bits(bits, &work);
    ASSERT_EQ(result, ECHO_OK);

    /* Work should be non-zero */
    ASSERT_FALSE(work256_is_zero(&work));
}

TEST(test_work256_from_bits_higher_difficulty) {
    work256_t work_easy, work_hard;

    /* Easy target (higher target value = less work) */
    work256_from_bits(0x1d00ffff, &work_easy);

    /* Harder target (lower target value = more work) */
    work256_from_bits(0x1c00ffff, &work_hard);

    /* Harder difficulty should mean more work */
    ASSERT_EQ(work256_compare(&work_hard, &work_easy), 1);
}

/* ========================================================================
 * Block Index Tests
 * ======================================================================== */

TEST(test_block_index_create_genesis) {
    block_header_t header;
    make_block_header(&header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *index = block_index_create(&header, NULL);
    ASSERT_NOT_NULL(index);

    ASSERT_EQ(index->height, 0);
    ASSERT_EQ(index->timestamp, 1231006505);
    ASSERT_EQ(index->bits, 0x1d00ffff);
    ASSERT_NULL(index->prev);
    ASSERT_FALSE(work256_is_zero(&index->chainwork));

    block_index_destroy(index);
}

TEST(test_block_index_create_with_prev) {
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *genesis = block_index_create(&genesis_header, NULL);
    ASSERT_NOT_NULL(genesis);

    /* Create second block */
    hash256_t genesis_hash;
    block_header_hash(&genesis_header, &genesis_hash);

    block_header_t second_header;
    make_block_header(&second_header, &genesis_hash, 1231006506, 0x1d00ffff, 12345);

    block_index_t *second = block_index_create(&second_header, genesis);
    ASSERT_NOT_NULL(second);

    ASSERT_EQ(second->height, 1);
    ASSERT_EQ(second->prev, genesis);

    /* Chainwork should be greater than genesis */
    ASSERT_EQ(work256_compare(&second->chainwork, &genesis->chainwork), 1);

    block_index_destroy(second);
    block_index_destroy(genesis);
}

/* ========================================================================
 * Block Delta Tests
 * ======================================================================== */

TEST(test_block_delta_create) {
    hash256_t hash;
    memset(hash.bytes, 0x42, 32);

    block_delta_t *delta = block_delta_create(&hash, 100);
    ASSERT_NOT_NULL(delta);

    ASSERT_EQ(memcmp(delta->block_hash.bytes, hash.bytes, 32), 0);
    ASSERT_EQ(delta->height, 100);
    ASSERT_EQ(delta->created_count, 0);
    ASSERT_EQ(delta->spent_count, 0);

    block_delta_destroy(delta);
}

TEST(test_block_delta_add_created) {
    hash256_t hash;
    memset(hash.bytes, 0x42, 32);

    block_delta_t *delta = block_delta_create(&hash, 100);
    ASSERT_NOT_NULL(delta);

    outpoint_t op;
    memset(op.txid.bytes, 0xAB, 32);
    op.vout = 0;

    echo_result_t result = block_delta_add_created(delta, &op);
    ASSERT_EQ(result, ECHO_OK);
    ASSERT_EQ(delta->created_count, 1);
    ASSERT_EQ(memcmp(delta->created[0].txid.bytes, op.txid.bytes, 32), 0);

    /* Add another */
    op.vout = 1;
    result = block_delta_add_created(delta, &op);
    ASSERT_EQ(result, ECHO_OK);
    ASSERT_EQ(delta->created_count, 2);

    block_delta_destroy(delta);
}

TEST(test_block_delta_add_spent) {
    hash256_t hash;
    memset(hash.bytes, 0x42, 32);

    block_delta_t *delta = block_delta_create(&hash, 100);
    ASSERT_NOT_NULL(delta);

    /* Create a UTXO entry */
    outpoint_t op;
    memset(op.txid.bytes, 0xCD, 32);
    op.vout = 0;

    uint8_t script[25];
    memset(script, 0x76, 25);

    utxo_entry_t *entry = utxo_entry_create(&op, 5000000000, script, 25, 50, false);
    ASSERT_NOT_NULL(entry);

    echo_result_t result = block_delta_add_spent(delta, entry);
    ASSERT_EQ(result, ECHO_OK);
    ASSERT_EQ(delta->spent_count, 1);
    ASSERT_NOT_NULL(delta->spent[0]);
    ASSERT_EQ(delta->spent[0]->value, 5000000000);

    utxo_entry_destroy(entry);
    block_delta_destroy(delta);
}

/* ========================================================================
 * Chain State Tests
 * ======================================================================== */

TEST(test_chainstate_create) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    ASSERT_EQ(chainstate_get_height(state), 0);

    const utxo_set_t *utxo_set = chainstate_get_utxo_set(state);
    ASSERT_NOT_NULL(utxo_set);
    ASSERT_EQ(utxo_set_size(utxo_set), 0);

    chainstate_destroy(state);
}

TEST(test_chainstate_get_tip_initial) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    chain_tip_t tip;
    echo_result_t result = chainstate_get_tip(state, &tip);
    ASSERT_EQ(result, ECHO_OK);

    ASSERT_EQ(tip.height, 0);
    ASSERT_TRUE(work256_is_zero(&tip.chainwork));

    chainstate_destroy(state);
}

TEST(test_chainstate_apply_genesis) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    /* Create genesis block header */
    block_header_t header;
    make_block_header(&header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    /* Create genesis coinbase transaction */
    tx_t coinbase;
    make_simple_tx(&coinbase, 0x01, 5000000000, true);

    /* Apply the genesis block */
    block_delta_t *delta = NULL;
    echo_result_t result = chainstate_apply_block(state, &header, &coinbase, 1, &delta);
    ASSERT_EQ(result, ECHO_OK);
    ASSERT_NOT_NULL(delta);

    /* Verify chain tip updated */
    chain_tip_t tip;
    chainstate_get_tip(state, &tip);
    ASSERT_EQ(tip.height, 0);
    ASSERT_FALSE(work256_is_zero(&tip.chainwork));

    /* Verify UTXO created */
    const utxo_set_t *utxo_set = chainstate_get_utxo_set(state);
    ASSERT_EQ(utxo_set_size(utxo_set), 1);

    /* Delta should record created UTXO */
    ASSERT_EQ(delta->created_count, 1);
    ASSERT_EQ(delta->spent_count, 0);

    block_delta_destroy(delta);
    tx_free(&coinbase);
    chainstate_destroy(state);
}

TEST(test_chainstate_apply_second_block) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    /* Apply genesis */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    tx_t genesis_coinbase;
    make_simple_tx(&genesis_coinbase, 0x01, 5000000000, true);

    block_delta_t *genesis_delta = NULL;
    chainstate_apply_block(state, &genesis_header, &genesis_coinbase, 1, &genesis_delta);

    /* Get genesis hash for linking */
    hash256_t genesis_hash;
    block_header_hash(&genesis_header, &genesis_hash);

    /* Create second block */
    block_header_t second_header;
    make_block_header(&second_header, &genesis_hash, 1231006506, 0x1d00ffff, 12345);

    tx_t second_coinbase;
    make_simple_tx(&second_coinbase, 0x02, 5000000000, true);

    block_delta_t *second_delta = NULL;
    echo_result_t result = chainstate_apply_block(state, &second_header, &second_coinbase, 1, &second_delta);
    ASSERT_EQ(result, ECHO_OK);

    /* Verify chain tip */
    chain_tip_t tip;
    chainstate_get_tip(state, &tip);
    ASSERT_EQ(tip.height, 1);

    /* Verify UTXOs */
    ASSERT_EQ(utxo_set_size(chainstate_get_utxo_set(state)), 2);

    block_delta_destroy(genesis_delta);
    block_delta_destroy(second_delta);
    tx_free(&genesis_coinbase);
    tx_free(&second_coinbase);
    chainstate_destroy(state);
}

TEST(test_chainstate_apply_block_wrong_prev) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    /* Apply genesis first */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    tx_t genesis_coinbase;
    make_simple_tx(&genesis_coinbase, 0x01, 5000000000, true);

    chainstate_apply_block(state, &genesis_header, &genesis_coinbase, 1, NULL);

    /* Try to apply block with wrong prev_hash */
    hash256_t wrong_prev;
    memset(wrong_prev.bytes, 0xFF, 32);  /* Wrong hash */

    block_header_t second_header;
    make_block_header(&second_header, &wrong_prev, 1231006506, 0x1d00ffff, 12345);

    tx_t second_coinbase;
    make_simple_tx(&second_coinbase, 0x02, 5000000000, true);

    echo_result_t result = chainstate_apply_block(state, &second_header, &second_coinbase, 1, NULL);
    ASSERT_EQ(result, ECHO_ERR_INVALID_BLOCK);

    /* Height should still be 0 */
    ASSERT_EQ(chainstate_get_height(state), 0);

    tx_free(&genesis_coinbase);
    tx_free(&second_coinbase);
    chainstate_destroy(state);
}

TEST(test_chainstate_revert_block) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    /* Apply genesis */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    tx_t genesis_coinbase;
    make_simple_tx(&genesis_coinbase, 0x01, 5000000000, true);

    block_delta_t *genesis_delta = NULL;
    chainstate_apply_block(state, &genesis_header, &genesis_coinbase, 1, &genesis_delta);

    hash256_t genesis_hash;
    block_header_hash(&genesis_header, &genesis_hash);

    /* Apply second block */
    block_header_t second_header;
    make_block_header(&second_header, &genesis_hash, 1231006506, 0x1d00ffff, 12345);

    tx_t second_coinbase;
    make_simple_tx(&second_coinbase, 0x02, 5000000000, true);

    block_delta_t *second_delta = NULL;
    chainstate_apply_block(state, &second_header, &second_coinbase, 1, &second_delta);

    ASSERT_EQ(chainstate_get_height(state), 1);
    ASSERT_EQ(utxo_set_size(chainstate_get_utxo_set(state)), 2);

    /* Revert second block */
    echo_result_t result = chainstate_revert_block(state, second_delta);
    ASSERT_EQ(result, ECHO_OK);

    /* Verify we're back to height 0 */
    ASSERT_EQ(chainstate_get_height(state), 0);

    /* Verify UTXO from second block is removed */
    ASSERT_EQ(utxo_set_size(chainstate_get_utxo_set(state)), 1);

    block_delta_destroy(genesis_delta);
    block_delta_destroy(second_delta);
    tx_free(&genesis_coinbase);
    tx_free(&second_coinbase);
    chainstate_destroy(state);
}

TEST(test_chainstate_spending_utxo) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    /* Apply genesis with coinbase */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    tx_t genesis_coinbase;
    make_simple_tx(&genesis_coinbase, 0x01, 5000000000, true);

    chainstate_apply_block(state, &genesis_header, &genesis_coinbase, 1, NULL);

    /* Get the UTXO created by genesis coinbase */
    hash256_t genesis_txid;
    tx_compute_txid(&genesis_coinbase, &genesis_txid);

    outpoint_t genesis_outpoint;
    genesis_outpoint.txid = genesis_txid;
    genesis_outpoint.vout = 0;

    /* Verify it exists */
    const utxo_entry_t *utxo = chainstate_lookup_utxo(state, &genesis_outpoint);
    ASSERT_NOT_NULL(utxo);
    ASSERT_EQ(utxo->value, 5000000000);

    /* Create second block that spends the genesis coinbase */
    hash256_t genesis_hash;
    block_header_hash(&genesis_header, &genesis_hash);

    block_header_t second_header;
    make_block_header(&second_header, &genesis_hash, 1231006506, 0x1d00ffff, 12345);

    /* Create coinbase for second block */
    tx_t second_coinbase;
    make_simple_tx(&second_coinbase, 0x02, 5000000000, true);

    /* Create spending transaction */
    tx_t spending_tx;
    tx_init(&spending_tx);
    spending_tx.version = 1;
    spending_tx.locktime = 0;

    /* Input references genesis coinbase output */
    spending_tx.input_count = 1;
    spending_tx.inputs = malloc(sizeof(tx_input_t));
    spending_tx.inputs[0].prevout = genesis_outpoint;
    spending_tx.inputs[0].sequence = 0xFFFFFFFF;
    spending_tx.inputs[0].script_sig_len = 4;
    spending_tx.inputs[0].script_sig = malloc(4);
    memset(spending_tx.inputs[0].script_sig, 0xAA, 4);
    spending_tx.inputs[0].witness.count = 0;
    spending_tx.inputs[0].witness.items = NULL;

    /* Output sends to new address */
    spending_tx.output_count = 1;
    spending_tx.outputs = malloc(sizeof(tx_output_t));
    spending_tx.outputs[0].value = 4999000000;  /* Minus fee */
    spending_tx.outputs[0].script_pubkey_len = 25;
    spending_tx.outputs[0].script_pubkey = malloc(25);
    memset(spending_tx.outputs[0].script_pubkey, 0xBB, 25);

    /* Apply second block with both transactions */
    tx_t txs[2] = {second_coinbase, spending_tx};
    block_delta_t *delta = NULL;
    echo_result_t result = chainstate_apply_block(state, &second_header, txs, 2, &delta);
    ASSERT_EQ(result, ECHO_OK);

    /* Genesis coinbase output should now be spent */
    utxo = chainstate_lookup_utxo(state, &genesis_outpoint);
    ASSERT_NULL(utxo);

    /* New UTXO from spending tx should exist */
    hash256_t spending_txid;
    tx_compute_txid(&spending_tx, &spending_txid);

    outpoint_t new_outpoint;
    new_outpoint.txid = spending_txid;
    new_outpoint.vout = 0;

    utxo = chainstate_lookup_utxo(state, &new_outpoint);
    ASSERT_NOT_NULL(utxo);
    ASSERT_EQ(utxo->value, 4999000000);

    /* Delta should record both spent and created */
    ASSERT_EQ(delta->spent_count, 1);
    ASSERT_EQ(delta->created_count, 2);  /* coinbase + spending tx output */

    block_delta_destroy(delta);
    tx_free(&genesis_coinbase);
    tx_free(&second_coinbase);
    tx_free(&spending_tx);
    chainstate_destroy(state);
}

TEST(test_chainstate_is_on_main_chain) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    /* Apply genesis */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    tx_t genesis_coinbase;
    make_simple_tx(&genesis_coinbase, 0x01, 5000000000, true);

    chainstate_apply_block(state, &genesis_header, &genesis_coinbase, 1, NULL);

    hash256_t genesis_hash;
    block_header_hash(&genesis_header, &genesis_hash);

    /* Genesis should be on main chain */
    ASSERT_TRUE(chainstate_is_on_main_chain(state, &genesis_hash));

    /* Random hash should not be on main chain */
    hash256_t random_hash;
    memset(random_hash.bytes, 0xDE, 32);
    ASSERT_FALSE(chainstate_is_on_main_chain(state, &random_hash));

    tx_free(&genesis_coinbase);
    chainstate_destroy(state);
}

TEST(test_chainstate_get_block_at_height) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    /* Apply genesis */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    tx_t genesis_coinbase;
    make_simple_tx(&genesis_coinbase, 0x01, 5000000000, true);

    chainstate_apply_block(state, &genesis_header, &genesis_coinbase, 1, NULL);

    hash256_t genesis_hash;
    block_header_hash(&genesis_header, &genesis_hash);

    /* Query height 0 */
    hash256_t queried_hash;
    echo_result_t result = chainstate_get_block_at_height(state, 0, &queried_hash);
    ASSERT_EQ(result, ECHO_OK);
    ASSERT_EQ(memcmp(queried_hash.bytes, genesis_hash.bytes, 32), 0);

    /* Query non-existent height */
    result = chainstate_get_block_at_height(state, 1, &queried_hash);
    ASSERT_EQ(result, ECHO_ERR_NOT_FOUND);

    tx_free(&genesis_coinbase);
    chainstate_destroy(state);
}

TEST(test_chainstate_multiple_outputs) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    /* Create block with transaction that has multiple outputs */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    tx_t coinbase;
    tx_init(&coinbase);
    coinbase.version = 1;
    coinbase.locktime = 0;

    /* Coinbase input */
    coinbase.input_count = 1;
    coinbase.inputs = malloc(sizeof(tx_input_t));
    memset(coinbase.inputs[0].prevout.txid.bytes, 0, 32);
    coinbase.inputs[0].prevout.vout = 0xFFFFFFFF;
    coinbase.inputs[0].sequence = 0xFFFFFFFF;
    coinbase.inputs[0].script_sig_len = 4;
    coinbase.inputs[0].script_sig = malloc(4);
    memset(coinbase.inputs[0].script_sig, 0x01, 4);
    coinbase.inputs[0].witness.count = 0;
    coinbase.inputs[0].witness.items = NULL;

    /* Three outputs */
    coinbase.output_count = 3;
    coinbase.outputs = malloc(3 * sizeof(tx_output_t));

    for (int i = 0; i < 3; i++) {
        coinbase.outputs[i].value = 1000000000 + i * 100;
        coinbase.outputs[i].script_pubkey_len = 25;
        coinbase.outputs[i].script_pubkey = malloc(25);
        memset(coinbase.outputs[i].script_pubkey, 0x70 + i, 25);
    }

    block_delta_t *delta = NULL;
    echo_result_t result = chainstate_apply_block(state, &genesis_header, &coinbase, 1, &delta);
    ASSERT_EQ(result, ECHO_OK);

    /* Should have 3 UTXOs */
    ASSERT_EQ(utxo_set_size(chainstate_get_utxo_set(state)), 3);
    ASSERT_EQ(delta->created_count, 3);

    /* Verify each UTXO */
    hash256_t txid;
    tx_compute_txid(&coinbase, &txid);

    for (int i = 0; i < 3; i++) {
        outpoint_t op;
        op.txid = txid;
        op.vout = i;

        const utxo_entry_t *utxo = chainstate_lookup_utxo(state, &op);
        ASSERT_NOT_NULL(utxo);
        ASSERT_EQ(utxo->value, 1000000000 + i * 100);
    }

    block_delta_destroy(delta);
    tx_free(&coinbase);
    chainstate_destroy(state);
}

/* ========================================================================
 * Block Index Map Tests (Session 6.3)
 * ======================================================================== */

TEST(test_block_index_map_create) {
    block_index_map_t *map = block_index_map_create(0);
    ASSERT_NOT_NULL(map);
    ASSERT_EQ(block_index_map_size(map), 0);
    block_index_map_destroy(map);
}

TEST(test_block_index_map_insert_and_lookup) {
    block_index_map_t *map = block_index_map_create(16);
    ASSERT_NOT_NULL(map);

    /* Create a block index */
    block_header_t header;
    make_block_header(&header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *index = block_index_create(&header, NULL);
    ASSERT_NOT_NULL(index);

    /* Insert */
    echo_result_t result = block_index_map_insert(map, index);
    ASSERT_EQ(result, ECHO_OK);
    ASSERT_EQ(block_index_map_size(map), 1);

    /* Lookup by hash */
    block_index_t *found = block_index_map_lookup(map, &index->hash);
    ASSERT_NOT_NULL(found);
    ASSERT_EQ(found, index);

    /* Lookup unknown hash */
    hash256_t unknown_hash;
    memset(unknown_hash.bytes, 0xFF, 32);
    found = block_index_map_lookup(map, &unknown_hash);
    ASSERT_NULL(found);

    block_index_map_destroy(map);
}

TEST(test_block_index_map_insert_duplicate) {
    block_index_map_t *map = block_index_map_create(16);
    ASSERT_NOT_NULL(map);

    block_header_t header;
    make_block_header(&header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *index1 = block_index_create(&header, NULL);
    ASSERT_NOT_NULL(index1);

    block_index_t *index2 = block_index_create(&header, NULL);
    ASSERT_NOT_NULL(index2);

    /* Insert first */
    ASSERT_EQ(block_index_map_insert(map, index1), ECHO_OK);

    /* Insert duplicate should fail */
    ASSERT_EQ(block_index_map_insert(map, index2), ECHO_ERR_EXISTS);

    /* Clean up - index2 was not inserted, so we own it */
    block_index_destroy(index2);
    block_index_map_destroy(map);
}

TEST(test_block_index_map_multiple_blocks) {
    block_index_map_t *map = block_index_map_create(16);
    ASSERT_NOT_NULL(map);

    /* Create genesis */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *genesis = block_index_create(&genesis_header, NULL);
    ASSERT_NOT_NULL(genesis);
    ASSERT_EQ(block_index_map_insert(map, genesis), ECHO_OK);

    /* Create second block */
    block_header_t second_header;
    make_block_header(&second_header, &genesis->hash, 1231006506, 0x1d00ffff, 12345);

    block_index_t *second = block_index_create(&second_header, genesis);
    ASSERT_NOT_NULL(second);
    ASSERT_EQ(block_index_map_insert(map, second), ECHO_OK);

    /* Create third block */
    block_header_t third_header;
    make_block_header(&third_header, &second->hash, 1231006507, 0x1d00ffff, 54321);

    block_index_t *third = block_index_create(&third_header, second);
    ASSERT_NOT_NULL(third);
    ASSERT_EQ(block_index_map_insert(map, third), ECHO_OK);

    ASSERT_EQ(block_index_map_size(map), 3);

    /* Verify lookups */
    ASSERT_EQ(block_index_map_lookup(map, &genesis->hash), genesis);
    ASSERT_EQ(block_index_map_lookup(map, &second->hash), second);
    ASSERT_EQ(block_index_map_lookup(map, &third->hash), third);

    block_index_map_destroy(map);
}

TEST(test_block_index_map_find_best) {
    block_index_map_t *map = block_index_map_create(16);
    ASSERT_NOT_NULL(map);

    /* Create chain of blocks */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *genesis = block_index_create(&genesis_header, NULL);
    block_index_map_insert(map, genesis);

    block_header_t second_header;
    make_block_header(&second_header, &genesis->hash, 1231006506, 0x1d00ffff, 12345);

    block_index_t *second = block_index_create(&second_header, genesis);
    block_index_map_insert(map, second);

    /* Best should be second (most chainwork) */
    block_index_t *best = block_index_map_find_best(map);
    ASSERT_NOT_NULL(best);
    ASSERT_EQ(best, second);

    block_index_map_destroy(map);
}

/* ========================================================================
 * Chain Comparison Tests (Session 6.3)
 * ======================================================================== */

TEST(test_chain_compare_equal) {
    block_header_t header;
    make_block_header(&header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *a = block_index_create(&header, NULL);
    ASSERT_NOT_NULL(a);

    /* Create another genesis with same difficulty */
    block_header_t header_b;
    make_block_header(&header_b, NULL, 1231006506, 0x1d00ffff, 12345);

    block_index_t *b = block_index_create(&header_b, NULL);
    ASSERT_NOT_NULL(b);

    /* Same work - should return EQUAL */
    ASSERT_EQ(chain_compare(a, b), CHAIN_COMPARE_EQUAL);

    block_index_destroy(a);
    block_index_destroy(b);
}

TEST(test_chain_compare_a_better) {
    /* Chain A: genesis + one block */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *genesis = block_index_create(&genesis_header, NULL);
    ASSERT_NOT_NULL(genesis);

    block_header_t second_header;
    make_block_header(&second_header, &genesis->hash, 1231006506, 0x1d00ffff, 12345);

    block_index_t *chain_a_tip = block_index_create(&second_header, genesis);
    ASSERT_NOT_NULL(chain_a_tip);

    /* Chain B: just genesis (different nonce) */
    block_header_t genesis_b_header;
    make_block_header(&genesis_b_header, NULL, 1231006506, 0x1d00ffff, 99999);

    block_index_t *chain_b_tip = block_index_create(&genesis_b_header, NULL);
    ASSERT_NOT_NULL(chain_b_tip);

    /* A has more work (2 blocks vs 1) */
    ASSERT_EQ(chain_compare(chain_a_tip, chain_b_tip), CHAIN_COMPARE_A_BETTER);

    block_index_destroy(genesis);
    block_index_destroy(chain_a_tip);
    block_index_destroy(chain_b_tip);
}

TEST(test_chain_compare_b_better) {
    /* Chain A: just genesis */
    block_header_t genesis_a_header;
    make_block_header(&genesis_a_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *chain_a_tip = block_index_create(&genesis_a_header, NULL);
    ASSERT_NOT_NULL(chain_a_tip);

    /* Chain B: genesis + one block */
    block_header_t genesis_b_header;
    make_block_header(&genesis_b_header, NULL, 1231006506, 0x1d00ffff, 99999);

    block_index_t *genesis_b = block_index_create(&genesis_b_header, NULL);
    ASSERT_NOT_NULL(genesis_b);

    block_header_t second_b_header;
    make_block_header(&second_b_header, &genesis_b->hash, 1231006507, 0x1d00ffff, 11111);

    block_index_t *chain_b_tip = block_index_create(&second_b_header, genesis_b);
    ASSERT_NOT_NULL(chain_b_tip);

    /* B has more work (2 blocks vs 1) */
    ASSERT_EQ(chain_compare(chain_a_tip, chain_b_tip), CHAIN_COMPARE_B_BETTER);

    block_index_destroy(chain_a_tip);
    block_index_destroy(genesis_b);
    block_index_destroy(chain_b_tip);
}

TEST(test_chain_compare_higher_difficulty) {
    /* Chain A: 1 block at low difficulty */
    block_header_t header_a;
    make_block_header(&header_a, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *chain_a = block_index_create(&header_a, NULL);
    ASSERT_NOT_NULL(chain_a);

    /* Chain B: 1 block at higher difficulty */
    block_header_t header_b;
    make_block_header(&header_b, NULL, 1231006506, 0x1c00ffff, 99999);  /* 0x1c is harder */

    block_index_t *chain_b = block_index_create(&header_b, NULL);
    ASSERT_NOT_NULL(chain_b);

    /* B has more work due to higher difficulty */
    ASSERT_EQ(chain_compare(chain_a, chain_b), CHAIN_COMPARE_B_BETTER);

    block_index_destroy(chain_a);
    block_index_destroy(chain_b);
}

/* ========================================================================
 * Common Ancestor Tests (Session 6.3)
 * ======================================================================== */

TEST(test_chain_find_common_ancestor_same_chain) {
    /* Create a simple chain: A -> B -> C */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *genesis = block_index_create(&genesis_header, NULL);
    ASSERT_NOT_NULL(genesis);

    block_header_t block_b_header;
    make_block_header(&block_b_header, &genesis->hash, 1231006506, 0x1d00ffff, 12345);

    block_index_t *block_b = block_index_create(&block_b_header, genesis);
    ASSERT_NOT_NULL(block_b);

    block_header_t block_c_header;
    make_block_header(&block_c_header, &block_b->hash, 1231006507, 0x1d00ffff, 54321);

    block_index_t *block_c = block_index_create(&block_c_header, block_b);
    ASSERT_NOT_NULL(block_c);

    /* Common ancestor of B and C is B */
    block_index_t *ancestor = chain_find_common_ancestor(block_b, block_c);
    ASSERT_NOT_NULL(ancestor);
    ASSERT_EQ(memcmp(ancestor->hash.bytes, block_b->hash.bytes, 32), 0);

    /* Common ancestor of A and C is A */
    ancestor = chain_find_common_ancestor(genesis, block_c);
    ASSERT_NOT_NULL(ancestor);
    ASSERT_EQ(memcmp(ancestor->hash.bytes, genesis->hash.bytes, 32), 0);

    block_index_destroy(genesis);
    block_index_destroy(block_b);
    block_index_destroy(block_c);
}

TEST(test_chain_find_common_ancestor_fork) {
    /* Create a fork:
     *   A -> B -> C
     *        \-> D -> E
     */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *genesis = block_index_create(&genesis_header, NULL);

    block_header_t block_b_header;
    make_block_header(&block_b_header, &genesis->hash, 1231006506, 0x1d00ffff, 12345);
    block_index_t *block_b = block_index_create(&block_b_header, genesis);

    /* Main chain continues: B -> C */
    block_header_t block_c_header;
    make_block_header(&block_c_header, &block_b->hash, 1231006507, 0x1d00ffff, 54321);
    block_index_t *block_c = block_index_create(&block_c_header, block_b);

    /* Fork: B -> D -> E */
    block_header_t block_d_header;
    make_block_header(&block_d_header, &block_b->hash, 1231006508, 0x1d00ffff, 11111);
    block_index_t *block_d = block_index_create(&block_d_header, block_b);

    block_header_t block_e_header;
    make_block_header(&block_e_header, &block_d->hash, 1231006509, 0x1d00ffff, 22222);
    block_index_t *block_e = block_index_create(&block_e_header, block_d);

    /* Common ancestor of C and E is B */
    block_index_t *ancestor = chain_find_common_ancestor(block_c, block_e);
    ASSERT_NOT_NULL(ancestor);
    ASSERT_EQ(memcmp(ancestor->hash.bytes, block_b->hash.bytes, 32), 0);

    block_index_destroy(genesis);
    block_index_destroy(block_b);
    block_index_destroy(block_c);
    block_index_destroy(block_d);
    block_index_destroy(block_e);
}

TEST(test_chain_find_common_ancestor_different_heights) {
    /* Create chain: A -> B -> C -> D */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 1);

    block_index_t *a = block_index_create(&genesis_header, NULL);

    block_header_t b_header;
    make_block_header(&b_header, &a->hash, 1231006506, 0x1d00ffff, 2);
    block_index_t *b = block_index_create(&b_header, a);

    block_header_t c_header;
    make_block_header(&c_header, &b->hash, 1231006507, 0x1d00ffff, 3);
    block_index_t *c = block_index_create(&c_header, b);

    block_header_t d_header;
    make_block_header(&d_header, &c->hash, 1231006508, 0x1d00ffff, 4);
    block_index_t *d = block_index_create(&d_header, c);

    /* Common ancestor of A and D is A */
    block_index_t *ancestor = chain_find_common_ancestor(a, d);
    ASSERT_NOT_NULL(ancestor);
    ASSERT_EQ(ancestor->height, 0);

    /* Common ancestor of B and D is B */
    ancestor = chain_find_common_ancestor(b, d);
    ASSERT_NOT_NULL(ancestor);
    ASSERT_EQ(ancestor->height, 1);

    block_index_destroy(a);
    block_index_destroy(b);
    block_index_destroy(c);
    block_index_destroy(d);
}

/* ========================================================================
 * Reorganization Planning Tests (Session 6.3)
 * ======================================================================== */

TEST(test_chain_reorg_create_simple) {
    /* Create a fork:
     *   A -> B -> C  (current)
     *        \-> D   (new tip)
     */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 1);
    block_index_t *a = block_index_create(&genesis_header, NULL);

    block_header_t b_header;
    make_block_header(&b_header, &a->hash, 1231006506, 0x1d00ffff, 2);
    block_index_t *b = block_index_create(&b_header, a);

    block_header_t c_header;
    make_block_header(&c_header, &b->hash, 1231006507, 0x1d00ffff, 3);
    block_index_t *c = block_index_create(&c_header, b);

    block_header_t d_header;
    make_block_header(&d_header, &b->hash, 1231006508, 0x1d00ffff, 4);
    block_index_t *d = block_index_create(&d_header, b);

    /* Create reorg plan from C to D */
    chain_reorg_t *reorg = chain_reorg_create(c, d);
    ASSERT_NOT_NULL(reorg);

    /* Should disconnect C, connect D */
    ASSERT_EQ(reorg->disconnect_count, 1);
    ASSERT_EQ(reorg->connect_count, 1);
    ASSERT_EQ(reorg->disconnect[0], c);
    ASSERT_EQ(reorg->connect[0], d);
    ASSERT_EQ(memcmp(reorg->ancestor->hash.bytes, b->hash.bytes, 32), 0);

    chain_reorg_destroy(reorg);
    block_index_destroy(a);
    block_index_destroy(b);
    block_index_destroy(c);
    block_index_destroy(d);
}

TEST(test_chain_reorg_create_longer_fork) {
    /* Create a fork:
     *   A -> B -> C -> D  (current)
     *        \-> E -> F -> G (new tip, longer)
     */
    block_header_t a_header;
    make_block_header(&a_header, NULL, 1231006505, 0x1d00ffff, 1);
    block_index_t *a = block_index_create(&a_header, NULL);

    block_header_t b_header;
    make_block_header(&b_header, &a->hash, 1231006506, 0x1d00ffff, 2);
    block_index_t *b = block_index_create(&b_header, a);

    /* Main chain: B -> C -> D */
    block_header_t c_header;
    make_block_header(&c_header, &b->hash, 1231006507, 0x1d00ffff, 3);
    block_index_t *c = block_index_create(&c_header, b);

    block_header_t d_header;
    make_block_header(&d_header, &c->hash, 1231006508, 0x1d00ffff, 4);
    block_index_t *d = block_index_create(&d_header, c);

    /* Fork: B -> E -> F -> G */
    block_header_t e_header;
    make_block_header(&e_header, &b->hash, 1231006509, 0x1d00ffff, 5);
    block_index_t *e = block_index_create(&e_header, b);

    block_header_t f_header;
    make_block_header(&f_header, &e->hash, 1231006510, 0x1d00ffff, 6);
    block_index_t *f = block_index_create(&f_header, e);

    block_header_t g_header;
    make_block_header(&g_header, &f->hash, 1231006511, 0x1d00ffff, 7);
    block_index_t *g = block_index_create(&g_header, f);

    /* Create reorg plan from D to G */
    chain_reorg_t *reorg = chain_reorg_create(d, g);
    ASSERT_NOT_NULL(reorg);

    /* Should disconnect D, C (2 blocks), connect E, F, G (3 blocks) */
    ASSERT_EQ(reorg->disconnect_count, 2);
    ASSERT_EQ(reorg->connect_count, 3);

    /* Disconnect order: D, C (tip to ancestor) */
    ASSERT_EQ(reorg->disconnect[0], d);
    ASSERT_EQ(reorg->disconnect[1], c);

    /* Connect order: E, F, G (ancestor to new tip) */
    ASSERT_EQ(reorg->connect[0], e);
    ASSERT_EQ(reorg->connect[1], f);
    ASSERT_EQ(reorg->connect[2], g);

    ASSERT_EQ(memcmp(reorg->ancestor->hash.bytes, b->hash.bytes, 32), 0);

    chain_reorg_destroy(reorg);
    block_index_destroy(a);
    block_index_destroy(b);
    block_index_destroy(c);
    block_index_destroy(d);
    block_index_destroy(e);
    block_index_destroy(f);
    block_index_destroy(g);
}

TEST(test_chain_reorg_create_extend_tip) {
    /* Not a true reorg - just extending the chain:
     *   A -> B (current)
     *        \-> C (new tip extending B)
     */
    block_header_t a_header;
    make_block_header(&a_header, NULL, 1231006505, 0x1d00ffff, 1);
    block_index_t *a = block_index_create(&a_header, NULL);

    block_header_t b_header;
    make_block_header(&b_header, &a->hash, 1231006506, 0x1d00ffff, 2);
    block_index_t *b = block_index_create(&b_header, a);

    block_header_t c_header;
    make_block_header(&c_header, &b->hash, 1231006507, 0x1d00ffff, 3);
    block_index_t *c = block_index_create(&c_header, b);

    /* "Reorg" from B to C - no disconnect, just connect */
    chain_reorg_t *reorg = chain_reorg_create(b, c);
    ASSERT_NOT_NULL(reorg);

    ASSERT_EQ(reorg->disconnect_count, 0);
    ASSERT_EQ(reorg->connect_count, 1);
    ASSERT_EQ(reorg->connect[0], c);
    ASSERT_EQ(memcmp(reorg->ancestor->hash.bytes, b->hash.bytes, 32), 0);

    chain_reorg_destroy(reorg);
    block_index_destroy(a);
    block_index_destroy(b);
    block_index_destroy(c);
}

/* ========================================================================
 * Chain State Integration Tests (Session 6.3)
 * ======================================================================== */

TEST(test_chainstate_add_header) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *index = NULL;
    echo_result_t result = chainstate_add_header(state, &genesis_header, &index);
    ASSERT_EQ(result, ECHO_OK);
    ASSERT_NOT_NULL(index);
    ASSERT_EQ(index->height, 0);

    /* Verify it's in the map */
    block_index_map_t *map = chainstate_get_block_index_map(state);
    ASSERT_NOT_NULL(map);
    ASSERT_EQ(block_index_map_size(map), 1);

    block_index_t *found = block_index_map_lookup(map, &index->hash);
    ASSERT_EQ(found, index);

    chainstate_destroy(state);
}

TEST(test_chainstate_add_header_duplicate) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    /* Add first time */
    ASSERT_EQ(chainstate_add_header(state, &genesis_header, NULL), ECHO_OK);

    /* Add again - should fail */
    ASSERT_EQ(chainstate_add_header(state, &genesis_header, NULL), ECHO_ERR_EXISTS);

    chainstate_destroy(state);
}

TEST(test_chainstate_add_header_chain) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    /* Add genesis */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *genesis_index;
    chainstate_add_header(state, &genesis_header, &genesis_index);

    /* Add second block */
    block_header_t second_header;
    make_block_header(&second_header, &genesis_index->hash, 1231006506, 0x1d00ffff, 12345);

    block_index_t *second_index;
    chainstate_add_header(state, &second_header, &second_index);

    /* Verify chain link */
    ASSERT_EQ(second_index->height, 1);
    ASSERT_EQ(second_index->prev, genesis_index);

    /* Verify accumulated work */
    ASSERT_EQ(work256_compare(&second_index->chainwork, &genesis_index->chainwork), 1);

    chainstate_destroy(state);
}

TEST(test_chainstate_should_reorg) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    /* Add genesis and set as tip */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *genesis_index;
    chainstate_add_header(state, &genesis_header, &genesis_index);
    chainstate_set_tip_index(state, genesis_index);

    /* Create a longer chain separately */
    block_header_t alt_genesis_header;
    make_block_header(&alt_genesis_header, NULL, 1231006506, 0x1d00ffff, 99999);
    block_index_t *alt_genesis = block_index_create(&alt_genesis_header, NULL);

    block_header_t alt_second_header;
    make_block_header(&alt_second_header, &alt_genesis->hash, 1231006507, 0x1d00ffff, 11111);
    block_index_t *alt_second = block_index_create(&alt_second_header, alt_genesis);

    /* Should want to reorg to the longer chain */
    ASSERT_TRUE(chainstate_should_reorg(state, alt_second));

    /* Should NOT reorg to shorter chain */
    ASSERT_FALSE(chainstate_should_reorg(state, alt_genesis));

    block_index_destroy(alt_genesis);
    block_index_destroy(alt_second);
    chainstate_destroy(state);
}

TEST(test_chainstate_tip_index) {
    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    /* Initially no tip */
    ASSERT_NULL(chainstate_get_tip_index(state));

    /* Add and set tip */
    block_header_t genesis_header;
    make_block_header(&genesis_header, NULL, 1231006505, 0x1d00ffff, 2083236893);

    block_index_t *genesis_index;
    chainstate_add_header(state, &genesis_header, &genesis_index);
    chainstate_set_tip_index(state, genesis_index);

    ASSERT_EQ(chainstate_get_tip_index(state), genesis_index);

    /* Verify tip info matches */
    chain_tip_t tip;
    chainstate_get_tip(state, &tip);
    ASSERT_EQ(memcmp(tip.hash.bytes, genesis_index->hash.bytes, 32), 0);
    ASSERT_EQ(tip.height, genesis_index->height);

    chainstate_destroy(state);
}

/* ========================================================================
 * Main Test Runner
 * ======================================================================== */

int main(void) {
    printf("Running Chain State tests...\n\n");

    /* Work256 tests */
    run_test_work256_zero();
    run_test_work256_is_zero_nonzero();
    run_test_work256_compare_equal();
    run_test_work256_compare_less();
    run_test_work256_compare_greater();
    run_test_work256_add_simple();
    run_test_work256_add_carry();
    run_test_work256_add_large();
    run_test_work256_sub_simple();
    run_test_work256_sub_borrow();
    run_test_work256_sub_underflow();
    run_test_work256_from_bits_mainnet();
    run_test_work256_from_bits_higher_difficulty();

    /* Block index tests */
    run_test_block_index_create_genesis();
    run_test_block_index_create_with_prev();

    /* Block delta tests */
    run_test_block_delta_create();
    run_test_block_delta_add_created();
    run_test_block_delta_add_spent();

    /* Chain state tests */
    run_test_chainstate_create();
    run_test_chainstate_get_tip_initial();
    run_test_chainstate_apply_genesis();
    run_test_chainstate_apply_second_block();
    run_test_chainstate_apply_block_wrong_prev();
    run_test_chainstate_revert_block();
    run_test_chainstate_spending_utxo();
    run_test_chainstate_is_on_main_chain();
    run_test_chainstate_get_block_at_height();
    run_test_chainstate_multiple_outputs();

    /* Block index map tests (Session 6.3) */
    run_test_block_index_map_create();
    run_test_block_index_map_insert_and_lookup();
    run_test_block_index_map_insert_duplicate();
    run_test_block_index_map_multiple_blocks();
    run_test_block_index_map_find_best();

    /* Chain comparison tests (Session 6.3) */
    run_test_chain_compare_equal();
    run_test_chain_compare_a_better();
    run_test_chain_compare_b_better();
    run_test_chain_compare_higher_difficulty();

    /* Common ancestor tests (Session 6.3) */
    run_test_chain_find_common_ancestor_same_chain();
    run_test_chain_find_common_ancestor_fork();
    run_test_chain_find_common_ancestor_different_heights();

    /* Reorganization planning tests (Session 6.3) */
    run_test_chain_reorg_create_simple();
    run_test_chain_reorg_create_longer_fork();
    run_test_chain_reorg_create_extend_tip();

    /* Chain state integration tests (Session 6.3) */
    run_test_chainstate_add_header();
    run_test_chainstate_add_header_duplicate();
    run_test_chainstate_add_header_chain();
    run_test_chainstate_should_reorg();
    run_test_chainstate_tip_index();

    printf("\n========================================\n");
    printf("Chain State Tests: %d/%d passed\n", tests_passed, tests_run);
    printf("========================================\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
