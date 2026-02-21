/*
 * Bitcoin Echo — BIP-342 Tapscript Test Vectors
 *
 * Tests for OP_CHECKSIGADD and OP_CHECKSIG behavior under BIP-342 rules:
 *   1. OP_CHECKSIGADD with 32-byte key, empty sig -> push n unchanged
 *   2. OP_CHECKSIGADD with 33-byte key (unknown type), non-empty sig -> n++
 *   3. OP_CHECKSIGADD with 33-byte key (unknown type), empty sig -> n unchanged
 *   4. OP_CHECKSIGADD with 0-byte key -> fails SCRIPT_ERR_PUBKEYTYPE
 *   5. OP_CHECKSIG in Tapscript with 33-byte key, non-empty sig -> true
 *   6. OP_CHECKSIG in Tapscript with 0-byte key -> fails SCRIPT_ERR_PUBKEYTYPE
 *   7. Tapscript with OP_SUCCESS opcode (0xbb) -> immediate success
 *   8. Tapscript MINIMALIF enforcement -> OP_IF with non-{0x00,0x01} arg fails
 *
 * BIP-342 key type dispatch rule:
 *   pubkey_len == 0   -> fail SCRIPT_ERR_PUBKEYTYPE
 *   pubkey_len == 32  -> Schnorr verify (known key type)
 *   pubkey_len other  -> succeed without crypto (unknown key type upgrade rule)
 *
 * Build once. Build right. Stop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "script.h"
#include "test_utils.h"

/* ============================================================================
 * Test Helpers
 * ============================================================================ */

/*
 * Build a Tapscript context with is_tapscript=true and reasonable defaults.
 * No transaction context — signature verification must not reach crypto layer
 * for unknown key type tests.
 */
static echo_result_t make_tapscript_ctx(script_context_t *ctx) {
  echo_result_t res = script_context_init(ctx, SCRIPT_VERIFY_TAPROOT);
  if (res != ECHO_OK) {
    return res;
  }
  ctx->is_tapscript = ECHO_TRUE;
  /* tapleaf_hash all-zeros is fine for unknown-key-type tests (no Schnorr) */
  memset(ctx->tapleaf_hash, 0, 32);
  memset(ctx->tap_internal_key, 0, 32);
  ctx->key_version = 0;
  return ECHO_OK;
}

/*
 * Push a byte array onto the stack, aborting on failure.
 * Returns ECHO_OK on success.
 */
static echo_result_t push_bytes(script_context_t *ctx, const uint8_t *data,
                                size_t len) {
  return stack_push(&ctx->stack, data, len);
}

/*
 * Push a script number (n) as minimal encoding.
 */
static echo_result_t push_n(script_context_t *ctx, script_num_t n) {
  return stack_push_num(&ctx->stack, n);
}

/* ============================================================================
 * Test: OP_CHECKSIGADD with 32-byte key and empty sig
 *
 * BIP-342: Empty signature -> push n unchanged (no increment, no crypto).
 * This path is handled before the pubkey length check, so even a 32-byte
 * key reaches it correctly.
 * ============================================================================ */
