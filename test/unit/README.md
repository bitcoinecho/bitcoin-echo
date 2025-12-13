# Bitcoin Echo — Test Suite

## Overview

Bitcoin Echo uses a unified test framework (`test_utils.h` / `test_utils.c`) that provides:

- **Color-coded output**: Green `[PASS]`, Red `[FAIL]`
- **Consistent formatting**: Uniform indentation and test case presentation
- **Detailed failure reporting**: Shows expected vs actual with hex dumps
- **Global summary**: Accumulates totals across all test suites
- **Suite tracking**: Reports both per-suite and global statistics

## Test Coverage

All unit tests are organized by architectural layer:

### Crypto Layer
Cryptographic primitives:
- SHA-256, RIPEMD-160
- secp256k1 field arithmetic and group operations
- ECDSA and Schnorr signature verification

### Consensus Layer
Bitcoin consensus rules:
- Serialization, transactions, blocks, merkle trees
- Script engine, stack operations, opcodes
- P2SH, timelocks (CLTV/CSV)
- Transaction validation, block validation, coinbase validation

### State Management
UTXO and chain state:
- UTXO set operations
- Chain state management
- Consensus engine integration

### Storage Layer
Persistent storage:
- Block storage and retrieval
- Database integration (SQLite)
- UTXO database, block index database

### Protocol Layer
P2P networking:
- Protocol messages and serialization
- Peer management and discovery
- Block relay, synchronization, mempool
- Event loop

### Application Layer
Node operations:
- Node lifecycle management
- RPC interface
- Logging system

### Special Cases
- `test_script_vectors.c` - JSON test vector harness (uses different architecture)

## Running Tests

```bash
# Run all tests with colored output and summary
make test

# Run individual test suite
./test/unit/test_sha256

# Run global test runner directly
./test/run_all_tests.sh
```

Expected output format:
```
================================================================================
                     BITCOIN ECHO — GLOBAL TEST SUMMARY
================================================================================

Test Suite Results:
  ✓ SHA-256 tests                              9/  9 passed
  ✓ RIPEMD-160 tests                          17/ 17 passed
  ...

Summary:
  Test Suites: X/X passed
  Test Cases:  Y/Y passed

                    ALL TESTS PASSED!
================================================================================
```

## Continuous Integration

Tests run automatically on GitHub Actions for:
- **macOS** (latest)
- **Ubuntu** (latest)

CI workflow: [`.github/workflows/test.yml`](../../.github/workflows/test.yml)

Both platforms must pass before merge.

## Writing Tests

### Structure

All test files follow this pattern:

1. **Add includes:**
   ```c
   #include <stdint.h>
   #include <stdio.h>
   #include "test_utils.h"
   ```

2. **Structure main() function:**
   ```c
   int main(void)
   {
       test_suite_begin("Your Test Suite Name");

       // ... your tests ...

       test_suite_end();
       return test_global_summary();
   }
   ```

3. **Write test functions:**
   ```c
   static void run_something_test(const char *name, ...)
   {
       // ... perform test ...

       test_case(name);
       if (condition) {
           test_pass();
       } else {
           test_fail_bytes("mismatch description", expected, actual, len);
       }
   }
   ```

4. **Use section headers (optional):**
   ```c
   test_section("Advanced Features");
   ```

### API Reference

#### Suite Management

```c
void test_suite_begin(const char *suite_name);
void test_suite_end(void);
int test_global_summary(void);  // Returns 0 on success, 1 on failure
```

#### Test Case Management

```c
void test_case(const char *test_name);
void test_pass(void);
```

#### Failure Reporting

```c
void test_fail(const char *message);
void test_fail_int(const char *message, long expected, long actual);
void test_fail_uint(const char *message, unsigned long expected, unsigned long actual);
void test_fail_str(const char *message, const char *expected, const char *actual);
void test_fail_bytes(const char *message, const uint8_t *expected, const uint8_t *actual, size_t len);
```

#### Visual Organization

```c
void test_section(const char *section_name);  // Prints cyan section header
```

### Examples

See any test file in this directory for complete working examples:
- [`test_sha256.c`](test_sha256.c) - Simple hash function tests
- [`test_script.c`](test_script.c) - Complex script execution tests
- [`test_consensus.c`](test_consensus.c) - Engine integration tests

### Makefile Integration

All test rules in the Makefile link against `$(TEST_UTILS_OBJ)` automatically.

## Philosophy

The test framework embodies Bitcoin Echo's core principles:

- **Build once, build right:** Tests are the proof
- **Simplicity:** No macros, no magic, just clean function calls
- **Quality:** Professional output that makes a statement about code quality
- **Ossification:** Once the framework is complete, it freezes with the codebase

---

*Build once. Build right. Stop.*
