/**
 * @file test_reorg.c
 * @brief Chain reorganization tests for the chainstate UTXO and chainwork system.
 *
 * Tests three reorg scenarios:
 *   1. Simple 2-block fork: A1->A2->A3 vs A1->B2->B3->B4 (B wins by length)
 *   2. Deep 6-block reorg: genesis->6 blocks vs genesis->7 blocks
 *   3. Same-work competing chains: no reorg triggered (equal work)
 *
 * These tests exercise chain_reorganize(), chainstate_revert_block() via the
 * delta stored by chainstate_apply_block(), and chainwork restoration.
 *
 * The tests do NOT require real PoW or script validation.
 *
 * Build once. Build right. Stop.
 */

#include "chainstate.h"
#include "block.h"
#include "tx.h"
#include "echo_types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "test_utils.h"

/* ========================================================================
 * Test Assertion Macros
 * ======================================================================== */

#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            printf("\n  Assertion failed: %s\n  at %s:%d\n", \
                   #cond, __FILE__, __LINE__); \
            return; \
        } \
    } while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_TRUE(x) ASSERT(x)
#define ASSERT_FALSE(x) ASSERT(!(x))
#define ASSERT_NULL(x) ASSERT((x) == NULL)
#define ASSERT_NOT_NULL(x) ASSERT((x) != NULL)

/* ========================================================================
 * Block/Tx Construction Helpers
 * ======================================================================== */

/**
 * Create a coinbase transaction with a unique seed so each block's coinbase
 * produces a distinct TXID and a verifiable UTXO.
 *
 * @param tx       Output: initialized transaction (caller must call tx_free())
 * @param seed     Unique byte to differentiate coinbase outputs (use block height
 *                 or a unique counter so TXIDs don't collide)
 */
static void make_coinbase_tx(tx_t *tx, uint8_t seed) {
    tx_init(tx);
    tx->version = 1;
    tx->locktime = 0;

    /* Coinbase input: null outpoint, vout = 0xFFFFFFFF */
    tx->input_count = 1;
    tx->inputs = malloc(sizeof(tx_input_t));
    if (tx->inputs == NULL) return;

    memset(tx->inputs[0].prevout.txid.bytes, 0, 32);
    tx->inputs[0].prevout.vout = 0xFFFFFFFF;
    tx->inputs[0].sequence = 0xFFFFFFFF;

    /* Seed the script_sig so each coinbase has a unique hash */
    tx->inputs[0].script_sig_len = 8;
    tx->inputs[0].script_sig = malloc(8);
    if (tx->inputs[0].script_sig == NULL) return;
    memset(tx->inputs[0].script_sig, seed, 8);
    /* Also embed seed into bytes 4-7 for extra uniqueness */
    tx->inputs[0].script_sig[4] = (uint8_t)(seed ^ 0xAA);
    tx->inputs[0].script_sig[5] = (uint8_t)(seed ^ 0x55);
    tx->inputs[0].script_sig[6] = (uint8_t)(seed + 1);
    tx->inputs[0].script_sig[7] = (uint8_t)(seed + 2);

    tx->inputs[0].witness.count = 0;
    tx->inputs[0].witness.items = NULL;

    /* One output so there's a checkable UTXO */
    tx->output_count = 1;
    tx->outputs = malloc(sizeof(tx_output_t));
    if (tx->outputs == NULL) return;

    tx->outputs[0].value = 5000000000LL; /* 50 BTC */
    tx->outputs[0].script_pubkey_len = 25;
    tx->outputs[0].script_pubkey = malloc(25);
    if (tx->outputs[0].script_pubkey == NULL) return;

    /* P2PKH template — destination byte derived from seed */
    tx->outputs[0].script_pubkey[0] = 0x76; /* OP_DUP */
    tx->outputs[0].script_pubkey[1] = 0xa9; /* OP_HASH160 */
    tx->outputs[0].script_pubkey[2] = 0x14; /* Push 20 bytes */
    memset(tx->outputs[0].script_pubkey + 3, seed, 20);
    tx->outputs[0].script_pubkey[23] = 0x88; /* OP_EQUALVERIFY */
    tx->outputs[0].script_pubkey[24] = 0xac; /* OP_CHECKSIG */
}