static void test_checksigadd_32byte_key_empty_sig(void) {
  test_case("OP_CHECKSIGADD with 32-byte key and empty sig -> n unchanged");

  script_context_t ctx;
  if (make_tapscript_ctx(&ctx) != ECHO_OK) {
    test_fail("context init failed");
    return;
  }

  /* Stack layout (bottom to top): sig n pubkey
   * OP_CHECKSIGADD pops pubkey, n, sig in that order from top.
   * So we push: sig (empty), n=5, pubkey (32 bytes).
   */
  uint8_t pubkey_32[32];
  memset(pubkey_32, 0xAA, 32);

  /* Push sig (empty) */
  if (push_bytes(&ctx, NULL, 0) != ECHO_OK) {
    test_fail("push sig failed");
    script_context_free(&ctx);
    return;
  }
  /* Push n=5 */
  if (push_n(&ctx, 5) != ECHO_OK) {
    test_fail("push n failed");
    script_context_free(&ctx);
    return;
  }
  /* Push pubkey */
  if (push_bytes(&ctx, pubkey_32, 32) != ECHO_OK) {
    test_fail("push pubkey failed");
    script_context_free(&ctx);
    return;
  }

  /* Execute: single OP_CHECKSIGADD byte */
  uint8_t script[1] = {OP_CHECKSIGADD};
  echo_result_t res = script_execute_tapscript(&ctx, script, 1);

  if (res != ECHO_OK) {
    test_fail_int("expected ECHO_OK", ECHO_OK, res);
    script_context_free(&ctx);
    return;
  }

  /* Stack should have exactly one element: n=5 unchanged */
  if (ctx.stack.count != 1) {
    test_fail_uint("expected 1 element on stack", 1, ctx.stack.count);
    script_context_free(&ctx);
    return;
  }

  script_num_t result_n;
  const stack_element_t *top;
  stack_peek(&ctx.stack, &top);
  echo_result_t decode_res =
      script_num_decode(top->data, top->len, &result_n, ECHO_TRUE,
                        SCRIPT_NUM_MAX_SIZE);
  if (decode_res != ECHO_OK) {
    test_fail("could not decode result n");
    script_context_free(&ctx);
    return;
  }

  if (result_n != 5) {
    test_fail_int("n should be unchanged at 5", 5, (long)result_n);
    script_context_free(&ctx);
    return;
  }

  script_context_free(&ctx);
  test_pass();
}

/* ============================================================================
 * Test: OP_CHECKSIGADD with 33-byte key (unknown type), non-empty sig
 *
 * BIP-342 unknown key type upgrade rule: pubkey_len != 0 and != 32 means
 * treat as successful verification without cryptographic check. n is
 * incremented (sig is non-empty, so it "succeeds").
 * ============================================================================ */
static void test_checksigadd_unknown_key_nonempty_sig(void) {
  test_case("OP_CHECKSIGADD with 33-byte key (unknown type), non-empty sig -> "
            "n incremented");

  script_context_t ctx;
  if (make_tapscript_ctx(&ctx) != ECHO_OK) {
    test_fail("context init failed");
    return;
  }

  /* 33-byte "unknown" pubkey (not a valid 32-byte x-only key) */
  uint8_t pubkey_33[33];
  pubkey_33[0] = 0x02; /* compressed EC prefix — not valid for Tapscript */
  memset(pubkey_33 + 1, 0xBB, 32);

  /* 64-byte arbitrary "signature" — crypto is never checked for unknown type */
  uint8_t sig_64[64];
  memset(sig_64, 0x55, 64);

  /* Push sig */
  if (push_bytes(&ctx, sig_64, 64) != ECHO_OK) {
    test_fail("push sig failed");
    script_context_free(&ctx);
    return;
  }
  /* Push n=3 */
  if (push_n(&ctx, 3) != ECHO_OK) {
    test_fail("push n failed");
    script_context_free(&ctx);
    return;
  }
  /* Push pubkey */
  if (push_bytes(&ctx, pubkey_33, 33) != ECHO_OK) {
    test_fail("push pubkey failed");
    script_context_free(&ctx);
    return;
  }

  uint8_t script[1] = {OP_CHECKSIGADD};
  echo_result_t res = script_execute_tapscript(&ctx, script, 1);

  if (res != ECHO_OK) {
    test_fail_int("expected ECHO_OK for unknown key type", ECHO_OK, res);
    printf("    Error: %s\n", script_error_string(ctx.error));
    script_context_free(&ctx);
    return;
  }

  if (ctx.stack.count != 1) {
    test_fail_uint("expected 1 element on stack", 1, ctx.stack.count);
    script_context_free(&ctx);
    return;
  }

  script_num_t result_n;
  const stack_element_t *top;
  stack_peek(&ctx.stack, &top);
  echo_result_t decode_res =
      script_num_decode(top->data, top->len, &result_n, ECHO_TRUE,
                        SCRIPT_NUM_MAX_SIZE);
  if (decode_res != ECHO_OK) {
    test_fail("could not decode result n");
    script_context_free(&ctx);
    return;
  }

  /* n should be incremented: 3 -> 4 */
  if (result_n != 4) {
    test_fail_int("n should be incremented to 4", 4, (long)result_n);
    script_context_free(&ctx);
    return;
  }

  script_context_free(&ctx);
  test_pass();
}

