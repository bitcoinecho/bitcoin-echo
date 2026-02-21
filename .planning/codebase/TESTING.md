# Testing Patterns

**Analysis Date:** 2026-02-20

## Test Framework

**Runner:**
- Custom shell script: `test/run_all_tests.sh`
- Invokes individual test binaries compiled from unit test files
- Each test binary runs one test suite

**Assertion Library:**
- Custom lightweight framework in `test/unit/test_utils.h` and `test/unit/test_utils.c`
- No external dependencies (no CUnit, no Google Test)

**Run Commands:**
```bash
make test              # Compile all tests and run via test/run_all_tests.sh
make clean             # Remove all test binaries and object files
./test/unit/test_sha256  # Run individual test (after make test)
```

**Build Integration:**
- Makefile has individual test targets: `$(TEST_SHA256)`, `$(TEST_MERKLE)`, etc.
- Each test links required source modules (not full binary)
- Test utility object file: `test/unit/test_utils.o` linked into every test

## Test File Organization

**Location:**
- All tests in `test/unit/` directory
- Co-located with source (tests are separate, not embedded)

**Naming:**
- Pattern: `test_<module>.c` where module matches source file
- Examples: `test_sha256.c`, `test_consensus.c`, `test_download_mgr.c`
- Compiled to executables: `test/unit/test_sha256`, `test/unit/test_consensus`

**Structure:**
```
test/unit/
├── test_sha256.c
├── test_ripemd160.c
├── test_serialize.c
├── test_tx.c
├── test_consensus.c
├── ...
├── test_utils.h       # Test framework header
├── test_utils.c       # Test framework implementation
└── run_all_tests.sh   # Master test runner script
```

## Test Structure

**Suite Organization:**
Test files follow this pattern:
```c
/*
 * File header with test purpose
 */

#include <stdio.h>
#include <stdint.h>
#include "test_utils.h"
#include "module_to_test.h"

/*
 * Helper functions (optional, static)
 */
static void run_sha256_test(const char *name,
                            const uint8_t *input, size_t input_len,
                            const uint8_t expected[32])
{
    uint8_t result[32];
    sha256(input, input_len, result);

    test_case(name);
    if (bytes_equal(result, expected, 32)) {
        test_pass();
    } else {
        test_fail_bytes("hash mismatch", expected, result, 32);
    }
}

/*
 * Individual test functions (void, no args)
 */
static void test_empty_string(void) {
    const uint8_t input[] = "";
    const uint8_t expected[32] = { /* ... */ };
    run_sha256_test("Empty string", input, 0, expected);
}

static void test_abc(void) {
    const uint8_t input[] = "abc";
    const uint8_t expected[32] = { /* ... */ };
    run_sha256_test("abc", input, 3, expected);
}

/*
 * Main entry point
 */
int main(void)
{
    test_suite_begin("SHA-256 Test Suite");

    test_empty_string();
    test_abc();
    test_ripemd160_compat();
    /* ... more tests ... */

    test_suite_end();

    return test_global_summary();
}
```

**Patterns:**
- No setup/teardown per test (tests are independent)
- Global state minimal to support summary reporting
- Test cases are static functions taking no arguments
- Assertion-style validation via test_case() → test_pass() or test_fail_*()
- Suite lifecycle: `test_suite_begin()` at start, `test_suite_end()` at end, `test_global_summary()` for exit code

## Test Utilities API

**Core Functions:**

Suite management:
- `void test_suite_begin(const char *suite_name)` - Begin test suite, print header
- `void test_suite_end(void)` - End suite, print results for this suite
- `int test_global_summary(void)` - Print global summary, return exit code (0=success, 1=failure)

Per-test:
- `void test_case(const char *test_name)` - Begin test case
- `void test_pass(void)` - Mark current test as passed (prints green [PASS])
- `void test_section(const char *section_name)` - Print optional section header

Failure reporting (one of these per test):
- `void test_fail(const char *message)` - Simple failure message
- `void test_fail_int(const char *message, long expected, long actual)` - Integer mismatch
- `void test_fail_uint(const char *message, unsigned long expected, unsigned long actual)` - Unsigned integer mismatch
- `void test_fail_str(const char *message, const char *expected, const char *actual)` - String mismatch
- `void test_fail_bytes(const char *message, const uint8_t *expected, const uint8_t *actual, size_t len)` - Hex dump comparison

Helpers:
- `void print_hex(const uint8_t *data, size_t len)` - Print bytes as hex (for debug output)
- `int bytes_equal(const uint8_t *a, const uint8_t *b, size_t len)` - Compare byte arrays

