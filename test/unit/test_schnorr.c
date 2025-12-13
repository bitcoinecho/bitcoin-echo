/*
 * Bitcoin Echo — Schnorr Signature Test Suite (BIP-340)
 *
 * Tests for BIP-340 Schnorr signature verification.
 * Test vectors from: https://github.com/bitcoin/bips/blob/master/bip-0340/test-vectors.csv
 *
 * Build once. Build right. Stop.
 */

#include "secp256k1.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>
#include "test_utils.h"


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
 * Tagged Hash Tests
 * ============================================================================
 */

static void test_tagged_hash_basic(void)
{
    uint8_t out[32];
    uint8_t msg[] = {0x00};
    /*
     * Compute tagged_hash("BIP0340/challenge", 0x00)
     * This is a basic sanity check.
     */
    secp256k1_schnorr_tagged_hash(out, "BIP0340/challenge", msg, 1);

    /* Just verify it produces non-zero output */
    int all_zero = 1;
    int i;
    for (i = 0; i < 32; i++) {
        if (out[i] != 0) {
            all_zero = 0;
            break;
        }
    }

    if (!all_zero) {
        test_case("Tagged hash produces output");
        test_pass();
    } else {
        test_case("Tagged hash produces output");
        test_fail("Tagged hash produces output");
    }
}

static void test_tagged_hash_empty_msg(void)
{
    uint8_t out[32];
    /* Empty message should still work */
    secp256k1_schnorr_tagged_hash(out, "BIP0340/aux", NULL, 0);

    /* Just verify it runs without crash */
    test_case("Tagged hash with empty message");
        test_pass();
}

/*
 * ============================================================================
 * x-only Pubkey Tests
 * ============================================================================
 */

static void test_xonly_pubkey_parse_valid(void)
{
    secp256k1_point_t p;
    uint8_t xonly[32];
    /* Generator point x-coordinate */
    hex_to_bytes(xonly,
        "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
        32);

    if (secp256k1_xonly_pubkey_parse(&p, xonly)) {
        /* Verify we got a point on the curve */
        if (secp256k1_point_is_valid(&p) && !secp256k1_point_is_infinity(&p)) {
            test_case("Parse valid x-only pubkey (G)");
        test_pass();
        } else {
            test_case("Parse valid x-only pubkey - point invalid");
        test_fail("Parse valid x-only pubkey - point invalid");
        }
    } else {
        test_case("Parse valid x-only pubkey");
        test_fail("Parse valid x-only pubkey");
    }
}

static void test_xonly_pubkey_parse_invalid(void)
{
    secp256k1_point_t p;
    uint8_t xonly[32];
    /* x = p (field size), should fail */
    hex_to_bytes(xonly,
        "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f",
        32);

    if (!secp256k1_xonly_pubkey_parse(&p, xonly)) {
        test_case("Reject x >= p");
        test_pass();
    } else {
        test_case("Reject x >= p");
        test_fail("Reject x >= p");
    }
}

static void test_xonly_pubkey_serialize(void)
{
    secp256k1_point_t p;
    uint8_t xonly_in[32], xonly_out[32];
    hex_to_bytes(xonly_in,
        "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
        32);

    secp256k1_xonly_pubkey_parse(&p, xonly_in);
    secp256k1_xonly_pubkey_serialize(xonly_out, &p);

    if (memcmp(xonly_in, xonly_out, 32) == 0) {
        test_case("x-only serialize roundtrip");
        test_pass();
    } else {
        test_case("x-only serialize roundtrip");
        test_fail("x-only serialize roundtrip");
    }
}

/*
 * ============================================================================
 * BIP-340 Schnorr Verification Tests
 * Official test vectors from BIP-340
 * ============================================================================
 */

/* Test vector 0: Basic valid signature */
static void test_schnorr_vector_0(void)
{
    uint8_t pubkey[32], msg[32], sig[64];
    hex_to_bytes(pubkey,
        "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9", 32);
    hex_to_bytes(msg,
        "0000000000000000000000000000000000000000000000000000000000000000", 32);
    hex_to_bytes(sig,
        "e907831f80848d1069a5371b402410364bdf1c5f8307b0084c55f1ce2dca8215"
        "25f66a4a85ea8b71e482a74f382d2ce5ebeee8fdb2172f477df4900d310536c0", 64);

    if (secp256k1_schnorr_verify(sig, msg, 32, pubkey)) {
        test_case("Vector 0: valid signature");
        test_pass();
    } else {
        test_case("Vector 0: valid signature");
        test_fail("Vector 0: valid signature");
    }
}

