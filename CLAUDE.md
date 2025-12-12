# Bitcoin Echo — C Implementation Context

## Critical Constraints

**You must follow these constraints in all implementation work:**

1. **Pure C11** — No C++ features, no extensions beyond rotation intrinsics
2. **Zero external dependencies** — Only C compiler and standard library (+ SQLite, which is embedded)
3. **Consensus engine purity** — No I/O, no system calls, no dynamic allocation during validation
4. **Simplicity over optimization** — Correct and clear beats fast and clever
5. **15,000–25,000 lines target** — Every line must justify its existence
6. **Heavy commenting** — Code must be understandable by auditor in 2125
7. **No future features** — Implement what exists in Bitcoin today, nothing speculative

## Code Style

- Functions are obvious in purpose
- Every branch justifiable by protocol specification
- No clever tricks
- Bounds checking on all buffers
- Constants over magic numbers
- Descriptive names over abbreviations

**Include Hygiene (Critical for Ossification):**
- Every symbol used must have its header explicitly included
- No transitive include dependencies (don't rely on A→B→C chains)
- Headers must be self-contained and independently compilable
- Remove unused includes - they mislead future maintainers
- If you use `uint32_t`, explicitly include `<stdint.h>` even if another header does

## Code Quality Enforcement (Added Phase 8)

**As of Phase 8, all code must pass clangd + clang-tidy analysis before commit:**

### Active Tooling
- **clangd LSP server** configured via `.clangd` in project root
- **Format on save** enabled in VSCode
- **clang-tidy checks** enforce cert-*, bugprone-*, misc-*, portability-*, readability-* rules
- **Include-cleaner** validates explicit include hygiene (see MissingIncludes/UnusedIncludes diagnostics)

### Writing New Code
1. Write code and save file - auto-formatting applies immediately
2. Address ALL clangd warnings shown in Problems panel
3. Fix include issues: add missing headers, remove unused ones
4. Only use NOLINT suppressions when absolutely necessary with explanatory comment

### NOLINT Usage Guidelines
Use NOLINT suppressions **sparingly** and **only** with clear justification:

```c
// NOLINTBEGIN(cert-err34-c) - sscanf is correct here: we check return value
// and need exactly 2 hex chars, not all available hex like strtoul would read
if (sscanf(hex + i * 2, "%02x", &byte) != 1)
  return 0;
// NOLINTEND(cert-err34-c)
```

**Valid reasons for NOLINT:**
- Platform-specific false positives (see `posix.c` in `.clangd` config)
- Algorithm requires specific pattern clang-tidy flags incorrectly
- Bitcoin protocol quirk that appears wrong but is intentional

**Invalid reasons:**
- "It's too hard to fix properly"
- "The warning is annoying"
- "I don't understand the warning"

### Compliance Status
- **`include/` and `src/` folders**: ✅ Fully compliant - use as reference examples
- **`test/` folder**: ⚠️  Partially compliant - being incrementally updated
  - `test/unit/test_block.c` - proper NOLINT usage for sscanf
  - `test/unit/test_block_validate.c` - include hygiene example

**Key reference files:**
- `src/platform/posix.c` - platform-specific suppression via .clangd config
- `.clangd` - project-wide configuration with documented exceptions

## Context Loading Strategy

To avoid context window limits and ensure focused implementation:
1. **DO NOT read full source files (.c)** for completed modules unless you are modifying them.
2. **Read `include/*.h`** to understand available APIs and types.
3. **Read ONLY the specific `.c` file** you are currently working on.
4. Treat other modules as **black boxes** based on their headers.

## Architecture Layers

```
Application Layer     — Node operation, RPC, logging
Protocol Layer        — P2P messages, peers, mempool
Consensus Engine      — FROZEN CORE (block/tx validation, chain selection)
Platform Abstraction  — OS interface (sockets, threads, files, time, entropy)
```

Information flows down as function calls, up as return values. Lower layers know nothing of higher layers.

## Directory Structure

```
bitcoin-echo/
├── src/
│   ├── platform/     ← OS abstraction (POSIX, Windows)
│   ├── crypto/       ← SHA-256, RIPEMD-160, secp256k1
│   ├── consensus/    ← FROZEN CORE - block/tx validation
│   ├── protocol/     ← P2P networking, message handling
│   └── app/          ← Node orchestration, RPC, logging
├── include/          ← Public headers
├── test/
│   ├── vectors/      ← Bitcoin Core test vectors
│   └── unit/         ← Unit tests
├── docs/             ← Implementation notes
├── Makefile          ← POSIX build
└── build.bat         ← Windows build
```

## What NOT To Do

- Don't add features beyond Bitcoin protocol as it exists today
- Don't optimize at the expense of clarity
- Don't use external libraries (embed everything)
- Don't make the consensus engine touch I/O
- Don't create abstractions for one-time operations
- Don't add configuration options or runtime flags
- Don't write wallet functionality

## Quick Reference

**Signature verification seam:** `sig_verify.h` / `sig_verify.c` — the quantum succession boundary

**Platform interface:** See whitepaper Appendix A for complete API

**Supported soft forks:** P2SH, BIP-66, CLTV, CSV, SegWit, Taproot

**Test vectors:** Embed Bitcoin Core's consensus test suite; 100% pass required

## Session Completion Workflow

**After completing each session and all tests pass:**

1. **Update roadmap** — Mark session complete in `bitcoinecho-org/ROADMAP.md` with status and test count
3. **Commit bitcoin-echo** — Commit implementation changes with descriptive message
4. **Commit bitcoinecho-org** — Commit roadmap update
5. **Push both repos** — Push changes to GitHub
6. **Pause and check in** — Wait for user confirmation before starting next session

## When Uncertain

Consult in order:
1. The whitepaper (authoritative specification)
2. Bitcoin Core source (reference implementation)
3. BIPs (protocol documentation)

When the whitepaper is silent, choose simplicity.
