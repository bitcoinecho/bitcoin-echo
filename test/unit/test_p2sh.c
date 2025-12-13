/*
 * Bitcoin Echo — P2SH Evaluation Tests
 *
 * Test vectors for Pay-to-Script-Hash (BIP-16) evaluation including:
 *   - Push-only scriptSig validation
 *   - Redeem script hash verification
 *   - P2SH-wrapped multisig
 *   - P2SH-P2WPKH and P2SH-P2WSH detection
 *
 * Build once. Build right. Stop.
 */

#include <stdio.h>
#include "test_utils.h"
#include <stdlib.h>
#include <string.h>
#include "script.h"
#include "sha256.h"
#include "ripemd160.h"


/*
 * Test: script_is_push_only
 */
static void test_push_only(const char *name, const uint8_t *script, size_t len,
                            echo_bool_t expected)
{
    echo_bool_t result = script_is_push_only(script, len);

    if (result != expected) {
        printf("  [FAIL] %s (expected %s, got %s)\n", name,
               expected ? "push-only" : "not push-only",
               result ? "push-only" : "not push-only");
        return;
    }

    test_pass();
    test_case(name);
        test_pass();
}

/*
 * Test: script_is_p2sh detection
 */
static void test_is_p2sh(const char *name, const uint8_t *script, size_t len,
                          echo_bool_t expected)
{
    echo_bool_t result = script_is_p2sh(script, len, NULL);

    if (result != expected) {
        printf("  [FAIL] %s (expected %s, got %s)\n", name,
               expected ? "P2SH" : "not P2SH",
               result ? "P2SH" : "not P2SH");
        return;
    }

    test_pass();
    test_case(name);
        test_pass();
}

/*
 * Test: P2SH verification (simple redeem script)
 */
static void test_p2sh_simple(const char *name,
                              const uint8_t *script_sig, size_t script_sig_len,
                              const uint8_t *redeem_script, size_t redeem_len,
                              echo_bool_t should_succeed)
{
    script_context_t ctx;
    script_context_init(&ctx, SCRIPT_VERIFY_P2SH);

    /* Compute script hash from redeem script */
    uint8_t script_hash[20];
    hash160(redeem_script, redeem_len, script_hash);

    echo_result_t res = script_verify_p2sh(&ctx, script_sig, script_sig_len,
                                            script_hash, NULL, 0);

    if (should_succeed) {
        if (res != ECHO_OK) {
            printf("  [FAIL] %s (expected success, got error: %s)\n", name,
                   script_error_string(ctx.error));
            script_context_free(&ctx);
            return;
        }
    } else {
        if (res == ECHO_OK) {
            test_case(name);
        test_fail(name);
            script_context_free(&ctx);
            return;
        }
    }

    test_pass();
    test_case(name);
        test_pass();
    script_context_free(&ctx);
}

/*
 * Test: P2SH hash mismatch
 */
static void test_p2sh_hash_mismatch(void)
{
    script_context_t ctx;
    script_context_init(&ctx, SCRIPT_VERIFY_P2SH);

    /* Redeem script: OP_1 */
    uint8_t redeem_script[] = {OP_1};
    (void)redeem_script;  /* For documentation - scriptSig below is this script */

    /* scriptSig: push the redeem script */
    uint8_t script_sig[2];
    script_sig[0] = 0x01;  /* Push 1 byte */
    script_sig[1] = OP_1;

    /* Use a wrong hash */
    uint8_t wrong_hash[20] = {0};

    echo_result_t res = script_verify_p2sh(&ctx, script_sig, sizeof(script_sig),
                                            wrong_hash, NULL, 0);

    if (res == ECHO_OK) {
        test_case("P2SH hash mismatch (expected failure)");
        test_fail("P2SH hash mismatch (expected failure)");
        script_context_free(&ctx);
        return;
    }

    if (ctx.error != SCRIPT_ERR_EQUALVERIFY) {
        printf("  [FAIL] P2SH hash mismatch (wrong error: %s)\n",
               script_error_string(ctx.error));
        script_context_free(&ctx);
        return;
    }

    test_pass();
    test_case("P2SH hash mismatch detected");
        test_pass();
    script_context_free(&ctx);
}

/*
 * Test: Non-push-only scriptSig
 */
