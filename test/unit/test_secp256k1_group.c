/*
 * Bitcoin Echo — secp256k1 Group Operations Tests
 *
 * Test vectors for elliptic curve point operations.
 *
 * Build once. Build right. Stop.
 */

#include "secp256k1.h"
#include "test_utils.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void print_hex(const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

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

static void test_generator_on_curve(void)
{
    secp256k1_point_t g;
    secp256k1_fe_t gx, gy;
    uint8_t gx_bytes[32], gy_bytes[32];

    /* Known generator point coordinates */
    hex_to_bytes(gx_bytes, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", 32);
    hex_to_bytes(gy_bytes, "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8", 32);

    secp256k1_fe_set_bytes(&gx, gx_bytes);
    secp256k1_fe_set_bytes(&gy, gy_bytes);
    secp256k1_point_set_xy(&g, &gx, &gy);

    test_case("Generator G is on curve");
    if (secp256k1_point_is_valid(&g)) {
        test_pass();
    } else {
        test_fail("Generator point not on curve");
    }
}

static void test_point_double(void)
{
    secp256k1_point_t g, g2;
    secp256k1_fe_t gx, gy, x, y;
    uint8_t gx_bytes[32], gy_bytes[32];
    uint8_t result[32];

    

    /* Load G */
    hex_to_bytes(gx_bytes, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", 32);
    hex_to_bytes(gy_bytes, "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8", 32);
    secp256k1_fe_set_bytes(&gx, gx_bytes);
    secp256k1_fe_set_bytes(&gy, gy_bytes);
    secp256k1_point_set_xy(&g, &gx, &gy);

    /* Compute 2*G */
    secp256k1_point_double(&g2, &g);

    /* Get affine coordinates */
    secp256k1_point_get_xy(&x, &y, &g2);
    secp256k1_fe_get_bytes(result, &x);

    /* Known value: 2*G x-coordinate */
    uint8_t expected_x[32];
    hex_to_bytes(expected_x, "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5", 32);

    test_case("2*G x-coordinate");
    if (memcmp(result, expected_x, 32) == 0) {
        
        test_pass();
    } else {
        test_fail("2*G x-coordinate");
        printf("    Expected: ");
        print_hex(expected_x, 32);
        printf("\n");
        printf("    Got:      ");
        print_hex(result, 32);
        printf("\n");
    }

    

    /* Verify 2*G is on curve */
    test_case("2*G is on curve");
    if (secp256k1_point_is_valid(&g2)) {
        
        test_pass();
    } else {
        test_fail("2*G is on curve");
    }
}

static void test_point_add(void)
{
    secp256k1_point_t g, g2, g3_add;
    secp256k1_fe_t gx, gy, x_add, x_dbl;
    uint8_t gx_bytes[32], gy_bytes[32];
    uint8_t result_add[32], result_dbl[32];

    

    /* Load G */
    hex_to_bytes(gx_bytes, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", 32);
    hex_to_bytes(gy_bytes, "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8", 32);
    secp256k1_fe_set_bytes(&gx, gx_bytes);
    secp256k1_fe_set_bytes(&gy, gy_bytes);
    secp256k1_point_set_xy(&g, &gx, &gy);

    /* Compute 2*G via doubling */
    secp256k1_point_double(&g2, &g);

    /* Compute G + G via addition */
    secp256k1_point_add(&g3_add, &g, &g);

    /* They should be equal */
    secp256k1_point_get_xy(&x_add, NULL, &g3_add);
    secp256k1_point_get_xy(&x_dbl, NULL, &g2);
    secp256k1_fe_get_bytes(result_add, &x_add);
    secp256k1_fe_get_bytes(result_dbl, &x_dbl);

    test_case("G + G = 2*G");
    if (memcmp(result_add, result_dbl, 32) == 0) {
        
        test_pass();
    } else {
        test_fail("G + G = 2*G");
    }

    

    /* Compute 3*G = 2*G + G */
    secp256k1_point_add(&g3_add, &g2, &g);

    uint8_t expected_3g_x[32];
    hex_to_bytes(expected_3g_x, "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9", 32);

    secp256k1_point_get_xy(&x_add, NULL, &g3_add);
    secp256k1_fe_get_bytes(result_add, &x_add);

    test_case("3*G x-coordinate");
    if (memcmp(result_add, expected_3g_x, 32) == 0) {
        
        test_pass();
    } else {
        test_fail("3*G x-coordinate");
        printf("    Expected: ");
        print_hex(expected_3g_x, 32);
        printf("\n");
        printf("    Got:      ");
        print_hex(result_add, 32);
        printf("\n");
    }
}

static void test_point_neg(void)
{
    secp256k1_point_t g, neg_g, sum;
    secp256k1_fe_t gx, gy;
    uint8_t gx_bytes[32], gy_bytes[32];

    

    /* Load G */
    hex_to_bytes(gx_bytes, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", 32);
    hex_to_bytes(gy_bytes, "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8", 32);
    secp256k1_fe_set_bytes(&gx, gx_bytes);
    secp256k1_fe_set_bytes(&gy, gy_bytes);
    secp256k1_point_set_xy(&g, &gx, &gy);

    /* Compute -G */
    secp256k1_point_neg(&neg_g, &g);

    /* G + (-G) should be infinity */
    secp256k1_point_add(&sum, &g, &neg_g);

    test_case("G + (-G) = infinity");
    if (secp256k1_point_is_infinity(&sum)) {
        
        test_pass();
    } else {
        test_fail("G + (-G) = infinity");
    }
}

static void test_scalar_mul(void)
{
    secp256k1_point_t result;
    secp256k1_scalar_t k;
    secp256k1_fe_t x;
    uint8_t k_bytes[32], result_x[32];

    

    /* k = 2 */
    memset(k_bytes, 0, 32);
    k_bytes[31] = 2;
    secp256k1_scalar_set_bytes(&k, k_bytes);

    /* Compute 2*G via scalar multiplication */
    secp256k1_point_mul_gen(&result, &k);

    secp256k1_point_get_xy(&x, NULL, &result);
    secp256k1_fe_get_bytes(result_x, &x);

    uint8_t expected_2g_x[32];
    hex_to_bytes(expected_2g_x, "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5", 32);

    test_case("2*G via scalar mul");
    if (memcmp(result_x, expected_2g_x, 32) == 0) {
        
        test_pass();
    } else {
        test_fail("2*G via scalar mul");
        printf("    Expected: ");
        print_hex(expected_2g_x, 32);
        printf("\n");
        printf("    Got:      ");
        print_hex(result_x, 32);
        printf("\n");
    }

    

    /* k = 7 */
    memset(k_bytes, 0, 32);
    k_bytes[31] = 7;
    secp256k1_scalar_set_bytes(&k, k_bytes);

    secp256k1_point_mul_gen(&result, &k);

    secp256k1_point_get_xy(&x, NULL, &result);
    secp256k1_fe_get_bytes(result_x, &x);

    /* Known value: 7*G x-coordinate */
    uint8_t expected_7g_x[32];
    hex_to_bytes(expected_7g_x, "5cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bc", 32);

    test_case("7*G via scalar mul");
    if (memcmp(result_x, expected_7g_x, 32) == 0) {
        
        test_pass();
    } else {
        test_fail("7*G via scalar mul");
        printf("    Expected: ");
        print_hex(expected_7g_x, 32);
        printf("\n");
        printf("    Got:      ");
        print_hex(result_x, 32);
        printf("\n");
    }

    

    /* Test with a larger scalar */
    /* k = 0xAA...AA (alternating bits) */
    memset(k_bytes, 0xAA, 32);
    secp256k1_scalar_set_bytes(&k, k_bytes);

    secp256k1_point_mul_gen(&result, &k);

    test_case("Large scalar result on curve");
    if (secp256k1_point_is_valid(&result)) {
        
        test_pass();
    } else {
        test_fail("Large scalar result on curve");
    }
}

static void test_pubkey_roundtrip(void)
{
    secp256k1_point_t g, parsed;
    secp256k1_fe_t gx, gy, px, py;
    uint8_t gx_bytes[32], gy_bytes[32];
    uint8_t compressed[33], uncompressed[65];
    uint8_t gx_out[32], px_out[32];

    

    /* Load G */
    hex_to_bytes(gx_bytes, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", 32);
    hex_to_bytes(gy_bytes, "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8", 32);
    secp256k1_fe_set_bytes(&gx, gx_bytes);
    secp256k1_fe_set_bytes(&gy, gy_bytes);
    secp256k1_point_set_xy(&g, &gx, &gy);

    /* Serialize compressed */
    secp256k1_pubkey_serialize(compressed, &g, 1);

    /* Parse compressed */
    test_case("Compressed pubkey roundtrip");
    if (secp256k1_pubkey_parse(&parsed, compressed, 33)) {
        secp256k1_point_get_xy(&px, &py, &parsed);
        secp256k1_fe_get_bytes(gx_out, &gx);
        secp256k1_fe_get_bytes(px_out, &px);

        if (memcmp(gx_out, px_out, 32) == 0) {
            test_pass();
        } else {
            test_fail("Compressed pubkey x-coordinate mismatch");
        }
    } else {
        test_fail("Compressed pubkey parse failed");
    }

    

    /* Serialize uncompressed */
    secp256k1_pubkey_serialize(uncompressed, &g, 0);

    /* Parse uncompressed */
    test_case("Uncompressed pubkey roundtrip");
    if (secp256k1_pubkey_parse(&parsed, uncompressed, 65)) {
        secp256k1_point_get_xy(&px, &py, &parsed);
        secp256k1_fe_get_bytes(px_out, &px);

        if (memcmp(gx_out, px_out, 32) == 0) {
            test_pass();
        } else {
            test_fail("Uncompressed pubkey x-coordinate mismatch");
        }
    } else {
        test_fail("Uncompressed pubkey parse failed");
    }

    

    /* Test parsing known compressed pubkey */
    uint8_t known_compressed[33];
    hex_to_bytes(known_compressed, "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", 33);

    test_case("Parse known compressed G");
    if (secp256k1_pubkey_parse(&parsed, known_compressed, 33)) {
        if (secp256k1_point_is_valid(&parsed)) {
            test_pass();
        } else {
            test_fail("Parsed point not on curve");
        }
    } else {
        test_fail("Failed to parse known compressed G");
    }
}

static void test_infinity_handling(void)
{
    secp256k1_point_t inf, g, result;
    secp256k1_fe_t gx, gy;
    uint8_t gx_bytes[32], gy_bytes[32];

    

    /* Set up infinity */
    secp256k1_point_set_infinity(&inf);

    /* Load G */
    hex_to_bytes(gx_bytes, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", 32);
    hex_to_bytes(gy_bytes, "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8", 32);
    secp256k1_fe_set_bytes(&gx, gx_bytes);
    secp256k1_fe_set_bytes(&gy, gy_bytes);
    secp256k1_point_set_xy(&g, &gx, &gy);

    /* infinity + G = G */
    secp256k1_point_add(&result, &inf, &g);

    secp256k1_fe_t rx;
    uint8_t rx_bytes[32];
    secp256k1_point_get_xy(&rx, NULL, &result);
    secp256k1_fe_get_bytes(rx_bytes, &rx);

    test_case("infinity + G = G");
    if (memcmp(rx_bytes, gx_bytes, 32) == 0) {
        
        test_pass();
    } else {
        test_fail("infinity + G = G");
    }

    

    /* G + infinity = G */
    secp256k1_point_add(&result, &g, &inf);

    secp256k1_point_get_xy(&rx, NULL, &result);
    secp256k1_fe_get_bytes(rx_bytes, &rx);

    test_case("G + infinity = G");
    if (memcmp(rx_bytes, gx_bytes, 32) == 0) {
        
        test_pass();
    } else {
        test_fail("G + infinity = G");
    }

    

    /* 2*infinity = infinity */
    secp256k1_point_double(&result, &inf);

    test_case("2*infinity = infinity");
    if (secp256k1_point_is_infinity(&result)) {
        
        test_pass();
    } else {
        test_fail("2*infinity = infinity");
    }
}

int main(void)
{
    test_suite_begin("secp256k1 Group Operations Tests");

    test_generator_on_curve();
    test_point_double();
    test_point_add();
    test_point_neg();
    test_scalar_mul();
    test_pubkey_roundtrip();
    test_infinity_handling();

    test_suite_end();
    return test_global_summary();
}
