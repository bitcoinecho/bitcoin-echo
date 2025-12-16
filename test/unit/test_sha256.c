/*
 * Bitcoin Echo — SHA-256 Test Vectors
 *
 * Test vectors from:
 *   - NIST FIPS 180-4 examples
 *   - NIST CAVP (Cryptographic Algorithm Validation Program)
 *   - Bitcoin-specific test cases
 *
 * Build once. Build right. Stop.
 */

#include <stdint.h>
#include <stdio.h>
#include "sha256.h"
#include "test_utils.h"


/*
 * Run a single SHA-256 test.
 */
static void run_sha256_test(const char *name,
                            const uint8_t *input, size_t input_len,
                            const uint8_t expected[32])
{
    uint8_t result[32];

    sha256(input, input_len, result);

    test_case(name);
    if (bytes_equal(result, expected, 32)) {
        test_pass();
    } else {
        test_fail_bytes("hash mismatch", expected, result, 32);
    }
}

/*
 * Run a single SHA-256d test.
 */
static void run_sha256d_test(const char *name,
                             const uint8_t *input, size_t input_len,
                             const uint8_t expected[32])
{
    uint8_t result[32];

    sha256d(input, input_len, result);

    test_case(name);
    if (bytes_equal(result, expected, 32)) {
        test_pass();
    } else {
        test_fail_bytes("hash mismatch", expected, result, 32);
    }
}

/*
 * Test streaming interface produces same result as one-shot.
 */
static void run_streaming_test(const char *name,
                               const uint8_t *input, size_t input_len)
{
    uint8_t oneshot[32];
    uint8_t streaming[32];
    sha256_ctx_t ctx;
    size_t i;
    char test_name[256];

    /* One-shot */
    sha256(input, input_len, oneshot);

    /* Streaming: feed one byte at a time */
    sha256_init(&ctx);
    for (i = 0; i < input_len; i++) {
        sha256_update(&ctx, input + i, 1);
    }
    sha256_final(&ctx, streaming);

    snprintf(test_name, sizeof(test_name), "%s (streaming)", name);
    test_case(test_name);
    if (bytes_equal(oneshot, streaming, 32)) {
        test_pass();
    } else {
        test_fail_bytes("streaming mismatch", oneshot, streaming, 32);
    }
}

int main(void)
{
    test_suite_begin("SHA-256 Test Suite");

    /*
     * NIST FIPS 180-4 Example: "abc"
     */
    {
        const uint8_t input[] = "abc";
        const uint8_t expected[32] = {
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
            0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
            0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
        };
        run_sha256_test("NIST abc", input, 3, expected);
        run_streaming_test("NIST abc", input, 3);
    }

    /*
     * NIST FIPS 180-4 Example: empty string
     */
    {
        const uint8_t *input = (const uint8_t *)"";
        const uint8_t expected[32] = {
            0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
            0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
            0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
            0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
        };
        run_sha256_test("Empty string", input, 0, expected);
    }

    /*
     * NIST FIPS 180-4 Example: 448 bits (56 bytes)
     * "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
     */
    {
        const uint8_t input[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        const uint8_t expected[32] = {
            0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
            0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
            0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
            0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1
        };
        run_sha256_test("NIST 448 bits", input, 56, expected);
        run_streaming_test("NIST 448 bits", input, 56);
    }

    /*
     * NIST FIPS 180-4 Example: 896 bits (112 bytes)
     * "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn
     *  hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"
     */
    {
        const uint8_t input[] =
            "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
            "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
        const uint8_t expected[32] = {
            0xcf, 0x5b, 0x16, 0xa7, 0x78, 0xaf, 0x83, 0x80,
            0x03, 0x6c, 0xe5, 0x9e, 0x7b, 0x04, 0x92, 0x37,
            0x0b, 0x24, 0x9b, 0x11, 0xe8, 0xf0, 0x7a, 0x51,
            0xaf, 0xac, 0x45, 0x03, 0x7a, 0xfe, 0xe9, 0xd1
        };
        run_sha256_test("NIST 896 bits", input, 112, expected);
        run_streaming_test("NIST 896 bits", input, 112);
    }

    /*
     * SHA-256d test: empty string
     * SHA256d("") = SHA256(SHA256(""))
     */
    {
        const uint8_t *input = (const uint8_t *)"";
        const uint8_t expected[32] = {
            0x5d, 0xf6, 0xe0, 0xe2, 0x76, 0x13, 0x59, 0xd3,
            0x0a, 0x82, 0x75, 0x05, 0x8e, 0x29, 0x9f, 0xcc,
            0x03, 0x81, 0x53, 0x45, 0x45, 0xf5, 0x5c, 0xf4,
            0x3e, 0x41, 0x98, 0x3f, 0x5d, 0x4c, 0x94, 0x56
        };
        run_sha256d_test("SHA256d empty", input, 0, expected);
    }

    /*
     * Bitcoin Genesis Block Header Hash
     * The genesis block header (80 bytes) hashes to the famous
     * 000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f
     * (displayed in little-endian as is convention)
     */
    {
        /* Genesis block header (80 bytes) */
        const uint8_t genesis_header[80] = {
            0x01, 0x00, 0x00, 0x00,  /* version */
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* prev block (zeros) */
            0x3b, 0xa3, 0xed, 0xfd, 0x7a, 0x7b, 0x12, 0xb2,
            0x7a, 0xc7, 0x2c, 0x3e, 0x67, 0x76, 0x8f, 0x61,
            0x7f, 0xc8, 0x1b, 0xc3, 0x88, 0x8a, 0x51, 0x32,
            0x3a, 0x9f, 0xb8, 0xaa, 0x4b, 0x1e, 0x5e, 0x4a,  /* merkle root */
            0x29, 0xab, 0x5f, 0x49,  /* timestamp */
            0xff, 0xff, 0x00, 0x1d,  /* bits */
            0x1d, 0xac, 0x2b, 0x7c   /* nonce */
        };
        /* Expected hash (big-endian, as computed) */
        const uint8_t expected[32] = {
            0x6f, 0xe2, 0x8c, 0x0a, 0xb6, 0xf1, 0xb3, 0x72,
            0xc1, 0xa6, 0xa2, 0x46, 0xae, 0x63, 0xf7, 0x4f,
            0x93, 0x1e, 0x83, 0x65, 0xe1, 0x5a, 0x08, 0x9c,
            0x68, 0xd6, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        run_sha256d_test("Bitcoin Genesis Block", genesis_header, 80, expected);
    }

    test_suite_end();

    return test_global_summary();
}
