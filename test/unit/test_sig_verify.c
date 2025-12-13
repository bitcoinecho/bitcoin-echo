/*
 * Bitcoin Echo — Signature Verification Interface Test Suite
 *
 * Tests for the unified signature verification interface (sig_verify.h).
 * This verifies that the succession seam correctly dispatches to
 * ECDSA and Schnorr verification.
 *
 * Build once. Build right. Stop.
 */

#include "sig_verify.h"
#include "secp256k1.h"
#include "test_utils.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Helper: convert hex string to bytes
 */
static int hex_to_bytes(uint8_t *out, const char *hex, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned int val;
        if (sscanf(hex + i * 2, "%02x", &val) != 1) {
            return 0;
        }
        out[i] = (uint8_t)val;
    }
    return 1;
}

/*
 * ============================================================================
 * sig_type_known Tests
 * ============================================================================
 */

static void test_sig_type_known_ecdsa(void)
{
    test_case("SIG_ECDSA is known");

    if (sig_type_known(SIG_ECDSA)) {
        test_pass();
    } else {
        test_fail("SIG_ECDSA not recognized");
    }
}

static void test_sig_type_known_schnorr(void)
{
    test_case("SIG_SCHNORR is known");

    if (sig_type_known(SIG_SCHNORR)) {
        test_pass();
    } else {
        test_fail("SIG_SCHNORR not recognized");
    }
}

static void test_sig_type_unknown(void)
{
    test_case("Unknown type rejected");

    /* Use an invalid enum value */
    if (!sig_type_known((sig_type_t)999)) {
        test_pass();
    } else {
        test_fail("Invalid signature type accepted");
    }
}

/*
 * ============================================================================
 * ECDSA via sig_verify Tests
 * ============================================================================
 */

/*
 * Valid ECDSA signature test.
 * Uses the same test case as test_ecdsa.c:
 *   d=1, e=1, k=1
 *   pubkey = G (compressed)
 *   r = Gx, s = Gx + 1
 */
static void test_sig_verify_ecdsa_valid(void)
{
    uint8_t pubkey[33];
    uint8_t hash[32];
    uint8_t sig[] = {
        0x30, 0x44,  /* SEQUENCE, 68 bytes */
        0x02, 0x20,  /* INTEGER, 32 bytes (r) */
        0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac,
        0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b, 0x07,
        0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9,
        0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98,
        0x02, 0x20,  /* INTEGER, 32 bytes (s) */
        0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac,
        0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b, 0x07,
        0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9,
        0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x99
    };

    /* Public key = G (compressed) */
    hex_to_bytes(pubkey,
        "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
        33);

    /* Message hash = 1 */
    hex_to_bytes(hash,
        "0000000000000000000000000000000000000000000000000000000000000001",
        32);

    test_case("ECDSA valid signature via sig_verify");
    if (sig_verify(SIG_ECDSA, sig, sizeof(sig), hash, pubkey, 33)) {
        test_pass();
    } else {
        test_fail("Valid ECDSA signature rejected");
    }
}

static void test_sig_verify_ecdsa_wrong_hash(void)
{
    uint8_t pubkey[33];
    uint8_t hash[32];
    uint8_t sig[] = {
        0x30, 0x44,
        0x02, 0x20,
        0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac,
        0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b, 0x07,
        0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9,
        0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98,
        0x02, 0x20,
        0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac,
        0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b, 0x07,
        0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9,
        0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x99
    };

    hex_to_bytes(pubkey,
        "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
        33);

    /* Wrong hash (2 instead of 1) */
    hex_to_bytes(hash,
        "0000000000000000000000000000000000000000000000000000000000000002",
        32);

    test_case("ECDSA reject wrong hash");
    if (!sig_verify(SIG_ECDSA, sig, sizeof(sig), hash, pubkey, 33)) {
        test_pass();
    } else {
        test_fail("ECDSA accepted signature with wrong hash");
    }
}

static void test_sig_verify_ecdsa_invalid_sig_len(void)
{
    uint8_t pubkey[33];
    uint8_t hash[32];
    uint8_t sig[4] = {0x30, 0x02, 0x02, 0x00};  /* Too short */

    hex_to_bytes(pubkey,
        "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
        33);
    hex_to_bytes(hash,
        "0000000000000000000000000000000000000000000000000000000000000001",
        32);

    test_case("ECDSA reject invalid sig length");
    if (!sig_verify(SIG_ECDSA, sig, sizeof(sig), hash, pubkey, 33)) {
        test_pass();
    } else {
        test_fail("ECDSA accepted invalid signature length");
    }
}

static void test_sig_verify_ecdsa_invalid_pubkey_len(void)
{
    uint8_t pubkey[32];  /* Wrong length */
    uint8_t hash[32];
    uint8_t sig[] = {
        0x30, 0x44,
        0x02, 0x20,
        0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac,
        0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b, 0x07,
        0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9,
        0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98,
        0x02, 0x20,
        0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac,
        0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b, 0x07,
        0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9,
        0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x99
    };

    memset(pubkey, 0, 32);
    hex_to_bytes(hash,
        "0000000000000000000000000000000000000000000000000000000000000001",
        32);

    test_case("ECDSA reject invalid pubkey length");
    if (!sig_verify(SIG_ECDSA, sig, sizeof(sig), hash, pubkey, 32)) {
        test_pass();
    } else {
        test_fail("ECDSA accepted invalid pubkey length");
    }
}

