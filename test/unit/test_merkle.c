/*
 * Bitcoin Echo — Merkle Tree Test Vectors
 *
 * Test vectors for Merkle tree computation, including known
 * Bitcoin block Merkle roots.
 *
 * Build once. Build right. Stop.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "merkle.h"
#include "test_utils.h"
#include "sha256.h"


/*
 * Convert hex string to bytes.
 */
static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t max_len)
{
    size_t len = strlen(hex);
    size_t i;
    unsigned int byte;

    if (len % 2 != 0) return 0;
    if (len / 2 > max_len) return 0;

    for (i = 0; i < len / 2; i++) {
        if (sscanf(hex + i * 2, "%02x", &byte) != 1) return 0;
        out[i] = (uint8_t)byte;
    }

    return len / 2;
}

/*
 * Convert bytes to hex string.
 */
static void bytes_to_hex(const uint8_t *data, size_t len, char *out)
{
    size_t i;
    for (i = 0; i < len; i++) {
        snprintf(out + i * 2, 3, "%02x", data[i]);
    }
    out[len * 2] = '\0';
}

/*
 * Compare hash to expected hex string.
 */
static int hash_equals_hex(const hash256_t *hash, const char *expected_hex)
{
    uint8_t expected[32];
    if (hex_to_bytes(expected_hex, expected, 32) != 32) return 0;
    return memcmp(hash->bytes, expected, 32) == 0;
}

/*
 * Test: Empty tree returns zeros.
 */
static void test_empty_tree(void)
{
    hash256_t root;
    echo_result_t result;
    int is_zero;
    size_t i;


    result = merkle_root(NULL, 0, &root);

    is_zero = 1;
    for (i = 0; i < 32; i++) {
        if (root.bytes[i] != 0) {
            is_zero = 0;
            break;
        }
    }

    if (result == ECHO_OK && is_zero) {
        test_pass();
        test_case("Empty tree returns zeros");
        test_pass();
    } else {
        test_fail("Empty tree returns zeros");
        printf("    Result: %d, Is zero: %d\n", result, is_zero);
    }
}

/*
 * Test: Single hash returns itself.
 */
static void test_single_hash(void)
{
    hash256_t hashes[1];
    hash256_t root;
    echo_result_t result;


    /* Use a known hash */
    hex_to_bytes("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20",
                 hashes[0].bytes, 32);

    result = merkle_root(hashes, 1, &root);

    if (result == ECHO_OK &&
        memcmp(root.bytes, hashes[0].bytes, 32) == 0)
    {
        test_pass();
        test_case("Single hash returns itself");
        test_pass();
    } else {
        test_fail("Single hash returns itself");
    }
}

/*
 * Test: Two hashes are paired.
 *
 * With two leaves A and B, the root is SHA256d(A || B).
 */
static void test_two_hashes(void)
{
    hash256_t hashes[2];
    hash256_t root;
    hash256_t expected;
    uint8_t combined[64];
    echo_result_t result;


    /* Two simple hashes */
    memset(hashes[0].bytes, 0x11, 32);
    memset(hashes[1].bytes, 0x22, 32);

    /* Compute expected root manually */
    memcpy(combined, hashes[0].bytes, 32);
    memcpy(combined + 32, hashes[1].bytes, 32);
    sha256d(combined, 64, expected.bytes);

    result = merkle_root(hashes, 2, &root);

    if (result == ECHO_OK &&
        memcmp(root.bytes, expected.bytes, 32) == 0)
    {
        test_pass();
        test_case("Two hashes paired correctly");
        test_pass();
    } else {
        test_fail("Two hashes paired correctly");
    }
}

/*
 * Test: Odd number of hashes (last is duplicated).
 *
 * With three leaves A, B, C:
 *   Level 1: hash(A,B), hash(C,C)
 *   Root: hash(hash(A,B), hash(C,C))
 */
static void test_odd_count(void)
{
    hash256_t hashes[3];
    hash256_t root;
    hash256_t ab, cc, expected;
    uint8_t combined[64];
    echo_result_t result;


    memset(hashes[0].bytes, 0xAA, 32);
    memset(hashes[1].bytes, 0xBB, 32);
    memset(hashes[2].bytes, 0xCC, 32);

    /* Compute expected manually */
    /* AB = hash(A || B) */
    memcpy(combined, hashes[0].bytes, 32);
    memcpy(combined + 32, hashes[1].bytes, 32);
    sha256d(combined, 64, ab.bytes);

    /* CC = hash(C || C) */
    memcpy(combined, hashes[2].bytes, 32);
    memcpy(combined + 32, hashes[2].bytes, 32);
    sha256d(combined, 64, cc.bytes);

    /* Root = hash(AB || CC) */
    memcpy(combined, ab.bytes, 32);
    memcpy(combined + 32, cc.bytes, 32);
    sha256d(combined, 64, expected.bytes);

    result = merkle_root(hashes, 3, &root);

    if (result == ECHO_OK &&
        memcmp(root.bytes, expected.bytes, 32) == 0)
    {
        test_pass();
        test_case("Odd count duplicates last");
        test_pass();
    } else {
        test_fail("Odd count duplicates last");
    }
}