static void test_p2sh_non_push_only(void)
{
    script_context_t ctx;
    script_context_init(&ctx, SCRIPT_VERIFY_P2SH);

    /* Redeem script: OP_1 */
    uint8_t redeem_script[] = {OP_1};
    uint8_t script_hash[20];
    hash160(redeem_script, sizeof(redeem_script), script_hash);

    /* scriptSig with non-push opcode: OP_1 OP_DROP <redeem_script> */
    uint8_t script_sig[] = {
        OP_1,
        OP_DROP,
        0x01, OP_1  /* Push 1 byte: the redeem script */
    };

    echo_result_t res = script_verify_p2sh(&ctx, script_sig, sizeof(script_sig),
                                            script_hash, NULL, 0);

    if (res == ECHO_OK) {
        test_case("Non-push-only scriptSig (expected failure)");
        test_fail("Non-push-only scriptSig (expected failure)");
        script_context_free(&ctx);
        return;
    }

    if (ctx.error != SCRIPT_ERR_SIG_PUSHONLY) {
        printf("  [FAIL] Non-push-only scriptSig (wrong error: %s)\n",
               script_error_string(ctx.error));
        script_context_free(&ctx);
        return;
    }

    test_pass();
    test_case("Non-push-only scriptSig rejected");
        test_pass();
    script_context_free(&ctx);
}