/* ============================================================================
 * Test: OP_CHECKSIGADD with 33-byte key (unknown type), empty sig
 *
 * BIP-342: Empty sig is handled before pubkey length check. The empty-sig
 * path pushes n unchanged regardless of pubkey type.
 * ============================================================================ */
static void test_checksigadd_unknown_key_empty_sig(void) {
  test_case("OP_CHECKSIGADD with 33-byte key (unknown type), empty sig -> n "
            "unchanged");

  script_context_t ctx;
  if (make_tapscript_ctx(&ctx) != ECHO_OK) {
    test_fail("context init failed");
    return;
  }

  uint8_t pubkey_33[33];
  pubkey_33[0] = 0x03;
  memset(pubkey_33 + 1, 0xCC, 32);

  /* Push sig (empty) */
  if (push_bytes(&ctx, NULL, 0) != ECHO_OK) {
    test_fail("push sig failed");
    script_context_free(&ctx);
    return;
  }
  /* Push n=7 */
  if (push_n(&ctx, 7) != ECHO_OK) {
    test_fail("push n failed");
    script_context_free(&ctx);
    return;
  }
  /* Push pubkey */
  if (push_bytes(&ctx, pubkey_33, 33) != ECHO_OK) {
    test_fail("push pubkey failed");
    script_context_free(&ctx);
    return;
  }

  uint8_t script[1] = {OP_CHECKSIGADD};
  echo_result_t res = script_execute_tapscript(&ctx, script, 1);

  if (res != ECHO_OK) {
    test_fail_int("expected ECHO_OK", ECHO_OK, res);
    script_context_free(&ctx);
    return;
  }

  if (ctx.stack.count != 1) {
    test_fail_uint("expected 1 element on stack", 1, ctx.stack.count);
    script_context_free(&ctx);
    return;
  }

  script_num_t result_n;
  const stack_element_t *top;
  stack_peek(&ctx.stack, &top);
  echo_result_t decode_res =
      script_num_decode(top->data, top->len, &result_n, ECHO_TRUE,
                        SCRIPT_NUM_MAX_SIZE);
  if (decode_res != ECHO_OK) {
    test_fail("could not decode result n");
    script_context_free(&ctx);
    return;
  }

  /* n should be unchanged at 7 (empty sig takes the early-exit path) */
  if (result_n != 7) {
    test_fail_int("n should be unchanged at 7", 7, (long)result_n);
    script_context_free(&ctx);
    return;
  }

  script_context_free(&ctx);
  test_pass();
}

/* ============================================================================
 * Test: OP_CHECKSIGADD with 0-byte key -> fails SCRIPT_ERR_PUBKEYTYPE
 *
 * BIP-342: Empty pubkey (length == 0) is explicitly invalid and must fail
 * with SCRIPT_ERR_PUBKEYTYPE regardless of signature content.
 * ============================================================================ */
static void test_checksigadd_empty_key_fails(void) {
  test_case("OP_CHECKSIGADD with 0-byte key -> fails SCRIPT_ERR_PUBKEYTYPE");

  script_context_t ctx;
  if (make_tapscript_ctx(&ctx) != ECHO_OK) {
    test_fail("context init failed");
    return;
  }

  uint8_t sig_64[64];
  memset(sig_64, 0x11, 64);

  /* Push sig (non-empty so we reach the pubkey check) */
  if (push_bytes(&ctx, sig_64, 64) != ECHO_OK) {
    test_fail("push sig failed");
    script_context_free(&ctx);
    return;
  }
  /* Push n=0 */
  if (push_n(&ctx, 0) != ECHO_OK) {
    test_fail("push n failed");
    script_context_free(&ctx);
    return;
  }
  /* Push pubkey (empty = 0 bytes) */
  if (push_bytes(&ctx, NULL, 0) != ECHO_OK) {
    test_fail("push pubkey failed");
    script_context_free(&ctx);
    return;
  }

  uint8_t script[1] = {OP_CHECKSIGADD};
  echo_result_t res = script_execute_tapscript(&ctx, script, 1);

  if (res == ECHO_OK) {
    test_fail("expected failure but got ECHO_OK");
    script_context_free(&ctx);
    return;
  }

  if (ctx.error != SCRIPT_ERR_PUBKEYTYPE) {
    test_fail_int("expected SCRIPT_ERR_PUBKEYTYPE", SCRIPT_ERR_PUBKEYTYPE,
                  ctx.error);
    printf("    Got error: %s\n", script_error_string(ctx.error));
    script_context_free(&ctx);
    return;
  }

  script_context_free(&ctx);
  test_pass();
}