/*
 * Test: Four hashes (perfect binary tree).
 */
static void test_four_hashes(void)
{
    hash256_t hashes[4];
    hash256_t root;
    hash256_t ab, cd, expected;
    uint8_t combined[64];
    echo_result_t result;
    size_t i;


    for (i = 0; i < 4; i++) {
        memset(hashes[i].bytes, (uint8_t)(i + 1), 32);
    }

    /* AB = hash(A || B) */
    memcpy(combined, hashes[0].bytes, 32);
    memcpy(combined + 32, hashes[1].bytes, 32);
    sha256d(combined, 64, ab.bytes);

    /* CD = hash(C || D) */
    memcpy(combined, hashes[2].bytes, 32);
    memcpy(combined + 32, hashes[3].bytes, 32);
    sha256d(combined, 64, cd.bytes);

    /* Root = hash(AB || CD) */
    memcpy(combined, ab.bytes, 32);
    memcpy(combined + 32, cd.bytes, 32);
    sha256d(combined, 64, expected.bytes);

    result = merkle_root(hashes, 4, &root);

    if (result == ECHO_OK &&
        memcmp(root.bytes, expected.bytes, 32) == 0)
    {
        test_pass();
        test_case("Four hashes (perfect tree)");
        test_pass();
    } else {
        test_fail("Four hashes (perfect tree)");
    }
}

/*
 * Test: Genesis block Merkle root.
 *
 * The genesis block has only one transaction (coinbase).
 * The Merkle root is just the txid of that transaction.
 *
 * Genesis coinbase txid (little-endian):
 * 4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b
 */
static void test_genesis_merkle_root(void)
{
    hash256_t txid;
    hash256_t root;
    echo_result_t result;


    /* Genesis block's single transaction txid (already in internal byte order) */
    hex_to_bytes("3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a",
                 txid.bytes, 32);

    result = merkle_root(&txid, 1, &root);

    if (result == ECHO_OK &&
        memcmp(root.bytes, txid.bytes, 32) == 0)
    {
        test_pass();
        test_case("Genesis block Merkle root");
        test_pass();
    } else {
        test_fail("Genesis block Merkle root");
    }
}

/*
 * Test: Block 170 Merkle root (first non-coinbase transaction).
 *
 * Block 170 contains 2 transactions. We verify the Merkle root
 * computation matches the known value from the block.
 *
 * txid 1 (coinbase): b1fea52486ce0c62bb442b530a3f0132b826c74e473d1f2c220bfa78111c5082
 * txid 2: f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16
 * Merkle root: 7dac2c5666815c17a3b36427de37bb9d2e2c5ccec3f8633eb91a4205cb4c10ff
 */
static void test_block_170_merkle_root(void)
{
    hash256_t txids[2];
    hash256_t root;
    echo_result_t result;
    char root_hex[65];


    /* txid 1 (coinbase) in internal byte order */
    hex_to_bytes("82501c1178fa0b222c1f3d474ec726b832013f0a532b44bb620cce8624a5feb1",
                 txids[0].bytes, 32);

    /* txid 2 in internal byte order */
    hex_to_bytes("169e1e83e930853391bc6f35f605c6754cfead57cf8387639d3b4096c54f18f4",
                 txids[1].bytes, 32);

    result = merkle_root(txids, 2, &root);

    /* Expected Merkle root in internal byte order */
    bytes_to_hex(root.bytes, 32, root_hex);

    if (result == ECHO_OK &&
        hash_equals_hex(&root, "ff104ccb05421ab93e63f8c3ce5c2c2e9dbb37de2764b3a3175c8166562cac7d"))
    {
        test_pass();
        test_case("Block 170 Merkle root");
        test_pass();
    } else {
        test_fail("Block 170 Merkle root");
        printf("    Got: %s\n", root_hex);
    }
}

/*
 * Test: Merkle proof generation for single element.
 */