int main(void)
{
    test_suite_begin("P2SH Evaluation Tests");

    /*
     * ==========================================
     * PUSH-ONLY SCRIPT TESTS
     * ==========================================
     */
    test_section("Push-only script tests");
    {
        /* Empty script is push-only */
        test_push_only("Empty script", NULL, 0, ECHO_TRUE);

        /* OP_0 is push-only */
        uint8_t op0[] = {OP_0};
        test_push_only("OP_0", op0, sizeof(op0), ECHO_TRUE);

        /* OP_1 through OP_16 are push-only */
        uint8_t op1[] = {OP_1};
        test_push_only("OP_1", op1, sizeof(op1), ECHO_TRUE);

        uint8_t op16[] = {OP_16};
        test_push_only("OP_16", op16, sizeof(op16), ECHO_TRUE);

        /* OP_1NEGATE is push-only */
        uint8_t op1neg[] = {OP_1NEGATE};
        test_push_only("OP_1NEGATE", op1neg, sizeof(op1neg), ECHO_TRUE);

        /* Direct push (0x01-0x4b) is push-only */
        uint8_t push3[] = {0x03, 0xaa, 0xbb, 0xcc};
        test_push_only("Push 3 bytes", push3, sizeof(push3), ECHO_TRUE);

        /* OP_PUSHDATA1 is push-only */
        uint8_t pushdata1[] = {OP_PUSHDATA1, 0x02, 0xaa, 0xbb};
        test_push_only("OP_PUSHDATA1", pushdata1, sizeof(pushdata1), ECHO_TRUE);

        /* OP_PUSHDATA2 is push-only */
        uint8_t pushdata2[] = {OP_PUSHDATA2, 0x02, 0x00, 0xaa, 0xbb};
        test_push_only("OP_PUSHDATA2", pushdata2, sizeof(pushdata2), ECHO_TRUE);

        /* Multiple pushes are push-only */
        uint8_t multi[] = {OP_1, OP_2, 0x02, 0xaa, 0xbb, OP_3};
        test_push_only("Multiple pushes", multi, sizeof(multi), ECHO_TRUE);

        /* Non-push opcodes fail */
        uint8_t add[] = {OP_ADD};
        test_push_only("OP_ADD", add, sizeof(add), ECHO_FALSE);

        uint8_t dup[] = {OP_DUP};
        test_push_only("OP_DUP", dup, sizeof(dup), ECHO_FALSE);

        uint8_t checksig[] = {OP_CHECKSIG};
        test_push_only("OP_CHECKSIG", checksig, sizeof(checksig), ECHO_FALSE);

        /* Push followed by non-push fails */
        uint8_t push_then_add[] = {OP_1, OP_2, OP_ADD};
        test_push_only("Push then ADD", push_then_add, sizeof(push_then_add), ECHO_FALSE);
    }

    /*
     * ==========================================
     * P2SH DETECTION TESTS
     * ==========================================
     */
    test_section("P2SH detection tests");
    {
        /* Valid P2SH pattern: OP_HASH160 <20 bytes> OP_EQUAL */
        uint8_t p2sh[] = {
            OP_HASH160,
            0x14,  /* Push 20 bytes */
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
            0x10, 0x11, 0x12, 0x13,
            OP_EQUAL
        };
        test_is_p2sh("Valid P2SH", p2sh, sizeof(p2sh), ECHO_TRUE);

        /* Wrong length */
        uint8_t short_p2sh[] = {OP_HASH160, 0x14, 0x00, OP_EQUAL};
        test_is_p2sh("Short P2SH", short_p2sh, sizeof(short_p2sh), ECHO_FALSE);

        /* Wrong opcode at end */
        uint8_t wrong_end[] = {
            OP_HASH160,
            0x14,
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
            0x10, 0x11, 0x12, 0x13,
            OP_EQUALVERIFY  /* Wrong! Should be OP_EQUAL */
        };
        test_is_p2sh("Wrong end opcode", wrong_end, sizeof(wrong_end), ECHO_FALSE);

        /* P2PKH is not P2SH */
        uint8_t p2pkh[] = {
            OP_DUP, OP_HASH160,
            0x14,
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
            0x10, 0x11, 0x12, 0x13,
            OP_EQUALVERIFY, OP_CHECKSIG
        };
        test_is_p2sh("P2PKH is not P2SH", p2pkh, sizeof(p2pkh), ECHO_FALSE);
    }

    /*
     * ==========================================
     * P2SH VERIFICATION TESTS
     * ==========================================
     */
    test_section("P2SH verification tests");
    {
        /*
         * Test: Simple redeem script OP_1
         * The redeem script just pushes 1 (true)
         */
        {
            uint8_t redeem[] = {OP_1};

            /* scriptSig pushes the redeem script */
            uint8_t script_sig[2];
            script_sig[0] = 0x01;  /* Push 1 byte */
            script_sig[1] = OP_1;

            test_p2sh_simple("Simple OP_1 redeem script",
                             script_sig, sizeof(script_sig),
                             redeem, sizeof(redeem),
                             ECHO_TRUE);
        }

        /*
         * Test: Redeem script OP_1 OP_1 OP_ADD OP_2 OP_EQUAL
         * Computes 1+1=2 and checks equality
         */
        {
            uint8_t redeem[] = {OP_1, OP_1, OP_ADD, OP_2, OP_EQUAL};

            /* scriptSig pushes the redeem script */
            uint8_t script_sig[6];
            script_sig[0] = 0x05;  /* Push 5 bytes */
            memcpy(script_sig + 1, redeem, 5);

            test_p2sh_simple("Arithmetic redeem script (1+1=2)",
                             script_sig, sizeof(script_sig),
                             redeem, sizeof(redeem),
                             ECHO_TRUE);
        }

        /*
         * Test: Redeem script that evaluates to false
         */
        {
            uint8_t redeem[] = {OP_0};  /* Pushes false */

            uint8_t script_sig[2];
            script_sig[0] = 0x01;
            script_sig[1] = OP_0;

            test_p2sh_simple("Redeem script evaluates to false",
                             script_sig, sizeof(script_sig),
                             redeem, sizeof(redeem),
                             ECHO_FALSE);
        }

        /*
         * Test: Redeem script using stack data from scriptSig
         * scriptSig pushes: <data> <redeem_script>
         * redeem_script verifies the data
         */
        {
            /* Redeem script: OP_5 OP_EQUAL (expects 5 on stack) */
            uint8_t redeem[] = {OP_5, OP_EQUAL};

            /* scriptSig: push 5, then push redeem script */
            uint8_t script_sig[4];
            script_sig[0] = OP_5;           /* Push 5 */
            script_sig[1] = 0x02;           /* Push 2 bytes (redeem script) */
            script_sig[2] = OP_5;
            script_sig[3] = OP_EQUAL;

            test_p2sh_simple("Redeem script uses scriptSig data",
                             script_sig, sizeof(script_sig),
                             redeem, sizeof(redeem),
                             ECHO_TRUE);
        }

        /* Test hash mismatch */
        test_p2sh_hash_mismatch();

        /* Test non-push-only rejection */
        test_p2sh_non_push_only();
    }

    /*
     * ==========================================
     * SUMMARY
     * ==========================================
     */
    test_suite_end();
    return test_global_summary();
}