/*
 * ============================================================================
 * Schnorr via sig_verify Tests
 * ============================================================================
 */

/* BIP-340 test vector 0 */
static void test_sig_verify_schnorr_valid(void)
{
    uint8_t pubkey[32], hash[32], sig[64];

    hex_to_bytes(pubkey,
        "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9", 32);
    hex_to_bytes(hash,
        "0000000000000000000000000000000000000000000000000000000000000000", 32);
    hex_to_bytes(sig,
        "e907831f80848d1069a5371b402410364bdf1c5f8307b0084c55f1ce2dca8215"
        "25f66a4a85ea8b71e482a74f382d2ce5ebeee8fdb2172f477df4900d310536c0", 64);

    test_case("Schnorr valid signature via sig_verify");
    if (sig_verify(SIG_SCHNORR, sig, 64, hash, pubkey, 32)) {
        test_pass();
    } else {
        test_fail("Valid Schnorr signature rejected");
    }
}

static void test_sig_verify_schnorr_wrong_hash(void)
{
    uint8_t pubkey[32], hash[32], sig[64];

    hex_to_bytes(pubkey,
        "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9", 32);
    /* Wrong hash */
    hex_to_bytes(hash,
        "0000000000000000000000000000000000000000000000000000000000000001", 32);
    hex_to_bytes(sig,
        "e907831f80848d1069a5371b402410364bdf1c5f8307b0084c55f1ce2dca8215"
        "25f66a4a85ea8b71e482a74f382d2ce5ebeee8fdb2172f477df4900d310536c0", 64);

    test_case("Schnorr reject wrong hash");
    if (!sig_verify(SIG_SCHNORR, sig, 64, hash, pubkey, 32)) {
        test_pass();
    } else {
        test_fail("Schnorr accepted signature with wrong hash");
    }
}

static void test_sig_verify_schnorr_invalid_sig_len(void)
{
    uint8_t pubkey[32], hash[32], sig[32];  /* Wrong length */

    hex_to_bytes(pubkey,
        "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9", 32);
    hex_to_bytes(hash,
        "0000000000000000000000000000000000000000000000000000000000000000", 32);
    memset(sig, 0, 32);

    test_case("Schnorr reject invalid sig length");
    if (!sig_verify(SIG_SCHNORR, sig, 32, hash, pubkey, 32)) {
        test_pass();
    } else {
        test_fail("Schnorr accepted invalid signature length");
    }
}

static void test_sig_verify_schnorr_invalid_pubkey_len(void)
{
    uint8_t pubkey[33], hash[32], sig[64];  /* Wrong pubkey length */

    hex_to_bytes(pubkey,
        "02f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9", 33);
    hex_to_bytes(hash,
        "0000000000000000000000000000000000000000000000000000000000000000", 32);
    memset(sig, 0, 64);

    test_case("Schnorr reject invalid pubkey length");
    if (!sig_verify(SIG_SCHNORR, sig, 64, hash, pubkey, 33)) {
        test_pass();
    } else {
        test_fail("Schnorr accepted invalid pubkey length");
    }
}

/*
 * ============================================================================
 * Unknown Type Tests
 * ============================================================================
 */

static void test_sig_verify_unknown_type(void)
{
    uint8_t pubkey[33], hash[32], sig[64];

    memset(pubkey, 0, 33);
    memset(hash, 0, 32);
    memset(sig, 0, 64);

    test_case("Reject unknown signature type");
    if (!sig_verify((sig_type_t)999, sig, 64, hash, pubkey, 33)) {
        test_pass();
    } else {
        test_fail("Unknown signature type accepted");
    }
}

/*
 * ============================================================================
 * NULL Input Tests
 * ============================================================================
 */

static void test_sig_verify_null_inputs(void)
{
    uint8_t data[64];

    memset(data, 0, 64);

    /* All NULL combinations should fail */
    int failed = 0;
    if (sig_verify(SIG_ECDSA, NULL, 64, data, data, 33)) failed = 1;
    if (sig_verify(SIG_ECDSA, data, 64, NULL, data, 33)) failed = 1;
    if (sig_verify(SIG_ECDSA, data, 64, data, NULL, 33)) failed = 1;

    test_case("Reject NULL inputs");
    if (!failed) {
        test_pass();
    } else {
        test_fail("NULL inputs accepted");
    }
}

/*
 * ============================================================================
 * Main
 * ============================================================================
 */

int main(void)
{
    test_suite_begin("Signature Verification Interface Tests");

    test_section("sig_type_known");
    test_sig_type_known_ecdsa();
    test_sig_type_known_schnorr();
    test_sig_type_unknown();

    test_section("ECDSA via sig_verify");
    test_sig_verify_ecdsa_valid();
    test_sig_verify_ecdsa_wrong_hash();
    test_sig_verify_ecdsa_invalid_sig_len();
    test_sig_verify_ecdsa_invalid_pubkey_len();

    test_section("Schnorr via sig_verify");
    test_sig_verify_schnorr_valid();
    test_sig_verify_schnorr_wrong_hash();
    test_sig_verify_schnorr_invalid_sig_len();
    test_sig_verify_schnorr_invalid_pubkey_len();

    test_section("Edge cases");
    test_sig_verify_unknown_type();
    test_sig_verify_null_inputs();

    test_suite_end();
    return test_global_summary();
}
