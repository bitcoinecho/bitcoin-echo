/*
 * Bitcoin Echo — Block Test Vectors
 *
 * Test vectors for block parsing, serialization, and hash computation.
 * Includes the Bitcoin genesis block.
 *
 * Build once. Build right. Stop.
 */

#include "block.h"
#include "echo_types.h"
#include "test_utils.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 * Convert hex string to bytes.
 */
static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t max_len) {
  size_t len = strlen(hex);
  size_t i;
  unsigned int byte;

  if (len % 2 != 0)
    return 0;
  if (len / 2 > max_len)
    return 0;

  for (i = 0; i < len / 2; i++) {
    // NOLINTBEGIN(cert-err34-c) - sscanf is correct here: we check return value
    // and need exactly 2 hex chars, not all available hex like strtoul would
    // read
    if (sscanf(hex + i * 2, "%02x", &byte) != 1)
      return 0;
    // NOLINTEND(cert-err34-c)
    out[i] = (uint8_t)byte;
  }

  return len / 2;
}

/*
 * Print bytes as hex.
 */
static void print_hex(const uint8_t *data, size_t len) {
  size_t i;
  for (i = 0; i < len; i++) {
    printf("%02x", data[i]);
  }
}

/*
 * Reverse bytes for display (little-endian to big-endian).
 */
static void reverse_bytes(uint8_t *data, size_t len) {
  size_t i;
  uint8_t tmp;
  for (i = 0; i < len / 2; i++) {
    tmp = data[i];
    data[i] = data[len - 1 - i];
    data[len - 1 - i] = tmp;
  }
}

/*
 * Test genesis block header generation.
 */
static void test_genesis_header(void) {
  block_header_t header;
  hash256_t hash;
  uint8_t display_hash[32];

  /* Expected genesis block hash (displayed in big-endian) */
  static const char *expected_hash_hex =
      "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";
  uint8_t expected_hash[32];


  /* Generate genesis header */
  block_genesis_header(&header);

  /* Verify header fields */
  if (header.version != 1 || header.timestamp != 1231006505 ||
      header.bits != 0x1d00ffff || header.nonce != 2083236893) {
    test_fail("Genesis header fields incorrect");
    return;
  }

  /* Compute hash */
  if (block_header_hash(&header, &hash) != ECHO_OK) {
    test_fail("Genesis header hash computation failed");
    return;
  }

  /* Convert expected hash from big-endian display format */
  hex_to_bytes(expected_hash_hex, expected_hash, 32);
  reverse_bytes(expected_hash, 32);

  /* Copy and reverse for comparison */
  memcpy(display_hash, hash.bytes, 32);

  if (bytes_equal(hash.bytes, expected_hash, 32)) {
    test_pass();
    test_case("Genesis block hash correct");
      test_pass();
  } else {
    reverse_bytes(display_hash, 32);
    test_fail("Genesis block hash incorrect");
    printf("    Expected: %s\n", expected_hash_hex);
    printf("    Got:      ");
    print_hex(display_hash, 32);
    printf("\n");
  }
}

/*
 * Test block header parsing and serialization roundtrip.
 */
static void test_header_roundtrip(const char *name, const char *hex) {
  uint8_t data[BLOCK_HEADER_SIZE];
  uint8_t serialized[BLOCK_HEADER_SIZE];
  block_header_t header;
  echo_result_t result;


  if (hex_to_bytes(hex, data, sizeof(data)) != BLOCK_HEADER_SIZE) {
    test_fail("%s (invalid hex)");
    return;
  }

  /* Parse */
  result = block_header_parse(data, BLOCK_HEADER_SIZE, &header);
  if (result != ECHO_OK) {
    printf("  [FAIL] %s (parse failed: %d)\n", name, result);
    return;
  }

  /* Serialize */
  result = block_header_serialize(&header, serialized, sizeof(serialized));
  if (result != ECHO_OK) {
    printf("  [FAIL] %s (serialize failed: %d)\n", name, result);
    return;
  }

  /* Compare */
  if (bytes_equal(data, serialized, BLOCK_HEADER_SIZE)) {
    test_pass();
    test_case(name);
        test_pass();
  } else {
    test_fail("%s (roundtrip mismatch)");
  }
}