static void test_merkle_proof_single(void)
{
    hash256_t hashes[1];
    hash256_t proof[10];
    size_t proof_len;
    echo_result_t result;


    memset(hashes[0].bytes, 0x42, 32);

    result = merkle_proof(hashes, 1, 0, proof, &proof_len, 10);

    if (result == ECHO_OK && proof_len == 0) {
        test_pass();
        test_case("Merkle proof single element (no proof needed)");
        test_pass();
    } else {
        test_fail("Merkle proof single element");
        printf("    Result: %d, proof_len: %zu\n", result, proof_len);
    }
}

/*
 * Test: Merkle proof generation for two elements.
 */
static void test_merkle_proof_two(void)
{
    hash256_t hashes[2];
    hash256_t proof[10];
    hash256_t root;
    size_t proof_len;
    echo_result_t result;
    echo_bool_t verified;


    memset(hashes[0].bytes, 0x11, 32);
    memset(hashes[1].bytes, 0x22, 32);

    /* Compute root */
    result = merkle_root(hashes, 2, &root);
    if (result != ECHO_OK) {
        test_fail("Merkle proof two elements (root computation failed)");
        return;
    }

    /* Generate proof for element 0 */
    result = merkle_proof(hashes, 2, 0, proof, &proof_len, 10);
    if (result != ECHO_OK || proof_len != 1) {
        test_fail("Merkle proof two elements (proof generation failed)");
        return;
    }

    /* Verify proof */
    verified = merkle_verify(&hashes[0], 0, 2, proof, proof_len, &root);

    if (verified == ECHO_TRUE) {
        test_pass();
        test_case("Merkle proof two elements");
        test_pass();
    } else {
        test_fail("Merkle proof two elements (verification failed)");
    }
}

/*
 * Test: Merkle proof generation and verification for larger tree.
 */
static void test_merkle_proof_large(void)
{
    hash256_t hashes[7];
    hash256_t proof[10];
    hash256_t root;
    size_t proof_len;
    echo_result_t result;
    echo_bool_t verified;
    size_t i;


    /* Create 7 hashes */
    for (i = 0; i < 7; i++) {
        memset(hashes[i].bytes, (uint8_t)(i + 1), 32);
    }

    /* Compute root */
    result = merkle_root(hashes, 7, &root);
    if (result != ECHO_OK) {
        test_fail("Merkle proof large tree (root computation failed)");
        return;
    }

    /* Test proof for each element */
    for (i = 0; i < 7; i++) {
        result = merkle_proof(hashes, 7, i, proof, &proof_len, 10);
        if (result != ECHO_OK) {
            printf("  [FAIL] Merkle proof large tree (proof gen failed for %zu)\n", i);
            return;
        }

        verified = merkle_verify(&hashes[i], i, 7, proof, proof_len, &root);
        if (verified != ECHO_TRUE) {
            printf("  [FAIL] Merkle proof large tree (verify failed for %zu)\n", i);
            return;
        }
    }

    test_pass();
    test_case("Merkle proof large tree (7 elements, all verified)");
        test_pass();
}

/*
 * Test: Merkle proof with invalid leaf.
 */
static void test_merkle_proof_invalid(void)
{
    hash256_t hashes[4];
    hash256_t proof[10];
    hash256_t root;
    hash256_t fake_leaf;
    size_t proof_len;
    echo_result_t result;
    echo_bool_t verified;
    size_t i;


    for (i = 0; i < 4; i++) {
        memset(hashes[i].bytes, (uint8_t)(i + 1), 32);
    }

    /* Compute root */
    merkle_root(hashes, 4, &root);

    /* Generate proof for element 2 */
    result = merkle_proof(hashes, 4, 2, proof, &proof_len, 10);
    if (result != ECHO_OK) {
        test_fail("Merkle proof invalid (proof gen failed)");
        return;
    }

    /* Try to verify with a different leaf */
    memset(fake_leaf.bytes, 0xFF, 32);
    verified = merkle_verify(&fake_leaf, 2, 4, proof, proof_len, &root);

    if (verified == ECHO_FALSE) {
        test_pass();
        test_case("Merkle proof rejects invalid leaf");
        test_pass();
    } else {
        test_fail("Merkle proof accepted invalid leaf");
    }
}

/*
 * Test: Witness commitment computation.
 */
