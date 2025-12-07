/*
 * Bitcoin Echo — Timelock Opcode Tests
 *
 * Test vectors for BIP-65 OP_CHECKLOCKTIMEVERIFY and
 * BIP-112 OP_CHECKSEQUENCEVERIFY.
 *
 * Build once. Build right. Stop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script.h"
#include "tx.h"

static int tests_run = 0;
static int tests_passed = 0;

/*
 * Create a minimal transaction for testing.
 * Caller must call tx_free() when done.
 */
static void create_test_tx(tx_t *tx, int32_t version, uint32_t locktime,
                           uint32_t sequence)
{
    tx_init(tx);
    tx->version = version;
    tx->locktime = locktime;
    tx->input_count = 1;
    tx->inputs = calloc(1, sizeof(tx_input_t));
    tx->inputs[0].sequence = sequence;
    tx->output_count = 1;
    tx->outputs = calloc(1, sizeof(tx_output_t));
    tx->outputs[0].value = 100000000;  /* 1 BTC */
}

/*
 * Test OP_CHECKLOCKTIMEVERIFY with transaction context.
 */
static void test_cltv(const char *name, uint32_t flags,
                      int32_t version, uint32_t tx_locktime, uint32_t sequence,
                      const uint8_t *script, size_t script_len,
                      echo_bool_t should_succeed, script_error_t expected_error)
{
    tests_run++;
    script_context_t ctx;

    echo_result_t res = script_context_init(&ctx, flags);
    if (res != ECHO_OK) {
        printf("  [FAIL] %s (context init failed)\n", name);
        return;
    }

    /* Create test transaction */
    tx_t tx;
    create_test_tx(&tx, version, tx_locktime, sequence);
    script_set_tx_context(&ctx, &tx, 0, 100000000, NULL, 0);

    /* Execute script */
    res = script_execute(&ctx, script, script_len);

    if (should_succeed) {
        if (res != ECHO_OK) {
            printf("  [FAIL] %s (expected success, got error: %s)\n",
                   name, script_error_string(ctx.error));
            script_context_free(&ctx);
            tx_free(&tx);
            return;
        }
    } else {
        if (res == ECHO_OK) {
            printf("  [FAIL] %s (expected failure, got success)\n", name);
            script_context_free(&ctx);
            tx_free(&tx);
            return;
        }
        if (ctx.error != expected_error) {
            printf("  [FAIL] %s (wrong error: expected %s, got %s)\n",
                   name, script_error_string(expected_error),
                   script_error_string(ctx.error));
            script_context_free(&ctx);
            tx_free(&tx);
            return;
        }
    }

    tests_passed++;
    printf("  [PASS] %s\n", name);
    script_context_free(&ctx);
    tx_free(&tx);
}

/*
 * Test OP_CHECKSEQUENCEVERIFY with transaction context.
 */
static void test_csv(const char *name, uint32_t flags,
                     int32_t version, uint32_t sequence,
                     const uint8_t *script, size_t script_len,
                     echo_bool_t should_succeed, script_error_t expected_error)
{
    tests_run++;
    script_context_t ctx;

    echo_result_t res = script_context_init(&ctx, flags);
    if (res != ECHO_OK) {
        printf("  [FAIL] %s (context init failed)\n", name);
        return;
    }

    /* Create test transaction */
    tx_t tx;
    create_test_tx(&tx, version, 0, sequence);
    script_set_tx_context(&ctx, &tx, 0, 100000000, NULL, 0);

    /* Execute script */
    res = script_execute(&ctx, script, script_len);

    if (should_succeed) {
        if (res != ECHO_OK) {
            printf("  [FAIL] %s (expected success, got error: %s)\n",
                   name, script_error_string(ctx.error));
            script_context_free(&ctx);
            tx_free(&tx);
            return;
        }
    } else {
        if (res == ECHO_OK) {
            printf("  [FAIL] %s (expected failure, got success)\n", name);
            script_context_free(&ctx);
            tx_free(&tx);
            return;
        }
        if (ctx.error != expected_error) {
            printf("  [FAIL] %s (wrong error: expected %s, got %s)\n",
                   name, script_error_string(expected_error),
                   script_error_string(ctx.error));
            script_context_free(&ctx);
            tx_free(&tx);
            return;
        }
    }

    tests_passed++;
    printf("  [PASS] %s\n", name);
    script_context_free(&ctx);
    tx_free(&tx);
}

