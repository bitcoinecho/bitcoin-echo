/*
 * Bitcoin Echo — Script Test Vectors
 *
 * Test vectors for script type detection, opcode parsing, and
 * helper functions. Tests cover all standard output types and
 * edge cases.
 *
 * Build once. Build right. Stop.
 */

#include <stdio.h>
#include "test_utils.h"
#include <string.h>
#include "script.h"


/*
 * Test script type classification.
 */
static void test_classify(const char *name, const uint8_t *script,
                          size_t len, script_type_t expected)
{
    script_type_t result;

    result = script_classify(script, len);

    if (result == expected) {
        test_pass();
        test_case(name);
        test_pass();
    } else {
        test_case(name);
        test_fail(name);
        printf("    Script: ");
        print_hex(script, len);
        printf("\n");
        printf("    Expected type: %d\n", expected);
        printf("    Got type: %d\n", result);
    }
}

/*
 * Test P2PKH detection.
 */
static void test_p2pkh(const char *name, const uint8_t *script, size_t len,
                       echo_bool_t expected, const uint8_t *expected_hash)
{
    hash160_t hash;
    echo_bool_t result;

    memset(&hash, 0, sizeof(hash));
    result = script_is_p2pkh(script, len, &hash);

    if (result == expected) {
        if (expected == ECHO_TRUE && expected_hash != NULL) {
            if (memcmp(hash.bytes, expected_hash, 20) == 0) {
                test_pass();
                test_case(name);
        test_pass();
            } else {
                test_case(name);
        test_fail(name);
                printf("    Expected hash: ");
                print_hex(expected_hash, 20);
                printf("\n");
                printf("    Got hash: ");
                print_hex(hash.bytes, 20);
                printf("\n");
            }
        } else {
            test_pass();
            test_case(name);
        test_pass();
        }
    } else {
        test_case(name);
        test_fail(name);
        printf("    Script: ");
        print_hex(script, len);
        printf("\n");
        printf("    Expected: %s\n", expected ? "true" : "false");
        printf("    Got: %s\n", result ? "true" : "false");
    }
}

/*
 * Test P2SH detection.
 */
static void test_p2sh(const char *name, const uint8_t *script, size_t len,
                      echo_bool_t expected, const uint8_t *expected_hash)
{
    hash160_t hash;
    echo_bool_t result;

    memset(&hash, 0, sizeof(hash));
    result = script_is_p2sh(script, len, &hash);

    if (result == expected) {
        if (expected == ECHO_TRUE && expected_hash != NULL) {
            if (memcmp(hash.bytes, expected_hash, 20) == 0) {
                test_pass();
                test_case(name);
        test_pass();
            } else {
                test_case(name);
        test_fail(name);
            }
        } else {
            test_pass();
            test_case(name);
        test_pass();
        }
    } else {
        test_case(name);
        test_fail(name);
        printf("    Expected: %s\n", expected ? "true" : "false");
        printf("    Got: %s\n", result ? "true" : "false");
    }
}

/*
 * Test P2WPKH detection.
 */
static void test_p2wpkh(const char *name, const uint8_t *script, size_t len,
                        echo_bool_t expected)
{
    echo_bool_t result;

    result = script_is_p2wpkh(script, len, NULL);

    if (result == expected) {
        test_pass();
        test_case(name);
        test_pass();
    } else {
        test_case(name);
        test_fail(name);
        printf("    Expected: %s\n", expected ? "true" : "false");
        printf("    Got: %s\n", result ? "true" : "false");
    }
}

/*
 * Test P2WSH detection.
 */
static void test_p2wsh(const char *name, const uint8_t *script, size_t len,
                       echo_bool_t expected)
{
    echo_bool_t result;

    result = script_is_p2wsh(script, len, NULL);

    if (result == expected) {
        test_pass();
        test_case(name);
        test_pass();
    } else {
        test_case(name);
        test_fail(name);
        printf("    Expected: %s\n", expected ? "true" : "false");
        printf("    Got: %s\n", result ? "true" : "false");
    }
}

/*
 * Test P2TR detection.
 */
static void test_p2tr(const char *name, const uint8_t *script, size_t len,
                      echo_bool_t expected)
{
    echo_bool_t result;

    result = script_is_p2tr(script, len, NULL);

    if (result == expected) {
        test_pass();
        test_case(name);
        test_pass();
    } else {
        test_case(name);
        test_fail(name);
        printf("    Expected: %s\n", expected ? "true" : "false");
        printf("    Got: %s\n", result ? "true" : "false");
    }
}

/*
 * Test OP_RETURN detection.
 */
static void test_op_return(const char *name, const uint8_t *script, size_t len,
                           echo_bool_t expected)
{
    echo_bool_t result;

    result = script_is_op_return(script, len);

    if (result == expected) {
        test_pass();
        test_case(name);
        test_pass();
    } else {
        test_case(name);
        test_fail(name);
        printf("    Expected: %s\n", expected ? "true" : "false");
        printf("    Got: %s\n", result ? "true" : "false");
    }
}

