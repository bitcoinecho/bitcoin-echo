/*
 * Bitcoin Echo — Mining Module Unit Tests
 *
 * Tests for regtest mining support:
 *   - Coinbase transaction construction
 *   - BIP-34 height encoding
 *   - Regtest genesis block
 *   - Nonce mining
 *
 * Build once. Build right. Stop.
 */

#include "test_utils.h"
#include "block.h"
#include "echo_types.h"
#include "mining.h"
#include "tx.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * ============================================================================
 * HEIGHT ENCODING TESTS
 * ============================================================================
 */

static void test_height_encoding_zero(void) {
  uint8_t buf[9];
  size_t written;

  test_case("Height encoding: 0");

  echo_result_t res = coinbase_encode_height(0, buf, sizeof(buf), &written);
  if (res != ECHO_OK) {
    test_fail_int("encode_height failed", ECHO_OK, res);
    return;
  }
  if (written != 1) {
    test_fail_uint("wrong length", 1, written);
    return;
  }
  if (buf[0] != 0x00) {
    test_fail_uint("wrong value (expected OP_0)", 0x00, buf[0]);
    return;
  }
  test_pass();
}

static void test_height_encoding_small(void) {
  uint8_t buf[9];
  size_t written;

  /* Height 1: should encode as 0x01 0x01 (push 1 byte, value 1) */
  test_case("Height encoding: 1");
  echo_result_t res = coinbase_encode_height(1, buf, sizeof(buf), &written);
  if (res != ECHO_OK) {
    test_fail_int("encode_height failed", ECHO_OK, res);
    return;
  }
  if (written != 2 || buf[0] != 0x01 || buf[1] != 0x01) {
    test_fail("wrong encoding for height 1");
    return;
  }
  test_pass();

  /* Height 127: should encode as 0x01 0x7f */
  test_case("Height encoding: 127");
  res = coinbase_encode_height(127, buf, sizeof(buf), &written);
  if (res != ECHO_OK || written != 2 || buf[0] != 0x01 || buf[1] != 0x7f) {
    test_fail("wrong encoding for height 127");
    return;
  }
  test_pass();

  /* Height 128: needs 2 bytes because high bit set */
  test_case("Height encoding: 128 (needs sign byte)");
  res = coinbase_encode_height(128, buf, sizeof(buf), &written);
  if (res != ECHO_OK) {
    test_fail_int("encode_height failed", ECHO_OK, res);
    return;
  }
  if (written != 3) {
    test_fail_uint("wrong length", 3, written);
    return;
  }
  if (buf[0] != 0x02 || buf[1] != 0x80 || buf[2] != 0x00) {
    test_fail("wrong encoding for height 128");
    return;
  }
  test_pass();
}

static void test_height_encoding_medium(void) {
  uint8_t buf[9];
  size_t written;

  /* Height 256 = 0x100 */
  test_case("Height encoding: 256");
  echo_result_t res = coinbase_encode_height(256, buf, sizeof(buf), &written);
  if (res != ECHO_OK || written != 3) {
    test_fail("wrong encoding for height 256");
    return;
  }
  if (buf[0] != 0x02 || buf[1] != 0x00 || buf[2] != 0x01) {
    test_fail("wrong bytes for height 256");
    return;
  }
  test_pass();

  /* Height 500000 = 0x07A120 */
  test_case("Height encoding: 500000");
  res = coinbase_encode_height(500000, buf, sizeof(buf), &written);
  if (res != ECHO_OK || written != 4) {
    test_fail("wrong encoding for height 500000");
    return;
  }
  if (buf[0] != 0x03 || buf[1] != 0x20 || buf[2] != 0xa1 || buf[3] != 0x07) {
    test_fail("wrong bytes for height 500000");
    return;
  }
  test_pass();
}

static void test_height_encoding_buffer_too_small(void) {
  uint8_t buf[1];
  size_t written;

  test_case("Height encoding: buffer too small");
  /* Height 256 needs 3 bytes but buffer is only 1 */
  echo_result_t res = coinbase_encode_height(256, buf, sizeof(buf), &written);
  if (res != ECHO_ERR_BUFFER_TOO_SMALL) {
    test_fail_int("should fail with buffer too small", ECHO_ERR_BUFFER_TOO_SMALL,
                  res);
    return;
  }
  test_pass();
}

/*
 * ============================================================================
 * COINBASE CREATION TESTS
 * ============================================================================
 */