**Output Format:**
```
SHA-256 Test Suite
===================

  [PASS] Empty string
  [PASS] NIST abc
  [PASS] NIST abc (streaming)
  [FAIL] Invalid test (example)
    hash mismatch
    expected: ba7816...
    got:      deadbe...

Results: 3/4 tests passed (1 failed)
```

Global summary output (from `test/run_all_tests.sh`):
```
================================================================================
                     BITCOIN ECHO — GLOBAL TEST SUMMARY
================================================================================

Test Suite Results:
  ✓ SHA-256 tests                              100/100 passed
  ✓ Transaction tests                           50/50 passed
  ✗ Consensus tests                             45/50 passed

Summary:
  Test Suites: 35/36 passed, 1 failed
  Test Cases:  1245/1250 passed, 5 failed

                    5 TEST(S) FAILED
================================================================================
```

## Mocking

**Framework:** No external mocking library used
- Tests use direct function calls (functions under test are not mocked)
- Database tests create real SQLite databases in memory or temp files
- Network tests mock message passing with manual buffer manipulation

**Patterns:**

Simple module testing - Direct invocation:
```c
/* test_serialize.c */
static void test_varint_read(void) {
    uint8_t buf[] = {0xFD, 0xFD, 0x00};  // Encoded value 253
    uint64_t value;
    size_t consumed;

    test_case("varint read 0xFD");
    echo_result_t result = varint_read(buf, 3, &value, &consumed);

    if (result == ECHO_OK && value == 253 && consumed == 3) {
        test_pass();
    } else {
        test_fail("varint parse failed");
    }
}
```

Database testing - Real in-memory databases:
```c
/* test_db.c */
static void test_db_insert_query(void) {
    sqlite3 *db;
    int rc = sqlite3_open(":memory:", &db);

    test_case("database insert and retrieve");
    if (rc == SQLITE_OK) {
        /* Set up schema and test operations */
        test_pass();
        sqlite3_close(db);
    } else {
        test_fail("database creation failed");
    }
}
```

Consensus engine testing - Create real engine instances:
```c
/* test_consensus.c */
static void engine_create_destroy(void) {
    consensus_engine_t *engine = consensus_engine_create();

    test_case("engine creation");
    if (engine != NULL) {
        test_pass();
        consensus_engine_destroy(engine);
    } else {
        test_fail("engine create returned NULL");
    }
}
```

**What to Mock:**
- Nothing in standard approach; test_utils.h doesn't support mocking
- When testing networking code, manually construct protocol messages as byte buffers
- When testing storage, use real SQLite databases (either `:memory:` or temp files)

**What NOT to Mock:**
- Core algorithms (hash functions, signature verification, script execution)
- Public API functions under test
- Memory allocation patterns (test real allocation/deallocation)

## Fixtures and Factories

**Test Data:**
Fixtures are typically constants defined in test file:
```c
/* test_script.c */
static void run_sha256_test(const char *name,
                            const uint8_t *input, size_t input_len,
                            const uint8_t expected[32])
{
    uint8_t result[32];
    sha256(input, input_len, result);

    test_case(name);
    if (bytes_equal(result, expected, 32)) {
        test_pass();
    } else {
        test_fail_bytes("hash mismatch", expected, result, 32);
    }
}
```

Test vectors (from NIST, Bitcoin, etc.):
```c
{
    const uint8_t input[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const uint8_t expected[32] = {
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
        0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
        0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
        0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1
    };
    run_sha256_test("NIST 448 bits", input, 56, expected);
}
```

**Location:**
- Fixtures inline in test file (no separate data files)
- Test vectors as static const arrays in the test function

**Pattern for complex fixtures:**
```c
/* test_tx.c */
static const uint8_t COINBASE_TX[] = {
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* ... more bytes ... */
};
static const size_t COINBASE_TX_LEN = sizeof(COINBASE_TX);
```

## Coverage

**Requirements:** None enforced (not configured in Makefile)

**View Coverage:** Not applicable - no coverage tooling integrated

**Test execution models:**
- Unit tests exercise individual modules in isolation
- Integration tests (`test_integration.c`) verify layer interactions
- Script vector tests (`test_script_vectors.c`) run against Bitcoin Core vectors
- Consensus tests (`test_consensus.c`) validate engine behavior end-to-end

## Test Types

**Unit Tests:**
- Scope: Single function or module
- Approach: Direct function invocation with test vectors
- Examples: `test_sha256.c`, `test_serialize.c`, `test_opcodes.c`
- Typically 20-100 test cases per file
- No I/O (except database tests use real SQLite)

**Integration Tests:**
- Scope: Multi-module workflows
- Approach: Create consensus engine, validate blocks, check chain state
- File: `test_integration.c`
- Tests node lifecycle, block syncing, mempool behavior
- Verifies layers work together correctly