/*
 * Test opcode name.
 */
static void test_opcode_name(const char *name, script_opcode_t op,
                             const char *expected_name)
{
    const char *result;

    result = script_opcode_name(op);

    if (strcmp(result, expected_name) == 0) {
        test_pass();
        test_case(name);
        test_pass();
    } else {
        test_case(name);
        test_fail(name);
        printf("    Opcode: 0x%02x\n", op);
        printf("    Expected: %s\n", expected_name);
        printf("    Got: %s\n", result);
    }
}

/*
 * Test opcode disabled check.
 */
static void test_opcode_disabled(const char *name, script_opcode_t op,
                                 echo_bool_t expected)
{
    echo_bool_t result;

    result = script_opcode_disabled(op);

    if (result == expected) {
        test_pass();
        test_case(name);
        test_pass();
    } else {
        test_case(name);
        test_fail(name);
        printf("    Opcode: 0x%02x (%s)\n", op, script_opcode_name(op));
        printf("    Expected disabled: %s\n", expected ? "true" : "false");
        printf("    Got: %s\n", result ? "true" : "false");
    }
}

/*
 * Test script iterator.
 */
static void test_iterator(const char *name, const uint8_t *script, size_t len,
                          size_t expected_ops)
{
    script_iter_t iter;
    script_op_t op;
    size_t count = 0;

    script_iter_init(&iter, script, len);

    while (script_iter_next(&iter, &op)) {
        count++;
    }

    if (!script_iter_error(&iter) && count == expected_ops) {
        test_pass();
        test_case(name);
        test_pass();
    } else {
        test_case(name);
        test_fail(name);
        printf("    Script: ");
        print_hex(script, len);
        printf("\n");
        printf("    Expected ops: %zu\n", expected_ops);
        printf("    Got ops: %zu\n", count);
        printf("    Error: %s\n", script_iter_error(&iter) ? "yes" : "no");
    }
}

/*
 * Test sigops counting.
 */
static void test_sigops(const char *name, const uint8_t *script, size_t len,
                        size_t expected_count)
{
    size_t result;

    result = script_sigops_count(script, len, ECHO_TRUE);

    if (result == expected_count) {
        test_pass();
        test_case(name);
        test_pass();
    } else {
        test_case(name);
        test_fail(name);
        printf("    Expected sigops: %zu\n", expected_count);
        printf("    Got sigops: %zu\n", result);
    }
}

/*
 * Test push size calculation.
 */
static void test_push_size(const char *name, size_t data_len,
                           size_t expected_size)
{
    size_t result;

    result = script_push_size(data_len);

    if (result == expected_size) {
        test_pass();
        test_case(name);
        test_pass();
    } else {
        test_case(name);
        test_fail(name);
        printf("    Data length: %zu\n", data_len);
        printf("    Expected push size: %zu\n", expected_size);
        printf("    Got push size: %zu\n", result);
    }
}

