# Bitcoin Echo

A complete, ossified implementation of the Bitcoin protocol in pure C.

*Build once. Build right. Stop.*

## Status

**Phase 6: Chain Selection** — Complete

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

Next: [Phase 7 — Storage Layer](https://github.com/bitcoinecho/bitcoinecho-org/blob/main/ROADMAP.md#phase-7-storage)

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
