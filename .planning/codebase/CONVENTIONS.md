# Coding Conventions

**Analysis Date:** 2026-02-20

## Naming Patterns

**Files:**
- Snake case with descriptive names: `sha256.c`, `block_validate.c`, `download_mgr.c`
- Header files always have corresponding `.h` in `include/` directory
- Include guards use ALL_CAPS with module prefix: `ECHO_TYPES_H`, `ECHO_SERIALIZE_H`
- Test files follow pattern: `test/unit/test_<module>.c` (e.g., `test_sha256.c`)

**Functions:**
- Snake case, lowercase: `sha256()`, `tx_init()`, `varint_write()`
- Module prefixes optional but common: `consensus_engine_create()`, `script_classify()`
- Private/static functions use same naming as public (no leading underscore convention)
- Init/free patterns: `<type>_init()` and `<type>_free()` for lifecycle (e.g., `tx_init()`, `tx_free()`)

**Variables:**
- Snake case, lowercase: `buffer_len`, `input_count`, `block_height`
- Type suffixes rare; clarity comes from context and comments
- Global variables prefixed with `g_`: `g_log_file`, `g_node`, `g_enabled_components`
- Static variables with file scope also use `g_` prefix
- Local variable names kept short but meaningful: `i`, `j` for loop indices, `result` for return values

**Types:**
- Typedef structs and enums with `_t` suffix: `hash256_t`, `tx_t`, `echo_result_t`
- Enum values ALL_CAPS: `ECHO_OK`, `ECHO_ERR_NULL_PARAM`, `TX_VALIDATE_OK`
- Constants ALL_CAPS: `VARINT_PREFIX_16`, `CONSENSUS_BIP16_HEIGHT`, `ECHO_MAX_SATOSHIS`
- Boolean type custom: `echo_bool_t` with constants `ECHO_TRUE` (1), `ECHO_FALSE` (0)

## Code Style

**Formatting:**
- C11 standard strictly enforced via clang-tidy
- Line length: practical limit around 100 chars (no hard limit stated)
- Indentation: 2 spaces (inferred from code samples, not explicitly stated)
- Brace style: Allman (opening brace on new line for functions, same line for control structures)
- Example from `sha256.c`:
  ```c
  static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h;
    int t;

    for (t = 0; t < 16; t++) {
      W[t] = load_be32(block + (size_t)(t * 4));
    }
  }
  ```

**Linting:**
- Tool: clang-tidy (configured in `.clangd`)
- Enabled checks:
  - `bugprone-*` - Likely bugs and anti-patterns
  - `cert-*` - CERT secure coding standards
  - `clang-analyzer-*` - Static analysis
  - `misc-*` - Miscellaneous checks
  - `portability-*` - Platform independence
  - `readability-identifier-naming` - Consistent naming
  - `readability-simplify-boolean-expr` - Boolean logic clarity
- Disabled checks:
  - `bugprone-easily-swappable-parameters` - False positives common
  - `readability-magic-numbers` - Protocol constants are justified
  - `cert-err33-c` - Too noisy for errno handling
- Compiler flags enforce strict checking (`-Werror=implicit-function-declaration`, `-Wcast-qual`, `-Wshadow`)

## Import Organization

**Order:**
1. Local project headers from `include/`: `#include "sha256.h"`
2. Standard library headers: `#include <stdint.h>`, `#include <string.h>`
3. Platform-specific headers: `#include <time.h>`, `#include <pthread.h>`

**Path Aliases:**
- Not used; imports are always relative to `-Iinclude` compiler flag
- All project headers accessed as `#include "header.h"` (no paths)

**Header Guard Pattern:**
All headers use `#ifndef HEADER_H` / `#define HEADER_H` / `#endif` guards:
```c
#ifndef ECHO_TYPES_H
#define ECHO_TYPES_H
/* content */
#endif /* ECHO_TYPES_H */
```

## Error Handling

**Patterns:**
- Unified return type: `echo_result_t` enum defined in `include/echo_types.h`
- Success: `ECHO_OK` (value 0)
- Sentinel value: `ECHO_DONE` (value 1) for "no more items" iteration
- Errors: Negative enums like `ECHO_ERR_NULL_PARAM` (-1), `ECHO_ERR_TRUNCATED` (-11)
- Domain-specific errors: `ECHO_ERR_TX_*`, `ECHO_ERR_BLOCK_*`, `ECHO_ERR_SCRIPT_*`, `ECHO_ERR_CONSENSUS_*`

**Validation pattern:**
```c
echo_result_t varint_read(const uint8_t *buf, size_t buf_len, uint64_t *value,
                          size_t *consumed) {
  if (buf == NULL || value == NULL) {
    return ECHO_ERR_NULL_PARAM;
  }
  if (buf_len < 1) {
    return ECHO_ERR_TRUNCATED;
  }
  /* ... implementation ... */
  return ECHO_OK;
}
```

**Out parameters:**
- Functions use output parameters (pointers) for results when needed
- Example: `varint_read()` returns error code, sets `*value` and `*consumed` on success
- Optional output parameters can be NULL when caller doesn't need result
- All parameters checked for NULL before use when required