/*
 * Test block hash computation.
 */
static void test_block_hash(const char *name, const char *header_hex,
                            const char *expected_hash_hex) {
  uint8_t header_data[BLOCK_HEADER_SIZE];
  block_header_t header;
  hash256_t hash;
  uint8_t expected_hash[32];
  uint8_t display_hash[32];


  if (hex_to_bytes(header_hex, header_data, sizeof(header_data)) !=
      BLOCK_HEADER_SIZE) {
    test_fail("%s (invalid header hex)");
    return;
  }

  if (hex_to_bytes(expected_hash_hex, expected_hash, 32) != 32) {
    test_fail("%s (invalid expected hash hex)");
    return;
  }
  /* Expected hash is in big-endian display format, reverse to little-endian */
  reverse_bytes(expected_hash, 32);

  if (block_header_parse(header_data, BLOCK_HEADER_SIZE, &header) != ECHO_OK) {
    test_fail("%s (parse failed)");
    return;
  }

  if (block_header_hash(&header, &hash) != ECHO_OK) {
    test_fail("%s (hash computation failed)");
    return;
  }

  if (bytes_equal(hash.bytes, expected_hash, 32)) {
    test_pass();
    test_case(name);
        test_pass();
  } else {
    memcpy(display_hash, hash.bytes, 32);
    reverse_bytes(display_hash, 32);
    test_fail("%s");
    printf("    Expected: %s\n", expected_hash_hex);
    printf("    Got:      ");
    print_hex(display_hash, 32);
    printf("\n");
  }
}

/*
 * Test bits to target conversion.
 */
static void test_bits_to_target(const char *name, uint32_t bits,
                                const char *expected_target_hex) {
  hash256_t target;
  uint8_t expected_target[32];


  /* Parse expected target (big-endian display format) */
  memset(expected_target, 0, 32);
  if (strlen(expected_target_hex) > 0) {
    size_t hex_len = strlen(expected_target_hex);
    size_t byte_len = hex_len / 2;
    uint8_t temp[32];

    if (hex_to_bytes(expected_target_hex, temp, byte_len) != byte_len) {
      test_fail("%s (invalid expected target hex)");
      return;
    }

    /* Copy to expected_target, right-aligned (big-endian) */
    /* Then reverse to little-endian */
    memcpy(expected_target + (32 - byte_len), temp, byte_len);
    reverse_bytes(expected_target, 32);
  }

  if (block_bits_to_target(bits, &target) != ECHO_OK) {
    test_fail("%s (conversion failed)");
    return;
  }

  if (bytes_equal(target.bytes, expected_target, 32)) {
    test_pass();
    test_case(name);
        test_pass();
  } else {
    uint8_t display_target[32];
    memcpy(display_target, target.bytes, 32);
    reverse_bytes(display_target, 32);

    test_fail("%s");
    printf("    Expected: %s\n", expected_target_hex);
    printf("    Got:      ");
    print_hex(display_target, 32);
    printf("\n");
  }
}

/*
 * Test target to bits conversion.
 */
static void test_target_to_bits(const char *name, const char *target_hex,
                                uint32_t expected_bits) {
  hash256_t target;
  uint32_t bits;
  uint8_t temp[32];
  size_t hex_len;
  size_t byte_len;


  /* Parse target (big-endian display format) */
  memset(target.bytes, 0, 32);
  hex_len = strlen(target_hex);
  byte_len = hex_len / 2;

  if (byte_len > 0) {
    if (hex_to_bytes(target_hex, temp, byte_len) != byte_len) {
      test_fail("%s (invalid target hex)");
      return;
    }

    /* Copy right-aligned, then reverse to little-endian */
    memcpy(target.bytes + (32 - byte_len), temp, byte_len);
    reverse_bytes(target.bytes, 32);
  }

  if (block_target_to_bits(&target, &bits) != ECHO_OK) {
    test_fail("%s (conversion failed)");
    return;
  }

  if (bits == expected_bits) {
    test_pass();
    test_case(name);
        test_pass();
  } else {
    test_fail("%s");
    printf("    Expected bits: 0x%08x\n", expected_bits);
    printf("    Got bits:      0x%08x\n", bits);
  }
}

