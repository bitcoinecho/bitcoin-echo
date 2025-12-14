# Bitcoin Echo

[![Version](https://img.shields.io/badge/version-0.1-orange.svg)](https://github.com/bitcoinecho/bitcoin-echo/releases)
[![Tests](https://github.com/bitcoinecho/bitcoin-echo/actions/workflows/test.yml/badge.svg)](https://github.com/bitcoinecho/bitcoin-echo/actions/workflows/test.yml)
[![C Standard](https://img.shields.io/badge/C-C11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Dependencies](https://img.shields.io/badge/dependencies-none-success.svg)]()

> **⚠️ Development Status**: Bitcoin Echo is currently under active development (v0.1). The codebase is not yet feature-complete and should not be used in production. See [Status](#status) below for implementation progress.

A complete, ossified implementation of the Bitcoin protocol in pure C.

*Build once. Build right. Stop.*

## Status

**Phase 9: Application Layer** — Complete

| Component | Status |
|-----------|--------|
| **Cryptography** | |
| SHA-256 | Complete |
| RIPEMD-160 | Complete |
| secp256k1 (Field/Group) | Complete |
| ECDSA Verification | Complete |
| Schnorr (BIP-340) | Complete |
| **Data Structures** | |
| Serialization | Complete |
| Transactions | Complete |
| Blocks | Complete |
| Merkle Trees | Complete |
| **Script Interpreter** | |
| Stack Operations | Complete |
| Arithmetic/Logic Opcodes | Complete |
| Crypto Opcodes | Complete |
| Flow Control | Complete |
| P2SH Support | Complete |
| Timelocks (BIP-65/68/112) | Complete |
| Signature Verification | Complete |
| **Transaction Validation** | |
| Syntactic Validation | Complete |
| Script Execution | Complete |
| UTXO Context | Complete |
| **Block Validation** | |
| Header Validation (PoW, MTP) | Complete |
| Difficulty Adjustment | Complete |
| Coinbase Validation | Complete |
| Full Block Validation | Complete |
| **Chain Selection** | |
| UTXO Set | Complete |
| Chain State | Complete |
| Chain Selection Algorithm | Complete |
| Consensus Engine Integration | Complete |
| **Storage Layer** | |
| Block File Storage | Complete |
| SQLite Integration | Complete |
| UTXO Database | Complete |
| Block Index Database | Complete |
| **Protocol Layer** | |
| P2P Message Structures | Complete |
| Message Serialization | Complete |
| Peer Connection Management | Complete |
| Peer Discovery | Complete |
| Inventory/Data Relay | Complete |
| Headers-First Sync | Complete |
| Transaction Mempool | Complete |
| **Application Layer** | |
| Node Initialization | Complete |
| Main Event Loop | Complete |
| RPC Interface | Complete |
| Logging System | Complete |

Next: [Phase 10 — Mining Interface](https://github.com/bitcoinecho/bitcoinecho-org/blob/main/ROADMAP.md#phase-10-mining-interface)

## Building

### POSIX (Linux, macOS, BSD)

```sh
make
./echo
```

### Windows

```cmd
build.bat
echo.exe
```

## Running Observer Mode

Observer mode connects to Bitcoin mainnet and watches live network traffic without validation or chain sync. The "Pinocchio moment."

```sh
./echo --observe
```

This starts the node in observer mode with:
- RPC server on `localhost:8332`
- P2P port on `8333`
- Data directory at `~/.bitcoin-echo`

**Optional flags:**
```sh
./echo --observe --datadir /path/to/data --port 8333 --rpcport 8332
```

**Testing the RPC API:**
```sh
# Get observer statistics
curl -X POST http://localhost:8332/ \
  -H "Content-Type: application/json" \
  -d '{"method":"getobserverstats","params":[],"id":1}'

# Get recent block announcements
curl -X POST http://localhost:8332/ \
  -H "Content-Type: application/json" \
  -d '{"method":"getobservedblocks","params":[],"id":1}'

# Get recent transaction announcements
curl -X POST http://localhost:8332/ \
  -H "Content-Type: application/json" \
  -d '{"method":"getobservedtxs","params":[],"id":1}'
```

**Using with the GUI:**

See [bitcoinecho-gui](https://github.com/bitcoinecho/bitcoinecho-gui) for a browser-based interface to observer mode.

## Testing

```sh
make test
```

Runs all unit tests for cryptographic primitives, data structures, and script execution.

## Requirements

- C11 compiler (GCC, Clang, or MSVC)
- No external dependencies

## Documentation

- [Whitepaper](https://bitcoinecho.org/docs/whitepaper) — Technical specification
- [Manifesto](https://bitcoinecho.org/docs/manifesto) — Philosophical foundation
- [Bitcoin Primer](https://bitcoinecho.org/docs/primer) — What is Bitcoin?
- [Building Guide](https://bitcoinecho.org/docs/building) — Compilation for the future
- [Roadmap](https://github.com/bitcoinecho/bitcoinecho-org/blob/main/ROADMAP.md) — Detailed implementation progress

## License

MIT License — see [LICENSE](LICENSE)