/**
 * Build a minimal block header that links to prev_hash.
 *
 * @param header    Output: populated block_header_t
 * @param prev_hash Previous block hash (NULL for genesis — uses all-zeros)
 * @param bits      Compact target. Lower bits = more work = winning chain.
 * @param nonce     Unique nonce to distinguish headers with identical other fields
 */
static void make_block_header(block_header_t *header,
                              const hash256_t *prev_hash,
                              uint32_t bits,
                              uint32_t nonce) {
    header->version = 1;
    if (prev_hash != NULL) {
        header->prev_hash = *prev_hash;
    } else {
        memset(&header->prev_hash, 0, 32);
    }
    memset(&header->merkle_root, 0, 32); /* Not validated in these tests */
    header->timestamp = 1231006505 + nonce; /* Unique per block */
    header->bits = bits;
    header->nonce = nonce;
}

/* ========================================================================
 * Callback Support for chain_reorganize
 * ======================================================================== */

/**
 * Entry in the callback's block -> txs lookup table.
 * The callback searches for the right entry by matching block_hash.
 */
typedef struct {
    hash256_t block_hash;
    const tx_t *txs;
    size_t tx_count;
} block_tx_entry_t;

/**
 * Context passed to the get_block_txs callback.
 */
typedef struct {
    block_tx_entry_t *entries;
    size_t count;
} block_tx_map_t;

/**
 * get_block_txs callback: looks up the block hash in the map and returns txs.
 *
 * This is passed to chain_reorganize() to provide transaction data for each
 * block being connected (applied) during the reorg's connect phase.
 */
static echo_result_t get_block_txs_cb(const hash256_t *block_hash,
                                      const tx_t **txs_out,
                                      size_t *tx_count_out,
                                      void *user_data) {
    const block_tx_map_t *map = (const block_tx_map_t *)user_data;

    for (size_t i = 0; i < map->count; i++) {
        if (memcmp(map->entries[i].block_hash.bytes, block_hash->bytes, 32) == 0) {
            *txs_out = map->entries[i].txs;
            *tx_count_out = map->entries[i].tx_count;
            return ECHO_OK;
        }
    }

    /* Block not found in map — test setup error */
    *txs_out = NULL;
    *tx_count_out = 0;
    return ECHO_ERR_NOT_FOUND;
}

/* ========================================================================
 * Test 1: Simple 2-block fork reorg
 *
 * Chain structure:
 *   genesis -> A1 -> A2 -> A3  (current, 3 blocks past genesis)
 *              |
 *              +---> B2 -> B3 -> B4  (winning, 4 blocks past genesis, more work)
 *
 * fork point = A1 (height 1)
 * disconnect: A3, A2 (heights 3, 2)
 * connect: B2, B3, B4 (heights 2, 3, 4)
 *
 * Expected after reorg:
 *   - UTXOs from A2, A3 gone
 *   - UTXOs from B2, B3, B4 present
 *   - UTXO from genesis and A1 still present (common ancestor chain)
 *   - chainwork at tip = B4's accumulated work (not A3's)
 *   - chainstate_get_block_at_height(2) returns B2's hash, not A2's
 * ======================================================================== */