/*
 * Test proof-of-work validation.
 */
static void test_pow_check(const char *name, const char *hash_hex,
                           const char *target_hex, echo_bool_t expected) {
  hash256_t hash, target;
  uint8_t temp[32];
  size_t hex_len, byte_len;
  echo_bool_t result;


  /* Parse hash (little-endian in storage, but display is big-endian) */
  if (hex_to_bytes(hash_hex, temp, 32) != 32) {
    test_fail("%s (invalid hash hex)");
    return;
  }
  memcpy(hash.bytes, temp, 32);
  reverse_bytes(hash.bytes, 32);

  /* Parse target (big-endian display format) */
  memset(target.bytes, 0, 32);
  hex_len = strlen(target_hex);
  byte_len = hex_len / 2;

  if (byte_len > 0) {
    if (hex_to_bytes(target_hex, temp, byte_len) != byte_len) {
      test_fail("%s (invalid target hex)");
      return;
    }
    memcpy(target.bytes + (32 - byte_len), temp, byte_len);
    reverse_bytes(target.bytes, 32);
  }

  result = block_hash_meets_target(&hash, &target);

  if (result == expected) {
    test_pass();
    test_case(name);
        test_pass();
  } else {
    test_fail("%s");
    printf("    Expected: %s\n", expected ? "valid" : "invalid");
    printf("    Got:      %s\n", result ? "valid" : "invalid");
  }
}

/*
 * Test full block parsing.
 */
static void test_block_parse(const char *name, const char *hex,
                             size_t expected_tx_count) {
  uint8_t *data;
  size_t data_len;
  block_t block;
  size_t consumed;
  echo_result_t result;


  data_len = strlen(hex) / 2;
  data = malloc(data_len);
  if (data == NULL) {
    test_fail("%s (malloc failed)");
    return;
  }

  if (hex_to_bytes(hex, data, data_len) != data_len) {
    test_fail("%s (invalid hex)");
    free(data);
    return;
  }

  result = block_parse(data, data_len, &block, &consumed);

  if (result == ECHO_OK && consumed == data_len &&
      block.tx_count == expected_tx_count) {
    test_pass();
    test_case(name);
        test_pass();
  } else {
    test_fail("%s");
    printf("    Result: %d\n", result);
    if (result == ECHO_OK) {
      printf("    Consumed: %zu/%zu\n", consumed, data_len);
      printf("    TX count: %zu (expected %zu)\n", block.tx_count,
             expected_tx_count);
    }
  }

  block_free(&block);
  free(data);
}