int main(void)
{
    test_suite_begin("Script Tests");

    /* P2PKH test vectors */
    test_section("P2PKH detection tests");
    {
        /* Standard P2PKH: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG */
        uint8_t p2pkh[] = {
            0x76, 0xa9, 0x14,  /* OP_DUP OP_HASH160 PUSH_20 */
            0x89, 0xab, 0xcd, 0xef, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba,  /* 20-byte hash */
            0x88, 0xac  /* OP_EQUALVERIFY OP_CHECKSIG */
        };
        uint8_t expected_hash[] = {
            0x89, 0xab, 0xcd, 0xef, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba
        };
        test_p2pkh("valid P2PKH", p2pkh, sizeof(p2pkh), ECHO_TRUE, expected_hash);
        test_classify("classify P2PKH", p2pkh, sizeof(p2pkh), SCRIPT_TYPE_P2PKH);

        /* Too short */
        test_p2pkh("too short", p2pkh, 24, ECHO_FALSE, NULL);

        /* Wrong prefix */
        uint8_t wrong_prefix[] = {
            0x75, 0xa9, 0x14,  /* Wrong first byte */
            0x89, 0xab, 0xcd, 0xef, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba,
            0x88, 0xac
        };
        test_p2pkh("wrong prefix", wrong_prefix, sizeof(wrong_prefix), ECHO_FALSE, NULL);
    }

    /* P2SH test vectors */
    test_section("P2SH detection tests");
    {
        /* Standard P2SH: OP_HASH160 <20 bytes> OP_EQUAL */
        uint8_t p2sh[] = {
            0xa9, 0x14,  /* OP_HASH160 PUSH_20 */
            0x89, 0xab, 0xcd, 0xef, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba,  /* 20-byte hash */
            0x87  /* OP_EQUAL */
        };
        uint8_t expected_hash[] = {
            0x89, 0xab, 0xcd, 0xef, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba
        };
        test_p2sh("valid P2SH", p2sh, sizeof(p2sh), ECHO_TRUE, expected_hash);
        test_classify("classify P2SH", p2sh, sizeof(p2sh), SCRIPT_TYPE_P2SH);

        /* Too short */
        test_p2sh("too short", p2sh, 22, ECHO_FALSE, NULL);
    }

    /* P2WPKH test vectors */
    test_section("P2WPKH detection tests");
    {
        /* Standard P2WPKH: OP_0 <20 bytes> */
        uint8_t p2wpkh[] = {
            0x00, 0x14,  /* OP_0 PUSH_20 */
            0x89, 0xab, 0xcd, 0xef, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba  /* 20-byte hash */
        };
        test_p2wpkh("valid P2WPKH", p2wpkh, sizeof(p2wpkh), ECHO_TRUE);
        test_classify("classify P2WPKH", p2wpkh, sizeof(p2wpkh), SCRIPT_TYPE_P2WPKH);

        /* Wrong length */
        test_p2wpkh("wrong length", p2wpkh, 21, ECHO_FALSE);
    }

    /* P2WSH test vectors */
    test_section("P2WSH detection tests");
    {
        /* Standard P2WSH: OP_0 <32 bytes> */
        uint8_t p2wsh[] = {
            0x00, 0x20,  /* OP_0 PUSH_32 */
            0x89, 0xab, 0xcd, 0xef, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba  /* 32-byte hash */
        };
        test_p2wsh("valid P2WSH", p2wsh, sizeof(p2wsh), ECHO_TRUE);
        test_classify("classify P2WSH", p2wsh, sizeof(p2wsh), SCRIPT_TYPE_P2WSH);
    }

    /* P2TR test vectors */
    test_section("P2TR detection tests");
    {
        /* Standard P2TR: OP_1 <32 bytes> */
        uint8_t p2tr[] = {
            0x51, 0x20,  /* OP_1 PUSH_32 */
            0x89, 0xab, 0xcd, 0xef, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba  /* 32-byte x-only pubkey */
        };
        test_p2tr("valid P2TR", p2tr, sizeof(p2tr), ECHO_TRUE);
        test_classify("classify P2TR", p2tr, sizeof(p2tr), SCRIPT_TYPE_P2TR);

        /* Wrong version (OP_2 instead of OP_1) */
        uint8_t wrong_version[] = {
            0x52, 0x20,  /* OP_2 PUSH_32 */
            0x89, 0xab, 0xcd, 0xef, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba
        };
        test_p2tr("wrong version (OP_2)", wrong_version, sizeof(wrong_version), ECHO_FALSE);
        test_classify("classify future witness v2", wrong_version, sizeof(wrong_version),
                     SCRIPT_TYPE_WITNESS_UNKNOWN);
    }

    /* OP_RETURN test vectors */
    test_section("OP_RETURN detection tests");
    {
        uint8_t op_return_empty[] = { 0x6a };  /* Just OP_RETURN */
        uint8_t op_return_data[] = { 0x6a, 0x04, 0xde, 0xad, 0xbe, 0xef };
        uint8_t not_op_return[] = { 0x76, 0xa9 };  /* OP_DUP OP_HASH160 */

        test_op_return("empty OP_RETURN", op_return_empty, 1, ECHO_TRUE);
        test_op_return("OP_RETURN with data", op_return_data, sizeof(op_return_data), ECHO_TRUE);
        test_op_return("not OP_RETURN", not_op_return, sizeof(not_op_return), ECHO_FALSE);
        test_classify("classify OP_RETURN", op_return_empty, 1, SCRIPT_TYPE_NULL_DATA);
    }

    /* Opcode name tests */
    test_section("Opcode name tests");
    {
        test_opcode_name("OP_0 name", OP_0, "OP_0");
        test_opcode_name("OP_DUP name", OP_DUP, "OP_DUP");
        test_opcode_name("OP_HASH160 name", OP_HASH160, "OP_HASH160");
        test_opcode_name("OP_CHECKSIG name", OP_CHECKSIG, "OP_CHECKSIG");
        test_opcode_name("OP_CHECKMULTISIG name", OP_CHECKMULTISIG, "OP_CHECKMULTISIG");
        test_opcode_name("OP_CHECKLOCKTIMEVERIFY name", OP_CHECKLOCKTIMEVERIFY, "OP_CHECKLOCKTIMEVERIFY");
        test_opcode_name("OP_CHECKSEQUENCEVERIFY name", OP_CHECKSEQUENCEVERIFY, "OP_CHECKSEQUENCEVERIFY");
        test_opcode_name("OP_CHECKSIGADD name", OP_CHECKSIGADD, "OP_CHECKSIGADD");
        test_opcode_name("OP_CAT name (disabled)", OP_CAT, "OP_CAT");
        test_opcode_name("OP_RETURN name", OP_RETURN, "OP_RETURN");
    }

    /* Disabled opcode tests */
    test_section("Disabled opcode tests");
    {
        test_opcode_disabled("OP_CAT disabled", OP_CAT, ECHO_TRUE);
        test_opcode_disabled("OP_SUBSTR disabled", OP_SUBSTR, ECHO_TRUE);
        test_opcode_disabled("OP_MUL disabled", OP_MUL, ECHO_TRUE);
        test_opcode_disabled("OP_DIV disabled", OP_DIV, ECHO_TRUE);
        test_opcode_disabled("OP_LSHIFT disabled", OP_LSHIFT, ECHO_TRUE);
        test_opcode_disabled("OP_DUP not disabled", OP_DUP, ECHO_FALSE);
        test_opcode_disabled("OP_ADD not disabled", OP_ADD, ECHO_FALSE);
        test_opcode_disabled("OP_CHECKSIG not disabled", OP_CHECKSIG, ECHO_FALSE);
    }

    /* Script iterator tests */
    test_section("Script iterator tests");
    {
        /* P2PKH has 5 ops: OP_DUP, OP_HASH160, <push20>, OP_EQUALVERIFY, OP_CHECKSIG */
        uint8_t p2pkh[] = {
            0x76, 0xa9, 0x14,
            0x89, 0xab, 0xcd, 0xef, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba,
            0x88, 0xac
        };
        test_iterator("P2PKH iteration", p2pkh, sizeof(p2pkh), 5);

        /* Simple push script */
        uint8_t push_only[] = { 0x00, 0x51, 0x02, 0xab, 0xcd };  /* OP_0, OP_1, PUSH2 */
        test_iterator("push only", push_only, sizeof(push_only), 3);

        /* Empty script */
        uint8_t empty[] = { 0 };
        test_iterator("empty script", empty, 0, 0);

        /* PUSHDATA1 */
        uint8_t pushdata1[78];
        pushdata1[0] = OP_PUSHDATA1;
        pushdata1[1] = 76;  /* Length */
        memset(&pushdata1[2], 0xAB, 76);
        test_iterator("PUSHDATA1", pushdata1, 78, 1);
    }

    /* Sigops counting tests */
    test_section("Sigops counting tests");
    {
        /* Single CHECKSIG */
        uint8_t one_sig[] = { OP_CHECKSIG };
        test_sigops("single CHECKSIG", one_sig, 1, 1);

        /* CHECKSIGVERIFY */
        uint8_t checksigverify[] = { OP_CHECKSIGVERIFY };
        test_sigops("CHECKSIGVERIFY", checksigverify, 1, 1);

        /* P2PKH (1 CHECKSIG) */
        uint8_t p2pkh[] = {
            0x76, 0xa9, 0x14,
            0x89, 0xab, 0xcd, 0xef, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba, 0xab, 0xba, 0xab, 0xba,
            0xab, 0xba, 0xab, 0xba,
            0x88, 0xac
        };
        test_sigops("P2PKH", p2pkh, sizeof(p2pkh), 1);

        /* 2-of-3 multisig: OP_2 <pk1> <pk2> <pk3> OP_3 OP_CHECKMULTISIG */
        /* For accurate counting, last opcode before CHECKMULTISIG is OP_3 = 3 sigops */
        uint8_t multisig[] = {
            OP_2,
            0x21, /* push 33 bytes */
            0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
            0x00,
            0x21, /* push 33 bytes */
            0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
            0x00,
            0x21, /* push 33 bytes */
            0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
            0x00,
            OP_3,
            OP_CHECKMULTISIG
        };
        test_sigops("2-of-3 multisig", multisig, sizeof(multisig), 3);

        /* Empty script */
        test_sigops("empty script", NULL, 0, 0);
    }

    /* Push size calculation tests */
    test_section("Push size calculation tests");
    {
        test_push_size("push 0 bytes", 0, 1);  /* OP_0 */
        test_push_size("push 1 byte", 1, 2);   /* opcode + data */
        test_push_size("push 20 bytes", 20, 21);
        test_push_size("push 75 bytes", 75, 76);
        test_push_size("push 76 bytes", 76, 78);   /* PUSHDATA1 + len + data */
        test_push_size("push 255 bytes", 255, 257);
        test_push_size("push 256 bytes", 256, 259);  /* PUSHDATA2 + 2-byte len + data */
        test_push_size("push 65535 bytes", 65535, 65538);
        test_push_size("push 65536 bytes", 65536, 65541);  /* PUSHDATA4 + 4-byte len + data */
    }

    test_suite_end();
    return test_global_summary();
}
