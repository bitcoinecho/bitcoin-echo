/*
 * Bitcoin Echo — Script Opcode Execution Tests
 *
 * Test vectors for push, flow control, stack, arithmetic, logic,
 * and cryptographic opcodes.
 *
 * Build once. Build right. Stop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script.h"
#include "sha256.h"
#include "ripemd160.h"

#include "test_utils.h"

/*
 * Helper: Execute a script and check result.
 */
static void test_script(const char *name, const uint8_t *script, size_t len,
                        echo_bool_t should_succeed, script_num_t expected_top)
{
    script_context_t ctx;

    echo_result_t res = script_context_init(&ctx, SCRIPT_VERIFY_NONE);
    if (res != ECHO_OK) {
        test_case(name);
        test_fail(name);
        return;
    }

    res = script_execute(&ctx, script, len);

    if (should_succeed) {
        if (res != ECHO_OK) {
            printf("  [FAIL] %s (execution failed: %s)\n", name,
                   script_error_string(ctx.error));
            script_context_free(&ctx);
            return;
        }

        /* Check top of stack */
        if (!stack_empty(&ctx.stack)) {
            script_num_t top;
            const stack_element_t *elem;
            stack_peek(&ctx.stack, &elem);
            script_num_decode(elem->data, elem->len, &top, ECHO_FALSE, 8);

            if (top != expected_top) {
                printf("  [FAIL] %s (expected top=%lld, got=%lld)\n",
                       name, (long long)expected_top, (long long)top);
                script_context_free(&ctx);
                return;
            }
        } else if (expected_top != 0) {
            printf("  [FAIL] %s (stack empty, expected top=%lld)\n",
                   name, (long long)expected_top);
            script_context_free(&ctx);
            return;
        }

        test_pass();
        test_case(name);
        test_pass();
    } else {
        if (res == ECHO_OK) {
            test_case(name);
        test_fail(name);
            script_context_free(&ctx);
            return;
        }
        test_pass();
        test_case(name);
        test_pass();
    }

    script_context_free(&ctx);
}

/*
 * Helper: Execute a script and check stack size.
 */
static void test_script_stack_size(const char *name, const uint8_t *script,
                                    size_t len, size_t expected_size)
{
    script_context_t ctx;

    script_context_init(&ctx, SCRIPT_VERIFY_NONE);
    echo_result_t res = script_execute(&ctx, script, len);

    if (res != ECHO_OK) {
        printf("  [FAIL] %s (execution failed: %s)\n", name,
               script_error_string(ctx.error));
        script_context_free(&ctx);
        return;
    }

    if (stack_size(&ctx.stack) != expected_size) {
        printf("  [FAIL] %s (expected stack size=%zu, got=%zu)\n",
               name, expected_size, stack_size(&ctx.stack));
        script_context_free(&ctx);
        return;
    }

    test_pass();
    test_case(name);
        test_pass();
    script_context_free(&ctx);
}

/*
 * Helper: Execute a script and check that it fails with specific error.
 */
static void test_script_error(const char *name, const uint8_t *script,
                               size_t len, script_error_t expected_error)
{
    script_context_t ctx;

    script_context_init(&ctx, SCRIPT_VERIFY_NONE);
    echo_result_t res = script_execute(&ctx, script, len);

    if (res == ECHO_OK) {
        test_case(name);
        test_fail(name);
        script_context_free(&ctx);
        return;
    }

    if (ctx.error != expected_error) {
        printf("  [FAIL] %s (expected error=%s, got=%s)\n", name,
               script_error_string(expected_error),
               script_error_string(ctx.error));
        script_context_free(&ctx);
        return;
    }

    test_pass();
    test_case(name);
        test_pass();
    script_context_free(&ctx);
}

/*
 * Helper: Execute a script and check top element bytes.
 */