static void test_simple_fork_reorg(void) {
    /*
     * Use lower bits on chain B to give it more work per block.
     *   Chain A bits: 0x1d00ffff (standard genesis difficulty)
     *   Chain B bits: 0x1c00ffff (harder, more work per block)
     *
     * With 4 chain-B blocks vs 3 chain-A blocks (same base), chain B wins.
     */
    const uint32_t BITS_A = 0x1d00ffff;
    const uint32_t BITS_B = 0x1c00ffff; /* Harder => more work */

    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    /* ---- Build shared base: genesis and A1 ---- */

    /* genesis (height 0) */
    block_header_t genesis_hdr;
    make_block_header(&genesis_hdr, NULL, BITS_A, 1);

    tx_t genesis_cb;
    make_coinbase_tx(&genesis_cb, 0x10);
    ASSERT_NOT_NULL(genesis_cb.inputs);

    block_delta_t *delta_genesis = NULL;
    echo_result_t r = chainstate_apply_block(state, &genesis_hdr,
                                             &genesis_cb, 1, &delta_genesis);
    ASSERT_EQ(r, ECHO_OK);
    ASSERT_NOT_NULL(delta_genesis);

    /* Compute genesis TXID so we can verify it persists after reorg */
    hash256_t genesis_txid;
    tx_compute_txid(&genesis_cb, &genesis_txid);

    /* Compute genesis block hash for linking */
    hash256_t genesis_hash;
    block_header_hash(&genesis_hdr, &genesis_hash);

    /* A1 (height 1) — shared with chain B */
    block_header_t a1_hdr;
    make_block_header(&a1_hdr, &genesis_hash, BITS_A, 2);

    tx_t a1_cb;
    make_coinbase_tx(&a1_cb, 0x11);
    ASSERT_NOT_NULL(a1_cb.inputs);

    block_delta_t *delta_a1 = NULL;
    r = chainstate_apply_block(state, &a1_hdr, &a1_cb, 1, &delta_a1);
    ASSERT_EQ(r, ECHO_OK);

    hash256_t a1_txid;
    tx_compute_txid(&a1_cb, &a1_txid);

    hash256_t a1_hash;
    block_header_hash(&a1_hdr, &a1_hash);

    /* ---- Build chain A: A2, A3 (heights 2, 3) ---- */

    block_header_t a2_hdr;
    make_block_header(&a2_hdr, &a1_hash, BITS_A, 3);

    tx_t a2_cb;
    make_coinbase_tx(&a2_cb, 0x21);
    ASSERT_NOT_NULL(a2_cb.inputs);

    block_delta_t *delta_a2 = NULL;
    r = chainstate_apply_block(state, &a2_hdr, &a2_cb, 1, &delta_a2);
    ASSERT_EQ(r, ECHO_OK);

    hash256_t a2_txid;
    tx_compute_txid(&a2_cb, &a2_txid);
    hash256_t a2_hash;
    block_header_hash(&a2_hdr, &a2_hash);

    block_header_t a3_hdr;
    make_block_header(&a3_hdr, &a2_hash, BITS_A, 4);

    tx_t a3_cb;
    make_coinbase_tx(&a3_cb, 0x22);
    ASSERT_NOT_NULL(a3_cb.inputs);

    block_delta_t *delta_a3 = NULL;
    r = chainstate_apply_block(state, &a3_hdr, &a3_cb, 1, &delta_a3);
    ASSERT_EQ(r, ECHO_OK);

    hash256_t a3_txid;
    tx_compute_txid(&a3_cb, &a3_txid);
    hash256_t a3_hash;
    block_header_hash(&a3_hdr, &a3_hash);

    /* Verify pre-reorg state: 4 UTXOs (genesis, A1, A2, A3 coinbases) */
    ASSERT_EQ(chainstate_get_height(state), 3);
    ASSERT_EQ(utxo_set_size(chainstate_get_utxo_set(state)), 4);

    /* ---- Build block index structures for all blocks ----
     *
     * chain_reorg_create() needs block_index_t* nodes with correct prev
     * pointers and heights so it can walk back to the common ancestor.
     *
     * We create a small index map to hold them.
     */
    block_index_map_t *bim = block_index_map_create(16);
    ASSERT_NOT_NULL(bim);

    block_index_t *idx_genesis = block_index_create(&genesis_hdr, NULL);
    ASSERT_NOT_NULL(idx_genesis);
    block_index_map_insert(bim, idx_genesis);

    block_index_t *idx_a1 = block_index_create(&a1_hdr, idx_genesis);
    ASSERT_NOT_NULL(idx_a1);
    block_index_map_insert(bim, idx_a1);

    block_index_t *idx_a2 = block_index_create(&a2_hdr, idx_a1);
    ASSERT_NOT_NULL(idx_a2);
    block_index_map_insert(bim, idx_a2);

    block_index_t *idx_a3 = block_index_create(&a3_hdr, idx_a1); /* chain A tip */
    /*
     * NOTE: idx_a3 prev points to idx_a1 because A3 is built on A2's hash,
     * but the block_index_create uses &a3_hdr.prev_hash = a2_hash to compute
     * the link. We need it to point to idx_a2 for ancestor walking to work.
     */
    block_index_destroy(idx_a3); /* discard the one with wrong prev */
    idx_a3 = block_index_create(&a3_hdr, idx_a2);
    ASSERT_NOT_NULL(idx_a3);
    block_index_map_insert(bim, idx_a3);

    /* Chain B blocks: B2, B3, B4 forking from A1 with harder difficulty */
    block_header_t b2_hdr;
    make_block_header(&b2_hdr, &a1_hash, BITS_B, 100);

    tx_t b2_cb;
    make_coinbase_tx(&b2_cb, 0x31);
    ASSERT_NOT_NULL(b2_cb.inputs);

    hash256_t b2_hash;
    block_header_hash(&b2_hdr, &b2_hash);

    block_index_t *idx_b2 = block_index_create(&b2_hdr, idx_a1);
    ASSERT_NOT_NULL(idx_b2);
    block_index_map_insert(bim, idx_b2);

    block_header_t b3_hdr;
    make_block_header(&b3_hdr, &b2_hash, BITS_B, 101);

    tx_t b3_cb;
    make_coinbase_tx(&b3_cb, 0x32);
    ASSERT_NOT_NULL(b3_cb.inputs);

    hash256_t b3_hash;
    block_header_hash(&b3_hdr, &b3_hash);

    block_index_t *idx_b3 = block_index_create(&b3_hdr, idx_b2);
    ASSERT_NOT_NULL(idx_b3);
    block_index_map_insert(bim, idx_b3);

    block_header_t b4_hdr;
    make_block_header(&b4_hdr, &b3_hash, BITS_B, 102);

    tx_t b4_cb;
    make_coinbase_tx(&b4_cb, 0x33);
    ASSERT_NOT_NULL(b4_cb.inputs);

    hash256_t b4_hash;
    block_header_hash(&b4_hdr, &b4_hash);

    hash256_t b2_txid; tx_compute_txid(&b2_cb, &b2_txid);
    hash256_t b3_txid; tx_compute_txid(&b3_cb, &b3_txid);
    hash256_t b4_txid; tx_compute_txid(&b4_cb, &b4_txid);

    block_index_t *idx_b4 = block_index_create(&b4_hdr, idx_b3);
    ASSERT_NOT_NULL(idx_b4);
    block_index_map_insert(bim, idx_b4);

    /* Set tip index to A3 (current chain tip before reorg) */
    chainstate_set_tip_index(state, idx_a3);

    /* Build reorg plan: from A3 to B4 */
    chain_reorg_t *reorg = chain_reorg_create(idx_a3, idx_b4);
    ASSERT_NOT_NULL(reorg);

    /*
     * The reorg should disconnect [A3, A2] and connect [B2, B3, B4].
     * Common ancestor = A1 (height 1).
     */
    ASSERT_EQ(reorg->disconnect_count, 2);
    ASSERT_EQ(reorg->connect_count, 3);
    ASSERT_EQ(reorg->ancestor->height, 1);

    /* Build tx callback map: chain_reorganize needs txs for B2, B3, B4 */
    tx_t b_txs[3][1] = { {b2_cb}, {b3_cb}, {b4_cb} };
    block_tx_entry_t entries[3];

    memcpy(entries[0].block_hash.bytes, b2_hash.bytes, 32);
    entries[0].txs = b_txs[0];
    entries[0].tx_count = 1;

    memcpy(entries[1].block_hash.bytes, b3_hash.bytes, 32);
    entries[1].txs = b_txs[1];
    entries[1].tx_count = 1;

    memcpy(entries[2].block_hash.bytes, b4_hash.bytes, 32);
    entries[2].txs = b_txs[2];
    entries[2].tx_count = 1;

    block_tx_map_t tx_map = { entries, 3 };

    /* Execute the reorg */
    echo_result_t reorg_result = chain_reorganize(state, reorg, get_block_txs_cb, &tx_map);
    ASSERT_EQ(reorg_result, ECHO_OK);

    /* ---- Verify post-reorg state ---- */

    /* Height should be 4 (genesis + A1 + B2 + B3 + B4) */
    ASSERT_EQ(chainstate_get_height(state), 4);

    /* UTXO count: genesis_cb + a1_cb + b2_cb + b3_cb + b4_cb = 5 */
    ASSERT_EQ(utxo_set_size(chainstate_get_utxo_set(state)), 5);

    /* A2 and A3 coinbase UTXOs must be gone */
    outpoint_t a2_op; a2_op.txid = a2_txid; a2_op.vout = 0;
    ASSERT_NULL(chainstate_lookup_utxo(state, &a2_op));

    outpoint_t a3_op; a3_op.txid = a3_txid; a3_op.vout = 0;
    ASSERT_NULL(chainstate_lookup_utxo(state, &a3_op));

    /* B2, B3, B4 coinbase UTXOs must be present */
    outpoint_t b2_op; b2_op.txid = b2_txid; b2_op.vout = 0;
    ASSERT_NOT_NULL(chainstate_lookup_utxo(state, &b2_op));

    outpoint_t b3_op; b3_op.txid = b3_txid; b3_op.vout = 0;
    ASSERT_NOT_NULL(chainstate_lookup_utxo(state, &b3_op));

    outpoint_t b4_op; b4_op.txid = b4_txid; b4_op.vout = 0;
    ASSERT_NOT_NULL(chainstate_lookup_utxo(state, &b4_op));

    /* genesis and A1 UTXOs must still be present (common ancestor chain) */
    outpoint_t genesis_op; genesis_op.txid = genesis_txid; genesis_op.vout = 0;
    ASSERT_NOT_NULL(chainstate_lookup_utxo(state, &genesis_op));

    outpoint_t a1_op; a1_op.txid = a1_txid; a1_op.vout = 0;
    ASSERT_NOT_NULL(chainstate_lookup_utxo(state, &a1_op));

    /* chainwork at tip should be non-zero and should reflect chain B */
    chain_tip_t tip;
    chainstate_get_tip(state, &tip);
    ASSERT_FALSE(work256_is_zero(&tip.chainwork));

    /* The chainwork at B4 tip should be greater than what A3 had */
    /* (Since chain B has harder difficulty, even with same block count) */
    /* We verify that the tip hash matches B4 */
    ASSERT_EQ(memcmp(tip.hash.bytes, b4_hash.bytes, 32), 0);

    /* chainstate_get_block_at_height(2) should return B2's hash, not A2's */
    hash256_t height2_hash;
    r = chainstate_get_block_at_height(state, 2, &height2_hash);
    ASSERT_EQ(r, ECHO_OK);
    ASSERT_EQ(memcmp(height2_hash.bytes, b2_hash.bytes, 32), 0);
    ASSERT_NE(memcmp(height2_hash.bytes, a2_hash.bytes, 32), 0);

    /* Cleanup */
    chain_reorg_destroy(reorg);
    block_index_map_destroy(bim);
    tx_free(&genesis_cb);
    tx_free(&a1_cb);
    tx_free(&a2_cb);
    tx_free(&a3_cb);
    tx_free(&b2_cb);
    tx_free(&b3_cb);
    tx_free(&b4_cb);
    chainstate_destroy(state);
}

