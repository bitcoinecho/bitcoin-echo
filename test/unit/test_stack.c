/*
 * Bitcoin Echo — Script Stack Machine Tests
 *
 * Test vectors for stack operations, number encoding/decoding,
 * and boolean conversions.
 *
 * Build once. Build right. Stop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script.h"
#include "test_utils.h"

/*
 * Print a byte array as hex.
 */
static void print_hex(const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

/*
 * Test stack initialization and basic operations.
 */
static void test_stack_init(const char *name)
{
    script_stack_t stack;
    echo_result_t res;

    res = stack_init(&stack);

    if (res == ECHO_OK && stack_size(&stack) == 0 && stack_empty(&stack)) {
        test_case(name);
        test_pass();
        stack_free(&stack);
    } else {
        test_fail(name);
        printf("    Result: %d, size: %zu, empty: %d\n",
               res, stack_size(&stack), stack_empty(&stack));
    }
}

/*
 * Test pushing and popping bytes.
 */
static void test_push_pop(const char *name, const uint8_t *data, size_t len)
{
    script_stack_t stack;
    stack_element_t elem;
    echo_result_t res;


    stack_init(&stack);
    res = stack_push(&stack, data, len);

    if (res != ECHO_OK || stack_size(&stack) != 1) {
        test_case(name);
        test_fail("push failed");
        stack_free(&stack);
        return;
    }

    res = stack_pop(&stack, &elem);

    if (res == ECHO_OK && elem.len == len &&
        (len == 0 || memcmp(elem.data, data, len) == 0)) {
        test_case(name);
            test_pass();
    } else {
        test_fail(name);
        printf("    Expected len: %zu, got: %zu\n", len, elem.len);
    }

    if (elem.data != NULL) free(elem.data);
    stack_free(&stack);
}

/*
 * Test number encoding.
 */
static void test_num_encode(const char *name, script_num_t num,
                            const uint8_t *expected, size_t expected_len)
{
    uint8_t buf[9];
    size_t len;
    echo_result_t res;

    res = script_num_encode(num, buf, &len);

    if (res == ECHO_OK && len == expected_len &&
        (expected_len == 0 || memcmp(buf, expected, expected_len) == 0)) {
        test_case(name);
            test_pass();
    } else {
        test_fail(name);
        printf("    Number: %lld\n", (long long)num);
        printf("    Expected (%zu bytes): ", expected_len);
        print_hex(expected, expected_len);
        printf("\n    Got (%zu bytes): ", len);
        print_hex(buf, len);
        printf("\n");
    }
}

/*
 * Test number decoding.
 */
static void test_num_decode(const char *name, const uint8_t *data, size_t len,
                            script_num_t expected, echo_bool_t should_succeed)
{
    script_num_t num;
    echo_result_t res;

    res = script_num_decode(data, len, &num, ECHO_TRUE, SCRIPT_NUM_MAX_SIZE);

    if (should_succeed) {
        if (res == ECHO_OK && num == expected) {
            test_case(name);
            test_pass();
        } else {
            test_fail(name);
            printf("    Input: ");
            print_hex(data, len);
            printf("\n    Expected: %lld, got: %lld (res=%d)\n",
                   (long long)expected, (long long)num, res);
        }
    } else {
        if (res != ECHO_OK) {
            test_case(name);
            test_pass();
        } else {
            test_case(name);
            test_fail("should have failed");
            printf("    Got: %lld\n", (long long)num);
        }
    }
}

/*
 * Test number round-trip.
 */
static void test_num_roundtrip(const char *name, script_num_t num)
{
    uint8_t buf[9];
    size_t len;
    script_num_t decoded;
    echo_result_t res;


    res = script_num_encode(num, buf, &len);
    if (res != ECHO_OK) {
        test_case(name);
        test_fail("encode failed");
        return;
    }

    res = script_num_decode(buf, len, &decoded, ECHO_TRUE, 8);
    if (res == ECHO_OK && decoded == num) {
        test_case(name);
            test_pass();
    } else {
        test_fail(name);
        printf("    Original: %lld, decoded: %lld\n",
               (long long)num, (long long)decoded);
    }
}

/*
 * Test boolean conversion.
 */
static void test_bool(const char *name, const uint8_t *data, size_t len,
                      echo_bool_t expected)
{
    echo_bool_t result;

    result = script_bool(data, len);

    if (result == expected) {
        test_case(name);
            test_pass();
    } else {
        test_fail(name);
        printf("    Data: ");
        print_hex(data, len);
        printf("\n    Expected: %s, got: %s\n",
               expected ? "true" : "false",
               result ? "true" : "false");
    }
}

/*
 * Test stack operation and verify result.
 */
static void test_stack_op(const char *name, echo_result_t (*op)(script_stack_t *),
                          size_t initial_count, size_t expected_count,
                          echo_bool_t should_succeed)
{
    script_stack_t stack;
    echo_result_t res;

    stack_init(&stack);

    /* Push initial elements */
    for (size_t i = 0; i < initial_count; i++) {
        uint8_t val = (uint8_t)(i + 1);
        stack_push(&stack, &val, 1);
    }

    res = op(&stack);

    if (should_succeed) {
        if (res == ECHO_OK && stack_size(&stack) == expected_count) {
            test_case(name);
            test_pass();
        } else {
            test_fail(name);
            printf("    Result: %d, expected size: %zu, got: %zu\n",
                   res, expected_count, stack_size(&stack));
        }
    } else {
        if (res != ECHO_OK) {
            test_case(name);
            test_pass();
        } else {
            test_case(name);
            test_fail("should have failed");
        }
    }

    stack_free(&stack);
}

/*
 * Test context initialization.
 */
static void test_context_init(const char *name)
{
    script_context_t ctx;
    echo_result_t res;

    res = script_context_init(&ctx, SCRIPT_VERIFY_NONE);

    if (res == ECHO_OK &&
        stack_size(&ctx.stack) == 0 &&
        stack_size(&ctx.altstack) == 0 &&
        ctx.error == SCRIPT_ERR_OK &&
        ctx.op_count == 0) {
        test_case(name);
            test_pass();
        script_context_free(&ctx);
    } else {
        test_fail(name);
    }
}

/*
 * Test OP_DUP behavior.
 */
static void test_dup(const char *name)
{
    script_stack_t stack;
    const stack_element_t *elem;

    stack_init(&stack);

    uint8_t data[] = {0xab, 0xcd};
    stack_push(&stack, data, 2);
    stack_dup(&stack);

    if (stack_size(&stack) == 2) {
        stack_peek_at(&stack, 0, &elem);
        if (elem->len == 2 && elem->data[0] == 0xab && elem->data[1] == 0xcd) {
            stack_peek_at(&stack, 1, &elem);
            if (elem->len == 2 && elem->data[0] == 0xab && elem->data[1] == 0xcd) {
                test_case(name);
            test_pass();
                stack_free(&stack);
                return;
            }
        }
    }

    test_fail(name);
    stack_free(&stack);
}

/*
 * Test OP_SWAP behavior.
 */
static void test_swap(const char *name)
{
    script_stack_t stack;
    const stack_element_t *elem;

    stack_init(&stack);

    uint8_t a = 1, b = 2;
    stack_push(&stack, &a, 1);
    stack_push(&stack, &b, 1);
    stack_swap(&stack);

    stack_peek_at(&stack, 0, &elem);
    if (elem->len == 1 && elem->data[0] == 1) {
        stack_peek_at(&stack, 1, &elem);
        if (elem->len == 1 && elem->data[0] == 2) {
            test_case(name);
            test_pass();
            stack_free(&stack);
            return;
        }
    }

    test_fail(name);
    stack_free(&stack);
}

/*
 * Test OP_ROT behavior: (x1 x2 x3 -- x2 x3 x1)
 */
static void test_rot(const char *name)
{
    script_stack_t stack;
    const stack_element_t *elem;

    stack_init(&stack);

    uint8_t a = 1, b = 2, c = 3;
    stack_push(&stack, &a, 1);  /* Bottom */
    stack_push(&stack, &b, 1);
    stack_push(&stack, &c, 1);  /* Top */
    stack_rot(&stack);

    /* After ROT: (2 3 1) with 1 on top */
    stack_peek_at(&stack, 0, &elem);  /* Top should be 1 */
    if (elem->data[0] != 1) goto fail;

    stack_peek_at(&stack, 1, &elem);  /* Second should be 3 */
    if (elem->data[0] != 3) goto fail;

    stack_peek_at(&stack, 2, &elem);  /* Bottom should be 2 */
    if (elem->data[0] != 2) goto fail;

        test_case(name);
            test_pass();
    stack_free(&stack);
    return;

fail:
    test_fail(name);
    stack_free(&stack);
}

/*
 * Test pick operation.
 */
static void test_pick(const char *name)
{
    script_stack_t stack;
    const stack_element_t *elem;

    stack_init(&stack);

    uint8_t a = 1, b = 2, c = 3;
    stack_push(&stack, &a, 1);
    stack_push(&stack, &b, 1);
    stack_push(&stack, &c, 1);

    /* Pick element at index 2 (bottom element, value 1) */
    stack_pick(&stack, 2);

    if (stack_size(&stack) == 4) {
        stack_peek(&stack, &elem);
        if (elem->len == 1 && elem->data[0] == 1) {
            test_case(name);
            test_pass();
            stack_free(&stack);
            return;
        }
    }

    test_fail(name);
    stack_free(&stack);
}

/*
 * Test roll operation.
 */
static void test_roll(const char *name)
{
    script_stack_t stack;
    const stack_element_t *elem;

    stack_init(&stack);

    uint8_t a = 1, b = 2, c = 3;
    stack_push(&stack, &a, 1);
    stack_push(&stack, &b, 1);
    stack_push(&stack, &c, 1);

    /* Roll element at index 2 (bottom element, value 1) to top */
    stack_roll(&stack, 2);

    /* Stack should now be: 2, 3, 1 (bottom to top) */
    if (stack_size(&stack) == 3) {
        stack_peek_at(&stack, 0, &elem);  /* Top */
        if (elem->data[0] != 1) goto fail;

        stack_peek_at(&stack, 1, &elem);  /* Middle */
        if (elem->data[0] != 3) goto fail;

        stack_peek_at(&stack, 2, &elem);  /* Bottom */
        if (elem->data[0] != 2) goto fail;

        test_case(name);
            test_pass();
        stack_free(&stack);
        return;
    }

fail:
    test_fail(name);
    stack_free(&stack);
}

/*
 * Test 2swap operation: (x1 x2 x3 x4 -- x3 x4 x1 x2)
 */
static void test_2swap(const char *name)
{
    script_stack_t stack;
    const stack_element_t *elem;

    stack_init(&stack);

    uint8_t a = 1, b = 2, c = 3, d = 4;
    stack_push(&stack, &a, 1);
    stack_push(&stack, &b, 1);
    stack_push(&stack, &c, 1);
    stack_push(&stack, &d, 1);

    stack_2swap(&stack);

    /* Stack should now be: 3, 4, 1, 2 (bottom to top) */
    stack_peek_at(&stack, 0, &elem); if (elem->data[0] != 2) goto fail;
    stack_peek_at(&stack, 1, &elem); if (elem->data[0] != 1) goto fail;
    stack_peek_at(&stack, 2, &elem); if (elem->data[0] != 4) goto fail;
    stack_peek_at(&stack, 3, &elem); if (elem->data[0] != 3) goto fail;

        test_case(name);
            test_pass();
    stack_free(&stack);
    return;

fail:
    test_fail(name);
    stack_free(&stack);
}

/*
 * Test pushing/popping numbers.
 */
static void test_push_pop_num(const char *name, script_num_t num)
{
    script_stack_t stack;
    script_num_t popped;
    echo_result_t res;

    stack_init(&stack);

    res = stack_push_num(&stack, num);
    if (res != ECHO_OK) {
        test_case(name);
        test_fail("push_num failed");
        stack_free(&stack);
        return;
    }

    res = stack_pop_num(&stack, &popped, ECHO_TRUE, SCRIPT_NUM_MAX_SIZE);
    if (res == ECHO_OK && popped == num) {
        test_case(name);
            test_pass();
    } else {
        test_fail(name);
        printf("    Pushed: %lld, popped: %lld\n",
               (long long)num, (long long)popped);
    }

    stack_free(&stack);
}

/*
 * Test error string.
 */
static void test_error_string(const char *name, script_error_t err)
{
    const char *str = script_error_string(err);

    test_case(name);
    if (str != NULL && strlen(str) > 0) {
        test_pass();
        printf("    (%s)\n", str);
    } else {
        test_fail("error string empty or NULL");
    }
}

int main(void)
{
    test_suite_begin("Script Stack Machine Tests");
    

    /* Stack initialization */
    test_section("Stack initialization tests");
    test_stack_init("stack init");
    test_context_init("context init");

    /* Push/pop bytes */
    test_section("Push/pop tests");
    {
        uint8_t one[] = {0x01};
        uint8_t multi[] = {0xde, 0xad, 0xbe, 0xef};

        test_push_pop("push/pop empty", NULL, 0);
        test_push_pop("push/pop single byte", one, 1);
        test_push_pop("push/pop multi byte", multi, sizeof(multi));
    }

    /* Number encoding */
    test_section("Number encoding tests");
    {
        uint8_t exp_zero[] = {0};  /* Empty */
        uint8_t exp_1[] = {0x01};
        uint8_t exp_127[] = {0x7f};
        uint8_t exp_128[] = {0x80, 0x00};  /* Need sign byte */
        uint8_t exp_255[] = {0xff, 0x00};  /* Need sign byte */
        uint8_t exp_256[] = {0x00, 0x01};
        uint8_t exp_neg1[] = {0x81};       /* 1 with sign bit */
        uint8_t exp_neg127[] = {0xff};     /* 127 with sign bit */
        uint8_t exp_neg128[] = {0x80, 0x80};  /* 128 with sign byte */

        test_num_encode("encode 0", 0, exp_zero, 0);
        test_num_encode("encode 1", 1, exp_1, 1);
        test_num_encode("encode 127", 127, exp_127, 1);
        test_num_encode("encode 128", 128, exp_128, 2);
        test_num_encode("encode 255", 255, exp_255, 2);
        test_num_encode("encode 256", 256, exp_256, 2);
        test_num_encode("encode -1", -1, exp_neg1, 1);
        test_num_encode("encode -127", -127, exp_neg127, 1);
        test_num_encode("encode -128", -128, exp_neg128, 2);
    }

    /* Number decoding */
    test_section("Number decoding tests");
    {
        uint8_t in_1[] = {0x01};
        uint8_t in_127[] = {0x7f};
        uint8_t in_128[] = {0x80, 0x00};
        uint8_t in_neg1[] = {0x81};
        uint8_t in_neg128[] = {0x80, 0x80};
        uint8_t non_minimal[] = {0x01, 0x00};  /* 1 with unnecessary zero */

        test_num_decode("decode empty -> 0", NULL, 0, 0, ECHO_TRUE);
        test_num_decode("decode 0x01 -> 1", in_1, 1, 1, ECHO_TRUE);
        test_num_decode("decode 0x7f -> 127", in_127, 1, 127, ECHO_TRUE);
        test_num_decode("decode 0x8000 -> 128", in_128, 2, 128, ECHO_TRUE);
        test_num_decode("decode 0x81 -> -1", in_neg1, 1, -1, ECHO_TRUE);
        test_num_decode("decode 0x8080 -> -128", in_neg128, 2, -128, ECHO_TRUE);
        test_num_decode("reject non-minimal", non_minimal, 2, 0, ECHO_FALSE);
    }

    /* Number round-trip */
    test_section("Number round-trip tests");
    test_num_roundtrip("roundtrip 0", 0);
    test_num_roundtrip("roundtrip 1", 1);
    test_num_roundtrip("roundtrip -1", -1);
    test_num_roundtrip("roundtrip 127", 127);
    test_num_roundtrip("roundtrip 128", 128);
    test_num_roundtrip("roundtrip -128", -128);
    test_num_roundtrip("roundtrip 32767", 32767);
    test_num_roundtrip("roundtrip -32768", -32768);
    test_num_roundtrip("roundtrip INT32_MAX", 2147483647LL);
    test_num_roundtrip("roundtrip INT32_MIN", -2147483648LL);

    /* Boolean conversion */
    test_section("Boolean conversion tests");
    {
        uint8_t zero_1[] = {0x00};
        uint8_t zero_2[] = {0x00, 0x00};
        uint8_t neg_zero[] = {0x80};  /* Negative zero */
        uint8_t one[] = {0x01};
        uint8_t neg_one[] = {0x81};
        uint8_t nonzero[] = {0x00, 0x01};

        test_bool("empty is false", NULL, 0, ECHO_FALSE);
        test_bool("0x00 is false", zero_1, 1, ECHO_FALSE);
        test_bool("0x0000 is false", zero_2, 2, ECHO_FALSE);
        test_bool("0x80 (neg zero) is false", neg_zero, 1, ECHO_FALSE);
        test_bool("0x01 is true", one, 1, ECHO_TRUE);
        test_bool("0x81 is true", neg_one, 1, ECHO_TRUE);
        test_bool("0x0001 is true", nonzero, 2, ECHO_TRUE);
    }

    /* Stack operations */
    test_section("Stack operation tests");
    test_dup("OP_DUP");
    test_swap("OP_SWAP");
    test_rot("OP_ROT");
    test_pick("OP_PICK");
    test_roll("OP_ROLL");
    test_2swap("OP_2SWAP");
    test_stack_op("OP_DROP (1 elem)", stack_drop, 1, 0, ECHO_TRUE);
    test_stack_op("OP_DROP (empty)", stack_drop, 0, 0, ECHO_FALSE);
    test_stack_op("OP_DUP (empty)", stack_dup, 0, 0, ECHO_FALSE);
    test_stack_op("OP_2DUP (2 elem)", stack_2dup, 2, 4, ECHO_TRUE);
    test_stack_op("OP_2DUP (1 elem)", stack_2dup, 1, 1, ECHO_FALSE);
    test_stack_op("OP_3DUP (3 elem)", stack_3dup, 3, 6, ECHO_TRUE);
    test_stack_op("OP_2DROP (2 elem)", stack_2drop, 2, 0, ECHO_TRUE);
    test_stack_op("OP_2DROP (1 elem)", stack_2drop, 1, 1, ECHO_FALSE);
    test_stack_op("OP_OVER (2 elem)", stack_over, 2, 3, ECHO_TRUE);
    test_stack_op("OP_NIP (2 elem)", stack_nip, 2, 1, ECHO_TRUE);
    test_stack_op("OP_TUCK (2 elem)", stack_tuck, 2, 3, ECHO_TRUE);
    test_stack_op("OP_2OVER (4 elem)", stack_2over, 4, 6, ECHO_TRUE);
    test_stack_op("OP_2ROT (6 elem)", stack_2rot, 6, 6, ECHO_TRUE);

    /* Push/pop numbers via stack */
    test_section("Stack number tests");
    test_push_pop_num("push/pop 0", 0);
    test_push_pop_num("push/pop 1", 1);
    test_push_pop_num("push/pop -1", -1);
    test_push_pop_num("push/pop 1000", 1000);
    test_push_pop_num("push/pop -1000", -1000);

    /* Error strings */
    test_section("Error string tests");
    test_error_string("SCRIPT_ERR_OK", SCRIPT_ERR_OK);
    test_error_string("SCRIPT_ERR_EVAL_FALSE", SCRIPT_ERR_EVAL_FALSE);
    test_error_string("SCRIPT_ERR_DISABLED_OPCODE", SCRIPT_ERR_DISABLED_OPCODE);
    test_error_string("SCRIPT_ERR_INVALID_STACK_OPERATION", SCRIPT_ERR_INVALID_STACK_OPERATION);

    test_suite_end();
    return test_global_summary();
}