/* Test vector 1 */
static void test_schnorr_vector_1(void)
{
    uint8_t pubkey[32], msg[32], sig[64];
    hex_to_bytes(pubkey,
        "dff1d77f2a671c5f36183726db2341be58feae1da2deced843240f7b502ba659", 32);
    hex_to_bytes(msg,
        "243f6a8885a308d313198a2e03707344a4093822299f31d0082efa98ec4e6c89", 32);
    hex_to_bytes(sig,
        "6896bd60eeae296db48a229ff71dfe071bde413e6d43f917dc8dcf8c78de3341"
        "8906d11ac976abccb20b091292bff4ea897efcb639ea871cfa95f6de339e4b0a", 64);

    if (secp256k1_schnorr_verify(sig, msg, 32, pubkey)) {
        test_case("Vector 1: valid signature");
        test_pass();
    } else {
        test_case("Vector 1: valid signature");
        test_fail("Vector 1: valid signature");
    }
}

/* Test vector 2 */
static void test_schnorr_vector_2(void)
{
    uint8_t pubkey[32], msg[32], sig[64];
    hex_to_bytes(pubkey,
        "dd308afec5777e13121fa72b9cc1b7cc0139715309b086c960e18fd969774eb8", 32);
    hex_to_bytes(msg,
        "7e2d58d8b3bcdf1abadec7829054f90dda9805aab56c77333024b9d0a508b75c", 32);
    hex_to_bytes(sig,
        "5831aaeed7b44bb74e5eab94ba9d4294c49bcf2a60728d8b4c200f50dd313c1b"
        "ab745879a5ad954a72c45a91c3a51d3c7adea98d82f8481e0e1e03674a6f3fb7", 64);

    if (secp256k1_schnorr_verify(sig, msg, 32, pubkey)) {
        test_case("Vector 2: valid signature");
        test_pass();
    } else {
        test_case("Vector 2: valid signature");
        test_fail("Vector 2: valid signature");
    }
}

