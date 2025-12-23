/*
 * Bitcoin Echo — Serialization Test Vectors
 *
 * Test vectors for CompactSize (varint) encoding/decoding.
 * Tests cover edge cases at encoding boundaries and error conditions.
 *
 * Build once. Build right. Stop.
 */

#include "serialize.h"
#include "test_utils.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>


/*
 * Test varint_size computation.
 */
static void test_varint_size(const char *name, uint64_t value,
                             size_t expected_size)
{
    size_t result;

    result = varint_size(value);

    test_case(name);
    if (result == expected_size) {
        test_pass();
    } else {
        test_fail(name);
        printf("    Value: %llu\n", (unsigned long long)value);
        printf("    Expected size: %zu\n", expected_size);
        printf("    Got size: %zu\n", result);
    }
}

/*
 * Test varint encoding.
 */
static void test_varint_write(const char *name, uint64_t value,
                              const uint8_t *expected, size_t expected_len)
{
    uint8_t buf[16];
    size_t written;
    echo_result_t result;

    memset(buf, 0xAA, sizeof(buf));  /* Fill with sentinel */
    result = varint_write(buf, sizeof(buf), value, &written);

    test_case(name);
    if (result == ECHO_OK && written == expected_len &&
        bytes_equal(buf, expected, expected_len)) {
        test_pass();
    } else {
        test_fail(name);
        printf("    Value: %llu\n", (unsigned long long)value);
        printf("    Result: %d\n", result);
        printf("    Expected (%zu bytes): ", expected_len);
        print_hex(expected, expected_len);
        
        printf("    Got (%zu bytes): ", written);
        print_hex(buf, written);
        
    }
}

/*
 * Test varint decoding.
 */
static void test_varint_read(const char *name, const uint8_t *input,
                             size_t input_len, uint64_t expected_value,
                             size_t expected_consumed)
{
    uint64_t value;
    size_t consumed;
    echo_result_t result;

    result = varint_read(input, input_len, &value, &consumed);

    test_case(name);
    if (result == ECHO_OK && value == expected_value &&
        consumed == expected_consumed) {
        test_pass();
    } else {
        test_fail(name);
        printf("    Input: ");
        print_hex(input, input_len);
        
        printf("    Result: %d\n", result);
        printf("    Expected: value=%llu, consumed=%zu\n",
               (unsigned long long)expected_value, expected_consumed);
        printf("    Got: value=%llu, consumed=%zu\n",
               (unsigned long long)value, consumed);
    }
}

/*
 * Test round-trip encoding/decoding.
 */
static void test_varint_roundtrip(const char *name, uint64_t value)
{
    uint8_t buf[16];
    size_t written, consumed;
    uint64_t decoded;
    echo_result_t result;

    test_case(name);

    result = varint_write(buf, sizeof(buf), value, &written);
    if (result != ECHO_OK) {
        test_fail("write failed");
        return;
    }

    result = varint_read(buf, written, &decoded, &consumed);
    if (result != ECHO_OK) {
        test_fail("read failed");
        return;
    }

    if (decoded == value && consumed == written) {
        test_pass();
    } else {
        test_fail(name);
        printf("    Original: %llu\n", (unsigned long long)value);
        printf("    Decoded: %llu\n", (unsigned long long)decoded);
        printf("    Written: %zu, Consumed: %zu\n", written, consumed);
    }
}

/*
 * Test error handling.
 */
static void test_varint_error(const char *name, const uint8_t *input,
                              size_t input_len, echo_result_t expected_result)
{
    uint64_t value;
    size_t consumed;
    echo_result_t result;

    result = varint_read(input, input_len, &value, &consumed);

    test_case(name);
    if (result == expected_result) {
        test_pass();
    } else {
        test_fail(name);
        printf("    Expected result: %d\n", expected_result);
        printf("    Got result: %d\n", result);
    }
}

/*
 * Test non-strict reading of non-canonical encoding.
 */