/* ============================================================================
 * Test: OP_CHECKSIG in Tapscript with 33-byte key, non-empty sig -> succeeds
 *
 * BIP-342 unknown key type upgrade rule applies to OP_CHECKSIG too.
 * A 33-byte pubkey in Tapscript is unknown type -> push true without crypto.
 * ============================================================================ */
static void test_checksig_tapscript_unknown_key(void) {
  test_case(
      "OP_CHECKSIG in Tapscript with 33-byte key (unknown type) -> succeeds");

  script_context_t ctx;
  if (make_tapscript_ctx(&ctx) != ECHO_OK) {
    test_fail("context init failed");
    return;
  }

  /* 33-byte "unknown" pubkey */
  uint8_t pubkey_33[33];
  pubkey_33[0] = 0x02;
  memset(pubkey_33 + 1, 0xDE, 32);

  /* 64-byte arbitrary "signature" */
  uint8_t sig_64[64];
  memset(sig_64, 0x77, 64);

  /* Stack: sig pubkey (pubkey is on top, sig below for CHECKSIG) */
  if (push_bytes(&ctx, sig_64, 64) != ECHO_OK) {
    test_fail("push sig failed");
    script_context_free(&ctx);
    return;
  }
  if (push_bytes(&ctx, pubkey_33, 33) != ECHO_OK) {
    test_fail("push pubkey failed");
    script_context_free(&ctx);
    return;
  }

  uint8_t script[1] = {OP_CHECKSIG};
  echo_result_t res = script_execute_tapscript(&ctx, script, 1);

  if (res != ECHO_OK) {
    test_fail_int("expected ECHO_OK for unknown key type in CHECKSIG", ECHO_OK,
                  res);
    printf("    Error: %s\n", script_error_string(ctx.error));
    script_context_free(&ctx);
    return;
  }

  /* Stack should have exactly one element: true (0x01) */
  if (ctx.stack.count != 1) {
    test_fail_uint("expected 1 element on stack", 1, ctx.stack.count);
    script_context_free(&ctx);
    return;
  }

  const stack_element_t *top;
  stack_peek(&ctx.stack, &top);
  /* True is represented as {0x01} in Bitcoin Script */
  if (top->len != 1 || top->data[0] != 0x01) {
    test_fail("expected true (0x01) on stack");
    printf("    Got: len=%zu, data[0]=%02x\n", top->len,
           top->len > 0 ? top->data[0] : 0);
    script_context_free(&ctx);
    return;
  }

  script_context_free(&ctx);
  test_pass();
}

/* ============================================================================
 * Test: OP_CHECKSIG in Tapscript with 0-byte key -> fails SCRIPT_ERR_PUBKEYTYPE
 * ============================================================================ */