static void test_coinbase_create_simple(void) {
  coinbase_params_t params;
  tx_t tx;

  test_case("Coinbase creation: simple");

  coinbase_params_init(&params);
  params.height = 1;
  params.value = 5000000000LL; /* 50 BTC */

  echo_result_t res = coinbase_create(&params, &tx);
  if (res != ECHO_OK) {
    test_fail_int("coinbase_create failed", ECHO_OK, res);
    return;
  }

  /* Verify structure */
  if (tx.version != 1) {
    test_fail_int("wrong version", 1, tx.version);
    tx_free(&tx);
    return;
  }
  if (tx.input_count != 1) {
    test_fail_uint("wrong input count", 1, tx.input_count);
    tx_free(&tx);
    return;
  }
  if (tx.output_count != 1) {
    test_fail_uint("wrong output count", 1, tx.output_count);
    tx_free(&tx);
    return;
  }
  if (tx.locktime != 0) {
    test_fail_uint("wrong locktime", 0, tx.locktime);
    tx_free(&tx);
    return;
  }

  /* Verify coinbase input */
  if (!tx_is_coinbase(&tx)) {
    test_fail("not detected as coinbase");
    tx_free(&tx);
    return;
  }

  /* Verify output value */
  if (tx.outputs[0].value != 5000000000LL) {
    test_fail("wrong output value");
    tx_free(&tx);
    return;
  }

  /* Verify default output script (OP_TRUE) */
  if (tx.outputs[0].script_pubkey_len != 1 ||
      tx.outputs[0].script_pubkey[0] != 0x51) {
    test_fail("wrong default output script");
    tx_free(&tx);
    return;
  }

  tx_free(&tx);
  test_pass();
}

static void test_coinbase_create_with_script(void) {
  coinbase_params_t params;
  tx_t tx;

  test_case("Coinbase creation: custom script");

  /* P2PKH-style output script */
  uint8_t script[] = {0x76, 0xa9, 0x14, /* OP_DUP OP_HASH160 PUSH20 */
                      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                      0x10, 0x11, 0x12, 0x13,
                      0x88, 0xac}; /* OP_EQUALVERIFY OP_CHECKSIG */

  coinbase_params_init(&params);
  params.height = 100;
  params.value = 2500000000LL; /* 25 BTC */
  params.output_script = script;
  params.output_script_len = sizeof(script);

  echo_result_t res = coinbase_create(&params, &tx);
  if (res != ECHO_OK) {
    test_fail_int("coinbase_create failed", ECHO_OK, res);
    return;
  }

  /* Verify output script */
  if (tx.outputs[0].script_pubkey_len != sizeof(script)) {
    test_fail_uint("wrong script length", sizeof(script),
                   tx.outputs[0].script_pubkey_len);
    tx_free(&tx);
    return;
  }
  if (memcmp(tx.outputs[0].script_pubkey, script, sizeof(script)) != 0) {
    test_fail("script content mismatch");
    tx_free(&tx);
    return;
  }

  tx_free(&tx);
  test_pass();
}

static void test_coinbase_create_with_extra_data(void) {
  coinbase_params_t params;
  tx_t tx;

  test_case("Coinbase creation: extra data");

  uint8_t extra[] = "Bitcoin Echo Test";

  coinbase_params_init(&params);
  params.height = 500;
  params.value = 1250000000LL; /* 12.5 BTC */
  params.extra_data = extra;
  params.extra_data_len = sizeof(extra) - 1; /* Exclude null terminator */

  echo_result_t res = coinbase_create(&params, &tx);
  if (res != ECHO_OK) {
    test_fail_int("coinbase_create failed", ECHO_OK, res);
    return;
  }

  /* Verify scriptsig contains height encoding + extra data */
  if (tx.inputs[0].script_sig_len <= sizeof(extra) - 1) {
    test_fail("scriptsig too short for extra data");
    tx_free(&tx);
    return;
  }

  tx_free(&tx);
  test_pass();
}

/*
 * ============================================================================
 * REGTEST GENESIS BLOCK TESTS
 * ============================================================================
 */

static void test_regtest_genesis_header(void) {
  block_header_t header;

  test_case("Regtest genesis header");

  block_genesis_header_regtest(&header);

  /* Verify regtest-specific values */
  if (header.version != REGTEST_GENESIS_VERSION) {
    test_fail_int("wrong version", REGTEST_GENESIS_VERSION, header.version);
    return;
  }
  if (header.timestamp != REGTEST_GENESIS_TIMESTAMP) {
    test_fail_uint("wrong timestamp", REGTEST_GENESIS_TIMESTAMP,
                   header.timestamp);
    return;
  }
  if (header.bits != REGTEST_GENESIS_BITS) {
    test_fail_uint("wrong bits", REGTEST_GENESIS_BITS, header.bits);
    return;
  }
  if (header.nonce != REGTEST_GENESIS_NONCE) {
    test_fail_uint("wrong nonce", REGTEST_GENESIS_NONCE, header.nonce);
    return;
  }

  /* Verify prev_hash is all zeros */
  hash256_t zero_hash = {{0}};
  if (memcmp(&header.prev_hash, &zero_hash, sizeof(hash256_t)) != 0) {
    test_fail("prev_hash is not all zeros");
    return;
  }

  test_pass();
}