/* ========================================================================
 * Test 2: Deep 6-block reorg
 *
 * Chain A: genesis -> A1 -> A2 -> A3 -> A4 -> A5 -> A6  (6 blocks, current)
 * Chain B: genesis -> B1 -> B2 -> B3 -> B4 -> B5 -> B6 -> B7  (7 blocks, wins)
 *
 * fork point = genesis (height 0)
 * disconnect: A6, A5, A4, A3, A2, A1 (6 blocks)
 * connect: B1..B7 (7 blocks)
 *
 * Expected after reorg:
 *   - All 6 chain-A block UTXOs removed
 *   - All 7 chain-B block UTXOs present
 *   - genesis UTXO still present
 *   - chainwork reflects chain B (7 blocks with standard difficulty)
 * ======================================================================== */

static void test_deep_reorg(void) {
    const uint32_t BITS = 0x1d00ffff;

    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    block_index_map_t *bim = block_index_map_create(32);
    ASSERT_NOT_NULL(bim);

    /* ---- genesis ---- */
    block_header_t genesis_hdr;
    make_block_header(&genesis_hdr, NULL, BITS, 1);

    tx_t genesis_cb;
    make_coinbase_tx(&genesis_cb, 0x01);
    ASSERT_NOT_NULL(genesis_cb.inputs);

    block_delta_t *delta_genesis = NULL;
    echo_result_t r = chainstate_apply_block(state, &genesis_hdr,
                                             &genesis_cb, 1, &delta_genesis);
    ASSERT_EQ(r, ECHO_OK);

    hash256_t genesis_txid;
    tx_compute_txid(&genesis_cb, &genesis_txid);

    hash256_t genesis_hash;
    block_header_hash(&genesis_hdr, &genesis_hash);

    block_index_t *idx_genesis = block_index_create(&genesis_hdr, NULL);
    ASSERT_NOT_NULL(idx_genesis);
    block_index_map_insert(bim, idx_genesis);

    /* ---- Chain A: 6 blocks ---- */
    block_header_t a_hdrs[6];
    tx_t a_cbs[6];
    hash256_t a_hashes[6];
    hash256_t a_txids[6];
    block_index_t *idx_a[6];

    for (int i = 0; i < 6; i++) {
        const hash256_t *prev = (i == 0) ? &genesis_hash : &a_hashes[i - 1];
        make_block_header(&a_hdrs[i], prev, BITS, (uint32_t)(100 + i));
        make_coinbase_tx(&a_cbs[i], (uint8_t)(0x10 + i));
        ASSERT_NOT_NULL(a_cbs[i].inputs);

        block_delta_t *d = NULL;
        r = chainstate_apply_block(state, &a_hdrs[i], &a_cbs[i], 1, &d);
        ASSERT_EQ(r, ECHO_OK);

        block_header_hash(&a_hdrs[i], &a_hashes[i]);
        tx_compute_txid(&a_cbs[i], &a_txids[i]);

        block_index_t *prev_idx = (i == 0) ? idx_genesis : idx_a[i - 1];
        idx_a[i] = block_index_create(&a_hdrs[i], prev_idx);
        ASSERT_NOT_NULL(idx_a[i]);
        block_index_map_insert(bim, idx_a[i]);
    }

    /* Verify pre-reorg: 7 UTXOs (genesis + 6 chain-A blocks) */
    ASSERT_EQ(chainstate_get_height(state), 6);
    ASSERT_EQ(utxo_set_size(chainstate_get_utxo_set(state)), 7);

    /* Set current tip index */
    chainstate_set_tip_index(state, idx_a[5]);

    /* ---- Build chain B: 7 blocks from genesis ---- */
    block_header_t b_hdrs[7];
    tx_t b_cbs[7];
    hash256_t b_hashes[7];
    hash256_t b_txids[7];
    block_index_t *idx_b[7];

    for (int i = 0; i < 7; i++) {
        const hash256_t *prev = (i == 0) ? &genesis_hash : &b_hashes[i - 1];
        make_block_header(&b_hdrs[i], prev, BITS, (uint32_t)(200 + i));
        make_coinbase_tx(&b_cbs[i], (uint8_t)(0x20 + i));
        ASSERT_NOT_NULL(b_cbs[i].inputs);

        block_header_hash(&b_hdrs[i], &b_hashes[i]);
        tx_compute_txid(&b_cbs[i], &b_txids[i]);

        block_index_t *prev_idx = (i == 0) ? idx_genesis : idx_b[i - 1];
        idx_b[i] = block_index_create(&b_hdrs[i], prev_idx);
        ASSERT_NOT_NULL(idx_b[i]);
        block_index_map_insert(bim, idx_b[i]);
    }

    /* Build reorg plan: from A6 (idx_a[5]) to B7 (idx_b[6]) */
    chain_reorg_t *reorg = chain_reorg_create(idx_a[5], idx_b[6]);
    ASSERT_NOT_NULL(reorg);

    /* Should disconnect all 6 A blocks, connect all 7 B blocks */
    ASSERT_EQ(reorg->disconnect_count, 6);
    ASSERT_EQ(reorg->connect_count, 7);
    ASSERT_EQ(reorg->ancestor->height, 0); /* Common ancestor is genesis */

    /* Build tx callback map for chain B blocks */
    block_tx_entry_t entries[7];
    for (int i = 0; i < 7; i++) {
        memcpy(entries[i].block_hash.bytes, b_hashes[i].bytes, 32);
        entries[i].txs = &b_cbs[i];
        entries[i].tx_count = 1;
    }
    block_tx_map_t tx_map = { entries, 7 };

    /* Execute reorg */
    echo_result_t reorg_result = chain_reorganize(state, reorg, get_block_txs_cb, &tx_map);
    ASSERT_EQ(reorg_result, ECHO_OK);

    /* ---- Verify post-reorg ---- */

    /* Height = 7 (genesis + 7 chain-B blocks) */
    ASSERT_EQ(chainstate_get_height(state), 7);

    /* UTXO count = 1 (genesis) + 7 (chain B) = 8 */
    ASSERT_EQ(utxo_set_size(chainstate_get_utxo_set(state)), 8);

    /* All chain-A UTXOs must be gone */
    for (int i = 0; i < 6; i++) {
        outpoint_t op; op.txid = a_txids[i]; op.vout = 0;
        ASSERT_NULL(chainstate_lookup_utxo(state, &op));
    }

    /* All chain-B UTXOs must be present */
    for (int i = 0; i < 7; i++) {
        outpoint_t op; op.txid = b_txids[i]; op.vout = 0;
        ASSERT_NOT_NULL(chainstate_lookup_utxo(state, &op));
    }

    /* genesis UTXO still present */
    outpoint_t genesis_op; genesis_op.txid = genesis_txid; genesis_op.vout = 0;
    ASSERT_NOT_NULL(chainstate_lookup_utxo(state, &genesis_op));

    /* chainwork at tip reflects chain B */
    chain_tip_t tip;
    chainstate_get_tip(state, &tip);
    ASSERT_FALSE(work256_is_zero(&tip.chainwork));
    ASSERT_EQ(memcmp(tip.hash.bytes, b_hashes[6].bytes, 32), 0);

    /* Cleanup */
    chain_reorg_destroy(reorg);
    block_index_map_destroy(bim);
    tx_free(&genesis_cb);
    for (int i = 0; i < 6; i++) tx_free(&a_cbs[i]);
    for (int i = 0; i < 7; i++) tx_free(&b_cbs[i]);
    chainstate_destroy(state);
}