int main(void) {
  test_suite_begin("Block Tests");

  /*
   * Genesis block tests
   */
  test_section("Genesis block tests");
  test_genesis_header();

  /* Genesis block header hex */
  const char *genesis_header_hex = "0100000000000000000000000000000000000000000"
                                   "000000000000000000000000000003ba3edfd"
                                   "7a7b12b27ac72c3e67768f617fc81bc3888a51323a9"
                                   "fb8aa4b1e5e4a29ab5f49ffff001d1dac2b7c";

  test_header_roundtrip("Genesis header roundtrip", genesis_header_hex);

  test_block_hash(
      "Genesis block hash", genesis_header_hex,
      "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");


  /*
   * Block 170 tests (first block with a non-coinbase transaction)
   */
  test_section("Block 170 tests");

  const char *block_170_header_hex = "0100000055bd840a78798ad0da853f68974f3d183"
                                     "e2bd1db6a842c1feecf222a00000000ff104ccb"
                                     "05421ab93e63f8c3ce5c2c2e9dbb37de2764b3a31"
                                     "75c8166562cac7d51b96a49ffff001d283e9e70";

  test_header_roundtrip("Block 170 header roundtrip", block_170_header_hex);

  test_block_hash(
      "Block 170 hash", block_170_header_hex,
      "00000000d1145790a8694403d4063f323d499e655c83426834d4ce2f8dd4a2ee");


  /*
   * Bits/target conversion tests
   */
  test_section("Bits to target tests");

  /* Genesis block difficulty (bits = 0x1d00ffff) */
  test_bits_to_target(
      "Genesis bits (0x1d00ffff)", 0x1d00ffff,
      "00000000ffff0000000000000000000000000000000000000000000000000000");

  /* A higher difficulty example */
  test_bits_to_target(
      "Higher difficulty (0x1b0404cb)", 0x1b0404cb,
      "00000000000404cb000000000000000000000000000000000000000000000000");

  /* Very high difficulty (bits = 0x170b8c8b) */
  /* Target = 0x0b8c8b * 256^(0x17-3) = 0x0b8c8b at byte position 20
   * (little-endian) */
  test_bits_to_target(
      "Very high difficulty (0x170b8c8b)", 0x170b8c8b,
      "0000000000000000000b8c8b0000000000000000000000000000000000000000");


  test_section("Target to bits tests");
  test_target_to_bits(
      "Genesis target",
      "00000000ffff0000000000000000000000000000000000000000000000000000",
      0x1d00ffff);


  /*
   * Proof-of-work validation tests
   */
  test_section("Proof-of-work tests");

  /* Genesis block hash should meet genesis target */
  test_pow_check(
      "Genesis block meets target",
      "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f",
      "00000000ffff0000000000000000000000000000000000000000000000000000",
      ECHO_TRUE);

  /* A hash that doesn't meet target */
  test_pow_check(
      "Invalid PoW (hash too high)",
      "000000010000000000000000000000000000000000000000000000000000000a",
      "00000000ffff0000000000000000000000000000000000000000000000000000",
      ECHO_FALSE);


  /*
   * Full block parsing test
   * Genesis block: header + 1 coinbase transaction
   */
  test_section("Full block parsing tests");

  /* Genesis block (header + varint(1) + coinbase tx) */
  /* Note: The genesis block coinbase has a 65-byte pubkey */
  const char *genesis_block_hex =
      /* Header (80 bytes) */
      "010000000000000000000000000000000000000000000000000000000000000000000000"
      "3ba3edfd"
      "7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a29ab5f49ffff001d"
      "1dac2b7c"
      /* Transaction count: 1 */
      "01"
      /* Coinbase transaction (version=1, 1 input, 1 output, locktime=0) */
      "01000000" /* version */
      "01"       /* input count */
      "0000000000000000000000000000000000000000000000000000000000000000" /* prev
                                                                            txid
                                                                          */
      "ffffffff" /* prev vout (coinbase marker) */
      "4d"       /* scriptSig length = 77 bytes */
      "04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e6365"
      "6c6c6f72"
      "206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b"
      "73"
      "ffffffff"         /* sequence */
      "01"               /* output count */
      "00f2052a01000000" /* value: 50 BTC = 5000000000 satoshis */
      "43"               /* scriptPubKey length = 67 bytes */
      "4104678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6"
      "bc3f4cef"
      "38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5fac"
      "00000000"; /* locktime */

  test_block_parse("Genesis block parse", genesis_block_hex, 1);


  /*
   * Error handling tests
   */
  test_section("Error handling tests");

  /* Truncated header */
  {
    uint8_t truncated[40];
    block_header_t header;
    echo_result_t result;

    memset(truncated, 0, sizeof(truncated));
    result = block_header_parse(truncated, sizeof(truncated), &header);

    test_case("Truncated header rejected");
    if (result == ECHO_ERR_TRUNCATED) {
      test_pass();
    } else {
      test_fail("Truncated header not rejected");
      printf("    Result: %d\n", result);
    }
  }

  /* NULL parameters */
  {
    block_header_t header;
    echo_result_t result;

    result = block_header_parse(NULL, 80, &header);

    test_case("NULL data rejected");
    if (result == ECHO_ERR_NULL_PARAM) {
      test_pass();
    } else {
      test_fail("NULL data not rejected");
      printf("    Result: %d\n", result);
    }
  }

  test_suite_end();
  return test_global_summary();
}
