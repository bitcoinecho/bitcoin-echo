/*
 * Bitcoin Echo — secp256k1 Field Arithmetic Tests
 *
 * Test vectors for field operations modulo p.
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

__attribute__((unused))
static void print_fe(const secp256k1_fe_t *a)
{
    uint8_t bytes[32];
    secp256k1_fe_get_bytes(bytes, a);
    print_hex(bytes, 32);
}

static int fe_from_hex(secp256k1_fe_t *r, const char *hex)
{
    uint8_t bytes[32];
    int i;

    if (strlen(hex) != 64) {
        return 0;
    }

    for (i = 0; i < 32; i++) {
        unsigned int val;
        if (sscanf(hex + i * 2, "%02x", &val) != 1) {
            return 0;
        }
        bytes[i] = (uint8_t)val;
    }

    return secp256k1_fe_set_bytes(r, bytes);
}

static void test_fe_zero_one(void)
{
    secp256k1_fe_t a, b;

    secp256k1_fe_zero(&a);
    secp256k1_fe_one(&b);

    test_case("Zero and one");
    if (secp256k1_fe_is_zero(&a) && !secp256k1_fe_is_zero(&b)) {
        test_pass();
    } else {
        test_fail("Zero or one initialization failed");
    }
}

static void test_fe_set_bytes(void)
{
    secp256k1_fe_t a;
    uint8_t bytes[32];
    int valid;

    /* Set from valid value */
    valid = fe_from_hex(&a, "0000000000000000000000000000000000000000000000000000000000000001");
    secp256k1_fe_get_bytes(bytes, &a);

    test_case("Set bytes (1)");
    if (valid && bytes[31] == 1) {
        test_pass();
    } else {
        test_fail("Failed to set bytes to 1");
    }

    /* Test value at boundary (p - 1 should be valid) */
    valid = fe_from_hex(&a, "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2e");

    test_case("Set bytes (p-1)");
    if (valid) {
        test_pass();
    } else {
        test_fail("p-1 should be valid but was rejected");
    }

    /* Test value >= p (should be invalid) */
    valid = fe_from_hex(&a, "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f");

    test_case("Set bytes (p) rejected");
    if (!valid) {
        test_pass();
    } else {
        test_fail("Value >= p should be invalid but was accepted");
    }
}

static void test_fe_add(void)
{
    secp256k1_fe_t a, b, r, expected;

    /* Simple addition: 1 + 2 = 3 */
    secp256k1_fe_set_int(&a, 1);
    secp256k1_fe_set_int(&b, 2);
    secp256k1_fe_add(&r, &a, &b);
    secp256k1_fe_set_int(&expected, 3);

    test_case("Add: 1 + 2 = 3");
    if (secp256k1_fe_equal(&r, &expected)) {
        test_pass();
    } else {
        test_fail("1 + 2 != 3");
    }

    /* Addition wrapping around p */
    /* (p - 1) + 2 = 1 (mod p) */
    fe_from_hex(&a, "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2e");
    secp256k1_fe_set_int(&b, 2);
    secp256k1_fe_add(&r, &a, &b);
    secp256k1_fe_set_int(&expected, 1);

    test_case("Add: (p-1) + 2 = 1");
    if (secp256k1_fe_equal(&r, &expected)) {
        test_pass();
    } else {
        test_fail("Addition modulo p wrap failed");
    }
}

static void test_fe_sub(void)
{
    secp256k1_fe_t a, b, r, expected;

    /* Simple subtraction: 5 - 3 = 2 */
    secp256k1_fe_set_int(&a, 5);
    secp256k1_fe_set_int(&b, 3);
    secp256k1_fe_sub(&r, &a, &b);
    secp256k1_fe_set_int(&expected, 2);

    test_case("Sub: 5 - 3 = 2");
    if (secp256k1_fe_equal(&r, &expected)) {
        test_pass();
    } else {
        test_fail("5 - 3 != 2");
    }

    /* Subtraction with wrap: 1 - 2 = p - 1 (mod p) */
    secp256k1_fe_set_int(&a, 1);
    secp256k1_fe_set_int(&b, 2);
    secp256k1_fe_sub(&r, &a, &b);
    fe_from_hex(&expected, "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2e");

    test_case("Sub: 1 - 2 = p-1");
    if (secp256k1_fe_equal(&r, &expected)) {
        test_pass();
    } else {
        test_fail("Subtraction modulo p wrap failed");
    }
}

static void test_fe_neg(void)
{
    secp256k1_fe_t a, r, expected;

    /* -1 = p - 1 (mod p) */
    secp256k1_fe_set_int(&a, 1);
    secp256k1_fe_neg(&r, &a);
    fe_from_hex(&expected, "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2e");

    test_case("Neg: -1 = p-1");
    if (secp256k1_fe_equal(&r, &expected)) {
        test_pass();
    } else {
        test_fail("-1 != p-1");
    }

    /* -0 = 0 */
    secp256k1_fe_zero(&a);
    secp256k1_fe_neg(&r, &a);

    test_case("Neg: -0 = 0");
    if (secp256k1_fe_is_zero(&r)) {
        test_pass();
    } else {
        test_fail("-0 != 0");
    }
}