static void test_checksig_tapscript_empty_key_fails(void) {
  test_case(
      "OP_CHECKSIG in Tapscript with 0-byte key -> fails SCRIPT_ERR_PUBKEYTYPE");

  script_context_t ctx;
  if (make_tapscript_ctx(&ctx) != ECHO_OK) {
    test_fail("context init failed");
    return;
  }

  uint8_t sig_64[64];
  memset(sig_64, 0x33, 64);

  /* Push sig */
  if (push_bytes(&ctx, sig_64, 64) != ECHO_OK) {
    test_fail("push sig failed");
    script_context_free(&ctx);
    return;
  }
  /* Push empty pubkey (0 bytes) */
  if (push_bytes(&ctx, NULL, 0) != ECHO_OK) {
    test_fail("push pubkey failed");
    script_context_free(&ctx);
    return;
  }

  uint8_t script[1] = {OP_CHECKSIG};
  echo_result_t res = script_execute_tapscript(&ctx, script, 1);

  if (res == ECHO_OK) {
    test_fail("expected failure but got ECHO_OK");
    script_context_free(&ctx);
    return;
  }

  if (ctx.error != SCRIPT_ERR_PUBKEYTYPE) {
    test_fail_int("expected SCRIPT_ERR_PUBKEYTYPE", SCRIPT_ERR_PUBKEYTYPE,
                  ctx.error);
    printf("    Got error: %s\n", script_error_string(ctx.error));
    script_context_free(&ctx);
    return;
  }

  script_context_free(&ctx);
  test_pass();
}

/* ============================================================================
 * Test: Tapscript with OP_SUCCESS opcode (0xbb) -> immediate success
 *
 * BIP-342: OP_SUCCESSx opcodes (range 0xbb-0xfe) cause immediate success
 * regardless of stack state or what follows in the script.
 * ============================================================================ */
static void test_opsuccess_immediate(void) {
  test_case("Tapscript OP_SUCCESS (0xbb) causes immediate success");

  script_context_t ctx;
  if (make_tapscript_ctx(&ctx) != ECHO_OK) {
    test_fail("context init failed");
    return;
  }

  /*
   * Script: OP_SUCCESS(0xbb) followed by OP_RETURN
   * Without OP_SUCCESS semantics, OP_RETURN would make this fail.
   * With OP_SUCCESS semantics, we succeed before reaching OP_RETURN.
   */
  uint8_t script[2] = {0xbb, OP_RETURN};
  echo_result_t res = script_execute_tapscript(&ctx, script, 2);

  if (res != ECHO_OK) {
    test_fail_int("expected ECHO_OK for OP_SUCCESS", ECHO_OK, res);
    printf("    Error: %s\n", script_error_string(ctx.error));
    script_context_free(&ctx);
    return;
  }

  script_context_free(&ctx);
  test_pass();
}

/* ============================================================================
 * Test: Tapscript MINIMALIF enforcement
 *
 * BIP-342: In Tapscript, OP_IF and OP_NOTIF arguments must be exactly
 * 0x00 (empty/false) or 0x01 (true). Any other value fails with
 * SCRIPT_ERR_TAPSCRIPT_MINIMALIF.
 * ============================================================================ */
static void test_minimalif_enforced(void) {
  test_case("Tapscript MINIMALIF: OP_IF with non-minimal arg fails");

  script_context_t ctx;
  if (make_tapscript_ctx(&ctx) != ECHO_OK) {
    test_fail("context init failed");
    return;
  }

  /*
   * Push 0x02 onto stack (non-minimal: not 0x00 or 0x01).
   * Then execute OP_IF ... OP_ENDIF.
   * In Tapscript, this must fail with SCRIPT_ERR_TAPSCRIPT_MINIMALIF.
   */
  uint8_t two_byte[1] = {0x02};
  if (push_bytes(&ctx, two_byte, 1) != ECHO_OK) {
    test_fail("push non-minimal value failed");
    script_context_free(&ctx);
    return;
  }

  /*
   * Script: OP_IF OP_1 OP_ENDIF
   * 0x63 = OP_IF, 0x51 = OP_1, 0x68 = OP_ENDIF
   */
  uint8_t script[3] = {OP_IF, OP_1, OP_ENDIF};
  echo_result_t res = script_execute_tapscript(&ctx, script, 3);

  if (res == ECHO_OK) {
    test_fail("expected MINIMALIF failure but got ECHO_OK");
    script_context_free(&ctx);
    return;
  }

  if (ctx.error != SCRIPT_ERR_TAPSCRIPT_MINIMALIF) {
    test_fail_int("expected SCRIPT_ERR_TAPSCRIPT_MINIMALIF",
                  SCRIPT_ERR_TAPSCRIPT_MINIMALIF, ctx.error);
    printf("    Got error: %s\n", script_error_string(ctx.error));
    script_context_free(&ctx);
    return;
  }

  script_context_free(&ctx);
  test_pass();
}