static void test_regtest_difficulty_bits(void) {
  test_case("Regtest difficulty bits");

  /* Regtest should always use trivial difficulty */
  uint32_t bits = mining_get_difficulty_bits(0, ECHO_TRUE);
  if (bits != REGTEST_POWLIMIT_BITS) {
    test_fail_uint("wrong bits at height 0", REGTEST_POWLIMIT_BITS, bits);
    return;
  }

  bits = mining_get_difficulty_bits(1000, ECHO_TRUE);
  if (bits != REGTEST_POWLIMIT_BITS) {
    test_fail_uint("wrong bits at height 1000", REGTEST_POWLIMIT_BITS, bits);
    return;
  }

  bits = mining_get_difficulty_bits(100000, ECHO_TRUE);
  if (bits != REGTEST_POWLIMIT_BITS) {
    test_fail_uint("wrong bits at height 100000", REGTEST_POWLIMIT_BITS, bits);
    return;
  }

  test_pass();
}

/*
 * ============================================================================
 * NONCE MINING TESTS
 * ============================================================================
 */

static void test_mining_find_nonce_regtest(void) {
  block_header_t header;

  test_case("Mining: find nonce (regtest)");

  /* Set up a regtest header */
  memset(&header, 0, sizeof(header));
  header.version = 0x20000000;
  header.timestamp = 1700000000;
  header.bits = REGTEST_POWLIMIT_BITS;

  /* Mining should succeed quickly with regtest difficulty */
  echo_result_t res = mining_find_nonce(&header, 1000);
  if (res != ECHO_OK) {
    test_fail_int("mining_find_nonce failed", ECHO_OK, res);
    return;
  }

  /* Verify the found nonce produces valid PoW */
  hash256_t hash;
  hash256_t target;
  block_header_hash(&header, &hash);
  block_bits_to_target(header.bits, &target);

  if (!block_hash_meets_target(&hash, &target)) {
    test_fail("hash does not meet target");
    return;
  }

  test_pass();
}

static void test_mining_find_nonce_fails_impossible(void) {
  block_header_t header;

  test_case("Mining: fail on impossible target");

  /* Set up a header with very hard difficulty */
  memset(&header, 0, sizeof(header));
  header.version = 0x20000000;
  header.timestamp = 1700000000;
  header.bits = 0x03000001; /* Very very hard difficulty */

  /* Mining should fail to find nonce in limited range */
  echo_result_t res = mining_find_nonce(&header, 100);
  if (res != ECHO_ERR_NOT_FOUND) {
    test_fail_int("should fail with not found", ECHO_ERR_NOT_FOUND, res);
    return;
  }

  test_pass();
}

/*
 * ============================================================================
 * INITIALIZATION TESTS
 * ============================================================================
 */

static void test_block_template_init(void) {
  block_template_t tmpl;

  test_case("Block template init");

  block_template_init(&tmpl);

  if (tmpl.version != 0x20000000) {
    test_fail_int("wrong version", 0x20000000, tmpl.version);
    return;
  }
  if (tmpl.curtime != 0 || tmpl.bits != 0 || tmpl.height != 0 ||
      tmpl.mintime != 0 || tmpl.coinbase_value != 0) {
    test_fail("fields not zeroed");
    return;
  }

  test_pass();
}

static void test_coinbase_params_init(void) {
  coinbase_params_t params;

  test_case("Coinbase params init");

  coinbase_params_init(&params);

  if (params.height != 0 || params.value != 0) {
    test_fail("height/value not zeroed");
    return;
  }
  if (params.output_script != NULL || params.output_script_len != 0) {
    test_fail("output_script not NULL");
    return;
  }
  if (params.extra_data != NULL || params.extra_data_len != 0) {
    test_fail("extra_data not NULL");
    return;
  }
  if (params.include_witness_commitment != ECHO_FALSE) {
    test_fail("include_witness_commitment not FALSE");
    return;
  }

  test_pass();
}

/*
 * ============================================================================
 * TEST RUNNER
 * ============================================================================
 */

int main(void) {
  test_suite_begin("Mining Module Tests");

  /* Height encoding tests */
  test_section("Height Encoding");
  test_height_encoding_zero();
  test_height_encoding_small();
  test_height_encoding_medium();
  test_height_encoding_buffer_too_small();

  /* Coinbase creation tests */
  test_section("Coinbase Creation");
  test_coinbase_create_simple();
  test_coinbase_create_with_script();
  test_coinbase_create_with_extra_data();

  /* Regtest genesis tests */
  test_section("Regtest Genesis");
  test_regtest_genesis_header();
  test_regtest_difficulty_bits();

  /* Nonce mining tests */
  test_section("Nonce Mining");
  test_mining_find_nonce_regtest();
  test_mining_find_nonce_fails_impossible();

  /* Initialization tests */
  test_section("Initialization");
  test_block_template_init();
  test_coinbase_params_init();

  test_suite_end();
  return test_global_summary();
}