static void test_fe_mul(void)
{
    secp256k1_fe_t a, b, r, expected;

    /* Simple multiplication: 3 * 7 = 21 */
    secp256k1_fe_set_int(&a, 3);
    secp256k1_fe_set_int(&b, 7);
    secp256k1_fe_mul(&r, &a, &b);
    secp256k1_fe_set_int(&expected, 21);

    test_case("Mul: 3 * 7 = 21");
    if (secp256k1_fe_equal(&r, &expected)) {
        test_pass();
    } else {
        test_fail("3 * 7 != 21");
    }

    /* Multiplication by 1 */
    fe_from_hex(&a, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    secp256k1_fe_one(&b);
    secp256k1_fe_mul(&r, &a, &b);

    test_case("Mul: Gx * 1 = Gx");
    if (secp256k1_fe_equal(&r, &a)) {
        test_pass();
    } else {
        test_fail("Gx * 1 != Gx");
    }

    /* Multiplication by 0 */
    fe_from_hex(&a, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    secp256k1_fe_zero(&b);
    secp256k1_fe_mul(&r, &a, &b);

    test_case("Mul: Gx * 0 = 0");
    if (secp256k1_fe_is_zero(&r)) {
        test_pass();
    } else {
        test_fail("Gx * 0 != 0");
    }

    /* Larger multiplication that requires reduction */
    /* 2^128 * 2^128 = 2^256 ≡ 2^32 + 977 (mod p) */
    fe_from_hex(&a, "0000000000000000000000000000000100000000000000000000000000000000");
    secp256k1_fe_mul(&r, &a, &a);
    /* Expected: 2^32 + 977 = 0x100000000 + 0x3d1 = 0x1000003d1 */
    fe_from_hex(&expected, "00000000000000000000000000000000000000000000000000000001000003d1");

    test_case("Mul: 2^128 * 2^128 = 2^32 + 977");
    if (secp256k1_fe_equal(&r, &expected)) {
        test_pass();
    } else {
        test_fail("Large multiplication with reduction failed");
    }
}

static void test_fe_sqr(void)
{
    secp256k1_fe_t a, r, expected;

    /* 7² = 49 */
    secp256k1_fe_set_int(&a, 7);
    secp256k1_fe_sqr(&r, &a);
    secp256k1_fe_set_int(&expected, 49);

    test_case("Sqr: 7^2 = 49");
    if (secp256k1_fe_equal(&r, &expected)) {
        test_pass();
    } else {
        test_fail("7^2 != 49");
    }
}

static void test_fe_inv(void)
{
    secp256k1_fe_t a, inv, product, one;

    /* inv(7) * 7 = 1 */
    secp256k1_fe_set_int(&a, 7);
    secp256k1_fe_inv(&inv, &a);
    secp256k1_fe_mul(&product, &inv, &a);
    secp256k1_fe_one(&one);

    test_case("Inv: inv(7) * 7 = 1");
    if (secp256k1_fe_equal(&product, &one)) {
        test_pass();
    } else {
        test_fail("inv(7) * 7 != 1");
    }

    /* inv(Gx) * Gx = 1 */
    fe_from_hex(&a, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    secp256k1_fe_inv(&inv, &a);
    secp256k1_fe_mul(&product, &inv, &a);

    test_case("Inv: inv(Gx) * Gx = 1");
    if (secp256k1_fe_equal(&product, &one)) {
        test_pass();
    } else {
        test_fail("inv(Gx) * Gx != 1");
    }
}

static void test_fe_sqrt(void)
{
    secp256k1_fe_t a, root, squared;
    int has_sqrt;

    /* sqrt(49) = 7 (or p - 7) */
    secp256k1_fe_set_int(&a, 49);
    has_sqrt = secp256k1_fe_sqrt(&root, &a);
    secp256k1_fe_sqr(&squared, &root);

    test_case("Sqrt: sqrt(49)^2 = 49");
    if (has_sqrt && secp256k1_fe_equal(&squared, &a)) {
        test_pass();
    } else {
        test_fail("sqrt(49)^2 != 49");
    }

    /* Verify Gy² = Gx³ + 7 (curve equation) */
    secp256k1_fe_t gx, gy, gx3, rhs, seven;

    fe_from_hex(&gx, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    fe_from_hex(&gy, "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8");

    /* Compute Gx³ */
    secp256k1_fe_sqr(&gx3, &gx);      /* Gx² */
    secp256k1_fe_mul(&gx3, &gx3, &gx); /* Gx³ */

    /* Compute Gx³ + 7 */
    secp256k1_fe_set_int(&seven, 7);
    secp256k1_fe_add(&rhs, &gx3, &seven);

    /* Compute Gy² */
    secp256k1_fe_sqr(&a, &gy);

    test_case("Curve: Gy^2 = Gx^3 + 7");
    if (secp256k1_fe_equal(&a, &rhs)) {
        test_pass();
    } else {
        test_fail("Curve equation doesn't hold for generator point");
    }
}

int main(void)
{
    test_suite_begin("secp256k1 Field Arithmetic Tests");

    test_fe_zero_one();
    test_fe_set_bytes();
    test_fe_add();
    test_fe_sub();
    test_fe_neg();
    test_fe_mul();
    test_fe_sqr();
    test_fe_inv();
    test_fe_sqrt();

    test_suite_end();
    return test_global_summary();
}