static void test_witness_commitment(void)
{
    hash256_t witness_root;
    hash256_t witness_nonce;
    hash256_t commitment;
    hash256_t expected;
    uint8_t combined[64];
    echo_result_t result;


    memset(witness_root.bytes, 0x11, 32);
    memset(witness_nonce.bytes, 0x22, 32);

    /* Compute expected: SHA256d(witness_root || witness_nonce) */
    memcpy(combined, witness_root.bytes, 32);
    memcpy(combined + 32, witness_nonce.bytes, 32);
    sha256d(combined, 64, expected.bytes);

    result = witness_commitment(&witness_root, &witness_nonce, &commitment);

    if (result == ECHO_OK &&
        memcmp(commitment.bytes, expected.bytes, 32) == 0)
    {
        test_pass();
        test_case("Witness commitment");
        test_pass();
    } else {
        test_fail("Witness commitment");
    }
}

/*
 * Test: NULL parameter handling.
 */
static void test_null_params(void)
{
    hash256_t hashes[2];
    hash256_t root;
    echo_result_t result;
    int all_passed = 1;


    memset(hashes[0].bytes, 0, 32);
    memset(hashes[1].bytes, 0, 32);

    /* NULL root */
    result = merkle_root(hashes, 2, NULL);
    if (result != ECHO_ERR_NULL_PARAM) {
        printf("    NULL root not rejected\n");
        all_passed = 0;
    }

    /* NULL hashes with count > 0 */
    result = merkle_root(NULL, 2, &root);
    if (result != ECHO_ERR_NULL_PARAM) {
        printf("    NULL hashes not rejected\n");
        all_passed = 0;
    }

    /* NULL with count 0 should succeed */
    result = merkle_root(NULL, 0, &root);
    if (result != ECHO_OK) {
        printf("    NULL with count 0 failed\n");
        all_passed = 0;
    }

    if (all_passed) {
        test_pass();
        test_case("NULL parameter handling");
        test_pass();
    } else {
        test_fail("NULL parameter handling");
    }
}

/*
 * Test: Proof index out of range.
 */
static void test_proof_out_of_range(void)
{
    hash256_t hashes[3];
    hash256_t proof[10];
    size_t proof_len;
    echo_result_t result;


    memset(hashes, 0, sizeof(hashes));

    result = merkle_proof(hashes, 3, 5, proof, &proof_len, 10);

    if (result == ECHO_ERR_OUT_OF_RANGE) {
        test_pass();
        test_case("Proof index out of range rejected");
        test_pass();
    } else {
        printf("  [FAIL] Proof index out of range not rejected (result: %d)\n", result);
    }
}

/*
 * Test: Large tree (16 elements).
 */
static void test_large_tree(void)
{
    hash256_t hashes[16];
    hash256_t root;
    hash256_t proof[10];
    size_t proof_len;
    echo_result_t result;
    echo_bool_t verified;
    size_t i;


    /* Create 16 unique hashes */
    for (i = 0; i < 16; i++) {
        sha256d((uint8_t *)&i, sizeof(i), hashes[i].bytes);
    }

    /* Compute root */
    result = merkle_root(hashes, 16, &root);
    if (result != ECHO_OK) {
        test_fail("Large tree (16 elements) - root failed");
        return;
    }

    /* Verify proof for element 7 (middle-ish) */
    result = merkle_proof(hashes, 16, 7, proof, &proof_len, 10);
    if (result != ECHO_OK) {
        test_fail("Large tree - proof gen failed");
        return;
    }

    /* Proof should be log2(16) = 4 elements */
    if (proof_len != 4) {
        printf("  [FAIL] Large tree - wrong proof length: %zu (expected 4)\n", proof_len);
        return;
    }

    verified = merkle_verify(&hashes[7], 7, 16, proof, proof_len, &root);

    if (verified == ECHO_TRUE) {
        test_pass();
        test_case("Large tree (16 elements)");
        test_pass();
    } else {
        test_fail("Large tree - verification failed");
    }
}

int main(void)
{
    test_suite_begin("Merkle Tree Tests");

    test_section("Basic Merkle tree tests");
    test_empty_tree();
    test_single_hash();
    test_two_hashes();
    test_odd_count();
    test_four_hashes();

    test_section("Real Bitcoin block tests");
    test_genesis_merkle_root();
    test_block_170_merkle_root();

    test_section("Merkle proof tests");
    test_merkle_proof_single();
    test_merkle_proof_two();
    test_merkle_proof_large();
    test_merkle_proof_invalid();
    test_large_tree();

    test_section("Witness commitment tests");
    test_witness_commitment();

    test_section("Error handling tests");
    test_null_params();
    test_proof_out_of_range();

    test_suite_end();
    return test_global_summary();
}