static void test_script_bytes(const char *name, const uint8_t *script, size_t len,
                               const uint8_t *expected, size_t expected_len)
{
    script_context_t ctx;

    script_context_init(&ctx, SCRIPT_VERIFY_NONE);
    echo_result_t res = script_execute(&ctx, script, len);

    if (res != ECHO_OK) {
        printf("  [FAIL] %s (execution failed: %s)\n", name,
               script_error_string(ctx.error));
        script_context_free(&ctx);
        return;
    }

    if (stack_empty(&ctx.stack)) {
        test_case(name);
        test_fail(name);
        script_context_free(&ctx);
        return;
    }

    const stack_element_t *top;
    stack_peek(&ctx.stack, &top);

    if (top->len != expected_len) {
        printf("  [FAIL] %s (expected len=%zu, got=%zu)\n", name,
               expected_len, top->len);
        script_context_free(&ctx);
        return;
    }

    if (memcmp(top->data, expected, expected_len) != 0) {
        test_case(name);
        test_fail(name);
        script_context_free(&ctx);
        return;
    }

    test_pass();
    test_case(name);
        test_pass();
    script_context_free(&ctx);
}

/*
 * Helper: Execute a script with specific flags.
 */
static void test_script_flags(const char *name, const uint8_t *script, size_t len,
                               uint32_t flags, echo_bool_t should_succeed,
                               script_error_t expected_error)
{
    script_context_t ctx;

    script_context_init(&ctx, flags);
    echo_result_t res = script_execute(&ctx, script, len);

    if (should_succeed) {
        if (res != ECHO_OK) {
            printf("  [FAIL] %s (execution failed: %s)\n", name,
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
        if (ctx.error != expected_error) {
            printf("  [FAIL] %s (expected error=%s, got=%s)\n", name,
                   script_error_string(expected_error),
                   script_error_string(ctx.error));
            script_context_free(&ctx);
            return;
        }
    }

    test_pass();
    test_case(name);
        test_pass();
    script_context_free(&ctx);
}

int main(void)
{
    test_suite_begin("Script Opcode Execution Tests");

    /*
     * ==========================================
     * PUSH OPCODES
     * ==========================================
     */
    test_section("Push opcode tests");
    {
        /* OP_0 pushes empty (which is 0) */
        uint8_t op0[] = {OP_0};
        test_script("OP_0 pushes 0", op0, sizeof(op0), ECHO_TRUE, 0);

        /* OP_1NEGATE pushes -1 */
        uint8_t op1neg[] = {OP_1NEGATE};
        test_script("OP_1NEGATE pushes -1", op1neg, sizeof(op1neg), ECHO_TRUE, -1);

        /* OP_1 through OP_16 */
        uint8_t op1[] = {OP_1};
        test_script("OP_1 pushes 1", op1, sizeof(op1), ECHO_TRUE, 1);

        uint8_t op5[] = {OP_5};
        test_script("OP_5 pushes 5", op5, sizeof(op5), ECHO_TRUE, 5);

        uint8_t op16[] = {OP_16};
        test_script("OP_16 pushes 16", op16, sizeof(op16), ECHO_TRUE, 16);

        /* Direct push: push 1 byte */
        uint8_t push1[] = {0x01, 0x42};  /* Push 1 byte: 0x42 = 66 */
        test_script("Push 1 byte (66)", push1, sizeof(push1), ECHO_TRUE, 66);

        /* Direct push: push 2 bytes (little-endian) */
        uint8_t push2[] = {0x02, 0x00, 0x01};  /* Push 2 bytes: 0x0100 = 256 */
        test_script("Push 2 bytes (256)", push2, sizeof(push2), ECHO_TRUE, 256);
    }

    /*
     * ==========================================
     * STACK OPCODES
     * ==========================================
     */
    test_section("Stack opcode tests");
    {
        /* OP_DUP: Duplicate top */
        uint8_t dup[] = {OP_5, OP_DUP, OP_ADD};  /* 5 DUP ADD -> 10 */
        test_script("OP_DUP", dup, sizeof(dup), ECHO_TRUE, 10);

        /* OP_DROP */
        uint8_t drop[] = {OP_5, OP_3, OP_DROP};  /* 5 3 DROP -> 5 */
        test_script("OP_DROP", drop, sizeof(drop), ECHO_TRUE, 5);

        /* OP_SWAP */
        uint8_t swap[] = {OP_3, OP_5, OP_SWAP, OP_SUB};  /* 3 5 SWAP SUB -> 5-3=2 */
        test_script("OP_SWAP", swap, sizeof(swap), ECHO_TRUE, 2);

        /* OP_ROT */
        uint8_t rot[] = {OP_1, OP_2, OP_3, OP_ROT};  /* 1 2 3 ROT -> 2 3 1 (top=1) */
        test_script("OP_ROT", rot, sizeof(rot), ECHO_TRUE, 1);

        /* OP_OVER */
        uint8_t over[] = {OP_3, OP_5, OP_OVER};  /* 3 5 OVER -> 3 5 3 (top=3) */
        test_script("OP_OVER", over, sizeof(over), ECHO_TRUE, 3);

        /* OP_DEPTH */
        uint8_t depth[] = {OP_1, OP_2, OP_3, OP_DEPTH};  /* Stack has 3 items, DEPTH pushes 3 */
        test_script("OP_DEPTH", depth, sizeof(depth), ECHO_TRUE, 3);

        /* OP_2DUP */
        uint8_t dup2[] = {OP_3, OP_5, OP_2DUP, OP_DEPTH};  /* 3 5 2DUP -> 3 5 3 5, DEPTH -> 4 */
        test_script("OP_2DUP", dup2, sizeof(dup2), ECHO_TRUE, 4);

        /* OP_PICK */
        uint8_t pick[] = {OP_1, OP_2, OP_3, OP_2, OP_PICK};  /* 1 2 3, pick index 2 -> 1 */
        test_script("OP_PICK", pick, sizeof(pick), ECHO_TRUE, 1);

        /* OP_ROLL */
        uint8_t roll[] = {OP_1, OP_2, OP_3, OP_2, OP_ROLL};  /* 1 2 3, roll 2 -> 2 3 1 */
        test_script("OP_ROLL", roll, sizeof(roll), ECHO_TRUE, 1);

        /* OP_IFDUP with true value */
        uint8_t ifdup_true[] = {OP_5, OP_IFDUP, OP_DEPTH};
        test_script("OP_IFDUP (true)", ifdup_true, sizeof(ifdup_true), ECHO_TRUE, 2);

        /* OP_IFDUP with false value */
        uint8_t ifdup_false[] = {OP_0, OP_IFDUP, OP_DEPTH};
        test_script("OP_IFDUP (false)", ifdup_false, sizeof(ifdup_false), ECHO_TRUE, 1);
    }

    /*
     * ==========================================
     * ALTSTACK OPCODES
     * ==========================================
     */
    test_section("Altstack opcode tests");
    {
        /* OP_TOALTSTACK and OP_FROMALTSTACK */
        uint8_t alt[] = {OP_5, OP_TOALTSTACK, OP_3, OP_FROMALTSTACK, OP_ADD};
        test_script("TOALTSTACK/FROMALTSTACK", alt, sizeof(alt), ECHO_TRUE, 8);

        /* Empty altstack error */
        uint8_t alt_empty[] = {OP_FROMALTSTACK};
        test_script_error("FROMALTSTACK empty", alt_empty, sizeof(alt_empty),
                          SCRIPT_ERR_INVALID_ALTSTACK_OPERATION);
    }

    /*
     * ==========================================
     * ARITHMETIC OPCODES
     * ==========================================
     */
    test_section("Arithmetic opcode tests");
    {
        /* OP_ADD */
        uint8_t add[] = {OP_3, OP_5, OP_ADD};
        test_script("OP_ADD (3+5=8)", add, sizeof(add), ECHO_TRUE, 8);

        /* OP_SUB */
        uint8_t sub[] = {OP_10, OP_3, OP_SUB};
        test_script("OP_SUB (10-3=7)", sub, sizeof(sub), ECHO_TRUE, 7);

        /* OP_1ADD */
        uint8_t add1[] = {OP_5, OP_1ADD};
        test_script("OP_1ADD (5+1=6)", add1, sizeof(add1), ECHO_TRUE, 6);

        /* OP_1SUB */
        uint8_t sub1[] = {OP_5, OP_1SUB};
        test_script("OP_1SUB (5-1=4)", sub1, sizeof(sub1), ECHO_TRUE, 4);

        /* OP_NEGATE */
        uint8_t neg[] = {OP_5, OP_NEGATE};
        test_script("OP_NEGATE (5 -> -5)", neg, sizeof(neg), ECHO_TRUE, -5);

        /* OP_ABS positive */
        uint8_t abs_pos[] = {OP_5, OP_ABS};
        test_script("OP_ABS (5 -> 5)", abs_pos, sizeof(abs_pos), ECHO_TRUE, 5);

        /* OP_ABS negative */
        uint8_t abs_neg[] = {OP_5, OP_NEGATE, OP_ABS};
        test_script("OP_ABS (-5 -> 5)", abs_neg, sizeof(abs_neg), ECHO_TRUE, 5);

        /* OP_MIN */
        uint8_t min[] = {OP_3, OP_7, OP_MIN};
        test_script("OP_MIN (3,7 -> 3)", min, sizeof(min), ECHO_TRUE, 3);

        /* OP_MAX */
        uint8_t max[] = {OP_3, OP_7, OP_MAX};
        test_script("OP_MAX (3,7 -> 7)", max, sizeof(max), ECHO_TRUE, 7);

        /* OP_WITHIN true */
        uint8_t within_true[] = {OP_5, OP_3, OP_7, OP_WITHIN};  /* 3 <= 5 < 7 */
        test_script("OP_WITHIN (5 in [3,7))", within_true, sizeof(within_true), ECHO_TRUE, 1);

        /* OP_WITHIN false */
        uint8_t within_false[] = {OP_7, OP_3, OP_7, OP_WITHIN};  /* 3 <= 7 < 7 is false */
        test_script("OP_WITHIN (7 not in [3,7))", within_false, sizeof(within_false), ECHO_TRUE, 0);
    }

    /*
     * ==========================================
     * LOGIC OPCODES
     * ==========================================
     */
    test_section("Logic opcode tests");
    {
        /* OP_NOT */
        uint8_t not_zero[] = {OP_0, OP_NOT};
        test_script("OP_NOT (0 -> 1)", not_zero, sizeof(not_zero), ECHO_TRUE, 1);

        uint8_t not_nonzero[] = {OP_5, OP_NOT};
        test_script("OP_NOT (5 -> 0)", not_nonzero, sizeof(not_nonzero), ECHO_TRUE, 0);

        /* OP_0NOTEQUAL */
        uint8_t nz_zero[] = {OP_0, OP_0NOTEQUAL};
        test_script("OP_0NOTEQUAL (0 -> 0)", nz_zero, sizeof(nz_zero), ECHO_TRUE, 0);

        uint8_t nz_nonzero[] = {OP_5, OP_0NOTEQUAL};
        test_script("OP_0NOTEQUAL (5 -> 1)", nz_nonzero, sizeof(nz_nonzero), ECHO_TRUE, 1);

        /* OP_BOOLAND */
        uint8_t and_tt[] = {OP_1, OP_1, OP_BOOLAND};
        test_script("OP_BOOLAND (1,1 -> 1)", and_tt, sizeof(and_tt), ECHO_TRUE, 1);

        uint8_t and_tf[] = {OP_1, OP_0, OP_BOOLAND};
        test_script("OP_BOOLAND (1,0 -> 0)", and_tf, sizeof(and_tf), ECHO_TRUE, 0);

        /* OP_BOOLOR */
        uint8_t or_tf[] = {OP_1, OP_0, OP_BOOLOR};
        test_script("OP_BOOLOR (1,0 -> 1)", or_tf, sizeof(or_tf), ECHO_TRUE, 1);

        uint8_t or_ff[] = {OP_0, OP_0, OP_BOOLOR};
        test_script("OP_BOOLOR (0,0 -> 0)", or_ff, sizeof(or_ff), ECHO_TRUE, 0);
    }

    /*
     * ==========================================
     * NUMERIC COMPARISON OPCODES
     * ==========================================
     */
    test_section("Numeric comparison opcode tests");
    {
        /* OP_NUMEQUAL */
        uint8_t eq_true[] = {OP_5, OP_5, OP_NUMEQUAL};
        test_script("OP_NUMEQUAL (5==5)", eq_true, sizeof(eq_true), ECHO_TRUE, 1);

        uint8_t eq_false[] = {OP_3, OP_5, OP_NUMEQUAL};
        test_script("OP_NUMEQUAL (3!=5)", eq_false, sizeof(eq_false), ECHO_TRUE, 0);

        /* OP_NUMNOTEQUAL */
        uint8_t neq[] = {OP_3, OP_5, OP_NUMNOTEQUAL};
        test_script("OP_NUMNOTEQUAL (3!=5)", neq, sizeof(neq), ECHO_TRUE, 1);

        /* OP_LESSTHAN */
        uint8_t lt_true[] = {OP_3, OP_5, OP_LESSTHAN};
        test_script("OP_LESSTHAN (3<5)", lt_true, sizeof(lt_true), ECHO_TRUE, 1);

        uint8_t lt_false[] = {OP_5, OP_3, OP_LESSTHAN};
        test_script("OP_LESSTHAN (5<3 false)", lt_false, sizeof(lt_false), ECHO_TRUE, 0);

        /* OP_GREATERTHAN */
        uint8_t gt_true[] = {OP_5, OP_3, OP_GREATERTHAN};
        test_script("OP_GREATERTHAN (5>3)", gt_true, sizeof(gt_true), ECHO_TRUE, 1);

        /* OP_LESSTHANOREQUAL */
        uint8_t lte_eq[] = {OP_5, OP_5, OP_LESSTHANOREQUAL};
        test_script("OP_LESSTHANOREQUAL (5<=5)", lte_eq, sizeof(lte_eq), ECHO_TRUE, 1);

        /* OP_GREATERTHANOREQUAL */
        uint8_t gte[] = {OP_5, OP_3, OP_GREATERTHANOREQUAL};
        test_script("OP_GREATERTHANOREQUAL (5>=3)", gte, sizeof(gte), ECHO_TRUE, 1);
    }

    /*
     * ==========================================
     * BYTE COMPARISON OPCODES
     * ==========================================
     */
    test_section("Byte comparison opcode tests");
    {
        /* OP_EQUAL */
        uint8_t equal_true[] = {0x02, 0xab, 0xcd, 0x02, 0xab, 0xcd, OP_EQUAL};
        test_script("OP_EQUAL (bytes equal)", equal_true, sizeof(equal_true), ECHO_TRUE, 1);

        uint8_t equal_false[] = {0x02, 0xab, 0xcd, 0x02, 0xab, 0xce, OP_EQUAL};
        test_script("OP_EQUAL (bytes differ)", equal_false, sizeof(equal_false), ECHO_TRUE, 0);

        /* OP_EQUALVERIFY succeeds */
        uint8_t eqv_ok[] = {OP_5, OP_5, OP_EQUALVERIFY, OP_1};
        test_script("OP_EQUALVERIFY success", eqv_ok, sizeof(eqv_ok), ECHO_TRUE, 1);

        /* OP_EQUALVERIFY fails */
        uint8_t eqv_fail[] = {OP_3, OP_5, OP_EQUALVERIFY};
        test_script_error("OP_EQUALVERIFY failure", eqv_fail, sizeof(eqv_fail),
                          SCRIPT_ERR_EQUALVERIFY);
    }

    /*
     * ==========================================
     * FLOW CONTROL OPCODES
     * ==========================================
     */
    test_section("Flow control opcode tests");
    {
        /* OP_IF true branch */
        uint8_t if_true[] = {OP_1, OP_IF, OP_5, OP_ELSE, OP_3, OP_ENDIF};
        test_script("OP_IF (true branch)", if_true, sizeof(if_true), ECHO_TRUE, 5);

        /* OP_IF false branch */
        uint8_t if_false[] = {OP_0, OP_IF, OP_5, OP_ELSE, OP_3, OP_ENDIF};
        test_script("OP_IF (false branch)", if_false, sizeof(if_false), ECHO_TRUE, 3);

        /* OP_NOTIF true branch (executed when condition is false) */
        uint8_t notif_true[] = {OP_0, OP_NOTIF, OP_5, OP_ELSE, OP_3, OP_ENDIF};
        test_script("OP_NOTIF (0 -> true branch)", notif_true, sizeof(notif_true), ECHO_TRUE, 5);

        /* OP_NOTIF false branch (skipped when condition is true) */
        uint8_t notif_false[] = {OP_1, OP_NOTIF, OP_5, OP_ELSE, OP_3, OP_ENDIF};
        test_script("OP_NOTIF (1 -> else branch)", notif_false, sizeof(notif_false), ECHO_TRUE, 3);

        /* Nested IF */
        uint8_t nested[] = {OP_1, OP_IF, OP_1, OP_IF, OP_5, OP_ENDIF, OP_ENDIF};
        test_script("Nested IF", nested, sizeof(nested), ECHO_TRUE, 5);

        /* OP_VERIFY success */
        uint8_t verify_ok[] = {OP_1, OP_VERIFY, OP_5};
        test_script("OP_VERIFY success", verify_ok, sizeof(verify_ok), ECHO_TRUE, 5);

        /* OP_VERIFY failure */
        uint8_t verify_fail[] = {OP_0, OP_VERIFY};
        test_script_error("OP_VERIFY failure", verify_fail, sizeof(verify_fail),
                          SCRIPT_ERR_VERIFY);

        /* OP_RETURN always fails */
        uint8_t ret[] = {OP_RETURN};
        test_script_error("OP_RETURN", ret, sizeof(ret), SCRIPT_ERR_OP_RETURN);

        /* Unbalanced IF */
        uint8_t unbal_if[] = {OP_1, OP_IF, OP_5};
        test_script_error("Unbalanced IF", unbal_if, sizeof(unbal_if),
                          SCRIPT_ERR_UNBALANCED_CONDITIONAL);

        /* Unbalanced ENDIF */
        uint8_t unbal_endif[] = {OP_ENDIF};
        test_script_error("Unbalanced ENDIF", unbal_endif, sizeof(unbal_endif),
                          SCRIPT_ERR_UNBALANCED_CONDITIONAL);

        /* OP_NOP does nothing */
        uint8_t nop[] = {OP_5, OP_NOP, OP_NOP};
        test_script("OP_NOP", nop, sizeof(nop), ECHO_TRUE, 5);
    }

    /*
     * ==========================================
     * SPLICE OPCODES
     * ==========================================
     */
    test_section("Splice opcode tests");
    {
        /* OP_SIZE */
        uint8_t size[] = {0x03, 0xaa, 0xbb, 0xcc, OP_SIZE};  /* Push 3 bytes, SIZE -> 3 */
        test_script("OP_SIZE", size, sizeof(size), ECHO_TRUE, 3);

        /* OP_SIZE of empty */
        uint8_t size_empty[] = {OP_0, OP_SIZE};  /* Empty element has size 0 */
        test_script("OP_SIZE (empty)", size_empty, sizeof(size_empty), ECHO_TRUE, 0);
    }

    /*
     * ==========================================
     * ERROR HANDLING
     * ==========================================
     */
    test_section("Error handling tests");
    {
        /* Stack underflow */
        uint8_t underflow[] = {OP_ADD};
        test_script_error("Stack underflow", underflow, sizeof(underflow),
                          SCRIPT_ERR_INVALID_STACK_OPERATION);

        /* Disabled opcode */
        uint8_t disabled[] = {OP_CAT};
        test_script_error("Disabled opcode", disabled, sizeof(disabled),
                          SCRIPT_ERR_DISABLED_OPCODE);
    }

    /*
     * ==========================================
     * COMBINED SCRIPT TESTS
     * ==========================================
     */
    test_section("Combined script tests");
    {
        /* Simple addition with verify */
        uint8_t add_verify[] = {OP_2, OP_3, OP_ADD, OP_5, OP_NUMEQUAL};
        test_script("2 + 3 == 5", add_verify, sizeof(add_verify), ECHO_TRUE, 1);

        /* Conditional computation */
        uint8_t cond_comp[] = {
            OP_5, OP_3, OP_LESSTHAN,  /* 5 < 3 = false */
            OP_IF, OP_10, OP_ELSE, 0x01, 0x14, OP_ENDIF  /* 0x14 = 20 */
        };
        test_script("5<3 ? 10 : 20 = 20", cond_comp, sizeof(cond_comp), ECHO_TRUE, 20);

        /* Use altstack for temporary storage */
        uint8_t altstack_calc[] = {
            OP_5, OP_TOALTSTACK,  /* Save 5 */
            OP_3, OP_2, OP_ADD,   /* 3 + 2 = 5 */
            OP_FROMALTSTACK,       /* Get 5 back */
            OP_NUMEQUAL            /* 5 == 5 */
        };
        test_script("Altstack calculation", altstack_calc, sizeof(altstack_calc), ECHO_TRUE, 1);

        /* Stack check after operations */
        uint8_t stack_check[] = {OP_1, OP_2, OP_3, OP_2DROP};
        test_script_stack_size("2DROP leaves 1 element", stack_check, sizeof(stack_check), 1);
    }

    /*
     * ==========================================
     * CRYPTO OPCODES
     * ==========================================
     */
    test_section("Crypto opcode tests");
    {
        /*
         * OP_RIPEMD160: RIPEMD-160 hash
         * Test vector: RIPEMD160("") = 9c1185a5c5e9fc54612808977ee8f548b2258d31
         */
        uint8_t ripemd_empty[] = {OP_0, OP_RIPEMD160};
        uint8_t ripemd_empty_expected[20];
        ripemd160(NULL, 0, ripemd_empty_expected);
        test_script_bytes("OP_RIPEMD160 empty", ripemd_empty, sizeof(ripemd_empty),
                          ripemd_empty_expected, 20);

        /* RIPEMD160("abc") */
        uint8_t ripemd_abc[] = {0x03, 'a', 'b', 'c', OP_RIPEMD160};
        uint8_t ripemd_abc_expected[20];
        ripemd160((const uint8_t *)"abc", 3, ripemd_abc_expected);
        test_script_bytes("OP_RIPEMD160 'abc'", ripemd_abc, sizeof(ripemd_abc),
                          ripemd_abc_expected, 20);

        /*
         * OP_SHA256: SHA-256 hash
         * Test vector: SHA256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
         */
        uint8_t sha256_empty[] = {OP_0, OP_SHA256};
        uint8_t sha256_empty_expected[32];
        sha256(NULL, 0, sha256_empty_expected);
        test_script_bytes("OP_SHA256 empty", sha256_empty, sizeof(sha256_empty),
                          sha256_empty_expected, 32);

        /* SHA256("abc") */
        uint8_t sha256_abc[] = {0x03, 'a', 'b', 'c', OP_SHA256};
        uint8_t sha256_abc_expected[32];
        sha256((const uint8_t *)"abc", 3, sha256_abc_expected);
        test_script_bytes("OP_SHA256 'abc'", sha256_abc, sizeof(sha256_abc),
                          sha256_abc_expected, 32);

        /*
         * OP_HASH160: RIPEMD160(SHA256(x))
         * This is the standard Bitcoin address hash
         */
        uint8_t hash160_empty[] = {OP_0, OP_HASH160};
        uint8_t hash160_empty_expected[20];
        hash160(NULL, 0, hash160_empty_expected);
        test_script_bytes("OP_HASH160 empty", hash160_empty, sizeof(hash160_empty),
                          hash160_empty_expected, 20);

        /* HASH160("abc") */
        uint8_t hash160_abc[] = {0x03, 'a', 'b', 'c', OP_HASH160};
        uint8_t hash160_abc_expected[20];
        hash160((const uint8_t *)"abc", 3, hash160_abc_expected);
        test_script_bytes("OP_HASH160 'abc'", hash160_abc, sizeof(hash160_abc),
                          hash160_abc_expected, 20);

        /*
         * OP_HASH256: SHA256(SHA256(x))
         * This is the standard Bitcoin double-hash
         */
        uint8_t hash256_empty[] = {OP_0, OP_HASH256};
        uint8_t hash256_empty_expected[32];
        sha256d(NULL, 0, hash256_empty_expected);
        test_script_bytes("OP_HASH256 empty", hash256_empty, sizeof(hash256_empty),
                          hash256_empty_expected, 32);

        /* HASH256("abc") */
        uint8_t hash256_abc[] = {0x03, 'a', 'b', 'c', OP_HASH256};
        uint8_t hash256_abc_expected[32];
        sha256d((const uint8_t *)"abc", 3, hash256_abc_expected);
        test_script_bytes("OP_HASH256 'abc'", hash256_abc, sizeof(hash256_abc),
                          hash256_abc_expected, 32);

        /* Hash opcodes require stack element */
        uint8_t hash_underflow[] = {OP_SHA256};
        test_script_error("Hash underflow", hash_underflow, sizeof(hash_underflow),
                          SCRIPT_ERR_INVALID_STACK_OPERATION);

        /*
         * OP_CODESEPARATOR: Should succeed and do nothing
         */
        uint8_t codesep[] = {OP_1, OP_CODESEPARATOR};
        test_script("OP_CODESEPARATOR", codesep, sizeof(codesep), ECHO_TRUE, 1);

        /*
         * OP_CHECKSIG: Without transaction context, returns false
         * Stack needs: <sig> <pubkey>
         * With empty sig and empty pubkey, should return 0
         */
        uint8_t checksig_empty[] = {OP_0, OP_0, OP_CHECKSIG};
        test_script("OP_CHECKSIG empty sig/pubkey", checksig_empty,
                    sizeof(checksig_empty), ECHO_TRUE, 0);

        /* CHECKSIG underflow */
        uint8_t checksig_underflow[] = {OP_1, OP_CHECKSIG};
        test_script_error("CHECKSIG underflow", checksig_underflow,
                          sizeof(checksig_underflow), SCRIPT_ERR_INVALID_STACK_OPERATION);

        /* CHECKSIGVERIFY with empty sig fails */
        uint8_t checksigverify[] = {OP_0, OP_0, OP_CHECKSIGVERIFY};
        test_script_error("CHECKSIGVERIFY fails without context", checksigverify,
                          sizeof(checksigverify), SCRIPT_ERR_CHECKSIGVERIFY);

        /*
         * OP_CHECKMULTISIG: m-of-n multisig with off-by-one bug
         * Stack: dummy sig... m pubkey... n
         *
         * 0-of-0 multisig with dummy (tests off-by-one bug)
         */
        uint8_t multisig_0_0[] = {
            OP_0,   /* Dummy (off-by-one bug) */
            OP_0,   /* n_sigs = 0 */
            OP_0,   /* n_keys = 0 */
            OP_CHECKMULTISIG
        };
        test_script("CHECKMULTISIG 0-of-0", multisig_0_0,
                    sizeof(multisig_0_0), ECHO_TRUE, 1);  /* 0-of-0 succeeds, pushes TRUE */

        /* 0-of-1 multisig: 0 required signatures, 1 key */
        uint8_t multisig_0_1[] = {
            OP_0,                              /* Dummy */
            OP_0,                              /* n_sigs = 0 */
            0x21,                              /* Push 33 bytes (fake pubkey) */
            0x02, 0x00, 0x00, 0x00, 0x00,      /* Fake compressed pubkey */
            0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00,
            OP_1,                              /* n_keys = 1 */
            OP_CHECKMULTISIG
        };
        test_script("CHECKMULTISIG 0-of-1", multisig_0_1,
                    sizeof(multisig_0_1), ECHO_TRUE, 1);  /* 0-of-1 succeeds, pushes TRUE */

        /* Missing dummy element (off-by-one bug) */
        uint8_t multisig_no_dummy[] = {
            OP_0,   /* This becomes n_sigs */
            OP_0,   /* This becomes n_keys */
            OP_CHECKMULTISIG  /* No dummy! */
        };
        test_script_error("CHECKMULTISIG missing dummy", multisig_no_dummy,
                          sizeof(multisig_no_dummy), SCRIPT_ERR_INVALID_STACK_OPERATION);

        /* NULLDUMMY: With non-empty dummy, fails if NULLDUMMY flag set */
        uint8_t multisig_bad_dummy[] = {
            OP_1,   /* Dummy is OP_1 (not empty!) */
            OP_0,   /* n_sigs = 0 */
            OP_0,   /* n_keys = 0 */
            OP_CHECKMULTISIG
        };
        test_script_flags("CHECKMULTISIG NULLDUMMY violation", multisig_bad_dummy,
                          sizeof(multisig_bad_dummy), SCRIPT_VERIFY_NULLDUMMY,
                          ECHO_FALSE, SCRIPT_ERR_SIG_NULLDUMMY);

        /* With no flags, non-empty dummy is OK */
        test_script("CHECKMULTISIG non-empty dummy allowed", multisig_bad_dummy,
                    sizeof(multisig_bad_dummy), ECHO_TRUE, 1);  /* Succeeds, pushes TRUE */

        /*
         * OP_SHA1: Implemented for Bitcoin Script compatibility.
         * SHA1("") = da39a3ee5e6b4b0d3255bfef95601890afd80709
         */
        uint8_t sha1[] = {OP_0, OP_SHA1};
        test_script("OP_SHA1 empty", sha1, sizeof(sha1), ECHO_TRUE, 0);  /* Stack has 20-byte hash */
    }

    /*
     * ==========================================
     * SUMMARY
     * ==========================================
     */
    test_suite_end();
    return test_global_summary();
}
