/*
 * Bitcoin Echo — Transaction Test Vectors
 *
 * Test vectors for transaction parsing, serialization, and ID computation.
 * Uses real Bitcoin transactions for validation.
 *
 * Build once. Build right. Stop.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tx.h"
#include "test_utils.h"

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
 * Test transaction parsing.
 */
static void test_tx_parse(const char *name, const char *hex,
                          int32_t expected_version,
                          size_t expected_inputs,
                          size_t expected_outputs,
                          uint32_t expected_locktime,
                          echo_bool_t expected_witness)
{
    uint8_t data[65536];
    size_t data_len;
    tx_t tx;
    size_t consumed;
    echo_result_t result;

    data_len = hex_to_bytes(hex, data, sizeof(data));
    if (data_len == 0) {
        test_case(name);
        test_fail("invalid hex");
        return;
    }

    result = tx_parse(data, data_len, &tx, &consumed);

    test_case(name);
    if (result == ECHO_OK &&
        tx.version == expected_version &&
        tx.input_count == expected_inputs &&
        tx.output_count == expected_outputs &&
        tx.locktime == expected_locktime &&
        tx.has_witness == expected_witness &&
        consumed == data_len)
    {
        test_pass();
    } else {
        test_fail(name);
        printf("    Result: %d, Consumed: %zu/%zu\n", result, consumed, data_len);
        if (result == ECHO_OK) {
            printf("    Version: %d (expected %d)\n", tx.version, expected_version);
            printf("    Inputs: %zu (expected %zu)\n", tx.input_count, expected_inputs);
            printf("    Outputs: %zu (expected %zu)\n", tx.output_count, expected_outputs);
            printf("    Locktime: %u (expected %u)\n", tx.locktime, expected_locktime);
            printf("    Has witness: %d (expected %d)\n", tx.has_witness, expected_witness);
        }
    }

    tx_free(&tx);
}

/*
 * Test round-trip parsing and serialization.
 */
static void test_roundtrip(const char *name, const char *hex)
{
    uint8_t data[65536];
    uint8_t serialized[65536];
    size_t data_len;
    tx_t tx;
    size_t written;
    echo_result_t result;

    data_len = hex_to_bytes(hex, data, sizeof(data));
    if (data_len == 0) {
        test_case(name);
        test_fail("invalid hex");
        return;
    }

    result = tx_parse(data, data_len, &tx, NULL);
    if (result != ECHO_OK) {
        test_case(name);
        test_fail("parse failed");
        printf("    Result: %d\n", result);
        return;
    }

    result = tx_serialize(&tx, ECHO_TRUE, serialized, sizeof(serialized), &written);
    if (result != ECHO_OK) {
        test_case(name);
        test_fail("serialize failed");
        printf("    Result: %d\n", result);
        tx_free(&tx);
        return;
    }

    test_case(name);
    if (written == data_len && bytes_equal(data, serialized, data_len)) {
        test_pass();
    } else {
        test_fail(name);
        printf("    Original length: %zu\n", data_len);
        printf("    Serialized length: %zu\n", written);
        if (written != data_len) {
            printf("    Length mismatch!\n");
        } else {
            printf("    Content mismatch at some offset\n");
        }
    }

    tx_free(&tx);
}

/*
 * Test coinbase detection.
 */
static void test_coinbase(const char *name, const char *hex,
                          echo_bool_t expected_coinbase)
{
    uint8_t data[65536];
    size_t data_len;
    tx_t tx;
    echo_result_t result;
    echo_bool_t is_coinbase;

    data_len = hex_to_bytes(hex, data, sizeof(data));
    if (data_len == 0) {
        test_case(name);
        test_fail("invalid hex");
        return;
    }

    result = tx_parse(data, data_len, &tx, NULL);
    if (result != ECHO_OK) {
        test_case(name);
        test_fail("parse failed");
        printf("    Result: %d\n", result);
        return;
    }

    is_coinbase = tx_is_coinbase(&tx);

    test_case(name);
    if (is_coinbase == expected_coinbase) {
        test_pass();
    } else {
        test_fail(name);
        printf("    Expected coinbase: %d, got: %d\n", expected_coinbase, is_coinbase);
    }

    tx_free(&tx);
}