/* ============================================================================
 * Test: OP_CHECKSIGADD disabled in legacy (non-tapscript) context
 *
 * OP_CHECKSIGADD (0xba) must return SCRIPT_ERR_BAD_OPCODE when executed
 * outside Tapscript. This validates the existing stub behavior.
 * ============================================================================ */
static void test_checksigadd_disabled_in_legacy(void) {
  test_case("OP_CHECKSIGADD in legacy script -> SCRIPT_ERR_BAD_OPCODE");

  script_context_t ctx;
  echo_result_t res = script_context_init(&ctx, SCRIPT_VERIFY_NONE);
  if (res != ECHO_OK) {
    test_fail("context init failed");
    return;
  }
  /* is_tapscript is ECHO_FALSE by default */

  uint8_t script[1] = {OP_CHECKSIGADD};
  res = script_execute(&ctx, script, 1);

  if (res == ECHO_OK) {
    test_fail("expected failure for legacy OP_CHECKSIGADD");
    script_context_free(&ctx);
    return;
  }

  if (ctx.error != SCRIPT_ERR_BAD_OPCODE) {
    test_fail_int("expected SCRIPT_ERR_BAD_OPCODE", SCRIPT_ERR_BAD_OPCODE,
                  ctx.error);
    script_context_free(&ctx);
    return;
  }

  script_context_free(&ctx);
  test_pass();
}

/* ============================================================================
 * Test: OP_CHECKMULTISIG disabled in Tapscript
 *
 * BIP-342: OP_CHECKMULTISIG and OP_CHECKMULTISIGVERIFY must fail with
 * SCRIPT_ERR_TAPSCRIPT_CHECKMULTISIG when executed in Tapscript context.
 * ============================================================================ */
static void test_checkmultisig_disabled_in_tapscript(void) {
  test_case("OP_CHECKMULTISIG in Tapscript -> SCRIPT_ERR_TAPSCRIPT_CHECKMULTISIG");

  script_context_t ctx;
  if (make_tapscript_ctx(&ctx) != ECHO_OK) {
    test_fail("context init failed");
    return;
  }

  uint8_t script[1] = {OP_CHECKMULTISIG};
  echo_result_t res = script_execute_tapscript(&ctx, script, 1);

  if (res == ECHO_OK) {
    test_fail("expected failure for CHECKMULTISIG in Tapscript");
    script_context_free(&ctx);
    return;
  }

  if (ctx.error != SCRIPT_ERR_TAPSCRIPT_CHECKMULTISIG) {
    test_fail_int("expected SCRIPT_ERR_TAPSCRIPT_CHECKMULTISIG",
                  SCRIPT_ERR_TAPSCRIPT_CHECKMULTISIG, ctx.error);
    printf("    Got error: %s\n", script_error_string(ctx.error));
    script_context_free(&ctx);
    return;
  }

  script_context_free(&ctx);
  test_pass();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
  test_suite_begin("BIP-342 Tapscript");

  test_section("OP_CHECKSIGADD key type dispatch");
  test_checksigadd_32byte_key_empty_sig();
  test_checksigadd_unknown_key_nonempty_sig();
  test_checksigadd_unknown_key_empty_sig();
  test_checksigadd_empty_key_fails();

  test_section("OP_CHECKSIG Tapscript key type dispatch");
  test_checksig_tapscript_unknown_key();
  test_checksig_tapscript_empty_key_fails();

  test_section("OP_SUCCESS and special rules");
  test_opsuccess_immediate();
  test_minimalif_enforced();
  test_checksigadd_disabled_in_legacy();
  test_checkmultisig_disabled_in_tapscript();

  test_suite_end();
  return test_global_summary();
}