int main(void)
{
    printf("Bitcoin Echo — Timelock Opcode Tests\n");
    printf("=====================================\n\n");

    uint32_t cltv_flags = SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    uint32_t csv_flags = SCRIPT_VERIFY_CHECKSEQUENCEVERIFY;

    /*
     * ==========================================
     * OP_CHECKLOCKTIMEVERIFY (BIP-65) TESTS
     * ==========================================
     */
    printf("OP_CHECKLOCKTIMEVERIFY (BIP-65) tests:\n");
    {
        /*
         * Script: <locktime> OP_CHECKLOCKTIMEVERIFY OP_DROP OP_1
         *
         * The CLTV does not pop from the stack, so we DROP and push OP_1
         * to leave a success value.
         */

        /*
         * Test 1: Basic CLTV success (block height)
         * Stack value: 500000, tx locktime: 500001, sequence: not final
         */
        {
            uint8_t script[] = {
                0x03, 0x20, 0xa1, 0x07,  /* Push 3 bytes: 500000 (0x07a120) */
                OP_CHECKLOCKTIMEVERIFY,
                OP_DROP,
                OP_1
            };
            test_cltv("CLTV success (block height)",
                      cltv_flags, 1, 500001, 0xFFFFFFFE,
                      script, sizeof(script), ECHO_TRUE, SCRIPT_ERR_OK);
        }

        /*
         * Test 2: CLTV success (equal locktime)
         */
        {
            uint8_t script[] = {
                0x03, 0x20, 0xa1, 0x07,  /* 500000 */
                OP_CHECKLOCKTIMEVERIFY,
                OP_DROP,
                OP_1
            };
            test_cltv("CLTV success (equal locktime)",
                      cltv_flags, 1, 500000, 0xFFFFFFFE,
                      script, sizeof(script), ECHO_TRUE, SCRIPT_ERR_OK);
        }

        /*
         * Test 3: CLTV failure (locktime not reached)
         * Stack value: 500000, tx locktime: 499999
         */
        {
            uint8_t script[] = {
                0x03, 0x20, 0xa1, 0x07,  /* 500000 */
                OP_CHECKLOCKTIMEVERIFY,
                OP_DROP,
                OP_1
            };
            test_cltv("CLTV failure (locktime not reached)",
                      cltv_flags, 1, 499999, 0xFFFFFFFE,
                      script, sizeof(script), ECHO_FALSE,
                      SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        /*
         * Test 4: CLTV failure (sequence is final)
         * Input sequence == 0xFFFFFFFF disables locktime
         */
        {
            uint8_t script[] = {
                0x03, 0x20, 0xa1, 0x07,  /* 500000 */
                OP_CHECKLOCKTIMEVERIFY,
                OP_DROP,
                OP_1
            };
            test_cltv("CLTV failure (sequence final)",
                      cltv_flags, 1, 500001, TX_SEQUENCE_FINAL,
                      script, sizeof(script), ECHO_FALSE,
                      SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        /*
         * Test 5: CLTV failure (negative locktime on stack)
         */
        {
            uint8_t script[] = {
                0x01, 0x81,  /* Push -1 (0x81 = sign bit set for 1) */
                OP_CHECKLOCKTIMEVERIFY,
                OP_DROP,
                OP_1
            };
            test_cltv("CLTV failure (negative locktime)",
                      cltv_flags, 1, 500000, 0xFFFFFFFE,
                      script, sizeof(script), ECHO_FALSE,
                      SCRIPT_ERR_NEGATIVE_LOCKTIME);
        }

        /*
         * Test 6: CLTV failure (type mismatch - time vs block)
         * Stack: time value (500000000), tx locktime: block height (100)
         */
        {
            uint8_t script[] = {
                0x04, 0x00, 0x65, 0xcd, 0x1d,  /* Push 500000000 (time) */
                OP_CHECKLOCKTIMEVERIFY,
                OP_DROP,
                OP_1
            };
            test_cltv("CLTV failure (type mismatch)",
                      cltv_flags, 1, 100, 0xFFFFFFFE,
                      script, sizeof(script), ECHO_FALSE,
                      SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        /*
         * Test 7: CLTV success with time-based locktime
         */
        {
            uint8_t script[] = {
                0x04, 0x00, 0x65, 0xcd, 0x1d,  /* Push 500000000 (time) */
                OP_CHECKLOCKTIMEVERIFY,
                OP_DROP,
                OP_1
            };
            test_cltv("CLTV success (time-based)",
                      cltv_flags, 1, 500000001, 0xFFFFFFFE,
                      script, sizeof(script), ECHO_TRUE, SCRIPT_ERR_OK);
        }

        /*
         * Test 8: CLTV with zero locktime (should work)
         */
        {
            uint8_t script[] = {
                OP_0,
                OP_CHECKLOCKTIMEVERIFY,
                OP_DROP,
                OP_1
            };
            test_cltv("CLTV success (zero locktime)",
                      cltv_flags, 1, 0, 0xFFFFFFFE,
                      script, sizeof(script), ECHO_TRUE, SCRIPT_ERR_OK);
        }

        /*
         * Test 9: CLTV treated as NOP when flag not set
         */
        {
            uint8_t script[] = {
                0x03, 0x20, 0xa1, 0x07,  /* 500000 - would fail if checked */
                OP_CHECKLOCKTIMEVERIFY,
                OP_DROP,
                OP_1
            };
            test_cltv("CLTV as NOP (flag not set)",
                      0, /* no flags */ 1, 100, 0xFFFFFFFE,
                      script, sizeof(script), ECHO_TRUE, SCRIPT_ERR_OK);
        }

        /*
         * Test 10: CLTV failure (empty stack)
         */
        {
            uint8_t script[] = {
                OP_CHECKLOCKTIMEVERIFY
            };
            test_cltv("CLTV failure (empty stack)",
                      cltv_flags, 1, 500000, 0xFFFFFFFE,
                      script, sizeof(script), ECHO_FALSE,
                      SCRIPT_ERR_INVALID_STACK_OPERATION);
        }
    }
    printf("\n");

    /*
     * ==========================================
     * OP_CHECKSEQUENCEVERIFY (BIP-112) TESTS
     * ==========================================
     */
    printf("OP_CHECKSEQUENCEVERIFY (BIP-112) tests:\n");
    {
        /*
         * Test 1: Basic CSV success (block-based)
         * Stack: 10 blocks, input sequence: 15 blocks
         */
        {
            uint8_t script[] = {
                0x01, 0x0a,  /* Push 10 */
                OP_CHECKSEQUENCEVERIFY,
                OP_DROP,
                OP_1
            };
            test_csv("CSV success (block-based)",
                     csv_flags, 2, 15,
                     script, sizeof(script), ECHO_TRUE, SCRIPT_ERR_OK);
        }

        /*
         * Test 2: CSV success (equal values)
         */
        {
            uint8_t script[] = {
                0x01, 0x0a,  /* Push 10 */
                OP_CHECKSEQUENCEVERIFY,
                OP_DROP,
                OP_1
            };
            test_csv("CSV success (equal values)",
                     csv_flags, 2, 10,
                     script, sizeof(script), ECHO_TRUE, SCRIPT_ERR_OK);
        }

        /*
         * Test 3: CSV failure (not enough blocks)
         */
        {
            uint8_t script[] = {
                0x01, 0x0a,  /* Push 10 */
                OP_CHECKSEQUENCEVERIFY,
                OP_DROP,
                OP_1
            };
            test_csv("CSV failure (not enough blocks)",
                     csv_flags, 2, 5,
                     script, sizeof(script), ECHO_FALSE,
                     SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        /*
         * Test 4: CSV failure (version < 2)
         */
        {
            uint8_t script[] = {
                0x01, 0x0a,  /* Push 10 */
                OP_CHECKSEQUENCEVERIFY,
                OP_DROP,
                OP_1
            };
            test_csv("CSV failure (version 1)",
                     csv_flags, 1, 15,
                     script, sizeof(script), ECHO_FALSE,
                     SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        /*
         * Test 5: CSV failure (sequence disable flag set)
         * Input sequence has bit 31 set (relative locktime disabled)
         */
        {
            uint8_t script[] = {
                0x01, 0x0a,  /* Push 10 */
                OP_CHECKSEQUENCEVERIFY,
                OP_DROP,
                OP_1
            };
            test_csv("CSV failure (sequence disabled)",
                     csv_flags, 2, SEQUENCE_LOCKTIME_DISABLE_FLAG | 15,
                     script, sizeof(script), ECHO_FALSE,
                     SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        /*
         * Test 6: CSV success when stack value has disable flag
         * If stack value has bit 31 set, CSV acts as NOP (success)
         */
        {
            uint8_t script[] = {
                0x04, 0x00, 0x00, 0x00, 0x80,  /* Push 0x80000000 (disable flag) */
                OP_CHECKSEQUENCEVERIFY,
                OP_DROP,
                OP_1
            };
            test_csv("CSV as NOP (stack disable flag)",
                     csv_flags, 2, 15,
                     script, sizeof(script), ECHO_TRUE, SCRIPT_ERR_OK);
        }

        /*
         * Test 7: CSV failure (type mismatch - time vs blocks)
         * Stack: time-based (bit 22 set), input: block-based
         */
        {
            uint8_t script[] = {
                0x03, 0x0a, 0x00, 0x40,  /* Push 0x40000a (time type + 10) */
                OP_CHECKSEQUENCEVERIFY,
                OP_DROP,
                OP_1
            };
            test_csv("CSV failure (type mismatch)",
                     csv_flags, 2, 15,
                     script, sizeof(script), ECHO_FALSE,
                     SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        }

        /*
         * Test 8: CSV success (time-based)
         * Both stack and sequence have bit 22 set
         */
        {
            uint8_t script[] = {
                0x03, 0x0a, 0x00, 0x40,  /* Push 0x40000a (time type + 10) */
                OP_CHECKSEQUENCEVERIFY,
                OP_DROP,
                OP_1
            };
            /* Sequence = 0x40000f (time type + 15) */
            test_csv("CSV success (time-based)",
                     csv_flags, 2, SEQUENCE_LOCKTIME_TYPE_FLAG | 15,
                     script, sizeof(script), ECHO_TRUE, SCRIPT_ERR_OK);
        }

        /*
         * Test 9: CSV treated as NOP when flag not set
         */
        {
            uint8_t script[] = {
                0x02, 0xff, 0xff,  /* Push 65535 - would fail if checked */
                OP_CHECKSEQUENCEVERIFY,
                OP_DROP,
                OP_1
            };
            test_csv("CSV as NOP (flag not set)",
                     0, /* no flags */ 2, 10,
                     script, sizeof(script), ECHO_TRUE, SCRIPT_ERR_OK);
        }

        /*
         * Test 10: CSV failure (empty stack)
         */
        {
            uint8_t script[] = {
                OP_CHECKSEQUENCEVERIFY
            };
            test_csv("CSV failure (empty stack)",
                     csv_flags, 2, 15,
                     script, sizeof(script), ECHO_FALSE,
                     SCRIPT_ERR_INVALID_STACK_OPERATION);
        }

        /*
         * Test 11: CSV failure (negative value)
         */
        {
            uint8_t script[] = {
                0x01, 0x81,  /* Push -1 */
                OP_CHECKSEQUENCEVERIFY,
                OP_DROP,
                OP_1
            };
            test_csv("CSV failure (negative value)",
                     csv_flags, 2, 15,
                     script, sizeof(script), ECHO_FALSE,
                     SCRIPT_ERR_NEGATIVE_LOCKTIME);
        }

        /*
         * Test 12: CSV with zero (should succeed)
         */
        {
            uint8_t script[] = {
                OP_0,
                OP_CHECKSEQUENCEVERIFY,
                OP_DROP,
                OP_1
            };
            test_csv("CSV success (zero value)",
                     csv_flags, 2, 0,
                     script, sizeof(script), ECHO_TRUE, SCRIPT_ERR_OK);
        }
    }
    printf("\n");

    /*
     * ==========================================
     * COMBINED CLTV + CSV TESTS
     * ==========================================
     */
    printf("Combined CLTV + CSV tests:\n");
    {
        /*
         * Script using both CLTV and CSV
         */
        {
            tests_run++;
            script_context_t ctx;
            uint32_t flags = SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY |
                            SCRIPT_VERIFY_CHECKSEQUENCEVERIFY;

            script_context_init(&ctx, flags);

            tx_t tx;
            create_test_tx(&tx, 2, 500000, 10);  /* version 2, locktime 500000, seq 10 */
            script_set_tx_context(&ctx, &tx, 0, 100000000, NULL, 0);

            /*
             * Script:
             *   <locktime> CLTV DROP
             *   <sequence> CSV DROP
             *   OP_1
             */
            uint8_t script[] = {
                0x03, 0xa0, 0x86, 0x01,  /* Push 100000 (locktime) */
                OP_CHECKLOCKTIMEVERIFY,
                OP_DROP,
                0x01, 0x05,              /* Push 5 (sequence) */
                OP_CHECKSEQUENCEVERIFY,
                OP_DROP,
                OP_1
            };

            echo_result_t res = script_execute(&ctx, script, sizeof(script));

            if (res != ECHO_OK) {
                printf("  [FAIL] Combined CLTV + CSV success (error: %s)\n",
                       script_error_string(ctx.error));
            } else {
                tests_passed++;
                printf("  [PASS] Combined CLTV + CSV success\n");
            }

            script_context_free(&ctx);
            tx_free(&tx);
        }
    }
    printf("\n");

    /*
     * ==========================================
     * SUMMARY
     * ==========================================
     */
    printf("=====================================\n");
    printf("Tests: %d/%d passed\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