**Script Execution Tests:**
- Scope: Bitcoin script evaluation
- Approach: Load test vectors, execute scripts, validate stack state
- File: `test_script_vectors.c`
- Uses vectors from Bitcoin Core regression test data
- Tests all opcodes, combinations, edge cases

**Database Tests:**
- Scope: Storage layer (SQLite integration)
- Approach: Real in-memory SQLite databases
- Files: `test_db.c`, `test_utxo_db.c`, `test_block_index_db.c`
- Verify CRUD operations, indexing, queries

**E2E Tests:**
- Not explicitly separate; integration tests serve this purpose
- Would require multiple nodes (not currently supported)
- Node startup/shutdown tested in `test_node.c`

## Common Patterns

**Streaming Test Pattern:**
Test that streaming API produces same result as one-shot:
```c
static void run_streaming_test(const char *name,
                               const uint8_t *input, size_t input_len)
{
    uint8_t oneshot[32];
    uint8_t streaming[32];
    sha256_ctx_t ctx;
    size_t i;

    /* One-shot */
    sha256(input, input_len, oneshot);

    /* Streaming: feed one byte at a time */
    sha256_init(&ctx);
    for (i = 0; i < input_len; i++) {
        sha256_update(&ctx, input + i, 1);
    }
    sha256_final(&ctx, streaming);

    test_case(name);
    if (bytes_equal(oneshot, streaming, 32)) {
        test_pass();
    } else {
        test_fail_bytes("streaming mismatch", oneshot, streaming, 32);
    }
}
```

**Error Case Testing:**
```c
static void test_varint_truncated(void) {
    uint8_t buf[] = {0xFD};  // Incomplete multi-byte varint
    uint64_t value = 0;

    test_case("varint truncated");
    echo_result_t result = varint_read(buf, 1, &value, NULL);

    if (result == ECHO_ERR_TRUNCATED) {
        test_pass();
    } else {
        test_fail_int("expected ECHO_ERR_TRUNCATED", ECHO_ERR_TRUNCATED, result);
    }
}
```

**Roundtrip Testing (serialize → deserialize):**
```c
static void test_tx_roundtrip(void) {
    tx_t original, deserialized;
    uint8_t buf[1024];
    size_t written, consumed;

    /* Create and serialize */
    tx_init(&original);
    /* ... populate original ... */
    echo_result_t result = tx_serialize(&original, buf, sizeof(buf), &written);

    /* Deserialize */
    test_case("transaction roundtrip");
    result = tx_parse(buf, written, &deserialized, &consumed);

    if (result == ECHO_OK && consumed == written && tx_equal(&original, &deserialized)) {
        test_pass();
    } else {
        test_fail("roundtrip produced different transaction");
    }

    tx_free(&original);
    tx_free(&deserialized);
}
```

**Bounds Checking Tests:**
```c
static void test_buffer_too_small(void) {
    uint8_t buf[2];  // Too small for large varint
    uint64_t value = 0x100000000;  // Requires 9 bytes

    test_case("varint buffer too small");
    echo_result_t result = varint_write(buf, sizeof(buf), value, NULL);

    if (result == ECHO_ERR_BUFFER_TOO_SMALL) {
        test_pass();
    } else {
        test_fail_int("expected ECHO_ERR_BUFFER_TOO_SMALL", ECHO_ERR_BUFFER_TOO_SMALL, result);
    }
}
```

**Consensus Validation Pattern:**
```c
static void test_block_valid(void) {
    consensus_engine_t *engine = consensus_engine_create();
    block_t block;
    /* ... construct valid block ... */

    test_case("valid block accepted");
    echo_result_t result = consensus_validate_block(engine, &block);

    if (result == ECHO_OK) {
        test_pass();
    } else {
        test_fail_int("validation failed", ECHO_OK, result);
    }

    consensus_engine_destroy(engine);
}
```

## Test Coverage

**Files with comprehensive tests:**
- `src/crypto/sha256.c` - Full FIPS 180-4 vector coverage
- `src/consensus/serialize.c` - All varint encoding paths
- `src/consensus/script.c` - All opcodes and combinations
- `src/consensus/tx.c` - Transaction parsing and validation
- `src/consensus/block.c` - Block structure and merkle validation

**High-value integration tests:**
- Full node lifecycle in `test_node.c`
- Block sync and chain selection in `test_sync.c`
- Mempool transaction validation in `test_mempool.c`
- RPC interface in `test_rpc.c`

**Areas with lighter coverage:**
- Network protocol specifics (message serialization tested, but not network I/O)
- Peer discovery (deterministic in tests, not real DNS)
- Download manager with actual network timeouts (simulated in tests)

---

*Testing analysis: 2026-02-20*