/* Test vector 3: test fails if msg is reduced modulo p or n */
static void test_schnorr_vector_3(void)
{
    uint8_t pubkey[32], msg[32], sig[64];
    hex_to_bytes(pubkey,
        "25d1dff95105f5253c4022f628a996ad3a0d95fbf21d468a1b33f8c160d8f517", 32);
    hex_to_bytes(msg,
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", 32);
    hex_to_bytes(sig,
        "7eb0509757e246f19449885651611cb965ecc1a187dd51b64fda1edc9637d5ec"
        "97582b9cb13db3933705b32ba982af5af25fd78881ebb32771fc5922efc66ea3", 64);

    if (secp256k1_schnorr_verify(sig, msg, 32, pubkey)) {
        test_case("Vector 3: msg not reduced");
        test_pass();
    } else {
        test_case("Vector 3: msg not reduced");
        test_fail("Vector 3: msg not reduced");
    }
}

/* Test vector 4: signature with r starting with 00 */
static void test_schnorr_vector_4(void)
{
    uint8_t pubkey[32], msg[32], sig[64];
    hex_to_bytes(pubkey,
        "d69c3509bb99e412e68b0fe8544e72837dfa30746d8be2aa65975f29d22dc7b9", 32);
    hex_to_bytes(msg,
        "4df3c3f68fcc83b27e9d42c90431a72499f17875c81a599b566c9889b9696703", 32);
    hex_to_bytes(sig,
        "00000000000000000000003b78ce563f89a0ed9414f5aa28ad0d96d6795f9c63"
        "76afb1548af603b3eb45c9f8207dee1060cb71c04e80f593060b07d28308d7f4", 64);

    if (secp256k1_schnorr_verify(sig, msg, 32, pubkey)) {
        test_case("Vector 4: r with leading zeros");
        test_pass();
    } else {
        test_case("Vector 4: r with leading zeros");
        test_fail("Vector 4: r with leading zeros");
    }
}

/* Test vector 5: public key not on curve */
static void test_schnorr_vector_5(void)
{
    uint8_t pubkey[32], msg[32], sig[64];
    hex_to_bytes(pubkey,
        "eefdea4cdb677750a420fee807eacf21eb9898ae79b9768766e4faa04a2d4a34", 32);
    hex_to_bytes(msg,
        "243f6a8885a308d313198a2e03707344a4093822299f31d0082efa98ec4e6c89", 32);
    hex_to_bytes(sig,
        "6cff5c3ba86c69ea4b7376f31a9bcb4f74c1976089b2d9963da2e5543e177769"
        "69e89b4c5564d00349106b8497785dd7d1d713a8ae82b32fa79d5f7fc407d39b", 64);

    if (!secp256k1_schnorr_verify(sig, msg, 32, pubkey)) {
        test_case("Vector 5: reject pubkey not on curve");
        test_pass();
    } else {
        test_case("Vector 5: reject pubkey not on curve");
        test_fail("Vector 5: reject pubkey not on curve");
    }
}

/* Test vector 6: has_even_y(R) is false */
static void test_schnorr_vector_6(void)
{
    uint8_t pubkey[32], msg[32], sig[64];
    hex_to_bytes(pubkey,
        "dff1d77f2a671c5f36183726db2341be58feae1da2deced843240f7b502ba659", 32);
    hex_to_bytes(msg,
        "243f6a8885a308d313198a2e03707344a4093822299f31d0082efa98ec4e6c89", 32);
    hex_to_bytes(sig,
        "fff97bd5755eeea420453a14355235d382f6472f8568a18b2f057a1460297556"
        "3cc27944640ac607cd107ae10923d9ef7a73c643e166be5ebeafa34b1ac553e2", 64);

    if (!secp256k1_schnorr_verify(sig, msg, 32, pubkey)) {
        test_case("Vector 6: reject odd R.y");
        test_pass();
    } else {
        test_case("Vector 6: reject odd R.y");
        test_fail("Vector 6: reject odd R.y");
    }
}

/* Test vector 7: negated message */
static void test_schnorr_vector_7(void)
{
    uint8_t pubkey[32], msg[32], sig[64];
    hex_to_bytes(pubkey,
        "dff1d77f2a671c5f36183726db2341be58feae1da2deced843240f7b502ba659", 32);
    hex_to_bytes(msg,
        "243f6a8885a308d313198a2e03707344a4093822299f31d0082efa98ec4e6c89", 32);
    hex_to_bytes(sig,
        "1fa62e331edbc21c394792d2ab1100a7b432b013df3f6ff4f99fcb33e0e1515f"
        "28890b3edb6e7189b630448b515ce4f8622a954cfe545735aaea5134fccdb2bd", 64);

    if (!secp256k1_schnorr_verify(sig, msg, 32, pubkey)) {
        test_case("Vector 7: reject negated message");
        test_pass();
    } else {
        test_case("Vector 7: reject negated message");
        test_fail("Vector 7: reject negated message");
    }
}

/* Test vector 8: negated s value */
static void test_schnorr_vector_8(void)
{
    uint8_t pubkey[32], msg[32], sig[64];
    hex_to_bytes(pubkey,
        "dff1d77f2a671c5f36183726db2341be58feae1da2deced843240f7b502ba659", 32);
    hex_to_bytes(msg,
        "243f6a8885a308d313198a2e03707344a4093822299f31d0082efa98ec4e6c89", 32);
    hex_to_bytes(sig,
        "6cff5c3ba86c69ea4b7376f31a9bcb4f74c1976089b2d9963da2e5543e177769"
        "961764b3aa9b2ffcb6ef947b6887a226e8d7c93e00c5ed0c1834ff0d0c2e6da6", 64);

    if (!secp256k1_schnorr_verify(sig, msg, 32, pubkey)) {
        test_case("Vector 8: reject negated s");
        test_pass();
    } else {
        test_case("Vector 8: reject negated s");
        test_fail("Vector 8: reject negated s");
    }
}

/* Test vector 12: sig[0:32] is equal to field size */
static void test_schnorr_vector_12(void)
{
    uint8_t pubkey[32], msg[32], sig[64];
    hex_to_bytes(pubkey,
        "dff1d77f2a671c5f36183726db2341be58feae1da2deced843240f7b502ba659", 32);
    hex_to_bytes(msg,
        "243f6a8885a308d313198a2e03707344a4093822299f31d0082efa98ec4e6c89", 32);
    hex_to_bytes(sig,
        "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f"
        "69e89b4c5564d00349106b8497785dd7d1d713a8ae82b32fa79d5f7fc407d39b", 64);

    if (!secp256k1_schnorr_verify(sig, msg, 32, pubkey)) {
        test_case("Vector 12: reject r == p");
        test_pass();
    } else {
        test_case("Vector 12: reject r == p");
        test_fail("Vector 12: reject r == p");
    }
}

/* Test vector 13: sig[32:64] is equal to curve order */
static void test_schnorr_vector_13(void)
{
    uint8_t pubkey[32], msg[32], sig[64];
    hex_to_bytes(pubkey,
        "dff1d77f2a671c5f36183726db2341be58feae1da2deced843240f7b502ba659", 32);
    hex_to_bytes(msg,
        "243f6a8885a308d313198a2e03707344a4093822299f31d0082efa98ec4e6c89", 32);
    hex_to_bytes(sig,
        "6cff5c3ba86c69ea4b7376f31a9bcb4f74c1976089b2d9963da2e5543e177769"
        "fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141", 64);

    if (!secp256k1_schnorr_verify(sig, msg, 32, pubkey)) {
        test_case("Vector 13: reject s == n");
        test_pass();
    } else {
        test_case("Vector 13: reject s == n");
        test_fail("Vector 13: reject s == n");
    }
}

/* Test vector 14: public key exceeds field size */
static void test_schnorr_vector_14(void)
{
    uint8_t pubkey[32], msg[32], sig[64];
    hex_to_bytes(pubkey,
        "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc30", 32);
    hex_to_bytes(msg,
        "243f6a8885a308d313198a2e03707344a4093822299f31d0082efa98ec4e6c89", 32);
    hex_to_bytes(sig,
        "6cff5c3ba86c69ea4b7376f31a9bcb4f74c1976089b2d9963da2e5543e177769"
        "69e89b4c5564d00349106b8497785dd7d1d713a8ae82b32fa79d5f7fc407d39b", 64);

    if (!secp256k1_schnorr_verify(sig, msg, 32, pubkey)) {
        test_case("Vector 14: reject pubkey >= p");
        test_pass();
    } else {
        test_case("Vector 14: reject pubkey >= p");
        test_fail("Vector 14: reject pubkey >= p");
    }
}

/* Test vector 15: message of size 0 */
static void test_schnorr_vector_15(void)
{
    uint8_t pubkey[32], sig[64];
    hex_to_bytes(pubkey,
        "778caa53b4393ac467774d09497a87224bf9fab6f6e68b23086497324d6fd117", 32);
    hex_to_bytes(sig,
        "71535db165ecd9fbbc046e5ffaea61186bb6ad436732fccc25291a55895464cf"
        "6069ce26bf03466228f19a3a62db8a649f2d560fac652827d1af0574e427ab63", 64);

    if (secp256k1_schnorr_verify(sig, NULL, 0, pubkey)) {
        test_case("Vector 15: empty message");
        test_pass();
    } else {
        test_case("Vector 15: empty message");
        test_fail("Vector 15: empty message");
    }
}

/* Test vector 16: message of size 1 */
static void test_schnorr_vector_16(void)
{
    uint8_t pubkey[32], msg[1], sig[64];
    hex_to_bytes(pubkey,
        "778caa53b4393ac467774d09497a87224bf9fab6f6e68b23086497324d6fd117", 32);
    hex_to_bytes(msg, "11", 1);
    hex_to_bytes(sig,
        "08a20a0afef64124649232e0693c583ab1b9934ae63b4c3511f3ae1134c6a303"
        "ea3173bfea6683bd101fa5aa5dbc1996fe7cacfc5a577d33ec14564cec2bacbf", 64);

    if (secp256k1_schnorr_verify(sig, msg, 1, pubkey)) {
        test_case("Vector 16: 1-byte message");
        test_pass();
    } else {
        test_case("Vector 16: 1-byte message");
        test_fail("Vector 16: 1-byte message");
    }
}

/* Test vector 17: message of size 17 */
static void test_schnorr_vector_17(void)
{
    uint8_t pubkey[32], msg[17], sig[64];
    hex_to_bytes(pubkey,
        "778caa53b4393ac467774d09497a87224bf9fab6f6e68b23086497324d6fd117", 32);
    hex_to_bytes(msg, "0102030405060708090a0b0c0d0e0f1011", 17);
    hex_to_bytes(sig,
        "5130f39a4059b43bc7cac09a19ece52b5d8699d1a71e3c52da9afdb6b50ac370"
        "c4a482b77bf960f8681540e25b6771ece1e5a37fd80e5a51897c5566a97ea5a5", 64);

    if (secp256k1_schnorr_verify(sig, msg, 17, pubkey)) {
        test_case("Vector 17: 17-byte message");
        test_pass();
    } else {
        test_case("Vector 17: 17-byte message");
        test_fail("Vector 17: 17-byte message");
    }
}

/*
 * ============================================================================
 * Main
 * ============================================================================
 */

int main(void)
{
    test_suite_begin("Schnorr Signature Tests (BIP-340)");

    test_section("Tagged Hash");
    test_tagged_hash_basic();
    test_tagged_hash_empty_msg();

    test_section("x-only Pubkey");
    test_xonly_pubkey_parse_valid();
    test_xonly_pubkey_parse_invalid();
    test_xonly_pubkey_serialize();

    test_section("BIP-340 Test Vectors (valid signatures)");
    test_schnorr_vector_0();
    test_schnorr_vector_1();
    test_schnorr_vector_2();
    test_schnorr_vector_3();
    test_schnorr_vector_4();

    test_section("BIP-340 Test Vectors (invalid signatures)");
    test_schnorr_vector_5();
    test_schnorr_vector_6();
    test_schnorr_vector_7();
    test_schnorr_vector_8();
    test_schnorr_vector_12();
    test_schnorr_vector_13();
    test_schnorr_vector_14();

    test_section("BIP-340 Test Vectors (variable message sizes)");
    test_schnorr_vector_15();
    test_schnorr_vector_16();
    test_schnorr_vector_17();

    test_suite_end();
    return test_global_summary();
}