**Memory management:**
- Manual allocation/deallocation with `malloc()`/`free()`
- Init/free pattern for complex types: `tx_init()` initializes, `tx_free()` cleans up
- No automatic cleanup; caller responsible for `free()`
- Example from `tx.c`:
  ```c
  void tx_free(tx_t *tx) {
    if (tx == NULL) return;
    if (tx->inputs != NULL) {
      for (size_t i = 0; i < tx->input_count; i++) {
        free(tx->inputs[i].script_sig);
        witness_stack_free(&tx->inputs[i].witness);
      }
      free(tx->inputs);
    }
    /* ... clean other fields ... */
  }
  ```

## Logging

**Framework:** Custom implementation in `src/app/log.c` (not external library)

**Patterns:**
- Signature: `log_<level>()` functions (e.g., `log_error()`, `log_warn()`, `log_info()`, `log_debug()`)
- Takes component, format string, and variadic args
- Example from codebase: `log_error(LOG_COMP_SYNC, "Failed to sync block %u", height);`
- Components are bitfield flags: `LOG_COMP_MAIN`, `LOG_COMP_NET`, `LOG_COMP_CONS`, etc.
- All logging goes through mutex for thread safety
- Output format: `YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [COMP] message`

**When to log:**
- Errors and validation failures: `log_error()`
- Important state changes: `log_info()`
- Diagnostic details: `log_debug()`
- Warnings: `log_warn()`
- No logging in consensus engine (pure computation only)

## Comments

**When to Comment:**
- File header comments (always, at top of every `.c` and `.h` file)
- Function header comments (always, before function definition)
- Complex algorithm explanations (always reference specification or BIP)
- Non-obvious logic branches (e.g., canonical encoding checks)
- Why, not what: code shows what, comments explain why

**JSDoc/DocBlock Style:**
Uses C-style block comments with structured header:
```c
/**
 * Brief description (one line)
 *
 * Longer description if needed.
 *
 * Parameters:
 *   param1 - Description
 *   param2 - Description
 *
 * Returns:
 *   Description of return value(s)
 */
```

**Specification References:**
Comments reference Bitcoin sources directly:
- FIPS standards: "FIPS 180-4 (Secure Hash Standard)"
- BIPs: "BIP-16 (P2SH)", "BIP-141/143 (SegWit)"
- Bitcoin Core: "Bitcoin Core reference implementation"

**File Header Example:**
```c
/*
 * Bitcoin Echo — SHA-256 Implementation
 *
 * SHA-256 as specified in FIPS 180-4 (Secure Hash Standard).
 *
 * Reference: https://csrc.nist.gov/publications/detail/fips/180/4/final
 *
 * This implementation prioritizes correctness and clarity.
 * Every operation maps directly to the specification.
 *
 * Build once. Build right. Stop.
 */
```

## Function Design

**Size:** Functions kept focused and small (typically under 50 lines)
- Helper functions extracted for readability
- Complex algorithms broken into logical steps with comments
- Example: SHA-256 transform uses separate functions for message scheduling, initialization, rounds

**Parameters:**
- Input parameters come first, output parameters last
- Const pointers for inputs: `const uint8_t *buf`
- Mutable pointers for outputs: `uint64_t *value`, `size_t *consumed`
- Context/opaque types as first parameter when applicable
- Buffer+length pairs always together: `const uint8_t *buf, size_t buf_len`
- Maximum ~6 parameters; more indicates design issue

**Return Values:**
- Always return error code (`echo_result_t`) for functions that can fail
- Use output parameters for multiple results
- Never return allocated memory directly; use init/output parameter pattern
- Void return only for truly void operations (e.g., `tx_free()`)

## Module Design

**Exports:**
- Public API in `include/<module>.h` header file
- All exported functions declared in header with full documentation
- Implementation in `src/<layer>/<module>.c`
- No undeclared external symbols

**Organization by layer:**
- `src/crypto/` - Cryptographic primitives (SHA-256, RIPEMD160, secp256k1 interface)
- `src/consensus/` - Bitcoin validation rules (transactions, blocks, scripts, UTXO)
- `src/storage/` - Persistent storage (databases, block files)
- `src/protocol/` - Network protocol implementation (P2P messages, sync, relay)
- `src/app/` - Application logic (node lifecycle, RPC, logging, mining)
- `src/node/` - Chase event system for block validation coordination
- `src/platform/` - Platform abstraction (POSIX/threading/time)

**Barrel Files:**
- Not used; explicit imports required
- Encourages knowing module dependencies

## Type System

**Fixed-width types:**
- Always use `stdint.h` types for protocol data: `uint8_t`, `uint32_t`, `uint64_t`, `int64_t`
- Custom type wrapping for semantic clarity:
  - `hash256_t` for 256-bit hashes
  - `hash160_t` for 160-bit hashes
  - `satoshi_t` (int64_t) for amounts (signed to detect underflow)

**Struct initialization:**
- Explicit init functions preferred: `tx_init(tx_t *tx)`
- Memset-based initialization for bulk data: `memset(&tx, 0, sizeof(tx))`
- Compound literals for constants: `((hash256_t){{0}})` for null hash

## Bounds Checking

**Buffer safety:**
- Every buffer read checks length before access
- Example from `varint_read()`: Check `buf_len` before reading each byte
- Sizes passed explicitly as parameters: `const uint8_t *buf, size_t buf_len`
- No reliance on null-terminated strings for binary data

**Array access:**
- Loop bounds always explicit: `for (i = 0; i < count; i++)`
- Cast indices to prevent overflow: `(size_t)(t * 4)`

---

*Convention analysis: 2026-02-20*