static void test_varint_nonstrict(const char *name, const uint8_t *input,
                                  size_t input_len, uint64_t expected_value,
                                  size_t expected_consumed)
{
    uint64_t value;
    size_t consumed;
    echo_result_t result;

    result = varint_read_nonstrict(input, input_len, &value, &consumed);

    test_case(name);
    if (result == ECHO_OK && value == expected_value &&
        consumed == expected_consumed) {
        test_pass();
    } else {
        test_fail(name);
        printf("    Input: ");
        print_hex(input, input_len);
        
        printf("    Result: %d\n", result);
        printf("    Expected: value=%llu, consumed=%zu\n",
               (unsigned long long)expected_value, expected_consumed);
        printf("    Got: value=%llu, consumed=%zu\n",
               (unsigned long long)value, consumed);
    }
}

int main(void)
{
    test_suite_begin("Serialization Tests");

    /* Test varint_size */
    test_section("varint_size tests");
    test_varint_size("size of 0", 0, 1);
    test_varint_size("size of 1", 1, 1);
    test_varint_size("size of 252 (max 1-byte)", 252, 1);
    test_varint_size("size of 253 (min 3-byte)", 253, 3);
    test_varint_size("size of 65535 (max 3-byte)", 65535, 3);
    test_varint_size("size of 65536 (min 5-byte)", 65536, 5);
    test_varint_size("size of 4294967295 (max 5-byte)", 4294967295ULL, 5);
    test_varint_size("size of 4294967296 (min 9-byte)", 4294967296ULL, 9);
    test_varint_size("size of UINT64_MAX", UINT64_MAX, 9);
    

    /* Test varint_write - single byte values */
    test_section("varint_write tests");
    {
        uint8_t exp0[] = {0x00};
        uint8_t exp1[] = {0x01};
        uint8_t exp252[] = {0xFC};
        test_varint_write("encode 0", 0, exp0, 1);
        test_varint_write("encode 1", 1, exp1, 1);
        test_varint_write("encode 252", 252, exp252, 1);
    }
    

    /* Test varint_write - 3-byte values */
    test_section("varint_write tests");
    {
        uint8_t exp253[] = {0xFD, 0xFD, 0x00};
        uint8_t exp254[] = {0xFD, 0xFE, 0x00};
        uint8_t exp255[] = {0xFD, 0xFF, 0x00};
        uint8_t exp256[] = {0xFD, 0x00, 0x01};
        uint8_t exp65535[] = {0xFD, 0xFF, 0xFF};
        test_varint_write("encode 253", 253, exp253, 3);
        test_varint_write("encode 254", 254, exp254, 3);
        test_varint_write("encode 255", 255, exp255, 3);
        test_varint_write("encode 256", 256, exp256, 3);
        test_varint_write("encode 65535", 65535, exp65535, 3);
    }
    

    /* Test varint_write - 5-byte values */
    test_section("varint_write tests");
    {
        uint8_t exp65536[] = {0xFE, 0x00, 0x00, 0x01, 0x00};
        uint8_t exp_max32[] = {0xFE, 0xFF, 0xFF, 0xFF, 0xFF};
        test_varint_write("encode 65536", 65536, exp65536, 5);
        test_varint_write("encode 4294967295", 4294967295ULL, exp_max32, 5);
    }
    

    /* Test varint_write - 9-byte values */
    test_section("varint_write tests");
    {
        uint8_t exp_4G[] = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
        uint8_t exp_max[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        test_varint_write("encode 4294967296", 4294967296ULL, exp_4G, 9);
        test_varint_write("encode UINT64_MAX", UINT64_MAX, exp_max, 9);
    }
    

    /* Test varint_read - single byte values */
    test_section("varint_read tests");
    {
        uint8_t in0[] = {0x00};
        uint8_t in1[] = {0x01};
        uint8_t in252[] = {0xFC};
        test_varint_read("decode 0", in0, 1, 0, 1);
        test_varint_read("decode 1", in1, 1, 1, 1);
        test_varint_read("decode 252", in252, 1, 252, 1);
    }
    

    /* Test varint_read - 3-byte values */
    test_section("varint_read tests");
    {
        uint8_t in253[] = {0xFD, 0xFD, 0x00};
        uint8_t in65535[] = {0xFD, 0xFF, 0xFF};
        test_varint_read("decode 253", in253, 3, 253, 3);
        test_varint_read("decode 65535", in65535, 3, 65535, 3);
    }
    

    /* Test varint_read - 5-byte values */
    test_section("varint_read tests");
    {
        uint8_t in65536[] = {0xFE, 0x00, 0x00, 0x01, 0x00};
        uint8_t in_max32[] = {0xFE, 0xFF, 0xFF, 0xFF, 0xFF};
        test_varint_read("decode 65536", in65536, 5, 65536, 5);
        test_varint_read("decode 4294967295", in_max32, 5, 4294967295ULL, 5);
    }
    

    /* Test varint_read - 9-byte values */
    test_section("varint_read tests");
    {
        uint8_t in_4G[] = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
        uint8_t in_max[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        test_varint_read("decode 4294967296", in_4G, 9, 4294967296ULL, 9);
        test_varint_read("decode UINT64_MAX", in_max, 9, UINT64_MAX, 9);
    }
    

    /* Test round-trip encoding/decoding */
    test_section("Round-trip tests");
    test_varint_roundtrip("roundtrip 0", 0);
    test_varint_roundtrip("roundtrip 127", 127);
    test_varint_roundtrip("roundtrip 252", 252);
    test_varint_roundtrip("roundtrip 253", 253);
    test_varint_roundtrip("roundtrip 1000", 1000);
    test_varint_roundtrip("roundtrip 65535", 65535);
    test_varint_roundtrip("roundtrip 65536", 65536);
    test_varint_roundtrip("roundtrip 1000000", 1000000);
    test_varint_roundtrip("roundtrip 4294967295", 4294967295ULL);
    test_varint_roundtrip("roundtrip 4294967296", 4294967296ULL);
    test_varint_roundtrip("roundtrip UINT64_MAX", UINT64_MAX);
    

    /* Test error conditions */
    test_section("Error handling tests");
    {
        /* Truncated input */
        uint8_t trunc_3[] = {0xFD};  /* Missing 2 bytes */
        uint8_t trunc_5[] = {0xFE, 0x00, 0x00};  /* Missing 2 bytes */
        uint8_t trunc_9[] = {0xFF, 0x00, 0x00, 0x00, 0x00};  /* Missing 4 bytes */
        test_varint_error("truncated 3-byte", trunc_3, 1, ECHO_ERR_TRUNCATED);
        test_varint_error("truncated 5-byte", trunc_5, 3, ECHO_ERR_TRUNCATED);
        test_varint_error("truncated 9-byte", trunc_9, 5, ECHO_ERR_TRUNCATED);

        /* Non-canonical encoding (strict mode should reject) */
        uint8_t noncanon_253_small[] = {0xFD, 0x00, 0x00};  /* 0 encoded as 3-byte */
        uint8_t noncanon_253_max[] = {0xFD, 0xFC, 0x00};    /* 252 encoded as 3-byte */
        uint8_t noncanon_32_small[] = {0xFE, 0xFF, 0xFF, 0x00, 0x00};  /* 65535 as 5-byte */
        uint8_t noncanon_64_small[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};  /* 4294967295 as 9-byte */
        test_varint_error("non-canonical 0 as 3-byte", noncanon_253_small, 3, ECHO_ERR_INVALID_FORMAT);
        test_varint_error("non-canonical 252 as 3-byte", noncanon_253_max, 3, ECHO_ERR_INVALID_FORMAT);
        test_varint_error("non-canonical 65535 as 5-byte", noncanon_32_small, 5, ECHO_ERR_INVALID_FORMAT);
        test_varint_error("non-canonical 4294967295 as 9-byte", noncanon_64_small, 9, ECHO_ERR_INVALID_FORMAT);
    }
    

    /* Test non-strict mode accepts non-canonical encoding */
    test_section("Non-strict mode tests");
    {
        uint8_t noncanon_0[] = {0xFD, 0x00, 0x00};  /* 0 encoded as 3-byte */
        uint8_t noncanon_252[] = {0xFD, 0xFC, 0x00};  /* 252 encoded as 3-byte */
        test_varint_nonstrict("non-strict 0 as 3-byte", noncanon_0, 3, 0, 3);
        test_varint_nonstrict("non-strict 252 as 3-byte", noncanon_252, 3, 252, 3);
    }

    test_suite_end();
    return test_global_summary();
}