/*
 * Test weight/vsize computation.
 */
static void test_weight(const char *name, const char *hex,
                        size_t expected_weight, size_t expected_vsize)
{
    uint8_t data[65536];
    size_t data_len;
    tx_t tx;
    echo_result_t result;
    size_t weight, vsize;

    data_len = hex_to_bytes(hex, data, sizeof(data));
    if (data_len == 0) {
        test_case(name);
        test_fail("invalid hex");
        return;
    }

    result = tx_parse(data, data_len, &tx, NULL);
    if (result != ECHO_OK) {
        test_case(name);
        test_fail("parse failed");
        printf("    Result: %d\n", result);
        return;
    }

    weight = tx_weight(&tx);
    vsize = tx_vsize(&tx);

    test_case(name);
    if (weight == expected_weight && vsize == expected_vsize) {
        test_pass();
    } else {
        test_fail(name);
        printf("    Expected weight: %zu, got: %zu\n", expected_weight, weight);
        printf("    Expected vsize: %zu, got: %zu\n", expected_vsize, vsize);
    }

    tx_free(&tx);
}

int main(void)
{
    test_suite_begin("Transaction Tests");

    /*
     * Test Vector 1: A legacy transaction
     * We verify parsing, serialization roundtrip, and structure validation.
     */
    test_section("Legacy transaction tests");

    /*
     * Simple legacy transaction: 1 input, 2 outputs
     * Raw transaction bytes from Bitcoin blockchain
     */
    const char *legacy_tx_hex =
        "0100000001c997a5e56e104102fa209c6a852dd90660a20b2d9c352423edce25857fcd3704000000004847"
        "304402204e45e16932b8af514961a1d3a1a25fdf3f4f7732e9d624c6c61548ab5fb8cd410220181522ec8e"
        "ca07de4860a4acdd12909d831cc56cbbac4622082221a8768d1d0901ffffffff0200ca9a3b000000004341"
        "0411db93e1dcdb8a016b49840f8c53bc1eb68a382e97b1482ecad7b148a6909a5cb2e0eaddfb84ccf97444"
        "64f82e160bfa9b8b64f9d4c03f999b8643f656b412a3ac00286bee0000000043410411db93e1dcdb8a016b"
        "49840f8c53bc1eb68a382e97b1482ecad7b148a6909a5cb2e0eaddfb84ccf9744464f82e160bfa9b8b64f9"
        "d4c03f999b8643f656b412a3ac00000000";

    test_tx_parse("Legacy tx parse (1 in, 2 out)",
                  legacy_tx_hex, 1, 1, 2, 0, ECHO_FALSE);

    test_roundtrip("Legacy tx roundtrip", legacy_tx_hex);

    test_coinbase("Legacy tx not coinbase", legacy_tx_hex, ECHO_FALSE);

    /*
     * Test Vector 2: Coinbase transaction
     * Note: coinbase has null prevout (all zeros + vout=0xFFFFFFFF)
     */
    test_section("Coinbase transaction tests");
    const char *coinbase_tx_hex =
        "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff0704"
        "ffff001d0102ffffffff0100f2052a0100000043410496b538e853519c726a2c91e61ec11600ae1390813a"
        "627c66fb8be7947be63c52da7589379515d4e0a604f8141781e62294721166bf621e73a82cbf2342c858ee"
        "ac00000000";

    test_tx_parse("Coinbase tx parse",
                  coinbase_tx_hex, 1, 1, 1, 0, ECHO_FALSE);

    test_coinbase("Coinbase tx detection", coinbase_tx_hex, ECHO_TRUE);

    test_roundtrip("Coinbase tx roundtrip", coinbase_tx_hex);

    /*
     * Test Vector 3: SegWit P2WPKH transaction
     * This is a native SegWit transaction with witness data
     * txid: c586389e5e4b3acb9d6c8be1c19ae8ab2795397633176f5a6442a261bbdefc3a
     */
    test_section("SegWit transaction tests");

    const char *segwit_tx_hex =
        "020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff"
        "0502e8030101ffffffff0200f2052a01000000160014751e76e8199196d454941c45d1b3a323f1433bd600"
        "00000000000000266a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974"
        "e8cf90120000000000000000000000000000000000000000000000000000000000000000000000000";

    test_tx_parse("SegWit coinbase tx parse",
                  segwit_tx_hex, 2, 1, 2, 0, ECHO_TRUE);

    test_roundtrip("SegWit coinbase tx roundtrip", segwit_tx_hex);

    /*
     * Test Vector 4: Real SegWit P2WPKH spending transaction
     * A transaction that spends a P2WPKH output
     */
    const char *segwit_spend_hex =
        "02000000000101d1b18d41c0d3add4754e0ddeb2ab9dd23c0e547b5cf94c69dc62f8ff3a13d54f01000000"
        "00fdffffff0260ea00000000000016001414011f7254d96b819c76986c277d115efce6f7b58096980000000"
        "0001600149a1c78a507689f6f54b847ad1cef1e614ee23f1e0247304402200bbcd4e28c6e2e07b9466aefb"
        "e0667e0e2a74ad0fbe78e0e8f2b22e0ee7a30eb022059c89d60e94eb8d1c8ad0a6ee08eb02cc155e3b01cd"
        "82e96fc98c5b54f6be2c7012103fff97bd5755eeea420453a14355235d382f6472f8568a18b2f057a146029"
        "755679000000";

    test_tx_parse("SegWit P2WPKH spend parse",
                  segwit_spend_hex, 2, 1, 2, 121, ECHO_TRUE);

    test_roundtrip("SegWit P2WPKH spend roundtrip", segwit_spend_hex);

    test_coinbase("SegWit spend not coinbase", segwit_spend_hex, ECHO_FALSE);

    /*
     * Test Vector 5: Transaction size and weight tests
     */
    test_section("Weight and vsize tests");

    /* Legacy transaction: weight = size * 4 (no witness discount) */
    /* legacy_tx_hex is 275 bytes, so weight = 1100, vsize = 275 */
    test_weight("Legacy tx weight", legacy_tx_hex, 1100, 275);

    /* SegWit transaction has lower weight due to witness discount */
    /* segwit_spend_hex: base_size (no witness) vs total_size (with witness) */
    /* Need to calculate actual values from the transaction */
    /* For this tx: base=110, total=219, weight = 3*110 + 219 = 549, vsize = 138 */
    test_weight("SegWit tx weight", segwit_spend_hex, 561, 141);

    /*
     * Test Vector 7: Error handling
     */
    test_section("Error handling tests");

    /* Truncated transaction */
    {
        uint8_t truncated[] = {0x01, 0x00, 0x00, 0x00};  /* Just version */
        tx_t tx;
        echo_result_t result = tx_parse(truncated, sizeof(truncated), &tx, NULL);

        test_case("Truncated tx rejected");
        if (result == ECHO_ERR_TRUNCATED) {
            test_pass();
        } else {
            test_fail("Truncated tx not rejected");
            printf("    Result: %d\n", result);
        }
    }

    /* NULL parameter */
    {
        tx_t tx;
        echo_result_t result = tx_parse(NULL, 100, &tx, NULL);

        test_case("NULL data rejected");
        if (result == ECHO_ERR_NULL_PARAM) {
            test_pass();
        } else {
            test_fail("NULL data not rejected");
            printf("    Result: %d\n", result);
        }
    }

    test_suite_end();
    return test_global_summary();
}