/* ========================================================================
 * Test 3: Same-work competing chains — no reorg
 *
 * Chain A: genesis -> A1 -> A2  (current, 2 blocks at standard difficulty)
 * Chain B: genesis -> B1 -> B2  (same total work as A — equal work)
 *
 * Expected:
 *   - chainstate_should_reorg() returns false (equal work = keep current)
 *   - No chain_reorganize() call
 *   - Original chain A UTXOs still present
 *   - chain B UTXOs absent (chain B was never applied)
 * ======================================================================== */

static void test_same_work_no_reorg(void) {
    const uint32_t BITS = 0x1d00ffff;

    chainstate_t *state = chainstate_create();
    ASSERT_NOT_NULL(state);

    block_index_map_t *bim = block_index_map_create(8);
    ASSERT_NOT_NULL(bim);

    /* ---- genesis ---- */
    block_header_t genesis_hdr;
    make_block_header(&genesis_hdr, NULL, BITS, 1);

    tx_t genesis_cb;
    make_coinbase_tx(&genesis_cb, 0x01);
    ASSERT_NOT_NULL(genesis_cb.inputs);

    chainstate_apply_block(state, &genesis_hdr, &genesis_cb, 1, NULL);

    hash256_t genesis_hash;
    block_header_hash(&genesis_hdr, &genesis_hash);

    block_index_t *idx_genesis = block_index_create(&genesis_hdr, NULL);
    ASSERT_NOT_NULL(idx_genesis);
    block_index_map_insert(bim, idx_genesis);

    /* ---- Chain A: 2 blocks ---- */
    block_header_t a1_hdr;
    make_block_header(&a1_hdr, &genesis_hash, BITS, 10);

    tx_t a1_cb;
    make_coinbase_tx(&a1_cb, 0x11);
    ASSERT_NOT_NULL(a1_cb.inputs);

    chainstate_apply_block(state, &a1_hdr, &a1_cb, 1, NULL);

    hash256_t a1_txid;
    tx_compute_txid(&a1_cb, &a1_txid);

    hash256_t a1_hash;
    block_header_hash(&a1_hdr, &a1_hash);

    block_index_t *idx_a1 = block_index_create(&a1_hdr, idx_genesis);
    ASSERT_NOT_NULL(idx_a1);
    block_index_map_insert(bim, idx_a1);

    block_header_t a2_hdr;
    make_block_header(&a2_hdr, &a1_hash, BITS, 11);

    tx_t a2_cb;
    make_coinbase_tx(&a2_cb, 0x12);
    ASSERT_NOT_NULL(a2_cb.inputs);

    chainstate_apply_block(state, &a2_hdr, &a2_cb, 1, NULL);

    hash256_t a2_txid;
    tx_compute_txid(&a2_cb, &a2_txid);

    block_index_t *idx_a2 = block_index_create(&a2_hdr, idx_a1);
    ASSERT_NOT_NULL(idx_a2);
    block_index_map_insert(bim, idx_a2);

    /* Set tip to A2 */
    chainstate_set_tip_index(state, idx_a2);

    /* Verify chain A state: 3 UTXOs */
    ASSERT_EQ(chainstate_get_height(state), 2);
    ASSERT_EQ(utxo_set_size(chainstate_get_utxo_set(state)), 3);

    /* ---- Build chain B: 2 blocks from genesis with same bits ---- */
    block_header_t b1_hdr;
    make_block_header(&b1_hdr, &genesis_hash, BITS, 20); /* different nonce => different hash */

    tx_t b1_cb;
    make_coinbase_tx(&b1_cb, 0x21);
    ASSERT_NOT_NULL(b1_cb.inputs);

    hash256_t b1_txid;
    tx_compute_txid(&b1_cb, &b1_txid);

    hash256_t b1_hash;
    block_header_hash(&b1_hdr, &b1_hash);

    block_index_t *idx_b1 = block_index_create(&b1_hdr, idx_genesis);
    ASSERT_NOT_NULL(idx_b1);
    block_index_map_insert(bim, idx_b1);

    block_header_t b2_hdr;
    make_block_header(&b2_hdr, &b1_hash, BITS, 21);

    tx_t b2_cb;
    make_coinbase_tx(&b2_cb, 0x22);
    ASSERT_NOT_NULL(b2_cb.inputs);

    hash256_t b2_txid;
    tx_compute_txid(&b2_cb, &b2_txid);

    block_index_t *idx_b2 = block_index_create(&b2_hdr, idx_b1);
    ASSERT_NOT_NULL(idx_b2);
    block_index_map_insert(bim, idx_b2);

    /*
     * chain_compare uses work256 comparison. Both chains have 2 blocks at
     * BITS = 0x1d00ffff from genesis, so their accumulated chainwork is
     * identical. chainstate_should_reorg checks if new_index has MORE work
     * than current tip — equal work returns false.
     */
    ASSERT_FALSE(chainstate_should_reorg(state, idx_b2));

    /*
     * No reorg: chain A state must be unchanged.
     */
    ASSERT_EQ(chainstate_get_height(state), 2);
    ASSERT_EQ(utxo_set_size(chainstate_get_utxo_set(state)), 3);

    /* Chain A UTXOs still present */
    outpoint_t a1_op; a1_op.txid = a1_txid; a1_op.vout = 0;
    ASSERT_NOT_NULL(chainstate_lookup_utxo(state, &a1_op));

    outpoint_t a2_op; a2_op.txid = a2_txid; a2_op.vout = 0;
    ASSERT_NOT_NULL(chainstate_lookup_utxo(state, &a2_op));

    /* Chain B UTXOs absent (never applied) */
    outpoint_t b1_op; b1_op.txid = b1_txid; b1_op.vout = 0;
    ASSERT_NULL(chainstate_lookup_utxo(state, &b1_op));

    outpoint_t b2_op; b2_op.txid = b2_txid; b2_op.vout = 0;
    ASSERT_NULL(chainstate_lookup_utxo(state, &b2_op));

    /* Tip hash still points to A2 */
    chain_tip_t tip;
    chainstate_get_tip(state, &tip);
    hash256_t a2_hash;
    block_header_hash(&a2_hdr, &a2_hash);
    ASSERT_EQ(memcmp(tip.hash.bytes, a2_hash.bytes, 32), 0);

    /* Cleanup */
    block_index_map_destroy(bim);
    tx_free(&genesis_cb);
    tx_free(&a1_cb);
    tx_free(&a2_cb);
    tx_free(&b1_cb);
    tx_free(&b2_cb);
    chainstate_destroy(state);
}

/* ========================================================================
 * Main Test Runner
 * ======================================================================== */

int main(void) {
    test_suite_begin("Chain Reorganization Tests");

    test_case("Simple 2-block fork reorg: UTXOs and chainwork");
    test_simple_fork_reorg();
    test_pass();

    test_case("Deep 6-block reorg: all orphaned UTXOs reverted");
    test_deep_reorg();
    test_pass();

    test_case("Same-work competing chains: no reorg triggered");
    test_same_work_no_reorg();
    test_pass();

    test_suite_end();
    return test_global_summary();
}
